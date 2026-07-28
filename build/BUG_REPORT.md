# CBTree 并发数据丢失问题记录

## 测试环境
- 系统: Linux 6.17.0-19-generic
- 编译: g++ -std=c++17, 优化构建
- 树配置: kCacheSlots=16, kLeafFanout=32, kInternalFanout=32

---

## 已修复的 Bug

### Bug 1: 分裂后叶指针过期 (Stale Leaf Pointer After Split)
**文件**: `src/tree.cpp`
**影响位置**:
- `evict_parent_if_needed()` (line ~161)
- `put()` Phase 1 中两个重试循环

**原因**: `evict_leaf_if_needed` → `flush_all_chunks` → `split_leaf` 会在叶子分裂后使
原来持有的 `leaf` 指针指向旧的左兄弟节点，而目标 key 可能已移到右兄弟节点。
后续的 `leaf->cache->upsert(victim_key, victim_val)` 将数据写入错误的叶子缓存。

**修复**: 在 eviction 之后通过 `find_leaf_for_key(parent, victim_key)` 重新定位正确的叶子。

### Bug 2: get() 不检查 height-2 内部节点缓存
**文件**: `src/tree.cpp` get() 函数

**原因**: 树高度 >= 3 时，root 没有缓存。但高度为 2 的内部节点（root 的直接子节点）
仍然有缓存。`get()` 跳过了这些节点缓存的检查，导致存在其中的 key 找不到。

**修复**: 在 `get()` 中添加对 `leaf->parent->height == 2 && leaf->parent->cache` 的检查。

### Bug 3: flush_all_chunks 中块指针过期
**文件**: `src/tree.cpp` flush_all_chunks() Phase 2

**原因**: chunk 创建时记录了 `c->leaf` 和 `c->page_id`，但在 flush 时这些指针可能
已经过期（叶子被分裂）。直接使用这些过期指针写入 SSD 会导致数据写入错误页面。

**修复**: 在 flush 时通过 `find_leaf_for_key(root_, key)` 为每个条目实时解析正确的叶子。

### Bug 4: put() Phase 3 缺少版本检查
**文件**: `src/tree.cpp` put() Phase 3

**原因**: `descend_to_leaf` 返回叶子指针后、`upsert` 执行前，叶子可能被并发分裂。
数据可能写入错误的叶子缓存，且未被检测。

**修复**: 在 `upsert` 成功后检查 `leaf->version != leaf_v`，若不匹配则重试。

### Bug 5: put() Phase 1 与 split_internal 竞争根缓存
**文件**: `src/tree.cpp` put() Phase 1

**原因**: `put()` Phase 1 访问 `root_->cache` 时未持有任何锁，而 `split_internal` 可能在
同一时刻通过 `split_into` 修改根缓存（将条目迁移到新节点）。

**修复**: Phase 1 中获取 `tree_mutex_` 的 shared_lock 保护根缓存访问，在调用
`evict_parent_if_needed` 之前释放锁（避免与 split_leaf 需要的 exclusive_lock 死锁）。

### Bug 6: 树结构并发保护缺失
**文件**: `src/tree.cpp`, `include/cbtree/tree.hpp`

**原因**: 多个线程同时遍历和修改树结构（分隔符、子节点指针）时缺乏保护。

**修复**:
- 添加 `std::shared_mutex tree_mutex_`
- `descend_to_leaf`, `find_leaf_for_key` 使用 shared_lock
- `split_leaf`, `split_internal` 使用 exclusive_lock（递归前释放）
- `scan()` 中树遍历使用 shared_lock
- `put()` 和 `get()` 不持有锁（避免 eviction → split 死锁）

---

## 已知但未修复的问题

### 问题: 高并发下约 0.01% 数据丢失
**严重程度**: 低（仅在特定条件下触发）
**影响**: 8 线程、20000+ key 并发写入时，约 0.005%-0.02% 的 key 永久丢失

**触发条件**:
1. 树在并发写入过程中发生分裂（从 height 1→2→3）
2. 高线程数（≥ 4）
3. 非同步模式（sync=off）

**关键观察**:
- 丢失的 key 聚集在线程边界附近（如线程 3 范围 7500-9999 和线程 4 范围 10000-12499 交界处）
- 预热树（预建 height 3 树结构）后并发写入: **0% 丢失**
- 单线程: **100% 干净**（1000+ 次测试零丢失）
- 丢失后立即 re-put + get: 可以找到（存储机制正常）
- get() 重试 10 次 + scan 均失败（真正丢失，非暂时不可见）

