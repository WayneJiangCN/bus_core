#ifndef _TM_RING_L2_BUFFER_NODE_H_
#define _TM_RING_L2_BUFFER_NODE_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "tm_engine.h"
#include "tm_que.h"
#include "tm_ring_fanout.h"
#include "tm_ring_node_interface.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"

// L2 Buffer 只建模 Ring DAT 出口的物理暂存、fanout 组装和发送节奏，
// 不保存持久 cache 数据。
class TmRingL2BufferNode : public tm_engine::TmModule {
 public:
  TmRingL2BufferNode(const std::string& name, tm_engine::p_tm_clk_t clk,
                     const TmRingL2TrafficConfig& cfg,
                     uint32_t inject_depth, uint32_t eject_depth);

  void attach(uint32_t target_id,
              std::shared_ptr<TmRingTopology> topology);
  void reset();
  void clear_stats();
  bool idle() const;
  TmRingL2AcceptResult accept_response(
      const TmRingL2ResponseCandidate& candidate);
  std::vector<TmRingL2GroupSummary> take_frozen_summaries();

  p_tm_ring_node_interface_t node_interface() const;
  const TmRingL2BufferStats& stats() const;

 private:
  void service_ready_response();
  void release_issue_token();
  void service();
  void prepare_dat_route(p_tm_pld_t rsp);
  bool has_capacity();
  bool fanout_candidate_valid(
      const TmRingL2ResponseCandidate& candidate) const;
  uint64_t allocate_group_token();
  bool append_fanout_recipient(
      p_tm_pld_t envelope, const TmRingL2ResponseCandidate& candidate);
  p_tm_pld_t make_unicast_response(
      const TmRingL2ResponseCandidate& candidate) const;
  p_tm_pld_t make_fanout_envelope(
      const TmRingL2ResponseCandidate& candidate,
      uint64_t group_token) const;
  void freeze_fanout_group(p_tm_pld_t envelope);
  p_tm_pld_t make_frozen_carrier(p_tm_pld_t envelope,
                                 uint32_t target_vring) const;
  bool materialize_fanout(p_tm_pld_t carrier) const;
  uint32_t carrier_size(p_tm_pld_t envelope,
                        uint32_t* carrier_offset) const;
  void record_accepted_entry();

  TmRingL2TrafficConfig cfg_;
  uint32_t target_id_ = 0;
  std::shared_ptr<TmRingTopology> topology_ = nullptr;
  p_tm_ring_node_interface_t node_interface_ = nullptr;
  p_tm_com_que_t response_q_ = nullptr;
  tm_engine::p_tm_event_t issue_ready_event_ = nullptr;
  bool issue_ready_event_pending_ = false;
  bool retired_issue_ready_event_pending_ = false;
  uint32_t response_count_ = 0;
  uint32_t ready_response_count_ = 0;
  bool issue_token_available_ = true;
  std::unordered_map<uint64_t, p_tm_pld_t> open_groups_;
  p_tm_pld_t frozen_summary_response_ = nullptr;
  TmRingL2GroupSummary frozen_summary_;
  std::vector<TmRingL2GroupSummary> frozen_summaries_;
  p_tm_pld_t frozen_carrier_ = nullptr;
  uint32_t frozen_carrier_vring_ = 0;
  uint64_t next_group_token_ = 0;
  TmRingL2BufferStats stats_;
};

using tm_ring_l2_buffer_node_t = TmRingL2BufferNode;
using p_tm_ring_l2_buffer_node_t = std::shared_ptr<tm_ring_l2_buffer_node_t>;

inline p_tm_ring_l2_buffer_node_t tm_make_ring_l2_buffer_node(
    const std::string& name, tm_engine::p_tm_clk_t clk,
    const TmRingL2TrafficConfig& cfg, uint32_t inject_depth,
    uint32_t eject_depth) {
  return std::make_shared<TmRingL2BufferNode>(
      name, clk, cfg, inject_depth, eject_depth);
}

#endif  // _TM_RING_L2_BUFFER_NODE_H_
