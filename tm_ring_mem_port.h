#ifndef _TM_RING_MEM_PORT_H_
#define _TM_RING_MEM_PORT_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "pem_log.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_mem.h"
#include "tm_que.h"
#include "tm_ring_home_agent.h"
#include "tm_ring_l2_buffer_node.h"
#include "tm_ring_node_interface.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"

// Memory-side access adapter. Owns request queues and home-agent state.
// TmMem owns backend service limits; the L2 Buffer owns read-response cadence.
class TmRingMemPort : public tm_engine::TmModule {
 public:
  TmRingMemPort(
      const std::string& name, tm_engine::p_tm_clk_t clk,
      const tm_ring_target_cfg_t& target_cfg, const TmRingCfg& ring_cfg,
      const TmRingEndpointQueueDepths& queue_depths);
  void reset();
  bool idle() const;

  // Bind target id and its Ring topology. TmMem owns backend service only.
  void attach(uint32_t target_id, std::shared_ptr<TmRingTopology> topology);
  // Bind the real TmMem interface; inf_ carries both requests and responses.
  void attach(p_tm_com_inf_t inf);
  void attach(p_tm_mem_t mem);
  void attach_l2_buffer(p_tm_ring_l2_buffer_node_t l2_buffer);
  p_tm_ring_node_interface_t node_interface() const;
  TmRingHomeAgentStats home_agent_stats() const;
  const std::vector<TmRingHaSourceStats>& ha_source_stats() const;
  void clear_stats();

  void send_rd_cmd();
  void send_wr_cmd();
  void send_wr_dat();

  void recv_ring_req();
  void recv_ring_dat();

 private:
  // Unified request send path. TmMem handles backend credit and bandwidth.
  void send_cmd(PldCmd cmd);
  // 每拍至多执行一次各阶段：接收读 -> 发射后端读 -> 捕获后端响应 -> 注入一笔 Ring
  // 响应。HA helper 只维护扁平 transaction 状态；MemPort 仍拥有队列、接口和
  // 所有“send 成功后”的 commit。
  void service_home_agent();
  bool accept_rd_into_home_agent();
  bool issue_home_agent_backend_read();
  bool capture_home_agent_backend_rsp();
  bool service_home_agent_hit();
  bool send_home_agent_l2_rsp();
  void recv_mem_rsp();
  bool recv_rd_cmd_rsp();
  bool recv_wr_cmd_rsp();
  bool recv_wr_dat_rsp();
  void prepare_ring_response(p_tm_pld_t pld, PldCmd cmd);
  p_tm_com_que_t req_q(PldCmd cmd) const;
  bool has_response(PldCmd cmd, uint32_t lane = 0) const;
  p_tm_pld_t front_response(PldCmd cmd, uint32_t lane = 0) const;
  void pop_response(PldCmd cmd, uint32_t lane = 0);
  uint32_t response_channel(PldCmd cmd, uint32_t lane = 0) const;

  uint32_t target_id_ = 0;
  std::shared_ptr<TmRingTopology> topology_ = nullptr;
  uint32_t rd_rsp_port_num_ = 0;
  // rd_rsp_port_num_ belongs to the local TmMem response-channel interface.
  p_tm_mem_t mem_ = nullptr;
  p_tm_com_inf_t inf_ = nullptr;  // TmMem interface; carries requests and responses.
  p_tm_ring_node_interface_t node_interface_ = nullptr;

  // Real request buffering between the ring and TmMem.
  p_tm_com_que_t rd_req_q_ = nullptr;
  p_tm_com_que_t wr_req_q_ = nullptr;
  p_tm_com_que_t wr_dat_q_ = nullptr;
  std::shared_ptr<TmRingHomeAgent> home_agent_ = nullptr;
  std::vector<TmRingHaSourceStats> ha_source_stats_;
  p_tm_ring_l2_buffer_node_t l2_buffer_ = nullptr;
  // Original read requests accepted by this endpoint but not returned to Ring.
  uint32_t pending_rd_rsp_ = 0;

  uint32_t next_rsp_class_ = 0;
  uint32_t next_rd_rsp_lane_ = 0;
  p_logger_t log_ = nullptr;
};

using tm_ring_mem_port_t = TmRingMemPort;
using p_tm_ring_mem_port_t = std::shared_ptr<tm_ring_mem_port_t>;

inline p_tm_ring_mem_port_t tm_make_ring_mem_port(
    const std::string& name, tm_engine::p_tm_clk_t clk,
    const tm_ring_target_cfg_t& target_cfg, const TmRingCfg& ring_cfg,
    const TmRingEndpointQueueDepths& queue_depths) {
  return std::make_shared<TmRingMemPort>(
      name, clk, target_cfg, ring_cfg, queue_depths);
}

#endif  // _TM_RING_MEM_PORT_H_
