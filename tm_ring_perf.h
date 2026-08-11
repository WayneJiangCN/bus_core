#ifndef _TM_RING_PERF_H_
#define _TM_RING_PERF_H_

#include <stdint.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "tm_ring.h"

enum class TmRingPerfOp {
  READ,
  WRITE,
  READ_WRITE
};

enum class TmRingPerfPattern {
  SEQUENTIAL_PRIVATE,
  SEQUENTIAL_SHARED,
  SAME_LINE_SCATTER,
  STRIDED_PRIVATE,
  SINGLE_TARGET
};

enum class TmRingPerfRunMode {
  FREE_RUNNING,
  AGGREGATION_WAVE
};

enum class TmRingPerfAggregationModel {
  NO_MERGE,
  IDEAL_TRACE_MERGE
};

struct TmRingPerfCase {
  std::string name;
  TmRingPerfOp op = TmRingPerfOp::READ;
  TmRingPerfPattern pattern = TmRingPerfPattern::SEQUENTIAL_PRIVATE;
  uint32_t active_masters = 1;
  uint64_t bytes_per_master = 128 * 1024;
  uint32_t burst_bytes = 128;
  uint64_t read_base = 0;
  uint64_t write_base = 0x40000000ull;
  uint64_t stride_bytes = 128;
  uint32_t target_id = 0;
  uint64_t drain_cycle_limit = 200000;
  TmRingPerfRunMode run_mode = TmRingPerfRunMode::FREE_RUNNING;
  uint32_t max_aicore_per_vring = 0;
  uint32_t home_agent_waiters_per_entry = 0;
  uint32_t l2_response_latency = 0;
};

struct TmRingPerfTxn {
  uint32_t master_port = 0;
  PldCmd cmd = PldCmd::RD;
  uint64_t addr = 0;
  uint32_t size = 0;
  uint64_t ordinal = 0;
};

struct TmRingPerfMasterStats {
  uint64_t attempted_packets = 0;
  uint64_t accepted_packets = 0;
  uint64_t send_stall_cycles = 0;
  uint64_t completed_packets = 0;
  uint64_t completed_bytes = 0;
  uint64_t duplicate_responses = 0;
  uint64_t unknown_responses = 0;
  uint64_t outstanding_peak = 0;
  uint64_t first_request_cycle = 0;
  uint64_t first_response_cycle = 0;
  uint64_t last_response_cycle = 0;
  std::vector<uint64_t> latency_cycles;
  bool has_first_request = false;
  bool has_first_response = false;
};

struct TmRingPerfEdgeKey {
  TmRingPerfEdgeKey() {}
  TmRingPerfEdgeKey(TmRingDomainType ring_type_value, uint32_t ring_id_value,
                    TmRingSubnet subnet_value, uint32_t source_station,
                    TmRingPortDir direction_value)
      : ring_type(ring_type_value),
        ring_id(ring_id_value),
        subnet(subnet_value),
        src_station(source_station),
        direction(direction_value) {}

  TmRingDomainType ring_type = TmRingDomainType::V_RING;
  uint32_t ring_id = 0;
  TmRingSubnet subnet = TmRingSubnet::REQ;
  uint32_t src_station = 0;
  TmRingPortDir direction = TmRingPortDir::CW;

  bool operator==(const TmRingPerfEdgeKey& other) const;
  bool operator<(const TmRingPerfEdgeKey& other) const;
};

struct TmRingPerfRbrgKey {
  TmRingPerfRbrgKey() {}
  TmRingPerfRbrgKey(uint32_t id, TmRingRbrgPath path_value)
      : rbrg_id(id), path(path_value) {}

  uint32_t rbrg_id = 0;
  TmRingRbrgPath path = TmRingRbrgPath::V_TO_H_REQ;

  bool operator==(const TmRingPerfRbrgKey& other) const;
  bool operator<(const TmRingPerfRbrgKey& other) const;
};

struct TmRingPerfEstimate {
  uint64_t total_useful_bytes = 0;
  uint64_t physical_packets = 0;
  uint64_t logical_read_requests = 0;
  uint64_t backend_reads = 0;
  uint64_t backend_read_saved = 0;
  uint64_t h_carriers = 0;
  uint64_t h_unicast_carriers = 0;
  uint64_t h_multicast_carriers = 0;
  uint64_t h_scatter_carriers = 0;
  uint64_t h_carrier_recipients = 0;
  uint64_t v_carriers = 0;
  uint64_t fabric_min_cycles = 0;
  double fabric_model_ceiling_bpc = 0.0;
  TmRingPerfEdgeKey hottest_edge;
  uint64_t hottest_ring_edge_cycles = 0;
  std::map<TmRingPerfEdgeKey, uint64_t> edge_cycles;
  std::map<TmRingPerfRbrgKey, uint64_t> rbrg_path_cycles;
  uint64_t hottest_rbrg_path_cycles = 0;
};

struct TmRingPerfResult {
  TmRingPerfCase perf_case;
  uint64_t first_request_time = 0;
  uint64_t first_response_time = 0;
  uint64_t last_response_time = 0;
  uint64_t transfer_cycles = 0;
  uint64_t measurement_start_time = 0;
  uint64_t measurement_end_time = 0;
  uint64_t measurement_cycles = 0;
  bool measurement_valid = false;
  uint32_t ring_link_width_bytes = 0;
  uint32_t rbrg_width_bytes = 0;
  uint64_t completed_packets = 0;
  uint64_t completed_bytes = 0;
  uint64_t protocol_errors = 0;
  double end_to_end_bandwidth_bpc = 0.0;
  double steady_response_bandwidth_bpc = 0.0;
  double scaling_efficiency = 0.0;
  bool scaling_efficiency_available = false;
  double jain_fairness = 0.0;
  uint64_t latency_p50 = 0;
  uint64_t latency_p95 = 0;
  uint64_t latency_p99 = 0;
  uint64_t latency_max = 0;
  bool drained = false;
  TmRingPerfEstimate estimate;
  TmRingPerfEstimate no_merge_estimate;
  std::array<TmRingConnStats, 3> conn_stats;
  TmRingCrossStationStats cross_station_stats;
  TmRingHomeAgentStats home_agent_stats;
  TmRingL2BufferStats l2_buffer_stats;
  TmMemStats memory_stats;
  std::vector<TmRingDomainStats> ring_domain_stats;
  std::vector<TmRingRbrgStats> rbrg_stats;
  std::vector<TmRingEndpointQueueStats> endpoint_queue_stats;
};

std::vector<TmRingPerfTxn> tm_ring_build_perf_trace(
    const TmRingPerfCase& perf_case,
    uint32_t configured_masters,
    const TmRingTopology& topology);

TmRingPerfEstimate tm_ring_estimate_fabric(
    const std::vector<TmRingPerfTxn>& trace,
    const TmRingTopology& topology,
    const TmRingCfg& ring_cfg,
    TmRingPerfAggregationModel aggregation_model =
        TmRingPerfAggregationModel::IDEAL_TRACE_MERGE);

TmRingPerfResult tm_ring_collect_perf_result(
    const TmRingPerfCase& perf_case,
    const std::vector<TmRingPerfMasterStats>& master_stats,
    const TmRingFabric& fabric,
    const std::vector<TmMemStats>& memory_stats,
    const TmRingPerfEstimate& estimate,
    const TmRingPerfEstimate& no_merge_estimate,
    uint64_t measurement_end_cycle,
    bool drained);

double tm_ring_scaling_efficiency(const TmRingPerfResult& multi,
                                  const TmRingPerfResult& single);

#endif  // _TM_RING_PERF_H_
