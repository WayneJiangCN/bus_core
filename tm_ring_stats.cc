#include "tm_ring_stats.h"

#include <algorithm>

void TmRingQueueCounters::clear() { *this = TmRingQueueCounters(); }

void TmRingQueueCounters::merge_from(const TmRingQueueCounters& other) {
  pushes += other.pushes;
  pops += other.pops;
  push_rejects += other.push_rejects;
  occupancy_area += other.occupancy_area;
  full_cycles += other.full_cycles;
}

void TmRingConnStats::clear() { *this = TmRingConnStats(); }

void TmRingConnStats::merge_from(const TmRingConnStats& other) {
  packets += other.packets;
  bytes += other.bytes;
  busy_cycles += other.busy_cycles;
  downstream_register_full_stall += other.downstream_register_full_stall;
  serialization_busy_stall += other.serialization_busy_stall;
  pipeline_full_stall += other.pipeline_full_stall;
  send_reject_stall += other.send_reject_stall;
  inflight_peak = std::max(inflight_peak, other.inflight_peak);
}

void TmRingRbrgPathStats::clear() { *this = TmRingRbrgPathStats(); }

void TmRingRbrgPathStats::merge_from(const TmRingRbrgPathStats& other) {
  packets += other.packets;
  bytes += other.bytes;
  busy_cycles += other.busy_cycles;
  queue_occupancy_peak =
      std::max(queue_occupancy_peak, other.queue_occupancy_peak);
  queue_full_stalls += other.queue_full_stalls;
  destination_inject_stalls += other.destination_inject_stalls;
}

void TmRingRbrgStats::clear() { *this = TmRingRbrgStats(); }

void TmRingRbrgStats::merge_from(const TmRingRbrgStats& other) {
  for (uint32_t i = 0; i < paths.size(); ++i) {
    paths[i].merge_from(other.paths[i]);
  }
}

void TmRingL2BufferStats::clear() { *this = TmRingL2BufferStats(); }

void TmRingL2BufferStats::merge_from(const TmRingL2BufferStats& other) {
  responses_accepted += other.responses_accepted;
  dat_bytes += other.dat_bytes;
  buffer_occupancy_peak =
      std::max(buffer_occupancy_peak, other.buffer_occupancy_peak);
  buffer_full_stall_cycles += other.buffer_full_stall_cycles;
  latency_wait_cycles += other.latency_wait_cycles;
  issue_interval_stall_cycles += other.issue_interval_stall_cycles;
  dat_inject_full_stall_cycles += other.dat_inject_full_stall_cycles;
  h_carriers += other.h_carriers;
  h_unicast_carriers += other.h_unicast_carriers;
  h_multicast_carriers += other.h_multicast_carriers;
  h_scatter_carriers += other.h_scatter_carriers;
  h_carrier_recipients += other.h_carrier_recipients;
  injected_carrier_128b += other.injected_carrier_128b;
  injected_carrier_256b += other.injected_carrier_256b;
  injected_carrier_512b += other.injected_carrier_512b;
  injected_carrier_other += other.injected_carrier_other;
}

void TmRingHomeAgentStats::clear() { *this = TmRingHomeAgentStats(); }

void TmRingHomeAgentStats::merge_from(const TmRingHomeAgentStats& other) {
  rd_requests += other.rd_requests;
  rd_entries_allocated += other.rd_entries_allocated;
  rd_merged_pending += other.rd_merged_pending;
  rd_merged_inflight += other.rd_merged_inflight;
  rd_merged_responding += other.rd_merged_responding;
  rd_backend_issued += other.rd_backend_issued;
  backend_read_saved += other.backend_read_saved;
  l2_hit_transactions += other.l2_hit_transactions;
  l2_miss_transactions += other.l2_miss_transactions;
  functional_reads += other.functional_reads;
  private_l2_full_stall_cycles += other.private_l2_full_stall_cycles;
  table_full_stall_cycles += other.table_full_stall_cycles;
  waiter_full_stall_cycles += other.waiter_full_stall_cycles;
  aggregation_closed_stall_cycles += other.aggregation_closed_stall_cycles;
  write_hazard_stall_cycles += other.write_hazard_stall_cycles;
  completion_buffer_bytes_peak =
      std::max(completion_buffer_bytes_peak, other.completion_buffer_bytes_peak);
  useful_bytes += other.useful_bytes;
  backend_read_bytes += other.backend_read_bytes;
  for (uint32_t i = 0; i < completed_transaction_waiters.size(); ++i) {
    completed_transaction_waiters[i] += other.completed_transaction_waiters[i];
  }
}

void TmRingDeflectionStats::clear() { *this = TmRingDeflectionStats(); }

void TmRingDeflectionStats::merge_from(const TmRingDeflectionStats& other) {
  events += other.events;
  unique_packets += other.unique_packets;
  eligible_unicast_packets += other.eligible_unicast_packets;
  completed_packets += other.completed_packets;
  rounds_sum += other.rounds_sum;
  rounds_max = std::max(rounds_max, other.rounds_max);
  delay_cycles_sum += other.delay_cycles_sum;
  delay_cycles_max = std::max(delay_cycles_max, other.delay_cycles_max);
  fanout_recipient_retry_events += other.fanout_recipient_retry_events;
}

void TmRingCrossStationStats::clear() { *this = TmRingCrossStationStats(); }

void TmRingCrossStationStats::merge_from(
    const TmRingCrossStationStats& other) {
  transit_slots += other.transit_slots;
  injected_packets += other.injected_packets;
  ejected_packets += other.ejected_packets;
  inject_queue_full_stalls += other.inject_queue_full_stalls;
  eject_queue_full_stalls += other.eject_queue_full_stalls;
  slot_pool_full_stalls += other.slot_pool_full_stalls;
  for (uint32_t i = 0; i < deflection.size(); ++i) {
    deflection[i].merge_from(other.deflection[i]);
  }
  i_tag_sets += other.i_tag_sets;
  i_tag_claims += other.i_tag_claims;
  e_tag_sets += other.e_tag_sets;
  e_tag_claims += other.e_tag_claims;
  tagged_empty_slots += other.tagged_empty_slots;
}
