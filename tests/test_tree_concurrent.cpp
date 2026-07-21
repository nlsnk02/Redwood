#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <atomic>
#include "cbtree/tree.hpp"

class TreeConcurrentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_concurrent.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

// kCacheSlots=16, kMaxRecordsPerPage=255 => max 271 keys.
// Use 8x30=240 to stay within single-page capacity.
TEST_F(TreeConcurrentTest, ParallelPutGet) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 30;
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        uint64_t k = static_cast<uint64_t>(t * kPerThread + i);
        auto s = tree_->put(k, k);
        if (s != cbtree::Status::Ok) { ++errors; return; }
        auto r = tree_->get(k);
        if (r.status != cbtree::Status::Ok || r.value != k) { ++errors; return; }
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
}

TEST_F(TreeConcurrentTest, ParallelPutSameKeys) {
  constexpr int kThreads = 4;
  constexpr int kRounds = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kRounds; ++i) {
        ASSERT_EQ(tree_->put(42, static_cast<uint64_t>(t * 1000 + i)),
                  cbtree::Status::Ok);
      }
    });
  }
  for (auto& th : threads) th.join();
  auto r = tree_->get(42);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
}

TEST_F(TreeConcurrentTest, MixedReadWrite) {
  // Pre-fill data and flush to SSD
  for (uint64_t i = 0; i < 100; ++i)
    tree_->put(i, i);
  tree_->debug_flush_all();

  constexpr int kThreads = 6;
  std::vector<std::thread> threads;
  std::atomic<int> writers_done{0};
  // 3 writer threads: write disjoint ranges
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&, t] {
      for (uint64_t i = 100 + t * 30; i < 130 + t * 30; ++i)
        tree_->put(i, i);
      ++writers_done;
    });
  }
  // 3 reader threads: repeatedly read existing keys
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 500; ++i)
        tree_->get(static_cast<uint64_t>(i % 50));
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(writers_done.load(), 3);
}
