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

  // ---- Phase 1: flush dirty entries → dirty chunk ----
  std::vector<std::pair<Key, Value>> dirty;
  leaf->cache_B->flush_dirty(dirty);

  bool pushed_dirty = false;
  if (!dirty.empty()) {
    auto* chunk = new EvictChunk{};
    chunk->page_id = leaf->page_id;
    chunk->leaf = leaf;
    chunk->num_entries = dirty.size();
    chunk->is_clean_only = false;
    for (size_t i = 0; i < dirty.size(); ++i) {
      chunk->entries[i].key = dirty[i].first;
      chunk->entries[i].value = dirty[i].second;
      chunk->entries[i].fp = fingerprint(dirty[i].first);
    }

    // Push to lock-free chain (newest at head).
    // The chunk is NOW visible to readers — safety net for the window
    // between cache eviction and SSD write.
    {
      EvictChunk* old_head = leaf->chunk_head_.load(std::memory_order_acquire);
      do {
        chunk->next.store(old_head, std::memory_order_release);
      } while (!leaf->chunk_head_.compare_exchange_weak(old_head, chunk,
                                                         std::memory_order_acq_rel));
    }
    leaf->chunk_count_.fetch_add(1, std::memory_order_relaxed);
    leaf->dirty_chunk_count_.fetch_add(1, std::memory_order_relaxed);
    {
      size_t t = total_chunk_count_.fetch_add(1, std::memory_order_relaxed) + 1;
      size_t p = peak_chunk_count_.load(std::memory_order_relaxed);
      while (t > p && !peak_chunk_count_.compare_exchange_weak(p, t,
          std::memory_order_relaxed)) {}
    }
    pushed_dirty = true;
  }

  // ---- Phase 2: if still near capacity, flush clean entries → clean chunk ----
  // Clean chunks are a read buffer — entries already on SSD, no I/O needed on flush.
  std::vector<std::pair<Key, Value>> clean;
  std::vector<bool> clean_is_absent;
  bool pushed_clean = false;

  if (leaf->cache_B->occupied_count() >
      static_cast<int>(kCacheSlots * kLeafFillThreshold)) {
    int n = leaf->cache_B->collect_clean_clock(clean, clean_is_absent, 16);
    if (n > 0) {
      auto* chunk = new EvictChunk{};
      chunk->page_id = leaf->page_id;
      chunk->leaf = leaf;
      chunk->num_entries = static_cast<size_t>(n);
      chunk->is_clean_only = true;
      for (int i = 0; i < n; ++i) {
        chunk->entries[i].key = clean[i].first;
        chunk->entries[i].value = clean[i].second;
        chunk->entries[i].fp = fingerprint(clean[i].first);
        chunk->entries[i].is_absent = clean_is_absent[i];
      }

      {
        EvictChunk* old_head = leaf->chunk_head_.load(std::memory_order_acquire);
        do {
          chunk->next.store(old_head, std::memory_order_release);
        } while (!leaf->chunk_head_.compare_exchange_weak(old_head, chunk,
                                                           std::memory_order_acq_rel));
      }
      leaf->chunk_count_.fetch_add(1, std::memory_order_relaxed);
      {
        size_t t = total_chunk_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        size_t p = peak_chunk_count_.load(std::memory_order_relaxed);
        while (t > p && !peak_chunk_count_.compare_exchange_weak(p, t,
            std::memory_order_relaxed)) {}
      }
      pushed_clean = true;
    }
  }

  // ---- Phase 3: clear cache slots ----
  if (pushed_dirty) {
    for (const auto& [k, v] : dirty) {
      leaf->cache_B->evict_clean_slot(k);
    }
  }
  if (pushed_clean) {
    for (const auto& [k, v] : clean) {
      leaf->cache_B->evict_clean_slot(k);
    }
  }

  // Fallback: if nothing was pushed (no dirty, no clean eligible), wipe all clean.
  if (!pushed_dirty && !pushed_clean) {
    leaf->cache_B->clear_clean_occupied();
    return Status::Ok;
  }

  // ---- Phase 4: deferred batch flush check ----
  evict_lock.unlock();

  size_t dc = leaf->dirty_chunk_count_.load(std::memory_order_acquire);
  size_t tc = leaf->chunk_count_.load(std::memory_order_acquire);
      if (dc > 16 || tc > 20) {
    flush_leaf(leaf);
  }

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
      // Deferred batch flush: modified cache_B via demotion.
      {
        size_t dc = leaf->dirty_chunk_count_.load(std::memory_order_acquire);
        size_t tc = leaf->chunk_count_.load(std::memory_order_acquire);
            if (dc > 16 || tc > 20) {
          flush_leaf(leaf);
        }
      }
    }
  }
  leaf->cache_A->evict_slot(victim_idx, victim_key, victim_gen);
  return Status::Ok;
}

