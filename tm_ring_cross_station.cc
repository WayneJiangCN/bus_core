#include "tm_ring_cross_station.h"

#include <algorithm>

#include "tm_ring_fanout.h"

using namespace tm_engine;

TmRingCrossStation::TmRingCrossStation(const std::string& name,
                                       p_tm_clk_t clk)
    : TmModule(name) {

  for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
    for (uint32_t direction = 0; direction < 2; ++direction) {
      transit_regs_[subnet][direction] =
          tm_make_com_que(clk,
                          name + "_transit_reg_" + std::to_string(subnet) +
                              "_" + std::to_string(direction),
                          1);
    }
  }

  auto schedule_req_proc = TM_MAKE_CPROC(&TmRingCrossStation::schedule_req);
  auto schedule_rsp_proc = TM_MAKE_CPROC(&TmRingCrossStation::schedule_rsp);
  auto schedule_dat_proc = TM_MAKE_CPROC(&TmRingCrossStation::schedule_dat);
  for (uint32_t direction = 0; direction < 2; ++direction) {
    tm_sensitive(
        schedule_req_proc,
        transit_regs_[static_cast<uint32_t>(TmRingSubnet::REQ)][direction]
            ->vld);
    tm_sensitive(
        schedule_rsp_proc,
        transit_regs_[static_cast<uint32_t>(TmRingSubnet::RSP)][direction]
            ->vld);
    tm_sensitive(
        schedule_dat_proc,
        transit_regs_[static_cast<uint32_t>(TmRingSubnet::DAT)][direction]
            ->vld);
  }

  e_tag_txn_keys_.assign(tm_ring_subnet_count(), TmPldTxnKey());
  reset();
}

void TmRingCrossStation::reset() {
  for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
    for (uint32_t direction = 0; direction < 2; ++direction) {
      transit_regs_[subnet][direction]->clear();
      i_tag_pending_[subnet][direction] = false;
    }
  }
  std::fill(e_tag_reserved_.begin(), e_tag_reserved_.end(), false);
  std::fill(e_tag_txn_keys_.begin(), e_tag_txn_keys_.end(), TmPldTxnKey());
  clear_stats();
}

void TmRingCrossStation::clear_stats() {
  stats_.clear();
}

bool TmRingCrossStation::idle() const {
  for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
    for (uint32_t direction = 0; direction < 2; ++direction) {
      if (!transit_regs_[subnet][direction]->empty()) {
        return false;
      }
    }
  }
  for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
    for (uint32_t direction = 0; direction < 2; ++direction) {
      if (i_tag_pending_[subnet][direction]) {
        return false;
      }
    }
  }
  return true;
}

void TmRingCrossStation::attach(uint32_t station_id,
                                p_tm_ring_conn_t cw_out_conn,
                                p_tm_ring_conn_t ccw_out_conn,
                                p_tm_ring_conn_t cw_in_conn,
                                p_tm_ring_conn_t ccw_in_conn,
                                p_tm_ring_slot_pool_t slot_pool) {
  station_id_ = station_id;
  cw_out_conn_ = cw_out_conn;
  ccw_out_conn_ = ccw_out_conn;
  cw_in_conn_ = cw_in_conn;
  ccw_in_conn_ = ccw_in_conn;
  slot_pool_ = slot_pool;
}

void TmRingCrossStation::bind_node_interface(
    p_tm_ring_node_interface_t node_interface) {
  node_interface_ = node_interface;
  for (uint32_t direction = 0; direction < 2; ++direction) {
    const auto dir = direction == 0 ? TmRingPortDir::CW : TmRingPortDir::CCW;
    tm_sensitive(TM_MAKE_CPROC(&TmRingCrossStation::schedule_req),
                 node_interface_->inject_bank(TmRingSubnet::REQ, dir)->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingCrossStation::schedule_rsp),
                 node_interface_->inject_bank(TmRingSubnet::RSP, dir)->vld);
    tm_sensitive(TM_MAKE_CPROC(&TmRingCrossStation::schedule_dat),
                 node_interface_->inject_bank(TmRingSubnet::DAT, dir)->vld);
  }
}

p_tm_com_que_t TmRingCrossStation::transit_in_reg(TmRingPortDir in_dir,
                                                  TmRingSubnet subnet) const {
  return transit_reg(in_dir, subnet);
}

const TmRingCrossStationStats& TmRingCrossStation::stats() const {
  return stats_;
}

void TmRingCrossStation::schedule_req() { schedule_subnet(TmRingSubnet::REQ); }

