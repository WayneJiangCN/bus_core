#include "tm_ring_home_agent.h"

#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <vector>

TmRingHomeAgent::TmRingHomeAgent(const TmRingHomeAgentConfig& cfg)
    : cfg_(cfg) {
  reset();
}

void TmRingHomeAgent::reset() {
  entries_.clear();
  backend_rr_cursor_ = entries_.end();
  backend_candidate_ = entries_.end();
  functional_candidate_ = entries_.end();
  response_rr_cursor_ = entries_.end();
  response_candidate_ = entries_.end();
  write_lines_.clear();
  write_txn_lines_.clear();
  clear_stats();
}

void TmRingHomeAgent::clear_stats() {
  stats_.clear();
}

/*
 * 做什么：接收一笔 Ring 读请求，建立或合并一个完整 line transaction。
 * 输入/前提：pld 是 MemPort 的队首；返回 STALL 时调用者必须保留该 payload。
 * 核心流程：先检查写 hazard 和 line 边界，再以 line_base 查找活跃 transaction；
 *            新 transaction 立即处于 PENDING。
 * 结果/重试：首笔请求无需 timer 即可作为 backend candidate；同 key 请求追加 waiter；
 *            表项或 waiter 满时由 MemPort 在后续拍重试。
 */
TmHaAcceptResult TmRingHomeAgent::accept_read(p_tm_pld_t pld) {
  if (pld == nullptr) {
    return TmHaAcceptResult::BYPASS;
  }

  const uint64_t req_line_base = line_base(pld->addr);
  if (write_lines_.find(req_line_base) != write_lines_.end()) {
    stats_.write_hazard_stall_cycles++;
    return TmHaAcceptResult::STALL_WRITE_HAZARD;
  }

  if (!in_one_line(pld)) {
    return TmHaAcceptResult::BYPASS;
  }

  TmHaTxnIter transaction_it;
  if (find_transaction(req_line_base, &transaction_it)) {
    TmHaReadTxn& transaction = *transaction_it;
    if (!transaction.waiter_admission_open) {
      stats_.aggregation_closed_stall_cycles++;
      return TmHaAcceptResult::STALL_AGGREGATION_CLOSED;
    }
    if (transaction.waiters.size() >= cfg_.waiters_per_entry) {
      stats_.waiter_full_stall_cycles++;
      return TmHaAcceptResult::STALL_WAITER_FULL;
    }

    transaction.waiters.push_back(make_waiter(pld));
    record_read(pld);
    if (transaction.state == TmHaTxnState::PENDING_HIT ||
        transaction.state == TmHaTxnState::PENDING_MISS) {
      stats_.rd_merged_pending++;
    } else if (transaction.state == TmHaTxnState::INFLIGHT) {
      stats_.rd_merged_inflight++;
    } else {
      stats_.rd_merged_responding++;
    }
    stats_.backend_read_saved++;
    return TmHaAcceptResult::MERGED;
  }

  if (entries_.size() >= cfg_.entry_limit) {
    stats_.table_full_stall_cycles++;
    return TmHaAcceptResult::STALL_TABLE_FULL;
  }

  TmHaReadTxn transaction;
  transaction.line_base = req_line_base;
  transaction.waiter_admission_open = true;
  const bool l2_hit = classify_l2_hit(req_line_base, pld->gid);
  transaction.state = l2_hit ? TmHaTxnState::PENDING_HIT
                             : TmHaTxnState::PENDING_MISS;
  transaction.waiters.push_back(make_waiter(pld));
  entries_.push_back(transaction);
  record_read(pld);
  stats_.rd_entries_allocated++;
  if (l2_hit) {
    stats_.l2_hit_transactions++;
  } else {
    stats_.l2_miss_transactions++;
  }
  return TmHaAcceptResult::ACCEPTED;
}

/*
 * 做什么：为 WR/WR_DAT 建立同一 cacheline 的顺序保护。
 * 输入/前提：同一 write transaction 的 WR_DAT 可复用已经建立的 hazard。
 * 核心流程：新写先登记 line owner，随后检查更早读是否仍处于 PENDING 或 INFLIGHT。
 * 结果/重试：旧读的数据一旦被 HA 捕获为 RESPONDING，写可继续；后续同 line 读保持阻塞。
 */
