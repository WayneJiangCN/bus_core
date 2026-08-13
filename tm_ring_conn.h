#ifndef _TM_RING_CONN_H_
#define _TM_RING_CONN_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_que.h"
#include "tm_ring_pmu.h"
#include "tm_ring_types.h"

// One directed hop between adjacent Cross Stations.
class TmRingConn : public tm_engine::TmModule {
 public:
  TmRingConn(const std::string& name, tm_engine::p_tm_clk_t clk,
             uint32_t latency, uint32_t width_bytes,
             uint32_t dst_station, TmRingPortDir dst_dir,
             TmRingConnPmuPort pmu);
  void reset();
  bool idle() const;

  bool accept_slot(p_tm_pld_t slot);
  bool has_ready_slot(TmRingSubnet subnet);
  void attach(p_tm_com_que_t dst_req_transit_reg,
              p_tm_com_que_t dst_rsp_transit_reg,
              p_tm_com_que_t dst_dat_transit_reg);
  uint32_t dst_station() const;
  TmRingPortDir dst_dir() const;

 private:
  friend class TmRingConnTestAccess;

  bool can_accept(p_tm_pld_t slot,
                  TmRingConnRejectReason* reject_reason) const;
  void reserve_slot(p_tm_pld_t slot);
  void drain_ready_slots();
  void release_serializer(TmRingSubnet subnet);
  void move_serializer_to_pipeline(TmRingSubnet subnet);
  void release_req_serializer();
  void release_rsp_serializer();
  void release_dat_serializer();
  void move_req_serializer_to_pipeline();
  void move_rsp_serializer_to_pipeline();
  void move_dat_serializer_to_pipeline();
  TmRingSubnet slot_subnet(p_tm_pld_t slot) const;

  uint32_t latency_ = 1;
  uint32_t pipeline_depth_ = 1;
  uint32_t width_bytes_ = 16;
  uint32_t dst_station_ = 0;
  TmRingPortDir dst_dir_ = TmRingPortDir::LOCAL;
  std::vector<uint32_t> inflight_count_;
  std::vector<p_tm_pld_t> serializer_slots_;
  std::vector<bool> serializer_available_;
  std::vector<bool> serializer_release_event_pending_;
  std::vector<bool> serializer_to_pipeline_event_pending_;
  std::vector<bool> retired_serializer_release_event_pending_;
  std::vector<bool> retired_serializer_to_pipeline_event_pending_;
  std::vector<tm_engine::p_tm_event_t> serializer_release_events_;
  std::vector<tm_engine::p_tm_event_t> serializer_to_pipeline_events_;
  // Fixed-latency stages, not a Cross Station transit FIFO.
  std::vector<p_tm_com_que_t> slot_pipelines_;
  TmRingConnPmuPort pmu_;
  p_tm_com_que_t dst_req_transit_reg_ = nullptr;
  p_tm_com_que_t dst_rsp_transit_reg_ = nullptr;
  p_tm_com_que_t dst_dat_transit_reg_ = nullptr;
};

using tm_ring_conn_t = TmRingConn;
using p_tm_ring_conn_t =
    std::shared_ptr<tm_ring_conn_t>;

inline p_tm_ring_conn_t tm_make_ring_conn(
    const std::string& name, tm_engine::p_tm_clk_t clk, uint32_t latency,
    uint32_t width_bytes, uint32_t dst_station, TmRingPortDir dst_dir,
    TmRingConnPmuPort pmu) {
  return std::make_shared<TmRingConn>(
      name, clk, latency, width_bytes, dst_station, dst_dir, pmu);
}

#endif  // _TM_RING_CONN_H_
