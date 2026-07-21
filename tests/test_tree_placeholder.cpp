#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreePlaceholderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_ph.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
    tree_->set_probabilities(0.0, 1.0);  // P_placeholder=1 for deterministic tests
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreePlaceholderTest, NotFoundCreatesAbsent) {
  auto r = tree_->get(999);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
  // Second get should hit ABSENT in cache
  auto r2 = tree_->get(999);
  EXPECT_EQ(r2.status, cbtree::Status::NotFound);
}

TEST_F(TreePlaceholderTest, WriteFillsPlaceholder) {
  // get miss creates placeholder/ABSENT
  ASSERT_EQ(tree_->get(3).status, cbtree::Status::NotFound);
  // then put same key
  ASSERT_EQ(tree_->put(3, 30), cbtree::Status::Ok);
  auto r = tree_->get(3);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 30u);
}

TEST_F(TreePlaceholderTest, GetExistingAfterAbsent) {
  // First confirm not found
  ASSERT_EQ(tree_->get(7).status, cbtree::Status::NotFound);
  // Write
  ASSERT_EQ(tree_->put(7, 77), cbtree::Status::Ok);
  // put should convert ABSENT to OCCUPIED
  auto r = tree_->get(7);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 77u);
}
