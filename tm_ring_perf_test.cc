#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "tm_ring.h"
#include "tm_ring_home_agent.h"
#include "tm_ring_perf.h"
#include "tm_ring_perf_master.h"
#include "tm_ring_perf_report.h"
#include "tm_ring_pmu.h"
#include "tm_ring_write_tracker.h"

namespace {

using namespace tm_engine;

constexpr uint32_t kMultiVringBenchmarkMasters = 8;
constexpr uint32_t kMultiVringBenchmarkMaxAicorePerVring = 4;
constexpr uint32_t kMultiVringBenchmarkLineBytes = 512;
constexpr uint32_t kMultiVringBenchmarkBeatBytes = 128;
constexpr uint32_t kMultiVringBenchmarkBYTES_PER_MASTER =  1024* 1024;// 1 MiB
void set_conn_stats(TmRingConnStats* stats, uint64_t packets,
                    uint64_t bytes, uint64_t busy_cycles,
                    uint64_t downstream_stalls, uint64_t serialization_stalls,
                    uint64_t pipeline_stalls, uint64_t send_reject_stalls) {
  stats->packets = packets;
  stats->bytes = bytes;
  stats->busy_cycles = busy_cycles;
  stats->downstream_register_full_stall = downstream_stalls;
  stats->serialization_busy_stall = serialization_stalls;
  stats->pipeline_full_stall = pipeline_stalls;
  stats->send_reject_stall = send_reject_stalls;
}

p_tm_ring_cfg_t make_perf_cfg(uint32_t masters, uint32_t targets) {
  auto cfg = tm_make_ring_cfg("perf_trace_test");
  cfg->num_masters = masters;
  cfg->targets.clear();
  for (uint32_t target = 0; target < targets; ++target) {
    cfg->targets.push_back(tm_make_ring_target_cfg(
        target, targets, true, tm_bus_interleave_type_t::LINEAR, 512, 128,
        6, 0, 32, 2));
  }
  return cfg;
}

TEST(TmRingWriteTrackerTest, HoldsDataUntilMatchingAddressArrives) {
  tm_init();
  TmRingWriteTracker tracker;

  p_tm_pld_t address = tm_make_pld(PldCmd::WR, 0x1000, 512);
  address->mst_id = 3;
  address->gid = 9;
  p_tm_pld_t data = tm_make_pld(address);
  data->cmd = PldCmd::WR_DAT;

  EXPECT_FALSE(tracker.has_matching_address(data));
  EXPECT_THROW(tracker.commit_data(data), std::logic_error);

  ASSERT_TRUE(tracker.accept_address(address));
  EXPECT_TRUE(tracker.has_matching_address(data));
  tracker.commit_data(data);
  EXPECT_TRUE(tracker.empty());
}

TEST(TmRingWriteTrackerTest, UsesMasterScopedKeys) {
  tm_init();
  TmRingWriteTracker tracker;

  p_tm_pld_t first = tm_make_pld(PldCmd::WR, 0x2000, 128);
  first->mst_id = 0;
  first->gid = 17;
  p_tm_pld_t different_master = tm_make_pld(PldCmd::WR, 0x2000, 128);
  different_master->mst_id = 1;
  different_master->gid = 17;

  EXPECT_TRUE(tracker.can_accept_address(first));
  ASSERT_TRUE(tracker.accept_address(first));
  EXPECT_TRUE(tracker.can_accept_address(different_master));
  ASSERT_TRUE(tracker.accept_address(different_master));
  p_tm_pld_t first_data = tm_make_pld(first);
  first_data->cmd = PldCmd::WR_DAT;
  EXPECT_TRUE(tracker.has_matching_address(first_data));
}

TEST(TmRingWriteTrackerTest, AdmitsAddressNeededByDataFifoHead) {
  tm_init();
  TmRingWriteTracker tracker;

  for (uint32_t gid = 0; gid < 2; ++gid) {
    p_tm_pld_t address = tm_make_pld(PldCmd::WR, 0x2000 + gid * 128, 128);
    address->mst_id = 0;
    address->gid = gid;
    ASSERT_TRUE(tracker.accept_address(address));
  }

  p_tm_pld_t blocked_address = tm_make_pld(PldCmd::WR, 0x3000, 128);
  blocked_address->mst_id = 1;
  blocked_address->gid = 2;
  p_tm_pld_t data_head = tm_make_pld(blocked_address);
  data_head->cmd = PldCmd::WR_DAT;

  ASSERT_FALSE(tracker.has_matching_address(data_head));
  EXPECT_TRUE(tracker.accept_address(blocked_address));
  EXPECT_TRUE(tracker.has_matching_address(data_head));
}

TEST(TmRingHomeAgentTest, SingleWaiterGroupAcceptsLaterSameLineWaiter) {
  tm_init();
  TmRingHomeAgentConfig cfg;
  cfg.line_size = 512;
  cfg.entry_limit = 4;
  cfg.waiters_per_entry = 4;
  cfg.hit_rate_pct = 100;
  TmRingPmu pmu;
  TmRingHomeAgent home_agent(cfg, pmu.register_home_agent(0, 8));

  p_tm_pld_t first = tm_make_pld(PldCmd::RD, 0x04000000, 128);
  first->mst_id = 0;
  first->gid = 1;
  ASSERT_EQ(TmHaAcceptResult::ACCEPTED, home_agent.accept_read(first));
  ASSERT_NE(nullptr, home_agent.front_functional_read());
  home_agent.commit_functional_read(PldRsp::OK);

  const TmRingL2ResponseCandidate first_candidate =
      home_agent.front_l2_response();
  ASSERT_NE(nullptr, first_candidate.response);
  ASSERT_TRUE(first_candidate.fanout_eligible);
  EXPECT_EQ(uint64_t(0), first_candidate.open_group_token);

  TmRingL2AcceptResult new_group;
  new_group.status = TmRingL2AcceptStatus::ACCEPTED_NEW_GROUP;
  new_group.group_token = 7;
  home_agent.commit_l2_response(new_group);
  EXPECT_FALSE(home_agent.idle());

  p_tm_pld_t second = tm_make_pld(PldCmd::RD, 0x04000080, 128);
  second->mst_id = 1;
  second->gid = 2;
  ASSERT_EQ(TmHaAcceptResult::MERGED, home_agent.accept_read(second));
  const TmRingL2ResponseCandidate second_candidate =
      home_agent.front_l2_response();
  ASSERT_NE(nullptr, second_candidate.response);
  ASSERT_TRUE(second_candidate.fanout_eligible);
  EXPECT_EQ(uint64_t(7), second_candidate.open_group_token);

  TmRingL2AcceptResult merged_group;
  merged_group.status = TmRingL2AcceptStatus::MERGED_GROUP;
  merged_group.group_token = 7;
  home_agent.commit_l2_response(merged_group);

  TmRingL2GroupSummary summary;
  summary.group_token = 7;
  summary.mode = TmRingFanoutMode::SCATTER;
  summary.recipient_count = 2;
  EXPECT_TRUE(home_agent.consume_l2_group_summary(summary));
  EXPECT_TRUE(home_agent.idle());

  const TmRingPmuSnapshot snapshot = pmu.snapshot(0);
  EXPECT_EQ(uint64_t(2), snapshot.ha.total.rd_requests);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.rd_merged_responding);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.backend_read_saved);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.functional_reads);
  EXPECT_EQ(uint64_t(1), snapshot.ha.total.completed_transaction_waiters[2]);
}

TEST(TmRingHomeAgentTest, CrossLineReadBypassesProvisionalGroup) {
  tm_init();
  TmRingHomeAgentConfig cfg;
  cfg.line_size = 512;
  cfg.entry_limit = 4;
  cfg.waiters_per_entry = 4;
  cfg.hit_rate_pct = 100;
  TmRingPmu pmu;
  TmRingHomeAgent home_agent(cfg, pmu.register_home_agent(0, 8));

  p_tm_pld_t request = tm_make_pld(PldCmd::RD, 0x04000000, 1024);
  request->mst_id = 0;
  request->gid = 1;
  EXPECT_EQ(TmHaAcceptResult::BYPASS, home_agent.accept_read(request));
  EXPECT_TRUE(home_agent.idle());
  EXPECT_EQ(uint64_t(0), pmu.snapshot(0).ha.total.rd_requests);
}

TEST(TmRingPerfTraceTest, BuildsPrivateAndSharedFiniteReadTraces) {
  tm_init();
  TmRingTopology topology;
  auto cfg = make_perf_cfg(4, 2);
  cfg->ring_link_width_bytes = 128;
  topology.config(cfg);

  TmRingPerfCase private_case;
  private_case.active_masters = 4;
  private_case.bytes_per_master = 4 * 1024;
  private_case.burst_len = 1;
  private_case.pattern = TmRingPerfPattern::SEQUENTIAL_PRIVATE;

  const std::vector<TmRingPerfTxn> private_trace =
      tm_ring_build_perf_trace(private_case, 4, topology,
                               cfg->ring_link_width_bytes);
  EXPECT_EQ(size_t(128), private_trace.size());
  std::set<uint64_t> private_addresses;
  for (const auto& txn : private_trace) {
    EXPECT_EQ(PldCmd::RD, txn.cmd);
    EXPECT_EQ(uint32_t(128), txn.size);
    private_addresses.insert(txn.addr);
  }
  EXPECT_EQ(private_trace.size(), private_addresses.size());

  TmRingPerfCase shared_case = private_case;
  shared_case.active_masters = 2;
  shared_case.pattern = TmRingPerfPattern::SEQUENTIAL_SHARED;
  const std::vector<TmRingPerfTxn> shared_trace =
      tm_ring_build_perf_trace(shared_case, 4, topology,
                               cfg->ring_link_width_bytes);
  EXPECT_EQ(size_t(64), shared_trace.size());
  for (size_t ordinal = 0; ordinal < 32; ++ordinal) {
    EXPECT_EQ(shared_trace[ordinal].addr,
              shared_trace[ordinal + 32].addr);
  }
}

