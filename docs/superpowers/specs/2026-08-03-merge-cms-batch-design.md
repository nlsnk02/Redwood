# Merge feat/count-min-sketch-admission × feat/batch-merge-flush

Date: 2026-08-03 | Branch: `feat/merge-cms-batch`

## Goal

Create a new feature branch that merges the best features from both `feat/count-min-sketch-admission` (CMS) and `feat/batch-merge-flush` (Batch), both based on master `b2e467e`.

## Features to Merge (9 items)

### 1. Remove in-memory leaf key index (Batch)

**Files**: `include/cbtree/node.hpp`, `include/cbtree/tree.hpp`, `src/tree.cpp`

- Remove `Node::leaf_keys`, `Node::leaf_page_ids`, `Node::leaf_index_mutex` from `node.hpp`
- Remove `Tree::register_in_leaf_index()` from `tree.hpp` and `tree.cpp`
- Replace all `leaf_keys`-based checks with `ssd_->dump_sorted()` queries
- `debug_leaf_index_empty()` returns `true` unconditionally

### 2. Batch-merge flush_leaf (Batch, adapted)

**Files**: `src/tree.cpp`

- Use Batch's `flush_batch_merged` lambda (does NOT call `register_in_leaf_index`)
- Keeps Batch's `flush_batch_remote` for remote overflow entries
- Per-leaf `flush_mutex_` instead of global `flush_mutex_`
- Per-leaf chunk count thresholds (`dirty > 16 || total > 20`)

### 3. Simplified get() placeholder routing (CMS)

**Files**: `src/tree.cpp`

- Hot keys (CMS freq >= threshold) → cache_A only, evict if full, no fallback
- Cold keys → cache_B with `p_placeholder_` probability, evict if full, no fallback
- No cross-cache fallback between cache_A and cache_B

### 4. Clean chunk read-buffer + two-phase eviction (Batch)

**Files**: `src/tree.cpp`, `include/cbtree/chunk.hpp`, `src/cache_attachment.cpp`, `include/cbtree/cache_attachment.hpp`

- `EvictChunk::is_clean_only` flag — clean chunks need no I/O on flush
- `EvictChunk::Entry::is_absent` — marks negative-cache entries
- `Node::dirty_chunk_count_` — separate tracking for dirty chunks
- Two-phase `evict_to_chunk()`:
  - Phase 1: dirty entries → dirty chunk (SSD safety net)
  - Phase 2: if still near capacity → clean entries → clean chunk (read buffer)
- `collect_clean_clock()` — CLOCK-walk collecting clean entries without modifying slots
- `flush_dirty()` — CAS on dirty flag, winner captures key+value
- Clean chunk compaction on flush: dedup, CMS frequency scoring, keep top K
- Chunk-chain lookups handle `is_absent` entries (NotFound for point lookups, skip for scans)

### 5. Lock-free CacheAttachment + live_count (Batch + CMS)

**Files**: `src/cache_attachment.cpp`, `include/cbtree/cache_attachment.hpp`

- Remove per-slot `std::mutex` (CacheSlot ~80B → 32B)
- Seqlock protocol: `seqlock_read()`, CAS insert/update, generation-based ABA protection
- `cas_insert()` — CAS Empty/Tombstone → Occupied, writes data fields only on success
- `cas_update_in_place()` — in-place update with re-verification
- `KeyLockTable` (atomic bitmap) for key-level mutual exclusion
- Add `live_count_` atomic counter (Occupied + Placeholder) from CMS:
  - `fetch_add(1)` in upsert, mark_absent, try_place_placeholder, rehash, split_into
  - `fetch_sub(1)` in pick_clock_victim, evict_slot, evict_clean_slot, clear_clean_occupied
  - `live_count()` accessor
- CLOCK eviction includes Placeholder as a valid victim type (CMS change)

### 6. KeyLockTable atomic bitmap (Batch)

**Files**: `include/cbtree/key_lock_table.hpp`

- Replace 64×`std::mutex` (2560B) with single `std::atomic<uint64_t>` bitmap (8B)
- CAS spin-lock on per-stripe bit, `fetch_and` unlock
- Busy-wait is cheaper than kernel transition for microsecond-scale critical sections

### 7. SsDPageStore::IoStats + split_page overload (Batch)

**Files**: `include/cbtree/ssd_page_store.hpp`, `src/ssd_page_store.cpp`

- `IoStats` struct with 5 atomic counters: reads, writes, dump_sorted, splits, write_entries
- `split_page(pid, mid, entries, &new_right_id)` — uses pre-loaded entries, skips internal read
- `io_stats()`, `reset_io_stats()` accessors
- `Tree::io_stats()` delegation from `tree.hpp`

### 8. EvictDebugCounters (CMS)

**Files**: `include/cbtree/tree.hpp`, `src/tree.cpp`

- `EvictDebugCounters` struct: evict_a_calls, evict_a_actual, evict_b_calls, evict_b_actual
- Four `std::atomic<uint64_t>` members in Tree
- Incremented in `get()` placeholder placement path when eviction is attempted
- NOT the Batch compat stub — uses CMS real implementation

### 9. p_placeholder default = 0.75 (CMS)

**Files**: `include/cbtree/types.hpp`

- `kDefaultPPlaceholder` changed from `1.0` to `0.75`
- Reduces cold-key pollution in cache_B

## Excluded

- Batch branch compat stubs for `EvictDebugCounters` (use CMS real impl)
- CMS branch `register_in_leaf_index` calls (removed per #1)
- `leaf_keys`-based `split_leaf` median selection (use Batch's `dump_sorted` approach)
- Temporary test/result files from either branch

## Merge Strategy

Base the new branch on master `b2e467e`, then apply changes in dependency order:

1. `types.hpp` (p_placeholder default) — independent constants change
2. `chunk.hpp` (is_clean_only, is_absent) — struct change, no logic
3. `key_lock_table.hpp` — standalone replacement
4. `node.hpp` (remove leaf index, add dirty_chunk_count, flush_mutex) — struct changes
5. `ssd_page_store.hpp/.cpp` (IoStats, split_page overload) — new API
6. `cache_attachment.hpp` (CacheSlot reorder, live_count, collect_clean_clock, flush_dirty decl)
7. `tree.hpp` (remove register_in_leaf_index, add EvictDebugCounters, debug_leaf_count, io_stats, live_count atomic members)
8. `cache_attachment.cpp` — full lock-free rewrite + live_count integration
9. `tree.cpp` — all functional logic: flush_leaf, evict_to_chunk, get(), split_leaf, flush_and_split_leaf

## Key Test Scenarios

After merge, verify with benchmark: DIO, 3 threads, 3 rounds, workloads A/B/C, 10k records. Compare against original CMS and Batch results.
