#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "tm_ring.h"
#include "tm_ring_perf.h"
#include "tm_ring_perf_master.h"
#include "tm_ring_perf_report.h"

namespace {

using namespace tm_engine;

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

void set_rbrg_path_stats(TmRingRbrgPathStats* stats, uint64_t packets,
                         uint64_t bytes, uint64_t queue_peak,
                         uint64_t queue_full_stalls,
                         uint64_t destination_inject_stalls) {
  stats->packets = packets;
  stats->bytes = bytes;
  stats->queue_occupancy_peak = queue_peak;
  stats->queue_full_stalls = queue_full_stalls;
  stats->destination_inject_stalls = destination_inject_stalls;
}

void expect_report_direction(const std::string& report, const char* prefix,
                             const TmRingConnStats& stats) {
  std::ostringstream expected;
  expected << prefix << "_packets=" << stats.packets;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
  expected.str("");
  expected << prefix << "_bytes=" << stats.bytes;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
  expected.str("");
  expected << prefix << "_busy_cycles=" << stats.busy_cycles;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
  expected.str("");
  expected << prefix << "_stalls=" << tm_ring_conn_total_stalls(stats);
  EXPECT_NE(std::string::npos, report.find(expected.str()));
}

void expect_report_rbrg_path(const std::string& report, const char* prefix,
                             const TmRingRbrgPathStats& stats) {
  std::ostringstream expected;
  expected << prefix << "_packets=" << stats.packets;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
  expected.str("");
  expected << prefix << "_bytes=" << stats.bytes;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
  expected.str("");
  expected << prefix << "_queue_peak=" << stats.queue_occupancy_peak;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
  expected.str("");
  expected << prefix << "_queue_full_stalls=" << stats.queue_full_stalls;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
  expected.str("");
  expected << prefix << "_inject_stalls=" << stats.destination_inject_stalls;
  EXPECT_NE(std::string::npos, report.find(expected.str()));
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

TEST(TmRingPerfTraceTest, BuildsPrivateAndSharedFiniteReadTraces) {
  tm_init();
  TmRingTopology topology;
  auto cfg = make_perf_cfg(4, 2);
  topology.config(cfg);

  TmRingPerfCase private_case;
  private_case.active_masters = 4;
  private_case.bytes_per_master = 4 * 1024;
  private_case.burst_bytes = 128;
  private_case.pattern = TmRingPerfPattern::SEQUENTIAL_PRIVATE;

  const std::vector<TmRingPerfTxn> private_trace =
      tm_ring_build_perf_trace(private_case, 4, topology);
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
      tm_ring_build_perf_trace(shared_case, 4, topology);
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
  topology.config(cfg);

  const uint32_t request_sizes[] = {128, 256};
  for (const uint32_t request_bytes : request_sizes) {
    TmRingPerfCase perf_case;
    perf_case.active_masters = 5;
    perf_case.bytes_per_master = 2 * request_bytes;
    perf_case.burst_bytes = request_bytes;
    perf_case.pattern = TmRingPerfPattern::SAME_LINE_SCATTER;
    perf_case.read_base = 64ull * 1024 * 1024;

    const std::vector<TmRingPerfTxn> trace =
        tm_ring_build_perf_trace(perf_case, 5, topology);
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
  topology.config(cfg);

  const uint32_t request_sizes[] = {128, 256, 512};
  for (const uint32_t request_bytes : request_sizes) {
    TmRingPerfCase perf_case;
    perf_case.active_masters = 3;
    perf_case.bytes_per_master = 2 * request_bytes;
    perf_case.burst_bytes = request_bytes;
    perf_case.pattern = TmRingPerfPattern::SEQUENTIAL_SHARED;
    perf_case.read_base = 64ull * 1024 * 1024;
    perf_case.stride_bytes = 512;

    const std::vector<TmRingPerfTxn> trace =
        tm_ring_build_perf_trace(perf_case, 3, topology);
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
  topology.config(cfg);

  TmRingPerfCase perf_case;
  perf_case.active_masters = 2;
  perf_case.bytes_per_master = 2 * 1024;
  perf_case.burst_bytes = 1024;
  perf_case.pattern = TmRingPerfPattern::SEQUENTIAL_SHARED;
  perf_case.read_base = 64ull * 1024 * 1024;
  perf_case.stride_bytes = 1024;

  const std::vector<TmRingPerfTxn> trace =
      tm_ring_build_perf_trace(perf_case, 2, topology);
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

TEST(TmRingPerfEstimatorTest, SharedSectorReadsUseOneFixedCwCarrier) {
  tm_init();
  auto cfg = make_perf_cfg(3, 1);
  TmRingTopology topology;
  topology.config(cfg);

  cfg->ring_link_width_bytes = 128;
  const uint32_t carrier_sizes[] = {128, 256, 512};
  for (const uint32_t carrier_size : carrier_sizes) {
    std::vector<TmRingPerfTxn> trace;
    for (uint32_t master = 0; master < 3; ++master) {
      TmRingPerfTxn txn;
      txn.master_port = master;
      txn.cmd = PldCmd::RD;
      txn.addr = 0;
      txn.size = carrier_size;
      trace.push_back(txn);
    }

    const TmRingPerfEstimate estimate =
        tm_ring_estimate_fabric(trace, topology, *cfg);
    const uint64_t packet_cycles = carrier_size / 128;
    const TmRingPerfEdgeKey h_dat_edge(
        TmRingDomainType::H_RING, 0, TmRingSubnet::DAT, 2,
        TmRingPortDir::CW);
    const TmRingPerfEdgeKey v0_dat_edge(
        TmRingDomainType::V_RING, 0, TmRingSubnet::DAT, 0,
        TmRingPortDir::CW);
    const TmRingPerfEdgeKey v1_dat_edge(
        TmRingDomainType::V_RING, 0, TmRingSubnet::DAT, 1,
        TmRingPortDir::CW);
    const TmRingPerfEdgeKey v2_dat_edge(
        TmRingDomainType::V_RING, 0, TmRingSubnet::DAT, 2,
        TmRingPortDir::CW);
    const TmRingPerfRbrgKey dat_path(0, TmRingRbrgPath::H_TO_V_DAT);
    EXPECT_EQ(uint64_t(4), estimate.physical_packets);
    EXPECT_EQ(uint64_t(3) * carrier_size, estimate.total_useful_bytes);
    EXPECT_EQ(packet_cycles, estimate.edge_cycles.at(h_dat_edge));
    EXPECT_EQ(packet_cycles, estimate.edge_cycles.at(v0_dat_edge));
    EXPECT_EQ(packet_cycles, estimate.edge_cycles.at(v1_dat_edge));
    EXPECT_EQ(packet_cycles, estimate.edge_cycles.at(v2_dat_edge));
    EXPECT_EQ(packet_cycles, estimate.rbrg_path_cycles.at(dat_path));
  }
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
      perf_case, masters, *ring, memories, estimate, estimate, true);

  EXPECT_EQ(uint64_t(384), result.completed_bytes);
  EXPECT_EQ(uint64_t(4), result.completed_packets);
  EXPECT_EQ(uint64_t(10), result.first_request_time);
  EXPECT_EQ(uint64_t(32), result.last_response_time);
  EXPECT_EQ(uint64_t(23), result.transfer_cycles);
  EXPECT_EQ(uint64_t(15), result.latency_p50);
  EXPECT_EQ(uint64_t(25), result.latency_max);
  EXPECT_EQ(uint64_t(256), result.memory_stats.accepted_read_bytes);
  EXPECT_EQ(uint64_t(128), result.memory_stats.accepted_write_bytes);
  EXPECT_EQ(uint64_t(7), result.memory_stats.outstanding_peak);
  EXPECT_TRUE(result.drained);
  EXPECT_GT(result.jain_fairness, 0.0);
}

TEST(TmRingPerfReportTest, EmitsStableSectionsAndKeys) {
  TmRingPerfResult result;
  result.perf_case.name = "report_test";
  result.perf_case.op = TmRingPerfOp::READ;
  result.perf_case.pattern = TmRingPerfPattern::SAME_LINE_SCATTER;
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
  EXPECT_NE(std::string::npos, report.find("completed_bytes=512"));
  EXPECT_NE(std::string::npos, report.find("status=PASS"));
  EXPECT_NE(std::string::npos, report.find("hottest_cycles=2"));
  EXPECT_NE(std::string::npos, report.find("hottest_edge_cycles=2"));
  EXPECT_NE(std::string::npos, report.find("fabric_min_cycles=4"));
  EXPECT_NE(std::string::npos,
            report.find("hottest_ring_edge_cycles=2"));
  EXPECT_NE(std::string::npos,
            report.find("hottest_rbrg_path_cycles=4"));
  EXPECT_NE(std::string::npos, report.find("run_mode=free_running"));
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

TEST(TmRingPerfReportTest, EmitsMultiRingSnapshotRecords) {
  TmRingPerfResult result;
  result.perf_case.name = "multi_ring_report";
  result.drained = true;

  TmRingDomainStats h_domain;
  h_domain.type = TmRingDomainType::H_RING;
  set_conn_stats(&h_domain.cw[static_cast<uint32_t>(TmRingSubnet::REQ)], 11,
                 111, 211, 1, 2, 3, 4);
  set_conn_stats(&h_domain.ccw[static_cast<uint32_t>(TmRingSubnet::REQ)], 12,
                 112, 212, 2, 3, 4, 5);
  set_conn_stats(&h_domain.cw[static_cast<uint32_t>(TmRingSubnet::RSP)], 13,
                 113, 213, 3, 4, 5, 6);
  set_conn_stats(&h_domain.ccw[static_cast<uint32_t>(TmRingSubnet::RSP)], 14,
                 114, 214, 4, 5, 6, 7);
  set_conn_stats(&h_domain.cw[static_cast<uint32_t>(TmRingSubnet::DAT)], 15,
                 115, 215, 5, 6, 7, 8);
  set_conn_stats(&h_domain.ccw[static_cast<uint32_t>(TmRingSubnet::DAT)], 16,
                 116, 216, 6, 7, 8, 9);
  h_domain.hottest.src_station = 1;
  h_domain.hottest.src_dir = TmRingPortDir::CW;
  h_domain.hottest.dst_station = 2;
  h_domain.hottest.dst_dir = TmRingPortDir::CCW;
  h_domain.hottest.subnet = TmRingSubnet::REQ;
  h_domain.hottest.busy_cycles = 211;
  h_domain.hottest.total_stalls = tm_ring_conn_total_stalls(
      h_domain.cw[static_cast<uint32_t>(TmRingSubnet::REQ)]);

  TmRingDomainStats v_domain0;
  v_domain0.type = TmRingDomainType::V_RING;
  v_domain0.ring_id = 0;
  v_domain0.ccw[static_cast<uint32_t>(TmRingSubnet::DAT)].packets = 3;
  v_domain0.ccw[static_cast<uint32_t>(TmRingSubnet::DAT)].bytes = 384;
  v_domain0.ccw[static_cast<uint32_t>(TmRingSubnet::DAT)].busy_cycles = 6;

  TmRingDomainStats v_domain1;
  v_domain1.type = TmRingDomainType::V_RING;
  v_domain1.ring_id = 1;
  v_domain1.cw[static_cast<uint32_t>(TmRingSubnet::RSP)].packets = 2;
  v_domain1.cw[static_cast<uint32_t>(TmRingSubnet::RSP)].bytes = 16;
  v_domain1.cw[static_cast<uint32_t>(TmRingSubnet::RSP)].busy_cycles = 5;

  result.ring_domain_stats = {h_domain, v_domain0, v_domain1};

  TmRingRbrgStats rbrg0;
  set_rbrg_path_stats(
      &rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::V_TO_H_REQ)], 21,
      210, 31, 41, 51);
  set_rbrg_path_stats(
      &rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::V_TO_H_DAT)], 22,
      220, 32, 42, 52);
  set_rbrg_path_stats(
      &rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::H_TO_V_RSP)], 23,
      230, 33, 43, 53);
  set_rbrg_path_stats(
      &rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::H_TO_V_DAT)], 2,
      256, 34, 44, 54);
  TmRingRbrgStats rbrg1;
  rbrg1.paths[static_cast<uint32_t>(TmRingRbrgPath::H_TO_V_DAT)].packets = 1;
  result.rbrg_stats = {rbrg0, rbrg1};
  result.l2_buffer_stats.h_carrier_recipients = 10;
  result.l2_buffer_stats.h_carriers = 4;

  const std::string report = tm_ring_format_perf_result(result);

  EXPECT_NE(std::string::npos, report.find("PERF_RING"));
  EXPECT_NE(std::string::npos, report.find("PERF_RING_DOMAIN type=h id=0"));
  EXPECT_NE(std::string::npos, report.find("PERF_RING_DOMAIN type=v id=0"));
  EXPECT_NE(std::string::npos, report.find("PERF_RING_DOMAIN type=v id=1"));
  expect_report_direction(
      report, "req_cw", h_domain.cw[static_cast<uint32_t>(TmRingSubnet::REQ)]);
  expect_report_direction(
      report, "req_ccw", h_domain.ccw[static_cast<uint32_t>(TmRingSubnet::REQ)]);
  expect_report_direction(
      report, "rsp_cw", h_domain.cw[static_cast<uint32_t>(TmRingSubnet::RSP)]);
  expect_report_direction(
      report, "rsp_ccw", h_domain.ccw[static_cast<uint32_t>(TmRingSubnet::RSP)]);
  expect_report_direction(
      report, "dat_cw", h_domain.cw[static_cast<uint32_t>(TmRingSubnet::DAT)]);
  expect_report_direction(
      report, "dat_ccw", h_domain.ccw[static_cast<uint32_t>(TmRingSubnet::DAT)]);
  EXPECT_NE(std::string::npos, report.find("hottest_dst_station=2"));
  EXPECT_NE(std::string::npos, report.find("hottest_stalls=10"));
  EXPECT_NE(std::string::npos, report.find("PERF_RBRG id=0"));
  EXPECT_NE(std::string::npos, report.find("PERF_RBRG id=1"));
  expect_report_rbrg_path(
      report, "v_to_h_req",
      rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::V_TO_H_REQ)]);
  expect_report_rbrg_path(
      report, "v_to_h_dat",
      rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::V_TO_H_DAT)]);
  expect_report_rbrg_path(
      report, "h_to_v_rsp",
      rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::H_TO_V_RSP)]);
  expect_report_rbrg_path(
      report, "h_to_v_dat",
      rbrg0.paths[static_cast<uint32_t>(TmRingRbrgPath::H_TO_V_DAT)]);
  EXPECT_NE(std::string::npos, report.find("PERF_FANOUT_CROSS_RING"));
  EXPECT_NE(std::string::npos, report.find("logical_recipients=10"));
  EXPECT_NE(std::string::npos, report.find("h_ring_carriers=4"));
  EXPECT_NE(std::string::npos, report.find("v_ring_carriers=3"));
  EXPECT_NE(std::string::npos, report.find("total_segment_packets_saved=13"));
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
                               topology);
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
      estimate, no_merge_estimate, result.idle);
  return result;
}

