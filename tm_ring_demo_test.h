#ifndef _TM_RING_DEMO_TEST_H_
#define _TM_RING_DEMO_TEST_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "cfg.h"
#include "pem_biu.h"
#include "pem_trdemo.h"
#include "tm_engine.h"
#include "tm_mem.h"
#include "tm_ring.h"
#include "types.h"

namespace tm_ring_demo {

using namespace tm_engine;

constexpr uint64_t kDemoSrcAddr = 0x30000000ull;
constexpr uint64_t kDemoDstAddr = 0x40000000ull;
constexpr const char* kDefaultRingScenarioConfig =
    "../etc/pem_config_cloud.toml";

struct TmRingDemoCfg {
  std::string case_name = "multi_core";
  uint32_t uops_per_master = 256;
  uint32_t cycles = 200000;
  double clock_ghz = 1.0;
  double performance_target_pct = 80.0;
  bool require_performance_target = false;
  bool detailed_output = false;
};

inline bool preload_demo_data(const std::vector<p_tm_mem_t>& targets,
                              uint64_t addr, uint32_t bytes) {
  std::vector<uint8_t> data(bytes, 1);
  bool success = true;
  for (auto& target : targets) {
    success = target->pv_write(addr, bytes, data.data(), false) && success;
  }
  return success;
}

inline p_isa_t make_demo_instr(uint64_t src_addr, uint64_t dst_addr,
                               uint32_t uops) {
  auto instr = std::make_shared<isa_t>();
  instr->get_binfo()->name_ = isa_name_t::ZADD;
  instr->start_addr_ = src_addr;
  instr->end_addr_ = dst_addr;
  instr->number_size_ = uops;
  return instr;
}

inline uint32_t demo_cacheline_size(const p_tm_ring_cfg_t& ring_cfg) {
  return ring_cfg->targets[0]->interleave_size == 0
             ? BAND_WIDTH
             : ring_cfg->targets[0]->interleave_size;
}

inline uint32_t demo_sector_size(const p_tm_ring_cfg_t& ring_cfg) {
  return ring_cfg->targets[0]->sector_size == 0
             ? BAND_WIDTH
             : ring_cfg->targets[0]->sector_size;
}

inline uint32_t demo_core_burst_bytes(const p_tm_ring_cfg_t& ring_cfg) {
  return demo_sector_size(ring_cfg);
}

inline uint32_t demo_access_stride_bytes(const p_tm_ring_cfg_t& ring_cfg) {
  return demo_cacheline_size(ring_cfg);
}

inline uint64_t demo_master_base(uint64_t base_addr,
                                 const p_tm_ring_cfg_t& ring_cfg,
                                 uint32_t master) {
  (void)ring_cfg;
  (void)master;
  return base_addr;
}

inline uint32_t demo_write_stride_bytes(
    const p_tm_ring_cfg_t& ring_cfg) {
  return demo_cacheline_size(ring_cfg);
}

inline uint64_t demo_write_region_size(
    const p_tm_ring_cfg_t& ring_cfg, uint32_t uops_per_master) {
  return static_cast<uint64_t>(uops_per_master / 2) *
         demo_write_stride_bytes(ring_cfg);
}

inline uint64_t demo_master_write_base(
    uint64_t base_addr, const p_tm_ring_cfg_t& ring_cfg,
    uint32_t uops_per_master, uint32_t master) {
  return base_addr + static_cast<uint64_t>(master) *
      demo_write_region_size(ring_cfg, uops_per_master);
}

inline uint64_t demo_transfer_span(const p_tm_ring_cfg_t& ring_cfg,
                                   uint32_t uops) {
  if (uops == 0) {
    return 0;
  }
  return static_cast<uint64_t>(uops - 1) * demo_access_stride_bytes(ring_cfg) +
         demo_core_burst_bytes(ring_cfg);
}

inline uint64_t target_home_line(const p_tm_ring_target_cfg_t& target,
                                 uint64_t addr) {
  return (addr - target->addr_begin) / target->interleave_size;
}

inline uint64_t target_home_slice(const p_tm_ring_target_cfg_t& target,
                                  uint64_t addr) {
  uint64_t home_line = target_home_line(target, addr);
  if (target->interleave_type == tm_bus_interleave_type_t::XOR_HASH) {
    const uint64_t hashed = (home_line ^ target->interleave_hash_seed) ^
                            (home_line >> target->interleave_hash_shift) ^
                            (home_line >>
                             (target->interleave_hash_shift * 2));
    return hashed % target->interleave_num;
  }
  return home_line % target->interleave_num;
}

inline uint32_t target_for_address(const p_tm_ring_cfg_t& ring_cfg,
                                   uint64_t addr) {
  uint32_t default_target = 0;
  bool has_default = false;

  for (uint32_t i = 0; i < ring_cfg->targets.size(); ++i) {
    const auto& target = ring_cfg->targets[i];
    if (target->is_default) {
      default_target = i;
      has_default = true;
      continue;
    }
    if (!target->contains(addr)) {
      continue;
    }
    if (!target->interleave_enabled()) {
      return i;
    }

    if (target_home_slice(target, addr) == target->interleave_idx) {
      return i;
    }
  }
  return has_default ? default_target : 0;
}

inline void check_interleave_policy(const p_tm_ring_cfg_t& ring_cfg,
                                    std::vector<std::string>* failures) {
  if (ring_cfg->targets.empty() ||
      !ring_cfg->targets[0]->interleave_enabled()) {
    return;
  }

  const uint32_t cacheline_size = demo_cacheline_size(ring_cfg);
  const uint32_t sector_size = demo_sector_size(ring_cfg);
  if (cacheline_size == 0 || sector_size == 0 ||
      cacheline_size % sector_size != 0) {
    failures->push_back("invalid cacheline/sector interleave geometry");
    return;
  }

  const uint64_t line_base =
      (kDemoSrcAddr / cacheline_size) * cacheline_size;
  const uint32_t home_target = target_for_address(ring_cfg, line_base);
  for (uint32_t sector = 0; sector < cacheline_size;
       sector += sector_size) {
    const uint64_t addr = line_base + sector;
    const uint32_t target_id = target_for_address(ring_cfg, addr);
    if (target_id != home_target) {
      std::ostringstream os;
      os << "interleave split one cacheline: addr=0x" << std::hex << addr
         << " home_target=" << std::dec << home_target
         << " target=" << target_id
         << " cacheline_size=" << cacheline_size
         << " sector_size=" << sector_size;
      failures->push_back(os.str());
      return;
    }
  }
}

inline const char* interleave_name(const p_tm_ring_cfg_t& ring_cfg) {
  if (ring_cfg->targets.empty() ||
      !ring_cfg->targets[0]->interleave_enabled()) {
    return "none";
  }
  return ring_cfg->targets[0]->interleave_type ==
                 tm_bus_interleave_type_t::XOR_HASH
             ? "xor"
             : "linear";
}

inline const char* interleave_name_cn(const p_tm_ring_cfg_t& ring_cfg) {
  if (ring_cfg->targets.empty() ||
      !ring_cfg->targets[0]->interleave_enabled()) {
    return "无交织";
  }
  return ring_cfg->targets[0]->interleave_type ==
                 tm_bus_interleave_type_t::XOR_HASH
             ? "异或哈希交织"
             : "线性交织";
}

inline const char* bottleneck_name_cn(const std::string& bottleneck) {
  if (bottleneck == "ring_conn_serialization_busy") {
    return "Ring链路串行化忙";
  }
  if (bottleneck == "ring_conn_pipeline_full") {
    return "Ring链路在途包达到上限";
  }
  if (bottleneck == "ring_conn_downstream_register_full") {
    return "下游逐拍寄存器忙";
  }
  return "无明显瓶颈";
}

inline uint32_t expected_demo_result(uint32_t read_payload_bytes = BAND_WIDTH) {
  constexpr uint32_t kPreloadWord = 0x01010101u;
  const uint64_t words_per_pair =
      2ull * read_payload_bytes / sizeof(uint32_t);
  return static_cast<uint32_t>(words_per_pair * kPreloadWord);
}

struct MemoryCheck {
  uint64_t checked = 0;
  uint64_t mismatches = 0;
  uint64_t read_failures = 0;
};

inline MemoryCheck verify_demo_memory(const std::vector<p_tm_mem_t>& targets,
                                      const p_tm_ring_cfg_t& ring_cfg,
                                      uint32_t uops_per_master,
                                      std::vector<std::string>* failures) {
  MemoryCheck result;
  const uint32_t read_payload_bytes = demo_core_burst_bytes(ring_cfg);
  const uint32_t write_stride_bytes = demo_write_stride_bytes(ring_cfg);
  const uint32_t expected = expected_demo_result(read_payload_bytes);
  const uint32_t pairs_per_master = uops_per_master / 2;
  uint32_t detailed_errors = 0;
  constexpr uint32_t kMaxDetailedErrors = 8;

  for (uint32_t master = 0; master < ring_cfg->num_masters; ++master) {
    const uint64_t dst_base =
        demo_master_write_base(kDemoDstAddr, ring_cfg, uops_per_master,
                               master);
    for (uint32_t pair = 0; pair < pairs_per_master; ++pair) {
      const uint64_t addr =
          dst_base + static_cast<uint64_t>(pair) * write_stride_bytes;
      const uint32_t target_id = target_for_address(ring_cfg, addr);
      uint32_t actual = 0;
      ++result.checked;
      if (!targets[target_id]->pv_read(addr, sizeof(actual),
                                       reinterpret_cast<uint8_t*>(&actual))) {
        ++result.read_failures;
        if (detailed_errors++ < kMaxDetailedErrors) {
          std::ostringstream os;
          os << "memory read failed: master=" << master << " pair=" << pair
             << " target=" << target_id << " addr=0x" << std::hex << addr;
          failures->push_back(os.str());
        }
        continue;
      }
      if (actual != expected) {
        ++result.mismatches;
        if (detailed_errors++ < kMaxDetailedErrors) {
          std::ostringstream os;
          os << "memory mismatch: master=" << master << " pair=" << pair
             << " target=" << target_id << " addr=0x" << std::hex << addr
             << " expected=0x" << expected << " actual=0x" << actual;
          failures->push_back(os.str());
        }
      }
    }
  }
  return result;
}

inline void check_count(std::vector<std::string>* failures, uint32_t master,
                        const char* name, uint64_t actual, uint64_t expected) {
  if (actual == expected) {
    return;
  }
  std::ostringstream os;
  os << "master " << master << " " << name << ": expected=" << expected
     << " actual=" << actual;
  failures->push_back(os.str());
}

inline uint64_t latency_min_or_zero(uint64_t count, uint64_t value) {
  return count == 0 ? 0 : value;
}

inline uint64_t distribution_entry_count(
    const std::array<uint64_t, 65>& buckets) {
  uint64_t total = 0;
  for (size_t bucket = 1; bucket < buckets.size(); ++bucket) {
    total += buckets[bucket];
  }
  return total;
}

inline uint64_t distribution_weighted_count(
    const std::array<uint64_t, 65>& buckets) {
  uint64_t total = 0;
  for (size_t bucket = 1; bucket < buckets.size(); ++bucket) {
    total += static_cast<uint64_t>(bucket) * buckets[bucket];
  }
  return total;
}

inline void print_distribution(
    const std::array<uint64_t, 65>& buckets, std::ostream& output) {
  output << "buckets=";
  bool printed = false;
  for (size_t bucket = 1; bucket < buckets.size(); ++bucket) {
    if (buckets[bucket] == 0) {
      continue;
    }
    if (printed) {
      output << ',';
    }
    output << bucket << ':' << buckets[bucket];
    printed = true;
  }
  if (!printed) {
    output << "none";
  }
}

template <typename DemoT>
inline int print_demo_performance(
    const std::string& config_file, const TmRingDemoCfg& demo_case,
    const p_tm_ring_cfg_t& ring_cfg, const p_tm_mem_cfg_t& ddr_cfg,
    const p_tm_ring_fabric_t& ring, const std::vector<p_pem_biu_t>& bius,
    const std::vector<std::shared_ptr<DemoT>>& demos,
    const std::vector<p_tm_mem_t>& targets,
    std::vector<std::string>* failures) {
  const std::string& case_name = demo_case.case_name;
  const uint32_t uops_per_master = demo_case.uops_per_master;
  const uint32_t cycles = demo_case.cycles;
  const uint32_t cacheline_size = demo_cacheline_size(ring_cfg);
  const uint32_t sector_size = demo_sector_size(ring_cfg);
  const uint32_t read_payload_bytes = demo_core_burst_bytes(ring_cfg);
  const uint32_t read_stride_bytes = demo_access_stride_bytes(ring_cfg);
  const uint32_t write_stride_bytes = demo_write_stride_bytes(ring_cfg);
  const double clock_ghz = demo_case.clock_ghz;
  const double performance_target_pct = demo_case.performance_target_pct;
  const bool require_performance_target = demo_case.require_performance_target;
  const bool detailed_output = demo_case.detailed_output;

  const bool ring_idle = ring->idle();
  bool demo_idle = true;
  bool biu_idle = true;
  bool target_idle = true;
  for (auto& demo : demos) {
    demo_idle = demo_idle && demo->idle();
  }
  for (auto& biu : bius) {
    biu_idle = biu_idle && biu->idle();
  }
  for (auto& target : targets) {
    target_idle = target_idle && target->idle();
  }
  if (!ring_idle) {
    failures->push_back("ring is not idle at cycle limit");
  }
  if (!demo_idle) {
    failures->push_back("traffic generator is not idle at cycle limit");
  }
  if (!biu_idle) {
    failures->push_back("BIU is not idle at cycle limit");
  }
  if (!target_idle) {
    failures->push_back("target memory is not idle at cycle limit");
  }

  const uint64_t expected_reads = uops_per_master;
  const uint64_t expected_writes = uops_per_master / 2;
  uint64_t total_reads = 0;
  uint64_t total_read_responses = 0;
  uint64_t total_pairs = 0;
  uint64_t total_writes = 0;
  uint64_t total_write_responses = 0;
  uint64_t total_read_bytes = 0;
  uint64_t total_write_bytes = 0;
  uint64_t total_read_stalls = 0;
  uint64_t total_read_response_stalls = 0;
  uint64_t total_write_stalls = 0;
  uint64_t total_write_buffer_stalls = 0;
  uint64_t total_protocol_errors = 0;
  uint64_t total_read_latency = 0;
  uint64_t total_write_latency = 0;
  uint64_t overall_first_cycle = std::numeric_limits<uint64_t>::max();
  uint64_t overall_last_read_cycle = 0;
  uint64_t overall_last_cycle = 0;
  std::vector<double> master_rates;

  std::cout << "TEST_CONFIG case=" << case_name << " config=" << config_file
            << " masters=" << ring_cfg->num_masters
            << " targets=" << ring_cfg->targets.size()
            << " uops_per_master=" << uops_per_master
            << " access_pattern=same_sector_multicast"
            << " cacheline_size=" << cacheline_size
            << " sector_size=" << sector_size
            << " core_burst_bytes=" << read_payload_bytes
            << " read_stride_bytes=" << read_stride_bytes
            << " write_stride_bytes=" << write_stride_bytes
            << " cycle_limit=" << cycles << " clock_ghz=" << clock_ghz
            << std::endl;
  std::cout << "TEST_CONFIG_CN 场景=" << case_name
            << " master数量=" << ring_cfg->num_masters
            << " target数量=" << ring_cfg->targets.size()
            << " 每个master微操作数=" << uops_per_master
            << " 访问模式=多core同一sector多播"
            << " cacheline字节=" << cacheline_size
            << " sector字节=" << sector_size
            << " 每core突发字节=" << read_payload_bytes
            << " 读stride字节=" << read_stride_bytes
            << " 写回stride字节=" << write_stride_bytes
            << " 最大运行周期=" << cycles << " 主频GHz=" << clock_ghz
            << std::endl;
  std::cout << "TEST_BUS_CONFIG interleave=" << interleave_name(ring_cfg)
            << " interleave_size=" << ring_cfg->targets[0]->interleave_size
            << " sector_size=" << ring_cfg->targets[0]->sector_size
            << " link_latency=" << ring_cfg->ring_link_latency
            << " slot_pipeline_depth=" << (ring_cfg->ring_link_latency + 1)
            << " link_width=" << ring_cfg->ring_link_width_bytes
            << " v_ring_count="
            << (ring_cfg->num_masters + ring_cfg->max_aicore_per_vring - 1) /
                   ring_cfg->max_aicore_per_vring
            << " max_aicore_per_vring=" << ring_cfg->max_aicore_per_vring
            << " rbrg_count="
            << (ring_cfg->num_masters + ring_cfg->max_aicore_per_vring - 1) /
                   ring_cfg->max_aicore_per_vring
            << " rbrg_queue_depth=" << ring_cfg->rbrg_queue_depth
            << " rbrg_latency=" << ring_cfg->rbrg_latency
            << " rbrg_width_bytes=" << ring_cfg->rbrg_width_bytes
            << " physical_subnets=req,rsp,dat"
            << " physical_lanes_per_subnet=1"
            << " rd_rsp_port_num=" << ring_cfg->rd_rsp_port_num
            << " target_width=" << ring_cfg->targets[0]->width
            << " target_latency="
            << (ring_cfg->targets[0]->frontend_latency +
                ring_cfg->targets[0]->forward_latency +
                ring_cfg->targets[0]->response_latency +
                ring_cfg->targets[0]->header_latency)
            << " ddr_read_latency=" << ddr_cfg->ddr->min_rd_lat
            << " ddr_write_latency=" << ddr_cfg->ddr->min_wr_lat
            << " tm_mem_rd_credit=" << ddr_cfg->ddr->max_rd_crdt
            << " tm_mem_wr_credit=" << ddr_cfg->ddr->max_wr_crdt
            << " tm_mem_acc_credit=" << ddr_cfg->ddr->max_acc_crdt
            << std::endl;
  std::cout << "TEST_BUS_CONFIG_CN 交织方式=" << interleave_name_cn(ring_cfg)
            << " home交织粒度字节=" << ring_cfg->targets[0]->interleave_size
            << " sector字节=" << ring_cfg->targets[0]->sector_size
            << " 链路延迟周期=" << ring_cfg->ring_link_latency
            << " slot流水深度=" << (ring_cfg->ring_link_latency + 1)
            << " 链路宽度字节=" << ring_cfg->ring_link_width_bytes
            << " 物理subnet=req,rsp,dat"
            << " 每个subnet物理lane数=1"
            << " 逻辑RD_RSP通道数=" << ring_cfg->rd_rsp_port_num
            << " target接口宽度字节=" << ring_cfg->targets[0]->width
            << " target总延迟周期="
            << (ring_cfg->targets[0]->frontend_latency +
                ring_cfg->targets[0]->forward_latency +
                ring_cfg->targets[0]->response_latency +
                ring_cfg->targets[0]->header_latency)
            << " DDR读延迟周期=" << ddr_cfg->ddr->min_rd_lat
            << " DDR写延迟周期=" << ddr_cfg->ddr->min_wr_lat
            << " TmMem读credit=" << ddr_cfg->ddr->max_rd_crdt
            << " TmMem写credit=" << ddr_cfg->ddr->max_wr_crdt
            << " TmMem访问credit=" << ddr_cfg->ddr->max_acc_crdt
            << std::endl;

  for (uint32_t master = 0; master < ring_cfg->num_masters; ++master) {
    const auto& stat = demos[master]->traffic_stats();
    check_count(failures, master, "read_requests", stat.read_requests,
                expected_reads);
    check_count(failures, master, "read_responses", stat.read_responses,
                expected_reads);
    check_count(failures, master, "completed_pairs", stat.completed_pairs,
                expected_writes);
    check_count(failures, master, "write_requests", stat.write_requests,
                expected_writes);
    check_count(failures, master, "write_responses", stat.write_responses,
                expected_writes);
    if (stat.protocol_errors != 0) {
      std::ostringstream os;
      os << "master " << master << " protocol_errors=" << stat.protocol_errors;
      failures->push_back(os.str());
    }

    uint64_t active_cycles = 0;
    if (stat.has_first_read_cycle) {
      overall_first_cycle =
          std::min(overall_first_cycle, stat.first_read_cycle);
    }
    if (stat.has_last_read_response_cycle) {
      overall_last_read_cycle =
          std::max(overall_last_read_cycle, stat.last_read_response_cycle);
    }
    uint64_t last_response_cycle = 0;
    bool has_last_response_cycle = false;
    if (stat.has_last_read_response_cycle) {
      last_response_cycle =
          std::max(last_response_cycle, stat.last_read_response_cycle);
      has_last_response_cycle = true;
    }
    if (stat.has_last_write_response_cycle) {
      last_response_cycle =
          std::max(last_response_cycle, stat.last_write_response_cycle);
      has_last_response_cycle = true;
    }
    if (stat.has_first_read_cycle && has_last_response_cycle &&
        last_response_cycle >= stat.first_read_cycle) {
      active_cycles = last_response_cycle - stat.first_read_cycle + 1;
      overall_last_cycle = std::max(overall_last_cycle, last_response_cycle);
    }
    const uint64_t payload_bytes = stat.read_responses * read_payload_bytes +
                                   stat.write_responses * sizeof(uint32_t);
    const double payload_bpc =
        active_cycles == 0 ? 0.0
                           : static_cast<double>(payload_bytes) / active_cycles;
    master_rates.push_back(payload_bpc);

    const double read_avg =
        stat.read_responses == 0
            ? 0.0
            : static_cast<double>(stat.read_latency_sum) / stat.read_responses;
    const double write_avg = stat.write_responses == 0
                                 ? 0.0
                                 : static_cast<double>(stat.write_latency_sum) /
                                       stat.write_responses;
    if (detailed_output) {
      std::cout << std::fixed << std::setprecision(3)
                << "MASTER_PERF master=" << master
                << " active_cycles=" << active_cycles
                << " reads=" << stat.read_responses
                << " writes=" << stat.write_responses
                << " payload_bytes_per_cycle=" << payload_bpc
                << " read_latency_avg=" << read_avg << " read_latency_min="
                << latency_min_or_zero(stat.read_responses,
                                       stat.read_latency_min)
                << " read_latency_max=" << stat.read_latency_max
                << " write_latency_avg=" << write_avg << " write_latency_min="
                << latency_min_or_zero(stat.write_responses,
                                       stat.write_latency_min)
                << " write_latency_max=" << stat.write_latency_max
                << " endpoint_stalls="
                << (stat.read_send_stalls + stat.read_response_stalls +
                    stat.write_send_stalls + stat.write_buffer_stalls)
                << std::endl;
    }

    total_reads += stat.read_requests;
    total_read_responses += stat.read_responses;
    total_pairs += stat.completed_pairs;
    total_writes += stat.write_requests;
    total_write_responses += stat.write_responses;
    // Throughput is based on completed responses. Counting issued bytes
    // makes an incomplete/deadlocked run look faster than the link peak.
    total_read_bytes += stat.read_responses * read_payload_bytes;
    total_write_bytes += stat.write_responses * sizeof(uint32_t);
    total_read_stalls += stat.read_send_stalls;
    total_read_response_stalls += stat.read_response_stalls;
    total_write_stalls += stat.write_send_stalls;
    total_write_buffer_stalls += stat.write_buffer_stalls;
    total_protocol_errors += stat.protocol_errors;
    total_read_latency += stat.read_latency_sum;
    total_write_latency += stat.write_latency_sum;
  }

  const MemoryCheck memory =
      verify_demo_memory(targets, ring_cfg, uops_per_master, failures);
  if (memory.read_failures != 0 || memory.mismatches != 0) {
    std::ostringstream os;
    os << "memory verification failed: read_failures=" << memory.read_failures
       << " mismatches=" << memory.mismatches;
    failures->push_back(os.str());
  }

  const uint64_t completion_cycles =
      overall_first_cycle == std::numeric_limits<uint64_t>::max() ||
              overall_last_cycle < overall_first_cycle
          ? 0
          : overall_last_cycle - overall_first_cycle + 1;
  const uint64_t read_completion_cycles =
      overall_first_cycle == std::numeric_limits<uint64_t>::max() ||
              overall_last_read_cycle < overall_first_cycle
          ? 0
          : overall_last_read_cycle - overall_first_cycle + 1;
  const double read_bpc =
      read_completion_cycles == 0
          ? 0.0
          : static_cast<double>(total_read_bytes) / read_completion_cycles;
  const double write_bpc =
      completion_cycles == 0
          ? 0.0
          : static_cast<double>(total_write_bytes) / completion_cycles;
  const double total_bpc =
      completion_cycles == 0
          ? 0.0
          : static_cast<double>(total_read_bytes + total_write_bytes) /
                completion_cycles;
  const double pair_ops_per_cycle =
      completion_cycles == 0
          ? 0.0
          : static_cast<double>(total_pairs) / completion_cycles;
  const double read_latency_avg =
      total_read_responses == 0
          ? 0.0
          : static_cast<double>(total_read_latency) / total_read_responses;
  const double write_latency_avg =
      total_write_responses == 0
          ? 0.0
          : static_cast<double>(total_write_latency) / total_write_responses;

  double rate_sum = 0.0;
  double rate_square_sum = 0.0;
  for (double rate : master_rates) {
    rate_sum += rate;
    rate_square_sum += rate * rate;
  }
  const double fairness =
      rate_square_sum == 0.0
          ? 0.0
          : rate_sum * rate_sum / (master_rates.size() * rate_square_sum);
  const uint64_t endpoint_stalls =
      total_read_stalls + total_read_response_stalls + total_write_stalls +
      total_write_buffer_stalls;
  const TmRingPmuSnapshot ring_pmu = ring->snapshot_pmu(clk->time());
  const TmRingConnStallBreakdown ring_conn_breakdown =
      ring_pmu.conn_stall_breakdown();
  const auto& csstats = ring_pmu.cross_station.total;
  const auto& home_agent_stats = ring_pmu.ha.total;
  const auto l2_buffer_stats = ring->l2_buffer_stats();
  const std::vector<TmRingDomainStats> ring_domain_stats =
      ring_pmu.conn.domains;
  const auto& rbrg_stats = ring_pmu.rbrg.instances;
  const auto& rbrg_instance_ids = ring_pmu.rbrg.instance_ids;
  const uint64_t l2_classified_transactions =
      home_agent_stats.l2_hit_transactions +
      home_agent_stats.l2_miss_transactions;
  const double observed_l2_hit_rate_pct =
      l2_classified_transactions == 0
          ? 0.0
          : 100.0 * static_cast<double>(home_agent_stats.l2_hit_transactions) /
                static_cast<double>(l2_classified_transactions);
  const uint64_t completed_ha_transactions = distribution_entry_count(
      home_agent_stats.completed_transaction_waiters);
  const uint64_t completed_ha_waiters = distribution_weighted_count(
      home_agent_stats.completed_transaction_waiters);
  const uint64_t h_carriers = l2_buffer_stats.h_carriers;
  const uint64_t h_carrier_recipients =
      l2_buffer_stats.h_carrier_recipients;
  const double average_ha_waiters =
      completed_ha_transactions == 0
          ? 0.0
          : static_cast<double>(completed_ha_waiters) /
                static_cast<double>(completed_ha_transactions);
  const double average_h_carrier_recipients =
      h_carriers == 0
          ? 0.0
          : static_cast<double>(h_carrier_recipients) /
                static_cast<double>(h_carriers);
  const double physical_packet_reduction_pct =
      h_carrier_recipients == 0
          ? 0.0
          : 100.0 *
                (1.0 - static_cast<double>(h_carriers) /
                           static_cast<double>(h_carrier_recipients));
  const uint64_t ring_conn_stalls = ring_conn_breakdown.total();
  const uint64_t connection_stalls = ring_conn_stalls;
  const uint64_t all_stalls = endpoint_stalls + connection_stalls;
  if (case_name == "multi_core_backpressure" && all_stalls == 0) {
    failures->push_back(
        "backpressure case completed without exercising a stall path");
  }
  const char* dominant_bottleneck = "none";
  uint64_t dominant_stalls = 0;
  if (ring_conn_stalls > dominant_stalls) {
    dominant_stalls = ring_conn_stalls;
    dominant_bottleneck = "ring_conn_serialization_busy";
    uint64_t dominant_conn_stalls =
        ring_conn_breakdown.serialization_busy;
    if (ring_conn_breakdown.pipeline_full >
        dominant_conn_stalls) {
      dominant_conn_stalls = ring_conn_breakdown.pipeline_full;
      dominant_bottleneck = "ring_conn_pipeline_full";
    }
    if (ring_conn_breakdown.downstream_register_full >
        dominant_conn_stalls) {
      dominant_bottleneck = "ring_conn_downstream_register_full";
    }
  }

  const uint32_t parallel_paths = std::min(
      ring_cfg->num_masters, static_cast<uint32_t>(ring_cfg->targets.size()));
  const uint32_t dat_effective_path_width =
      std::min(ring_cfg->ring_link_width_bytes, ring_cfg->targets[0]->width);
  const double estimated_peak_bpc =
      static_cast<double>(parallel_paths) * dat_effective_path_width;
  const double utilization_pct =
      estimated_peak_bpc == 0.0 ? 0.0 : 100.0 * read_bpc / estimated_peak_bpc;
  const uint64_t expected_total_reads = expected_reads * ring_cfg->num_masters;
  const uint64_t expected_total_writes =
      expected_writes * ring_cfg->num_masters;
  const bool measurement_valid =
      total_reads == expected_total_reads &&
      total_read_responses == expected_total_reads &&
      total_pairs == expected_total_writes &&
      total_writes == expected_total_writes &&
      total_write_responses == expected_total_writes &&
      total_protocol_errors == 0 && ring_idle && demo_idle && biu_idle &&
      target_idle;
  const bool performance_target_met =
      measurement_valid && utilization_pct >= performance_target_pct;
  if (require_performance_target && !performance_target_met) {
    std::ostringstream os;
    os << "performance target missed: target=" << performance_target_pct
       << "% actual=" << utilization_pct << "%";
    failures->push_back(os.str());
  }
  const bool expect_home_agent_merge =
      ring_cfg->enable_home_agent && ring_cfg->num_masters > 1 &&
      read_payload_bytes == sector_size;
  if (expect_home_agent_merge && home_agent_stats.rd_requests > 1 &&
      home_agent_stats.backend_read_saved == 0) {
    failures->push_back(
        "home agent did not merge the same-sector multi-core reads");
  }
  if (expect_home_agent_merge && home_agent_stats.rd_requests > 1 &&
      home_agent_stats.rd_backend_issued >= home_agent_stats.rd_requests) {
    failures->push_back(
        "home agent backend read count was not reduced by coalescing");
  }
  if (ring_cfg->enable_home_agent &&
      ring_cfg->l2_traffic.hit_rate_pct == 100 &&
      home_agent_stats.rd_backend_issued != 0) {
    failures->push_back("100% L2 hit still issued HBM backend reads");
  }
  if (l2_buffer_stats.responses_accepted != 0 && h_carriers == 0) {
    failures->push_back(
        "L2 Buffer accepted responses but injected no H carrier");
  }
  if (expect_home_agent_merge && home_agent_stats.rd_requests > 1 &&
      l2_buffer_stats.h_multicast_carriers == 0) {
    failures->push_back(
        "same-sector multi-core reads did not produce a multicast H carrier");
  }
  if (l2_buffer_stats.h_scatter_carriers != 0) {
    failures->push_back(
        "default same-sector demo unexpectedly produced scatter H carriers");
  }

  std::cout << std::fixed << std::setprecision(3)
            << "TEST_COUNTS read_requests=" << total_reads
            << " read_responses=" << total_read_responses
            << " completed_pairs=" << total_pairs
            << " write_requests=" << total_writes
            << " write_responses=" << total_write_responses
            << " protocol_errors=" << total_protocol_errors << std::endl;
  std::cout << "TEST_HOME_AGENT enabled="
            << (ring_cfg->enable_home_agent ? "yes" : "no")
            << " rd_requests=" << home_agent_stats.rd_requests
            << " rd_entries_allocated="
            << home_agent_stats.rd_entries_allocated
            << " backend_reads=" << home_agent_stats.rd_backend_issued
            << " l2_hit_transactions="
            << home_agent_stats.l2_hit_transactions
            << " l2_miss_transactions="
            << home_agent_stats.l2_miss_transactions
            << " functional_reads=" << home_agent_stats.functional_reads
            << " merged_pending="
            << home_agent_stats.rd_merged_pending
            << " merged_inflight="
            << home_agent_stats.rd_merged_inflight
            << " merged_responding="
            << home_agent_stats.rd_merged_responding
            << " backend_read_saved=" << home_agent_stats.backend_read_saved
            << " table_full_stall_cycles="
            << home_agent_stats.table_full_stall_cycles
            << " waiter_full_stall_cycles="
            << home_agent_stats.waiter_full_stall_cycles
            << " write_hazard_stall_cycles="
            << home_agent_stats.write_hazard_stall_cycles
            << " aggregation_closed_stall_cycles="
            << home_agent_stats.aggregation_closed_stall_cycles
            << " useful_bytes=" << home_agent_stats.useful_bytes
            << " backend_read_bytes=" << home_agent_stats.backend_read_bytes
            << " completion_buffer_bytes_peak="
            << home_agent_stats.completion_buffer_bytes_peak
            << " private_l2_full_stall_cycles="
            << home_agent_stats.private_l2_full_stall_cycles
            << std::endl;
  std::cout << "TEST_HA_WAITER_DIST ";
  print_distribution(home_agent_stats.completed_transaction_waiters, std::cout);
  std::cout << std::endl;
  std::cout << "TEST_L2_CARRIER_DIST bytes_128="
            << l2_buffer_stats.injected_carrier_128b
            << " bytes_256=" << l2_buffer_stats.injected_carrier_256b
            << " bytes_512=" << l2_buffer_stats.injected_carrier_512b
            << " other=" << l2_buffer_stats.injected_carrier_other
            << std::endl;
  const char* subnet_names[] = {"req", "rsp", "dat"};
  const char* direction_names[] = {"cw", "ccw"};
  const char* hotspot_direction_names[] = {"local", "cw", "ccw"};
  for (const TmRingDomainStats& domain : ring_domain_stats) {
    std::cout << "TEST_RING_DOMAIN type="
              << (domain.type == TmRingDomainType::H_RING ? "h" : "v")
              << " id=" << domain.ring_id;
    for (uint32_t subnet = 0; subnet < 3; ++subnet) {
      const TmRingConnStats* directions[] = {&domain.cw[subnet],
                                              &domain.ccw[subnet]};
      for (uint32_t direction = 0; direction < 2; ++direction) {
        const TmRingConnStats& stats = *directions[direction];
        std::cout << ' ' << subnet_names[subnet] << '_' << direction_names[direction]
                  << "_packets=" << stats.packets << ' ' << subnet_names[subnet]
                  << '_' << direction_names[direction] << "_bytes=" << stats.bytes
                  << ' ' << subnet_names[subnet] << '_' << direction_names[direction]
                  << "_busy_cycles=" << stats.busy_cycles << ' '
                  << subnet_names[subnet] << '_' << direction_names[direction]
                  << "_stalls="
                  << (stats.downstream_register_full_stall +
                      stats.serialization_busy_stall + stats.pipeline_full_stall +
                      stats.send_reject_stall);
      }
    }
    std::cout << " hottest_subnet=" << subnet_names[static_cast<uint32_t>(
                     domain.hottest.subnet)]
              << " hottest_src_station=" << domain.hottest.src_station
              << " hottest_src_direction="
              << hotspot_direction_names[static_cast<uint32_t>(
                     domain.hottest.src_dir)]
              << " hottest_dst_station=" << domain.hottest.dst_station
              << " hottest_dst_direction="
              << hotspot_direction_names[static_cast<uint32_t>(
                     domain.hottest.dst_dir)]
              << " hottest_busy_cycles=" << domain.hottest.busy_cycles
              << " hottest_stalls=" << domain.hottest.total_stalls << std::endl;
  }
  const char* rbrg_path_names[] = {"v_to_h_req", "v_to_h_dat",
                                   "h_to_v_rsp", "h_to_v_dat"};
  for (uint32_t rbrg_index = 0; rbrg_index < rbrg_stats.size();
       ++rbrg_index) {
    std::cout << "TEST_RBRG id=" << rbrg_instance_ids.at(rbrg_index);
    for (uint32_t path = 0; path < 4; ++path) {
      const TmRingRbrgPathStats& stats = rbrg_stats[rbrg_index].paths[path];
      std::cout << ' ' << rbrg_path_names[path] << "_packets=" << stats.packets
                << ' ' << rbrg_path_names[path] << "_bytes=" << stats.bytes
                << ' ' << rbrg_path_names[path]
                << "_queue_peak=" << stats.queue_occupancy_peak << ' '
                << rbrg_path_names[path]
                << "_queue_full_stalls=" << stats.queue_full_stalls << ' '
                << rbrg_path_names[path]
                << "_inject_stalls=" << stats.destination_inject_stalls;
    }
    std::cout << std::endl;
  }
  std::cout << "TEST_FANOUT_SUMMARY average_ha_waiters="
            << average_ha_waiters
            << " average_h_carrier_recipients="
            << average_h_carrier_recipients
            << " physical_packet_reduction_pct="
            << physical_packet_reduction_pct
            << " h_carrier_recipients=" << h_carrier_recipients
            << " h_carriers=" << h_carriers << std::endl;
  std::cout << "TEST_FANOUT_SUMMARY_CN HA平均每事务请求数="
            << average_ha_waiters << " H载体平均接收者数="
            << average_h_carrier_recipients << " 物理包减少比例百分比="
            << physical_packet_reduction_pct << " H载体接收者数="
            << h_carrier_recipients << " H载体数=" << h_carriers << std::endl;
  std::cout << "TEST_L2_TRAFFIC configured_hit_rate_pct="
            << ring_cfg->l2_traffic.hit_rate_pct
            << " observed_hit_rate_pct=" << observed_l2_hit_rate_pct
            << " hit_transactions=" << home_agent_stats.l2_hit_transactions
            << " miss_transactions=" << home_agent_stats.l2_miss_transactions
            << " functional_reads=" << home_agent_stats.functional_reads
            << " backend_reads=" << home_agent_stats.rd_backend_issued
            << " l2_buffer_accepted=" << l2_buffer_stats.responses_accepted
            << " h_carriers=" << l2_buffer_stats.h_carriers
            << " h_unicast_carriers="
            << l2_buffer_stats.h_unicast_carriers
            << " h_multicast_carriers="
            << l2_buffer_stats.h_multicast_carriers
            << " h_scatter_carriers="
            << l2_buffer_stats.h_scatter_carriers
            << " h_carrier_recipients="
            << l2_buffer_stats.h_carrier_recipients
            << " l2_buffer_peak=" << l2_buffer_stats.buffer_occupancy_peak
            << " l2_buffer_full_stalls="
            << l2_buffer_stats.buffer_full_stall_cycles
            << " l2_dat_inject_stalls="
            << l2_buffer_stats.dat_inject_full_stall_cycles << std::endl;
  std::cout << "TEST_COUNTS_CN 读请求数=" << total_reads
            << " 读响应数=" << total_read_responses
            << " 完成读写配对数=" << total_pairs
            << " 写请求数=" << total_writes
            << " 写响应数=" << total_write_responses
            << " 协议错误数=" << total_protocol_errors << std::endl;
  std::cout << "TEST_MEMORY checked=" << memory.checked
            << " mismatches=" << memory.mismatches
            << " read_failures=" << memory.read_failures << " expected_value=0x"
            << std::hex << expected_demo_result(read_payload_bytes) << std::dec
            << std::endl;
  std::cout << "TEST_MEMORY_CN 校验地址数=" << memory.checked
            << " 数据不匹配数=" << memory.mismatches
            << " 读取失败数=" << memory.read_failures
            << " 期望值=0x" << std::hex
            << expected_demo_result(read_payload_bytes) << std::dec << std::endl;
  std::cout << "TEST_PERF completion_cycles=" << completion_cycles
            << " read_completion_cycles=" << read_completion_cycles
            << " read_payload_bytes_per_cycle=" << read_bpc
            << " write_payload_bytes_per_cycle=" << write_bpc
            << " total_payload_bytes_per_cycle=" << total_bpc
            << " read_bandwidth_GBps=" << (read_bpc * clock_ghz)
            << " total_payload_bandwidth_GBps=" << (total_bpc * clock_ghz)
            << " pair_ops_per_cycle=" << pair_ops_per_cycle
            << " utilization_pct=" << utilization_pct
            << " target_met=" << (performance_target_met ? "yes" : "no")
            << std::endl;
  std::cout << "TEST_PERF_CN 完成周期=" << completion_cycles
            << " 读完成周期=" << read_completion_cycles
            << " 读吞吐B每周期=" << read_bpc
            << " 写吞吐B每周期=" << write_bpc
            << " 总吞吐B每周期=" << total_bpc
            << " 读带宽GB每秒=" << (read_bpc * clock_ghz)
            << " 总带宽GB每秒=" << (total_bpc * clock_ghz)
            << " 配对操作每周期=" << pair_ops_per_cycle
            << " 利用率百分比=" << utilization_pct
            << " 达标=" << (performance_target_met ? "是" : "否")
            << std::endl;
  std::cout << "TEST_UTILIZATION estimated_peak_bytes_per_cycle="
            << estimated_peak_bpc << " utilization_pct=" << utilization_pct
            << " dat_effective_path_width=" << dat_effective_path_width
            << " physical_lane_factor=1"
            << " target_pct=" << performance_target_pct
            << " measurement_valid=" << (measurement_valid ? "yes" : "no")
            << " target_met=" << (performance_target_met ? "yes" : "no")
            << std::endl;
  std::cout << "TEST_UTILIZATION_CN 估算峰值B每周期=" << estimated_peak_bpc
            << " 实际利用率百分比=" << utilization_pct
            << " DAT有效路径宽度字节=" << dat_effective_path_width
            << " 物理lane放大系数=1"
            << " 目标百分比=" << performance_target_pct
            << " 测量有效=" << (measurement_valid ? "是" : "否")
            << " 达标=" << (performance_target_met ? "是" : "否")
            << std::endl;
  std::cout << "TEST_LATENCY read_avg_cycles=" << read_latency_avg
            << " write_avg_cycles=" << write_latency_avg << std::endl;
  std::cout << "TEST_LATENCY_CN 平均读延迟周期=" << read_latency_avg
            << " 平均写延迟周期=" << write_latency_avg << std::endl;
  std::cout << "TEST_STALLS read_send=" << total_read_stalls
            << " read_response=" << total_read_response_stalls
            << " write_send=" << total_write_stalls
            << " write_buffer=" << total_write_buffer_stalls
            << " endpoint_total=" << endpoint_stalls
            << " connection_total=" << connection_stalls << " total=" << all_stalls
            << " backpressure_observed=" << (all_stalls == 0 ? "no" : "yes")
            << " dominant=" << dominant_bottleneck
            << std::endl;
  std::cout << "TEST_STALLS_CN 读发送阻塞=" << total_read_stalls
            << " 读响应阻塞=" << total_read_response_stalls
            << " 写发送阻塞=" << total_write_stalls
            << " 写缓冲阻塞=" << total_write_buffer_stalls
            << " endpoint总阻塞=" << endpoint_stalls
            << " connection总阻塞=" << connection_stalls << " 总阻塞=" << all_stalls
            << " 观察到反压=" << (all_stalls == 0 ? "否" : "是")
            << " 主要瓶颈=" << bottleneck_name_cn(dominant_bottleneck)
            << std::endl;
  std::cout << "TEST_BOTTLENECK ring_conn_serialization_busy_stalls="
            << ring_conn_breakdown.serialization_busy
            << " ring_conn_pipeline_full_stalls="
            << ring_conn_breakdown.pipeline_full
            << " ring_conn_downstream_register_full_stalls="
            << ring_conn_breakdown.downstream_register_full
            << " ring_conn_stalls=" << ring_conn_stalls
            << " dominant=" << dominant_bottleneck << std::endl;
  std::cout << "TEST_CROSS_STATION transit_slots="
            << csstats.transit_slots
            << " injected_packets=" << csstats.injected_packets
            << " ejected_packets=" << csstats.ejected_packets
            << " slot_pool_full_stalls="
            << csstats.slot_pool_full_stalls
            << " i_tag_sets=" << csstats.i_tag_sets
            << " i_tag_claims=" << csstats.i_tag_claims
            << " e_tag_sets=" << csstats.e_tag_sets
            << " e_tag_claims=" << csstats.e_tag_claims
            << " tagged_empty_slots="
            << csstats.tagged_empty_slots << std::endl;
  auto print_hot_conns =
      [](const char* subnet_name,
         const std::vector<TmRingConnHotspot>& hot_conns) {
    for (uint32_t i = 0; i < hot_conns.size(); ++i) {
      const auto& connection = hot_conns[i];
      if (connection.packets == 0 && connection.busy_cycles == 0 &&
          connection.total_stalls == 0) {
        continue;
      }
      std::cout << "TEST_CONN_HOTSPOT subnet=" << subnet_name
                << " rank=" << (i + 1)
                << " src_station=" << connection.src_station
                << " src_dir=" << tm_ring_port_index(connection.src_dir)
                << " dst_station=" << connection.dst_station
                << " dst_dir=" << tm_ring_port_index(connection.dst_dir)
                << " packets=" << connection.packets
                << " bytes=" << connection.bytes
                << " busy_cycles=" << connection.busy_cycles
                << " serialization_busy_stalls="
                << connection.serialization_busy_stall
                << " total_stalls=" << connection.total_stalls
                << " inflight_peak=" << connection.inflight_peak << std::endl;
    }
  };
  if (detailed_output) {
    print_hot_conns(
        "req", ring_pmu.top_busy_conns(TmRingSubnet::REQ, 5));
    print_hot_conns(
        "rsp", ring_pmu.top_busy_conns(TmRingSubnet::RSP, 5));
    print_hot_conns(
        "dat", ring_pmu.top_busy_conns(TmRingSubnet::DAT, 5));
  }
  std::cout << "TEST_FAIRNESS jain_index=" << fairness << std::endl;
  std::cout << "TEST_FAIRNESS_CN Jain公平性指数=" << fairness << std::endl;
  std::cout << "TEST_IDLE ring=" << ring_idle << " demo=" << demo_idle
            << " biu=" << biu_idle << " target=" << target_idle << std::endl;
  std::cout << "TEST_IDLE_CN Ring空闲=" << (ring_idle ? "是" : "否")
            << " demo空闲=" << (demo_idle ? "是" : "否")
            << " BIU空闲=" << (biu_idle ? "是" : "否")
            << " target空闲=" << (target_idle ? "是" : "否") << std::endl;

  for (const auto& failure : *failures) {
    std::cout << "TEST_FAILURE " << failure << std::endl;
  }
  const bool passed = failures->empty();
  std::cout << "TEST_RESULT case=" << case_name
            << " status=" << (passed ? "PASS" : "FAIL")
            << " failures=" << failures->size() << std::endl;
  std::cout << "TEST_RESULT_CN 场景=" << case_name
            << " 状态=" << (passed ? "通过" : "失败")
            << " 失败数=" << failures->size() << std::endl;
  return passed ? 0 : 1;
}

template <typename DemoT>
inline int run_demo(const std::string& config_file) {
  tm_init();
  auto clk = tm_make_clk();

  auto scenario_cfg = std::make_shared<cfg::Cfg>();
  scenario_cfg->read_cfg_file(config_file);
  cfg::p_cfg_t demo_cfg = scenario_cfg;

  TmRingDemoCfg demo_case;
  auto ring_cfg = tm_make_ring_cfg(std::string("demo_ring"), demo_cfg);
  const uint32_t uops_per_master = demo_case.uops_per_master;
  const uint32_t cycles = demo_case.cycles;
  const uint32_t core_burst_bytes = demo_core_burst_bytes(ring_cfg);
  const uint32_t access_stride_bytes = demo_access_stride_bytes(ring_cfg);
  const uint32_t write_stride_bytes = demo_write_stride_bytes(ring_cfg);
  std::vector<std::string> failures;
  check_interleave_policy(ring_cfg, &failures);

  auto biu_cfg = demo_cfg->get_cfg_tab("BIU");
  auto ddr_cfg = tm_make_mem_cfg(std::string("ddr"), demo_cfg);

  std::vector<p_tm_mem_t> targets;
  for (uint32_t target = 0; target < ring_cfg->targets.size(); ++target) {
    auto mem_cfg = tm_make_mem_cfg(ring_cfg->targets[target]->name, demo_cfg);
    targets.push_back(tm_make_mem(clk, mem_cfg));
  }

  auto ring = tm_make_ring(clk, ring_cfg);
  ring->build();

  std::vector<p_pem_biu_t> bius;
  std::vector<std::shared_ptr<DemoT>> demos;
  for (uint32_t master = 0; master < ring_cfg->num_masters; ++master) {
    auto biu = std::make_shared<pem_biu_t>("biu" + std::to_string(master), clk,
                                           biu_cfg);
    biu->core_id_ = master;
    biu->build();
    biu->reset();
    ring->attach_master(master, biu);
    bius.push_back(biu);

    const uint64_t src_addr =
        demo_master_base(kDemoSrcAddr, ring_cfg, master);
    const uint64_t dst_addr =
        demo_master_write_base(kDemoDstAddr, ring_cfg, uops_per_master,
                               master);
    auto demo =
        std::make_shared<DemoT>("pem_trdemo" + std::to_string(master), clk);
    demo->configure_traffic(src_addr, dst_addr, uops_per_master,
                            core_burst_bytes, access_stride_bytes,
                            write_stride_bytes);
    demo->attach(biu);
    demo->build();
    demos.push_back(demo);
  }

  for (uint32_t target = 0; target < ring_cfg->targets.size(); ++target) {
    ring->attach_target(target, targets[target]);
  }

  // Ring was reset during construction; after attach, only reset demos.
  for (auto& demo : demos) {
    demo->reset();
  }

  // PV memory initialization must happen after every model/interface reset.
  // Otherwise a reset in the build/attach sequence can silently restore the
  // memory reset value and all read data becomes zero.
  for (uint32_t master = 0; master < ring_cfg->num_masters; ++master) {
    const uint64_t src_addr =
        demo_master_base(kDemoSrcAddr, ring_cfg, master);
    const uint32_t bytes =
        static_cast<uint32_t>(demo_transfer_span(ring_cfg, uops_per_master));
    if (!preload_demo_data(targets, src_addr, bytes)) {
      std::ostringstream os;
      os << "source preload failed for master " << master;
      failures.push_back(os.str());
    }
  }
  for (uint32_t master = 0; master < ring_cfg->num_masters; ++master) {
    const uint64_t src_addr =
        demo_master_base(kDemoSrcAddr, ring_cfg, master);
    const uint64_t dst_addr =
        demo_master_write_base(kDemoDstAddr, ring_cfg, uops_per_master,
                               master);
    demos[master]->instr_que_->push_back(
        make_demo_instr(src_addr, dst_addr, uops_per_master));
  }

  tm_start(cycles);
  stats::stat->dump();

  return print_demo_performance(config_file, demo_case, ring_cfg, ddr_cfg, ring,
                                bius, demos, targets, &failures);
}

class ScopedStreamRedirect {
 public:
  ScopedStreamRedirect(std::ostream& stream, std::streambuf* destination)
      : stream_(stream), original_(stream.rdbuf(destination)) {}

  ~ScopedStreamRedirect() { stream_.rdbuf(original_); }

  ScopedStreamRedirect(const ScopedStreamRedirect&) = delete;
  ScopedStreamRedirect& operator=(const ScopedStreamRedirect&) = delete;

 private:
  std::ostream& stream_;
  std::streambuf* original_;
};

template <typename DemoT>
inline int run_demo_to_file(const std::string& config_file,
                            const std::string& result_file_name) {
  std::ofstream result_file(result_file_name, std::ios::out | std::ios::trunc);
  if (!result_file.is_open()) {
    throw std::runtime_error("cannot open result file: " + result_file_name);
  }

  ScopedStreamRedirect cout_redirect(std::cout, result_file.rdbuf());
  ScopedStreamRedirect cerr_redirect(std::cerr, result_file.rdbuf());
  std::cout << "TM_RING_DEMO_RESULT_FILE " << result_file_name << std::endl;
  return run_demo<DemoT>(config_file);
}

}  // namespace tm_ring_demo

#endif  // _TM_RING_DEMO_TEST_H_
