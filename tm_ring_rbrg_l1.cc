#include "tm_ring_rbrg_l1.h"

#include <stdexcept>
#include <vector>

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
  TmRingEndpointQueueDepths v_split_depths = v_queue_depths;
  TmRingEndpointQueueDepths h_split_depths = h_queue_depths;
  for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
    v_split_depths.inject[subnet] = queue_depth;
    h_split_depths.inject[subnet] = queue_depth;
  }
  v_niu_ = tm_make_ring_node_interface(
      clk, name + "_v_node_interface", v_split_depths,
      TmRingNodeInterfaceMode::RBRG_DIRECTIONAL, latency);
  h_niu_ = tm_make_ring_node_interface(
      clk, name + "_h_node_interface", h_split_depths,
      TmRingNodeInterfaceMode::RBRG_DIRECTIONAL, latency);

  for (uint32_t path = 0; path < paths_.size(); ++path) {
    for (uint32_t direction = 0; direction < 2; ++direction) {
      DirectionState& state = paths_[path].directions[direction];
      const std::string direction_name = std::to_string(direction);
      state.bandwidth_ready_event = tm_make_event(
          name + "_path_" + std::to_string(path) + "_" +
          direction_name + "_bandwidth_ready");
      state.serializer_handoff_event = tm_make_event(
          name + "_path_" + std::to_string(path) + "_" +
          direction_name + "_serializer_handoff");
    }
  }

  const TmRingRbrgPath v_req = TmRingRbrgPath::V_TO_H_REQ;
  const TmRingRbrgPath v_dat = TmRingRbrgPath::V_TO_H_DAT;
  const TmRingRbrgPath h_rsp = TmRingRbrgPath::H_TO_V_RSP;
  const TmRingRbrgPath h_dat = TmRingRbrgPath::H_TO_V_DAT;
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_req),
               v_niu_->eject_q(TmRingSubnet::REQ, TmRingPortDir::CW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_req),
               v_niu_->eject_q(TmRingSubnet::REQ, TmRingPortDir::CCW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_req),
               h_niu_->inject_space_event(TmRingSubnet::REQ,
                                          TmRingPortDir::CW));
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_req),
               h_niu_->inject_space_event(TmRingSubnet::REQ,
                                          TmRingPortDir::CCW));

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_dat),
               v_niu_->eject_q(TmRingSubnet::DAT, TmRingPortDir::CW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_dat),
               v_niu_->eject_q(TmRingSubnet::DAT, TmRingPortDir::CCW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_dat),
               h_niu_->inject_space_event(TmRingSubnet::DAT,
                                          TmRingPortDir::CW));
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_v_dat),
               h_niu_->inject_space_event(TmRingSubnet::DAT,
                                          TmRingPortDir::CCW));

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_rsp),
               h_niu_->eject_q(TmRingSubnet::RSP, TmRingPortDir::CW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_rsp),
               h_niu_->eject_q(TmRingSubnet::RSP, TmRingPortDir::CCW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_rsp),
               v_niu_->inject_space_event(TmRingSubnet::RSP,
                                          TmRingPortDir::CW));
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_rsp),
               v_niu_->inject_space_event(TmRingSubnet::RSP,
                                          TmRingPortDir::CCW));

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_dat),
               h_niu_->eject_q(TmRingSubnet::DAT, TmRingPortDir::CW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_dat),
               h_niu_->eject_q(TmRingSubnet::DAT, TmRingPortDir::CCW)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_dat),
               v_niu_->inject_space_event(TmRingSubnet::DAT,
                                          TmRingPortDir::CW));
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::recv_h_dat),
               v_niu_->inject_space_event(TmRingSubnet::DAT,
                                          TmRingPortDir::CCW));

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_v_req_cw_bandwidth),
               paths_[path_index(v_req)].directions[0].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_v_req_ccw_bandwidth),
               paths_[path_index(v_req)].directions[1].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_v_req_cw_serializer),
               paths_[path_index(v_req)].directions[0].serializer_handoff_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_v_req_ccw_serializer),
               paths_[path_index(v_req)].directions[1].serializer_handoff_event);

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_v_dat_cw_bandwidth),
               paths_[path_index(v_dat)].directions[0].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_v_dat_ccw_bandwidth),
               paths_[path_index(v_dat)].directions[1].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_v_dat_cw_serializer),
               paths_[path_index(v_dat)].directions[0].serializer_handoff_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_v_dat_ccw_serializer),
               paths_[path_index(v_dat)].directions[1].serializer_handoff_event);

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_h_rsp_cw_bandwidth),
               paths_[path_index(h_rsp)].directions[0].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_h_rsp_ccw_bandwidth),
               paths_[path_index(h_rsp)].directions[1].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_h_rsp_cw_serializer),
               paths_[path_index(h_rsp)].directions[0].serializer_handoff_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_h_rsp_ccw_serializer),
               paths_[path_index(h_rsp)].directions[1].serializer_handoff_event);

  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_h_dat_cw_bandwidth),
               paths_[path_index(h_dat)].directions[0].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::release_h_dat_ccw_bandwidth),
               paths_[path_index(h_dat)].directions[1].bandwidth_ready_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_h_dat_cw_serializer),
               paths_[path_index(h_dat)].directions[0].serializer_handoff_event);
  tm_sensitive(TM_MAKE_CPROC(&TmRingRbrgL1::handoff_h_dat_ccw_serializer),
               paths_[path_index(h_dat)].directions[1].serializer_handoff_event);

  reset();
}

