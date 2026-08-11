#include "tm_ring_l2_buffer_node.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "tm_ring_fanout.h"

using namespace tm_engine;

TmRingL2BufferNode::TmRingL2BufferNode(const std::string& name,
                                       p_tm_clk_t clk,
                                       const TmRingL2TrafficConfig& cfg,
                                       const TmRingEndpointQueueDepths& queue_depths)
    : TmModule(name),
      cfg_(cfg) {
  node_interface_ = tm_make_ring_node_interface(
      clk, name + "_node_interface", queue_depths);
  response_q_ = tm_make_que<p_tm_pld_t>(
      clk, name + "_response_q", cfg_.buffer_depth, cfg_.response_latency);
  issue_ready_event_ = tm_make_event(name + "_issue_ready");
  tm_sensitive(TM_MAKE_CPROC(&TmRingL2BufferNode::service_ready_response),
               response_q_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingL2BufferNode::release_issue_token),
               issue_ready_event_);
  tm_sensitive(TM_MAKE_CPROC(&TmRingL2BufferNode::service), clk->pos_edge);
  reset();
}

void TmRingL2BufferNode::attach(uint32_t target_id,
                                std::shared_ptr<TmRingTopology> topology) {
  target_id_ = target_id;
  topology_ = topology;
}

void TmRingL2BufferNode::reset() {
  retired_issue_ready_event_pending_ =
      retired_issue_ready_event_pending_ || issue_ready_event_pending_;
  issue_ready_event_pending_ = false;
  response_q_->clear();
  response_count_ = 0;
  ready_response_count_ = 0;
  issue_token_available_ = true;
  open_groups_.clear();
  frozen_summary_response_ = nullptr;
  frozen_summary_ = TmRingL2GroupSummary();
  frozen_summaries_.clear();
  frozen_carrier_ = nullptr;
  frozen_carrier_vring_ = 0;
  clear_stats();
  node_interface_->reset();
}

void TmRingL2BufferNode::clear_stats() {
  stats_.clear();
}

bool TmRingL2BufferNode::idle() const {
  return !issue_ready_event_pending_ && !retired_issue_ready_event_pending_ &&
         response_count_ == 0 && open_groups_.empty() &&
         frozen_summary_response_ == nullptr && frozen_summaries_.empty() &&
         frozen_carrier_ == nullptr && node_interface_->idle();
}

bool TmRingL2BufferNode::has_capacity() {
  return response_count_ < cfg_.buffer_depth && !response_q_->full();
}

bool TmRingL2BufferNode::fanout_candidate_valid(
    const TmRingL2ResponseCandidate& candidate) const {
  if (cfg_.sector_size == 0 || cfg_.line_size == 0 ||
      candidate.response == nullptr ||
      candidate.response->size < cfg_.sector_size ||
      candidate.response->size > cfg_.line_size ||
      candidate.response->size % cfg_.sector_size != 0 ||
      candidate.payload_offset % cfg_.sector_size != 0 ||
      candidate.payload_offset > cfg_.line_size ||
      candidate.response->size >
          cfg_.line_size - candidate.payload_offset) {
    return false;
  }
  const uint32_t sector_count =
      candidate.response->size / cfg_.sector_size;
  return (sector_count & (sector_count - 1)) == 0;
}

uint64_t TmRingL2BufferNode::allocate_group_token() {
  ++next_group_token_;
  if (next_group_token_ == 0) {
    ++next_group_token_;
  }
  return next_group_token_;
}

