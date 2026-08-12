#include "tm_ring_perf.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

#include "tm_ring.h"
#include "tm_ring_perf_master.h"

namespace {

uint64_t align_up(uint64_t value, uint64_t alignment) {
  return ((value + alignment - 1) / alignment) * alignment;
}

uint64_t private_span(const TmRingPerfCase& perf_case) {
  const uint64_t count = perf_case.bytes_per_master /
                         perf_case.burst_bytes;
  if (perf_case.pattern == TmRingPerfPattern::STRIDED_PRIVATE) {
    return align_up((count - 1) * perf_case.stride_bytes +
                        perf_case.burst_bytes,
                    512);
  }
  return align_up(perf_case.bytes_per_master, 512);
}

uint64_t base_address(const TmRingPerfCase& perf_case, uint32_t master,
                      bool write) {
  const uint64_t base = write ? perf_case.write_base : perf_case.read_base;
  if (perf_case.pattern == TmRingPerfPattern::SEQUENTIAL_SHARED ||
      perf_case.pattern == TmRingPerfPattern::SAME_LINE_SCATTER) {
    return base;
  }
  return base + static_cast<uint64_t>(master) * private_span(perf_case);
}

uint64_t address_for(const TmRingPerfCase& perf_case, uint32_t master,
                     uint64_t ordinal, bool write) {
  const uint64_t base = base_address(perf_case, master, write);
  if (perf_case.pattern == TmRingPerfPattern::STRIDED_PRIVATE ||
      perf_case.pattern == TmRingPerfPattern::SEQUENTIAL_SHARED) {
    return base + ordinal * perf_case.stride_bytes;
  }
  if (perf_case.pattern == TmRingPerfPattern::SAME_LINE_SCATTER) {
    const uint32_t requests_per_line = 512 / perf_case.burst_bytes;
    const uint32_t lines_per_wave =
        (perf_case.active_masters + requests_per_line - 1) /
        requests_per_line;
    const uint32_t line_group = master / requests_per_line;
    const uint32_t slot = master % requests_per_line;
    return base + (ordinal * lines_per_wave + line_group) * 512 +
           slot * perf_case.burst_bytes;
  }
  return base + ordinal * perf_case.burst_bytes;
}

void append_transaction(std::vector<TmRingPerfTxn>* trace,
                        const TmRingPerfCase& perf_case, uint32_t master,
                        uint64_t ordinal, PldCmd cmd, uint64_t addr) {
  TmRingPerfTxn txn;
  txn.master_port = master;
  txn.cmd = cmd;
  txn.addr = addr;
  txn.size = perf_case.burst_bytes;
  txn.ordinal = ordinal;
  trace->push_back(txn);
}

struct ReadGroupKey {
  uint32_t target_id = 0;
  uint64_t line_base = 0;
  uint64_t ordinal = 0;

