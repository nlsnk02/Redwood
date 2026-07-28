#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include "cbtree/fingerprint.hpp"

namespace cbtree {

// Count-Min Sketch: probabilistic frequency estimator.
//
// Guarantees (for d rows, w columns, N total increments):
//   estimate(x) >= true_frequency(x)           (never underestimates)
//   estimate(x) <= true_frequency(x) + ε·N     with probability ≥ 1 - δ
//   where ε ≈ e/w, δ ≈ e^{-d}
//
// Template parameters:
//   Key      — integral type (uint64_t)
//   NumRows  — number of hash rows (d); more rows → lower δ
//   NumCols  — counters per row (w); more columns → lower ε
//
// Thread safety: all public methods are safe for concurrent access.
// Increment and estimate are lock-free.  decay() is called infrequently
// from a single writer; it reads atomics but writes non-atomically while
// concurrent incrementers may produce transient over-counts (acceptable).

template <typename Key, size_t NumRows = 4, size_t NumCols = 1024>
class CountMinSketch {
  static_assert(NumRows > 0 && NumCols > 0,
                "CountMinSketch requires positive dimensions");

 public:
  CountMinSketch() { clear(); }

  // Increment the frequency count for |key| by 1.
  // O(NumRows) atomic additions — lock-free.
  void increment(Key key) {
    for (size_t r = 0; r < NumRows; ++r) {
      size_t col = hash(key, r) % NumCols;
      table_[r][col].fetch_add(1, std::memory_order_relaxed);
    }
    total_.fetch_add(1, std::memory_order_relaxed);
  }

  // Estimate the frequency of |key|.
  // Returns min(counter[r][hash_r(key)]) across all rows.
  // O(NumRows) atomic loads — lock-free.  Never underestimates.
  uint64_t estimate(Key key) const {
    uint64_t min_val = UINT64_MAX;
    for (size_t r = 0; r < NumRows; ++r) {
      size_t col = hash(key, r) % NumCols;
      uint64_t val = table_[r][col].load(std::memory_order_relaxed);
      if (val < min_val) min_val = val;
    }
    return min_val;
  }

  // Multiply every counter by |factor| (clamped to [0.0, 1.0]).
  // Counter values are truncated to integers after scaling.
  // This is NOT atomic per-counter — call from a single thread,
  // typically the eviction or maintenance path.  Concurrent incrementers
  // may observe intermediate values; this is acceptable for frequency
  // estimation (transient over-count, no under-count).
  void decay(double factor) {
    if (factor <= 0.0) {
      clear();
      return;
    }
    if (factor >= 1.0) return;

    total_.store(0, std::memory_order_relaxed);
    for (size_t r = 0; r < NumRows; ++r) {
      for (size_t c = 0; c < NumCols; ++c) {
        uint64_t old = table_[r][c].load(std::memory_order_relaxed);
        uint64_t scaled = static_cast<uint64_t>(static_cast<double>(old) * factor);
        // Only write back if changed, to reduce cache-coherence traffic.
        if (scaled != old) {
          table_[r][c].store(scaled, std::memory_order_relaxed);
        }
      }
    }
  }

  // Reset all counters to zero.
  void clear() {
    total_.store(0, std::memory_order_relaxed);
    for (size_t r = 0; r < NumRows; ++r) {
      for (size_t c = 0; c < NumCols; ++c) {
        table_[r][c].store(0, std::memory_order_relaxed);
      }
    }
  }

  // Total number of increment() calls (approximate — relaxed atomic).
  uint64_t total_count() const {
    return total_.load(std::memory_order_relaxed);
  }

  // Memory footprint in bytes.
  static constexpr size_t memory_bytes() {
    return sizeof(std::atomic<uint64_t>) * NumRows * NumCols + sizeof(total_);
  }

 private:
  // d independent hash functions derived from splitmix64 with per-row seeds.
  // Row seed = golden-ratio-derived constants to decorrelate rows.
  static uint64_t hash(Key key, size_t row) {
    // Per-row seed constants (first 8 primes above 2^60, splitmix64 style).
    static constexpr uint64_t kSeeds[] = {
        0x9E3779B97F4A7C15ULL,  // golden ratio φ · 2^63
        0xBF58476D1CE4E5B9ULL,
        0xC2B2AE3D27D4EB4FULL,
        0xE17F4A9C3B6D8527ULL,
        0xF3A25173D4B6C80DULL,
        0x8A2D5371E39F6C4BULL,
        0x9D7B4E21F6C3A85DULL,
        0xB1E5F837D62A4C9FULL,
    };
    // splitmix64: mix the key with the row seed.
    uint64_t x = key + kSeeds[row % 8];
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x = x ^ (x >> 31);
    return x;
  }

  // d × w matrix of atomic counters.
  // Using a 2D array-of-arrays: NumRows rows, each with NumCols atomics.
  // Layout: table_[row][col] — row-major, good for increment (all rows
  // accessed for a single key, no cache-line bouncing between rows).
  std::array<std::array<std::atomic<uint64_t>, NumCols>, NumRows> table_;
  std::atomic<uint64_t> total_{0};
};

}  // namespace cbtree