TmRingL2AcceptResult TmRingL2BufferNode::accept_response(
    const TmRingL2ResponseCandidate& candidate) {
  TmRingL2AcceptResult result;
  if (candidate.response == nullptr || candidate.completion_data == nullptr ||
      topology_ == nullptr ||
      candidate.response->size == 0 ||
      candidate.payload_offset > candidate.completion_data->size() ||
      candidate.response->size >
          candidate.completion_data->size() - candidate.payload_offset) {
    throw std::invalid_argument("Invalid L2 response candidate");
  }

  const bool use_fanout =
      candidate.fanout_eligible && fanout_candidate_valid(candidate);

  if (use_fanout) {
    if (candidate.open_group_token != 0) {
      const std::unordered_map<uint64_t, p_tm_pld_t>::const_iterator group_it =
          open_groups_.find(candidate.open_group_token);
      if (group_it == open_groups_.end() || group_it->second == nullptr ||
          !tm_ring_has_fanout(group_it->second)) {
        throw std::logic_error("Open L2 group token is missing");
      }

      p_tm_pld_t envelope = group_it->second;
      if (envelope->ring_fanout->line_base != candidate.line_base ||
          envelope->ring_fanout->group_token != candidate.open_group_token ||
          !append_fanout_recipient(envelope, candidate)) {
        throw std::logic_error("Open L2 group does not match candidate");
      }
      result.status = TmRingL2AcceptStatus::MERGED_GROUP;
      result.group_token = envelope->ring_fanout->group_token;
      return result;
    }

    if (!has_capacity()) {
      stats_.buffer_full_stall_cycles++;
      result.status = TmRingL2AcceptStatus::REJECTED_BUFFER_FULL;
      return result;
    }

    const uint64_t group_token = allocate_group_token();
    p_tm_pld_t envelope = make_fanout_envelope(candidate, group_token);
    if (envelope == nullptr ||
        !append_fanout_recipient(envelope, candidate)) {
      throw std::logic_error("Failed to create L2 fanout group");
    }
    response_q_->push_back(envelope);
    response_count_++;
    open_groups_[group_token] = envelope;
    record_accepted_entry();
    result.status = TmRingL2AcceptStatus::ACCEPTED_NEW_GROUP;
    result.group_token = group_token;
    return result;
  }

  if (!has_capacity()) {
    stats_.buffer_full_stall_cycles++;
    result.status = TmRingL2AcceptStatus::REJECTED_BUFFER_FULL;
    return result;
  }

  p_tm_pld_t response = make_unicast_response(candidate);
  if (response == nullptr) {
    throw std::logic_error("Failed to create L2 unicast response");
  }
  response_q_->push_back(response);
  response_count_++;
  record_accepted_entry();
  result.status = TmRingL2AcceptStatus::ACCEPTED_UNICAST;
  return result;
}

std::vector<TmRingL2GroupSummary> TmRingL2BufferNode::take_frozen_summaries() {
  std::vector<TmRingL2GroupSummary> summaries = frozen_summaries_;
  frozen_summaries_.clear();
  return summaries;
}

void TmRingL2BufferNode::record_accepted_entry() {
  stats_.responses_accepted++;
  stats_.latency_wait_cycles += cfg_.response_latency;
  stats_.buffer_occupancy_peak = std::max<uint64_t>(
      stats_.buffer_occupancy_peak, static_cast<uint64_t>(response_count_));
}

p_tm_pld_t TmRingL2BufferNode::make_unicast_response(
    const TmRingL2ResponseCandidate& candidate) const {
  p_tm_pld_t response = tm_make_pld(candidate.response);
  if (response == nullptr) {
    return nullptr;
  }

  response->ring_fanout.reset();
  response->buf_u8 = std::make_shared<std::vector<uint8_t>>(
      candidate.response->size, 0);
  response->data = response->buf_u8->data();
  if (response->rsp == PldRsp::OK) {
    std::memcpy(response->data,
                candidate.completion_data->data() + candidate.payload_offset,
                candidate.response->size);
  }
  response->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
  response->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
  response->ring_slot_empty = false;
  response->ring_i_tag_owner = tm_ring_invalid_tag_owner();
  response->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  response->reset_ring_deflection_state();
  return response;
}

