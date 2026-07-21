// tests/test_tree_evict.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreeEvictTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_evict.pages").string();
  }
  void TearDown() override {
    std::filesystem::remove(path_);
  }
  std::string path_;
};

TEST_F(TreeEvictTest, ParentDemotesToLeaf) {
  auto t = cbtree::Tree::DebugTwoLeaves(path_);
  t.set_probabilities(1.0, 0.0);
  // Fill parent cache
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(t.put(1000 + i, i), cbtree::Status::Ok);
  }
  // One more put triggers parent->leaf demotion
  ASSERT_EQ(t.put(2000, 1), cbtree::Status::Ok);
  EXPECT_TRUE(t.debug_some_keys_in_leaf_cache());
}

TEST_F(TreeEvictTest, LeafDirtyFlushesToSsd) {
  cbtree::Tree t{path_};
  t.set_probabilities(0.0, 0.0);
  // Fill leaf cache (dirty, all un-flushed)
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(t.put(i, i * 10), cbtree::Status::Ok);
  }
  // Trigger leaf eviction -> flush -> lazy index registration
  ASSERT_EQ(t.put(999, 1), cbtree::Status::Ok);
  // Flush all remaining dirty data to SSD (lazy index registration)
  ASSERT_EQ(t.debug_flush_all(), cbtree::Status::Ok);
  // After flushing + clearing caches, can still read from SSD
  t.debug_clear_all_caches();
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    auto r = t.get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok) << "key=" << i;
    EXPECT_EQ(r.value, i * 10);
  }
}

TEST_F(TreeEvictTest, FlushRegistersLeafIndex) {
  cbtree::Tree t{path_};
  t.set_probabilities(0.0, 0.0);
  t.put(1, 10);
  t.put(2, 20);
  // Before flush, leaf index should be empty (lazy registration)
  EXPECT_TRUE(t.debug_leaf_index_empty());
  // Force flush
  ASSERT_EQ(t.debug_flush_all(), cbtree::Status::Ok);
  // After flush, leaf index should have entries
  EXPECT_FALSE(t.debug_leaf_index_empty());
}
