#include <algorithm>

#include <stdexcept>

#include "pem_biu.h"
#include "tm_ring.h"
#include "tm_ring_conn.h"
#include "tm_ring_cross_station.h"
#include "tm_ring_master_niu.h"
#include "tm_ring_mem_port.h"
#include "tm_ring_slot_pool.h"

using namespace std;
using namespace tm_engine;

namespace {

bool is_power_of_two(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

void validate_ring_config(const p_tm_ring_cfg_t& cfg) {
  if (cfg == nullptr) {
    throw std::invalid_argument("Ring configuration must not be null");
  }
  if (cfg->num_masters == 0 || cfg->max_aicore_per_vring == 0) {
    throw std::invalid_argument(
        "Ring master count and V-Ring capacity must be nonzero");
  }
  if (cfg->rbrg_queue_depth == 0 || cfg->rbrg_width_bytes == 0) {
    throw std::invalid_argument("RBRG queue depth and width must be nonzero");
  }

  if (cfg->targets.empty()) {
    throw std::invalid_argument("Ring must have at least one target");
  }
  const uint64_t master_count = static_cast<uint64_t>(cfg->num_masters);
  const uint64_t v_ring_capacity =
      static_cast<uint64_t>(cfg->max_aicore_per_vring);
  const uint64_t v_ring_count =
      (master_count + v_ring_capacity - 1) / v_ring_capacity;
  const uint64_t masters_per_vring = master_count / v_ring_count;
  const uint64_t extra_masters = master_count % v_ring_count;
  for (uint64_t ring_id = 0; ring_id < v_ring_count; ++ring_id) {
    const uint64_t v_ring_stations =
        masters_per_vring + (ring_id < extra_masters ? 1ull : 0ull) + 1ull;
    if (v_ring_stations > 64) {
      throw std::invalid_argument("V-Ring station count must not exceed 64");
    }
  }
  const uint64_t h_ring_stations =
      v_ring_count + 2ull * static_cast<uint64_t>(cfg->targets.size());
  if (h_ring_stations > 64) {
    throw std::invalid_argument("H-Ring station count must not exceed 64");
  }
  for (uint32_t node_type = 0;
       node_type < static_cast<uint32_t>(TmRingNodeType::COUNT);
       ++node_type) {
    const TmRingEndpointQueueDepths& queue_depths =
        cfg->endpoint_queue_depths[node_type];
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      if (queue_depths.inject[subnet] == 0 ||
          queue_depths.eject[subnet] == 0) {
        throw std::invalid_argument(
            "Ring endpoint queue depths must be nonzero");
      }
    }
  }
  if (cfg->l2_traffic.buffer_depth == 0) {
    throw std::invalid_argument("L2 Buffer depth must be nonzero");
  }
  if (cfg->enable_home_agent &&
      (cfg->home_agent_transaction_entries == 0 ||
       cfg->home_agent_waiters_per_entry == 0)) {
    throw std::invalid_argument(
        "Home Agent entry and waiter limits must be nonzero");
  }

  const uint32_t line_size = cfg->l2_traffic.line_size;
  const uint32_t sector_size = cfg->l2_traffic.sector_size;
  if (!is_power_of_two(line_size) || !is_power_of_two(sector_size) ||
      line_size % sector_size != 0) {
    throw std::invalid_argument(
        "L2 line and sector sizes must be powers of two and line divisible "
        "by sector");
  }

  for (const p_tm_ring_target_cfg_t& target : cfg->targets) {
    if (target == nullptr || target->sector_size == 0 ||
        target->sector_size != sector_size) {
      throw std::invalid_argument(
          "Each Ring target must use the configured sector size");
    }
    if (target->interleave_size != 0 &&
        (!is_power_of_two(target->interleave_size) ||
         target->interleave_size % sector_size != 0)) {
      throw std::invalid_argument(
          "Target interleave size must be a power of two divisible by sector");
    }
    if (cfg->enable_home_agent && target->interleave_size != 0 &&
        target->interleave_size != line_size) {
      throw std::invalid_argument(
          "Home Agent targets must use the configured line size");
    }
  }
}

const char* ring_dir_name(TmRingPortDir dir) {
  if (dir == TmRingPortDir::CW) {
    return "CW";
  }
  if (dir == TmRingPortDir::CCW) {
    return "CCW";
  }
  return "L";
}

}  // namespace

