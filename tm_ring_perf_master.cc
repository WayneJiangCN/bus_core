#include "tm_ring_perf_master.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

using namespace tm_engine;

TmRingPerfWaveCoordinator::TmRingPerfWaveCoordinator(uint32_t masters)
    : completed_waves_(masters, std::numeric_limits<uint64_t>::max()) {
  if (masters == 0) {
    throw std::invalid_argument("wave coordinator requires masters");
  }
}

bool TmRingPerfWaveCoordinator::can_issue(uint32_t master,
                                          uint64_t wave) const {
  if (master >= completed_waves_.size()) {
    throw std::out_of_range("wave coordinator master is out of range");
  }
  if (wave < current_wave_) {
    throw std::logic_error("wave issue is behind current wave");
  }
  return wave == current_wave_;
}

void TmRingPerfWaveCoordinator::record_completion(uint32_t master,
                                                   uint64_t wave) {
  if (master >= completed_waves_.size()) {
    throw std::out_of_range("wave coordinator master is out of range");
  }
  if (wave != current_wave_) {
    throw std::logic_error("wave completion does not match current wave");
  }
  if (completed_waves_[master] == wave) {
    throw std::logic_error("duplicate wave completion");
  }
  completed_waves_[master] = wave;
  for (const uint64_t completed : completed_waves_) {
    if (completed != current_wave_) {
      return;
    }
  }
  ++current_wave_;
}

TmRingPerfMaster::TmRingPerfMaster() {}

TmRingPerfMaster::~TmRingPerfMaster() {}

void TmRingPerfMaster::config(
    const std::string& name, p_tm_clk_t clk, uint32_t master_port,
    const std::vector<TmRingPerfTxn>& transactions,
    const std::shared_ptr<TmRingPerfWaveCoordinator>& wave_coordinator,
    uint32_t max_outstanding) {
  this->name(name);
  clk_ = clk;
  master_port_ = master_port;
  wave_coordinator_ = wave_coordinator;
  max_outstanding_ = max_outstanding;
  transactions_ = transactions;
  read_transactions_.clear();
  write_transactions_.clear();
  for (const auto& txn : transactions_) {
    if (txn.cmd != PldCmd::RD && txn.cmd != PldCmd::WR) {
      throw std::invalid_argument("PerfMaster accepts RD and WR only");
    }
    if (txn.cmd == PldCmd::RD) {
      read_transactions_.push_back(txn);
    } else {
      write_transactions_.push_back(txn);
    }
  }
  if (wave_coordinator_ != nullptr) {
    wave_coordinator_->can_issue(master_port_, 0);
    if (!write_transactions_.empty()) {
      throw std::invalid_argument(
          "wave coordinator supports read-only traces");
    }
    for (std::size_t index = 0; index < read_transactions_.size(); ++index) {
      if (read_transactions_[index].ordinal != index) {
        throw std::invalid_argument(
            "wave trace ordinals must start at zero and be contiguous");
      }
    }
  }

  read_port_ = tm_make_inf<p_tm_pld_t>(clk_, name + "_read_port");
  write_port_ = tm_make_inf<p_tm_pld_t>(clk_, name + "_write_port");
  tm_sensitive(TM_MAKE_CPROC(&TmRingPerfMaster::issue), clk_->pos_edge);
  tm_sensitive(TM_MAKE_CPROC(&TmRingPerfMaster::receive_read_response),
               read_port_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingPerfMaster::receive_write_response),
               write_port_->vld);
  reset();
}

void TmRingPerfMaster::attach(p_pem_biu_t biu) { biu_ = biu; }

void TmRingPerfMaster::build() {
  read_port_->connect(biu_->v_dcache_rd_inf_[0]);
  write_port_->connect(biu_->v_dcache_wr_inf_[0]);
}

void TmRingPerfMaster::reset() {
  next_read_transaction_ = 0;
  next_write_transaction_ = 0;
  pending_read_candidate_.reset();
  pending_write_candidate_.reset();
  issue_cycles_.clear();
  outstanding_sizes_.clear();
  outstanding_waves_.clear();
  completed_gids_.clear();
  stats_ = TmRingPerfMasterStats();
}

bool TmRingPerfMaster::idle() const {
  return next_read_transaction_ == read_transactions_.size() &&
         next_write_transaction_ == write_transactions_.size() &&
         pending_read_candidate_ == nullptr &&
         pending_write_candidate_ == nullptr && issue_cycles_.empty() &&
         outstanding_sizes_.empty() && outstanding_waves_.empty() &&
         read_port_->idle() &&
         write_port_->idle();
}

void TmRingPerfMaster::issue() {
  issue_read();
  issue_write();
}