TEST(TmRingPerfTraceTest, BuildsSameLineScatterReadTraces) {
  tm_init();
  TmRingTopology topology;
  auto cfg = make_perf_cfg(5, 2);
  cfg->ring_link_width_bytes = 128;
  topology.config(cfg);

  const uint32_t burst_lengths[] = {1, 2};
  for (const uint32_t burst_len : burst_lengths) {
    const uint32_t request_bytes =
        burst_len * cfg->ring_link_width_bytes;
    TmRingPerfCase perf_case;
    perf_case.active_masters = 5;
    perf_case.bytes_per_master = 2 * request_bytes;
    perf_case.burst_len = burst_len;
    perf_case.pattern = TmRingPerfPattern::SAME_LINE_SCATTER;
    perf_case.read_base = 64ull * 1024 * 1024;

    const std::vector<TmRingPerfTxn> trace =
        tm_ring_build_perf_trace(perf_case, 5, topology,
                                 cfg->ring_link_width_bytes);
    ASSERT_EQ(size_t(10), trace.size());
    const uint32_t requests_per_line = 512 / request_bytes;
    const uint32_t lines_per_wave =
        (perf_case.active_masters + requests_per_line - 1) /
        requests_per_line;
    for (uint32_t master = 0; master < perf_case.active_masters; ++master) {
      const uint32_t line_group = master / requests_per_line;
      const uint32_t slot = master % requests_per_line;
      for (uint64_t wave = 0; wave < 2; ++wave) {
        const TmRingPerfTxn& txn = trace[master * 2 + wave];
        const uint64_t expected_addr =
            perf_case.read_base +
            (wave * lines_per_wave + line_group) * 512 +
            slot * request_bytes;
        EXPECT_EQ(master, txn.master_port);
        EXPECT_EQ(PldCmd::RD, txn.cmd);
        EXPECT_EQ(request_bytes, txn.size);
        EXPECT_EQ(expected_addr, txn.addr);
      }
    }
  }
}

TEST(TmRingPerfTraceTest, SharedReadsUseConfiguredLineStride) {
  tm_init();
  TmRingTopology topology;
  auto cfg = make_perf_cfg(3, 2);
  cfg->ring_link_width_bytes = 128;
  topology.config(cfg);

  const uint32_t burst_lengths[] = {1, 2, 4};
  for (const uint32_t burst_len : burst_lengths) {
    const uint32_t request_bytes =
        burst_len * cfg->ring_link_width_bytes;
    TmRingPerfCase perf_case;
    perf_case.active_masters = 3;
    perf_case.bytes_per_master = 2 * request_bytes;
    perf_case.burst_len = burst_len;
    perf_case.pattern = TmRingPerfPattern::SEQUENTIAL_SHARED;
    perf_case.read_base = 64ull * 1024 * 1024;
    perf_case.stride_bytes = 512;

    const std::vector<TmRingPerfTxn> trace =
        tm_ring_build_perf_trace(perf_case, 3, topology,
                                 cfg->ring_link_width_bytes);
    ASSERT_EQ(size_t(6), trace.size());
    for (uint32_t master = 0; master < perf_case.active_masters; ++master) {
      for (uint64_t wave = 0; wave < 2; ++wave) {
        const TmRingPerfTxn& txn = trace[master * 2 + wave];
        EXPECT_EQ(master, txn.master_port);
        EXPECT_EQ(request_bytes, txn.size);
        EXPECT_EQ(perf_case.read_base + wave * 512, txn.addr);
      }
    }
  }
}

TEST(TmRingPerfTraceTest, KeepsCrossLineSharedReadAsOne1024ByteRequest) {
  tm_init();
  TmRingTopology topology;
  auto cfg = make_perf_cfg(2, 2);
  cfg->ring_link_width_bytes = 128;
  topology.config(cfg);

  TmRingPerfCase perf_case;
  perf_case.active_masters = 2;
  perf_case.bytes_per_master = 2 * 1024;
  perf_case.burst_len = 8;
  perf_case.pattern = TmRingPerfPattern::SEQUENTIAL_SHARED;
  perf_case.read_base = 64ull * 1024 * 1024;
  perf_case.stride_bytes = 1024;

  const std::vector<TmRingPerfTxn> trace =
      tm_ring_build_perf_trace(perf_case, 2, topology,
                               cfg->ring_link_width_bytes);
  ASSERT_EQ(size_t(4), trace.size());
  for (uint32_t master = 0; master < perf_case.active_masters; ++master) {
    for (uint64_t wave = 0; wave < 2; ++wave) {
      const TmRingPerfTxn& txn = trace[master * 2 + wave];
      EXPECT_EQ(uint32_t(1024), txn.size);
      EXPECT_EQ(perf_case.read_base + wave * 1024, txn.addr);
      EXPECT_GT(txn.addr % 512 + txn.size, uint64_t(512));
    }
  }
}

TEST(TmRingPerfWaveCoordinatorTest, AdvancesOnlyAfterEveryMasterCompletes) {
  TmRingPerfWaveCoordinator coordinator(3);

  EXPECT_EQ(uint64_t(0), coordinator.current_wave());
  EXPECT_TRUE(coordinator.can_issue(0, 0));
  EXPECT_FALSE(coordinator.can_issue(0, 1));
  coordinator.record_completion(0, 0);
  coordinator.record_completion(2, 0);
  EXPECT_EQ(uint64_t(0), coordinator.current_wave());
  coordinator.record_completion(1, 0);
  EXPECT_EQ(uint64_t(1), coordinator.current_wave());
  EXPECT_TRUE(coordinator.can_issue(2, 1));
  EXPECT_THROW(coordinator.can_issue(0, 0), std::logic_error);
}

TEST(TmRingPerfWaveCoordinatorTest, RejectsInvalidAndDuplicateCompletions) {
  EXPECT_THROW(TmRingPerfWaveCoordinator(0), std::invalid_argument);
  TmRingPerfWaveCoordinator coordinator(2);

  EXPECT_THROW(coordinator.can_issue(2, 0), std::out_of_range);
  EXPECT_THROW(coordinator.record_completion(2, 0), std::out_of_range);
  EXPECT_THROW(coordinator.record_completion(0, 1), std::logic_error);
  coordinator.record_completion(0, 0);
  EXPECT_THROW(coordinator.record_completion(0, 0), std::logic_error);
  coordinator.record_completion(1, 0);
  EXPECT_THROW(coordinator.record_completion(1, 0), std::logic_error);
}

TEST(TmRingPerfWaveCoordinatorTest, MasterRejectsInvalidWaveTraces) {
  tm_init();
  auto clk = tm_make_clk();
  auto coordinator = std::make_shared<TmRingPerfWaveCoordinator>(1);

  std::vector<TmRingPerfTxn> write_trace(1);
  write_trace[0].cmd = PldCmd::WR;
  write_trace[0].size = 128;
  TmRingPerfMaster write_master;
  EXPECT_THROW(write_master.config("wave_write_master", clk, 0, write_trace,
                                   coordinator),
               std::invalid_argument);

  std::vector<TmRingPerfTxn> skipped_wave_trace(1);
  skipped_wave_trace[0].cmd = PldCmd::RD;
  skipped_wave_trace[0].size = 128;
  skipped_wave_trace[0].ordinal = 1;
  TmRingPerfMaster skipped_wave_master;
  EXPECT_THROW(skipped_wave_master.config("skipped_wave_master", clk, 0,
                                          skipped_wave_trace, coordinator),
               std::invalid_argument);
}

TEST(TmRingPerfEstimatorTest, CountsExplicitDomainEdgesAndRbrgPaths) {
  tm_init();
  auto cfg = make_perf_cfg(4, 1);
  cfg->max_aicore_per_vring = 2;
  TmRingTopology topology;
  topology.config(cfg);

  std::vector<TmRingPerfTxn> trace;
  TmRingPerfTxn first;
  first.master_port = 0;
  first.cmd = PldCmd::RD;
  first.addr = 0;
  first.size = 16;
  trace.push_back(first);
  TmRingPerfTxn second;
  second.master_port = 2;
  second.cmd = PldCmd::RD;
  second.addr = 129;
  second.size = 129;
  trace.push_back(second);

  cfg->ring_link_width_bytes = 128;
  const TmRingPerfEstimate estimate =
      tm_ring_estimate_fabric(trace, topology, *cfg);
  const TmRingPerfEdgeKey v0_req_edge(
      TmRingDomainType::V_RING, 0, TmRingSubnet::REQ, 1,
      TmRingPortDir::CCW);
  const TmRingPerfEdgeKey v1_req_edge(
      TmRingDomainType::V_RING, 1, TmRingSubnet::REQ, 1,
      TmRingPortDir::CCW);
  const TmRingPerfEdgeKey h0_req_edge(
      TmRingDomainType::H_RING, 0, TmRingSubnet::REQ, 0,
      TmRingPortDir::CW);
  const TmRingPerfEdgeKey h1_req_edge(
      TmRingDomainType::H_RING, 0, TmRingSubnet::REQ, 2,
      TmRingPortDir::CCW);
  const TmRingPerfEdgeKey h0_dat_edge(
      TmRingDomainType::H_RING, 0, TmRingSubnet::DAT, 3,
      TmRingPortDir::CW);
  const TmRingPerfEdgeKey h1_dat_edge(
      TmRingDomainType::H_RING, 0, TmRingSubnet::DAT, 3,
      TmRingPortDir::CCW);
  const TmRingPerfRbrgKey v0_req_path(0, TmRingRbrgPath::V_TO_H_REQ);
  const TmRingPerfRbrgKey v1_req_path(1, TmRingRbrgPath::V_TO_H_REQ);
  const TmRingPerfRbrgKey v0_dat_path(0, TmRingRbrgPath::H_TO_V_DAT);
  const TmRingPerfRbrgKey v1_dat_path(1, TmRingRbrgPath::H_TO_V_DAT);

  EXPECT_EQ(uint64_t(4), estimate.physical_packets);
  EXPECT_EQ(uint64_t(145), estimate.total_useful_bytes);
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(v0_req_edge));
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(v1_req_edge));
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(h0_req_edge));
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(h1_req_edge));
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(h0_dat_edge));
  EXPECT_EQ(uint64_t(2), estimate.edge_cycles.at(h1_dat_edge));
  EXPECT_EQ(uint64_t(1), estimate.rbrg_path_cycles.at(v0_req_path));
  EXPECT_EQ(uint64_t(1), estimate.rbrg_path_cycles.at(v1_req_path));
  EXPECT_EQ(uint64_t(1), estimate.rbrg_path_cycles.at(v0_dat_path));
  EXPECT_EQ(uint64_t(2), estimate.rbrg_path_cycles.at(v1_dat_path));
  EXPECT_EQ(uint64_t(2), estimate.fabric_min_cycles);
}

