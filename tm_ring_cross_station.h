#ifndef _TM_RING_CROSS_STATION_H_
#define _TM_RING_CROSS_STATION_H_

#include <stdint.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_pld.h"
#include "tm_que.h"
#include "tm_ring_conn.h"
#include "tm_ring_node_interface.h"
#include "tm_ring_slot_pool.h"
#include "tm_ring_types.h"

// Ring stop slot scheduler. It does not decode BIU or memory commands.
class TmRingCrossStation : public tm_engine::TmModule {
 public:
  TmRingCrossStation(const std::string& name, tm_engine::p_tm_clk_t clk);

  void reset();
  void clear_stats();
  bool idle() const;

  void attach(uint32_t station_id, p_tm_ring_conn_t cw_out_conn,
              p_tm_ring_conn_t ccw_out_conn,
              p_tm_ring_slot_pool_t slot_pool);
  void bind_node_interface(p_tm_ring_node_interface_t node_interface);

  p_tm_com_que_t transit_in_reg(TmRingPortDir in_dir,
                                TmRingSubnet subnet) const;
  const TmRingCrossStationStats& stats() const;

 private:
  using OutputUsed = std::array<bool, 2>;
  enum class OutputSource : uint32_t {
    NONE = 0,
    TRANSIT = 1,
    LOCAL = 2,
  };

  void schedule_req();
  void schedule_rsp();
  void schedule_dat();
  void schedule_subnet(TmRingSubnet subnet);
  void schedule_output(TmRingSubnet subnet, TmRingPortDir out_dir,
                       OutputUsed& output_used);
  OutputSource process_transit(TmRingPortDir in_dir, TmRingSubnet subnet,
                               OutputUsed& output_used);
  OutputSource process_fanout_transit(TmRingPortDir in_dir,
                                      TmRingSubnet subnet,
                                      OutputUsed& output_used);
  OutputSource try_normal_injection(TmRingSubnet subnet,
                                    TmRingPortDir out_dir,
                                    OutputUsed& output_used);
  bool try_slot_replacement(p_tm_pld_t transit_slot, TmRingSubnet subnet,
                            TmRingPortDir out_dir, bool tag_required);
  bool try_preserve_or_replace_i_tag(p_tm_pld_t slot, TmRingSubnet subnet,
                                     TmRingPortDir out_dir,
                                     OutputUsed& output_used,
                                     bool* replacement_sent);
  bool forward_slot(p_tm_pld_t slot, TmRingPortDir out_dir);

  bool can_eject(p_tm_pld_t slot, TmRingSubnet subnet) const;
  bool owns_e_tag(p_tm_pld_t slot, TmRingSubnet subnet) const;
  bool mark_e_tag_for_forward(p_tm_pld_t slot, TmRingSubnet subnet);
  void rollback_e_tag_mark(p_tm_pld_t slot, bool marked);
  void claim_e_tag(p_tm_pld_t slot, TmRingSubnet subnet);
  void commit_eject(p_tm_pld_t slot, TmRingSubnet subnet);
  void commit_fanout_eject(p_tm_pld_t envelope, p_tm_pld_t response,
                           TmRingSubnet subnet);

  p_tm_com_que_t transit_reg(TmRingPortDir in_dir, TmRingSubnet subnet) const;
  p_tm_ring_conn_t output_conn(TmRingPortDir out_dir) const;

  uint32_t destination_node(p_tm_pld_t slot) const;
  bool is_destination(p_tm_pld_t slot) const;
  bool fanout_recipient_for_station(p_tm_pld_t slot,
                                    size_t* recipient_index) const;
  p_tm_pld_t make_fanout_forward(p_tm_pld_t slot,
                                 size_t recipient_index) const;
  p_tm_pld_t make_fanout_response(p_tm_pld_t envelope,
                                  size_t recipient_index) const;
  TmRingPortDir slot_direction(p_tm_pld_t slot) const;
  uint32_t direction_index(TmRingPortDir dir) const;
  bool transit_waiting_for_output(TmRingSubnet subnet,
                                  TmRingPortDir out_dir) const;
  bool normal_injection_ready(TmRingSubnet subnet,
                              TmRingPortDir out_dir) const;
  bool local_waiting_for_output(TmRingSubnet subnet,
                                 TmRingPortDir out_dir) const;
  void release_ring_slot(TmRingSubnet subnet, TmRingPortDir out_dir);
  p_tm_pld_t make_tagged_empty_slot(p_tm_pld_t source) const;

  // Node Interface owns directional Inject Banks and Eject Queues.
  p_tm_ring_node_interface_t node_interface_ = nullptr;
  // [subnet][incoming CW/CCW] depth-1 Reg_In state.
  std::array<std::array<p_tm_com_que_t, 2>, 3> transit_regs_;
  // One outstanding I-tag protects each directional Inject Bank head.
  std::array<std::array<bool, 2>, 3> i_tag_pending_;
  std::array<std::array<OutputSource, 2>, 3> next_output_source_;
  std::array<bool, 3> e_tag_reserved_;
  std::vector<TmPldTxnKey> e_tag_txn_keys_;

  uint32_t station_id_ = 0;
  p_tm_ring_conn_t cw_out_conn_ = nullptr;
  p_tm_ring_conn_t ccw_out_conn_ = nullptr;
  p_tm_ring_slot_pool_t slot_pool_ = nullptr;
  TmRingCrossStationStats stats_;
};

using tm_ring_cs_t = TmRingCrossStation;
using p_tm_ring_cs_t = std::shared_ptr<tm_ring_cs_t>;

inline p_tm_ring_cs_t tm_make_ring_cs(const std::string& name,
                                      tm_engine::p_tm_clk_t clk) {
  return std::make_shared<TmRingCrossStation>(name, clk);
}

#endif  // _TM_RING_CROSS_STATION_H_
