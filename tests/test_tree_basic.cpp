#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include "cbtree/tree.hpp"

class TreeBasicTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_basic.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreeBasicTest, EmptyGetNotFound) {
  auto r = tree_->get(1);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
}

TEST_F(TreeBasicTest, PutGet) {
  ASSERT_EQ(tree_->put(1, 10), cbtree::Status::Ok);
  auto r = tree_->get(1);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 10u);
}

TEST_F(TreeBasicTest, PutMultipleGet) {
  for (uint64_t i = 0; i < 10; ++i) {
    ASSERT_EQ(tree_->put(i, i * 10), cbtree::Status::Ok);
  }
  for (uint64_t i = 0; i < 10; ++i) {
    auto r = tree_->get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok);
    EXPECT_EQ(r.value, i * 10);
  }
}

TEST_F(TreeBasicTest, PutUpdatesExisting) {
  ASSERT_EQ(tree_->put(5, 50), cbtree::Status::Ok);
  ASSERT_EQ(tree_->put(5, 99), cbtree::Status::Ok);
  auto r = tree_->get(5);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 99u);
}

TEST_F(TreeBasicTest, InitialHeightIsOne) {
  EXPECT_EQ(tree_->debug_height(), 1);
}

// Exercise CLOCK eviction and SSD write/read path:
// kCacheSlots = 16; inserting 20 keys forces eviction.
TEST_F(TreeBasicTest, EvictDirtyToSSD) {
  for (uint64_t i = 0; i < 20; ++i) {
    ASSERT_EQ(tree_->put(i, i * 100), cbtree::Status::Ok);
  }
  // All 20 keys must be retrievable — some will have been evicted to SSD
  for (uint64_t i = 0; i < 20; ++i) {
    auto r = tree_->get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok) << "key " << i;
    EXPECT_EQ(r.value, i * 100) << "key " << i;
  }
}

// =============================================================================
// Task 9: Multi-level tree with P_parent cache routing
// =============================================================================

TEST(TreeMultiLevel, DebugTwoLeaves) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  EXPECT_EQ(t.debug_height(), 2);
  // Parent cache exists and is initially empty
  EXPECT_FALSE(t.debug_parent_cache_contains(10));
}

TEST(TreeMultiLevel, PutToParentCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(1.0, 0.0);  // always go to parent cache
  ASSERT_EQ(t.put(10, 1), cbtree::Status::Ok);
  EXPECT_TRUE(t.debug_parent_cache_contains(10));
}

TEST(TreeMultiLevel, PutToLeafCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(0.0, 0.0);  // never go to parent cache
  ASSERT_EQ(t.put(10, 1), cbtree::Status::Ok);
  // Parent cache should NOT have the key
  EXPECT_FALSE(t.debug_parent_cache_contains(10));
}

TEST(TreeMultiLevel, DescendFindsCorrectLeaf) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(0.0, 0.0);
  // Keys should route to correct leaf based on separator
  ASSERT_EQ(t.put(5, 50), cbtree::Status::Ok);
  ASSERT_EQ(t.put(50, 500), cbtree::Status::Ok);
  EXPECT_EQ(t.get(5).value, 50u);
  EXPECT_EQ(t.get(50).value, 500u);
}

TEST(TreeMultiLevel, GetChecksParentCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(1.0, 0.0);  // put into parent cache
  ASSERT_EQ(t.put(42, 420), cbtree::Status::Ok);
  // get should find it in parent cache
  auto r = t.get(42);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 420u);
}

TEST(TreeMultiLevel, GetFromLeafCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(0.0, 0.0);  // put into leaf cache
  ASSERT_EQ(t.put(7, 77), cbtree::Status::Ok);
  // Not in parent cache
  EXPECT_FALSE(t.debug_parent_cache_contains(7));
  // Still findable via leaf
  auto r = t.get(7);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 77u);
}

TEST(TreeMultiLevel, GetNotFoundMultiLevel) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  auto r = t.get(999);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
}

TEST(TreeMultiLevel, DegenerateHeight1StillWorks) {
  auto t = cbtree::Tree("/tmp/test_ml_deg.pages");
  EXPECT_EQ(t.debug_height(), 1);
  ASSERT_EQ(t.put(1, 10), cbtree::Status::Ok);
  auto r = t.get(1);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 10u);
}
