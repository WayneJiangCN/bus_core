#include "pem_trdemo.h"

#include <algorithm>
#include <iostream>

//----------------------------------------------------------------------------------------
PemTrDemo::PemTrDemo(const std::string& name, tm_engine::p_tm_clk_t clk)
    : TmModule(name), clk_(clk) {
  config();
}

PemTrDemo::~PemTrDemo() {}

//-----------------------------------------------------------------------------------------
void PemTrDemo::config() {
  instr_que_ = tm_make_que<p_isa_t>(clk_, "instr_que", tm_engine::TM_MAX_UL);
  uop_que_ = tm_make_que<p_demo_uop_t>(clk_, "uop_que", UOP_GEN_LATCY + 1,
                                       UOP_GEN_LATCY);
  read_port_ = tm_make_inf<p_tm_pld_t>(clk_, "read_port");
  write_port_ = tm_make_inf<p_tm_pld_t>(clk_, "write_port");
  pipe_que_ =
      tm_make_que<p_demo_uop_t>(clk_, "pipe_que", CALC_LATCY + 1, CALC_LATCY);
  // pipe_que_->enable_rdy_event();
  tm_sensitive(TM_MAKE_CPROC(&PemTrDemo::gen_uop), instr_que_->vld);
  tm_sensitive(TM_MAKE_CPROC(&PemTrDemo::read_mem), uop_que_->vld);
  tm_sensitive(TM_MAKE_CPROC(&PemTrDemo::recv_rsp), read_port_->vld);
  tm_sensitive(TM_MAKE_CPROC(&PemTrDemo::pipeline), pipe_que_->vld);
  // tm_sensitive(TM_MAKE_CPROC(&PemTrDemo::process_ready_pairs),
  // pipe_que_->rdy);
  tm_sensitive(TM_MAKE_CPROC(&PemTrDemo::wr_recv_rsp), write_port_->vld);
  // log
  std::string log_name = this->name() + ".log";
  log_para_t log_para = log_para_t(log_name);
  log_ = pem_log::create_logger(log_para);
  std::string rd_log_name = this->name() + "rd.log";
  log_para_t rd_log_para = log_para_t(rd_log_name);
  rd_log_ = pem_log::create_logger(rd_log_para);
  std::string wr_log_name = this->name() + "wr.log";
  log_para_t wr_log_para = log_para_t(wr_log_name);
  wr_log_ = pem_log::create_logger(wr_log_para);

  // 初始化配对缓冲区
  pair_buffer_.clear();
  for (uint32_t i = 0; i < WRITE_BUF_POOL_SIZE; ++i) {
    free_write_buf_ids_.push(i);
  }
}

void PemTrDemo::configure_traffic(uint64_t start_addr, uint64_t end_addr,
                                  uint32_t total_uop_count,
                                  uint32_t read_size_bytes,
                                  uint32_t read_stride_bytes,
                                  uint32_t write_stride_bytes) {
  start_addr_ = start_addr;
  end_addr_ = end_addr;
  total_uop_count_ = total_uop_count;
  read_size_bytes_ = read_size_bytes;
  read_stride_bytes_ = read_stride_bytes;
  write_stride_bytes_ = write_stride_bytes;
  max_pair_entries_ = total_uop_count_;
}

void PemTrDemo::attach(p_pem_biu_t biu) { biu_ = biu; }

void PemTrDemo::build() {
  read_port_->connect(biu_->v_dcache_rd_inf_[0]);
  write_port_->connect(biu_->v_dcache_wr_inf_[0]);
}

bool PemTrDemo::idle() {
  return instr_que_->empty() && uop_que_->empty() && pipe_que_->empty() &&
         pair_buffer_.empty() && read_issue_cycles_.empty() &&
         write_buffer_ids_.empty() && write_issue_cycles_.empty() &&
         free_write_buf_ids_.size() == WRITE_BUF_POOL_SIZE &&
         read_port_->idle() && write_port_->idle();
}

void PemTrDemo::reset() {
  current_uop_count_ = 0;
  pair_buffer_.clear();
  read_issue_cycles_.clear();
  write_buffer_ids_.clear();
  write_issue_cycles_.clear();
  stats_ = PemTrDemoStats{};
  while (!free_write_buf_ids_.empty()) {
    free_write_buf_ids_.pop();
  }
  for (uint32_t i = 0; i < WRITE_BUF_POOL_SIZE; ++i) {
    free_write_buf_ids_.push(i);
  }
  instr_que_->clear();
  uop_que_->clear();
  pipe_que_->clear();
}

