#ifndef _TM_RING_NODE_INTERFACE_H_
#define _TM_RING_NODE_INTERFACE_H_

#include <stdint.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_pld.h"
#include "tm_que.h"
#include "tm_ring_pmu.h"
#include "tm_ring_types.h"

enum class TmRingNodeInterfaceMode : uint32_t {
  SHARED_EJECT = 0,
  RBRG_DIRECTIONAL = 1,
};

// Ring-side queue resources of one endpoint. It has no event or arbitration.
class TmRingNodeInterface {
 public:
  TmRingNodeInterface(tm_engine::p_tm_clk_t clk, const std::string& name,
                      const TmRingEndpointQueueDepths& queue_depths,
                      const std::vector<TmRingQueuePmuPort>& queue_pmu_ports,
                      TmRingNodeInterfaceMode mode =
                          TmRingNodeInterfaceMode::SHARED_EJECT,
                      uint32_t inject_latency = 0)
      : clk_(clk), name_(name), mode_(mode) {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        QueueState& state = inject_states_[subnet][direction];
        const std::string queue_name =
            name_ + "_inject_bank_" + std::to_string(subnet) + "_" +
            std::to_string(direction);
        state.queue = inject_latency == 0
                          ? tm_make_com_que(clk_, queue_name,
                                             queue_depths.inject[subnet])
                          : tm_make_que<p_tm_pld_t>(
                                clk_, queue_name, queue_depths.inject[subnet],
                                inject_latency);
        state.depth = queue_depths.inject[subnet];
        state.space_event = tm_engine::tm_make_event(queue_name + "_space");
        state.pmu.reset(new TmRingQueuePmuPort(
            queue_pmu_ports[subnet * 2 + direction]));
      }

