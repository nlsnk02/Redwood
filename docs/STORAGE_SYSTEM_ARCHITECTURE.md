# Cache-Augmented B+ Tree Storage System — Architecture Overview

**Generated:** 2026-07-28
**Based on:** source code analysis of `cbtree` project (post leaf-only-cache refactoring, seqlock, open-addressing)

---

## 1. Project Overview

**Language:** C++20
**Build:** CMake (googletest via FetchContent)
**Library:** `cbtree` — a cache-augmented B+ tree (B-link) storage engine
**Namespace:** `cbtree`

### Core Idea

A B-link tree where:
- Internal nodes are memory-resident routing structures (no data, no cache)
- Leaf nodes store `key → SSD page address` mappings and carry **two** independent 64-slot caches:
  - `cache_A` (hot cache): authority 0, probabilistic insertion target (`p_parent`)
  - `cache_B` (local cache): authority 1, default write target
- Cache reads use **seqlock** — lock-free for the common hit path
- Cache lookup uses **open-addressing** with linear probing and tombstone compaction
- Fingerprint-based pre-filtering accelerates point lookups
- CLOCK algorithm manages cache eviction
- Chunk-based eviction pipeline: dirty cache entries → per-leaf chunk chains → batch SSD write
- Per-page `shared_mutex` (64 stripes) enables concurrent SSD I/O on different pages
- B-link protocol (Lehman & Yao 1981) enables lock-free read descent

---

## 2. Module Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                           Tree                                    │
│  B-link routing, split, version protocol, put/get/scan, eviction  │
│  Hit-rate tracking, deferred batch flush                          │
├───────────────────────────────────────────────────────────────────┤
│                        Node                                       │
│  B-link fields, leaf index, chunk chain, cache_A + cache_B ptrs   │
├──────────────┬───────────────────┬────────────────────────────────┤
│CacheAttachment│  SsDPageStore    │         EvictChunk              │
│ 64-slot cache│  Direct I/O SSD   │  Lock-free chunk chain          │
│ seqlock reads│  per-page mutex   │  per-leaf, newest-at-head       │
│ open-addressing 64 stripes      │  reader-safe during flush       │
│ CLOCK eviction 4KB, 255 rec/page │  immutable after push          │
├──────────────┴───────────────────┴────────────────────────────────┤
│ KeyLockTable    Fingerprint    AdaptivePolicy   WalSink  DeleteOps │
│ striped mutex   splitmix64     stub (v1)        stub     stub      │
│ 64 stripes      lower 16 bits  returns defaults  no-op    no-op    │
└───────────────────────────────────────────────────────────────────┘
```

---

## 3. File Layout

### Headers (`include/cbtree/`)

| File | Description | Status |
|------|-------------|--------|
| `types.hpp` | Core types (Key, Value, PageId, Fingerprint), enums (SlotState, Status), MemoryHitStats, constants | Complete |
| `node.hpp` | Node struct: version, height, parent, B-link fields, leaf index, cache_A, cache_B, chunk chain | Complete |
| `tree.hpp` | Tree class: put/get/scan, split, eviction, deferred flush, hit-rate tracking, debug hooks | Complete |
| `cache_attachment.hpp` | CacheAttachment class: 64-slot open-addressing cache with seqlock, CLOCK, tombstone/rehash | Complete |
| `ssd_page_store.hpp` | SsDPageStore class: direct I/O page store, per-page shared_mutex, 4KB pages, 255 rec/page | Complete |
| `chunk.hpp` | EvictChunk struct: lock-free chunk for eviction pipeline, per-leaf chain | Complete |
| `key_lock_table.hpp` | KeyLockTable: 64-stripe mutex array for per-key write locking + KeyLockGuard RAII helper | Complete |
| `fingerprint.hpp` | fingerprint() function: splitmix64 hash, lower 16 bits | Complete |
| `adaptive_policy.hpp` | AdaptivePolicy class: stub returning default probabilities | Stub (v1) |
| `wal_sink.hpp` | WalSink class: stub ARIES-style WAL interface | Stub (v1) |
| `delete_ops.hpp` | DeleteOps class: stub delete/merge/rebalance interface | Stub (v1) |

### Sources (`src/`)

| File | Description | Status |
|------|-------------|--------|
| `tree.cpp` | Core tree logic: put, get, scan, split, eviction, chunk flushing, deferred batch flush, hit-rate tracking | Complete |
| `cache_attachment.cpp` | Cache operations: seqlock lookup, upsert, placeholder, CLOCK victim, open-addressing rehash, split, sort | Complete |
| `ssd_page_store.cpp` | Per-page I/O with striped shared_mutex: alloc, read, write, put_record, get_record, split | Complete |
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
| `test_cache_attachment.cpp` | Cache slot operations, CLOCK, open-addressing, rehash tests |
| `test_ssd_page_store.cpp` | SSD page store I/O tests |
| `test_tree_basic.cpp` | Basic tree put/get operations (cache_A + cache_B) |
| `test_tree_placeholder.cpp` | Placeholder mechanism tests |
| `test_tree_split.cpp` | Tree split (leaf + internal) tests |
| `test_tree_evict.cpp` | Eviction pipeline tests (cache_A→cache_B→chunk→SSD cascade) |
| `test_tree_range.cpp` | Range scan tests |
| `test_tree_concurrent.cpp` | Concurrent put/get stress tests |
| `test_stubs.cpp` | Stub component tests |

---

## 4. Core Types

```cpp
Key         = uint64_t    // 8-byte key
Value       = uint64_t    // 8-byte value
PageId      = uint64_t    // SSD page identifier
Fingerprint = uint16_t    // 16-bit hash for cache pre-filter

