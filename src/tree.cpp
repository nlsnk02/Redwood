// src/tree.cpp
#include "cbtree/tree.hpp"
#include "cbtree/cache_attachment.hpp"
#include "cbtree/fingerprint.hpp"
#include <algorithm>
#include <chrono>
#include <map>
#include <random>
#include <set>
#include <thread>

namespace cbtree {

Tree::Tree(const std::string& ssd_path, bool use_direct)
    : ssd_(std::make_unique<SsDPageStore>(ssd_path, use_direct)) {
  root_ = new Node{};
  root_->height = 1;
  root_->cache_A = std::make_unique<CacheAttachment>();
  root_->cache_B = std::make_unique<CacheAttachment>();
  root_->page_id = ssd_->alloc_page();
}

static void delete_subtree(Node* node) {
  if (!node) return;
  for (Node* child : node->children) {
    delete_subtree(child);
  }
  delete node;
}

Tree::~Tree() {
  // Free per-leaf chunk chains (should already be empty if properly flushed)
  {
    std::vector<Node*> leaves;
    collect_leaves(root_, leaves);
    for (Node* leaf : leaves) {
      EvictChunk* head = leaf->chunk_head_.load(std::memory_order_acquire);
      while (head) {
        EvictChunk* next = head->next.load(std::memory_order_acquire);
        delete head;
        head = next;
      }
    }
  }

  delete_subtree(root_);
  root_ = nullptr;
}

Node* Tree::descend_to_leaf(Key k,
                            std::vector<std::pair<Node*, uint64_t>>& versions) {
  // B-link: lock-free descent.  Pre-allocated internal-node vectors
  // (kInternalFanout+2) prevent reallocation during concurrent insert,
  // so readers never touch freed memory.  Readers may see transiently
  // stale/duplicate child pointers during insert shifts — the right-link
  // chase corrects for that.
  static constexpr Key kInf = std::numeric_limits<Key>::max();
  Node* cur = root_;
  while (cur->height > 1) {
    versions.emplace_back(cur, cur->version.load(std::memory_order_acquire));
    auto it = std::upper_bound(cur->separators.begin(), cur->separators.end(), k);
    size_t idx = static_cast<size_t>(it - cur->separators.begin());
    Node* child = cur->children[idx];

    // Chase right-links: if k is beyond this child's high_key, the child
    // was split and the parent hasn't been updated yet.  The sibling link
    // was set BEFORE the split committed, so following it is always safe.
    while (child->high_key != kInf && k >= child->high_key) {
      Node* next = child->next_sibling.load(std::memory_order_acquire);
      if (!next) break;  // safety — shouldn't happen under correct protocol
      child = next;
    }

    cur = child;
  }
  versions.emplace_back(cur, cur->version.load(std::memory_order_acquire));
  return cur;  // leaf node
}

Node* Tree::find_leaf_for_key(Node* parent, Key k) {
  // B-link: lock-free descent (same pre-allocation guarantee as above).
  static constexpr Key kInf = std::numeric_limits<Key>::max();
  Node* cur = parent;
  while (cur->height > 1) {
    auto it = std::upper_bound(cur->separators.begin(), cur->separators.end(), k);
    size_t idx = static_cast<size_t>(it - cur->separators.begin());
    Node* child = cur->children[idx];

    while (child->high_key != kInf && k >= child->high_key) {
      Node* next = child->next_sibling.load(std::memory_order_acquire);
      if (!next) break;
      child = next;
    }

    cur = child;
  }
  return cur;
}

void Tree::register_in_leaf_index(Node* leaf, Key k) {
  std::lock_guard<std::mutex> lock(leaf->leaf_index_mutex);
  auto it = std::lower_bound(leaf->leaf_keys.begin(), leaf->leaf_keys.end(), k);
  if (it != leaf->leaf_keys.end() && *it == k) return;  // already exists
  size_t idx = it - leaf->leaf_keys.begin();
  leaf->leaf_keys.insert(it, k);
  leaf->leaf_page_ids.insert(leaf->leaf_page_ids.begin() + idx, leaf->page_id);
}

Status Tree::evict_leaf_if_needed(Node* leaf) {
  if (leaf->cache_B->occupied_count() <=
      static_cast<int>(kCacheSlots * kLeafFillThreshold))
    return Status::Ok;
  Status s = evict_to_chunk(leaf);
  return (s == Status::Retry) ? Status::Ok : s;
}

Status Tree::evict_to_chunk(Node* leaf) {
  // Serialize evictions on this leaf. If another thread is already
  // evicting, skip — it will free enough slots for us.
  std::unique_lock<std::mutex> evict_lock(leaf->eviction_mutex, std::try_to_lock);
  if (!evict_lock.owns_lock()) return Status::Retry;

  // 1. Collect all dirty entries and mark them clean (keeps slots Occupied).
  std::vector<std::pair<Key, Value>> dirty;
  leaf->cache_B->flush_dirty(dirty);
  if (dirty.empty()) {
    leaf->cache_B->clear_clean_occupied();
    return Status::Ok;
  }

  // 2. Create chunk with collected entries.
  auto* chunk = new EvictChunk{};
  chunk->page_id = leaf->page_id;
  chunk->leaf = leaf;
  chunk->num_entries = dirty.size();
  for (size_t i = 0; i < dirty.size(); ++i) {
    chunk->entries[i].key = dirty[i].first;
    chunk->entries[i].value = dirty[i].second;
    chunk->entries[i].fp = fingerprint(dirty[i].first);
  }

  // 3. Push chunk to this leaf's lock-free chain (newest at head).
  //    Per-leaf chains eliminate global CAS contention between leaves.
  //    The chunk is NOW visible to readers (get/scan) — this is the safety
  //    net that covers the window between cache eviction and SSD write.
  {
    EvictChunk* old_head = leaf->chunk_head_.load(std::memory_order_acquire);
    do {
      chunk->next.store(old_head, std::memory_order_release);
    } while (!leaf->chunk_head_.compare_exchange_weak(old_head, chunk,
                                                       std::memory_order_acq_rel));
  }
  leaf->chunk_count_.fetch_add(1, std::memory_order_relaxed);

  // 4. Clear cache slots for entries now in the chunk.
  //    Only clears slots that are still clean (not re-dirtied by another
  //    thread). Re-dirtied slots stay Occupied for the next eviction.
  for (const auto& [k, v] : dirty) {
    leaf->cache_B->evict_clean_slot(k);
  }

  // 5. RELEASE eviction_mutex BEFORE flushing to SSD.
  //    The critical section (step 1-4) is ~microseconds.
  //    SSD I/O (~37ms fsync in sync mode) happens entirely outside the lock.
  //    Readers that miss in cache will find the data in the chunk chain.
  evict_lock.unlock();

  // 6. Flush THIS leaf's chunks to SSD. Uses leaf->flush_mutex_ (try_lock)
  //    so only one thread flushes a given leaf at a time. Different leaves
  //    flush independently.
  flush_leaf(leaf);

  return Status::Ok;
}

Status Tree::evict_cache_A_if_needed(Node* leaf) {
  if (leaf->cache_A->occupied_count() <=
      static_cast<int>(kCacheSlots * kParentFillThreshold))
    return Status::Ok;
  Key victim_key = 0;
  Value victim_val = 0;
  bool victim_dirty = false;
  int victim_idx = -1;
  uint32_t victim_gen = 0;
  if (leaf->cache_A->find_clock_victim(&victim_key, &victim_val, &victim_dirty,
                                       &victim_idx, &victim_gen) != Status::Ok)
    return Status::Ok;

  if (victim_dirty) {
    // Demote to cache_B on same leaf — no re-descent needed.
    if (leaf->cache_B) {
      evict_leaf_if_needed(leaf);  // ensure cache_B has room
      leaf->cache_B->upsert(victim_key, victim_val);
    }
  }
  leaf->cache_A->evict_slot(victim_idx, victim_key, victim_gen);
  return Status::Ok;
}

void Tree::flush_and_split_leaf(Node* leaf) {
  std::lock_guard<std::mutex> lock(leaf->leaf_index_mutex);

  std::sort(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
  auto last = std::unique(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
  leaf->leaf_keys.erase(last, leaf->leaf_keys.end());
  leaf->leaf_page_ids.assign(leaf->leaf_keys.size(), leaf->page_id);

  if (leaf->leaf_keys.size() > kLeafFanout) {
    split_leaf(leaf);
  }
}

// ---- Chunk chain lookup ----

LookupResult Tree::lookup_chunks(Key k) {
  Fingerprint fp = fingerprint(k);

  // Find the leaf that owns key K.
  Node* leaf = find_leaf_for_key(root_, k);

  // Search this leaf's chain and walk prev_sibling to catch chunks
  // created before recent splits.  When a leaf L splits into L and R,
  // chunks for keys >= mid that were created on L BEFORE the split
  // remain on L's chain.  B-link descent finds R (correct leaf for K),
  // so we must also check R's prev_sibling (L).
  Node* cur = leaf;
  while (cur) {
    cur->chunk_readers_.fetch_add(1, std::memory_order_acquire);

    EvictChunk* head = cur->chunk_head_.load(std::memory_order_acquire);
    while (head) {
      for (size_t i = 0; i < head->num_entries; ++i) {
        if (head->entries[i].fp != fp) continue;
        if (head->entries[i].key == k) {
          cur->chunk_readers_.fetch_sub(1, std::memory_order_release);
          return {Status::Ok, head->entries[i].value};
        }
      }
      head = head->next.load(std::memory_order_acquire);
    }

    cur->chunk_readers_.fetch_sub(1, std::memory_order_release);

    // Walk back one step — chunks may be on the leaf that existed
    // before the most recent split.
    cur = cur->prev_sibling.load(std::memory_order_acquire);
    if (cur == leaf) break;  // safety: prevent cycles
  }

  return {Status::NotFound};
}

void Tree::collect_chunk_entries_in_range(Key lo, Key hi,
                                          std::map<Key, Value>& out) {
  static constexpr Key kInf = std::numeric_limits<Key>::max();

  // Collect leaves overlapping [lo, hi] via B-link sibling traversal.
  std::vector<Node*> leaves;
  {
    std::vector<std::pair<Node*, uint64_t>> dummy;
    Node* leaf = descend_to_leaf(lo, dummy);
    while (leaf && leaf->high_key != kInf && lo >= leaf->high_key) {
      leaf = leaf->next_sibling.load(std::memory_order_acquire);
    }
    while (leaf) {
      leaves.push_back(leaf);
      if (hi < leaf->high_key) break;
      leaf = leaf->next_sibling.load(std::memory_order_acquire);
    }
  }

  // Search each leaf's chunk chain, newest first (first occurrence wins).
  for (Node* leaf : leaves) {
    leaf->chunk_readers_.fetch_add(1, std::memory_order_acquire);

    EvictChunk* head = leaf->chunk_head_.load(std::memory_order_acquire);
    while (head) {
      for (size_t i = 0; i < head->num_entries; ++i) {
        Key k = head->entries[i].key;
        if (k < lo || k > hi) continue;
        if (out.find(k) == out.end()) {
          out[k] = head->entries[i].value;
        }
      }
      head = head->next.load(std::memory_order_acquire);
    }

    leaf->chunk_readers_.fetch_sub(1, std::memory_order_release);
  }
}

// ---- Flush all chunks to SSD ----

void Tree::flush_leaf(Node* leaf) {
  // Serialize flush operations on this leaf — only one thread flushes
  // a given leaf at a time.  Different leaves flush independently.
  std::unique_lock<std::mutex> flush_lock(leaf->flush_mutex_, std::try_to_lock);
  if (!flush_lock.owns_lock()) return;

  // ---- Phase 1: collect unflushed chunks oldest-first ----
  // Flushing oldest-first ensures that if the same key appears in
  // multiple chunks, the newest value is written last and survives.
  std::vector<EvictChunk*> to_flush;
  {
    EvictChunk* h = leaf->chunk_head_.load(std::memory_order_acquire);
    while (h) {
      if (!h->flushed.load(std::memory_order_acquire)) {
        to_flush.push_back(h);
      }
      h = h->next.load(std::memory_order_acquire);
    }
    // Reverse: chain is newest-first, we want oldest-first for flush.
    std::reverse(to_flush.begin(), to_flush.end());
  }

  if (to_flush.empty()) return;

  // ---- Phase 2: batch-write to SSD ----
  // Walk each chunk's entries.  Entries with key < leaf->high_key still
  // belong to this leaf's page — batch them into a single read+write.
  // Entries with key >= high_key were moved to a sibling by a split;
  // route those to the correct page via find_leaf_for_key (rare path).
  // This eliminates O(N) tree descents: almost all entries take the
  // cheap range check.
  {
    std::vector<std::pair<Key, Value>> local_entries;
    std::map<PageId, std::vector<std::pair<Key, Value>>> remote;
    std::map<PageId, Node*> page_leaf;  // for registration

    Key hk = leaf->high_key;
    for (EvictChunk* c : to_flush) {
      for (size_t i = 0; i < c->num_entries; ++i) {
        Key key = c->entries[i].key;
        Value val = c->entries[i].value;
        if (hk == std::numeric_limits<Key>::max() || key < hk) {
          // Fast path: still in this leaf's range.
          local_entries.emplace_back(key, val);
        } else {
          // Slow path: may have been split to a sibling.
          Node* target = find_leaf_for_key(root_, key);
          remote[target->page_id].emplace_back(key, val);
          page_leaf[target->page_id] = target;
        }
      }
    }
    // Only one page for local entries.
    if (!local_entries.empty()) {
      page_leaf[leaf->page_id] = leaf;
    }

    // Helper: flush one batch of entries to a page.
    auto flush_batch = [&](PageId pid,
                           std::vector<std::pair<Key, Value>>& entries) {
      std::vector<std::pair<Key, Value>> overflow;
      Status s = ssd_->write_page_entries(pid, entries, overflow);
      if (s != Status::Ok) {
        Node* n = page_leaf[pid];
        for (const auto& [key, val] : entries) n->cache_B->upsert(key, val);
        return;
      }

      // Register successfully-written entries.
      Node* n = page_leaf[pid];
      if (overflow.empty()) {
        // Fast path: no overflow — register all entries directly.
        for (const auto& [key, val] : entries) {
          register_in_leaf_index(n, key);
        }
      } else {
        // Slow path: filter overflow entries from registration.
        std::set<Key> overflow_keys;
        for (const auto& [k, v] : overflow) overflow_keys.insert(k);
        for (const auto& [key, val] : entries) {
          if (overflow_keys.count(key)) continue;
          register_in_leaf_index(n, key);
        }
      }

      for (const auto& [key, val] : overflow) {
        Node* target = find_leaf_for_key(root_, key);
        flush_and_split_leaf(target);
        Node* correct = find_leaf_for_key(
            target->parent ? target->parent : root_, key);
        correct->cache_B->upsert(key, val);
      }
    };

    // Write local entries: one read + one write for this leaf's page.
    if (!local_entries.empty()) {
      flush_batch(leaf->page_id, local_entries);
    }
    // Write remote entries: one read + one write per sibling page.
    for (auto& [pid, entries] : remote) {
      flush_batch(pid, entries);
    }
  }

  // ---- Phase 3: data is written to SSD (O_DIRECT bypasses page cache) ----

  // ---- Phase 4: mark chunks as flushed ----
  for (EvictChunk* c : to_flush) {
    c->flushed.store(true, std::memory_order_release);
  }

  // ---- Phase 5: sweep flushed chunks from this leaf's chain ----
  std::vector<EvictChunk*> freed;
  for (;;) {
    EvictChunk* old_head = leaf->chunk_head_.load(std::memory_order_acquire);

    EvictChunk* new_head = nullptr;
    EvictChunk* new_tail = nullptr;
    EvictChunk* h = old_head;
    while (h) {
      EvictChunk* next = h->next.load(std::memory_order_acquire);
      if (!h->flushed.load(std::memory_order_acquire)) {
        h->next.store(nullptr, std::memory_order_release);
        if (!new_head) {
          new_head = h;
        } else {
          new_tail->next.store(h, std::memory_order_release);
        }
        new_tail = h;
      } else {
        freed.push_back(h);
      }
      h = next;
    }

    if (leaf->chunk_head_.compare_exchange_weak(old_head, new_head,
                                                 std::memory_order_acq_rel)) {
      break;
    }
    freed.clear();
  }

  // ---- Phase 6: wait for in-flight readers, then free ----
  while (leaf->chunk_readers_.load(std::memory_order_acquire) > 0) {
    std::this_thread::yield();
  }

  for (EvictChunk* c : freed) {
    delete c;
  }
  leaf->chunk_count_.fetch_sub(freed.size(), std::memory_order_relaxed);
}

// ---- Core operations ----

Status Tree::put(Key k, Value v) {
  thread_local std::mt19937_64 rng(std::random_device{}());

  // Always descend to leaf first — no parent cache shortcut.
  std::vector<std::pair<Node*, uint64_t>> versions;
  Node* leaf = descend_to_leaf(k, versions);

  constexpr int kMaxUpsertRetries = 256;
  for (int retry = 0; retry < kMaxUpsertRetries; ++retry) {
    versions.clear();
    leaf = descend_to_leaf(k, versions);
    uint64_t leaf_v = versions.back().second;

    if (leaf_v & 1) {
      std::this_thread::yield();
      continue;
    }

    // Phase 1: try cache_A (hot cache, former parent semantics)
    {
      LookupResult lr = leaf->cache_A->lookup(k);
      bool exists_in_A =
          (lr.status == Status::Ok) || leaf->cache_A->has_absent(k);

      if (exists_in_A) {
        Status s = leaf->cache_A->upsert(k, v);
        if (s == Status::Ok) {
          if (leaf->version.load(std::memory_order_acquire) != leaf_v) continue;
          evict_cache_A_if_needed(leaf);
          return Status::Ok;
        }
      } else {
        bool use_A = false;
        if (p_parent_ >= 1.0) {
          use_A = true;
        } else if (p_parent_ > 0.0) {
          use_A = std::bernoulli_distribution{p_parent_}(rng);
        }

        if (use_A) {
          Status s = leaf->cache_A->upsert(k, v);
          if (s == Status::Ok) {
            if (leaf->version.load(std::memory_order_acquire) != leaf_v) continue;
            evict_cache_A_if_needed(leaf);
            return Status::Ok;
          }
        }
      }
    }

    // Phase 2: upsert into cache_B (local cache)
    Status s = leaf->cache_B->upsert(k, v);
    if (s == Status::Ok) {
      if (leaf->version.load(std::memory_order_acquire) != leaf_v) continue;
      evict_leaf_if_needed(leaf);
      return Status::Ok;
    }

    if (s != Status::Full) return s;

    // cache_B full — evict to make room
    Status evict_s = evict_to_chunk(leaf);
    if (evict_s == Status::Retry) {
      std::this_thread::yield();
      continue;
    }
    if (evict_s != Status::Ok) return Status::Full;

    leaf = descend_to_leaf(k, versions);
  }
  return Status::Full;
}

void Tree::collect_leaves_in_range(Node* node, Key lo, Key hi,
                                    std::vector<Node*>& leaves) {
  if (!node) return;
  if (node->height == 1) {
    leaves.push_back(node);
    return;
  }

  size_t lo_idx =
      std::upper_bound(node->separators.begin(), node->separators.end(), lo) -
      node->separators.begin();
  size_t hi_idx =
      std::upper_bound(node->separators.begin(), node->separators.end(), hi) -
      node->separators.begin();

  for (size_t i = lo_idx; i <= hi_idx && i < node->children.size(); ++i) {
    collect_leaves_in_range(node->children[i], lo, hi, leaves);
  }
}

std::vector<std::pair<Key, Value>> Tree::scan(Key lo, Key hi) {
  // 1. Find leaves overlapping [lo, hi] via B-link sibling traversal.
  //    descend_to_leaf returns the leaf containing lo (or the leftmost leaf
  //    if lo is before all keys).  We then walk right via next_sibling until
  //    we pass hi.  No shared_lock needed — concurrent splits are handled
  //    by the B-link right-link protocol.
  static constexpr Key kInf = std::numeric_limits<Key>::max();
  std::vector<Node*> leaves;
  {
    std::vector<std::pair<Node*, uint64_t>> dummy;
    Node* leaf = descend_to_leaf(lo, dummy);

    // If lo is beyond this leaf's range, chase right-links to the correct leaf.
    while (leaf && leaf->high_key != kInf && lo >= leaf->high_key) {
      leaf = leaf->next_sibling.load(std::memory_order_acquire);
    }

    while (leaf) {
      leaves.push_back(leaf);
      // If hi is strictly before the next leaf's start, we're done.
      if (hi < leaf->high_key) break;
      // Otherwise, there may be keys <= hi in the next leaf.
      leaf = leaf->next_sibling.load(std::memory_order_acquire);
    }
  }

  if (leaves.empty()) return {};

  // 2. Build result map: key -> (value, authority)
  //    authority: 0 = parent cache, 1 = leaf cache, 2 = chunks, 3 = SSD
  struct AuthEntry {
    Value value;
    int authority;
  };
  std::map<Key, AuthEntry> merged;

  std::set<CacheAttachment*> processed_parents;

  for (Node* leaf : leaves) {
    // ---- Parent cache (authority 0) ----
    if (leaf->parent && leaf->parent->height == 2 && leaf->parent->cache_A) {
      CacheAttachment* pc = leaf->parent->cache_A.get();
      if (processed_parents.insert(pc).second) {
        if (!pc->sorted_flag()) pc->sort_and_set_flag();
        auto occ = pc->occupied_sorted();
        for (const auto& [k, v] : occ) {
          if (k >= lo && k <= hi) {
            merged[k] = {v, 0};
          }
        }
        auto abs = pc->absent_keys();
        for (Key k : abs) {
          if (k >= lo && k <= hi) {
            merged[k] = {0, -1};
          }
        }
      }
    }

    // ---- Leaf cache (authority 1) ----
    CacheAttachment* lc = leaf->cache_B.get();
    if (!lc->sorted_flag()) lc->sort_and_set_flag();
    auto occ = lc->occupied_sorted();
    for (const auto& [k, v] : occ) {
      if (k >= lo && k <= hi) {
        auto it = merged.find(k);
        if (it == merged.end()) {
          merged[k] = {v, 1};
        }
      }
    }
    auto abs = lc->absent_keys();
    for (Key k : abs) {
      if (k >= lo && k <= hi) {
        auto it = merged.find(k);
        if (it == merged.end()) {
          merged[k] = {0, -1};
        }
      }
    }
  }

  // ---- Chunks (authority 2) ----
  {
    std::map<Key, Value> chunk_entries;
    collect_chunk_entries_in_range(lo, hi, chunk_entries);
    for (const auto& [k, v] : chunk_entries) {
      auto it = merged.find(k);
      if (it == merged.end()) {
        merged[k] = {v, 2};
      }
    }
  }

  // ---- SSD (authority 3, lowest) ----
  for (Node* leaf : leaves) {
    std::vector<std::pair<Key, Value>> page_data;
    ssd_->dump_sorted(leaf->page_id, &page_data);
    for (const auto& [k, v] : page_data) {
      if (k >= lo && k <= hi) {
        auto it = merged.find(k);
        if (it == merged.end()) {
          merged[k] = {v, 3};
        }
      }
    }
  }

  // 3. Build result: iterate sorted map, skip absent entries
  std::vector<std::pair<Key, Value>> result;
  for (const auto& [k, entry] : merged) {
    if (entry.authority >= 0) {
      result.emplace_back(k, entry.value);
    }
  }

  return result;
}

LookupResult Tree::get(Key k) {
  constexpr int kMaxRetries = 64;

  thread_local std::mt19937_64 rng(std::random_device{}());

  Node* root = root_;

  for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
    root = root_;
    uint64_t v = root->version.load(std::memory_order_acquire);
    if (v & 1) {
      continue;
    }

    std::vector<std::pair<Node*, uint64_t>> versions;
    Node* leaf = descend_to_leaf(k, versions);
    uint64_t leaf_v = versions.back().second;

    // Check cache_A first (hot cache, authority 0)
    {
      LookupResult pr = leaf->cache_A->lookup(k);
      if (pr.status == Status::Ok) {
        record_get_hit(true);
        return pr;
      }
      if (leaf->cache_A->has_absent(k)) {
        record_get_hit(true);
        return {Status::NotFound};
      }
    }

    // Check cache_B (local cache, authority 1)
    {
      LookupResult r = leaf->cache_B->lookup(k);
      if (r.status == Status::Ok) {
        record_get_hit(true);
        return r;
      }
      if (leaf->cache_B->has_absent(k)) {
        record_get_hit(true);
        return {Status::NotFound};
      }
    }

    // Check chunk chains
    LookupResult cr = lookup_chunks(k);
    if (cr.status == Status::Ok) {
      record_get_hit(true);
      return cr;
    }

    // Placeholder placement (in cache_B only)
    bool has_placed = false;
    int placeholder_idx = -1;
    if (p_placeholder_ >= 1.0) {
      Status ps = leaf->cache_B->try_place_placeholder(k, &placeholder_idx);
      has_placed = (ps == Status::Ok);
    } else if (p_placeholder_ > 0.0) {
      if (std::bernoulli_distribution{p_placeholder_}(rng)) {
        Status ps = leaf->cache_B->try_place_placeholder(k, &placeholder_idx);
        has_placed = (ps == Status::Ok);
      }
    }

    // Query SSD
    LookupResult r = ssd_->get_record(leaf->page_id, k);

    // Post-SSD cache recheck (cache_B only)
    LookupResult r2 = leaf->cache_B->lookup(k);
    if (r2.status == Status::Ok) {
      if (has_placed) {
        leaf->cache_B->fill_placeholder(placeholder_idx, r2.value);
      }
      if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
        record_get_hit(false);
        return r2;
      }
      continue;
    }
    if (leaf->cache_B->has_absent(k)) {
      if (has_placed) {
        leaf->cache_B->fill_placeholder_absent(placeholder_idx);
      }
      if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
        record_get_hit(false);
        return {Status::NotFound};
      }
      continue;
    }

    // Fill placeholder based on SSD result
    if (has_placed) {
      if (r.status == Status::Ok) {
        leaf->cache_B->fill_placeholder(placeholder_idx, r.value);
      } else {
        leaf->cache_B->fill_placeholder_absent(placeholder_idx);
      }
    }

    // Version check after read
    if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
      record_get_hit(false);
      return r;
    }
  }

  return {Status::Retry};
}

