#include "cbtree/ssd_page_store.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace cbtree {

// Internal record layout: pair of Key and Value
struct Record {
  Key key;
  Value value;
};
static_assert(sizeof(Record) == 16, "Record must be 16 bytes");

// Page layout: 4-byte count header followed by up to kMaxRecordsPerPage records
struct PageHeader {
  uint32_t count;
};

static constexpr size_t kHeaderSize = sizeof(PageHeader);
static constexpr size_t kRecordSize = sizeof(Record);

static uint32_t read_count(const std::array<std::byte, kPageSize>& page) {
  uint32_t count;
  std::memcpy(&count, page.data(), sizeof(count));
  return count;
}

static void write_count(std::array<std::byte, kPageSize>& page, uint32_t count) {
  std::memcpy(page.data(), &count, sizeof(count));
}

static Record* record_at(std::array<std::byte, kPageSize>& page, uint32_t index) {
  return reinterpret_cast<Record*>(page.data() + kHeaderSize + index * kRecordSize);
}

static const Record* record_at(const std::array<std::byte, kPageSize>& page, uint32_t index) {
  return reinterpret_cast<const Record*>(page.data() + kHeaderSize + index * kRecordSize);
}

// Thread-local DIO bounce buffer — one per thread so that per-page locking
// can run concurrent pwrite/pread without a shared buffer.
thread_local std::array<std::byte, kPageSize> SsDPageStore::tl_dio_buf_{};

static off_t page_offset(PageId id) {
  return static_cast<off_t>(id * static_cast<PageId>(kPageSize));
}

SsDPageStore::SsDPageStore(const std::string& file_path, bool use_direct)
    : fd_(-1), use_direct_(use_direct) {
  int flags = O_RDWR | O_CREAT;
  if (use_direct_) {
    flags |= O_DIRECT;
  }
  fd_ = open(file_path.c_str(), flags, 0644);
  if (fd_ < 0) {
    fd_ = -1;
  }
}

SsDPageStore::~SsDPageStore() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

PageId SsDPageStore::alloc_page() {
  std::lock_guard<std::mutex> lock(alloc_mutex_);
  off_t end = lseek(fd_, 0, SEEK_END);
  if (end < 0) return 0;
  // Extend file by one page
  if (ftruncate(fd_, end + static_cast<off_t>(kPageSize)) != 0) return 0;
  return static_cast<PageId>(end / static_cast<off_t>(kPageSize));
}

// Internal helpers — assume the caller already holds the per-page lock.

Status SsDPageStore::read_page_locked(PageId id, std::array<std::byte, kPageSize>& buf) {
  off_t off = page_offset(id);
  void* rd_buf;
  if (use_direct_) {
    rd_buf = tl_dio_buf_.data();
  } else {
    rd_buf = buf.data();
  }
  ssize_t n = pread(fd_, rd_buf, kPageSize, off);
  if (n != static_cast<ssize_t>(kPageSize)) return Status::Error;
  if (use_direct_) {
    std::memcpy(buf.data(), tl_dio_buf_.data(), kPageSize);
  }
  return Status::Ok;
}

Status SsDPageStore::write_page_locked(PageId id, const std::array<std::byte, kPageSize>& buf) {
  off_t off = page_offset(id);
  const void* wr_buf;
  if (use_direct_) {
    // O_DIRECT requires aligned buffers — copy through the bounce buffer.
    std::memcpy(tl_dio_buf_.data(), buf.data(), kPageSize);
    wr_buf = tl_dio_buf_.data();
  } else {
    wr_buf = buf.data();
  }
  ssize_t written = pwrite(fd_, wr_buf, kPageSize, off);
  if (written != static_cast<ssize_t>(kPageSize)) return Status::Error;
  return Status::Ok;
}

// ---- Public API with per-page locking ----

Status SsDPageStore::read_page(PageId id, std::array<std::byte, kPageSize>& buf) {
  std::shared_lock<std::shared_mutex> lock(page_lock(id));
  io_reads_.fetch_add(1, std::memory_order_relaxed);
  return read_page_locked(id, buf);
}