SlotState: Empty | Placeholder | Occupied | Absent | Tombstone
Status:    Ok | NotFound | NotImplemented | Retry | Full | Error
```

### Constants

| Constant | Value | Meaning |
|----------|-------|---------|
| `kCacheSlots` | 64 | Entries per CacheAttachment (applies to both cache_A and cache_B) |
| `kLeafFanout` | 32 | Max keys per leaf before split |
| `kInternalFanout` | 32 | Max children per internal node |
| `kPageSize` | 4096 | SSD page size (bytes) |
| `kMaxRecordsPerPage` | 255 | (4096-4)/16, max records per SSD page |
| `kParentFillThreshold` | 0.8 | Eviction trigger for cache_A: 80% full |
| `kLeafFillThreshold` | 0.8 | Eviction trigger for cache_B: 80% full |
| `kDefaultPParent` | 0.23 | Default probability of cache_A insertion |
| `kDefaultPPlaceholder` | 1.0 | Default probability of placeholder creation |

### MemoryHitStats (hit-rate tracking)

```cpp
struct MemoryHitStats {
    uint64_t total_gets;    // total completed get() calls
    uint64_t memory_hits;   // served from cache/chunk/absent (no SSD I/O)
    uint64_t ssd_accesses;  // required at least one SSD read
};
```

---

## 5. Node Structure

```
Node
├── version: atomic<uint64_t>           // even=stable, odd=structural change
├── height: int                         // 1=leaf, ≥2=internal
├── parent: Node*
│
├── [B-link fields — lock-free descent]
│   ├── high_key: Key                   // upper bound of node's range (~Key{0} for rightmost)
│   ├── next_sibling: atomic<Node*>     // right sibling pointer
│   └── prev_sibling: atomic<Node*>     // left sibling (for chunk lookup after split)
│
├── [Leaf fields — height == 1]
│   ├── leaf_keys: vector<Key>          // ordered keys (populated on flush)
│   ├── leaf_page_ids: vector<PageId>   // SSD page per key
│   ├── page_id: PageId                 // SSD page for this leaf
│   ├── leaf_index_mutex: mutex         // protects leaf_keys/page_ids
│   └── eviction_mutex: mutex           // serializes batch eviction
│
├── [Internal fields — height >= 2]
│   ├── separators: vector<Key>         // separator keys
│   └── children: vector<Node*>         // child pointers
│
├── [Dual cache — height == 1 only; both nullptr on internal nodes]
│   ├── cache_A: unique_ptr<CacheAttachment>  // hot cache (authority 0)
│   └── cache_B: unique_ptr<CacheAttachment>  // local cache (authority 1)
│
└── [Chunk chain — per-leaf, lock-free]
    ├── chunk_head_: atomic<EvictChunk*>
    ├── chunk_count_: atomic<size_t>
    ├── chunk_readers_: atomic<int>     // RCU-style reader count
    └── flush_mutex_: mutex             // serializes SSD flush per leaf
