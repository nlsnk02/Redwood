#pragma once
#include "cbtree/types.hpp"
namespace cbtree {
struct Node;
struct DeleteOps {
  static Status remove(Key) { return Status::NotImplemented; }
  static Status try_merge(Node*) { return Status::NotImplemented; }
  static Status rebalance(Node*) { return Status::NotImplemented; }
};
}  // namespace cbtree