void expect_perf_block_complete(const PerfSmokeResult& result,
                                const TmRingPerfCase& perf_case) {
  ASSERT_TRUE(result.idle);
  ASSERT_EQ(static_cast<size_t>(perf_case.active_masters),
            result.master_stats.size());
  const uint64_t packets_per_master =
      perf_case.bytes_per_master / perf_case.burst_bytes;
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
                               uint32_t burst_bytes) {
  TmRingPerfCase perf_case;
  perf_case.name = name;
  perf_case.op = op;
  perf_case.pattern = pattern;
  perf_case.active_masters = masters;
  perf_case.bytes_per_master = 128 * 1024;
  perf_case.burst_bytes = burst_bytes;
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
  for (const TmRingRbrgStats& stats : result.rbrg_stats) {
    carriers += stats.paths[static_cast<uint32_t>(
        TmRingRbrgPath::H_TO_V_DAT)].packets;
  }
  return carriers;
}

enum class PerfAggregationExpectation {
  SCATTER,
  MULTICAST
};

void run_multi_vring_aggregated_read_benchmark(
    const std::string& name, TmRingPerfPattern pattern, uint32_t masters,
    uint32_t max_aicore_per_vring, uint32_t request_bytes,
    uint64_t address_stride, PerfAggregationExpectation expectation) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));

  TmRingPerfCase perf_case = make_128kb_case(
      name, TmRingPerfOp::READ, pattern, masters, request_bytes);
  if (address_stride != 0) {
    perf_case.stride_bytes = address_stride;
  }
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  overrides.home_agent_waiters_per_entry = masters;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_domain_stats.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.rbrg_stats.size());
  for (const TmRingRbrgStats& stats : result.perf_result.rbrg_stats) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  ASSERT_GT(result.perf_result.home_agent_stats.backend_read_saved,
            uint64_t(0));
  if (expectation == PerfAggregationExpectation::SCATTER) {
    ASSERT_GT(result.perf_result.l2_buffer_stats.h_scatter_carriers,
              uint64_t(0));
    ASSERT_EQ(uint64_t(0),
              result.perf_result.l2_buffer_stats.h_multicast_carriers);
  } else {
    ASSERT_GT(result.perf_result.l2_buffer_stats.h_multicast_carriers,
              uint64_t(0));
    ASSERT_EQ(uint64_t(0),
              result.perf_result.l2_buffer_stats.h_scatter_carriers);
  }
  expect_perf_block_complete(result, perf_case);
}