void Tree::flush_and_split_leaf(Node* leaf) {
  // Called only from the overflow path — page is already overfull.
  // split_leaf reads the page once to get the median, then splits.
  split_leaf(leaf);
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
          if (head->entries[i].is_absent) {
            LookupResult r{Status::NotFound};
            r.absent = true;
            return r;
          }
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
        if (head->entries[i].is_absent) continue;  // negative cache, skip
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
  // Per-leaf flush serialization: at most one thread flushes this leaf
  // at a time.  try_to_lock ensures we never block — if another thread
  // is already flushing this leaf, the caller will retry next time.
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

  // Separate dirty (must write to SSD) from clean (read buffer, no I/O).
  std::vector<EvictChunk*> dirty_to_flush;
  std::vector<EvictChunk*> clean_chunks;
  for (EvictChunk* c : to_flush) {
    if (c->is_clean_only) {
      clean_chunks.push_back(c);
    } else {
      dirty_to_flush.push_back(c);
    }
  }

  if (dirty_to_flush.empty() && clean_chunks.empty()) return;

  // ---- Phase 2: batch-write dirty chunks to SSD ----
  // Walk each dirty chunk's entries.  Entries with key < leaf->high_key still
  // belong to this leaf's page — batch them into a single read+write.
  // Entries with key >= high_key were moved to a sibling by a split;
  // route those to the correct page via find_leaf_for_key (rare path).
  // Clean chunks are skipped here — their data is already on SSD.
  if (!dirty_to_flush.empty()) {
    std::vector<std::pair<Key, Value>> local_entries;
    std::map<PageId, std::vector<std::pair<Key, Value>>> remote;
    std::map<PageId, Node*> page_leaf;  // for registration

    Key hk = leaf->high_key;
    for (EvictChunk* c : dirty_to_flush) {
      for (size_t i = 0; i < c->num_entries; ++i) {
        Key key = c->entries[i].key;
        Value val = c->entries[i].value;
        if (hk == std::numeric_limits<Key>::max() || key < hk) {
          local_entries.emplace_back(key, val);
        } else {
          Node* target = find_leaf_for_key(root_, key);
          remote[target->page_id].emplace_back(key, val);
          page_leaf[target->page_id] = target;
        }
      }
    }
    if (!local_entries.empty()) {
      page_leaf[leaf->page_id] = leaf;
    }

    // Helper: batch-merge flush (same as before).
    auto flush_batch_merged = [&](Node* target_leaf, PageId pid,
                                   std::vector<std::pair<Key, Value>>& entries) {
      if (entries.empty()) return;

      std::vector<std::pair<Key, Value>> existing;
      ssd_->dump_sorted(pid, &existing);

      std::vector<std::pair<Key, Value>> merged;
      merged.reserve(existing.size() + entries.size());
      merged.insert(merged.end(), existing.begin(), existing.end());
      merged.insert(merged.end(), entries.begin(), entries.end());
      std::sort(merged.begin(), merged.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

      size_t w = 0;
      for (size_t i = 1; i < merged.size(); ++i) {
        if (merged[i].first != merged[w].first) {
          ++w;
          if (w != i) merged[w] = merged[i];
        } else {
          merged[w] = merged[i];
        }
      }
      merged.resize(w + 1);

      if (merged.size() <= static_cast<size_t>(kMaxRecordsPerPage)) {
        std::vector<std::pair<Key, Value>> dummy;
        Status s = ssd_->write_page_entries(pid, merged, dummy);
        if (s != Status::Ok) {
          for (const auto& [key, val] : merged)
            target_leaf->cache_B->upsert(key, val);
          return;
        }
        return;
      }

      size_t total = merged.size();
      size_t num_pages = (total + kMaxRecordsPerPage - 1) / kMaxRecordsPerPage;
      std::vector<std::vector<std::pair<Key, Value>>> groups(num_pages);
      for (size_t i = 0; i < total; ++i)
        groups[i / kMaxRecordsPerPage].push_back(merged[i]);

      std::vector<Key> boundaries;
      for (size_t p = 0; p < num_pages; ++p)
        boundaries.push_back(groups[p][0].first);

      std::vector<Node*> leaves;
      std::vector<PageId> page_ids;
      leaves.push_back(target_leaf);
      page_ids.push_back(pid);

      {
        std::unique_lock<std::shared_mutex> tree_lock(tree_mutex_);
        target_leaf->version.fetch_add(1, std::memory_order_acq_rel);

        for (size_t p = 1; p < num_pages; ++p) {
          PageId new_pid = ssd_->alloc_page();
          Node* new_leaf = new Node{};
          new_leaf->height = 1;
          new_leaf->cache_A = std::make_unique<CacheAttachment>();
          new_leaf->cache_B = std::make_unique<CacheAttachment>();
          new_leaf->page_id = new_pid;
          page_ids.push_back(new_pid);
          leaves.push_back(new_leaf);
        }

        Key old_high = target_leaf->high_key;
        Node* old_next = target_leaf->next_sibling.load(
            std::memory_order_acquire);

        for (size_t p = 0; p < num_pages - 1; ++p)
          leaves[p]->high_key = boundaries[p + 1];
        leaves.back()->high_key = old_high;

        for (size_t p = 0; p < num_pages - 1; ++p) {
          leaves[p]->next_sibling.store(leaves[p + 1],
                                        std::memory_order_release);
          leaves[p + 1]->prev_sibling.store(leaves[p],
                                            std::memory_order_release);
        }
        leaves.back()->next_sibling.store(old_next,
                                          std::memory_order_release);
        if (old_next)
          old_next->prev_sibling.store(leaves.back(),
                                       std::memory_order_release);

        if (target_leaf == root_) {
          Node* new_root = new Node{};
          new_root->height = target_leaf->height + 1;
          new_root->separators.reserve(kInternalFanout + 2);
          new_root->children.reserve(kInternalFanout + 2);
          for (size_t p = 0; p < num_pages - 1; ++p)
            new_root->separators.push_back(boundaries[p + 1]);
          for (size_t p = 0; p < num_pages; ++p) {
            new_root->children.push_back(leaves[p]);
            leaves[p]->parent = new_root;
          }
          root_ = new_root;
        } else {
          Node* parent = target_leaf->parent;
          auto it = std::find(parent->children.begin(),
                              parent->children.end(), target_leaf);
          size_t idx = static_cast<size_t>(it - parent->children.begin());
          for (size_t p = 1; p < num_pages; ++p) {
            Key sep = boundaries[p];
            auto sit = std::lower_bound(
                parent->separators.begin() + static_cast<long>(idx),
                parent->separators.end(), sep);
            size_t sidx =
                static_cast<size_t>(sit - parent->separators.begin());
            parent->separators.insert(sit, sep);
            parent->children.insert(
                parent->children.begin() + static_cast<long>(sidx) + 1,
                leaves[p]);
            leaves[p]->parent = parent;
          }
        }

        for (size_t p = 0; p < num_pages - 1; ++p) {
          leaves[p]->cache_A->split_into(boundaries[p + 1],
                                          leaves[p + 1]->cache_A.get());
          leaves[p]->cache_B->split_into(boundaries[p + 1],
                                          leaves[p + 1]->cache_B.get());
        }

        target_leaf->version.fetch_add(1, std::memory_order_acq_rel);

        bool need_split_internal =
            target_leaf->parent &&
            target_leaf->parent->children.size() > kInternalFanout;
        Node* split_parent = target_leaf->parent;
        tree_lock.unlock();

        if (need_split_internal) split_internal(split_parent);
      }

      for (size_t p = 0; p < num_pages; ++p) {
        std::vector<std::pair<Key, Value>> dummy;
        Status s = ssd_->write_page_entries(page_ids[p], groups[p], dummy);
        if (s != Status::Ok) {
          for (const auto& [key, val] : groups[p])
            leaves[p]->cache_B->upsert(key, val);
          continue;
        }
        }
    };

    auto flush_batch_remote = [&](PageId pid,
                                   std::vector<std::pair<Key, Value>>& entries) {
      std::vector<std::pair<Key, Value>> overflow;
      Status s = ssd_->write_page_entries(pid, entries, overflow);
      if (s != Status::Ok) {
        Node* n = page_leaf[pid];
        for (const auto& [key, val] : entries) n->cache_B->upsert(key, val);
        return;
      }

      for (const auto& [key, val] : overflow) {
        Node* target = find_leaf_for_key(root_, key);
        flush_and_split_leaf(target);
        Node* correct = find_leaf_for_key(
            target->parent ? target->parent : root_, key);
        Status us = correct->cache_B->upsert(key, val);
        if (us == Status::Full) {
          evict_to_chunk(correct);
          correct->cache_B->upsert(key, val);
        }
      }
    };

    if (!local_entries.empty()) {
      flush_batch_merged(leaf, leaf->page_id, local_entries);
    }
    for (auto& [pid, entries] : remote) {
      flush_batch_remote(pid, entries);
    }
  }

  // ---- Phase 3: compact clean chunks into a single read-buffer chunk ----
  // Merge all clean entries (newest wins since reversed list is oldest-first),
  // dedup by key, keep at most kMaxEntries (discard overflow = LRU-like eviction).
  EvictChunk* compact_clean = nullptr;
  if (!clean_chunks.empty()) {
    // Collect entries newest-first (reverse reversed list back).
    // When the same key appears in multiple clean chunks, the one closer to
    // head (newer) wins.  Iterating clean_chunks forward (oldest→newest)
    // naturally lets newer values overwrite older ones in the map.
    std::map<Key, std::pair<Value, bool>> merged;  // key → (value, is_absent)
    for (EvictChunk* c : clean_chunks) {
      for (size_t i = 0; i < c->num_entries; ++i) {
        merged[c->entries[i].key] = {c->entries[i].value,
                                     c->entries[i].is_absent};
      }
    }

    // Build compact chunk: keep newest entries up to kMaxEntries.
    compact_clean = new EvictChunk{};
    compact_clean->page_id = leaf->page_id;
    compact_clean->leaf = leaf;
    compact_clean->is_clean_only = true;
    compact_clean->num_entries = 0;

    // Iterate in reverse (newest last in map → insert at tail, then oldest
    // at head).  Actually, just fill compact_clean from the most recent entries
    // and keep up to kMaxEntries.
    size_t kept = 0;
    for (auto it = merged.rbegin();
         it != merged.rend() && kept < EvictChunk::kMaxEntries; ++it, ++kept) {
      compact_clean->entries[kept].key = it->first;
      compact_clean->entries[kept].value = it->second.first;
      compact_clean->entries[kept].fp = fingerprint(it->first);
      compact_clean->entries[kept].is_absent = it->second.second;
    }
    compact_clean->num_entries = kept;
  }

  // ---- Phase 4: mark processed chunks for sweep ----
  for (EvictChunk* c : dirty_to_flush) {
    c->flushed.store(true, std::memory_order_release);
  }
  for (EvictChunk* c : clean_chunks) {
    c->flushed.store(true, std::memory_order_release);  // will be freed (compacted)
  }

  // ---- Phase 5: sweep flushed chunks, insert compact at tail ----
  std::vector<EvictChunk*> freed;
  size_t freed_dirty = 0;
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
        if (!h->is_clean_only) freed_dirty++;
      }
      h = next;
    }

    // Append compact clean chunk at tail (old end of chain).
    // It represents merged old entries — searched after newer concurrent pushes.
    if (compact_clean) {
      compact_clean->next.store(nullptr, std::memory_order_release);
      if (!new_head) {
        new_head = compact_clean;
      } else {
        new_tail->next.store(compact_clean, std::memory_order_release);
      }
    }

    if (leaf->chunk_head_.compare_exchange_weak(old_head, new_head,
                                                 std::memory_order_acq_rel)) {
      break;
    }
    freed.clear();
    freed_dirty = 0;
  }

  // ---- Phase 6: wait for in-flight readers, then free ----
  while (leaf->chunk_readers_.load(std::memory_order_acquire) > 0) {
    std::this_thread::yield();
  }

  for (EvictChunk* c : freed) {
    delete c;
  }
  size_t freed_count = freed.size();
  if (compact_clean) {
    // Compact chunk replaces old clean chunks — account for the new chunk.
    leaf->chunk_count_.fetch_add(1, std::memory_order_relaxed);
    total_chunk_count_.fetch_add(1, std::memory_order_relaxed);
  }
  leaf->chunk_count_.fetch_sub(freed_count, std::memory_order_relaxed);
  leaf->dirty_chunk_count_.fetch_sub(freed_dirty, std::memory_order_relaxed);
  total_chunk_count_.fetch_sub(freed_count, std::memory_order_relaxed);
}

