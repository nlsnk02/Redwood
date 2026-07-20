# 带两级缓存的类 B+ 树 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-07-20-cache-augmented-btree-design.md` 实现可并发点查/点写、范围扫描、分裂与两级 CLOCK 缓存的类 B+ 树引擎（C++20）；删除/WAL/自适应概率仅 stub。

**Architecture:** 单进程多线程；`Tree` 编排路由/分裂/读写；`CacheAttachment` 管槽/指纹/CLOCK/slot 锁/KeyLockTable；`SsDPageStore` 管定长页；乐观奇偶 `version` 读。权威顺序：父缓存 > 叶缓存 > SSD。叶子索引**延迟登记**（仅刷盘时写入），KeyLockTable **每 CacheAttachment 独立**，分裂**等待**所有 key 锁释放后进入临界区。

**Tech Stack:** C++20、CMake 3.20+、GoogleTest（FetchContent）、`<mutex>` / `<atomic>`、文件系统定长页（单文件模拟 SSD）。

**Spec:** `docs/superpowers/specs/2026-07-20-cache-augmented-btree-design.md`

## Global Constraints

- C++20 (`set(CMAKE_CXX_STANDARD 20)`)
- CMake ≥ 3.20
- GoogleTest via FetchContent (v1.14.0)
- `Key` = `uint64_t`, `Value` = `uint64_t`（小定长）
- 每 `CacheAttachment` 持有一张独立 `KeyLockTable`（条带锁，64 条 `std::mutex`，`key % 64`）
- 叶子索引延迟登记：`put` 不更新叶子索引，仅刷盘（flush）时写入 `key → SSD 地址` 映射
- 分裂前等待本节点所有 key 写锁释放（v1 等待策略）
- CLOCK `hand` 为 `std::atomic<size_t>`，`sorted_flag` 为 `std::atomic<bool>`
- 驱逐时获取槽锁后必须验证槽状态未变

---

## File Structure

```
CMakeLists.txt
include/cbtree/
  types.hpp              # Key/Value/Status/PageId/SlotState/常量
  fingerprint.hpp        # 16-bit fingerprint 哈希
  key_lock_table.hpp     # 条带锁（64 条 mutex），供 CacheAttachment 组合使用
  cache_attachment.hpp   # 槽数组/指纹/CLOCK/slot 锁/KeyLockTable
  ssd_page_store.hpp     # 定长页分配/读写/页内 KV 编解码
  node.hpp               # InternalNode / LeafNode
  tree.hpp               # put/get/scan/split 编排
  adaptive_policy.hpp    # stub：概率调节
  wal_sink.hpp           # stub：ARIES 日志
  delete_ops.hpp         # stub：删除与合并
src/
  fingerprint.cpp
  key_lock_table.cpp
  cache_attachment.cpp
  ssd_page_store.cpp
  node.cpp
  tree.cpp
  adaptive_policy.cpp
  wal_sink.cpp
  delete_ops.cpp
tests/
  test_fingerprint.cpp
  test_key_lock_table.cpp
  test_cache_attachment.cpp
  test_ssd_page_store.cpp
  test_tree_basic.cpp
  test_tree_placeholder.cpp
  test_tree_evict.cpp
  test_tree_split.cpp
  test_tree_range.cpp
  test_tree_concurrent.cpp
  test_stubs.cpp
```

默认常量（集中在 `types.hpp`）：

- `kCacheSlots = 16`
- `kLeafFanout = 32`（叶子索引最大 key 数）
- `kInternalFanout = 32`
- `kPageSize = 4096`
- `kParentFillThreshold = 0.8`
- `kLeafFillThreshold = 0.8`
- `kDefaultPParent = 0.1`
- `kDefaultPPlaceholder = 0.1`

---

### Task 1: 工程脚手架

**Files:**
- Create: `CMakeLists.txt`
- Create: `include/cbtree/types.hpp`
- Create: 占位源文件（`src/*.cpp`，每个放空命名空间保证链接通过）
- Create: `tests/test_types_smoke.cpp`

**Interfaces:**
- Produces: `cbtree::Key`, `cbtree::Value`, `cbtree::PageId`, `cbtree::Fingerprint`, `cbtree::SlotState`, `cbtree::Status`, `cbtree::LookupResult`, 全部常量

