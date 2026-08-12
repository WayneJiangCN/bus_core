#include "tm_ring_pmu.h"

#include <algorithm>
#include <stdexcept>

namespace {

void merge_conn(TmRingConnStats* into, const TmRingConnStats& from) {
  into->packets += from.packets;
  into->bytes += from.bytes;
  into->busy_cycles += from.busy_cycles;
  into->downstream_register_full_stall += from.downstream_register_full_stall;
  into->serialization_busy_stall += from.serialization_busy_stall;
  into->pipeline_full_stall += from.pipeline_full_stall;
  into->send_reject_stall += from.send_reject_stall;
  into->inflight_peak = std::max(into->inflight_peak, from.inflight_peak);
}

void merge_cross(TmRingCrossStationStats* into,
                 const TmRingCrossStationStats& from) {
  into->transit_slots += from.transit_slots;
  into->injected_packets += from.injected_packets;
  into->ejected_packets += from.ejected_packets;
  into->inject_queue_full_stalls += from.inject_queue_full_stalls;
  into->eject_queue_full_stalls += from.eject_queue_full_stalls;
  into->slot_pool_full_stalls += from.slot_pool_full_stalls;
  into->i_tag_sets += from.i_tag_sets;
  into->i_tag_claims += from.i_tag_claims;
  into->e_tag_sets += from.e_tag_sets;
  into->e_tag_claims += from.e_tag_claims;
  into->tagged_empty_slots += from.tagged_empty_slots;
  for (uint32_t i = 0; i < into->deflection.size(); ++i) {
    TmRingDeflectionStats& dst = into->deflection[i];
    const TmRingDeflectionStats& src = from.deflection[i];
    dst.events += src.events;
    dst.unique_packets += src.unique_packets;
    dst.eligible_unicast_packets += src.eligible_unicast_packets;
    dst.completed_packets += src.completed_packets;
    dst.rounds_sum += src.rounds_sum;
    dst.rounds_max = std::max(dst.rounds_max, src.rounds_max);
    dst.delay_cycles_sum += src.delay_cycles_sum;
    dst.delay_cycles_max = std::max(dst.delay_cycles_max, src.delay_cycles_max);
    dst.fanout_recipient_retry_events += src.fanout_recipient_retry_events;
  }
}

void merge_ha(TmRingHomeAgentStats* into, const TmRingHomeAgentStats& from) {
  into->rd_requests += from.rd_requests;
  into->rd_entries_allocated += from.rd_entries_allocated;
  into->rd_merged_pending += from.rd_merged_pending;
  into->rd_merged_inflight += from.rd_merged_inflight;
  into->rd_merged_responding += from.rd_merged_responding;
  into->rd_backend_issued += from.rd_backend_issued;
  into->backend_read_saved += from.backend_read_saved;
  into->l2_hit_transactions += from.l2_hit_transactions;
  into->l2_miss_transactions += from.l2_miss_transactions;
  into->functional_reads += from.functional_reads;
  into->private_l2_full_stall_cycles += from.private_l2_full_stall_cycles;
  into->table_full_stall_cycles += from.table_full_stall_cycles;
  into->waiter_full_stall_cycles += from.waiter_full_stall_cycles;
  into->aggregation_closed_stall_cycles += from.aggregation_closed_stall_cycles;
  into->write_hazard_stall_cycles += from.write_hazard_stall_cycles;
  into->completion_buffer_bytes_peak = std::max(
      into->completion_buffer_bytes_peak, from.completion_buffer_bytes_peak);
  into->useful_bytes += from.useful_bytes;
  into->backend_read_bytes += from.backend_read_bytes;
  for (uint32_t i = 0; i < into->completed_transaction_waiters.size(); ++i) {
    into->completed_transaction_waiters[i] += from.completed_transaction_waiters[i];
  }
}

void merge_l2(TmRingL2BufferStats* into, const TmRingL2BufferStats& from) {
  into->responses_accepted += from.responses_accepted;
  into->dat_bytes += from.dat_bytes;
  into->buffer_occupancy_peak =
      std::max(into->buffer_occupancy_peak, from.buffer_occupancy_peak);
  into->buffer_full_stall_cycles += from.buffer_full_stall_cycles;
  into->latency_wait_cycles += from.latency_wait_cycles;
  into->issue_interval_stall_cycles += from.issue_interval_stall_cycles;
  into->dat_inject_full_stall_cycles += from.dat_inject_full_stall_cycles;
  into->h_carriers += from.h_carriers;
  into->h_unicast_carriers += from.h_unicast_carriers;
  into->h_multicast_carriers += from.h_multicast_carriers;
  into->h_scatter_carriers += from.h_scatter_carriers;
  into->h_carrier_recipients += from.h_carrier_recipients;
  into->injected_carrier_128b += from.injected_carrier_128b;
  into->injected_carrier_256b += from.injected_carrier_256b;
  into->injected_carrier_512b += from.injected_carrier_512b;
  into->injected_carrier_other += from.injected_carrier_other;
}

}  // namespace