TmRingFabric::TmRingFabric(p_tm_clk_t clk, p_tm_ring_cfg_t cfg)
    : clk_(clk), cfg_(cfg) {
  validate_ring_config(cfg_);
  this->name(cfg_->name);
  config();
}

void TmRingFabric::config() {
  init_topology();
  clear_components();
  h_ring_ = create_domain(TmRingDomainType::H_RING, 0,
                          topology_->h_ring_station_count());
  for (uint32_t ring = 0; ring < topology_->v_ring_count(); ++ring) {
    v_rings_.push_back(create_domain(
        TmRingDomainType::V_RING, ring,
        topology_->v_ring_station_count(ring)));
  }
  create_master_nius();
  create_mem_ports();
  create_l2_buffer_nodes();
  create_rbrgs();
  bind_master_nius();
  bind_mem_ports();
  bind_l2_buffer_nodes();
  bind_rbrgs();
  attach_domain(&h_ring_);
  for (uint32_t ring = 0; ring < v_rings_.size(); ++ring) {
    attach_domain(&v_rings_[ring]);
  }
  attach_l2_buffers();
  reset();
}

void TmRingFabric::init_topology() {
  topology_ = std::make_shared<TmRingTopology>();
  topology_->config(cfg_);
}

void TmRingFabric::clear_components() {
  master_nius_.clear();
  mem_ports_.clear();
  l2_buffer_nodes_.clear();
  h_ring_ = TmRingDomain();
  v_rings_.clear();
  rbrgs_.clear();
}

TmRingFabric::TmRingDomain TmRingFabric::create_domain(
    TmRingDomainType type, uint32_t ring_id, uint32_t station_count) {
  TmRingDomain domain;
  domain.type = type;
  domain.ring_id = ring_id;
  domain.slot_pool =
      tm_make_ring_slot_pool(station_count, cfg_->ring_link_latency);

  const std::string domain_name =
      this->name() +
      (type == TmRingDomainType::H_RING
           ? "_h_ring"
           : "_v_ring" + std::to_string(ring_id));
  for (uint32_t station_id = 0; station_id < station_count; ++station_id) {
    domain.stations.push_back(tm_make_ring_cs(
        domain_name + "_cs" + std::to_string(station_id), clk_));
    for (uint32_t direction_index = 0; direction_index < 2;
         ++direction_index) {
      const TmRingPortDir direction =
          direction_index == 0 ? TmRingPortDir::CW : TmRingPortDir::CCW;
      const uint32_t dst_station = topology_->neighbor_station(
          type, ring_id, station_id, direction);
      const TmRingPortDir dst_direction = tm_ring_opposite_dir(direction);
      const std::string connection_name =
          domain_name + "_conn_" + std::to_string(station_id) + "_" +
          ring_dir_name(direction) + "_" + std::to_string(dst_station) +
          "_" + ring_dir_name(dst_direction);
      domain.connections.push_back(tm_make_ring_conn(
          connection_name, clk_, cfg_->ring_link_latency,
          cfg_->ring_link_width_bytes, dst_station, dst_direction));
    }
  }
  return domain;
}

void TmRingFabric::attach_domain(TmRingDomain* domain) {
  for (uint32_t station_id = 0; station_id < domain->stations.size();
       ++station_id) {
    domain->stations[station_id]->attach(
        station_id,
        output_conn(*domain, station_id, TmRingPortDir::CW),
        output_conn(*domain, station_id, TmRingPortDir::CCW),
        domain->slot_pool);
  }

  for (const p_tm_ring_conn_t& connection : domain->connections) {
    const p_tm_ring_cs_t destination =
        domain->stations[connection->dst_station()];
    connection->attach(
        destination->transit_in_reg(connection->dst_dir(), TmRingSubnet::REQ),
        destination->transit_in_reg(connection->dst_dir(), TmRingSubnet::RSP),
        destination->transit_in_reg(connection->dst_dir(), TmRingSubnet::DAT));
  }
}

