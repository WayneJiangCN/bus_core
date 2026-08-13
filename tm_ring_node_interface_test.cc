#include <gtest/gtest.h>

#include "tm_ring_node_interface.h"
#include "tm_ring_pmu.h"

namespace {

using namespace tm_engine;

p_tm_pld_t make_packet(PldCmd cmd) {
  return tm_make_pld(cmd, 0, 16);
}

p_tm_ring_node_interface_t make_node_interface(
    const p_tm_clk_t& clk, const std::string& name,
    const TmRingEndpointQueueDepths& depths, TmRingPmu* pmu) {
  const std::vector<TmRingQueuePmuPort> ports =
      pmu->register_endpoint_queues(TmRingNodeType::MASTER, 0, depths);
  return tm_make_ring_node_interface(clk, name, depths, ports);
}

TEST(TmRingNodeInterfaceTest, UsesIndependentSubnetDepths) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{1, 2, 3}};
  depths.eject = {{2, 3, 4}};
  TmRingPmu pmu;
  const p_tm_ring_node_interface_t node_interface =
      make_node_interface(clk, "independent_subnet_depths", depths, &pmu);

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

  const TmRingPmuSnapshot snapshot = pmu.snapshot(clk->time());
  ASSERT_EQ(size_t(9), snapshot.queue.endpoints.size());
  const TmRingQueueStats& dat_ccw_inject = snapshot.queue.endpoints[5].queue;
  EXPECT_EQ(TmRingSubnet::DAT, dat_ccw_inject.subnet);
  EXPECT_EQ(TmRingPortDir::CCW, dat_ccw_inject.direction);
  EXPECT_EQ(uint32_t(3), dat_ccw_inject.depth);
  EXPECT_EQ(uint64_t(3), dat_ccw_inject.counters.pushes);
  EXPECT_EQ(uint64_t(1), dat_ccw_inject.counters.push_rejects);
}

TEST(TmRingNodeInterfaceTest, AccumulatesOccupancyAndFullCyclesOnEvents) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{2, 2, 2}};
  depths.eject = {{2, 2, 2}};
  TmRingPmu pmu;
  const p_tm_ring_node_interface_t node_interface =
      make_node_interface(clk, "stats_test", depths, &pmu);

  // Existing Ring tests use tm_start(N) with N as the simulated cycle count.
  EXPECT_TRUE(node_interface->push_eject(TmRingSubnet::DAT,
                                         make_packet(PldCmd::WR_DAT)));
  tm_start(3);
  EXPECT_TRUE(node_interface->push_eject(TmRingSubnet::DAT,
                                         make_packet(PldCmd::WR_DAT)));
  tm_start(5);
  node_interface->pop_eject(TmRingSubnet::DAT);
  tm_start(2);

  const TmRingPmuSnapshot snapshot = pmu.snapshot(clk->time());
  ASSERT_EQ(size_t(9), snapshot.queue.endpoints.size());
  const TmRingQueueStats& dat_eject = snapshot.queue.endpoints[8].queue;
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
  TmRingPmu pmu;
  const p_tm_ring_node_interface_t node_interface =
      make_node_interface(clk, "reject_stats_test", depths, &pmu);

  EXPECT_TRUE(node_interface->push_inject(TmRingSubnet::REQ,
                                           TmRingPortDir::CW,
                                           make_packet(PldCmd::RD)));
  EXPECT_FALSE(node_interface->push_inject(TmRingSubnet::REQ,
                                            TmRingPortDir::CW,
                                            make_packet(PldCmd::RD)));

  const TmRingPmuSnapshot snapshot = pmu.snapshot(clk->time());
  ASSERT_EQ(size_t(9), snapshot.queue.endpoints.size());
  const TmRingQueueStats& req_cw_inject = snapshot.queue.endpoints[0].queue;
  EXPECT_EQ(uint64_t(1), req_cw_inject.counters.pushes);
  EXPECT_EQ(uint64_t(0), req_cw_inject.counters.pops);
  EXPECT_EQ(uint64_t(1), req_cw_inject.counters.push_rejects);
  EXPECT_EQ(uint32_t(1), req_cw_inject.occupancy);
  EXPECT_EQ(uint32_t(1), req_cw_inject.occupancy_peak);
}