class TmRingPmu::Impl {
 public:
  struct QueueEntry {
    TmRingEndpointQueueStats endpoint;
    uint64_t last_change_cycle = 0;
  };
  struct ConnEntry {
    TmRingDomainType domain = TmRingDomainType::V_RING;
    uint32_t ring_id = 0;
    uint32_t src_station = 0;
    TmRingPortDir src_dir = TmRingPortDir::LOCAL;
    uint32_t dst_station = 0;
    TmRingPortDir dst_dir = TmRingPortDir::LOCAL;
    std::array<TmRingConnStats, 3> stats;
  };
  struct CrossEntry {
    TmRingDomainType domain = TmRingDomainType::V_RING;
    uint32_t ring_id = 0;
    uint32_t station_id = 0;
    TmRingCrossStationStats stats;
  };
  struct RbrgEntry {
    uint32_t rbrg_id = 0;
    TmRingRbrgStats stats;
    std::array<uint32_t, 4> occupancy;
  };
  struct HaEntry {
    uint32_t ha_id = 0;
    uint32_t master_count = 0;
    TmRingHomeAgentStats stats;
    std::vector<TmRingHaSourceStats> sources;
  };
  struct L2Entry {
    uint32_t target_id = 0;
    TmRingL2BufferStats stats;
  };

  uint64_t reset_cycle = 0;
  std::vector<QueueEntry> queues;
  std::vector<ConnEntry> conns;
  std::vector<CrossEntry> crosses;
  std::vector<RbrgEntry> rbrgs;
  std::vector<HaEntry> has;
  std::vector<L2Entry> l2s;
};

namespace {

void settle_queue(TmRingPmu::Impl::QueueEntry* entry, uint64_t cycle) {
  TmRingQueueStats& queue = entry->endpoint.queue;
  queue.counters.occupancy_area += static_cast<uint64_t>(queue.occupancy) *
                                   (cycle - entry->last_change_cycle);
  if (queue.occupancy == queue.depth) {
    queue.counters.full_cycles += cycle - entry->last_change_cycle;
  }
  entry->last_change_cycle = cycle;
}

TmRingConnHotspot conn_hotspot(const TmRingPmu::Impl::ConnEntry& entry,
                               TmRingSubnet subnet) {
  const TmRingConnStats& stats = entry.stats[tm_ring_subnet_index(subnet)];
  TmRingConnHotspot hot;
  hot.src_station = entry.src_station;
  hot.src_dir = entry.src_dir;
  hot.dst_station = entry.dst_station;
  hot.dst_dir = entry.dst_dir;
  hot.subnet = subnet;
  hot.packets = stats.packets;
  hot.bytes = stats.bytes;
  hot.busy_cycles = stats.busy_cycles;
  hot.serialization_busy_stall = stats.serialization_busy_stall;
  hot.total_stalls = tm_ring_conn_total_stalls(stats);
  hot.inflight_peak = stats.inflight_peak;
  return hot;
}

}  // namespace

TmRingQueuePmuPort::TmRingQueuePmuPort(TmRingPmu* pmu, uint32_t id)
    : pmu_(pmu), id_(id) {}
void TmRingQueuePmuPort::push_accepted(uint64_t cycle,
                                       uint32_t occupancy_after) const {
  pmu_->queue_push_accepted(id_, cycle, occupancy_after);
}
void TmRingQueuePmuPort::push_rejected(uint64_t cycle,
                                       uint32_t occupancy_current) const {
  pmu_->queue_push_rejected(id_, cycle, occupancy_current);
}
void TmRingQueuePmuPort::popped(uint64_t cycle, uint32_t occupancy_after) const {
  pmu_->queue_popped(id_, cycle, occupancy_after);
}

TmRingConnPmuPort::TmRingConnPmuPort(TmRingPmu* pmu, uint32_t id)
    : pmu_(pmu), id_(id) {}
void TmRingConnPmuPort::accepted(TmRingSubnet subnet, uint32_t bytes,
                                 uint32_t serialization_cycles,
                                 uint32_t inflight_after) const {
  pmu_->conn_accepted(id_, subnet, bytes, serialization_cycles, inflight_after);
}
void TmRingConnPmuPort::rejected(TmRingSubnet subnet,
                                 TmRingConnRejectReason reason) const {
  pmu_->conn_rejected(id_, subnet, reason);
}
void TmRingConnPmuPort::downstream_blocked(TmRingSubnet subnet) const {
  pmu_->conn_downstream_blocked(id_, subnet);
}