void Tree::set_probabilities(double p_parent, double p_placeholder) {
  p_parent_ = p_parent;
  p_placeholder_ = p_placeholder;
}

int Tree::debug_height() const {
  return root_->height;
}

bool Tree::debug_cache_a_contains(Key k) const {
  if (root_->height < 2 || !root_->cache_A) return false;
  LookupResult r = root_->cache_A->lookup(k);
  if (r.status == Status::Ok) return true;
  return root_->cache_A->has_absent(k);
}

void Tree::collect_leaves(const Node* node, std::vector<const Node*>& leaves) {
  if (!node) return;
  if (node->height == 1) {
    leaves.push_back(node);
    return;
  }
  for (const Node* child : node->children) {
    collect_leaves(child, leaves);
  }
}

void Tree::collect_leaves(Node* node, std::vector<Node*>& leaves) {
  if (!node) return;
  if (node->height == 1) {
    leaves.push_back(node);
    return;
  }
  for (Node* child : node->children) {
    collect_leaves(child, leaves);
  }
}

Status Tree::debug_flush_all() {
  // Flush all dirty cache entries through the chunk pipeline, then write
  // everything to SSD.
  //
  // The loop handles a subtlety in overflow: when flush_leaf's
  // write_page_entries hits a full page, it re-inserts overflow entries
  // into the correct leaf's cache.  Those re-inserted entries need to be
  // flushed again in a subsequent iteration.  The loop terminates because
  // each overflow round triggers a split that roughly halves the page,
  // guaranteeing that entries eventually fit.

  for (;;) {
    // ---- Phase A: drain all caches into per-leaf chunks ----
    std::vector<Node*> leaves;
    collect_leaves(root_, leaves);

    bool any_entries = false;

    // 1. Evict all leaf caches to per-leaf chunks.
    for (Node* leaf : leaves) {
      std::lock_guard<std::mutex> lock(leaf->eviction_mutex);
      std::vector<std::pair<Key, Value>> dirty;
      leaf->cache_B->flush_dirty(dirty);
      if (dirty.empty()) {
        leaf->cache_B->clear_clean_occupied();
        if (leaf->cache_B->occupied_count() > 0) any_entries = true;
        continue;
      }
      any_entries = true;
      auto* chunk = new EvictChunk{};
      chunk->page_id = leaf->page_id;
      chunk->leaf = leaf;
      chunk->num_entries = dirty.size();
      for (size_t i = 0; i < dirty.size(); ++i) {
        chunk->entries[i].key = dirty[i].first;
        chunk->entries[i].value = dirty[i].second;
        chunk->entries[i].fp = fingerprint(dirty[i].first);
      }
      EvictChunk* old_head = leaf->chunk_head_.load(std::memory_order_acquire);
      do {
        chunk->next.store(old_head, std::memory_order_release);
      } while (!leaf->chunk_head_.compare_exchange_weak(old_head, chunk,
                                                         std::memory_order_acq_rel));
      leaf->chunk_count_.fetch_add(1, std::memory_order_relaxed);

      for (const auto& [k, v] : dirty) {
        leaf->cache_B->evict_clean_slot(k);
      }
    }

    // 2. Flush parent cache dirty entries to leaf caches, then evict.
    if (root_->height >= 2 && root_->cache_A) {
      if (root_->cache_A->occupied_count() > 0) any_entries = true;
      std::vector<std::pair<Key, Value>> root_dirty;
      root_->cache_A->flush_dirty(root_dirty);
      for (const auto& [k, v] : root_dirty) {
        Node* leaf = find_leaf_for_key(root_, k);
        leaf->cache_B->upsert(k, v);
      }
      root_->cache_A->clear_clean_occupied();

      for (Node* leaf : leaves) {
        std::lock_guard<std::mutex> lock(leaf->eviction_mutex);
        std::vector<std::pair<Key, Value>> dirty;
        leaf->cache_B->flush_dirty(dirty);
        if (dirty.empty()) {
          leaf->cache_B->clear_clean_occupied();
          continue;
        }
        any_entries = true;
        auto* chunk = new EvictChunk{};
        chunk->page_id = leaf->page_id;
        chunk->leaf = leaf;
        chunk->num_entries = dirty.size();
        for (size_t i = 0; i < dirty.size(); ++i) {
          chunk->entries[i].key = dirty[i].first;
          chunk->entries[i].value = dirty[i].second;
          chunk->entries[i].fp = fingerprint(dirty[i].first);
        }
        EvictChunk* old_head = leaf->chunk_head_.load(std::memory_order_acquire);
        do {
          chunk->next.store(old_head, std::memory_order_release);
        } while (!leaf->chunk_head_.compare_exchange_weak(old_head, chunk,
                                                           std::memory_order_acq_rel));
        leaf->chunk_count_.fetch_add(1, std::memory_order_relaxed);

        for (const auto& [k, v] : dirty) {
          leaf->cache_B->evict_clean_slot(k);
        }
      }
    }

    // If nothing to flush, we're done.
    if (!any_entries) {
      // Still run step 4 below for any leaves that need splitting.
      // Re-collect leaves since splits may have created new ones.
      leaves.clear();
      collect_leaves(root_, leaves);
      for (Node* leaf : leaves) {
        std::lock_guard<std::mutex> lock(leaf->leaf_index_mutex);
        std::sort(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
        auto last = std::unique(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
        leaf->leaf_keys.erase(last, leaf->leaf_keys.end());
        leaf->leaf_page_ids.assign(leaf->leaf_keys.size(), leaf->page_id);
        if (leaf->leaf_keys.size() > kLeafFanout) {
          split_leaf(leaf);
        }
      }
      return Status::Ok;
    }

    // 3. Flush each leaf's chunks to SSD.
    for (Node* leaf : leaves) {
      flush_leaf(leaf);
    }

    // 4. Update leaf indexes after flush, splitting if needed.
    //    Split may create new leaves — they will be picked up in the
    //    next loop iteration if overflow entries were re-inserted there.
    for (Node* leaf : leaves) {
      std::lock_guard<std::mutex> lock(leaf->leaf_index_mutex);
      std::sort(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
      auto last = std::unique(leaf->leaf_keys.begin(), leaf->leaf_keys.end());
      leaf->leaf_keys.erase(last, leaf->leaf_keys.end());
      leaf->leaf_page_ids.assign(leaf->leaf_keys.size(), leaf->page_id);

      if (leaf->leaf_keys.size() > kLeafFanout) {
        split_leaf(leaf);
      }
    }
  }
}

void Tree::split_leaf(Node* leaf) {
  std::unique_lock<std::shared_mutex> lock(tree_mutex_);
  leaf->version.fetch_add(1, std::memory_order_acq_rel);

  Key mid = leaf->leaf_keys[leaf->leaf_keys.size() / 2];

  PageId new_right_id = 0;
  Status split_s = ssd_->split_page(leaf->page_id, mid, &new_right_id);
  if (split_s != Status::Ok) {
    leaf->version.fetch_add(1, std::memory_order_acq_rel);
    return;
  }

  Node* L_right = new Node{};
  L_right->height = 1;
  L_right->cache_A = std::make_unique<CacheAttachment>();
  L_right->cache_B = std::make_unique<CacheAttachment>();
  L_right->page_id = new_right_id;

  std::vector<Key> left_keys;
  std::vector<PageId> left_pids;
  std::vector<Key> right_keys;
  std::vector<PageId> right_pids;

  for (size_t i = 0; i < leaf->leaf_keys.size(); ++i) {
    if (leaf->leaf_keys[i] < mid) {
      left_keys.push_back(leaf->leaf_keys[i]);
      left_pids.push_back(leaf->leaf_page_ids[i]);
    } else {
      right_keys.push_back(leaf->leaf_keys[i]);
      right_pids.push_back(leaf->leaf_page_ids[i]);
    }
  }
  leaf->leaf_keys = std::move(left_keys);
  leaf->leaf_page_ids = std::move(left_pids);
  L_right->leaf_keys = std::move(right_keys);
  L_right->leaf_page_ids = std::move(right_pids);

  leaf->cache_B->split_into(mid, L_right->cache_B.get());

  // ---- B-link protocol (Lehman & Yao 1981) ----
  // Step 1: link new right sibling BEFORE updating high_key bounds.
  // The sibling link must be established first, so any reader who sees
  // the new (truncated) high_key will find a valid next_sibling to follow.
  // Also update prev_sibling for backward traversal (chunk lookup).
  {
    Node* old_next = leaf->next_sibling.load(std::memory_order_acquire);
    L_right->next_sibling.store(old_next, std::memory_order_release);
    if (old_next) old_next->prev_sibling.store(L_right, std::memory_order_release);
    L_right->prev_sibling.store(leaf, std::memory_order_release);
    leaf->next_sibling.store(L_right, std::memory_order_release);
  }
  // Step 2: update high_key bounds.
  // NOW it is safe to truncate leaf->high_key — readers that chase the
  // right-link will find L_right.
  {
    Key old_high = leaf->high_key;
    leaf->high_key = mid;           // L's range truncated at mid
    L_right->high_key = old_high;   // R inherits L's old upper bound
  }

  // Step 2: now update the parent.  Readers can already reach R via
  // right-links; updating the parent just makes it faster.
  if (leaf == root_) {
    Node* new_root = new Node{};
    new_root->height = 2;
    new_root->cache_A = std::make_unique<CacheAttachment>();
    new_root->separators.reserve(kInternalFanout + 1);
    new_root->children.reserve(kInternalFanout + 1);
    new_root->separators.push_back(mid);
    new_root->children.push_back(leaf);
    new_root->children.push_back(L_right);
    leaf->parent = new_root;
    L_right->parent = new_root;
    root_ = new_root;
  } else {
    Node* parent = leaf->parent;
    auto it = std::lower_bound(parent->separators.begin(), parent->separators.end(),
                               mid);
    size_t idx = static_cast<size_t>(it - parent->separators.begin());
    parent->separators.insert(it, mid);
    parent->children.insert(parent->children.begin() +
                                static_cast<long>(idx) + 1,
                            L_right);
    L_right->parent = parent;
  }

  leaf->version.fetch_add(1, std::memory_order_acq_rel);

  // Release tree_mutex_ before recursing into split_internal.
  lock.unlock();

  if (leaf->parent && leaf->parent->children.size() > kInternalFanout) {
    split_internal(leaf->parent);
  }
}

void Tree::split_internal(Node* node) {
  std::unique_lock<std::shared_mutex> lock(tree_mutex_);
  node->version.fetch_add(1, std::memory_order_acq_rel);

  size_t mid_idx = node->separators.size() / 2;
  Key mid = node->separators[mid_idx];

  if (node->height >= 3 && node->cache_A) {
    if (!node->cache_A->sorted_flag()) node->cache_A->sort_and_set_flag();
    auto occ = node->cache_A->occupied_sorted();
    for (const auto& [k, v] : occ) {
      auto it = std::upper_bound(node->separators.begin(), node->separators.end(), k);
      size_t child_idx = static_cast<size_t>(it - node->separators.begin());
      Node* child = node->children[child_idx];
      if (child->cache_A) {
        child->cache_A->upsert(k, v);
      }
    }
    auto abs = node->cache_A->absent_keys();
    for (Key k : abs) {
      auto it = std::upper_bound(node->separators.begin(), node->separators.end(), k);
      size_t child_idx = static_cast<size_t>(it - node->separators.begin());
      Node* child = node->children[child_idx];
      if (child->cache_A) {
        child->cache_A->mark_absent(k);
      }
    }
    node->cache_A = nullptr;
  }

  Node* new_node = new Node{};
  new_node->height = node->height;
  new_node->separators.reserve(kInternalFanout + 2);
  new_node->children.reserve(kInternalFanout + 2);
  if (new_node->height == 2) {
    new_node->cache_A = std::make_unique<CacheAttachment>();
  }

  new_node->separators.assign(node->separators.begin() + static_cast<long>(mid_idx) + 1,
                              node->separators.end());
  new_node->children.assign(node->children.begin() + static_cast<long>(mid_idx) + 1,
                            node->children.end());

  for (Node* child : new_node->children) {
    child->parent = new_node;
  }

  node->separators.resize(mid_idx);
  node->children.resize(mid_idx + 1);

  if (node->height == 2) {
    node->cache_A->split_into(mid, new_node->cache_A.get());
  }

  // ---- B-link protocol ----
  // Link siblings BEFORE updating high_key bounds, so concurrent readers
  // who follow a stale parent pointer can still reach the right half.
  {
    Node* old_next = node->next_sibling.load(std::memory_order_acquire);
    new_node->next_sibling.store(old_next, std::memory_order_release);
    if (old_next) old_next->prev_sibling.store(new_node, std::memory_order_release);
    new_node->prev_sibling.store(node, std::memory_order_release);
    node->next_sibling.store(new_node, std::memory_order_release);
  }
  {
    Key old_high = node->high_key;
    node->high_key = mid;
    new_node->high_key = old_high;
  }

  if (node == root_) {
    Node* new_root = new Node{};
    new_root->height = node->height + 1;
    new_root->separators.reserve(kInternalFanout + 2);
    new_root->children.reserve(kInternalFanout + 2);
    if (new_root->height < 3) {
      new_root->cache_A = std::make_unique<CacheAttachment>();
    }
    new_root->separators.push_back(mid);
    new_root->children.push_back(node);
    new_root->children.push_back(new_node);
    node->parent = new_root;
    new_node->parent = new_root;
    root_ = new_root;
  } else {
    Node* parent = node->parent;
    auto it = std::lower_bound(parent->separators.begin(), parent->separators.end(),
                               mid);
    size_t idx = static_cast<size_t>(it - parent->separators.begin());
    parent->separators.insert(it, mid);
    parent->children.insert(parent->children.begin() +
                                static_cast<long>(idx) + 1,
                            new_node);
    new_node->parent = parent;
  }

  node->version.fetch_add(1, std::memory_order_acq_rel);

  lock.unlock();

  if (node->parent && node->parent->children.size() > kInternalFanout) {
    split_internal(node->parent);
  }
}

bool Tree::debug_all_leaves_have_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (!leaf->cache_B) return false;
  }
  return true;
}

bool Tree::debug_root_has_cache() const {
  return root_->cache_A != nullptr;
}

bool Tree::debug_height3_nodes_have_no_cache() const {
  std::vector<const Node*> stack;
  stack.push_back(root_);
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();
    if (!node) continue;
    if (node->height >= 3 && (node->cache_A || node->cache_B)) return false;
    for (Node* child : node->children) {
      stack.push_back(child);
    }
  }
  return true;
}

