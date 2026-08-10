#include "tm_ring_mem_port.h"

#include <cstring>

#include "tm_pld.h"

using namespace tm_engine;
using namespace std;
TmRingMemPort::TmRingMemPort(const std::string& name, p_tm_clk_t clk,
                              const tm_ring_target_cfg_t& target_cfg,
                              const TmRingCfg& ring_cfg)
    : TmModule(name), rd_rsp_port_num_(ring_cfg.rd_rsp_port_num) {

  log_para_t log_para(name + ".log");
  log_ = pem_log::create_logger(log_para);
  PEM_LOG_INFO(log_, "[{0:d}] config rd_rsp_ports:{1:d}", time(),
               rd_rsp_port_num_);

  const uint32_t request_chan_num = std::max<uint32_t>(
      tm_ring_cmd_bus_channel(PldCmd::RD) + 1,
      tm_ring_cmd_bus_channel(PldCmd::WR_DAT) + 1);
  const uint32_t chan_num = std::max<uint32_t>(
      request_chan_num,
      tm_ring_rd_rsp_bus_channel(0) + rd_rsp_port_num_);
  inf_ = tm_make_com_inf(clk, name + "_inf", tm_ring_inf_depth());
  inf_->set_chan_num(chan_num);
  if (ring_cfg.enable_home_agent) {
    TmRingHomeAgentConfig home_agent_cfg;
    home_agent_cfg.line_size = ring_cfg.l2_traffic.line_size;
    home_agent_cfg.entry_limit = ring_cfg.home_agent_transaction_entries;
    home_agent_cfg.waiters_per_entry = ring_cfg.home_agent_waiters_per_entry;
    home_agent_cfg.hit_rate_pct = ring_cfg.l2_traffic.hit_rate_pct;
    home_agent_cfg.hit_seed = ring_cfg.l2_traffic.hit_seed;
    home_agent_ = std::make_shared<TmRingHomeAgent>(home_agent_cfg);
  }

  if (home_agent_ != nullptr) {
    tm_sensitive(TM_MAKE_CPROC(&TmRingMemPort::service_home_agent),
                 clk->pos_edge);
  }
  tm_sensitive(TM_MAKE_CPROC(&TmRingMemPort::recv_mem_rsp), inf_->vld);

  node_interface_ = tm_make_ring_node_interface(
      clk, name + "_node_interface", ring_cfg.ring_inject_queue_depth,
      ring_cfg.ring_eject_queue_depth);
  tm_sensitive(TM_MAKE_CPROC(&TmRingMemPort::recv_ring_req),
               node_interface_->eject_q(TmRingSubnet::REQ)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingMemPort::recv_ring_dat),
               node_interface_->eject_q(TmRingSubnet::DAT)->vld);

  rd_req_q_ =
      tm_make_com_que(clk, name + "_rd_req_q", target_cfg.rd_req_fifo_depth);
  wr_req_q_ =
      tm_make_com_que(clk, name + "_wr_req_q", target_cfg.wr_req_fifo_depth);
  wr_dat_q_ =
      tm_make_com_que(clk, name + "_wr_dat_q", target_cfg.wr_dat_fifo_depth);

  tm_sensitive(TM_MAKE_CPROC(&TmRingMemPort::send_rd_cmd), rd_req_q_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingMemPort::send_wr_cmd), wr_req_q_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingMemPort::send_wr_dat), wr_dat_q_->vld);

  reset();
}

void TmRingMemPort::reset() {
  if (inf_ != nullptr) {
    inf_->reset();
  }
  if (node_interface_ != nullptr) {
    node_interface_->reset();
  }
  if (rd_req_q_ != nullptr) {
    rd_req_q_->clear();
  }
  if (wr_req_q_ != nullptr) {
    wr_req_q_->clear();
  }
  if (wr_dat_q_ != nullptr) {
    wr_dat_q_->clear();
  }
  if (home_agent_ != nullptr) {
    home_agent_->reset();
  }
  pending_rd_rsp_ = 0;
  next_rsp_class_ = 0;
  next_rd_rsp_lane_ = 0;
}

void TmRingMemPort::attach(uint32_t target_id,
                           std::shared_ptr<TmRingTopology> topology) {
  target_id_ = target_id;
  topology_ = topology;
  PEM_LOG_INFO(log_, "[{0:d}] attach_mem_port target:{1:d}", time(),
               target_id_);
}

