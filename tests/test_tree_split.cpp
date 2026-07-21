#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreeSplitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_split.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
    tree_->set_probabilities(0.0, 0.0);  // all go to leaf cache, simplify test
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreeSplitTest, LeafSplitOnFlush) {
  // Insert enough entries, flushing after each put to fill leaf index
  for (uint64_t i = 0; i < cbtree::kLeafFanout + 1; ++i) {
    ASSERT_EQ(tree_->put(i, i), cbtree::Status::Ok);
    ASSERT_EQ(tree_->debug_flush_all(), cbtree::Status::Ok);
  }
  EXPECT_GE(tree_->debug_height(), 2);
  // All keys still readable
  for (uint64_t i = 0; i < cbtree::kLeafFanout + 1; ++i) {
    auto r = tree_->get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok) << "key=" << i;
    EXPECT_EQ(r.value, i);
  }
}

TEST_F(TreeSplitTest, SplitPreservesCache) {
  for (uint64_t i = 0; i < cbtree::kLeafFanout + 1; ++i) {
    ASSERT_EQ(tree_->put(i, i), cbtree::Status::Ok);
    ASSERT_EQ(tree_->debug_flush_all(), cbtree::Status::Ok);
  }
  // After split, all leaves and parent have caches
  EXPECT_GE(tree_->debug_height(), 2);
  EXPECT_TRUE(tree_->debug_all_leaves_have_cache());
  // New root (if height==2) should have cache
  if (tree_->debug_height() == 2) {
    EXPECT_TRUE(tree_->debug_root_has_cache());
  }
}
