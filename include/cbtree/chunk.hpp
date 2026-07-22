#pragma once
#include <atomic>
#include <cstddef>
#include "cbtree/types.hpp"

namespace cbtree {

struct Node;  // forward declaration

// An EvictChunk holds dirty cache entries evicted from a single leaf.
// Once created and pushed to the global chain, chunks are immutable to
// writers — only the background flush thread may read/modify them.
// Readers traverse the chain with O(1) fingerprint pre-filter per chunk.
struct EvictChunk {
  static constexpr size_t kMaxEntries = kCacheSlots;  // 16

  PageId page_id;     // target SSD page
  Node* leaf;         // leaf this chunk was evicted from
  size_t num_entries;

  struct Entry {
    Key key;
    Value value;
    Fingerprint fp;
  };
  Entry entries[kMaxEntries];

  // Lock-free singly-linked list: newest chunk is at head.
  // Written once by the evicting thread, read by get()/scan().
  std::atomic<EvictChunk*> next{nullptr};

  // Set by the background writer after all entries are safely on SSD.
  // Once flushed, readers can skip this chunk (data is on SSD).
  std::atomic<bool> flushed{false};
};

}  // namespace cbtree
