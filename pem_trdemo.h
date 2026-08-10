#ifndef _PEM_TRDEMO_H_
#define _PEM_TRDEMO_H_

#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>
#include "tm_engine.h"
#include "tm_que.h"
#include "tm_inf.h"
#include "tm_mem.h"
#include "tm_pld.h"
#include "fp.h"
#include "isa.h"
#include "pem_log.h"
#include "pem_biu.h"

using namespace std;
using namespace tm_engine;

/*
 * pem_trdemo.h:
 * 一个轻量示例模块，用来驱动 PemBiu 产生读写事务，
 * 方便联调 bus/ring/mem 这一整条路径。
 */

const static uint BAND_WIDTH = 128;

// ----------------------------------------------
// UOP 结构体（精简版）
// ----------------------------------------------
using demo_uop_t = struct DemoUop
{
    uint32_t vld_time = 0;
    uint32_t req_id = 0;
    uint64_t addr = 0;
    uint32_t size = 0;
    uint64_t result = 0;

    DemoUop() {}
    DemoUop(uint32_t vt, uint32_t rid, uint64_t a, uint32_t s)
        : vld_time(vt), req_id(rid), addr(a), size(s), result(0) {}
};

using p_demo_uop_t = std::shared_ptr<demo_uop_t>;

// 流水线延迟参数
const static int UOP_GEN_LATCY = 2;
const static int CALC_LATCY = 6;

const uint32_t TOTAL_UOP_COUNT = 1600;
const uint32_t START_ADDR = 0x30000000;
const uint32_t END_ADDR = 0x40000000;

struct PairEntry
{
    uint64_t wr_addr = 0;
    uint32_t size = 0;
    std::vector<uint8_t> stored_data;
    bool has_data = false;

    void reset()
    {
        wr_addr = 0;
        size = 0;
        stored_data.clear();
        has_data = false;
    }
};

struct PemTrDemoStats
{
    uint64_t read_requests = 0;
    uint64_t read_responses = 0;
    uint64_t completed_pairs = 0;
    uint64_t write_requests = 0;
    uint64_t write_responses = 0;
    uint64_t read_bytes = 0;
    uint64_t write_bytes = 0;

    uint64_t read_send_stalls = 0;
    uint64_t read_response_stalls = 0;
    uint64_t write_send_stalls = 0;
    uint64_t write_buffer_stalls = 0;
    uint64_t protocol_errors = 0;

    uint64_t first_read_cycle = 0;
    uint64_t last_read_response_cycle = 0;
    uint64_t last_write_response_cycle = 0;
    bool has_first_read_cycle = false;
    bool has_last_read_response_cycle = false;
    bool has_last_write_response_cycle = false;

    uint64_t read_latency_sum = 0;
    uint64_t read_latency_min = std::numeric_limits<uint64_t>::max();
    uint64_t read_latency_max = 0;
    uint64_t write_latency_sum = 0;
    uint64_t write_latency_min = std::numeric_limits<uint64_t>::max();
    uint64_t write_latency_max = 0;
};

// ----------------------------------------------
// PemTrDemo 类
// ----------------------------------------------
class PemTrDemo : public tm_engine::TmModule
{
public:
    PemTrDemo(const std::string &name, tm_engine::p_tm_clk_t clk);
    ~PemTrDemo();

public:
    virtual void config() override;
    void configure_traffic(uint64_t start_addr, uint64_t end_addr,
                           uint32_t total_uop_count,
                           uint32_t read_size_bytes = BAND_WIDTH,
                           uint32_t read_stride_bytes = BAND_WIDTH,
                           uint32_t write_stride_bytes = sizeof(uint32_t));
    virtual void attach(p_pem_biu_t biu);
    virtual void build() override;
    virtual void reset() override;
    virtual bool idle() override;
    const PemTrDemoStats& traffic_stats() const { return stats_; }

public:
    tm_engine::p_tm_clk_t clk_ = nullptr;
    p_tm_mem_t mem_ = nullptr;
    p_pem_biu_t biu_ = nullptr;

    p_tm_com_inf_t read_port_ = nullptr;
    p_tm_com_inf_t write_port_ = nullptr;

    p_tm_que_t<p_isa_t> instr_que_ = nullptr;

    p_tm_que_t<p_demo_uop_t> uop_que_ = nullptr;
    p_tm_que_t<p_demo_uop_t> pipe_que_ = nullptr;

    p_logger_t rd_log_ = nullptr;
    p_logger_t wr_log_ = nullptr;
    p_logger_t log_ = nullptr;

public:
    void gen_uop();
    void read_mem();
    void recv_rsp();
    void calc_pair(uint64_t wr_addr);
    void pipeline();
    void wr_recv_rsp();

private:
    uint32_t current_uop_count_ = 0;
    uint64_t start_addr_ = START_ADDR;
    uint64_t end_addr_ = END_ADDR;
    uint32_t total_uop_count_ = TOTAL_UOP_COUNT;
    uint32_t read_size_bytes_ = BAND_WIDTH;
    uint32_t read_stride_bytes_ = BAND_WIDTH;
    uint32_t write_stride_bytes_ = sizeof(uint32_t);

    std::unordered_map<uint64_t, PairEntry> pair_buffer_;
    uint32_t max_pair_entries_ = TOTAL_UOP_COUNT;
    static constexpr uint32_t WRITE_BUF_POOL_SIZE = 2048;
    uint8_t write_buf_pool_[WRITE_BUF_POOL_SIZE][4];
    std::queue<uint32_t> free_write_buf_ids_;
    std::unordered_map<uint64_t, uint64_t> read_issue_cycles_;
    // Target grant may replace tnx_id with a DBID; gid survives end to end.
    std::unordered_map<uint64_t, uint32_t> write_buffer_ids_;
    std::unordered_map<uint64_t, uint64_t> write_issue_cycles_;
    PemTrDemoStats stats_;

    bool handle_read_response(p_tm_pld_t rd_resp);
    void record_read_response(uint64_t addr);
    uint32_t allocate_write_buf();
    void release_write_buf(uint32_t id);
};

#endif  // _PEM_TRDEMO_H_
