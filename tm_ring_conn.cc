#include "tm_ring_conn.h"

#include <algorithm>

using namespace tm_engine;

TmRingConn::TmRingConn(
    const std::string& name, p_tm_clk_t clk, uint32_t latency,
    uint32_t width_bytes, uint32_t dst_station, TmRingPortDir dst_dir)
    : TmModule(name), latency_(latency), dst_station_(dst_station),
      dst_dir_(dst_dir) {
  pipeline_depth_ = std::max<uint32_t>(1, latency_ + 1);
  width_bytes_ = std::max<uint32_t>(1, width_bytes);

  slot_pipelines_.clear();
  const uint32_t lanes = tm_ring_subnet_count();
  inflight_count_.assign(lanes, 0);
  serializer_slots_.assign(lanes, nullptr);
  serializer_available_.assign(lanes, true);
  serializer_release_event_pending_.assign(lanes, false);
  serializer_to_pipeline_event_pending_.assign(lanes, false);
  retired_serializer_release_event_pending_.assign(lanes, false);
  retired_serializer_to_pipeline_event_pending_.assign(lanes, false);
  serializer_release_events_.clear();
  serializer_to_pipeline_events_.clear();
  lane_stats_.assign(lanes, TmRingConnStats());

  dst_req_transit_reg_ = nullptr;
  dst_rsp_transit_reg_ = nullptr;
  dst_dat_transit_reg_ = nullptr;

  auto drain_proc =
      TM_MAKE_CPROC(&TmRingConn::drain_ready_slots);
  for (uint32_t lane = 0; lane < lanes; ++lane) {
    slot_pipelines_.push_back(tm_make_que<p_tm_pld_t>(
        clk, name + "_lane" + std::to_string(lane) + "_slot_pipeline",
        pipeline_depth_, latency_));
    serializer_release_events_.push_back(
        tm_make_event(name + "_lane" + std::to_string(lane) +
                      "_serializer_release"));
    serializer_to_pipeline_events_.push_back(
        tm_make_event(name + "_lane" + std::to_string(lane) +
                      "_serializer_to_pipeline"));
  }
  for (auto& pipeline : slot_pipelines_) {
    tm_sensitive(drain_proc, pipeline->vld);
  }
  tm_sensitive(TM_MAKE_CPROC(&TmRingConn::release_req_serializer),
               serializer_release_events_[
                   tm_ring_subnet_index(TmRingSubnet::REQ)]);
  tm_sensitive(TM_MAKE_CPROC(&TmRingConn::release_rsp_serializer),
               serializer_release_events_[
                   tm_ring_subnet_index(TmRingSubnet::RSP)]);
  tm_sensitive(TM_MAKE_CPROC(&TmRingConn::release_dat_serializer),
               serializer_release_events_[
                   tm_ring_subnet_index(TmRingSubnet::DAT)]);
  tm_sensitive(TM_MAKE_CPROC(&TmRingConn::move_req_serializer_to_pipeline),
               serializer_to_pipeline_events_[
                   tm_ring_subnet_index(TmRingSubnet::REQ)]);
  tm_sensitive(TM_MAKE_CPROC(&TmRingConn::move_rsp_serializer_to_pipeline),
               serializer_to_pipeline_events_[
                   tm_ring_subnet_index(TmRingSubnet::RSP)]);
  tm_sensitive(TM_MAKE_CPROC(&TmRingConn::move_dat_serializer_to_pipeline),
               serializer_to_pipeline_events_[
                   tm_ring_subnet_index(TmRingSubnet::DAT)]);

  reset();
}

TmRingSubnet TmRingConn::slot_subnet(p_tm_pld_t slot) const {
  if (slot->ring_subnet == static_cast<uint32_t>(TmRingSubnet::DAT)) {
    return TmRingSubnet::DAT;
  }
  if (slot->ring_subnet == static_cast<uint32_t>(TmRingSubnet::RSP)) {
    return TmRingSubnet::RSP;
  }
  return TmRingSubnet::REQ;
}