bool TmRingMemPort::idle() const {
  return (inf_ == nullptr || inf_->idle()) &&
         (node_interface_ == nullptr || node_interface_->idle()) &&
         (rd_req_q_ == nullptr || rd_req_q_->empty()) &&
         (wr_req_q_ == nullptr || wr_req_q_->empty()) &&
         (wr_dat_q_ == nullptr || wr_dat_q_->empty()) &&
         pending_rd_rsp_ == 0 &&
         (home_agent_ == nullptr || home_agent_->idle());
}
// Attach a communication interface to the memory port.
void TmRingMemPort::attach(p_tm_com_inf_t inf) {
  inf_->connect(inf);
  PEM_LOG_INFO(log_, "[{0:d}] attach_mem_inf target:{1:d}", time(), target_id_);
}

void TmRingMemPort::attach(p_tm_mem_t mem) {
  if (mem != nullptr) {
    mem_ = mem;
    attach(mem->rw_inf_);
  }
}

void TmRingMemPort::attach_l2_buffer(
    p_tm_ring_l2_buffer_node_t l2_buffer) {
  l2_buffer_ = l2_buffer;
}

p_tm_ring_node_interface_t TmRingMemPort::node_interface() const {
  return node_interface_;
}

TmRingHomeAgentStats TmRingMemPort::home_agent_stats() const {
  return home_agent_ == nullptr ? TmRingHomeAgentStats()
                                : home_agent_->stats();
}

void TmRingMemPort::clear_stats() {
  if (home_agent_ != nullptr) {
    home_agent_->clear_stats();
  }
}

p_tm_com_que_t TmRingMemPort::req_q(PldCmd cmd) const {
  if (cmd == PldCmd::RD) {
    return rd_req_q_;
  }
  if (cmd == PldCmd::WR) {
    return wr_req_q_;
  }
  return wr_dat_q_;
}

void TmRingMemPort::prepare_ring_response(p_tm_pld_t pld, PldCmd cmd) {
  const TmRingLocation src = topology_->ha_location(target_id_);
  const TmRingLocation dst =
      topology_->rbrg_h_location(topology_->master_vring(pld->mst_id));
  pld->cmd = cmd;
  pld->ring_subnet = static_cast<uint32_t>(tm_ring_cmd_subnet(cmd));
  pld->ring_traffic_class = static_cast<uint32_t>(cmd);
  tm_pld_set_ring_route(pld, tm_pld_req_type(pld), target_id_,
                        src.station_id, dst.station_id);
  pld->ring_direction = static_cast<uint32_t>(
      topology_->route_direction(src, dst));
}

/*
 * 做什么：把 Cross Station eject 到本 target 的 Ring 请求放入对应本地 FIFO。
 * 输入/前提：请求已经到达 Node Interface 的 REQ Eject Queue；命令类型决定进入 RD 或 WR 队列。
 * 核心流程：每次只接收一个可用命令，并保留 payload 的原始 master/gid/地址信息。
 * 结果/重试：FIFO 满时不 pop 接口请求，Ring 上游按 ready/valid 语义继续反压。
 */
void TmRingMemPort::recv_ring_req() {
  auto ring_q = node_interface_->eject_q(TmRingSubnet::REQ);
  if (ring_q->empty()) {
    return;
  }

  auto pld = ring_q->front();
  auto cmd = static_cast<PldCmd>(pld->ring_traffic_class);
  if (cmd != PldCmd::RD && cmd != PldCmd::WR) {
    return;
  }
  auto q = req_q(cmd);
  if (q->full()) {
    return;
  }

  q->push_back(pld);
  node_interface_->pop_eject(TmRingSubnet::REQ);
  PEM_LOG_INFO(log_,
               "[{0:d}] recv_ring_cmd target:{1:d} cmd:{2:d} "
               "gid:{3:d} addr:0x{4:x} size:{5:d}",
               time(), target_id_, static_cast<uint32_t>(cmd), pld->gid,
               pld->addr, pld->size);
}

