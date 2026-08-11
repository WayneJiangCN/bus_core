#ifndef _TM_RING_HOME_AGENT_H_
#define _TM_RING_HOME_AGENT_H_

#include <stdint.h>

#include <cstddef>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tm_pld.h"
#include "tm_ring_fanout.h"
#include "tm_ring_types.h"

struct TmRingHomeAgentConfig {
  // 一笔扁平 transaction 对应一个完整 line 后端读及其 waiter 列表。
  uint32_t line_size = 0;
  uint32_t entry_limit = 0;
  uint32_t waiters_per_entry = 0;
  uint32_t hit_rate_pct = 0;
  uint32_t hit_seed = 0;
};

enum class TmHaAcceptResult {
  ACCEPTED,
  MERGED,
  STALL_TABLE_FULL,
  STALL_WAITER_FULL,
  STALL_AGGREGATION_CLOSED,
  STALL_WRITE_HAZARD,
  // 跨 line 请求保持 MemPort 直接后端路径，避免错误合并。
  BYPASS
};

enum class TmHaTxnState {
  // 已由 HA 接收，可通过私有 L2 接口读取功能数据。
  PENDING_HIT,
  // 已由 HA 接收，可立即成为 TmMem backend candidate。
  PENDING_MISS,
  // backend request 已提交，等待完整 line 数据。
  INFLIGHT,
  // 已保存 completion data，按 waiter 生成待交给 L2 的 response candidate。
  RESPONDING
};

/*
 * Home Agent 位于 Ring target 一侧，介于 MemPort 与 TmMem 之间。
 *
 * 每个活跃表项直接对应一个 line 读事务。首个请求立即可向后端发射；同 line
 * 后续请求仅在 waiter admission 尚未关闭时追加。完成后首个有效 waiter 即可
 * 建立 provisional fanout group，并在 L2 response latency 内继续追加 waiter；
 * 单 recipient group 最终只发一个 DAT carrier，并按 unicast 统计。HA 不拥有
 * endpoint FIFO 或 interface，也不负责 Ring 路由、TmMem credit 或完整 coherence。
 */
class TmRingHomeAgent {
 public:
  explicit TmRingHomeAgent(const TmRingHomeAgentConfig& cfg);

  void reset();
  void clear_stats();

  bool idle() const {
    return entries_.empty() && write_lines_.empty() &&
           write_txn_lines_.empty();
  }

  const TmRingHomeAgentStats& stats() const { return stats_; }

  // 返回 STALL 时调用者保留 payload；ACCEPTED/MERGED 后由 HA 跟踪。
  TmHaAcceptResult accept_read(p_tm_pld_t pld);

  // 写请求不进入读表。写 hazard 持续到最终写响应完成，避免同 line 读越过写。
  bool reserve_write(p_tm_pld_t pld);
  void complete_write(p_tm_pld_t pld);

  // candidate 查询不推进状态；只有下游 send 成功后调用 commit。
  bool has_backend_request();
  p_tm_pld_t front_backend_request();
  void commit_backend_request();

  bool has_functional_read();
  p_tm_pld_t front_functional_read();
  void commit_functional_read(pld_rsp_t status);
  void record_private_l2_full_stall();

  bool accept_backend_response(p_tm_pld_t rsp);
  bool has_l2_response();
  TmRingL2ResponseCandidate front_l2_response();
  void commit_l2_response(const TmRingL2AcceptResult& result);
  bool consume_l2_group_summary(const TmRingL2GroupSummary& summary);

 private:
  struct TmHaWaiter {
    p_tm_pld_t pld = nullptr;
    uint32_t line_offset = 0;
  };

  struct TmHaReadTxn {
    uint64_t line_base = 0;
    TmHaTxnState state = TmHaTxnState::PENDING_MISS;
    bool waiter_admission_open = true;
    std::vector<TmHaWaiter> waiters;
    p_tm_pld_t backend_request = nullptr;
    p_tm_pld_t functional_request = nullptr;
    TmPldTxnKey backend_transaction_key;
    pld_rsp_t backend_response_status = PldRsp::UNDEF;
    std::shared_ptr<const std::vector<uint8_t>> completion_data = nullptr;
    uint64_t open_group_token = 0;
    TmRingL2ResponseCandidate l2_candidate;
    size_t next_response_waiter = 0;
  };

  using TmHaTxnList = std::list<TmHaReadTxn>;
  using TmHaTxnIter = TmHaTxnList::iterator;

  uint64_t line_base(uint64_t addr) const;
  bool in_one_line(p_tm_pld_t pld) const;
  bool fanout_waiter_set_supported(const TmHaReadTxn& transaction) const;
  TmHaWaiter make_waiter(p_tm_pld_t pld) const;
  void record_read(p_tm_pld_t pld);
  bool find_transaction(uint64_t req_line_base, TmHaTxnIter* transaction);
  bool find_pending_transaction(TmHaTxnIter* transaction);
  bool find_pending_functional_transaction(TmHaTxnIter* transaction);
  bool find_responding_transaction(TmHaTxnIter* transaction);
  bool has_blocking_read_transaction(uint64_t req_line_base) const;
  p_tm_pld_t make_backend_request(const TmHaReadTxn& txn) const;
  std::shared_ptr<const std::vector<uint8_t>> make_completion_data(
      p_tm_pld_t response, pld_rsp_t status) const;
  TmRingL2ResponseCandidate make_l2_candidate(
      const TmHaReadTxn& txn, const TmHaWaiter& waiter) const;
  bool classify_l2_hit(uint64_t req_line_base, uint64_t first_gid) const;
  uint64_t current_completion_buffer_bytes() const;
  void erase_transaction(TmHaTxnIter transaction);

  TmRingHomeAgentConfig cfg_;
  TmHaTxnList entries_;
  TmHaTxnIter backend_rr_cursor_;
  TmHaTxnIter backend_candidate_;
  TmHaTxnIter functional_candidate_;
  TmHaTxnIter response_rr_cursor_;
  TmHaTxnIter response_candidate_;
  std::unordered_set<uint64_t> write_lines_;
  std::unordered_map<TmPldTxnKey, uint64_t, TmPldTxnKeyHash>
      write_txn_lines_;
  TmRingHomeAgentStats stats_;
};

#endif  // _TM_RING_HOME_AGENT_H_
