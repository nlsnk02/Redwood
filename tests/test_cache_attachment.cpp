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