void TmRingMemPort::recv_ring_dat() {
  auto ring_q = node_interface_->eject_q(TmRingSubnet::DAT);
  if (ring_q->empty()) {
    return;
  }

  auto pld = ring_q->front();
  if (static_cast<PldCmd>(pld->ring_traffic_class) != PldCmd::WR_DAT ||
      wr_dat_q_->full()) {
    return;
  }
  wr_dat_q_->push_back(pld);
  node_interface_->pop_eject(TmRingSubnet::DAT);
  PEM_LOG_INFO(log_,
               "[{0:d}] recv_ring_dat target:{1:d} gid:{2:d} addr:0x{3:x} "
               "size:{4:d}",
               time(), target_id_, pld->gid, pld->addr, pld->size);
}

void TmRingMemPort::send_rd_cmd() { send_cmd(PldCmd::RD); }

void TmRingMemPort::send_wr_cmd() { send_cmd(PldCmd::WR); }

void TmRingMemPort::send_wr_dat() { send_cmd(PldCmd::WR_DAT); }

/*
 * 做什么：执行一类普通请求的“队首 -> TmMem”发送。
 * 输入/前提：cmd 为 RD、WR 或 WR_DAT；HA 启用时 RD 不在此路径发送，写先经过 hazard 检查。
 * 核心流程：先预留 HA 写保护，再调用 inf_->send()；成功后才弹出 FIFO。
 * 结果/重试：send 失败时 FIFO 和写预留保持不变，下一拍重试相同队首。
 */
void TmRingMemPort::send_cmd(PldCmd cmd) {
  if (cmd == PldCmd::RD && home_agent_ != nullptr) {
    return;
  }

  auto q = req_q(cmd);
  if (q->empty()) {
    return;
  }

  auto pld = q->front();
  if (home_agent_ != nullptr &&
      (cmd == PldCmd::WR || cmd == PldCmd::WR_DAT) &&
      !home_agent_->reserve_write(pld)) {
    return;
  }

  const bool accepted = inf_->send(tm_ring_cmd_bus_channel(cmd), pld);
  if (accepted) {
    q->pop_front();
    if (cmd == PldCmd::RD) {
      pending_rd_rsp_++;
    }
    PEM_LOG_INFO(log_,
                 "[{0:d}] send_mem_cmd target:{1:d} cmd:{2:d} "
                 "gid:{3:d} addr:0x{4:x}",
                 time(), target_id_, static_cast<uint32_t>(cmd), pld->gid,
                 pld->addr);
  }
}

/*
 * 做什么：每拍推进一次 Home Agent 的完整读请求生命周期。
 * 输入/前提：仅在 HA 已启用时运行；MemPort 仍拥有队列、接口和成功发送后的 commit。
 * 核心流程：按 group summary -> hit -> 接收读 -> 后端读 -> 捕获后端响应 -> Ring 响应的固定顺序执行。
 * 结果/重试：每阶段最多完成一笔；反压时对应 candidate/FIFO 保持不变并在下一拍重试。
 */
void TmRingMemPort::service_home_agent() {
  if (home_agent_ == nullptr) {
    return;
  }

  // 先回收已完成的 L2 group，再处理新的 waiter；这样 HA 能在同一拍关闭
  // 已冻结 group 的 admission，避免新请求拿到过期 group token。
  if (l2_buffer_ != nullptr) {
    const std::vector<TmRingL2GroupSummary> summaries =
        l2_buffer_->take_frozen_summaries();
    for (const TmRingL2GroupSummary& summary : summaries) {
      home_agent_->consume_l2_group_summary(summary);
    }
  }

  // 首个扁平 transaction 立即可发射；本拍捕获的 completion 也可在本拍交给 L2。
  // 每组 front()/commit() 仅在对应 send 成功后推进。
  service_home_agent_hit();
  accept_rd_into_home_agent();
  issue_home_agent_backend_read();
  capture_home_agent_backend_rsp();
  if (!send_home_agent_l2_rsp()) {
    recv_rd_cmd_rsp();
  }
  send_wr_cmd();
  send_wr_dat();
}

bool TmRingMemPort::service_home_agent_hit() {
  if (home_agent_ == nullptr || mem_ == nullptr ||
      !home_agent_->has_functional_read()) {
    return false;
  }

  p_tm_pld_t candidate = home_agent_->front_functional_read();
  if (candidate == nullptr) {
    return false;
  }
  const bool ok =
      mem_->direct_read(candidate->addr, candidate->size, candidate->data);
  home_agent_->commit_functional_read(ok ? PldRsp::OK : PldRsp::ERR);
  return true;
}

