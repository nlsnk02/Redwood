# Leaf-Only Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate all caching from internal nodes; move parent-cache semantics into `cache_A` on leaf nodes alongside existing `cache_B`.

**Architecture:** Leaf nodes gain two `CacheAttachment` instances: `cache_A` (hot cache, former parent semantics, authority 0) and `cache_B` (local cache, authority 1). Internal nodes have neither. Eviction chain: `cache_A` → `cache_B` → chunk → SSD. `CacheAttachment` class itself is unchanged.

**Tech Stack:** C++20, CMake, GoogleTest (FetchContent)

## Global Constraints

- No changes to `CacheAttachment` class (public interface or implementation).
- No changes to `EvictChunk`, `SsDPageStore`, chunk pipeline, or B-link protocol.
- `kCacheSlots = 64`, `kLeafFillThreshold = 0.8`, `kParentFillThreshold = 0.8`, `kDefaultPParent = 0.1` remain unchanged.
- All existing test scenarios must pass (adapted for new semantics).
- Internal nodes at any height must not hold a `CacheAttachment`.

---

### Task 1: Update node.hpp — replace `cache` with `cache_A` + `cache_B`

**Files:**
- Modify: `include/cbtree/node.hpp`

**Interfaces:**
- Produces: `Node::cache_A` (`std::unique_ptr<CacheAttachment>`), `Node::cache_B` (`std::unique_ptr<CacheAttachment>`)
- Removes: `Node::cache`

- [ ] **Step 1: Replace the single cache field with cache_A + cache_B**

Open `include/cbtree/node.hpp`. Replace line 41 (the `std::unique_ptr<CacheAttachment> cache;` field and its comment) with:

```cpp
  // Dual cache on leaf nodes (height == 1); both nullptr on internal nodes.
  // cache_A: hot cache (former parent-cache semantics, authority 0)
  // cache_B: local cache (former leaf-cache semantics, authority 1)
  std::unique_ptr<CacheAttachment> cache_A;
  std::unique_ptr<CacheAttachment> cache_B;
```

- [ ] **Step 2: Verify the file compiles standalone**

No compilation yet — just verify the file is syntactically correct by inspection: two `unique_ptr<CacheAttachment>` fields, no other changes.

- [ ] **Step 3: Commit**

```bash
git add include/cbtree/node.hpp
git commit -m "refactor: replace Node::cache with cache_A + cache_B

Dual-cache model: cache_A for hot keys (former parent cache),
cache_B for local keys. Internal nodes leave both as nullptr.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 2: Update tree.hpp — function signatures

**Files:**
- Modify: `include/cbtree/tree.hpp`

**Interfaces:**
- Removes: `evict_parent_if_needed(Node*)`, `debug_parent_cache_contains(Key)`, `debug_root_has_cache()`, `find_leaf_for_key(Node*, Key)`, `register_in_leaf_index(Node*, Key)`
- Produces: `evict_cache_A_if_needed(Node*)`, `evict_cache_B_if_needed(Node*)`

- [ ] **Step 1: Update tree.hpp private section**

Read the file, then replace the private section (lines 86-104 and the debug hooks on lines 39-54) as specified below.

In `include/cbtree/tree.hpp`, replace the entire private section and relevant debug hooks:

**Remove these declarations:**
```cpp
  // old — delete these
  Node* find_leaf_for_key(Node* parent, Key k);
  void register_in_leaf_index(Node* leaf, Key k);
  Status evict_parent_if_needed(Node* parent);
  bool debug_parent_cache_contains(Key k) const;
  bool debug_root_has_cache() const;
```

**Add these declarations in the private section:**
```cpp
  Status evict_cache_A_if_needed(Node* leaf);
  Status evict_cache_B_if_needed(Node* leaf);
  Status evict_to_chunk(Node* leaf);
  void flush_and_split_leaf(Node* leaf);
```

**Keep these unchanged:**
```cpp
  Node* descend_to_leaf(Key k,
                        std::vector<std::pair<Node*, uint64_t>>& versions);
  void split_leaf(Node* leaf);
  void split_internal(Node* node);
  static void collect_leaves(const Node* node, std::vector<const Node*>& leaves);
  static void collect_leaves(Node* node, std::vector<Node*>& leaves);
  static void collect_leaves_in_range(Node* node, Key lo, Key hi,
                                      std::vector<Node*>& leaves);
  Node* find_leaf_for_key(Node* parent, Key k);    // keep — used by chunk/flush internals
  void register_in_leaf_index(Node* leaf, Key k);   // keep — used by flush_leaf
  Status evict_leaf_if_needed(Node* leaf);           // keep — renamed to evict_cache_B_if_needed internally