void Tree::debug_clear_all_caches() {
  std::vector<Node*> leaves;
  collect_leaves(root_, leaves);
  for (Node* leaf : leaves) {
    if (leaf->cache_B) leaf->cache_B->clear();
    if (leaf->cache_A) leaf->cache_A->clear();
  }
  if (root_->cache_A) root_->cache_A->clear();
}

bool Tree::debug_leaf_index_empty() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (!leaf->leaf_keys.empty()) return false;
  }
  return true;
}

bool Tree::debug_some_keys_in_leaf_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (leaf->cache_B && leaf->cache_B->occupied_count() > 0) return true;
  }
  return false;
}

size_t Tree::debug_chunk_count() const {
  size_t total = 0;
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    total += leaf->chunk_count_.load(std::memory_order_acquire);
  }
  return total;
}

size_t Tree::debug_peak_chunk_count() const {
  // Per-leaf chains: peak is the sum across all leaves.
  // This is a best-effort snapshot — racy but sufficient for debug.
  size_t total = 0;
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    total += leaf->chunk_count_.load(std::memory_order_relaxed);
  }
  return total;
}

void Tree::debug_reset_peak_chunk_count() {
  // No-op with per-leaf chains — peak tracking is not maintained per-leaf.
  // debug_peak_chunk_count() now returns a live snapshot instead.
}