p_tm_pld_t TmRingL2BufferNode::make_fanout_envelope(
    const TmRingL2ResponseCandidate& candidate,
    uint64_t group_token) const {
  p_tm_pld_t envelope = tm_make_pld(candidate.response);
  if (envelope == nullptr) {
    return nullptr;
  }

  envelope->ring_fanout = std::make_shared<TmRingFanoutState>();
  envelope->ring_fanout->active_on_ring = false;
  envelope->ring_fanout->txn_key = tm_pld_txn_key(candidate.response);
  envelope->ring_fanout->line_base = candidate.line_base;
  envelope->ring_fanout->group_token = group_token;
  envelope->ring_fanout->pending_stations = 0;
  envelope->ring_fanout->source_data = candidate.completion_data;
  envelope->buf_u8.reset();
  envelope->data = nullptr;
  envelope->addr = candidate.line_base;
  envelope->size = 0;
  envelope->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
  envelope->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
  envelope->ring_slot_empty = false;
  envelope->ring_i_tag_owner = tm_ring_invalid_tag_owner();
  envelope->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  envelope->reset_ring_deflection_state();
  return envelope;
}

bool TmRingL2BufferNode::append_fanout_recipient(
    p_tm_pld_t envelope,
    const TmRingL2ResponseCandidate& candidate) {
  if (envelope == nullptr || !tm_ring_has_fanout(envelope) ||
      envelope->ring_fanout->source_data != candidate.completion_data ||
      envelope->ring_fanout->line_base != candidate.line_base) {
    return false;
  }

  const TmRingLocation destination =
      topology_->master_location(candidate.response->mst_id);
  for (const TmRingFanoutRecipient& recipient :
       envelope->ring_fanout->recipients) {
    if (recipient.dst_ring_id == destination.ring_id &&
        recipient.dst_node == destination.station_id) {
      return false;
    }
  }

  TmRingFanoutRecipient recipient;
  recipient.dst_ring_id = destination.ring_id;
  recipient.dst_node = destination.station_id;
  recipient.payload_offset = candidate.payload_offset;
  recipient.response_template = tm_make_pld(candidate.response);
  if (recipient.response_template == nullptr) {
    return false;
  }
  recipient.response_template->ring_fanout.reset();
  recipient.response_template->buf_u8.reset();
  recipient.response_template->data = nullptr;
  recipient.response_template->ring_slot_empty = false;
  recipient.response_template->ring_i_tag_owner = tm_ring_invalid_tag_owner();
  recipient.response_template->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  recipient.response_template->reset_ring_deflection_state();

  if (!envelope->ring_fanout->recipients.empty()) {
    const TmRingFanoutRecipient& first =
        envelope->ring_fanout->recipients.front();
    if (first.response_template->addr != recipient.response_template->addr ||
        first.response_template->size != recipient.response_template->size) {
      envelope->ring_fanout->mode = TmRingFanoutMode::SCATTER;
    }
  }
  envelope->ring_fanout->recipients.push_back(recipient);
  return true;
}

uint32_t TmRingL2BufferNode::carrier_size(
    p_tm_pld_t envelope, uint32_t* carrier_offset) const {
  if (carrier_offset == nullptr || envelope == nullptr ||
      !tm_ring_has_fanout(envelope) || cfg_.sector_size == 0 ||
      cfg_.line_size == 0) {
    return 0;
  }

  uint32_t min_offset = std::numeric_limits<uint32_t>::max();
  uint64_t max_end = 0;
  for (const TmRingFanoutRecipient& recipient :
       envelope->ring_fanout->recipients) {
    if (recipient.response_template == nullptr ||
        recipient.response_template->size == 0 ||
        recipient.payload_offset > cfg_.line_size ||
        recipient.response_template->size >
            cfg_.line_size - recipient.payload_offset) {
      return 0;
    }
    min_offset = std::min(min_offset, recipient.payload_offset);
    max_end = std::max<uint64_t>(
        max_end, static_cast<uint64_t>(recipient.payload_offset) +
                     recipient.response_template->size);
  }
  if (min_offset == std::numeric_limits<uint32_t>::max()) {
    return 0;
  }

  uint32_t offset = (min_offset / cfg_.sector_size) * cfg_.sector_size;
  const uint64_t needed = max_end - offset;
  uint32_t size = cfg_.sector_size;
  while (size < needed && size < cfg_.line_size) {
    if (size > cfg_.line_size / 2) {
      size = cfg_.line_size;
      break;
    }
    size *= 2;
  }
  if (size > cfg_.line_size ||
      static_cast<uint64_t>(offset) + size > cfg_.line_size) {
    offset = 0;
    size = cfg_.line_size;
  }
  if (size < needed || size == 0) {
    return 0;
  }
  *carrier_offset = offset;
  return size;
}