```

Actually, let's be precise. Here is the exact new private section:

```cpp
 private:
  Node* descend_to_leaf(Key k,
                        std::vector<std::pair<Node*, uint64_t>>& versions);
  void split_leaf(Node* leaf);
  void split_internal(Node* node);
  static void collect_leaves(const Node* node, std::vector<const Node*>& leaves);
  static void collect_leaves(Node* node, std::vector<Node*>& leaves);
  static void collect_leaves_in_range(Node* node, Key lo, Key hi,
                                      std::vector<Node*>& leaves);

  Node* find_leaf_for_key(Node* parent, Key k);
  void register_in_leaf_index(Node* leaf, Key k);
  Status evict_leaf_if_needed(Node* leaf);
  Status evict_to_chunk(Node* leaf);
  Status evict_cache_A_if_needed(Node* leaf);
  void flush_and_split_leaf(Node* leaf);
  // Chunk-based eviction: pack dirty slots into a chunk, push to the leaf's
  // own chain, then flush that leaf's chain to SSD inline.

  // Hit-rate tracking: record one completed get() result.
  void record_get_hit(bool memory) const;

  // Chunk chain helpers — operate on per-leaf chains.
  LookupResult lookup_chunks(Key k);
  void collect_chunk_entries_in_range(Key lo, Key hi,
                                      std::map<Key, Value>& out);

  // Flush one leaf's pending chunks to SSD (synchronous).
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
  bool enable_hit_tracking_{true};
```

**Update the public debug hooks section** — replace:
```cpp
  bool debug_parent_cache_contains(Key k) const;
  bool debug_root_has_cache() const;
```
with:
```cpp
  bool debug_cache_a_contains(Key k) const;
```

- [ ] **Step 2: Commit**

```bash
git add include/cbtree/tree.hpp
git commit -m "refactor: update Tree API — remove parent cache hooks, add cache_A eviction

- Remove debug_parent_cache_contains, debug_root_has_cache
- Add evict_cache_A_if_needed, evict_cache_B_if_needed
- Add debug_cache_a_contains
- Keep find_leaf_for_key + register_in_leaf_index (used by flush internals)

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 3: Update tree.cpp — constructor, destructor, and leaf eviction helpers

**Files:**
- Modify: `src/tree.cpp`

**Interfaces:**
- Consumes: `Node::cache_A`, `Node::cache_B`
- Produces: `evict_cache_B_if_needed`, `evict_to_chunk` (using cache_B), `evict_cache_A_if_needed`

- [ ] **Step 1: Update Tree constructor**

Find the constructor (line 14-20). Replace:

```cpp
Tree::Tree(const std::string& ssd_path, bool use_direct)
    : ssd_(std::make_unique<SsDPageStore>(ssd_path, use_direct)) {
  root_ = new Node{};
  root_->height = 1;
  root_->cache = std::make_unique<CacheAttachment>();
  root_->page_id = ssd_->alloc_page();
}
```

with:

```cpp
Tree::Tree(const std::string& ssd_path, bool use_direct)
    : ssd_(std::make_unique<SsDPageStore>(ssd_path, use_direct)) {
  root_ = new Node{};
  root_->height = 1;
  root_->cache_A = std::make_unique<CacheAttachment>();
  root_->cache_B = std::make_unique<CacheAttachment>();
  root_->page_id = ssd_->alloc_page();
}
```

- [ ] **Step 2: Update evict_leaf_if_needed → evict_cache_B_if_needed**

Find `evict_leaf_if_needed` (line 108-115). Replace:

```cpp
Status Tree::evict_leaf_if_needed(Node* leaf) {
  if (leaf->cache->occupied_count() <=
      static_cast<int>(kCacheSlots * kLeafFillThreshold))
    return Status::Ok;
  // Chunk-based eviction: pack dirty slots into chunk, SSD write deferred.
  Status s = evict_to_chunk(leaf);
  return (s == Status::Retry) ? Status::Ok : s;
}
```

with:

```cpp
Status Tree::evict_leaf_if_needed(Node* leaf) {
  if (leaf->cache_B->occupied_count() <=
      static_cast<int>(kCacheSlots * kLeafFillThreshold))
    return Status::Ok;
  Status s = evict_to_chunk(leaf);
  return (s == Status::Retry) ? Status::Ok : s;
}
```

- [ ] **Step 3: Update evict_to_chunk — all cache → cache_B**

In `evict_to_chunk` (lines 117-173), replace every `leaf->cache` with `leaf->cache_B`. There are 5 occurrences:

Line 125: `leaf->cache->flush_dirty(dirty);` → `leaf->cache_B->flush_dirty(dirty);`
Line 127: `leaf->cache->clear_clean_occupied();` → `leaf->cache_B->clear_clean_occupied();`
Line 159: `leaf->cache->evict_clean_slot(k);` → `leaf->cache_B->evict_clean_slot(k);`

- [ ] **Step 4: Rewrite evict_parent_if_needed → evict_cache_A_if_needed**

Replace the entire `evict_parent_if_needed` function (lines 176-203) with:

```cpp
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
    evict_leaf_if_needed(leaf);  // ensure cache_B has room
    leaf->cache_B->upsert(victim_key, victim_val);
  }
  leaf->cache_A->evict_slot(victim_idx, victim_key, victim_gen);
  return Status::Ok;
}
```

- [ ] **Step 5: Update flush_and_split_leaf — cache → cache_B (overflow re-insert)**

Find `flush_and_split_leaf` (line 205-216). No direct cache reference here — this handles leaf keys. No change needed.

But check line 363 and 389 in `flush_leaf`: `n->cache->upsert(key, val)` and `correct->cache->upsert(key, val)` — both re-insert overflow entries. Replace with `cache_B`:

Line 363: `n->cache->upsert(key, val);` → `n->cache_B->upsert(key, val);`
Line 389: `correct->cache->upsert(key, val);` → `correct->cache_B->upsert(key, val);`

- [ ] **Step 6: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: eviction helpers — cache_A demotion + cache_B plumbing

- Constructor creates cache_A + cache_B on root leaf
- evict_to_chunk uses cache_B
- evict_cache_A_if_needed demotes to cache_B on same leaf (no re-descent)
- flush_leaf overflow re-inserts go to cache_B

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 4: Rewrite put() — always descend, then cache_A / cache_B

**Files:**
- Modify: `src/tree.cpp`

- [ ] **Step 1: Rewrite the entire put() function**

Find `Tree::put` (lines 454-536). Replace the entire function body with:

```cpp
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
```

- [ ] **Step 2: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: rewrite put() — always descend, cache_A then cache_B

Remove shared_lock phase for parent cache. Always descend first.
At the leaf: try cache_A (with p_parent probability), then cache_B.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 5: Rewrite get() — descend first, then cache_A → cache_B

**Files:**
- Modify: `src/tree.cpp`

- [ ] **Step 1: Rewrite the entire get() function**

Find `Tree::get` (lines 677-799). Replace the entire function body with:

```cpp
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
```

- [ ] **Step 2: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: rewrite get() — descend first, check cache_A then cache_B

Remove root/parent cache probing. Always descend to leaf first,
then check cache_A (authority 0) then cache_B (authority 1).

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 6: Rewrite scan() — per-leaf dual cache

**Files:**
- Modify: `src/tree.cpp`

- [ ] **Step 1: Rewrite scan() — remove parent cache sweep logic**

Find `Tree::scan` (lines 558-675). Replace the entire function body with:

```cpp
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
```

- [ ] **Step 2: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: rewrite scan() — per-leaf cache_A + cache_B authority merge

Remove global processed_parents tracking. Each leaf self-contains
its cache_A (auth 0) and cache_B (auth 1) data for the merge.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 7: Rewrite split_leaf() — dual cache split

**Files:**
- Modify: `src/tree.cpp`

- [ ] **Step 1: Update split_leaf — create cache_A + cache_B on new right leaf**

Find `split_leaf` (lines 973-1067). The key change is at lines 988-989 where the new right leaf is created and at line 1010 where `split_into` is called.

Replace lines 986-989:
```cpp
  Node* L_right = new Node{};
  L_right->height = 1;
  L_right->cache = std::make_unique<CacheAttachment>();
  L_right->page_id = new_right_id;
```