TEST(TmRingPerfEstimatorTest,
     AxiWriteUsesOneRequestOneDataAndOneCompletion) {
  tm_init();
  auto cfg = make_perf_cfg(1, 1);
  cfg->ring_link_width_bytes = 128;
  cfg->rbrg_width_bytes = 128;
  TmRingTopology topology;
  topology.config(cfg);

  TmRingPerfTxn write;
  write.master_port = 0;
  write.cmd = PldCmd::WR;
  write.addr = 0x1000;
  write.size = 512;

  const TmRingPerfEstimate estimate =
      tm_ring_estimate_fabric({write}, topology, *cfg);
  EXPECT_EQ(uint64_t(3), estimate.physical_packets);
}

TEST(TmRingPerfEstimatorTest, SharedSectorReadsAlternateEqualFanoutSpans) {
  tm_init();
  auto cfg = make_perf_cfg(3, 1);
  TmRingTopology topology;
  topology.config(cfg);

  cfg->ring_link_width_bytes = 128;
  const uint32_t carrier_sizes[] = {128, 256, 512};
  for (const uint32_t carrier_size : carrier_sizes) {
    std::vector<TmRingPerfTxn> trace;
    for (uint32_t line = 0; line < 2; ++line) {
      for (uint32_t master = 0; master < 3; ++master) {
        TmRingPerfTxn txn;
        txn.master_port = master;
        txn.cmd = PldCmd::RD;
        txn.addr = line * 512;
        txn.size = carrier_size;
        txn.ordinal = line;
        trace.push_back(txn);
      }
    }

    const TmRingPerfEstimate estimate =
        tm_ring_estimate_fabric(trace, topology, *cfg);
    const uint64_t packet_cycles = carrier_size / 128;
    const TmRingPerfEdgeKey cw_edge(
        TmRingDomainType::V_RING, 0, TmRingSubnet::DAT, 0,
        TmRingPortDir::CW);
    const TmRingPerfEdgeKey ccw_edge(
        TmRingDomainType::V_RING, 0, TmRingSubnet::DAT, 0,
        TmRingPortDir::CCW);
    const TmRingPerfRbrgKey dat_path(0, TmRingRbrgPath::H_TO_V_DAT);
    EXPECT_EQ(uint64_t(8), estimate.physical_packets);
    EXPECT_EQ(uint64_t(6) * carrier_size, estimate.total_useful_bytes);
    EXPECT_EQ(uint64_t(2), estimate.v_carriers);
    EXPECT_EQ(packet_cycles, estimate.edge_cycles.at(cw_edge));
    EXPECT_EQ(packet_cycles, estimate.edge_cycles.at(ccw_edge));
    EXPECT_EQ(uint64_t(2) * packet_cycles,
              estimate.rbrg_path_cycles.at(dat_path));
    EXPECT_EQ(std::max(uint64_t(3), packet_cycles),
              estimate.hottest_rbrg_path_cycles);
  }
}

TEST(TmRingPerfTraceTest, ConvertsBurstLengthToRequestBytes) {
  tm_init();
  TmRingTopology topology;
  auto cfg = make_perf_cfg(1, 1);
  cfg->ring_link_width_bytes = 128;
  topology.config(cfg);

  TmRingPerfCase perf_case;
  perf_case.active_masters = 1;
  perf_case.bytes_per_master = 1024;
  perf_case.burst_len = 4;

  const std::vector<TmRingPerfTxn> trace = tm_ring_build_perf_trace(
      perf_case, 1, topology, cfg->ring_link_width_bytes);
  ASSERT_EQ(size_t(2), trace.size());
  EXPECT_EQ(uint32_t(512), trace.front().size);
}

TEST(TmRingPerfTraceTest, RejectsInvalidBurstGeometry) {
  tm_init();
  TmRingTopology topology;
  auto cfg = make_perf_cfg(1, 1);
  cfg->ring_link_width_bytes = 128;
  topology.config(cfg);

  TmRingPerfCase perf_case;
  perf_case.active_masters = 1;
  perf_case.bytes_per_master = 1024;
  perf_case.burst_len = 0;
  EXPECT_THROW(tm_ring_build_perf_trace(
                   perf_case, 1, topology, cfg->ring_link_width_bytes),
               std::invalid_argument);

  perf_case.burst_len = std::numeric_limits<uint32_t>::max();
  EXPECT_THROW(tm_ring_build_perf_trace(
                   perf_case, 1, topology, cfg->ring_link_width_bytes),
               std::invalid_argument);
}

TEST(TmRingPerfEstimatorTest, SharedSectorReadsSplitCarriersByVRing) {
  tm_init();
  auto cfg = make_perf_cfg(4, 1);
  cfg->max_aicore_per_vring = 2;
  cfg->ring_link_width_bytes = 128;
  cfg->rbrg_width_bytes = 128;
  TmRingTopology topology;
  topology.config(cfg);

  std::vector<TmRingPerfTxn> trace;
  for (uint32_t master = 0; master < 4; ++master) {
    TmRingPerfTxn txn;
    txn.master_port = master;
    txn.cmd = PldCmd::RD;
    txn.addr = 0;
    txn.size = 128;
    trace.push_back(txn);
  }

  const TmRingPerfEstimate estimate =
      tm_ring_estimate_fabric(trace, topology, *cfg);
  const TmRingPerfEdgeKey h0_dat_edge(
      TmRingDomainType::H_RING, 0, TmRingSubnet::DAT, 3,
      TmRingPortDir::CW);
  const TmRingPerfEdgeKey h1_dat_edge(
      TmRingDomainType::H_RING, 0, TmRingSubnet::DAT, 3,
      TmRingPortDir::CCW);
  const TmRingPerfEdgeKey v0_dat_edge(
      TmRingDomainType::V_RING, 0, TmRingSubnet::DAT, 0,
      TmRingPortDir::CW);
  const TmRingPerfEdgeKey v1_dat_edge(
      TmRingDomainType::V_RING, 1, TmRingSubnet::DAT, 0,
      TmRingPortDir::CW);
  const TmRingPerfRbrgKey v0_dat_path(0, TmRingRbrgPath::H_TO_V_DAT);
  const TmRingPerfRbrgKey v1_dat_path(1, TmRingRbrgPath::H_TO_V_DAT);

  EXPECT_EQ(uint64_t(6), estimate.physical_packets);
  EXPECT_EQ(uint64_t(512), estimate.total_useful_bytes);
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(h0_dat_edge));
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(h1_dat_edge));
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(v0_dat_edge));
  EXPECT_EQ(uint64_t(1), estimate.edge_cycles.at(v1_dat_edge));
  EXPECT_EQ(uint64_t(1), estimate.rbrg_path_cycles.at(v0_dat_path));
  EXPECT_EQ(uint64_t(1), estimate.rbrg_path_cycles.at(v1_dat_path));
}

TEST(TmRingPerfEstimatorTest, SeparatesNoMergeAndIdealAggregationModels) {
  tm_init();
  auto cfg = make_perf_cfg(4, 1);
  cfg->max_aicore_per_vring = 2;
  cfg->ring_link_width_bytes = 128;
  cfg->rbrg_width_bytes = 128;
  TmRingTopology topology;
  topology.config(cfg);

  std::vector<TmRingPerfTxn> shared_trace;
  for (uint32_t master = 0; master < 4; ++master) {
    TmRingPerfTxn txn;
    txn.master_port = master;
    txn.cmd = PldCmd::RD;
    txn.addr = 0;
    txn.size = 128;
    txn.ordinal = 0;
    shared_trace.push_back(txn);
  }

  const TmRingPerfEstimate no_merge = tm_ring_estimate_fabric(
      shared_trace, topology, *cfg,
      TmRingPerfAggregationModel::NO_MERGE);
  const TmRingPerfEstimate ideal = tm_ring_estimate_fabric(
      shared_trace, topology, *cfg,
      TmRingPerfAggregationModel::IDEAL_TRACE_MERGE);

  EXPECT_EQ(uint64_t(4), no_merge.logical_read_requests);
  EXPECT_EQ(uint64_t(4), no_merge.backend_reads);
  EXPECT_EQ(uint64_t(0), no_merge.backend_read_saved);
  EXPECT_EQ(uint64_t(4), no_merge.h_carriers);
  EXPECT_EQ(uint64_t(4), no_merge.h_unicast_carriers);
  EXPECT_EQ(uint64_t(0), no_merge.h_multicast_carriers);
  EXPECT_EQ(uint64_t(4), no_merge.v_carriers);

  EXPECT_EQ(uint64_t(4), ideal.logical_read_requests);
  EXPECT_EQ(uint64_t(1), ideal.backend_reads);
  EXPECT_EQ(uint64_t(3), ideal.backend_read_saved);
  EXPECT_EQ(uint64_t(2), ideal.h_carriers);
  EXPECT_EQ(uint64_t(0), ideal.h_unicast_carriers);
  EXPECT_EQ(uint64_t(2), ideal.h_multicast_carriers);
  EXPECT_EQ(uint64_t(0), ideal.h_scatter_carriers);
  EXPECT_EQ(uint64_t(4), ideal.h_carrier_recipients);
  EXPECT_EQ(uint64_t(2), ideal.v_carriers);
}

TEST(TmRingPerfEstimatorTest, ClassifiesIdealSameLineScatterCarriers) {
  tm_init();
  auto cfg = make_perf_cfg(4, 1);
  cfg->max_aicore_per_vring = 2;
  TmRingTopology topology;
  topology.config(cfg);

  std::vector<TmRingPerfTxn> trace;
  for (uint32_t master = 0; master < 4; ++master) {
    TmRingPerfTxn txn;
    txn.master_port = master;
    txn.cmd = PldCmd::RD;
    txn.addr = master * 128;
    txn.size = 128;
    txn.ordinal = 0;
    trace.push_back(txn);
  }

  const TmRingPerfEstimate ideal = tm_ring_estimate_fabric(
      trace, topology, *cfg,
      TmRingPerfAggregationModel::IDEAL_TRACE_MERGE);
  EXPECT_EQ(uint64_t(1), ideal.backend_reads);
  EXPECT_EQ(uint64_t(3), ideal.backend_read_saved);
  EXPECT_EQ(uint64_t(2), ideal.h_carriers);
  EXPECT_EQ(uint64_t(2), ideal.h_scatter_carriers);
  EXPECT_EQ(uint64_t(0), ideal.h_multicast_carriers);
  EXPECT_EQ(uint64_t(2), ideal.v_carriers);
}

class RetryPerfMaster : public TmRingPerfMaster {
 public:
  using TmRingPerfMaster::issue;

  const std::vector<p_tm_pld_t>& attempts() const { return attempts_; }

 protected:
  bool send_read_candidate(const p_tm_pld_t& pld) override {
    attempts_.push_back(pld);
    return attempts_.size() > 1;
  }