bool TmRingHomeAgent::reserve_write(p_tm_pld_t pld) {
  if (pld == nullptr ||
      (pld->cmd != PldCmd::WR && pld->cmd != PldCmd::WR_DAT)) {
    return false;
  }

  const uint64_t req_line_base = line_base(pld->addr);
  const TmPldTxnKey txn_key = tm_pld_txn_key(pld);
  std::unordered_map<TmPldTxnKey, uint64_t, TmPldTxnKeyHash>::iterator txn_it =
      write_txn_lines_.find(txn_key);
  const bool known_txn = txn_it != write_txn_lines_.end();
  if (known_txn && txn_it->second != req_line_base) {
    stats_.write_hazard_stall_cycles++;
    return false;
  }

  if (!known_txn) {
    if (write_lines_.find(req_line_base) != write_lines_.end()) {
      stats_.write_hazard_stall_cycles++;
      return false;
    }
    write_txn_lines_[txn_key] = req_line_base;
    write_lines_.insert(req_line_base);
  }

  // WR_DAT 属于已经登记的写事务；当前协议允许它继续占用写数据路径。
  if (known_txn && pld->cmd == PldCmd::WR_DAT) {
    return true;
  }

  if (has_blocking_read_transaction(req_line_base)) {
    stats_.write_hazard_stall_cycles++;
    return false;
  }
  return true;
}

void TmRingHomeAgent::complete_write(p_tm_pld_t pld) {
  if (pld == nullptr) {
    return;
  }

  const TmPldTxnKey txn_key = tm_pld_txn_key(pld);
  const std::unordered_map<TmPldTxnKey, uint64_t,
                           TmPldTxnKeyHash>::iterator txn_it =
      write_txn_lines_.find(txn_key);
  if (txn_it == write_txn_lines_.end()) {
    return;
  }

  const uint64_t req_line_base = txn_it->second;
  write_txn_lines_.erase(txn_it);
  write_lines_.erase(req_line_base);
}

bool TmRingHomeAgent::has_backend_request() {
  if (backend_candidate_ != entries_.end()) {
    return true;
  }
  return find_pending_transaction(&backend_candidate_);
}

/*
 * 做什么：返回一个已接收但尚未提交的 line backend read。
 * 输入/前提：调用者在 send 前可以重复调用本函数。
 * 核心流程：固定 RR 选中的 PENDING transaction 并缓存其 backend payload。
 * 结果/重试：send 失败时 transaction 和 payload 不变；commit 后才进入 INFLIGHT。
 */
p_tm_pld_t TmRingHomeAgent::front_backend_request() {
  if (!has_backend_request()) {
    return nullptr;
  }
  TmHaReadTxn& transaction = *backend_candidate_;
  if (transaction.backend_request == nullptr) {
    transaction.backend_request = make_backend_request(transaction);
  }
  return transaction.backend_request;
}

void TmRingHomeAgent::commit_backend_request() {
  p_tm_pld_t backend_request = front_backend_request();
  if (backend_request == nullptr) {
    return;
  }

  TmHaReadTxn& transaction = *backend_candidate_;
  transaction.state = TmHaTxnState::INFLIGHT;
  transaction.backend_transaction_key = tm_pld_txn_key(backend_request);
  stats_.rd_backend_issued++;
  stats_.backend_read_bytes += cfg_.line_size;

  backend_rr_cursor_ = std::next(backend_candidate_);
  if (backend_rr_cursor_ == entries_.end()) {
    backend_rr_cursor_ = entries_.begin();
  }
  backend_candidate_ = entries_.end();
}

bool TmRingHomeAgent::has_functional_read() {
  if (functional_candidate_ != entries_.end()) {
    return true;
  }
  return find_pending_functional_transaction(&functional_candidate_);
}

p_tm_pld_t TmRingHomeAgent::front_functional_read() {
  if (!has_functional_read()) {
    return nullptr;
  }
  TmHaReadTxn& transaction = *functional_candidate_;
  if (transaction.functional_request == nullptr) {
    transaction.functional_request = make_backend_request(transaction);
  }
  return transaction.functional_request;
}

