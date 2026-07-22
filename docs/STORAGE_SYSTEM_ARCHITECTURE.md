# Cache-Augmented B+ Tree Storage System — Architecture Overview

**Generated:** 2026-07-22
**Based on:** source code analysis of `cbtree` project
**Design spec:** `docs/superpowers/specs/2026-07-20-cache-augmented-btree-design.md`

---

## 1. Project Overview

**Language:** C++20
**Build:** CMake (gtest fetching via FetchContent)
**Library:** `cbtree` — a cache-augmented B+ tree storage engine
**Namespace:** `cbtree`

### Core Idea

A B+ tree where:
- Internal nodes are memory-resident
- Leaf nodes store `key → SSD page address` mappings (not full values)
- Small per-node caches (`CacheAttachment`) are mounted on **leaves** (height=1) and **leaf parents** (height=2) for hotspot acceleration
- Fingerprint-based pre-filtering accelerates point lookups
- CLOCK algorithm manages cache eviction
- Chunk-based eviction pipeline: dirty cache entries → per-leaf chunk chains → batch SSD write

---

## 2. Module Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                           Tree                                   │
│  Routing, splitting, version protocol, put/get/scan orchestration│
├─────────────────────────────────────────────────────────────────┤
│                        Node                                      │
│  B-link fields, leaf index, chunk chain, CacheAttachment pointer │
├───────────────┬──────────────────┬───────────────────────────────┤
│CacheAttachment│  SsDPageStore    │         EvictChunk            │
│ 64-slot cache │  Direct I/O SSD  │  Lock-free chunk chain        │
│ fingerprint   │  fixed-size pages│  per-leaf, newest-at-head      │
│ CLOCK eviction│  4KB, 255 records│  reader-safe during flush      │
├───────────────┴──────────────────┴───────────────────────────────┤
│  KeyLockTable    Fingerprint    AdaptivePolicy   WalSink  DeleteOps│
│  striped mutex   splitmix64     stub (v1)        stub     stub   │
│  64 stripes      lower 16 bits  returns defaults  no-op    no-op │
└─────────────────────────────────────────────────────────────────┘
```

---

## 3. File Layout

### Headers (`include/cbtree/`)

| File | Description | Status |
|------|-------------|--------|
| `types.hpp` | Core types (Key, Value, PageId, Fingerprint), enums (SlotState, Status), constants | Complete |
| `node.hpp` | Node struct: version, height, parent, B-link fields, leaf index, cache, chunk chain | Complete |
| `tree.hpp` | Tree class: public API (put/get/scan), internal helpers, debug hooks | Complete |
| `cache_attachment.hpp` | CacheAttachment class: 64-slot cache with fingerprint, CLOCK, key locks | Complete |
| `ssd_page_store.hpp` | SsDPageStore class: direct I/O page store, fixed-size 4KB pages, 255 records/page | Complete |
| `chunk.hpp` | EvictChunk struct: lock-free chunk for eviction pipeline, per-leaf chain | Complete |
| `key_lock_table.hpp` | KeyLockTable: 64-stripe mutex array for per-key write locking | Complete |
| `fingerprint.hpp` | fingerprint() function: splitmix64 hash, lower 16 bits | Complete |
| `adaptive_policy.hpp` | AdaptivePolicy class: stub returning default probabilities | Stub (v1) |
| `wal_sink.hpp` | WalSink class: stub ARIES-style WAL interface | Stub (v1) |
| `delete_ops.hpp` | DeleteOps class: stub delete/merge/rebalance interface | Stub (v1) |

### Sources (`src/`)

| File | Description | Status |
|------|-------------|--------|
| `tree.cpp` | Core tree logic: put, get, scan, split, eviction, chunk flushing, debug helpers | Complete |
| `cache_attachment.cpp` | Cache operations: upsert, lookup, placeholder, CLOCK victim, split, sort | Complete |
| `ssd_page_store.cpp` | Direct I/O page I/O: alloc, read, write, put_record, get_record, split | Complete |
| `node.cpp` | Minimal — Node uses aggregate initialization | Complete |
| `fingerprint.cpp` | splitmix64 hash implementation | Complete |
| `key_lock_table.cpp` | Minimal — KeyLockTable is header-only | Complete |
| `adaptive_policy.cpp` | Placeholder stub | Stub |
| `wal_sink.cpp` | Placeholder stub | Stub |
| `delete_ops.cpp` | Placeholder stub | Stub |

### Tests (`tests/`)

| File | Tests |
|------|-------|
| `test_types_smoke.cpp` | Type system smoke tests |
| `test_fingerprint.cpp` | Fingerprint hash function tests |
| `test_key_lock_table.cpp` | Key lock table concurrency tests |
| `test_cache_attachment.cpp` | Cache slot operations, CLOCK, split tests |
| `test_ssd_page_store.cpp` | SSD page store I/O tests |
| `test_tree_basic.cpp` | Basic tree put/get operations |
| `test_tree_placeholder.cpp` | Placeholder mechanism tests |
| `test_tree_split.cpp` | Tree split (leaf + internal) tests |
| `test_tree_evict.cpp` | Eviction pipeline tests |
| `test_tree_range.cpp` | Range scan tests |
| `test_tree_concurrent.cpp` | Concurrent put/get stress tests |
| `test_stubs.cpp` | Stub component tests |

### Build Config

| File | Description |
|------|-------------|
| `CMakeLists.txt` | CMake build, googletest fetch, library + test targets |

### Docs

| File | Description |
|------|-------------|
| `docs/superpowers/specs/2026-07-20-cache-augmented-btree-design.md` | Design specification (the blueprint) |
| `docs/superpowers/plans/2026-07-20-cache-augmented-btree.md` | Implementation plan |

---

## 4. Core Types

```cpp
Key       = uint64_t    // 8-byte key
Value     = uint64_t    // 8-byte value
PageId    = uint64_t    // SSD page identifier
Fingerprint = uint16_t   // 16-bit hash for cache pre-filter