/*
 * 做什么：将 RD FIFO 队首交给 HA，或让跨 line 请求绕过 HA 直接送后端。
 * 输入/前提：RD FIFO 非空。
 * 核心流程：调用 accept_read()；ACCEPTED/MERGED 弹出 FIFO，BYPASS 尝试直接 send，STALL 保留队首。
 * 结果/重试：每个成功接收的原始读都增加 pending_rd_rsp_；STALL 或 send 失败下一拍重试。
 */
bool TmRingMemPort::accept_rd_into_home_agent() {
  if (home_agent_ == nullptr || rd_req_q_->empty()) {
    return false;
  }

  auto pld = rd_req_q_->front();
  const TmHaAcceptResult result = home_agent_->accept_read(pld);
  bool accepted = result == TmHaAcceptResult::ACCEPTED ||
                  result == TmHaAcceptResult::MERGED;
  if (result == TmHaAcceptResult::BYPASS) {
    accepted = inf_->send(tm_ring_cmd_bus_channel(PldCmd::RD), pld);
  }
  if (!accepted) {
    return false;
  }

  rd_req_q_->pop_front();
  pending_rd_rsp_++;
  PEM_LOG_INFO(log_,
               "[{0:d}] accept_home_agent_rd target:{1:d} result:{2:d} "
               "gid:{3:d} addr:0x{4:x}",
               time(), target_id_, static_cast<uint32_t>(result), pld->gid,
               pld->addr);
  return true;
}

/*
 * 做什么：把 HA 已准备好的一个 line 读请求发送给 TmMem。
 * 输入/前提：HA 至少有一个 PENDING transaction。
 * 核心流程：读取 front candidate，尝试后端 send；仅 send 成功后调用 commit_backend_request()。
 * 结果/重试：成功后 transaction 变为 INFLIGHT；失败时 candidate 保持 PENDING，下一拍重试。
 */
bool TmRingMemPort::issue_home_agent_backend_read() {
  if (home_agent_ == nullptr || !home_agent_->has_backend_request()) {
    return false;
  }

  auto backend_request = home_agent_->front_backend_request();
  if (backend_request == nullptr ||
      !inf_->send(tm_ring_cmd_bus_channel(PldCmd::RD), backend_request)) {
    return false;
  }

  // 仅在 TmMem 接收 candidate 后提交；若失败，HA 保留相同 candidate 到下一拍重试。
  home_agent_->commit_backend_request();
  PEM_LOG_INFO(log_,
               "[{0:d}] issue_home_agent_backend target:{1:d} gid:{2:d} "
               "addr:0x{3:x} size:{4:d}",
               time(), target_id_, backend_request->gid, backend_request->addr,
               backend_request->size);
  return true;
}

/*
 * 做什么：从 TmMem 的逻辑读响应通道取回一个完成 line，交给 HA fanout。
 * 输入/前提：HA 启用且至少配置一个读响应通道。
 * 核心流程：从本地 backend response channel 开始轮询，只有 HA 按 backend key
 * 接受响应后才 pop 通道。
 * 结果/重试：成功时 transaction 进入 RESPONDING；不匹配响应不弹出，留给普通读响应路径处理。
 */
bool TmRingMemPort::capture_home_agent_backend_rsp() {
  if (home_agent_ == nullptr || rd_rsp_port_num_ == 0) {
    return false;
  }

  for (uint32_t offset = 0; offset < rd_rsp_port_num_; ++offset) {
    const uint32_t backend_lane =
        (next_rd_rsp_lane_ + offset) % rd_rsp_port_num_;
    if (!has_response(PldCmd::RD, backend_lane)) {
      continue;
    }

    auto backend_rsp = front_response(PldCmd::RD, backend_lane);
    if (!home_agent_->accept_backend_response(backend_rsp)) {
      continue;
    }

    pop_response(PldCmd::RD, backend_lane);
    next_rd_rsp_lane_ = (backend_lane + 1) % rd_rsp_port_num_;
    PEM_LOG_INFO(log_,
                 "[{0:d}] capture_home_agent_rsp target:{1:d} gid:{2:d} "
                 "lane:{3:d}",
                 time(), target_id_, backend_rsp->gid, backend_lane);
    return true;
  }
  return false;
}

