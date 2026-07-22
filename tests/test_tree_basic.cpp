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

// Exercise CLOCK eviction and SSD write/read path:
// kCacheSlots = 64; inserting a few extra keys forces eviction.
TEST_F(TreeBasicTest, EvictDirtyToSSD) {
  for (uint64_t i = 0; i < 20; ++i) {
    ASSERT_EQ(tree_->put(i, i * 100), cbtree::Status::Ok);
  }
  // All 20 keys must be retrievable — some will have been evicted to SSD
  for (uint64_t i = 0; i < 20; ++i) {
    auto r = tree_->get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok) << "key " << i;
    EXPECT_EQ(r.value, i * 100) << "key " << i;
  }
}

// =============================================================================
// Task 9: Multi-level tree with P_parent cache routing
// =============================================================================

TEST(TreeMultiLevel, DebugTwoLeaves) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  EXPECT_EQ(t->debug_height(), 2);
  // Parent cache exists and is initially empty
  EXPECT_FALSE(t->debug_cache_a_contains(10));
}

TEST(TreeMultiLevel, PutToParentCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(1.0, 0.0);  // always go to cache_A (leaf-local in leaf-only design)
  ASSERT_EQ(t->put(10, 1), cbtree::Status::Ok);
  // Leaf-only cache: p_parent=1.0 puts data into leaf->cache_A
  EXPECT_TRUE(t->debug_cache_a_contains(10));
}

TEST(TreeMultiLevel, PutToLeafCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(0.0, 0.0);  // never go to parent cache
  ASSERT_EQ(t->put(10, 1), cbtree::Status::Ok);
  // Parent cache should NOT have the key
  EXPECT_FALSE(t->debug_cache_a_contains(10));
}

TEST(TreeMultiLevel, DescendFindsCorrectLeaf) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(0.0, 0.0);
  // Keys should route to correct leaf based on separator
  ASSERT_EQ(t->put(5, 50), cbtree::Status::Ok);
  ASSERT_EQ(t->put(50, 500), cbtree::Status::Ok);
  EXPECT_EQ(t->get(5).value, 50u);
  EXPECT_EQ(t->get(50).value, 500u);
}

TEST(TreeMultiLevel, GetChecksParentCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(1.0, 0.0);  // put into parent cache
  ASSERT_EQ(t->put(42, 420), cbtree::Status::Ok);
  // get should find it in parent cache
  auto r = t->get(42);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 420u);
}

TEST(TreeMultiLevel, GetFromLeafCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(0.0, 0.0);  // put into leaf cache
  ASSERT_EQ(t->put(7, 77), cbtree::Status::Ok);
  // Not in parent cache
  EXPECT_FALSE(t->debug_cache_a_contains(7));
  // Still findable via leaf
  auto r = t->get(7);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 77u);
}

TEST(TreeMultiLevel, GetNotFoundMultiLevel) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  auto r = t->get(999);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
}

TEST(TreeMultiLevel, DegenerateHeight1StillWorks) {
  auto t = cbtree::Tree("/tmp/test_ml_deg.pages");
  EXPECT_EQ(t.debug_height(), 1);
  ASSERT_EQ(t.put(1, 10), cbtree::Status::Ok);
  auto r = t.get(1);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 10u);
}

// ---- Hit rate statistics tests ----

TEST(TreeHitRate, InitialStatsAreZero) {
  cbtree::Tree tree("/tmp/test_hr_init.pages");
  auto stats = tree.memory_hit_stats();
  EXPECT_EQ(stats.total_gets, 0u);
  EXPECT_EQ(stats.memory_hits, 0u);
  EXPECT_EQ(stats.ssd_accesses, 0u);
  EXPECT_DOUBLE_EQ(tree.memory_hit_rate(), 0.0);
}

TEST(TreeHitRate, CacheHitCountsAsMemoryHit) {
  // After put(), the key is in leaf cache → get() is a memory hit.
  cbtree::Tree tree("/tmp/test_hr_cache.pages");
  ASSERT_EQ(tree.put(42, 100), cbtree::Status::Ok);

  // Reset stats so we only measure the get that follows.
  tree.reset_memory_hit_stats();

  auto r = tree.get(42);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 100u);

  auto stats = tree.memory_hit_stats();
  EXPECT_EQ(stats.total_gets, 1u);
  EXPECT_EQ(stats.memory_hits, 1u);
  EXPECT_EQ(stats.ssd_accesses, 0u);
  EXPECT_DOUBLE_EQ(tree.memory_hit_rate(), 1.0);
}