std::vector<size_t> Tree::debug_chunk_len_samples() const {
  // Per-leaf chains: aggregate samples across all leaves.
  std::vector<size_t> all;
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    all.push_back(leaf->chunk_count_.load(std::memory_order_acquire));
  }
  return all;
}

void Tree::debug_reset_chunk_len_samples() {
  // No-op with per-leaf chains — samples are live snapshots.
}

std::unique_ptr<Tree> Tree::DebugTwoLeaves(const std::string& ssd_path) {
  auto t = std::make_unique<Tree>(ssd_path);

  delete t->root_;

  Node* leaf0 = new Node{};
  leaf0->height = 1;
  leaf0->cache_A = std::make_unique<CacheAttachment>();
  leaf0->cache_B = std::make_unique<CacheAttachment>();
  leaf0->page_id = t->ssd_->alloc_page();
  leaf0->high_key = 50;                           // separator to leaf1

  Node* leaf1 = new Node{};
  leaf1->height = 1;
  leaf1->cache_A = std::make_unique<CacheAttachment>();
  leaf1->cache_B = std::make_unique<CacheAttachment>();
  leaf1->page_id = t->ssd_->alloc_page();
  // leaf1->high_key defaults to max (rightmost)

  leaf0->next_sibling.store(leaf1, std::memory_order_release);
  leaf1->prev_sibling.store(leaf0, std::memory_order_release);

  Node* root = new Node{};
  root->height = 2;
  root->cache_A = std::make_unique<CacheAttachment>();
  root->separators.reserve(kInternalFanout + 2);
  root->children.reserve(kInternalFanout + 2);
  root->separators.push_back(50);
  root->children.push_back(leaf0);
  root->children.push_back(leaf1);
  // root->high_key defaults to max (rightmost)

  leaf0->parent = root;
  leaf1->parent = root;

  t->root_ = root;
  return t;
}