      const uint32_t eject_banks = directional_eject() ? 2 : 1;
      for (uint32_t direction = 0; direction < eject_banks; ++direction) {
        QueueState& state = eject_states_[subnet][direction];
        const std::string queue_name =
            name_ + "_eject_q_" + std::to_string(subnet) + "_" +
            std::to_string(direction);
        state.queue = tm_make_com_que(clk_, queue_name,
                                       queue_depths.eject[subnet]);
        state.depth = queue_depths.eject[subnet];
        if (!directional_eject()) {
          state.pmu.reset(
              new TmRingQueuePmuPort(queue_pmu_ports[6 + subnet]));
        }
      }
      QueueState& aggregate = aggregate_eject_states_[subnet];
      aggregate.depth = directional_eject() ? 2 * queue_depths.eject[subnet]
                                            : queue_depths.eject[subnet];
      if (directional_eject()) {
        aggregate.pmu.reset(
            new TmRingQueuePmuPort(queue_pmu_ports[6 + subnet]));
      }
    }
    reset();
  }

  void reset() {
    const uint64_t now = clk_->time();
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        reset_queue_state(inject_states_[subnet][direction], now);
      }
      for (uint32_t direction = 0; direction < 2; ++direction) {
        if (eject_states_[subnet][direction].queue != nullptr) {
          reset_queue_state(eject_states_[subnet][direction], now);
        }
      }
      reset_accounting_state(aggregate_eject_states_[subnet], now);
    }
  }

  bool idle() const {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      for (uint32_t direction = 0; direction < 2; ++direction) {
        if (!inject_states_[subnet][direction].queue->empty()) {
          return false;
        }
      }
      for (uint32_t direction = 0; direction < 2; ++direction) {
        const QueueState& state = eject_states_[subnet][direction];
        if (state.queue != nullptr && !state.queue->empty()) {
          return false;
        }
      }
    }
    return true;
  }

  p_tm_com_que_t inject_bank(TmRingSubnet subnet,
                             TmRingPortDir direction) const {
    return inject_state(subnet, direction).queue;
  }

  p_tm_com_que_t eject_q(TmRingSubnet subnet) const {
    return eject_q(subnet, TmRingPortDir::CW);
  }

  p_tm_com_que_t eject_q(TmRingSubnet subnet,
                          TmRingPortDir direction) const {
    return eject_state(subnet, direction).queue;
  }

  bool has_eject_capacity(TmRingSubnet subnet,
                          uint32_t reserved_entries = 0) const {
    return has_eject_capacity(subnet, TmRingPortDir::CW, reserved_entries);
  }

  bool has_eject_capacity(TmRingSubnet subnet, TmRingPortDir direction,
                          uint32_t reserved_entries = 0) const {
    const QueueState& state = eject_state(subnet, direction);
    return state.occupancy + reserved_entries < state.depth;
  }

  bool has_inject_capacity(TmRingSubnet subnet,
                           TmRingPortDir direction) const {
    const QueueState& state = inject_state(subnet, direction);
    return state.occupancy < state.depth;
  }

  tm_engine::p_tm_event_t inject_space_event(
      TmRingSubnet subnet, TmRingPortDir direction) const {
    return inject_state(subnet, direction).space_event;
  }

  bool directional_eject() const {
    return mode_ == TmRingNodeInterfaceMode::RBRG_DIRECTIONAL;
  }

  bool push_inject(TmRingSubnet subnet, p_tm_pld_t pld) {
    return push_inject(subnet,
                       static_cast<TmRingPortDir>(pld->ring_direction), pld);
  }

  bool push_inject(TmRingSubnet subnet, TmRingPortDir direction,
                   p_tm_pld_t pld) {
    QueueState& state = inject_state(subnet, direction);
    if (state.queue->full()) {
      state.pmu->push_rejected(clk_->time(), state.occupancy);
      return false;
    }
    // A successful injection begins a new Ring serialization segment.
    pld->ring_segment_serialization_paid = false;
    record_push(state, pld);
    return true;
  }

  p_tm_pld_t front_inject(TmRingSubnet subnet,
                          TmRingPortDir direction) const {
    auto q = inject_bank(subnet, direction);
    return !q->valid() || q->empty() ? nullptr : q->front();
  }

  void pop_inject(TmRingSubnet subnet, TmRingPortDir direction) {
    QueueState& state = inject_state(subnet, direction);
    if (state.queue->empty()) {
      return;
    }
    record_pop(state);
  }

  bool push_eject(TmRingSubnet subnet, p_tm_pld_t pld) {
    return push_eject(subnet, TmRingPortDir::CW, pld);
  }

  bool push_eject(TmRingSubnet subnet, TmRingPortDir direction,
                  p_tm_pld_t pld) {
    QueueState& state = eject_state(subnet, direction);
    if (state.queue->full()) {
      if (directional_eject()) {
        QueueState& aggregate =
            aggregate_eject_states_[tm_ring_subnet_index(subnet)];
        aggregate.pmu->push_rejected(clk_->time(), aggregate.occupancy);
      } else {
        state.pmu->push_rejected(clk_->time(), state.occupancy);
      }
      return false;
    }
    record_push(state, pld);
    if (directional_eject()) {
      record_account_push(aggregate_eject_states_[tm_ring_subnet_index(subnet)]);
    }
    return true;
  }

  p_tm_pld_t front_eject(TmRingSubnet subnet) const {
    return front_eject(subnet, TmRingPortDir::CW);
  }

  p_tm_pld_t front_eject(TmRingSubnet subnet,
                          TmRingPortDir direction) const {
    auto q = eject_q(subnet, direction);
    return q->empty() ? nullptr : q->front();
  }

  void pop_eject(TmRingSubnet subnet) {
    pop_eject(subnet, TmRingPortDir::CW);
  }

  void pop_eject(TmRingSubnet subnet, TmRingPortDir direction) {
    QueueState& state = eject_state(subnet, direction);
    if (state.queue->empty()) {
      return;
    }
    record_pop(state);
    if (directional_eject()) {
      record_account_pop(aggregate_eject_states_[tm_ring_subnet_index(subnet)]);
    }
  }

 private:
  struct QueueState {
    p_tm_com_que_t queue = nullptr;
    tm_engine::p_tm_event_t space_event = nullptr;
    uint32_t depth = 0;
    uint32_t occupancy = 0;
    std::unique_ptr<TmRingQueuePmuPort> pmu;
  };

  uint32_t direction_index(TmRingPortDir direction) const {
    return direction == TmRingPortDir::CCW ? 1 : 0;
  }

  uint32_t eject_direction_index(TmRingPortDir direction) const {
    return directional_eject() ? direction_index(direction) : 0;
  }

  QueueState& inject_state(TmRingSubnet subnet, TmRingPortDir direction) {
    return inject_states_[tm_ring_subnet_index(subnet)]
                         [direction_index(direction)];
  }

  const QueueState& inject_state(TmRingSubnet subnet,
                                 TmRingPortDir direction) const {
    return inject_states_[tm_ring_subnet_index(subnet)]
                         [direction_index(direction)];
  }

  QueueState& eject_state(TmRingSubnet subnet, TmRingPortDir direction) {
    return eject_states_[tm_ring_subnet_index(subnet)]
                        [eject_direction_index(direction)];
  }

  const QueueState& eject_state(TmRingSubnet subnet,
                                TmRingPortDir direction) const {
    return eject_states_[tm_ring_subnet_index(subnet)]
                        [eject_direction_index(direction)];
  }

  void record_push(QueueState& state, p_tm_pld_t pld) {
    state.queue->push_back(pld);
    record_account_push(state);
  }

  void record_account_push(QueueState& state) {
    ++state.occupancy;
    if (state.pmu != nullptr) {
      state.pmu->push_accepted(clk_->time(), state.occupancy);
    }
  }

  void record_pop(QueueState& state) {
    state.queue->pop_front();
    record_account_pop(state);
    if (state.space_event != nullptr) {
      state.space_event->notify_after(0);
    }
  }

  void record_account_pop(QueueState& state) {
    --state.occupancy;
    if (state.pmu != nullptr) {
      state.pmu->popped(clk_->time(), state.occupancy);
    }
  }

  void reset_queue_state(QueueState& state, uint64_t now) {
    state.queue->clear();
    reset_accounting_state(state, now);
  }

  void reset_accounting_state(QueueState& state, uint64_t now) {
    state.occupancy = 0;
  }

  tm_engine::p_tm_clk_t clk_ = nullptr;
  std::string name_;
  TmRingNodeInterfaceMode mode_ =
      TmRingNodeInterfaceMode::SHARED_EJECT;
  std::array<std::array<QueueState, 2>, 3> inject_states_;
  std::array<std::array<QueueState, 2>, 3> eject_states_;
  std::array<QueueState, 3> aggregate_eject_states_;
};

using p_tm_ring_node_interface_t = std::shared_ptr<TmRingNodeInterface>;

inline p_tm_ring_node_interface_t tm_make_ring_node_interface(
    tm_engine::p_tm_clk_t clk, const std::string& name,
    const TmRingEndpointQueueDepths& queue_depths,
    const std::vector<TmRingQueuePmuPort>& queue_pmu_ports,
    TmRingNodeInterfaceMode mode =
        TmRingNodeInterfaceMode::SHARED_EJECT,
    uint32_t inject_latency = 0) {
  return std::make_shared<TmRingNodeInterface>(
      clk, name, queue_depths, queue_pmu_ports, mode, inject_latency);
}

#endif  // _TM_RING_NODE_INTERFACE_H_