 private:
  std::vector<p_tm_pld_t> attempts_;
};

TEST(TmRingPerfMasterTest, RetryKeepsCandidateAndGidBeforeCommit) {
  tm_init();
  auto clk = tm_make_clk();
  std::vector<TmRingPerfTxn> transactions(1);
  transactions[0].master_port = 3;
  transactions[0].cmd = PldCmd::RD;
  transactions[0].addr = 0x2000;
  transactions[0].size = 128;
  transactions[0].ordinal = 0;

  RetryPerfMaster master;
  master.config("retry_perf_master", clk, 3, transactions);
  master.issue();
  master.issue();

  ASSERT_EQ(size_t(2), master.attempts().size());
  EXPECT_EQ(master.attempts()[0]->gid, master.attempts()[1]->gid);
  EXPECT_EQ(master.attempts()[0]->addr, master.attempts()[1]->addr);
  EXPECT_EQ(master.attempts()[0]->size, master.attempts()[1]->size);
  EXPECT_EQ(uint64_t(2), master.stats().attempted_packets);
  EXPECT_EQ(uint64_t(1), master.stats().accepted_packets);
  EXPECT_EQ(uint64_t(1), master.stats().send_stall_cycles);
}

TEST(TmRingPerfCollectorTest, AggregatesLatencyFairnessAndMemoryStats) {
  tm_init();
  auto clk = tm_make_clk();
  auto cfg = make_perf_cfg(2, 1);
  auto ring = tm_make_ring(clk, cfg);

  TmRingPerfCase perf_case;
  perf_case.active_masters = 2;
  TmRingPerfMasterStats first;
  first.has_first_request = true;
  first.first_request_cycle = 10;
  first.has_first_response = true;
  first.first_response_cycle = 20;
  first.last_response_cycle = 30;
  first.completed_packets = 2;
  first.completed_bytes = 256;
  first.latency_cycles.push_back(10);
  first.latency_cycles.push_back(20);
  TmRingPerfMasterStats second = first;
  second.first_request_cycle = 12;
  second.first_response_cycle = 22;
  second.last_response_cycle = 32;
  second.completed_bytes = 128;
  second.latency_cycles.clear();
  second.latency_cycles.push_back(15);
  second.latency_cycles.push_back(25);

  TmMemStats memory0;
  memory0.accepted_read_bytes = 256;
  memory0.outstanding_peak = 4;
  TmMemStats memory1;
  memory1.accepted_write_bytes = 128;
  memory1.outstanding_peak = 7;
  const std::vector<TmRingPerfMasterStats> masters = {first, second};
  const std::vector<TmMemStats> memories = {memory0, memory1};
  TmRingPerfEstimate estimate;
  const TmRingPerfResult result = tm_ring_collect_perf_result(
      perf_case, masters, *ring, memories, estimate, estimate, 32, true);

  EXPECT_EQ(uint64_t(384), result.completed_bytes);
  EXPECT_EQ(uint64_t(4), result.completed_packets);
  EXPECT_EQ(uint64_t(10), result.first_request_time);
  EXPECT_EQ(uint64_t(32), result.last_response_time);
  EXPECT_EQ(uint64_t(23), result.transfer_cycles);
  EXPECT_EQ(uint64_t(10), result.measurement_start_time);
  EXPECT_EQ(uint64_t(32), result.measurement_end_time);
  EXPECT_EQ(uint64_t(23), result.measurement_cycles);
  EXPECT_TRUE(result.measurement_valid);
  EXPECT_EQ(uint64_t(15), result.latency_p50);
  EXPECT_EQ(uint64_t(25), result.latency_max);
  EXPECT_EQ(uint64_t(256), result.memory_stats.accepted_read_bytes);
  EXPECT_EQ(uint64_t(128), result.memory_stats.accepted_write_bytes);
  EXPECT_EQ(uint64_t(7), result.memory_stats.outstanding_peak);
  EXPECT_TRUE(result.drained);
  EXPECT_GT(result.jain_fairness, 0.0);
}

TEST(TmRingPerfCollectorTest, LeavesMeasurementInvalidWithoutRequests) {
  tm_init();
  auto clk = tm_make_clk();
  auto cfg = make_perf_cfg(1, 1);
  auto ring = tm_make_ring(clk, cfg);

  TmRingPerfCase perf_case;
  const std::vector<TmRingPerfMasterStats> masters;
  const std::vector<TmMemStats> memories;
  TmRingPerfEstimate estimate;
  const TmRingPerfResult result = tm_ring_collect_perf_result(
      perf_case, masters, *ring, memories, estimate, estimate, 42, true);

  EXPECT_EQ(uint64_t(0), result.measurement_start_time);
  EXPECT_EQ(uint64_t(42), result.measurement_end_time);
  EXPECT_EQ(uint64_t(0), result.measurement_cycles);
  EXPECT_FALSE(result.measurement_valid);
}

TEST(TmRingPerfReportTest, EmitsStableSectionsAndKeys) {
  TmRingPerfResult result;
  result.perf_case.name = "report_test";
  result.perf_case.op = TmRingPerfOp::READ;
  result.perf_case.pattern = TmRingPerfPattern::SAME_LINE_SCATTER;
  result.perf_case.burst_len = 4;
  result.completed_packets = 4;
  result.completed_bytes = 512;
  result.drained = true;
  result.end_to_end_bandwidth_bpc = 8.0;
  result.estimate.fabric_model_ceiling_bpc = 16.0;
  result.estimate.hottest_ring_edge_cycles = 2;
  result.estimate.hottest_rbrg_path_cycles = 4;
  result.estimate.fabric_min_cycles = 4;
  result.estimate.physical_packets = 4;
  result.no_merge_estimate.physical_packets = 8;
  const std::string report = tm_ring_format_perf_result(result);

  const char* sections[] = {
      "PERF_CONFIG",          "PERF_COUNTS",
      "PERF_BANDWIDTH",       "PERF_LATENCY",
      "PERF_RING",            "PERF_HOME_AGENT",
      "PERF_L2_BUFFER",       "PERF_MEMORY",
      "PERF_THEORY_NO_MERGE", "PERF_THEORY_IDEAL_MERGE",
      "PERF_THEORY",          "PERF_RESULT"};
  for (const char* section : sections) {
    EXPECT_NE(std::string::npos, report.find(section));
  }
  EXPECT_NE(std::string::npos, report.find("case=report_test"));
  EXPECT_NE(std::string::npos, report.find("pattern=same_line_scatter"));
  EXPECT_NE(std::string::npos, report.find("burst_len=4"));
  EXPECT_EQ(std::string::npos, report.find("burst_bytes="));
  EXPECT_NE(std::string::npos, report.find("completed_bytes=512"));
  EXPECT_NE(std::string::npos, report.find("status=PASS"));
  EXPECT_NE(std::string::npos, report.find("fabric_min_cycles=4"));
  EXPECT_NE(std::string::npos,
            report.find("hottest_ring_edge_cycles=2"));
  EXPECT_NE(std::string::npos,
            report.find("hottest_rbrg_path_cycles=4"));
  EXPECT_NE(std::string::npos, report.find("run_mode=free_running"));
  EXPECT_NE(std::string::npos,
            report.find("transfer_model=packet_cut_through_approx"));
  EXPECT_NE(std::string::npos,
            report.find("scaling_efficiency_available=0"));
  EXPECT_NE(std::string::npos, report.find("rd_merged_pending=0"));
  EXPECT_NE(std::string::npos, report.find("waiter_full_stalls=0"));
  EXPECT_NE(std::string::npos,
            report.find("aggregation_closed_stalls=0"));
  EXPECT_NE(std::string::npos, report.find("carrier_other=0"));
  EXPECT_NE(std::string::npos,
            report.find("PERF_THEORY_NO_MERGE total_useful_bytes=0 "
                        "physical_packets=8"));
  EXPECT_NE(std::string::npos,
            report.find("PERF_THEORY_IDEAL_MERGE total_useful_bytes=0 "
                        "physical_packets=4"));
}

TEST(TmRingPerfReportTest, EmitsHomeAgentRequestSourcesByMasterAndCommand) {
  TmRingPerfResult result;
  result.perf_case.name = "ha_source_report";
  result.drained = true;

  TmRingHaSourceStats source;
  source.ha_id = 2;
  source.master_id = 5;
  source.rd_packets = 7;
  source.wr_packets = 3;
  result.ring_pmu.ha.sources.push_back(source);

  const std::string report = tm_ring_format_perf_result(result);

  EXPECT_NE(std::string::npos,
            report.find("PERF_HA_SOURCE ha=2 master=5 rd_packets=7 "
                        "wr_packets=3 total_packets=10"));
}