void run_multi_vring_128kb_benchmark(
    const std::string& name, TmRingPerfOp op, TmRingPerfPattern pattern,
    uint32_t masters, uint32_t max_aicore_per_vring, uint32_t burst_bytes) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));

  const TmRingPerfCase perf_case =
      make_128kb_case(name, op, pattern, masters, burst_bytes);
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_domain_stats.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.rbrg_stats.size());
  for (const TmRingRbrgStats& stats : result.perf_result.rbrg_stats) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  expect_perf_block_complete(result, perf_case);
}

void run_multi_vring_no_merge_read_benchmark(
    const std::string& name, uint32_t masters,
    uint32_t max_aicore_per_vring, uint32_t request_bytes) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));
  TmRingPerfCase perf_case = make_128kb_case(
      name, TmRingPerfOp::READ, TmRingPerfPattern::STRIDED_PRIVATE, masters,
      request_bytes);
  perf_case.stride_bytes = 512;
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_domain_stats.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.rbrg_stats.size());
  for (const TmRingRbrgStats& stats : result.perf_result.rbrg_stats) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  ASSERT_EQ(uint64_t(0),
            result.perf_result.home_agent_stats.backend_read_saved);
  ASSERT_EQ(uint64_t(0),
            result.perf_result.l2_buffer_stats.h_multicast_carriers);
  ASSERT_EQ(uint64_t(0),
            result.perf_result.l2_buffer_stats.h_scatter_carriers);
  ASSERT_EQ(result.perf_result.completed_packets,
            result.perf_result.l2_buffer_stats.h_carriers);
  ASSERT_EQ(result.perf_result.completed_packets,
            result.perf_result.l2_buffer_stats.h_unicast_carriers);
  ASSERT_EQ(result.perf_result.completed_packets,
            v_ring_dat_carriers(result.perf_result));
  expect_perf_block_complete(result, perf_case);
}

