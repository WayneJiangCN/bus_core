#include <gtest/gtest.h>

#include <memory>

#include "tm_ring_fanout.h"
#include "tm_ring.h"

namespace {

using namespace tm_engine;

p_tm_ring_cfg_t make_rbrg_test_cfg() {
  const p_tm_ring_cfg_t cfg = tm_make_ring_cfg("rbrg_directional_test");
  cfg->num_masters = 2;
  cfg->max_aicore_per_vring = 8;
  cfg->targets.clear();
  for (uint32_t target = 0; target < 2; ++target) {
    cfg->targets.push_back(tm_make_ring_target_cfg(
        target, 2, true, tm_bus_interleave_type_t::LINEAR, 512, 128, 6, 0,
        128, 4));
  }
  return cfg;
}

class TmRingRbrgL1DirectionalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    tm_init();
    clk_ = tm_make_clk();
    topology_.reset(new TmRingTopology());
    topology_->config(make_rbrg_test_cfg());

    TmRingEndpointQueueDepths depths;
    depths.inject = {{2, 2, 2}};
    depths.eject = {{2, 2, 2}};
    TmRingEndpointQueueDepths pmu_depths = depths;
    pmu_depths.eject = {{4, 4, 4}};
    rbrg_ = tm_make_ring_rbrg_l1("rbrg_directional", clk_, 0, 2, 0, 16,
                                  pmu_.register_rbrg(0), depths, depths,
                                  pmu_.register_endpoint_queues(
                                      TmRingNodeType::RBRG_V, 0, pmu_depths),
                                  pmu_.register_endpoint_queues(
                                      TmRingNodeType::RBRG_H, 0, pmu_depths),
                                  topology_);
  }

  p_tm_pld_t make_req(uint32_t target, uint64_t gid) const {
    const p_tm_pld_t packet = tm_make_pld(PldCmd::RD, 0, 16);
    packet->gid = gid;
    packet->slv_id = target;
    packet->ring_subnet = static_cast<uint32_t>(TmRingSubnet::REQ);
    packet->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD);
    return packet;
  }

  p_tm_pld_t make_dat(uint32_t target, uint64_t gid,
                       uint32_t bytes = 32) const {
    const p_tm_pld_t packet = tm_make_pld(PldCmd::WR_DAT, 0, bytes);
    packet->gid = gid;
    packet->slv_id = target;
    packet->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
    packet->ring_traffic_class = static_cast<uint32_t>(PldCmd::WR_DAT);
    return packet;
  }

  p_tm_pld_t make_rsp(uint32_t master, uint64_t gid) const {
    const p_tm_pld_t packet = tm_make_pld(PldCmd::RSP, 0, 16);
    packet->gid = gid;
    packet->mst_id = master;
    packet->ring_subnet = static_cast<uint32_t>(TmRingSubnet::RSP);
    packet->ring_traffic_class = static_cast<uint32_t>(PldCmd::RSP);
    return packet;
  }

  p_tm_pld_t make_fanout_dat(uint64_t gid) const {
    const p_tm_pld_t packet = tm_make_pld(PldCmd::RD_RSP, 0, 32);
    packet->gid = gid;
    packet->ring_subnet = static_cast<uint32_t>(TmRingSubnet::DAT);
    packet->ring_traffic_class = static_cast<uint32_t>(PldCmd::RD_RSP);
    packet->ring_fanout = std::make_shared<TmRingFanoutState>();
    for (uint32_t master = 0; master < 2; ++master) {
      TmRingFanoutRecipient recipient;
      recipient.dst_ring_id = 0;
      recipient.dst_node = topology_->master_location(master).station_id;
      recipient.response_template = tm_make_pld(PldCmd::RD_RSP, 0, 32);
      recipient.response_template->mst_id = master;
      recipient.response_template->gid = gid + master;
      packet->ring_fanout->recipients.push_back(recipient);
    }
    return packet;
  }

  p_tm_clk_t clk_ = nullptr;
  TmRingPmu pmu_;
  std::shared_ptr<TmRingTopology> topology_ = nullptr;
  p_tm_ring_rbrg_l1_t rbrg_ = nullptr;
};