SlotState: Empty | Placeholder | Occupied | Absent
Status:    Ok | NotFound | NotImplemented | Retry | Full | Error
```

### Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `kCacheSlots` | 64 | Entries per CacheAttachment |
| `kLeafFanout` | 32 | Max keys per leaf before split |
| `kInternalFanout` | 32 | Max children per internal node |
| `kPageSize` | 4096 | SSD page size (bytes) |
| `kMaxRecordsPerPage` | 255 | (4096-4)/16, max records per SSD page |
| `kParentFillThreshold` | 0.8 | Eviction trigger: 80% parent cache full |
| `kLeafFillThreshold` | 0.8 | Eviction trigger: 80% leaf cache full |
| `kDefaultPParent` | 0.1 | Default probability of parent cache insertion |
| `kDefaultPPlaceholder` | 0.1 | Default probability of placeholder creation |

---

## 5. Node Structure

```
Node
├── version: atomic<uint64_t>          // even=stable, odd=structural change
├── height: int                        // 1=leaf, ≥2=internal
├── parent: Node*
│
├── [B-link fields — lock-free descent]
│   ├── high_key: Key                  // upper bound of node's range
│   ├── next_sibling: atomic<Node*>    // right sibling pointer
│   └── prev_sibling: atomic<Node*>    // left sibling (for chunk lookup)
│
├── [Leaf fields — height == 1]
│   ├── leaf_keys: vector<Key>         // ordered keys (populated on flush)
│   ├── leaf_page_ids: vector<PageId>  // SSD page per key
│   ├── page_id: PageId               // SSD page for this leaf
│   ├── leaf_index_mutex: mutex        // protects leaf_keys/page_ids
│   └── eviction_mutex: mutex          // serializes batch eviction
│
├── [Internal fields — height >= 2]
│   ├── separators: vector<Key>        // separator keys
│   └── children: vector<Node*>        // child pointers
│
├── cache: unique_ptr<CacheAttachment> // mounted at height 1 or 2; null for ≥3
│
└── [Chunk chain — per-leaf, lock-free]
    ├── chunk_head_: atomic<EvictChunk*>
    ├── chunk_count_: atomic<size_t>
    ├── chunk_readers_: atomic<int>    // RCU-style reader count
    └── flush_mutex_: mutex            // serializes SSD flush per leaf
