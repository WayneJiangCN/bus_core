#ifndef _TM_RING_WRITE_TRACKER_H_
#define _TM_RING_WRITE_TRACKER_H_

#include <stdexcept>
#include <unordered_map>

#include "tm_pld.h"

// Tracks Ring write addresses until the matching data packet arrives. The
// tracker is keyed by protocol transaction identity rather than FIFO position.
// Its live entries are bounded by requester outstanding writes; mirroring a
// target FIFO depth here would couple the independent REQ and DAT subnets and
// can deadlock an unmatched DAT FIFO head behind a full address tracker.
class TmRingWriteTracker {
 public:
  void reset() { addresses_.clear(); }

  bool empty() const { return addresses_.empty(); }

  bool can_accept_address(p_tm_pld_t address) const {
    validate_address(address);
    return addresses_.find(tm_pld_txn_key(address)) == addresses_.end();
  }

  bool accept_address(p_tm_pld_t address) {
    if (!can_accept_address(address)) {
      return false;
    }
    addresses_.emplace(tm_pld_txn_key(address), tm_make_pld(address));
    return true;
  }

  bool has_matching_address(p_tm_pld_t data) const {
    validate_data(data);
    const auto it = addresses_.find(tm_pld_txn_key(data));
    if (it == addresses_.end()) {
      return false;
    }
    validate_range(it->second, data);
    return true;
  }

  void commit_data(p_tm_pld_t data) {
    validate_data(data);
    const auto it = addresses_.find(tm_pld_txn_key(data));
    if (it == addresses_.end()) {
      throw std::logic_error("write data has no matching address");
    }
    validate_range(it->second, data);
    addresses_.erase(it);
  }

 private:
  static void validate_address(p_tm_pld_t address) {
    if (address == nullptr || address->cmd != PldCmd::WR) {
      throw std::invalid_argument("write tracker expects a WR address");
    }
  }

  static void validate_data(p_tm_pld_t data) {
    if (data == nullptr || data->cmd != PldCmd::WR_DAT) {
      throw std::invalid_argument("write tracker expects WR_DAT data");
    }
  }

  static void validate_range(p_tm_pld_t address, p_tm_pld_t data) {
    if (address->addr != data->addr || address->size != data->size) {
      throw std::logic_error("write address/data range mismatch");
    }
  }

  std::unordered_map<TmPldTxnKey, p_tm_pld_t, TmPldTxnKeyHash> addresses_;
};

#endif  // _TM_RING_WRITE_TRACKER_H_