  bool operator<(const ReadGroupKey& other) const {
    if (target_id != other.target_id) {
      return target_id < other.target_id;
    }
    if (line_base != other.line_base) {
      return line_base < other.line_base;
    }
    return ordinal < other.ordinal;
  }
};

uint32_t packet_cycles(PldCmd cmd, uint32_t size, uint32_t link_width) {
  auto pld = tm_make_pld(cmd, 0, size);
  pld->ring_traffic_class = static_cast<uint32_t>(cmd);
  return tm_ring_serialization_cycles(tm_ring_packet_bytes(pld),
                                      link_width);
}

void add_edge_cycles(TmRingPerfEstimate* estimate, TmRingSubnet subnet,
                     const TmRingLocation& source,
                     TmRingPortDir direction, uint32_t cycles) {
  TmRingPerfEdgeKey key(source.ring_type, source.ring_id, subnet,
                        source.station_id, direction);
  estimate->edge_cycles[key] += cycles;
}

uint32_t domain_station_count(const TmRingLocation& location,
                              const TmRingTopology& topology) {
  return location.ring_type == TmRingDomainType::H_RING
             ? topology.h_ring_station_count()
             : topology.v_ring_station_count(location.ring_id);
}

void add_directed_path(TmRingPerfEstimate* estimate, TmRingSubnet subnet,
                       const TmRingLocation& source,
                       const TmRingLocation& destination,
                       TmRingPortDir direction, uint32_t packet_cost,
                       const TmRingTopology& topology) {
  uint32_t current = source.station_id;
  const uint32_t station_count = domain_station_count(source, topology);
  for (uint32_t hop = 0;
       current != destination.station_id && hop < station_count; ++hop) {
    TmRingLocation edge_source(source.ring_type, source.ring_id, current);
    add_edge_cycles(estimate, subnet, edge_source, direction, packet_cost);
    current = topology.neighbor_station(source.ring_type, source.ring_id,
                                        current, direction);
  }
}

void add_routed_path(TmRingPerfEstimate* estimate, PldCmd cmd, uint32_t size,
                     const TmRingLocation& source,
                     const TmRingLocation& destination,
                     const TmRingTopology& topology, uint32_t link_width) {
  if (source.station_id == destination.station_id) {
    return;
  }
  const TmRingPortDir direction =
      topology.route_direction(source, destination);
  add_directed_path(estimate, tm_ring_cmd_subnet(cmd), source, destination,
                    direction, packet_cycles(cmd, size, link_width),
                    topology);
}

void add_rbrg_path_cycles(TmRingPerfEstimate* estimate, uint32_t rbrg_id,
                          TmRingRbrgPath path, PldCmd cmd, uint32_t size,
                          uint32_t rbrg_width) {
  const TmRingPerfRbrgKey key(rbrg_id, path);
  estimate->rbrg_path_cycles[key] += packet_cycles(cmd, size, rbrg_width);
}

void add_fanout_path(TmRingPerfEstimate* estimate, PldCmd cmd,
                     uint32_t size, const TmRingLocation& source,
                     const std::vector<TmRingLocation>& recipients,
                     TmRingPortDir direction,
                     const TmRingTopology& topology,
                     uint32_t link_width) {
  if (recipients.empty()) {
    return;
  }

  const uint32_t span = topology.fanout_span(source, recipients, direction);
  const uint32_t packet_cost = packet_cycles(cmd, size, link_width);
  uint32_t current = source.station_id;
  for (uint32_t hop = 0; hop < span; ++hop) {
    TmRingLocation edge_source(source.ring_type, source.ring_id, current);
    add_edge_cycles(estimate, tm_ring_cmd_subnet(cmd), edge_source, direction,
                    packet_cost);
    current = topology.neighbor_station(source.ring_type, source.ring_id,
                                        current, direction);
  }
}

void add_v_to_h_packet(TmRingPerfEstimate* estimate, PldCmd cmd,
                       uint32_t size, const TmRingLocation& master,
                       const TmRingLocation& ha,
                       TmRingRbrgPath rbrg_path,
                       const TmRingTopology& topology,
                       const TmRingCfg& ring_cfg) {
  const uint32_t rbrg_id = master.ring_id;
  ++estimate->physical_packets;
  add_routed_path(estimate, cmd, size, master,
                  topology.rbrg_v_location(rbrg_id), topology,
                  ring_cfg.ring_link_width_bytes);
  add_rbrg_path_cycles(estimate, rbrg_id, rbrg_path, cmd, size,
                       ring_cfg.rbrg_width_bytes);
  add_routed_path(estimate, cmd, size, topology.rbrg_h_location(rbrg_id), ha,
                  topology, ring_cfg.ring_link_width_bytes);
}

void add_h_to_v_packet(TmRingPerfEstimate* estimate, PldCmd cmd,
                       uint32_t size, const TmRingLocation& source,
                       const TmRingLocation& master,
                       TmRingRbrgPath rbrg_path,
                       const TmRingTopology& topology,
                       const TmRingCfg& ring_cfg) {
  const uint32_t rbrg_id = master.ring_id;
  ++estimate->physical_packets;
  add_routed_path(estimate, cmd, size, source,
                  topology.rbrg_h_location(rbrg_id), topology,
                  ring_cfg.ring_link_width_bytes);
  add_rbrg_path_cycles(estimate, rbrg_id, rbrg_path, cmd, size,
                       ring_cfg.rbrg_width_bytes);
  add_routed_path(estimate, cmd, size, topology.rbrg_v_location(rbrg_id),
                  master, topology, ring_cfg.ring_link_width_bytes);
}

void add_unicast_read_response(TmRingPerfEstimate* estimate,
                               const TmRingPerfTxn& request,
                               const TmRingLocation& source,
                               const TmRingTopology& topology,
                               const TmRingCfg& ring_cfg) {
  add_h_to_v_packet(estimate, PldCmd::RD_RSP, request.size, source,
                    topology.master_location(request.master_port),
                    TmRingRbrgPath::H_TO_V_DAT, topology, ring_cfg);
  ++estimate->h_carriers;
  ++estimate->h_unicast_carriers;
  ++estimate->h_carrier_recipients;
  ++estimate->v_carriers;
}

bool power_of_two(uint32_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

bool fanout_waiter_set_supported(
    const std::vector<const TmRingPerfTxn*>& requests) {
  if (requests.size() < 2) {
    return false;
  }

  std::set<uint32_t> masters;
  for (const TmRingPerfTxn* request : requests) {
    if (request == nullptr || !masters.insert(request->master_port).second) {
      return false;
    }
  }
  return true;
}

bool fanout_candidate_valid(const TmRingPerfTxn& request,
                            uint64_t line_base,
                            const TmRingCfg& ring_cfg) {
  if (ring_cfg.l2_traffic.line_size == 0 ||
      ring_cfg.l2_traffic.sector_size == 0 || request.addr < line_base) {
    return false;
  }

  const uint32_t sector_size = ring_cfg.l2_traffic.sector_size;
  const uint32_t line_size = ring_cfg.l2_traffic.line_size;
  const uint64_t payload_offset = request.addr - line_base;
  return request.size >= sector_size && request.size <= line_size &&
         request.size % sector_size == 0 &&
         power_of_two(request.size / sector_size) &&
         request.addr % sector_size == 0 &&
         request.size <= line_size - payload_offset;
}

bool request_in_one_line(const TmRingPerfTxn& request,
                         uint64_t line_base,
                         const TmRingCfg& ring_cfg) {
  const uint32_t line_size = ring_cfg.l2_traffic.line_size;
  if (line_size == 0 || request.addr < line_base) {
    return false;
  }
  const uint64_t offset = request.addr - line_base;
  return offset < line_size && request.size <= line_size - offset;
}

uint32_t fanout_carrier_size(
    const std::vector<const TmRingPerfTxn*>& requests, uint64_t line_base,
    const TmRingCfg& ring_cfg) {
  const uint32_t sector_size = ring_cfg.l2_traffic.sector_size;
  const uint32_t line_size = ring_cfg.l2_traffic.line_size;
  uint32_t min_offset = std::numeric_limits<uint32_t>::max();
  uint64_t max_end = 0;
  for (const TmRingPerfTxn* request : requests) {
    const uint64_t offset = request->addr - line_base;
    min_offset = std::min(min_offset, static_cast<uint32_t>(offset));
    max_end = std::max(max_end, offset + request->size);
  }
  if (min_offset == std::numeric_limits<uint32_t>::max()) {
    return 0;
  }

  uint32_t carrier_offset = (min_offset / sector_size) * sector_size;
  const uint64_t needed = max_end - carrier_offset;
  uint32_t carrier_size = sector_size;
  while (carrier_size < needed && carrier_size < line_size) {
    if (carrier_size > line_size / 2) {
      carrier_size = line_size;
      break;
    }
    carrier_size *= 2;
  }
  if (carrier_size > line_size ||
      static_cast<uint64_t>(carrier_offset) + carrier_size > line_size) {
    carrier_size = line_size;
  }
  return carrier_size >= needed ? carrier_size : 0;
}

uint64_t nearest_rank(const std::vector<uint64_t>& values, double percentile) {
  if (values.empty()) {
    return 0;
  }
  const std::size_t rank = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(percentile * values.size())));
  return values[std::min(rank, values.size()) - 1];
}

}  // namespace