TEST_F(TmRingRbrgL1DirectionalTest,
       DifferentPreferredOutputsAdvanceInOneCycle) {
  const p_tm_pld_t cw = make_req(0, 1);
  const p_tm_pld_t ccw = make_req(1, 2);
  cw->ring_segment_serialization_paid = true;
  ccw->ring_segment_serialization_paid = true;
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw));

  tm_start(1);

  EXPECT_EQ(cw, rbrg_->h_node_interface()->front_inject(
                    TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(ccw, rbrg_->h_node_interface()->front_inject(
                     TmRingSubnet::REQ, TmRingPortDir::CCW));
  EXPECT_FALSE(cw->ring_segment_serialization_paid);
  EXPECT_FALSE(ccw->ring_segment_serialization_paid);
}

TEST_F(TmRingRbrgL1DirectionalTest, SamePreferredOutputUsesBothFreeSplits) {
  const p_tm_pld_t cw = make_req(0, 1);
  const p_tm_pld_t ccw = make_req(0, 2);
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw));

  tm_start(1);

  EXPECT_EQ(cw, rbrg_->h_node_interface()->front_inject(
                    TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(ccw, rbrg_->h_node_interface()->front_inject(
                     TmRingSubnet::REQ, TmRingPortDir::CCW));
  EXPECT_EQ(TmRingPortDir::CCW,
            static_cast<TmRingPortDir>(ccw->ring_direction));
}

TEST_F(TmRingRbrgL1DirectionalTest,
       FullPreferredSplitUsesAlternateWithoutDuplicatingPacket) {
  const p_tm_pld_t filler = make_req(0, 100);
  const p_tm_pld_t second_filler = make_req(0, 101);
  const p_tm_pld_t packet = make_req(0, 1);
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, second_filler));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, packet));

  tm_start(1);

  EXPECT_EQ(filler, rbrg_->h_node_interface()->front_inject(
                        TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(packet, rbrg_->h_node_interface()->front_inject(
                        TmRingSubnet::REQ, TmRingPortDir::CCW));
  EXPECT_EQ(TmRingPortDir::CCW,
            static_cast<TmRingPortDir>(packet->ring_direction));
}

TEST_F(TmRingRbrgL1DirectionalTest, BothFullKeepsHeadsAndPayloadUnchanged) {
  const p_tm_pld_t cw_filler = make_req(0, 100);
  const p_tm_pld_t cw_second_filler = make_req(0, 101);
  const p_tm_pld_t ccw_filler = make_req(0, 102);
  const p_tm_pld_t ccw_second_filler = make_req(0, 103);
  const p_tm_pld_t packet = make_req(0, 1);
  packet->mst_addr = 71;
  packet->slv_addr = 72;
  packet->ring_direction = static_cast<uint32_t>(TmRingPortDir::CCW);
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_second_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_second_filler));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, packet));

  tm_start(1);

  EXPECT_EQ(packet, rbrg_->v_node_interface()->front_eject(
                        TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(uint32_t(71), packet->mst_addr);
  EXPECT_EQ(uint32_t(72), packet->slv_addr);
  EXPECT_EQ(TmRingPortDir::CCW,
            static_cast<TmRingPortDir>(packet->ring_direction));
  EXPECT_EQ(cw_filler, rbrg_->h_node_interface()->front_inject(
                           TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(ccw_filler, rbrg_->h_node_interface()->front_inject(
                            TmRingSubnet::REQ, TmRingPortDir::CCW));
  const TmRingPmuSnapshot snapshot = pmu_.snapshot(clk_->time());
  EXPECT_GT(snapshot.rbrg_path_stats(0, TmRingRbrgPath::V_TO_H_REQ)
                .destination_inject_stalls,
            uint64_t(0));
}

TEST_F(TmRingRbrgL1DirectionalTest, SplitPopWakesBlockedPath) {
  const p_tm_pld_t cw_filler = make_req(0, 100);
  const p_tm_pld_t cw_second_filler = make_req(0, 101);
  const p_tm_pld_t ccw_filler = make_req(0, 102);
  const p_tm_pld_t ccw_second_filler = make_req(0, 103);
  const p_tm_pld_t packet = make_req(0, 1);
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_second_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_second_filler));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, packet));
  tm_start(1);
  ASSERT_EQ(packet, rbrg_->v_node_interface()->front_eject(
                        TmRingSubnet::REQ, TmRingPortDir::CW));

  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::REQ,
                                        TmRingPortDir::CCW);
  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::REQ,
                                        TmRingPortDir::CCW);
  tm_start(1);

  EXPECT_EQ(packet, rbrg_->h_node_interface()->front_inject(
                        TmRingSubnet::REQ, TmRingPortDir::CCW));
  EXPECT_EQ(nullptr, rbrg_->v_node_interface()->front_eject(
                         TmRingSubnet::REQ, TmRingPortDir::CW));
}