Status SsDPageStore::write_page(PageId id, const std::array<std::byte, kPageSize>& buf) {
  std::unique_lock<std::shared_mutex> lock(page_lock(id));
  io_writes_.fetch_add(1, std::memory_order_relaxed);
  return write_page_locked(id, buf);
}

Status SsDPageStore::sync() {
  // Direct I/O writes directly to the storage device — no page cache to flush.
  // In buffered mode, fdatasync would be called here, but we no longer use
  // that mode.  Kept as a no-op for API compatibility.
  return Status::Ok;
}

Status SsDPageStore::put_record(PageId id, Key key, Value value) {
  std::unique_lock<std::shared_mutex> lock(page_lock(id));
  std::array<std::byte, kPageSize> page{};
  Status s = read_page_locked(id, page);
  if (s != Status::Ok) return s;

  uint32_t count = read_count(page);

  // Linear scan for existing key
  for (uint32_t i = 0; i < count; ++i) {
    Record* rec = record_at(page, i);
    if (rec->key == key) {
      rec->value = value;
      return write_page_locked(id, page);
    }
  }

  // Not found, append if there's room
  if (count >= static_cast<uint32_t>(kMaxRecordsPerPage)) {
    return Status::Full;
  }

  Record* rec = record_at(page, count);
  rec->key = key;
  rec->value = value;
  count++;
  write_count(page, count);
  return write_page_locked(id, page);
}

Status SsDPageStore::write_page_entries(
    PageId id, const std::vector<std::pair<Key, Value>>& entries,
    std::vector<std::pair<Key, Value>>& overflow) {
  io_write_entries_.fetch_add(1, std::memory_order_relaxed);
  std::unique_lock<std::shared_mutex> lock(page_lock(id));
  std::array<std::byte, kPageSize> page{};
  Status s = read_page_locked(id, page);
  if (s != Status::Ok) return s;

  uint32_t count = read_count(page);

  for (const auto& [key, value] : entries) {
    // Check for existing key — update in-place
    bool found = false;
    for (uint32_t i = 0; i < count; ++i) {
      Record* rec = record_at(page, i);
      if (rec->key == key) {
        rec->value = value;
        found = true;
        break;
      }
    }
    if (found) continue;

    // Not found — try to append
    if (count >= static_cast<uint32_t>(kMaxRecordsPerPage)) {
      overflow.push_back({key, value});
      continue;
    }
    Record* rec = record_at(page, count);
    rec->key = key;
    rec->value = value;
    count++;
  }

  write_count(page, count);
  return write_page_locked(id, page);
}

LookupResult SsDPageStore::get_record(PageId id, Key key) {
  std::shared_lock<std::shared_mutex> lock(page_lock(id));
  std::array<std::byte, kPageSize> page{};
  Status s = read_page_locked(id, page);
  if (s != Status::Ok) return {s, 0};

  uint32_t count = read_count(page);
  for (uint32_t i = 0; i < count; ++i) {
    const Record* rec = record_at(page, i);
    if (rec->key == key) {
      return {Status::Ok, rec->value};
    }
  }
  return {Status::NotFound, 0};
}

Status SsDPageStore::dump_sorted(PageId id, std::vector<std::pair<Key, Value>>* out) {
  io_dump_sorted_.fetch_add(1, std::memory_order_relaxed);
  std::shared_lock<std::shared_mutex> lock(page_lock(id));
  std::array<std::byte, kPageSize> page{};
  Status s = read_page_locked(id, page);
  if (s != Status::Ok) return s;

  uint32_t count = read_count(page);
  out->clear();
  out->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const Record* rec = record_at(page, i);
    out->emplace_back(rec->key, rec->value);
  }

  std::sort(out->begin(), out->end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return Status::Ok;
}