bool TmRingPerfEdgeKey::operator==(const TmRingPerfEdgeKey& other) const {
  return ring_type == other.ring_type && ring_id == other.ring_id &&
         subnet == other.subnet && src_station == other.src_station &&
         direction == other.direction;
}

bool TmRingPerfEdgeKey::operator<(const TmRingPerfEdgeKey& other) const {
  if (ring_type != other.ring_type) {
    return static_cast<uint32_t>(ring_type) <
           static_cast<uint32_t>(other.ring_type);
  }
  if (ring_id != other.ring_id) {
    return ring_id < other.ring_id;
  }
  if (subnet != other.subnet) {
    return static_cast<uint32_t>(subnet) <
           static_cast<uint32_t>(other.subnet);
  }
  if (src_station != other.src_station) {
    return src_station < other.src_station;
  }
  return static_cast<uint32_t>(direction) <
         static_cast<uint32_t>(other.direction);
}

bool TmRingPerfRbrgKey::operator==(const TmRingPerfRbrgKey& other) const {
  return rbrg_id == other.rbrg_id && path == other.path;
}

bool TmRingPerfRbrgKey::operator<(const TmRingPerfRbrgKey& other) const {
  if (rbrg_id != other.rbrg_id) {
    return rbrg_id < other.rbrg_id;
  }
  return static_cast<uint32_t>(path) < static_cast<uint32_t>(other.path);
}

