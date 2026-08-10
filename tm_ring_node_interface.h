#ifndef _TM_RING_NODE_INTERFACE_H_
#define _TM_RING_NODE_INTERFACE_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <string>

#include "tm_clock.h"
#include "tm_pld.h"
#include "tm_que.h"
#include "tm_ring_types.h"

// Ring-side queue resources of one endpoint. It has no event or arbitration.
class TmRingNodeInterface {
 public:
  TmRingNodeInterface(tm_engine::p_tm_clk_t clk, const std::string& name,
                      uint32_t inject_depth, uint32_t eject_depth)
      : clk_(clk), name_(name), eject_depth_(eject_depth) {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        inject_banks_[subnet][direction] = tm_make_com_que(
            clk_, name_ + "_inject_bank_" + std::to_string(subnet) + "_" +
                      std::to_string(direction),
            inject_depth);
      }
      eject_qs_[subnet] = tm_make_com_que(
          clk_, name_ + "_eject_q_" + std::to_string(subnet), eject_depth);
    }
    reset();
  }

  void reset() {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        inject_banks_[subnet][direction]->clear();
      }
      eject_qs_[subnet]->clear();
      eject_counts_[subnet] = 0;
    }
  }

  bool idle() const {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        if (!inject_banks_[subnet][direction]->empty()) {
          return false;
        }
      }
      if (!eject_qs_[subnet]->empty()) {
        return false;
      }
    }
    return true;
  }

  p_tm_com_que_t inject_bank(TmRingSubnet subnet,
                             TmRingPortDir direction) const {
    return inject_banks_[static_cast<uint32_t>(subnet)]
                        [direction_index(direction)];
  }

  p_tm_com_que_t eject_q(TmRingSubnet subnet) const {
    return eject_qs_[static_cast<uint32_t>(subnet)];
  }

  bool has_eject_capacity(TmRingSubnet subnet,
                          uint32_t reserved_entries = 0) const {
    const uint32_t idx = tm_ring_subnet_index(subnet);
    return eject_counts_[idx] + reserved_entries < eject_depth_;
  }

  bool push_inject(TmRingSubnet subnet, p_tm_pld_t pld) {
    return push_inject(subnet,
                       static_cast<TmRingPortDir>(pld->ring_direction), pld);
  }

  bool push_inject(TmRingSubnet subnet, TmRingPortDir direction,
                   p_tm_pld_t pld) {
    auto q = inject_bank(subnet, direction);
    if (q->full()) {
      return false;
    }
    q->push_back(pld);
    return true;
  }

  p_tm_pld_t front_inject(TmRingSubnet subnet,
                          TmRingPortDir direction) const {
    auto q = inject_bank(subnet, direction);
    return q->empty() ? nullptr : q->front();
  }

  void pop_inject(TmRingSubnet subnet, TmRingPortDir direction) {
    auto q = inject_bank(subnet, direction);
    if (q->empty()) {
      return;
    }
    q->pop_front();
  }

  bool push_eject(TmRingSubnet subnet, p_tm_pld_t pld) {
    auto q = eject_q(subnet);
    if (q->full()) {
      return false;
    }
    q->push_back(pld);
    eject_counts_[static_cast<uint32_t>(subnet)]++;
    return true;
  }

  p_tm_pld_t front_eject(TmRingSubnet subnet) const {
    auto q = eject_q(subnet);
    return q->empty() ? nullptr : q->front();
  }

  void pop_eject(TmRingSubnet subnet) {
    auto q = eject_q(subnet);
    if (q->empty()) {
      return;
    }
    q->pop_front();
    eject_counts_[static_cast<uint32_t>(subnet)]--;
  }

 private:
  uint32_t direction_index(TmRingPortDir direction) const {
    return direction == TmRingPortDir::CW ? 0 : 1;
  }

  tm_engine::p_tm_clk_t clk_ = nullptr;
  std::string name_;
  std::array<std::array<p_tm_com_que_t, 2>, 3> inject_banks_;
  std::array<p_tm_com_que_t, 3> eject_qs_;
  uint32_t eject_depth_ = 0;
  std::array<uint32_t, 3> eject_counts_ = {{0, 0, 0}};
};

using p_tm_ring_node_interface_t = std::shared_ptr<TmRingNodeInterface>;

inline p_tm_ring_node_interface_t tm_make_ring_node_interface(
    tm_engine::p_tm_clk_t clk, const std::string& name,
    uint32_t inject_depth, uint32_t eject_depth) {
  return std::make_shared<TmRingNodeInterface>(clk, name, inject_depth,
                                                eject_depth);
}

#endif  // _TM_RING_NODE_INTERFACE_H_