void run_aggregation_wave_test(
    const std::string& name, TmRingPerfPattern pattern, uint32_t masters,
    uint32_t max_aicore_per_vring, uint32_t request_bytes,
    uint64_t address_stride, uint32_t l2_response_latency,
    PerfAggregationExpectation expectation) {
  ASSERT_GT(max_aicore_per_vring, uint32_t(0));
  ASSERT_GT(l2_response_latency, uint32_t(0));
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  ASSERT_GT(expected_vrings, uint32_t(1));

  TmRingPerfCase perf_case;
  perf_case.name = name;
  perf_case.op = TmRingPerfOp::READ;
  perf_case.pattern = pattern;
  perf_case.active_masters = masters;
  perf_case.bytes_per_master = 4 * 1024;
  perf_case.burst_bytes = request_bytes;
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
  const TmRingHomeAgentStats& ha = result.perf_result.home_agent_stats;
  const TmRingL2BufferStats& l2 = result.perf_result.l2_buffer_stats;

  std::ostringstream diagnostic;
  diagnostic << "l2_response_latency=" << l2_response_latency
             << " backend_reads(expected/actual)=" << ideal.backend_reads
             << "/" << ha.rd_entries_allocated
             << " backend_saved(expected/actual)="
             << ideal.backend_read_saved << "/" << ha.backend_read_saved
             << " merged(pending/inflight/responding)="
             << ha.rd_merged_pending << "/" << ha.rd_merged_inflight << "/"
             << ha.rd_merged_responding
             << " admission_stalls(table/waiter/closed)="
             << ha.table_full_stall_cycles << "/" << ha.waiter_full_stall_cycles
             << "/" << ha.aggregation_closed_stall_cycles
             << " h_carriers(expected/actual)=" << ideal.h_carriers << "/"
             << l2.h_carriers << " h_multicast(expected/actual)="
             << ideal.h_multicast_carriers << "/" << l2.h_multicast_carriers
             << " h_scatter(expected/actual)=" << ideal.h_scatter_carriers
             << "/" << l2.h_scatter_carriers
             << " h_recipients(expected/actual)="
             << ideal.h_carrier_recipients << "/" << l2.h_carrier_recipients
             << " v_carriers(expected/actual)=" << ideal.v_carriers << "/"
             << v_ring_dat_carriers(result.perf_result);
  SCOPED_TRACE(diagnostic.str());

  ASSERT_TRUE(result.idle);
  ASSERT_TRUE(result.perf_result.drained);
  ASSERT_EQ(uint64_t(0), result.perf_result.protocol_errors);
  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_domain_stats.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.rbrg_stats.size());
  for (const TmRingRbrgStats& stats : result.perf_result.rbrg_stats) {
    ASSERT_GT(stats.paths[static_cast<uint32_t>(
                  TmRingRbrgPath::H_TO_V_DAT)].packets,
              uint64_t(0));
  }
  ASSERT_EQ(ideal.logical_read_requests,
            result.perf_result.home_agent_stats.rd_requests);
  ASSERT_EQ(ideal.backend_reads,
            result.perf_result.home_agent_stats.rd_entries_allocated);
  ASSERT_EQ(ideal.backend_read_saved,
            result.perf_result.home_agent_stats.backend_read_saved);
  ASSERT_EQ(
      ideal.backend_read_saved,
      result.perf_result.home_agent_stats.rd_merged_pending +
          result.perf_result.home_agent_stats.rd_merged_inflight +
          result.perf_result.home_agent_stats.rd_merged_responding);
  ASSERT_EQ(ideal.h_carriers,
            result.perf_result.l2_buffer_stats.h_carriers);
  ASSERT_EQ(ideal.h_unicast_carriers,
            result.perf_result.l2_buffer_stats.h_unicast_carriers);
  ASSERT_EQ(ideal.h_multicast_carriers,
            result.perf_result.l2_buffer_stats.h_multicast_carriers);
  ASSERT_EQ(ideal.h_scatter_carriers,
            result.perf_result.l2_buffer_stats.h_scatter_carriers);
  ASSERT_EQ(ideal.h_carrier_recipients,
            result.perf_result.l2_buffer_stats.h_carrier_recipients);
  ASSERT_EQ(ideal.v_carriers, v_ring_dat_carriers(result.perf_result));
  ASSERT_EQ(uint64_t(0),
            result.perf_result.home_agent_stats.table_full_stall_cycles);
  ASSERT_EQ(uint64_t(0),
            result.perf_result.home_agent_stats.waiter_full_stall_cycles);
  ASSERT_EQ(
      uint64_t(0),
      result.perf_result.home_agent_stats.aggregation_closed_stall_cycles);
  if (expectation == PerfAggregationExpectation::SCATTER) {
    ASSERT_GT(ideal.h_scatter_carriers, uint64_t(0));
    ASSERT_EQ(uint64_t(0), ideal.h_multicast_carriers);
  } else {
    ASSERT_GT(ideal.h_multicast_carriers, uint64_t(0));
    ASSERT_EQ(uint64_t(0), ideal.h_scatter_carriers);
  }
  expect_perf_block_complete(result, perf_case);
}