void TmRingHomeAgent::commit_functional_read(pld_rsp_t status) {
  p_tm_pld_t functional_read = front_functional_read();
  if (functional_read == nullptr) {
    return;
  }

  TmHaReadTxn& transaction = *functional_candidate_;
  transaction.backend_response_status = status;
  transaction.completion_data =
      make_completion_data(functional_read, status);
  transaction.next_response_waiter = 0;
  transaction.state = TmHaTxnState::RESPONDING;
  stats_.functional_reads++;
  backend_rr_cursor_ = std::next(functional_candidate_);
  if (backend_rr_cursor_ == entries_.end()) {
    backend_rr_cursor_ = entries_.begin();
  }
  functional_candidate_ = entries_.end();

  const uint64_t completion_bytes = current_completion_buffer_bytes();
  if (completion_bytes > stats_.completion_buffer_bytes_peak) {
    stats_.completion_buffer_bytes_peak = completion_bytes;
  }
}

void TmRingHomeAgent::record_private_l2_full_stall() {
  stats_.private_l2_full_stall_cycles++;
}

/*
 * 做什么：捕获一笔 backend line response，并保存为 waiter 共享数据。
 * 输入/前提：OK 响应携带完整 line；ERR 响应允许没有 data。
 * 核心流程：用独立 backend transaction key 匹配唯一 INFLIGHT transaction，复制数据。
 * 结果/重试：成功后进入 RESPONDING；不匹配响应返回 false，调用者不得 pop 该 channel。
 */
bool TmRingHomeAgent::accept_backend_response(p_tm_pld_t rsp) {
  if (rsp == nullptr || (rsp->rsp != PldRsp::OK && rsp->rsp != PldRsp::ERR)) {
    return false;
  }
  if (rsp->rsp == PldRsp::OK &&
      (rsp->size != cfg_.line_size || rsp->data == nullptr)) {
    return false;
  }

  const TmPldTxnKey backend_transaction_key = tm_pld_txn_key(rsp);
  for (TmHaReadTxn& transaction : entries_) {
    if (transaction.state != TmHaTxnState::INFLIGHT ||
        transaction.backend_transaction_key != backend_transaction_key) {
      continue;
    }

    transaction.backend_response_status = rsp->rsp;
    transaction.completion_data = make_completion_data(rsp, rsp->rsp);
    transaction.next_response_waiter = 0;
    transaction.state = TmHaTxnState::RESPONDING;

    const uint64_t completion_bytes = current_completion_buffer_bytes();
    if (completion_bytes > stats_.completion_buffer_bytes_peak) {
      stats_.completion_buffer_bytes_peak = completion_bytes;
    }
    return true;
  }
  return false;
}

bool TmRingHomeAgent::has_l2_response() {
  if (response_candidate_ != entries_.end()) {
    return true;
  }
  return find_responding_transaction(&response_candidate_);
}

/*
 * 做什么：为当前 transaction 生成一个稳定的 L2 response candidate。
 * 输入/前提：调用者已确认存在可回复 transaction。
 * 核心流程：candidate 携带 waiter 元数据、group token 和共享 completion data；
 *            fanout 是否成立及物理 carrier 由 L2 Buffer 决定。
 * 结果/重试：front 不修改 HA 提交状态；下游失败时再次返回同一个 candidate。
 */
TmRingL2ResponseCandidate TmRingHomeAgent::front_l2_response() {
  if (!has_l2_response()) {
    return TmRingL2ResponseCandidate();
  }

  TmHaReadTxn& transaction = *response_candidate_;
  if (transaction.l2_candidate.response != nullptr) {
    return transaction.l2_candidate;
  }

  const TmHaWaiter& waiter =
      transaction.waiters[transaction.next_response_waiter];
  transaction.l2_candidate = make_l2_candidate(transaction, waiter);
  return transaction.l2_candidate;
}