TEST(TmRingPerfReportTest, EmitsMeasuredChannelAndBufferRecords) {
  TmRingPerfResult result;
  result.perf_case.name = "measured_channel_report";
  result.drained = true;
  result.measurement_start_time = 10;
  result.measurement_end_time = 109;
  result.measurement_cycles = 100;
  result.measurement_valid = true;
  result.ring_link_width_bytes = 128;
  result.rbrg_width_bytes = 128;

  TmRingDomainStats h_domain;
  h_domain.type = TmRingDomainType::H_RING;
  h_domain.directed_edge_count = 1;
  set_conn_stats(&h_domain.cw[static_cast<uint32_t>(TmRingSubnet::REQ)], 1,
                 16, 1, 0, 0, 0, 0);
  set_conn_stats(&h_domain.cw[static_cast<uint32_t>(TmRingSubnet::DAT)], 5,
                 6400, 80, 0, 0, 0, 0);
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .events = 3;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .unique_packets = 2;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .eligible_unicast_packets = 4;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .completed_packets = 2;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .rounds_sum = 3;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .rounds_max = 2;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .delay_cycles_sum = 30;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .delay_cycles_max = 20;
  h_domain.cross_station.deflection[static_cast<uint32_t>(TmRingSubnet::DAT)]
      .fanout_recipient_retry_events = 1;

  TmRingConnHotspot edge;
  edge.src_station = 0;
  edge.src_dir = TmRingPortDir::CW;
  edge.dst_station = 1;
  edge.dst_dir = TmRingPortDir::CW;
  edge.subnet = TmRingSubnet::DAT;
  edge.packets = 5;
  edge.bytes = 6400;
  edge.busy_cycles = 80;
  h_domain.edges.push_back(edge);
  result.ring_pmu.conn.domains.push_back(h_domain);

  TmRingEndpointQueueStats endpoint;
  endpoint.node_type = TmRingNodeType::MASTER;
  endpoint.queue.subnet = TmRingSubnet::DAT;
  endpoint.queue.side = TmRingQueueSide::EJECT;
  endpoint.queue.direction = TmRingPortDir::CW;
  endpoint.queue.depth = 8;
  endpoint.queue.occupancy = 2;
  endpoint.queue.occupancy_peak = 5;
  endpoint.queue.counters.pushes = 7;
  endpoint.queue.counters.pops = 5;
  endpoint.queue.counters.push_rejects = 1;
  endpoint.queue.counters.occupancy_area = 200;
  endpoint.queue.counters.full_cycles = 10;
  result.ring_pmu.queue.endpoints.push_back(endpoint);

  TmRingRbrgStats rbrg0;
  TmRingRbrgPathStats& rbrg_path =
      rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::V_TO_H_DAT)];
  rbrg_path.packets = 5;
  rbrg_path.bytes = 6400;
  rbrg_path.busy_cycles = 50;
  rbrg_path.queue_occupancy_peak = 3;
  rbrg_path.queue_full_stalls = 4;
  rbrg_path.destination_inject_stalls = 5;
  result.ring_pmu.rbrg.instances.push_back(rbrg0);
  result.ring_pmu.rbrg.instance_ids.push_back(1);

  TmRingRbrgStats rbrg7;
  rbrg7.paths[static_cast<uint32_t>(TmRingRbrgPath::H_TO_V_DAT)].packets = 2;
  result.ring_pmu.rbrg.instances.push_back(rbrg7);
  result.ring_pmu.rbrg.instance_ids.push_back(7);

  const std::string report = tm_ring_format_perf_result(result);

  EXPECT_NE(std::string::npos,
            report.find("PERF_MEASUREMENT start_cycle=10 end_cycle=109 "
                        "window_cycles=100 measurement_valid=1"));
  EXPECT_NE(std::string::npos, report.find("PERF_RING_BUFFER"));
  EXPECT_NE(std::string::npos,
            report.find("node_type=master node=0 subnet=dat side=eject "
                        "direction=cw depth=8"));
  EXPECT_NE(std::string::npos,
            report.find("PERF_DEFLECTION domain=h ring=0 subnet=dat"));
  EXPECT_NE(std::string::npos,
            report.find("PERF_HW_CHANNEL domain=h ring=0 subnet=dat "
                        "direction=cw"));
  EXPECT_NE(std::string::npos,
            report.find("PERF_RING_EDGE domain=h ring=0 subnet=dat "
                        "direction=cw src_station=0 dst_station=1"));
  EXPECT_NE(std::string::npos,
            report.find("PERF_RBRG_CHANNEL id=1 path=v_to_h_dat"));
  EXPECT_NE(std::string::npos,
            report.find("PERF_RBRG_CHANNEL id=7 path=h_to_v_dat"));
  EXPECT_EQ(std::string::npos,
            report.find("PERF_RBRG_CHANNEL id=0"));
  EXPECT_NE(std::string::npos, report.find("cycle_util_pct=80.000000"));
  EXPECT_NE(std::string::npos,
            report.find("payload_util_pct=50.000000"));
  EXPECT_NE(std::string::npos,
            report.find("serialization_efficiency_pct=62.500000"));
  EXPECT_NE(std::string::npos,
            report.find("serialization_efficiency_pct=12.500000"));
}

struct PerfSmokeResult {
  std::vector<TmRingPerfMasterStats> master_stats;
  std::vector<TmMemStats> memory_stats;
  TmRingPerfResult perf_result;
  bool idle = false;
  uint64_t cycles = 0;
};

struct PerfOverrides {
  uint32_t max_aicore_per_vring = 0;
  uint32_t home_agent_waiters_per_entry = 0;
  uint32_t l2_response_latency = 0;
};

std::string perf_config_path() {
  const std::string paths[] = {
      "../etc/pem_config_cloud.toml",
      "../etc/pem_config_cloud.toml"};
  for (const auto& path : paths) {
    std::ifstream file(path.c_str());
    if (file.good()) {
      return path;
    }
  }
  return std::string();
}

PerfSmokeResult run_perf_smoke(const TmRingPerfCase& perf_case,
                               const PerfOverrides& overrides =
                                   PerfOverrides()) {
  const std::string config_path = perf_config_path();
  if (config_path.empty()) {
    ADD_FAILURE() << "pem_config_cloud.toml is not available";
    return PerfSmokeResult();
  }

  tm_init();
  auto clk = tm_make_clk();
  auto scenario_cfg = std::make_shared<cfg::Cfg>();
  scenario_cfg->read_cfg_file(config_path);
  cfg::p_cfg_t cfg = scenario_cfg;
  auto ring_cfg = tm_make_ring_cfg("perf_smoke_ring", cfg);
  ring_cfg->num_masters = perf_case.active_masters;
  if (overrides.max_aicore_per_vring != 0) {
    ring_cfg->max_aicore_per_vring = overrides.max_aicore_per_vring;
  }
  if (overrides.home_agent_waiters_per_entry != 0) {
    ring_cfg->home_agent_waiters_per_entry =
        overrides.home_agent_waiters_per_entry;
  }
  if (overrides.l2_response_latency != 0) {
    ring_cfg->l2_traffic.response_latency = overrides.l2_response_latency;
  }
  TmRingPerfCase effective_case = perf_case;
  effective_case.max_aicore_per_vring = ring_cfg->max_aicore_per_vring;
  effective_case.home_agent_waiters_per_entry =
      ring_cfg->home_agent_waiters_per_entry;
  effective_case.l2_response_latency =
      ring_cfg->l2_traffic.response_latency;
  auto biu_cfg = cfg->get_cfg_tab("BIU");

  std::vector<p_tm_mem_t> memories;
  for (uint32_t target = 0; target < ring_cfg->targets.size(); ++target) {
    auto mem_cfg = tm_make_mem_cfg(ring_cfg->targets[target]->name, cfg);
    memories.push_back(tm_make_mem(clk, mem_cfg));
  }

  auto ring = tm_make_ring(clk, ring_cfg);
  ring->build();
  TmRingTopology topology;
  topology.config(ring_cfg);
  const std::vector<TmRingPerfTxn> trace =
      tm_ring_build_perf_trace(effective_case, ring_cfg->num_masters,
                               topology, ring_cfg->ring_link_width_bytes);
  const TmRingPerfEstimate estimate =
      tm_ring_estimate_fabric(trace, topology, *ring_cfg,
                              TmRingPerfAggregationModel::IDEAL_TRACE_MERGE);
  const TmRingPerfEstimate no_merge_estimate =
      tm_ring_estimate_fabric(trace, topology, *ring_cfg,
                              TmRingPerfAggregationModel::NO_MERGE);

  std::vector<std::vector<TmRingPerfTxn> > master_traces(
      effective_case.active_masters);
  for (const TmRingPerfTxn& txn : trace) {
    if (txn.master_port >= master_traces.size()) {
      ADD_FAILURE() << "trace contains an out-of-range master";
      return PerfSmokeResult();
    }
    master_traces[txn.master_port].push_back(txn);
  }

  std::shared_ptr<TmRingPerfWaveCoordinator> wave_coordinator;
  if (effective_case.run_mode == TmRingPerfRunMode::AGGREGATION_WAVE) {
    const size_t transactions_per_master = master_traces.front().size();
    for (const std::vector<TmRingPerfTxn>& master_trace : master_traces) {
      if (master_trace.size() != transactions_per_master) {
        ADD_FAILURE() << "aggregation wave traces must have equal lengths";
        return PerfSmokeResult();
      }
    }
    wave_coordinator = std::make_shared<TmRingPerfWaveCoordinator>(
        effective_case.active_masters);
  }

  std::vector<p_pem_biu_t> bius;
  std::vector<std::shared_ptr<TmRingPerfMaster>> masters;
  for (uint32_t master_id = 0; master_id < perf_case.active_masters;
       ++master_id) {
    auto biu = std::make_shared<pem_biu_t>(
        "perf_biu" + std::to_string(master_id), clk, biu_cfg);
    biu->core_id_ = master_id;
    biu->build();
    biu->reset();
    ring->attach_master(master_id, biu);
    bius.push_back(biu);

    auto master = std::make_shared<TmRingPerfMaster>();
    master->config("perf_master" + std::to_string(master_id), clk,
                   master_id, master_traces[master_id], wave_coordinator);
    master->attach(biu);
    master->build();
    masters.push_back(master);
  }

  for (uint32_t target = 0; target < memories.size(); ++target) {
    ring->attach_target(target, memories[target]);
  }

  const uint64_t limit = perf_case.drain_cycle_limit;
  const bool diagnose_write_stall =
      perf_case.name == "multi_vring_private_write_128b";
  uint64_t last_completed = 0;
  uint64_t last_completion_cycle = 0;
  const uint64_t no_completion_limit = 10000;
  auto emit_write_progress = [&](const char* state, uint64_t cycle) {
    uint64_t accepted = 0;
    uint64_t completed = 0;
    uint64_t send_stalls = 0;
    for (const auto& master : masters) {
      const TmRingPerfMasterStats& stats = master->stats();
      accepted += stats.accepted_packets;
      completed += stats.completed_packets;
      send_stalls += stats.send_stall_cycles;
    }
    uint64_t biu_outstanding = 0;
    uint32_t biu_cmd_pending = 0;
    uint32_t biu_data_pending = 0;
    for (const auto& biu : bius) {
      biu_outstanding += biu->wr_otsd_;
      biu_cmd_pending += biu->wr_cmds_->empty() ? 0 : 1;
      biu_data_pending += biu->wr_data_->empty() ? 0 : 1;
    }
    uint64_t master_dat_inject_pushes = 0;
    uint64_t master_dat_inject_pops = 0;
    uint64_t master_dat_inject_rejects = 0;
    uint32_t master_dat_inject_occupancy = 0;
    const TmRingPmuSnapshot ring_snapshot =
        ring->snapshot_pmu(static_cast<uint64_t>(clk->time()));
    for (const TmRingEndpointQueueStats& endpoint :
         ring_snapshot.queue.endpoints) {
      const TmRingQueueStats& queue = endpoint.queue;
      if (endpoint.node_type != TmRingNodeType::MASTER ||
          queue.subnet != TmRingSubnet::DAT ||
          queue.side != TmRingQueueSide::INJECT) {
        continue;
      }
      master_dat_inject_pushes += queue.counters.pushes;
      master_dat_inject_pops += queue.counters.pops;
      master_dat_inject_rejects += queue.counters.push_rejects;
      master_dat_inject_occupancy += queue.occupancy;
    }
    uint64_t memory_accepted_bytes = 0;
    uint64_t memory_queue_full_stalls = 0;
    uint32_t memory_busy = 0;
    for (const auto& memory : memories) {
      const TmMemStats& stats = memory->stats();
      memory_accepted_bytes += stats.accepted_write_bytes;
      memory_queue_full_stalls += stats.queue_full_stall_cycles;
      memory_busy += memory->idle() ? 0 : 1;
    }
    std::cout << "WRITE_LIVENESS state=" << state << " cycle=" << cycle
              << " accepted=" << accepted << " completed=" << completed
              << " send_stalls=" << send_stalls
              << " biu_outstanding=" << biu_outstanding
              << " biu_cmd_pending=" << biu_cmd_pending
              << " biu_data_pending=" << biu_data_pending
              << " master_dat_inject_pushes=" << master_dat_inject_pushes
              << " master_dat_inject_pops=" << master_dat_inject_pops
              << " master_dat_inject_rejects="
              << master_dat_inject_rejects
              << " master_dat_inject_occupancy="
              << master_dat_inject_occupancy
              << " master_biu_data_pending="
              << ring->pending_master_write_data()
              << " memory_accepted_bytes=" << memory_accepted_bytes
              << " memory_queue_full_stalls=" << memory_queue_full_stalls
              << " memory_busy=" << memory_busy
              << " ring_idle=" << ring->idle() << std::endl;
  };
  uint64_t cycle = 0;
  for (; cycle < limit; ++cycle) {
    bool done = ring->idle();
    for (const auto& master : masters) {
      done = done && master->idle();
    }
    for (const auto& biu : bius) {
      done = done && biu->idle();
    }
    for (const auto& memory : memories) {
      done = done && memory->idle();
    }
    if (done && cycle > 0) {
      break;
    }
    if (diagnose_write_stall) {
      uint64_t completed = 0;
      for (const auto& master : masters) {
        completed += master->stats().completed_packets;
      }
      if (completed != last_completed) {
        last_completed = completed;
        last_completion_cycle = cycle;
      }
      if (cycle != 0 && cycle % no_completion_limit == 0) {
        emit_write_progress("progress", cycle);
      }
      if (cycle - last_completion_cycle >= no_completion_limit) {
        emit_write_progress("stalled", cycle);
        break;
      }
    }
    tm_start(1);
  }

  PerfSmokeResult result;
  result.cycles = cycle;
  result.idle = ring->idle();
  for (const auto& master : masters) {
    result.idle = result.idle && master->idle();
    result.master_stats.push_back(master->stats());
  }
  for (const auto& biu : bius) {
    result.idle = result.idle && biu->idle();
  }
  for (const auto& memory : memories) {
    result.idle = result.idle && memory->idle();
    result.memory_stats.push_back(memory->stats());
  }
  result.perf_result = tm_ring_collect_perf_result(
      effective_case, result.master_stats, *ring, result.memory_stats,
      estimate, no_merge_estimate, clk->time(), result.idle);
  return result;
}

