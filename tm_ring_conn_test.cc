#include <gtest/gtest.h>

#include <vector>

#include "tm_ring_conn.h"
#include "tm_ring_pmu.h"

class TmRingConnTestAccess {
 public:
  static bool can_accept(const TmRingConn& conn, tm_engine::p_tm_pld_t slot) {
    TmRingConnRejectReason reason;
    return conn.can_accept(slot, &reason);
  }
};

namespace {

using namespace tm_engine;

class TmRingConnFixture {
 public:
  TmRingConnFixture(uint32_t conn_latency = 2,
                    uint32_t conn_width_bytes = 16,
                    uint32_t dat_capacity = 1)
      : conn_latency_(conn_latency), conn_width_bytes_(conn_width_bytes) {
    tm_init();
    clk = tm_make_clk();
    req = tm_make_com_que(clk, "conn_test_req", 1);
    rsp = tm_make_com_que(clk, "conn_test_rsp", 1);
    dat = tm_make_com_que(clk, "conn_test_dat", dat_capacity);
    pmu = std::make_shared<TmRingPmu>();
    conn = tm_make_ring_conn("conn_test", clk, latency(), width_bytes(), 1,
                             TmRingPortDir::CW,
                             pmu->register_conn(TmRingDomainType::V_RING, 0,
                                                0, TmRingPortDir::CW, 1,
                                                TmRingPortDir::CW));
    conn->attach(req, rsp, dat);
  }

  p_tm_pld_t make_dat(uint32_t bytes = 128) {
    auto slot = tm_make_pld(PldCmd::RD_RSP, 0, bytes);
    slot->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
    slot->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
    return slot;
  }

  p_tm_pld_t make_req() {
    auto slot = tm_make_pld(PldCmd::RD, 0, 16);
    slot->ring_subnet = static_cast<uint32_t>(TmRingSubnet::REQ);
    slot->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD);
    return slot;
  }

  uint32_t serialization_cycles(p_tm_pld_t slot) const {
    return tm_ring_serialization_cycles(tm_ring_packet_bytes(slot),
                                        width_bytes());
  }

  uint32_t arrival_cycles(p_tm_pld_t slot) const {
    return serialization_cycles(slot) + latency() - 1;
  }

 private:
  uint32_t latency() const { return conn_latency_; }
  uint32_t width_bytes() const { return conn_width_bytes_; }

  uint32_t conn_latency_;
  uint32_t conn_width_bytes_;

 public:

  p_tm_clk_t clk;
  p_tm_com_que_t req;
  p_tm_com_que_t rsp;
  p_tm_com_que_t dat;
  std::shared_ptr<TmRingPmu> pmu;
  p_tm_ring_conn_t conn;
};

void expect_conn_stats_eq(const TmRingConnStats& expected,
                          const TmRingConnStats& actual) {
  EXPECT_EQ(expected.packets, actual.packets);
  EXPECT_EQ(expected.bytes, actual.bytes);
  EXPECT_EQ(expected.busy_cycles, actual.busy_cycles);
  EXPECT_EQ(expected.downstream_register_full_stall,
            actual.downstream_register_full_stall);
  EXPECT_EQ(expected.serialization_busy_stall,
            actual.serialization_busy_stall);
  EXPECT_EQ(expected.pipeline_full_stall, actual.pipeline_full_stall);
  EXPECT_EQ(expected.send_reject_stall, actual.send_reject_stall);
  EXPECT_EQ(expected.inflight_peak, actual.inflight_peak);
}