```

### Cache Mounting Rule

- **height == 1 (leaf):** Both `cache_A` and `cache_B` always present
- **height >= 2 (internal):** Both are `nullptr` — no internal node ever holds a cache
- **Root node:** If height == 1 (empty/single-node tree), has both caches. If height >= 2, no cache.

---

## 6. CacheAttachment — 64-Slot CPU Cache (Open-Addressing + Seqlock)

### Slot Layout

```
CacheSlot
├── seq: atomic<uint32_t>      // Seqlock counter: writers increment before/after modify;
│                              //   readers snapshot before reading, retry if odd or changed
├── state: SlotState           // Empty | Placeholder | Occupied | Absent | Tombstone
├── key: Key
├── value: Value
├── fp: Fingerprint            // 16-bit hash for O(1) pre-filter
├── dirty: bool                // true = not yet persisted to SSD
├── clock_bit: atomic<bool>    // CLOCK reference bit
├── slot_mutex: mutex          // per-slot write lock (writers only; readers use seqlock)
└── generation: atomic<uint32_t> // ABA detection counter, incremented on reuse
```

### Seqlock Read Protocol

Cache reads (`lookup`, `has_absent`) are **lock-free**:
1. Snapshot `slot.seq` — abort if odd (writer active)
2. Read slot fields (key, value, fp, state)
3. Re-read `slot.seq` — retry if changed or odd
4. On fingerprint match, verify full key equality

Writers increment `seq` (odd→even) around modifications within `slot_mutex`. This eliminates ~20-40 futex lock/unlock pairs per `get()` call.

### Open-Addressing with Linear Probing

- Slots are probed linearly; tombstones are skipped for insertion but not for lookup
- `tombstone_count_` tracks tombstone accumulation
- `maybe_rehash()` triggers compaction when tombstones exceed threshold — O(64), rarely called
- `rehash()` compacts all live entries, removing tombstones

### Slot State Machine

```
EMPTY ──write insert / read placeholder──▶ OCCUPIED(value)  OR  PLACEHOLDER
  ▲                           │                         │
  │                           │                         ├─ write hits placeholder → OCCUPIED
  │                           │                         └─ read confirms absent → ABSENT
  │        CLOCK eviction      │
  └────────── EMPTY ◀─────────┴── ABSENT also evictable (negative cache is optional)
  TOMBSTONE: cleared slot in open-addressing chain (compacted by rehash)