TEST(RingPerfBenchmark, MultiVringPrivateRead128B) {
  run_multi_vring_128kb_benchmark(
      "multi_vring_private_read_128b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 18, 8, 128);
}

TEST(RingPerfBenchmark, MultiVringPrivateWrite128B) {
  run_multi_vring_128kb_benchmark(
      "multi_vring_private_write_128b", TmRingPerfOp::WRITE,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 18, 8, 128);
}

TEST(RingPerfBenchmark, MultiVringIndependentReadWrite128B) {
  run_multi_vring_128kb_benchmark(
      "multi_vring_independent_read_write_128b", TmRingPerfOp::READ_WRITE,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 18, 8, 128);
}

TEST(RingPerfBenchmark, MultiVringSameLineScatterRead128B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_same_line_scatter_read_128b",
      TmRingPerfPattern::SAME_LINE_SCATTER, 18, 8, 128, 0,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingPerfBenchmark, MultiVringSameLineScatterRead256B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_same_line_scatter_read_256b",
      TmRingPerfPattern::SAME_LINE_SCATTER, 18, 8, 256, 0,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingPerfBenchmark, MultiVringSharedRead128B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_shared_read_128b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      18, 8, 128, 512,
      PerfAggregationExpectation::MULTICAST);
}

TEST(RingPerfBenchmark, MultiVringSharedRead256B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_shared_read_256b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      18, 8, 256, 512, PerfAggregationExpectation::MULTICAST);
}

