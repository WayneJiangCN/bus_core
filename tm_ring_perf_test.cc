#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
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
      perf_case, masters, *ring, memories, estimate, true);

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
  result.perf_case.pattern = TmRingPerfPattern::SEQUENTIAL_PRIVATE;
  result.completed_packets = 4;
  result.completed_bytes = 512;
  result.drained = true;
  result.end_to_end_bandwidth_bpc = 8.0;
  result.estimate.fabric_model_ceiling_bpc = 16.0;
  result.estimate.hottest_ring_edge_cycles = 2;
  result.estimate.hottest_rbrg_path_cycles = 4;
  result.estimate.fabric_min_cycles = 4;
  const std::string report = tm_ring_format_perf_result(result);

  const char* sections[] = {"PERF_CONFIG", "PERF_COUNTS", "PERF_BANDWIDTH",
                            "PERF_LATENCY", "PERF_RING", "PERF_HOME_AGENT",
                            "PERF_L2_BUFFER", "PERF_MEMORY", "PERF_THEORY",
                            "PERF_RESULT"};
  for (const char* section : sections) {
    EXPECT_NE(std::string::npos, report.find(section));
  }
  EXPECT_NE(std::string::npos, report.find("case=report_test"));
  EXPECT_NE(std::string::npos, report.find("completed_bytes=512"));
  EXPECT_NE(std::string::npos, report.find("status=PASS"));
  EXPECT_NE(std::string::npos, report.find("hottest_cycles=2"));
  EXPECT_NE(std::string::npos, report.find("hottest_edge_cycles=2"));
  EXPECT_NE(std::string::npos, report.find("fabric_min_cycles=4"));
  EXPECT_NE(std::string::npos,
            report.find("hottest_ring_edge_cycles=2"));
  EXPECT_NE(std::string::npos,
            report.find("hottest_rbrg_path_cycles=4"));
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
  uint32_t ring_link_width_bytes = 0;
  uint32_t l2_issue_interval = UINT32_MAX;
  uint32_t l2_hit_rate_pct = UINT32_MAX;
  uint32_t ddr_bandwidth_limit = 0;
  uint32_t ddr_max_acc_credit = 0;
  std::string interleave;
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
  if (overrides.ring_link_width_bytes != 0) {
    ring_cfg->ring_link_width_bytes = overrides.ring_link_width_bytes;
  }
  if (overrides.l2_issue_interval != UINT32_MAX) {
    ring_cfg->l2_traffic.issue_interval = overrides.l2_issue_interval;
  }
  if (overrides.l2_hit_rate_pct != UINT32_MAX) {
    ring_cfg->l2_traffic.hit_rate_pct = overrides.l2_hit_rate_pct;
  }
  if (!overrides.interleave.empty()) {
    const tm_bus_interleave_type_t type =
        overrides.interleave == "xor"
            ? tm_bus_interleave_type_t::XOR_HASH
            : tm_bus_interleave_type_t::LINEAR;
    for (auto& target : ring_cfg->targets) {
      target->interleave_type = type;
    }
  }
  auto biu_cfg = cfg->get_cfg_tab("BIU");

  std::vector<p_tm_mem_t> memories;
  for (uint32_t target = 0; target < ring_cfg->targets.size(); ++target) {
    auto mem_cfg = tm_make_mem_cfg(ring_cfg->targets[target]->name, cfg);
    if (overrides.ddr_bandwidth_limit != 0) {
      mem_cfg->ddr->acc_bw_limit = overrides.ddr_bandwidth_limit;
      mem_cfg->ddr->acc_crdt_update = overrides.ddr_bandwidth_limit;
    }
    if (overrides.ddr_max_acc_credit != 0) {
      mem_cfg->ddr->max_acc_crdt = overrides.ddr_max_acc_credit;
    }
    memories.push_back(tm_make_mem(clk, mem_cfg));
  }

  auto ring = tm_make_ring(clk, ring_cfg);
  ring->build();
  TmRingTopology topology;
  topology.config(ring_cfg);
  const std::vector<TmRingPerfTxn> trace =
      tm_ring_build_perf_trace(perf_case, ring_cfg->num_masters, topology);
  const TmRingPerfEstimate estimate =
      tm_ring_estimate_fabric(trace, topology, *ring_cfg);

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

    std::vector<TmRingPerfTxn> master_trace;
    for (const auto& txn : trace) {
      if (txn.master_port == master_id) {
        master_trace.push_back(txn);
      }
    }
    auto master = std::make_shared<TmRingPerfMaster>();
    master->config("perf_master" + std::to_string(master_id), clk,
                   master_id, master_trace);
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
      perf_case, result.master_stats, *ring, result.memory_stats, estimate,
      result.idle);
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

TEST(RingPerfBenchmark, DISABLED_SingleMasterPrivateRead128B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "single_master_private_read_128b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 1, 128);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_EightMasterPrivateRead128B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "eight_master_private_read_128b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_EightMasterPrivateWrite128B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "eight_master_private_write_128b", TmRingPerfOp::WRITE,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_EightMasterIndependentReadWrite128B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "eight_master_independent_read_write_128b", TmRingPerfOp::READ_WRITE,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_EightMasterSharedRead16B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "eight_master_shared_read_16b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_SHARED, 8, 16);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_EightMasterSharedRead128B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "eight_master_shared_read_128b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_SHARED, 8, 128);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_EightMasterSharedRead256B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "eight_master_shared_read_256b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_SHARED, 8, 256);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_EightMasterSharedRead512B) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "eight_master_shared_read_512b", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_SHARED, 8, 512);
  expect_perf_block_complete(run_perf_smoke(perf_case), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_BottleneckRingWidth) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "bottleneck_ring_width", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  PerfOverrides overrides;
  overrides.ring_link_width_bytes = 16;
  expect_perf_block_complete(run_perf_smoke(perf_case, overrides), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_BottleneckL2IssueInterval) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "bottleneck_l2_issue_interval", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  PerfOverrides overrides;
  overrides.l2_issue_interval = 4;
  expect_perf_block_complete(run_perf_smoke(perf_case, overrides), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_BottleneckL2HitRate) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "bottleneck_l2_hit_rate", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  PerfOverrides overrides;
  overrides.l2_hit_rate_pct = 0;
  expect_perf_block_complete(run_perf_smoke(perf_case, overrides), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_BottleneckDdrBandwidth) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "bottleneck_ddr_bandwidth", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  PerfOverrides overrides;
  overrides.ddr_bandwidth_limit = 16;
  expect_perf_block_complete(run_perf_smoke(perf_case, overrides), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_BottleneckDdrCredit) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "bottleneck_ddr_credit", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  PerfOverrides overrides;
  overrides.ddr_max_acc_credit = 256;
  expect_perf_block_complete(run_perf_smoke(perf_case, overrides), perf_case);
}

TEST(RingPerfBenchmark, DISABLED_InterleaveXor) {
  const TmRingPerfCase perf_case = make_128kb_case(
      "interleave_xor", TmRingPerfOp::READ,
      TmRingPerfPattern::SEQUENTIAL_PRIVATE, 8, 128);
  PerfOverrides overrides;
  overrides.interleave = "xor";
  expect_perf_block_complete(run_perf_smoke(perf_case, overrides), perf_case);
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