// ---- Core operations ----

Status Tree::put(Key k, Value v) {
  // Always descend to leaf first — no parent cache shortcut.
  std::vector<std::pair<Node*, uint64_t>> versions;
  Node* leaf = descend_to_leaf(k, versions);

  // CMS tracking: increment frequency counter on successful write,
  // with periodic decay to adapt to workload shifts.
  auto track_success = [&]() {
    cms_.increment(k);
    uint64_t cnt = cms_op_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cnt % kCMSDecayInterval == 0) {
      std::unique_lock<std::mutex> lock(cms_decay_mutex_, std::try_to_lock);
      if (lock.owns_lock()) {
        cms_.decay(0.5);
      }
    }
    return Status::Ok;
  };

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

      // Placeholder found — fill it unconditionally
      if (lr.placeholder_idx >= 0) {
        Status s = leaf->cache_A->upsert(k, v);
        if (s == Status::Ok) {
          if (leaf->version.load(std::memory_order_acquire) != leaf_v) continue;
          evict_cache_A_if_needed(leaf);
          return track_success();
        }
      }

      bool exists_in_A = (lr.status == Status::Ok) || lr.absent;

      if (exists_in_A) {
        Status s = leaf->cache_A->upsert(k, v);
        if (s == Status::Ok) {
          if (leaf->version.load(std::memory_order_acquire) != leaf_v) continue;
          evict_cache_A_if_needed(leaf);
          return track_success();
        }
      } else {
        bool use_A = false;
        if (p_parent_ >= 1.0) {
          use_A = true;
        } else if (p_parent_ > 0.0) {
          // Count-Min Sketch: estimate write frequency for this key.
          // Promote to cache_A when frequency >= admission threshold.
          // This replaces the old fixed-probability Bernoulli coin flip
          // with a workload-adaptive decision based on actual access patterns.
          uint64_t freq = cms_.estimate(k);
          use_A = (freq >= static_cast<uint64_t>(cms_admission_threshold_));
        }

        if (use_A) {
          Status s = leaf->cache_A->upsert(k, v, true);
          if (s == Status::Ok) {
            if (leaf->version.load(std::memory_order_acquire) != leaf_v) continue;
            evict_cache_A_if_needed(leaf);
            return track_success();
          }
        }
      }
    }

    // Phase 2: upsert into cache_B (local cache)
    {
      Status s = leaf->cache_B->upsert(k, v);
      if (s == Status::Ok) {
        if (leaf->version.load(std::memory_order_acquire) != leaf_v) continue;
        evict_leaf_if_needed(leaf);
        // Deferred batch flush: trigger when chunk counts cross threshold.
        {
          size_t dc = leaf->dirty_chunk_count_.load(std::memory_order_acquire);
          size_t tc = leaf->chunk_count_.load(std::memory_order_acquire);
              if (dc > 16 || tc > 20) {
            flush_leaf(leaf);
          }
        }
        return track_success();
      }
      if (s != Status::Full) return s;
      Status evict_s = evict_to_chunk(leaf);
      if (evict_s == Status::Retry) {
        std::this_thread::yield();
        continue;
      }
      if (evict_s != Status::Ok) return Status::Full;
      leaf = descend_to_leaf(k, versions);
      continue;
    }
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
  static constexpr Key kInf = std::numeric_limits<Key>::max();
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

  if (leaves.empty()) return {};

  // 2. Build result map: key -> (value, authority)
  //    authority: 0 = cache_A, 1 = cache_B, 2 = chunks, 3 = SSD
  struct AuthEntry {
    Value value;
    int authority;
  };
  std::map<Key, AuthEntry> merged;

  for (Node* leaf : leaves) {
    // ---- cache_A (authority 0, highest) ----
    CacheAttachment* ca = leaf->cache_A.get();
    if (!ca->sorted_flag()) ca->sort_and_set_flag();
    auto occ = ca->occupied_sorted();
    for (const auto& [k, v] : occ) {
      if (k >= lo && k <= hi) {
        merged[k] = {v, 0};
      }
    }
    auto abs_a = ca->absent_keys();
    for (Key k : abs_a) {
      if (k >= lo && k <= hi) {
        merged[k] = {0, -1};
      }
    }

    // ---- cache_B (authority 1) ----
    CacheAttachment* cb = leaf->cache_B.get();
    if (!cb->sorted_flag()) cb->sort_and_set_flag();
    auto occ_b = cb->occupied_sorted();
    for (const auto& [k, v] : occ_b) {
      if (k >= lo && k <= hi) {
        auto it = merged.find(k);
        if (it == merged.end()) {
          merged[k] = {v, 1};
        }
      }
    }
    auto abs_b = cb->absent_keys();
    for (Key k : abs_b) {
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
      if (pr.absent) {
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
      if (r.absent) {
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

    // Placeholder placement: always attempt when p_placeholder_ > 0.
    // The CMS access-frequency estimate chooses which cache to try first:
    //   Hot keys (freq >= threshold) → cache_A first, fallback to cache_B
    //   Cold keys (freq < threshold) → cache_B first, fallback to cache_A
    // This protects cache_A from cold-key pollution while ensuring every
    // read miss gets a placeholder somewhere.
    bool has_placed = false;
    int placeholder_idx = -1;
    CacheAttachment* ph_cache = nullptr;

    auto try_place = [&](CacheAttachment* cache) -> bool {
      Status ps = cache->try_place_placeholder(k, &placeholder_idx);
      if (ps == Status::Full) {
        // Make room so the read path can place new placeholders.
        // cache_B → evict_to_chunk (CLOCK-driven, may create dirty/clean chunk);
        // cache_A → evict_cache_A_if_needed (CLOCK victim, demote dirty to cache_B).
        if (cache == leaf->cache_B.get()) {
          evict_to_chunk(leaf);
        } else {
          evict_cache_A_if_needed(leaf);
        }
        ps = cache->try_place_placeholder(k, &placeholder_idx);
      }
      if (ps == Status::Ok) {
        ph_cache = cache;
        has_placed = true;
      }
      return has_placed;
    };

    // Increment CMS on read miss — tracks read access frequency alongside
    // write frequency for a unified hotness signal.
    cms_.increment(k);
    {
      uint64_t cnt = cms_op_count_.fetch_add(1, std::memory_order_relaxed) + 1;
      if (cnt % kCMSDecayInterval == 0) {
        std::unique_lock<std::mutex> lock(cms_decay_mutex_, std::try_to_lock);
        if (lock.owns_lock()) { cms_.decay(0.5); }
      }
    }

    uint64_t freq = cms_.estimate(k);
    bool hot = (freq >= static_cast<uint64_t>(cms_admission_threshold_));

    if (p_placeholder_ >= 1.0) {
      // Always place: CMS chooses which cache to try first.
      if (hot) {
        if (!try_place(leaf->cache_A.get())) try_place(leaf->cache_B.get());
      } else {
        if (!try_place(leaf->cache_B.get())) try_place(leaf->cache_A.get());
      }
    } else if (p_placeholder_ > 0.0) {
      // Legacy Bernoulli: p_placeholder_ controls placement probability.
      // When placing, still use CMS for cache selection.
      if (std::bernoulli_distribution{p_placeholder_}(rng)) {
        if (hot) {
          if (!try_place(leaf->cache_A.get())) try_place(leaf->cache_B.get());
        } else {
          if (!try_place(leaf->cache_B.get())) try_place(leaf->cache_A.get());
        }
      }
    }

    // Query SSD
    LookupResult r = ssd_->get_record(leaf->page_id, k);

    // Fast path: if we placed a placeholder, try to fill it with the SSD
    // result.  If the placeholder was untouched during the SSD read, no
    // concurrent put() modified our key — the SSD result is authoritative.
    // This eliminates two full cache-probe scans (cache_B + cache_A recheck)
    // on the common path.
    if (has_placed) {
      Status fill_s = (r.status == Status::Ok)
          ? ph_cache->fill_placeholder(placeholder_idx, r.value)
          : ph_cache->fill_placeholder_absent(placeholder_idx);

      if (fill_s == Status::Ok) {
        // The placeholder was untouched.  However, a concurrent put() may
        // have written to the OTHER cache (e.g. placeholder in cache_B but
        // put() wrote to cache_A via p_parent).  Check the other cache.
        CacheAttachment* other = (ph_cache == leaf->cache_A.get())
            ? leaf->cache_B.get() : leaf->cache_A.get();
        LookupResult other_r = other->lookup(k);
        if (other_r.status == Status::Ok) {
          // Other cache has a fresher value — use it.
          ph_cache->fill_placeholder(placeholder_idx, other_r.value);
          if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
            record_get_hit(false);
            return other_r;
          }
          continue;
        }
        // Both caches missed — SSD result is authoritative.
        if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
          record_get_hit(false);
          return r;
        }
        continue;
      }
      // fill_placeholder failed — race detected, fall through to rechecks.
    }

    // Rare path: either we didn't place a placeholder, or a concurrent
    // put() modified it during the SSD read.  Recheck caches.
    {
      LookupResult r2 = leaf->cache_B->lookup(k);
      if (r2.status == Status::Ok) {
        if (has_placed) {
          ph_cache->fill_placeholder(placeholder_idx, r2.value);
        }
        if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
          record_get_hit(false);
          return r2;
        }
        continue;
      }
      if (leaf->cache_B->has_absent(k)) {
        if (has_placed) {
          ph_cache->fill_placeholder_absent(placeholder_idx);
        }
        if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
          record_get_hit(false);
          return {Status::NotFound};
        }
        continue;
      }

      LookupResult rA = leaf->cache_A->lookup(k);
      if (rA.status == Status::Ok) {
        if (has_placed) {
          ph_cache->fill_placeholder(placeholder_idx, rA.value);
        }
        if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
          record_get_hit(false);
          return rA;
        }
        continue;
      }
      if (leaf->cache_A->has_absent(k)) {
        if (has_placed) {
          ph_cache->fill_placeholder_absent(placeholder_idx);
        }
        if (leaf->version.load(std::memory_order_acquire) == leaf_v) {
          record_get_hit(false);
          return {Status::NotFound};
        }
        continue;
      }
    }

    // If we reach here without a placeholder: return the SSD result.
    // With a placeholder: we already returned on successful fill above;
    // reaching here means all lookups missed — return SSD result.
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

