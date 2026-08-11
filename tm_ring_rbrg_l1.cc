#include "tm_ring_rbrg_l1.h"

#include <algorithm>
#include <stdexcept>

#include "tm_ring_fanout.h"

using namespace tm_engine;

TmRingRbrgL1::TmRingRbrgL1(const std::string& name, p_tm_clk_t clk,
                           uint32_t v_ring_id, uint32_t queue_depth,
                           uint32_t latency, uint32_t width_bytes,
                           const TmRingEndpointQueueDepths& v_queue_depths,
                           const TmRingEndpointQueueDepths& h_queue_depths,
                           std::shared_ptr<TmRingTopology> topology)
    : TmModule(name),
      v_ring_id_(v_ring_id),
      rbrg_width_bytes_(width_bytes),
      topology_(topology) {
  v_niu_ = tm_make_ring_node_interface(
      clk, name + "_v_node_interface", v_queue_depths);
  h_niu_ = tm_make_ring_node_interface(
      clk, name + "_h_node_interface", h_queue_depths);

  for (uint32_t path = 0; path < paths_.size(); ++path) {
    paths_[path].transfer_q = tm_make_que<p_tm_pld_t>(
        clk, name + "_path_" + std::to_string(path) + "_transfer_q",
        queue_depth, latency);
    paths_[path].bandwidth_ready_event = tm_make_event(
        name + "_path_" + std::to_string(path) + "_bandwidth_ready");
  }

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_req),
               v_niu_->eject_q(TmRingSubnet::REQ)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_dat),
               v_niu_->eject_q(TmRingSubnet::DAT)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_rsp),
               h_niu_->eject_q(TmRingSubnet::RSP)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_dat),
               h_niu_->eject_q(TmRingSubnet::DAT)->vld);

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::send_v_req),
               paths_[path_index(TmRingRbrgPath::V_TO_H_REQ)].transfer_q->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::send_v_dat),
               paths_[path_index(TmRingRbrgPath::V_TO_H_DAT)].transfer_q->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::send_h_rsp),
               paths_[path_index(TmRingRbrgPath::H_TO_V_RSP)].transfer_q->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::send_h_dat),
               paths_[path_index(TmRingRbrgPath::H_TO_V_DAT)].transfer_q->vld);

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_v_req_bandwidth),
               paths_[path_index(TmRingRbrgPath::V_TO_H_REQ)]
                   .bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_v_dat_bandwidth),
               paths_[path_index(TmRingRbrgPath::V_TO_H_DAT)]
                   .bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_h_rsp_bandwidth),
               paths_[path_index(TmRingRbrgPath::H_TO_V_RSP)]
                   .bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_h_dat_bandwidth),
               paths_[path_index(TmRingRbrgPath::H_TO_V_DAT)]
                   .bandwidth_ready_event);

  reset();
}

void TmRingRbrgL1::reset() {
  for (uint32_t path = 0; path < paths_.size(); ++path) {
    PathState& state = paths_[path];
    state.retired_bandwidth_event_pending =
        state.retired_bandwidth_event_pending || state.bandwidth_event_pending;
    state.bandwidth_available = true;
    state.bandwidth_event_pending = false;
    state.queue_occupancy = 0;
    state.transfer_q->clear();
  }
  v_niu_->reset();
  h_niu_->reset();
  clear_stats();
}

bool TmRingRbrgL1::idle() const {
  if (!v_niu_->idle() || !h_niu_->idle()) {
    return false;
  }
  for (uint32_t path = 0; path < paths_.size(); ++path) {
    const PathState& state = paths_[path];
    if (!state.transfer_q->empty() || state.bandwidth_event_pending ||
        state.retired_bandwidth_event_pending) {
      return false;
    }
  }
  return true;
}

void TmRingRbrgL1::clear_stats() {
  stats_.clear();
}

p_tm_ring_node_interface_t TmRingRbrgL1::v_node_interface() const {
  return v_niu_;
}

p_tm_ring_node_interface_t TmRingRbrgL1::h_node_interface() const {
  return h_niu_;
}