TmRingCrossStationPmuPort::TmRingCrossStationPmuPort(TmRingPmu* pmu,
                                                     uint32_t id)
    : pmu_(pmu), id_(id) {}
void TmRingCrossStationPmuPort::transit_committed(
    TmRingSubnet subnet, TmRingSlotKind slot_kind) const {
  pmu_->cross_transit_committed(id_, subnet, slot_kind);
}
void TmRingCrossStationPmuPort::packet_injected(TmRingSubnet subnet) const {
  pmu_->cross_packet_injected(id_, subnet);
}
void TmRingCrossStationPmuPort::packet_ejected(TmRingSubnet subnet,
                                               p_tm_pld_t slot,
                                               uint64_t cycle) const {
  pmu_->cross_packet_ejected(id_, subnet, slot, cycle);
}
void TmRingCrossStationPmuPort::packet_deflected(TmRingSubnet subnet,
                                                 p_tm_pld_t slot,
                                                 uint64_t cycle,
                                                 bool fanout_retry) const {
  pmu_->cross_packet_deflected(id_, subnet, slot, cycle, fanout_retry);
}
void TmRingCrossStationPmuPort::slot_pool_blocked(
    TmRingSubnet subnet, TmRingPortDir direction) const {
  pmu_->cross_slot_pool_blocked(id_, subnet, direction);
}
void TmRingCrossStationPmuPort::i_tag_set(TmRingSubnet subnet) const {
  pmu_->cross_i_tag_set(id_, subnet);
}
void TmRingCrossStationPmuPort::i_tag_claimed(TmRingSubnet subnet) const {
  pmu_->cross_i_tag_claimed(id_, subnet);
}
void TmRingCrossStationPmuPort::e_tag_set(TmRingSubnet subnet) const {
  pmu_->cross_e_tag_set(id_, subnet);
}
void TmRingCrossStationPmuPort::e_tag_claimed(TmRingSubnet subnet) const {
  pmu_->cross_e_tag_claimed(id_, subnet);
}

TmRingRbrgPmuPort::TmRingRbrgPmuPort(TmRingPmu* pmu, uint32_t id)
    : pmu_(pmu), id_(id) {}
void TmRingRbrgPmuPort::enqueued(TmRingRbrgPath path,
                                 uint32_t serialization_cycles) const {
  pmu_->rbrg_enqueued(id_, path, serialization_cycles);
}
void TmRingRbrgPmuPort::queue_blocked(TmRingRbrgPath path) const {
  pmu_->rbrg_queue_blocked(id_, path);
}
void TmRingRbrgPmuPort::destination_blocked(TmRingRbrgPath path) const {
  pmu_->rbrg_destination_blocked(id_, path);
}
void TmRingRbrgPmuPort::delivered(TmRingRbrgPath path, uint32_t bytes) const {
  pmu_->rbrg_delivered(id_, path, bytes);
}

TmRingHaPmuPort::TmRingHaPmuPort(TmRingPmu* pmu, uint32_t id)
    : pmu_(pmu), id_(id) {}
void TmRingHaPmuPort::read_admission(uint32_t master_id, uint32_t bytes,
                                     TmRingHaReadOutcome outcome) const {
  pmu_->ha_read_admission(id_, master_id, bytes, outcome);
}
void TmRingHaPmuPort::backend_read_issued(uint32_t bytes) const {
  pmu_->ha_backend_read_issued(id_, bytes);
}
void TmRingHaPmuPort::functional_read_completed() const {
  pmu_->ha_functional_read_completed(id_);
}
void TmRingHaPmuPort::private_l2_blocked() const {
  pmu_->ha_private_l2_blocked(id_);
}
void TmRingHaPmuPort::completion_buffer_sample(uint32_t bytes) const {
  pmu_->ha_completion_buffer_sample(id_, bytes);
}
void TmRingHaPmuPort::transaction_completed(uint32_t waiter_count) const {
  pmu_->ha_transaction_completed(id_, waiter_count);
}
void TmRingHaPmuPort::write_reservation_blocked() const {
  pmu_->ha_write_reservation_blocked(id_);
}
void TmRingHaPmuPort::source_request_received(uint32_t master_id,
                                              PldCmd cmd) const {
  pmu_->ha_source_request_received(id_, master_id, cmd);
}

TmRingL2PmuPort::TmRingL2PmuPort(TmRingPmu* pmu, uint32_t id)
    : pmu_(pmu), id_(id) {}