void TmRingHomeAgent::commit_l2_response(
    const TmRingL2AcceptResult& result) {
  if (!result.accepted()) {
    return;
  }

  const TmRingL2ResponseCandidate candidate = front_l2_response();
  if (candidate.response == nullptr) {
    return;
  }

  const TmHaTxnIter transaction_it = response_candidate_;
  TmHaReadTxn& transaction = *transaction_it;
  if (result.is_group()) {
    if (!candidate.fanout_eligible || result.group_token == 0) {
      return;
    }
    if (result.status == TmRingL2AcceptStatus::ACCEPTED_NEW_GROUP) {
      if (candidate.open_group_token != 0) {
        return;
      }
    } else if (result.status == TmRingL2AcceptStatus::MERGED_GROUP) {
      if (candidate.open_group_token == 0 ||
          result.group_token != candidate.open_group_token) {
        return;
      }
    }
    transaction.open_group_token = result.group_token;
  }

  transaction.next_response_waiter++;

  // Once an L2 fanout group is open, feed its remaining waiters contiguously.
  // Rotating after every waiter can let response_latency expire before the
  // transaction is selected again, freezing a one-recipient "multicast".
  if (result.is_group() &&
      transaction.next_response_waiter < transaction.waiters.size()) {
    response_rr_cursor_ = transaction_it;
  } else {
    response_rr_cursor_ = std::next(transaction_it);
    if (response_rr_cursor_ == entries_.end()) {
      response_rr_cursor_ = entries_.begin();
    }
  }
  transaction.l2_candidate = TmRingL2ResponseCandidate();
  response_candidate_ = entries_.end();

  if (transaction.next_response_waiter == transaction.waiters.size() &&
      transaction.open_group_token == 0) {
    erase_transaction(transaction_it);
  }
}

bool TmRingHomeAgent::consume_l2_group_summary(
    const TmRingL2GroupSummary& summary) {
  if (summary.group_token == 0 || summary.recipient_count == 0 ||
      (summary.mode != TmRingFanoutMode::MULTICAST &&
       summary.mode != TmRingFanoutMode::SCATTER)) {
    return false;
  }

  TmHaTxnIter transaction_it = entries_.begin();
  for (; transaction_it != entries_.end(); ++transaction_it) {
    if (transaction_it->open_group_token == summary.group_token) {
      break;
    }
  }
  if (transaction_it == entries_.end()) {
    return false;
  }

  TmHaReadTxn& transaction = *transaction_it;
  transaction.open_group_token = 0;
  transaction.waiter_admission_open = false;

  if (transaction.next_response_waiter == transaction.waiters.size()) {
    erase_transaction(transaction_it);
  }
  return true;
}

uint64_t TmRingHomeAgent::line_base(uint64_t addr) const {
  return cfg_.line_size == 0 ? addr : addr - (addr % cfg_.line_size);
}

bool TmRingHomeAgent::in_one_line(p_tm_pld_t pld) const {
  if (pld == nullptr || cfg_.line_size == 0 || pld->size == 0 ||
      pld->size > cfg_.line_size ||
      static_cast<uint64_t>(pld->size - 1) >
          std::numeric_limits<uint64_t>::max() - pld->addr) {
    return false;
  }
  return line_base(pld->addr) ==
         line_base(pld->addr + static_cast<uint64_t>(pld->size) - 1);
}

bool TmRingHomeAgent::fanout_waiter_set_supported(
    const TmHaReadTxn& transaction) const {
  if (transaction.waiters.empty()) {
    return false;
  }
  for (size_t first = 0; first < transaction.waiters.size(); ++first) {
    if (transaction.waiters[first].pld == nullptr) {
      return false;
    }
    for (size_t second = first + 1; second < transaction.waiters.size();
         ++second) {
      if (transaction.waiters[second].pld == nullptr ||
          transaction.waiters[first].pld->mst_id ==
              transaction.waiters[second].pld->mst_id) {
        return false;
      }
    }
  }
  return true;
}

TmRingHomeAgent::TmHaWaiter TmRingHomeAgent::make_waiter(
    p_tm_pld_t pld) const {
  TmHaWaiter waiter;
  waiter.pld = pld;
  waiter.line_offset =
      static_cast<uint32_t>(pld->addr - line_base(pld->addr));
  return waiter;
}