**可能原因分析**:
怀疑是 `split_leaf` → `leaf->cache->split_into()` 与并发 `leaf->cache->upsert()`
之间的精细竞争。`split_into` 遍历缓存槽并在 exclusive_lock 保护下迁移条目，
但 Phase 3 中的 `upsert` 不持有 tree_mutex_ 锁。版本号检查（V→V+1→V+2）
理应捕获此竞争，但在极端时序交错下可能存在窗口。

**复现脚本**: `build/stress_big.cc` (8 线程, 20000 key, 20 轮)
**验证脚本**: `build/stress_verify.cc` (在线验证 put 后立即可读)
**诊断脚本**: `build/stress_diag2.cc` (10 次重试 + scan 确认永久丢失)

---

## 吞吐量与 Chunk 链统计

### 吞吐量对比 (10000 key, 并发更新已预热树)

| 模式 | 线程 | 吞吐量 | 相对 |
|------|------|--------|------|
| non-sync | 2T | 803K ops/s | 71x |
| non-sync | 4T | 827K ops/s | 73x |
| non-sync | 8T | 707K ops/s | 62x |
| **sync** | **8T** | **11.4K ops/s** | **1x** |

- Sync 模式因每次 fdatasync (~37ms HDD) 导致吞吐量大幅下降
- Non-sync 模式在 4T 达到峰值，8T 有轻微下降（锁竞争）

### Chunk 链统计 (8T, 10000 key, height=3)

| 指标 | 值 |
|------|-----|
| 峰值长度 | 555 |
| 平均长度 | 245.2 |
| 中位长度 | 241 |
| P90 | 494 |
| P99 | 551 |
| 最终残余 | 0 |

- Chunk 链在高度 3 树下可积累 500+ 个块
- 所有块在 flush_all_chunks 后最终清空
- 较低线程数（2T）下 chunk 链较短（峰值 ~60）

---

## Bug: Buffered I/O 模式段错误 (2026-07-28, 更新 2026-07-28)

**严重程度**: 中→高（DIO 模式也开始触发）

**触发条件**:
1. ~~Buffered I/O 模式（不开启 `--dio`）~~ **DIO 模式也会触发 (2026-07-28 更新)**
2. D 工作负载（latest 分布 + 5% insert）: ≥2 线程 + ≥2 repeat
3. F 工作负载（RMW 50%）: ≥2 repeat（buffered 下 ~60%）

**复现率**: D: 100%（8/8 次，含 DIO），F: ~60%（buffered）

**复现命令**:
```bash
cd /home/u332/ycsb/adapters/cbtree
# D — 稳定 segfault
./ycsb_cbtree_bench -t 4 -r 3 --records 20000 D
# F — 间歇 segfault
./ycsb_cbtree_bench -t 4 -r 3 --records 20000 F
```

**关键观察**:
- ~~DIO 模式完全不触发~~ **2026-07-28 更新: DIO 模式也会触发。** 之前 DIO 不触发可能是因为并发进程数少或有其他同步点。本次测试中 2 进程并发访问同磁盘即可复现
- Buffered 模式快 10-20x → 并发窗口压缩 → 暴露竞态；DIO 慢 IO 充当部分同步但仍不足
- 段错误发生在程序启动早期（日志为空，cbtree_D.log 为 0 字节），在 `Open()` / 首次 `descend_to_leaf()` 阶段
- D 工作负载触发 split（insert 新 key 超出 20k 预加载范围，树高度 4）
- 与已有 "0.01% 数据丢失" 问题可能同源：`split_leaf`/`split_internal` 与并发 `upsert` 之间的版本号竞争

**推测根因**:
`split_leaf` 中版本号协议: leaf->version (V→V+1) → 修改 B-link 指针 → 修改 parent → leaf->version (V+1→V+2)，之后 `unlock()`。但 `evict_to_chunk` 在 `split_leaf` 释放 `tree_mutex_` 后、`split_internal` 递归返回前可能看到不一致的中间状态。

**缓解措施**:
- D 工作负载 buffered 模式: 限制 1 线程
- F 工作负载 buffered 模式: 限制 1 repeat
- 或统一使用 DIO 模式测试
