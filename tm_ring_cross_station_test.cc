#include <gtest/gtest.h>

#include "tm_ring_cross_station.h"

namespace {

using namespace tm_engine;

TEST(TmRingSlotPoolTest, CanAcquireDoesNotConsumeTheReservedSlot) {
  TmRingSlotPool slot_pool(1, 0);

  EXPECT_TRUE(slot_pool.can_acquire(TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_TRUE(slot_pool.can_acquire(TmRingSubnet::DAT, TmRingPortDir::CW));
  ASSERT_TRUE(slot_pool.try_acquire(TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_FALSE(slot_pool.can_acquire(TmRingSubnet::DAT,
                                     TmRingPortDir::CW));
  slot_pool.release(TmRingSubnet::DAT, TmRingPortDir::CW);
  EXPECT_TRUE(slot_pool.can_acquire(TmRingSubnet::DAT, TmRingPortDir::CW));
}

class TmRingCrossStationFixture {
 public:
  explicit TmRingCrossStationFixture(bool directional_destination = false) {
    tm_init();
    clk = tm_make_clk();
    slot_pool = tm_make_ring_slot_pool(2, 1);

    TmRingEndpointQueueDepths queue_depths;
    queue_depths.inject = {{1, 1, 1}};
    queue_depths.eject = {{1, 1, 1}};
    source_node = tm_make_ring_node_interface(
        clk, "deflection_source", queue_depths,
        pmu.register_endpoint_queues(TmRingNodeType::MASTER, 0,
                                     queue_depths));
    destination_node = directional_destination
                           ? tm_make_ring_node_interface(
                                 clk, "deflection_destination", queue_depths,
                                 pmu.register_endpoint_queues(
                                     TmRingNodeType::MASTER, 1, queue_depths),
                                 TmRingNodeInterfaceMode::RBRG_DIRECTIONAL)
                           : tm_make_ring_node_interface(
                                 clk, "deflection_destination", queue_depths,
                                 pmu.register_endpoint_queues(
                                     TmRingNodeType::MASTER, 1, queue_depths));

    source = tm_make_ring_cs(
        "deflection_source_station", clk,
        pmu.register_cross_station(TmRingDomainType::V_RING, 0, 0));
    destination = tm_make_ring_cs(
        "deflection_destination_station", clk,
        pmu.register_cross_station(TmRingDomainType::V_RING, 0, 1));

    cw_source_to_destination = tm_make_ring_conn(
        "deflection_cw_source_to_destination", clk, 1, 16, 1,
        TmRingPortDir::CCW,
        pmu.register_conn(TmRingDomainType::V_RING, 0, 0,
                          TmRingPortDir::CW, 1, TmRingPortDir::CCW));
    cw_destination_to_source = tm_make_ring_conn(
        "deflection_cw_destination_to_source", clk, 1, 16, 0,
        TmRingPortDir::CCW,
        pmu.register_conn(TmRingDomainType::V_RING, 0, 1,
                          TmRingPortDir::CW, 0, TmRingPortDir::CCW));
    ccw_source_to_destination = tm_make_ring_conn(
        "deflection_ccw_source_to_destination", clk, 1, 16, 1,
        TmRingPortDir::CW,
        pmu.register_conn(TmRingDomainType::V_RING, 0, 0,
                          TmRingPortDir::CCW, 1, TmRingPortDir::CW));
    ccw_destination_to_source = tm_make_ring_conn(
        "deflection_ccw_destination_to_source", clk, 1, 16, 0,
        TmRingPortDir::CW,
        pmu.register_conn(TmRingDomainType::V_RING, 0, 1,
                          TmRingPortDir::CCW, 0, TmRingPortDir::CW));

    source->attach(0, cw_source_to_destination, ccw_source_to_destination,
                   slot_pool);
    destination->attach(1, cw_destination_to_source,
                        ccw_destination_to_source, slot_pool);
    source->bind_node_interface(source_node);
    destination->bind_node_interface(destination_node);

    attach_to_destination(cw_source_to_destination, destination,
                          TmRingPortDir::CCW);
    attach_to_destination(cw_destination_to_source, source,
                          TmRingPortDir::CCW);
    attach_to_destination(ccw_source_to_destination, destination,
                          TmRingPortDir::CW);
    attach_to_destination(ccw_destination_to_source, source,
                          TmRingPortDir::CW);
  }

  p_tm_pld_t make_req(uint32_t destination_station) const {
    p_tm_pld_t packet = tm_make_pld(PldCmd::RD, 0, 16);
    packet->ring_subnet = static_cast<uint32_t>(TmRingSubnet::REQ);
    packet->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD);
    packet->ring_direction = static_cast<uint32_t>(TmRingPortDir::CW);
    tm_pld_set_ring_route(packet, static_cast<uint32_t>(PldCmd::RD), 0, 0,
                          destination_station);
    return packet;
  }

  p_tm_pld_t make_dat(uint32_t destination_station) const {
    p_tm_pld_t packet = tm_make_pld(PldCmd::RD_RSP, 0, 128);
    packet->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
    packet->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
    packet->ring_direction = static_cast<uint32_t>(TmRingPortDir::CW);
    tm_pld_set_ring_route(packet, static_cast<uint32_t>(PldCmd::RD), 0, 0,
                          destination_station);
    return packet;
  }

  void run_until_deflections(uint64_t count) const {
    for (uint32_t cycle = 0; cycle < 128; ++cycle) {
      const TmRingPmuSnapshot snapshot = pmu.snapshot(clk->time());
      if (snapshot.conn.domains[0]
              .cross_station.deflection[tm_ring_subnet_index(TmRingSubnet::REQ)]
              .events >= count) {
        return;
      }
      tm_start(1);
    }
  }

  bool run_until_eject() const {
    for (uint32_t cycle = 0; cycle < 128; ++cycle) {
      if (destination_node->front_eject(TmRingSubnet::REQ) != nullptr) {
        return true;
      }
      tm_start(1);
    }
    return false;
  }

  p_tm_clk_t clk;
  TmRingPmu pmu;
  p_tm_ring_slot_pool_t slot_pool;
  p_tm_ring_node_interface_t source_node;
  p_tm_ring_node_interface_t destination_node;
  p_tm_ring_cs_t source;
  p_tm_ring_cs_t destination;
  p_tm_ring_conn_t cw_source_to_destination;
  p_tm_ring_conn_t cw_destination_to_source;
  p_tm_ring_conn_t ccw_source_to_destination;
  p_tm_ring_conn_t ccw_destination_to_source;

 private:
  static void attach_to_destination(const p_tm_ring_conn_t& conn,
                                    const p_tm_ring_cs_t& station,
                                    TmRingPortDir direction) {
    conn->attach(station->transit_in_reg(direction, TmRingSubnet::REQ),
                 station->transit_in_reg(direction, TmRingSubnet::RSP),
                 station->transit_in_reg(direction, TmRingSubnet::DAT));
  }
};

TEST(TmRingCrossStationTest,
     RecordsTwoUnicastDeflectionsBeforeOneSuccessfulEject) {
  TmRingCrossStationFixture fixture;
  ASSERT_TRUE(fixture.destination_node->push_eject(
      TmRingSubnet::REQ, fixture.make_req(1)));

  p_tm_pld_t packet = fixture.make_req(1);
  const uint64_t gid = packet->gid;
  ASSERT_TRUE(fixture.source_node->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, packet));

  fixture.run_until_deflections(2);
  const TmRingPmuSnapshot before_release_snapshot =
      fixture.pmu.snapshot(fixture.clk->time());
  const TmRingDeflectionStats& before_release =
      before_release_snapshot.conn.domains[0]
          .cross_station.deflection[tm_ring_subnet_index(TmRingSubnet::REQ)];
  ASSERT_GE(before_release.events, uint64_t(2));

  fixture.destination_node->pop_eject(TmRingSubnet::REQ);
  ASSERT_TRUE(fixture.run_until_eject());
  ASSERT_EQ(gid, fixture.destination_node->front_eject(TmRingSubnet::REQ)->gid);
  fixture.destination_node->pop_eject(TmRingSubnet::REQ);
  tm_start(8);

  const TmRingPmuSnapshot after_eject_snapshot =
      fixture.pmu.snapshot(fixture.clk->time());
  const TmRingDeflectionStats& stats =
      after_eject_snapshot.conn.domains[0]
          .cross_station.deflection[tm_ring_subnet_index(TmRingSubnet::REQ)];
  EXPECT_EQ(uint64_t(2), stats.events);
  EXPECT_EQ(uint64_t(1), stats.unique_packets);
  EXPECT_EQ(uint64_t(1), stats.eligible_unicast_packets);
  EXPECT_EQ(uint64_t(1), stats.completed_packets);
  EXPECT_EQ(uint64_t(2), stats.rounds_sum);
  EXPECT_EQ(uint64_t(2), stats.rounds_max);
  EXPECT_GT(stats.delay_cycles_sum, uint64_t(0));
  EXPECT_GT(stats.delay_cycles_max, uint64_t(0));
  EXPECT_TRUE(fixture.destination_node->eject_q(TmRingSubnet::REQ)->empty());
  EXPECT_EQ(uint64_t(1),
            after_eject_snapshot.conn.domains[0].cross_station.ejected_packets);
}

TEST(TmRingCrossStationTest, TransitAndLocalHeadsShareOneOutputFairly) {
  TmRingCrossStationFixture fixture;
  ASSERT_TRUE(fixture.source_node->push_inject(
      TmRingSubnet::DAT, TmRingPortDir::CW, fixture.make_dat(1)));

  for (uint32_t cycle = 0; cycle < 64; ++cycle) {
    if (fixture.source_node->front_inject(TmRingSubnet::DAT,
                                          TmRingPortDir::CW) == nullptr) {
      ASSERT_TRUE(fixture.source_node->push_inject(
          TmRingSubnet::DAT, TmRingPortDir::CW, fixture.make_dat(1)));
    }
    p_tm_com_que_t transit = fixture.source->transit_in_reg(
        TmRingPortDir::CCW, TmRingSubnet::DAT);
    if (transit->empty() && fixture.slot_pool->can_acquire(
                                TmRingSubnet::DAT, TmRingPortDir::CW)) {
      ASSERT_TRUE(fixture.slot_pool->try_acquire(
          TmRingSubnet::DAT, TmRingPortDir::CW));
      transit->push_back(fixture.make_dat(1));
    }
    if (fixture.destination_node->front_eject(TmRingSubnet::DAT) != nullptr) {
      fixture.destination_node->pop_eject(TmRingSubnet::DAT);
    }
    tm_start(1);
  }

  const TmRingPmuSnapshot snapshot = fixture.pmu.snapshot(fixture.clk->time());
  const TmRingCrossStationStats& stats =
      snapshot.conn.domains[0].cross_station;
  EXPECT_GT(stats.transit_slots, uint64_t(1));
  EXPECT_GT(stats.injected_packets, uint64_t(1));
}

TEST(TmRingCrossStationTest, FailedLocalWinnerKeepsHeadAndPriority) {
  TmRingCrossStationFixture fixture;
  ASSERT_TRUE(fixture.slot_pool->try_acquire(
      TmRingSubnet::DAT, TmRingPortDir::CW));
  ASSERT_TRUE(fixture.cw_source_to_destination->accept_slot(
      fixture.make_dat(1)));

  ASSERT_TRUE(fixture.source_node->push_inject(
      TmRingSubnet::DAT, TmRingPortDir::CW, fixture.make_dat(1)));
  ASSERT_TRUE(fixture.slot_pool->try_acquire(
      TmRingSubnet::DAT, TmRingPortDir::CW));
  p_tm_com_que_t transit = fixture.source->transit_in_reg(
      TmRingPortDir::CCW, TmRingSubnet::DAT);
  transit->push_back(fixture.make_dat(1));

  tm_start(1);
  ASSERT_NE(nullptr, fixture.source_node->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
  ASSERT_FALSE(transit->empty());

  for (uint32_t cycle = 0; cycle < 32; ++cycle) {
    tm_start(1);
    if (fixture.source_node->front_inject(
            TmRingSubnet::DAT, TmRingPortDir::CW) == nullptr ||
        transit->empty()) {
      break;
    }
  }
  EXPECT_EQ(nullptr, fixture.source_node->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_FALSE(transit->empty());
}

TEST(TmRingCrossStationTest,
     DirectionalDestinationEjectsCwAndCcwInSameCycle) {
  TmRingCrossStationFixture fixture(true);
  const p_tm_pld_t cw_packet = fixture.make_req(1);
  const p_tm_pld_t ccw_packet = fixture.make_req(1);
  ccw_packet->ring_direction = static_cast<uint32_t>(TmRingPortDir::CCW);

  ASSERT_TRUE(fixture.slot_pool->try_acquire(TmRingSubnet::REQ,
                                             TmRingPortDir::CW));
  ASSERT_TRUE(fixture.slot_pool->try_acquire(TmRingSubnet::REQ,
                                             TmRingPortDir::CCW));
  fixture.destination
      ->transit_in_reg(TmRingPortDir::CCW, TmRingSubnet::REQ)
      ->push_back(cw_packet);
  fixture.destination
      ->transit_in_reg(TmRingPortDir::CW, TmRingSubnet::REQ)
      ->push_back(ccw_packet);

  tm_start(1);

  EXPECT_EQ(cw_packet, fixture.destination_node->front_eject(
                           TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(ccw_packet, fixture.destination_node->front_eject(
                            TmRingSubnet::REQ, TmRingPortDir::CCW));
}

TEST(TmRingCrossStationTest, FullCwDownFifoDoesNotBlockCcwEject) {
  TmRingCrossStationFixture fixture(true);
  const p_tm_pld_t cw_filler = fixture.make_req(1);
  ASSERT_TRUE(fixture.destination_node->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_filler));

  const p_tm_pld_t cw_packet = fixture.make_req(1);
  const p_tm_pld_t ccw_packet = fixture.make_req(1);
  ccw_packet->ring_direction = static_cast<uint32_t>(TmRingPortDir::CCW);
  ASSERT_TRUE(fixture.slot_pool->try_acquire(TmRingSubnet::REQ,
                                             TmRingPortDir::CW));
  ASSERT_TRUE(fixture.slot_pool->try_acquire(TmRingSubnet::REQ,
                                             TmRingPortDir::CCW));
  fixture.destination
      ->transit_in_reg(TmRingPortDir::CCW, TmRingSubnet::REQ)
      ->push_back(cw_packet);
  fixture.destination
      ->transit_in_reg(TmRingPortDir::CW, TmRingSubnet::REQ)
      ->push_back(ccw_packet);

  tm_start(1);

  EXPECT_EQ(cw_filler, fixture.destination_node->front_eject(
                           TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(ccw_packet, fixture.destination_node->front_eject(
                            TmRingSubnet::REQ, TmRingPortDir::CCW));
  const TmRingPmuSnapshot snapshot = fixture.pmu.snapshot(fixture.clk->time());
  EXPECT_GT(snapshot.conn.domains[0]
                .cross_station.eject_queue_full_stalls,
            uint64_t(0));
}

}  // namespace
