#include "tm_ring_topology.h"

#include <stdexcept>

void TmRingTopology::config(p_tm_ring_cfg_t cfg) {
  cfg_ = cfg;

  const uint32_t target_num = static_cast<uint32_t>(cfg_->targets.size());
  const uint32_t v_ring_num =
      (cfg_->num_masters + cfg_->max_aicore_per_vring - 1) /
      cfg_->max_aicore_per_vring;
  const uint32_t masters_per_vring = cfg_->num_masters / v_ring_num;
  const uint32_t extra_masters = cfg_->num_masters % v_ring_num;

  master_locations_.assign(cfg_->num_masters, TmRingLocation());
  ha_locations_.assign(target_num, TmRingLocation());
  l2_locations_.assign(target_num, TmRingLocation());
  rbrg_v_locations_.assign(v_ring_num, TmRingLocation());
  rbrg_h_locations_.assign(v_ring_num, TmRingLocation());
  v_ring_station_counts_.assign(v_ring_num, 0);
  h_ring_station_count_ = v_ring_num + 2 * target_num;

  uint32_t master = 0;
  for (uint32_t ring = 0; ring < v_ring_num; ++ring) {
    const uint32_t master_count =
        masters_per_vring + (ring < extra_masters ? 1 : 0);
    rbrg_v_locations_[ring] =
        TmRingLocation(TmRingDomainType::V_RING, ring, 0);
    v_ring_station_counts_[ring] = master_count + 1;
    for (uint32_t station = 1; station <= master_count; ++station) {
      master_locations_[master++] =
          TmRingLocation(TmRingDomainType::V_RING, ring, station);
    }
  }

  for (uint32_t ring = 0; ring < v_ring_num; ++ring) {
    rbrg_h_locations_[ring] = TmRingLocation(
        TmRingDomainType::H_RING, 0,
        ring * h_ring_station_count_ / v_ring_num);
  }

  uint32_t target = 0;
  bool place_ha = true;
  for (uint32_t station = 0; station < h_ring_station_count_; ++station) {
    bool is_rbrg = false;
    for (uint32_t ring = 0; ring < v_ring_num; ++ring) {
      if (rbrg_h_locations_[ring].station_id == station) {
        is_rbrg = true;
        break;
      }
    }
    if (is_rbrg) {
      continue;
    }
    if (place_ha) {
      ha_locations_[target] =
          TmRingLocation(TmRingDomainType::H_RING, 0, station);
    } else {
      l2_locations_[target++] =
          TmRingLocation(TmRingDomainType::H_RING, 0, station);
    }
    place_ha = !place_ha;
  }
}

uint32_t TmRingTopology::decode_target(uint64_t addr) const {
  uint32_t default_target = 0;
  bool has_default = false;

  for (uint32_t i = 0; i < cfg_->targets.size(); ++i) {
    auto target_cfg = cfg_->targets[i];

    if (target_cfg->is_default) {
      default_target = i;
      has_default = true;
      continue;
    }

    if (target_matches(addr, target_cfg)) {
      return i;
    }
  }

  return has_default ? default_target : 0;
}

