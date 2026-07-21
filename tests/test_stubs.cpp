// tests/test_stubs.cpp
#include <gtest/gtest.h>
#include "cbtree/delete_ops.hpp"
#include "cbtree/wal_sink.hpp"
#include "cbtree/adaptive_policy.hpp"

TEST(Stubs, DeleteNotImplemented) {
  EXPECT_EQ(cbtree::DeleteOps::remove(1), cbtree::Status::NotImplemented);
  EXPECT_EQ(cbtree::DeleteOps::try_merge(nullptr), cbtree::Status::NotImplemented);
  EXPECT_EQ(cbtree::DeleteOps::rebalance(nullptr), cbtree::Status::NotImplemented);
}

TEST(Stubs, AdaptiveDefaults) {
  cbtree::AdaptivePolicy p;
  auto pr = p.update({});
  EXPECT_DOUBLE_EQ(pr.p_parent, cbtree::kDefaultPParent);
  EXPECT_DOUBLE_EQ(pr.p_placeholder, cbtree::kDefaultPPlaceholder);
}

TEST(Stubs, WalNoOp) {
  cbtree::WalSink wal;
  EXPECT_EQ(wal.log_insert(1, 1), cbtree::Status::Ok);
  EXPECT_EQ(wal.log_update(1, 1, 2), cbtree::Status::Ok);
  EXPECT_EQ(wal.log_compensate(1, 1), cbtree::Status::Ok);
  EXPECT_EQ(wal.recover(), cbtree::Status::Ok);
}