void TmRingL2PmuPort::response_admitted(TmRingL2AcceptStatus result,
                                        uint32_t occupancy_after,
                                        uint32_t response_latency) const {
  pmu_->l2_response_admitted(id_, result, occupancy_after, response_latency);
}
void TmRingL2PmuPort::buffer_blocked() const { pmu_->l2_buffer_blocked(id_); }
void TmRingL2PmuPort::issue_interval_blocked() const {
  pmu_->l2_issue_interval_blocked(id_);
}
void TmRingL2PmuPort::dat_inject_blocked() const {
  pmu_->l2_dat_inject_blocked(id_);
}
void TmRingL2PmuPort::carrier_injected(uint32_t bytes,
                                       uint32_t recipient_count,
                                       TmRingFanoutMode fanout_mode) const {
  pmu_->l2_carrier_injected(id_, bytes, recipient_count, fanout_mode);
}

TmRingPmu::TmRingPmu() : impl_(new Impl()) {}
TmRingPmu::~TmRingPmu() {}

std::vector<TmRingQueuePmuPort> TmRingPmu::register_endpoint_queues(
    TmRingNodeType node_type, uint32_t node_id,
    const TmRingEndpointQueueDepths& depths) {
  std::vector<TmRingQueuePmuPort> ports;
  for (uint32_t dir = 0; dir < 2; ++dir) {
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      Impl::QueueEntry entry;
      entry.endpoint.node_type = node_type;
      entry.endpoint.node_id = node_id;
      entry.endpoint.queue.subnet = static_cast<TmRingSubnet>(subnet);
      entry.endpoint.queue.side = TmRingQueueSide::INJECT;
      entry.endpoint.queue.direction =
          dir == 0 ? TmRingPortDir::CW : TmRingPortDir::CCW;
      entry.endpoint.queue.depth = depths.inject[subnet];
      impl_->queues.push_back(entry);
      ports.push_back(TmRingQueuePmuPort(this, impl_->queues.size() - 1));
    }
  }
  for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
    Impl::QueueEntry entry;
    entry.endpoint.node_type = node_type;
    entry.endpoint.node_id = node_id;
    entry.endpoint.queue.subnet = static_cast<TmRingSubnet>(subnet);
    entry.endpoint.queue.side = TmRingQueueSide::EJECT;
    entry.endpoint.queue.direction = TmRingPortDir::LOCAL;
    entry.endpoint.queue.depth = depths.eject[subnet];
    impl_->queues.push_back(entry);
    ports.push_back(TmRingQueuePmuPort(this, impl_->queues.size() - 1));
  }
  return ports;
}

TmRingConnPmuPort TmRingPmu::register_conn(
    TmRingDomainType domain, uint32_t ring_id, uint32_t src_station,
    TmRingPortDir src_dir, uint32_t dst_station, TmRingPortDir dst_dir) {
  Impl::ConnEntry entry;
  entry.domain = domain;
  entry.ring_id = ring_id;
  entry.src_station = src_station;
  entry.src_dir = src_dir;
  entry.dst_station = dst_station;
  entry.dst_dir = dst_dir;
  impl_->conns.push_back(entry);
  return TmRingConnPmuPort(this, impl_->conns.size() - 1);
}

TmRingCrossStationPmuPort TmRingPmu::register_cross_station(
    TmRingDomainType domain, uint32_t ring_id, uint32_t station_id) {
  Impl::CrossEntry entry;
  entry.domain = domain;
  entry.ring_id = ring_id;
  entry.station_id = station_id;
  impl_->crosses.push_back(entry);
  return TmRingCrossStationPmuPort(this, impl_->crosses.size() - 1);
}

TmRingRbrgPmuPort TmRingPmu::register_rbrg(uint32_t rbrg_id) {
  if (impl_->rbrgs.size() <= rbrg_id) impl_->rbrgs.resize(rbrg_id + 1);
  Impl::RbrgEntry entry;
  entry.rbrg_id = rbrg_id;
  impl_->rbrgs[rbrg_id] = entry;
  return TmRingRbrgPmuPort(this, rbrg_id);
}

TmRingHaPmuPort TmRingPmu::register_home_agent(uint32_t ha_id,
                                                uint32_t master_count) {
  Impl::HaEntry entry;
  entry.ha_id = ha_id;
  entry.master_count = master_count;
  impl_->has.push_back(entry);
  return TmRingHaPmuPort(this, impl_->has.size() - 1);
}

TmRingL2PmuPort TmRingPmu::register_l2(uint32_t target_id) {
  Impl::L2Entry entry;
  entry.target_id = target_id;
  impl_->l2s.push_back(entry);
  return TmRingL2PmuPort(this, impl_->l2s.size() - 1);
}