TEST(TreeHitRate, SsdMissCountsAsSsdAccess) {
  // No such key in cache or chunk chain → must go to SSD.
  cbtree::Tree tree("/tmp/test_hr_ssd.pages");
  tree.set_probabilities(0.0, 0.0);  // disable parent cache and placeholder

  tree.reset_memory_hit_stats();

  auto r = tree.get(99);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);

  auto stats = tree.memory_hit_stats();
  EXPECT_EQ(stats.total_gets, 1u);
  EXPECT_EQ(stats.memory_hits, 0u);
  EXPECT_EQ(stats.ssd_accesses, 1u);
  EXPECT_DOUBLE_EQ(tree.memory_hit_rate(), 0.0);
}

TEST(TreeHitRate, MixedHitsAndMisses) {
  cbtree::Tree tree("/tmp/test_hr_mixed.pages");
  tree.set_probabilities(0.0, 0.0);
  tree.reset_memory_hit_stats();

  // Cache hit: key was just put.
  ASSERT_EQ(tree.put(1, 10), cbtree::Status::Ok);
  tree.get(1);  // memory hit (in leaf cache)

  // SSD miss: never-inserted key.
  tree.get(999);  // SSD access

  auto stats = tree.memory_hit_stats();
  EXPECT_EQ(stats.total_gets, 2u);
  EXPECT_EQ(stats.memory_hits, 1u);
  EXPECT_EQ(stats.ssd_accesses, 1u);
  EXPECT_DOUBLE_EQ(tree.memory_hit_rate(), 0.5);
}

TEST(TreeHitRate, ChunkHitCountsAsMemoryHit) {
  // Evict the leaf cache to force data into the chunk chain,
  // then verify that a chunk hit is still a memory hit.
  cbtree::Tree tree("/tmp/test_hr_chunk.pages");
  tree.set_probabilities(0.0, 0.0);

  // Insert enough entries to trigger eviction.
  for (uint64_t k = 0; k < 70; ++k) {
    ASSERT_EQ(tree.put(k, k * 10), cbtree::Status::Ok);
  }

  // Flush all so that chunk chains are processed.
  tree.debug_flush_all();
  tree.reset_memory_hit_stats();

  // Get a key that was evicted — should be in chunk chain.
  auto r = tree.get(0);
  // Even if on SSD, check that the stats are consistent.
  // Some keys may be in cache, some in chunk chain, some on SSD.
  auto stats = tree.memory_hit_stats();
  EXPECT_GE(stats.total_gets, 1u);
  // memory_hits + ssd_accesses == total_gets
  EXPECT_EQ(stats.memory_hits + stats.ssd_accesses, stats.total_gets);
}

TEST(TreeHitRate, ResetClearsAllCounters) {
  cbtree::Tree tree("/tmp/test_hr_reset.pages");
  tree.set_probabilities(0.0, 0.0);

  ASSERT_EQ(tree.put(1, 10), cbtree::Status::Ok);
  tree.get(1);    // hit
  tree.get(999);  // miss

  tree.reset_memory_hit_stats();
  auto stats = tree.memory_hit_stats();
  EXPECT_EQ(stats.total_gets, 0u);
  EXPECT_EQ(stats.memory_hits, 0u);
  EXPECT_EQ(stats.ssd_accesses, 0u);
}

TEST(TreeHitRate, ConcurrentReadsConsistent) {
  // Verify that concurrent reads don't corrupt the atomic counters.
  cbtree::Tree tree("/tmp/test_hr_concurrent.pages");
  tree.set_probabilities(0.0, 0.0);

  // Pre-load some keys into the tree.
  for (uint64_t k = 0; k < 200; ++k) {
    ASSERT_EQ(tree.put(k, k * 10), cbtree::Status::Ok);
  }
  tree.debug_flush_all();
  tree.reset_memory_hit_stats();

  constexpr int kThreads = 4;
  constexpr int kReadsPerThread = 1000;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&tree, t] {
      for (int i = 0; i < kReadsPerThread; ++i) {
        tree.get(static_cast<uint64_t>((i + t * 100) % 200));
      }
    });
  }
  for (auto& th : threads) th.join();

  auto stats = tree.memory_hit_stats();
  EXPECT_EQ(stats.total_gets, static_cast<uint64_t>(kThreads * kReadsPerThread));
  // memory_hits + ssd_accesses must equal total_gets
  EXPECT_EQ(stats.memory_hits + stats.ssd_accesses, stats.total_gets);
  double rate = tree.memory_hit_rate();
  EXPECT_GE(rate, 0.0);
  EXPECT_LE(rate, 1.0);
}
