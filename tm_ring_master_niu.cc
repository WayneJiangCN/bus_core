#include "tm_ring_master_niu.h"

#include <algorithm>

using namespace tm_engine;
using namespace std;
TmRingMasterNiu::TmRingMasterNiu(const std::string& name, p_tm_clk_t clk,
                                  uint32_t master_port,
                                  const TmRingEndpointQueueDepths& queue_depths,
                                  const vector<TmRingQueuePmuPort>& queue_pmu_ports)
    : TmModule(name), master_port_(master_port) {
  config(clk, queue_depths, queue_pmu_ports);
}

void TmRingMasterNiu::config(
    p_tm_clk_t clk, const TmRingEndpointQueueDepths& queue_depths,
    const vector<TmRingQueuePmuPort>& queue_pmu_ports) {
  // The BIU-side interface keeps its local request/response channel layout.
  // Ring-side queues belong to the Node Interface and are consumed by the
  // attached Cross Station.
  const uint32_t biu_channel_count = tm_ring_rd_rsp_bus_channel(0) + 2;
  biu_inf_ =
      tm_make_com_inf(clk, this->name() + "_biu_inf", tm_ring_inf_depth());
  biu_inf_->set_chan_num(biu_channel_count);

  node_interface_ = tm_make_ring_node_interface(
      clk, this->name() + "_node_interface", queue_depths, queue_pmu_ports);

  tm_sensitive(TM_MAKE_CPROC(&TmRingMasterNiu::recv_biu_request),
               biu_inf_->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingMasterNiu::recv_biu_data), clk->pos_edge);
  tm_sensitive(TM_MAKE_CPROC(&TmRingMasterNiu::recv_rsp),
               node_interface_->eject_q(TmRingSubnet::RSP)->vld);
  tm_sensitive(TM_MAKE_CPROC(&TmRingMasterNiu::recv_dat),
               node_interface_->eject_q(TmRingSubnet::DAT)->vld);

  req_rr_arb_ = make_shared<RR_Arb>(2);
  reserved_req_class_ = UINT32_MAX;
  reset();
}

void TmRingMasterNiu::reset() {
  biu_inf_->reset();
  node_interface_->reset();
  reserved_req_class_ = UINT32_MAX;
}

bool TmRingMasterNiu::idle() const {
  return biu_inf_->idle() && node_interface_->idle();
}

void TmRingMasterNiu::attach(p_tm_com_inf_t biu_inf) {
  biu_inf_->connect(biu_inf);
}

void TmRingMasterNiu::attach(p_tm_ring_topology_t topology) {
  topology_ = topology;
}

p_tm_ring_node_interface_t TmRingMasterNiu::node_interface() const {
  return node_interface_;
}

void TmRingMasterNiu::recv_biu_request() {
  const PldCmd cmds[] = {PldCmd::RD, PldCmd::WR};
  if (reserved_req_class_ == UINT32_MAX) {
    for (uint32_t i = 0; i < 2; ++i) {
      if (biu_inf_->valid(tm_ring_cmd_bus_channel(cmds[i]))) {
        req_rr_arb_->req(i);
      }
    }
    const uint32_t winner = req_rr_arb_->get_arb();
    if (winner == INV_ARB) {
      return;
    }
    reserved_req_class_ = winner;
  }

  if (forward_biu_request(cmds[reserved_req_class_])) {
    reserved_req_class_ = UINT32_MAX;
  }
}

void TmRingMasterNiu::recv_biu_data() { forward_biu_data(); }

void TmRingMasterNiu::recv_rsp() {
  auto q = node_interface_->eject_q(TmRingSubnet::RSP);
  if (q->empty()) {
    return;
  }

  auto rsp = q->front();
  // AXI-style writes return only the final completion on the WR_DAT channel.
  const uint32_t biu_channel = tm_ring_cmd_bus_channel(PldCmd::WR_DAT);
  if (!biu_inf_->send(biu_channel, rsp)) {
    return;
  }
  node_interface_->pop_eject(TmRingSubnet::RSP);
}

void TmRingMasterNiu::recv_dat() {
  auto q = node_interface_->eject_q(TmRingSubnet::DAT);
  if (q->empty()) {
    return;
  }

  auto rsp = q->front();
  if (!biu_inf_->send(tm_ring_rd_rsp_bus_channel(0), rsp)) {
    return;
  }
  node_interface_->pop_eject(TmRingSubnet::DAT);
}

bool TmRingMasterNiu::forward_biu_request(PldCmd cmd) {
  const uint32_t biu_channel = tm_ring_cmd_bus_channel(cmd);
  if (!biu_inf_->valid(biu_channel)) {
    return false;
  }

  auto pld = biu_inf_->get_pld(biu_channel);
  prepare_ring_request(pld, cmd);
  if (!node_interface_->push_inject(TmRingSubnet::REQ, pld)) {
    return false;
  }
  biu_inf_->pop_pld(biu_channel);
  return true;
}

bool TmRingMasterNiu::forward_biu_data() {
  const uint32_t biu_channel = tm_ring_cmd_bus_channel(PldCmd::WR_DAT);
  if (!biu_inf_->valid(biu_channel)) {
    return false;
  }

  auto pld = biu_inf_->get_pld(biu_channel);
  prepare_ring_request(pld, PldCmd::WR_DAT);
  if (!node_interface_->push_inject(TmRingSubnet::DAT, pld)) {
    return false;
  }
  biu_inf_->pop_pld(biu_channel);
  return true;
}

void TmRingMasterNiu::prepare_ring_request(p_tm_pld_t pld, PldCmd cmd) {
  const uint32_t target_id = topology_->decode_target(pld->addr);
  const TmRingLocation src = topology_->master_location(master_port_);
  const TmRingLocation dst =
      topology_->rbrg_v_location(topology_->master_vring(master_port_));
  pld->mst_id = master_port_;
  pld->cmd = cmd;
  pld->ring_subnet = static_cast<uint32_t>(tm_ring_cmd_subnet(cmd));
  pld->ring_traffic_class = static_cast<uint32_t>(cmd);
  tm_pld_set_ring_route(pld, static_cast<uint32_t>(cmd), target_id,
                        src.station_id, dst.station_id);
  pld->ring_direction = static_cast<uint32_t>(
      topology_->route_direction(src, dst));
}
