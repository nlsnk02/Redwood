# Leaf-Only Cache: Parent Cache Moved into Leaf Nodes

**Date:** 2026-07-22  
**Branch:** `refactor/leaf-only-cache`  
**Status:** Approved

---

## 1. Motivation

**Current architecture:** Internal nodes at height=2 carry a `CacheAttachment` ("parent cache") that intercepts reads/writes before descending to the leaf. This creates a two-level caching hierarchy:

```
Internal Node (height=2):  CacheAttachment (64 slot, "parent cache")
Leaf Node (height=1):      CacheAttachment (64 slot, "leaf cache")
Eviction chain:  parent → leaf → chunk → SSD
```

Problems:
- `get()` and `put()` both have top-down split logic: check root/parent cache, then descend. This adds branching complexity on every hot-path operation.
- `scan()` must collect from parent cache separately, tracking processed parents with a `std::set<CacheAttachment*>` to avoid double-counting.
- `evict_parent_if_needed()` must re-descend (`find_leaf_for_key`) to push dirty entries down — an O(height) cost paid during eviction.
- During `split_internal` at heights ≥3, cache entries must be pushed down to children via separate tree traversal.

**Goal:** Eliminate all caching from internal nodes. All cache resides exclusively in leaf nodes. Each leaf gets two cache instances — one for hot/global keys (the former parent-cache semantic) and one for local keys.

---

## 2. Architecture

```
BEFORE (current):
  Internal Node (height=2):   CacheAttachment (64 slot)
  Leaf Node (height=1):        CacheAttachment (64 slot)

AFTER (new):
  Internal Node (any height):  NO CACHE (cache_A=null, cache_B=null)
  Leaf Node (height=1):        cache_A (64 slot, "hot cache" — former parent cache)
                               cache_B (64 slot, "local cache" — former leaf cache)
```

### 2.1 Cache Semantics

| Cache | Size | Write target | Evict target | Authority |
|-------|------|-------------|--------------|-----------|
| `cache_A` (hot) | 64 slots | `p_parent` probability, + promotion | `cache_B` (demotion) | 0 (highest) |
| `cache_B` (local) | 64 slots | default write target | chunk → SSD | 1 |

### 2.2 Eviction Chain

```
cache_A full (occupied > 51):
  → CLOCK victim from cache_A
  → if dirty: upsert into cache_B (same leaf, no re-descent needed)
  → if cache_B full: evict_to_chunk(cache_B) first
  → evict victim slot from cache_A

cache_B full (occupied > 51):
  → evict_to_chunk (unchanged from current code)
  → Chunk pipeline → flush_leaf → SSD
```

### 2.3 Authority Order (scan/merge)

All data sources live under the leaf now. For scan:

```
cache_A (authority 0) > cache_B (authority 1) > chunks (2) > SSD (3)
```

No global parent cache to track — each leaf's authority chain is self-contained.

---

## 3. Write Path (put)

```
put(key, value):

1. Descend to leaf (B-link, lock-free).  // always descend — no parent cache shortcut

2. Check leaf->cache_A:
   - key exists (Occupied or Absent): upsert in cache_A,
     evict_cache_A_if_needed(leaf), return
   - key not found: with probability p_parent, insert into cache_A, return

3. upsert into leaf->cache_B

4. evict_cache_B_if_needed(leaf)
```

Key change: Phase 1 (parent cache check under `shared_lock`) is removed entirely. `put()` always descends first. The `p_parent` probability check happens at the leaf level for `cache_A`.

---

## 4. Read Path (get)

```
get(key):

1. Descend to leaf (B-link, lock-free).  // always descend

2. Check leaf->cache_A → hit return / absent return NotFound
3. Check leaf->cache_B → hit return / absent return NotFound

4. Check chunk chains (leaf's chain + prev_sibling)
5. Optional: place placeholder (p_placeholder) in cache_B
6. SSD query
7. Post-SSD cache recheck (cache_B only)
8. Version check → retry if changed
```

Key change: No root/parent cache probing before descent. All cache lookups happen at the leaf after descent. The top-down "check root → check parent → descend → check leaf" pattern collapses to "descend → check A → check B".

---

## 5. Scan Path