TEST_F(TmRingRbrgL1DirectionalTest, BlockedHeadCannotBeBypassed) {
  const p_tm_pld_t cw_filler = make_req(0, 100);
  const p_tm_pld_t cw_second_filler = make_req(0, 101);
  const p_tm_pld_t ccw_filler = make_req(0, 102);
  const p_tm_pld_t ccw_second_filler = make_req(0, 103);
  const p_tm_pld_t first = make_req(0, 1);
  const p_tm_pld_t second = make_req(0, 2);
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_second_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_second_filler));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, first));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, second));
  tm_start(1);

  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::REQ,
                                        TmRingPortDir::CCW);
  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::REQ,
                                        TmRingPortDir::CCW);
  tm_start(1);

  EXPECT_EQ(first, rbrg_->h_node_interface()->front_inject(
                       TmRingSubnet::REQ, TmRingPortDir::CCW));
  EXPECT_EQ(second, rbrg_->v_node_interface()->front_eject(
                        TmRingSubnet::REQ, TmRingPortDir::CW));
}

TEST_F(TmRingRbrgL1DirectionalTest,
       CwAndCcwSerializersOperateIndependently) {
  const p_tm_pld_t first_cw = make_dat(0, 1);
  const p_tm_pld_t first_ccw = make_dat(1, 2);
  const p_tm_pld_t second_cw = make_dat(0, 3);
  const p_tm_pld_t second_ccw = make_dat(1, 4);
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CW, first_cw));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CCW, first_ccw));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CW, second_cw));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CCW, second_ccw));

  tm_start(1);
  EXPECT_EQ(nullptr, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(nullptr, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CCW));
  tm_start(1);
  EXPECT_EQ(first_cw, rbrg_->h_node_interface()->front_inject(
                          TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(first_ccw, rbrg_->h_node_interface()->front_inject(
                           TmRingSubnet::DAT, TmRingPortDir::CCW));

  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::DAT,
                                        TmRingPortDir::CW);
  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::DAT,
                                        TmRingPortDir::CCW);
  tm_start(1);
  EXPECT_EQ(nullptr, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(nullptr, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CCW));
  tm_start(1);
  EXPECT_EQ(second_cw, rbrg_->h_node_interface()->front_inject(
                           TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(second_ccw, rbrg_->h_node_interface()->front_inject(
                            TmRingSubnet::DAT, TmRingPortDir::CCW));
}

TEST_F(TmRingRbrgL1DirectionalTest, BusyDatDoesNotBlockReqOrRsp) {
  const p_tm_pld_t dat = make_dat(0, 1, 64);
  const p_tm_pld_t rsp = make_rsp(0, 2);
  const p_tm_pld_t req = make_req(1, 3);
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CW, dat));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_eject(
      TmRingSubnet::RSP, TmRingPortDir::CW, rsp));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, req));

  tm_start(2);

  EXPECT_EQ(nullptr, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(req, rbrg_->h_node_interface()->front_inject(
                     TmRingSubnet::REQ, TmRingPortDir::CCW));
  EXPECT_EQ(rsp, rbrg_->v_node_interface()->front_inject(
                     TmRingSubnet::RSP, TmRingPortDir::CW));
}

TEST_F(TmRingRbrgL1DirectionalTest, ResetRetiresBothDirectionalEvents) {
  const p_tm_pld_t old_cw = make_dat(0, 1);
  const p_tm_pld_t old_ccw = make_dat(1, 2);
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CW, old_cw));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CCW, old_ccw));
  tm_start(1);

  rbrg_->reset();
  tm_start(4);

  EXPECT_EQ(nullptr, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(nullptr, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CCW));
  EXPECT_TRUE(rbrg_->idle());

  const p_tm_pld_t current = make_dat(0, 3);
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CW, current));
  tm_start(2);
  EXPECT_EQ(current, rbrg_->h_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
}

