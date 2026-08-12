#include <gtest/gtest.h>

#include <type_traits>

#include "tm_ring_pmu.h"

static_assert(!std::is_default_constructible<TmRingQueuePmuPort>::value,
              "Queue PMU port must always be registered");
static_assert(!std::is_default_constructible<TmRingConnPmuPort>::value,
              "Conn PMU port must always be registered");
static_assert(!std::is_default_constructible<TmRingCrossStationPmuPort>::value,
              "Cross-station PMU port must always be registered");
static_assert(!std::is_default_constructible<TmRingRbrgPmuPort>::value,
              "RBRG PMU port must always be registered");
static_assert(!std::is_default_constructible<TmRingHaPmuPort>::value,
              "HA PMU port must always be registered");
static_assert(!std::is_default_constructible<TmRingL2PmuPort>::value,
              "L2 PMU port must always be registered");

TEST(TmRingPmuTest, QueueSnapshotSettlesWithoutMutation) {
  TmRingPmu pmu;
  TmRingEndpointQueueDepths depths;
  depths.inject = {{2, 2, 2}};
  depths.eject = {{2, 2, 2}};
  const std::vector<TmRingQueuePmuPort> ports =
      pmu.register_endpoint_queues(TmRingNodeType::MASTER, 3, depths);

  TmRingQueuePmuPort dat_eject =
      ports[6 + tm_ring_subnet_index(TmRingSubnet::DAT)];
  dat_eject.push_accepted(0, 1);
  dat_eject.push_accepted(3, 2);
  dat_eject.popped(8, 1);

  const TmRingPmuSnapshot first = pmu.snapshot(10);
  const TmRingPmuSnapshot second = pmu.snapshot(10);
  const TmRingQueueStats& queue = first.queue.endpoints[8].queue;
  EXPECT_EQ(uint64_t(2), queue.counters.pushes);
  EXPECT_EQ(uint64_t(1), queue.counters.pops);
  EXPECT_EQ(uint32_t(2), queue.occupancy_peak);
  EXPECT_EQ(uint64_t(15), queue.counters.occupancy_area);
  EXPECT_EQ(uint64_t(5), queue.counters.full_cycles);
  EXPECT_EQ(queue.counters.occupancy_area,
            second.queue.endpoints[8].queue.counters.occupancy_area);
}

TEST(TmRingPmuTest, ConnRejectMapsOneAttemptToOneTotalReject) {
  TmRingPmu pmu;
  TmRingConnPmuPort conn = pmu.register_conn(
      TmRingDomainType::V_RING, 1, 2, TmRingPortDir::CW, 3,
      TmRingPortDir::CCW);
  conn.rejected(TmRingSubnet::DAT, TmRingConnRejectReason::SERIALIZER_BUSY);
  conn.downstream_blocked(TmRingSubnet::DAT);
  const TmRingPmuSnapshot snapshot = pmu.snapshot(0);
  const TmRingConnStats& dat = snapshot.conn.total[2];
  EXPECT_EQ(uint64_t(1), dat.send_reject_stall);
  EXPECT_EQ(uint64_t(1), dat.serialization_busy_stall);
  EXPECT_EQ(uint64_t(1), dat.downstream_register_full_stall);
  EXPECT_EQ(uint64_t(2), snapshot.conn_stall_breakdown().total());
}

TEST(TmRingPmuTest, ConnHotspotsKeepLegacySerializationFirstOrdering) {
  TmRingPmu pmu;
  TmRingConnPmuPort busy = pmu.register_conn(
      TmRingDomainType::V_RING, 0, 1, TmRingPortDir::CW, 2,
      TmRingPortDir::CCW);
  TmRingConnPmuPort serialization = pmu.register_conn(
      TmRingDomainType::V_RING, 0, 0, TmRingPortDir::CW, 1,
      TmRingPortDir::CCW);
  busy.accepted(TmRingSubnet::REQ, 4096, 100, 1);
  serialization.accepted(TmRingSubnet::REQ, 16, 1, 1);
  serialization.rejected(TmRingSubnet::REQ,
                         TmRingConnRejectReason::SERIALIZER_BUSY);

  const TmRingPmuSnapshot snapshot = pmu.snapshot(0);
  const std::vector<TmRingConnHotspot> hotspots =
      snapshot.top_busy_conns(TmRingSubnet::REQ, 2);
  ASSERT_EQ(size_t(2), hotspots.size());
  EXPECT_EQ(uint32_t(0), hotspots[0].src_station);
  EXPECT_EQ(uint64_t(1), hotspots[0].serialization_busy_stall);
  ASSERT_EQ(size_t(2), snapshot.conn.domains[0].edges.size());
  EXPECT_EQ(uint32_t(0), snapshot.conn.domains[0].hottest.src_station);
}

