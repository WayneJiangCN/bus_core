#ifndef _TM_RING_MASTER_NIU_H_
#define _TM_RING_MASTER_NIU_H_

#include <stdint.h>

#include <memory>
#include <string>

#include "arbiter.h"
#include "tm_clock.h"
#include "tm_engine.h"
#include "tm_inf.h"
#include "tm_ring_node_interface.h"
#include "tm_ring_topology.h"
#include "tm_ring_types.h"

using tm_ring_topology_t = TmRingTopology;
using p_tm_ring_topology_t = std::shared_ptr<tm_ring_topology_t>;

// BIU-side access adapter. Converts BIU channels to Ring slot metadata.
// BIU owns requester queues, OSD, and write protocol state.
class TmRingMasterNiu : public tm_engine::TmModule {
 public:
  TmRingMasterNiu(const std::string& name, tm_engine::p_tm_clk_t clk,
                  uint32_t master_port, uint32_t inject_depth,
                  uint32_t eject_depth);
  void reset();
  bool idle() const;

  void attach(p_tm_com_inf_t biu_inf);
  void attach(p_tm_ring_topology_t topology);

  p_tm_ring_node_interface_t node_interface() const;

 private:
  void config(tm_engine::p_tm_clk_t clk, uint32_t inject_depth,
              uint32_t eject_depth);
  void recv_biu_request();
  void recv_biu_data();
  void recv_rsp();
  void recv_dat();

  bool forward_biu_request(PldCmd cmd);
  bool forward_biu_data();
  void prepare_ring_request(p_tm_pld_t pld, PldCmd cmd);

  uint32_t master_port_ = 0;
  p_tm_ring_topology_t topology_ = nullptr;

  p_tm_com_inf_t biu_inf_ = nullptr;
  p_tm_ring_node_interface_t node_interface_ = nullptr;
  p_rr_arb_t req_rr_arb_ = nullptr;
  uint32_t reserved_req_class_ = UINT32_MAX;
};

using tm_ring_m_niu_t = TmRingMasterNiu;
using p_tm_ring_m_niu_t = std::shared_ptr<tm_ring_m_niu_t>;

inline p_tm_ring_m_niu_t tm_make_ring_master_niu(
    const std::string& name, tm_engine::p_tm_clk_t clk, uint32_t master_port,
    uint32_t inject_depth, uint32_t eject_depth) {
  return std::make_shared<TmRingMasterNiu>(
      name, clk, master_port, inject_depth, eject_depth);
}

#endif  // _TM_RING_MASTER_NIU_H_
