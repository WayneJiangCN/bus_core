#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "pem_biu.h"
#include "tm_mem.h"
#include "tm_ring.h"
#include "tm_ring_perf.h"
#include "tm_ring_perf_master.h"

namespace {
using namespace tm_engine;
p_tm_ring_cfg_t make_multiring_cfg(uint32_t masters,
                                    uint32_t targets,
                                    uint32_t max_per_vring) {
  p_tm_ring_cfg_t cfg = tm_make_ring_cfg("multiring_test");
  cfg->num_masters = masters;
  cfg->max_aicore_per_vring = max_per_vring;
  cfg->targets.clear();
  for (uint32_t target = 0; target < targets; ++target) {
    cfg->targets.push_back(tm_make_ring_target_cfg(
        target, targets, true, tm_bus_interleave_type_t::LINEAR,
        512, 128, 6, 0, 128, 4));
  }
  return cfg;
}

TEST(TmRingMultiRingTopologyTest, BalancesEighteenMastersAcrossThreeRings) {
  TmRingTopology topology;
  topology.config(make_multiring_cfg(18, 4, 8));

  ASSERT_EQ(uint32_t(3), topology.v_ring_count());
  EXPECT_EQ(uint32_t(7), topology.v_ring_station_count(0));
  EXPECT_EQ(uint32_t(7), topology.v_ring_station_count(1));
  EXPECT_EQ(uint32_t(7), topology.v_ring_station_count(2));
  EXPECT_EQ(uint32_t(11), topology.h_ring_station_count());

  for (uint32_t ring = 0; ring < 3; ++ring) {
    const TmRingLocation location = topology.rbrg_v_location(ring);
    EXPECT_EQ(TmRingDomainType::V_RING, location.ring_type);
    EXPECT_EQ(ring, location.ring_id);
    EXPECT_EQ(uint32_t(0), location.station_id);
  }

  for (uint32_t master = 0; master < 18; ++master) {
    const TmRingLocation location = topology.master_location(master);
    EXPECT_EQ(TmRingDomainType::V_RING, location.ring_type);
    EXPECT_EQ(master / 6, location.ring_id);
    EXPECT_EQ(master % 6 + 1, location.station_id);
  }
}

TEST(TmRingMultiRingTopologyTest, PlacesL2StationsBetweenSideHomeAgents) {
  TmRingTopology topology;
  topology.config(make_multiring_cfg(8, 4, 4));

  const uint32_t ha_stations[] = {1, 4, 6, 9};
  const uint32_t l2_stations[] = {2, 3, 7, 8};
  for (uint32_t target = 0; target < 4; ++target) {
    EXPECT_EQ(ha_stations[target], topology.ha_location(target).station_id);
    EXPECT_EQ(l2_stations[target], topology.l2_location(target).station_id);
  }
}

TEST(TmRingMultiRingTopologyTest, PlacesHringStationsAndRoutesLocations) {
  TmRingTopology topology;
  topology.config(make_multiring_cfg(18, 4, 8));

  const uint32_t rbrg_stations[] = {0, 3, 7};
  const uint32_t ha_stations[] = {1, 5, 6, 10};
  const uint32_t l2_stations[] = {2, 4, 8, 9};
  for (uint32_t ring = 0; ring < 3; ++ring) {
    const TmRingLocation location = topology.rbrg_h_location(ring);
    EXPECT_EQ(TmRingDomainType::H_RING, location.ring_type);
    EXPECT_EQ(uint32_t(0), location.ring_id);
    EXPECT_EQ(rbrg_stations[ring], location.station_id);
  }
  for (uint32_t target = 0; target < 4; ++target) {
    EXPECT_EQ(ha_stations[target], topology.ha_location(target).station_id);
    EXPECT_EQ(l2_stations[target], topology.l2_location(target).station_id);
  }

  EXPECT_EQ(TmRingPortDir::CW,
            topology.route_direction(TmRingLocation(TmRingDomainType::H_RING,
                                                    0, 1),
                                     TmRingLocation(TmRingDomainType::H_RING,
                                                    0, 6)));
  EXPECT_EQ(TmRingPortDir::CCW,
            topology.route_direction(TmRingLocation(TmRingDomainType::H_RING,
                                                    0, 1),
                                     TmRingLocation(TmRingDomainType::H_RING,
                                                    0, 8)));
  EXPECT_THROW(topology.route_direction(topology.master_location(0),
                                         topology.master_location(6)),
               std::invalid_argument);
}

TEST(TmRingMultiRingTopologyTest, ChoosesCwForEqualDistance) {
  TmRingTopology topology;
  topology.config(make_multiring_cfg(3, 1, 8));

  EXPECT_EQ(TmRingPortDir::CW,
            topology.route_direction(TmRingLocation(TmRingDomainType::V_RING,
                                                    0, 1),
                                     TmRingLocation(TmRingDomainType::V_RING,
                                                    0, 3)));
}

TEST(TmRingMultiRingTopologyTest, MeasuresFanoutSpanInBothDirections) {
  TmRingTopology topology;
  topology.config(make_multiring_cfg(4, 1, 8));

  const TmRingLocation source = topology.rbrg_v_location(0);
  std::vector<TmRingLocation> recipients;
  recipients.push_back(topology.master_location(0));
  recipients.push_back(topology.master_location(1));
  EXPECT_EQ(uint32_t(2),
            topology.fanout_span(source, recipients, TmRingPortDir::CW));
  EXPECT_EQ(uint32_t(4),
            topology.fanout_span(source, recipients, TmRingPortDir::CCW));

  recipients.push_back(topology.master_location(2));
  recipients.push_back(topology.master_location(3));
  EXPECT_EQ(uint32_t(4),
            topology.fanout_span(source, recipients, TmRingPortDir::CW));
  EXPECT_EQ(uint32_t(4),
            topology.fanout_span(source, recipients, TmRingPortDir::CCW));
  EXPECT_THROW(
      topology.fanout_span(source, recipients, TmRingPortDir::LOCAL),
      std::invalid_argument);

  TmRingTopology split_topology;
  split_topology.config(make_multiring_cfg(4, 1, 2));
  std::vector<TmRingLocation> cross_ring_recipients;
  cross_ring_recipients.push_back(split_topology.master_location(2));
  EXPECT_THROW(split_topology.fanout_span(
                   split_topology.rbrg_v_location(0),
                   cross_ring_recipients, TmRingPortDir::CW),
               std::invalid_argument);
}

class TmRingMultiRingFabricTest : public ::testing::Test {
 protected:
  struct RunResult {
    p_tm_ring_fabric_t fabric = nullptr;
    TmRingTopology topology;
    std::vector<TmRingPerfMasterStats> master_stats;
    TmRingPerfResult perf_result;
    bool idle = false;
  };