p_tm_ring_cs_t TmRingFabric::station(
    const TmRingLocation& location) const {
  if (location.ring_type == TmRingDomainType::H_RING) {
    return h_ring_.stations[location.station_id];
  }
  return v_rings_[location.ring_id].stations[location.station_id];
}

p_tm_ring_conn_t TmRingFabric::output_conn(
    const TmRingDomain& domain, uint32_t station_id,
    TmRingPortDir direction) const {
  const uint32_t direction_index = direction == TmRingPortDir::CW ? 0 : 1;
  return domain.connections[station_id * 2 + direction_index];
}

void TmRingFabric::create_master_nius() {
  const TmRingEndpointQueueDepths& queue_depths =
      cfg_->endpoint_queue_depths[static_cast<uint32_t>(
          TmRingNodeType::MASTER)];
  for (uint32_t i = 0; i < cfg_->num_masters; ++i) {
    master_nius_.push_back(tm_make_ring_master_niu(
        this->name() + "_master_niu" + std::to_string(i), clk_, i,
        queue_depths));
  }
}

void TmRingFabric::create_mem_ports() {
  const TmRingEndpointQueueDepths& queue_depths =
      cfg_->endpoint_queue_depths[static_cast<uint32_t>(
          TmRingNodeType::HOME_AGENT)];
  for (uint32_t i = 0; i < cfg_->targets.size(); ++i) {
    mem_ports_.push_back(tm_make_ring_mem_port(
        this->name() + "_mem_port_" + std::to_string(i), clk_,
        *cfg_->targets[i], *cfg_, queue_depths));
  }
}

void TmRingFabric::create_l2_buffer_nodes() {
  const TmRingEndpointQueueDepths& queue_depths =
      cfg_->endpoint_queue_depths[static_cast<uint32_t>(
          TmRingNodeType::L2_BUFFER)];
  for (uint32_t target = 0; target < cfg_->targets.size(); ++target) {
    l2_buffer_nodes_.push_back(tm_make_ring_l2_buffer_node(
        this->name() + "_l2_buffer_" + std::to_string(target), clk_,
        cfg_->l2_traffic, queue_depths));
  }
}

void TmRingFabric::create_rbrgs() {
  const TmRingEndpointQueueDepths& v_queue_depths =
      cfg_->endpoint_queue_depths[static_cast<uint32_t>(
          TmRingNodeType::RBRG_V)];
  const TmRingEndpointQueueDepths& h_queue_depths =
      cfg_->endpoint_queue_depths[static_cast<uint32_t>(
          TmRingNodeType::RBRG_H)];
  for (uint32_t ring = 0; ring < topology_->v_ring_count(); ++ring) {
    rbrgs_.push_back(tm_make_ring_rbrg_l1(
        this->name() + "_rbrg_" + std::to_string(ring), clk_, ring,
        cfg_->rbrg_queue_depth, cfg_->rbrg_latency, cfg_->rbrg_width_bytes,
        v_queue_depths, h_queue_depths, topology_));
  }
}

void TmRingFabric::bind_master_nius() {
  for (uint32_t i = 0; i < master_nius_.size(); ++i) {
    bind_master_niu(i, master_nius_[i]);
  }
}

void TmRingFabric::bind_master_niu(uint32_t idx,
                                   p_tm_ring_m_niu_t niu) {
  master_nius_[idx] = niu;

  niu->attach(topology_);
  station(topology_->master_location(idx))->bind_node_interface(
      niu->node_interface());
}