TEST(RingPerfBenchmark, MultiVringSharedRead512B) {
  run_multi_vring_aggregated_read_benchmark(
      "multi_vring_shared_read_512b", TmRingPerfPattern::SEQUENTIAL_SHARED,
      18, 8, 512, 512, PerfAggregationExpectation::MULTICAST);
}

TEST(RingPerfBenchmark, MultiVringCrossLineNonMergedRead1024B) {
  const uint32_t masters = 18;
  const uint32_t max_aicore_per_vring = 8;
  const uint32_t expected_vrings =
      (masters + max_aicore_per_vring - 1) / max_aicore_per_vring;
  TmRingPerfCase perf_case = make_128kb_case(
      "multi_vring_cross_line_non_merged_read_1024b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_SHARED, masters, 1024);
  perf_case.stride_bytes = 1024;
  PerfOverrides overrides;
  overrides.max_aicore_per_vring = max_aicore_per_vring;
  overrides.home_agent_waiters_per_entry = masters;
  const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);

  ASSERT_EQ(static_cast<size_t>(expected_vrings + 1),
            result.perf_result.ring_domain_stats.size());
  ASSERT_EQ(static_cast<size_t>(expected_vrings),
            result.perf_result.rbrg_stats.size());
  for (const TmRingRbrgStats& stats : result.perf_result.rbrg_stats) {
    ASSERT_GT(rbrg_packets(stats), uint64_t(0));
  }
  ASSERT_EQ(uint64_t(0), result.perf_result.home_agent_stats.rd_requests);
  ASSERT_EQ(uint64_t(0),
            result.perf_result.home_agent_stats.backend_read_saved);
  ASSERT_EQ(uint64_t(0),
            result.perf_result.l2_buffer_stats.h_multicast_carriers);
  ASSERT_EQ(uint64_t(0),
            result.perf_result.l2_buffer_stats.h_scatter_carriers);
  ASSERT_EQ(result.perf_result.completed_packets,
            result.perf_result.l2_buffer_stats.h_unicast_carriers);
  ASSERT_EQ(result.perf_result.completed_packets,
            result.perf_result.l2_buffer_stats.injected_carrier_other);
  expect_perf_block_complete(result, perf_case);
}