void TmRingPmu::reset_model(uint64_t cycle) {
  impl_->reset_cycle = cycle;
  for (Impl::QueueEntry& entry : impl_->queues) {
    entry.endpoint.queue.occupancy = 0;
    entry.endpoint.queue.occupancy_peak = 0;
    entry.endpoint.queue.counters = TmRingQueueCounters();
    entry.last_change_cycle = cycle;
  }
  for (Impl::ConnEntry& entry : impl_->conns) entry.stats = {};
  for (Impl::CrossEntry& entry : impl_->crosses) entry.stats = {};
  for (Impl::RbrgEntry& entry : impl_->rbrgs) {
    entry.stats = {};
    entry.occupancy = {};
  }
  for (Impl::HaEntry& entry : impl_->has) {
    entry.stats = {};
    entry.sources.clear();
  }
  for (Impl::L2Entry& entry : impl_->l2s) entry.stats = {};
}

void TmRingPmu::queue_push_accepted(uint32_t id, uint64_t cycle,
                                    uint32_t occupancy_after) {
  Impl::QueueEntry& entry = impl_->queues[id];
  settle_queue(&entry, cycle);
  TmRingQueueStats& queue = entry.endpoint.queue;
  ++queue.counters.pushes;
  queue.occupancy = occupancy_after;
  queue.occupancy_peak = std::max(queue.occupancy_peak, occupancy_after);
}
void TmRingPmu::queue_push_rejected(uint32_t id, uint64_t cycle,
                                    uint32_t occupancy_current) {
  Impl::QueueEntry& entry = impl_->queues[id];
  settle_queue(&entry, cycle);
  TmRingQueueStats& queue = entry.endpoint.queue;
  ++queue.counters.push_rejects;
  queue.occupancy = occupancy_current;
  queue.occupancy_peak = std::max(queue.occupancy_peak, occupancy_current);
}
void TmRingPmu::queue_popped(uint32_t id, uint64_t cycle,
                             uint32_t occupancy_after) {
  Impl::QueueEntry& entry = impl_->queues[id];
  settle_queue(&entry, cycle);
  TmRingQueueStats& queue = entry.endpoint.queue;
  ++queue.counters.pops;
  queue.occupancy = occupancy_after;
  queue.occupancy_peak = std::max(queue.occupancy_peak, occupancy_after);
}

void TmRingPmu::conn_accepted(uint32_t id, TmRingSubnet subnet,
                              uint32_t bytes, uint32_t serialization_cycles,
                              uint32_t inflight_after) {
  TmRingConnStats& stats = impl_->conns[id].stats[tm_ring_subnet_index(subnet)];
  ++stats.packets;
  stats.bytes += bytes;
  stats.busy_cycles += serialization_cycles;
  stats.inflight_peak = std::max(stats.inflight_peak, inflight_after);
}
void TmRingPmu::conn_rejected(uint32_t id, TmRingSubnet subnet,
                              TmRingConnRejectReason reason) {
  TmRingConnStats& stats = impl_->conns[id].stats[tm_ring_subnet_index(subnet)];
  ++stats.send_reject_stall;
  if (reason == TmRingConnRejectReason::SERIALIZER_BUSY) {
    ++stats.serialization_busy_stall;
  } else if (reason == TmRingConnRejectReason::PIPELINE_FULL) {
    ++stats.pipeline_full_stall;
  }
}
void TmRingPmu::conn_downstream_blocked(uint32_t id, TmRingSubnet subnet) {
  ++impl_->conns[id]
        .stats[tm_ring_subnet_index(subnet)]
        .downstream_register_full_stall;
}