```

### Cache Mounting by Height

| Height | CacheAttachment | Rationale |
|--------|----------------|-----------|
| 1 (leaf, including initial root) | Always present | Direct cache for hot reads/writes |
| 2 (leaf parent) | Always present | Secondary cache layer |
| ≥ 3 | nullptr (steady state) | Cache pushed down to children on split |

---

## 6. CacheAttachment — 64-Slot CPU Cache

Each slot holds:

```
CacheSlot
├── state: SlotState           // Empty | Placeholder | Occupied | Absent
├── key: Key
├── value: Value
├── fp: Fingerprint            // 16-bit hash for O(1) pre-filter
├── dirty: bool                // true = not yet persisted to SSD
├── clock_bit: atomic<bool>    // CLOCK reference bit
├── slot_mutex: mutex          // per-slot write lock
└── generation: atomic<uint32_t> // ABA detection counter
```

### Slot State Machine

```
EMPTY ──write insert / read placeholder──▶ OCCUPIED(value)  OR  PLACEHOLDER
  ▲                           │                         │
  │                           │                         ├─ write hits placeholder → OCCUPIED
  │                           │                         └─ read confirms absent → ABSENT
  │        CLOCK eviction      │
  └────────── EMPTY ◀─────────┴── ABSENT also evictable (negative cache is optional)
```

### Key Operations

| Operation | Description |
|-----------|-------------|
| `upsert(k, v)` | Insert or update; sets dirty=true, clock_bit=true |
| `lookup(k)` | Fingerprint pre-filter → full key check; sets clock_bit on hit |
| `has_absent(k)` | Check if key is marked absent (negative cache) |
| `mark_absent(k)` | Mark key as known-not-present |
| `try_place_placeholder(k, &idx)` | Reserve a slot during SSD read miss |
| `fill_placeholder(idx, v)` | Convert placeholder to Occupied with value |
| `fill_placeholder_absent(idx)` | Convert placeholder to Absent |
| `find_clock_victim(...)` | CLOCK scan → find victim, capture generation (ABA-safe) |
| `evict_slot(idx, key, gen)` | Clear slot only if key+generation match (ABA-safe) |
| `split_into(mid, right)` | Move keys ≥ mid from left to right cache during split |
| `flush_dirty(&out)` | Collect dirty entries, mark clean |
| `clear_clean_occupied()` | Clear non-dirty Occupied slots (safe after SSD write) |
| `evict_clean_slot(k)` | Clear one specific clean slot |
| `sort_and_set_flag()` | Sort slots by key, set sorted_flag for range scans |

### Key Lock Table

Each CacheAttachment has its own `KeyLockTable` with **64 stripes** of `std::mutex`, indexed by `key % 64`. This provides per-key write mutual exclusion within a single cache level.

---

## 7. SsDPageStore — Direct I/O SSD Layer

### Page Layout

```
[4-byte header: record_count] [Record 0] [Record 1] ... [Record N-1]
                              ↑ 16 bytes each: Key(8) + Value(8)
```

Max records per page: `(4096 - 4) / 16 = 255`

### Two I/O Modes

| Mode | `use_direct=false` | `use_direct=true` |
|------|-------------------|-------------------|
| File open flags | `O_RDWR \| O_CREAT` | `O_RDWR \| O_CREAT \| O_DIRECT` |
| Buffer | Direct to caller's buffer | Aligned bounce buffer (`dio_buf_`) |
| OS page cache | Yes | Bypassed |
| `fdatasync` | Not called | Not needed (direct to device) |

### Operations

| Operation | Description |
|-----------|-------------|
| `alloc_page()` | Extend file by one page, return new PageId |
| `write_page(id, buf)` | Write 4KB aligned buffer to SSD |
| `read_page(id, buf)` | Read 4KB from SSD |
| `put_record(id, k, v)` | Read → update/append → write (single page) |
| `get_record(id, k)` | Read page, linear scan for key |
| `write_page_entries(id, entries, overflow)` | Batch read+write one page; full pages → overflow |
| `dump_sorted(id, &out)` | Read all records, sort by key |
| `split_page(left_id, mid, &right_id)` | Partition page at mid, write both halves |

### Thread Safety

All public methods hold a `recursive_mutex` — one I/O at a time per store instance. This is acceptable because I/O is the bottleneck, not lock contention.

---

## 8. Eviction Pipeline — Chunk-Based

### Problem

Naive eviction: evict slot → write to SSD → clear slot. SSD writes (especially fsync) can take ~37ms. Holding locks during I/O kills concurrency.

### Solution: Three-Phase Chunk Pipeline

```
Phase 1 (LOCKED, ~microseconds):
  Collect dirty slots → create EvictChunk → push to leaf's lock-free chain

Phase 2 (UNLOCKED, I/O):
  flush_leaf: read all unflushed chunks → batch-write to SSD

Phase 3 (LOCKED, cleanup):
  Mark chunks as flushed → sweep from chain → wait for readers → free
