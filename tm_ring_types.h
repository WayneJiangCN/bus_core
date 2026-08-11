#ifndef _TM_RING_TYPES_H_
#define _TM_RING_TYPES_H_

#include <stdint.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "tm_bus_types.h"
#include "tm_mem.h"
#include "tm_ring_stats.h"

using PldCmd = pld_cmd_t;
using plt_cmt_t = PldCmd;
using plt_cmd_t = PldCmd;

using tm_ring_target_cfg_t = tm_bus_target_cfg_t;
using p_tm_ring_target_cfg_t = p_tm_bus_target_cfg_t;

enum class TmRingPortDir : uint32_t {
  // Local endpoint attached to this Cross Station: master NIU or MemPort.
  LOCAL = 0,
  // Clockwise direction on the ring.
  CW = 1,
  // Counter-clockwise direction on the ring.
  CCW = 2,
};

enum class TmRingDomainType : uint32_t {
  H_RING = 0,
  V_RING = 1,
};

struct TmRingLocation {
  TmRingLocation() {}
  TmRingLocation(TmRingDomainType type, uint32_t id, uint32_t station)
      : ring_type(type), ring_id(id), station_id(station) {}

  TmRingDomainType ring_type = TmRingDomainType::V_RING;
  uint32_t ring_id = 0;
  uint32_t station_id = 0;
};

enum class TmRingSubnet : uint32_t {
  // Request commands: RD and WR.
  REQ = 0,
  // Control responses: RSP and WR_RSP.
  RSP = 1,
  // Data transfers: WR_DAT and RD_RSP.
  DAT = 2,
};

enum class TmRingNodeType : uint32_t {
  MASTER = 0,
  HOME_AGENT,
  L2_BUFFER,
  RBRG_V,
  RBRG_H,
  COUNT,
};

enum class TmRingQueueSide : uint32_t { INJECT = 0, EJECT = 1 };

struct TmRingQueueStats {
  TmRingSubnet subnet = TmRingSubnet::REQ;
  TmRingQueueSide side = TmRingQueueSide::INJECT;
  TmRingPortDir direction = TmRingPortDir::LOCAL;
  uint32_t depth = 0;
  uint32_t occupancy = 0;
  uint32_t occupancy_peak = 0;
  TmRingQueueCounters counters;
};

struct TmRingEndpointQueueStats {
  TmRingNodeType node_type = TmRingNodeType::MASTER;
  uint32_t node_id = 0;
  TmRingQueueStats queue;
};

struct TmRingEndpointQueueDepths {
  std::array<uint32_t, 3> inject = {{2, 2, 2}};
  std::array<uint32_t, 3> eject = {{2, 2, 2}};
};

inline constexpr uint32_t tm_ring_port_count() { return 3; }

inline constexpr uint32_t tm_ring_subnet_count() { return 3; }

inline constexpr uint32_t tm_ring_invalid_tag_owner() { return 0xffffffffu; }

// TmInf is only the valid/ready boundary; real buffering uses TmQue.
inline constexpr uint32_t tm_ring_inf_depth() { return 2; }

inline constexpr uint32_t tm_ring_port_index(TmRingPortDir dir) {
  return static_cast<uint32_t>(dir);
}

inline constexpr uint32_t tm_ring_subnet_index(TmRingSubnet subnet) {
  return static_cast<uint32_t>(subnet);
}

inline constexpr bool tm_ring_is_req_cmd(PldCmd cmd) {
  return cmd == PldCmd::RD || cmd == PldCmd::WR;
}

inline constexpr TmRingSubnet tm_ring_cmd_subnet(PldCmd cmd) {
  return cmd == PldCmd::RD || cmd == PldCmd::WR
             ? TmRingSubnet::REQ
         : cmd == PldCmd::WR_DAT || cmd == PldCmd::RD_RSP
             ? TmRingSubnet::DAT
             : TmRingSubnet::RSP;
}

inline constexpr bool tm_ring_is_xor_hash_interleave(
    tm_bus_interleave_type_t type) {
  return type == tm_bus_interleave_type_t::XOR_HASH;
}