void TmRingRbrgL1::reset() {
  for (uint32_t path = 0; path < paths_.size(); ++path) {
    PathState& path_state = paths_[path];
    path_state.next_input = TmRingPortDir::CW;
    for (uint32_t direction = 0; direction < 2; ++direction) {
      DirectionState& state = path_state.directions[direction];
      state.retired_bandwidth_event_pending =
          state.retired_bandwidth_event_pending || state.bandwidth_event_pending;
      state.retired_serializer_handoff_event_pending =
          state.retired_serializer_handoff_event_pending ||
          state.serializer_handoff_event_pending;
      state.bandwidth_available = true;
      state.bandwidth_event_pending = false;
      state.serializer_handoff_event_pending = false;
      state.serializer_slot = nullptr;
    }
  }
  fanout_tie_next_direction_ = TmRingPortDir::CW;
  v_niu_->reset();
  h_niu_->reset();
  clear_stats();
}

bool TmRingRbrgL1::idle() const {
  if (!v_niu_->idle() || !h_niu_->idle()) {
    return false;
  }
  for (uint32_t path = 0; path < paths_.size(); ++path) {
    const PathState& path_state = paths_[path];
    for (uint32_t direction = 0; direction < 2; ++direction) {
      const DirectionState& state = path_state.directions[direction];
      if (state.serializer_slot != nullptr || state.bandwidth_event_pending ||
          state.retired_bandwidth_event_pending ||
          state.serializer_handoff_event_pending ||
          state.retired_serializer_handoff_event_pending) {
        return false;
      }
    }
  }
  return true;
}

void TmRingRbrgL1::clear_stats() { stats_.clear(); }

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
  schedule_path(TmRingRbrgPath::V_TO_H_REQ);
}

void TmRingRbrgL1::recv_v_dat() {
  schedule_path(TmRingRbrgPath::V_TO_H_DAT);
}

void TmRingRbrgL1::recv_h_rsp() {
  schedule_path(TmRingRbrgPath::H_TO_V_RSP);
}

void TmRingRbrgL1::recv_h_dat() {
  schedule_path(TmRingRbrgPath::H_TO_V_DAT);
}

void TmRingRbrgL1::release_v_req_cw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::V_TO_H_REQ, TmRingPortDir::CW);
}

void TmRingRbrgL1::release_v_req_ccw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::V_TO_H_REQ, TmRingPortDir::CCW);
}

void TmRingRbrgL1::release_v_dat_cw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::V_TO_H_DAT, TmRingPortDir::CW);
}

void TmRingRbrgL1::release_v_dat_ccw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::V_TO_H_DAT, TmRingPortDir::CCW);
}

void TmRingRbrgL1::release_h_rsp_cw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::H_TO_V_RSP, TmRingPortDir::CW);
}

void TmRingRbrgL1::release_h_rsp_ccw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::H_TO_V_RSP, TmRingPortDir::CCW);
}

void TmRingRbrgL1::release_h_dat_cw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::H_TO_V_DAT, TmRingPortDir::CW);
}

