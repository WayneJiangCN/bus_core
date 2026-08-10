#include <gtest/gtest.h>

#include "tm_ring_types.h"

namespace {

p_tm_pld_t make_packet(PldCmd cmd, uint32_t size) {
  auto pld = tm_make_pld();
  pld->ring_slot_empty = false;
  pld->ring_traffic_class = static_cast<uint32_t>(cmd);
  pld->size = size;
  return pld;
}

TEST(TmRingPacketCostTest, ControlPacketsUseHeaderBytes) {
  EXPECT_EQ(uint32_t(16), tm_ring_packet_bytes(make_packet(PldCmd::RD, 128)));
  EXPECT_EQ(uint32_t(16), tm_ring_packet_bytes(make_packet(PldCmd::WR, 128)));
  EXPECT_EQ(uint32_t(16), tm_ring_packet_bytes(make_packet(PldCmd::RSP, 128)));
  EXPECT_EQ(uint32_t(16),
            tm_ring_packet_bytes(make_packet(PldCmd::WR_RSP, 128)));
}

TEST(TmRingPacketCostTest, DataPacketsUsePayloadSize) {
  EXPECT_EQ(uint32_t(128),
            tm_ring_packet_bytes(make_packet(PldCmd::WR_DAT, 128)));
  EXPECT_EQ(uint32_t(256),
            tm_ring_packet_bytes(make_packet(PldCmd::RD_RSP, 256)));
}

TEST(TmRingPacketCostTest, SerializationRoundsUpAndKeepsEmptySlotAlive) {
  EXPECT_EQ(uint32_t(1), tm_ring_serialization_cycles(0, 128));
  EXPECT_EQ(uint32_t(1), tm_ring_serialization_cycles(16, 128));
  EXPECT_EQ(uint32_t(1), tm_ring_serialization_cycles(128, 128));
  EXPECT_EQ(uint32_t(2), tm_ring_serialization_cycles(129, 128));
  EXPECT_EQ(uint32_t(3), tm_ring_serialization_cycles(257, 128));
}

}  // namespace
