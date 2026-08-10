#include "tm_ring_perf_report.h"

#include <iomanip>
#include <sstream>

namespace {

const char* perf_op_name(TmRingPerfOp op) {
  switch (op) {
    case TmRingPerfOp::READ:
      return "read";
    case TmRingPerfOp::WRITE:
      return "write";
    case TmRingPerfOp::READ_WRITE:
      return "read_write";
  }
  return "unknown";
}

const char* perf_pattern_name(TmRingPerfPattern pattern) {
  switch (pattern) {
    case TmRingPerfPattern::SEQUENTIAL_PRIVATE:
      return "sequential_private";
    case TmRingPerfPattern::SEQUENTIAL_SHARED:
      return "sequential_shared";
    case TmRingPerfPattern::STRIDED_PRIVATE:
      return "strided_private";
    case TmRingPerfPattern::SINGLE_TARGET:
      return "single_target";
  }
  return "unknown";
}

const char* subnet_name(TmRingSubnet subnet) {
  switch (subnet) {
    case TmRingSubnet::REQ:
      return "req";
    case TmRingSubnet::RSP:
      return "rsp";
    case TmRingSubnet::DAT:
      return "dat";
  }
  return "unknown";
}

const char* direction_name(TmRingPortDir direction) {
  switch (direction) {
    case TmRingPortDir::LOCAL:
      return "local";
    case TmRingPortDir::CW:
      return "cw";
    case TmRingPortDir::CCW:
      return "ccw";
  }
  return "unknown";
}

const char* domain_type_name(TmRingDomainType type) {
  return type == TmRingDomainType::H_RING ? "h" : "v";
}

const char* rbrg_path_name(TmRingRbrgPath path) {
  switch (path) {
    case TmRingRbrgPath::V_TO_H_REQ:
      return "v_to_h_req";
    case TmRingRbrgPath::V_TO_H_DAT:
      return "v_to_h_dat";
    case TmRingRbrgPath::H_TO_V_RSP:
      return "h_to_v_rsp";
    case TmRingRbrgPath::H_TO_V_DAT:
      return "h_to_v_dat";
  }
  return "unknown";
}

struct DirectionCycles {
  DirectionCycles() : cw(0), ccw(0) {}

  uint64_t cw;
  uint64_t ccw;
};

DirectionCycles collect_direction_cycles(const TmRingPerfEstimate& estimate,
                                         TmRingSubnet subnet) {
  DirectionCycles total;
  for (const auto& edge : estimate.edge_cycles) {
    if (edge.first.subnet != subnet) {
      continue;
    }
    if (edge.first.direction == TmRingPortDir::CW) {
      total.cw += edge.second;
    } else if (edge.first.direction == TmRingPortDir::CCW) {
      total.ccw += edge.second;
    }
  }
  return total;
}

double direction_imbalance_pct(const DirectionCycles& cycles) {
  const uint64_t total = cycles.cw + cycles.ccw;
  if (total == 0) {
    return 0.0;
  }
  const uint64_t difference = cycles.cw > cycles.ccw
                                  ? cycles.cw - cycles.ccw
                                  : cycles.ccw - cycles.cw;
  return 100.0 * static_cast<double>(difference) /
         static_cast<double>(total);
}

void append_domain_direction(std::ostringstream* out, const char* subnet,
                             const char* direction,
                             const TmRingConnStats& stats) {
  const std::string prefix = std::string(subnet) + "_" + direction;
  *out << ' ' << prefix << "_packets=" << stats.packets << ' ' << prefix
       << "_bytes=" << stats.bytes << ' ' << prefix
       << "_busy_cycles=" << stats.busy_cycles << ' ' << prefix
       << "_stalls=" << tm_ring_conn_total_stalls(stats);
}

uint64_t saturating_subtract(uint64_t left, uint64_t right) {
  return left > right ? left - right : 0;
}

}  // namespace