const TmRingRbrgStats& TmRingRbrgL1::stats() const { return stats_; }

const TmRingRbrgPathStats& TmRingRbrgL1::path_stats(
    TmRingRbrgPath path) const {
  return stats_.paths[path_index(path)];
}

void TmRingRbrgL1::recv_v_req() {
  receive(TmRingRbrgPath::V_TO_H_REQ, TmRingSubnet::REQ, v_niu_);
}

void TmRingRbrgL1::recv_v_dat() {
  receive(TmRingRbrgPath::V_TO_H_DAT, TmRingSubnet::DAT, v_niu_);
}

void TmRingRbrgL1::recv_h_rsp() {
  receive(TmRingRbrgPath::H_TO_V_RSP, TmRingSubnet::RSP, h_niu_);
}

void TmRingRbrgL1::recv_h_dat() {
  receive(TmRingRbrgPath::H_TO_V_DAT, TmRingSubnet::DAT, h_niu_);
}

void TmRingRbrgL1::send_v_req() {
  send(TmRingRbrgPath::V_TO_H_REQ, TmRingSubnet::REQ, h_niu_);
}

void TmRingRbrgL1::send_v_dat() {
  send(TmRingRbrgPath::V_TO_H_DAT, TmRingSubnet::DAT, h_niu_);
}

void TmRingRbrgL1::send_h_rsp() {
  send(TmRingRbrgPath::H_TO_V_RSP, TmRingSubnet::RSP, v_niu_);
}

void TmRingRbrgL1::send_h_dat() {
  send(TmRingRbrgPath::H_TO_V_DAT, TmRingSubnet::DAT, v_niu_);
}

void TmRingRbrgL1::release_v_req_bandwidth() {
  release_bandwidth(TmRingRbrgPath::V_TO_H_REQ);
}

void TmRingRbrgL1::release_v_dat_bandwidth() {
  release_bandwidth(TmRingRbrgPath::V_TO_H_DAT);
}

void TmRingRbrgL1::release_h_rsp_bandwidth() {
  release_bandwidth(TmRingRbrgPath::H_TO_V_RSP);
}

void TmRingRbrgL1::release_h_dat_bandwidth() {
  release_bandwidth(TmRingRbrgPath::H_TO_V_DAT);
}

void TmRingRbrgL1::receive(TmRingRbrgPath path, TmRingSubnet subnet,
                            const p_tm_ring_node_interface_t& source) {
  const uint32_t index = path_index(path);
  PathState& state = paths_[index];
  TmRingRbrgPathStats& stats = stats_.paths[index];
  if (source->eject_q(subnet)->empty()) {
    return;
  }
  if (state.retired_bandwidth_event_pending) {
    return;
  }
  if (!state.bandwidth_available) {
    return;
  }
  if (state.transfer_q->full()) {
    stats.queue_full_stalls++;
    return;
  }

  p_tm_pld_t pld = source->front_eject(subnet);
  if (path == TmRingRbrgPath::V_TO_H_REQ ||
      path == TmRingRbrgPath::V_TO_H_DAT) {
    prepare_h_segment(pld);
  } else {
    prepare_v_segment(pld);
  }
  state.transfer_q->push_back(pld);
  state.queue_occupancy++;
  stats.queue_occupancy_peak =
      std::max(stats.queue_occupancy_peak, state.queue_occupancy);
  source->pop_eject(subnet);

  const uint32_t serialization_cycles = tm_ring_serialization_cycles(
      tm_ring_packet_bytes(pld), rbrg_width_bytes_);
  stats.busy_cycles += serialization_cycles;
  state.bandwidth_available = false;
  state.bandwidth_event_pending = true;
  state.bandwidth_ready_event->notify_after(serialization_cycles);
}

void TmRingRbrgL1::send(TmRingRbrgPath path, TmRingSubnet subnet,
                         const p_tm_ring_node_interface_t& destination) {
  const uint32_t index = path_index(path);
  PathState& state = paths_[index];
  if (state.transfer_q->empty()) {
    return;
  }
  p_tm_pld_t pld = state.transfer_q->front();
  if (!destination->push_inject(subnet, pld)) {
    stats_.paths[index].destination_inject_stalls++;
    return;
  }
  stats_.paths[index].packets++;
  stats_.paths[index].bytes += tm_ring_packet_bytes(pld);
  state.transfer_q->pop_front();
  state.queue_occupancy--;
  receive(path, subnet, source_for(path));
}