TEST(TmRingConnTest, SerializerAndPacketCompletionKeepExistingTiming) {
  {
    TmRingConnFixture serializer_fixture;
    const auto first_packet = serializer_fixture.make_dat();
    const uint32_t serialization_cycles =
        serializer_fixture.serialization_cycles(first_packet);
    ASSERT_TRUE(serializer_fixture.conn->accept_slot(first_packet));

    tm_start(serialization_cycles - 1);
    ASSERT_FALSE(serializer_fixture.conn->accept_slot(
        serializer_fixture.make_dat()));
    tm_start(1);
    EXPECT_TRUE(serializer_fixture.conn->accept_slot(
        serializer_fixture.make_dat()));
  }

  TmRingConnFixture timing_fixture;
  const auto packet = timing_fixture.make_dat();
  const uint32_t arrival_cycles = timing_fixture.arrival_cycles(packet);
  ASSERT_TRUE(timing_fixture.conn->accept_slot(packet));

  tm_start(arrival_cycles - 1);
  ASSERT_TRUE(timing_fixture.dat->empty());
  tm_start(1);
  ASSERT_FALSE(timing_fixture.dat->empty());
  EXPECT_EQ(packet, timing_fixture.dat->front());
}

TEST(TmRingConnTest, PaidSegmentPacketOnlyWaitsForPropagation) {
  TmRingConnFixture fixture;
  const auto packet = fixture.make_dat();
  packet->ring_segment_serialization_paid = true;

  ASSERT_TRUE(fixture.conn->accept_slot(packet));
  EXPECT_TRUE(packet->ring_segment_serialization_paid);
  tm_start(1);
  EXPECT_TRUE(fixture.dat->empty());
  tm_start(1);
  ASSERT_FALSE(fixture.dat->empty());
  EXPECT_EQ(packet, fixture.dat->front());
}

TEST(TmRingConnTest, TwoHopDatPaysSerializationOncePerSegment) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  const p_tm_com_que_t b_req = tm_make_com_que(clk, "b_req", 1);
  const p_tm_com_que_t b_rsp = tm_make_com_que(clk, "b_rsp", 1);
  const p_tm_com_que_t b_dat = tm_make_com_que(clk, "b_dat", 1);
  const p_tm_com_que_t c_req = tm_make_com_que(clk, "c_req", 1);
  const p_tm_com_que_t c_rsp = tm_make_com_que(clk, "c_rsp", 1);
  const p_tm_com_que_t c_dat = tm_make_com_que(clk, "c_dat", 1);
  TmRingPmu pmu;
  const p_tm_ring_conn_t ab = tm_make_ring_conn(
      "ab", clk, 1, 128, 1, TmRingPortDir::CW,
      pmu.register_conn(TmRingDomainType::V_RING, 0, 0,
                        TmRingPortDir::CW, 1, TmRingPortDir::CW));
  const p_tm_ring_conn_t bc = tm_make_ring_conn(
      "bc", clk, 1, 128, 2, TmRingPortDir::CW,
      pmu.register_conn(TmRingDomainType::V_RING, 0, 1,
                        TmRingPortDir::CW, 2, TmRingPortDir::CW));
  ab->attach(b_req, b_rsp, b_dat);
  bc->attach(c_req, c_rsp, c_dat);

  const p_tm_pld_t packet = tm_make_pld(PldCmd::RD_RSP, 0, 512);
  packet->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
  packet->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
  ASSERT_TRUE(ab->accept_slot(packet));

  tm_start(4);
  ASSERT_FALSE(b_dat->empty());
  b_dat->pop_front();
  ASSERT_TRUE(bc->accept_slot(packet));
  tm_start(1);
  ASSERT_FALSE(c_dat->empty());
  EXPECT_EQ(packet, c_dat->front());
}

TEST(TmRingConnTest, BusyDatSerializerDoesNotBlockReq) {
  TmRingConnFixture fixture;
  ASSERT_TRUE(fixture.conn->accept_slot(fixture.make_dat()));
  EXPECT_TRUE(fixture.conn->accept_slot(fixture.make_req()));
}