void TmRingRbrgL1::release_h_dat_ccw_bandwidth() {
  release_bandwidth(TmRingRbrgPath::H_TO_V_DAT, TmRingPortDir::CCW);
}

void TmRingRbrgL1::handoff_v_req_cw_serializer() {
  handoff_serializer(TmRingRbrgPath::V_TO_H_REQ, TmRingPortDir::CW);
}

void TmRingRbrgL1::handoff_v_req_ccw_serializer() {
  handoff_serializer(TmRingRbrgPath::V_TO_H_REQ, TmRingPortDir::CCW);
}

void TmRingRbrgL1::handoff_v_dat_cw_serializer() {
  handoff_serializer(TmRingRbrgPath::V_TO_H_DAT, TmRingPortDir::CW);
}

void TmRingRbrgL1::handoff_v_dat_ccw_serializer() {
  handoff_serializer(TmRingRbrgPath::V_TO_H_DAT, TmRingPortDir::CCW);
}

void TmRingRbrgL1::handoff_h_rsp_cw_serializer() {
  handoff_serializer(TmRingRbrgPath::H_TO_V_RSP, TmRingPortDir::CW);
}

void TmRingRbrgL1::handoff_h_rsp_ccw_serializer() {
  handoff_serializer(TmRingRbrgPath::H_TO_V_RSP, TmRingPortDir::CCW);
}

void TmRingRbrgL1::handoff_h_dat_cw_serializer() {
  handoff_serializer(TmRingRbrgPath::H_TO_V_DAT, TmRingPortDir::CW);
}

void TmRingRbrgL1::handoff_h_dat_ccw_serializer() {
  handoff_serializer(TmRingRbrgPath::H_TO_V_DAT, TmRingPortDir::CCW);
}

void TmRingRbrgL1::schedule_path(TmRingRbrgPath path) {
  const TmRingPortDir directions[] = {TmRingPortDir::CW,
                                      TmRingPortDir::CCW};
  const uint32_t index = path_index(path);
  PathState& path_state = paths_[index];
  const p_tm_ring_node_interface_t destination = destination_for(path);
  const TmRingSubnet subnet = subnet_for(path);
  std::array<HeadCandidate, 2> candidates;
  for (uint32_t input = 0; input < candidates.size(); ++input) {
    candidates[input] = head_candidate(path, directions[input]);
  }

  Assignment best;
  bool have_assignment = false;
  for (int32_t first_output = -1; first_output <= 1; ++first_output) {
    for (int32_t second_output = -1; second_output <= 1; ++second_output) {
      const int32_t outputs[] = {first_output, second_output};
      Assignment trial;
      trial.output[0] = first_output;
      trial.output[1] = second_output;
      bool valid = true;
      for (uint32_t input = 0; input < candidates.size(); ++input) {
        const int32_t output = outputs[input];
        if (output < 0) {
          continue;
        }
        if (!candidates[input].valid ||
            (input != 0 && output == outputs[0])) {
          valid = false;
          break;
        }
        const TmRingPortDir direction = directions[output];
        const DirectionState& state =
            path_state.directions[direction_index(direction)];
        if (!state.bandwidth_available || state.bandwidth_event_pending ||
            state.retired_bandwidth_event_pending ||
            state.serializer_handoff_event_pending ||
            state.retired_serializer_handoff_event_pending ||
            !destination->has_inject_capacity(subnet, direction)) {
          valid = false;
          break;
        }
        ++trial.transfers;
        if (candidates[input].preferred == direction) {
          ++trial.preferred_transfers;
        }
      }
      if (!valid) {
        continue;
      }

      const uint32_t next_input = direction_index(path_state.next_input);
      const bool trial_services_next = trial.output[next_input] >= 0;
      const bool best_services_next = have_assignment &&
                                      best.output[next_input] >= 0;
      const bool trial_services_cw = trial.output[0] >= 0;
      const bool best_services_cw = have_assignment && best.output[0] >= 0;
      if (!have_assignment || trial.transfers > best.transfers ||
          (trial.transfers == best.transfers &&
           trial.preferred_transfers > best.preferred_transfers) ||
          (trial.transfers == best.transfers &&
           trial.preferred_transfers == best.preferred_transfers &&
           trial_services_next && !best_services_next) ||
          (trial.transfers == best.transfers &&
           trial.preferred_transfers == best.preferred_transfers &&
           trial_services_next == best_services_next && trial_services_cw &&
           !best_services_cw)) {
        best = trial;
        have_assignment = true;
      }
    }
  }

  if (!have_assignment || best.transfers == 0) {
    return;
  }
  const bool same_preferred_output = candidates[0].valid &&
                                     candidates[1].valid &&
                                     candidates[0].preferred ==
                                         candidates[1].preferred;
  for (uint32_t input = 0; input < candidates.size(); ++input) {
    if (best.output[input] >= 0) {
      start_serializer(path, candidates[input], directions[best.output[input]]);
    }
  }
  if (same_preferred_output) {
    path_state.next_input = tm_ring_opposite_dir(path_state.next_input);
  }
}

