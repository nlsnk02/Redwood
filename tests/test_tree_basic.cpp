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