```

### Key Operations

| Operation | Description |
|-----------|-------------|
| `upsert(k, v, known_new)` | Insert/update; `known_new=true` skips existing-key scan for open-addressing probe |
| `lookup(k)` | Seqlock read: fingerprint pre-filter → full key check; sets clock_bit on hit |
| `has_absent(k)` | Seqlock check if key is marked absent (negative cache) |
| `mark_absent(k)` | Mark key as known-not-present |
| `try_place_placeholder(k, &idx)` | Reserve a slot during SSD read miss |
| `fill_placeholder(idx, v)` | Convert placeholder to Occupied with value |
| `fill_placeholder_absent(idx)` | Convert placeholder to Absent |
| `find_clock_victim(...)` | CLOCK scan → find victim, capture generation (ABA-safe), two-phase |
| `evict_slot(idx, key, gen)` | Clear slot only if key+generation match (ABA-safe) |
| `split_into(mid, right)` | Move keys ≥ mid to right cache during leaf split |
| `flush_dirty(&out)` | Collect dirty entries, mark clean |
| `clear_clean_occupied()` | Clear non-dirty Occupied slots (safe after SSD write) |
| `evict_clean_slot(k)` | Clear one specific clean slot |
| `sort_and_set_flag()` | Sort slots by key, set sorted_flag for range scans |
| `rehash()` | Compact all entries, remove tombstones — O(64) |
| `maybe_rehash()` | Trigger rehash if tombstone_count_ exceeds threshold |

### Key Lock Table

Each CacheAttachment has its own `KeyLockTable` with **64 stripes** of `std::mutex`, indexed by `key % 64`. A `KeyLockGuard` RAII wrapper is provided. This is per-cache, not global.

---

## 7. SsDPageStore — Direct I/O SSD Layer with Per-Page Locking

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
| Buffer | Direct to caller's buffer | Thread-local aligned bounce buffer (`tl_dio_buf_`) |
| OS page cache | Yes | Bypassed |
| `fdatasync` | Not called | Not needed (direct to device) |

### Operations

| Operation | Description |
|-----------|-------------|
| `alloc_page()` | Extend file by one page, return new PageId (serialized by `alloc_mutex_`) |
| `write_page(id, buf)` | Write 4KB aligned buffer to SSD (exclusive lock) |
| `read_page(id, buf)` | Read 4KB from SSD (shared lock) |
| `put_record(id, k, v)` | Read → update/append → write (single page) |
| `get_record(id, k)` | Read page, linear scan for key |
| `write_page_entries(id, entries, overflow)` | Batch read+write one page; full pages → overflow |
| `dump_sorted(id, &out)` | Read all records, sort by key |
| `split_page(left_id, mid, &right_id)` | Partition page at mid, write both halves |

### Thread Safety

Per-page `shared_mutex` with **64 stripes** (`page_id % 64`):
- **read_page**: shared lock → concurrent reads on same or different pages
- **write_page / put_record**: exclusive lock → one writer per page stripe
- **alloc_page**: separate `alloc_mutex_` — serializes file extension only

This replaces the old global `recursive_mutex`, allowing concurrent I/O on different pages.

---

## 8. Eviction Pipeline — Chunk-Based (Three-Phase)

### Problem

Naive eviction: evict slot → write to SSD → clear slot. SSD writes (especially fsync) can take ~37ms. Holding locks during I/O kills concurrency.

### Solution: Three-Phase Chunk Pipeline

```
Phase 1 (LOCKED, ~microseconds):
  Collect dirty slots → create EvictChunk → CAS-push to leaf's lock-free chain

Phase 2 (UNLOCKED, I/O):
  flush_leaf: read all unflushed chunks → batch-write to SSD

Phase 3 (LOCKED, cleanup):
  Mark chunks as flushed → sweep from chain → wait for readers → free
```

### EvictChunk Structure

```
EvictChunk
├── page_id: PageId            // target SSD page
├── leaf: Node*                // owner leaf
├── num_entries: size_t        // number of active entries
├── entries[kMaxEntries]: Entry[]  // {key, value, fingerprint}, kMaxEntries = kCacheSlots (64)
├── next: atomic<EvictChunk*>  // lock-free singly-linked list
└── flushed: atomic<bool>      // set by flush thread after SSD write
```

### Chain Properties

- **Per-leaf chains** — no global CAS contention between leaves
- **Newest-at-head** — most recent entries checked first
- **Immutable after push** — writers don't modify existing chunks
- **Reader-safe** — RCU-style `chunk_readers_` counter; free only when reader count = 0
- **prev_sibling walk** — after a split, chunks for keys ≥ mid may be on the old leaf; readers check prev_sibling chain to find them

### Data Safety Guarantee

Readers that miss in cache will find data in the chunk chain — this is the **safety net** between cache eviction (slot cleared) and SSD persistence. No dirty data can be lost.

### Deferred Flush

The Tree maintains a global `total_chunk_count_` (atomic) and a configurable `flush_batch_threshold_` (default 1000). When total unflushed chunks across all leaves exceed the threshold, a batch flush is triggered. Peak chunk count (`peak_chunk_count_`) and chain length samples are tracked for diagnostics.

---

## 9. B-link Tree Protocol

### Lock-Free Descent

Readers descend without locks using the **Lehman & Yao (1981) B-link** protocol:

1. Follow separators to child pointer
2. If `search_key >= child->high_key`, follow `next_sibling` (right-link chase)
3. Right-links are set **before** updating parent separators or truncating `high_key`
4. This guarantees reachability even during concurrent splits

### Split Protocol

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
| `tree_mutex_` (shared_mutex) | Tree-wide | Serializes split_leaf/split_internal (exclusive); descend/find_leaf (shared) |
| Node `version` | Per-node | Optimistic read validation |
| `slot_mutex` | Per cache slot | Serializes slot state changes (writers); readers bypass via seqlock |
| `seq` (seqlock) | Per cache slot | Lock-free read protocol for lookup/has_absent |
| Key write lock | Per key, per cache level | Serializes same-key writes |
| `leaf_index_mutex` | Per leaf | Protects leaf_keys/leaf_page_ids vectors |
| `eviction_mutex` | Per leaf | Serializes eviction batch creation |
| `flush_mutex_` | Per leaf | Serializes SSD flush operations |
| `page_locks_[]` (shared_mutex × 64) | Per page stripe | Concurrent page I/O; shared for reads, exclusive for writes |
| `alloc_mutex_` | Store-wide | Serializes SSD page allocation |

---

## 10. Read Path (get)

```
get(key):