TEST(TmRingNodeInterfaceTest, StartsNewSegmentOnlyAfterAcceptedInjection) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{1, 1, 1}};
  depths.eject = {{1, 1, 1}};
  TmRingPmu pmu;
  const std::vector<TmRingQueuePmuPort> ports =
      pmu.register_endpoint_queues(TmRingNodeType::MASTER, 0, depths);
  const p_tm_ring_node_interface_t node_interface =
      tm_make_ring_node_interface(clk, "serialization_segment_boundary",
                                  depths, ports);

  const p_tm_pld_t accepted = make_packet(PldCmd::RD);
  accepted->ring_segment_serialization_paid = true;
  ASSERT_TRUE(node_interface->push_inject(TmRingSubnet::REQ,
                                          TmRingPortDir::CW, accepted));
  EXPECT_FALSE(accepted->ring_segment_serialization_paid);

  const p_tm_pld_t rejected = make_packet(PldCmd::RD);
  rejected->ring_segment_serialization_paid = true;
  EXPECT_FALSE(node_interface->push_inject(TmRingSubnet::REQ,
                                            TmRingPortDir::CW, rejected));
  EXPECT_TRUE(rejected->ring_segment_serialization_paid);
}

TEST(TmRingNodeInterfaceTest, KeepsRbrgDirectionalEjectHeadsIndependent) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{2, 2, 2}};
  depths.eject = {{1, 1, 1}};
  TmRingPmu pmu;
  TmRingEndpointQueueDepths pmu_depths = depths;
  pmu_depths.eject = {{2, 2, 2}};
  const std::vector<TmRingQueuePmuPort> ports =
      pmu.register_endpoint_queues(TmRingNodeType::RBRG_V, 0, pmu_depths);
  const p_tm_ring_node_interface_t node_interface =
      tm_make_ring_node_interface(
          clk, "rbrg_directional_eject", depths, ports,
          TmRingNodeInterfaceMode::RBRG_DIRECTIONAL, 0);

  const p_tm_pld_t cw_packet = make_packet(PldCmd::RD);
  const p_tm_pld_t ccw_packet = make_packet(PldCmd::RD);
  ASSERT_TRUE(node_interface->push_eject(TmRingSubnet::REQ,
                                         TmRingPortDir::CW, cw_packet));
  ASSERT_TRUE(node_interface->push_eject(TmRingSubnet::REQ,
                                         TmRingPortDir::CCW, ccw_packet));
  EXPECT_EQ(cw_packet, node_interface->front_eject(TmRingSubnet::REQ,
                                                    TmRingPortDir::CW));
  EXPECT_EQ(ccw_packet, node_interface->front_eject(TmRingSubnet::REQ,
                                                     TmRingPortDir::CCW));
  EXPECT_FALSE(node_interface->has_eject_capacity(TmRingSubnet::REQ,
                                                   TmRingPortDir::CW));
  EXPECT_FALSE(node_interface->has_eject_capacity(TmRingSubnet::REQ,
                                                   TmRingPortDir::CCW));
  EXPECT_FALSE(node_interface->push_eject(TmRingSubnet::REQ,
                                           TmRingPortDir::CW,
                                           make_packet(PldCmd::RD)));
  const TmRingPmuSnapshot full_snapshot = pmu.snapshot(clk->time());
  const TmRingQueueStats& full_stats =
      full_snapshot.queue.endpoints[6].queue;
  EXPECT_EQ(uint32_t(2), full_stats.occupancy);
  EXPECT_EQ(uint32_t(2), full_stats.occupancy_peak);
  EXPECT_EQ(uint64_t(2), full_stats.counters.pushes);
  EXPECT_EQ(uint64_t(1), full_stats.counters.push_rejects);

  node_interface->pop_eject(TmRingSubnet::REQ, TmRingPortDir::CW);
  EXPECT_EQ(nullptr, node_interface->front_eject(TmRingSubnet::REQ,
                                                 TmRingPortDir::CW));
  EXPECT_EQ(ccw_packet, node_interface->front_eject(TmRingSubnet::REQ,
                                                     TmRingPortDir::CCW));
  EXPECT_TRUE(node_interface->has_eject_capacity(TmRingSubnet::REQ,
                                                  TmRingPortDir::CW));
  EXPECT_FALSE(node_interface->has_eject_capacity(TmRingSubnet::REQ,
                                                   TmRingPortDir::CCW));
  const TmRingPmuSnapshot popped_snapshot = pmu.snapshot(clk->time());
  const TmRingQueueStats& popped_stats =
      popped_snapshot.queue.endpoints[6].queue;
  EXPECT_EQ(uint32_t(1), popped_stats.occupancy);
  EXPECT_EQ(uint64_t(1), popped_stats.counters.pops);
}