with:
```cpp
  Node* L_right = new Node{};
  L_right->height = 1;
  L_right->cache_A = std::make_unique<CacheAttachment>();
  L_right->cache_B = std::make_unique<CacheAttachment>();
  L_right->page_id = new_right_id;
```

Replace line 1010:
```cpp
  leaf->cache->split_into(mid, L_right->cache.get());
```

with:
```cpp
  leaf->cache_A->split_into(mid, L_right->cache_A.get());
  leaf->cache_B->split_into(mid, L_right->cache_B.get());
```

Also update the parent/root creation section (lines 1035-1046) — replace:
```cpp
    new_root->cache = std::make_unique<CacheAttachment>();
```
with nothing (remove that line — new root at height=2 shouldn't create cache):
```cpp
    Node* new_root = new Node{};
    new_root->height = 2;
    new_root->separators.reserve(kInternalFanout + 1);
    new_root->children.reserve(kInternalFanout + 1);
```

- [ ] **Step 2: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: split_leaf creates cache_A + cache_B on right leaf

New right leaf gets both caches. split_into called for both.
New root at height=2 does NOT create a cache.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 8: Rewrite split_internal() — no cache handling

**Files:**
- Modify: `src/tree.cpp`

- [ ] **Step 1: Remove all cache push-down and creation logic from split_internal**

Find `split_internal` (lines 1069-1172). Three changes needed:

**Change 1:** Remove the cache push-down block (lines 1076-1097):

Delete these lines entirely:
```cpp
  if (node->height >= 3 && node->cache) {
    if (!node->cache->sorted_flag()) node->cache->sort_and_set_flag();
    auto occ = node->cache->occupied_sorted();
    for (const auto& [k, v] : occ) {
      auto it = std::upper_bound(node->separators.begin(), node->separators.end(), k);
      size_t child_idx = static_cast<size_t>(it - node->separators.begin());
      Node* child = node->children[child_idx];
      if (child->cache) {
        child->cache->upsert(k, v);
      }
    }
    auto abs = node->cache->absent_keys();
    for (Key k : abs) {
      auto it = std::upper_bound(node->separators.begin(), node->separators.end(), k);
      size_t child_idx = static_cast<size_t>(it - node->separators.begin());
      Node* child = node->children[child_idx];
      if (child->cache) {
        child->cache->mark_absent(k);
      }
    }
    node->cache = nullptr;
  }
```

**Change 2:** Remove cache creation for new_node (lines 1103-1105):

Replace:
```cpp
  if (new_node->height == 2) {
    new_node->cache = std::make_unique<CacheAttachment>();
  }
```
with nothing (delete these 3 lines).

**Change 3:** Remove split_into for height==2 (lines 1119-1121):

Replace:
```cpp
  if (node->height == 2) {
    node->cache->split_into(mid, new_node->cache.get());
  }
```
with nothing (delete these 3 lines).

**Change 4:** Remove cache creation for new_root (lines 1144-1146):

Replace:
```cpp
    if (new_root->height < 3) {
      new_root->cache = std::make_unique<CacheAttachment>();
    }
```
with nothing (delete these 3 lines).

- [ ] **Step 2: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: split_internal — remove all cache push-down and creation

Internal nodes never hold caches. No cache push-down at height>=3,
no cache creation for new internal nodes or new roots.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 9: Update debug_flush_all() — dual cache, no parent

**Files:**
- Modify: `src/tree.cpp`

- [ ] **Step 1: Rewrite debug_flush_all — Phase A drain both caches, no parent flush**

Find `debug_flush_all` (lines 840-971). The key changes are in Phase A (the loop body):

**Change the leaf cache drain loop** (around lines 858-888) — replace `leaf->cache` with `leaf->cache_B`:

All 6+ occurrences of `leaf->cache->` become `leaf->cache_B->`.

**Remove the entire "Flush parent cache dirty entries" block** (lines 891-929, the `if (root_->height >= 2 && root_->cache)` block). Delete it entirely.

**Add a cache_A drain loop before the cache_B loop** — after evicting cache_B entries, also drain cache_A:

After the existing leaf cache_B drain loop, add:

```cpp
    // 2. Drain cache_A entries into cache_B, then evict cache_B.
    for (Node* leaf : leaves) {
      // Move cache_A dirty entries to cache_B
      std::vector<std::pair<Key, Value>> a_dirty;
      leaf->cache_A->flush_dirty(a_dirty);
      for (const auto& [k, v] : a_dirty) {
        leaf->cache_B->upsert(k, v);
      }
      leaf->cache_A->clear_clean_occupied();

      // Now evict cache_B
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
```

Actually, let me think about this more carefully. The original `debug_flush_all` has two phases in its loop. Let me rewrite it more cleanly. The whole function needs to:

1. Drain cache_B dirty entries → chunk chain (existing logic)
2. Drain cache_A dirty entries → cache_B → chunk chain (new)
3. Flush all leaf chunks to SSD
4. Update leaf indexes, split if needed

Let me provide the complete replacement. But this is a large function. Let me instead provide targeted edits.

**Actually, let me simplify.** The original Phase A has two sub-steps: first evict leaf caches, then flush parent cache dirty → leaf caches → re-evict. Instead:

1. For each leaf: flush cache_B dirty → chunk (keep existing)
2. For each leaf: flush cache_A dirty → upsert into cache_B; then flush cache_B dirty → chunk
3. Flush chunks → SSD
4. Split if needed

Let me just provide the edits to the existing function. I'll replace the inner loop body.

Replace the block from line 851 `for (;;) {` through line 930 (end of Phase A):

The first leaf cache_B drain stays the same (lines 858-888), just `->cache` → `->cache_B`.

Then replace lines 891-929 (the parent cache flush block) with a cache_A drain:

```cpp
    // 2. Drain cache_A entries into cache_B, then evict to chunks.
    for (Node* leaf : leaves) {
      // Move cache_A dirty entries to cache_B
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
    }
```

This is getting complex. Let me take a step back. For this task, I'll provide the edits and commit, then verify at the end with compilation.

- [ ] **Step 2: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: debug_flush_all — dual cache drain, no parent flush

- cache_B drain kept as-is (cache -> cache_B)
- cache_A dirty entries demoted to cache_B, then evicted
- Parent cache flush block removed entirely

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 10: Update DebugTwoLeaves() and all debug hooks

**Files:**
- Modify: `src/tree.cpp`

- [ ] **Step 1: Update DebugTwoLeaves**

Find `DebugTwoLeaves` (lines 1271-1306). Replace:

Lines 1278-1279 (leaf0):
```cpp
  leaf0->cache = std::make_unique<CacheAttachment>();
```
with:
```cpp
  leaf0->cache_A = std::make_unique<CacheAttachment>();
  leaf0->cache_B = std::make_unique<CacheAttachment>();
```

Lines 1283-1284 (leaf1):
```cpp
  leaf1->cache = std::make_unique<CacheAttachment>();
```
with:
```cpp
  leaf1->cache_A = std::make_unique<CacheAttachment>();
  leaf1->cache_B = std::make_unique<CacheAttachment>();
```

Lines 1292-1293 (root) — remove cache creation entirely:
```cpp
  root->cache = std::make_unique<CacheAttachment>();
```
Delete this line (root at height=2 has no cache).

- [ ] **Step 2: Update debug_all_leaves_have_cache**

Find `debug_all_leaves_have_cache` (lines 1174-1181). Replace:

```cpp
bool Tree::debug_all_leaves_have_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (!leaf->cache) return false;
  }
  return true;
}
```

with:

```cpp
bool Tree::debug_all_leaves_have_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (!leaf->cache_A || !leaf->cache_B) return false;
  }
  return true;
}
```

- [ ] **Step 3: Replace debug_parent_cache_contains with debug_cache_a_contains**

Find `debug_parent_cache_contains` (lines 811-816). Replace:

```cpp
bool Tree::debug_parent_cache_contains(Key k) const {
  if (root_->height < 2 || !root_->cache) return false;
  LookupResult r = root_->cache->lookup(k);
  if (r.status == Status::Ok) return true;
  return root_->cache->has_absent(k);
}
```

with:

```cpp
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
```

- [ ] **Step 4: Update debug_clear_all_caches**

Find `debug_clear_all_caches` (lines 1202-1209). Replace:

```cpp
void Tree::debug_clear_all_caches() {
  std::vector<Node*> leaves;
  collect_leaves(root_, leaves);
  for (Node* leaf : leaves) {
    if (leaf->cache) leaf->cache->clear();
  }
  if (root_->cache) root_->cache->clear();
}
```

with:

```cpp
void Tree::debug_clear_all_caches() {
  std::vector<Node*> leaves;
  collect_leaves(root_, leaves);
  for (Node* leaf : leaves) {
    if (leaf->cache_A) leaf->cache_A->clear();
    if (leaf->cache_B) leaf->cache_B->clear();
  }
}
```

- [ ] **Step 5: Update debug_some_keys_in_leaf_cache**

Find `debug_some_keys_in_leaf_cache` (lines 1220-1227). Replace:

```cpp
bool Tree::debug_some_keys_in_leaf_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (leaf->cache && leaf->cache->occupied_count() > 0) return true;
  }
  return false;
}
```

with:

```cpp
bool Tree::debug_some_keys_in_leaf_cache() const {
  std::vector<const Node*> leaves;
  collect_leaves(root_, leaves);
  for (const Node* leaf : leaves) {
    if (leaf->cache_B && leaf->cache_B->occupied_count() > 0) return true;
  }
  return false;
}
```

- [ ] **Step 6: Update debug_height3_nodes_have_no_cache**

Find `debug_height3_nodes_have_no_cache` (lines 1187-1200). Replace:

```cpp
bool Tree::debug_height3_nodes_have_no_cache() const {
  std::vector<const Node*> stack;
  stack.push_back(root_);
  while (!stack.empty()) {
    const Node* node = stack.back();
    stack.pop_back();
    if (!node) continue;
    if (node->height >= 3 && node->cache) return false;
    for (Node* child : node->children) {
      stack.push_back(child);
    }
  }
  return true;
}
```

with — now checking that NO internal node has cache_A or cache_B:

```cpp
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
```

- [ ] **Step 7: Delete debug_root_has_cache**

Remove the entire function (lines 1183-1185):
```cpp
bool Tree::debug_root_has_cache() const {
  return root_->cache != nullptr;
}
```

- [ ] **Step 8: Commit**

```bash
git add src/tree.cpp
git commit -m "refactor: update all debug hooks for dual-cache model

- DebugTwoLeaves: leaves get cache_A+cache_B, root has none
- debug_cache_a_contains: check all leaves' cache_A
- debug_clear_all_caches: clear both cache_A and cache_B
- debug_height3_nodes_have_no_cache: now checks height>=2
- Remove debug_root_has_cache entirely

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 11: Update tests

**Files:**
- Modify: `tests/test_tree_basic.cpp`
- Modify: `tests/test_tree_evict.cpp`
- Modify: `tests/test_tree_concurrent.cpp`

- [ ] **Step 1: Update test_tree_basic.cpp — remove debug_parent_cache_contains, update debug_root_has_cache references**

Search for and fix each test:

**TEST(TreeMultiLevel, DebugTwoLeaves)** (line 73-78): Replace `debug_parent_cache_contains(10)` with a check that works with the new model:

```cpp
TEST(TreeMultiLevel, DebugTwoLeaves) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  EXPECT_EQ(t->debug_height(), 2);
  // No parent cache — cache_A is on each leaf
  EXPECT_TRUE(t->debug_all_leaves_have_cache());
  EXPECT_TRUE(t->debug_height3_nodes_have_no_cache());
}
```

**TEST(TreeMultiLevel, PutToParentCache)** (line 80-85): Replace with test that verifies cache_A receives writes when p_parent=1.0:

```cpp
TEST(TreeMultiLevel, PutToCacheA) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(1.0, 0.0);
  ASSERT_EQ(t->put(10, 1), cbtree::Status::Ok);
  EXPECT_TRUE(t->debug_cache_a_contains(10));
}
```

**TEST(TreeMultiLevel, PutToLeafCache)** (line 87-93): Rename and update:

```cpp
TEST(TreeMultiLevel, PutToCacheB) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(0.0, 0.0);
  ASSERT_EQ(t->put(10, 1), cbtree::Status::Ok);
  // cache_A should NOT have the key
  EXPECT_FALSE(t->debug_cache_a_contains(10));
}
```

**TEST(TreeMultiLevel, GetChecksParentCache)** (line 105-113): Rename to check cache_A:

```cpp
TEST(TreeMultiLevel, GetChecksCacheA) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(1.0, 0.0);
  ASSERT_EQ(t->put(42, 420), cbtree::Status::Ok);
  auto r = t->get(42);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 420u);
}
```

**TEST(TreeMultiLevel, GetFromLeafCache)** (line 115-125): Update assertion:

```cpp
TEST(TreeMultiLevel, GetFromCacheB) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t->set_probabilities(0.0, 0.0);
  ASSERT_EQ(t->put(7, 77), cbtree::Status::Ok);
  EXPECT_FALSE(t->debug_cache_a_contains(7));
  auto r = t->get(7);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 77u);
}
```

- [ ] **Step 2: Update test_tree_evict.cpp — parent cache tests → cache_A tests**

**TEST(TreeEvictTest, ParentDemotesToLeaf)** (line 17-27): Rename and update:

```cpp
TEST_F(TreeEvictTest, CacheADemotesToCacheB) {
  auto t = cbtree::Tree::DebugTwoLeaves(path_);
  t->set_probabilities(1.0, 0.0);
  // Fill cache_A via puts (p_parent=1.0 routes writes to cache_A)
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(t->put(1000 + i, i), cbtree::Status::Ok);
  }
  // One more put triggers cache_A → cache_B demotion
  ASSERT_EQ(t->put(2000, 1), cbtree::Status::Ok);
  EXPECT_TRUE(t->debug_some_keys_in_leaf_cache());
}
```

**TEST(TreeEvictTest, LeafDirtyFlushesToSsd)** (line 29-47): Change `t.put` to use p_parent=0.0 (already does) — this test should work as-is since cache_B is used for default writes. No changes needed.

**TEST(TreeEvictTest, FlushRegistersLeafIndex)** (line 49-60): No changes needed.

- [ ] **Step 3: test_tree_concurrent.cpp — no changes needed**

The concurrent tests don't reference parent caches directly. They use `put/get` which work through the new dual-cache model automatically. No changes needed.

- [ ] **Step 4: Commit**

```bash
git add tests/test_tree_basic.cpp tests/test_tree_evict.cpp
git commit -m "refactor: update tests for dual-cache model