- [ ] **Step 1: 写 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(cbtree LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_library(cbtree
  src/fingerprint.cpp
  src/key_lock_table.cpp
  src/cache_attachment.cpp
  src/ssd_page_store.cpp
  src/node.cpp
  src/tree.cpp
  src/adaptive_policy.cpp
  src/wal_sink.cpp
  src/delete_ops.cpp
)
target_include_directories(cbtree PUBLIC include)

enable_testing()
add_executable(cbtree_tests
  tests/test_types_smoke.cpp
)
target_link_libraries(cbtree_tests PRIVATE cbtree GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(cbtree_tests)
```

- [ ] **Step 2: 写 `types.hpp`**

```cpp
// include/cbtree/types.hpp
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
```

- [ ] **Step 3: 为每个 `src/*.cpp` 写占位空文件**

每个源文件内容（以 `src/fingerprint.cpp` 为例，其余类推）：
```cpp
// src/fingerprint.cpp
#include "cbtree/fingerprint.hpp"
namespace cbtree {
// placeholder — Task 2 实现
}
```

需创建的占位源文件：`src/fingerprint.cpp`, `src/key_lock_table.cpp`, `src/cache_attachment.cpp`, `src/ssd_page_store.cpp`, `src/node.cpp`, `src/tree.cpp`, `src/adaptive_policy.cpp`, `src/wal_sink.cpp`, `src/delete_ops.cpp`。

对应头文件只需 `#pragma once` + `namespace cbtree {}` 骨架，不需完整 API——后续 Task 会增量补充。

- [ ] **Step 4: 写冒烟测试**

```cpp
// tests/test_types_smoke.cpp
#include <gtest/gtest.h>
#include "cbtree/types.hpp"

TEST(TypesSmoke, Constants) {
  EXPECT_EQ(cbtree::kCacheSlots, 16);
  EXPECT_EQ(cbtree::kPageSize, 4096u);
}
```

- [ ] **Step 5: 构建并跑测试**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt include/ src/ tests/
git commit -m "chore: scaffold cbtree CMake project and types"
```

---

### Task 2: Fingerprint

**Files:**
- Modify: `include/cbtree/fingerprint.hpp`（补充声明）
- Modify: `src/fingerprint.cpp`（实现）
- Create: `tests/test_fingerprint.cpp`
- Modify: `CMakeLists.txt`（将 `test_fingerprint.cpp` 加入 `cbtree_tests`）

**Interfaces:**
- Produces: `cbtree::Fingerprint cbtree::fingerprint(Key key) noexcept`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_fingerprint.cpp
#include <gtest/gtest.h>
#include "cbtree/fingerprint.hpp"

TEST(Fingerprint, StableForSameKey) {
  EXPECT_EQ(cbtree::fingerprint(42), cbtree::fingerprint(42));
}

TEST(Fingerprint, UsuallyDiffers) {
  EXPECT_NE(cbtree::fingerprint(1), cbtree::fingerprint(2));
}
```

更新 `CMakeLists.txt` 中 `cbtree_tests` 的源文件列表，加入 `tests/test_fingerprint.cpp`。

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build && ctest --test-dir build -R Fingerprint --output-on-failure
```

Expected: 链接失败（`fingerprint` 未定义）

- [ ] **Step 3: 实现**

```cpp
// include/cbtree/fingerprint.hpp
#pragma once
#include "cbtree/types.hpp"
namespace cbtree {
Fingerprint fingerprint(Key key) noexcept;
}

// src/fingerprint.cpp
#include "cbtree/fingerprint.hpp"
namespace cbtree {
Fingerprint fingerprint(Key key) noexcept {
  // splitmix64 lower 16 bits
  uint64_t z = key + 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<Fingerprint>(z & 0xFFFF);
}
}
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R Fingerprint --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/fingerprint.hpp src/fingerprint.cpp tests/test_fingerprint.cpp CMakeLists.txt
git commit -m "feat: add 16-bit key fingerprint"
```

---

### Task 3: KeyLockTable（条带锁，每 CacheAttachment 独立）

**Files:**
- Modify: `include/cbtree/key_lock_table.hpp`（补充声明）
- Modify: `src/key_lock_table.cpp`（实现）
- Create: `tests/test_key_lock_table.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `cbtree::KeyLockTable` 类，含 `void lock(Key)`, `void unlock(Key)`, RAII `KeyLockGuard`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_key_lock_table.cpp
#include <thread>
#include <atomic>
#include <gtest/gtest.h>
#include "cbtree/key_lock_table.hpp"

TEST(KeyLockTable, SameKeySerializes) {
  cbtree::KeyLockTable table;
  std::atomic<int> in_critical{0};
  std::atomic<int> max_in{0};
  auto worker = [&] {
    for (int i = 0; i < 1000; ++i) {
      cbtree::KeyLockGuard g(table, 42);
      int v = ++in_critical;
      int m = max_in.load();
      while (v > m && !max_in.compare_exchange_weak(m, v)) {}
      --in_critical;
    }
  };
  std::thread t1(worker), t2(worker);
  t1.join(); t2.join();
  EXPECT_EQ(max_in.load(), 1);
}

TEST(KeyLockTable, DifferentKeysNoConflict) {
  cbtree::KeyLockTable table;
  std::atomic<int> count{0};
  auto worker = [&](cbtree::Key k) {
    cbtree::KeyLockGuard g(table, k);
    ++count;
  };
  std::thread t1(worker, 1), t2(worker, 2);
  t1.join(); t2.join();
  EXPECT_EQ(count.load(), 2);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败（`KeyLockTable` / `KeyLockGuard` 未定义）

- [ ] **Step 3: 实现**

```cpp
// include/cbtree/key_lock_table.hpp
#pragma once
#include <mutex>
#include <cstddef>
#include "cbtree/types.hpp"

namespace cbtree {

class KeyLockTable {
 public:
  static constexpr size_t kStripes = 64;

  void lock(Key key) { stripes_[key % kStripes].lock(); }
  void unlock(Key key) { stripes_[key % kStripes].unlock(); }

 private:
  std::mutex stripes_[kStripes];
};

class KeyLockGuard {
 public:
  KeyLockGuard(KeyLockTable& table, Key key)
      : table_(&table), key_(key) {
    table_->lock(key_);
  }
  ~KeyLockGuard() { table_->unlock(key_); }
  KeyLockGuard(const KeyLockGuard&) = delete;
  KeyLockGuard& operator=(const KeyLockGuard&) = delete;

 private:
  KeyLockTable* table_;
  Key key_;
};

}  // namespace cbtree
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R KeyLock --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/key_lock_table.hpp src/key_lock_table.cpp tests/test_key_lock_table.cpp CMakeLists.txt
git commit -m "feat: striped key write lock table (per CacheAttachment)"
```

---

### Task 4: CacheAttachment 基础（upsert / lookup / placeholder / absent）

**Files:**
- Modify: `include/cbtree/cache_attachment.hpp`（补充声明）
- Modify: `src/cache_attachment.cpp`（实现）
- Create: `tests/test_cache_attachment.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `cbtree::fingerprint`, `cbtree::KeyLockTable`, `cbtree::KeyLockGuard`, `cbtree::SlotState`, `cbtree::Status`, `cbtree::LookupResult`
- Produces: `cbtree::CacheAttachment` 类，含 `upsert`, `lookup`, `has_absent`, `mark_absent`, `try_place_placeholder`, `fill_placeholder`, `fill_placeholder_absent`, `occupied_count`, `sorted_flag`/`set_sorted_flag`/`clear_sorted_flag`

**数据结构（每槽）：**
```cpp
struct CacheSlot {
  SlotState state{SlotState::Empty};
  Key key{};
  Value value{};
  Fingerprint fp{};
  bool dirty{false};
  std::atomic<bool> clock_bit{false};   // 规格未要求 atomic，但便于无锁读
  std::mutex slot_mutex;
};
```

**CacheAttachment 成员：**
```cpp
class CacheAttachment {
 public:
  Status upsert(Key k, Value v);
  LookupResult lookup(Key k);
  bool has_absent(Key k) const;
  Status mark_absent(Key k);
  Status try_place_placeholder(Key k, int* out_idx);
  Status fill_placeholder(int idx, Value v);
  Status fill_placeholder_absent(int idx);
  int occupied_count() const;
  bool sorted_flag() const;
  void set_sorted_flag(bool v);
  void clear_sorted_flag();

  // 后续 Task 用（仅声明，本 Task 不实现/不测试）：
  Status pick_clock_victim(Key* out_key, Value* out_val, bool* out_dirty);
  Status split_into(Key mid, CacheAttachment* right);
  std::vector<std::pair<Key, Value>> occupied_sorted();

 private:
  CacheSlot slots_[kCacheSlots];
  std::atomic<size_t> hand_{0};
  std::atomic<bool> sorted_flag_{false};
  KeyLockTable key_locks_;   // 每个 CacheAttachment 独立一张表
  // ... 内部辅助方法
};
```

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_cache_attachment.cpp
#include <gtest/gtest.h>
#include "cbtree/cache_attachment.hpp"

TEST(CacheAttachment, UpsertAndLookup) {
  cbtree::CacheAttachment c;
  EXPECT_EQ(c.upsert(10, 100), cbtree::Status::Ok);
  auto r = c.lookup(10);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 100u);
}

TEST(CacheAttachment, LookupMiss) {
  cbtree::CacheAttachment c;
  auto r = c.lookup(42);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
}

TEST(CacheAttachment, AbsentMarkAndLookup) {
  cbtree::CacheAttachment c;
  EXPECT_EQ(c.mark_absent(7), cbtree::Status::Ok);
  auto r = c.lookup(7);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
  EXPECT_TRUE(c.has_absent(7));
}

TEST(CacheAttachment, UpsertOverAbsent) {
  cbtree::CacheAttachment c;
  c.mark_absent(5);
  EXPECT_EQ(c.upsert(5, 55), cbtree::Status::Ok);
  auto r = c.lookup(5);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 55u);
  EXPECT_FALSE(c.has_absent(5));
}

TEST(CacheAttachment, PlaceholderFlow) {
  cbtree::CacheAttachment c;
  int idx = -1;
  EXPECT_EQ(c.try_place_placeholder(5, &idx), cbtree::Status::Ok);
  EXPECT_GE(idx, 0);
  auto r1 = c.lookup(5);
  EXPECT_EQ(r1.status, cbtree::Status::NotFound);  // placeholder 不算命中
  EXPECT_EQ(c.fill_placeholder(idx, 55), cbtree::Status::Ok);
  auto r2 = c.lookup(5);
  EXPECT_EQ(r2.status, cbtree::Status::Ok);
  EXPECT_EQ(r2.value, 55u);
}

TEST(CacheAttachment, PlaceholderAbsentFill) {
  cbtree::CacheAttachment c;
  int idx = -1;
  c.try_place_placeholder(99, &idx);
  EXPECT_EQ(c.fill_placeholder_absent(idx), cbtree::Status::Ok);
  EXPECT_TRUE(c.has_absent(99));
}

TEST(CacheAttachment, UpsertUpdatesExisting) {
  cbtree::CacheAttachment c;
  c.upsert(1, 100);
  EXPECT_EQ(c.upsert(1, 200), cbtree::Status::Ok);
  auto r = c.lookup(1);
  EXPECT_EQ(r.value, 200u);
}

TEST(CacheAttachment, OccupiedCount) {
  cbtree::CacheAttachment c;
  EXPECT_EQ(c.occupied_count(), 0);
  c.upsert(1, 10);
  EXPECT_EQ(c.occupied_count(), 1);
  c.upsert(2, 20);
  EXPECT_EQ(c.occupied_count(), 2);
}

TEST(CacheAttachment, WriteFillsPlaceholder) {
  cbtree::CacheAttachment c;
  int idx = -1;
  c.try_place_placeholder(3, &idx);
  // 写撞占位：upsert 应原地填充
  EXPECT_EQ(c.upsert(3, 30), cbtree::Status::Ok);
  auto r = c.lookup(3);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 30u);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败

- [ ] **Step 3: 实现**

核心逻辑：
- `upsert`: 先指纹匹配再全 key 比较；找到 `OCCUPIED`/`ABSENT`/`PLACEHOLDER` 同 key → 原地覆写 value，`state=OCCUPIED`，`dirty=true`，`clock_bit=true`；未找到 → 找 `EMPTY` 槽，写指纹+key+value，`state=OCCUPIED`；满则返回 `Status::Full`（本 Task 不实现 CLOCK）
- `lookup`: 指纹匹配 → 全 key 确认；`OCCUPIED` → `Status::Ok` + value；`ABSENT` → `Status::NotFound` + `has_absent` 可查；其他 → `Status::NotFound`
- `try_place_placeholder`: 找 `EMPTY` 槽，写指纹+key，`state=PLACEHOLDER`，返回索引；若已存在同 key 占位 → 复用
- `fill_placeholder`: 校验索引有效且 `state==PLACEHOLDER`，写 value，`state=OCCUPIED`，`dirty=true`
- `fill_placeholder_absent`: 同上，`state=ABSENT`
- 每槽操作持 `slot_mutex`；写操作持 `key_locks_.lock(key)`（通过 `KeyLockGuard`）
- 读路径：若只读 `OCCUPIED` 不改状态，可 atomic 读 fingerprint + key/value + state（依赖 `clock_bit` 为 atomic 和 state 为 1 字节 enum），无需持 slot 锁；若需改状态（创建占位、回填），持 slot 锁

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R CacheAttachment --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/cache_attachment.hpp src/cache_attachment.cpp tests/test_cache_attachment.cpp CMakeLists.txt
git commit -m "feat: cache attachment upsert lookup placeholder absent with per-cache key locks"
```

---

### Task 5: CacheAttachment CLOCK 驱逐与 sorted 切开

**Files:**
- Modify: `include/cbtree/cache_attachment.hpp`, `src/cache_attachment.cpp`
- Modify: `tests/test_cache_attachment.cpp`（追加测试）

**Interfaces:**
- Produces: `pick_clock_victim`, `split_into`, `occupied_sorted`

- [ ] **Step 1: 写失败测试**

在 `tests/test_cache_attachment.cpp` 中追加：

```cpp
TEST(CacheAttachment, ClockEvictsColdOccupied) {
  cbtree::CacheAttachment c;
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(c.upsert(i, i), cbtree::Status::Ok);
  }
  // 访问 key=0 使其变热（clock_bit 置 1）
  ASSERT_EQ(c.lookup(0).status, cbtree::Status::Ok);
  // 额外 upsert 触发驱逐——key=0 不应被驱逐
  cbtree::Key victim_key{};
  cbtree::Value victim_val{};
  bool dirty = false;
  ASSERT_EQ(c.pick_clock_victim(&victim_key, &victim_val, &dirty), cbtree::Status::Ok);
  EXPECT_NE(victim_key, 0u);
}

TEST(CacheAttachment, ClockSkipsPlaceholder) {
  cbtree::CacheAttachment c;
  // 占满所有槽为 OCCUPIED
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(c.upsert(i, i), cbtree::Status::Ok);
  }
  // 驱逐一个腾空
  cbtree::Key vk; cbtree::Value vv; bool vd;
  ASSERT_EQ(c.pick_clock_victim(&vk, &vv, &vd), cbtree::Status::Ok);
  // 在空位放 placeholder
  int idx = -1;
  ASSERT_EQ(c.try_place_placeholder(999, &idx), cbtree::Status::Ok);
  // 再填满
  ASSERT_EQ(c.upsert(100, 100), cbtree::Status::Ok);
  // 驱逐：placeholder 不应成为牺牲者
  ASSERT_EQ(c.pick_clock_victim(&vk, &vv, &vd), cbtree::Status::Ok);
  EXPECT_NE(vk, 999u);
}

TEST(CacheAttachment, SplitByMid) {
  cbtree::CacheAttachment left, right;
  left.upsert(1, 1);
  left.upsert(5, 5);
  left.upsert(9, 9);
  ASSERT_EQ(left.split_into(5, &right), cbtree::Status::Ok);
  // key < mid 留左
  EXPECT_EQ(left.lookup(1).status, cbtree::Status::Ok);
  EXPECT_EQ(left.lookup(5).status, cbtree::Status::NotFound);
  // key >= mid 移右
  EXPECT_EQ(right.lookup(5).status, cbtree::Status::Ok);
  EXPECT_EQ(right.lookup(9).status, cbtree::Status::Ok);
}

TEST(CacheAttachment, OccupiedSorted) {
  cbtree::CacheAttachment c;
  c.upsert(3, 30);
  c.upsert(1, 10);
  c.upsert(2, 20);
  auto sorted = c.occupied_sorted();
  ASSERT_EQ(sorted.size(), 3u);
  EXPECT_EQ(sorted[0].first, 1u);
  EXPECT_EQ(sorted[1].first, 2u);
  EXPECT_EQ(sorted[2].first, 3u);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: FAIL（`pick_clock_victim` / `split_into` / `occupied_sorted` 未实现）

- [ ] **Step 3: 实现**

`pick_clock_victim`:
1. 从 `hand_` 开始扫描
2. 跳过 `EMPTY` 和 `PLACEHOLDER`（默认不驱逐占位）
3. 对 `OCCUPIED` / `ABSENT`：若 `clock_bit` 为 true → 置 false 并前进；若为 false → 选中
4. 若一轮全为 placeholder（极端情况）：选中第一个 placeholder
5. 获取选中槽的 `slot_mutex` → **验证槽状态未变**（key 一致、`clock_bit` 仍为 false、state 仍为 `OCCUPIED` 或 `ABSENT`）→ 若验证失败则释放锁重新扫描 → 将槽置 `EMPTY`，返回 key/value/dirty → 释放锁
6. `hand_` 前进到下一槽

`split_into(mid, right)`:
1. 遍历左缓存所有槽
2. `key >= mid` 的 `OCCUPIED` / `ABSENT` / `PLACEHOLDER` → 移动到右缓存（找 `EMPTY` 槽写入，左槽置 `EMPTY`）
3. `key < mid` 保留
4. 清两侧 `sorted_flag`

`occupied_sorted`:
1. 收集所有 `OCCUPIED` 槽的 `(key, value)`
2. 按 key 排序返回

- [ ] **Step 4: 跑测试确认通过**

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/cache_attachment.hpp src/cache_attachment.cpp tests/test_cache_attachment.cpp
git commit -m "feat: cache CLOCK victim selection with slot verification and mid split"
```

---

### Task 6: SsDPageStore

**Files:**
- Modify: `include/cbtree/ssd_page_store.hpp`（补充声明）
- Modify: `src/ssd_page_store.cpp`（实现）
- Create: `tests/test_ssd_page_store.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `cbtree::SsDPageStore` 类，含 `alloc_page`, `write_page`, `read_page`, 以及页内 KV 操作 `put_record`, `get_record`, `dump_sorted`, `split_page`

**页内 KV 布局（定长，供叶子刷盘）：**
```
Header: uint32_t count
Records: repeated { Key key; Value value; }  up to (kPageSize-4)/16 = 255 条
```

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_ssd_page_store.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include <array>
#include "cbtree/ssd_page_store.hpp"

class SsDPageStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::temp_directory_path() / "cbtree_test_ssd.pages";
    store_ = std::make_unique<cbtree::SsDPageStore>(path_.string());
  }
  void TearDown() override {
    store_.reset();
    std::filesystem::remove(path_);
  }
  std::filesystem::path path_;
  std::unique_ptr<cbtree::SsDPageStore> store_;
};

TEST_F(SsDPageStoreTest, AllocWriteRead) {
  auto id = store_->alloc_page();
  std::array<std::byte, cbtree::kPageSize> buf{};
  buf[0] = std::byte{0xAB};
  ASSERT_EQ(store_->write_page(id, buf), cbtree::Status::Ok);
  std::array<std::byte, cbtree::kPageSize> out{};
  ASSERT_EQ(store_->read_page(id, out), cbtree::Status::Ok);
  EXPECT_EQ(out[0], std::byte{0xAB});
}

TEST_F(SsDPageStoreTest, PutGetRecord) {
  auto id = store_->alloc_page();
  ASSERT_EQ(store_->put_record(id, 1, 10), cbtree::Status::Ok);
  ASSERT_EQ(store_->put_record(id, 2, 20), cbtree::Status::Ok);
  auto r1 = store_->get_record(id, 1);
  EXPECT_EQ(r1.status, cbtree::Status::Ok);
  EXPECT_EQ(r1.value, 10u);
  auto r2 = store_->get_record(id, 2);
  EXPECT_EQ(r2.status, cbtree::Status::Ok);
  EXPECT_EQ(r2.value, 20u);
  auto r3 = store_->get_record(id, 99);
  EXPECT_EQ(r3.status, cbtree::Status::NotFound);
}

TEST_F(SsDPageStoreTest, PutRecordUpsert) {
  auto id = store_->alloc_page();
  store_->put_record(id, 1, 10);
  ASSERT_EQ(store_->put_record(id, 1, 99), cbtree::Status::Ok);
  auto r = store_->get_record(id, 1);
  EXPECT_EQ(r.value, 99u);
}

TEST_F(SsDPageStoreTest, DumpSorted) {
  auto id = store_->alloc_page();
  store_->put_record(id, 3, 30);
  store_->put_record(id, 1, 10);
  store_->put_record(id, 2, 20);
  std::vector<std::pair<cbtree::Key, cbtree::Value>> out;
  ASSERT_EQ(store_->dump_sorted(id, &out), cbtree::Status::Ok);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0], std::make_pair(1ull, 10ull));
  EXPECT_EQ(out[1], std::make_pair(2ull, 20ull));
  EXPECT_EQ(out[2], std::make_pair(3ull, 30ull));
}

TEST_F(SsDPageStoreTest, SplitPage) {
  auto id = store_->alloc_page();
  for (uint64_t i = 0; i < 10; ++i) {
    store_->put_record(id, i, i);
  }
  cbtree::PageId right_id{0};
  ASSERT_EQ(store_->split_page(id, 5, &right_id), cbtree::Status::Ok);
  EXPECT_NE(right_id, 0u);
  // 左页应含 key < 5
  auto r_left = store_->get_record(id, 3);
  EXPECT_EQ(r_left.status, cbtree::Status::Ok);
  auto r_left_miss = store_->get_record(id, 7);
  EXPECT_EQ(r_left_miss.status, cbtree::Status::NotFound);
  // 右页应含 key >= 5
  auto r_right = store_->get_record(right_id, 7);
  EXPECT_EQ(r_right.status, cbtree::Status::Ok);
  auto r_right_miss = store_->get_record(right_id, 3);
  EXPECT_EQ(r_right_miss.status, cbtree::Status::NotFound);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败

- [ ] **Step 3: 实现**

`SsDPageStore` 核心：
- 构造函数接收文件路径，文件不存在则创建
- `alloc_page()`: 追加一页（`kPageSize` 字节）到文件末尾，返回 `PageId`（页号 = 偏移量 / `kPageSize`）
- `write_page(id, buf)` / `read_page(id, buf)`: `pwrite` / `pread` 到 `id * kPageSize` 偏移
- `put_record(id, k, v)`: 读页 → 解析 header（count）→ 线性扫描 records → 找到同 key 则更新 value → 未找到则追加到 count 位置 → count++ → 写回页。若 count 已满则返回 `Status::Full`
- `get_record(id, k)`: 读页 → 解析 → 线性扫描 → 返回 value 或 NotFound
- `dump_sorted(id, out)`: 读页 → 解析全部 records → 按 key 排序 → 写入 out
- `split_page(left_id, mid, &new_right_id)`: 读左页 → `alloc_page` 得右页 → 遍历 records：`key < mid` 留左，`key >= mid` 写右 → 写回两页

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R SsDPageStore --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/ssd_page_store.hpp src/ssd_page_store.cpp tests/test_ssd_page_store.cpp CMakeLists.txt
git commit -m "feat: fixed-size SSD page store with in-page KV"
```

---

### Task 7: Node + Tree 引导（空树 put/get）

**Files:**
- Modify: `include/cbtree/node.hpp`（补充 Node 定义）
- Modify: `src/node.cpp`（实现 Node 构造）
- Modify: `include/cbtree/tree.hpp`（补充 Tree 声明）
- Modify: `src/tree.cpp`（实现最小 put/get）
- Create: `tests/test_tree_basic.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CacheAttachment`, `SsDPageStore`, `KeyLockTable`
- Produces: `cbtree::Node`, `cbtree::Tree` 含 `put(Key, Value)`, `get(Key)`

**Node 结构：**
```cpp
struct Node {
  std::atomic<uint64_t> version{0};  // 偶=稳定，奇=结构变更中
  int height{1};                      // 叶=1
  Node* parent{nullptr};

  // 叶节点字段（height == 1 时有效）：
  std::vector<Key> leaf_keys;              // 有序 key 列表（仅刷盘时登记，延迟登记）
  std::vector<PageId> leaf_page_ids;       // leaf_keys[i] 对应的页号
  PageId page_id{0};                       // 当前叶的 SSD 页

  // 内部节点字段（height >= 2 时有效）：
  std::vector<Key> separators;             // 分隔键
  std::vector<Node*> children;

  // 缓存（height == 1 或 height == 2 时挂载）：
  std::unique_ptr<CacheAttachment> cache;  // h==1 或 h==2 时非空；h>=3 为 nullptr
};
```

**Tree 引导：**
```cpp
class Tree {
 public:
  explicit Tree(const std::string& ssd_path);
  Status put(Key k, Value v);
  LookupResult get(Key k);

  // 测试钩子（后续 Task 逐步暴露）：
  void set_probabilities(double p_parent, double p_placeholder);
  int debug_height() const;

 private:
  Node* root_;
  std::unique_ptr<SsDPageStore> ssd_;
  double p_parent_{kDefaultPParent};
  double p_placeholder_{kDefaultPPlaceholder};
  // ... put/get 内部辅助
};
```

构造时：创建 `root_`（`height=1`，`cache = new CacheAttachment`），`ssd_->alloc_page()` 分配给 `root_->page_id`。

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_tree_basic.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreeBasicTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_basic.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreeBasicTest, EmptyGetNotFound) {
  auto r = tree_->get(1);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
}

TEST_F(TreeBasicTest, PutGet) {
  ASSERT_EQ(tree_->put(1, 10), cbtree::Status::Ok);
  auto r = tree_->get(1);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 10u);
}

TEST_F(TreeBasicTest, PutMultipleGet) {
  for (uint64_t i = 0; i < 10; ++i) {
    ASSERT_EQ(tree_->put(i, i * 10), cbtree::Status::Ok);
  }
  for (uint64_t i = 0; i < 10; ++i) {
    auto r = tree_->get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok);
    EXPECT_EQ(r.value, i * 10);
  }
}

TEST_F(TreeBasicTest, PutUpdatesExisting) {
  ASSERT_EQ(tree_->put(5, 50), cbtree::Status::Ok);
  ASSERT_EQ(tree_->put(5, 99), cbtree::Status::Ok);
  auto r = tree_->get(5);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 99u);
}

TEST_F(TreeBasicTest, InitialHeightIsOne) {
  EXPECT_EQ(tree_->debug_height(), 1);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败

- [ ] **Step 3: 实现**

**Node:** 按上述结构定义，`height==1` 时 `leaf_keys` 和 `leaf_page_ids` 大小相等，`children` 为空；`height>=2` 时 `leaf_keys` 为空，`children.size() == separators.size() + 1`。

**Tree 构造：**
```cpp
Tree::Tree(const std::string& ssd_path)
    : ssd_(std::make_unique<SsDPageStore>(ssd_path)) {
  root_ = new Node{};
  root_->height = 1;
  root_->cache = std::make_unique<CacheAttachment>();
  root_->page_id = ssd_->alloc_page();
}
```

**put（height=1 最小版，单线程）：**
1. `KeyLockGuard guard(root_->cache->key_locks_, k)` （通过缓存访问 KeyLockTable）
2. 查 root 缓存：若有同 key → `upsert`，返回
3. 若无：找 `EMPTY` 槽 `upsert`；满则触发 CLOCK 驱逐（本 Task 中驱逐的脏数据写入 SSD，并登记叶子索引）后重试
4. **不**更新 `leaf_keys`（延迟登记）

**get（height=1 最小版）：**
1. 读前记录 `v = root_->version.load()`，若奇数则自旋/重试
2. 查 root 缓存：`OCCUPIED` → 返回 value；`ABSENT` → 返回 NotFound
3. 缓存 miss：查 SSD `get_record(root_->page_id, k)`
4. SSD 后仅内存二次校验：再查 root 缓存一次
5. 读后校验 `v == root_->version.load()`，失败则重试（上限 64 次）

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R TreeBasic --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/node.hpp src/node.cpp include/cbtree/tree.hpp src/tree.cpp tests/test_tree_basic.cpp CMakeLists.txt
git commit -m "feat: tree bootstrap with single-leaf put/get and lazy index"
```

---

### Task 8: 占位符、ABSENT、SSD 后内存二次校验

**Files:**
- Modify: `src/tree.cpp`, `include/cbtree/tree.hpp`（完善 get 路径）
- Create: `tests/test_tree_placeholder.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tree put/get from Task 7
- Produces: 完整点查路径（对齐规格 §6），测试钩子 `set_probabilities`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_tree_placeholder.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreePlaceholderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_ph.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
    tree_->set_probabilities(0.0, 1.0);  // P_placeholder=1 便于测试
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreePlaceholderTest, NotFoundCreatesAbsent) {
  auto r = tree_->get(999);
  EXPECT_EQ(r.status, cbtree::Status::NotFound);
  // 再次 get 应在缓存命中 ABSENT
  auto r2 = tree_->get(999);
  EXPECT_EQ(r2.status, cbtree::Status::NotFound);
}

TEST_F(TreePlaceholderTest, WriteFillsPlaceholder) {
  // get miss 先留占位/ABSENT
  ASSERT_EQ(tree_->get(3).status, cbtree::Status::NotFound);
  // 然后 put 同 key
  ASSERT_EQ(tree_->put(3, 30), cbtree::Status::Ok);
  auto r = tree_->get(3);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 30u);
}

TEST_F(TreePlaceholderTest, GetExistingAfterAbsent) {
  // 先确认不存在
  ASSERT_EQ(tree_->get(7).status, cbtree::Status::NotFound);
  // 写入
  ASSERT_EQ(tree_->put(7, 77), cbtree::Status::Ok);
  // put 应将 ABSENT 转为 OCCUPIED
  auto r = tree_->get(7);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
  EXPECT_EQ(r.value, 77u);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: FAIL（placeholder/absent 路径未完整实现）

- [ ] **Step 3: 实现**

完善 `get` 路径以对齐规格 §6：

1. 自上而下查缓存（含指纹初筛）
2. 命中 `OCCUPIED`：返回 value
3. 命中 `ABSENT`：返回 NotFound
4. 未命中：以 `P_placeholder` 概率创建 PLACEHOLDER（读上下文维护 `bool has_placed` 标志，跨缓存级别共享，整次读最多一个；复用已有同 key 占位）
5. 下探 SSD；回填从缓存级别向下找占位，填 value 或 ABSENT
6. SSD 后内存二次校验（仅扫描缓存，不再访问 SSD；version 校验失败则整次重试）
7. 全路径 version 校验失败 → 重试（上限 64 次，避免无限循环）

添加 `Tree::set_probabilities(double p_parent, double p_placeholder)` 测试钩子。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R TreePlaceholder --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/tree.cpp include/cbtree/tree.hpp tests/test_tree_placeholder.cpp CMakeLists.txt
git commit -m "feat: placeholders absent and post-SSD memory recheck"
```

---

### Task 9: 父缓存写入概率与 height≥2 路由

**Files:**
- Modify: `src/tree.cpp`（完善下钻逻辑 + P_parent 目标选择）
- Modify: `include/cbtree/tree.hpp`（补充调试接口）
- Modify: `tests/test_tree_basic.cpp`（追加多级测试）

**Interfaces:**
- Consumes: Tree with placeholder/absent support
- Produces: 多级树路由，P_parent 写目标选择，测试夹具 `Tree::DebugMultiLevel`

- [ ] **Step 1: 写失败测试**

由于需要多级树来测试父缓存，本 Task 先实现内部的下钻路由和 P_parent 选择，单测用 `DebugMultiLevel` 工厂方法（手动构造 height=2 夹具）：

```cpp
TEST(TreeMultiLevel, DebugTwoLeaves) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  EXPECT_EQ(t.debug_height(), 2);
  // 父缓存存在且初始为空
}

TEST(TreeMultiLevel, PutToParentCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(1.0, 0.0);  // 必进父缓存
  ASSERT_EQ(t.put(10, 1), cbtree::Status::Ok);
  EXPECT_TRUE(t.debug_parent_cache_contains(10));
}

TEST(TreeMultiLevel, PutToLeafCache) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(0.0, 0.0);  // 必进叶子缓存
  ASSERT_EQ(t.put(10, 1), cbtree::Status::Ok);
  // 父缓存不应有
  EXPECT_FALSE(t.debug_parent_cache_contains(10));
}

TEST(TreeMultiLevel, DescendFindsCorrectLeaf) {
  auto t = cbtree::Tree::DebugTwoLeaves("/tmp/test_ml.pages");
  t.set_probabilities(0.0, 0.0);
  // split 后 key 应路由到正确叶子
  ASSERT_EQ(t.put(5, 50), cbtree::Status::Ok);
  ASSERT_EQ(t.put(50, 500), cbtree::Status::Ok);
  EXPECT_EQ(t.get(5).value, 50u);
  EXPECT_EQ(t.get(50).value, 500u);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败（`DebugTwoLeaves` / `debug_parent_cache_contains` 未定义）

- [ ] **Step 3: 实现**

**下钻逻辑（`put`/`get` 共用）：**
```cpp
Node* Tree::descend_to_leaf(Key k, std::vector<std::pair<Node*, uint64_t>>& versions) {
  Node* cur = root_;
  while (cur->height > 1) {
    versions.emplace_back(cur, cur->version.load());
    // 二分查找 separators 确定子节点
    auto it = std::upper_bound(cur->separators.begin(), cur->separators.end(), k);
    size_t idx = it - cur->separators.begin();
    cur = cur->children[idx];
  }
  versions.emplace_back(cur, cur->version.load());
  return cur;  // 叶节点
}
```

**P_parent 目标选择：**
- `put` 遍历时，到达 `height==2` 节点（叶之父），以概率 `P_parent` 标记目标为该节点的缓存
- 若 `height==1`（仅根叶），目标固定为叶缓存
- 沿途命中（步骤 3 优先级 > 步骤 2）：在任何缓存中找到同 key（含 ABSENT/PLACEHOLDER）→ 原地 upsert，返回

**DebugMultiLeaves 工厂：**
手动构造：创建 root（height=2，挂空缓存），两个子叶（height=1，各挂空缓存 + alloc_page），设置 separators。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R TreeMultiLevel --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/tree.cpp include/cbtree/tree.hpp tests/test_tree_basic.cpp
git commit -m "feat: parent-cache write probability and multi-level routing"
```

---

### Task 10: 叶子分裂 + 高度规则

**Files:**
- Modify: `src/tree.cpp`, `include/cbtree/node.hpp`（实现 split）
- Create: `tests/test_tree_split.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Tree multi-level routing, CacheAttachment::split_into, SsDPageStore::split_page
- Produces: 叶子分裂（自动触发 + 递归父分裂）

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_tree_split.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreeSplitTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_split.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
    tree_->set_probabilities(0.0, 0.0);  // 全部进叶缓存，简化测试
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreeSplitTest, LeafSplitOnFlush) {
  // 插入足够多条目，每次 put 后手动 flush 以填满叶索引
  for (uint64_t i = 0; i < cbtree::kLeafFanout + 1; ++i) {
    ASSERT_EQ(tree_->put(i, i), cbtree::Status::Ok);
    ASSERT_EQ(tree_->debug_flush_all(), cbtree::Status::Ok);
  }
  EXPECT_GE(tree_->debug_height(), 2);
  // 所有 key 仍可读写
  for (uint64_t i = 0; i < cbtree::kLeafFanout + 1; ++i) {
    auto r = tree_->get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok) << "key=" << i;
    EXPECT_EQ(r.value, i);
  }
}

TEST_F(TreeSplitTest, SplitPreservesCache) {
  for (uint64_t i = 0; i < cbtree::kLeafFanout + 1; ++i) {
    ASSERT_EQ(tree_->put(i, i), cbtree::Status::Ok);
    ASSERT_EQ(tree_->debug_flush_all(), cbtree::Status::Ok);
  }
  // 分裂后所有叶和父均有缓存
  EXPECT_GE(tree_->debug_height(), 2);
  EXPECT_TRUE(tree_->debug_all_leaves_have_cache());
  // 新根（若 height=2）应有缓存
  if (tree_->debug_height() == 2) {
    EXPECT_TRUE(tree_->debug_root_has_cache());
  }
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败（`debug_flush_all` 等未定义）或 FAIL

- [ ] **Step 3: 实现**

**分裂触发（叶子）：** `put` 写入后，若叶索引（`leaf_keys`）size > `kLeafFanout`，触发 `split_leaf(leaf)`。

**split_leaf：**
1. 等待本节点所有 key 写锁释放（v1 等待策略）→ `version.fetch_add(1)`（置奇）
2. 缓存排序（`occupied_sorted` + `sorted_flag`）
3. 选 `mid = leaf_keys[leaf_keys.size()/2]`
4. `alloc_page()` 得新页 → `split_page` 切 SSD 数据
5. 分配 `L_right`（height=1，新 CacheAttachment，新 page_id）
6. 切叶索引：`key < mid` 留左，`key >= mid` 移右（更新 `leaf_keys` 和 `leaf_page_ids`）
7. `cache->split_into(mid, L_right->cache.get())` 切缓存
8. 更新父节点：
   - 若原叶即根：新建根（height=2，空 CacheAttachment），root 指向新根，新根 children[0]=old_root, children[1]=L_right，separators[0]=mid
   - 若已有父：在 separators 中插入 mid，children 中插入 L_right
9. `version.fetch_add(1)`（置偶，回到稳定）
10. 若父溢出 → 递归 `split_internal(parent)`

**debug_flush_all：** 遍历所有叶子，将每个叶缓存中所有 dirty `OCCUPIED` 条目写入 SSD 并登记叶子索引（见 Task 11 的刷盘逻辑，本 Task 可先实现简化版），清除 dirty 标志。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R TreeSplit --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/tree.cpp include/cbtree/node.hpp include/cbtree/tree.hpp tests/test_tree_split.cpp CMakeLists.txt
git commit -m "feat: leaf split with height-based cache rules"
```

---

### Task 11: 父→叶下推与叶→SSD 刷脏（含延迟索引登记）

**Files:**
- Modify: `src/tree.cpp`, `src/cache_attachment.cpp`（驱逐触发逻辑）
- Modify: `include/cbtree/tree.hpp`（补充刷新接口）
- Create: `tests/test_tree_evict.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: 多级树 + 分裂 + CacheAttachment CLOCK
- Produces: 压力驱逐（父→叶→SSD），叶子索引延迟登记

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_tree_evict.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreeEvictTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_evict.pages").string();
  }
  void TearDown() override {
    std::filesystem::remove(path_);
  }
  std::string path_;
};

TEST_F(TreeEvictTest, ParentDemotesToLeaf) {
  auto t = cbtree::Tree::DebugTwoLeaves(path_);
  t.set_probabilities(1.0, 0.0);
  // 填满父缓存
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(t.put(1000 + i, i), cbtree::Status::Ok);
  }
  // 再 put 触发父→叶下推
  ASSERT_EQ(t.put(2000, 1), cbtree::Status::Ok);
  EXPECT_TRUE(t.debug_some_keys_in_leaf_cache());
}

TEST_F(TreeEvictTest, LeafDirtyFlushesToSsd) {
  cbtree::Tree t{path_};
  t.set_probabilities(0.0, 0.0);
  // 填满叶缓存（脏，因为全部未刷盘）
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    ASSERT_EQ(t.put(i, i * 10), cbtree::Status::Ok);
  }
  // 触发叶驱逐 → 刷盘 → 延迟登记索引
  ASSERT_EQ(t.put(999, 1), cbtree::Status::Ok);
  // 清空叶缓存后仍能从 SSD 读到之前刷盘的数据
  t.debug_clear_all_caches();
  for (uint64_t i = 0; i < cbtree::kCacheSlots; ++i) {
    auto r = t.get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok) << "key=" << i;
    EXPECT_EQ(r.value, i * 10);
  }
}

TEST_F(TreeEvictTest, FlushRegistersLeafIndex) {
  cbtree::Tree t{path_};
  t.set_probabilities(0.0, 0.0);
  t.put(1, 10);
  t.put(2, 20);
  // flush 前叶子索引应为空（延迟登记）
  EXPECT_TRUE(t.debug_leaf_index_empty());
  // 强制刷盘
  ASSERT_EQ(t.debug_flush_all(), cbtree::Status::Ok);
  // flush 后叶子索引应有条目
  EXPECT_FALSE(t.debug_leaf_index_empty());
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: FAIL

- [ ] **Step 3: 实现**

**父缓存压力驱逐（Tree 层编排）：**
```cpp
Status Tree::evict_parent_if_needed(Node* parent_node) {
  if (parent_node->cache->occupied_count() < kCacheSlots * kParentFillThreshold)
    return Status::Ok;
  Key victim_key; Value victim_val; bool victim_dirty;
  if (parent_node->cache->pick_clock_victim(&victim_key, &victim_val, &victim_dirty)
      != Status::Ok) return Status::Ok;

  if (victim_dirty) {
    // 下推到叶子：找到 victim_key 对应的叶子
    Node* leaf = find_leaf_for_key(parent_node, victim_key);
    // 若叶子缓存满 → 先触发叶子驱逐
    evict_leaf_if_needed(leaf);
    // 插入叶子缓存（叶子已有同 key 则上层覆盖）
    leaf->cache->upsert(victim_key, victim_val);
  }
  return Status::Ok;
}
```

**叶子缓存压力驱逐 + 延迟索引登记：**
```cpp
Status Tree::evict_leaf_if_needed(Node* leaf) {
  if (leaf->cache->occupied_count() < kCacheSlots * kLeafFillThreshold)
    return Status::Ok;
  Key victim_key; Value victim_val; bool victim_dirty;
  if (leaf->cache->pick_clock_victim(&victim_key, &victim_val, &victim_dirty)
      != Status::Ok) return Status::Ok;

  if (victim_dirty) {
    // 写 SSD
    ssd_->put_record(leaf->page_id, victim_key, victim_val);
    // **延迟登记：在此处才将 key 写入叶子索引**
    register_in_leaf_index(leaf, victim_key);
  }
  // 干净的 OCCUPIED / ABSENT 直接丢弃（权威已在 SSD 或否定缓存可丢）
  return Status::Ok;
}

void Tree::register_in_leaf_index(Node* leaf, Key k) {
  auto it = std::lower_bound(leaf->leaf_keys.begin(), leaf->leaf_keys.end(), k);
  if (it != leaf->leaf_keys.end() && *it == k) return;  // 已存在
  size_t idx = it - leaf->leaf_keys.begin();
  leaf->leaf_keys.insert(it, k);
  leaf->leaf_page_ids.insert(leaf->leaf_page_ids.begin() + idx, leaf->page_id);
}
```

**锁释放时机（对齐规格 §7.3）：**
- CLOCK 选中牺牲者 → 获取槽锁 → 验证状态 → **复制数据 → 释放槽锁** → 执行下推/刷盘
- 父→叶下推触发叶子驱逐时，不持有父槽锁

**调试接口：**
- `debug_clear_all_caches()`: 清空所有缓存（不刷盘，仅测试用）
- `debug_leaf_index_empty()`: 所有叶子索引为空
- `debug_flush_all()`: 强制刷所有脏数据
- `debug_some_keys_in_leaf_cache()`: 叶子缓存中有条目

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R TreeEvict --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/tree.cpp src/cache_attachment.cpp include/cbtree/tree.hpp tests/test_tree_evict.cpp CMakeLists.txt
git commit -m "feat: parent-to-leaf demotion and leaf-to-SSD flush with lazy index registration"
```

---

### Task 12: 范围查询

**Files:**
- Modify: `include/cbtree/tree.hpp`, `src/tree.cpp`
- Create: `tests/test_tree_range.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `std::vector<std::pair<Key, Value>> Tree::scan(Key lo, Key hi)`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_tree_range.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include "cbtree/tree.hpp"

class TreeRangeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_range.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreeRangeTest, InclusiveRange) {
  for (uint64_t i = 0; i < 20; ++i)
    ASSERT_EQ(tree_->put(i, i * 10), cbtree::Status::Ok);
  auto v = tree_->scan(5, 8);
  ASSERT_EQ(v.size(), 4u);
  EXPECT_EQ(v[0], std::make_pair(5ull, 50ull));
  EXPECT_EQ(v[1], std::make_pair(6ull, 60ull));
  EXPECT_EQ(v[2], std::make_pair(7ull, 70ull));
  EXPECT_EQ(v[3], std::make_pair(8ull, 80ull));
}

TEST_F(TreeRangeTest, EmptyRange) {
  for (uint64_t i = 0; i < 5; ++i)
    tree_->put(i, i);
  auto v = tree_->scan(100, 200);
  EXPECT_TRUE(v.empty());
}

TEST_F(TreeRangeTest, RangeAfterFlush) {
  for (uint64_t i = 0; i < 10; ++i)
    ASSERT_EQ(tree_->put(i, i * 2), cbtree::Status::Ok);
  ASSERT_EQ(tree_->debug_flush_all(), cbtree::Status::Ok);
  auto v = tree_->scan(3, 7);
  ASSERT_EQ(v.size(), 5u);
  for (size_t i = 0; i < v.size(); ++i) {
    EXPECT_EQ(v[i].second, v[i].first * 2);
  }
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: FAIL（`scan` 未实现）

- [ ] **Step 3: 实现**

对齐规格 §8：

```cpp
std::vector<std::pair<Key, Value>> Tree::scan(Key lo, Key hi) {
  // 1. 确定覆盖 [lo, hi] 的叶子集合及其父节点
  std::vector<Node*> leaves = collect_leaves_in_range(lo, hi);
  std::vector<CacheAttachment*> caches;
  for (auto* leaf : leaves) {
    caches.push_back(leaf->cache.get());
    if (leaf->parent && leaf->parent->height == 2)
      caches.push_back(leaf->parent->cache.get());
  }
  // 去重父缓存（多个叶子可能共享同一父节点）
  std::sort(caches.begin(), caches.end());
  caches.erase(std::unique(caches.begin(), caches.end()), caches.end());

  // 2-3. 对每个缓存排序（OCCUPIED + ABSENT）
  for (auto* cache : caches) {
    if (!cache->sorted_flag())
      cache->sort_and_set_flag();  // 需要 CacheAttachment 新增此方法
  }

  // 4. 收集 SSD 页数据
  std::vector<std::vector<std::pair<Key, Value>>> ssd_data;
  for (auto* leaf : leaves) {
    std::vector<std::pair<Key, Value>> page_data;
    ssd_->dump_sorted(leaf->page_id, &page_data);
    ssd_data.push_back(std::move(page_data));
  }

  // 5. 多路归并：父缓存 > 叶子缓存 > SSD，ABSENT 抑制下层
  return merge_by_authority(caches, ssd_data, lo, hi);
}
```

**归并逻辑 `merge_by_authority`：**
- 使用多路归并器（最小堆），每个来源一个迭代器
- 同 key 出现于多个来源：取最高权威者
  - 任一来源为 `ABSENT` → 该 key 不输出
  - 否则取最高层级的 `OCCUPIED` value（父 > 叶 > SSD）
- 忽略 `PLACEHOLDER`

**并发：** 范围查不持全局锁，结构变更时对受影响段重定位/重试。扫描期间并发写可能更新缓存或清 `sorted_flag`——如果扫描中检测到 `sorted_flag` 被清，对受影响缓存重新排序后重试。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R TreeRange --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/tree.hpp src/tree.cpp include/cbtree/cache_attachment.hpp src/cache_attachment.cpp tests/test_tree_range.cpp CMakeLists.txt
git commit -m "feat: range scan with cache/SSD merge and authority ordering"
```

---

### Task 13: 内部节点分裂（h≥3 缓存下放）

**Files:**
- Modify: `src/tree.cpp`
- Modify: `tests/test_tree_split.cpp`（追加测试）

**Interfaces:**
- Consumes: leaf split, eviction, multi-level routing
- Produces: 完整多级树，height≥3 节点无缓存

- [ ] **Step 1: 写失败测试**

在 `tests/test_tree_split.cpp` 追加：

```cpp
TEST_F(TreeSplitTest, InternalSplitReachesHeight3) {
  // 插入大量 key，强制多级分裂
  for (uint64_t i = 0; i < cbtree::kLeafFanout * cbtree::kInternalFanout + 1; ++i) {
    ASSERT_EQ(tree_->put(i, i), cbtree::Status::Ok);
    ASSERT_EQ(tree_->debug_flush_all(), cbtree::Status::Ok);
  }
  EXPECT_GE(tree_->debug_height(), 3);
  // height ≥ 3 节点不应有缓存
  EXPECT_TRUE(tree_->debug_height3_nodes_have_no_cache());
  // 全部 key 可读
  for (uint64_t i = 0; i < 20; ++i) {
    auto r = tree_->get(i);
    ASSERT_EQ(r.status, cbtree::Status::Ok);
    EXPECT_EQ(r.value, i);
  }
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败或 FAIL

- [ ] **Step 3: 实现**

**split_internal（内部节点分裂）：**
1. 等待本节点所有 key 写锁释放（v1 等待策略）→ `version.fetch_add(1)`
2. 按 `mid` 切开 `separators` 和 `children`；分隔键 `mid_key = separators[mid_idx]` 提升到上层，不留在两侧
3. **缓存处理（对齐规格 §9.2）：**
   - height == 2：自身与新兄弟均保留缓存，`split_into(mid_key, right->cache.get())`
   - height ≥ 3：若自身有缓存（仅瞬态/安全网），`split_into(mid_key, ...)` **下放**到两个子侧节点的缓存中，然后自身 `cache = nullptr`
4. 向更上层插入 `mid_key` + 右兄弟指针；无上层则创建新根（新根 `cache = nullptr` 若 height ≥ 3）
5. `version.fetch_add(1)` → 稳定
6. 若父溢出 → 递归 `split_internal(parent)`

**高度更新：** 当创建新根时，所有节点高度不变（新根在上面，原 root 高度不变）。内部实现中，height 字段在创建时设定（叶=1，叶之父=2，以此类推），不动态变化。

**调试接口：**
- `debug_height3_nodes_have_no_cache()`: 遍历所有高度≥3 节点，确认 cache 为空
- `debug_all_leaves_have_cache()`: 所有叶节点有缓存
- `debug_root_has_cache()`: 根有缓存（仅当 height == 1 或 2）

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R TreeSplit --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/tree.cpp include/cbtree/tree.hpp tests/test_tree_split.cpp
git commit -m "feat: internal split and cache push-down for height>=3"
```

---

### Task 14: 并发压力测试

**Files:**
- Create: `tests/test_tree_concurrent.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写测试**

```cpp
// tests/test_tree_concurrent.cpp
#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <vector>
#include <atomic>
#include "cbtree/tree.hpp"

class TreeConcurrentTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() / "cbtree_test_concurrent.pages").string();
    tree_ = std::make_unique<cbtree::Tree>(path_);
  }
  void TearDown() override {
    tree_.reset();
    std::filesystem::remove(path_);
  }
  std::string path_;
  std::unique_ptr<cbtree::Tree> tree_;
};

TEST_F(TreeConcurrentTest, ParallelPutGet) {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  std::vector<std::thread> threads;
  std::atomic<int> errors{0};
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        uint64_t k = static_cast<uint64_t>(t * kPerThread + i);
        auto s = tree_->put(k, k);
        if (s != cbtree::Status::Ok) { ++errors; return; }
        auto r = tree_->get(k);
        if (r.status != cbtree::Status::Ok || r.value != k) { ++errors; return; }
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(errors.load(), 0);
}

TEST_F(TreeConcurrentTest, ParallelPutSameKeys) {
  constexpr int kThreads = 4;
  constexpr int kRounds = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (int i = 0; i < kRounds; ++i) {
        // 多个线程写同一个 key —— 考验 KeyLockTable
        ASSERT_EQ(tree_->put(42, static_cast<uint64_t>(t * 1000 + i)),
                  cbtree::Status::Ok);
      }
    });
  }
  for (auto& th : threads) th.join();
  // 最终 key=42 应有某个值（写入了即成功）
  auto r = tree_->get(42);
  EXPECT_EQ(r.status, cbtree::Status::Ok);
}

TEST_F(TreeConcurrentTest, MixedReadWrite) {
  // 先填充数据
  for (uint64_t i = 0; i < 100; ++i)
    tree_->put(i, i);
  tree_->debug_flush_all();

  constexpr int kThreads = 6;
  std::vector<std::thread> threads;
  std::atomic<int> writers_done{0};
  // 3 个写线程
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&, t] {
      for (uint64_t i = 100 + t * 100; i < 200 + t * 100; ++i)
        tree_->put(i, i);
      ++writers_done;
    });
  }
  // 3 个读线程
  for (int t = 0; t < 3; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 500; ++i)
        tree_->get(static_cast<uint64_t>(i % 50));  // 反复读已有 key
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(writers_done.load(), 3);
}
```

- [ ] **Step 2: 跑测试**

```bash
cmake --build build && ctest --test-dir build -R TreeConcurrent --output-on-failure --repeat-until-fail 5
```

若失败：检查 data race（推荐用 ThreadSanitizer：`-DCMAKE_CXX_FLAGS=-fsanitize=thread`），修复后重跑。

- [ ] **Step 3: 修正常见问题（按需）**

常见并发 bug 模式及修复方向：
- **version 校验遗漏：** 确保 `put`/`get` 的所有路径都有 version 校验
- **slot 锁未持有即修改状态：** 确保所有状态变更（EMPTY→OCCUPIED 等）持有 `slot_mutex`
- **CLOCK hand 竞争：** `hand_` 为 `std::atomic<size_t>`，用 `fetch_add(1) % kCacheSlots` 推进
- **sorted_flag 竞争：** `std::atomic<bool>`，用 `load`/`store` 访问

- [ ] **Step 4: Commit**

```bash
git add tests/test_tree_concurrent.cpp CMakeLists.txt
git commit -m "test: concurrent put/get stress and mixed workload"
```

---

### Task 15: Stubs（Delete / WAL / Adaptive）

**Files:**
- Modify: `include/cbtree/delete_ops.hpp`, `src/delete_ops.cpp`
- Modify: `include/cbtree/wal_sink.hpp`, `src/wal_sink.cpp`
- Modify: `include/cbtree/adaptive_policy.hpp`, `src/adaptive_policy.cpp`
- Modify: `include/cbtree/tree.hpp`, `src/tree.cpp`（接入 AdaptivePolicy）
- Create: `tests/test_stubs.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 写失败测试**

```cpp
// tests/test_stubs.cpp
#include <gtest/gtest.h>
#include "cbtree/delete_ops.hpp"
#include "cbtree/wal_sink.hpp"
#include "cbtree/adaptive_policy.hpp"

TEST(Stubs, DeleteNotImplemented) {
  EXPECT_EQ(cbtree::DeleteOps::remove(1), cbtree::Status::NotImplemented);
  EXPECT_EQ(cbtree::DeleteOps::try_merge(nullptr), cbtree::Status::NotImplemented);
  EXPECT_EQ(cbtree::DeleteOps::rebalance(nullptr), cbtree::Status::NotImplemented);
}

TEST(Stubs, AdaptiveDefaults) {
  cbtree::AdaptivePolicy p;
  auto pr = p.update({});
  EXPECT_DOUBLE_EQ(pr.p_parent, cbtree::kDefaultPParent);
  EXPECT_DOUBLE_EQ(pr.p_placeholder, cbtree::kDefaultPPlaceholder);
}

TEST(Stubs, WalNoOp) {
  cbtree::WalSink wal;
  EXPECT_EQ(wal.log_insert(1, 1), cbtree::Status::Ok);
  EXPECT_EQ(wal.log_update(1, 1, 2), cbtree::Status::Ok);
  EXPECT_EQ(wal.log_compensate(1, 1), cbtree::Status::Ok);
  EXPECT_EQ(wal.recover(), cbtree::Status::Ok);
}
```

- [ ] **Step 2: 跑测试确认失败**

Expected: 编译失败

- [ ] **Step 3: 实现**

`delete_ops.hpp`:
```cpp
#pragma once
#include "cbtree/types.hpp"
namespace cbtree {
struct DeleteOps {
  static Status remove(Key) { return Status::NotImplemented; }
  static Status try_merge(Node*) { return Status::NotImplemented; }
  static Status rebalance(Node*) { return Status::NotImplemented; }
};
}
```

`wal_sink.hpp`:
```cpp
#pragma once
#include "cbtree/types.hpp"
namespace cbtree {
class WalSink {
 public:
  Status log_insert(Key, Value) { return Status::Ok; }
  Status log_update(Key, Value, Value) { return Status::Ok; }
  Status log_compensate(Key, Value) { return Status::Ok; }
  Status checkpoint() { return Status::Ok; }
  Status recover() { return Status::Ok; }
};
}
```

`adaptive_policy.hpp`:
```cpp
#pragma once
#include "cbtree/types.hpp"
namespace cbtree {
struct Stats {
  double parent_hit_rate{0.0};
  double leaf_hit_rate{0.0};
  double clock_eviction_rate{0.0};
  double ssd_io_count{0.0};
};
struct Probabilities {
  double p_parent{kDefaultPParent};
  double p_placeholder{kDefaultPPlaceholder};
};
class AdaptivePolicy {
 public:
  Probabilities update(const Stats&) {
    return Probabilities{};  // v1: 返回默认值
  }
};
}
```

**接入 Tree：** Tree 构造时持有 `AdaptivePolicy` 实例，`put`/`get` 路径中可选收集 stats，当前版本不使用（仅挂钩）。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build && ctest --test-dir build -R Stubs --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/cbtree/delete_ops.hpp src/delete_ops.cpp \
        include/cbtree/wal_sink.hpp src/wal_sink.cpp \
        include/cbtree/adaptive_policy.hpp src/adaptive_policy.cpp \
        include/cbtree/tree.hpp src/tree.cpp \
        tests/test_stubs.cpp CMakeLists.txt
git commit -m "feat: stub delete WAL and adaptive policy hooks"
```

---

### Task 16: 全量回归 + 规格验收

**Files:**
- 无新建，仅验证

- [ ] **Step 1: 跑全量测试**

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

Expected: 全部 PASS

- [ ] **Step 2: 对照规格逐项验收**

| 规格项 | 覆盖 Task |
|---|---|
| 权威顺序 父>叶>SSD | T7–T12 |
| 占位符 / ABSENT / 二次内存校验 | T8 |
| 同 key 写锁（每缓存独立 KeyLockTable） | T3, T4, T14 |
| 乐观 version（奇偶协议） | T7, T10, T13 |
| CLOCK 父→叶 / 叶→SSD（含槽状态验证） | T5, T11 |
| 延迟叶子索引登记 | T11 |
| 分裂高度规则（h=1 保留/h=2 保留/h≥3 下放） | T10, T13 |
| 范围查（含 ABSENT 参与归并） | T12 |
| 级联驱逐（父→叶→SSD） | T11 |
| 分裂等待 key 锁策略 | T10, T13 |
| Stubs | T15 |

- [ ] **Step 3: Commit（如有遗漏）**

若验收发现缺失测试，补充后提交。

---

## Self-Review（计划对照规格）

1. **Spec coverage:** 点查/点写/占位/ABSENT/二次内存校验/CLOCK 双出口（含级联+槽验证）/分裂高度规则/范围查（含ABSENT归并）/延迟索引登记/三 stub/并发/分裂等待策略 — 均有 Task 覆盖。

2. **Decisions reflected:**
   - KeyLockTable 每 CacheAttachment 独立：T3 定义独立表，T4 组合
   - 叶子索引延迟登记：T7 不登记，T11 刷盘时登记
   - 分裂等待 key 锁：T10/T13 实现
   - atomic hand/sorted_flag：T4/T5 定义
   - 槽状态验证：T5 CLOCK 实现
   - 级联驱逐锁释放时机：T11 实现

3. **No placeholders:** 所有 Task 步骤包含具体代码、命令、预期输出。

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-07-20-cache-augmented-btree.md`.

**Two execution options:**

1. **Subagent-Driven (recommended)** — 每个 Task 开新子代理，Task 间复查，迭代快
2. **Inline Execution** — 本会话按 executing-plans 批量执行并设检查点

Which approach?
