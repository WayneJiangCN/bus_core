#ifndef _TM_RING_TOPOLOGY_H_
#define _TM_RING_TOPOLOGY_H_

#include <stdint.h>

#include <vector>

#include "tm_ring_types.h"

/*
 * Ring 拓扑辅助模块。
 *
 * 当前模型是一维双向 Ring。每个 Cross Station 是一个带 LOCAL
 * 注入/弹出端口的 ring stop；Target 存储分区尽量均匀分布；
 * CW 为顺时针，CCW 为逆时针。
 */
class TmRingTopology
{
  public:
    // 根据 Master 数量和 targets.size() 生成 Ring stop 布局。
    void config(p_tm_ring_cfg_t cfg);
    // 地址优先匹配非默认 Target；无匹配时回退到配置中的默认 Target。
    uint32_t decode_target(uint64_t addr) const;

    uint32_t v_ring_count() const;
    uint32_t v_ring_station_count(uint32_t ring_id) const;
    uint32_t h_ring_station_count() const;
    TmRingLocation master_location(uint32_t master_port) const;
    TmRingLocation ha_location(uint32_t target_id) const;
    TmRingLocation l2_location(uint32_t target_id) const;
    TmRingLocation rbrg_v_location(uint32_t v_ring_id) const;
    TmRingLocation rbrg_h_location(uint32_t v_ring_id) const;
    uint32_t master_vring(uint32_t master_port) const;
    TmRingPortDir route_direction(const TmRingLocation& src,
                                  const TmRingLocation& dst) const;
    uint32_t fanout_span(
        const TmRingLocation& src,
        const std::vector<TmRingLocation>& recipients,
        TmRingPortDir direction) const;
    uint32_t neighbor_station(TmRingDomainType type, uint32_t ring_id,
                              uint32_t station_id,
                              TmRingPortDir direction) const;

  private:
    // 地址命中既要满足地址范围，也要满足可选 interleave slice。
    bool target_matches(uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const;
    uint64_t calc_home_line(uint64_t addr,
                            p_tm_ring_target_cfg_t target_cfg) const;
    uint32_t calc_linear_slice(uint64_t addr,
                               p_tm_ring_target_cfg_t target_cfg) const;
    uint32_t calc_xor_hash_slice(uint64_t addr,
                                 p_tm_ring_target_cfg_t target_cfg) const;
    uint32_t calc_interleave_slice(uint64_t addr,
                                   p_tm_ring_target_cfg_t target_cfg) const;

    p_tm_ring_cfg_t cfg_ = nullptr;
    std::vector<TmRingLocation> master_locations_;
    std::vector<TmRingLocation> ha_locations_;
    std::vector<TmRingLocation> l2_locations_;
    std::vector<TmRingLocation> rbrg_v_locations_;
    std::vector<TmRingLocation> rbrg_h_locations_;
    std::vector<uint32_t> v_ring_station_counts_;
    uint32_t h_ring_station_count_ = 0;
};

#endif  // _TM_RING_TOPOLOGY_H_