void TmRingL2BufferNode::freeze_fanout_group(p_tm_pld_t envelope) {
  if (envelope == nullptr || !tm_ring_has_fanout(envelope) ||
      frozen_summary_response_ == envelope) {
    return;
  }
  if (frozen_summary_response_ != nullptr ||
      envelope->ring_fanout->group_token == 0 ||
      envelope->ring_fanout->recipients.empty() ||
      envelope->ring_fanout->source_data == nullptr) {
    throw std::logic_error("Invalid L2 fanout group freeze");
  }

  const uint64_t group_token = envelope->ring_fanout->group_token;
  const std::unordered_map<uint64_t, p_tm_pld_t>::const_iterator group_it =
      open_groups_.find(group_token);
  if (group_it == open_groups_.end() || group_it->second != envelope) {
    throw std::logic_error("Open L2 group token is missing at freeze");
  }

  envelope->ring_fanout->active_on_ring = false;
  envelope->ring_fanout->pending_stations = 0;
  frozen_summary_.group_token = group_token;
  frozen_summary_.mode = envelope->ring_fanout->mode;
  frozen_summary_.recipient_count = static_cast<uint32_t>(
      envelope->ring_fanout->recipients.size());
  frozen_summary_response_ = envelope;
  frozen_summaries_.push_back(frozen_summary_);
  open_groups_.erase(group_it);
}

p_tm_pld_t TmRingL2BufferNode::make_frozen_carrier(
    p_tm_pld_t envelope, uint32_t target_vring) const {
  if (envelope == nullptr || !tm_ring_has_fanout(envelope)) {
    return nullptr;
  }

  p_tm_pld_t carrier = tm_make_pld(envelope);
  if (carrier == nullptr) {
    return nullptr;
  }
  carrier->ring_fanout =
      tm_ring_clone_fanout_state(envelope->ring_fanout);
  carrier->ring_fanout->active_on_ring = false;
  carrier->ring_fanout->pending_stations = 0;
  carrier->ring_fanout->recipients.clear();
  for (const TmRingFanoutRecipient& recipient :
       envelope->ring_fanout->recipients) {
    if (recipient.dst_ring_id == target_vring) {
      carrier->ring_fanout->recipients.push_back(recipient);
    }
  }
  if (carrier->ring_fanout->recipients.empty()) {
    return nullptr;
  }
  const TmRingFanoutRecipient& first =
      carrier->ring_fanout->recipients.front();
  carrier->ring_fanout->mode = TmRingFanoutMode::MULTICAST;
  for (const TmRingFanoutRecipient& recipient :
       carrier->ring_fanout->recipients) {
    if (recipient.response_template->addr != first.response_template->addr ||
        recipient.response_template->size != first.response_template->size) {
      carrier->ring_fanout->mode = TmRingFanoutMode::SCATTER;
      break;
    }
  }
  if (!materialize_fanout(carrier)) {
    return nullptr;
  }

  carrier->mst_id = first.response_template->mst_id;
  carrier->gid = first.response_template->gid;
  return carrier;
}