```

### EvictChunk Structure

```
EvictChunk
├── page_id: PageId           // target SSD page
├── leaf: Node*               // owner leaf
├── num_entries: size_t       // number of active entries
├── entries[64]: Entry[]      // {key, value, fingerprint}
├── next: atomic<EvictChunk*> // lock-free singly-linked list
└── flushed: atomic<bool>     // set by flush thread after SSD write
```

### Chain Properties

- **Per-leaf chains** — no global CAS contention between leaves
- **Newest-at-head** — most recent entries checked first
- **Immutable after push** — writers don't modify existing chunks
- **Reader-safe** — RCU-style `chunk_readers_` counter; free only when reader count = 0
- **prev_sibling walk** — after a split, chunks for keys ≥ mid may be on the old leaf; readers check prev_sibling chain to find them

### Data Safety Guarantee

Readers that miss in cache will find data in the chunk chain — this is the **safety net** between cache eviction (slot cleared) and SSD persistence. No dirty data can be lost.

---

## 9. B-link Tree Protocol

### Lock-Free Descent

Readers descend without locks using the **Lehman & Yao (1981) B-link** protocol:

1. Follow separators to child pointer
2. If `search_key >= child->high_key`, follow `next_sibling` (right-link chase)
3. Right-links are set **before** updating parent separators or truncating `high_key`
4. This guarantees reachability even during concurrent splits

### Split Protocol (Leaf and Internal)

```
Step 1: Link siblings BEFORE updating high_key bounds
        new_node->next_sibling = old_node->next_sibling
        old_node->next_sibling = new_node
        (also set prev_sibling for backward chain walking)

Step 2: Update high_key bounds
        old_node->high_key = mid
        new_node->high_key = old_high

