#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreeRangeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_range.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreeRangeTest, InclusiveRange) {
  for (uint64_t i = 0; i < 20; ++i)
    ASSERT_EQ(tree_->put(i, i * 10), cbtree::Status::Ok);
  auto v = tree_->scan(5, 8);
  ASSERT_EQ(v.size(), 4u);
  EXPECT_EQ(v[0].first, 5u);
  EXPECT_EQ(v[0].second, 50u);
  EXPECT_EQ(v[1].first, 6u);
  EXPECT_EQ(v[1].second, 60u);
  EXPECT_EQ(v[2].first, 7u);
  EXPECT_EQ(v[2].second, 70u);
  EXPECT_EQ(v[3].first, 8u);
  EXPECT_EQ(v[3].second, 80u);
}

TEST_F(TreeRangeTest, EmptyRange) {
  for (uint64_t i = 0; i < 5; ++i)
    tree_->put(i, i);
  auto v = tree_->scan(100, 200);
  EXPECT_TRUE(v.empty());
}

TEST_F(TreeRangeTest, RangeAfterFlush) {
  for (uint64_t i = 0; i < 10; ++i)
    ASSERT_EQ(tree_->put(i, i * 2), cbtree::Status::Ok);
  ASSERT_EQ(tree_->debug_flush_all(), cbtree::Status::Ok);
  auto v = tree_->scan(3, 7);
  ASSERT_EQ(v.size(), 5u);
  for (size_t i = 0; i < v.size(); ++i) {
    EXPECT_EQ(v[i].second, v[i].first * 2);
  }
}