void TmRingFabric::bind_mem_ports() {
  for (uint32_t target_id = 0; target_id < mem_ports_.size(); ++target_id) {
    auto mem_port = mem_ports_[target_id];
    mem_port->attach(target_id, topology_);
    station(topology_->ha_location(target_id))->bind_node_interface(
        mem_port->node_interface());
  }
}

void TmRingFabric::bind_l2_buffer_nodes() {
  for (uint32_t target_id = 0; target_id < l2_buffer_nodes_.size();
       ++target_id) {
    auto l2_buffer = l2_buffer_nodes_[target_id];
    l2_buffer->attach(target_id, topology_);
    station(topology_->l2_location(target_id))->bind_node_interface(
        l2_buffer->node_interface());
  }
}

void TmRingFabric::bind_rbrgs() {
  for (uint32_t ring = 0; ring < rbrgs_.size(); ++ring) {
    station(topology_->rbrg_v_location(ring))->bind_node_interface(
        rbrgs_[ring]->v_node_interface());
    station(topology_->rbrg_h_location(ring))->bind_node_interface(
        rbrgs_[ring]->h_node_interface());
  }
}

void TmRingFabric::attach_l2_buffers() {
  for (uint32_t target_id = 0; target_id < l2_buffer_nodes_.size();
       ++target_id) {
    mem_ports_[target_id]->attach_l2_buffer(l2_buffer_nodes_[target_id]);
  }
}

void TmRingFabric::build() {}

void TmRingFabric::reset() {
  h_ring_.slot_pool->reset();
  for (const p_tm_ring_cs_t& ring_station : h_ring_.stations) {
    ring_station->reset();
  }
  for (const p_tm_ring_conn_t& connection : h_ring_.connections) {
    connection->reset();
  }
  for (TmRingDomain& domain : v_rings_) {
    domain.slot_pool->reset();
    for (const p_tm_ring_cs_t& ring_station : domain.stations) {
      ring_station->reset();
    }
    for (const p_tm_ring_conn_t& connection : domain.connections) {
      connection->reset();
    }
  }
  for (const p_tm_ring_rbrg_l1_t& rbrg : rbrgs_) {
    rbrg->reset();
  }
  for (auto& niu : master_nius_) {
    niu->reset();
  }
  for (auto& mem_port : mem_ports_) {
    mem_port->reset();
  }
  for (auto& l2_buffer : l2_buffer_nodes_) {
    l2_buffer->reset();
  }
}

void TmRingFabric::clear_stats() {
  const uint64_t now = clk_->time();
  for (const p_tm_ring_cs_t& ring_station : h_ring_.stations) {
    ring_station->clear_stats();
  }
  for (const p_tm_ring_conn_t& connection : h_ring_.connections) {
    connection->clear_stats();
  }
  for (const TmRingDomain& domain : v_rings_) {
    for (const p_tm_ring_cs_t& ring_station : domain.stations) {
      ring_station->clear_stats();
    }
    for (const p_tm_ring_conn_t& connection : domain.connections) {
      connection->clear_stats();
    }
  }
  for (const p_tm_ring_rbrg_l1_t& rbrg : rbrgs_) {
    rbrg->clear_stats();
    rbrg->v_node_interface()->clear_stats(now);
    rbrg->h_node_interface()->clear_stats(now);
  }
  for (const auto& master_niu : master_nius_) {
    master_niu->node_interface()->clear_stats(now);
  }
  for (const auto& mem_port : mem_ports_) {
    mem_port->clear_stats();
    mem_port->node_interface()->clear_stats(now);
  }
  for (const auto& l2_buffer : l2_buffer_nodes_) {
    l2_buffer->clear_stats();
    l2_buffer->node_interface()->clear_stats(now);
  }
}

