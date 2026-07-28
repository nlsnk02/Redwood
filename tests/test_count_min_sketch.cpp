#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <algorithm>
#include "cbtree/count_min_sketch.hpp"
#include "cbtree/types.hpp"

using cbtree::CountMinSketch;
using cbtree::Key;

// Small sketch for deterministic testing.
using TestCMS = CountMinSketch<Key, 4, 64>;

TEST(CountMinSketch, InitialEstimateIsZero) {
  TestCMS cms;
  EXPECT_EQ(cms.estimate(42), 0);
  EXPECT_EQ(cms.estimate(0), 0);
  EXPECT_EQ(cms.estimate(UINT64_MAX), 0);
}

TEST(CountMinSketch, SingleIncrement) {
  TestCMS cms;
  cms.increment(42);
  EXPECT_GE(cms.estimate(42), 1);  // CMS never underestimates
  EXPECT_EQ(cms.total_count(), 1);
}

TEST(CountMinSketch, MultipleIncrementsSameKey) {
  TestCMS cms;
  for (int i = 0; i < 100; ++i) {
    cms.increment(42);
  }
  EXPECT_GE(cms.estimate(42), 100);
  EXPECT_EQ(cms.total_count(), 100);
}

TEST(CountMinSketch, DifferentKeys) {
  TestCMS cms;
  for (int i = 0; i < 50; ++i) cms.increment(1);
  for (int i = 0; i < 10; ++i) cms.increment(2);
  for (int i = 0; i < 5;  ++i) cms.increment(3);

  // Estimate must be >= true frequency for each key.
  EXPECT_GE(cms.estimate(1), 50);
  EXPECT_GE(cms.estimate(2), 10);
  EXPECT_GE(cms.estimate(3), 5);
  // Unseen key should still be 0.
  EXPECT_EQ(cms.estimate(999), 0);
  EXPECT_EQ(cms.total_count(), 65);
}

TEST(CountMinSketch, RelativeOrderingPreserved) {
  // More increments → higher (or equal) estimate.
  TestCMS cms;
  for (int i = 0; i < 200; ++i) cms.increment(100);  // hot
  for (int i = 0; i < 30;  ++i) cms.increment(200);  // warm
  for (int i = 0; i < 3;   ++i) cms.increment(300);  // cold

  uint64_t hot  = cms.estimate(100);
  uint64_t warm = cms.estimate(200);
  uint64_t cold = cms.estimate(300);

  // Should preserve rough ordering; allow for hash-collision noise.
  EXPECT_GE(hot, warm);
  EXPECT_GE(warm, cold);
}

TEST(CountMinSketch, Clear) {
  TestCMS cms;
  cms.increment(42);
  cms.increment(42);
  EXPECT_GE(cms.estimate(42), 2);

  cms.clear();
  EXPECT_EQ(cms.estimate(42), 0);
  EXPECT_EQ(cms.total_count(), 0);
}

TEST(CountMinSketch, DecayHalves) {
  TestCMS cms;
  for (int i = 0; i < 100; ++i) cms.increment(1);
  uint64_t before = cms.estimate(1);
  EXPECT_GE(before, 100);

  cms.decay(0.5);
  uint64_t after = cms.estimate(1);

  // After decay, estimate should be roughly halved.
  // CMS may overestimate after decay due to hash collisions being scaled
  // independently, but the estimate should be <= before and > 0.
  EXPECT_LE(after, before);
  EXPECT_GT(after, 0);
}

TEST(CountMinSketch, DecayZeroClears) {
  TestCMS cms;
  cms.increment(42);
  cms.decay(0.0);
  EXPECT_EQ(cms.estimate(42), 0);
  EXPECT_EQ(cms.total_count(), 0);
}

TEST(CountMinSketch, DecayOneIsNoop) {
  TestCMS cms;
  for (int i = 0; i < 100; ++i) cms.increment(1);
  uint64_t before = cms.estimate(1);
  cms.decay(1.0);
  EXPECT_EQ(cms.estimate(1), before);
}

TEST(CountMinSketch, MemoryFootprint) {
  // d=4, w=64 → 4*64*8 + 8 = 2056 bytes.
  EXPECT_EQ(TestCMS::memory_bytes(),
            4 * 64 * sizeof(std::atomic<uint64_t>) + sizeof(std::atomic<uint64_t>));
}

TEST(CountMinSketch, ThreadSafetyIncrement) {
  // Concurrent increments should not crash or lose counts.
  TestCMS cms;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 10000;
  constexpr Key kKey = 12345;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cms]() {
      for (int i = 0; i < kPerThread; ++i) {
        cms.increment(kKey);
      }
    });
  }
  for (auto& t : threads) t.join();

  uint64_t est = cms.estimate(kKey);
  uint64_t expected = kThreads * kPerThread;
  EXPECT_GE(est, expected);
  EXPECT_EQ(cms.total_count(), expected);
}

TEST(CountMinSketch, ThreadSafetyMixedKeys) {
  TestCMS cms;
  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cms, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        cms.increment(t * 1000 + (i % 100));  // 100 unique keys per thread
      }
    });
  }
  for (auto& t : threads) t.join();

  EXPECT_EQ(cms.total_count(), kThreads * kPerThread);
}

TEST(CountMinSketch, DecayThreadSafety) {
  // decay() + concurrent increment() should not crash.
  TestCMS cms;
  constexpr int kIncrements = 100000;

  std::thread writer([&cms]() {
    for (int i = 0; i < kIncrements; ++i) {
      cms.increment(i % 100);
    }
  });

  std::thread decayer([&cms]() {
    for (int i = 0; i < 100; ++i) {
      cms.decay(0.99);
    }
  });

  writer.join();
  decayer.join();

  // Sanity: total shouldn't exceed increments.
  EXPECT_LE(cms.total_count(), kIncrements);
}