  RunResult run_unicast_read_write() {
    std::vector<std::vector<TmRingPerfTxn>> traces(4);
    TmRingPerfTxn read_m0;
    read_m0.master_port = 0;
    read_m0.cmd = PldCmd::RD;
    read_m0.addr = 0x0000;
    read_m0.size = 128;
    traces[0].push_back(read_m0);

    TmRingPerfTxn read_m3;
    read_m3.master_port = 3;
    read_m3.cmd = PldCmd::RD;
    read_m3.addr = 0x0200;
    read_m3.size = 128;
    traces[3].push_back(read_m3);

    TmRingPerfTxn write_m1;
    write_m1.master_port = 1;
    write_m1.cmd = PldCmd::WR;
    write_m1.addr = 0x0400;
    write_m1.size = 128;
    traces[1].push_back(write_m1);

    return run_fabric("multiring_unicast_read_write", 0, traces);
  }

  RunResult run_cross_vring_fanout() {
    std::vector<std::vector<TmRingPerfTxn>> traces(4);
    const uint32_t master_count = static_cast<uint32_t>(traces.size());
    for (uint32_t master_id = 0; master_id < master_count; ++master_id) {
      for (uint32_t line = 0; line < 2; ++line) {
        TmRingPerfTxn read;
        read.master_port = master_id;
        read.cmd = PldCmd::RD;
        read.addr = line * 512;
        read.size = 128;
        read.ordinal = line;
        traces[master_id].push_back(read);
      }
    }
    return run_fabric("cross_vring_fanout", 100, traces, true);
  }