void TmRingCrossStation::schedule_rsp() { schedule_subnet(TmRingSubnet::RSP); }

void TmRingCrossStation::schedule_dat() { schedule_subnet(TmRingSubnet::DAT); }

void TmRingCrossStation::schedule_subnet(TmRingSubnet subnet) {
  OutputUsed output_used = {{false, false}};
  process_transit(TmRingPortDir::CW, subnet, output_used);
  process_transit(TmRingPortDir::CCW, subnet, output_used);
  try_normal_injection(subnet, TmRingPortDir::CW, output_used);
  try_normal_injection(subnet, TmRingPortDir::CCW, output_used);
}

void TmRingCrossStation::process_transit(TmRingPortDir in_dir,
                                         TmRingSubnet subnet,
                                         OutputUsed& output_used) {
  auto reg = transit_reg(in_dir, subnet);
  if (!reg->valid() || reg->empty()) {
    return;
  }

  auto slot = reg->front();
  const TmRingPortDir out_dir = slot_direction(slot);
  const uint32_t out_idx = direction_index(out_dir);
  if (output_used[out_idx]) {
    return;
  }

  if (!slot->ring_slot_empty && tm_ring_fanout_active(slot)) {
    process_fanout_transit(in_dir, subnet, output_used);
    return;
  }

  if (slot->ring_slot_empty) {
    if (slot->ring_i_tag_owner == station_id_) {
      if (!local_waiting_for_output(subnet, out_dir)) {
        i_tag_pending_[static_cast<uint32_t>(subnet)][out_idx] = false;
        release_ring_slot(subnet, out_dir);
        reg->pop_front();
        stats_.transit_slots++;
        return;
      }
      output_used[out_idx] = true;
      if (try_slot_replacement(slot, subnet, out_dir, true)) {
        reg->pop_front();
        stats_.transit_slots++;
      }
      return;
    }
    output_used[out_idx] = true;
    if (forward_slot(slot, out_dir)) {
      reg->pop_front();
      stats_.transit_slots++;
      stats_.tagged_empty_slots++;
    }
    return;
  }

  if (is_destination(slot) && can_eject(slot, subnet)) {
    bool replacement_sent = false;
    if (!try_preserve_or_replace_i_tag(slot, subnet, out_dir, output_used,
                                       &replacement_sent)) {
      return;
    }

    commit_eject(slot, subnet);
    if (!replacement_sent) {
      if (slot->ring_i_tag_owner == station_id_ &&
          !local_waiting_for_output(subnet, out_dir)) {
        i_tag_pending_[static_cast<uint32_t>(subnet)][out_idx] = false;
      }
      release_ring_slot(subnet, out_dir);
    }
    reg->pop_front();
    stats_.transit_slots++;
    if (replacement_sent) {
      output_used[out_idx] = true;
    }
    return;
  }

  const uint32_t subnet_idx = static_cast<uint32_t>(subnet);
  const bool set_i_tag = !i_tag_pending_[subnet_idx][out_idx] &&
                         local_waiting_for_output(subnet, out_dir) &&
                         slot->ring_i_tag_owner == tm_ring_invalid_tag_owner();
  const bool set_e_tag =
      is_destination(slot) && mark_e_tag_for_forward(slot, subnet);
  if (set_i_tag) {
    slot->ring_i_tag_owner = station_id_;
  }

  if (!forward_slot(slot, out_dir)) {
    rollback_e_tag_mark(slot, set_e_tag);
    if (set_i_tag) {
      slot->ring_i_tag_owner = tm_ring_invalid_tag_owner();
    }
    output_used[out_idx] = true;
    return;
  }

  if (is_destination(slot)) {
    stats_.eject_queue_full_stalls++;
    stats_.deflected_packets++;
    slot->ring_deflection_count++;
    if (set_e_tag) {
      e_tag_reserved_[subnet_idx] = true;
      e_tag_txn_keys_[subnet_idx] = tm_ring_packet_txn_key(slot);
      stats_.e_tag_sets++;
    }
  }
  if (set_i_tag) {
    i_tag_pending_[subnet_idx][out_idx] = true;
    stats_.i_tag_sets++;
  }
  reg->pop_front();
  stats_.transit_slots++;
  output_used[out_idx] = true;
}

