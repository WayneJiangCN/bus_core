#ifndef _TM_RING_H_
#define _TM_RING_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfg.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_que.h"
#include "tm_ring_l2_buffer_node.h"
#include "tm_ring_pmu.h"
#include "tm_ring_rbrg_l1.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"

class PemBiu;
using p_tm_ring_biu_t = std::shared_ptr<PemBiu>;

class TmRingMasterNiu;
using tm_ring_m_niu_t = TmRingMasterNiu;
using p_tm_ring_m_niu_t = std::shared_ptr<tm_ring_m_niu_t>;

class TmRingCrossStation;
using tm_ring_cs_t = TmRingCrossStation;
using p_tm_ring_cs_t = std::shared_ptr<tm_ring_cs_t>;

class TmRingConn;
using tm_ring_conn_t = TmRingConn;
using p_tm_ring_conn_t =
    std::shared_ptr<tm_ring_conn_t>;

class TmRingMemPort;
using tm_ring_mem_port_t = TmRingMemPort;
using p_tm_ring_mem_port_t = std::shared_ptr<tm_ring_mem_port_t>;

class TmRingSlotPool;
using tm_ring_slot_pool_t = TmRingSlotPool;
using p_tm_ring_slot_pool_t = std::shared_ptr<tm_ring_slot_pool_t>;

/*
 * Top-level ring interconnect model.
 *
 * Fabric owns the H/V Ring domains, RBRGs and endpoint adapters.
 * Slot arbitration and event handling stay inside each child module.
 */
class TmRingFabric : public tm_engine::TmModule {
 public:
  TmRingFabric(tm_engine::p_tm_clk_t clk, p_tm_ring_cfg_t cfg);

  void build() override;
  void reset() override;
  bool idle() override;

  void attach_master(uint32_t idx, p_tm_ring_biu_t biu);
  void attach_master(uint32_t idx, p_tm_com_inf_t inf);
  void attach_target(uint32_t idx, p_tm_com_inf_t inf);
  void attach_target(uint32_t idx, p_tm_mem_t mem);

  void clear_stats();
  std::vector<TmRingEndpointQueueStats> ring_queue_stats(
      uint64_t snapshot_cycle) const;
  TmRingConnStallBreakdown ring_conn_stall_breakdown() const;
  std::vector<TmRingDomainStats> ring_domain_stats() const;
  std::vector<TmRingRbrgStats> rbrg_stats() const;
  uint32_t ring_link_width_bytes() const;
  uint32_t rbrg_width_bytes() const;
  TmRingConnStats conn_stats(TmRingSubnet subnet) const;
  std::vector<TmRingConnHotspot> ring_top_busy_conns(
      TmRingSubnet subnet, uint32_t limit) const;
  uint64_t ring_conn_stalls() const;
  TmRingCrossStationStats csstats() const;
  TmRingHomeAgentStats home_agent_stats() const;
  std::vector<TmRingHaSourceStats> ha_source_stats() const;
  const TmRingRbrgPathStats& rbrg_path_stats(
      uint32_t v_ring_id, TmRingRbrgPath path) const;

  TmRingL2BufferStats l2_buffer_stats() const;

 private:
  struct TmRingDomain {
    TmRingDomainType type = TmRingDomainType::V_RING;
    uint32_t ring_id = 0;
    std::vector<p_tm_ring_cs_t> stations;
    std::vector<p_tm_ring_conn_t> connections;
    p_tm_ring_slot_pool_t slot_pool = nullptr;
  };

  void config() override;

  tm_engine::p_tm_clk_t clk_ = nullptr;
  p_tm_ring_cfg_t cfg_ = nullptr;

  std::vector<p_tm_ring_m_niu_t> master_nius_;
  std::vector<p_tm_ring_mem_port_t> mem_ports_;
  std::vector<p_tm_ring_l2_buffer_node_t> l2_buffer_nodes_;
  TmRingDomain h_ring_;
  std::vector<TmRingDomain> v_rings_;
  std::vector<p_tm_ring_rbrg_l1_t> rbrgs_;

