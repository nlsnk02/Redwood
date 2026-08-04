#pragma once
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>
#include "cbtree/types.hpp"
#include "cbtree/cache_attachment.hpp"
#include "cbtree/chunk.hpp"

namespace cbtree {

struct Node {
  std::atomic<uint64_t> version{0};  // even = stable, odd = structural change in progress
  int height{1};                      // leaf = 1
  Node* parent{nullptr};

  // B-link fields: enable lock-free descent with right-link chasing.
  // high_key = upper bound of this node's key range; ~Key{0} (max) for rightmost.
  // During descent, if search_key >= child->high_key, the reader follows
  // next_sibling until it finds the correct node.  This guarantees key
  // reachability even when the parent's separators haven't been updated yet
  // after a concurrent split (Lehman & Yao 1981).
  Key high_key{std::numeric_limits<Key>::max()};
  std::atomic<Node*> next_sibling{nullptr};
  std::atomic<Node*> prev_sibling{nullptr};  // for chunk-lookup after split

  // Leaf fields (valid when height == 1)
  PageId page_id{0};                  // SSD page for this leaf
  mutable std::mutex eviction_mutex;   // serializes batch eviction on this leaf

  // Internal node fields (valid when height >= 2)
  std::vector<Key> separators;        // separator keys
  std::vector<Node*> children;

  // Dual cache on leaf nodes (height == 1); both nullptr on internal nodes.
  // cache_A: hot cache (former parent-cache semantics, authority 0)
  // cache_B: local cache (former leaf-cache semantics, authority 1)
  std::unique_ptr<CacheAttachment> cache_A;
  std::unique_ptr<CacheAttachment> cache_B;

  // Per-leaf chunk chain: lock-free head for evicting threads.
  // Each leaf owns its own chain — no global CAS contention.
  std::atomic<EvictChunk*> chunk_head_{nullptr};
  std::atomic<size_t> chunk_count_{0};
  std::atomic<size_t> dirty_chunk_count_{0};  // subset of chunk_count_ for dirty chunks

  // Reader count: incremented before traversing this leaf's chunk chain,
  // decremented after. flush_leaf waits for this to reach 0 before freeing.
  mutable std::atomic<int> chunk_readers_{0};

  // Serializes flush operations on this leaf.
  std::mutex flush_mutex_;
};

}  // namespace cbtree
