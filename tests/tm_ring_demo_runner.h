#ifndef _TM_RING_DEMO_RUNNER_H_
#define _TM_RING_DEMO_RUNNER_H_

#include <fstream>
#include <stdexcept>

#include "pem_trdemo.h"
#include "../tools/tm_ring_demo_report.h"

namespace tm_ring_demo {

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
  std::vector<std::shared_ptr<DemoT> > demos;
  for (uint32_t master = 0; master < ring_cfg->num_masters; ++master) {
    auto biu = std::make_shared<pem_biu_t>(
        "biu" + std::to_string(master), clk, biu_cfg);
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
                                bius, demos, targets, clk->time(), &failures);
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

#endif  // _TM_RING_DEMO_RUNNER_H_