void TmRingPmu::cross_transit_committed(uint32_t id, TmRingSubnet subnet,
                                        TmRingSlotKind slot_kind) {
  static_cast<void>(subnet);
  TmRingCrossStationStats& stats = impl_->crosses[id].stats;
  ++stats.transit_slots;
  if (slot_kind == TmRingSlotKind::TAGGED_EMPTY) ++stats.tagged_empty_slots;
}
void TmRingPmu::cross_packet_injected(uint32_t id, TmRingSubnet subnet) {
  static_cast<void>(subnet);
  ++impl_->crosses[id].stats.injected_packets;
}
void TmRingPmu::cross_packet_ejected(uint32_t id, TmRingSubnet subnet,
                                     p_tm_pld_t slot, uint64_t cycle) {
  TmRingCrossStationStats& stats = impl_->crosses[id].stats;
  ++stats.ejected_packets;
  if (slot == nullptr || !tm_ring_has_fanout(slot)) {
    TmRingDeflectionStats& deflection =
        stats.deflection[tm_ring_subnet_index(subnet)];
    ++deflection.eligible_unicast_packets;
    if (slot != nullptr && slot->ring_deflection_started) {
      const uint64_t rounds = slot->ring_deflection_count;
      const uint64_t delay = cycle - slot->ring_first_deflection_cycle;
      ++deflection.completed_packets;
      deflection.rounds_sum += rounds;
      deflection.rounds_max = std::max(deflection.rounds_max, rounds);
      deflection.delay_cycles_sum += delay;
      deflection.delay_cycles_max = std::max(deflection.delay_cycles_max, delay);
    }
  }
}
void TmRingPmu::cross_packet_deflected(uint32_t id, TmRingSubnet subnet,
                                       p_tm_pld_t slot, uint64_t cycle,
                                       bool fanout_retry) {
  TmRingCrossStationStats& stats = impl_->crosses[id].stats;
  TmRingDeflectionStats& deflection =
      stats.deflection[tm_ring_subnet_index(subnet)];
  ++stats.eject_queue_full_stalls;
  if (fanout_retry) {
    ++deflection.fanout_recipient_retry_events;
    return;
  }
  if (slot != nullptr && !slot->ring_deflection_started) {
    slot->ring_deflection_started = true;
    slot->ring_first_deflection_cycle = cycle;
    ++deflection.unique_packets;
  }
  ++deflection.events;
  if (slot != nullptr) ++slot->ring_deflection_count;
}
void TmRingPmu::cross_slot_pool_blocked(uint32_t id, TmRingSubnet subnet,
                                        TmRingPortDir direction) {
  static_cast<void>(subnet);
  static_cast<void>(direction);
  ++impl_->crosses[id].stats.slot_pool_full_stalls;
}
void TmRingPmu::cross_i_tag_set(uint32_t id, TmRingSubnet subnet) {
  static_cast<void>(subnet);
  ++impl_->crosses[id].stats.i_tag_sets;
}
void TmRingPmu::cross_i_tag_claimed(uint32_t id, TmRingSubnet subnet) {
  static_cast<void>(subnet);
  ++impl_->crosses[id].stats.i_tag_claims;
}
void TmRingPmu::cross_e_tag_set(uint32_t id, TmRingSubnet subnet) {
  static_cast<void>(subnet);
  ++impl_->crosses[id].stats.e_tag_sets;
}
void TmRingPmu::cross_e_tag_claimed(uint32_t id, TmRingSubnet subnet) {
  static_cast<void>(subnet);
  ++impl_->crosses[id].stats.e_tag_claims;
}

void TmRingPmu::rbrg_enqueued(uint32_t id, TmRingRbrgPath path,
                              uint32_t serialization_cycles) {
  Impl::RbrgEntry& entry = impl_->rbrgs[id];
  const uint32_t index = static_cast<uint32_t>(path);
  TmRingRbrgPathStats& stats = entry.stats.paths[index];
  stats.busy_cycles += serialization_cycles;
  ++entry.occupancy[index];
  stats.queue_occupancy_peak =
      std::max(stats.queue_occupancy_peak, uint64_t(entry.occupancy[index]));
}
void TmRingPmu::rbrg_queue_blocked(uint32_t id, TmRingRbrgPath path) {
  ++impl_->rbrgs[id].stats.paths[static_cast<uint32_t>(path)].queue_full_stalls;
}
void TmRingPmu::rbrg_destination_blocked(uint32_t id, TmRingRbrgPath path) {
  ++impl_->rbrgs[id]
        .stats.paths[static_cast<uint32_t>(path)]
        .destination_inject_stalls;
}
void TmRingPmu::rbrg_delivered(uint32_t id, TmRingRbrgPath path,
                               uint32_t bytes) {
  Impl::RbrgEntry& entry = impl_->rbrgs[id];
  const uint32_t index = static_cast<uint32_t>(path);
  TmRingRbrgPathStats& stats = entry.stats.paths[index];
  ++stats.packets;
  stats.bytes += bytes;
  if (entry.occupancy[index] != 0) --entry.occupancy[index];
}