```
scan(lo, hi):

1. Collect leaves overlapping [lo, hi] via B-link sibling traversal
2. For each leaf:
   a. Collect from cache_A (authority 0)
   b. Collect from cache_B (authority 1)
   c. Collect absent from both (authority -1, skipped)
3. Collect from chunk chains (authority 2)
4. Collect from SSD pages (authority 3)
5. Merge: highest-authority value per key wins
6. Filter absent, sort, return
```

Key change: No `std::set<CacheAttachment*> processed_parents` tracking. No separate parent cache sweep. Each leaf's scan is fully self-contained.

---

## 6. Split Behavior

### 6.1 Leaf Split (`split_leaf`)

```
leaf split at mid:
  right_leaf = new Node (height=1)
  right_leaf->cache_A = new CacheAttachment()
  right_leaf->cache_B = new CacheAttachment()

  cache_A->split_into(mid, right_leaf->cache_A)
  cache_B->split_into(mid, right_leaf->cache_B)

  // B-link protocol unchanged
  // leaf_keys/page_ids partition unchanged
  // parent update unchanged
```

### 6.2 Internal Split (`split_internal`)

```
internal node split at mid:
  // NO cache push-down (height >= 2 has no cache)
  // NO cache creation on new node
  // NO split_into for caches

  // Only: separator/children partition, B-link, parent update
```

### 6.3 New Root

```
new root creation (any height):
  new_root->height = old_height + 1
  // NO cache creation regardless of height
  // separators + children setup unchanged
```

---

## 7. evict_cache_A_if_needed (New Function)

Replaces the old `evict_parent_if_needed`. Key difference: no `find_leaf_for_key` re-descent required.

```
evict_cache_A_if_needed(leaf):

  if cache_A->occupied <= 51: return Ok

  victim_key, victim_val, victim_dirty, victim_idx, victim_gen
    = cache_A->find_clock_victim()

  if victim_dirty:
    // Ensure cache_B has room first
    evict_cache_B_if_needed(leaf)
    cache_B->upsert(victim_key, victim_val)

  cache_A->evict_slot(victim_idx, victim_key, victim_gen)
  return Ok
```

---

## 8. Node Structure Changes

```cpp
// BEFORE:
struct Node {
  // ...
  std::unique_ptr<CacheAttachment> cache;  // present at height 1 or 2
  // ...
};

// AFTER:
struct Node {
  // ...
  // Two caches on leaf nodes; both nullptr on internal nodes.
  std::unique_ptr<CacheAttachment> cache_A;  // hot cache (former parent)
  std::unique_ptr<CacheAttachment> cache_B;  // local cache (former leaf)
  // ...
};
```

Mounting rule:
- `height == 1` (leaf): `cache_A` and `cache_B` both present
- `height >= 2` (internal): both nullptr

---

## 9. Files Affected

| File | Change Scope | Description |
|------|-------------|-------------|
| `include/cbtree/node.hpp` | Small | Replace `cache` with `cache_A` + `cache_B` |
| `include/cbtree/tree.hpp` | Medium | Remove `evict_parent_if_needed`; add `evict_cache_A_if_needed`, `evict_cache_B_if_needed`; remove `debug_parent_cache_contains`; update other debug signatures |
| `src/tree.cpp` | Large | All `->cache` → `->cache_A` or `->cache_B`; rewrite `put`, `get`, `scan`, `split_leaf`, `split_internal`, `debug_flush_all`, `DebugTwoLeaves`, all debug hooks |
| `src/cache_attachment.cpp` | None | No changes needed |
| `src/node.cpp` | None | No changes needed |
| `tests/test_tree_basic.cpp` | Medium | Tests using `debug_parent_cache_contains` and parent-cache-specific scenarios need rewriting |
| `tests/test_tree_concurrent.cpp` | Small | Minimal changes |
| `tests/test_tree_evict.cpp` | Medium | Parent cache eviction tests need rewriting for `cache_A → cache_B` chain |

### Unchanged Files

