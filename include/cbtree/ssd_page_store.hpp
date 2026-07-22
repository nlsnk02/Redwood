#pragma once
#include <array>
#include <cstdint>
#include <cstddef>
#include <mutex>
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
  // OS page cache).  All I/O goes through an aligned bounce buffer to satisfy
  // O_DIRECT alignment requirements.  fdatasync is never called — data is
  // written directly to the storage device.
  explicit SsDPageStore(const std::string& file_path, bool use_direct = false);
  ~SsDPageStore();

  SsDPageStore(const SsDPageStore&) = delete;
  SsDPageStore& operator=(const SsDPageStore&) = delete;

  // Page-level I/O
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

 private:
  int fd_;
  bool use_direct_;
  mutable std::recursive_mutex mutex_;

  // When use_direct_ is true, all pread/pwrite go through this aligned buffer.
  // thread-safety: all public methods hold mutex_, so a single buffer is safe.
  static constexpr size_t kBufAlign = 4096;
  alignas(kBufAlign) std::array<std::byte, kPageSize> dio_buf_{};
};

}  // namespace cbtree
