#include <gtest/gtest.h>

#include "tm_ring_node_interface.h"

namespace {

using namespace tm_engine;

p_tm_pld_t make_packet(PldCmd cmd) {
  return tm_make_pld(cmd, 0, 16);
}

TEST(TmRingNodeInterfaceTest, UsesIndependentSubnetDepths) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{1, 2, 3}};
  depths.eject = {{2, 3, 4}};
  const p_tm_ring_node_interface_t node_interface =
      tm_make_ring_node_interface(clk, "independent_subnet_depths", depths);

  EXPECT_TRUE(node_interface->push_inject(TmRingSubnet::REQ,
                                          TmRingPortDir::CW,
                                          make_packet(PldCmd::RD)));
  EXPECT_FALSE(node_interface->push_inject(TmRingSubnet::REQ,
                                           TmRingPortDir::CW,
                                           make_packet(PldCmd::RD)));

  for (uint32_t index = 0; index < 3; ++index) {
    EXPECT_TRUE(node_interface->push_inject(TmRingSubnet::DAT,
                                            TmRingPortDir::CCW,
                                            make_packet(PldCmd::WR_DAT)));
  }
  EXPECT_FALSE(node_interface->push_inject(TmRingSubnet::DAT,
                                            TmRingPortDir::CCW,
                                            make_packet(PldCmd::WR_DAT)));
}

TEST(TmRingNodeInterfaceTest, AccumulatesOccupancyAndFullCyclesOnEvents) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{2, 2, 2}};
  depths.eject = {{2, 2, 2}};
  const p_tm_ring_node_interface_t node_interface =
      tm_make_ring_node_interface(clk, "stats_test", depths);

  // Existing Ring tests use tm_start(N) with N as the simulated cycle count.
  EXPECT_TRUE(node_interface->push_eject(TmRingSubnet::DAT,
                                         make_packet(PldCmd::WR_DAT)));
  tm_start(3);
  EXPECT_TRUE(node_interface->push_eject(TmRingSubnet::DAT,
                                         make_packet(PldCmd::WR_DAT)));
  tm_start(5);
  node_interface->pop_eject(TmRingSubnet::DAT);
  tm_start(2);

  const std::vector<TmRingQueueStats> snapshots =
      node_interface->queue_stats(clk->time());
  ASSERT_EQ(size_t(9), snapshots.size());
  const TmRingQueueStats& dat_eject = snapshots[8];
  EXPECT_EQ(uint64_t(2), dat_eject.counters.pushes);
  EXPECT_EQ(uint64_t(1), dat_eject.counters.pops);
  EXPECT_EQ(uint32_t(2), dat_eject.occupancy_peak);
  EXPECT_EQ(uint64_t(15), dat_eject.counters.occupancy_area);
  EXPECT_EQ(uint64_t(5), dat_eject.counters.full_cycles);
}

TEST(TmRingNodeInterfaceTest, CountsInjectRejectWithoutChangingOccupancy) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{1, 2, 2}};
  depths.eject = {{2, 2, 2}};
  const p_tm_ring_node_interface_t node_interface =
      tm_make_ring_node_interface(clk, "reject_stats_test", depths);

  EXPECT_TRUE(node_interface->push_inject(TmRingSubnet::REQ,
                                           TmRingPortDir::CW,
                                           make_packet(PldCmd::RD)));
  EXPECT_FALSE(node_interface->push_inject(TmRingSubnet::REQ,
                                            TmRingPortDir::CW,
                                            make_packet(PldCmd::RD)));

  const std::vector<TmRingQueueStats> snapshots =
      node_interface->queue_stats(clk->time());
  ASSERT_EQ(size_t(9), snapshots.size());
  const TmRingQueueStats& req_cw_inject = snapshots[0];
  EXPECT_EQ(uint64_t(1), req_cw_inject.counters.pushes);
  EXPECT_EQ(uint64_t(0), req_cw_inject.counters.pops);
  EXPECT_EQ(uint64_t(1), req_cw_inject.counters.push_rejects);
  EXPECT_EQ(uint32_t(1), req_cw_inject.occupancy);
  EXPECT_EQ(uint32_t(1), req_cw_inject.occupancy_peak);
}

}  // namespace