bool TmRingFabric::idle() {
  const auto domain_idle = [](const TmRingDomain& domain) {
    if (!domain.slot_pool->empty()) {
      return false;
    }
    for (const p_tm_ring_cs_t& ring_station : domain.stations) {
      if (!ring_station->idle()) {
        return false;
      }
    }
    for (const p_tm_ring_conn_t& connection : domain.connections) {
      if (!connection->idle()) {
        return false;
      }
    }
    return true;
  };

  if (!domain_idle(h_ring_)) {
    return false;
  }
  for (const TmRingDomain& domain : v_rings_) {
    if (!domain_idle(domain)) {
      return false;
    }
  }
  for (const p_tm_ring_rbrg_l1_t& rbrg : rbrgs_) {
    if (!rbrg->idle()) {
      return false;
    }
  }
  for (const p_tm_ring_m_niu_t& niu : master_nius_) {
    if (!niu->idle()) {
      return false;
    }
  }
  for (const p_tm_ring_mem_port_t& mem_port : mem_ports_) {
    if (!mem_port->idle()) {
      return false;
    }
  }
  for (const p_tm_ring_l2_buffer_node_t& l2_buffer : l2_buffer_nodes_) {
    if (!l2_buffer->idle()) {
      return false;
    }
  }
  return true;
}

TmRingConnStallBreakdown
TmRingFabric::ring_conn_stall_breakdown() const {
  TmRingConnStats total;
  total.merge_from(conn_stats(TmRingSubnet::REQ));
  total.merge_from(conn_stats(TmRingSubnet::RSP));
  total.merge_from(conn_stats(TmRingSubnet::DAT));

  TmRingConnStallBreakdown stalls;
  stalls.serialization_busy = total.serialization_busy_stall;
  stalls.pipeline_full = total.pipeline_full_stall;
  stalls.downstream_register_full = total.downstream_register_full_stall;
  return stalls;
}

std::vector<TmRingEndpointQueueStats> TmRingFabric::ring_queue_stats(
    uint64_t snapshot_cycle) const {
  std::vector<TmRingEndpointQueueStats> result;
  result.reserve((master_nius_.size() + mem_ports_.size() +
                  l2_buffer_nodes_.size() + 2 * rbrgs_.size()) *
                 9);

  const auto append_endpoint = [&result, snapshot_cycle](
                                   TmRingNodeType node_type, uint32_t node_id,
                                   const p_tm_ring_node_interface_t& niu) {
    const std::vector<TmRingQueueStats> queue_stats =
        niu->queue_stats(snapshot_cycle);
    for (const TmRingQueueStats& queue : queue_stats) {
      TmRingEndpointQueueStats endpoint_stats;
      endpoint_stats.node_type = node_type;
      endpoint_stats.node_id = node_id;
      endpoint_stats.queue = queue;
      result.push_back(endpoint_stats);
    }
  };

  for (uint32_t i = 0; i < master_nius_.size(); ++i) {
    append_endpoint(TmRingNodeType::MASTER, i,
                    master_nius_[i]->node_interface());
  }
  for (uint32_t i = 0; i < mem_ports_.size(); ++i) {
    append_endpoint(TmRingNodeType::HOME_AGENT, i,
                    mem_ports_[i]->node_interface());
  }
  for (uint32_t i = 0; i < l2_buffer_nodes_.size(); ++i) {
    append_endpoint(TmRingNodeType::L2_BUFFER, i,
                    l2_buffer_nodes_[i]->node_interface());
  }
  for (uint32_t i = 0; i < rbrgs_.size(); ++i) {
    append_endpoint(TmRingNodeType::RBRG_V, i,
                    rbrgs_[i]->v_node_interface());
    append_endpoint(TmRingNodeType::RBRG_H, i,
                    rbrgs_[i]->h_node_interface());
  }
  return result;
}