void TmRingRbrgL1::release_bandwidth(TmRingRbrgPath path) {
  PathState& state = paths_[path_index(path)];
  if (state.retired_bandwidth_event_pending) {
    state.retired_bandwidth_event_pending = false;
    return;
  }
  state.bandwidth_event_pending = false;
  state.bandwidth_available = true;
  receive(path, subnet_for(path), source_for(path));
}

void TmRingRbrgL1::prepare_h_segment(p_tm_pld_t pld) {
  const TmRingLocation src = topology_->rbrg_h_location(v_ring_id_);
  const TmRingLocation dst = topology_->ha_location(pld->slv_id);
  pld->mst_addr = src.station_id;
  pld->slv_addr = dst.station_id;
  pld->ring_direction =
      static_cast<uint32_t>(topology_->route_direction(src, dst));
  clear_ring_local_state(pld);
}

void TmRingRbrgL1::prepare_v_segment(p_tm_pld_t pld) {
  const TmRingLocation src = topology_->rbrg_v_location(v_ring_id_);
  if (tm_ring_has_fanout(pld)) {
    if (pld->ring_fanout->recipients.empty()) {
      throw std::logic_error("H-to-V DAT carrier has no recipients");
    }

    const TmRingFanoutRecipient& first =
        pld->ring_fanout->recipients.front();
    if (first.response_template == nullptr) {
      throw std::logic_error("H-to-V DAT recipient has no response template");
    }
    pld->mst_id = first.response_template->mst_id;
    pld->gid = first.response_template->gid;
    if (pld->ring_fanout->recipients.size() == 1) {
      pld->ring_fanout.reset();
    } else {
      uint64_t pending_stations = 0;
      for (const TmRingFanoutRecipient& recipient :
           pld->ring_fanout->recipients) {
        if (recipient.dst_ring_id != v_ring_id_) {
          throw std::logic_error("Mixed V-Ring H-to-V DAT carrier");
        }
        pending_stations |= tm_ring_station_bit(recipient.dst_node);
      }
      pld->ring_fanout->pending_stations = pending_stations;
      pld->ring_fanout->active_on_ring = true;
      pld->mst_addr = src.station_id;
      pld->slv_addr = first.dst_node;
      pld->ring_direction = static_cast<uint32_t>(TmRingPortDir::CW);
      clear_ring_local_state(pld);
      return;
    }
  }

  const TmRingLocation dst = topology_->master_location(pld->mst_id);
  pld->mst_addr = src.station_id;
  pld->slv_addr = dst.station_id;
  pld->ring_direction =
      static_cast<uint32_t>(topology_->route_direction(src, dst));
  clear_ring_local_state(pld);
}

void TmRingRbrgL1::clear_ring_local_state(p_tm_pld_t pld) {
  pld->ring_slot_empty = false;
  pld->ring_i_tag_owner = tm_ring_invalid_tag_owner();
  pld->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  pld->reset_ring_deflection_state();
}

uint32_t TmRingRbrgL1::path_index(TmRingRbrgPath path) const {
  return static_cast<uint32_t>(path);
}

p_tm_ring_node_interface_t TmRingRbrgL1::source_for(
    TmRingRbrgPath path) const {
  return path == TmRingRbrgPath::V_TO_H_REQ ||
                 path == TmRingRbrgPath::V_TO_H_DAT
             ? v_niu_
             : h_niu_;
}

TmRingSubnet TmRingRbrgL1::subnet_for(TmRingRbrgPath path) const {
  return path == TmRingRbrgPath::V_TO_H_REQ
             ? TmRingSubnet::REQ
         : path == TmRingRbrgPath::H_TO_V_RSP ? TmRingSubnet::RSP
                                               : TmRingSubnet::DAT;
}
