#ifndef _TM_RING_SLOT_POOL_H_
#define _TM_RING_SLOT_POOL_H_

#include <assert.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "tm_ring_types.h"

/*
 * Tracks occupied storage slots in each closed directed lane.
 * One slot stays empty so ready/valid backpressure can always propagate.
 */
class TmRingSlotPool {
 public:
  TmRingSlotPool(uint32_t station_count, uint32_t link_latency) {
    const uint32_t pipeline_depth =
        std::max<uint32_t>(1, link_latency + 1);
    capacity_per_lane_ = station_count * (pipeline_depth + 1);
    occupancy_.assign(tm_ring_subnet_count() * 2, 0);
  }

  void reset() {
    std::fill(occupancy_.begin(), occupancy_.end(), 0);
  }

  bool can_acquire(TmRingSubnet subnet, TmRingPortDir direction) const {
    const uint32_t idx = slot_index(subnet, direction);
    return occupancy_[idx] + reserved_empty_slots_ < capacity_per_lane_;
  }

  bool try_acquire(TmRingSubnet subnet, TmRingPortDir direction) {
    const uint32_t idx = slot_index(subnet, direction);
    if (!can_acquire(subnet, direction)) {
      return false;
    }
    occupancy_[idx]++;
    return true;
  }

  void release(TmRingSubnet subnet, TmRingPortDir direction) {
    const uint32_t idx = slot_index(subnet, direction);
    assert(occupancy_[idx] > 0);
    occupancy_[idx]--;
  }

  bool empty() const {
    for (uint32_t used : occupancy_) {
      if (used != 0) {
        return false;
      }
    }
    return true;
  }

 private:
  uint32_t slot_index(TmRingSubnet subnet, TmRingPortDir direction) const {
    const uint32_t dir_idx =
        direction == TmRingPortDir::CW ? 0 : 1;
    return static_cast<uint32_t>(subnet) * 2 + dir_idx;
  }

  uint32_t capacity_per_lane_ = 1;
  const uint32_t reserved_empty_slots_ = 1;
  std::vector<uint32_t> occupancy_;
};

using tm_ring_slot_pool_t = TmRingSlotPool;
using p_tm_ring_slot_pool_t = std::shared_ptr<tm_ring_slot_pool_t>;

inline p_tm_ring_slot_pool_t tm_make_ring_slot_pool(
    uint32_t station_count, uint32_t link_latency) {
  return std::make_shared<TmRingSlotPool>(station_count, link_latency);
}

#endif  // _TM_RING_SLOT_POOL_H_
