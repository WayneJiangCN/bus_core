#ifndef _TM_BUS_TYPES_H_
#define _TM_BUS_TYPES_H_

#include <stdint.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "tm_mem.h"

enum class tm_bus_interleave_type_t
{
    LINEAR,
    XOR_HASH
};

struct TmBusTargetCfg
{
    std::string name = "";
    uint64_t addr_begin = 0;
    uint64_t size = 0;
    bool is_default = false;

    tm_bus_interleave_type_t interleave_type =
        tm_bus_interleave_type_t::LINEAR;
    uint32_t interleave_size = 0;
    uint32_t sector_size = 0;
    uint32_t interleave_num = 1;
    uint32_t interleave_idx = 0;
    uint32_t interleave_hash_shift = 6;
    uint32_t interleave_hash_seed = 0;

    uint32_t frontend_latency = 1;
    uint32_t forward_latency = 0;
    uint32_t response_latency = 1;
    uint32_t header_latency = 1;
    uint32_t width = 32;

    uint32_t rd_req_fifo_depth = 8;
    uint32_t wr_req_fifo_depth = 8;
    uint32_t wr_dat_fifo_depth = 8;

    bool contains(uint64_t addr) const
    {
        if (size == 0) {
            return false;
        }
        return addr >= addr_begin && addr < (addr_begin + size);
    }

    bool interleave_enabled() const
    {
        return interleave_num > 1 && interleave_size != 0;
    }
};

using tm_bus_target_cfg_t = TmBusTargetCfg;
using p_tm_bus_target_cfg_t = std::shared_ptr<tm_bus_target_cfg_t>;

#endif  // _TM_BUS_TYPES_H_