std::string tm_ring_format_perf_result(const TmRingPerfResult& result) {
  const DirectionCycles req_direction_cycles =
      collect_direction_cycles(result.estimate, TmRingSubnet::REQ);
  const DirectionCycles dat_direction_cycles =
      collect_direction_cycles(result.estimate, TmRingSubnet::DAT);
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "PERF_CONFIG case=" << result.perf_case.name
      << " op=" << perf_op_name(result.perf_case.op)
      << " pattern=" << perf_pattern_name(result.perf_case.pattern)
      << " active_masters=" << result.perf_case.active_masters
      << " bytes_per_master=" << result.perf_case.bytes_per_master
      << " burst_bytes=" << result.perf_case.burst_bytes << '\n';

  out << "PERF_COUNTS completed_packets=" << result.completed_packets
      << " completed_bytes=" << result.completed_bytes
      << " protocol_errors=" << result.protocol_errors
      << " drained=" << (result.drained ? 1 : 0) << '\n';

  out << "PERF_BANDWIDTH first_request=" << result.first_request_time
      << " first_response=" << result.first_response_time
      << " last_response=" << result.last_response_time
      << " transfer_cycles=" << result.transfer_cycles
      << " end_to_end_bpc=" << result.end_to_end_bandwidth_bpc
      << " steady_response_bpc=" << result.steady_response_bandwidth_bpc
      << " scaling_efficiency=" << result.scaling_efficiency
      << " jain_fairness=" << result.jain_fairness << '\n';

  out << "PERF_LATENCY p50=" << result.latency_p50
      << " p95=" << result.latency_p95 << " p99=" << result.latency_p99
      << " max=" << result.latency_max << '\n';

  out << "PERF_RING";
  for (uint32_t subnet = 0; subnet < 3; ++subnet) {
    const TmRingConnStats& stats = result.conn_stats[subnet];
    out << ' ' << subnet_name(static_cast<TmRingSubnet>(subnet))
        << "_packets=" << stats.packets
        << ' ' << subnet_name(static_cast<TmRingSubnet>(subnet))
        << "_bytes=" << stats.bytes
        << ' ' << subnet_name(static_cast<TmRingSubnet>(subnet))
        << "_busy_cycles=" << stats.busy_cycles
        << ' ' << subnet_name(static_cast<TmRingSubnet>(subnet))
        << "_stalls=" << tm_ring_conn_total_stalls(stats);
  }
  out << " cross_station_injected="
      << result.cross_station_stats.injected_packets
      << " cross_station_ejected="
      << result.cross_station_stats.ejected_packets
      << " cross_station_deflected="
      << result.cross_station_stats.deflected_packets
      << " hottest_subnet="
      << subnet_name(result.estimate.hottest_edge.subnet)
      << " hottest_src_station=" << result.estimate.hottest_edge.src_station
      << " hottest_direction="
      << direction_name(result.estimate.hottest_edge.direction)
      << " hottest_cycles=" << result.estimate.hottest_ring_edge_cycles
      << " req_cw_cycles=" << req_direction_cycles.cw
      << " req_ccw_cycles=" << req_direction_cycles.ccw
      << " req_imbalance_pct="
      << direction_imbalance_pct(req_direction_cycles)
      << " dat_cw_cycles=" << dat_direction_cycles.cw
      << " dat_ccw_cycles=" << dat_direction_cycles.ccw
      << " dat_imbalance_pct="
      << direction_imbalance_pct(dat_direction_cycles)
       << " hottest_edge_cycles="
       << result.estimate.hottest_ring_edge_cycles << '\n';

  for (const TmRingDomainStats& domain : result.ring_domain_stats) {
    out << "PERF_RING_DOMAIN type=" << domain_type_name(domain.type)
        << " id=" << domain.ring_id;
    for (uint32_t subnet = 0; subnet < 3; ++subnet) {
      const char* name = subnet_name(static_cast<TmRingSubnet>(subnet));
      append_domain_direction(&out, name, "cw", domain.cw[subnet]);
      append_domain_direction(&out, name, "ccw", domain.ccw[subnet]);
    }
    const TmRingConnHotspot& hottest = domain.hottest;
    out << " hottest_subnet=" << subnet_name(hottest.subnet)
        << " hottest_src_station=" << hottest.src_station
        << " hottest_src_direction=" << direction_name(hottest.src_dir)
        << " hottest_dst_station=" << hottest.dst_station
        << " hottest_dst_direction=" << direction_name(hottest.dst_dir)
        << " hottest_packets=" << hottest.packets
        << " hottest_bytes=" << hottest.bytes
        << " hottest_busy_cycles=" << hottest.busy_cycles
        << " hottest_serialization_busy_stall="
        << hottest.serialization_busy_stall
        << " hottest_stalls=" << hottest.total_stalls
        << " hottest_inflight_peak=" << hottest.inflight_peak << '\n';
  }

  uint64_t v_ring_carriers = 0;
  for (uint32_t rbrg_id = 0; rbrg_id < result.rbrg_stats.size(); ++rbrg_id) {
    const TmRingRbrgStats& rbrg = result.rbrg_stats[rbrg_id];
    out << "PERF_RBRG id=" << rbrg_id;
    for (uint32_t path_index = 0; path_index < rbrg.paths.size();
         ++path_index) {
      const TmRingRbrgPath path = static_cast<TmRingRbrgPath>(path_index);
      const TmRingRbrgPathStats& stats = rbrg.paths[path_index];
      const char* path_name = rbrg_path_name(path);
      out << ' ' << path_name << "_packets=" << stats.packets << ' '
          << path_name << "_bytes=" << stats.bytes << ' ' << path_name
          << "_queue_peak=" << stats.queue_occupancy_peak << ' ' << path_name
          << "_queue_full_stalls=" << stats.queue_full_stalls << ' '
          << path_name << "_inject_stalls=" << stats.destination_inject_stalls;
    }
    v_ring_carriers += rbrg.paths[static_cast<uint32_t>(
        TmRingRbrgPath::H_TO_V_DAT)].packets;
    out << '\n';
  }

  const uint64_t logical_recipients =
      result.l2_buffer_stats.h_carrier_recipients;
  const uint64_t h_ring_carriers = result.l2_buffer_stats.h_carriers;
  const uint64_t h_ring_saved_packets =
      saturating_subtract(logical_recipients, h_ring_carriers);
  const uint64_t v_ring_saved_packets =
      saturating_subtract(logical_recipients, v_ring_carriers);
  out << "PERF_FANOUT_CROSS_RING logical_recipients=" << logical_recipients
      << " h_ring_carriers=" << h_ring_carriers
      << " v_ring_carriers=" << v_ring_carriers
      << " h_ring_saved_packets=" << h_ring_saved_packets
      << " v_ring_saved_packets=" << v_ring_saved_packets
      << " total_segment_packets_saved="
      << h_ring_saved_packets + v_ring_saved_packets << '\n';

  out << "PERF_HOME_AGENT rd_requests="
      << result.home_agent_stats.rd_requests
      << " backend_reads=" << result.home_agent_stats.rd_backend_issued
      << " backend_read_saved=" << result.home_agent_stats.backend_read_saved
      << " l2_hits=" << result.home_agent_stats.l2_hit_transactions
      << " l2_misses=" << result.home_agent_stats.l2_miss_transactions
      << " write_hazard_stalls="
      << result.home_agent_stats.write_hazard_stall_cycles << '\n';

  out << "PERF_L2_BUFFER responses_accepted="
      << result.l2_buffer_stats.responses_accepted
      << " h_carriers=" << result.l2_buffer_stats.h_carriers
      << " h_unicast_carriers="
      << result.l2_buffer_stats.h_unicast_carriers
      << " h_multicast_carriers="
      << result.l2_buffer_stats.h_multicast_carriers
      << " h_scatter_carriers="
      << result.l2_buffer_stats.h_scatter_carriers
      << " h_carrier_recipients="
      << result.l2_buffer_stats.h_carrier_recipients
      << " dat_bytes=" << result.l2_buffer_stats.dat_bytes
      << " occupancy_peak=" << result.l2_buffer_stats.buffer_occupancy_peak
      << " buffer_full_stalls="
      << result.l2_buffer_stats.buffer_full_stall_cycles
      << " issue_interval_stalls="
      << result.l2_buffer_stats.issue_interval_stall_cycles
      << " dat_inject_stalls="
      << result.l2_buffer_stats.dat_inject_full_stall_cycles
      << " carrier_128b=" << result.l2_buffer_stats.injected_carrier_128b
      << " carrier_256b=" << result.l2_buffer_stats.injected_carrier_256b
      << " carrier_512b=" << result.l2_buffer_stats.injected_carrier_512b
      << '\n';

  out << "PERF_MEMORY accepted_read_bytes="
      << result.memory_stats.accepted_read_bytes
      << " accepted_write_bytes="
      << result.memory_stats.accepted_write_bytes
      << " credit_stalls=" << result.memory_stats.credit_stall_cycles
      << " queue_full_stalls="
      << result.memory_stats.queue_full_stall_cycles
      << " outstanding_peak=" << result.memory_stats.outstanding_peak << '\n';

  const double theory_efficiency =
      result.estimate.fabric_model_ceiling_bpc == 0.0
          ? 0.0
          : result.end_to_end_bandwidth_bpc /
                result.estimate.fabric_model_ceiling_bpc;
  out << "PERF_THEORY total_useful_bytes="
      << result.estimate.total_useful_bytes
      << " physical_packets=" << result.estimate.physical_packets
      << " fabric_min_cycles=" << result.estimate.fabric_min_cycles
      << " hottest_ring_edge_cycles="
      << result.estimate.hottest_ring_edge_cycles
      << " hottest_rbrg_path_cycles="
      << result.estimate.hottest_rbrg_path_cycles
      << " fabric_ceiling_bpc="
      << result.estimate.fabric_model_ceiling_bpc
      << " measured_over_fabric_ceiling=" << theory_efficiency
      << " assumption=finite_trace_packet_slot_upper_bound\n";

  out << "PERF_RESULT status=" << (result.drained ? "PASS" : "INCOMPLETE")
      << " protocol_errors=" << result.protocol_errors << '\n';
  return out.str();
}