void TmRingPerfMaster::issue_read() {
  if (next_read_transaction_ == read_transactions_.size()) {
    return;
  }
  if (max_outstanding_ != 0 &&
      issue_cycles_.size() >= max_outstanding_) {
    return;
  }
  const TmRingPerfTxn& txn = read_transactions_[next_read_transaction_];
  if (wave_coordinator_ != nullptr &&
      !wave_coordinator_->can_issue(master_port_, txn.ordinal)) {
    return;
  }
  if (pending_read_candidate_ == nullptr) {
    pending_read_candidate_ = make_candidate(txn);
  }

  ++stats_.attempted_packets;
  if (!send_read_candidate(pending_read_candidate_)) {
    ++stats_.send_stall_cycles;
    return;
  }

  const uint64_t now = static_cast<uint64_t>(time());
  const uint64_t gid = pending_read_candidate_->gid;
  issue_cycles_[gid] = now;
  outstanding_sizes_[gid] = pending_read_candidate_->size;
  if (wave_coordinator_ != nullptr) {
    outstanding_waves_[gid] = txn.ordinal;
  }
  ++stats_.accepted_packets;
  if (!stats_.has_first_request) {
    stats_.first_request_cycle = now;
    stats_.has_first_request = true;
  }
  stats_.outstanding_peak = std::max<uint64_t>(
      stats_.outstanding_peak, static_cast<uint64_t>(issue_cycles_.size()));
  ++next_read_transaction_;
  pending_read_candidate_.reset();
}

void TmRingPerfMaster::issue_write() {
  if (next_write_transaction_ == write_transactions_.size()) {
    return;
  }
  if (max_outstanding_ != 0 &&
      issue_cycles_.size() >= max_outstanding_) {
    return;
  }
  const TmRingPerfTxn& txn = write_transactions_[next_write_transaction_];
  if (pending_write_candidate_ == nullptr) {
    pending_write_candidate_ = make_candidate(txn);
  }

  ++stats_.attempted_packets;
  if (!send_write_candidate(pending_write_candidate_)) {
    ++stats_.send_stall_cycles;
    return;
  }

  const uint64_t now = static_cast<uint64_t>(time());
  const uint64_t gid = pending_write_candidate_->gid;
  issue_cycles_[gid] = now;
  outstanding_sizes_[gid] = pending_write_candidate_->size;
  ++stats_.accepted_packets;
  if (!stats_.has_first_request) {
    stats_.first_request_cycle = now;
    stats_.has_first_request = true;
  }
  stats_.outstanding_peak = std::max<uint64_t>(
      stats_.outstanding_peak, static_cast<uint64_t>(issue_cycles_.size()));
  ++next_write_transaction_;
  pending_write_candidate_.reset();
}

bool TmRingPerfMaster::send_read_candidate(const p_tm_pld_t& pld) {
  return read_port_->send(pld);
}

bool TmRingPerfMaster::send_write_candidate(const p_tm_pld_t& pld) {
  return write_port_->send(pld);
}

p_tm_pld_t TmRingPerfMaster::make_candidate(const TmRingPerfTxn& txn) {
  auto pld = tm_make_pld(txn.cmd, txn.addr, txn.size);
  pld->gid = (static_cast<uint64_t>(master_port_) << 32) |
             (txn.ordinal + 1);
  pld->buf_u8 = std::make_shared<std::vector<uint8_t>>(txn.size, 0);
  pld->data = pld->buf_u8->data();
  return pld;
}

void TmRingPerfMaster::receive_read_response() {
  receive_response(read_port_);
}

void TmRingPerfMaster::receive_write_response() {
  receive_response(write_port_);
}

void TmRingPerfMaster::receive_response(p_tm_com_inf_t port) {
  if (!port->valid()) {
    return;
  }
  p_tm_pld_t response = port->get_pld();
  if (response == nullptr) {
    return;
  }

  const uint64_t gid = response->gid;
  auto issue = issue_cycles_.find(gid);
  if (issue == issue_cycles_.end()) {
    if (completed_gids_.find(gid) != completed_gids_.end()) {
      ++stats_.duplicate_responses;
    } else {
      ++stats_.unknown_responses;
    }
    port->pop_pld();
    return;
  }

  const uint64_t now = static_cast<uint64_t>(time());
  if (!stats_.has_first_response) {
    stats_.first_response_cycle = now;
    stats_.has_first_response = true;
  }
  stats_.last_response_cycle = now;
  ++stats_.completed_packets;
  stats_.latency_cycles.push_back(now - issue->second);
  stats_.completed_bytes += outstanding_sizes_[gid];
  const auto wave = outstanding_waves_.find(gid);
  if (wave != outstanding_waves_.end()) {
    wave_coordinator_->record_completion(master_port_, wave->second);
    outstanding_waves_.erase(wave);
  }
  completed_gids_.insert(gid);
  issue_cycles_.erase(issue);
  outstanding_sizes_.erase(gid);
  port->pop_pld();
}
