# B-Tree with Probabilistic Cache Hierarchy — Design Document

> 中间节点全内存 + 叶子节点存 SSD 地址 + 叶节点及父节点挂概率性缓存 + ARIES WAL

---

## 目录

1. [系统概览](#1-系统概览)
2. [数据结构](#2-数据结构)
3. [B-Tree 核心](#3-b-tree-核心)
4. [缓存系统](#4-缓存系统)
5. [SSD 存储层](#5-ssd-存储层)
6. [WAL 与恢复](#6-wal-与恢复)
7. [并发控制](#7-并发控制)
8. [概率性提升与自调节](#8-概率性提升与自调节)
9. [Split / Merge / 范围查询](#9-split--merge--范围查询)
10. [错误处理与边界条件](#10-错误处理与边界条件)
11. [性能考量与写放大控制](#11-性能考量与写放大控制)
12. [模块划分与实现顺序](#12-模块划分与实现顺序)
13. [测试策略](#13-测试策略)

---

## 1. 系统概览

### 1.1 架构分层

```
┌──────────────────────────────────────────────┐
│                 User API Layer                │
│     get / put / delete / range_scan          │
├──────────────────────────────────────────────┤
│              B-Tree Engine                    │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐ │
│  │Internal  │  │  Leaf    │  │  Split /   │ │
│  │  Node    │  │  Node    │  │  Merge     │ │
│  │(in mem)  │  │(in mem)  │  │            │ │
│  └──────────┘  └──────────┘  └────────────┘ │
├──────────────────────────────────────────────┤
│              Cache Subsystem                  │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐ │
│  │  Cache   │  │  Clock   │  │Placeholder │ │
│  │  Array   │  │Eviction  │  │  Manager   │ │
│  └──────────┘  └──────────┘  └────────────┘ │
├──────────────────────────────────────────────┤
│           Concurrency Control                 │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐ │
│  │Key-level │  │Node-level│  │ Slot-level │ │
│  │  Mutex   │  │  RwLock  │  │   Mutex    │ │
│  └──────────┘  └──────────┘  └────────────┘ │
├──────────────────────────────────────────────┤
│            Self-Tuning Module                 │
│     EMA stats → dynamic probability adj.     │
├──────────────────────────────────────────────┤
│         WAL & Recovery                        │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐ │
│  │Log Writer│  │Analysis  │  │  Redo/Undo │ │
│  └──────────┘  └──────────┘  └────────────┘ │
├──────────────────────────────────────────────┤
│              SSD Storage                      │
│  ┌──────────┐  ┌──────────┐  ┌────────────┐ │
│  │ Page     │  │  Slotted │  │   Free     │ │
│  │ Allocator│  │   Page   │  │   List     │ │
│  └──────────┘  └──────────┘  └────────────┘ │
└──────────────────────────────────────────────┘
```

### 1.2 核心数据流

```
写路径 (Write):
  获取 KeyWriterLock[K]
  从根向下遍历 (仅用节点读锁):
    每经过一个有缓存的节点:
      - 命中已有同 key 数据 → 原地更新 SlotLock → 释放锁，返回  ← 热数据留在上层
      - 命中空占位符 → 填入数据 SlotLock → 释放锁，返回
      - 否则继续向下
  达到叶节点:
    【B-Tree 结构层面】若 key 不在叶节点 keys 中:
      - 分配新 SSD 页地址
      - 将 key 插入叶节点 keys + ssd_addrs (可能触发 split)
      - 此时改为持有节点写锁
    【缓存层面】
    以概率 p_promote 选目标 = 父节点缓存
    以概率 1-p_promote 选目标 = 叶节点缓存
    在目标缓存中插入 (K, V)
    若缓存满 → Clock 淘汰
      内部节点淘汰 → 数据下沉到对应叶节点缓存 (无 WAL)
      叶节点淘汰 → 脏数据刷 SSD (记 WAL)
  释放 KeyWriterLock[K]

Delete 路径 (Delete):
  获取 KeyWriterLock[K]
  从根向下遍历 (仅用节点读锁):
    每经过一个有缓存的节点:
      - 命中同 key 数据 → 删除该槽位 (标记 Empty, dirty_count 调整), 记 WAL
      - 命中同 key 占位符 → 删除占位符
      - 继续向下（可能下层还有旧版本，一并清理）
  达到叶节点:
    - 检查叶节点缓存中有无残留
    - 从叶节点 keys 中删除 key → 释放 SSD 页地址到 free_list (结构变更需写锁)
    - 若叶节点 underflow → 触发 merge
  释放 KeyWriterLock[K]

读路径 (Read):
  placeholder_slot: Option<(NodeId, SlotIdx)> = None
  从根向下遍历 (仅用节点读锁):
    每经过一个有缓存的节点:
      指纹快速过滤:
        命中 Data → 回填 placeholder_slot（如有），返回
        命中 Placeholder → 复用此占位符，继续向下
      未命中 && 还没有占位符 && random() < p_placeholder
           && 当前缓存 Placeholder 数量 < CACHE_SIZE/4:
        在此缓存创建占位符
        placeholder_slot = Some(当前槽位)
  达到叶节点: 缓存全部未命中
    检查 B-Tree 结构: key 是否在叶节点 keys 中?
      NO → 主动删除 placeholder_slot（如有），返回 None
      YES → 从 SSD 读取数据 V
    重新遍历路径验证（检查路径各缓存是否有更新的写入）
    若发现更新数据 → 用更新的
    若 placeholder_slot 存在 → 安全回填 (在 placeholder_node 缓存中操作)
    返回 V
```

### 1.3 关键不变量

| 不变量 | 说明 |
|--------|------|
| **上层新鲜性** | 写永远从上到下，因此上层缓存中的数据永远不比下层旧 |
| **占位符唯一性** | 每次读最多创建一个占位符；发现已有占位符则复用 |
| **占位符数量上限** | 每个缓存中 Placeholder 数量 ≤ CACHE_SIZE/4，防止挤满 |
| **验证后回填** | 从 SSD 读完后必须重新遍历路径，确认无并发写入才回填 |
| **锁序** | KeyLock → NodeLock(parent→child) → SlotLock，严格有序 |
| **数据写不需要节点写锁** | 缓存写入由 SlotLock 保护，只用节点读锁；只有结构变更需要节点写锁 |
| **WAL 只记用户操作** | 仅 Put/Delete + Commit + Checkpoint 记 WAL；占位符/淘汰/结构变更不记（通过快照+重放重建） |
| **NodeId(0) 保留** | NodeId(0) 作为空/哨兵值，next_node_id 从 1 起始 |

---

## 2. 数据结构

### 2.1 基础类型

```rust
/// B-Tree 阶数：每个节点最多 ORDER 个子节点，最多 ORDER-1 个键
pub const ORDER: usize = 64;

/// 每个缓存数组的槽位数
pub const CACHE_SIZE: usize = 16;

/// SSD 页大小
pub const PAGE_SIZE: usize = 4096;

/// 节点 ID
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct NodeId(pub u64);

/// SSD 页地址
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SsdAddr(pub u64);

/// 日志序列号
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct Lsn(pub u64);
```

### 2.2 键值类型

```rust
/// K 需满足: Ord + Clone + Hash + Serialize + Deserialize
/// V 需满足: Clone + Serialize + Deserialize
///
/// 默认以 Vec<u8> 为键值，也支持用户自定义类型
```

### 2.3 缓存槽位

```rust
/// 缓存槽位状态
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SlotState {
    /// 空槽，指纹为 0
    Empty = 0,
    /// 读者预留的占位符，等待数据填入
    Placeholder = 1,
    /// 有数据，且与 SSD 一致（clean）
    Clean = 2,
    /// 有数据，已被修改但未刷入 SSD（dirty）
    Dirty = 3,
}

/// 单个缓存槽位
///
/// 指纹 = hash(K) 的低 8 位，0 保留表示 Empty。
/// fingerprint 和 state 打包在同一 AtomicU16 中：
///   bits[7:0]   = fingerprint (0 = Empty)
///   bits[15:8]  = state (SlotState 枚举值)
/// clock_bit 单独 AtomicBool。
/// key/value 在持有 lock 后访问，无需原子化。
pub struct CacheSlot<K, V> {
    /// 键（锁内访问，非原子）
    pub key: Option<K>,
    /// 值（锁内访问，非原子）
    pub value: Option<V>,
    /// 指纹(低8位) | 状态(高8位)，无锁快速路径用 Acquire/Release
    pub meta: AtomicU16,
    /// Clock 引用位
    pub clock_bit: AtomicBool,
    /// 每个槽位的独立锁
    pub lock: parking_lot::Mutex<()>,
}

impl<K, V> CacheSlot<K, V> {
    fn get_fingerprint(&self) -> u8 {
        (self.meta.load(Ordering::Acquire) & 0xFF) as u8
    }
    fn get_state(&self) -> SlotState {
        let s = (self.meta.load(Ordering::Acquire) >> 8) as u8;
        SlotState::from_u8(s)
    }
    fn set_meta(&self, fp: u8, state: SlotState) {
        let val = (fp as u16) | ((state as u16) << 8);
        self.meta.store(val, Ordering::Release);
    }
}
```

**指纹设计细节**：
- `fingerprint = FxHasher::hash(K) & 0xFF`，取低 8 位
- 若结果为 0，改为 1（保证 fingerprint 不为 0 意味着槽位非空）
- 点查时先比对 fingerprint（无锁 AtomicU16 读取），256 个值中约 255/256 的 miss 被快速过滤
- 仅 fingerprint 相等时才获取槽位锁并进行全 key 比较

### 2.4 缓存数组

```rust
/// 缓存数组，挂载在内部节点（仅叶节点父节点）或叶节点上
pub struct CacheArray<K, V> {
    /// 固定大小的槽位数组
    pub slots: Box<[CacheSlot<K, V>; CACHE_SIZE]>,
    /// Clock 算法：当前扫描位置
    pub clock_hand: AtomicUsize,
    /// 已占用槽位数（Empty 以外的状态）
    pub occupied: AtomicUsize,
    /// 脏槽位数（Dirty 状态）
    pub dirty_count: AtomicUsize,
    /// 占位符数量（Placeholder 状态），限制 ≤ CACHE_SIZE/4
    pub placeholder_count: AtomicUsize,
    /// "已排序" 标志，用于范围查询优化
    pub sorted: AtomicBool,
    /// 范围查询排序/扫描时的共享锁；普通写入不获取此锁
    pub sort_lock: parking_lot::RwLock<()>,
}

impl<K: Hash + Eq, V> CacheArray<K, V> {
    /// 快速查找：遍历槽位，先比对 fingerprint
    /// 返回 (slot_idx, 命中的状态) 或 None
    fn quick_find(&self, key: &K) -> Option<(usize, SlotState)>;

    /// 查找并获取槽位锁，进行完整 key 比较
    fn lookup(&self, key: &K) -> LookupResult;

    /// 插入 (K, V)，必要时触发 Clock 淘汰
    fn insert(&self, key: K, value: V) -> InsertResult;

    /// 创建占位符
    fn create_placeholder(&self, key: K) -> Option<usize>;

    /// 将占位符填充为数据
    fn fill_placeholder(&self, idx: usize, value: V) -> FillResult;

    /// 将数据状态从 Dirty 改为 Clean
    fn mark_clean(&self, idx: usize);

    /// Clock 淘汰一个槽位，返回被淘汰的 (key, value, was_dirty)
    fn evict_clock(&self) -> Option<(K, V, bool)>;

    /// 对缓存中的 (key, value) 按 key 排序，设置 sorted 标志
    fn sort(&self);
}
```

### 2.5 B-Tree 节点

```rust
/// B-Tree 节点
pub enum BTreeNode<K, V> {
    Internal(InternalNode<K, V>),
    Leaf(LeafNode<K, V>),
}

/// 内部节点（全部在内存中）
pub struct InternalNode<K, V> {
    /// 分隔键，len = children.len() - 1
    pub keys: Vec<K>,
    /// 子节点 ID
    pub children: Vec<NodeId>,
    /// 父节点 ID（根节点为 None）
    pub parent: Option<NodeId>,
    /// 是否为叶节点的父节点（所有直接子节点都是 Leaf）
    /// 只有 is_leaf_parent 为 true 时才分配 cache
    /// 在 split/merge 以及树高度变化时必须重新计算
    pub is_leaf_parent: bool,
    /// 缓存数组：仅当 is_leaf_parent 为 true 时存在
    pub cache: Option<CacheArray<K, V>>,
    /// 节点级读写锁
    pub lock: parking_lot::RwLock<()>,
}

/// 叶节点（元数据在内存中，实际数据在 SSD）
pub struct LeafNode<K, V> {
    /// 键，len = ssd_addrs.len()
    pub keys: Vec<K>,
    /// 对应值在 SSD 上的页地址
    pub ssd_addrs: Vec<SsdAddr>,
    /// 下一个叶节点 ID（用于范围扫描）
    pub next_leaf: Option<NodeId>,
    /// 上一个叶节点 ID（双向链表，用于反向扫描和 merge）
    pub prev_leaf: Option<NodeId>,
    /// 父节点 ID
    pub parent: Option<NodeId>,
    /// 缓存数组：每个叶节点都有
    pub cache: CacheArray<K, V>,
    /// 上次刷盘时间（用于判断"长时间未刷盘"）
    pub last_flush: Instant,
    /// 节点级读写锁
    pub lock: parking_lot::RwLock<()>,
}
```

### 2.6 SSD 页布局

```
┌────────────────────────────────────────────────┐
│              Page Header (16 bytes)             │
│  ┌──────────┬──────────┬──────────┬──────────┐ │
│  │ checksum │  lsn     │ num_slots│  flags   │ │
│  │  u32     │  u64     │  u16     │  u16     │ │
│  └──────────┴──────────┴──────────┴──────────┘ │
├────────────────────────────────────────────────┤
│           Slot Directory (变长)                 │
│  ┌──────────┬──────────┬───────────┐           │
│  │ offset 0 │ offset 1 │  ...      │           │
│  │  u16     │  u16     │           │           │
│  └──────────┴──────────┴───────────┘           │
├────────────────────────────────────────────────┤
│              Free Space                          │
├────────────────────────────────────────────────┤
│           Slot Data (从页尾向前增长)             │
│  ┌──────────────────┬──────────────────┐        │
│  │  slot N          │  slot N-1        │  ...   │
│  │ [klen][vlen]     │                  │        │
│  │ [key bytes]      │                  │        │
│  │ [value bytes]    │                  │        │
│  └──────────────────┴──────────────────┘        │
└────────────────────────────────────────────────┘
```

```rust
pub struct PageHeader {
    pub checksum: u32,       // CRC32（checksum 字段置 0 后计算）
    pub lsn: u64,            // 最后修改此页的 LSN
    pub num_slots: u16,      // 当前有效槽位数
    pub flags: u16,          // PageFlags
    // = 16 bytes，free_offset 由 slot_offsets[last] 推导，不单独存储
}

bitflags! {
    pub struct PageFlags: u16 {
        const LEAF_PAGE   = 0x0001;  // 叶节点数据页
        const OVERFLOW    = 0x0002;  // 溢出页（大 value）
        const SORTED      = 0x0004;  // 槽位已按 key 排序
    }
}
```

### 2.7 全局引擎结构

```rust
pub struct BTreeEngine<K, V> {
    /// 根节点 ID
    pub root_id: NodeId,

    /// 节点存储（全在内存中）
    pub nodes: DashMap<NodeId, BTreeNode<K, V>>,

    /// 下一个节点 ID
    pub next_node_id: AtomicU64,

    /// SSD 存储
    pub storage: SsdStorage,

    /// WAL 管理器
    pub wal: WalManager<K, V>,

    /// Key 级别的写锁，key → Mutex
    pub key_locks: KeyLockManager<K>,

    /// 自调节统计
    pub stats: Stats,

    /// 自调节概率
    pub tuning: TuningParams,

    /// B-Tree 阶数
    pub order: usize,

    /// 每个缓存的槽位数
    pub cache_size: usize,
}
```

---

## 3. B-Tree 核心

### 3.1 节点查找

```rust
impl<K: Ord, V> BTreeNode<K, V> {
    /// 在当前节点中二分查找 key 所属的子节点索引
    ///
    /// 返回值：
    /// - Found(offset): 键存在于 keys[offset]
    /// - GoTo(child_idx): 应进入 children[child_idx]
    fn search(&self, key: &K) -> SearchResult;
}

enum SearchResult {
    Found(usize),       // keys[usize] == key
    GoTo(usize),        // 应进入 children[usize]
}
```

### 3.2 遍历路径

```rust
/// 从根到叶遍历，返回路径上所有经过的节点 ID（不含根重复）
fn traverse_path(&self, key: &K) -> Vec<NodeId> {
    let mut path = vec![self.root_id];
    loop {
        let node = self.get_node(*path.last().unwrap());
        match &*node {
            BTreeNode::Leaf(_) => break,
            BTreeNode::Internal(n) => {
                let child_idx = match n.search(key) {
                    SearchResult::Found(i) => i + 1,
                    SearchResult::GoTo(i) => i,
                };
                path.push(n.children[child_idx]);
            }
        }
    }
    path
}

/// 从指定内部节点向下找到 key 所属的叶节点
fn find_leaf_descendant(&self, internal_id: NodeId, key: &K) -> NodeId {
    let mut current = internal_id;
    loop {
        let node = self.get_node(current);
        match &*node {
            BTreeNode::Leaf(_) => return current,
            BTreeNode::Internal(n) => {
                let child_idx = match n.search(key) {
                    SearchResult::Found(i) => i + 1,
                    SearchResult::GoTo(i) => i,
                };
                current = n.children[child_idx];
            }
        }
    }
}

/// 获取节点引用
fn get_node(&self, id: NodeId) -> dashmap::Ref<NodeId, BTreeNode<K, V>>;

/// 获取节点缓存引用（如果存在）
fn get_cache(&self, id: NodeId) -> Option<&CacheArray<K, V>>;

/// 检查节点是否有缓存
fn has_cache(&self, id: NodeId) -> bool;

/// 获取叶节点（key 应归属的叶节点）
fn find_leaf(&self, key: &K) -> NodeId {
    self.find_leaf_descendant(self.root_id, key)
}
```

### 3.3 节点 ID 分配

```rust
/// 分配新节点 ID
/// NodeId(0) 保留为哨兵值（表示"无"），从 1 开始分配
fn alloc_node_id(&self) -> NodeId {
    let id = self.next_node_id.fetch_add(1, Ordering::Relaxed);
    assert!(id > 0, "NodeId(0) is reserved");
    NodeId(id)
}
```

### 3.4 根分裂与缓存重新分配

当 B-Tree 高度增加时（根节点触发 split），旧根可能从一个 leaf parent 变为更高层的内部节点。此时需要处理旧根的缓存：

```rust
/// 根分裂（内部节点满时触发）
fn split_root(&self) -> Result<()> {
    let old_root = self.get_node(self.root_id);

    // 1. 创建新根
    let new_root_id = self.alloc_node_id();
    let mut new_root = InternalNode {
        keys: Vec::new(),
        children: vec![self.root_id],
        parent: None,
        is_leaf_parent: old_root.is_leaf_parent(),  // 继承旧根的 leaf-parent 状态
        cache: None,
        lock: parking_lot::RwLock::new(()),
    };

    // 2. 如果旧根原来是 leaf parent（有缓存），需要处理：
    //    - 如果旧根的子节点分裂后仍是 leaf → 旧根保留缓存，还是 leaf parent
    //    - 如果旧根分裂后子节点变成内部节点 → 旧根的缓存数据需要下沉到新的 leaf parents
    if let Some(old_cache) = old_root.take_cache() {
        if !old_root.is_leaf_parent_after_split() {
            // 旧根失去 leaf parent 身份
            // 将缓存中的 dirty 数据下沉到对应的叶节点
            for slot in old_cache.slots.iter() {
                if matches!(slot.state, SlotState::Clean | SlotState::Dirty) {
                    if let (Some(key), Some(value)) = (&slot.key, &slot.value) {
                        let leaf = self.find_leaf_descendant(old_root_id, key);
                        self.cache_insert(leaf, key.clone(), value.clone());
                    }
                }
            }
            new_root.is_leaf_parent = false;
            // 旧根的缓存被释放（不再需要）
        }
        // 如果旧根还是 leaf parent，缓存保留不变
    }

    // 3. 更新父子关系
    old_root.set_parent(new_root_id);
    self.nodes.insert(new_root_id, BTreeNode::Internal(new_root));
    self.root_id = new_root_id;
    Ok(())
}
```

**关键规则**：`is_leaf_parent` 标志在每次 split/merge 后必须重新计算。只有直接子节点是叶节点（或包含叶节点）时才为 true。

### 3.5 插入 / 删除（B-Tree 层面）

标准 B-Tree 插入删除，不在此赘述。特殊之处：

- **插入 ≠ 写入数据**。插入指的是 B-Tree 键结构变化（新增 key→SSD 地址映射）
- **写入数据**指的是在缓存中放入 (K, V)
- **SSD 地址分配**：新 key 在叶节点的 SSD 页中分配一个槽位（slot），
  而非独占整页。若页满则分配新页。多个 key 共享同一 SSD 页槽位。

### 3.6 分裂阈值

```rust
/// 叶节点满：keys.len() >= ORDER
/// 内部节点满：children.len() > ORDER
fn is_full(&self) -> bool;
fn is_underflow(&self) -> bool;  // keys.len() < ORDER/2
```

---

## 4. 缓存系统

### 4.1 缓存存在判定

缓存挂载在两类节点上：
- **所有叶节点**：必有缓存
- **叶节点的直接父节点（is_leaf_parent = true）**：有缓存
- **其他内部节点**：无缓存

**特殊：根为叶节点时**，树只有一层，无父节点。此时 `p_promote` 选择的 `ParentCache` 不存在，
自动回退到叶节点缓存。当树增长到 ≥2 层后，叶父节点自动创建缓存。

```rust
fn resolve_write_target(engine: &BTreeEngine<K, V>, leaf_id: NodeId) -> WriteTarget {
    let leaf = engine.get_node(leaf_id);
    if engine.decide_write_target() == WriteTarget::ParentCache {
        if let Some(parent_id) = leaf.as_leaf().parent {
            if let Some(cache) = engine.get_cache(parent_id) {
                return WriteTarget::ParentCache;
            }
        }
    }
    WriteTarget::LeafCache
}
```

```rust
fn has_cache(node: &BTreeNode<K, V>) -> bool {
    match node {
        BTreeNode::Leaf(_) => true,
        BTreeNode::Internal(n) => n.is_leaf_parent,
    }
}
```

### 4.2 指纹计算

```rust
/// 统一指纹计算：所有缓存操作使用此函数
/// 使用 FxHasher（快速非加密哈希），取低 8 位
/// 0 保留表示 Empty，若结果为 0 则改为 1
fn compute_fingerprint<K: Hash>(key: &K) -> u8 {
    use std::hash::Hasher;
    let mut hasher = rustc_hash::FxHasher::default();
    key.hash(&mut hasher);
    let fp = (hasher.finish() & 0xFF) as u8;
    if fp == 0 { 1 } else { fp }
}
// 注意：crc32 不用于指纹——FxHasher 更快且碰撞率在此场景下可接受
```

### 4.3 缓存查找流程

```rust
fn cache_lookup<K: Hash + Eq, V: Clone>(
    cache: &CacheArray<K, V>, key: &K
) -> LookupResult<K, V> {
    let fp = compute_fingerprint(key);

    // 第一遍：无锁快速扫描 fingerprint（Acquire 保证读到最新写入）
    for (idx, slot) in cache.slots.iter().enumerate() {
        if slot.get_fingerprint() != fp {
            continue;  // ~99.6% 的 miss 在此过滤
        }
        // fingerpint 匹配 → 获取锁做精确检查
        let guard = slot.lock.lock();
        // 锁内双重检查（锁提供了 Acquire 语义）
        if slot.get_fingerprint() == fp && slot.key.as_ref() == Some(key) {
            return match slot.get_state() {
                SlotState::Empty => unreachable!(),
                SlotState::Placeholder => LookupResult::Placeholder(idx),
                SlotState::Clean | SlotState::Dirty =>
                    LookupResult::Data(idx, slot.value.clone()),
            };
        }
    }
    LookupResult::Miss
}
```

### 4.4 缓存插入流程

```rust
fn cache_insert<K: Hash + Eq + Clone, V: Clone>(
    cache: &CacheArray<K, V>,
    key: K,
    value: V,
) -> InsertResult {
    let fp = compute_fingerprint(&key);

    // 先检查是否已存在同 key（更新而非新增）
    match cache_lookup(cache, &key) {
        LookupResult::Data(idx, _) => {
            let mut guard = cache.slots[idx].lock.lock();
            cache.slots[idx].value = Some(value);
            if cache.slots[idx].get_state() == SlotState::Clean {
                cache.slots[idx].set_meta(fp, SlotState::Dirty);
                cache.dirty_count.fetch_add(1, Ordering::Relaxed);
            }
            cache.slots[idx].clock_bit.store(true, Ordering::Release);
            cache.sorted.store(false, Ordering::Release);
            return InsertResult::Updated(idx);
        }
        LookupResult::Placeholder(idx) => {
            let mut guard = cache.slots[idx].lock.lock();
            // 双重检查
            if cache.slots[idx].get_state() == SlotState::Placeholder {
                cache.slots[idx].value = Some(value);
                cache.slots[idx].set_meta(fp, SlotState::Dirty);
                cache.slots[idx].clock_bit.store(true, Ordering::Release);
                cache.dirty_count.fetch_add(1, Ordering::Relaxed);
                cache.placeholder_count.fetch_sub(1, Ordering::Relaxed);
                cache.sorted.store(false, Ordering::Release);
                return InsertResult::FilledPlaceholder(idx);
            }
            // 否则状态已变化，走正常插入
        }
        _ => {}
    }

    // 没有同 key，需要新槽位
    let idx = match find_or_evict_slot(cache) {
        Some(i) => i,
        None => {
            // 强制刷盘，释放脏槽位
            cache.force_flush_dirty()?;
            find_or_evict_slot(cache).expect("force_flush must free at least one slot")
        }
    };

    let mut guard = cache.slots[idx].lock.lock();
    cache.slots[idx].key = Some(key);
    cache.slots[idx].value = Some(value);
    cache.slots[idx].set_meta(fp, SlotState::Dirty);
    cache.slots[idx].clock_bit.store(true, Ordering::Release);
    cache.occupied.fetch_add(1, Ordering::Relaxed);
    cache.dirty_count.fetch_add(1, Ordering::Relaxed);
    cache.sorted.store(false, Ordering::Release);
    InsertResult::Inserted(idx)
}
```

### 4.5 Clock 淘汰算法

```rust
fn evict_clock<K: Hash + Eq + Clone, V: Clone>(
    cache: &CacheArray<K, V>,
) -> Option<(usize, K, Option<V>, bool)> {
    let size = cache.slots.len();
    let start = cache.clock_hand.load(Ordering::Relaxed);
    // 最多扫描两圈确保找到可淘汰槽位（一圈给 clock_bit 清零，一圈淘汰）
    for _ in 0..size * 2 {
        let idx = cache.clock_hand.fetch_add(1, Ordering::Relaxed) % size;
        let slot = &cache.slots[idx];

        let mut guard = slot.lock.lock();

        if slot.get_state() == SlotState::Empty {
            return Some((idx, slot.key.clone().unwrap(), None, false));
        }

        if slot.clock_bit.load(Ordering::Relaxed) {
            slot.clock_bit.store(false, Ordering::Release);
            drop(guard);
            continue;
        }

        // 淘汰此槽位
        let key = slot.key.clone().unwrap();
        let value = slot.value.clone();
        let was_dirty = slot.get_state() == SlotState::Dirty;
        let was_placeholder = slot.get_state() == SlotState::Placeholder;

        if was_placeholder {
            STATS.placeholder_wasted.fetch_add(1, Ordering::Relaxed);
            cache.placeholder_count.fetch_sub(1, Ordering::Relaxed);
        }

        slot.key = None;
        slot.value = None;
        slot.set_meta(0, SlotState::Empty);
        slot.clock_bit.store(false, Ordering::Release);

        cache.occupied.fetch_sub(1, Ordering::Relaxed);
        if was_dirty {
            cache.dirty_count.fetch_sub(1, Ordering::Relaxed);
        }
        cache.sorted.store(false, Ordering::Release);

        return Some((idx, key, value, was_dirty));
    }
    // 所有槽位都被占满（极端情况）—— 此时整个缓存已满无法淘汰
    None
}
```

### 4.6 淘汰后的数据下沉

```rust
/// 内部节点缓存淘汰：将数据下沉到对应的叶节点
/// 注意：淘汰不是状态变更，不记 WAL。
/// 崩溃恢复时，原始 CacheInsert 会被 redo，数据自然回到原位置。
fn evict_downward<K: Ord + Hash + Clone, V: Clone>(
    engine: &BTreeEngine<K, V>,
    internal_id: NodeId,
    key: K,
    value: V,
) {
    // 沿树向下找到目标叶节点
    let leaf_id = engine.find_leaf_descendant(internal_id, &key);

    // 插入叶节点缓存（可能触发叶子节点的 Clock 淘汰）
    let result = engine.cache_insert(leaf_id, key, value);
    // 如果叶节点缓存满 → 触发刷盘
    if result.is_eviction() {
        engine.flush_leaf_cache_if_needed(leaf_id);
    }
}

/// 叶节点缓存淘汰：将脏数据刷入 SSD
/// 注意：必须处理 key 可能已被 split 移到其他叶节点的情况
fn evict_to_ssd<K: Ord + Hash + Clone, V: Clone + Serialize>(
    engine: &BTreeEngine<K, V>,
    leaf_id: NodeId,
    key: K,
    value: V,
) {
    // 定位 SSD 地址 —— 处理 split 导致的 key 迁移
    let addr = {
        let leaf = engine.get_node(leaf_id);
        match leaf.as_leaf().keys.binary_search(&key) {
            Ok(idx) => leaf.as_leaf().ssd_addrs[idx],
            Err(_) => {
                // key 已不在当前叶节点（由于 split），重新查找
                let actual_leaf = engine.find_leaf(&key);
                let actual = engine.get_node(actual_leaf);
                let idx = actual.as_leaf().keys.binary_search(&key)
                    .expect("key must exist somewhere");
                actual.as_leaf().ssd_addrs[idx]
            }
        }
    };

    // 写入 WAL（physiological logging: 页级 redo + before image）
    let mut page = engine.storage.read_page(addr);
    let before_image = page.serialize_slot_range(&key);
    page.upsert_slot(&key, &value);
    let after_image = page.serialize_slot_range(&key);
    page.header.lsn = engine.wal.current_lsn().0;
    page.header.checksum = page.compute_checksum();

    engine.wal.log(LogRecord::SsdPageWrite {
        lsn: engine.wal.next_lsn(),
        ssd_addr: addr,
        before_image,
        after_image,
    });

    engine.storage.write_page(addr, &page);
}
```

### 4.7 占位符机制（简化版，无引用计数）

```rust
/// 读路径中创建占位符（有数量上限）
/// 返回 None 的条件：已有同 key 槽位 | 占位符数量达上限 | 缓存满无法淘汰
fn try_create_placeholder<K: Hash + Clone, V>(
    cache: &CacheArray<K, V>,
    key: &K,
) -> Option<usize> {
    // 占位符数量上限检查
    let max_placeholders = cache.slots.len() / 4;
    if cache.placeholder_count.load(Ordering::Relaxed) >= max_placeholders {
        return None;
    }

    let fp = compute_fingerprint(key);

    // 先检查是否已有同 key 的槽位
    for (idx, slot) in cache.slots.iter().enumerate() {
        if slot.get_fingerprint() == fp && slot.key.as_ref() == Some(key) {
            let state = slot.get_state();
            match state {
                SlotState::Placeholder => {
                    // 已有占位符，复用
                    return Some(idx);
                }
                SlotState::Clean | SlotState::Dirty => {
                    // 已有数据！占位符不需要了，数据已存在
                    return None;
                }
                SlotState::Empty => continue,
            }
        }
    }

    // 需要新槽位——可能失败（缓存满 + clock 无法淘汰）
    let idx = match find_or_evict_slot(cache) {
        Some(i) => i,
        None => {
            // 缓存满且所有槽位 hot → 占位符创建失败，读继续向下
            return None;
        }
    };
    let mut guard = cache.slots[idx].lock.lock();

    cache.slots[idx].key = Some(key.clone());
    cache.slots[idx].value = None;  // 占位符无值
    cache.slots[idx].set_meta(fp, SlotState::Placeholder);
    cache.slots[idx].clock_bit.store(true, Ordering::Release);
    cache.occupied.fetch_add(1, Ordering::Relaxed);
    cache.placeholder_count.fetch_add(1, Ordering::Relaxed);
    cache.sorted.store(false, Ordering::Release);

    Some(idx)
}

/// 主动删除占位符（用于 key 不存在等场景）
fn remove_placeholder(cache: &CacheArray<K, V>, idx: usize) {
    let slot = &cache.slots[idx];
    let mut guard = slot.lock.lock();
    if slot.get_state() == SlotState::Placeholder {
        slot.key = None;
        slot.value = None;
        slot.set_meta(0, SlotState::Empty);
        slot.clock_bit.store(false, Ordering::Release);
        cache.occupied.fetch_sub(1, Ordering::Relaxed);
        cache.placeholder_count.fetch_sub(1, Ordering::Relaxed);
    }
}

/// 回填占位符（含并发安全再验证）
fn fill_placeholder_safely<K: Ord + Hash + Clone, V: Clone>(
    engine: &BTreeEngine<K, V>,
    placeholder_node: NodeId,
    placeholder_idx: usize,
    key: &K,
    value_from_ssd: V,
) -> FillResult {
    // 重新从根遍历路径，检查是否有更新的写入
    // 遍历过程中不访问槽位锁，只做缓存层级的 lookup
    let path = engine.traverse_path(key);
    for node_id in &path {
        if !engine.has_cache(*node_id) {
            continue;
        }
        let cache = engine.get_cache(*node_id).unwrap();

        match cache.lookup(key) {
            LookupResult::Data(_, existing_value) => {
                // 有并发写入的更新数据，不再需要回填
                // 但如果占位符还在且属于当前节点，需要清理
                if *node_id == placeholder_node {
                    let slot = &cache.slots[placeholder_idx];
                    let mut guard = slot.lock.lock();
                    if slot.get_state() == SlotState::Placeholder
                        && slot.key.as_ref() == Some(key)
                    {
                        slot.key = None;
                        slot.value = None;
                        slot.set_meta(0, SlotState::Empty);
                        slot.clock_bit.store(false, Ordering::Release);
                        cache.occupied.fetch_sub(1, Ordering::Relaxed);
                        cache.placeholder_count.fetch_sub(1, Ordering::Relaxed);
                    }
                }
                return FillResult::OverwrittenByNewer(existing_value);
            }
            LookupResult::Placeholder(idx) if *node_id == placeholder_node && idx == placeholder_idx => {
                let slot = &engine.get_cache(placeholder_node).unwrap().slots[placeholder_idx];
                let mut guard = slot.lock.lock();
                if slot.get_state() == SlotState::Placeholder
                    && slot.key.as_ref() == Some(key)
                {
                    slot.value = Some(value_from_ssd.clone());
                    slot.set_meta(slot.get_fingerprint(), SlotState::Clean);
                    slot.clock_bit.store(true, Ordering::Release);
                    engine.get_cache(placeholder_node).unwrap()
                        .placeholder_count.fetch_sub(1, Ordering::Relaxed);
                    STATS.placeholder_filled.fetch_add(1, Ordering::Relaxed);
                    return FillResult::Filled(value_from_ssd);
                } else {
                    return FillResult::AlreadyFilled;
                }
            }
            _ => continue,  // 继续检查更下层
        }
    }
    // 占位符已不存在（被淘汰），返回 SSD 读到的值，不需要回填
    FillResult::PlaceholderGone(value_from_ssd)
}
```

---

## 5. SSD 存储层

### 5.1 页管理器

```rust
pub struct SsdStorage {
    /// 数据文件
    file: std::fs::File,
    /// 文件总页数（file_len / PAGE_SIZE）
    total_pages: AtomicU64,
    /// 空闲页列表（回收的页），使用 crossbeam 或 lock-free 队列减少竞争
    free_list: crossbeam::queue::SegQueue<SsdAddr>,
    /// 页大小
    page_size: usize,
}

impl SsdStorage {
    fn alloc_page(&self) -> SsdAddr {
        if let Some(addr) = self.free_list.pop() {
            return addr;
        }
        let page = self.total_pages.fetch_add(1, Ordering::Relaxed);
        SsdAddr(page)
    }

    fn free_page(&self, addr: SsdAddr) {
        self.free_list.push(addr);
    }

    /// 读取一个页
    fn read_page(&self, addr: SsdAddr) -> SsdPage;

    /// 写入一个页（追加式写入 + checksum 保证原子性）
    /// 不使用 tmp+rename（对文件内偏移更新不适用）
    /// 而是依靠 checksum：写入完成后更新 checksum；
    /// 读取时若 checksum 不匹配，说明写入中断，从 WAL 恢复。
    fn write_page(&self, addr: SsdAddr, page: &SsdPage);

    /// 批量写入（用于叶节点缓存整体刷盘）
    fn write_pages_batch(&self, pages: &[(SsdAddr, SsdPage)]);
}
```

### 5.2 页面格式

```rust
pub struct SsdPage {
    pub header: PageHeader,
    /// 槽位偏移表（从页头向后增长）
    pub slot_offsets: Vec<u16>,
    /// 原始数据区（PAGE_SIZE - 16 字节，Header 已占 16 字节）
    pub data: Box<[u8; PAGE_SIZE - 16]>,
}

impl SsdPage {
    /// 在页中查找 key
    fn find_slot(&self, key: &[u8]) -> Option<(usize, &[u8])>;

    /// 插入/更新一个槽位
    fn upsert_slot(&mut self, key: &[u8], value: &[u8]) -> Result<(), PageFull>;

    /// 删除一个槽位
    fn delete_slot(&mut self, key: &[u8]) -> Result<(), KeyNotFound>;

    /// 校验和
    fn compute_checksum(&self) -> u32;
    fn verify_checksum(&self) -> bool;

    /// 遍历所有槽位
    fn iter_slots(&self) -> impl Iterator<Item = (&[u8], &[u8])>;
}
```

### 5.3 大 Value 处理（溢出页）

```rust
/// 当序列化后的 key+value 超过页的可用空间时：
/// 1. 将 value 分割，存储到溢出页链表中
/// 2. 主槽位存储 [klen][vlen][key][overflow_chain_head_addr]

pub struct OverflowChain {
    /// 溢出页地址链
    pages: Vec<SsdAddr>,
    /// 总 value 字节数
    total_len: usize,
}
```

---

## 6. WAL 与恢复

### 6.1 日志记录类型

```rust
pub enum LogRecordType {
    // === 用户操作（逻辑日志，用于恢复重放）===
    /// Put(key, value) — 完整的写操作
    Put              = 0x01,
    /// Delete(key) — 完整的删除操作
    Delete           = 0x02,

    // === 恢复相关 ===
    /// 提交标记（每条 Put/Delete 后紧跟 Commit）
    Commit           = 0x10,
    /// Checkpoint（节点快照 + 截断标记）
    Checkpoint       = 0xFF,
}
// 设计原则：
// 1. WAL 只记录用户意图（Put/Delete），不记录内部实现细节
// 2. 恢复时：加载节点快照 → 重放 Put/Delete → 重建 B-Tree 结构 + 填充缓存
// 3. B-Tree 结构通过重放操作自然重建，不需要单独的结构日志
// 4. 每个 Put/Delete 是一个原子操作，Commit 标记其完成
// 5. 占位符、缓存淘汰等不记 WAL（内存优化，崩溃后重新开始）

pub struct LogRecord<K, V> {
    pub lsn: Lsn,
    pub record_type: LogRecordType,
    pub key: K,
    pub value: Option<V>,       // Some for Put, None for Delete
    pub checksum: u32,
}

// Commit 记录仅包含 Lsn + Commit 标记，不需要 key/value

pub struct CheckpointRecord {
    pub lsn: Lsn,
    /// 节点快照文件的路径（相对 data/ 目录）
    pub snapshot_file: String,
    /// 节点快照的校验和
    pub snapshot_checksum: u32,
    /// 快照时的节点总数
    pub total_nodes: u64,
    /// 快照时的树高度
    pub tree_height: u32,
}
```

### 6.2 日志格式（磁盘布局）

```
┌──────────────────────────────────────────┐
│           Log Record (变长)               │
│  ┌──────────────────────────────────┐    │
│  │ checksum: u32                    │    │
│  │ record_len: u32                  │    │
│  │ lsn: u64                         │    │
│  │ record_type: u8 (Put=1,Del=2,Commit=0x10,CP=0xFF) │
│  │ key_len: u16                     │    │
│  │ val_len: u32 (0 for Delete)      │    │
│  │ key_bytes: [u8; key_len]         │    │
│  │ val_bytes: [u8; val_len]         │    │
│  └──────────────────────────────────┘    │
│  Commit record: 只有 checksum + lsn + type │
│  Checkpoint record: 含 snapshot 元信息     │
└──────────────────────────────────────────┘
```

### 6.3 WAL 管理器

```rust
pub struct WalManager<K, V> {
    /// WAL 文件
    log_file: std::fs::File,
    /// 当前 LSN（单调递增）
    current_lsn: AtomicU64,
    /// 已 fsync 的 LSN
    flushed_lsn: AtomicU64,
    /// 写缓冲区（批量刷盘减少 fsync）
    buffer: parking_lot::Mutex<Vec<u8>>,
    /// WAL 写锁（保证日志顺序写入）
    write_lock: parking_lot::Mutex<()>,
    /// Checkpoint 间隔（操作数）
    checkpoint_interval: usize,
    /// 上次 checkpoint 以来的操作数
    ops_since_checkpoint: AtomicUsize,
    /// 上次成功的 checkpoint LSN
    last_checkpoint_lsn: AtomicU64,
    _phantom: PhantomData<(K, V)>,
}

// WAL 不维护 active_keys / prev_lsn 链。
// 每个 Put/Delete 是自包含原子操作，Commit 标记其完成。
// 恢复时：跳过无 Commit 的记录即可。
```

### 6.4 WAL 写入协议

```rust
/// WAL 写入协议（简化版）
///
/// 每个 Put/Delete 是一个原子操作：
/// 1. 分配 LSN
/// 2. 写 Put/Delete 记录到 WAL buffer
/// 3. 写 Commit 记录到 WAL buffer
/// 4. 根据 sync_policy 决定是否 fsync
/// 5. 执行实际操作（修改 B-Tree + 缓存）
/// 6. 周期性 checkpoint（刷脏缓存 + 保存节点快照 + 截断 WAL）
impl<K: Serialize, V: Serialize> WalManager<K, V> {
    pub fn log_put(&self, key: &K, value: &V) -> Result<Lsn> {
        let _guard = self.write_lock.lock();
        let lsn = Lsn(self.current_lsn.fetch_add(1, Ordering::Relaxed));

        let put_record = LogRecord {
            lsn,
            record_type: LogRecordType::Put,
            key: key.clone(),
            value: Some(value.clone()),
            checksum: 0,
        };
        let commit_lsn = Lsn(self.current_lsn.fetch_add(1, Ordering::Relaxed));

        // 1. 写 Put + Commit
        self.append_bytes(&put_record.serialize());
        self.append_bytes(&serialize_commit(commit_lsn));

        // 2. 需要时 fsync
        if self.sync_policy() == SyncPolicy::Immediate {
            self.fsync()?;
        }

        // 3. 返回 LSN（但不立即 truncate — 等 checkpoint）
        self.ops_since_checkpoint.fetch_add(1, Ordering::Relaxed);
        Ok(lsn)
    }

    pub fn log_delete(&self, key: &K) -> Result<Lsn> {
        let _guard = self.write_lock.lock();
        let lsn = Lsn(self.current_lsn.fetch_add(1, Ordering::Relaxed));

        let del_record = LogRecord {
            lsn,
            record_type: LogRecordType::Delete,
            key: key.clone(),
            value: None,
            checksum: 0,
        };
        let commit_lsn = Lsn(self.current_lsn.fetch_add(1, Ordering::Relaxed));

        self.append_bytes(&del_record.serialize());
        self.append_bytes(&serialize_commit(commit_lsn));

        if self.sync_policy() == SyncPolicy::Immediate {
            self.fsync()?;
        }

        self.ops_since_checkpoint.fetch_add(1, Ordering::Relaxed);
        Ok(lsn)
    }
}
```

### 6.5 恢复协议（两阶段：快照加载 + WAL 重放）

```rust
/// 恢复协议
///
/// 两阶段（无 Undo，因为无多步事务）：
/// 1. 加载最近的节点快照 → 重建 B-Tree 骨架（含节点结构、SSD 地址映射）
/// 2. 从 checkpoint LSN 起重放 WAL：
///    - 跳过无 Commit 的记录（未完成的操作）
///    - 对有 Commit 的 Put/Delete：在实际 B-Tree + 缓存中重放
///    - 重放后缓存可能非空（直接填入操作涉及的节点缓存）
///
/// 为什么不需要 Undo：
/// - 每个 Put/Delete 是自包含的原子操作
/// - 没有跨 key 的多步事务
/// - 崩溃时未完成的 Put/Delete 没有 Commit → 恢复时跳过即可

impl<K: Deserialize + Ord + Hash + Clone, V: Deserialize + Clone> WalManager<K, V> {
    pub fn recover(&self, engine: &BTreeEngine<K, V>) -> Result<()> {
        // Phase 1: 加载节点快照
        let (snapshot, checkpoint_lsn) = self.load_latest_snapshot()?;
        engine.load_nodes_from_snapshot(&snapshot)?;

        // Phase 2: 重放 WAL
        let mut replayed = 0u64;
        let mut skipped = 0u64;
        let mut reader = self.open_wal_reader_from(checkpoint_lsn)?;

        while let Some(record) = reader.read_next()? {
            match record.record_type {
                LogRecordType::Put => {
                    // 查找下一个记录是否为 Commit
                    let next = reader.peek_next()?;
                    if next.is_commit() {
                        reader.skip_next()?;  // 消费 Commit
                        // 重放 Put：在实际 B-Tree + 缓存中执行
                        engine.replay_put(&record.key, record.value.as_ref().unwrap());
                        replayed += 1;
                    } else {
                        // 无 Commit → 跳过此 Put（未完成的操作）
                        skipped += 1;
                        continue;
                    }
                }
                LogRecordType::Delete => {
                    let next = reader.peek_next()?;
                    if next.is_commit() {
                        reader.skip_next()?;
                        engine.replay_delete(&record.key);
                        replayed += 1;
                    } else {
                        skipped += 1;
                        continue;
                    }
                }
                LogRecordType::Checkpoint => {
                    // 仅记录 checkpoint 位置，不重放
                    continue;
                }
                _ => continue,
            }
        }

        log::info!("Recovery: replayed {} ops, skipped {} incomplete", replayed, skipped);
        Ok(())
    }
}
```

### 6.6 Checkpoint 与节点快照

```rust
/// Checkpoint 流程（阻塞写入，简化但安全）：
///
/// 1. 获取全局 checkpoint 锁（阻塞新的 Put/Delete）
/// 2. 遍历所有节点，将脏缓存刷入 SSD（增量批量写入）
/// 3. 序列化全部 B-Tree 节点元数据到快照文件：
///    - 内部节点：keys, children, parent, is_leaf_parent（不含 cache）
///    - 叶节点：keys, ssd_addrs, parent, next_leaf, prev_leaf（不含 cache）
/// 4. Fsync 快照文件 + tree.db
/// 5. 写 Checkpoint 记录到 WAL（含快照文件路径和校验和）
/// 6. Fsync WAL
/// 7. 更新 superblock（checkpoint_lsn, root_id, next_node_id, total_pages）
/// 8. 释放 checkpoint 锁
/// 9. 截断 WAL（删除 checkpoint 之前的日志段）
fn checkpoint(&self) -> Result<()> {
    let _guard = self.checkpoint_lock.lock();

    // Step 1: 刷脏缓存到 SSD
    for entry in self.engine.nodes.iter() {
        if let BTreeNode::Leaf(leaf) = entry.value() {
            self.engine.flush_leaf_cache_incremental(entry.key().clone())?;
        }
    }

    // Step 2: 序列化节点快照
    let snap_path = format!("data/snapshot_{:016x}.bin", self.current_lsn());
    let snapshot = NodeSnapshot::capture(&self.engine.nodes, self.engine.root_id);
    let snap_bytes = snapshot.serialize();
    let snap_checksum = crc32(&snap_bytes);

    std::fs::write(&snap_path, &snap_bytes)?;

    // Step 3: 写 Checkpoint 到 WAL
    let cp_lsn = Lsn(self.current_lsn.fetch_add(1, Ordering::Relaxed));
    let cp_record = CheckpointRecord {
        lsn: cp_lsn,
        snapshot_file: snap_path.clone(),
        snapshot_checksum: snap_checksum,
        total_nodes: snapshot.node_count,
        tree_height: snapshot.height,
    };
    self.append_bytes(&cp_record.serialize());
    self.fsync()?;

    // Step 4: 更新 superblock
    self.update_superblock(cp_lsn)?;

    // Step 5: 清理旧快照文件（保留最近 2 个）
    self.cleanup_old_snapshots(2)?;

    // Step 6: 截断 WAL
    self.truncate_wal(cp_lsn)?;

    self.last_checkpoint_lsn.store(cp_lsn.0, Ordering::Relaxed);
    self.ops_since_checkpoint.store(0, Ordering::Relaxed);
    Ok(())
}

/// 节点快照：完整记录 B-Tree 结构元数据
pub struct NodeSnapshot {
    pub root_id: NodeId,
    pub next_node_id: u64,
    pub node_count: u64,
    pub height: u32,
    pub internal_nodes: Vec<InternalNodeSnapshot>,
    pub leaf_nodes: Vec<LeafNodeSnapshot>,
}

pub struct InternalNodeSnapshot {
    pub node_id: NodeId,
    pub keys: Vec<Vec<u8>>,          // 序列化的键
    pub children: Vec<NodeId>,
    pub parent: Option<NodeId>,
    pub is_leaf_parent: bool,
}

pub struct LeafNodeSnapshot {
    pub node_id: NodeId,
    pub keys: Vec<Vec<u8>>,
    pub ssd_addrs: Vec<SsdAddr>,
    pub parent: Option<NodeId>,
    pub next_leaf: Option<NodeId>,
    pub prev_leaf: Option<NodeId>,
}
// 注意：快照不包含缓存内容，恢复后所有缓存为空。
// 缓存中的数据要么已刷入 SSD（dirty）要么本来就和 SSD 一致（clean）。
// 
// 恢复后的"上层新鲜性"：
// - 恢复后所有缓存为空 → 所有数据在 SSD
// - 新的读写操作自然重新建立缓存层级
// - 写路径的"原地更新" + 概率提升机制确保恢复后重新收敛到热数据在上层

/// 恢复后重建缓存：
/// WAL 重放时，Put/Delete 操作会在对应节点的缓存中直接创建数据，
/// 因此热点数据在恢复后可能已有部分在缓存中。
/// 非热点数据需要第一次读取时从 SSD 加载。
```



---

## 7. 并发控制

### 7.1 锁层级

```
Level 1 (最外层):  KeyWriterLock[K]     — 全局 Map<Key, Mutex>
Level 2:           NodeLock              — 节点 RwLock (parent → child 序)
Level 3 (最内层):  SlotLock              — 缓存槽位 Mutex
```

**死锁预防**：
- Level 1 在 Level 2 之前获取 → 不会形成 Level1↔Level2 环
- Level 2 按 parent→child 顺序获取 → 树结构保证无环
- Level 3 在持有 Level 2 时获取，释放顺序为 L3→L2→L1

### 7.2 Key 写锁管理

```rust
pub struct KeyLockManager<K: Hash + Eq> {
    locks: DashMap<K, Arc<parking_lot::Mutex<()>>>,
}

impl<K: Hash + Eq + Clone> KeyLockManager<K> {
    pub fn acquire(&self, key: &K) -> KeyLockGuard<K> {
        let arc = self.locks
            .entry(key.clone())
            .or_insert_with(|| Arc::new(parking_lot::Mutex::new(())))
            .value()
            .clone();

        let guard = arc.lock();
        KeyLockGuard {
            key: key.clone(),
            _guard: guard,
            _arc: arc,   // 持有 Arc 引用，防止被 DashMap 回收
        }
    }
}

pub struct KeyLockGuard<K: Hash + Eq> {
    key: K,
    // 持有 Arc 使得内部 Mutex 在 guard 存活期间不会被释放
    _arc: Arc<parking_lot::Mutex<()>>,
    // MutexGuard 借用了 _arc 内的 Mutex；由于 _arc 被持有，借用始终有效
    _guard: parking_lot::MutexGuard<'static, ()>,
}
// Safety: _arc 和 _guard 的声明顺序确保了 drop 顺序正确
// （先 drop _guard 释放锁，再 drop _arc）
```

### 7.3 节点锁获取协议

```rust
/// 数据写路径：只需要读锁（缓存修改由 SlotLock 保护）
fn lock_path_for_data_write(&self, path: &[NodeId]) -> Vec<NodeLockReadGuard> {
    let mut guards = Vec::new();
    for node_id in path {
        let node = self.get_node(*node_id);
        let guard = node.lock.read();
        guards.push(guard);
    }
    guards
}

/// B-Tree 结构变更（split/merge/insert key）：需要写锁
fn lock_path_for_struct_write(&self, path: &[NodeId]) -> Vec<NodeLockWriteGuard> {
    let mut guards = Vec::new();
    for node_id in path {
        let node = self.get_node(*node_id);
        let guard = node.lock.write();
        guards.push(guard);
    }
    guards
}

/// 读路径：读锁
fn lock_path_for_read(&self, path: &[NodeId]) -> Vec<NodeLockReadGuard> {
    let mut guards = Vec::new();
    for node_id in path {
        let node = self.get_node(*node_id);
        let guard = node.lock.read();
        guards.push(guard);
    }
    guards
}
```

### 7.4 并发场景汇总

| 场景 | 锁获取顺序 | 说明 |
|------|-----------|------|
| Write K (数据) | KeyLock[K] → NodeLock(path, read) → SlotLock | 缓存修改由 SlotLock 保护 |
| Write K (新 key,结构) | KeyLock[K] → NodeLock(path, write) → SlotLock | 需插入 B-Tree 键 |
| Read K | NodeLock(path, read) → SlotLock | 无 KeyLock |
| Read K (SSD 回填) | NodeLock(path, read) → SlotLock | 重新遍历验证 |
| Delete K (数据) | KeyLock[K] → NodeLock(path, read) → SlotLock | 仅缓存删除 |
| Delete K (结构) | KeyLock[K] → NodeLock(path, write) → SlotLock | 需从 B-Tree 中移除键 |
| Split leaf | NodeLock(leaf, write) → NodeLock(parent, write) | 结构变更 |
| Range scan [A,B] | NodeLock(涉及节点, read) | 拷贝缓存排序，不阻塞写入 |
| Clock evict (internal→leaf) | SlotLock → NodeLock(target leaf, read) → SlotLock(leaf) | 下沉 |
| Clock evict (leaf→SSD) | SlotLock → WAL write → SSD write | 刷盘 |

### 7.5 写-写冲突

```
T1 写 K: 获取 KeyLock[K] → 遍历 → 写缓存 → 释放 KeyLock[K]
T2 写 K: 等待 KeyLock[K] → T1 释放后获取 → 遍历
  → T2 必然看到 T1 写入的数据（在缓存中命中）→ 原地更新

结果：T2 的写一定基于 T1 之后，不会丢失更新
```

### 7.6 写-读冲突

```
T1 写 K: 获取 KeyLock[K] → 在节点 N 的缓存写入 V1 → 释放
T2 读 K: 遍历到节点 N → 命中 Data(V1) → 返回 V1 ← 读到最新

T1 写 K: 获取 KeyLock[K] → 向下遍历中...
T2 读 K: 遍历路径，未命中缓存 → 从 SSD 读 V_old
T1 写 K: 在叶节点缓存写入 V1（T1 到达叶节点时 T2 已读过）
T2 读 K: SSD 读完后重新遍历验证 → 发现叶节点缓存的 V1 → 用 V1

结果：读者总是看到最新的已提交写入
```

### 7.7 读-读并发（占位符复用）

```
T1 读 K: 在节点 N 创建占位符 P → 继续向下 → SSD
T2 读 K: 在节点 N 发现 P（指纹+key 匹配）→ 复用 P → 继续向下 → SSD
T1 完成 SSD 读 → 重新遍历验证 → 安全回填 P → P 变为 Data(V)
T2 完成 SSD 读 → 重新遍历验证 → 发现 P 已是 Data(V) → 停止回填，返回 V

结果：两个读者都正确获得 V，且占位符被有效利用
```

---

## 8. 概率性提升与自调节

### 8.1 概率定义

```rust
/// 概率值使用 AtomicU64 存储定点数（值 × 10^6），支持无锁读写
pub struct TuningParams {
    pub p_promote: AtomicU64,      // 0 ~ 1_000_000 (0% ~ 100%)
    pub p_placeholder: AtomicU64,
}

impl Default for TuningParams {
    fn default() -> Self {
        Self {
            p_promote: AtomicU64::new(100_000),     // 10%
            p_placeholder: AtomicU64::new(50_000),   // 5%
        }
    }
}

impl TuningParams {
    fn get_p_promote(&self) -> f64 {
        self.p_promote.load(Ordering::Relaxed) as f64 / 1_000_000.0
    }
    fn set_p_promote(&self, val: f64) {
        self.p_promote.store((val * 1_000_000.0) as u64, Ordering::Relaxed);
    }
    fn get_p_placeholder(&self) -> f64 {
        self.p_placeholder.load(Ordering::Relaxed) as f64 / 1_000_000.0
    }
    fn set_p_placeholder(&self, val: f64) {
        self.p_placeholder.store((val * 1_000_000.0) as u64, Ordering::Relaxed);
    }
}
```

### 8.2 决策函数

```rust
impl BTreeEngine<K, V> {
    /// 写入时决定提升到父节点还是写入叶节点
    fn decide_write_target(&self) -> WriteTarget {
        if self.rng.gen_bool(self.tuning.get_p_promote()) {
            WriteTarget::ParentCache
        } else {
            WriteTarget::LeafCache
        }
    }

    /// 读时决定是否创建占位符
    fn should_create_placeholder(&self) -> bool {
        self.rng.gen_bool(self.tuning.get_p_placeholder())
    }
}
```

### 8.3 统计收集

```rust
pub struct Stats {
    // 写入统计
    pub writes_total: AtomicU64,
    pub writes_hit_upper: AtomicU64,     // 在路径上层缓存命中并更新
    pub writes_hit_leaf: AtomicU64,      // 在叶节点缓存命中
    pub writes_miss: AtomicU64,          // 未命中任何缓存（插入新槽位）
    pub writes_promoted: AtomicU64,      // 实际被提升到父节点的写入数
    pub writes_filled_placeholder: AtomicU64, // 写入占位符的次数

    // 读取统计
    pub reads_total: AtomicU64,
    pub reads_hit_upper: AtomicU64,      // 在上层缓存命中
    pub reads_hit_leaf: AtomicU64,       // 在叶节点缓存命中
    pub reads_hit_placeholder: AtomicU64,// 命中占位符（复用）
    pub reads_from_ssd: AtomicU64,       // 从 SSD 读取
    pub reads_reverify_changed: AtomicU64, // SSD 读后重验证发现更新的数据

    // 占位符效率
    pub placeholder_created: AtomicU64,
    pub placeholder_filled: AtomicU64,   // 被回填的占位符
    pub placeholder_wasted: AtomicU64,   // 被淘汰时仍为空的占位符

    // 淘汰统计
    pub evictions_internal: AtomicU64,   // 内部节点淘汰次数
    pub evictions_leaf_to_ssd: AtomicU64,// 叶子节点刷盘次数
    pub evictions_placeholder: AtomicU64,// 占位符被淘汰次数

    // SSD 统计
    pub ssd_reads: AtomicU64,
    pub ssd_writes: AtomicU64,
    pub ssd_write_bytes: AtomicU64,

    // B-Tree 结构统计
    pub splits: AtomicU64,
    pub merges: AtomicU64,

    // EMA 辅助
    pub ema_hit_ratio: AtomicF64,        // 指数移动平均命中率
    pub ema_dirty_ratio: AtomicF64,      // 缓存脏数据比例
}
```

### 8.4 自调节引擎

```rust
/// 每 N 次操作运行一次自调节（如每 10000 次操作）
const TUNING_INTERVAL: u64 = 10_000;

impl BTreeEngine<K, V> {
    fn maybe_tune(&self) {
        let total = self.stats.writes_total.load(Ordering::Relaxed)
                  + self.stats.reads_total.load(Ordering::Relaxed);

        if total % TUNING_INTERVAL != 0 {
            return;
        }

        // 1. 计算缓存命中率的 EMA
        let writes = self.stats.writes_total.load(Ordering::Relaxed) as f64;
        let reads = self.stats.reads_total.load(Ordering::Relaxed) as f64;
        let hits_upper = self.stats.writes_hit_upper.load(Ordering::Relaxed) as f64;
        let hits_leaf = self.stats.writes_hit_leaf.load(Ordering::Relaxed) as f64;
        let reads_upper = self.stats.reads_hit_upper.load(Ordering::Relaxed) as f64;
        let reads_leaf = self.stats.reads_hit_leaf.load(Ordering::Relaxed) as f64;
        let reads_ssd = self.stats.reads_from_ssd.load(Ordering::Relaxed) as f64;

        let write_hit_ratio = if writes > 0.0 {
            (hits_upper + hits_leaf) / writes
        } else { 0.0 };
        let read_hit_ratio = if reads > 0.0 {
            (reads_upper + reads_leaf) / reads.max(1.0)
        } else { 0.0 };

        // EMA 更新
        let alpha = 0.1;
        let old_ema = self.stats.ema_hit_ratio.load(Ordering::Relaxed);
        let new_ema = alpha * (write_hit_ratio + read_hit_ratio) / 2.0
                    + (1.0 - alpha) * old_ema;
        self.stats.ema_hit_ratio.store(new_ema, Ordering::Relaxed);

        // 2. 占位符效率
        let created = self.stats.placeholder_created.load(Ordering::Relaxed) as f64;
        let filled = self.stats.placeholder_filled.load(Ordering::Relaxed) as f64;
        let wasted = self.stats.placeholder_wasted.load(Ordering::Relaxed) as f64;
        let placeholder_efficiency = if created > 0.0 {
            filled / (filled + wasted).max(1.0)
        } else { 0.5 };

        // 3. 调整概率
        //
        // p_promote: 命中率高 → 降低（避免父节点缓存过载）
        //            命中率低 → 提高（更激进地提升热数据）
        let p_promote = clamp(
            0.05 + (1.0 - new_ema) * 0.15,  // 范围 0.05 ~ 0.20
            0.05,
            0.20,
        );

        // p_placeholder: 占位符效率高 → 保持或提高
        //                占位符效率低 → 降低
        let p_placeholder = clamp(
            placeholder_efficiency * 0.10,   // 范围 0.01 ~ 0.10
            0.01,
            0.10,
        );

        self.tuning.set_p_promote(p_promote);
        self.tuning.set_p_placeholder(p_placeholder);
    }
}

fn clamp(val: f64, min: f64, max: f64) -> f64 {
    if val < min { min } else if val > max { max } else { val }
}
```

---

## 9. Split / Merge / 范围查询

### 9.1 叶节点分裂

```rust
fn split_leaf(&self, leaf_id: NodeId) -> Result<()> {
    // 1. 锁序：parent → child（遵守锁层级，防止死锁）
    let leaf = self.get_node(leaf_id);
    let parent_id = leaf.as_leaf().parent
        .expect("leaf must have parent to split (if root is leaf, create new root first)");
    let parent = self.get_node(parent_id);
    let parent_guard = parent.lock.write();
    let leaf_guard = leaf.lock.write();  // 父锁已持有，现在锁子节点

    // 2. 写 WAL（在任何修改之前）
    let split_lsn = self.wal.next_lsn();
    // 注意：split 通过重放原始 Put 来恢复，B-Tree 结构不记 WAL
    // split 期间所有修改被 KeyLock 和 NodeLock 保护

    // 3. 对叶节点缓存排序
    leaf.as_leaf().cache.sort();

    // 4. 将缓存中的 (K, V) 合并入 keys + ssd_addrs
    let merged = merge_keys_with_cache(&leaf.as_leaf().keys,
                                       &leaf.as_leaf().ssd_addrs,
                                       &leaf.as_leaf().cache);

    // 5. 确定分裂点
    let split_idx = ORDER / 2;
    let separator = merged[split_idx].0.clone();

    // 6. 创建新叶节点 + 分配 SSD 页
    let new_leaf_id = self.alloc_node_id();
    let new_ssd_page = self.storage.alloc_page();
    let mut new_leaf = LeafNode {
        keys: merged[split_idx..].iter().map(|(k,_)| k.clone()).collect(),
        ssd_addrs: {
            let mut addrs: Vec<SsdAddr> = merged[split_idx..].iter()
                .map(|(_,a)| *a).collect();
            // 新 key 需要新 SSD 页
            for (i, (k, _)) in merged[split_idx..].iter().enumerate() {
                if leaf.as_leaf().keys.binary_search(k).is_err() {
                    addrs[i] = new_ssd_page;
                }
            }
            addrs
        },
        next_leaf: leaf.as_leaf().next_leaf,
        prev_leaf: Some(leaf_id),
        parent: Some(parent_id),
        cache: CacheArray::new(self.cache_size),
        last_flush: Instant::now(),
        lock: parking_lot::RwLock::new(()),
    };

    // 7. 更新旧叶节点
    leaf.as_leaf().keys.truncate(split_idx);
    leaf.as_leaf().ssd_addrs.truncate(split_idx);
    leaf.as_leaf().next_leaf = Some(new_leaf_id);

    // 8. 重新分配缓存数据
    redistribute_cache(&leaf.as_leaf().cache, &mut new_leaf.cache, &separator);

    // 9. 更新父节点（插入分隔键和新子节点指针）
    self.insert_into_internal(parent_id, separator, new_leaf_id);

    // 10. 如果父节点也满了，递归分裂
    drop(leaf_guard);
    drop(parent_guard);
    if self.get_node(parent_id).is_full() {
        self.split_internal(parent_id)?;
    }

    // 11. 更新后继节点的 prev_leaf
    if let Some(next_id) = new_leaf.next_leaf {
        let next = self.get_node(next_id);
        next.as_leaf().lock.write();
        next.as_leaf().prev_leaf = Some(new_leaf_id);
    }

    // 12. 触发刷盘
    self.flush_leaf_cache(leaf_id);
    self.stats.splits.fetch_add(1, Ordering::Relaxed);
    Ok(())
}
```

### 9.2 分裂时的缓存合并

```rust
/// 将叶节点的 keys+ssd_addrs 与缓存中的数据合并
///
/// 缓存覆盖原则：缓存中的数据比 SSD 更新，若同 key 存在，用缓存中的值
fn merge_keys_with_cache<K: Ord + Clone, V: Clone>(
    keys: &[K],
    ssd_addrs: &[SsdAddr],
    cache: &CacheArray<K, V>,
) -> Vec<(K, SsdAddr)> {
    let mut result: Vec<(K, SsdAddr)> = keys.iter()
        .zip(ssd_addrs.iter())
        .map(|(k, a)| (k.clone(), *a))
        .collect();

    // 缓存中有该 key 的 dirty 数据，说明值已被更新
    // 这种情况下 addr 虽不变，但值已不同（后续 flush 会写入）
    // 合并阶段保留 addr，实际 flush 时再写入最新值
    for slot in cache.slots.iter() {
        let st = slot.get_state();
        if st == SlotState::Dirty || st == SlotState::Clean {
            if let Some(key) = &slot.key {
                if let Err(idx) = result.binary_search_by(|(k, _)| k.cmp(key)) {
                    // 缓存中有新 key，需要分配 SSD 地址
                    let new_addr = STORAGE.alloc_page();
                    result.insert(idx, (key.clone(), new_addr));
                }
                // 如果已存在，保持原有 addr（数据后续 flush 更新）
            }
        }
    }

    result
}
```

### 9.3 范围查询

```rust
fn range_scan<K: Ord + Clone, V: Clone>(
    &self,
    start: &K,
    end: &K,
) -> Result<Vec<(K, V)>> {
    // 1. 定位起始叶节点
    let start_leaf_id = self.find_leaf(start);

    // 2. 收集涉及的节点（HashSet 去重，避免 O(n²)）
    let mut involved_nodes: HashSet<NodeId> = HashSet::new();
    let mut involved_caches: Vec<(NodeId, Vec<(K, V)>)> = Vec::new();

    let mut current = Some(start_leaf_id);
    while let Some(leaf_id) = current {
        let leaf = self.get_leaf(leaf_id);
        if leaf.keys.first().map_or(true, |k| k > end) {
            break;
        }
        involved_nodes.insert(leaf_id);
        if let Some(parent_id) = leaf.parent {
            involved_nodes.insert(parent_id);
        }
        current = leaf.next_leaf;
    }

    // 3. 按 NodeId 排序后依次加锁（确定性顺序，避免死锁）
    let mut sorted_nodes: Vec<NodeId> = involved_nodes.iter().cloned().collect();
    sorted_nodes.sort_by_key(|nid| nid.0);
    let _node_guards: Vec<_> = sorted_nodes.iter()
        .map(|&nid| self.get_node(nid).lock.read())
        .collect();

    // 4. 拷贝缓存内容并排序（不阻塞写入：缓存修改不获取 sort_lock）
    for &node_id in &sorted_nodes {
        if let Some(cache) = self.get_cache(node_id) {
            let mut entries: Vec<(K, V)> = cache.slots.iter()
                .filter(|s| matches!(s.state, SlotState::Clean | SlotState::Dirty))
                .filter_map(|s| {
                    s.key.as_ref().zip(s.value.as_ref())
                        .map(|(k, v)| (k.clone(), v.clone()))
                })
                .collect();
            entries.sort_by(|(a, _), (b, _)| a.cmp(b));
            involved_caches.push((node_id, entries));
        }
    }
    // 对所有拷贝的缓存内容排序
    involved_caches.sort_by(|(_, a), (_, b)| {
        a.first().map(|(k,_)| k).cmp(&b.first().map(|(k,_)| k))
    });

    // 5. 多路归并缓存拷贝 + SSD 数据
    let mut result = Vec::new();
    // ... 归并逻辑：缓存数据优先于 SSD 数据 ...
    result
}
```

### 9.4 空闲页恢复

启动时从节点快照重建 free_list：

```rust
/// 从节点快照重建空闲页列表
fn rebuild_free_list(snapshot: &NodeSnapshot, total_pages: u64) -> Vec<SsdAddr> {
    let mut used: HashSet<SsdAddr> = HashSet::new();

    // 收集所有叶节点使用的 SSD 地址
    for leaf in &snapshot.leaf_nodes {
        for addr in &leaf.ssd_addrs {
            used.insert(*addr);
        }
    }

    // 未使用的页就是空闲页
    let mut free: Vec<SsdAddr> = Vec::new();
    for p in 1..total_pages {  // 页 0 保留
        if !used.contains(&SsdAddr(p)) {
            free.push(SsdAddr(p));
        }
    }
    free
}
```

### 9.5 节点合并

```rust
fn merge_leaves(&self, left_id: NodeId, right_id: NodeId) -> Result<()> {
    let left = self.get_node(left_id);
    let right = self.get_node(right_id);

    // 1. 锁序：parent → left → right
    let parent_id = left.as_leaf().parent.expect("merge requires parent");
    let parent = self.get_node(parent_id);
    let _pg = parent.lock.write();
    let _lg = left.lock.write();
    let _rg = right.lock.write();

    // 2. 合并缓存（左侧缓存优先，因为是上层更新的视角）
    //    将 right 缓存中 left 没有的 key 插入 left 缓存
    for slot in right.as_leaf().cache.slots.iter() {
        if matches!(slot.get_state(), SlotState::Clean | SlotState::Dirty) {
            if let (Some(k), Some(v)) = (&slot.key, &slot.value) {
                if left.as_leaf().cache.lookup(k).is_miss() {
                    left.as_leaf().cache.insert(k.clone(), v.clone());
                }
            }
        }
    }

    // 3. 合并 B-Tree 结构
    left.as_leaf().keys.extend_from_slice(&right.as_leaf().keys);
    left.as_leaf().ssd_addrs.extend_from_slice(&right.as_leaf().ssd_addrs);

    // 4. 更新链表
    left.as_leaf().next_leaf = right.as_leaf().next_leaf;
    if let Some(next_id) = left.as_leaf().next_leaf {
        let next = self.get_node(next_id);
        next.as_leaf().prev_leaf = Some(left_id);
    }

    // 5. 从父节点删除分隔键（找到指向 right 的 child index）
    let separator_idx = parent.as_internal().children.iter()
        .position(|&c| c == right_id).unwrap();
    let sep_key_idx = separator_idx.saturating_sub(1);
    parent.as_internal().keys.remove(sep_key_idx);
    parent.as_internal().children.remove(separator_idx);

    // 6. 如果父节点 underflow → 递归合并
    if parent.as_internal().children.len() < ORDER / 2 {
        drop(_lg); drop(_rg); drop(_pg);
        self.merge_internal_or_rebalance(parent_id)?;
    }

    // 7. 释放 right 节点（不再插入 DashMap，GC 由 remove 处理）
    self.nodes.remove(&right_id);
    self.stats.merges.fetch_add(1, Ordering::Relaxed);
    Ok(())
}
```

---

## 10. 错误处理与边界条件

### 10.1 SSD I/O 错误

```rust
#[derive(Debug, thiserror::Error)]
pub enum StorageError {
    #[error("SSD read error at page {0}: {1}")]
    ReadError(SsdAddr, std::io::Error),

    #[error("SSD write error at page {0}: {1}")]
    WriteError(SsdAddr, std::io::Error),

    #[error("Checksum mismatch at page {0}: expected {1}, got {2}")]
    ChecksumError(SsdAddr, u32, u32),

    #[error("Page full at {0}")]
    PageFull(SsdAddr),

    #[error("Key not found in page {0}")]
    KeyNotFound(SsdAddr),

    #[error("SSD out of space")]
    OutOfSpace,
}
```

**处理策略**：
- 读错误 + 校验和失败 → 从 WAL 恢复该页
- 写错误 → 重试 N 次，仍失败则标记该 SSD 区域为坏块，迁移数据
- 空间不足 → 触发更激进的 merge/compaction

### 10.2 内存不足

```rust
/// 全局缓存内存上限（可配置）
const MAX_CACHE_MEMORY: usize = 1 << 30; // 1GB

/// 当缓存总内存超过上限：
/// 1. 遍历所有叶节点，强制 flush dirty 数据到 SSD
/// 2. 清理 clean 数据（直接丢弃）
/// 3. 若仍不足 → 将最冷叶节点的缓存整体搬入 SSD → 释放缓存数组
fn evict_memory_pressure(&self) {
    // 全局 Clock: 遍历所有节点
    // 优先淘汰 clean，再淘汰 dirty（刷盘后淘汰）
}
```

### 10.3 空树 / 单节点树

```rust
fn ensure_root_exists(&self) -> NodeId {
    if self.root_id == NodeId(0) {
        let leaf = LeafNode::new_empty(self.cache_size);
        let id = self.alloc_node_id();
        self.nodes.insert(id, BTreeNode::Leaf(leaf));
        self.root_id = id;
    }
    self.root_id
}
```

### 10.4 Key 不存在

- Read 时：遍历到叶节点，未在任何缓存或 SSD 中找到 → 返回 `None`
- Delete 时：同理，返回 `NotFound`
- 占位符场景：占位符的 key 后续没有被写入 → 被 clock 淘汰，无副作用

### 10.5 大 Value（超过 SSD 页大小）

```rust
/// 策略：
/// 1. ≤ PAGE_SIZE - header - slot_dir: 直接存储在页内
/// 2. > PAGE_SIZE: 使用溢出链
///    - 主槽位存储 key + overflow_head_addr
///    - 每个溢出页存储 value 的一段 + next_overflow_addr
fn write_large_value(&self, leaf_id: NodeId, key: &K, value: &V) -> Result<SsdAddr> {
    let serialized = bincode::serialize(value)?;
    if serialized.len() <= MAX_INLINE_SIZE {
        // 正常存储
    } else {
        // 溢出链
        let pages: Vec<SsdAddr> = self.split_and_write_overflow(&serialized)?;
        self.write_overflow_pointer(leaf_id, key, &pages)
    }
}
```

### 10.6 关机 / 优雅关闭

```rust
fn shutdown(&self) -> Result<()> {
    // 1. 停止接受新请求
    // 2. 等待所有正在进行的操作完成
    // 3. Flush 所有 dirty 缓存到 SSD
    // 4. 写 checkpoint
    // 5. Fsync WAL
    // 6. 写 superblock（root_id, node_count, checkpoint_lsn）
    // 7. 关闭文件
    Ok(())
}
```

---

## 11. 性能考量与写放大控制

### 11.1 SSD 写放大来源

| 来源 | 说明 | 缓解措施 |
|------|------|---------|
| 缓存刷盘 | 单个 key 修改 → 整页写入 | 批量刷盘：积累多个修改再写 |
| Split | 分裂产生新页 | 分裂时一并 flush 旧页 |
| 小值更新 | 同一 key 频繁更新 → 反复写同一页 | 缓存吸收多次更新 |
| 溢出链 | 大 value 跨多页 | 仅允许 > 阈值时使用 |

### 11.2 批量刷盘策略

```rust
/// 叶节点缓存刷盘：仅写 dirty 槽位
fn flush_leaf_cache_incremental(&self, leaf_id: NodeId) -> Result<()> {
    let leaf = self.get_node(leaf_id);
    let cache = &leaf.as_leaf().cache;

    // 收集所有 dirty 槽位
    let dirty_slots: Vec<(usize, &K, &V)> = cache.slots.iter()
        .enumerate()
        .filter(|(_, s)| s.state == SlotState::Dirty)
        .filter_map(|(i, s)| s.key.as_ref().map(|k| (i, k, s.value.as_ref().unwrap())))
        .collect();

    if dirty_slots.is_empty() {
        return Ok(());
    }

    // 按 SSD 地址分组（同一页的合并一次写入）
    let mut page_updates: HashMap<SsdAddr, Vec<(usize, &K, &V)>> = HashMap::new();
    for (slot_idx, key, value) in &dirty_slots {
        if let Ok(pos) = leaf.as_leaf().keys.binary_search(key) {
            let addr = leaf.as_leaf().ssd_addrs[pos];
            page_updates.entry(addr).or_default().push((*slot_idx, *key, *value));
        }
    }

    // 批量写入每页
    for (addr, updates) in &page_updates {
        let mut page = self.storage.read_page(*addr);
        for (_, key, value) in updates {
            page.upsert_slot(&serialize(key), &serialize(value))?;
        }
        page.header.checksum = page.compute_checksum();
        self.storage.write_page(*addr, &page)?;
    }

    // 标记为 clean
    for (slot_idx, _, _) in &dirty_slots {
        let _guard = cache.slots[*slot_idx].lock.lock();
        cache.slots[*slot_idx].state = SlotState::Clean;
        cache.dirty_count.fetch_sub(1, Ordering::Relaxed);
    }

    leaf.as_leaf().last_flush = Instant::now();
    Ok(())
}
```

### 11.3 定时刷盘

```rust
/// 后台线程：周期性检查
/// - 叶节点缓存 dirty 比例 > 阈值 → 增量刷盘
/// - 叶节点 last_flush 超过时间阈值 → 增量刷盘
fn background_flush_worker(&self) {
    loop {
        std::thread::sleep(Duration::from_millis(FLUSH_CHECK_INTERVAL_MS));
        for entry in self.nodes.iter() {
            if let BTreeNode::Leaf(leaf) = entry.value() {
                let dirty_ratio = leaf.cache.dirty_count.load(Ordering::Relaxed) as f64
                    / leaf.cache.slots.len() as f64;
                let stale = leaf.last_flush.elapsed() > MAX_FLUSH_INTERVAL;

                if dirty_ratio > DIRTY_RATIO_THRESHOLD || stale {
                    self.flush_leaf_cache_incremental(entry.key().clone());
                }
            }
        }
    }
}
```

### 11.4 内存布局优化

```rust
/// 缓存槽位内存布局：关键字段打包 + cache line 对齐
#[repr(C)]
#[repr(align(64))]  // cache line aligned
pub struct CacheSlot<K, V> {
    /// fingerprint(低8) | state(高8)，无锁快速路径读取此字段
    pub meta: AtomicU16,             // 2 bytes
    /// Clock 引用位
    pub clock_bit: AtomicBool,       // 1 byte
    pub _pad: [u8; 5],              // padding to 8 bytes
    /// 键（堆分配，减少槽位大小）
    pub key: Option<Box<K>>,        // 8 bytes (pointer)
    /// 值（堆分配，减少槽位大小）
    pub value: Option<Box<V>>,      // 8 bytes (pointer)
    /// 槽位锁（parking_lot::Mutex 约 1 byte + padding）
    pub lock: parking_lot::Mutex<()>,
}
// ~32 bytes per slot (不含 K,V 堆分配), × 16 slots = ~512 bytes per cache
// 对齐到 64 bytes 减少 false sharing
```

---

## 12. 模块划分与实现顺序

### 12.1 模块树

```
src/
├── main.rs                 # 入口，CLI / 测试入口
├── lib.rs                  # 库根，公开 API
├── error.rs                # 统一错误类型
│
├── btree/
│   ├── mod.rs              # BTreeEngine 主体
│   ├── node.rs             # InternalNode, LeafNode, BTreeNode
│   ├── traverse.rs         # 遍历、查找路径
│   ├── insert.rs           # B-Tree 键插入（结构层面）
│   ├── delete.rs           # B-Tree 键删除（结构层面）
│   ├── split.rs            # 节点分裂
│   └── merge.rs            # 节点合并
│
├── cache/
│   ├── mod.rs              # cache 模块入口
│   ├── array.rs            # CacheArray 实现
│   ├── slot.rs             # CacheSlot、SlotState
│   ├── lookup.rs           # 查找（指纹 + 全 key）
│   ├── insert.rs           # 插入（含更新、占位符填充）
│   ├── clock.rs            # Clock 淘汰算法
│   ├── placeholder.rs      # 占位符创建/回填/安全验证
│   └── sort.rs             # 缓存排序（范围查询用）
│
├── storage/
│   ├── mod.rs              # SsdStorage 主体
│   ├── page.rs             # SsdPage, PageHeader, 页格式
│   ├── allocator.rs        # 页分配/释放
│   ├── overflow.rs         # 大 Value 溢出链
│   └── io.rs               # 底层文件读写
│
├── wal/
│   ├── mod.rs              # WalManager
│   ├── record.rs           # LogRecord 类型定义与序列化
│   ├── writer.rs           # 日志写入（buffer, fsync）
│   ├── recovery.rs         # ARIES 恢复（analysis, redo, undo）
│   └── checkpoint.rs       # Checkpoint 逻辑
│
├── concurrency/
│   ├── mod.rs
│   ├── key_lock.rs         # KeyLockManager
│   └── lock_guard.rs       # RAII lock guards
│
├── tuning/
│   ├── mod.rs              # 自调节入口
│   ├── stats.rs            # Stats 结构
│   └── probability.rs      # 概率计算与调整
│
├── scan/
│   ├── mod.rs
│   └── range.rs            # 范围扫描实现
│
└── engine/
    ├── mod.rs              # 引擎整合
    ├── read.rs             # get 实现（读路径）
    ├── write.rs            # put 实现（写路径）
    ├── delete.rs           # delete 实现
    └── flush.rs            # 缓存放盘与后台 flush
```

### 12.2 实现阶段

| 阶段 | 内容 | 产出物 | 里程碑 |
|------|------|--------|--------|
| **Phase 1** | 基础数据结构 | BTreeNode, CacheArray, SsdPage | 编译通过 |
| **Phase 2** | 单线程 B-Tree | 插入键、查找键、分裂 | 基础结构正确 |
| **Phase 3** | 缓存读写（单线程） | cache insert/lookup/evict/placeholder | 缓存单测全过 |
| **Phase 4** | **WAL + 节点快照** | log put/delete + checkpoint + recovery | **崩溃恢复闭环** |
| **Phase 5** | SSD 读写 + 刷盘 | 页分配/释放/读写/checksum/flush | 数据持久化 |
| **Phase 6** | 完整读写路径（单线程） | read path + write path + promote + delete | 端到端正确 |
| **Phase 7** | 并发控制 | KeyLock, NodeLock, SlotLock | 并发安全 |
| **Phase 8** | 范围查询 + 自适应 + Merge | range scan + tuning + merge | 功能完整 |
| **Phase 9** | 空闲页恢复 + 启动 | free_list 重建 + 从快照启动 | 生产可用 |
| **Phase 10**| 压力测试 + 边界 | 并发测试、故障注入、大 value | 稳定可靠 |

**关键调整**：WAL 和节点快照提前到 Phase 4（先于 SSD 读写），确保后续每个 Phase 的"持久化"都有恢复保障。

---

## 13. 测试策略

### 13.1 单元测试

```rust
// cache/array.rs
#[test] fn test_fingerprint_negative_filter();
#[test] fn test_cache_insert_and_lookup();
#[test] fn test_cache_update_existing();
#[test] fn test_cache_evict_clock_basic();
#[test] fn test_cache_evict_dirty_data();
#[test] fn test_placeholder_create_and_fill();
#[test] fn test_placeholder_shared_by_two_readers();
#[test] fn test_placeholder_wasted_on_evict();
#[test] fn test_cache_sort_and_range();

// btree/node.rs
#[test] fn test_btree_insert_basic();
#[test] fn test_btree_insert_split_leaf();
#[test] fn test_btree_insert_split_internal();
#[test] fn test_btree_delete_and_merge();
#[test] fn test_btree_find_leaf();

// storage/page.rs
#[test] fn test_page_serialize_roundtrip();
#[test] fn test_page_checksum();
#[test] fn test_page_upsert_and_delete();
#[test] fn test_page_overflow_chain();
```

### 13.2 集成测试

```rust
// tests/integration.rs
#[test] fn test_end_to_end_put_get();
#[test] fn test_large_batch_insert_read();
#[test] fn test_concurrent_reads_same_key();
#[test] fn test_concurrent_writes_different_keys();
#[test] fn test_concurrent_writes_same_key();    // KeyLock 正确性
#[test] fn test_read_during_write_same_key();    // 读到最新版本
#[test] fn test_write_fills_placeholder();       // 占位符被写入命中
#[test] fn test_ssd_reverify_on_concurrent_write(); // SSD 读后重验证
#[test] fn test_range_scan_with_cached_data();
#[test] fn test_crash_recovery_basic();
#[test] fn test_crash_recovery_during_split();
#[test] fn test_flush_and_restart();
```

### 13.3 并发压力测试

```rust
// stress/concurrent_stress.rs
#[test] fn stress_concurrent_readers_writers();   // N 读者 + M 写者
#[test] fn stress_hot_key_contention();           // 大量并发写同一 key
#[test] fn stress_cache_eviction_under_load();    // 缓存满 + 持续写入
#[test] fn stress_split_under_concurrency();      // 并发导致频繁分裂
#[test] fn stress_ssd_full();                     // SSD 空间压力
```

### 13.4 故障注入测试

```rust
// chaos/mod.rs
#[test] fn chaos_ssd_read_error();                // 模拟 SSD 读错误
#[test] fn chaos_ssd_write_error_mid_operation(); // 模拟中途写错误
#[test] fn chaos_kill_process_mid_write();        // 进程被杀 → 重启恢复
#[test] fn chaos_kill_process_mid_split();        // 分裂中崩溃 → 恢复
#[test] fn chaos_kill_process_mid_flush();        // 刷盘中崩溃 → 恢复
#[test] fn chaos_checksum_corruption();           // 校验和损坏 → 恢复
```

### 13.5 性能基准测试

```rust
// benches/cache_bench.rs
#[bench] fn bench_cache_lookup_hit();
#[bench] fn bench_cache_lookup_miss();
#[bench] fn bench_cache_insert();
#[bench] fn bench_cache_evict();

// benches/btree_bench.rs
#[bench] fn bench_sequential_insert();
#[bench] fn bench_random_insert();
#[bench] fn bench_random_read();
#[bench] fn bench_hot_key_read();         // 热 key 始终在缓存中
#[bench] fn bench_cold_key_read();        // 冷 key 需要 SSD I/O
#[bench] fn bench_range_scan();

// benches/concurrency_bench.rs
#[bench] fn bench_parallel_reads();
#[bench] fn bench_parallel_writes();
```

---

## 附录 A：配置参数汇总

| 参数 | 默认值 | 范围 | 说明 |
|------|--------|------|------|
| `ORDER` | 64 | 16~256 | B-Tree 阶数 |
| `CACHE_SIZE` | 16 | 4~64 | 每个缓存数组的槽位数 |
| `PAGE_SIZE` | 4096 | 512~65536 | SSD 页大小 |
| `p_promote` | 0.10 | 0.01~0.30 | 写入父节点缓存概率 |
| `p_placeholder` | 0.05 | 0.01~0.15 | 创建占位符概率 |
| `TUNING_INTERVAL` | 10000 | 1000~100000 | 自调节间隔（操作数） |
| `EMA_ALPHA` | 0.1 | 0.01~0.3 | 指数移动平均系数 |
| `DIRTY_RATIO_THRESHOLD` | 0.7 | 0.3~0.9 | 触发增量刷盘的脏数据比例 |
| `MAX_FLUSH_INTERVAL` | 5s | 1s~60s | 强制刷盘时间间隔 |
| `CHECKPOINT_INTERVAL` | 50000 | 1000~1000000 | Checkpoint 间隔（日志条数） |
| `MAX_CACHE_MEMORY` | 1GB | 64MB~无上限 | 全局缓存内存上限 |
| `WAL_BUFFER_SIZE` | 1MB | 64KB~16MB | WAL 写缓冲区大小 |
| `SYNC_POLICY` | Batch | Immediate/Batch/Periodic | WAL 同步策略 |

## 附录 B：文件格式

```
data/
├── tree.db              # SSD 数据文件（页数组）
├── wal.log              # WAL 日志文件
├── wal.log.1            # 归档 WAL（轮转后）
├── snapshot_<lsn>.bin   # 节点快照文件（Checkpoint 时生成，保留最近 2 个）
├── snapshot_<lsn>.bin   # 上一个快照（恢复时优先选用最新）
└── superblock           # 超级块（元数据）
    ├── magic: u32       # 魔数 0x42545245 ("BTRE")
    ├── version: u32     # 格式版本
    ├── root_id: u64     # 根节点 ID（0 表示空树，首次 put 时创建）
    ├── next_node_id: u64# 下一个节点 ID（从 1 起始，NodeId(0) 为哨兵）
    ├── total_pages: u64 # SSD 总页数
    ├── checkpoint_lsn: u64
    ├── latest_snapshot: [u8; 256] # 最新快照文件名（用于恢复）
    ├── checksum: u32
    └── created_at: u64  # Unix 时间戳
```