inline constexpr aic_req_type_t tm_ring_cmd_to_req(PldCmd cmd) {
  return cmd == PldCmd::RD   ? aic_req_type_t::RD_REQ
         : cmd == PldCmd::WR ? aic_req_type_t::WR_REQ
                             : aic_req_type_t::WR_DAT;
}

inline constexpr uint32_t tm_ring_cmd_bus_channel(PldCmd cmd) {
  return static_cast<uint32_t>(tm_ring_cmd_to_req(cmd));
}

inline constexpr uint32_t tm_ring_rd_rsp_bus_channel(uint32_t lane) {
  return static_cast<uint32_t>(aic_req_type_t::RD_REQ) + lane;
}

inline constexpr uint32_t tm_ring_packet_channel(PldCmd cmd) {
  return static_cast<uint32_t>(cmd);
}

inline constexpr uint32_t tm_ring_packet_channel_count() {
  return static_cast<uint32_t>(PldCmd::UNDEF);
}

inline constexpr TmRingPortDir tm_ring_opposite_dir(TmRingPortDir dir) {
  return dir == TmRingPortDir::CW
             ? TmRingPortDir::CCW
             : dir == TmRingPortDir::CCW ? TmRingPortDir::CW
                                         : TmRingPortDir::LOCAL;
}

inline uint32_t tm_ring_packet_bytes(const p_tm_pld_t& pld) {
  if (pld->ring_slot_empty) {
    return 0;
  }
  const PldCmd cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (cmd == PldCmd::RD || cmd == PldCmd::WR || cmd == PldCmd::RSP ||
      cmd == PldCmd::WR_RSP) {
    return 16;
  }
  return pld->size;
}

inline uint32_t tm_ring_serialization_cycles(uint32_t packet_bytes,
                                             uint32_t link_width_bytes) {
  const uint32_t cycles = packet_bytes / link_width_bytes +
                          (packet_bytes % link_width_bytes == 0 ? 0 : 1);
  return std::max<uint32_t>(1, cycles);
}

struct TmRingL2TrafficConfig {
  uint32_t line_size = 512;
  uint32_t sector_size = 128;
  uint32_t hit_rate_pct = 80;
  uint32_t hit_seed = 0;
  uint32_t buffer_depth = 16;
  uint32_t response_latency = 20;
  uint32_t issue_interval = 1;
};

struct TmRingCfg {
  // Ring instance name, used for module and log names.
  std::string name = "";
  // Number of master infs.
  uint32_t num_masters = 1;
  // Logical TmMem RD response channels; this is not a Ring physical lane.
  uint32_t rd_rsp_port_num = 2;
  // Optional target-side home agent.
  bool enable_home_agent = false;
  uint32_t home_agent_transaction_entries = 64;
  uint32_t home_agent_waiters_per_entry = 8;
  TmRingL2TrafficConfig l2_traffic;

  // Per-hop propagation latency modeled by TmRingConn.
  uint32_t ring_link_latency = 1;
  // Bytes serialized by each physical link per cycle.
  uint32_t ring_link_width_bytes = 16;
  // Endpoint queues; transit registers always have depth one.
  std::array<TmRingEndpointQueueDepths,
             static_cast<uint32_t>(TmRingNodeType::COUNT)>
      endpoint_queue_depths;
  uint32_t max_aicore_per_vring = 8;
  uint32_t rbrg_queue_depth = 4;
  uint32_t rbrg_latency = 1;
  uint32_t rbrg_width_bytes = 128;
  // Target configs; runtime target count is targets.size().
  std::vector<p_tm_ring_target_cfg_t> targets;
};

using tm_ring_cfg_t = TmRingCfg;
using p_tm_ring_cfg_t = std::shared_ptr<tm_ring_cfg_t>;

struct TmRingRdRspState {
  // One read transaction may complete after multiple response fragments.
  uint32_t rsp_expected = 1;
  uint32_t rsp_seen = 0;
};

#endif  // _TM_RING_TYPES_H_