void TmRingPmu::ha_read_admission(uint32_t id, uint32_t master_id,
                                  uint32_t bytes,
                                  TmRingHaReadOutcome outcome) {
  static_cast<void>(master_id);
  TmRingHomeAgentStats& stats = impl_->has[id].stats;
  switch (outcome) {
    case TmRingHaReadOutcome::STALL_WRITE_HAZARD:
      ++stats.write_hazard_stall_cycles;
      break;
    case TmRingHaReadOutcome::STALL_AGGREGATION_CLOSED:
      ++stats.aggregation_closed_stall_cycles;
      break;
    case TmRingHaReadOutcome::STALL_WAITER_FULL:
      ++stats.waiter_full_stall_cycles;
      break;
    case TmRingHaReadOutcome::STALL_TABLE_FULL:
      ++stats.table_full_stall_cycles;
      break;
    case TmRingHaReadOutcome::MERGED_PENDING:
      ++stats.rd_merged_pending;
      ++stats.backend_read_saved;
      break;
    case TmRingHaReadOutcome::MERGED_INFLIGHT:
      ++stats.rd_merged_inflight;
      ++stats.backend_read_saved;
      break;
    case TmRingHaReadOutcome::MERGED_RESPONDING:
      ++stats.rd_merged_responding;
      ++stats.backend_read_saved;
      break;
    case TmRingHaReadOutcome::ACCEPTED_L2_HIT:
      ++stats.rd_requests;
      ++stats.rd_entries_allocated;
      ++stats.l2_hit_transactions;
      stats.useful_bytes += bytes;
      break;
    case TmRingHaReadOutcome::ACCEPTED_L2_MISS:
      ++stats.rd_requests;
      ++stats.rd_entries_allocated;
      ++stats.l2_miss_transactions;
      stats.useful_bytes += bytes;
      break;
    case TmRingHaReadOutcome::BYPASS:
      ++stats.rd_requests;
      stats.useful_bytes += bytes;
      break;
  }
}
void TmRingPmu::ha_backend_read_issued(uint32_t id, uint32_t bytes) {
  TmRingHomeAgentStats& stats = impl_->has[id].stats;
  ++stats.rd_backend_issued;
  stats.backend_read_bytes += bytes;
}
void TmRingPmu::ha_functional_read_completed(uint32_t id) {
  ++impl_->has[id].stats.functional_reads;
}
void TmRingPmu::ha_private_l2_blocked(uint32_t id) {
  ++impl_->has[id].stats.private_l2_full_stall_cycles;
}
void TmRingPmu::ha_completion_buffer_sample(uint32_t id, uint32_t bytes) {
  TmRingHomeAgentStats& stats = impl_->has[id].stats;
  stats.completion_buffer_bytes_peak =
      std::max(stats.completion_buffer_bytes_peak, uint64_t(bytes));
}
void TmRingPmu::ha_transaction_completed(uint32_t id, uint32_t waiter_count) {
  ++impl_->has[id]
        .stats.completed_transaction_waiters[std::min<uint32_t>(64, waiter_count)];
}
void TmRingPmu::ha_write_reservation_blocked(uint32_t id) {
  ++impl_->has[id].stats.write_hazard_stall_cycles;
}
void TmRingPmu::ha_source_request_received(uint32_t id, uint32_t master_id,
                                           PldCmd cmd) {
  Impl::HaEntry& entry = impl_->has[id];
  for (TmRingHaSourceStats& source : entry.sources) {
    if (source.master_id == master_id) {
      if (cmd == PldCmd::RD) ++source.rd_packets;
      if (cmd == PldCmd::WR) ++source.wr_packets;
      return;
    }
  }
  TmRingHaSourceStats source;
  source.ha_id = entry.ha_id;
  source.master_id = master_id;
  if (cmd == PldCmd::RD) ++source.rd_packets;
  if (cmd == PldCmd::WR) ++source.wr_packets;
  entry.sources.push_back(source);
}

void TmRingPmu::l2_response_admitted(uint32_t id, TmRingL2AcceptStatus result,
                                     uint32_t occupancy_after,
                                     uint32_t response_latency) {
  if (result == TmRingL2AcceptStatus::REJECTED_BUFFER_FULL) return;
  TmRingL2BufferStats& stats = impl_->l2s[id].stats;
  ++stats.responses_accepted;
  stats.latency_wait_cycles += response_latency;
  stats.buffer_occupancy_peak =
      std::max(stats.buffer_occupancy_peak, uint64_t(occupancy_after));
}
void TmRingPmu::l2_buffer_blocked(uint32_t id) {
  ++impl_->l2s[id].stats.buffer_full_stall_cycles;
}
void TmRingPmu::l2_issue_interval_blocked(uint32_t id) {
  ++impl_->l2s[id].stats.issue_interval_stall_cycles;
}
void TmRingPmu::l2_dat_inject_blocked(uint32_t id) {
  ++impl_->l2s[id].stats.dat_inject_full_stall_cycles;
}
void TmRingPmu::l2_carrier_injected(uint32_t id, uint32_t bytes,
                                    uint32_t recipient_count,
                                    TmRingFanoutMode fanout_mode) {
  TmRingL2BufferStats& stats = impl_->l2s[id].stats;
  ++stats.h_carriers;
  stats.dat_bytes += bytes;
  stats.h_carrier_recipients += recipient_count;
  if (recipient_count == 1) {
    ++stats.h_unicast_carriers;
  } else if (fanout_mode == TmRingFanoutMode::MULTICAST) {
    ++stats.h_multicast_carriers;
  } else {
    ++stats.h_scatter_carriers;
  }
  if (bytes == 128) {
    ++stats.injected_carrier_128b;
  } else if (bytes == 256) {
    ++stats.injected_carrier_256b;
  } else if (bytes == 512) {
    ++stats.injected_carrier_512b;
  } else {
    ++stats.injected_carrier_other;
  }
}