void PemTrDemo::gen_uop() {
  if (instr_que_->valid() && !instr_que_->empty()) {
    auto instr = instr_que_->front();
    if (instr == nullptr) {
      std::cout << "ERROR: instr is nullptr!" << std::endl;
      instr_que_->pop_front();
      return;
    }

    while (current_uop_count_ < total_uop_count_ && !uop_que_->full() &&
           instr != nullptr) {
      uint64_t src_addr =
          instr->start_addr_ + current_uop_count_ * read_stride_bytes_;
      int vld_time = time();
      auto uop = make_shared<demo_uop_t>(vld_time, current_uop_count_, src_addr,
                                         read_size_bytes_);

      uop_que_->push_back(uop);
      current_uop_count_++;
    }
    if (current_uop_count_ == total_uop_count_) {
      current_uop_count_ = 0;
      instr_que_->pop_front();
    }
  }
}
void PemTrDemo::read_mem() {
  if (uop_que_->empty()) return;
  auto uop = uop_que_->front();
  auto rd_pld = tm_make_pld(PldCmd::RD, uop->addr, uop->size);
  rd_pld->buf_u8 = make_shared<vector<uint8_t>>(uop->size, 0);
  rd_pld->data = rd_pld->buf_u8->data();
  if (read_port_->send(rd_pld)) {
    uop_que_->pop_front();
    const uint64_t now = static_cast<uint64_t>(time());
    read_issue_cycles_[uop->addr] = now;
    ++stats_.read_requests;
    stats_.read_bytes += uop->size;
    if (!stats_.has_first_read_cycle) {
      stats_.first_read_cycle = now;
      stats_.has_first_read_cycle = true;
    }
    PEM_LOG_INFO(rd_log_,
                 "M2,time:{2:d} ,  发送读请求,UOP[{0:d}], 地址=0x{1:x}",
                 uop->req_id, uop->addr, time());
  } else {
    ++stats_.read_send_stalls;
    PEM_LOG_INFO(rd_log_, "time:{0:d},read_port_->send失败", time());
  }
}
void PemTrDemo::recv_rsp() {
  if (!read_port_->valid()) return;
  auto rd_resp = read_port_->get_pld();
  if (rd_resp == nullptr) return;

  uint32_t req_id = (rd_resp->addr - start_addr_) / read_stride_bytes_;
  bool success = handle_read_response(rd_resp);
  if (success) {
    record_read_response(rd_resp->addr);
    read_port_->pop_pld();
  } else
    std::cout << "WARNING: 读响应处理失败，req_id=" << req_id << std::endl;
}
bool PemTrDemo::handle_read_response(p_tm_pld_t rd_resp) {
  uint64_t addr = rd_resp->addr;
  uint32_t size = rd_resp->size;
  if (addr < start_addr_ || (addr - start_addr_) % read_stride_bytes_ != 0 ||
      size != read_size_bytes_) {
    ++stats_.protocol_errors;
    PEM_LOG_ERROR(rd_log_, "Invalid RD response: addr=0x{0:x}, size={1:d}",
                  addr, size);
    return false;
  }
  uint32_t req_id = (addr - start_addr_) / read_stride_bytes_;
  if (req_id >= total_uop_count_) {
    ++stats_.protocol_errors;
    PEM_LOG_ERROR(rd_log_, "RD response req_id out of range: {0:d}", req_id);
    return false;
  }
  uint64_t wr_addr = end_addr_ + (req_id / 2) * write_stride_bytes_;
  PEM_LOG_INFO(log_, "Pair[{0:x}] req_id:{1:d},pair_buffer_.size():{2:d}",
               wr_addr, req_id, pair_buffer_.size());
  PEM_LOG_INFO(rd_log_, "time:{0:d},Pair[{1:x}] 到达 (req_id={2:d})", time(),
               wr_addr, req_id);
  if (rd_resp->data == nullptr) {
    ++stats_.protocol_errors;
    PEM_LOG_ERROR(rd_log_, "RD response data is nullptr, req_id={0:d}", req_id);
    return false;
  }
  std::vector<uint8_t> ptr(size);
  memcpy(ptr.data(), rd_resp->data, size);
  auto it = pair_buffer_.find(wr_addr);

  if (it == pair_buffer_.end()) {
    if (pair_buffer_.size() >= max_pair_entries_) {
      ++stats_.read_response_stalls;
      PEM_LOG_ERROR(rd_log_, "Pair Buffer 已满，无法处理 req_id={{0:d}}",
                    req_id);
      return false;
    }
    PairEntry entry;
    entry.wr_addr = wr_addr;
    entry.size = size;
    entry.stored_data.resize(size);
    memcpy(entry.stored_data.data(), ptr.data(), size);
    entry.has_data = true;
    pair_buffer_[wr_addr] = entry;
    return true;
  }
  if (pipe_que_->full()) {
    ++stats_.read_response_stalls;
    return false;
  }
  PairEntry& entry = it->second;
  if (!entry.has_data) {
    PEM_LOG_ERROR(rd_log_, "Pair[{0:x}] 状态异常：has_data=false", wr_addr);
    return false;
  }

  uint32_t count = size / sizeof(uint32_t);
  const uint32_t* p1 =
      reinterpret_cast<const uint32_t*>(entry.stored_data.data());
  const uint32_t* p2 = reinterpret_cast<const uint32_t*>(ptr.data());
  uint64_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) {
    sum += p1[i] + p2[i];
  }
  auto result_uop =
      make_shared<demo_uop_t>(time(), 0, wr_addr, sizeof(uint32_t));
  result_uop->result = sum;
  if (!pipe_que_->full()) {
    pipe_que_->push_back(result_uop);
    ++stats_.completed_pairs;
    PEM_LOG_INFO(log_, "Pair[{0:x}] 累加完成, 结果={1:d}", wr_addr, sum);
  } else {
    PEM_LOG_ERROR(log_, "pipe_que_ 已满，Pair[{0:x}] 结果丢失!", wr_addr);
    return false;
  }
  pair_buffer_.erase(it);
  return true;
}

