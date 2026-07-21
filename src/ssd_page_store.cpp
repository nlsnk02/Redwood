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

SsDPageStore::SsDPageStore(const std::string& file_path) {
  fd_ = open(file_path.c_str(), O_RDWR | O_CREAT, 0644);
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
  // Called from split_page (which holds mutex_) or during single-threaded init
  off_t end = lseek(fd_, 0, SEEK_END);
  if (end < 0) return 0;
  // Extend file by one page
  if (ftruncate(fd_, end + static_cast<off_t>(kPageSize)) != 0) return 0;
  return static_cast<PageId>(end / static_cast<off_t>(kPageSize));
}

static off_t page_offset(PageId id) {
  return static_cast<off_t>(id * static_cast<PageId>(kPageSize));
}

Status SsDPageStore::write_page(PageId id, const std::array<std::byte, kPageSize>& buf) {
  off_t off = page_offset(id);
  ssize_t written = pwrite(fd_, buf.data(), kPageSize, off);
  if (written != static_cast<ssize_t>(kPageSize)) return Status::Error;
  return Status::Ok;
}

Status SsDPageStore::read_page(PageId id, std::array<std::byte, kPageSize>& buf) {
  off_t off = page_offset(id);
  ssize_t n = pread(fd_, buf.data(), kPageSize, off);
  if (n != static_cast<ssize_t>(kPageSize)) return Status::Error;
  return Status::Ok;
}

Status SsDPageStore::put_record(PageId id, Key key, Value value) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::array<std::byte, kPageSize> page{};
  Status s = read_page(id, page);
  if (s != Status::Ok) return s;

  uint32_t count = read_count(page);

  // Linear scan for existing key
  for (uint32_t i = 0; i < count; ++i) {
    Record* rec = record_at(page, i);
    if (rec->key == key) {
      rec->value = value;
      return write_page(id, page);
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
  return write_page(id, page);
}

LookupResult SsDPageStore::get_record(PageId id, Key key) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::array<std::byte, kPageSize> page{};
  Status s = read_page(id, page);
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
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::array<std::byte, kPageSize> page{};
  Status s = read_page(id, page);
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
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  // Read left page
  std::array<std::byte, kPageSize> left_page{};
  Status s = read_page(left_id, left_page);
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

  // Allocate right page
  PageId right_id = alloc_page();
  *new_right_id = right_id;

  // Write left page
  std::array<std::byte, kPageSize> new_left_page{};
  write_count(new_left_page, static_cast<uint32_t>(left_records.size()));
  for (size_t i = 0; i < left_records.size(); ++i) {
    *record_at(new_left_page, static_cast<uint32_t>(i)) = left_records[i];
  }
  s = write_page(left_id, new_left_page);
  if (s != Status::Ok) return s;

  // Write right page
  std::array<std::byte, kPageSize> right_page{};
  write_count(right_page, static_cast<uint32_t>(right_records.size()));
  for (size_t i = 0; i < right_records.size(); ++i) {
    *record_at(right_page, static_cast<uint32_t>(i)) = right_records[i];
  }
  return write_page(right_id, right_page);
}

}  // namespace cbtree