bool TmRingTopology::target_matches(uint64_t addr,
                                    p_tm_ring_target_cfg_t target_cfg) const {
  if (!target_cfg->contains(addr)) {
    return false;
  }

  if (target_cfg->interleave_num <= 1) {
    return true;
  }

  return calc_interleave_slice(addr, target_cfg) == target_cfg->interleave_idx;
}
// interleave_size 表示 cacheline/home 粒度。当前 L2 口径是 512B。
// 128B sector 只影响填充粒度，不参与 target/home 选择。
uint64_t TmRingTopology::calc_home_line(
    uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const {
  return (addr - target_cfg->addr_begin) / target_cfg->interleave_size;
}

uint32_t TmRingTopology::calc_linear_slice(
    uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const {
  return static_cast<uint32_t>(calc_home_line(addr, target_cfg) %
                               target_cfg->interleave_num);
}
// 计算 XOR hash 的 home slice。先按 512B 得到 home_line，再把高位
// line bit 混入低位 bank 选择，避免固定 stride 只打到少数 target。
uint32_t TmRingTopology::calc_xor_hash_slice(
    uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const {
  uint64_t home_line = calc_home_line(addr, target_cfg);
  uint64_t hashed = (home_line ^ target_cfg->interleave_hash_seed) ^
                    (home_line >> target_cfg->interleave_hash_shift) ^
                    (home_line >> (target_cfg->interleave_hash_shift * 2));
  return static_cast<uint32_t>(hashed % target_cfg->interleave_num);
}
// 根据 interleave_type 选择计算 home slice 的方法。
uint32_t TmRingTopology::calc_interleave_slice(
    uint64_t addr, p_tm_ring_target_cfg_t target_cfg) const {
  if (tm_ring_is_xor_hash_interleave(target_cfg->interleave_type)) {
    return calc_xor_hash_slice(addr, target_cfg);
  }
  return calc_linear_slice(addr, target_cfg);
}

uint32_t TmRingTopology::v_ring_count() const {
  return static_cast<uint32_t>(v_ring_station_counts_.size());
}

uint32_t TmRingTopology::v_ring_station_count(uint32_t ring_id) const {
  return v_ring_station_counts_[ring_id];
}

uint32_t TmRingTopology::h_ring_station_count() const {
  return h_ring_station_count_;
}

TmRingLocation TmRingTopology::master_location(uint32_t master_port) const {
  return master_locations_[master_port];
}

TmRingLocation TmRingTopology::ha_location(uint32_t target_id) const {
  return ha_locations_[target_id];
}

TmRingLocation TmRingTopology::l2_location(uint32_t target_id) const {
  return l2_locations_[target_id];
}

TmRingLocation TmRingTopology::rbrg_v_location(uint32_t v_ring_id) const {
  return rbrg_v_locations_[v_ring_id];
}

TmRingLocation TmRingTopology::rbrg_h_location(uint32_t v_ring_id) const {
  return rbrg_h_locations_[v_ring_id];
}

uint32_t TmRingTopology::master_vring(uint32_t master_port) const {
  return master_location(master_port).ring_id;
}

TmRingPortDir TmRingTopology::route_direction(
    const TmRingLocation& src, const TmRingLocation& dst) const {
  if (src.ring_type != dst.ring_type || src.ring_id != dst.ring_id) {
    throw std::invalid_argument("Ring locations must share a domain");
  }

  const uint32_t station_count =
      src.ring_type == TmRingDomainType::H_RING
          ? h_ring_station_count_
          : v_ring_station_counts_[src.ring_id];
  if (src.station_id == dst.station_id) {
    return TmRingPortDir::LOCAL;
  }

  const uint32_t clockwise =
      (dst.station_id + station_count - src.station_id) % station_count;
  const uint32_t counter_clockwise =
      (src.station_id + station_count - dst.station_id) % station_count;
  return clockwise <= counter_clockwise ? TmRingPortDir::CW
                                        : TmRingPortDir::CCW;
}

uint32_t TmRingTopology::fanout_span(
    const TmRingLocation& src,
    const std::vector<TmRingLocation>& recipients,
    TmRingPortDir direction) const {
  if (direction != TmRingPortDir::CW && direction != TmRingPortDir::CCW) {
    throw std::invalid_argument("Fanout direction must be CW or CCW");
  }

  uint32_t station_count = h_ring_station_count_;
  if (src.ring_type == TmRingDomainType::H_RING) {
    if (src.ring_id != 0) {
      throw std::invalid_argument("Invalid H-Ring location");
    }
  } else {
    if (src.ring_id >= v_ring_station_counts_.size()) {
      throw std::invalid_argument("Invalid V-Ring location");
    }
    station_count = v_ring_station_counts_[src.ring_id];
  }
  if (src.station_id >= station_count) {
    throw std::invalid_argument("Invalid fanout source station");
  }

  uint32_t span = 0;
  for (const TmRingLocation& recipient : recipients) {
    if (recipient.ring_type != src.ring_type ||
        recipient.ring_id != src.ring_id ||
        recipient.station_id >= station_count) {
      throw std::invalid_argument(
          "Fanout recipients must share a valid ring domain");
    }
    const uint32_t distance =
        direction == TmRingPortDir::CW
            ? (recipient.station_id + station_count - src.station_id) %
                  station_count
            : (src.station_id + station_count - recipient.station_id) %
                  station_count;
    if (distance > span) {
      span = distance;
    }
  }
  return span;
}

uint32_t TmRingTopology::neighbor_station(TmRingDomainType type,
                                          uint32_t ring_id,
                                          uint32_t station_id,
                                          TmRingPortDir direction) const {
  const uint32_t station_count =
      type == TmRingDomainType::H_RING
          ? h_ring_station_count_
          : v_ring_station_counts_[ring_id];
  if (direction == TmRingPortDir::LOCAL) {
    return station_id;
  }
  if (direction == TmRingPortDir::CW) {
    return (station_id + 1) % station_count;
  }
  return (station_id + station_count - 1) % station_count;
}