| File | Reason |
|------|--------|
| `include/cbtree/cache_attachment.hpp` | CacheAttachment interface unchanged |
| `include/cbtree/chunk.hpp` | EvictChunk unchanged |
| `include/cbtree/types.hpp` | All types and constants unchanged |
| `include/cbtree/ssd_page_store.hpp` | SSD layer unaffected |
| `include/cbtree/fingerprint.hpp` | Unchanged |
| `include/cbtree/key_lock_table.hpp` | Unchanged |
| `include/cbtree/adaptive_policy.hpp` | Unchanged |
| `include/cbtree/wal_sink.hpp` | Stub, unchanged |
| `include/cbtree/delete_ops.hpp` | Stub, unchanged |
| `src/cache_attachment.cpp` | CacheAttachment methods unchanged |
| `src/ssd_page_store.cpp` | SSD I/O unchanged |
| `src/fingerprint.cpp` | Unchanged |
| `src/key_lock_table.cpp` | Unchanged |

---

## 10. Debug Hooks

### Removed
- `debug_parent_cache_contains(Key)` — no parent cache to check
- `debug_root_has_cache()` — root never has cache when height ≥ 2

### Modified
- `debug_clear_all_caches()` — clears all leaves' cache_A + cache_B
- `debug_all_leaves_have_cache()` — checks cache_A and cache_B
- `debug_height3_nodes_have_no_cache()` — now a tautology (no internal nodes have cache); may be removed or simplified

### Added (optional)
- `debug_leaf_cache_a_count(Node*)` — count of occupied slots in cache_A
- `debug_leaf_cache_b_count(Node*)` — count of occupied slots in cache_B

---

## 11. Constants

No new constants needed. Existing constants reused:
- `kCacheSlots = 64` — applies to both cache_A and cache_B independently
- `kLeafFillThreshold = 0.8` — triggers `evict_cache_B_if_needed`
- `kParentFillThreshold = 0.8` — repurposed for `evict_cache_A_if_needed`
- `kDefaultPParent = 0.1` — probability of writing to cache_A

---

## 12. Risks and Mitigations

| Risk | Severity | Mitigation |
|------|----------|------------|
| Concurrent `cache_A` eviction to `cache_B` + concurrent `cache_B` eviction to chunk | Medium | `eviction_mutex` already serializes per-leaf eviction; can be reused for both A and B, or separate mutexes if contention is high |
| `cache_B` full when `cache_A` needs to demote | Medium | `evict_cache_A_if_needed` first triggers `evict_cache_B_if_needed` to free space before upserting |
| Test breakage from removed parent cache semantics | Low | Tests will be rewritten to validate new cache_A / cache_B semantics |
| Performance: always descend cost | Low | Descent is already required for everything except p_parent=10% of puts; the B-link descent is lock-free and cheap |

---

## 13. Test Strategy

### Unit Tests
- `cache_A` and `cache_B` both respond to upsert/lookup on the same leaf
- Key in `cache_A` takes authority over `cache_B` in scan
- `evict_cache_A_if_needed` correctly demotes to `cache_B`
- `evict_cache_B_if_needed` correctly creates chunk → flush → SSD
- A→B→chunk cascade when both caches are full

### Integration Tests
- put/get with p_parent=0.0 (all to cache_B)
- put/get with p_parent=1.0 (all to cache_A)
- Range scan correctly merges cache_A, cache_B, chunks, SSD
- Leaf split correctly splits both cache_A and cache_B
- Height ≥ 3 trees: verify no cache on internal nodes
- Concurrent put/get stress on same leaf (cache_A and cache_B slots)

### Regression Tests
- Evict-to-SSD roundtrip (data survives cache flush)
- Chunk chain lookup after cache eviction
- B-link descent correctness under splits
- Placeholder mechanism still works

---

## 14. Implementation Order

1. **`node.hpp`** — replace `cache` with `cache_A` + `cache_B`
2. **`tree.hpp`** — update function signatures
3. **`tree.cpp`** — core implementation (largest change):
   a. Constructor, destructor
   b. `evict_cache_A_if_needed` / `evict_cache_B_if_needed` (new)
   c. `evict_to_chunk` (cache → cache_B)
   d. `put` (rewrite)
   e. `get` (rewrite)
   f. `scan` (rewrite)
   g. `split_leaf` (dual cache)
   h. `split_internal` (no cache)
   i. `debug_flush_all` (dual cache, no parent)
   j. `DebugTwoLeaves` (dual cache)
   k. All debug hooks
4. **Tests** — rewrite affected tests
5. **Build and verify** — compile, run test suite, fix regressions