std::vector<TmRingDomainStats> TmRingFabric::ring_domain_stats() const {
  std::vector<TmRingDomainStats> result;
  result.reserve(1 + v_rings_.size());

  const auto append_domain = [&result](const TmRingDomain& domain) {
    TmRingDomainStats domain_stats;
    domain_stats.type = domain.type;
    domain_stats.ring_id = domain.ring_id;
    domain_stats.directed_edge_count =
        static_cast<uint32_t>(domain.connections.size() / 2);
    for (const p_tm_ring_cs_t& station : domain.stations) {
      domain_stats.cross_station.merge_from(station->stats());
    }
    bool has_hottest = false;

    for (uint32_t index = 0; index < domain.connections.size(); ++index) {
      const p_tm_ring_conn_t& connection = domain.connections[index];
      const TmRingPortDir direction =
          index % 2 == 0 ? TmRingPortDir::CW : TmRingPortDir::CCW;
      for (uint32_t subnet_index = 0; subnet_index < 3; ++subnet_index) {
        const TmRingSubnet subnet =
            static_cast<TmRingSubnet>(subnet_index);
        const TmRingConnStats& stats = connection->subnet_stats(subnet);
        std::array<TmRingConnStats, 3>& direction_stats =
            direction == TmRingPortDir::CW ? domain_stats.cw
                                           : domain_stats.ccw;
        direction_stats[subnet_index].merge_from(stats);

        TmRingConnHotspot candidate;
        candidate.src_station = index / 2;
        candidate.src_dir = direction;
        candidate.dst_station = connection->dst_station();
        candidate.dst_dir = connection->dst_dir();
        candidate.subnet = subnet;
        candidate.packets = stats.packets;
        candidate.bytes = stats.bytes;
        candidate.busy_cycles = stats.busy_cycles;
        candidate.serialization_busy_stall =
            stats.serialization_busy_stall;
        candidate.total_stalls = tm_ring_conn_total_stalls(stats);
        candidate.inflight_peak = stats.inflight_peak;
        domain_stats.edges.push_back(candidate);

        const TmRingConnHotspot& hottest = domain_stats.hottest;
        const bool is_hotter =
            !has_hottest ||
            candidate.serialization_busy_stall >
                hottest.serialization_busy_stall ||
            (candidate.serialization_busy_stall ==
                 hottest.serialization_busy_stall &&
             candidate.busy_cycles > hottest.busy_cycles) ||
            (candidate.serialization_busy_stall ==
                 hottest.serialization_busy_stall &&
             candidate.busy_cycles == hottest.busy_cycles &&
             candidate.bytes > hottest.bytes);
        if (is_hotter) {
          domain_stats.hottest = candidate;
          has_hottest = true;
        }
      }
    }
    result.push_back(domain_stats);
  };

  append_domain(h_ring_);
  for (const TmRingDomain& domain : v_rings_) {
    append_domain(domain);
  }
  return result;
}

std::vector<TmRingRbrgStats> TmRingFabric::rbrg_stats() const {
  std::vector<TmRingRbrgStats> result;
  result.reserve(rbrgs_.size());
  for (const p_tm_ring_rbrg_l1_t& rbrg : rbrgs_) {
    result.push_back(rbrg->stats());
  }
  return result;
}

uint32_t TmRingFabric::ring_link_width_bytes() const {
  return cfg_->ring_link_width_bytes;
}

uint32_t TmRingFabric::rbrg_width_bytes() const {
  return cfg_->rbrg_width_bytes;
}

TmRingConnStats TmRingFabric::conn_stats(TmRingSubnet subnet) const {
  TmRingConnStats total;
  const uint32_t subnet_index = static_cast<uint32_t>(subnet);
  const std::vector<TmRingDomainStats> domains = ring_domain_stats();
  for (const TmRingDomainStats& domain : domains) {
    total.merge_from(domain.cw[subnet_index]);
    total.merge_from(domain.ccw[subnet_index]);
  }
  return total;
}

