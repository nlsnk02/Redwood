#pragma once
#include <cstdint>
#include <string>

namespace cbtree {

using Key = uint64_t;
using Value = uint64_t;
using PageId = uint64_t;
using Fingerprint = uint16_t;

enum class SlotState : uint8_t { Empty, Placeholder, Occupied, Absent };

enum class Status : uint8_t {
  Ok,
  NotFound,
  NotImplemented,
  Retry,
  Full,
  Error
};

struct LookupResult {
  Status status{Status::NotFound};
  Value value{};
};

// MemoryHitStats tracks cache/memory hit rate for get() operations.
// Counters are designed to be updated atomically from concurrent threads.
// A "memory hit" means the get() was served without touching SSD —
// from a CacheAttachment, chunk chain, or absent marker.
struct MemoryHitStats {
  uint64_t total_gets{0};    // total completed get() calls
  uint64_t memory_hits{0};   // served from cache/chunk/absent (no SSD I/O)
  uint64_t ssd_accesses{0};  // required at least one SSD read
};

inline constexpr int kCacheSlots = 64;
inline constexpr int kLeafFanout = 32;
inline constexpr int kInternalFanout = 32;
inline constexpr size_t kPageSize = 4096;
inline constexpr double kParentFillThreshold = 0.8;
inline constexpr double kLeafFillThreshold = 0.8;
inline constexpr double kDefaultPParent = 0.1;
inline constexpr double kDefaultPPlaceholder = 0.4;

}  // namespace cbtree
