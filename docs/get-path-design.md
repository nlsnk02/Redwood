# GET Path Design

> 版本: feat/count-min-sketch-admission  
> 文件: `src/tree.cpp:856-1065`

## 整体结构

```
get(k):
  loop (最多 64 次重试):
    1. B-link 无锁下降到叶子节点
    2. 查 cache_A → cache_B → chunk chain (fast path)
    3. 全 miss → CMS 判断冷热 → 放 placeholder → 读 SSD → 填充
    4. 版本校验 → 返回或重试
```

## 详细流程

### 1. B-link 下降

```
descend_to_leaf(k) → leaf + version snapshots
```

- 从 root 出发，`upper_bound` 二分查 child index
- 如果 `k >= child->high_key`，沿 `next_sibling` 右链追赶（并发 split 安全）
- 收集沿途 version 用于后续 ABA 校验

### 2. 三级缓存查找 (fast path)

| 顺序 | 层级 | 操作 | 命中时 |
|------|------|------|--------|
| 1 | cache_A | `lookup(k)` — seqlock 无锁读 | `record_get_hit(true)`, return |
| 2 | cache_B | `lookup(k)` — seqlock 无锁读 | `record_get_hit(true)`, return |
| 3 | chunk chain | `lookup_chunks(k)` | `record_get_hit(true)`, return |

- `lookup()` 是 seqlock 无锁读，不持 mutex
- `absent` 标记也算 hit（negative cache）

### 3. 全 miss → Placeholder 放置

三个缓存都没命中，进入 placeholder 逻辑。

#### 3a. CMS 更新

```
cms_.increment(k)                     // 记录本次读 miss
cms_op_count_++, 每 200k 次 decay(0.5) // 周期性衰减
freq = cms_.estimate(k)
hot  = (freq >= cms_admission_threshold_)  // 默认阈值 3
```

#### 3b. Placeholder 放置策略

```
if (hot):
    try_place(cache_A)           // 无条件放 A
    if 失败 (cache_A 100% 满):
        find_clock_victim(cache_A)  // CLOCK 找一个 victim
        if victim dirty: demote to cache_B
        evict_slot()                // 腾出 Tombstone
        try_place(cache_A) retry   // 重用 Tombstone
    // 无 fallback 到 B

else (cold):
    if Bernoulli(p_placeholder_) 命中:
        try_place(cache_B)           // 概率放 B
        if 失败 (cache_B 100% 满):
            evict_leaf_if_needed()   // evict_to_chunk (写 SSD)
            try_place(cache_B) retry
        // 无 fallback 到 A
    else: 跳过 placeholder
```

#### 3c. `try_place_placeholder` 内部逻辑

```
从 fp % 64 开始线性探测:
  Empty     → break, 记录位置
  Tombstone → 记录位置, 继续探测 (可回收)
  Occupied/Placeholder/Absent:
    fp+key 匹配 → 已存在, 返回
    不匹配     → 继续探测

first_free < 0 → 返回 Status::Full (64 个全满)
否则 → 在 first_free 写入 Placeholder
```

#### 3d. 驱逐路径差异 (当前实现)

```cpp
// Hot key 驱逐 — 直接 CLOCK (跳过 occupied_count 检查)
find_clock_victim(cache_A):
  主循环: 跳过 Empty/Placeholder/Tombstone
          CLOCK 找 clock_bit=0 的 Occupied/Absent
  兜底:   驱逐第一个 Placeholder

// Cold key 驱逐 — 走标准路径
evict_leaf_if_needed(cache_B):
  if occupied_count() <= 51: return  // 早退保护
  evict_to_chunk(): flush dirty → 创建 chunk → 推入 leaf chain
```

### 4. SSD 读取 + Placeholder 填充

```
r = ssd_->get_record(leaf->page_id, k)

if has_placed:
    fill_placeholder(idx, r.value) 或 fill_placeholder_absent(idx)
    if 成功:
        check other_cache  // 并发 put 可能写到另一个 cache
        if other_cache 有更新值 → 用更新值填充
        否则 → 返回 SSD 结果
    if 失败 (race):
        fall through to recheck
```

### 5. Recheck 兜底 (无 placeholder 或 race)

```
依次查:
  cache_B lookup → 命中则 return
  cache_B has_absent → return NotFound
  cache_A lookup → 命中则 return
  cache_A has_absent → return NotFound

都没命中 → 返回 SSD 结果
```

### 6. 版本校验

```
每次 return 前校验 leaf->version == leaf_v
version 变化 → 叶子被分裂 → continue 重试
version 奇数 → 结构改动进行中 → continue 重试
```

## 关键数据结构

| 结构 | 大小 | 说明 |
|------|------|------|
| CacheSlot | 64/leaf | Open-addressing hash table, seqlock 并发读 |
| cache_A | 64 slots | 热缓存 (authority 0), 只放 hot key |
| cache_B | 64 slots | 本地缓存 (authority 1), 所有 key |
| CMS | 4×1024=32KB | Count-Min Sketch, 频率估计 |
| EvictChunk | 256 entries | 批量驱逐缓冲区, lock-free chain |

## 锁开销分析

| 操作 | 锁类型 | 频率 |
|------|--------|------|
| cache lookup | seqlock (无锁读) | 每次 get |
| try_place | key_lock + probe mutex | miss 时 |
| find_clock_victim | per-slot mutex (CLOCK scan) | cache_A 满时 |
| evict_slot | per-slot mutex | 驱逐时 |
| fill_placeholder | key_lock + slot mutex | SSD 读回后 |

## 当前性能特征 (Workload C, p_ph=1.0, 4线程, buffered)

| 指标 | 优化版 | 旧 fallback |
|------|--------|-------------|
| Throughput | 891k ops/s | 1,003k ops/s |
| Hit rate | 60.9% | 61.2% |
| Evict A calls | ~53k/200k ops | N/A |
| Spread | 3% | 5% |

驱逐开销是吞吐量差距 (11%) 的主要原因。
