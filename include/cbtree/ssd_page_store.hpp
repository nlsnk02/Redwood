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
  explicit SsDPageStore(const std::string& file_path);
  ~SsDPageStore();

  SsDPageStore(const SsDPageStore&) = delete;
  SsDPageStore& operator=(const SsDPageStore&) = delete;

  // Page-level I/O
  PageId alloc_page();
  Status write_page(PageId id, const std::array<std::byte, kPageSize>& buf);
  Status read_page(PageId id, std::array<std::byte, kPageSize>& buf);

  // In-page KV operations
  Status put_record(PageId id, Key key, Value value);
  LookupResult get_record(PageId id, Key key);
  Status dump_sorted(PageId id, std::vector<std::pair<Key, Value>>* out);
  Status split_page(PageId left_id, Key mid, PageId* new_right_id);

 private:
  int fd_;
  mutable std::recursive_mutex mutex_;
};

}  // namespace cbtree