void expect_l2_carrier_size_bucket(const TmRingL2BufferStats& l2,
                                   uint32_t physical_carrier_bytes,
                                   uint64_t expected_carriers) {
  EXPECT_EQ(expected_carriers, l2.h_carriers);
  EXPECT_EQ(physical_carrier_bytes * expected_carriers, l2.dat_bytes);
  EXPECT_EQ(physical_carrier_bytes == 128 ? expected_carriers : uint64_t(0),
            l2.injected_carrier_128b);
  EXPECT_EQ(physical_carrier_bytes == 256 ? expected_carriers : uint64_t(0),
            l2.injected_carrier_256b);
  EXPECT_EQ(physical_carrier_bytes == 512 ? expected_carriers : uint64_t(0),
            l2.injected_carrier_512b);
  EXPECT_EQ(physical_carrier_bytes != 128 &&
                    physical_carrier_bytes != 256 &&
                    physical_carrier_bytes != 512
                ? expected_carriers
                : uint64_t(0),
            l2.injected_carrier_other);
}

void expect_l2_single_physical_carrier_size(
    const TmRingL2BufferStats& l2, uint32_t physical_carrier_bytes) {
  EXPECT_EQ(physical_carrier_bytes * l2.h_carriers, l2.dat_bytes);
  EXPECT_EQ(physical_carrier_bytes == 128 ? l2.h_carriers : uint64_t(0),
            l2.injected_carrier_128b);
  EXPECT_EQ(physical_carrier_bytes == 256 ? l2.h_carriers : uint64_t(0),
            l2.injected_carrier_256b);
  EXPECT_EQ(physical_carrier_bytes == 512 ? l2.h_carriers : uint64_t(0),
            l2.injected_carrier_512b);
  EXPECT_EQ(physical_carrier_bytes != 128 &&
                    physical_carrier_bytes != 256 &&
                    physical_carrier_bytes != 512
                ? l2.h_carriers
                : uint64_t(0),
            l2.injected_carrier_other);
}

void expect_perf_block_complete(const PerfSmokeResult& result,
                                const TmRingPerfCase& perf_case) {
  ASSERT_TRUE(result.idle);
  ASSERT_EQ(static_cast<size_t>(perf_case.active_masters),
            result.master_stats.size());
  const uint32_t request_bytes = tm_ring_perf_request_bytes(
      perf_case, result.perf_result.ring_link_width_bytes);
  const uint64_t packets_per_master =
      perf_case.bytes_per_master / request_bytes;
  const uint64_t directions = perf_case.op == TmRingPerfOp::READ_WRITE ? 2 : 1;
  const uint64_t bytes_per_master = perf_case.bytes_per_master * directions;
  for (const TmRingPerfMasterStats& stats : result.master_stats) {
    EXPECT_EQ(packets_per_master * directions, stats.completed_packets);
    EXPECT_EQ(bytes_per_master, stats.completed_bytes);
    EXPECT_EQ(uint64_t(0), stats.unknown_responses);
    EXPECT_EQ(uint64_t(0), stats.duplicate_responses);
  }
  EXPECT_EQ(bytes_per_master * perf_case.active_masters,
            result.perf_result.completed_bytes);
  EXPECT_EQ(uint64_t(0), result.perf_result.protocol_errors);
  EXPECT_TRUE(result.perf_result.drained);
  if (result.perf_result.estimate.fabric_model_ceiling_bpc > 0.0) {
    EXPECT_LE(result.perf_result.end_to_end_bandwidth_bpc,
               result.perf_result.estimate.fabric_model_ceiling_bpc * 1.01);
  }
  std::cout << tm_ring_format_perf_result(result.perf_result);
}

TmRingPerfCase make_128kb_case(const std::string& name, TmRingPerfOp op,
                               TmRingPerfPattern pattern, uint32_t masters,
                               uint32_t burst_len) {
  TmRingPerfCase perf_case;
  perf_case.name = name;
  perf_case.op = op;
  perf_case.pattern = pattern;
  perf_case.active_masters = masters;
  perf_case.bytes_per_master =kMultiVringBenchmarkBYTES_PER_MASTER;
  perf_case.burst_len = burst_len;
  // The benchmark exercises the DDR-side endpoint rather than the L2 address
  // window used by the small functional smoke tests.
  perf_case.read_base = 64ull * 1024 * 1024;
  perf_case.drain_cycle_limit = 2000000;
  return perf_case;
}

uint64_t rbrg_packets(const TmRingRbrgStats& stats) {
  uint64_t packets = 0;
  for (const TmRingRbrgPathStats& path : stats.paths) {
    packets += path.packets;
  }
  return packets;
}

uint64_t v_ring_dat_carriers(const TmRingPerfResult& result) {
  uint64_t carriers = 0;
  for (const TmRingRbrgStats& stats : result.ring_pmu.rbrg.instances) {
    carriers += stats.paths[static_cast<uint32_t>(
        TmRingRbrgPath::H_TO_V_DAT)].packets;
  }
  return carriers;
}

enum class PerfAggregationExpectation {
  SCATTER,
  MULTICAST
};

struct AggregatedReadExpectation {
  uint64_t logical_responses = 0;
  uint64_t responses_accepted = 0;
  uint64_t carriers = 0;
  uint64_t recipients = 0;
  uint32_t physical_carrier_bytes = 0;
};

AggregatedReadExpectation make_aggregated_read_expectation(
    uint64_t bytes_per_master, uint32_t masters,
    uint32_t max_aicore_per_vring, uint32_t request_bytes,
    PerfAggregationExpectation aggregation) {
  AggregatedReadExpectation expected;
  const uint64_t requests_per_master = bytes_per_master / request_bytes;
  const uint64_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  expected.logical_responses =
      static_cast<uint64_t>(masters) * requests_per_master;
  expected.recipients = expected.logical_responses;
  if (aggregation == PerfAggregationExpectation::SCATTER) {
    const uint32_t recipients_per_line =
        kMultiVringBenchmarkLineBytes / request_bytes;
    const uint64_t groups_per_wave = masters / recipients_per_line;
    expected.responses_accepted = requests_per_master * groups_per_wave;
    expected.carriers = expected.responses_accepted;
    // Each same-V-Ring recipient group spans every sector in its 512B line.
    expected.physical_carrier_bytes = kMultiVringBenchmarkLineBytes;
  } else {
    expected.responses_accepted = requests_per_master;
    expected.carriers = requests_per_master * expected_vrings;
    // All multicast recipients select the same address range.
    expected.physical_carrier_bytes = request_bytes;
  }
  return expected;
}

