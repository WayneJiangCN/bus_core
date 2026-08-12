#ifndef _TM_RING_NODE_INTERFACE_H_
#define _TM_RING_NODE_INTERFACE_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_pld.h"
#include "tm_que.h"
#include "tm_ring_pmu.h"
#include "tm_ring_types.h"

// Ring-side queue resources of one endpoint. It has no event or arbitration.
class TmRingNodeInterface {
 public:
  TmRingNodeInterface(tm_engine::p_tm_clk_t clk, const std::string& name,
                      const TmRingEndpointQueueDepths& queue_depths,
                      const std::vector<TmRingQueuePmuPort>& queue_pmu_ports)
      : clk_(clk), name_(name) {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        inject_states_.push_back(QueueState{
            tm_make_com_que(clk_, name_ + "_inject_bank_" +
                                      std::to_string(subnet) + "_" +
                                      std::to_string(direction),
                             queue_depths.inject[subnet]),
            queue_depths.inject[subnet], 0,
            queue_pmu_ports[queue_index(subnet, direction)]});
      }
      eject_states_.push_back(QueueState{
          tm_make_com_que(clk_, name_ + "_eject_q_" + std::to_string(subnet),
                           queue_depths.eject[subnet]),
          queue_depths.eject[subnet], 0,
          queue_pmu_ports[6 + subnet]});
    }
    reset();
  }

  void reset() {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        reset_queue_state(inject_states_[queue_index(subnet, direction)]);
      }
      reset_queue_state(eject_states_[subnet]);
    }
  }

  bool idle() const {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        if (!inject_states_[subnet][direction].queue->empty()) {
          return false;
        }
      }
      if (!eject_states_[subnet].queue->empty()) {
        return false;
      }
    }
    return true;
  }

  p_tm_com_que_t inject_bank(TmRingSubnet subnet,
                             TmRingPortDir direction) const {
    return inject_state(subnet, direction).queue;
  }

  p_tm_com_que_t eject_q(TmRingSubnet subnet) const {
    return eject_state(subnet).queue;
  }

  bool has_eject_capacity(TmRingSubnet subnet,
                          uint32_t reserved_entries = 0) const {
    const QueueState& state = eject_state(subnet);
    return state.occupancy + reserved_entries < state.depth;
  }

  bool push_inject(TmRingSubnet subnet, p_tm_pld_t pld) {
    return push_inject(subnet,
                       static_cast<TmRingPortDir>(pld->ring_direction), pld);
  }

  bool push_inject(TmRingSubnet subnet, TmRingPortDir direction,
                   p_tm_pld_t pld) {
    QueueState& state = inject_state(subnet, direction);
    if (state.queue->full()) {
      state.pmu.push_rejected(clk_->time(), state.occupancy);
      return false;
    }
    record_push(state, pld);
    return true;
  }

  p_tm_pld_t front_inject(TmRingSubnet subnet,
                          TmRingPortDir direction) const {
    auto q = inject_bank(subnet, direction);
    return q->empty() ? nullptr : q->front();
  }

  void pop_inject(TmRingSubnet subnet, TmRingPortDir direction) {
    QueueState& state = inject_state(subnet, direction);
    if (state.queue->empty()) {
      return;
    }
    record_pop(state);
  }

  bool push_eject(TmRingSubnet subnet, p_tm_pld_t pld) {
    QueueState& state = eject_state(subnet);
    if (state.queue->full()) {
      state.pmu.push_rejected(clk_->time(), state.occupancy);
      return false;
    }
    record_push(state, pld);
    return true;
  }

  p_tm_pld_t front_eject(TmRingSubnet subnet) const {
    auto q = eject_q(subnet);
    return q->empty() ? nullptr : q->front();
  }

  void pop_eject(TmRingSubnet subnet) {
    QueueState& state = eject_state(subnet);
    if (state.queue->empty()) {
      return;
    }
    record_pop(state);
  }

 private:
  struct QueueState {
    p_tm_com_que_t queue;
    uint32_t depth;
    uint32_t occupancy;
    TmRingQueuePmuPort pmu;
  };

  static uint32_t queue_index(uint32_t subnet, uint32_t direction) {
    return subnet * 2 + direction;
  }

  uint32_t direction_index(TmRingPortDir direction) const {
    return direction == TmRingPortDir::CW ? 0 : 1;
  }

  QueueState& inject_state(TmRingSubnet subnet, TmRingPortDir direction) {
    return inject_states_[queue_index(tm_ring_subnet_index(subnet),
                                      direction_index(direction))];
  }

  const QueueState& inject_state(TmRingSubnet subnet,
                                 TmRingPortDir direction) const {
    return inject_states_[queue_index(tm_ring_subnet_index(subnet),
                                      direction_index(direction))];
  }

  QueueState& eject_state(TmRingSubnet subnet) {
    return eject_states_[tm_ring_subnet_index(subnet)];
  }

  const QueueState& eject_state(TmRingSubnet subnet) const {
    return eject_states_[tm_ring_subnet_index(subnet)];
  }

  void record_push(QueueState& state, p_tm_pld_t pld) {
    state.queue->push_back(pld);
    ++state.occupancy;
    state.pmu.push_accepted(clk_->time(), state.occupancy);
  }

  void record_pop(QueueState& state) {
    state.queue->pop_front();
    --state.occupancy;
    state.pmu.popped(clk_->time(), state.occupancy);
  }

  void reset_queue_state(QueueState& state) {
    state.queue->clear();
    state.occupancy = 0;
  }

  tm_engine::p_tm_clk_t clk_ = nullptr;
  std::string name_;
  std::vector<QueueState> inject_states_;
  std::vector<QueueState> eject_states_;
};

using p_tm_ring_node_interface_t = std::shared_ptr<TmRingNodeInterface>;

inline p_tm_ring_node_interface_t tm_make_ring_node_interface(
    tm_engine::p_tm_clk_t clk, const std::string& name,
    const TmRingEndpointQueueDepths& queue_depths,
    const std::vector<TmRingQueuePmuPort>& queue_pmu_ports) {
  return std::make_shared<TmRingNodeInterface>(clk, name, queue_depths,
                                                queue_pmu_ports);
}

#endif  // _TM_RING_NODE_INTERFACE_H_
