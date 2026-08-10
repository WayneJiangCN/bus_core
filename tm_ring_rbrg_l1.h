#ifndef _TM_RING_RBRG_L1_H_
#define _TM_RING_RBRG_L1_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <string>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_que.h"
#include "tm_ring_node_interface.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"

enum class TmRingRbrgPath : uint32_t {
  V_TO_H_REQ = 0,
  V_TO_H_DAT = 1,
  H_TO_V_RSP = 2,
  H_TO_V_DAT = 3,
};

class TmRingRbrgL1 : public tm_engine::TmModule {
 public:
  TmRingRbrgL1(const std::string& name, tm_engine::p_tm_clk_t clk,
               uint32_t v_ring_id, const TmRingCfg& cfg,
               std::shared_ptr<TmRingTopology> topology);

  void reset() override;
  bool idle() const;
  void clear_stats();
  p_tm_ring_node_interface_t v_node_interface() const;
  p_tm_ring_node_interface_t h_node_interface() const;
  const TmRingRbrgStats& stats() const;
  const TmRingRbrgPathStats& path_stats(TmRingRbrgPath path) const;

 private:
  struct PathState {
    p_tm_com_que_t transfer_q = nullptr;
    tm_engine::p_tm_event_t bandwidth_ready_event = nullptr;
    bool bandwidth_available = true;
    bool bandwidth_event_pending = false;
    bool retired_bandwidth_event_pending = false;
    uint64_t queue_occupancy = 0;
  };

  void recv_v_req();
  void recv_v_dat();
  void recv_h_rsp();
  void recv_h_dat();
  void send_v_req();
  void send_v_dat();
  void send_h_rsp();
  void send_h_dat();
  void release_v_req_bandwidth();
  void release_v_dat_bandwidth();
  void release_h_rsp_bandwidth();
  void release_h_dat_bandwidth();

  void receive(TmRingRbrgPath path, TmRingSubnet subnet,
               const p_tm_ring_node_interface_t& source);
  void send(TmRingRbrgPath path, TmRingSubnet subnet,
            const p_tm_ring_node_interface_t& destination);
  void release_bandwidth(TmRingRbrgPath path);
  void prepare_h_segment(p_tm_pld_t pld);
  void prepare_v_segment(p_tm_pld_t pld);
  void clear_ring_local_state(p_tm_pld_t pld);

  uint32_t path_index(TmRingRbrgPath path) const;
  p_tm_ring_node_interface_t source_for(TmRingRbrgPath path) const;
  TmRingSubnet subnet_for(TmRingRbrgPath path) const;

  uint32_t v_ring_id_ = 0;
  uint32_t rbrg_width_bytes_ = 1;
  std::shared_ptr<TmRingTopology> topology_ = nullptr;
  p_tm_ring_node_interface_t v_niu_ = nullptr;
  p_tm_ring_node_interface_t h_niu_ = nullptr;
  std::array<PathState, 4> paths_;
  TmRingRbrgStats stats_;
};

using tm_ring_rbrg_l1_t = TmRingRbrgL1;
using p_tm_ring_rbrg_l1_t = std::shared_ptr<tm_ring_rbrg_l1_t>;

inline p_tm_ring_rbrg_l1_t tm_make_ring_rbrg_l1(
    const std::string& name, tm_engine::p_tm_clk_t clk, uint32_t v_ring_id,
    const TmRingCfg& cfg, std::shared_ptr<TmRingTopology> topology) {
  return std::shared_ptr<TmRingRbrgL1>(
      new TmRingRbrgL1(name, clk, v_ring_id, cfg, topology));
}

#endif  // _TM_RING_RBRG_L1_H_