void run_multi_vring_aggregated_read_benchmark(
    const std::string& name, TmRingPerfPattern pattern, uint32_t masters,
    uint32_t max_aicore_per_vring, uint32_t burst_len,
    uint64_t address_stride, PerfAggregationExpectation expectation) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  const uint32_t request_bytes =
      burst_len * kMultiVringBenchmarkBeatBytes;
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));
  ASSERT_EQ(uint64_t(0),
            kMultiVringBenchmarkBYTES_PER_MASTER % request_bytes);
  if (expectation == PerfAggregationExpectation::SCATTER) {
    ASSERT_EQ(uint32_t(0),
              kMultiVringBenchmarkLineBytes % request_bytes);
    const uint32_t recipients_per_line =
        kMultiVringBenchmarkLineBytes / request_bytes;
    ASSERT_EQ(uint32_t(0), masters % recipients_per_line);
    ASSERT_EQ(uint32_t(0),
              max_aicore_per_vring % recipients_per_line);
  }

  TmRingPerfCase perf_case = make_128kb_case(
      name, TmRingPerfOp::READ, pattern, masters, burst_len);
  if (address_stride != 0) {
    perf_case.stride_bytes = address_stride;
  }
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  overrides.home_agent_waiters_per_entry = masters;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_pmu.conn.domains.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.ring_pmu.rbrg.instances.size());
  for (const TmRingRbrgStats& stats :
       result.perf_result.ring_pmu.rbrg.instances) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  const TmRingL2BufferStats& l2 = result.perf_result.ring_pmu.l2.total;
  const uint64_t expected_logical_responses =
      static_cast<uint64_t>(masters) * perf_case.bytes_per_master /
      request_bytes;
  ASSERT_GT(result.perf_result.ring_pmu.ha.total.backend_read_saved,
            uint64_t(0));
  ASSERT_GT(l2.responses_accepted, uint64_t(0));
  ASSERT_LE(l2.responses_accepted, expected_logical_responses);
  ASSERT_EQ(expected_logical_responses, l2.h_carrier_recipients);
  ASSERT_EQ(l2.h_carriers, v_ring_dat_carriers(result.perf_result));
  if (expectation == PerfAggregationExpectation::SCATTER) {
    ASSERT_GT(l2.h_scatter_carriers, uint64_t(0));
    ASSERT_EQ(uint64_t(0), l2.h_multicast_carriers);
    ASSERT_EQ(l2.h_unicast_carriers + l2.h_scatter_carriers,
              l2.h_carriers);
    ASSERT_EQ(l2.h_carriers, l2.responses_accepted);
    ASSERT_EQ(uint64_t(0), l2.injected_carrier_other);
    ASSERT_EQ(l2.h_carriers,
              l2.injected_carrier_128b + l2.injected_carrier_256b +
                  l2.injected_carrier_512b);
    ASSERT_EQ(uint64_t(128) * l2.injected_carrier_128b +
                  uint64_t(256) * l2.injected_carrier_256b +
                  uint64_t(512) * l2.injected_carrier_512b,
              l2.dat_bytes);
    ASSERT_EQ(l2.h_unicast_carriers,
              request_bytes == 128 ? l2.injected_carrier_128b
                                   : l2.injected_carrier_256b);
    ASSERT_EQ(l2.h_scatter_carriers,
              request_bytes == 128
                  ? l2.injected_carrier_256b + l2.injected_carrier_512b
                  : l2.injected_carrier_512b);
  } else {
    ASSERT_GT(l2.h_multicast_carriers, uint64_t(0));
    ASSERT_EQ(uint64_t(0), l2.h_scatter_carriers);
    ASSERT_EQ(l2.h_unicast_carriers + l2.h_multicast_carriers,
              l2.h_carriers);
    ASSERT_GE(l2.h_carriers, l2.responses_accepted);
    ASSERT_LE(l2.h_carriers,
              static_cast<uint64_t>(expected_vrings) *
                  l2.responses_accepted);
    expect_l2_single_physical_carrier_size(l2, request_bytes);
  }
  expect_perf_block_complete(result, perf_case);
}

void run_multi_vring_128kb_benchmark(
    const std::string& name, TmRingPerfOp op, TmRingPerfPattern pattern,
    uint32_t masters, uint32_t max_aicore_per_vring, uint32_t burst_len) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  const uint32_t request_bytes =
      burst_len * kMultiVringBenchmarkBeatBytes;
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));

  const TmRingPerfCase perf_case =
      make_128kb_case(name, op, pattern, masters, burst_len);
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);
  if (!result.idle) {
    std::cout << tm_ring_format_perf_result(result.perf_result);
  }

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_pmu.conn.domains.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.ring_pmu.rbrg.instances.size());
  for (const TmRingRbrgStats& stats :
       result.perf_result.ring_pmu.rbrg.instances) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  if (op != TmRingPerfOp::WRITE &&
      pattern == TmRingPerfPattern::SEQUENTIAL_PRIVATE) {
    const uint64_t expected_read_responses =
        static_cast<uint64_t>(masters) * perf_case.bytes_per_master /
        request_bytes;
    const TmRingL2BufferStats& l2 = result.perf_result.ring_pmu.l2.total;
    ASSERT_EQ(expected_read_responses, l2.responses_accepted);
    ASSERT_EQ(expected_read_responses, l2.h_carrier_recipients);
    ASSERT_EQ(expected_read_responses, l2.h_unicast_carriers);
    ASSERT_EQ(uint64_t(0), l2.h_multicast_carriers);
    ASSERT_EQ(uint64_t(0), l2.h_scatter_carriers);
    expect_l2_carrier_size_bucket(
        l2, request_bytes, expected_read_responses);
    ASSERT_EQ(expected_read_responses,
              v_ring_dat_carriers(result.perf_result));
  }
  expect_perf_block_complete(result, perf_case);
}

void run_multi_vring_no_merge_read_benchmark(
    const std::string& name, uint32_t masters,
    uint32_t max_aicore_per_vring, uint32_t burst_len) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  const uint32_t request_bytes =
      burst_len * kMultiVringBenchmarkBeatBytes;
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));
  TmRingPerfCase perf_case = make_128kb_case(
      name, TmRingPerfOp::READ, TmRingPerfPattern::STRIDED_PRIVATE, masters,
      burst_len);
  perf_case.stride_bytes = 512;
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_pmu.conn.domains.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.ring_pmu.rbrg.instances.size());
  for (const TmRingRbrgStats& stats :
       result.perf_result.ring_pmu.rbrg.instances) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  const uint64_t expected_responses =
      static_cast<uint64_t>(masters) * perf_case.bytes_per_master /
      request_bytes;
  const TmRingL2BufferStats& l2 = result.perf_result.ring_pmu.l2.total;
  ASSERT_EQ(uint64_t(0),
            result.perf_result.ring_pmu.ha.total.backend_read_saved);
  ASSERT_EQ(uint64_t(0),
            l2.h_multicast_carriers);
  ASSERT_EQ(uint64_t(0),
            l2.h_scatter_carriers);
  ASSERT_EQ(expected_responses, l2.responses_accepted);
  ASSERT_EQ(expected_responses, l2.h_carrier_recipients);
  ASSERT_EQ(expected_responses, l2.h_unicast_carriers);
  expect_l2_carrier_size_bucket(l2, request_bytes, expected_responses);
  ASSERT_EQ(expected_responses,
            v_ring_dat_carriers(result.perf_result));
  expect_perf_block_complete(result, perf_case);
}

void run_aggregation_wave_test(
    const std::string& name, TmRingPerfPattern pattern, uint32_t masters,
    uint32_t max_aicore_per_vring, uint32_t burst_len,
    uint64_t address_stride, uint32_t l2_response_latency,
    PerfAggregationExpectation expectation) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  ASSERT_GT(l2_response_latency, uint32_t(0));
  const uint32_t request_bytes =
      burst_len * kMultiVringBenchmarkBeatBytes;
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));
  ASSERT_EQ(uint64_t(0), uint64_t(4 * 1024) % request_bytes);
  if (expectation == PerfAggregationExpectation::SCATTER) {
    ASSERT_EQ(uint32_t(0),
              kMultiVringBenchmarkLineBytes % request_bytes);
    const uint32_t recipients_per_line =
        kMultiVringBenchmarkLineBytes / request_bytes;
    ASSERT_EQ(uint32_t(0), masters % recipients_per_line);
    ASSERT_EQ(uint32_t(0),
              max_aicore_per_vring % recipients_per_line);
  }

  TmRingPerfCase perf_case;
  perf_case.name = name;
  perf_case.op = TmRingPerfOp::READ;
  perf_case.pattern = pattern;
  perf_case.active_masters = masters;
  perf_case.bytes_per_master = 4 * 1024;
  perf_case.burst_len = burst_len;
  perf_case.read_base = 64ull * 1024 * 1024;
  perf_case.stride_bytes = address_stride;
  perf_case.drain_cycle_limit = 2000000;
  perf_case.run_mode = TmRingPerfRunMode::AGGREGATION_WAVE;

  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  overrides.home_agent_waiters_per_entry = masters;
  overrides.l2_response_latency = l2_response_latency;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);
  const TmRingPerfEstimate& ideal = result.perf_result.estimate;
  const TmRingPerfEstimate& no_merge = result.perf_result.no_merge_estimate;
  const TmRingHomeAgentStats& ha = result.perf_result.ring_pmu.ha.total;
  const TmRingL2BufferStats& l2 = result.perf_result.ring_pmu.l2.total;
  const uint64_t actual_v_carriers =
      v_ring_dat_carriers(result.perf_result);

  std::ostringstream diagnostic;
  diagnostic << "l2_response_latency=" << l2_response_latency
             << " backend_reads(no_merge/ideal/actual)="
             << no_merge.backend_reads << "/" << ideal.backend_reads << "/"
             << ha.rd_entries_allocated
             << " backend_saved(ideal/actual)="
             << ideal.backend_read_saved << "/" << ha.backend_read_saved
             << " merged(pending/inflight/responding)="
             << ha.rd_merged_pending << "/" << ha.rd_merged_inflight << "/"
             << ha.rd_merged_responding
             << " admission_stalls(table/waiter/closed)="
             << ha.table_full_stall_cycles << "/" << ha.waiter_full_stall_cycles
             << "/" << ha.aggregation_closed_stall_cycles
             << " h_carriers(no_merge/ideal/actual)=" << no_merge.h_carriers
             << "/" << ideal.h_carriers << "/" << l2.h_carriers
             << " h_multicast(ideal/actual)="
             << ideal.h_multicast_carriers << "/" << l2.h_multicast_carriers
             << " h_scatter(ideal/actual)=" << ideal.h_scatter_carriers
             << "/" << l2.h_scatter_carriers
             << " h_recipients(no_merge/actual)="
             << no_merge.h_carrier_recipients << "/"
             << l2.h_carrier_recipients
             << " v_carriers(no_merge/ideal/actual)=" << no_merge.v_carriers
             << "/" << ideal.v_carriers << "/" << actual_v_carriers;
  SCOPED_TRACE(diagnostic.str());

  ASSERT_TRUE(result.idle);
  ASSERT_TRUE(result.perf_result.drained);
  ASSERT_EQ(uint64_t(0), result.perf_result.protocol_errors);
  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_pmu.conn.domains.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.ring_pmu.rbrg.instances.size());
  for (const TmRingRbrgStats& stats :
       result.perf_result.ring_pmu.rbrg.instances) {
    ASSERT_GT(stats.paths[static_cast<uint32_t>(
                  TmRingRbrgPath::H_TO_V_DAT)].packets,
              uint64_t(0));
  }
  ASSERT_EQ(no_merge.logical_read_requests, ha.rd_requests);
  ASSERT_EQ(no_merge.backend_reads, ha.rd_requests);
  ASSERT_EQ(ha.rd_requests,
            ha.rd_entries_allocated + ha.backend_read_saved);
  ASSERT_EQ(
      ha.backend_read_saved,
      ha.rd_merged_pending + ha.rd_merged_inflight + ha.rd_merged_responding);
  ASSERT_GT(ha.backend_read_saved, uint64_t(0));
  ASSERT_LT(l2.h_carriers, no_merge.h_carriers);
  ASSERT_LT(actual_v_carriers, no_merge.v_carriers);
  ASSERT_LE(ideal.h_carriers, no_merge.h_carriers);
  ASSERT_LE(ideal.v_carriers, no_merge.v_carriers);
  const uint64_t h_near_ideal_limit =
      ideal.h_carriers + (no_merge.h_carriers - ideal.h_carriers + 3) / 4;
  const uint64_t v_near_ideal_limit =
      ideal.v_carriers + (no_merge.v_carriers - ideal.v_carriers + 3) / 4;
  ASSERT_LE(l2.h_carriers, h_near_ideal_limit);
  ASSERT_LE(actual_v_carriers, v_near_ideal_limit);
  ASSERT_GE(ha.backend_read_saved,
            (ideal.backend_read_saved * 3 + 3) / 4);
  const AggregatedReadExpectation expected =
      make_aggregated_read_expectation(
          perf_case.bytes_per_master, masters, max_aicore_per_vring,
          request_bytes, expectation);
  ASSERT_EQ(expected.logical_responses, ha.rd_requests);
  ASSERT_EQ(expected.responses_accepted, ha.rd_entries_allocated);
  ASSERT_EQ(expected.logical_responses - expected.responses_accepted,
            ha.backend_read_saved);
  ASSERT_EQ(expected.responses_accepted, l2.responses_accepted);
  ASSERT_EQ(expected.recipients, l2.h_carrier_recipients);
  ASSERT_EQ(expected.carriers, actual_v_carriers);
  ASSERT_EQ(uint64_t(0), ha.table_full_stall_cycles);
  ASSERT_EQ(uint64_t(0), ha.waiter_full_stall_cycles);
  ASSERT_EQ(uint64_t(0), ha.aggregation_closed_stall_cycles);
  if (expectation == PerfAggregationExpectation::SCATTER) {
    ASSERT_EQ(expected.carriers, l2.h_scatter_carriers);
    ASSERT_EQ(uint64_t(0), l2.h_multicast_carriers);
  } else {
    ASSERT_EQ(expected.carriers, l2.h_multicast_carriers);
    ASSERT_EQ(uint64_t(0), l2.h_scatter_carriers);
  }
  ASSERT_EQ(uint64_t(0), l2.h_unicast_carriers);
  expect_l2_carrier_size_bucket(
      l2, expected.physical_carrier_bytes, expected.carriers);
  expect_perf_block_complete(result, perf_case);
}