void TmRingCrossStation::process_fanout_transit(
    TmRingPortDir in_dir, TmRingSubnet subnet,
    OutputUsed& output_used) {
  auto reg = transit_reg(in_dir, subnet);
  auto envelope = reg->front();
  const TmRingPortDir out_dir = slot_direction(envelope);
  const uint32_t out_idx = direction_index(out_dir);
  size_t recipient_index = 0;

  if (!fanout_recipient_for_station(envelope, &recipient_index)) {
    if (forward_slot(envelope, out_dir)) {
      reg->pop_front();
      stats_.transit_slots++;
      output_used[out_idx] = true;
    } else {
      output_used[out_idx] = true;
    }
    return;
  }

  if (!can_eject(envelope, subnet)) {
    const uint32_t subnet_idx = static_cast<uint32_t>(subnet);
    const bool set_e_tag = mark_e_tag_for_forward(envelope, subnet);
    if (!forward_slot(envelope, out_dir)) {
      rollback_e_tag_mark(envelope, set_e_tag);
      output_used[out_idx] = true;
      return;
    }

    stats_.eject_queue_full_stalls++;
    stats_.deflected_packets++;
    envelope->ring_deflection_count++;
    if (set_e_tag) {
      e_tag_reserved_[subnet_idx] = true;
      e_tag_txn_keys_[subnet_idx] = tm_ring_packet_txn_key(envelope);
      stats_.e_tag_sets++;
    }
    reg->pop_front();
    stats_.transit_slots++;
    output_used[out_idx] = true;
    return;
  }

  p_tm_pld_t response =
      make_fanout_response(envelope, recipient_index);
  const uint32_t remaining_stations =
      tm_ring_fanout_remaining_stations(*envelope->ring_fanout);
  if (remaining_stations > 1) {
    p_tm_pld_t forward =
        make_fanout_forward(envelope, recipient_index);
    if (owns_e_tag(envelope, subnet)) {
      forward->ring_e_tag_owner = tm_ring_invalid_tag_owner();
    }
    if (!forward_slot(forward, out_dir)) {
      output_used[out_idx] = true;
      return;
    }
    commit_fanout_eject(envelope, response, subnet);
    reg->pop_front();
    stats_.transit_slots++;
    output_used[out_idx] = true;
    return;
  }

  bool replacement_sent = false;
  if (!try_preserve_or_replace_i_tag(envelope, subnet, out_dir, output_used,
                                     &replacement_sent)) {
    return;
  }

  commit_fanout_eject(envelope, response, subnet);
  if (!replacement_sent) {
    if (envelope->ring_i_tag_owner == station_id_ &&
        !local_waiting_for_output(subnet, out_dir)) {
      i_tag_pending_[static_cast<uint32_t>(subnet)][out_idx] = false;
    }
    release_ring_slot(subnet, out_dir);
  }
  reg->pop_front();
  stats_.transit_slots++;
  if (replacement_sent) {
    output_used[out_idx] = true;
  }
}

void TmRingCrossStation::try_normal_injection(TmRingSubnet subnet,
                                               TmRingPortDir out_dir,
                                               OutputUsed& output_used) {
  const uint32_t out_idx = direction_index(out_dir);
  if (i_tag_pending_[static_cast<uint32_t>(subnet)][out_idx] ||
      output_used[out_idx] ||
      !local_waiting_for_output(subnet, out_dir) ||
      incoming_transit_ready(subnet, out_dir)) {
    return;
  }

  auto slot = node_interface_->front_inject(subnet, out_dir);
  if (!slot_pool_->try_acquire(subnet, out_dir)) {
    stats_.slot_pool_full_stalls++;
    return;
  }
  if (!forward_slot(slot, out_dir)) {
    slot_pool_->release(subnet, out_dir);
    return;
  }
  node_interface_->pop_inject(subnet, out_dir);
  output_used[out_idx] = true;
  stats_.injected_packets++;
}

bool TmRingCrossStation::try_slot_replacement(p_tm_pld_t transit_slot,
                                              TmRingSubnet subnet,
                                              TmRingPortDir out_dir,
                                              bool tag_required) {
  if (tag_required && transit_slot->ring_i_tag_owner != station_id_) {
    auto empty = make_tagged_empty_slot(transit_slot);
    return forward_slot(empty, out_dir);
  }

  const uint32_t subnet_idx = static_cast<uint32_t>(subnet);
  const uint32_t out_idx = direction_index(out_dir);
  const bool claims_i_tag =
      tag_required && transit_slot->ring_i_tag_owner == station_id_;
  if (i_tag_pending_[subnet_idx][out_idx] && !claims_i_tag) {
    return false;
  }
  if (!local_waiting_for_output(subnet, out_dir)) {
    return false;
  }

  auto injected = node_interface_->front_inject(subnet, out_dir);
  if (!forward_slot(injected, out_dir)) {
    return false;
  }
  node_interface_->pop_inject(subnet, out_dir);
  stats_.injected_packets++;
  if (claims_i_tag) {
    i_tag_pending_[subnet_idx][out_idx] = false;
    stats_.i_tag_claims++;
  }
  return true;
}