1. Descend to leaf (B-link, lock-free). Always descend — no parent cache shortcut.

2. Check leaf->cache_A (seqlock):
   → Occupied hit: record memory hit, return value
   → Absent hit: record memory hit, return NotFound

3. Check leaf->cache_B (seqlock):
   → Occupied hit: record memory hit, return value
   → Absent hit: record memory hit, return NotFound

4. Check chunk chains (leaf's chain + prev_sibling chains):
   → Hit: return value from newest matching chunk entry

5. Optional: place placeholder (probability p_placeholder) in cache_B

6. Query SSD page:
   → Linear scan of up to 255 records

7. Fill placeholder with result or Absent.
   If fill succeeds and the other cache also missed, return SSD result immediately.
   Only do full cache rechecks on race detection.

8. Version check → if changed, retry (up to 64 times)

Authority order (per leaf): cache_A (0) > cache_B (1) > chunks (2) > SSD (3)
```

---

## 11. Write Path (put)

```
put(key, value):

1. Descend to leaf (B-link, lock-free). Always descend.

2. Check leaf->cache_A:
   → Key exists (Occupied or Absent): upsert in cache_A,
     evict_cache_A_if_needed(leaf), return
   → Key not found: with probability p_parent, insert into cache_A, return

3. Upsert into leaf->cache_B (retry up to 256 times):
   → Ok: evict_cache_B_if_needed(leaf), return
   → Full: evict_to_chunk to free slots, retry

4. If eviction fails, return Full

Key property: key write lock is held during upsert — same-key concurrent writes are serialized.
```

---

## 12. Range Scan (scan)

```
scan(lo, hi):

1. Find leaves overlapping [lo, hi] via B-link sibling traversal
   (descend to lo, follow next_sibling until beyond hi)

2. For each leaf, collect from self-contained authority chain:
   a. cache_A (authority 0, highest)
   b. cache_B (authority 1)
   c. Chunk chains   (authority 2)
   d. SSD pages      (authority 3, lowest)

3. For each key: highest-authority value wins.
   cache_A > cache_B > Chunk > SSD

4. Skip Absent entries; sort and return

Note: No global parent cache to track — each leaf's authority chain is self-contained.
No `std::set<CacheAttachment*> processed_parents` needed.
Range scan correctness is weaker than point lookups — no global snapshot isolation.
```

---

## 13. Eviction Triggers and Cascade

| Trigger | Threshold | Action |
|---------|-----------|--------|
| cache_A full | `occupied > 64 * 0.8 = 51` | `evict_cache_A_if_needed`: CLOCK victim → demote to cache_B (same leaf, no re-descent) |
| cache_B full | `occupied > 51` | `evict_cache_B_if_needed` → `evict_to_chunk`: pack dirty → chunk chain → flush_leaf |
| Leaf index overflow | `leaf_keys > 32` | `split_leaf` (during flush) |
| Internal node overflow | `children > 32` | `split_internal` (no cache handling needed) |

### Cascade Depth

At most **two levels** within a single leaf: cache_A → cache_B → chunk → SSD. A cache_A eviction may demote to cache_B, which may then need eviction to chunk. No re-descent required — both caches are on the same leaf node. No deeper cascading.

---

## 14. Split Behavior

| Split at height | Cache handling |
|-----------------|----------------|
| 1 (leaf) | Both old and new leaf get brand-new `cache_A` + `cache_B`; entries split by mid |
| ≥ 2 (internal) | No cache handling — internal nodes never have cache. Only separators/children partitioned and B-link updated. |
| New root (any) | No cache created — new root height ≥ 2, no cache by rule |

### Leaf Split Detail

1. `version → odd`
2. Create right leaf with new `cache_A` + `cache_B`
3. `cache_A->split_into(mid, right_leaf->cache_A)`
4. `cache_B->split_into(mid, right_leaf->cache_B)`
5. Partition leaf_keys, leaf_page_ids, SSD page
6. B-link: link siblings, update high_key bounds
7. Update parent (insert separator)
8. `version → even`

### Internal Split Detail

1. `version → odd`
2. Partition separators and children at mid
3. B-link: link siblings, update high_key bounds
4. Update parent (insert separator) or create new root
5. No cache operations of any kind
6. `version → even`

---

## 15. Adaptive Policy Hook (Stub)

The `AdaptivePolicy` is designed to adjust `p_parent` and `p_placeholder` based on runtime statistics:

```cpp
struct Stats {
    double parent_hit_rate;
    double leaf_hit_rate;
    double clock_eviction_rate;
    double ssd_io_count;
};

Probabilities AdaptivePolicy::update(const Stats&);
```

Currently returns constant defaults (`p_parent = 0.23`, `p_placeholder = 1.0`).

---

## 16. Hit-Rate Tracking

The Tree class provides YCSB-compatible hit-rate statistics:

| Method | Description |
|--------|-------------|
| `memory_hit_stats()` | Snapshot of current counters (atomic reads — best-effort) |
| `memory_hit_rate()` | Fraction in [0.0, 1.0]; returns 0.0 when no get() calls recorded |
| `reset_memory_hit_stats()` | Zero all counters |
| `set_hit_tracking(bool)` | Enable/disable at runtime; when off, get() performs zero additional atomics |

Counters are `atomic<uint64_t>` updated by concurrent readers. A "memory hit" means the get() was served without touching SSD — from cache_A, cache_B, chunk chain, or absent marker.

---

## 17. Stub Interfaces (v1)

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

## 18. Memory Management

- **Node lifetime:** Tree destructor recursively deletes all children via `delete_subtree()`
- **Chunk lifetime:** Created by evicting thread; freed by flush thread after all readers have finished (RCU pattern with `chunk_readers_`)
- **CacheAttachment:** Owned by Node via `unique_ptr<CacheAttachment>` (two per leaf: cache_A, cache_B)
- **SsDPageStore:** Owned by Tree via `unique_ptr<SsDPageStore>`
- **File descriptor:** Closed in `SsDPageStore` destructor
- **Move semantics:** Tree is non-copyable and non-movable (mutex/atomic members)

---

## 19. Key Design Decisions

1. **Dual cache on leaf nodes only** — no internal node caching; eliminates top-down parent-cache shortcut complexity; always-descend simplifies put/get/scan
2. **Seqlock cache reads** — lock-free common path; writers signal via seq counter; eliminates ~20-40 mutex acquisitions per get()
3. **Open-addressing with tombstones** — linear probing for cache lookup; tombstone compaction via rehash
4. **Per-page shared_mutex (64 stripes)** — concurrent SSD reads on different pages; replaces global recursive_mutex bottleneck
5. **Per-leaf chunk chains** instead of global chain — eliminates CAS contention between leaves
6. **Chunk pipeline** separates locking (~microseconds) from I/O (~milliseconds) — enables concurrent writes during SSD flush
7. **B-link protocol** enables lock-free read descent — readers never block on splits
8. **Two-phase eviction with generation counter** — ABA-safe slot reuse detection
9. **Prev_sibling chain** — chunk lookup handles split races: chunks on old leaf remain reachable from new leaf
10. **Sorted flag** — cache sorted state is computed once and reused for multiple range queries until invalidated
11. **Authority order** (cache_A > cache_B > chunk > SSD) — strictly enforced per leaf, no exceptions
12. **Negative caching (Absent)** — "key not found" is cached to avoid repeated SSD lookups
13. **Direct I/O option** — bypasses OS page cache for predictable SSD performance; thread-local bounce buffer for alignment
14. **Per-cache KeyLockTable** — independent lock tables per cache level, not a global table
15. **Deferred batch flush** — configurable threshold for amortizing SSD writes across many chunks
16. **Hit-rate tracking** — atomic counters with runtime enable/disable; zero overhead when disabled
