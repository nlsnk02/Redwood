#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <utility>
#include "cbtree/types.hpp"

namespace cbtree {

// Max records per page: (kPageSize - sizeof(header)) / sizeof(Record)
// header = uint32_t count (4 bytes)
// Record = Key(8) + Value(8) = 16 bytes
// Max = (4096 - 4) / 16 = 255
inline constexpr int kMaxRecordsPerPage = (static_cast<int>(kPageSize) - 4) / 16;

class SsDPageStore {
 public:
  // When use_direct is true, the file is opened with O_DIRECT (bypassing the
  // OS page cache).  All I/O goes through a thread-local aligned bounce buffer
  // to satisfy O_DIRECT alignment requirements.  fdatasync is never called —
  // data is written directly to the storage device.
  explicit SsDPageStore(const std::string& file_path, bool use_direct = false);
  ~SsDPageStore();

  SsDPageStore(const SsDPageStore&) = delete;
  SsDPageStore& operator=(const SsDPageStore&) = delete;

  // Page-level I/O — per-page locking:
  //   read_page  → shared lock (concurrent reads)
  //   write_page → exclusive lock (one writer per page)
  PageId alloc_page();
  Status write_page(PageId id, const std::array<std::byte, kPageSize>& buf);
  Status read_page(PageId id, std::array<std::byte, kPageSize>& buf);

  // No-op in direct I/O mode; kept for API compatibility.
  Status sync();

  // In-page KV operations
  Status put_record(PageId id, Key key, Value value);
  LookupResult get_record(PageId id, Key key);

  // Batch write: apply all entries to one page with a single read + write.
  // Entries that don't fit (page full) are appended to |overflow|.
  // Called from flush_leaf to amortise DIO read/write overhead.
  Status write_page_entries(PageId id,
                            const std::vector<std::pair<Key, Value>>& entries,
                            std::vector<std::pair<Key, Value>>& overflow);

  Status dump_sorted(PageId id, std::vector<std::pair<Key, Value>>* out);
  Status split_page(PageId left_id, Key mid, PageId* new_right_id);
  // Split using pre-loaded entries — skips the internal read_page.
  // Caller must hold tree_mutex_ to serialise structural changes.
  Status split_page(PageId left_id, Key mid,
                    const std::vector<std::pair<Key, Value>>& entries,
                    PageId* new_right_id);

 private:
  // Internal I/O helpers — caller must hold the appropriate page_lock.
  Status read_page_locked(PageId id, std::array<std::byte, kPageSize>& buf);
  Status write_page_locked(PageId id, const std::array<std::byte, kPageSize>& buf);

  // Per-page shared_mutex: 64 stripes, indexed by (page_id % 64).
  // Readers take shared_lock; writers take unique_lock.
  static constexpr size_t kPageLockCount = 64;
  mutable std::shared_mutex page_locks_[kPageLockCount];

  std::shared_mutex& page_lock(PageId id) const {
    return page_locks_[id % kPageLockCount];
  }

  // Serializes alloc_page() — separate from page I/O locks.
  std::mutex alloc_mutex_;

  int fd_;
  bool use_direct_;

  // DIO bounce buffer — thread_local since per-page locking allows
  // concurrent I/O from multiple threads.
  static thread_local std::array<std::byte, kPageSize> tl_dio_buf_;
};

}  // namespace cbtree