bool TmRingCrossStation::try_preserve_or_replace_i_tag(
    p_tm_pld_t slot, TmRingSubnet subnet, TmRingPortDir out_dir,
    OutputUsed& output_used, bool* replacement_sent) {
  const uint32_t out_idx = direction_index(out_dir);
  const bool owner_still_waiting =
      slot->ring_i_tag_owner == station_id_ &&
      local_waiting_for_output(subnet, out_dir);
  const bool must_preserve_tag =
      slot->ring_i_tag_owner != tm_ring_invalid_tag_owner() &&
      (slot->ring_i_tag_owner != station_id_ || owner_still_waiting);

  if (slot->ring_i_tag_owner != tm_ring_invalid_tag_owner()) {
    if (must_preserve_tag) {
      output_used[out_idx] = true;
    }
    *replacement_sent = try_slot_replacement(slot, subnet, out_dir, true);
    return !must_preserve_tag || *replacement_sent;
  }

  *replacement_sent = try_slot_replacement(slot, subnet, out_dir, false);
  return true;
}

bool TmRingCrossStation::forward_slot(p_tm_pld_t slot, TmRingPortDir out_dir) {
  return output_conn(out_dir)->accept_slot(slot);
}

bool TmRingCrossStation::can_eject(p_tm_pld_t slot, TmRingSubnet subnet) const {
  const uint32_t idx = tm_ring_subnet_index(subnet);
  if (!e_tag_reserved_[idx] || owns_e_tag(slot, subnet)) {
    return node_interface_->has_eject_capacity(subnet);
  }
  return node_interface_->has_eject_capacity(subnet, 1);
}

bool TmRingCrossStation::owns_e_tag(p_tm_pld_t slot,
                                    TmRingSubnet subnet) const {
  const uint32_t idx = tm_ring_subnet_index(subnet);
  return e_tag_reserved_[idx] && slot->ring_e_tag_owner == station_id_ &&
         e_tag_txn_keys_[idx] == tm_ring_packet_txn_key(slot);
}

bool TmRingCrossStation::mark_e_tag_for_forward(p_tm_pld_t slot,
                                                TmRingSubnet subnet) {
  const uint32_t idx = tm_ring_subnet_index(subnet);
  const bool marked = !e_tag_reserved_[idx] &&
                      slot->ring_e_tag_owner == tm_ring_invalid_tag_owner();
  if (marked) {
    slot->ring_e_tag_owner = station_id_;
  }
  return marked;
}

void TmRingCrossStation::rollback_e_tag_mark(p_tm_pld_t slot, bool marked) {
  if (marked) {
    slot->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  }
}

void TmRingCrossStation::claim_e_tag(p_tm_pld_t slot, TmRingSubnet subnet) {
  const uint32_t idx = tm_ring_subnet_index(subnet);
  if (owns_e_tag(slot, subnet)) {
    e_tag_reserved_[idx] = false;
    e_tag_txn_keys_[idx] = TmPldTxnKey();
    slot->ring_e_tag_owner = tm_ring_invalid_tag_owner();
    stats_.e_tag_claims++;
  }
}

void TmRingCrossStation::commit_eject(p_tm_pld_t slot, TmRingSubnet subnet) {
  claim_e_tag(slot, subnet);
  node_interface_->push_eject(subnet, slot);
  stats_.ejected_packets++;
}

void TmRingCrossStation::commit_fanout_eject(p_tm_pld_t envelope,
                                             p_tm_pld_t response,
                                             TmRingSubnet subnet) {
  claim_e_tag(envelope, subnet);
  response->ring_slot_empty = false;
  response->ring_direction = static_cast<uint32_t>(TmRingPortDir::LOCAL);
  response->ring_i_tag_owner = tm_ring_invalid_tag_owner();
  response->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  node_interface_->push_eject(subnet, response);
  stats_.ejected_packets++;
}

p_tm_com_que_t TmRingCrossStation::transit_reg(TmRingPortDir in_dir,
                                               TmRingSubnet subnet) const {
  return transit_regs_[static_cast<uint32_t>(subnet)][direction_index(in_dir)];
}