void TmRingConn::record_stall(
    uint32_t lane_idx, uint64_t TmRingConnStats::*field) {
  lane_stats_[lane_idx].*field += 1;
}

void TmRingConn::record_slot(uint32_t lane_idx,
                                         uint32_t bytes,
                                         uint32_t serialization_cycles) {
  auto& lane_stats = lane_stats_[lane_idx];
  lane_stats.packets++;
  lane_stats.bytes += bytes;
  lane_stats.busy_cycles += serialization_cycles;
  lane_stats.inflight_peak =
      std::max(lane_stats.inflight_peak, inflight_count_[lane_idx]);

}

void TmRingConn::reset() {
  std::fill(inflight_count_.begin(), inflight_count_.end(), 0);
  std::fill(serializer_available_.begin(), serializer_available_.end(), true);
  for (uint32_t lane = 0; lane < tm_ring_subnet_count(); ++lane) {
    retired_serializer_release_event_pending_[lane] =
        retired_serializer_release_event_pending_[lane] ||
        serializer_release_event_pending_[lane];
    retired_serializer_to_pipeline_event_pending_[lane] =
        retired_serializer_to_pipeline_event_pending_[lane] ||
        serializer_to_pipeline_event_pending_[lane];
    serializer_slots_[lane] = nullptr;
    serializer_release_event_pending_[lane] = false;
    serializer_to_pipeline_event_pending_[lane] = false;
  }
  clear_stats();
  for (auto& pipeline : slot_pipelines_) {
    pipeline->clear();
  }
}

void TmRingConn::clear_stats() {
  for (TmRingConnStats& stats : lane_stats_) {
    stats.clear();
  }
}

bool TmRingConn::idle() const {
  for (uint32_t lane = 0; lane < slot_pipelines_.size(); ++lane) {
    if (retired_serializer_release_event_pending_[lane] ||
        retired_serializer_to_pipeline_event_pending_[lane] ||
        !slot_pipelines_[lane]->empty() || inflight_count_[lane] != 0) {
      return false;
    }
  }
  return true;
}

bool TmRingConn::can_accept(p_tm_pld_t slot) {
  const TmRingSubnet subnet = slot_subnet(slot);
  const uint32_t idx = tm_ring_subnet_index(subnet);
  if (retired_serializer_release_event_pending_[idx] ||
      retired_serializer_to_pipeline_event_pending_[idx]) {
    record_stall(idx, &TmRingConnStats::send_reject_stall);
    return false;
  }
  if (!serializer_available_[idx]) {
    record_stall(idx, &TmRingConnStats::serialization_busy_stall);
    record_stall(idx, &TmRingConnStats::send_reject_stall);
    return false;
  }
  if (inflight_count_[idx] >= pipeline_depth_ ||
      slot_pipelines_[idx]->full()) {
    record_stall(idx, &TmRingConnStats::pipeline_full_stall);
    record_stall(idx, &TmRingConnStats::send_reject_stall);
    return false;
  }
  return true;
}

bool TmRingConn::accept_slot(p_tm_pld_t slot) {
  if (!can_accept(slot)) {
    return false;
  }
  reserve_slot(slot);
  return true;
}

void TmRingConn::reserve_slot(p_tm_pld_t slot) {
  const TmRingSubnet subnet = slot_subnet(slot);
  const uint32_t idx = tm_ring_subnet_index(subnet);
  const uint32_t bytes = tm_ring_packet_bytes(slot);
  const uint32_t serialization_cycles =
      tm_ring_serialization_cycles(bytes, width_bytes_);

  serializer_available_[idx] = false;
  serializer_release_event_pending_[idx] = true;
  inflight_count_[idx]++;
  record_slot(idx, bytes, serialization_cycles);

  if (slot->ring_segment_serialization_paid) {
    slot_pipelines_[idx]->push_back(slot);
    serializer_release_events_[idx]->notify_after(serialization_cycles);
    return;
  }

  slot->ring_segment_serialization_paid = true;
  serializer_slots_[idx] = slot;
  serializer_to_pipeline_event_pending_[idx] = true;
  serializer_to_pipeline_events_[idx]->notify_after(
      serialization_cycles - 1);
  serializer_release_events_[idx]->notify_after(serialization_cycles);
}