void TmRingRbrgL1::start_serializer(TmRingRbrgPath path,
                                    const HeadCandidate& candidate,
                                    TmRingPortDir output) {
  const uint32_t index = path_index(path);
  const uint32_t output_index = direction_index(output);
  PathState& path_state = paths_[index];
  DirectionState& state = path_state.directions[output_index];
  const p_tm_ring_node_interface_t source = source_for(path);
  const p_tm_ring_node_interface_t destination = destination_for(path);
  const TmRingSubnet subnet = subnet_for(path);
  if (source->front_eject(subnet, candidate.input) != candidate.pld ||
      !state.bandwidth_available ||
      !destination->has_inject_capacity(subnet, output)) {
    return;
  }

  if (path == TmRingRbrgPath::V_TO_H_REQ ||
      path == TmRingRbrgPath::V_TO_H_DAT) {
    prepare_h_segment(candidate.pld, output);
  } else {
    prepare_v_segment(candidate.pld, output);
  }
  source->pop_eject(subnet, candidate.input);
  state.serializer_slot = candidate.pld;
  if (candidate.equal_fanout_span) {
    fanout_tie_next_direction_ = tm_ring_opposite_dir(fanout_tie_next_direction_);
  }

  const uint32_t serialization_cycles = tm_ring_serialization_cycles(
      tm_ring_packet_bytes(candidate.pld), rbrg_width_bytes_);
  stats_.paths[index].busy_cycles += serialization_cycles;
  state.bandwidth_available = false;
  state.bandwidth_event_pending = true;
  state.serializer_handoff_event_pending = true;
  state.serializer_handoff_event->notify_after(serialization_cycles - 1);
  state.bandwidth_ready_event->notify_after(serialization_cycles);
}

TmRingRbrgL1::HeadCandidate TmRingRbrgL1::head_candidate(
    TmRingRbrgPath path, TmRingPortDir input) const {
  HeadCandidate candidate;
  candidate.input = input;
  candidate.pld = source_for(path)->front_eject(subnet_for(path), input);
  if (candidate.pld == nullptr) {
    return candidate;
  }
  candidate.preferred =
      preferred_direction(path, candidate.pld, &candidate.equal_fanout_span);
  candidate.valid = true;
  return candidate;
}

TmRingPortDir TmRingRbrgL1::preferred_direction(
    TmRingRbrgPath path, p_tm_pld_t pld, bool* equal_fanout_span) const {
  *equal_fanout_span = false;
  if (path == TmRingRbrgPath::V_TO_H_REQ ||
      path == TmRingRbrgPath::V_TO_H_DAT) {
    return topology_->route_direction(topology_->rbrg_h_location(v_ring_id_),
                                      topology_->ha_location(pld->slv_id));
  }

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
    if (pld->ring_fanout->recipients.size() == 1) {
      return topology_->route_direction(
          src, topology_->master_location(first.response_template->mst_id));
    }

    std::vector<TmRingLocation> recipient_locations;
    recipient_locations.reserve(pld->ring_fanout->recipients.size());
    for (const TmRingFanoutRecipient& recipient :
         pld->ring_fanout->recipients) {
      if (recipient.dst_ring_id != v_ring_id_) {
        throw std::logic_error("Mixed V-Ring H-to-V DAT carrier");
      }
      recipient_locations.push_back(TmRingLocation(
          TmRingDomainType::V_RING, v_ring_id_, recipient.dst_node));
    }
    const uint32_t cw_span =
        topology_->fanout_span(src, recipient_locations, TmRingPortDir::CW);
    const uint32_t ccw_span = topology_->fanout_span(
        src, recipient_locations, TmRingPortDir::CCW);
    if (cw_span < ccw_span) {
      return TmRingPortDir::CW;
    }
    if (ccw_span < cw_span) {
      return TmRingPortDir::CCW;
    }
    *equal_fanout_span = true;
    return fanout_tie_next_direction_;
  }

  return topology_->route_direction(src, topology_->master_location(pld->mst_id));
}