bool TmRingL2BufferNode::materialize_fanout(p_tm_pld_t carrier) const {
  if (carrier == nullptr || !tm_ring_has_fanout(carrier) ||
      carrier->ring_fanout->source_data == nullptr) {
    return false;
  }

  uint32_t carrier_offset = 0;
  const uint32_t physical_size = carrier_size(carrier, &carrier_offset);
  if (physical_size == 0 ||
      carrier_offset > carrier->ring_fanout->source_data->size() ||
      physical_size >
          carrier->ring_fanout->source_data->size() - carrier_offset) {
    return false;
  }

  const std::shared_ptr<const std::vector<uint8_t>> source_data =
      carrier->ring_fanout->source_data;
  carrier->buf_u8 =
      std::make_shared<std::vector<uint8_t>>(physical_size, 0);
  carrier->data = carrier->buf_u8->data();
  if (carrier->rsp == PldRsp::OK) {
    std::memcpy(carrier->data, source_data->data() + carrier_offset,
                physical_size);
  }
  carrier->addr = carrier->ring_fanout->line_base + carrier_offset;
  carrier->size = physical_size;

  for (TmRingFanoutRecipient& recipient :
       carrier->ring_fanout->recipients) {
    recipient.payload_offset -= carrier_offset;
  }
  carrier->ring_fanout->source_data.reset();
  return true;
}

p_tm_ring_node_interface_t TmRingL2BufferNode::node_interface() const {
  return node_interface_;
}

const TmRingL2BufferStats& TmRingL2BufferNode::stats() const {
  return stats_;
}

void TmRingL2BufferNode::prepare_dat_route(p_tm_pld_t rsp) {
  const TmRingLocation src = topology_->l2_location(target_id_);
  TmRingLocation destination;
  TmRingPortDir direction = TmRingPortDir::CW;
  if (tm_ring_has_fanout(rsp)) {
    if (rsp->ring_fanout->active_on_ring ||
        rsp->ring_fanout->recipients.empty()) {
      throw std::logic_error("Invalid dormant H-Ring fanout carrier");
    }
    const TmRingFanoutRecipient& first =
        rsp->ring_fanout->recipients.front();
    for (const TmRingFanoutRecipient& recipient :
         rsp->ring_fanout->recipients) {
      if (recipient.dst_ring_id != first.dst_ring_id) {
        throw std::logic_error("Mixed V-Ring L2 carrier");
      }
    }
    destination = topology_->rbrg_h_location(first.dst_ring_id);
    rsp->mst_id = first.response_template->mst_id;
    rsp->gid = first.response_template->gid;
  } else {
    destination =
        topology_->rbrg_h_location(topology_->master_vring(rsp->mst_id));
  }
  direction = topology_->route_direction(src, destination);

  // Ring route metadata always records the physical packet endpoints.
  tm_pld_set_ring_route(rsp, static_cast<uint32_t>(PldCmd::RD), target_id_,
                        src.station_id, destination.station_id);
  rsp->cmd = PldCmd::RD_RSP;
  rsp->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
  rsp->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
  rsp->ring_direction = static_cast<uint32_t>(direction);
}

void TmRingL2BufferNode::service_ready_response() {
  // response_q_ 的 vld 事件已经包含 response_latency；时钟沿重试必须等
  // 队首收到该事件后才能发送，不能仅凭队列非空绕过固定延迟。
  if (ready_response_count_ < response_count_) {
    ready_response_count_++;
  }
  if (ready_response_count_ != 0 && response_q_->valid() &&
      !response_q_->empty()) {
    freeze_fanout_group(response_q_->front());
  }
  // 非零固定延迟到期时立即尝试发送；零延迟配置仍由 pos-edge 统一发射，
  // 避免同一个模拟时刻连续注入多个响应。
  if (cfg_.response_latency != 0 && cfg_.issue_interval != 0) {
    service();
  }
}

void TmRingL2BufferNode::release_issue_token() {
  if (retired_issue_ready_event_pending_) {
    retired_issue_ready_event_pending_ = false;
    return;
  }
  if (!issue_ready_event_pending_) {
    return;
  }
  issue_ready_event_pending_ = false;
  // 上一笔 DAT 已成功注入并等待完整 issue interval，才重新开放一个发送名额。
  issue_token_available_ = true;
  if (cfg_.issue_interval != 0) {
    service();
  }
}

