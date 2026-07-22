#pragma once
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>
#include "cbtree/types.hpp"
#include "cbtree/node.hpp"
#include "cbtree/ssd_page_store.hpp"
#include "cbtree/adaptive_policy.hpp"
#include "cbtree/chunk.hpp"

namespace cbtree {

class Tree {
 public:
  // When use_direct is true, the underlying page store uses O_DIRECT I/O,
  // bypassing the OS page cache.  All writes go directly to the storage device.
  explicit Tree(const std::string& ssd_path, bool use_direct = false);
  ~Tree();

  Tree(const Tree&) = delete;
  Tree& operator=(const Tree&) = delete;

  // Move is disabled — Tree contains non-movable members (mutex, atomic).
  // Use DebugTwoLeaves or wrap in unique_ptr if relocation is needed.
  Tree(Tree&&) = delete;
  Tree& operator=(Tree&&) = delete;

  Status put(Key k, Value v);
  LookupResult get(Key k);
  std::vector<std::pair<Key, Value>> scan(Key lo, Key hi);

  // Test hooks (progressively exposed in later tasks)
  void set_probabilities(double p_parent, double p_placeholder);
  int debug_height() const;
  bool debug_parent_cache_contains(Key k) const;
  Status debug_flush_all();
  bool debug_all_leaves_have_cache() const;
  bool debug_root_has_cache() const;

  // Debug: eviction hooks (Task 11)
  void debug_clear_all_caches();
  bool debug_leaf_index_empty() const;
  bool debug_some_keys_in_leaf_cache() const;

  // Debug factory: creates a height=2 tree with two leaf children
  static std::unique_ptr<Tree> DebugTwoLeaves(const std::string& ssd_path);

  // Debug: verify height >= 3 nodes have no cache
  bool debug_height3_nodes_have_no_cache() const;

  // Debug: chunk chain length
  size_t debug_chunk_count() const;

  // Debug: peak chunk count (since last reset)
  size_t debug_peak_chunk_count() const;
  void debug_reset_peak_chunk_count();

  // Debug: chunk chain length samples (one per push/flush sweep, for stats)
  std::vector<size_t> debug_chunk_len_samples() const;
  void debug_reset_chunk_len_samples();

  // ---- Hit rate statistics (YCSB-compatible API) ----

  // Snapshot of current memory hit counters (atomic reads — best-effort).
  MemoryHitStats memory_hit_stats() const;

  // Reset all hit-rate counters to zero.
  void reset_memory_hit_stats();

  // Return memory hit rate as a fraction in [0.0, 1.0].
  // Returns 0.0 when no get() calls have been recorded.
  // This is the primary YCSB-compatible hit-rate query.
  double memory_hit_rate() const;

  // Enable or disable hit-rate tracking at runtime.
  // When disabled (false), get() performs zero additional atomic operations —
  // the tracking branch is perfectly predicted not-taken.  Default is enabled.
  // Call reset_memory_hit_stats() after re-enabling to start a fresh window.
  void set_hit_tracking(bool on);

 private:
  Node* descend_to_leaf(Key k,
                        std::vector<std::pair<Node*, uint64_t>>& versions);
  void split_leaf(Node* leaf);
  void split_internal(Node* node);
  static void collect_leaves(const Node* node, std::vector<const Node*>& leaves);
  static void collect_leaves(Node* node, std::vector<Node*>& leaves);
  static void collect_leaves_in_range(Node* node, Key lo, Key hi,
                                      std::vector<Node*>& leaves);

  // Task 11: eviction helpers
  Node* find_leaf_for_key(Node* parent, Key k);
  void register_in_leaf_index(Node* leaf, Key k);
  Status evict_leaf_if_needed(Node* leaf);
  Status evict_parent_if_needed(Node* parent);
  void flush_and_split_leaf(Node* leaf);
  // Chunk-based eviction: pack dirty slots into a chunk, push to the leaf's
  // own chain, then flush that leaf's chain to SSD inline.
  Status evict_to_chunk(Node* leaf);

  // Hit-rate tracking: record one completed get() result.
  // When enable_hit_tracking_ is false, compiles to a single predictable
  // branch (not-taken) — effectively zero overhead on the hot path.
  void record_get_hit(bool memory) const;

  // Chunk chain helpers — operate on per-leaf chains.
  // lookup_chunks descends to the correct leaf and searches its chain
  // (and prev_sibling, covering recent splits).
  LookupResult lookup_chunks(Key k);
  // collect_chunk_entries_in_range iterates leaves via next_sibling
  // and collects entries from each leaf's chunk chain.
  void collect_chunk_entries_in_range(Key lo, Key hi,
                                      std::map<Key, Value>& out);

  // Flush one leaf's pending chunks to SSD (synchronous).
  // Called from evict_to_chunk after pushing a chunk to the leaf's chain.
  void flush_leaf(Node* leaf);

  Node* root_;
  std::unique_ptr<SsDPageStore> ssd_;
  AdaptivePolicy adaptive_policy_;
  double p_parent_{kDefaultPParent};
  double p_placeholder_{kDefaultPPlaceholder};

  // Serializes tree-structure mutations (split_leaf, split_internal).
  // Readers (descend_to_leaf, find_leaf_for_key) take a shared lock.
  mutable std::shared_mutex tree_mutex_;

  // Hit-rate counters (atomic — updated by concurrent readers).
  mutable std::atomic<uint64_t> total_gets_{0};
  mutable std::atomic<uint64_t> memory_hits_{0};
  mutable std::atomic<uint64_t> ssd_accesses_{0};

  // Master switch: when false, get() skips all counter increments.
  // Read-only on the hot path after initial setup — zero overhead when off.
  bool enable_hit_tracking_{true};
};

}  // namespace cbtree