std::vector<TmRingPerfTxn> tm_ring_build_perf_trace(
    const TmRingPerfCase& perf_case, uint32_t configured_masters,
    const TmRingTopology& topology) {
  if (perf_case.active_masters == 0 ||
      perf_case.active_masters > configured_masters) {
    throw std::invalid_argument("invalid active master count");
  }
  if (perf_case.burst_bytes == 0 || perf_case.bytes_per_master == 0 ||
      perf_case.bytes_per_master % perf_case.burst_bytes != 0) {
    throw std::invalid_argument("bytes_per_master must be burst aligned");
  }
  if ((perf_case.pattern == TmRingPerfPattern::SEQUENTIAL_SHARED ||
       perf_case.pattern == TmRingPerfPattern::SAME_LINE_SCATTER) &&
      perf_case.op != TmRingPerfOp::READ) {
    throw std::invalid_argument(
        "shared and same-line scatter traffic must be read-only");
  }
  if (perf_case.run_mode == TmRingPerfRunMode::AGGREGATION_WAVE &&
      perf_case.op != TmRingPerfOp::READ) {
    throw std::invalid_argument(
        "aggregation wave traffic must be read-only");
  }
  if (perf_case.pattern == TmRingPerfPattern::SEQUENTIAL_SHARED &&
      perf_case.stride_bytes == 0) {
    throw std::invalid_argument("shared read stride must be nonzero");
  }
  if (perf_case.pattern == TmRingPerfPattern::SAME_LINE_SCATTER &&
      perf_case.burst_bytes != 128 && perf_case.burst_bytes != 256) {
    throw std::invalid_argument(
        "same-line scatter supports 128B or 256B reads");
  }

  const uint64_t transaction_count =
      perf_case.bytes_per_master / perf_case.burst_bytes;
  std::vector<TmRingPerfTxn> trace;
  trace.reserve(static_cast<size_t>(perf_case.active_masters) *
                static_cast<size_t>(transaction_count));

  for (uint32_t master = 0; master < perf_case.active_masters; ++master) {
    for (uint64_t ordinal = 0; ordinal < transaction_count; ++ordinal) {
      const bool emit_read = perf_case.op != TmRingPerfOp::WRITE;
      const bool emit_write = perf_case.op != TmRingPerfOp::READ;
      const uint64_t read_ordinal =
          perf_case.op == TmRingPerfOp::READ_WRITE ? ordinal * 2 : ordinal;
      const uint64_t write_ordinal =
          perf_case.op == TmRingPerfOp::READ_WRITE ? ordinal * 2 + 1
                                                   : ordinal;

      if (emit_read) {
        uint64_t candidate = ordinal;
        uint64_t addr = address_for(perf_case, master, candidate, false);
        while (perf_case.pattern == TmRingPerfPattern::SINGLE_TARGET &&
               topology.decode_target(addr) != perf_case.target_id) {
          ++candidate;
          addr = address_for(perf_case, master, candidate, false);
        }
        append_transaction(&trace, perf_case, master, read_ordinal,
                           PldCmd::RD, addr);
      }

      if (emit_write) {
        uint64_t candidate = ordinal;
        uint64_t addr = address_for(perf_case, master, candidate, true);
        while (perf_case.pattern == TmRingPerfPattern::SINGLE_TARGET &&
               topology.decode_target(addr) != perf_case.target_id) {
          ++candidate;
          addr = address_for(perf_case, master, candidate, true);
        }
        append_transaction(&trace, perf_case, master, write_ordinal,
                           PldCmd::WR, addr);
      }
    }
  }
  return trace;
}