p_tm_ring_conn_t TmRingCrossStation::output_conn(TmRingPortDir out_dir) const {
  return out_dir == TmRingPortDir::CW ? cw_out_conn_ : ccw_out_conn_;
}

uint32_t TmRingCrossStation::destination_node(p_tm_pld_t slot) const {
  return tm_pld_dst_node(slot);
}

bool TmRingCrossStation::is_destination(p_tm_pld_t slot) const {
  return station_id_ == destination_node(slot);
}

bool TmRingCrossStation::fanout_recipient_for_station(
    p_tm_pld_t slot, size_t* recipient_index) const {
  if (!tm_ring_fanout_active(slot)) {
    return false;
  }
  const std::vector<TmRingFanoutRecipient>& recipients =
      slot->ring_fanout->recipients;
  for (size_t index = 0; index < recipients.size(); ++index) {
    if (tm_ring_fanout_has_station(*slot->ring_fanout, station_id_) &&
        recipients[index].dst_node == station_id_) {
      *recipient_index = index;
      return true;
    }
  }
  return false;
}

p_tm_pld_t TmRingCrossStation::make_fanout_forward(
    p_tm_pld_t slot, size_t recipient_index) const {
  p_tm_pld_t forward = tm_make_pld(slot);
  forward->ring_fanout =
      tm_ring_clone_fanout_state(slot->ring_fanout);
  tm_ring_fanout_clear_station(forward->ring_fanout.get(),
                               forward->ring_fanout->recipients[recipient_index]
                                   .dst_node);
  return forward;
}

p_tm_pld_t TmRingCrossStation::make_fanout_response(
    p_tm_pld_t envelope, size_t recipient_index) const {
  const TmRingFanoutRecipient& recipient =
      envelope->ring_fanout->recipients[recipient_index];
  p_tm_pld_t response = tm_make_pld(recipient.response_template);
  response->ring_fanout.reset();
  response->cmd = PldCmd::RD_RSP;
  response->rsp = envelope->rsp;
  response->rsp_count = 1;
  response->addr = recipient.response_template->addr;
  const uint32_t response_size = recipient.response_template->size;
  response->size = response_size;
  tm_pld_set_ring_route(response, tm_pld_req_type(envelope),
                        tm_pld_target_id(envelope),
                        tm_pld_src_node(envelope), recipient.dst_node);
  // Recipient responses share the physical carrier; each response points at
  // its own carrier slice while retaining the original request range.
  response->buf_u8 = envelope->buf_u8;
  response->data = response->buf_u8 == nullptr
                       ? nullptr
                       : response->buf_u8->data() + recipient.payload_offset;
  response->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
  response->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
  response->ring_direction = static_cast<uint32_t>(TmRingPortDir::LOCAL);
  response->ring_slot_empty = false;
  response->ring_i_tag_owner = tm_ring_invalid_tag_owner();
  response->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  response->ring_deflection_count = 0;
  return response;
}

TmRingPortDir TmRingCrossStation::slot_direction(p_tm_pld_t slot) const {
  return static_cast<TmRingPortDir>(slot->ring_direction);
}

uint32_t TmRingCrossStation::direction_index(TmRingPortDir dir) const {
  return dir == TmRingPortDir::CW ? 0 : 1;
}

bool TmRingCrossStation::local_waiting_for_output(TmRingSubnet subnet,
                                                   TmRingPortDir out_dir) const {
  if (node_interface_ == nullptr) {
    return false;
  }
  return node_interface_->front_inject(subnet, out_dir) != nullptr;
}

bool TmRingCrossStation::incoming_transit_ready(TmRingSubnet subnet,
                                                TmRingPortDir out_dir) {
  auto input_conn = out_dir == TmRingPortDir::CW ? ccw_in_conn_ : cw_in_conn_;
  return input_conn->has_ready_slot(subnet);
}

void TmRingCrossStation::release_ring_slot(TmRingSubnet subnet,
                                           TmRingPortDir out_dir) {
  slot_pool_->release(subnet, out_dir);
}

p_tm_pld_t TmRingCrossStation::make_tagged_empty_slot(p_tm_pld_t source) const {
  auto empty = tm_make_pld();
  empty->ring_subnet = source->ring_subnet;
  empty->ring_traffic_class = source->ring_traffic_class;
  empty->ring_direction = source->ring_direction;
  empty->ring_slot_empty = true;
  empty->ring_i_tag_owner = source->ring_i_tag_owner;
  empty->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  return empty;
}
