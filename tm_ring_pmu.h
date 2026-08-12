#ifndef _TM_RING_PMU_H_
#define _TM_RING_PMU_H_

#include <array>
#include <memory>
#include <stdint.h>
#include <vector>

#include "tm_ring_fanout.h"
#include "tm_ring_stats.h"
#include "tm_ring_types.h"

class TmRingPmu;

enum class TmRingConnRejectReason : uint32_t {
  RETIRED_EVENT_PENDING,
  SERIALIZER_BUSY,
  PIPELINE_FULL,
};

enum class TmRingSlotKind : uint32_t { NORMAL, TAGGED_EMPTY };

enum class TmRingHaReadOutcome : uint32_t {
  STALL_WRITE_HAZARD,
  STALL_AGGREGATION_CLOSED,
  STALL_WAITER_FULL,
  STALL_TABLE_FULL,
  MERGED_PENDING,
  MERGED_INFLIGHT,
  MERGED_RESPONDING,
  ACCEPTED_L2_HIT,
  ACCEPTED_L2_MISS,
  BYPASS,
};

class TmRingQueuePmuPort {
 public:
  void push_accepted(uint64_t cycle, uint32_t occupancy_after) const;
  void push_rejected(uint64_t cycle, uint32_t occupancy_current) const;
  void popped(uint64_t cycle, uint32_t occupancy_after) const;

 private:
  friend class TmRingPmu;
  TmRingQueuePmuPort(TmRingPmu* pmu, uint32_t id);
  TmRingPmu* pmu_;
  uint32_t id_;
};

class TmRingConnPmuPort {
 public:
  void accepted(TmRingSubnet subnet, uint32_t bytes,
                uint32_t serialization_cycles, uint32_t inflight_after) const;
  void rejected(TmRingSubnet subnet, TmRingConnRejectReason reason) const;
  void downstream_blocked(TmRingSubnet subnet) const;

 private:
  friend class TmRingPmu;
  TmRingConnPmuPort(TmRingPmu* pmu, uint32_t id);
  TmRingPmu* pmu_;
  uint32_t id_;
};

class TmRingCrossStationPmuPort {
 public:
  void transit_committed(TmRingSubnet subnet, TmRingSlotKind slot_kind) const;
  void packet_injected(TmRingSubnet subnet) const;
  void packet_ejected(TmRingSubnet subnet, p_tm_pld_t slot,
                      uint64_t cycle) const;
  void packet_deflected(TmRingSubnet subnet, p_tm_pld_t slot,
                        uint64_t cycle, bool fanout_retry) const;
  void slot_pool_blocked(TmRingSubnet subnet, TmRingPortDir direction) const;
  void i_tag_set(TmRingSubnet subnet) const;
  void i_tag_claimed(TmRingSubnet subnet) const;
  void e_tag_set(TmRingSubnet subnet) const;
  void e_tag_claimed(TmRingSubnet subnet) const;

 private:
  friend class TmRingPmu;
  TmRingCrossStationPmuPort(TmRingPmu* pmu, uint32_t id);
  TmRingPmu* pmu_;
  uint32_t id_;
};

class TmRingRbrgPmuPort {
 public:
  void enqueued(TmRingRbrgPath path, uint32_t serialization_cycles) const;
  void queue_blocked(TmRingRbrgPath path) const;
  void destination_blocked(TmRingRbrgPath path) const;
  void delivered(TmRingRbrgPath path, uint32_t bytes) const;

 private:
  friend class TmRingPmu;
  TmRingRbrgPmuPort(TmRingPmu* pmu, uint32_t id);
  TmRingPmu* pmu_;
  uint32_t id_;
};

class TmRingHaPmuPort {
 public:
  void read_admission(uint32_t master_id, uint32_t bytes,
                      TmRingHaReadOutcome outcome) const;
  void backend_read_issued(uint32_t bytes) const;
  void functional_read_completed() const;
  void private_l2_blocked() const;
  void completion_buffer_sample(uint32_t bytes) const;
  void transaction_completed(uint32_t waiter_count) const;
  void write_reservation_blocked() const;
  void source_request_received(uint32_t master_id, PldCmd cmd) const;

