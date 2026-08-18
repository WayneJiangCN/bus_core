#ifndef _TM_RING_PERF_MASTER_H_
#define _TM_RING_PERF_MASTER_H_

#include <stdint.h>

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pem_biu.h"
#include "tm_engine.h"
#include "tm_ring_perf.h"

class TmRingPerfWaveCoordinator {
 public:
  explicit TmRingPerfWaveCoordinator(uint32_t masters);

  bool can_issue(uint32_t master, uint64_t wave) const;
  void record_completion(uint32_t master, uint64_t wave);
  uint64_t current_wave() const { return current_wave_; }

 private:
  uint64_t current_wave_ = 0;
  std::vector<uint64_t> completed_waves_;
};

class TmRingPerfMaster : public tm_engine::TmModule {
 public:
  TmRingPerfMaster();
  virtual ~TmRingPerfMaster();

  void config(const std::string& name, tm_engine::p_tm_clk_t clk,
              uint32_t master_port,
              const std::vector<TmRingPerfTxn>& transactions,
              const std::shared_ptr<TmRingPerfWaveCoordinator>&
                  wave_coordinator =
                      std::shared_ptr<TmRingPerfWaveCoordinator>(),
              uint32_t max_outstanding = 0);
  void attach(p_pem_biu_t biu);
  void build();
  void reset();
  bool idle() const;
  const TmRingPerfMasterStats& stats() const { return stats_; }

 protected:
  void issue();
  virtual bool send_read_candidate(const p_tm_pld_t& pld);
  virtual bool send_write_candidate(const p_tm_pld_t& pld);

 private:
  void issue_read();
  void issue_write();
  void receive_read_response();
  void receive_write_response();
  void receive_response(p_tm_com_inf_t port);
  p_tm_pld_t make_candidate(const TmRingPerfTxn& txn);

  tm_engine::p_tm_clk_t clk_ = nullptr;
  p_pem_biu_t biu_ = nullptr;
  p_tm_com_inf_t read_port_ = nullptr;
  p_tm_com_inf_t write_port_ = nullptr;
  std::vector<TmRingPerfTxn> transactions_;
  std::vector<TmRingPerfTxn> read_transactions_;
  std::vector<TmRingPerfTxn> write_transactions_;
  std::size_t next_read_transaction_ = 0;
  std::size_t next_write_transaction_ = 0;
  p_tm_pld_t pending_read_candidate_ = nullptr;
  p_tm_pld_t pending_write_candidate_ = nullptr;
  uint32_t master_port_ = 0;
  uint32_t max_outstanding_ = 0;
  std::unordered_map<uint64_t, uint64_t> issue_cycles_;
  std::unordered_map<uint64_t, uint32_t> outstanding_sizes_;
  std::unordered_map<uint64_t, uint64_t> outstanding_waves_;
  std::unordered_set<uint64_t> completed_gids_;
  std::shared_ptr<TmRingPerfWaveCoordinator> wave_coordinator_;
  TmRingPerfMasterStats stats_;
};

#endif  // _TM_RING_PERF_MASTER_H_