  // Topology owns address/node mapping. DDR/L2 service limits live in TmMem.
  std::shared_ptr<TmRingTopology> topology_;
  void init_topology();
  void clear_components();
  TmRingDomain create_domain(TmRingDomainType type, uint32_t ring_id,
                             uint32_t station_count);
  void attach_domain(TmRingDomain* domain);
  p_tm_ring_cs_t station(const TmRingLocation& location) const;
  p_tm_ring_conn_t output_conn(const TmRingDomain& domain,
                               uint32_t station_id,
                               TmRingPortDir direction) const;
  void create_master_nius();
  void create_mem_ports();
  void create_l2_buffer_nodes();
  void create_rbrgs();

  void bind_master_nius();
  void bind_mem_ports();
  void bind_l2_buffer_nodes();
  void bind_rbrgs();
  void attach_l2_buffers();
  void bind_master_niu(uint32_t idx, p_tm_ring_m_niu_t niu);
};

using tm_ring_fabric_t = TmRingFabric;
using p_tm_ring_fabric_t = std::shared_ptr<TmRingFabric>;

inline constexpr uint64_t tm_ring_default_target_address_limit() {
  return 0x80000000ull;
}

inline p_tm_ring_target_cfg_t tm_make_ring_target_cfg(
    uint32_t target_id, uint32_t target_count, bool enable_interleave,
    tm_bus_interleave_type_t interleave_type, uint32_t interleave_size,
    uint32_t sector_size, uint32_t interleave_hash_shift,
    uint32_t interleave_hash_seed, uint32_t target_width_bytes,
    uint32_t target_fifo_depth) {
  auto target = std::make_shared<tm_ring_target_cfg_t>();
  target->name = "ddr" + std::to_string(target_id);
  target->addr_begin = 0;
  target->is_default = !enable_interleave && target_id == 0;
  target->size = !enable_interleave && target_id != 0
                     ? 0
                     : tm_ring_default_target_address_limit();
  target->sector_size = sector_size;

  if (enable_interleave) {
    target->interleave_type = interleave_type;
    // Ring interleave is cacheline/home based. With 512B home granularity,
    // all 128B sectors inside one 512B line route to the same target/home
    // bank. XOR mode only mixes higher home-line bits into target selection.
    target->interleave_size = interleave_size;
    target->interleave_num = target_count;
    target->interleave_idx = target_id;
    target->interleave_hash_shift = interleave_hash_shift;
    target->interleave_hash_seed = interleave_hash_seed;
  }

  target->width = target_width_bytes;
  target->rd_req_fifo_depth = target_fifo_depth;
  target->wr_req_fifo_depth = target_fifo_depth;
  target->wr_dat_fifo_depth = target_fifo_depth;

  return target;
}

inline p_tm_ring_cfg_t tm_make_ring_cfg() {
  return std::make_shared<tm_ring_cfg_t>();
}

inline p_tm_ring_cfg_t tm_make_ring_cfg(std::string name) {
  auto ring_cfg = tm_make_ring_cfg();
  ring_cfg->name = name;
  return ring_cfg;
}

inline uint32_t tm_ring_endpoint_queue_depth(cfg::p_cfg_t cfg,
                                             const std::string& key) {
  const int value = cfg->get_cfg<int>(key);
  if (value < 0) {
    throw std::invalid_argument(
        "Ring endpoint queue depth must be nonnegative");
  }
  return static_cast<uint32_t>(value);
}