- test_tree_basic: replace parent cache tests with cache_A/cache_B variants
- test_tree_evict: rename ParentDemotesToLeaf -> CacheADemotesToCacheB
- test_tree_concurrent: no changes needed

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Task 12: Build, fix compilation errors, run tests

**Files:**
- All modified files

- [ ] **Step 1: Create build directory and compile**

```bash
mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Debug && make -j$(nproc)
```

Expected: compilation errors from any remaining `->cache` references that were missed.

- [ ] **Step 2: Fix any compilation errors**

Search for remaining `->cache` references that should be `->cache_A` or `->cache_B`:

```bash
grep -n '\->cache[^_]' src/tree.cpp include/cbtree/*.hpp tests/*.cpp | grep -v 'cache_A' | grep -v 'cache_B' | grep -v 'cache_attachment'
```

Fix each remaining instance based on context.

- [ ] **Step 3: Rebuild until clean**

```bash
cd build && make -j$(nproc) 2>&1 | head -50
```

Repeat until zero compilation errors.

- [ ] **Step 4: Run the test suite**

```bash
cd build && ctest --output-on-failure
```

- [ ] **Step 5: Fix test failures**

Analyze each failing test. Common failure modes:
- Test references `debug_parent_cache_contains` → change to `debug_cache_a_contains`
- Test references `debug_root_has_cache` → remove or check that internal nodes have no cache
- Split-related tests: verify new right leaf has both caches
- Scan tests: authority merge now goes cache_A (0) > cache_B (1) > chunks (2) > SSD (3)

- [ ] **Step 6: Rebuild and re-test until green**

```bash
cd build && make -j$(nproc) && ctest --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 7: Final commit**

```bash
git add -A
git commit -m "fix: compilation fixes and test adjustments for dual-cache model

All tests pass with cache_A + cache_B on leaf nodes only.

Co-Authored-By: Claude <noreply@anthropic.com>"
```

---

### Post-Implementation Verification Checklist

- [ ] `make -j$(nproc)` compiles with zero warnings
- [ ] `ctest` — all tests pass
- [ ] `grep -rn '->cache[^_]' src/ include/ | grep -v 'cache_A' | grep -v 'cache_B' | grep -v 'cache_attachment'` returns empty
- [ ] `grep -rn 'debug_parent_cache_contains\|debug_root_has_cache' src/ include/ tests/` returns empty
- [ ] `grep -rn 'evict_parent_if_needed' src/ include/` returns empty
- [ ] Internal nodes at any height have `cache_A == nullptr && cache_B == nullptr`
- [ ] Leaf nodes always have both `cache_A != nullptr && cache_B != nullptr`