void TmRingHomeAgent::record_read(p_tm_pld_t pld) {
  stats_.rd_requests++;
  stats_.useful_bytes += pld->size;
}

bool TmRingHomeAgent::classify_l2_hit(uint64_t req_line_base,
                                      uint64_t first_gid) const {
  if (cfg_.hit_rate_pct == 0) {
    return false;
  }
  if (cfg_.hit_rate_pct >= 100) {
    return true;
  }

  uint64_t value = req_line_base;
  value ^= first_gid + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
  value ^= static_cast<uint64_t>(cfg_.hit_seed) * 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  return value % 100 < cfg_.hit_rate_pct;
}

bool TmRingHomeAgent::find_transaction(uint64_t req_line_base,
                                       TmHaTxnIter* transaction) {
  for (TmHaTxnIter current = entries_.begin(); current != entries_.end();
       ++current) {
    if (current->line_base == req_line_base) {
      *transaction = current;
      return true;
    }
  }
  return false;
}

bool TmRingHomeAgent::find_pending_transaction(TmHaTxnIter* transaction) {
  if (entries_.empty()) {
    return false;
  }

  TmHaTxnIter current = backend_rr_cursor_ == entries_.end()
                            ? entries_.begin()
                            : backend_rr_cursor_;
  const TmHaTxnIter start = current;
  do {
    if (current->state == TmHaTxnState::PENDING_MISS) {
      *transaction = current;
      return true;
    }
    ++current;
    if (current == entries_.end()) {
      current = entries_.begin();
    }
  } while (current != start);
  return false;
}

bool TmRingHomeAgent::find_pending_functional_transaction(
    TmHaTxnIter* transaction) {
  if (entries_.empty()) {
    return false;
  }

  TmHaTxnIter current = backend_rr_cursor_ == entries_.end()
                            ? entries_.begin()
                            : backend_rr_cursor_;
  const TmHaTxnIter start = current;
  do {
    if (current->state == TmHaTxnState::PENDING_HIT) {
      *transaction = current;
      return true;
    }
    ++current;
    if (current == entries_.end()) {
      current = entries_.begin();
    }
  } while (current != start);
  return false;
}

bool TmRingHomeAgent::find_responding_transaction(TmHaTxnIter* transaction) {
  if (entries_.empty()) {
    return false;
  }

  TmHaTxnIter current = response_rr_cursor_ == entries_.end()
                            ? entries_.begin()
                            : response_rr_cursor_;
  const TmHaTxnIter start = current;
  do {
    if (current->state == TmHaTxnState::RESPONDING &&
        current->next_response_waiter < current->waiters.size()) {
      *transaction = current;
      return true;
    }
    ++current;
    if (current == entries_.end()) {
      current = entries_.begin();
    }
  } while (current != start);
  return false;
}

bool TmRingHomeAgent::has_blocking_read_transaction(
    uint64_t req_line_base) const {
  for (const TmHaReadTxn& transaction : entries_) {
    if (transaction.line_base != req_line_base) {
      continue;
    }
    if (transaction.state == TmHaTxnState::PENDING_HIT ||
        transaction.state == TmHaTxnState::PENDING_MISS ||
        transaction.state == TmHaTxnState::INFLIGHT ||
        transaction.state == TmHaTxnState::RESPONDING) {
      return true;
    }
  }
  return false;
}

p_tm_pld_t TmRingHomeAgent::make_backend_request(
    const TmHaReadTxn& transaction) const {
  const p_tm_pld_t waiter_pld =
      transaction.waiters.empty() ? nullptr : transaction.waiters.front().pld;
  p_tm_pld_t backend_request =
      tm_make_pld(PldCmd::RD, transaction.line_base, cfg_.line_size);
  backend_request->chan = 0;
  backend_request->rsp_count = 1;
  backend_request->buf_u8 =
      std::make_shared<std::vector<uint8_t>>(cfg_.line_size, 0);
  backend_request->data = backend_request->buf_u8->data();
  if (waiter_pld == nullptr) {
    return backend_request;
  }

  backend_request->type_id = waiter_pld->type_id;
  backend_request->slv_id = waiter_pld->slv_id;
  backend_request->mst_addr = waiter_pld->mst_addr;
  backend_request->slv_addr = waiter_pld->slv_addr;
  backend_request->chan = waiter_pld->chan;
  backend_request->ring_subnet = waiter_pld->ring_subnet;
  backend_request->ring_traffic_class = waiter_pld->ring_traffic_class;
  backend_request->ring_direction = waiter_pld->ring_direction;
  return backend_request;
}