/*
 * 做什么：把 HA 当前 waiter 的 L2 candidate 交给有界 L2 Buffer。
 * 输入/前提：HA 有 RESPONDING transaction，L2 Buffer 可用。
 * 核心流程：取得 candidate，尝试 accept；只有 accept 成功后才 commit waiter。
 * 结果/重试：每次成功只提交一个 waiter；L2 physical group 的冻结由 summary 回收。
 * Ring 反压时
 *            candidate 不前移并在下一拍重试。
 */
bool TmRingMemPort::send_home_agent_l2_rsp() {
  if (home_agent_ == nullptr || l2_buffer_ == nullptr ||
      !home_agent_->has_l2_response()) {
    return false;
  }

  const TmRingL2ResponseCandidate candidate =
      home_agent_->front_l2_response();
  if (candidate.response == nullptr) {
    return false;
  }

  const TmRingL2AcceptResult result = l2_buffer_->accept_response(candidate);
  if (!result.accepted()) {
    if (result.status == TmRingL2AcceptStatus::REJECTED_BUFFER_FULL) {
      home_agent_->record_private_l2_full_stall();
    }
    return false;
  }

  home_agent_->commit_l2_response(result);
  if (pending_rd_rsp_ != 0) {
    pending_rd_rsp_--;
  }
  PEM_LOG_INFO(log_,
               "[{0:d}] handoff_home_agent_rsp target:{1:d} gid:{2:d} "
               "addr:0x{3:x} size:{4:d}",
               time(), target_id_, candidate.response->gid,
               candidate.response->addr, candidate.response->size);
  return true;
}

/*
 * 做什么：处理未被 HA 消费的 TmMem 响应，并按命令类型返回 Ring。
 * 输入/前提：HA 模式下，读响应优先由 service_home_agent() 捕获以完成 fanout。
 * 核心流程：跳过 RD 类的重复处理，轮询普通 RD/WR/WR_DAT 响应路径。
 * 结果/重试：任一路径的 Ring send 失败时不 pop 后端响应，保持原响应至下一拍。
 */
void TmRingMemPort::recv_mem_rsp() {
  // In HA mode the positive-edge service owns RD responses: it captures HA
  // responses first and then uses recv_rd_cmd_rsp() for direct fallback. Keep
  // the RD class out of this vld-triggered process so the ownership does not
  // depend on sensitive-process registration order.
  for (uint32_t offset = 0; offset < 3; ++offset) {
    const uint32_t rsp_class = (next_rsp_class_ + offset) % 3;
    if (rsp_class == 0 && home_agent_ != nullptr) {
      continue;
    }

    bool sent = false;
    if (rsp_class == 0) {
      sent = recv_rd_cmd_rsp();
    } else if (rsp_class == 1) {
      sent = recv_wr_cmd_rsp();
    } else {
      sent = recv_wr_dat_rsp();
    }
    if (sent) {
      next_rsp_class_ = (rsp_class + 1) % 3;
      return;
    }
  }
}

/*
 * 做什么：将一笔未被 HA 消费的后端读响应交给绑定的 L2 Buffer。
 * 输入/前提：该响应不是 HA 已接受的 line completion，且本地 response channel 有数据。
 * 核心流程：L2 Buffer 接收成功后才提交本地 backend response。
 * 结果/重试：成功才 pop 后端响应并减少 pending_rd_rsp_；Buffer 或 DAT 反压时保持原响应。
 */