TEST(RingPerfBenchmark, MultiVringNoMergeRead128B) {
  run_multi_vring_no_merge_read_benchmark(
      "multi_vring_no_merge_read_128b", 18, 8, 128);
}

TEST(RingPerfBenchmark, MultiVringNoMergeRead256B) {
  run_multi_vring_no_merge_read_benchmark(
      "multi_vring_no_merge_read_256b", 18, 8, 256);
}

TEST(RingPerfBenchmark, MultiVringNoMergeRead512B) {
  run_multi_vring_no_merge_read_benchmark(
      "multi_vring_no_merge_read_512b", 18, 8, 512);
}

TEST(RingAggregationWaveTest, SameLineScatterRead128B) {
  run_aggregation_wave_test(
      "wave_same_line_scatter_read_128b",
      TmRingPerfPattern::SAME_LINE_SCATTER, 18, 8, 128, 0, 4096,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingAggregationWaveTest, SameLineScatterRead256B) {
  run_aggregation_wave_test(
      "wave_same_line_scatter_read_256b",
      TmRingPerfPattern::SAME_LINE_SCATTER, 18, 8, 256, 0, 4096,
      PerfAggregationExpectation::SCATTER);
}

TEST(RingAggregationWaveTest, SharedRead128B) {
  run_aggregation_wave_test(
      "wave_shared_read_128b", TmRingPerfPattern::SEQUENTIAL_SHARED, 18, 8,
      128, 512, 4096, PerfAggregationExpectation::MULTICAST);
}

TEST(RingAggregationWaveTest, SharedRead256B) {
  run_aggregation_wave_test(
      "wave_shared_read_256b", TmRingPerfPattern::SEQUENTIAL_SHARED, 18, 8,
      256, 512, 4096, PerfAggregationExpectation::MULTICAST);
}

TEST(RingAggregationWaveTest, SharedRead512B) {
  run_aggregation_wave_test(
      "wave_shared_read_512b", TmRingPerfPattern::SEQUENTIAL_SHARED, 18, 8,
      512, 512, 4096, PerfAggregationExpectation::MULTICAST);
}

TEST(TmRingPerfSmokeTest, ReadWrite4KB) {
  TmRingPerfCase perf_case;
  perf_case.name = "read_write_4kb";
  perf_case.op = TmRingPerfOp::READ_WRITE;
  perf_case.active_masters = 2;
  perf_case.bytes_per_master = 4 * 1024;
  perf_case.burst_bytes = 128;
  const PerfSmokeResult result = run_perf_smoke(perf_case);
  ASSERT_TRUE(result.idle);
  ASSERT_EQ(size_t(2), result.master_stats.size());
  for (const auto& stats : result.master_stats) {
    EXPECT_EQ(uint64_t(64), stats.completed_packets);
    EXPECT_EQ(uint64_t(8 * 1024), stats.completed_bytes);
  }
}

}  // namespace
