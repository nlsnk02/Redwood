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

inline constexpr int kCacheSlots = 16;
inline constexpr int kLeafFanout = 32;
inline constexpr int kInternalFanout = 32;
inline constexpr size_t kPageSize = 4096;
inline constexpr double kParentFillThreshold = 0.8;
inline constexpr double kLeafFillThreshold = 0.8;
inline constexpr double kDefaultPParent = 0.1;
inline constexpr double kDefaultPPlaceholder = 0.1;

}  // namespace cbtree
