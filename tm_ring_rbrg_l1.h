#ifndef _TM_RING_RBRG_L1_H_
#define _TM_RING_RBRG_L1_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_ring_node_interface.h"
#include "tm_ring_pmu.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"

class TmRingRbrgL1 : public tm_engine::TmModule {
 public:
  TmRingRbrgL1(const std::string& name, tm_engine::p_tm_clk_t clk,
               uint32_t v_ring_id, uint32_t queue_depth,
               uint32_t latency, uint32_t width_bytes,
               const TmRingRbrgPmuPort& pmu,
               const TmRingEndpointQueueDepths& v_queue_depths,
               const TmRingEndpointQueueDepths& h_queue_depths,
               const std::vector<TmRingQueuePmuPort>& v_queue_pmu_ports,
               const std::vector<TmRingQueuePmuPort>& h_queue_pmu_ports,
               std::shared_ptr<TmRingTopology> topology);

  void reset() override;
  bool idle() const;
  p_tm_ring_node_interface_t v_node_interface() const;
  p_tm_ring_node_interface_t h_node_interface() const;

 private:
  struct DirectionState {
    tm_engine::p_tm_event_t bandwidth_ready_event = nullptr;
    tm_engine::p_tm_event_t serializer_handoff_event = nullptr;
    p_tm_pld_t serializer_slot = nullptr;
    bool bandwidth_available = true;
    bool bandwidth_event_pending = false;
    bool retired_bandwidth_event_pending = false;
    bool serializer_handoff_event_pending = false;
    bool retired_serializer_handoff_event_pending = false;
  };

  struct PathState {
    std::array<DirectionState, 2> directions;
    TmRingPortDir next_input = TmRingPortDir::CW;
  };

  struct HeadCandidate {
    bool valid = false;
    TmRingPortDir input = TmRingPortDir::CW;
    TmRingPortDir preferred = TmRingPortDir::CW;
    bool equal_fanout_span = false;
    p_tm_pld_t pld = nullptr;
  };

  struct Assignment {
    std::array<int32_t, 2> output = {{-1, -1}};
    uint32_t transfers = 0;
    uint32_t preferred_transfers = 0;
  };

  void recv_v_req();
  void recv_v_dat();
  void recv_h_rsp();
  void recv_h_dat();
  void release_v_req_cw_bandwidth();
  void release_v_req_ccw_bandwidth();
  void release_v_dat_cw_bandwidth();
  void release_v_dat_ccw_bandwidth();
  void release_h_rsp_cw_bandwidth();
  void release_h_rsp_ccw_bandwidth();
  void release_h_dat_cw_bandwidth();
  void release_h_dat_ccw_bandwidth();
  void handoff_v_req_cw_serializer();
  void handoff_v_req_ccw_serializer();
  void handoff_v_dat_cw_serializer();
  void handoff_v_dat_ccw_serializer();
  void handoff_h_rsp_cw_serializer();
  void handoff_h_rsp_ccw_serializer();
  void handoff_h_dat_cw_serializer();
  void handoff_h_dat_ccw_serializer();

  void release_bandwidth(TmRingRbrgPath path, TmRingPortDir direction);
  void handoff_serializer(TmRingRbrgPath path, TmRingPortDir direction);
  void schedule_path(TmRingRbrgPath path);
  void start_serializer(TmRingRbrgPath path,
                        const HeadCandidate& candidate,
                        TmRingPortDir output);
  HeadCandidate head_candidate(TmRingRbrgPath path,
                               TmRingPortDir input) const;
  TmRingPortDir preferred_direction(TmRingRbrgPath path, p_tm_pld_t pld,
                                    bool* equal_fanout_span) const;
  void prepare_h_segment(p_tm_pld_t pld, TmRingPortDir direction);
  void prepare_v_segment(p_tm_pld_t pld, TmRingPortDir direction);
  void clear_ring_local_state(p_tm_pld_t pld);

  uint32_t path_index(TmRingRbrgPath path) const;
  uint32_t direction_index(TmRingPortDir direction) const;
  p_tm_ring_node_interface_t source_for(TmRingRbrgPath path) const;
  p_tm_ring_node_interface_t destination_for(TmRingRbrgPath path) const;
  TmRingSubnet subnet_for(TmRingRbrgPath path) const;

  uint32_t v_ring_id_ = 0;
  uint32_t rbrg_width_bytes_ = 1;
  TmRingRbrgPmuPort pmu_;
  std::shared_ptr<TmRingTopology> topology_ = nullptr;
  p_tm_ring_node_interface_t v_niu_ = nullptr;
  p_tm_ring_node_interface_t h_niu_ = nullptr;
  std::array<PathState, 4> paths_;
  TmRingPortDir fanout_tie_next_direction_ = TmRingPortDir::CW;
};

using tm_ring_rbrg_l1_t = TmRingRbrgL1;
using p_tm_ring_rbrg_l1_t = std::shared_ptr<tm_ring_rbrg_l1_t>;

inline p_tm_ring_rbrg_l1_t tm_make_ring_rbrg_l1(
    const std::string& name, tm_engine::p_tm_clk_t clk, uint32_t v_ring_id,
    uint32_t queue_depth, uint32_t latency, uint32_t width_bytes,
    const TmRingRbrgPmuPort& pmu,
    const TmRingEndpointQueueDepths& v_queue_depths,
    const TmRingEndpointQueueDepths& h_queue_depths,
    const std::vector<TmRingQueuePmuPort>& v_queue_pmu_ports,
    const std::vector<TmRingQueuePmuPort>& h_queue_pmu_ports,
    std::shared_ptr<TmRingTopology> topology) {
  return std::shared_ptr<TmRingRbrgL1>(
      new TmRingRbrgL1(name, clk, v_ring_id, queue_depth, latency,
                       width_bytes, pmu, v_queue_depths, h_queue_depths,
                       v_queue_pmu_ports, h_queue_pmu_ports, topology));
}

#endif  // _TM_RING_RBRG_L1_H_
