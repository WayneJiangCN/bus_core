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
    case TmRingPerfPattern::SAME_LINE_SCATTER:
      return "same_line_scatter";
    case TmRingPerfPattern::STRIDED_PRIVATE:
      return "strided_private";
    case TmRingPerfPattern::SINGLE_TARGET:
      return "single_target";
  }
  return "unknown";
}

const char* perf_run_mode_name(TmRingPerfRunMode mode) {
  switch (mode) {
    case TmRingPerfRunMode::FREE_RUNNING:
      return "free_running";
    case TmRingPerfRunMode::AGGREGATION_WAVE:
      return "aggregation_wave";
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

const char* node_type_name(TmRingNodeType type) {
  switch (type) {
    case TmRingNodeType::MASTER:
      return "master";
    case TmRingNodeType::HOME_AGENT:
      return "home_agent";
    case TmRingNodeType::L2_BUFFER:
      return "l2_buffer";
    case TmRingNodeType::RBRG_V:
      return "rbrg_v";
    case TmRingNodeType::RBRG_H:
      return "rbrg_h";
    case TmRingNodeType::COUNT:
      break;
  }
  return "unknown";
}

const char* queue_side_name(TmRingQueueSide side) {
  return side == TmRingQueueSide::INJECT ? "inject" : "eject";
}

const char* queue_direction_name(TmRingPortDir direction) {
  return direction == TmRingPortDir::LOCAL ? "shared"
                                           : direction_name(direction);
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

double percentage(uint64_t numerator, uint64_t denominator) {
  return denominator == 0
             ? 0.0
             : 100.0 * static_cast<double>(numerator) /
                   static_cast<double>(denominator);
}

double average(uint64_t sum, uint64_t count) {
  return count == 0 ? 0.0
                    : static_cast<double>(sum) / static_cast<double>(count);
}

double cycle_util_pct(uint64_t busy_cycles, uint64_t measurement_cycles,
                      uint32_t edge_count) {
  return percentage(busy_cycles,
                    measurement_cycles * static_cast<uint64_t>(edge_count));
}

double payload_util_pct(uint64_t bytes, uint64_t measurement_cycles,
                        uint32_t width_bytes, uint32_t edge_count) {
  return percentage(bytes, measurement_cycles * width_bytes *
                               static_cast<uint64_t>(edge_count));
}

double serialization_efficiency_pct(uint64_t bytes, uint64_t busy_cycles,
                                    uint32_t width_bytes) {
  return percentage(bytes, busy_cycles * static_cast<uint64_t>(width_bytes));
}

double subnet_imbalance_pct(const TmRingConnStats& cw,
                            const TmRingConnStats& ccw) {
  const uint64_t total = cw.busy_cycles + ccw.busy_cycles;
  const uint64_t difference = cw.busy_cycles > ccw.busy_cycles
                                  ? cw.busy_cycles - ccw.busy_cycles
                                  : ccw.busy_cycles - cw.busy_cycles;
  return percentage(difference, total);
}

uint64_t saturating_subtract(uint64_t left, uint64_t right) {
  return left > right ? left - right : 0;
}

void append_theory(std::ostringstream* out, const char* section,
                   const TmRingPerfEstimate& estimate,
                   double measured_bandwidth) {
  const double theory_efficiency =
      estimate.fabric_model_ceiling_bpc == 0.0
          ? 0.0
          : measured_bandwidth / estimate.fabric_model_ceiling_bpc;
  *out << section << " total_useful_bytes=" << estimate.total_useful_bytes
       << " physical_packets=" << estimate.physical_packets
       << " logical_read_requests=" << estimate.logical_read_requests
       << " backend_reads=" << estimate.backend_reads
       << " backend_read_saved=" << estimate.backend_read_saved
       << " h_carriers=" << estimate.h_carriers
       << " h_unicast_carriers=" << estimate.h_unicast_carriers
       << " h_multicast_carriers=" << estimate.h_multicast_carriers
       << " h_scatter_carriers=" << estimate.h_scatter_carriers
       << " h_carrier_recipients=" << estimate.h_carrier_recipients
       << " v_carriers=" << estimate.v_carriers
       << " fabric_min_cycles=" << estimate.fabric_min_cycles
       << " hottest_ring_edge_cycles="
       << estimate.hottest_ring_edge_cycles
       << " hottest_rbrg_path_cycles="
       << estimate.hottest_rbrg_path_cycles
       << " fabric_ceiling_bpc=" << estimate.fabric_model_ceiling_bpc
       << " measured_over_fabric_ceiling=" << theory_efficiency
       << " assumption=finite_trace_packet_slot_upper_bound\n";
}

}  // namespace

std::string tm_ring_format_perf_result(const TmRingPerfResult& result) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6);
  out << "PERF_CONFIG case=" << result.perf_case.name
      << " op=" << perf_op_name(result.perf_case.op)
      << " pattern=" << perf_pattern_name(result.perf_case.pattern)
      << " active_masters=" << result.perf_case.active_masters
      << " bytes_per_master=" << result.perf_case.bytes_per_master
      << " burst_bytes=" << result.perf_case.burst_bytes
      << " run_mode=" << perf_run_mode_name(result.perf_case.run_mode)
      << " max_aicore_per_vring="
      << result.perf_case.max_aicore_per_vring
      << " home_agent_waiters_per_entry="
      << result.perf_case.home_agent_waiters_per_entry
      << " l2_response_latency="
      << result.perf_case.l2_response_latency << '\n';

  out << "PERF_MEASUREMENT start_cycle=" << result.measurement_start_time
      << " end_cycle=" << result.measurement_end_time
      << " window_cycles=" << result.measurement_cycles
      << " measurement_valid=" << (result.measurement_valid ? 1 : 0)
      << '\n';

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
      << " scaling_efficiency_available="
      << (result.scaling_efficiency_available ? 1 : 0)
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
      << result.cross_station_stats.ejected_packets << '\n';

  for (const TmRingEndpointQueueStats& endpoint :
       result.endpoint_queue_stats) {
    const TmRingQueueStats& queue = endpoint.queue;
    out << "PERF_RING_BUFFER node_type=" << node_type_name(endpoint.node_type)
        << " node=" << endpoint.node_id
        << " subnet=" << subnet_name(queue.subnet)
        << " side=" << queue_side_name(queue.side)
        << " direction=" << queue_direction_name(queue.direction)
        << " depth=" << queue.depth
        << " pushes=" << queue.counters.pushes
        << " pops=" << queue.counters.pops
        << " push_rejects=" << queue.counters.push_rejects
        << " occupancy=" << queue.occupancy
        << " peak=" << queue.occupancy_peak
        << " occupancy_area=" << queue.counters.occupancy_area
        << " avg_occupancy_pct="
        << percentage(queue.counters.occupancy_area,
                      result.measurement_cycles * queue.depth)
        << " full_cycles=" << queue.counters.full_cycles
        << " full_pct="
        << percentage(queue.counters.full_cycles, result.measurement_cycles)
        << '\n';
  }

  for (const TmRingDomainStats& domain : result.ring_domain_stats) {
    for (uint32_t subnet = 0; subnet < 3; ++subnet) {
      const TmRingSubnet subnet_type = static_cast<TmRingSubnet>(subnet);
      const TmRingDeflectionStats& deflection =
          domain.cross_station.deflection[subnet];
      out << "PERF_DEFLECTION domain=" << domain_type_name(domain.type)
          << " ring=" << domain.ring_id
          << " subnet=" << subnet_name(subnet_type)
          << " events=" << deflection.events
          << " unique_packets=" << deflection.unique_packets
          << " eligible_unicast_packets="
          << deflection.eligible_unicast_packets
          << " completed_packets=" << deflection.completed_packets
          << " deflection_rate_pct="
          << percentage(deflection.unique_packets,
                        deflection.eligible_unicast_packets)
          << " rounds_sum=" << deflection.rounds_sum
          << " avg_rounds="
          << average(deflection.rounds_sum, deflection.completed_packets)
          << " max_rounds=" << deflection.rounds_max
          << " delay_cycles_sum=" << deflection.delay_cycles_sum
          << " avg_delay_cycles="
          << average(deflection.delay_cycles_sum, deflection.completed_packets)
          << " max_delay_cycles=" << deflection.delay_cycles_max
          << " fanout_recipient_retry_events="
          << deflection.fanout_recipient_retry_events << '\n';

      const TmRingConnStats* directions[] = {&domain.cw[subnet],
                                              &domain.ccw[subnet]};
      const char* direction_names[] = {"cw", "ccw"};
      for (uint32_t direction = 0; direction < 2; ++direction) {
        const TmRingConnStats& stats = *directions[direction];
        out << "PERF_HW_CHANNEL domain=" << domain_type_name(domain.type)
            << " ring=" << domain.ring_id
            << " subnet=" << subnet_name(subnet_type)
            << " direction=" << direction_names[direction]
            << " window_cycles=" << result.measurement_cycles
            << " edge_count=" << domain.directed_edge_count
            << " width_bytes=" << result.ring_link_width_bytes
            << " packets=" << stats.packets
            << " bytes=" << stats.bytes
            << " busy_cycles=" << stats.busy_cycles
            << " downstream_register_full_stalls="
            << stats.downstream_register_full_stall
            << " serialization_busy_stalls=" << stats.serialization_busy_stall
            << " pipeline_full_stalls=" << stats.pipeline_full_stall
            << " send_reject_stalls=" << stats.send_reject_stall
            << " stalls=" << tm_ring_conn_total_stalls(stats)
            << " cycle_util_pct="
            << cycle_util_pct(stats.busy_cycles, result.measurement_cycles,
                              domain.directed_edge_count)
            << " payload_util_pct="
            << payload_util_pct(stats.bytes, result.measurement_cycles,
                                result.ring_link_width_bytes,
                                domain.directed_edge_count)
            << " serialization_efficiency_pct="
            << serialization_efficiency_pct(stats.bytes, stats.busy_cycles,
                                            result.ring_link_width_bytes)
            << " cw_busy_cycles=" << domain.cw[subnet].busy_cycles
            << " ccw_busy_cycles=" << domain.ccw[subnet].busy_cycles
            << " subnet_imbalance_pct="
            << subnet_imbalance_pct(domain.cw[subnet], domain.ccw[subnet])
            << '\n';
      }
    }

    for (const TmRingConnHotspot& edge : domain.edges) {
      out << "PERF_RING_EDGE domain=" << domain_type_name(domain.type)
          << " ring=" << domain.ring_id
          << " subnet=" << subnet_name(edge.subnet)
          << " direction=" << direction_name(edge.src_dir)
          << " src_station=" << edge.src_station
          << " dst_station=" << edge.dst_station
          << " window_cycles=" << result.measurement_cycles
          << " width_bytes=" << result.ring_link_width_bytes
          << " packets=" << edge.packets
          << " bytes=" << edge.bytes
          << " busy_cycles=" << edge.busy_cycles
          << " serialization_busy_stalls="
          << edge.serialization_busy_stall
          << " stalls=" << edge.total_stalls
          << " inflight_peak=" << edge.inflight_peak
          << " cycle_util_pct="
          << cycle_util_pct(edge.busy_cycles, result.measurement_cycles, 1)
          << " payload_util_pct="
          << payload_util_pct(edge.bytes, result.measurement_cycles,
                              result.ring_link_width_bytes, 1)
          << " serialization_efficiency_pct="
          << serialization_efficiency_pct(edge.bytes, edge.busy_cycles,
                                          result.ring_link_width_bytes)
          << '\n';
    }
  }

  uint64_t v_ring_carriers = 0;
  for (uint32_t rbrg_id = 0; rbrg_id < result.rbrg_stats.size(); ++rbrg_id) {
    const TmRingRbrgStats& rbrg = result.rbrg_stats[rbrg_id];
    for (uint32_t path_index = 0; path_index < rbrg.paths.size();
         ++path_index) {
      const TmRingRbrgPath path = static_cast<TmRingRbrgPath>(path_index);
      const TmRingRbrgPathStats& stats = rbrg.paths[path_index];
      out << "PERF_RBRG_CHANNEL id=" << rbrg_id
          << " path=" << rbrg_path_name(path)
          << " window_cycles=" << result.measurement_cycles
          << " width_bytes=" << result.rbrg_width_bytes
          << " packets=" << stats.packets
          << " bytes=" << stats.bytes
          << " busy_cycles=" << stats.busy_cycles
          << " cycle_util_pct="
          << cycle_util_pct(stats.busy_cycles, result.measurement_cycles, 1)
          << " payload_util_pct="
          << payload_util_pct(stats.bytes, result.measurement_cycles,
                              result.rbrg_width_bytes, 1)
          << " serialization_efficiency_pct="
          << serialization_efficiency_pct(stats.bytes, stats.busy_cycles,
                                          result.rbrg_width_bytes)
          << " queue_peak=" << stats.queue_occupancy_peak
          << " queue_full_stalls=" << stats.queue_full_stalls
          << " destination_inject_stalls="
          << stats.destination_inject_stalls << '\n';
    }
    v_ring_carriers += rbrg.paths[static_cast<uint32_t>(
        TmRingRbrgPath::H_TO_V_DAT)].packets;
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

  for (const TmRingHaSourceStats& source : result.ha_source_stats) {
    out << "PERF_HA_SOURCE ha=" << source.ha_id
        << " master=" << source.master_id
        << " rd_packets=" << source.rd_packets
        << " wr_packets=" << source.wr_packets
        << " total_packets=" << source.rd_packets + source.wr_packets << '\n';
  }

  out << "PERF_HOME_AGENT rd_requests="
      << result.home_agent_stats.rd_requests
      << " backend_reads=" << result.home_agent_stats.rd_backend_issued
      << " backend_read_saved=" << result.home_agent_stats.backend_read_saved
      << " l2_hits=" << result.home_agent_stats.l2_hit_transactions
      << " l2_misses=" << result.home_agent_stats.l2_miss_transactions
      << " write_hazard_stalls="
      << result.home_agent_stats.write_hazard_stall_cycles
      << " rd_merged_pending=" << result.home_agent_stats.rd_merged_pending
      << " rd_merged_inflight=" << result.home_agent_stats.rd_merged_inflight
      << " rd_merged_responding="
      << result.home_agent_stats.rd_merged_responding
      << " table_full_stalls="
      << result.home_agent_stats.table_full_stall_cycles
      << " waiter_full_stalls="
      << result.home_agent_stats.waiter_full_stall_cycles
      << " aggregation_closed_stalls="
      << result.home_agent_stats.aggregation_closed_stall_cycles << '\n';

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
      << " carrier_other=" << result.l2_buffer_stats.injected_carrier_other
      << '\n';

  out << "PERF_MEMORY accepted_read_bytes="
      << result.memory_stats.accepted_read_bytes
      << " accepted_write_bytes="
      << result.memory_stats.accepted_write_bytes
      << " credit_stalls=" << result.memory_stats.credit_stall_cycles
      << " queue_full_stalls="
      << result.memory_stats.queue_full_stall_cycles
      << " outstanding_peak=" << result.memory_stats.outstanding_peak << '\n';

  append_theory(&out, "PERF_THEORY_NO_MERGE", result.no_merge_estimate,
                result.end_to_end_bandwidth_bpc);
  append_theory(&out, "PERF_THEORY_IDEAL_MERGE", result.estimate,
                result.end_to_end_bandwidth_bpc);
  append_theory(&out, "PERF_THEORY", result.estimate,
                result.end_to_end_bandwidth_bpc);

  out << "PERF_RESULT status=" << (result.drained ? "PASS" : "INCOMPLETE")
      << " protocol_errors=" << result.protocol_errors << '\n';
  return out.str();
}