Status SsDPageStore::split_page(PageId left_id, Key mid, PageId* new_right_id) {
  io_splits_.fetch_add(1, std::memory_order_relaxed);
  // Lock only the left page — the right page is brand new and not yet
  // reachable by any other thread.
  std::unique_lock<std::shared_mutex> left_lock(page_lock(left_id));

  // Read left page
  std::array<std::byte, kPageSize> left_page{};
  Status s = read_page_locked(left_id, left_page);
  if (s != Status::Ok) return s;

  uint32_t left_count = read_count(left_page);

  // Collect records and partition
  std::vector<Record> left_records;
  std::vector<Record> right_records;
  for (uint32_t i = 0; i < left_count; ++i) {
    const Record* rec = record_at(left_page, i);
    if (rec->key < mid) {
      left_records.push_back(*rec);
    } else {
      right_records.push_back(*rec);
    }
  }

  // Allocate right page (uses alloc_mutex_, no page lock needed)
  PageId right_id = alloc_page();
  *new_right_id = right_id;

  // Write left page
  std::array<std::byte, kPageSize> new_left_page{};
  write_count(new_left_page, static_cast<uint32_t>(left_records.size()));
  for (size_t i = 0; i < left_records.size(); ++i) {
    *record_at(new_left_page, static_cast<uint32_t>(i)) = left_records[i];
  }
  s = write_page_locked(left_id, new_left_page);
  if (s != Status::Ok) return s;

  // Write right page — no lock needed (new page, id known only to caller)
  std::array<std::byte, kPageSize> right_page{};
  write_count(right_page, static_cast<uint32_t>(right_records.size()));
  for (size_t i = 0; i < right_records.size(); ++i) {
    *record_at(right_page, static_cast<uint32_t>(i)) = right_records[i];
  }
  return write_page_locked(right_id, right_page);
}

Status SsDPageStore::split_page(PageId left_id, Key mid,
                                const std::vector<std::pair<Key, Value>>& entries,
                                PageId* new_right_id) {
  io_splits_.fetch_add(1, std::memory_order_relaxed);
  std::unique_lock<std::shared_mutex> left_lock(page_lock(left_id));

  // Partition the pre-loaded entries (no read necessary).
  std::vector<Record> left_records;
  std::vector<Record> right_records;
  for (const auto& [k, v] : entries) {
    if (k < mid) {
      left_records.push_back({k, v});
    } else {
      right_records.push_back({k, v});
    }
  }

  PageId right_id = alloc_page();
  *new_right_id = right_id;

  // Write left page
  std::array<std::byte, kPageSize> new_left_page{};
  write_count(new_left_page, static_cast<uint32_t>(left_records.size()));
  for (size_t i = 0; i < left_records.size(); ++i) {
    *record_at(new_left_page, static_cast<uint32_t>(i)) = left_records[i];
  }
  Status s = write_page_locked(left_id, new_left_page);
  if (s != Status::Ok) return s;

  // Write right page
  std::array<std::byte, kPageSize> right_page{};
  write_count(right_page, static_cast<uint32_t>(right_records.size()));
  for (size_t i = 0; i < right_records.size(); ++i) {
    *record_at(right_page, static_cast<uint32_t>(i)) = right_records[i];
  }
  return write_page_locked(right_id, right_page);
}

SsDPageStore::IoStats SsDPageStore::io_stats() const {
  return {
    io_reads_.load(std::memory_order_relaxed),
    io_writes_.load(std::memory_order_relaxed),
    io_dump_sorted_.load(std::memory_order_relaxed),
    io_splits_.load(std::memory_order_relaxed),
    io_write_entries_.load(std::memory_order_relaxed),
  };
}

void SsDPageStore::reset_io_stats() {
  io_reads_.store(0, std::memory_order_relaxed);
  io_writes_.store(0, std::memory_order_relaxed);
  io_dump_sorted_.store(0, std::memory_order_relaxed);
  io_splits_.store(0, std::memory_order_relaxed);
  io_write_entries_.store(0, std::memory_order_relaxed);
}

}  // namespace cbtree
