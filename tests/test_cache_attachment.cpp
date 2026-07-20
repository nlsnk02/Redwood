#include <gtest/gtest.h>
#include "cbtree/cache_attachment.hpp"

TEST(CacheAttachment, UpsertAndLookup) {
  cbtree::CacheAttachment c;
  EXPECT_EQ(c.upsert(10, 100), cbtree::Status::Ok);
  auto r = c.lookup(10);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 100u);
}

TEST(CacheAttachment, LookupMiss) {
  cbtree::CacheAttachment c;
  auto r = c.lookup(42);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
}

TEST(CacheAttachment, AbsentMarkAndLookup) {
  cbtree::CacheAttachment c;
  EXPECT_EQ(c.mark_absent(7), cbtree::Status::Ok);
  auto r = c.lookup(7);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
  EXPECT_TRUE(c.has_absent(7));
}

TEST(CacheAttachment, UpsertOverAbsent) {
  cbtree::CacheAttachment c;
  c.mark_absent(5);
  EXPECT_EQ(c.upsert(5, 55), cbtree::Status::Ok);
  auto r = c.lookup(5);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 55u);
  EXPECT_FALSE(c.has_absent(5));
}

TEST(CacheAttachment, PlaceholderFlow) {
  cbtree::CacheAttachment c;
  int idx = -1;
  EXPECT_EQ(c.try_place_placeholder(5, &idx), cbtree::Status::Ok);
  EXPECT_GE(idx, 0);
  auto r1 = c.lookup(5);
  EXPECT_EQ(r1.status, cbtree::Status::NotFound);  // placeholder doesn't count as hit
  EXPECT_EQ(c.fill_placeholder(idx, 55), cbtree::Status::Ok);
  auto r2 = c.lookup(5);
  EXPECT_EQ(r2.status, cbtree::Status::Ok);
  EXPECT_EQ(r2.value, 55u);
}

TEST(CacheAttachment, PlaceholderAbsentFill) {
  cbtree::CacheAttachment c;
  int idx = -1;
  c.try_place_placeholder(99, &idx);
  EXPECT_EQ(c.fill_placeholder_absent(idx), cbtree::Status::Ok);
  EXPECT_TRUE(c.has_absent(99));
}

TEST(CacheAttachment, UpsertUpdatesExisting) {
  cbtree::CacheAttachment c;
  c.upsert(1, 100);
  EXPECT_EQ(c.upsert(1, 200), cbtree::Status::Ok);
  auto r = c.lookup(1);
  EXPECT_EQ(r.value, 200u);
}

TEST(CacheAttachment, OccupiedCount) {
  cbtree::CacheAttachment c;
  EXPECT_EQ(c.occupied_count(), 0);
  c.upsert(1, 10);
  EXPECT_EQ(c.occupied_count(), 1);
  c.upsert(2, 20);
  EXPECT_EQ(c.occupied_count(), 2);
}

TEST(CacheAttachment, WriteFillsPlaceholder) {
  cbtree::CacheAttachment c;
  int idx = -1;
  c.try_place_placeholder(3, &idx);
  // upsert should fill the placeholder in-place
  EXPECT_EQ(c.upsert(3, 30), cbtree::Status::Ok);
  auto r = c.lookup(3);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 30u);
}

TEST(CacheAttachment, ClockEvictsOccupiedSlot) {
  cbtree::CacheAttachment c;
  // Fill all slots with OCCUPIED entries
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(c.upsert(i, i), cbtree::Status::Ok);
  }
  cbtree::Key victim_key{};
  cbtree::Value victim_val{};
  bool dirty = false;
  ASSERT_EQ(c.pick_clock_victim(&victim_key, &victim_val, &dirty), cbtree::Status::Ok);
  // Victim should be one of our keys
  EXPECT_GE(victim_key, 0u);
  EXPECT_LE(victim_key, 15u);
  EXPECT_TRUE(dirty);
  // After eviction, the victim should not be findable
  EXPECT_EQ(c.lookup(victim_key).status, cbtree::Status::NotFound);
  // And we can now insert a new entry
  ASSERT_EQ(c.upsert(100, 100), cbtree::Status::Ok);
}

TEST(CacheAttachment, ClockSkipsPlaceholder) {
  cbtree::CacheAttachment c;
  // Fill all slots with OCCUPIED
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(c.upsert(i, i), cbtree::Status::Ok);
  }
  // Evict one to make room
  cbtree::Key vk; cbtree::Value vv; bool vd;
  ASSERT_EQ(c.pick_clock_victim(&vk, &vv, &vd), cbtree::Status::Ok);
  // Place a placeholder in the empty slot
  int idx = -1;
  ASSERT_EQ(c.try_place_placeholder(999, &idx), cbtree::Status::Ok);
  // Evict again: placeholder should NOT be the victim
  ASSERT_EQ(c.pick_clock_victim(&vk, &vv, &vd), cbtree::Status::Ok);
  EXPECT_NE(vk, 999u);
}

TEST(CacheAttachment, SplitByMid) {
  cbtree::CacheAttachment left, right;
  left.upsert(1, 1);
  left.upsert(5, 5);
  left.upsert(9, 9);
  ASSERT_EQ(left.split_into(5, &right), cbtree::Status::Ok);
  // key < mid stays left
  EXPECT_EQ(left.lookup(1).status, cbtree::Status::Ok);
  EXPECT_EQ(left.lookup(5).status, cbtree::Status::NotFound);
  // key >= mid moves right
  EXPECT_EQ(right.lookup(5).status, cbtree::Status::Ok);
  EXPECT_EQ(right.lookup(9).status, cbtree::Status::Ok);
}

TEST(CacheAttachment, OccupiedSorted) {
  cbtree::CacheAttachment c;
  c.upsert(3, 30);
  c.upsert(1, 10);
  c.upsert(2, 20);
  auto sorted = c.occupied_sorted();
  ASSERT_EQ(sorted.size(), 3u);
  EXPECT_EQ(sorted[0].first, 1u);
  EXPECT_EQ(sorted[1].first, 2u);
  EXPECT_EQ(sorted[2].first, 3u);
}
