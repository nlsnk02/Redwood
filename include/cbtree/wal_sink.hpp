#pragma once
#include "cbtree/types.hpp"
namespace cbtree {
class WalSink {
 public:
  Status log_insert(Key, Value) { return Status::Ok; }
  Status log_update(Key, Value, Value) { return Status::Ok; }
  Status log_compensate(Key, Value) { return Status::Ok; }
  Status checkpoint() { return Status::Ok; }
  Status recover() { return Status::Ok; }
};
}  // namespace cbtree