TmRingPerfEstimate tm_ring_estimate_fabric(
    const std::vector<TmRingPerfTxn>& trace,
    const TmRingTopology& topology,
    const TmRingCfg& ring_cfg,
    TmRingPerfAggregationModel aggregation_model) {
  TmRingPerfEstimate estimate;
  std::map<ReadGroupKey, std::vector<const TmRingPerfTxn*> > read_groups;
  std::vector<TmRingPortDir> fanout_tie_next_direction(
      topology.v_ring_count(), TmRingPortDir::CW);

  for (const TmRingPerfTxn& txn : trace) {
    estimate.total_useful_bytes += txn.size;
    const uint32_t target_id = topology.decode_target(txn.addr);
    const TmRingLocation master =
        topology.master_location(txn.master_port);
    const TmRingLocation ha = topology.ha_location(target_id);

    if (txn.cmd == PldCmd::RD) {
      ++estimate.logical_read_requests;
      add_v_to_h_packet(&estimate, PldCmd::RD, txn.size, master, ha,
                        TmRingRbrgPath::V_TO_H_REQ, topology, ring_cfg);
      const uint64_t line_size = ring_cfg.l2_traffic.line_size;
      ReadGroupKey key;
      key.target_id = target_id;
      key.line_base = (txn.addr / line_size) * line_size;
      key.ordinal = txn.ordinal;
      if (aggregation_model == TmRingPerfAggregationModel::NO_MERGE) {
        if (request_in_one_line(txn, key.line_base, ring_cfg)) {
          ++estimate.backend_reads;
        }
        add_unicast_read_response(&estimate, txn,
                                  topology.l2_location(target_id), topology,
                                  ring_cfg);
        continue;
      }
      read_groups[key].push_back(&txn);
      continue;
    }

    if (txn.cmd != PldCmd::WR) {
      throw std::invalid_argument("Perf estimator accepts RD and WR only");
    }
    add_v_to_h_packet(&estimate, PldCmd::WR, txn.size, master, ha,
                      TmRingRbrgPath::V_TO_H_REQ, topology, ring_cfg);
    add_h_to_v_packet(&estimate, PldCmd::WR_RSP, 0, ha, master,
                      TmRingRbrgPath::H_TO_V_RSP, topology, ring_cfg);
    add_v_to_h_packet(&estimate, PldCmd::WR_DAT, txn.size, master, ha,
                      TmRingRbrgPath::V_TO_H_DAT, topology, ring_cfg);
    add_h_to_v_packet(&estimate, PldCmd::RSP, 0, ha, master,
                      TmRingRbrgPath::H_TO_V_RSP, topology, ring_cfg);
  }

  for (const auto& group : read_groups) {
    const std::vector<const TmRingPerfTxn*>& requests = group.second;
    const uint32_t target_id = group.first.target_id;
    const TmRingLocation source = topology.l2_location(target_id);
    uint64_t ha_requests = 0;
    for (const TmRingPerfTxn* request : requests) {
      if (request_in_one_line(*request, group.first.line_base, ring_cfg)) {
        ++ha_requests;
      }
    }
    if (ha_requests != 0) {
      ++estimate.backend_reads;
      estimate.backend_read_saved += ha_requests - 1;
    }
    std::vector<const TmRingPerfTxn*> fanout_requests;
    if (fanout_waiter_set_supported(requests)) {
      for (const TmRingPerfTxn* request : requests) {
        if (fanout_candidate_valid(*request, group.first.line_base, ring_cfg)) {
          fanout_requests.push_back(request);
        } else {
          add_unicast_read_response(&estimate, *request, source, topology,
                                    ring_cfg);
        }
      }
    } else {
      for (const TmRingPerfTxn* request : requests) {
        add_unicast_read_response(&estimate, *request, source, topology,
                                  ring_cfg);
      }
      continue;
    }

    std::map<uint32_t, std::vector<const TmRingPerfTxn*> > v_ring_groups;
    for (const TmRingPerfTxn* request : fanout_requests) {
      v_ring_groups[topology.master_vring(request->master_port)].push_back(
          request);
    }

    for (const auto& v_ring_group : v_ring_groups) {
      const uint32_t rbrg_id = v_ring_group.first;
      const uint32_t carrier_size = fanout_carrier_size(
          v_ring_group.second, group.first.line_base, ring_cfg);
      if (carrier_size == 0) {
        for (const TmRingPerfTxn* request : v_ring_group.second) {
          add_unicast_read_response(&estimate, *request, source, topology,
                                    ring_cfg);
        }
        continue;
      }
      ++estimate.physical_packets;
      ++estimate.h_carriers;
      ++estimate.v_carriers;
      estimate.h_carrier_recipients += v_ring_group.second.size();
      if (v_ring_group.second.size() == 1) {
        ++estimate.h_unicast_carriers;
      } else {
        const TmRingPerfTxn* first = v_ring_group.second.front();
        bool multicast = true;
        for (const TmRingPerfTxn* request : v_ring_group.second) {
          if (request->addr != first->addr || request->size != first->size) {
            multicast = false;
            break;
          }
        }
        if (multicast) {
          ++estimate.h_multicast_carriers;
        } else {
          ++estimate.h_scatter_carriers;
        }
      }
      add_routed_path(&estimate, PldCmd::RD_RSP, carrier_size,
                      source, topology.rbrg_h_location(rbrg_id), topology,
                      ring_cfg.ring_link_width_bytes);
      add_rbrg_path_cycles(&estimate, rbrg_id, TmRingRbrgPath::H_TO_V_DAT,
                           PldCmd::RD_RSP, carrier_size,
                           ring_cfg.rbrg_width_bytes);
      std::vector<TmRingLocation> recipients;
      for (const TmRingPerfTxn* request : v_ring_group.second) {
        recipients.push_back(topology.master_location(request->master_port));
      }
      const TmRingLocation v_source = topology.rbrg_v_location(rbrg_id);
      const uint32_t cw_span = topology.fanout_span(
          v_source, recipients, TmRingPortDir::CW);
      const uint32_t ccw_span = topology.fanout_span(
          v_source, recipients, TmRingPortDir::CCW);
      TmRingPortDir direction = TmRingPortDir::CW;
      if (cw_span < ccw_span) {
        direction = TmRingPortDir::CW;
      } else if (ccw_span < cw_span) {
        direction = TmRingPortDir::CCW;
      } else {
        direction = fanout_tie_next_direction[rbrg_id];
        fanout_tie_next_direction[rbrg_id] =
            direction == TmRingPortDir::CW ? TmRingPortDir::CCW
                                           : TmRingPortDir::CW;
      }
      add_fanout_path(&estimate, PldCmd::RD_RSP, carrier_size, v_source,
                      recipients, direction, topology,
                      ring_cfg.ring_link_width_bytes);
    }
  }

  uint64_t hottest_edge_cycles = 0;
  for (const auto& edge : estimate.edge_cycles) {
    if (edge.second > hottest_edge_cycles) {
      hottest_edge_cycles = edge.second;
      estimate.hottest_edge = edge.first;
    }
  }
  estimate.hottest_ring_edge_cycles = hottest_edge_cycles;
  for (const auto& rbrg_path : estimate.rbrg_path_cycles) {
    estimate.hottest_rbrg_path_cycles = std::max(
        estimate.hottest_rbrg_path_cycles, rbrg_path.second);
  }
  estimate.fabric_min_cycles = std::max(estimate.hottest_ring_edge_cycles,
                                        estimate.hottest_rbrg_path_cycles);
  if (estimate.fabric_min_cycles != 0) {
    estimate.fabric_model_ceiling_bpc =
        static_cast<double>(estimate.total_useful_bytes) /
        static_cast<double>(estimate.fabric_min_cycles);
  }
  return estimate;
}