  RunResult run_fabric(
      const std::string& scenario_name, uint32_t l2_hit_rate_pct,
      const std::vector<std::vector<TmRingPerfTxn>>& traces,
      bool coordinate_waves = false) {
    RunResult result;
    const std::string config_path = "../etc/pem_config_cloud.toml";
    std::ifstream config_file(config_path.c_str());
    if (!config_file.good()) {
      ADD_FAILURE() << "pem_config_cloud.toml is not available";
      return result;
    }

    tm_init();
    p_tm_clk_t clk = tm_make_clk();
    std::shared_ptr<cfg::Cfg> scenario_cfg(new cfg::Cfg());
    scenario_cfg->read_cfg_file(config_path);
    cfg::p_cfg_t cfg = scenario_cfg;
    p_tm_ring_cfg_t ring_cfg = tm_make_ring_cfg("multiring_fabric_test", cfg);
    const uint32_t master_count = static_cast<uint32_t>(traces.size());
    ring_cfg->num_masters = master_count;
    ring_cfg->max_aicore_per_vring = 2;
    ring_cfg->l2_traffic.hit_rate_pct = l2_hit_rate_pct;
    if (ring_cfg->targets.empty()) {
      ADD_FAILURE() << "Ring test requires one target";
      return result;
    }
    ring_cfg->targets.resize(1);
    ring_cfg->targets[0]->is_default = true;
    ring_cfg->targets[0]->interleave_num = 1;
    ring_cfg->targets[0]->interleave_idx = 0;

    p_tm_mem_cfg_t mem_cfg =
        tm_make_mem_cfg(ring_cfg->targets[0]->name, cfg);
    p_tm_mem_t memory = tm_make_mem(clk, mem_cfg);
    p_tm_ring_fabric_t fabric = tm_make_ring(clk, ring_cfg);
    fabric->build();

    auto biu_cfg = cfg->get_cfg_tab("BIU");
    std::vector<p_pem_biu_t> bius;
    std::vector<std::shared_ptr<TmRingPerfMaster>> masters;
    std::shared_ptr<TmRingPerfWaveCoordinator> wave_coordinator;
    if (coordinate_waves) {
      wave_coordinator =
          std::make_shared<TmRingPerfWaveCoordinator>(master_count);
    }
    for (uint32_t master_id = 0; master_id < master_count; ++master_id) {
      p_pem_biu_t biu(new pem_biu_t(
          scenario_name + "_biu" + std::to_string(master_id), clk, biu_cfg));
      biu->core_id_ = master_id;
      biu->build();
      biu->reset();
      fabric->attach_master(master_id, biu);
      bius.push_back(biu);

      std::shared_ptr<TmRingPerfMaster> master(new TmRingPerfMaster());
      master->config(
          scenario_name + "_master" + std::to_string(master_id), clk,
          master_id, traces[master_id], wave_coordinator);
      master->attach(biu);
      master->build();
      masters.push_back(master);
    }
    fabric->attach_target(0, memory);

    const uint64_t cycle_limit = 20000;
    for (uint64_t cycle = 0; cycle < cycle_limit; ++cycle) {
      bool done = fabric->idle() && memory->idle();
      for (const std::shared_ptr<TmRingPerfMaster>& master : masters) {
        done = done && master->idle();
      }
      for (const p_pem_biu_t& biu : bius) {
        done = done && biu->idle();
      }
      if (done && cycle != 0) {
        break;
      }
      tm_start(1);
    }

    result.fabric = fabric;
    result.topology.config(ring_cfg);
    result.idle = fabric->idle() && memory->idle();
    for (const std::shared_ptr<TmRingPerfMaster>& master : masters) {
      result.idle = result.idle && master->idle();
      result.master_stats.push_back(master->stats());
    }
    for (const p_pem_biu_t& biu : bius) {
      result.idle = result.idle && biu->idle();
    }

    TmRingPerfCase perf_case;
    perf_case.name = scenario_name;
    perf_case.active_masters = static_cast<uint32_t>(traces.size());
    std::vector<TmMemStats> memory_stats(1, memory->stats());
    result.perf_result = tm_ring_collect_perf_result(
        perf_case, result.master_stats, *fabric, memory_stats,
        TmRingPerfEstimate(), TmRingPerfEstimate(), clk->time(), result.idle);
    return result;
  }
};

TEST_F(TmRingMultiRingFabricTest, UnicastReadWriteCrossesOneBridge) {
  const RunResult result = run_unicast_read_write();

  ASSERT_NE(nullptr, result.fabric);
  ASSERT_EQ(size_t(4), result.master_stats.size());
  EXPECT_EQ(uint32_t(2), result.topology.v_ring_count());
  EXPECT_EQ(uint64_t(1), result.master_stats[0].completed_packets);
  EXPECT_EQ(uint64_t(1), result.master_stats[3].completed_packets);
  EXPECT_EQ(uint64_t(1), result.master_stats[1].completed_packets);
  EXPECT_EQ(uint64_t(0), result.perf_result.protocol_errors);
  EXPECT_TRUE(result.idle);
  EXPECT_TRUE(result.fabric->idle());
  EXPECT_TRUE(result.perf_result.measurement_valid);
  EXPECT_FALSE(result.perf_result.endpoint_queue_stats.empty());
  ASSERT_FALSE(result.perf_result.ring_domain_stats.empty());
  EXPECT_GT(result.perf_result.ring_domain_stats[0].directed_edge_count,
            uint32_t(0));

  const TmRingPmuSnapshot rbrg_pmu = result.fabric->snapshot_pmu(
      result.perf_result.measurement_end_time);
  const std::vector<TmRingHaSourceStats>& ha_sources = rbrg_pmu.ha.sources;
  ASSERT_EQ(size_t(3), ha_sources.size());
  EXPECT_EQ(uint32_t(0), ha_sources[0].master_id);
  EXPECT_EQ(uint64_t(1), ha_sources[0].rd_packets);
  EXPECT_EQ(uint64_t(0), ha_sources[0].wr_packets);
  EXPECT_EQ(uint32_t(1), ha_sources[1].master_id);
  EXPECT_EQ(uint64_t(0), ha_sources[1].rd_packets);
  EXPECT_EQ(uint64_t(1), ha_sources[1].wr_packets);
  EXPECT_EQ(uint32_t(3), ha_sources[2].master_id);
  EXPECT_EQ(uint64_t(1), ha_sources[2].rd_packets);
  EXPECT_EQ(uint64_t(0), ha_sources[2].wr_packets);

  for (uint32_t ring = 0; ring < 2; ++ring) {
    const TmRingRbrgPath paths[] = {
        TmRingRbrgPath::V_TO_H_REQ,
        TmRingRbrgPath::V_TO_H_DAT,
        TmRingRbrgPath::H_TO_V_RSP,
        TmRingRbrgPath::H_TO_V_DAT,
    };
    const uint64_t v_to_h_packets =
        rbrg_pmu.rbrg_path_stats(ring, TmRingRbrgPath::V_TO_H_REQ).packets +
        rbrg_pmu.rbrg_path_stats(ring, TmRingRbrgPath::V_TO_H_DAT).packets;
    const uint64_t h_to_v_packets =
        rbrg_pmu.rbrg_path_stats(ring, TmRingRbrgPath::H_TO_V_RSP).packets +
        rbrg_pmu.rbrg_path_stats(ring, TmRingRbrgPath::H_TO_V_DAT).packets;
    EXPECT_GT(v_to_h_packets, uint64_t(0));
    EXPECT_GT(h_to_v_packets, uint64_t(0));
    for (TmRingRbrgPath path : paths) {
      const TmRingRbrgPathStats& path_stats =
          rbrg_pmu.rbrg_path_stats(ring, path);
      if (path_stats.packets != 0) {
        EXPECT_GT(path_stats.busy_cycles, uint64_t(0));
      }
    }
  }
}

TEST_F(TmRingMultiRingFabricTest,
       CrossVringFanoutUsesOneCarrierPerVring) {
  const RunResult result = run_cross_vring_fanout();

  ASSERT_NE(nullptr, result.fabric);
  ASSERT_EQ(size_t(4), result.master_stats.size());
  uint64_t logical_read_responses = 0;
  for (const TmRingPerfMasterStats& stats : result.master_stats) {
    logical_read_responses += stats.completed_packets;
  }

  const TmRingPmuSnapshot rbrg_pmu = result.fabric->snapshot_pmu(
      result.perf_result.measurement_end_time);
  const TmRingHomeAgentStats& home_agent_stats = rbrg_pmu.ha.total;
  const uint64_t backend_or_functional_line_reads =
      home_agent_stats.rd_backend_issued + home_agent_stats.functional_reads;
  const uint64_t l2_h_ring_carriers =
      result.fabric->l2_buffer_stats().h_carriers;
  uint64_t rbrg_v_ring_dat_carriers = 0;
  for (uint32_t ring = 0; ring < 2; ++ring) {
    rbrg_v_ring_dat_carriers +=
        rbrg_pmu.rbrg_path_stats(ring, TmRingRbrgPath::H_TO_V_DAT).packets;
  }

  EXPECT_EQ(uint64_t(8), logical_read_responses);
  EXPECT_EQ(uint64_t(2), backend_or_functional_line_reads);
  EXPECT_EQ(uint64_t(4), l2_h_ring_carriers);
  EXPECT_EQ(uint64_t(4), rbrg_v_ring_dat_carriers);

  uint32_t v_ring_domains = 0;
  for (const TmRingDomainStats& domain :
       result.perf_result.ring_domain_stats) {
    if (domain.type != TmRingDomainType::V_RING) {
      continue;
    }
    ++v_ring_domains;
    const uint32_t dat = static_cast<uint32_t>(TmRingSubnet::DAT);
    EXPECT_GT(domain.cw[dat].busy_cycles, uint64_t(0));
    EXPECT_GT(domain.ccw[dat].busy_cycles, uint64_t(0));
  }
  EXPECT_EQ(uint32_t(2), v_ring_domains);
  EXPECT_EQ(uint64_t(0), result.perf_result.protocol_errors);
  EXPECT_TRUE(result.idle);
  EXPECT_TRUE(result.fabric->idle());
}

}  // namespace