std::shared_ptr<const std::vector<uint8_t>>
TmRingHomeAgent::make_completion_data(p_tm_pld_t response,
                                      pld_rsp_t status) const {
  std::shared_ptr<std::vector<uint8_t>> data =
      std::make_shared<std::vector<uint8_t>>(cfg_.line_size, 0);
  if (status == PldRsp::OK && response != nullptr && response->data != nullptr) {
    std::memcpy(data->data(), response->data, cfg_.line_size);
  }
  return data;
}

TmRingL2ResponseCandidate TmRingHomeAgent::make_l2_candidate(
    const TmHaReadTxn& transaction, const TmHaWaiter& waiter) const {
  TmRingL2ResponseCandidate candidate;
  candidate.response = tm_make_pld(waiter.pld);
  if (candidate.response == nullptr) {
    return candidate;
  }

  candidate.response->cmd = PldCmd::RD_RSP;
  candidate.response->rsp = transaction.backend_response_status;
  candidate.response->rsp_count = 1;
  candidate.response->mst_id = waiter.pld->mst_id;
  candidate.response->gid = waiter.pld->gid;
  candidate.response->addr = waiter.pld->addr;
  candidate.response->size = waiter.pld->size;
  candidate.response->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
  candidate.response->ring_traffic_class =
      static_cast<uint32_t>(PldCmd::RD_RSP);
  candidate.response->ring_fanout.reset();
  candidate.response->buf_u8.reset();
  candidate.response->data = nullptr;
  candidate.response->ring_slot_empty = false;
  candidate.response->ring_i_tag_owner = tm_ring_invalid_tag_owner();
  candidate.response->ring_e_tag_owner = tm_ring_invalid_tag_owner();
  candidate.response->reset_ring_deflection_state();

  candidate.line_base = transaction.line_base;
  candidate.completion_data = transaction.completion_data;
  candidate.payload_offset = waiter.line_offset;
  candidate.fanout_eligible =
      transaction.waiter_admission_open &&
      candidate.completion_data != nullptr &&
      fanout_waiter_set_supported(transaction);
  if (candidate.fanout_eligible) {
    candidate.open_group_token = transaction.open_group_token;
  }
  return candidate;
}

uint64_t TmRingHomeAgent::current_completion_buffer_bytes() const {
  uint64_t bytes = 0;
  for (const TmHaReadTxn& transaction : entries_) {
    if (transaction.completion_data != nullptr) {
      bytes += transaction.completion_data->size();
    }
  }
  return bytes;
}

void TmRingHomeAgent::erase_transaction(TmHaTxnIter transaction) {
  if (transaction == entries_.end()) {
    return;
  }

  const size_t waiter_bucket =
      transaction->waiters.size() >= 64 ? 64 : transaction->waiters.size();
  stats_.completed_transaction_waiters[waiter_bucket]++;
  TmHaTxnIter successor = std::next(transaction);
  if (successor == entries_.end() && entries_.size() > 1) {
    successor = entries_.begin();
  }
  if (backend_rr_cursor_ == transaction) {
    backend_rr_cursor_ = successor;
  }
  if (backend_candidate_ == transaction) {
    backend_candidate_ = entries_.end();
  }
  if (functional_candidate_ == transaction) {
    functional_candidate_ = entries_.end();
  }
  if (response_rr_cursor_ == transaction) {
    response_rr_cursor_ = successor;
  }
  if (response_candidate_ == transaction) {
    response_candidate_ = entries_.end();
  }
  entries_.erase(transaction);
}