TEST(TmRingPmuTest, ConnReadinessProbeDoesNotRecordRejectUntilAcceptFails) {
  TmRingConnFixture fixture;
  ASSERT_TRUE(fixture.conn->accept_slot(fixture.make_dat()));
  const auto candidate = fixture.make_dat();
  const TmRingPmuSnapshot before_probe =
      fixture.pmu->snapshot(fixture.clk->time());

  EXPECT_FALSE(TmRingConnTestAccess::can_accept(*fixture.conn, candidate));
  EXPECT_FALSE(TmRingConnTestAccess::can_accept(*fixture.conn, candidate));
  const TmRingPmuSnapshot after_probe =
      fixture.pmu->snapshot(fixture.clk->time());
  expect_conn_stats_eq(
      before_probe.conn.total[tm_ring_subnet_index(TmRingSubnet::DAT)],
      after_probe.conn.total[tm_ring_subnet_index(TmRingSubnet::DAT)]);

  EXPECT_FALSE(fixture.conn->accept_slot(candidate));
  const TmRingPmuSnapshot after_reject =
      fixture.pmu->snapshot(fixture.clk->time());
  const TmRingConnStats& dat =
      after_reject.conn.total[tm_ring_subnet_index(TmRingSubnet::DAT)];
  EXPECT_EQ(uint64_t(1), dat.send_reject_stall);
  EXPECT_EQ(uint64_t(1), dat.serialization_busy_stall);
}

TEST(TmRingConnTest, ReadySlotWaitsForDownstreamSpace) {
  TmRingConnFixture fixture;
  fixture.dat->push_back(fixture.make_dat());
  const auto packet = fixture.make_dat();
  const uint32_t arrival_cycles = fixture.arrival_cycles(packet);
  ASSERT_TRUE(fixture.conn->accept_slot(packet));

  tm_start(arrival_cycles);
  EXPECT_FALSE(fixture.conn->idle());
  EXPECT_GT(fixture.pmu->snapshot(fixture.clk->time())
                .conn.total[tm_ring_subnet_index(TmRingSubnet::DAT)]
                .downstream_register_full_stall,
            uint64_t(0));

  fixture.dat->pop_front();
  tm_start(1);
  EXPECT_TRUE(fixture.conn->idle());
  ASSERT_FALSE(fixture.dat->empty());
  EXPECT_EQ(packet, fixture.dat->front());
}

TEST(TmRingConnTest, ResetRetiresOldSerializationEvents) {
  TmRingConnFixture fixture;
  const auto old_packet = fixture.make_dat();
  const uint32_t serialization_cycles =
      fixture.serialization_cycles(old_packet);
  ASSERT_TRUE(fixture.conn->accept_slot(old_packet));

  tm_start(1);
  fixture.conn->reset();
  EXPECT_FALSE(fixture.conn->accept_slot(fixture.make_dat()));

  tm_start(serialization_cycles - 1);
  EXPECT_TRUE(fixture.dat->empty());

  const auto current_packet = fixture.make_dat();
  ASSERT_TRUE(fixture.conn->accept_slot(current_packet));
  tm_start(fixture.arrival_cycles(current_packet));
  ASSERT_FALSE(fixture.dat->empty());
  EXPECT_EQ(current_packet, fixture.dat->front());
}

TEST(TmRingConnTest, SameLanePacketsReachDestinationInFifoOrder) {
  const uint32_t packet_count = 3;
  TmRingConnFixture fixture(64, 16, packet_count);
  std::vector<p_tm_pld_t> packets;
  packets.push_back(fixture.make_dat(16));
  packets.push_back(fixture.make_dat(32));
  packets.push_back(fixture.make_dat(48));

  ASSERT_TRUE(fixture.conn->accept_slot(packets.front()));
  for (uint32_t index = 1; index < packet_count; ++index) {
    const uint32_t predecessor_serialization_cycles =
        fixture.serialization_cycles(packets[index - 1]);
    tm_start(predecessor_serialization_cycles);
    ASSERT_TRUE(fixture.conn->accept_slot(packets[index]));
  }

  tm_start(fixture.arrival_cycles(packets.back()));
  for (uint32_t index = 0; index < packet_count; ++index) {
    ASSERT_FALSE(fixture.dat->empty());
    EXPECT_EQ(packets[index], fixture.dat->front());
    fixture.dat->pop_front();
  }
  EXPECT_TRUE(fixture.dat->empty());
}

}  // namespace