TEST(TmRingPmuTest, HaAndL2EventsPopulateBucketsAndSources) {
  TmRingPmu pmu;
  TmRingHaPmuPort ha = pmu.register_home_agent(2, 8);
  TmRingL2PmuPort l2 = pmu.register_l2(2);
  ha.source_request_received(5, PldCmd::RD);
  ha.read_admission(5, 128, TmRingHaReadOutcome::MERGED_INFLIGHT);
  ha.transaction_completed(3);
  l2.response_admitted(TmRingL2AcceptStatus::ACCEPTED_UNICAST, 1, 20);
  l2.carrier_injected(128, 1, TmRingFanoutMode::MULTICAST);
  const TmRingPmuSnapshot snapshot = pmu.snapshot(0);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.rd_merged_inflight);
  ASSERT_EQ(size_t(1), snapshot.ha.sources.size());
  EXPECT_EQ(uint32_t(5), snapshot.ha.sources[0].master_id);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.injected_carrier_128b);
}

TEST(TmRingPmuTest, ResetClearsCountersButKeepsRegisteredPorts) {
  TmRingPmu pmu;
  TmRingRbrgPmuPort rbrg = pmu.register_rbrg(0);
  rbrg.enqueued(TmRingRbrgPath::V_TO_H_REQ, 4);
  pmu.reset_model(9);
  rbrg.enqueued(TmRingRbrgPath::V_TO_H_REQ, 2);
  rbrg.delivered(TmRingRbrgPath::V_TO_H_REQ, 16);
  const TmRingPmuSnapshot snapshot = pmu.snapshot(9);
  EXPECT_EQ(uint64_t(1), snapshot.rbrg.instances[0].paths[0].packets);
  EXPECT_EQ(uint64_t(2), snapshot.rbrg.instances[0].paths[0].busy_cycles);
  EXPECT_EQ(uint64_t(1),
            snapshot.rbrg_path_stats(0, TmRingRbrgPath::V_TO_H_REQ).packets);
}

TEST(TmRingPmuTest, RbrgSnapshotPreservesRegistrationOrderAndIds) {
  TmRingPmu pmu;
  TmRingRbrgPmuPort first = pmu.register_rbrg(1);
  TmRingRbrgPmuPort second = pmu.register_rbrg(7);
  first.enqueued(TmRingRbrgPath::V_TO_H_REQ, 2);
  first.delivered(TmRingRbrgPath::V_TO_H_REQ, 16);
  second.enqueued(TmRingRbrgPath::H_TO_V_DAT, 3);
  second.delivered(TmRingRbrgPath::H_TO_V_DAT, 128);

  const TmRingPmuSnapshot snapshot = pmu.snapshot(3);
  ASSERT_EQ(size_t(2), snapshot.rbrg.instances.size());
  EXPECT_EQ(uint64_t(1), snapshot.rbrg.instances[0].paths[0].packets);
  EXPECT_EQ(uint64_t(1), snapshot.rbrg.instances[1].paths[3].packets);
  EXPECT_EQ(uint64_t(1),
            snapshot.rbrg_path_stats(1, TmRingRbrgPath::V_TO_H_REQ).packets);
  EXPECT_EQ(uint64_t(1),
            snapshot.rbrg_path_stats(7, TmRingRbrgPath::H_TO_V_DAT).packets);
}

TEST(TmRingPmuTest, SnapshotAggregatesCrossStationsByDomain) {
  TmRingPmu pmu;
  pmu.register_conn(TmRingDomainType::V_RING, 4, 0, TmRingPortDir::CW, 1,
                    TmRingPortDir::CCW);
  pmu.register_conn(TmRingDomainType::V_RING, 4, 1, TmRingPortDir::CCW, 0,
                    TmRingPortDir::CW);
  TmRingCrossStationPmuPort cross =
      pmu.register_cross_station(TmRingDomainType::V_RING, 4, 0);
  cross.packet_deflected(TmRingSubnet::DAT, nullptr, 3, false);
  cross.e_tag_set(TmRingSubnet::DAT);

  const TmRingPmuSnapshot snapshot = pmu.snapshot(3);
  ASSERT_EQ(size_t(1), snapshot.conn.domains.size());
  const TmRingDomainStats& domain = snapshot.conn.domains[0];
  EXPECT_EQ(uint32_t(1), domain.directed_edge_count);
  EXPECT_EQ(uint64_t(1), domain.cross_station.eject_queue_full_stalls);
  EXPECT_EQ(uint64_t(1), domain.cross_station.deflection[2].events);
  EXPECT_EQ(uint64_t(1), domain.cross_station.e_tag_sets);
}