void Tree::set_flush_batch_threshold(int threshold) {
  flush_batch_threshold_ = threshold;
}

void Tree::set_cms_admission_threshold(int threshold) {
  cms_admission_threshold_ = threshold;
}

int Tree::cms_admission_threshold() const {
  return cms_admission_threshold_;
}

int Tree::debug_height() const {
  return root_->height;
}

bool Tree::debug_cache_a_contains(Key k) const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (!leaf->cache_A) continue;
    LookupResult r = leaf->cache_A->lookup(k);
    if (r.status == Status::Ok) return true;
    if (leaf->cache_A->has_absent(k)) return true;
  }
  return false;
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

      // Phase 1: flush dirty → dirty chunk.
      std::vector<std::pair<Key, Value>> dirty;
      leaf->cache_B->flush_dirty(dirty);

      // Phase 2: flush clean (Occupied+Absent) → clean chunk.
      std::vector<std::pair<Key, Value>> clean;
      std::vector<bool> clean_is_absent;
      leaf->cache_B->collect_clean_clock(clean, clean_is_absent,
                                         EvictChunk::kMaxEntries);

      if (dirty.empty() && clean.empty()) {
        leaf->cache_B->clear_clean_occupied();  // fallback
        if (leaf->cache_B->occupied_count() > 0) any_entries = true;
        continue;
      }
      any_entries = true;

      // Push dirty chunk.
      if (!dirty.empty()) {
        auto* chunk = new EvictChunk{};
        chunk->page_id = leaf->page_id;
        chunk->leaf = leaf;
        chunk->num_entries = dirty.size();
        chunk->is_clean_only = false;
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
        leaf->dirty_chunk_count_.fetch_add(1, std::memory_order_relaxed);
        {
          size_t t = total_chunk_count_.fetch_add(1, std::memory_order_relaxed) + 1;
          size_t p = peak_chunk_count_.load(std::memory_order_relaxed);
          while (t > p && !peak_chunk_count_.compare_exchange_weak(p, t,
              std::memory_order_relaxed)) {}
        }
        for (const auto& [k, v] : dirty) {
          leaf->cache_B->evict_clean_slot(k);
        }
      }

      // Push clean chunk.
      if (!clean.empty()) {
        auto* chunk = new EvictChunk{};
        chunk->page_id = leaf->page_id;
        chunk->leaf = leaf;
        chunk->num_entries = clean.size();
        chunk->is_clean_only = true;
        for (size_t i = 0; i < clean.size(); ++i) {
          chunk->entries[i].key = clean[i].first;
          chunk->entries[i].value = clean[i].second;
          chunk->entries[i].fp = fingerprint(clean[i].first);
          chunk->entries[i].is_absent = clean_is_absent[i];
        }
        EvictChunk* old_head = leaf->chunk_head_.load(std::memory_order_acquire);
        do {
          chunk->next.store(old_head, std::memory_order_release);
        } while (!leaf->chunk_head_.compare_exchange_weak(old_head, chunk,
                                                           std::memory_order_acq_rel));
        leaf->chunk_count_.fetch_add(1, std::memory_order_relaxed);
        {
          size_t t = total_chunk_count_.fetch_add(1, std::memory_order_relaxed) + 1;
          size_t p = peak_chunk_count_.load(std::memory_order_relaxed);
          while (t > p && !peak_chunk_count_.compare_exchange_weak(p, t,
              std::memory_order_relaxed)) {}
        }
        for (size_t i = 0; i < clean.size(); ++i) {
          leaf->cache_B->evict_clean_slot(clean[i].first);
        }
      }
    }

    // 2. Drain cache_A entries into cache_B, then evict to chunks.
    for (Node* leaf : leaves) {
      std::vector<std::pair<Key, Value>> a_dirty;
      leaf->cache_A->flush_dirty(a_dirty);
      if (!a_dirty.empty()) {
        any_entries = true;
        for (const auto& [k, v] : a_dirty) {
          leaf->cache_B->upsert(k, v);
        }
        leaf->cache_A->clear_clean_occupied();

        // Evict cache_B entries just populated from cache_A
        std::lock_guard<std::mutex> lock(leaf->eviction_mutex);
        std::vector<std::pair<Key, Value>> dirty;
        leaf->cache_B->flush_dirty(dirty);
        if (!dirty.empty()) {
          auto* chunk = new EvictChunk{};
          chunk->page_id = leaf->page_id;
          chunk->leaf = leaf;
          chunk->num_entries = dirty.size();
          chunk->is_clean_only = false;
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
          leaf->dirty_chunk_count_.fetch_add(1, std::memory_order_relaxed);
          {
            size_t t = total_chunk_count_.fetch_add(1, std::memory_order_relaxed) + 1;
            size_t p = peak_chunk_count_.load(std::memory_order_relaxed);
            while (t > p && !peak_chunk_count_.compare_exchange_weak(p, t,
                std::memory_order_relaxed)) {}
          }

          for (const auto& [k, v] : dirty) {
            leaf->cache_B->evict_clean_slot(k);
          }
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
        // Check if the page has more entries than kLeafFanout due to
        // reader insertions that hit cache_B before flush cleaned it.
        std::vector<std::pair<Key, Value>> entries;
        if (ssd_->dump_sorted(leaf->page_id, &entries) != Status::Ok) continue;
        if (entries.size() > kLeafFanout) {
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
      // Check page fullness after flush — split any overfull leaves.
      std::vector<std::pair<Key, Value>> entries;
      if (ssd_->dump_sorted(leaf->page_id, &entries) != Status::Ok) continue;

      if (entries.size() > kLeafFanout) {
        split_leaf(leaf);
      }
    }
  }
}

void Tree::split_leaf(Node* leaf) {
  std::unique_lock<std::shared_mutex> lock(tree_mutex_);
  leaf->version.fetch_add(1, std::memory_order_acq_rel);

  // Read current keys from the SSD page to find the median split point.
  std::vector<std::pair<Key, Value>> entries;
  Status read_s = ssd_->dump_sorted(leaf->page_id, &entries);
  if (read_s != Status::Ok || entries.empty()) {
    leaf->version.fetch_add(1, std::memory_order_acq_rel);
    return;
  }
  Key mid = entries[entries.size() / 2].first;

  PageId new_right_id = 0;
  Status split_s = ssd_->split_page(leaf->page_id, mid, entries, &new_right_id);
  if (split_s != Status::Ok) {
    leaf->version.fetch_add(1, std::memory_order_acq_rel);
    return;
  }

  Node* L_right = new Node{};
  L_right->height = 1;
  L_right->cache_A = std::make_unique<CacheAttachment>();
  L_right->cache_B = std::make_unique<CacheAttachment>();
  L_right->page_id = new_right_id;

  leaf->cache_A->split_into(mid, L_right->cache_A.get());
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

  Node* new_node = new Node{};
  new_node->height = node->height;
  new_node->separators.reserve(kInternalFanout + 2);
  new_node->children.reserve(kInternalFanout + 2);

  new_node->separators.assign(node->separators.begin() + static_cast<long>(mid_idx) + 1,
                              node->separators.end());
  new_node->children.assign(node->children.begin() + static_cast<long>(mid_idx) + 1,
                            node->children.end());

  for (Node* child : new_node->children) {
    child->parent = new_node;
  }

  node->separators.resize(mid_idx);
  node->children.resize(mid_idx + 1);

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
    if (!leaf->cache_A || !leaf->cache_B) return false;
  }
  return true;
}

bool Tree::debug_height3_nodes_have_no_cache() const {
  std::vector<const Node*> stack;
  stack.push_back(root_);
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();
    if (!node) continue;
    if (node->height >= 2 && (node->cache_A || node->cache_B)) return false;
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
}

bool Tree::debug_leaf_index_empty() const {
  // leaf_keys/leaf_page_ids have been removed — the in-memory leaf index
  // no longer exists. Always returns true for backward compatibility.
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
  return peak_chunk_count_.load(std::memory_order_relaxed);
}

void Tree::debug_reset_peak_chunk_count() {
  peak_chunk_count_.store(0, std::memory_order_relaxed);
  total_chunk_count_.store(0, std::memory_order_relaxed);
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

SsDPageStore::IoStats Tree::io_stats() const {
  return ssd_->io_stats();
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