TmRingPerfResult tm_ring_collect_perf_result(
    const TmRingPerfCase& perf_case,
    const std::vector<TmRingPerfMasterStats>& master_stats,
    const TmRingFabric& fabric,
    const std::vector<TmMemStats>& memory_stats,
    const TmRingPerfEstimate& estimate,
    const TmRingPerfEstimate& no_merge_estimate,
    uint64_t measurement_end_cycle,
    bool drained) {
  TmRingPerfResult result;
  result.perf_case = perf_case;
  result.estimate = estimate;
  result.no_merge_estimate = no_merge_estimate;
  result.drained = drained;
  result.measurement_end_time = measurement_end_cycle;
  result.ring_link_width_bytes = fabric.ring_link_width_bytes();
  result.rbrg_width_bytes = fabric.rbrg_width_bytes();
  const TmRingPmuSnapshot ring_pmu =
      fabric.snapshot_pmu(measurement_end_cycle);
  result.endpoint_queue_stats = ring_pmu.queue.endpoints;
  result.ring_domain_stats = ring_pmu.conn.domains;
  result.rbrg_stats = ring_pmu.rbrg.instances;
  result.rbrg_instance_ids = ring_pmu.rbrg.instance_ids;

  bool has_request = false;
  bool has_response = false;
  std::vector<uint64_t> latencies;
  std::vector<uint64_t> completed_bytes;
  for (const TmRingPerfMasterStats& stats : master_stats) {
    result.completed_packets += stats.completed_packets;
    result.completed_bytes += stats.completed_bytes;
    result.protocol_errors +=
        stats.duplicate_responses + stats.unknown_responses;
    completed_bytes.push_back(stats.completed_bytes);
    latencies.insert(latencies.end(), stats.latency_cycles.begin(),
                     stats.latency_cycles.end());

    if (stats.has_first_request &&
        (!has_request || stats.first_request_cycle < result.first_request_time)) {
      result.first_request_time = stats.first_request_cycle;
      has_request = true;
    }
    if (stats.has_first_response &&
        (!has_response || stats.first_response_cycle <
                             result.first_response_time)) {
      result.first_response_time = stats.first_response_cycle;
      has_response = true;
    }
    if (stats.has_first_response &&
        stats.last_response_cycle > result.last_response_time) {
      result.last_response_time = stats.last_response_cycle;
      has_response = true;
    }
  }

  if (has_request) {
    result.measurement_start_time = result.first_request_time;
    if (measurement_end_cycle >= result.measurement_start_time) {
      result.measurement_cycles = measurement_end_cycle -
                                  result.measurement_start_time + 1;
    }
    result.measurement_valid = drained && result.measurement_cycles != 0;
  }

  if (has_request && has_response &&
      result.last_response_time >= result.first_request_time) {
    result.transfer_cycles = result.last_response_time -
                             result.first_request_time + 1;
    result.end_to_end_bandwidth_bpc =
        static_cast<double>(result.completed_bytes) /
        static_cast<double>(result.transfer_cycles);
  }
  if (has_response && result.last_response_time >= result.first_response_time) {
    const uint64_t response_cycles = std::max<uint64_t>(
        1, result.last_response_time - result.first_response_time);
    result.steady_response_bandwidth_bpc =
        static_cast<double>(result.completed_bytes) /
        static_cast<double>(response_cycles);
  }

  std::sort(latencies.begin(), latencies.end());
  result.latency_p50 = nearest_rank(latencies, 0.50);
  result.latency_p95 = nearest_rank(latencies, 0.95);
  result.latency_p99 = nearest_rank(latencies, 0.99);
  result.latency_max = latencies.empty() ? 0 : latencies.back();

  double sum = 0.0;
  double square_sum = 0.0;
  for (const uint64_t bytes : completed_bytes) {
    sum += static_cast<double>(bytes);
    square_sum += static_cast<double>(bytes) * static_cast<double>(bytes);
  }
  if (!completed_bytes.empty() && square_sum != 0.0) {
    result.jain_fairness =
        (sum * sum) /
        (static_cast<double>(completed_bytes.size()) * square_sum);
  }

  result.conn_stats = ring_pmu.conn.total;
  result.cross_station_stats = ring_pmu.cross_station.total;
  result.home_agent_stats = fabric.home_agent_stats();
  result.ha_source_stats = fabric.ha_source_stats();
  result.l2_buffer_stats = fabric.l2_buffer_stats();
  for (const TmMemStats& stats : memory_stats) {
    result.memory_stats.merge_from(stats);
  }
  return result;
}

double tm_ring_scaling_efficiency(const TmRingPerfResult& multi,
                                  const TmRingPerfResult& single) {
  if (single.perf_case.active_masters == 0 ||
      single.end_to_end_bandwidth_bpc == 0.0) {
    return 0.0;
  }
  const double scale =
      static_cast<double>(multi.perf_case.active_masters) /
      static_cast<double>(single.perf_case.active_masters);
  return multi.end_to_end_bandwidth_bpc /
         (single.end_to_end_bandwidth_bpc * scale);
}
