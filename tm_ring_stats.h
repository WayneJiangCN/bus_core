#ifndef _TM_RING_STATS_H_
#define _TM_RING_STATS_H_

#include <array>
#include <stdint.h>

struct TmRingConnStats {
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t busy_cycles = 0;
  uint64_t downstream_register_full_stall = 0;
  uint64_t serialization_busy_stall = 0;
  uint64_t pipeline_full_stall = 0;
  uint64_t send_reject_stall = 0;
  uint32_t inflight_peak = 0;

  void clear();
  void merge_from(const TmRingConnStats& other);
};

inline uint64_t tm_ring_conn_total_stalls(const TmRingConnStats& stats) {
  return stats.downstream_register_full_stall +
         stats.serialization_busy_stall + stats.pipeline_full_stall +
         stats.send_reject_stall;
}

struct TmRingRbrgPathStats {
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t queue_occupancy_peak = 0;
  uint64_t queue_full_stalls = 0;
  uint64_t destination_inject_stalls = 0;

  void clear();
  void merge_from(const TmRingRbrgPathStats& other);
};

struct TmRingRbrgStats {
  std::array<TmRingRbrgPathStats, 4> paths;

  void clear();
  void merge_from(const TmRingRbrgStats& other);
};

struct TmRingL2BufferStats {
  uint64_t responses_accepted = 0;
  uint64_t dat_bytes = 0;
  uint64_t buffer_occupancy_peak = 0;
  uint64_t buffer_full_stall_cycles = 0;
  uint64_t latency_wait_cycles = 0;
  uint64_t issue_interval_stall_cycles = 0;
  uint64_t dat_inject_full_stall_cycles = 0;
  uint64_t h_carriers = 0;
  uint64_t h_unicast_carriers = 0;
  uint64_t h_multicast_carriers = 0;
  uint64_t h_scatter_carriers = 0;
  uint64_t h_carrier_recipients = 0;
  uint64_t injected_carrier_128b = 0;
  uint64_t injected_carrier_256b = 0;
  uint64_t injected_carrier_512b = 0;
  uint64_t injected_carrier_other = 0;

  void clear();
  void merge_from(const TmRingL2BufferStats& other);
};

struct TmRingHomeAgentStats {
  uint64_t rd_requests = 0;
  uint64_t rd_entries_allocated = 0;
  uint64_t rd_merged_pending = 0;
  uint64_t rd_merged_inflight = 0;
  uint64_t rd_merged_responding = 0;
  uint64_t rd_backend_issued = 0;
  uint64_t backend_read_saved = 0;
  uint64_t l2_hit_transactions = 0;
  uint64_t l2_miss_transactions = 0;
  uint64_t functional_reads = 0;
  uint64_t private_l2_full_stall_cycles = 0;
  uint64_t table_full_stall_cycles = 0;
  uint64_t waiter_full_stall_cycles = 0;
  uint64_t aggregation_closed_stall_cycles = 0;
  uint64_t write_hazard_stall_cycles = 0;
  uint64_t completion_buffer_bytes_peak = 0;
  uint64_t useful_bytes = 0;
  uint64_t backend_read_bytes = 0;
  std::array<uint64_t, 65> completed_transaction_waiters{};

  void clear();
  void merge_from(const TmRingHomeAgentStats& other);
};

struct TmRingCrossStationStats {
  uint64_t transit_slots = 0;
  uint64_t injected_packets = 0;
  uint64_t ejected_packets = 0;
  uint64_t inject_queue_full_stalls = 0;
  uint64_t eject_queue_full_stalls = 0;
  uint64_t slot_pool_full_stalls = 0;
  uint64_t deflected_packets = 0;
  uint64_t i_tag_sets = 0;
  uint64_t i_tag_claims = 0;
  uint64_t e_tag_sets = 0;
  uint64_t e_tag_claims = 0;
  uint64_t tagged_empty_slots = 0;

  void clear();
  void merge_from(const TmRingCrossStationStats& other);
};

#endif  // _TM_RING_STATS_H_