Step 3: Update parent (insert separator + child pointer)
```

### Version Protocol

- `version` is `atomic<uint64_t>`
- Even = stable; Odd = structural change in progress
- Structural change: `version += 1` (odd) → modify → `version += 1` (even)
- Optimistic readers: check version before and after; retry on mismatch

### Concurrency Primitive Summary

| Primitive | Scope | Purpose |
|-----------|-------|---------|
| `tree_mutex_` (shared_mutex) | Tree-wide | Serializes split_leaf/split_internal; readers take shared lock for cache ops |
| Node `version` | Per-node | Optimistic read validation |
| `slot_mutex` | Per cache slot | Serializes slot state changes |
| Key write lock | Per key, per cache level | Serializes same-key writes |
| `leaf_index_mutex` | Per leaf | Protects leaf_keys/leaf_page_ids vectors |
| `eviction_mutex` | Per leaf | Serializes eviction batch creation |
| `flush_mutex_` | Per leaf | Serializes SSD flush operations |

---

## 10. Read Path (get)

```
get(key):
  1. Check root cache (if height ≥ 2)
     → Hit: return value
     → Absent: return NotFound

  2. Check leaf parent cache (if height ≥ 3, root has no cache)
     → Same logic

  3. Descend to leaf (B-link, lock-free)
     → Check leaf cache
     → Hit: return value
     → Absent: return NotFound

  4. Check chunk chains (leaf's chain + prev_sibling chains)
     → Hit: return value from newest chunk entry

  5. Optional: place placeholder (probability P_placeholder) in leaf cache

  6. Query SSD page
     → Linear scan of up to 255 records

  7. Post-SSD cache recheck (handles concurrent writes)
     → Check leaf cache again
     → Fill placeholder with result or Absent

  8. Version check → if changed, retry (up to 64 times)

Authority order: parent cache > leaf cache > chunk chain > SSD
```

---

## 11. Write Path (put)

```
put(key, value):
  1. Check root cache (if height ≥ 2)
     → Key exists (Occupied or Absent): upsert in-place, evict if needed, return
     → Key not found: with probability P_parent, insert into root cache
       (avoids full descent for hot keys)

  2. Descend to leaf (B-link, lock-free)

  3. Try upsert in leaf cache (retry up to 256 times)
     → Ok: version check, evict if needed, return
     → Full: evict_to_chunk to free slots, retry

  4. If eviction fails, return Full

Key property: key write lock is held during upsert —
same-key concurrent writes are serialized.
```

---

## 12. Range Scan (scan)

```
scan(lo, hi):
  1. Find leaves overlapping [lo, hi] via B-link sibling traversal
     (descend to lo, follow next_sibling until beyond hi)

  2. Collect from 4 layers (merged by authority):
     a. Parent cache (authority 0, highest)
     b. Leaf cache  (authority 1)
     c. Chunk chains (authority 2)
     d. SSD pages   (authority 3, lowest)

  3. For each key: highest-authority value wins
     Parent > Leaf > Chunk > SSD

  4. Skip Absent entries; sort and return

Note: Range scan correctness is weaker than point lookups —
no global snapshot isolation. Concurrent writes during scan
may produce inconsistent results across retries.
```

---

## 13. Eviction Triggers

| Trigger | Threshold | Action |
|---------|-----------|--------|
| Leaf cache full | `occupied > 64 * 0.8 = 51` | `evict_to_chunk`: pack dirty → chunk chain → flush_leaf |
| Parent cache full | `occupied > 51` | `evict_parent_if_needed`: CLOCK victim → push down to leaf cache → cascade |
| Leaf index overflow | `leaf_keys > 32` | `split_leaf` (during flush) |
| Internal node overflow | `children > 32` | `split_internal` (after leaf/internal split) |

### Cascade Depth

At most **one level**: parent → leaf → SSD. A parent eviction may push to a leaf that then needs eviction, triggering leaf → SSD. No deeper cascading.

---

## 14. Split Behavior by Height

| Split at height | Cache handling |
|-----------------|----------------|
| 1 (leaf) | Both old and new leaf keep `CacheAttachment` (split by mid) |
| 2 (leaf parent) | Both old and new node keep `CacheAttachment` (split by mid) |
| ≥ 3 (internal) | Push cache entries down to children, set own cache to nullptr |
| New root (any) | If new root height < 3: create cache; else: no cache |

---

## 15. Adaptive Policy Hook (Stub)

The `AdaptivePolicy` is designed to adjust `P_parent` and `P_placeholder` based on runtime statistics:

```cpp
struct Stats {
    double parent_hit_rate;
    double leaf_hit_rate;
    double clock_eviction_rate;
    double ssd_io_count;
};

Probabilities AdaptivePolicy::update(const Stats&);
```

Currently returns constant defaults (`kDefaultPParent = 0.1`, `kDefaultPPlaceholder = 0.1`).

---

## 16. Stub Interfaces (v1)

| Component | Purpose | Status |
|-----------|---------|--------|
| `DeleteOps::remove(k)` | Delete a key | `NotImplemented` |
| `DeleteOps::try_merge(n)` | Merge underfull node | `NotImplemented` |
| `DeleteOps::rebalance(n)` | Rebalance after delete | `NotImplemented` |
| `WalSink::log_insert(k,v)` | WAL insert log | No-op (returns Ok) |
| `WalSink::log_update(k,old,new)` | WAL update log | No-op |
| `WalSink::log_compensate(k,v)` | WAL compensation log | No-op |
| `WalSink::checkpoint()` | WAL checkpoint | No-op |
| `WalSink::recover()` | WAL crash recovery | No-op |
| `AdaptivePolicy::update(s)` | Adapt probabilities | Returns defaults |

---

## 17. Memory Management

- **Node lifetime:** Tree destructor recursively deletes all children via `delete_subtree()`
- **Chunk lifetime:** Created by evicting thread; freed by flush thread after all readers have finished (RCU pattern with `chunk_readers_`)
- **CacheAttachment:** Owned by Node via `unique_ptr<CacheAttachment>`
- **SsDPageStore:** Owned by Tree via `unique_ptr<SsDPageStore>`
- **File descriptor:** Closed in `SsDPageStore` destructor
- **Move semantics:** Tree is non-copyable and non-movable (mutex/atomic members)

---

## 18. Key Design Decisions

1. **Per-leaf chunk chains** instead of global chain — eliminates CAS contention between leaves
2. **Chunk pipeline** separates locking (~microseconds) from I/O (~milliseconds) — enables concurrent writes during SSD flush
3. **B-link protocol** enables lock-free read descent — readers never block on splits
4. **Two-phase eviction with generation counter** — ABA-safe slot reuse detection
5. **Prev_sibling chain** — chunk lookup handles split races: chunks on old leaf remain reachable from new leaf
6. **Sorted flag** — cache sorted state is computed once and reused for multiple range queries until invalidated
7. **Authority order** (parent > leaf > chunk > SSD) — strictly enforced, no exceptions
8. **Negative caching (Absent)** — "key not found" is cached to avoid repeated SSD lookups
9. **Direct I/O option** — bypasses OS page cache for predictable SSD performance
10. **Per-cache KeyLockTable** — independent lock tables per cache level, not a global table