bool TmRingMemPort::recv_rd_cmd_rsp() {
  if (l2_buffer_ == nullptr || rd_rsp_port_num_ == 0) {
    return false;
  }

  for (uint32_t offset = 0; offset < rd_rsp_port_num_; ++offset) {
    const uint32_t backend_lane =
        (next_rd_rsp_lane_ + offset) % rd_rsp_port_num_;
    if (!has_response(PldCmd::RD, backend_lane)) {
      continue;
    }

    auto backend_rsp = front_response(PldCmd::RD, backend_lane);
    TmRingL2ResponseCandidate candidate;
    candidate.response = backend_rsp;
    candidate.line_base = backend_rsp->addr;
    candidate.payload_offset = 0;
    candidate.fanout_eligible = false;
    if (backend_rsp->buf_u8 != nullptr &&
        backend_rsp->buf_u8->size() >= backend_rsp->size) {
      candidate.completion_data = backend_rsp->buf_u8;
    } else {
      std::shared_ptr<std::vector<uint8_t>> data =
          std::make_shared<std::vector<uint8_t>>(backend_rsp->size, 0);
      if (backend_rsp->rsp == PldRsp::OK && backend_rsp->data != nullptr) {
        std::memcpy(data->data(), backend_rsp->data, backend_rsp->size);
      }
      candidate.completion_data = data;
    }
    const TmRingL2AcceptResult result = l2_buffer_->accept_response(candidate);
    if (!result.accepted()) {
      continue;
    }
    pop_response(PldCmd::RD, backend_lane);
    if (pending_rd_rsp_ != 0) {
      pending_rd_rsp_--;
    }
    PEM_LOG_INFO(log_,
                 "[{0:d}] send_rd_rsp target:{1:d} gid:{2:d} "
                 "backend_lane:{3:d} addr:0x{4:x}",
                 time(), target_id_, backend_rsp->gid, backend_lane,
                 backend_rsp->addr);

    next_rd_rsp_lane_ = (backend_lane + 1) % rd_rsp_port_num_;
    return true;
  }
  return false;
}

/*
 * 做什么：将写命令阶段的后端响应返回给 Ring。
 * 输入/前提：TmMem 的 WR 响应通道有数据。
 * 核心流程：取得队首响应并发送至 Ring RSP。
 * 结果/重试：只有 Ring 接收后才 pop 后端响应；否则下一拍继续尝试。
 */
bool TmRingMemPort::recv_wr_cmd_rsp() {
  if (!has_response(PldCmd::WR)) {
    return false;
  }

  auto rsp = front_response(PldCmd::WR);
  prepare_ring_response(rsp, PldCmd::WR_RSP);
  if (!node_interface_->push_inject(TmRingSubnet::RSP, rsp)) {
    return false;
  }
  pop_response(PldCmd::WR);
  PEM_LOG_INFO(log_, "[{0:d}] send_wr_rsp target:{1:d} gid:{2:d} addr:0x{3:x}",
               time(), target_id_, rsp->gid, rsp->addr);

  return true;
}

/*
 * 做什么：返回 WR_DAT 的最终响应，并通知 HA 释放该写事务的顺序保护。
 * 输入/前提：TmMem 的 WR_DAT 响应通道有数据。
 * 核心流程：先成功发送 Ring 响应，再 pop 后端响应并调用 complete_write()。
 * 结果/重试：Ring 反压时 write hazard 继续保留；成功后同 line 的后续读可重新进入 HA。
 */
bool TmRingMemPort::recv_wr_dat_rsp() {
  if (!has_response(PldCmd::WR_DAT)) {
    return false;
  }

  auto rsp = front_response(PldCmd::WR_DAT);
  prepare_ring_response(rsp, PldCmd::RSP);
  if (!node_interface_->push_inject(TmRingSubnet::RSP, rsp)) {
    return false;
  }
  if (home_agent_ != nullptr) {
    home_agent_->complete_write(rsp);
  }
  pop_response(PldCmd::WR_DAT);
  PEM_LOG_INFO(log_,
               "[{0:d}] send_wr_dat_rsp target:{1:d} gid:{2:d} "
               "addr:0x{3:x}",
               time(), target_id_, rsp->gid, rsp->addr);

  return true;
}

bool TmRingMemPort::has_response(PldCmd cmd, uint32_t lane) const {
  return inf_->valid(response_channel(cmd, lane));
}

p_tm_pld_t TmRingMemPort::front_response(PldCmd cmd, uint32_t lane) const {
  return inf_->get_pld(response_channel(cmd, lane));
}

void TmRingMemPort::pop_response(PldCmd cmd, uint32_t lane) {
  inf_->pop_pld(response_channel(cmd, lane));
}

uint32_t TmRingMemPort::response_channel(PldCmd cmd, uint32_t lane) const {
  if (cmd == PldCmd::RD) {
    return tm_ring_rd_rsp_bus_channel(lane);
  }
  return tm_ring_cmd_bus_channel(cmd);
}