void TmRingRbrgL1::release_bandwidth(TmRingRbrgPath path,
                                     TmRingPortDir direction) {
  DirectionState& state =
      paths_[path_index(path)].directions[direction_index(direction)];
  if (state.retired_bandwidth_event_pending) {
    state.retired_bandwidth_event_pending = false;
    schedule_path(path);
    return;
  }
  if (!state.bandwidth_event_pending) {
    return;
  }
  state.bandwidth_event_pending = false;
  state.bandwidth_available = true;
  schedule_path(path);
}

void TmRingRbrgL1::handoff_serializer(TmRingRbrgPath path,
                                      TmRingPortDir direction) {
  const uint32_t index = path_index(path);
  const TmRingSubnet subnet = subnet_for(path);
  const p_tm_ring_node_interface_t destination = destination_for(path);
  DirectionState& state = paths_[index].directions[direction_index(direction)];
  if (state.retired_serializer_handoff_event_pending) {
    state.retired_serializer_handoff_event_pending = false;
    schedule_path(path);
    return;
  }
  if (!state.serializer_handoff_event_pending) {
    return;
  }
  if (state.serializer_slot == nullptr) {
    throw std::logic_error("RBRG lost reserved Split capacity");
  }
  // The packet enters a new Ring segment at the target Split.
  state.serializer_slot->ring_segment_serialization_paid = false;
  if (!destination->push_inject(subnet, direction, state.serializer_slot)) {
    throw std::logic_error("RBRG lost reserved Split capacity");
  }
  stats_.paths[index].packets++;
  stats_.paths[index].bytes += tm_ring_packet_bytes(state.serializer_slot);
  state.serializer_slot = nullptr;
  state.serializer_handoff_event_pending = false;
}

void TmRingRbrgL1::prepare_h_segment(p_tm_pld_t pld,
                                     TmRingPortDir direction) {
  const TmRingLocation src = topology_->rbrg_h_location(v_ring_id_);
  const TmRingLocation dst = topology_->ha_location(pld->slv_id);
  pld->mst_addr = src.station_id;
  pld->slv_addr = dst.station_id;
  pld->ring_direction = static_cast<uint32_t>(direction);
  clear_ring_local_state(pld);
}

void TmRingRbrgL1::prepare_v_segment(p_tm_pld_t pld,
                                     TmRingPortDir direction) {
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
      pld->ring_direction = static_cast<uint32_t>(direction);
      clear_ring_local_state(pld);
      return;
    }
  }

  const TmRingLocation dst = topology_->master_location(pld->mst_id);
  pld->mst_addr = src.station_id;
  pld->slv_addr = dst.station_id;
  pld->ring_direction = static_cast<uint32_t>(direction);
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

uint32_t TmRingRbrgL1::direction_index(TmRingPortDir direction) const {
  return direction == TmRingPortDir::CCW ? 1 : 0;
}

p_tm_ring_node_interface_t TmRingRbrgL1::source_for(
    TmRingRbrgPath path) const {
  return path == TmRingRbrgPath::V_TO_H_REQ ||
                 path == TmRingRbrgPath::V_TO_H_DAT
             ? v_niu_
             : h_niu_;
}

p_tm_ring_node_interface_t TmRingRbrgL1::destination_for(
    TmRingRbrgPath path) const {
  return source_for(path) == v_niu_ ? h_niu_ : v_niu_;
}

TmRingSubnet TmRingRbrgL1::subnet_for(TmRingRbrgPath path) const {
  return path == TmRingRbrgPath::V_TO_H_REQ
             ? TmRingSubnet::REQ
         : path == TmRingRbrgPath::H_TO_V_RSP ? TmRingSubnet::RSP
                                               : TmRingSubnet::DAT;
}
