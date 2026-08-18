#include <gtest/gtest.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#include "tm_ring.h"
#include "tm_ring_perf.h"
#include "tm_ring_perf_master.h"
#include "tm_ring_perf_report.h"
#include "tm_ring_pmu.h"

namespace {

using namespace tm_engine;

constexpr uint32_t kMultiVringBenchmarkMasters = 8;
constexpr uint32_t kMultiVringBenchmarkMaxAicorePerVring = 4;
constexpr uint32_t kMultiVringBenchmarkLineBytes = 512;
constexpr uint32_t kMultiVringBenchmarkBeatBytes = 128;
constexpr uint32_t kL2SweepPeakBytesPerCycle = 512;
constexpr uint32_t kMultiVringBenchmarkBYTES_PER_MASTER =  1024* 1024;// 1 MiB

struct PerfSmokeResult {
  std::vector<TmRingPerfMasterStats> master_stats;
  std::vector<TmMemStats> memory_stats;
  TmRingPerfResult perf_result;
  bool idle = false;
  uint64_t cycles = 0;
};

struct PerfOverrides {
  uint32_t max_aicore_per_vring = 0;
  uint32_t home_agent_transaction_entries = 0;
  uint32_t home_agent_waiters_per_entry = 0;
  bool has_l2_hit_rate_pct = false;
  uint32_t l2_hit_rate_pct = 0;
  uint32_t l2_buffer_depth = 0;
  uint32_t l2_response_latency = 0;
  uint32_t ddr_bandwidth_limit = 0;
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
  if (overrides.home_agent_transaction_entries != 0) {
    ring_cfg->home_agent_transaction_entries =
        overrides.home_agent_transaction_entries;
  }
  if (overrides.home_agent_waiters_per_entry != 0) {
    ring_cfg->home_agent_waiters_per_entry =
        overrides.home_agent_waiters_per_entry;
  }
  if (overrides.has_l2_hit_rate_pct) {
    if (overrides.l2_hit_rate_pct > 100) {
      ADD_FAILURE() << "L2 hit rate must be in [0, 100]";
      return PerfSmokeResult();
    }
    ring_cfg->l2_traffic.hit_rate_pct = overrides.l2_hit_rate_pct;
  }
  if (overrides.l2_buffer_depth != 0) {
    ring_cfg->l2_traffic.buffer_depth = overrides.l2_buffer_depth;
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
    if (overrides.ddr_bandwidth_limit != 0) {
      mem_cfg->ddr->acc_bw_limit = overrides.ddr_bandwidth_limit;
      mem_cfg->ddr->acc_crdt_update =
          overrides.ddr_bandwidth_limit * mem_cfg->ddr->crdt_update_period;
    }
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

std::string format_l2_sweep_summary(
    const PerfSmokeResult& result, const std::string& pattern_label,
    uint32_t request_bytes, uint32_t hit_rate_pct) {
  const TmRingPerfResult& perf = result.perf_result;
  const TmRingHomeAgentStats& ha = perf.ring_pmu.ha.total;
  const TmRingL2BufferStats& l2 = perf.ring_pmu.l2.total;
  const uint64_t classified =
      ha.l2_hit_transactions + ha.l2_miss_transactions;
  const double hit_observed_pct =
      classified == 0
          ? 0.0
          : 100.0 * static_cast<double>(ha.l2_hit_transactions) /
                static_cast<double>(classified);
  const auto bytes_per_cycle = [&](uint64_t bytes) {
    return perf.transfer_cycles == 0
               ? 0.0
               : static_cast<double>(bytes) /
                     static_cast<double>(perf.transfer_cycles);
  };
  const double peak_pct =
      perf.end_to_end_bandwidth_bpc /
      static_cast<double>(kL2SweepPeakBytesPerCycle) * 100.0;
  const bool pass = result.idle && perf.drained && perf.protocol_errors == 0;

  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << "L2_SWEEP case="
      << perf.perf_case.name << " pattern=" << pattern_label
      << " request_bytes=" << request_bytes << " hit_cfg_pct="
      << hit_rate_pct << " hit_obs_pct=" << hit_observed_pct
      << " e2e_bpc=" << perf.end_to_end_bandwidth_bpc
      << " peak_pct=" << peak_pct << " dat_bpc="
      << bytes_per_cycle(l2.dat_bytes) << " hbm_bpc="
      << bytes_per_cycle(perf.memory_stats.accepted_read_bytes) << " p99="
      << perf.latency_p99 << " status=" << (pass ? "PASS" : "INCOMPLETE")
      << "\n";
  return out.str();
}

void expect_perf_block_complete(const PerfSmokeResult& result,
                                const TmRingPerfCase& perf_case,
                                bool print_full_report = true) {
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
  if (print_full_report) {
    std::cout << tm_ring_format_perf_result(result.perf_result);
  }
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

void run_l2_hit_rate_sweep_case(const std::string& pattern_label,
                                TmRingPerfPattern pattern,
                                uint64_t address_stride) {
  const uint32_t hit_rates[] = {0, 25, 50, 75, 100};
  for (const uint32_t hit_rate_pct : hit_rates) {
    std::ostringstream trace;
    trace << "pattern=" << pattern_label << " hit_rate=" << hit_rate_pct;
    SCOPED_TRACE(trace.str());

    const std::string name =
        "l2_" + pattern_label + "_256b_hit" +
        std::to_string(hit_rate_pct);
    TmRingPerfCase perf_case = make_128kb_case(
        name, TmRingPerfOp::READ, pattern, kMultiVringBenchmarkMasters, 2);
    perf_case.stride_bytes = address_stride;

    PerfOverrides overrides;
    overrides.max_aicore_per_vring = 4;
    overrides.home_agent_transaction_entries = 64;
    overrides.home_agent_waiters_per_entry = 8;
    overrides.has_l2_hit_rate_pct = true;
    overrides.l2_hit_rate_pct = hit_rate_pct;
    overrides.l2_buffer_depth = 64;
    overrides.l2_response_latency = 64;
    overrides.ddr_bandwidth_limit = 256;

    const PerfSmokeResult result = run_perf_smoke(perf_case, overrides);
    expect_perf_block_complete(result, perf_case, false);

    const TmRingHomeAgentStats& ha = result.perf_result.ring_pmu.ha.total;
    const TmRingL2BufferStats& l2 = result.perf_result.ring_pmu.l2.total;
    const uint64_t classified =
        ha.l2_hit_transactions + ha.l2_miss_transactions;
    ASSERT_GT(classified, uint64_t(0));
    const double observed_hit_rate =
        100.0 * static_cast<double>(ha.l2_hit_transactions) /
        static_cast<double>(classified);
    if (hit_rate_pct == 0 || hit_rate_pct == 100) {
      EXPECT_DOUBLE_EQ(static_cast<double>(hit_rate_pct), observed_hit_rate);
    } else {
      EXPECT_NEAR(static_cast<double>(hit_rate_pct), observed_hit_rate, 2.0);
    }

    EXPECT_EQ(uint64_t(0),
              ha.backend_read_bytes % kMultiVringBenchmarkLineBytes);
    EXPECT_EQ(ha.rd_backend_issued * kMultiVringBenchmarkLineBytes,
              ha.backend_read_bytes);
    EXPECT_EQ(ha.backend_read_bytes,
              result.perf_result.memory_stats.accepted_read_bytes);

    if (pattern == TmRingPerfPattern::STRIDED_PRIVATE) {
      EXPECT_EQ(uint64_t(0), ha.backend_read_saved);
      EXPECT_EQ(ha.l2_miss_transactions, ha.rd_backend_issued);
      EXPECT_EQ(uint64_t(0), ha.rd_merged_pending);
      EXPECT_EQ(uint64_t(0), ha.rd_merged_inflight);
      EXPECT_EQ(uint64_t(0), ha.rd_merged_responding);
      EXPECT_EQ(l2.h_carriers, l2.h_unicast_carriers);
      EXPECT_EQ(uint64_t(0), l2.h_multicast_carriers);
      EXPECT_EQ(uint64_t(0), l2.h_scatter_carriers);
      if (hit_rate_pct == 0) {
        EXPECT_EQ(uint64_t(16) * 1024 * 1024,
                  result.perf_result.memory_stats.accepted_read_bytes);
      }
      if (hit_rate_pct == 100) {
        EXPECT_EQ(uint64_t(0),
                  result.perf_result.memory_stats.accepted_read_bytes);
      }
    }

    std::cout << format_l2_sweep_summary(result, pattern_label, 256,
                                         hit_rate_pct);
  }
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

TEST(RingL2HitRateSweep, NoMergeRead256B) {
  run_l2_hit_rate_sweep_case("nomerge", TmRingPerfPattern::STRIDED_PRIVATE,
                             512);
}

TEST(RingL2HitRateSweep, PrivateSequentialRead256B) {
  run_l2_hit_rate_sweep_case("private",
                             TmRingPerfPattern::SEQUENTIAL_PRIVATE, 256);
}

TEST(RingL2HitRateSweep, SameLineScatterRead256B) {
  run_l2_hit_rate_sweep_case("scatter", TmRingPerfPattern::SAME_LINE_SCATTER,
                             0);
}

TEST(RingL2HitRateSweep, SharedRead256B) {
  run_l2_hit_rate_sweep_case("shared", TmRingPerfPattern::SEQUENTIAL_SHARED,
                             512);
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

}  // namespace