void TmRingL2BufferNode::service() {
  if (ready_response_count_ == 0 || !response_q_->valid() ||
      response_q_->empty()) {
    return;
  }

  p_tm_pld_t logical_response = response_q_->front();
  freeze_fanout_group(logical_response);

  if (!issue_token_available_) {
    stats_.issue_interval_stall_cycles++;
    return;
  }

  const bool is_fanout_group = tm_ring_has_fanout(logical_response);
  p_tm_pld_t rsp = logical_response;
  uint32_t recipient_count = 1;
  if (is_fanout_group) {
    if (frozen_summary_response_ != logical_response ||
        logical_response->ring_fanout->recipients.empty()) {
      throw std::logic_error("L2 fanout group is not frozen");
    }
    if (frozen_carrier_ == nullptr) {
      uint32_t target_vring = std::numeric_limits<uint32_t>::max();
      for (const TmRingFanoutRecipient& recipient :
           logical_response->ring_fanout->recipients) {
        target_vring = std::min(target_vring, recipient.dst_ring_id);
      }
      frozen_carrier_ =
          make_frozen_carrier(logical_response, target_vring);
      if (frozen_carrier_ == nullptr) {
        throw std::logic_error("Failed to materialize L2 fanout carrier");
      }
      frozen_carrier_vring_ = target_vring;
      prepare_dat_route(frozen_carrier_);
    }
    rsp = frozen_carrier_;
    recipient_count =
        static_cast<uint32_t>(rsp->ring_fanout->recipients.size());
  } else {
    prepare_dat_route(rsp);
  }
  if (!node_interface_->push_inject(TmRingSubnet::DAT, rsp)) {
    stats_.dat_inject_full_stall_cycles++;
    return;
  }

  stats_.h_carriers++;
  stats_.dat_bytes += rsp->size;
  stats_.h_carrier_recipients += recipient_count;
  if (recipient_count == 1) {
    stats_.h_unicast_carriers++;
  } else if (rsp->ring_fanout->mode ==
             TmRingFanoutMode::MULTICAST) {
    stats_.h_multicast_carriers++;
  } else {
    stats_.h_scatter_carriers++;
  }
  const uint32_t carrier_bytes = rsp->size;
  if (carrier_bytes == 128) {
    stats_.injected_carrier_128b++;
  } else if (carrier_bytes == 256) {
    stats_.injected_carrier_256b++;
  } else if (carrier_bytes == 512) {
    stats_.injected_carrier_512b++;
  } else {
    stats_.injected_carrier_other++;
  }
  bool logical_response_complete = true;
  if (is_fanout_group) {
    std::vector<TmRingFanoutRecipient>& recipients =
        logical_response->ring_fanout->recipients;
    recipients.erase(
        std::remove_if(recipients.begin(), recipients.end(),
                       [this](const TmRingFanoutRecipient& recipient) {
                         return recipient.dst_ring_id ==
                                frozen_carrier_vring_;
                       }),
        recipients.end());
    logical_response_complete = recipients.empty();
    frozen_carrier_ = nullptr;
    frozen_carrier_vring_ = 0;
  }
  if (logical_response_complete) {
    if (is_fanout_group) {
      frozen_summary_response_ = nullptr;
      frozen_summary_ = TmRingL2GroupSummary();
    }
    response_q_->pop_front();
    response_count_--;
    ready_response_count_--;
  }
  if (cfg_.issue_interval == 0) {
    // 没有额外间隔时立即归还令牌；0 配置的 service() 只由 pos-edge 调用，
    // 因此同一时刻仍至多注入一包。
    issue_token_available_ = true;
    return;
  }

  issue_token_available_ = false;
  issue_ready_event_pending_ = true;
  // 仅在 DAT Inject Bank 已提交本包后启动下一张发送令牌。
  issue_ready_event_->notify_after(cfg_.issue_interval);
}