inline p_tm_ring_cfg_t tm_make_ring_cfg(std::string name, cfg::p_cfg_t cfg) {
  auto ring_cfg = tm_make_ring_cfg(name);
  if (cfg == nullptr) {
    return ring_cfg;
  }

  const int num_masters = cfg->get_cfg<int>("RING.num_masters");
  if (num_masters <= 0) {
    throw std::invalid_argument("num_masters must be positive");
  }
  ring_cfg->num_masters = static_cast<uint32_t>(num_masters);
  ring_cfg->rd_rsp_port_num = static_cast<uint32_t>(
      cfg->get_cfg<int>("BIU.bus_read_port_num"));
  ring_cfg->enable_home_agent =
      cfg->get_cfg<int>("RING.enable_home_agent") == 1;
  ring_cfg->home_agent_transaction_entries = static_cast<uint32_t>(
      cfg->get_cfg<int>("RING.home_agent_transaction_entries"));
  ring_cfg->home_agent_waiters_per_entry = static_cast<uint32_t>(
      cfg->get_cfg<int>("RING.home_agent_waiters_per_entry"));
  ring_cfg->l2_traffic.hit_rate_pct =
      static_cast<uint32_t>(cfg->get_cfg<int>("L2_TRAFFIC.hit_rate_pct"));
  ring_cfg->l2_traffic.hit_seed =
      static_cast<uint32_t>(cfg->get_cfg<int>("L2_TRAFFIC.hit_seed"));
  ring_cfg->l2_traffic.buffer_depth =
      static_cast<uint32_t>(cfg->get_cfg<int>("L2_TRAFFIC.buffer_depth"));
  ring_cfg->l2_traffic.response_latency = static_cast<uint32_t>(
      cfg->get_cfg<int>("L2_TRAFFIC.response_latency"));
  ring_cfg->l2_traffic.issue_interval = static_cast<uint32_t>(
      cfg->get_cfg<int>("L2_TRAFFIC.issue_interval"));

  const uint32_t target_count =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.num_targets"));
  const std::string interleave = cfg->get_cfg<std::string>("RING.interleave");
  const tm_bus_interleave_type_t interleave_type =
      interleave == "xor" ? tm_bus_interleave_type_t::XOR_HASH
                          : tm_bus_interleave_type_t::LINEAR;
  const bool enable_interleave = interleave != "none";

  const uint32_t interleave_size =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.interleave_size"));
  const uint32_t sector_size =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.sector_size"));
  ring_cfg->l2_traffic.line_size = interleave_size;
  ring_cfg->l2_traffic.sector_size = sector_size;
  const uint32_t interleave_hash_shift =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.interleave_hash_shift"));
  const uint32_t interleave_hash_seed =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.interleave_hash_seed"));
  const uint32_t target_width_bytes =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.target_width_bytes"));
  const uint32_t target_fifo_depth =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.target_fifo_depth"));

  ring_cfg->ring_link_latency =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.ring_link_latency"));
  ring_cfg->ring_link_width_bytes =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.ring_link_width_bytes"));
  const char* endpoint_names[] = {"master", "home_agent", "l2_buffer",
                                  "rbrg_v", "rbrg_h"};
  const char* subnet_names[] = {"req", "rsp", "dat"};
  for (uint32_t node_type = 0;
       node_type < static_cast<uint32_t>(TmRingNodeType::COUNT);
       ++node_type) {
    TmRingEndpointQueueDepths& queue_depths =
        ring_cfg->endpoint_queue_depths[node_type];
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      const std::string field_prefix =
          std::string("RING.") + endpoint_names[node_type] + "_" +
          subnet_names[subnet] + "_";
      queue_depths.inject[subnet] = tm_ring_endpoint_queue_depth(
          cfg, field_prefix + "inject_depth");
      queue_depths.eject[subnet] = tm_ring_endpoint_queue_depth(
          cfg, field_prefix + "eject_depth");
    }
  }
  const int max_aicore_per_vring =
      cfg->get_cfg<int>("RING.max_aicore_per_vring");
  if (max_aicore_per_vring <= 0) {
    throw std::invalid_argument("max_aicore_per_vring must be positive");
  }
  ring_cfg->max_aicore_per_vring =
      static_cast<uint32_t>(max_aicore_per_vring);
  ring_cfg->rbrg_queue_depth =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.rbrg_queue_depth"));
  ring_cfg->rbrg_latency =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.rbrg_latency"));
  ring_cfg->rbrg_width_bytes =
      static_cast<uint32_t>(cfg->get_cfg<int>("RING.rbrg_width_bytes"));
  ring_cfg->targets.clear();
  for (uint32_t target = 0; target < target_count; ++target) {
    ring_cfg->targets.push_back(tm_make_ring_target_cfg(
        target, target_count, enable_interleave, interleave_type,
        interleave_size, sector_size, interleave_hash_shift,
        interleave_hash_seed, target_width_bytes, target_fifo_depth));
  }

  return ring_cfg;
}

inline p_tm_ring_fabric_t tm_make_ring(tm_engine::p_tm_clk_t clk,
                                       p_tm_ring_cfg_t cfg) {
  return std::make_shared<TmRingFabric>(clk, cfg);
}

#endif  // _TM_RING_H_