 private:
  friend class TmRingPmu;
  TmRingHaPmuPort(TmRingPmu* pmu, uint32_t id);
  TmRingPmu* pmu_;
  uint32_t id_;
};

class TmRingL2PmuPort {
 public:
  void response_admitted(TmRingL2AcceptStatus result,
                         uint32_t occupancy_after,
                         uint32_t response_latency) const;
  void buffer_blocked() const;
  void issue_interval_blocked() const;
  void dat_inject_blocked() const;
  void carrier_injected(uint32_t bytes, uint32_t recipient_count,
                        TmRingFanoutMode fanout_mode) const;

 private:
  friend class TmRingPmu;
  TmRingL2PmuPort(TmRingPmu* pmu, uint32_t id);
  TmRingPmu* pmu_;
  uint32_t id_;
};

struct TmRingConnStallBreakdown {
  uint64_t serialization_busy = 0;
  uint64_t pipeline_full = 0;
  uint64_t downstream_register_full = 0;

  uint64_t total() const {
    return serialization_busy + pipeline_full + downstream_register_full;
  }
};

struct TmRingConnHotspot {
  uint32_t src_station = 0;
  TmRingPortDir src_dir = TmRingPortDir::LOCAL;
  uint32_t dst_station = 0;
  TmRingPortDir dst_dir = TmRingPortDir::LOCAL;
  TmRingSubnet subnet = TmRingSubnet::REQ;
  uint64_t packets = 0;
  uint64_t bytes = 0;
  uint64_t busy_cycles = 0;
  uint64_t serialization_busy_stall = 0;
  uint64_t total_stalls = 0;
  uint32_t inflight_peak = 0;
};

struct TmRingDomainStats {
  TmRingDomainType type = TmRingDomainType::V_RING;
  uint32_t ring_id = 0;
  std::array<TmRingConnStats, 3> cw;
  std::array<TmRingConnStats, 3> ccw;
  TmRingConnHotspot hottest;
  uint32_t directed_edge_count = 0;
  std::vector<TmRingConnHotspot> edges;
  TmRingCrossStationStats cross_station;
};

struct TmRingQueuePmuSnapshot {
  std::vector<TmRingEndpointQueueStats> endpoints;
};

struct TmRingConnPmuSnapshot {
  std::array<TmRingConnStats, 3> total;
  std::vector<TmRingDomainStats> domains;
};

struct TmRingCrossStationPmuSnapshot {
  TmRingCrossStationStats total;
};

struct TmRingRbrgPmuSnapshot {
  std::vector<TmRingRbrgStats> instances;
};

struct TmRingHaPmuSnapshot {
  TmRingHomeAgentStats total;
  std::vector<TmRingHaSourceStats> sources;
};

struct TmRingL2PmuSnapshot {
  TmRingL2BufferStats total;
};

struct TmRingPmuSnapshot {
  uint64_t reset_cycle = 0;
  TmRingQueuePmuSnapshot queue;
  TmRingConnPmuSnapshot conn;
  TmRingCrossStationPmuSnapshot cross_station;
  TmRingRbrgPmuSnapshot rbrg;
  TmRingHaPmuSnapshot ha;
  TmRingL2PmuSnapshot l2;

  TmRingConnStallBreakdown conn_stall_breakdown() const;
  std::vector<TmRingConnHotspot> top_busy_conns(TmRingSubnet subnet,
                                                 uint32_t limit) const;
  const TmRingRbrgPathStats& rbrg_path_stats(uint32_t rbrg_id,
                                              TmRingRbrgPath path) const;
};

class TmRingPmu {
 public:
  class Impl;
  TmRingPmu();
  ~TmRingPmu();