TmRingPmuSnapshot TmRingPmu::snapshot(uint64_t cycle) const {
  TmRingPmuSnapshot snapshot;
  snapshot.reset_cycle = impl_->reset_cycle;
  for (const Impl::QueueEntry& stored : impl_->queues) {
    Impl::QueueEntry entry = stored;
    settle_queue(&entry, cycle);
    snapshot.queue.endpoints.push_back(entry.endpoint);
  }
  for (const Impl::ConnEntry& entry : impl_->conns) {
    TmRingDomainStats* domain = nullptr;
    for (TmRingDomainStats& candidate : snapshot.conn.domains) {
      if (candidate.type == entry.domain && candidate.ring_id == entry.ring_id) {
        domain = &candidate;
        break;
      }
    }
    if (domain == nullptr) {
      TmRingDomainStats created;
      created.type = entry.domain;
      created.ring_id = entry.ring_id;
      snapshot.conn.domains.push_back(created);
      domain = &snapshot.conn.domains.back();
    }
    ++domain->directed_edge_count;
    for (uint32_t subnet = 0; subnet < tm_ring_subnet_count(); ++subnet) {
      const TmRingSubnet kind = static_cast<TmRingSubnet>(subnet);
      merge_conn(&snapshot.conn.total[subnet], entry.stats[subnet]);
      if (entry.src_dir == TmRingPortDir::CW) {
        merge_conn(&domain->cw[subnet], entry.stats[subnet]);
      } else if (entry.src_dir == TmRingPortDir::CCW) {
        merge_conn(&domain->ccw[subnet], entry.stats[subnet]);
      }
      domain->edges.push_back(conn_hotspot(entry, kind));
    }
  }
  for (TmRingDomainStats& domain : snapshot.conn.domains) {
    std::sort(domain.edges.begin(), domain.edges.end(),
              [](const TmRingConnHotspot& left,
                 const TmRingConnHotspot& right) {
                return left.busy_cycles != right.busy_cycles
                           ? left.busy_cycles > right.busy_cycles
                           : left.total_stalls > right.total_stalls;
              });
    if (!domain.edges.empty()) domain.hottest = domain.edges.front();
  }
  for (const Impl::CrossEntry& entry : impl_->crosses) {
    merge_cross(&snapshot.cross_station.total, entry.stats);
  }
  for (const Impl::RbrgEntry& entry : impl_->rbrgs) {
    snapshot.rbrg.instances.push_back(entry.stats);
  }
  for (const Impl::HaEntry& entry : impl_->has) {
    merge_ha(&snapshot.ha.total, entry.stats);
    snapshot.ha.sources.insert(snapshot.ha.sources.end(), entry.sources.begin(),
                               entry.sources.end());
  }
  for (const Impl::L2Entry& entry : impl_->l2s) {
    merge_l2(&snapshot.l2.total, entry.stats);
  }
  return snapshot;
}

TmRingConnStallBreakdown TmRingPmuSnapshot::conn_stall_breakdown() const {
  TmRingConnStallBreakdown result;
  for (const TmRingConnStats& stats : conn.total) {
    result.serialization_busy += stats.serialization_busy_stall;
    result.pipeline_full += stats.pipeline_full_stall;
    result.downstream_register_full += stats.downstream_register_full_stall;
  }
  return result;
}

std::vector<TmRingConnHotspot> TmRingPmuSnapshot::top_busy_conns(
    TmRingSubnet subnet, uint32_t limit) const {
  std::vector<TmRingConnHotspot> result;
  for (const TmRingDomainStats& domain : conn.domains) {
    for (const TmRingConnHotspot& edge : domain.edges) {
      if (edge.subnet == subnet) result.push_back(edge);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const TmRingConnHotspot& left,
               const TmRingConnHotspot& right) {
              return left.busy_cycles != right.busy_cycles
                         ? left.busy_cycles > right.busy_cycles
                         : left.total_stalls > right.total_stalls;
            });
  if (result.size() > limit) result.resize(limit);
  return result;
}

const TmRingRbrgPathStats& TmRingPmuSnapshot::rbrg_path_stats(
    uint32_t rbrg_id, TmRingRbrgPath path) const {
  if (rbrg_id >= rbrg.instances.size()) {
    throw std::out_of_range("unknown RBRG PMU id");
  }
  return rbrg.instances[rbrg_id].paths[static_cast<uint32_t>(path)];
}