// ---- Hit rate statistics (YCSB-compatible API) ----

MemoryHitStats Tree::memory_hit_stats() const {
  return {
      total_gets_.load(std::memory_order_acquire),
      memory_hits_.load(std::memory_order_acquire),
      ssd_accesses_.load(std::memory_order_acquire),
  };
}

void Tree::reset_memory_hit_stats() {
  total_gets_.store(0, std::memory_order_release);
  memory_hits_.store(0, std::memory_order_release);
  ssd_accesses_.store(0, std::memory_order_release);
}

double Tree::memory_hit_rate() const {
  uint64_t total = total_gets_.load(std::memory_order_acquire);
  if (total == 0) return 0.0;
  uint64_t hits = memory_hits_.load(std::memory_order_acquire);
  return static_cast<double>(hits) / static_cast<double>(total);
}

// ---- Hit-rate tracking ----

void Tree::record_get_hit(bool memory) const {
  // Single predictable branch: when tracking is off, CPU predicts not-taken
  // and the atomic increments are never executed → ~0 cycles overhead.
  if (enable_hit_tracking_) {
    total_gets_.fetch_add(1, std::memory_order_relaxed);
    if (memory) {
      memory_hits_.fetch_add(1, std::memory_order_relaxed);
    } else {
      ssd_accesses_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void Tree::set_hit_tracking(bool on) {
  enable_hit_tracking_ = on;
}

}  // namespace cbtree