TEST(RingPerfBenchmark, MultiVringPrivateRead128B) {
  run_multi_vring_128kb_benchmark(
      "multi_vring_private_read_128b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 1);
}

TEST(RingPerfBenchmark, MultiVringPrivateWrite128B) {
  run_multi_vring_128kb_benchmark(
      "multi_vring_private_write_128b", TmRingPerfOp::WRITE,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 1);
}

// TEST(RingPerfBenchmark, MultiVringIndependentReadWrite128B) {
//   run_multi_vring_128kb_benchmark(
//       "multi_vring_independent_read_write_128b", TmRingPerfOp::READ_WRITE,
//       TmRingPerfPattern::SEQUENTIAL_PRIVATE, kMultiVringBenchmarkMasters,
//       kMultiVringBenchmarkMaxAicorePerVring, 1);
// }

TEST(RingPerfBenchmark, MultiVringSameLineScatterRead128B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_same_line_scatter_read_128b",
      TmRingPerfPattern::SAME_LINE_SCATTER, kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 1, 0,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingPerfBenchmark, MultiVringSameLineScatterRead256B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_same_line_scatter_read_256b",
      TmRingPerfPattern::SAME_LINE_SCATTER, kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 2, 0,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingPerfBenchmark, MultiVringSharedRead128B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_shared_read_128b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      kMultiVringBenchmarkMasters, kMultiVringBenchmarkMaxAicorePerVring,
      1, 512,
      PerfAggregationExpectation::MULTICAST);
}

TEST(RingPerfBenchmark, MultiVringSharedRead256B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_shared_read_256b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      kMultiVringBenchmarkMasters, kMultiVringBenchmarkMaxAicorePerVring,
      2, 512, PerfAggregationExpectation::MULTICAST);
}

TEST(RingPerfBenchmark, MultiVringSharedRead512B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_shared_read_512b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      kMultiVringBenchmarkMasters, kMultiVringBenchmarkMaxAicorePerVring,
      4, 512, PerfAggregationExpectation::MULTICAST);
}

TEST(RingPerfBenchmark, MultiVringCrossLineNonMergedRead1024B) {
  const uint32_t masters = kMultiVringBenchmarkMasters;
  const uint32_t max_aicore_per_vring =
      kMultiVringBenchmarkMaxAicorePerVring;
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  TmRingPerfCase perf_case = make_128kb_case(
      "multi_vring_cross_line_non_merged_read_1024b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_SHARED, masters, 8);
  perf_case.stride_bytes = 1024;
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  overrides.home_agent_waiters_per_entry = masters;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);
  const uint32_t request_bytes = tm_ring_perf_request_bytes(
      perf_case, result.perf_result.ring_link_width_bytes);
  const uint64_t expected_responses =
      static_cast<uint64_t>(masters) * perf_case.bytes_per_master /
      request_bytes;
  const TmRingL2BufferStats& l2 = result.perf_result.ring_pmu.l2.total;

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_pmu.conn.domains.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.ring_pmu.rbrg.instances.size());
  for (const TmRingRbrgStats& stats :
       result.perf_result.ring_pmu.rbrg.instances) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  ASSERT_EQ(uint64_t(0), result.perf_result.ring_pmu.ha.total.rd_requests);
  ASSERT_EQ(uint64_t(0),
            result.perf_result.ring_pmu.ha.total.backend_read_saved);
  ASSERT_EQ(expected_responses, l2.responses_accepted);
  ASSERT_EQ(expected_responses, l2.h_carrier_recipients);
  ASSERT_EQ(expected_responses, l2.h_unicast_carriers);
  ASSERT_EQ(uint64_t(0), l2.h_multicast_carriers);
  ASSERT_EQ(uint64_t(0), l2.h_scatter_carriers);
  expect_l2_carrier_size_bucket(
      l2, request_bytes, expected_responses);
  ASSERT_EQ(expected_responses,
            v_ring_dat_carriers(result.perf_result));
  expect_perf_block_complete(result, perf_case);
}

TEST(RingPerfBenchmark, MultiVringNoMergeRead128B) {
  run_multi_vring_no_merge_read_benchmark(
      "multi_vring_no_merge_read_128b", kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 1);
}

TEST(RingPerfBenchmark, MultiVringNoMergeRead256B) {
  run_multi_vring_no_merge_read_benchmark(
      "multi_vring_no_merge_read_256b", kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 2);
}

TEST(RingPerfBenchmark, MultiVringNoMergeRead512B) {
  run_multi_vring_no_merge_read_benchmark(
      "multi_vring_no_merge_read_512b", kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 4);
}

TEST(RingAggregationWaveTest, SameLineScatterRead128B) {
  run_aggregation_wave_test(
      "wave_same_line_scatter_read_128b",
      TmRingPerfPattern::SAME_LINE_SCATTER, kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 1, 0, 256,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingAggregationWaveTest, SameLineScatterRead256B) {
  run_aggregation_wave_test(
      "wave_same_line_scatter_read_256b",
      TmRingPerfPattern::SAME_LINE_SCATTER, kMultiVringBenchmarkMasters,
      kMultiVringBenchmarkMaxAicorePerVring, 2, 0, 256,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingAggregationWaveTest, SharedRead128B) {
  run_aggregation_wave_test(
      "wave_shared_read_128b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      kMultiVringBenchmarkMasters, kMultiVringBenchmarkMaxAicorePerVring,
      1, 512, 256, PerfAggregationExpectation::MULTICAST);
}

TEST(RingAggregationWaveTest, SharedRead256B) {
  run_aggregation_wave_test(
      "wave_shared_read_256b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      kMultiVringBenchmarkMasters, kMultiVringBenchmarkMaxAicorePerVring,
      2, 512, 256, PerfAggregationExpectation::MULTICAST);
}

TEST(RingAggregationWaveTest, SharedRead512B) {
  run_aggregation_wave_test(
      "wave_shared_read_512b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      kMultiVringBenchmarkMasters, kMultiVringBenchmarkMaxAicorePerVring,
      4, 512, 256, PerfAggregationExpectation::MULTICAST);
}

TEST(TmRingPerfSmokeTest, ReadWrite4KB) {
  TmRingPerfCase perf_case;
  perf_case.name = "read_write_4kb";
  perf_case.op = TmRingPerfOp::READ_WRITE;
  perf_case.active_masters = 2;
  perf_case.bytes_per_master = 4 * 1024;
  perf_case.burst_len = 1;
  const PerfSmokeResult result = run_perf_smoke(perf_case);
  ASSERT_TRUE(result.idle);
  ASSERT_EQ(size_t(2), result.master_stats.size());
  for (const auto& stats : result.master_stats) {
    EXPECT_EQ(uint64_t(64), stats.completed_packets);
    EXPECT_EQ(uint64_t(8 * 1024), stats.completed_bytes);
  }
  const uint32_t request_bytes = tm_ring_perf_request_bytes(
      perf_case, result.perf_result.ring_link_width_bytes);
  const uint64_t expected_write_completions =
      static_cast<uint64_t>(perf_case.active_masters) *
      perf_case.bytes_per_master / request_bytes;
  uint64_t write_completions = 0;
  for (const TmRingRbrgStats& stats : result.perf_result.ring_pmu.rbrg.instances) {
    write_completions += stats.paths[static_cast<uint32_t>(
        TmRingRbrgPath::H_TO_V_RSP)].packets;
  }
  EXPECT_EQ(expected_write_completions, write_completions);
}

}  // namespace
