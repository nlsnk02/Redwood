#include <thread>
#include <atomic>
#include <gtest/gtest.h>
#include "cbtree/key_lock_table.hpp"

TEST(KeyLockTable, SameKeySerializes) {
  cbtree::KeyLockTable table;
  std::atomic<int> in_critical{0};
  std::atomic<int> max_in{0};
  auto worker = [&] {
    for (int i = 0; i < 1000; ++i) {
      cbtree::KeyLockGuard g(table, 42);
      int v = ++in_critical;
      int m = max_in.load();
      while (v > m && !max_in.compare_exchange_weak(m, v)) {}
      --in_critical;
    }
  };
  std::thread t1(worker), t2(worker);
  t1.join(); t2.join();
  EXPECT_EQ(max_in.load(), 1);
}

TEST(KeyLockTable, DifferentKeysNoConflict) {
  cbtree::KeyLockTable table;
  std::atomic<int> count{0};
  auto worker = [&](cbtree::Key k) {
    cbtree::KeyLockGuard g(table, k);
    ++count;
  };
  std::thread t1(worker, 1), t2(worker, 2);
  t1.join(); t2.join();
  EXPECT_EQ(count.load(), 2);
}
