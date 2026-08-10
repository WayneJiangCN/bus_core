#ifndef _TM_RING_FANOUT_H_
#define _TM_RING_FANOUT_H_

#include <stdint.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "tm_pld.h"

enum class TmRingFanoutMode { MULTICAST, SCATTER };

struct TmRingFanoutRecipient {
  uint32_t dst_ring_id = 0;
  uint32_t dst_node = 0;
  uint32_t payload_offset = 0;
  p_tm_pld_t response_template = nullptr;
};

struct TmRingFanoutState {
  bool active_on_ring = false;
  TmRingFanoutMode mode = TmRingFanoutMode::MULTICAST;
  TmPldTxnKey txn_key;
  uint64_t line_base = 0;
  uint64_t group_token = 0;
  uint64_t pending_stations = 0;
  // L2 retains this shared source while the group is open or frozen. Each
  // destination V-Ring carrier copies its physical range before injection.
  std::shared_ptr<const std::vector<uint8_t>> source_data = nullptr;
  std::vector<TmRingFanoutRecipient> recipients;
};

struct TmRingL2ResponseCandidate {
  p_tm_pld_t response = nullptr;
  uint64_t line_base = 0;
  std::shared_ptr<const std::vector<uint8_t>> completion_data = nullptr;
  uint32_t payload_offset = 0;
  bool fanout_eligible = false;
  uint64_t open_group_token = 0;
};

enum class TmRingL2AcceptStatus {
  REJECTED_BUFFER_FULL,
  ACCEPTED_NEW_GROUP,
  MERGED_GROUP,
  ACCEPTED_UNICAST,
};

struct TmRingL2AcceptResult {
  TmRingL2AcceptStatus status = TmRingL2AcceptStatus::REJECTED_BUFFER_FULL;
  uint64_t group_token = 0;

  bool accepted() const {
    return status == TmRingL2AcceptStatus::ACCEPTED_NEW_GROUP ||
           status == TmRingL2AcceptStatus::MERGED_GROUP ||
           status == TmRingL2AcceptStatus::ACCEPTED_UNICAST;
  }

  bool is_group() const {
    return status == TmRingL2AcceptStatus::ACCEPTED_NEW_GROUP ||
           status == TmRingL2AcceptStatus::MERGED_GROUP;
  }
};

struct TmRingL2GroupSummary {
  uint64_t group_token = 0;
  TmRingFanoutMode mode = TmRingFanoutMode::MULTICAST;
  uint32_t recipient_count = 0;
};

inline uint64_t tm_ring_station_bit(uint32_t station) {
  if (station >= 64) {
    throw std::out_of_range("Ring station bitmap supports at most 64 stations");
  }
  return uint64_t{1} << station;
}

inline bool tm_ring_fanout_has_station(const TmRingFanoutState& state,
                                       uint32_t station) {
  return (state.pending_stations & tm_ring_station_bit(station)) != 0;
}

inline void tm_ring_fanout_clear_station(TmRingFanoutState* state,
                                         uint32_t station) {
  if (state != nullptr) {
    state->pending_stations &= ~tm_ring_station_bit(station);
  }
}

inline uint32_t tm_ring_fanout_remaining_stations(
    const TmRingFanoutState& state) {
  uint64_t value = state.pending_stations;
  uint32_t count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
}

inline std::shared_ptr<TmRingFanoutState> tm_ring_clone_fanout_state(
    const std::shared_ptr<TmRingFanoutState>& state) {
  return state == nullptr ? nullptr
                          : std::make_shared<TmRingFanoutState>(*state);
}

inline bool tm_ring_has_fanout(p_tm_pld_t pld) {
  return pld != nullptr && pld->ring_fanout != nullptr;
}

inline bool tm_ring_fanout_active(p_tm_pld_t pld) {
  return tm_ring_has_fanout(pld) && pld->ring_fanout->active_on_ring;
}

inline TmPldTxnKey tm_ring_packet_txn_key(p_tm_pld_t pld) {
  return tm_ring_has_fanout(pld) ? pld->ring_fanout->txn_key
                                : tm_pld_txn_key(pld);
}

#endif  // _TM_RING_FANOUT_H_
