#include <gtest/gtest.h>

#include "tm_ring_types.h"

TEST(TmRingStatsTest, ConnTotalStallsIncludesSendReject) {
  TmRingConnStats stats;
  stats.downstream_register_full_stall = 2;
  stats.serialization_busy_stall = 3;
  stats.pipeline_full_stall = 5;
  stats.send_reject_stall = 7;

  EXPECT_EQ(uint64_t(17), tm_ring_conn_total_stalls(stats));
}
