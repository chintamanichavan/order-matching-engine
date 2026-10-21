#include "rtrade/bounded_log.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace rtrade {
namespace {

TEST(BoundedLog, StartsEmptyWithItsStorageInPlace) {
  const BoundedLog<int> log(4);
  EXPECT_TRUE(log.empty());
  EXPECT_EQ(log.size(), 0u);
  EXPECT_EQ(log.capacity(), 4u);
  EXPECT_EQ(log.begin(), log.end());
}

TEST(BoundedLog, KeepsEntriesInOrderUntilFull) {
  BoundedLog<int> log(4);
  for (int i = 1; i <= 3; ++i) log.push_back(i);
  EXPECT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0], 1);
  EXPECT_EQ(log[2], 3);
  EXPECT_EQ(log.back(), 3);
}

TEST(BoundedLog, ReplacesTheOldestOnceFull) {
  BoundedLog<int> log(4);
  for (int i = 1; i <= 10; ++i) log.push_back(i);
  EXPECT_EQ(log.size(), 4u);
  EXPECT_EQ(std::vector<int>(log.begin(), log.end()), (std::vector<int>{7, 8, 9, 10}));
  EXPECT_EQ(log[0], 7);
  EXPECT_EQ(log.back(), 10);
}

TEST(BoundedLog, NeverChangesItsStorageWhileAppending) {
  BoundedLog<int> log(3);
  const int* before = &log[0];
  for (int i = 0; i < 100; ++i) log.push_back(i);
  // Same three slots throughout: the oldest entry now sits somewhere in them.
  const int* slots[] = {&log[0], &log[1], &log[2]};
  for (const int* s : slots) EXPECT_TRUE(s >= before && s < before + 3);
  EXPECT_EQ(log.capacity(), 3u);
}

}  // namespace
}  // namespace rtrade