const TmRingConnStats&
TmRingConn::subnet_stats(TmRingSubnet subnet) const {
  return lane_stats_[tm_ring_subnet_index(subnet)];
}

bool TmRingConn::has_ready_slot(TmRingSubnet subnet) {
  const uint32_t idx = tm_ring_subnet_index(subnet);
  return slot_pipelines_[idx]->valid() &&
         !slot_pipelines_[idx]->empty();
}

void TmRingConn::release_serializer(TmRingSubnet subnet) {
  const uint32_t idx = tm_ring_subnet_index(subnet);
  if (retired_serializer_release_event_pending_[idx]) {
    retired_serializer_release_event_pending_[idx] = false;
    return;
  }
  if (!serializer_release_event_pending_[idx]) {
    return;
  }
  serializer_release_event_pending_[idx] = false;
  serializer_available_[idx] = true;
}

void TmRingConn::move_serializer_to_pipeline(TmRingSubnet subnet) {
  const uint32_t idx = tm_ring_subnet_index(subnet);
  if (retired_serializer_to_pipeline_event_pending_[idx]) {
    retired_serializer_to_pipeline_event_pending_[idx] = false;
    return;
  }
  if (!serializer_to_pipeline_event_pending_[idx]) {
    return;
  }
  serializer_to_pipeline_event_pending_[idx] = false;
  slot_pipelines_[idx]->push_back(serializer_slots_[idx]);
  serializer_slots_[idx] = nullptr;
}

void TmRingConn::release_req_serializer() {
  release_serializer(TmRingSubnet::REQ);
}

void TmRingConn::release_rsp_serializer() {
  release_serializer(TmRingSubnet::RSP);
}

void TmRingConn::release_dat_serializer() {
  release_serializer(TmRingSubnet::DAT);
}

void TmRingConn::move_req_serializer_to_pipeline() {
  move_serializer_to_pipeline(TmRingSubnet::REQ);
}

void TmRingConn::move_rsp_serializer_to_pipeline() {
  move_serializer_to_pipeline(TmRingSubnet::RSP);
}

void TmRingConn::move_dat_serializer_to_pipeline() {
  move_serializer_to_pipeline(TmRingSubnet::DAT);
}

void TmRingConn::attach(
    p_tm_com_que_t dst_req_transit_reg,
    p_tm_com_que_t dst_rsp_transit_reg,
    p_tm_com_que_t dst_dat_transit_reg) {
  dst_req_transit_reg_ = dst_req_transit_reg;
  dst_rsp_transit_reg_ = dst_rsp_transit_reg;
  dst_dat_transit_reg_ = dst_dat_transit_reg;
}

void TmRingConn::drain_ready_slots() {
  for (uint32_t idx = 0; idx < slot_pipelines_.size(); ++idx) {
    auto& pipeline = slot_pipelines_[idx];
    if (!pipeline->valid() || pipeline->empty()) {
      continue;
    }
    const TmRingSubnet subnet = static_cast<TmRingSubnet>(idx);
    auto slot = pipeline->front();
    p_tm_com_que_t dst_reg =
        subnet == TmRingSubnet::REQ
            ? dst_req_transit_reg_
            : subnet == TmRingSubnet::RSP ? dst_rsp_transit_reg_
                                          : dst_dat_transit_reg_;
    if (dst_reg->full()) {
      record_stall(idx, &TmRingConnStats::downstream_register_full_stall);
      continue;
    }

    dst_reg->push_back(slot);
    inflight_count_[idx]--;
    pipeline->pop_front();
  }
}

uint32_t TmRingConn::dst_station() const {
  return dst_station_;
}

TmRingPortDir TmRingConn::dst_dir() const { return dst_dir_; }