TEST(TmRingPmuTest, EventsMapToAllBuckets) {
  TmRingPmu pmu;
  TmRingConnPmuPort conn = pmu.register_conn(
      TmRingDomainType::H_RING, 0, 0, TmRingPortDir::CW, 1,
      TmRingPortDir::CCW);
  conn.accepted(TmRingSubnet::REQ, 16, 1, 2);
  conn.rejected(TmRingSubnet::REQ,
                TmRingConnRejectReason::RETIRED_EVENT_PENDING);
  conn.rejected(TmRingSubnet::REQ, TmRingConnRejectReason::SERIALIZER_BUSY);
  conn.rejected(TmRingSubnet::REQ, TmRingConnRejectReason::PIPELINE_FULL);

  TmRingCrossStationPmuPort cross =
      pmu.register_cross_station(TmRingDomainType::H_RING, 0, 0);
  cross.transit_committed(TmRingSubnet::REQ, TmRingSlotKind::TAGGED_EMPTY);
  cross.packet_injected(TmRingSubnet::REQ);
  cross.packet_ejected(TmRingSubnet::REQ, nullptr, 9);
  cross.packet_deflected(TmRingSubnet::REQ, nullptr, 7, false);
  cross.packet_deflected(TmRingSubnet::REQ, nullptr, 7, true);
  cross.slot_pool_blocked(TmRingSubnet::REQ, TmRingPortDir::CW);
  cross.i_tag_set(TmRingSubnet::REQ);
  cross.i_tag_claimed(TmRingSubnet::REQ);
  cross.e_tag_set(TmRingSubnet::REQ);
  cross.e_tag_claimed(TmRingSubnet::REQ);

  TmRingRbrgPmuPort rbrg = pmu.register_rbrg(1);
  const TmRingRbrgPath paths[] = {TmRingRbrgPath::V_TO_H_REQ,
                                  TmRingRbrgPath::V_TO_H_DAT,
                                  TmRingRbrgPath::H_TO_V_RSP,
                                  TmRingRbrgPath::H_TO_V_DAT};
  for (TmRingRbrgPath path : paths) {
    rbrg.enqueued(path, 3);
    rbrg.queue_blocked(path);
    rbrg.destination_blocked(path);
    rbrg.delivered(path, 128);
  }

  TmRingHaPmuPort ha = pmu.register_home_agent(1, 1);
  const TmRingHaReadOutcome outcomes[] = {
      TmRingHaReadOutcome::STALL_WRITE_HAZARD,
      TmRingHaReadOutcome::STALL_AGGREGATION_CLOSED,
      TmRingHaReadOutcome::STALL_WAITER_FULL,
      TmRingHaReadOutcome::STALL_TABLE_FULL,
      TmRingHaReadOutcome::MERGED_PENDING,
      TmRingHaReadOutcome::MERGED_INFLIGHT,
      TmRingHaReadOutcome::MERGED_RESPONDING,
      TmRingHaReadOutcome::ACCEPTED_L2_HIT,
      TmRingHaReadOutcome::ACCEPTED_L2_MISS,
      TmRingHaReadOutcome::BYPASS};
  for (TmRingHaReadOutcome outcome : outcomes) {
    ha.read_admission(0, 64, outcome);
  }
  ha.backend_read_issued(128);
  ha.functional_read_completed();
  ha.private_l2_blocked();
  ha.completion_buffer_sample(256);
  ha.transaction_completed(99);
  ha.write_reservation_blocked();

  TmRingL2PmuPort l2 = pmu.register_l2(1);
  l2.response_admitted(TmRingL2AcceptStatus::REJECTED_BUFFER_FULL, 0, 0);
  l2.response_admitted(TmRingL2AcceptStatus::ACCEPTED_NEW_GROUP, 2, 3);
  l2.response_admitted(TmRingL2AcceptStatus::MERGED_GROUP, 3, 4);
  l2.response_admitted(TmRingL2AcceptStatus::ACCEPTED_UNICAST, 4, 5);
  l2.buffer_blocked();
  l2.issue_interval_blocked();
  l2.dat_inject_blocked();
  l2.carrier_injected(128, 1, TmRingFanoutMode::MULTICAST);
  l2.carrier_injected(256, 2, TmRingFanoutMode::MULTICAST);
  l2.carrier_injected(512, 2, TmRingFanoutMode::SCATTER);
  l2.carrier_injected(64, 1, TmRingFanoutMode::SCATTER);

  const TmRingPmuSnapshot snapshot = pmu.snapshot(10);
  const TmRingConnStats& req = snapshot.conn.total[0];
  EXPECT_EQ(uint64_t(1), req.packets);
  EXPECT_EQ(uint64_t(16), req.bytes);
  EXPECT_EQ(uint64_t(1), req.busy_cycles);
  EXPECT_EQ(uint64_t(3), req.send_reject_stall);
  EXPECT_EQ(uint64_t(1), req.serialization_busy_stall);
  EXPECT_EQ(uint64_t(1), snapshot.conn.total[0].pipeline_full_stall);
  ASSERT_EQ(size_t(1), snapshot.top_busy_conns(TmRingSubnet::REQ, 1).size());
  EXPECT_EQ(uint64_t(1),
            snapshot.top_busy_conns(TmRingSubnet::REQ, 1)[0].busy_cycles);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.tagged_empty_slots);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.e_tag_claims);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.transit_slots);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.injected_packets);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.ejected_packets);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.slot_pool_full_stalls);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.i_tag_sets);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.i_tag_claims);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.e_tag_sets);
  EXPECT_EQ(uint64_t(1), snapshot.cross_station.total.deflection[0].events);
  EXPECT_EQ(uint64_t(1),
            snapshot.cross_station.total.deflection[0]
                .fanout_recipient_retry_events);
  for (uint32_t i = 0; i < 4; ++i) {
    const TmRingRbrgPathStats& path = snapshot.rbrg.instances[0].paths[i];
    EXPECT_EQ(uint64_t(1), path.packets);
    EXPECT_EQ(uint64_t(128), path.bytes);
    EXPECT_EQ(uint64_t(3), path.busy_cycles);
    EXPECT_EQ(uint64_t(1), path.queue_occupancy_peak);
    EXPECT_EQ(uint64_t(1), path.queue_full_stalls);
    EXPECT_EQ(uint64_t(1), path.destination_inject_stalls);
  }
  EXPECT_EQ(uint64_t(5), snapshot.ha.total.rd_requests);
  EXPECT_EQ(uint64_t(320), snapshot.ha.total.useful_bytes);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.rd_merged_pending);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.rd_merged_inflight);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.rd_merged_responding);
  EXPECT_EQ(uint64_t(3), snapshot.ha.total.backend_read_saved);
  EXPECT_EQ(uint64_t(2), snapshot.ha.total.rd_entries_allocated);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.l2_hit_transactions);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.l2_miss_transactions);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.functional_reads);
  EXPECT_EQ(uint64_t(2), snapshot.ha.total.write_hazard_stall_cycles);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.aggregation_closed_stall_cycles);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.waiter_full_stall_cycles);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.table_full_stall_cycles);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.rd_backend_issued);
  EXPECT_EQ(uint64_t(128), snapshot.ha.total.backend_read_bytes);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.private_l2_full_stall_cycles);
  EXPECT_EQ(uint64_t(256), snapshot.ha.total.completion_buffer_bytes_peak);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.completed_transaction_waiters[64]);
  EXPECT_EQ(uint64_t(3), snapshot.l2.total.responses_accepted);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.buffer_full_stall_cycles);
  EXPECT_EQ(uint64_t(12), snapshot.l2.total.latency_wait_cycles);
  EXPECT_EQ(uint64_t(4), snapshot.l2.total.buffer_occupancy_peak);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.issue_interval_stall_cycles);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.dat_inject_full_stall_cycles);
  EXPECT_EQ(uint64_t(960), snapshot.l2.total.dat_bytes);
  EXPECT_EQ(uint64_t(6), snapshot.l2.total.h_carrier_recipients);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.injected_carrier_128b);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.injected_carrier_256b);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.injected_carrier_512b);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.injected_carrier_other);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.h_unicast_carriers);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.h_multicast_carriers);
  EXPECT_EQ(uint64_t(1), snapshot.l2.total.h_scatter_carriers);
}