void PemTrDemo::record_read_response(uint64_t addr) {
  ++stats_.read_responses;
  auto it = read_issue_cycles_.find(addr);
  if (it == read_issue_cycles_.end()) {
    ++stats_.protocol_errors;
    return;
  }

  const uint64_t now = static_cast<uint64_t>(time());
  const uint64_t latency = now - it->second;
  stats_.last_read_response_cycle = now;
  stats_.has_last_read_response_cycle = true;
  stats_.read_latency_sum += latency;
  stats_.read_latency_min = std::min(stats_.read_latency_min, latency);
  stats_.read_latency_max = std::max(stats_.read_latency_max, latency);
  read_issue_cycles_.erase(it);
}

void PemTrDemo::pipeline() {
  if (pipe_que_->empty()) return;
  p_demo_uop_t uop = pipe_que_->front();
  uint32_t buf_id = allocate_write_buf();
  if (buf_id == UINT32_MAX) {
    ++stats_.write_buffer_stalls;
    return;
  }
  PEM_LOG_INFO(log_, "time:{0:d},pipe_que_", clk_->time());

  uint32_t value = static_cast<uint32_t>(uop->result);
  uint8_t* write_buf = write_buf_pool_[buf_id];
  memcpy(write_buf, &value, sizeof(value));
  auto wr_pld = tm_make_pld(PldCmd::WR, uop->addr, uop->size, write_buf);
  wr_pld->tnx_id = buf_id;
  if (write_port_->send(wr_pld)) {
    pipe_que_->pop_front();
    const uint64_t now = static_cast<uint64_t>(time());
    write_buffer_ids_[wr_pld->gid] = buf_id;
    write_issue_cycles_[wr_pld->gid] = now;
    ++stats_.write_requests;
    stats_.write_bytes += uop->size;
    PEM_LOG_INFO(wr_log_,
                 "wr_send,time:{3:d},size : {0:d}, 地址=0x{1:x}, 数据={2:x}",
                 wr_pld->size, uop->addr, value, time());
  } else {
    ++stats_.write_send_stalls;
    release_write_buf(buf_id);
    std::cout << "write_port_->send失败" << std::endl;
  }

}

void PemTrDemo::wr_recv_rsp() {
  if (!write_port_->valid()) {
    return;
  }

  auto wr_resp = write_port_->pop_pld();
  if (wr_resp == nullptr) {
    ++stats_.protocol_errors;
    std::cout << "ERROR: recv_rsp() - rd_resp is nullptr!" << std::endl;
    return;
  }
  auto buffer_it = write_buffer_ids_.find(wr_resp->gid);
  auto cycle_it = write_issue_cycles_.find(wr_resp->gid);
  uint32_t buf_id = 0;
  if (buffer_it == write_buffer_ids_.end() ||
      cycle_it == write_issue_cycles_.end()) {
    ++stats_.protocol_errors;
    std::cout << "ERROR: unexpected write response gid=" << wr_resp->gid
              << std::endl;
    return;
  }
  buf_id = buffer_it->second;
  const uint64_t now = static_cast<uint64_t>(time());
  const uint64_t latency = now - cycle_it->second;
  stats_.write_latency_sum += latency;
  stats_.write_latency_min = std::min(stats_.write_latency_min, latency);
  stats_.write_latency_max = std::max(stats_.write_latency_max, latency);
  ++stats_.write_responses;
  stats_.last_write_response_cycle = now;
  stats_.has_last_write_response_cycle = true;
  write_buffer_ids_.erase(buffer_it);
  write_issue_cycles_.erase(cycle_it);
  release_write_buf(buf_id);
  PEM_LOG_INFO(wr_log_, "wr_recv_rsp,time:{3:d},size : {0:d}, 地址=0x{1:x}, 数据={2:x}",
               wr_resp->size, wr_resp->addr,
               wr_resp->data == nullptr ? 0 : wr_resp->data[0], time());
}

uint32_t PemTrDemo::allocate_write_buf() {
  if (free_write_buf_ids_.empty()) return UINT32_MAX;
  uint32_t id = free_write_buf_ids_.front();
  free_write_buf_ids_.pop();
  return id;
}
void PemTrDemo::release_write_buf(uint32_t id) {
  if (id >= WRITE_BUF_POOL_SIZE) return;
  free_write_buf_ids_.push(id);
}
//--------------------------------------------------------------------------------------------
// model