  std::vector<TmRingQueuePmuPort> register_endpoint_queues(
      TmRingNodeType node_type, uint32_t node_id,
      const TmRingEndpointQueueDepths& depths);
  TmRingConnPmuPort register_conn(TmRingDomainType domain, uint32_t ring_id,
                                  uint32_t src_station, TmRingPortDir src_dir,
                                  uint32_t dst_station, TmRingPortDir dst_dir);
  TmRingCrossStationPmuPort register_cross_station(TmRingDomainType domain,
                                                    uint32_t ring_id,
                                                    uint32_t station_id);
  TmRingRbrgPmuPort register_rbrg(uint32_t rbrg_id);
  TmRingHaPmuPort register_home_agent(uint32_t ha_id, uint32_t master_count);
  TmRingL2PmuPort register_l2(uint32_t target_id);
  void reset_model(uint64_t cycle);
  TmRingPmuSnapshot snapshot(uint64_t cycle) const;

 private:
  friend class TmRingQueuePmuPort;
  friend class TmRingConnPmuPort;
  friend class TmRingCrossStationPmuPort;
  friend class TmRingRbrgPmuPort;
  friend class TmRingHaPmuPort;
  friend class TmRingL2PmuPort;
  void queue_push_accepted(uint32_t id, uint64_t cycle,
                           uint32_t occupancy_after);
  void queue_push_rejected(uint32_t id, uint64_t cycle,
                           uint32_t occupancy_current);
  void queue_popped(uint32_t id, uint64_t cycle, uint32_t occupancy_after);
  void conn_accepted(uint32_t id, TmRingSubnet subnet, uint32_t bytes,
                     uint32_t serialization_cycles, uint32_t inflight_after);
  void conn_rejected(uint32_t id, TmRingSubnet subnet,
                     TmRingConnRejectReason reason);
  void conn_downstream_blocked(uint32_t id, TmRingSubnet subnet);
  void cross_transit_committed(uint32_t id, TmRingSubnet subnet,
                               TmRingSlotKind slot_kind);
  void cross_packet_injected(uint32_t id, TmRingSubnet subnet);
  void cross_packet_ejected(uint32_t id, TmRingSubnet subnet, p_tm_pld_t slot,
                            uint64_t cycle);
  void cross_packet_deflected(uint32_t id, TmRingSubnet subnet,
                              p_tm_pld_t slot, uint64_t cycle,
                              bool fanout_retry);
  void cross_slot_pool_blocked(uint32_t id, TmRingSubnet subnet,
                               TmRingPortDir direction);
  void cross_i_tag_set(uint32_t id, TmRingSubnet subnet);
  void cross_i_tag_claimed(uint32_t id, TmRingSubnet subnet);
  void cross_e_tag_set(uint32_t id, TmRingSubnet subnet);
  void cross_e_tag_claimed(uint32_t id, TmRingSubnet subnet);
  void rbrg_enqueued(uint32_t id, TmRingRbrgPath path,
                     uint32_t serialization_cycles);
  void rbrg_queue_blocked(uint32_t id, TmRingRbrgPath path);
  void rbrg_destination_blocked(uint32_t id, TmRingRbrgPath path);
  void rbrg_delivered(uint32_t id, TmRingRbrgPath path, uint32_t bytes);
  void ha_read_admission(uint32_t id, uint32_t master_id, uint32_t bytes,
                         TmRingHaReadOutcome outcome);
  void ha_backend_read_issued(uint32_t id, uint32_t bytes);
  void ha_functional_read_completed(uint32_t id);
  void ha_private_l2_blocked(uint32_t id);
  void ha_completion_buffer_sample(uint32_t id, uint32_t bytes);
  void ha_transaction_completed(uint32_t id, uint32_t waiter_count);
  void ha_write_reservation_blocked(uint32_t id);
  void ha_source_request_received(uint32_t id, uint32_t master_id,
                                  PldCmd cmd);
  void l2_response_admitted(uint32_t id, TmRingL2AcceptStatus result,
                            uint32_t occupancy_after,
                            uint32_t response_latency);
  void l2_buffer_blocked(uint32_t id);
  void l2_issue_interval_blocked(uint32_t id);
  void l2_dat_inject_blocked(uint32_t id);
  void l2_carrier_injected(uint32_t id, uint32_t bytes,
                           uint32_t recipient_count,
                           TmRingFanoutMode fanout_mode);
  std::unique_ptr<Impl> impl_;
};

#endif  // _TM_RING_PMU_H_