std::vector<TmRingConnHotspot>
TmRingFabric::ring_top_busy_conns(TmRingSubnet subnet,
                                  uint32_t limit) const {
  std::vector<TmRingConnHotspot> hot_conns;
  const auto append_domain = [&hot_conns, subnet](const TmRingDomain& domain) {
    for (uint32_t index = 0; index < domain.connections.size(); ++index) {
      const p_tm_ring_conn_t& connection = domain.connections[index];
      const TmRingConnStats& stats = connection->subnet_stats(subnet);

      TmRingConnHotspot hot;
      hot.src_station = index / 2;
      hot.src_dir = index % 2 == 0 ? TmRingPortDir::CW : TmRingPortDir::CCW;
      hot.dst_station = connection->dst_station();
      hot.dst_dir = connection->dst_dir();
      hot.subnet = subnet;
      hot.packets = stats.packets;
      hot.bytes = stats.bytes;
      hot.busy_cycles = stats.busy_cycles;
      hot.serialization_busy_stall = stats.serialization_busy_stall;
      hot.total_stalls = tm_ring_conn_total_stalls(stats);
      hot.inflight_peak = stats.inflight_peak;
      hot_conns.push_back(hot);
    }
  };
  append_domain(h_ring_);
  for (const TmRingDomain& domain : v_rings_) {
    append_domain(domain);
  }

  std::sort(
      hot_conns.begin(), hot_conns.end(),
      [](const TmRingConnHotspot& lhs,
         const TmRingConnHotspot& rhs) {
        if (lhs.serialization_busy_stall != rhs.serialization_busy_stall) {
          return lhs.serialization_busy_stall >
                 rhs.serialization_busy_stall;
        }
        if (lhs.busy_cycles != rhs.busy_cycles) {
          return lhs.busy_cycles > rhs.busy_cycles;
        }
        if (lhs.bytes != rhs.bytes) {
          return lhs.bytes > rhs.bytes;
        }
        if (lhs.src_station != rhs.src_station) {
          return lhs.src_station < rhs.src_station;
        }
        return static_cast<uint32_t>(lhs.src_dir) <
               static_cast<uint32_t>(rhs.src_dir);
      });

  if (hot_conns.size() > limit) {
    hot_conns.resize(limit);
  }
  return hot_conns;
}

uint64_t TmRingFabric::ring_conn_stalls() const {
  return ring_conn_stall_breakdown().total();
}

TmRingL2BufferStats TmRingFabric::l2_buffer_stats() const {
  TmRingL2BufferStats total;
  for (const auto& l2_buffer : l2_buffer_nodes_) {
    total.merge_from(l2_buffer->stats());
  }
  return total;
}

TmRingCrossStationStats TmRingFabric::csstats() const {
  TmRingCrossStationStats total;
  for (const p_tm_ring_cs_t& ring_station : h_ring_.stations) {
    total.merge_from(ring_station->stats());
  }
  for (const TmRingDomain& domain : v_rings_) {
    for (const p_tm_ring_cs_t& ring_station : domain.stations) {
      total.merge_from(ring_station->stats());
    }
  }
  return total;
}

TmRingHomeAgentStats TmRingFabric::home_agent_stats() const {
  TmRingHomeAgentStats total;
  for (const auto& mem_port : mem_ports_) {
    const auto stats = mem_port->home_agent_stats();
    total.merge_from(stats);
  }
  return total;
}

const TmRingRbrgPathStats& TmRingFabric::rbrg_path_stats(
    uint32_t v_ring_id, TmRingRbrgPath path) const {
  return rbrgs_.at(v_ring_id)->path_stats(path);
}

void TmRingFabric::attach_master(uint32_t idx, p_tm_ring_biu_t biu) {
  if (biu != nullptr) {
    attach_master(idx, biu->out_intf_);
  }
}

void TmRingFabric::attach_master(uint32_t idx, p_tm_com_inf_t inf) {
  if (idx < master_nius_.size() && inf != nullptr) {
    master_nius_[idx]->attach(inf);
  }
}

void TmRingFabric::attach_target(uint32_t idx, p_tm_com_inf_t inf) {
  if (idx < mem_ports_.size() && inf != nullptr) {
    mem_ports_[idx]->attach(inf);
  }
}

void TmRingFabric::attach_target(uint32_t idx, p_tm_mem_t mem) {
  if (idx < mem_ports_.size() && mem != nullptr) {
    mem_ports_[idx]->attach(mem);
  }
}