TEST(TmRingNodeInterfaceTest,
     KeepsOrdinaryEndpointEjectQueueSharedAcrossDirections) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{2, 2, 2}};
  depths.eject = {{1, 1, 1}};
  TmRingPmu pmu;
  const std::vector<TmRingQueuePmuPort> ports =
      pmu.register_endpoint_queues(TmRingNodeType::MASTER, 0, depths);
  const p_tm_ring_node_interface_t node_interface =
      tm_make_ring_node_interface(clk, "ordinary_shared_eject", depths,
                                  ports);

  const p_tm_pld_t first = make_packet(PldCmd::RD);
  ASSERT_TRUE(node_interface->push_eject(TmRingSubnet::REQ,
                                         TmRingPortDir::CW, first));
  EXPECT_FALSE(node_interface->push_eject(TmRingSubnet::REQ,
                                          TmRingPortDir::CCW,
                                          make_packet(PldCmd::RD)));
  EXPECT_EQ(first, node_interface->front_eject(TmRingSubnet::REQ,
                                                TmRingPortDir::CW));
  EXPECT_EQ(first, node_interface->front_eject(TmRingSubnet::REQ,
                                                TmRingPortDir::CCW));
}

TEST(TmRingNodeInterfaceTest, DelaysRbrgSplitVisibilityUntilLatencyExpires) {
  tm_init();
  const p_tm_clk_t clk = tm_make_clk();
  TmRingEndpointQueueDepths depths;
  depths.inject = {{1, 1, 1}};
  depths.eject = {{1, 1, 1}};
  TmRingPmu pmu;
  const std::vector<TmRingQueuePmuPort> ports =
      pmu.register_endpoint_queues(TmRingNodeType::RBRG_V, 0, depths);
  const p_tm_ring_node_interface_t node_interface =
      tm_make_ring_node_interface(
          clk, "rbrg_split_latency", depths, ports,
          TmRingNodeInterfaceMode::RBRG_DIRECTIONAL, 2);

  const p_tm_pld_t packet = make_packet(PldCmd::RD);
  ASSERT_TRUE(node_interface->push_inject(TmRingSubnet::REQ,
                                          TmRingPortDir::CW, packet));
  EXPECT_EQ(nullptr, node_interface->front_inject(TmRingSubnet::REQ,
                                                   TmRingPortDir::CW));
  tm_start(1);
  EXPECT_EQ(nullptr, node_interface->front_inject(TmRingSubnet::REQ,
                                                   TmRingPortDir::CW));
  tm_start(1);
  EXPECT_EQ(packet, node_interface->front_inject(TmRingSubnet::REQ,
                                                  TmRingPortDir::CW));
}

}  // namespace