TEST_F(TmRingRbrgL1DirectionalTest,
       SameOutputContentionAdvancesRoundRobinOnlyAfterCommit) {
  const p_tm_pld_t cw_filler = make_req(0, 100);
  const p_tm_pld_t cw_second_filler = make_req(0, 101);
  const p_tm_pld_t ccw_filler = make_req(0, 102);
  const p_tm_pld_t ccw_second_filler = make_req(0, 103);
  const p_tm_pld_t cw = make_req(0, 1);
  const p_tm_pld_t ccw = make_req(0, 2);
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw_second_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw_second_filler));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CW, cw));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_eject(
      TmRingSubnet::REQ, TmRingPortDir::CCW, ccw));
  tm_start(1);

  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::REQ,
                                        TmRingPortDir::CW);
  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::REQ,
                                        TmRingPortDir::CW);
  tm_start(1);
  EXPECT_EQ(cw, rbrg_->h_node_interface()->front_inject(
                    TmRingSubnet::REQ, TmRingPortDir::CW));
  EXPECT_EQ(ccw, rbrg_->v_node_interface()->front_eject(
                     TmRingSubnet::REQ, TmRingPortDir::CCW));

  rbrg_->h_node_interface()->pop_inject(TmRingSubnet::REQ,
                                        TmRingPortDir::CW);
  tm_start(1);
  EXPECT_EQ(ccw, rbrg_->h_node_interface()->front_inject(
                     TmRingSubnet::REQ, TmRingPortDir::CW));
}

TEST_F(TmRingRbrgL1DirectionalTest,
       RbrgQueueDepthDefinesEachSplitCapacity) {
  TmRingEndpointQueueDepths depths;
  depths.inject = {{2, 2, 2}};
  depths.eject = {{2, 2, 2}};
  TmRingEndpointQueueDepths pmu_depths = depths;
  pmu_depths.inject = {{1, 1, 1}};
  pmu_depths.eject = {{4, 4, 4}};
  const p_tm_ring_rbrg_l1_t limited_rbrg = tm_make_ring_rbrg_l1(
      "rbrg_split_depth", clk_, 0, 1, 0, 16, pmu_.register_rbrg(1),
      depths, depths,
      pmu_.register_endpoint_queues(TmRingNodeType::RBRG_V, 1, pmu_depths),
      pmu_.register_endpoint_queues(TmRingNodeType::RBRG_H, 1, pmu_depths),
      topology_);

  EXPECT_TRUE(limited_rbrg->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, make_req(0, 1)));
  EXPECT_FALSE(limited_rbrg->h_node_interface()->push_inject(
      TmRingSubnet::REQ, TmRingPortDir::CW, make_req(0, 2)));
}

TEST_F(TmRingRbrgL1DirectionalTest,
       FanoutCarrierSpillsOnceAndKeepsItsAssignedDirection) {
  const p_tm_pld_t first_filler = make_dat(0, 100);
  const p_tm_pld_t second_filler = make_dat(0, 101);
  const p_tm_pld_t carrier = make_fanout_dat(1);
  ASSERT_TRUE(rbrg_->v_node_interface()->push_inject(
      TmRingSubnet::DAT, TmRingPortDir::CW, first_filler));
  ASSERT_TRUE(rbrg_->v_node_interface()->push_inject(
      TmRingSubnet::DAT, TmRingPortDir::CW, second_filler));
  ASSERT_TRUE(rbrg_->h_node_interface()->push_eject(
      TmRingSubnet::DAT, TmRingPortDir::CW, carrier));

  tm_start(2);

  EXPECT_EQ(first_filler, rbrg_->v_node_interface()->front_inject(
                             TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(carrier, rbrg_->v_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CCW));
  ASSERT_NE(nullptr, carrier->ring_fanout);
  EXPECT_EQ(size_t(2), carrier->ring_fanout->recipients.size());
  EXPECT_TRUE(carrier->ring_fanout->active_on_ring);
  EXPECT_EQ(TmRingPortDir::CCW,
            static_cast<TmRingPortDir>(carrier->ring_direction));

  rbrg_->v_node_interface()->pop_inject(TmRingSubnet::DAT,
                                        TmRingPortDir::CW);
  rbrg_->v_node_interface()->pop_inject(TmRingSubnet::DAT,
                                        TmRingPortDir::CW);
  tm_start(2);
  EXPECT_EQ(nullptr, rbrg_->v_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CW));
  EXPECT_EQ(carrier, rbrg_->v_node_interface()->front_inject(
                         TmRingSubnet::DAT, TmRingPortDir::CCW));
}

}  // namespace
