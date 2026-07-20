# 带两级缓存的类 B+ 树存储引擎 — 设计规格

**日期：** 2026-07-20  
**状态：** 已评审（对话确认各节）  
**范围：** 仅设计与接口约定，不含 C++ 实现代码

## 1. 目标与约束

### 1.1 目标

实现一棵**类 B+ 树**索引：中间节点常驻内存；叶子在内存中只存 `key → SSD 地址`；在**叶子**与**叶子的父节点**上挂载小型缓存（`CacheAttachment`），用指纹加速点查，用 CLOCK 管理冷热，并支持高并发点查/点写与范围扫描。

### 1.2 已确认约束

| 项 | 选择 |
|---|---|
| 并发模型 | 单进程多线程，共享内存 |
| Key 语义 | 唯一 key；写 = upsert 覆盖 |
| 点查正确性 | 线性一致 |
| KV 形态 | 小定长；缓存存完整 value；叶子索引存 SSD 地址 |
| 拓扑 | 类 B+（数据只在叶）；**无**叶子 sibling 链表 |
| SSD | 每叶子对应定长页；刷脏覆写或写新页并更新地址 |
| 并发方案 | **B**：乐观 version 读 + 每槽写锁 + 同 key 写锁 |

### 1.3 明确不做（v1）

- 节点删除 / 合并的完整实现（仅接口）
- ARIES 式 WAL 的真实落盘与恢复（仅接口）
- 自适应概率算法本体（仅挂钩，返回常量）
- B-link / 叶子链表
- 范围查询的全局快照隔离

---

## 2. 整体架构

```
Root / 上层内部节点
        │  （无 CacheAttachment）
        ▼
叶子的父节点（内部节点）
  ├─ 分隔键 + 子指针
  ├─ version（乐观读）
  └─ CacheAttachment     ← 小型数组 + 指纹 + 每槽写锁 + CLOCK
        │
        ▼
叶子节点（仅索引）
  ├─ 有序 key → SSD 页地址 / 页内槽
  ├─ version
  └─ CacheAttachment
        │
        ▼
SSD 定长页
```

**方向不变量：** 写自上而下，读自上而下 → 同一 key 在上层缓存中的数据**新于或等于**下层。

**权威顺序（唯一真相优先级）：**

```
父节点 CacheAttachment  >  叶子 CacheAttachment  >  SSD 叶页
```

- 任一上层命中（含「不存在」哨兵，见 §6）即以该结果为准，不再采用下层旧值。
- 叶子索引中的 `key → SSD 地址` 是 SSD 侧定位结构，**不是**比缓存更高的权威。叶子索引**延迟登记**——仅在叶子缓存脏条目刷盘时才添加/更新索引中的 `key → SSD 地址` 映射。新写入若仅存在于缓存（脏），叶子索引中**尚无该项**；点查和范围查通过缓存优先的权威顺序保证正确性，不依赖索引的完整性。
- 点查若曾下探 SSD：返回前必须从叶子父节点起再扫一遍**内存缓存**（§6）；因此即使第一次因竞态「看起来 miss 了缓存」，二次内存读仍可补救到并发写或上层结果。
- **驱逐不丢脏数据：** 父缓存驱逐 = 下推到叶子缓存；叶子缓存驱逐脏条目 = 写入 SSD。不存在「脏数据被丢掉且上下都无副本」的路径。

**缓存挂载与高度：**

| 节点高度（叶=1） | CacheAttachment |
|---|---|
| 1（叶，含最初的根） | 必须有；分裂时保留并在左右叶之间按 mid 切开 |
| 2（叶之父） | 必须有 |
| ≥ 3 | 稳态下不应持有；若分裂时自身仍带着缓存，则**下放**给子节点（按 mid 切开分配），自身不再挂载 |

**初始状态（空树）：** 构造时创建单一根叶节点（height = 1），挂空 `CacheAttachment`，分配一页空白 SSD 定长页（`page_addr` 指向该页）。此时叶子索引为空，缓存为空。

---

## 3. 数据结构

### 3.1 内部节点

- 有序分隔键 + 子指针
- 可选 `high_key`（本节点负责上界，便于乐观读发现走错）
- `version`：奇偶协议（见 §4）
- 高度为 2（孩子为叶子）时：`CacheAttachment*`；高度 ≥ 3 稳态为 `nullptr`（见 §2 高度表）

### 3.2 叶子节点

- 有序 `(key → PageId / 页内位置)`
- `page_addr`（或等价页标识）指向 SSD 定长页
- `version`
- 始终挂 `CacheAttachment*`
- 内存中**不**存完整 value（value 在缓存或 SSD 页内）

### 3.3 CacheAttachment

小型固定容量数组，逻辑上挂在节点上。

**每槽字段：**

| 字段 | 含义 |
|---|---|
| `state` | `EMPTY` / `PLACEHOLDER` / `OCCUPIED` / `ABSENT` |
| `key`, `value` | 定长；`PLACEHOLDER` 时 value 无效；`ABSENT` 表示已确认**不存在**（否定缓存），value 无意义 |
| `fingerprint` | 8–16 bit，点查初筛；冲突后全 key 比较 |
| `dirty` | `OCCUPIED` 且尚未落到 SSD / 完成下推持久化义务 |
| `clock_bit` | CLOCK 引用位 |
| `slot_mutex` | 每槽独立写锁（空位也有） |

**附加：**

- `hand`：CLOCK 扫描指针（`std::atomic`，多线程驱逐共享）
- `sorted_flag`：范围查/分裂前排序后置位；结构变更（插入/删除/下推/分裂切开）清位。（`std::atomic<bool>`，或受节点 `version` 奇偶协议保护——读侧在 version 校验窗口内访问，写侧在 version 奇数临界区内修改。）
- `KeyLockTable`：每个 `CacheAttachment` 持有一张独立的条带锁表（如 64 条 `std::mutex`，`key % 64`），用于同 key 写互斥。**不是** Tree 全局表——每级缓存各自独立，锁排序见 §4.3。

### 3.4 槽状态机

```
EMPTY ──写插入 / 读占位──▶ OCCUPIED(value)  或  PLACEHOLDER
  ▲                            │                      │
  │                            │                      ├─ 写撞占位 / 读回填 value → OCCUPIED
  │                            │                      └─ 读确认不存在 → ABSENT
  │         CLOCK 驱逐         │
  └────────── EMPTY ◀──────────┴── ABSENT 也可被驱逐（否定缓存可丢，只影响性能）
```

- `PLACEHOLDER` 默认不轻易作为 CLOCK 牺牲者；空间极紧时可取消占位（读者回填失败则放弃）。
- 写路径遇到同 key 的 `ABSENT`：视为可覆盖空位，原地改为 `OCCUPIED(value)`（upsert 创建）。
- 读路径命中 `ABSENT`：直接返回「不存在」（权威在上层）。

---

## 4. 并发：锁与 version

### 4.1 三类原语

1. **Node version（奇偶）**  
   - 偶数 = 稳定；奇数 = 结构写进行中  
   - 结构变更（分裂、改子指针、更新 `page_addr` 等）：先置奇 → 修改 → 再置偶（+2）  
   - 乐观读：读前见奇数则重试/等待；读完后 `v1 != v2` 则整次操作重试

2. **Slot mutex**  
   占位、插入、回填、原地 upsert、驱逐时持有

3. **Key write lock**  
   同 key 写互斥；持有期间其他同 key 写**不得**修改当前节点，也**不得**进入其子节点，须等待

### 4.2 读是否拿锁

- 纯 `OCCUPIED` 命中：可 atomic 读 + version；若需改槽状态则短持 `slot_mutex`
- 创建占位、回填：必须持 `slot_mutex`
- 读**不**拿 Key write lock

### 4.3 锁顺序（防死锁）

1. 自上而下获取 Key write lock（先父缓存的 KeyLockTable，再子缓存的 KeyLockTable；禁止先子后父，禁止跨缓存升级）
2. 再取 Slot mutex（同缓存内）
3. 分裂等结构变更走**独立结构临界区**（与 key 锁分离）；v1 策略：**分裂前等待本节点 key 写锁全部释放**（确保无 HELD 的 key 写锁后进入临界区），后续可演进为迁移锁表项。  
   - 结构临界区入口：将 `version` 置奇，此后阻塞新的乐观读（见自旋/等待）

### 4.4 持锁与状态转换

| 转换 | 锁 |
|---|---|
| EMPTY → PLACEHOLDER | slot |
| EMPTY → OCCUPIED（写插入） | slot + key write |
| PLACEHOLDER → OCCUPIED（写撞占位） | slot + key write |
| PLACEHOLDER → OCCUPIED（读回填） | slot |
| OCCUPIED → OCCUPIED（upsert） | slot + key write |
| OCCUPIED → EMPTY（驱逐） | slot；跨节点下推时相关 key 短持 key write |

---

## 5. 写路径

1. 从根向下选路，沿途记录 `version`。
2. 到达叶子父节点时：以概率 `P_parent` 将**本次新插入目标**定为父缓存，否则定为叶子缓存。若树高度为 1（仅根叶，不存在父节点），目标固定为叶子缓存（等价 `P_parent = 0`）。
3. 在向下过程中若缓存中已有同 key（含 `ABSENT`、`PLACEHOLDER`）——无论之前步骤 2 的目标是何级别——立刻**原地 upsert**，置 `dirty`、`clock_bit`，成功返回。步骤 3 优先级高于步骤 2：沿途命中即原地更新，不走到目标缓存。
4. 若撞到同 key 的 `PLACEHOLDER`：写入 value，状态 → `OCCUPIED`，视为写成功。
5. 无同 key：在目标缓存找 `EMPTY` 槽插入；若满则先 CLOCK 驱逐（见 §7）再插入。
6. 持有 Key write lock 期间：其他同 key 写阻塞在本节点，不得进入子树。

**不做：** 因 CLOCK 高而把叶子条目「提升」到父缓存。热 key 留在上层的唯一机制是：曾按 `P_parent` 进入父缓存，且后续写在路径上撞到后原地更新。

---

## 6. 读路径（点查）

1. 自上而下；每经过带缓存的节点做指纹点查，命中后全 key 确认。
2. 命中 `OCCUPIED`：返回 value；并按规则清理相关占位符（若适用）。
3. 命中 `ABSENT`：返回「不存在」。
4. 未命中：整次读最多留下**一个** `PLACEHOLDER`（读上下文维护 `bool has_placed_placeholder` 标志，跨各级缓存共用）；以概率 `P_placeholder` 在当前缓存占空槽。  
   - 若已存在同 key 的空占位（其他读者留下）：**复用**，不再新占（不计入「本读已放 PLACEHOLDER」计数）。
   - 已在本读中放过占位后又在下层遇到空槽：不再新占。
5. 继续向下；在叶子缓存或 SSD 页得到结果（value 或不存在）。  
   - 叶子索引无该 key 且缓存未命中时，视为 SSD 侧暂无该 key；**最终结论仍以二次内存校验为准**（可能并发写只落在上层缓存）。
6. **回填：** 从叶子父节点起向下找占位位置。回填判断：
   - 已被另一读者填好（value 或 `ABSENT`）→ 无事，用现有结果
   - 已被另一写者更新为 `OCCUPIED` → 返回写者的新值
   - 仍是 `PLACEHOLDER` → 写入 value，或写入 **`ABSENT`（不存在）**——与普通命中回填相同，只是内容为否定哨兵
   - 槽已空/被驱逐 → 默认放弃回填
7. **SSD 后二次校验（仅内存）：** 若本次读曾进入 SSD（含「SSD/叶索引显示不存在」），则从叶子父节点起再走一遍**仅内存**路径（父缓存 → 叶子缓存），**不再访问 SSD**。  
   - 内存中有 `OCCUPIED` → 返回该 value（补救并发写）  
   - 内存中有 `ABSENT` → 返回不存在  
   - 否则 → 采用本次 SSD/叶索引结论（value 或不存在）  
   - **二次校验同样遵循乐观读 version 协议**：重扫路径上若节点 version 校验失败，则整次点查重试（回退到步骤 1）。

路径上关键节点 version 校验失败 → 整次点查重试。

---

## 7. CLOCK、下推与刷盘

### 7.1 CLOCK

- 每级 `CacheAttachment` 独立 `hand` + `clock_bit`
- 读命中 / 写 upsert / 回填成功：`clock_bit = 1`
- 需要空位或占用 ≥ 阈值：扫到 bit=1 则清 0 前进；bit=0 为牺牲者

### 7.2 压力传递

**父缓存占用 ≥ `T_parent`：**

1. CLOCK 选最冷 `OCCUPIED` / 可驱逐的 `ABSENT`
2. 若为脏或仍需保留的 value：`下推到叶子缓存`（叶子已有同 key 则上层覆盖）；父槽 → `EMPTY`
3. 不存在「直接丢弃仍需要的脏 value」
4. **级联：** 若下推时目标叶子缓存也已满，先触发叶子驱逐（脏→SSD）腾出空间，再接纳下推条目。级联深度最多一层（父→叶→SSD，不存在叶→叶传递）。

**叶子缓存占用 ≥ `T_leaf`：**

1. 脏的 `OCCUPIED`：必须先写入该叶对应 SSD 定长页（覆写或写新页并更新地址），更新叶子索引，清 `dirty`，再腾槽或保留干净副本
2. 干净的 `OCCUPIED` / `ABSENT`：可直接腾槽（权威已在 SSD，或否定缓存可丢）
3. **驱逐出口只有这两种：父→叶，或叶→SSD**；禁止未落盘就丢弃脏数据

### 7.3 下推/刷盘锁顺序与槽状态验证

**槽状态验证：** CLOCK 扫描识别牺牲者 → 获取该槽 `slot_mutex` → **验证槽状态未变**（key 相同、`dirty` 标志未变、`clock_bit` 仍为 0、状态仍为 `OCCUPIED` 或 `ABSENT`）。若验证失败（并发写在此期间更新了该槽），释放 `slot_mutex` 并重新 CLOCK 扫描。

**锁顺序：**
- 先父后子拿槽锁
- 涉及 key 短持 key write lock（下推期间的 key 一致性保护）
- 修改 `page_addr` / 叶索引走 version 奇偶协议

**锁释放时机（避免持锁范围过大）：**
1. 父→叶下推：从父槽复制数据 → 释放父槽锁 → 在叶子缓存中执行插入（若触发级联叶→SSD，在叶子侧独立完成）
2. 叶→SSD 刷盘：从叶槽复制数据 → 释放叶槽锁 → 写 SSD 页 → 仅在修改叶索引时短暂获取节点级 version 临界区

### 7.4 不变量

- 同 key 同时存在于父、叶缓存时：父侧为**更新或相等**的副本（按写入先后，非 value 数值比较）；`ABSENT` 同理优先于下层旧 value
- 脏数据离开当前缓存的唯一方式：父缓存 → 叶子缓存，或叶子缓存 → SSD

---

## 8. 范围查询

1. 用分隔键确定覆盖 `[L, R]` 的叶子集合及其父节点。
2. 收集相关 `CacheAttachment`（预期数量通常 ≤ ~10）。
3. 对每个缓存：若 `sorted_flag == false`，将 `OCCUPIED` 和 `ABSENT` 条目均按 key 排序并置 flag。（`ABSENT` 虽然不产生输出，但必须在归并中参与以正确抑制下层同 key 数据。）
4. 与各叶 SSD 页内有序记录多路归并。
5. 同 key 多来源优先级：**父缓存 > 叶子缓存 > SSD**（与 §2 权威顺序一致）。若上层为 `ABSENT`，抑制该 key 在下层的输出；若上层为 `OCCUPIED`，采用上层 value。只有上层无记录且下层有记录时，才输出下层 value。

**并发：** 范围查正确性**弱于**点查：结构变更时对受影响段重定位/重试，不提供跨重试的全局快照。并发写可能在扫描期间更新缓存或清 `sorted_flag`——重定位时将以最新状态为准。

**占位符：** 范围扫描忽略 `PLACEHOLDER`（不参与排序，不参与归并）；遇到 `ABSENT` 则该 key 在归并中抑制下层但不输出；不在范围路径上新建占位。

---

## 9. 分裂（Split）

### 9.1 触发

- 叶子：索引项或页负载超过容量
- 内部节点：扇出超过上限
- 可由插入上溢或刷盘放不下触发

### 9.2 按高度处理 CacheAttachment（分裂时）

- **自身高度 = 1（叶，含最初根）：** 保留 `CacheAttachment`；按 `mid` 在左右叶之间切开（含 `OCCUPIED` / `ABSENT` / `PLACEHOLDER`）。
- **自身高度 = 2（叶之父）：** 自身与新兄弟均保留缓存；按 `mid` 切开缓存条目。若无上层则新根高度为 3，**新根不挂缓存**。
- **自身高度 ≥ 3：** 自身不应继续持有缓存；若仍带有 `CacheAttachment`，按 `mid` **下放**给两个子侧节点（分配到高度更低、需要缓存的孩子），然后自身挂载清空为 `nullptr`。

### 9.3 叶子分裂（高度 1）

1. 进入结构临界区，`version → 奇数`
2. 将该叶缓存排序（可设 `sorted_flag`）
3. 选分裂键 `mid`
4. 分配新叶子 `L_right` + 新 SSD 页
5. 按 `mid` 切开：叶索引、缓存槽、SSD 页数据；更新 `page_addr`；**两侧均保留 CacheAttachment**
6. 在父节点插入分隔键 `mid` + 指向 `L_right` 的指针  
   - 若原叶即根：新建根（高度 2），新根挂 `CacheAttachment`（初始可空）；两叶保留各自缓存
   - 若已有父（高度 2）：父缓存**不必**按孩子切开（仍覆盖该父下全部 key 空间）
7. `version → 偶数`；若父溢出则递归分裂父

### 9.4 内部节点分裂

1. `version → 奇数`；分隔键与子指针按 `mid` 切开
2. 按 §9.2 高度规则处理 `CacheAttachment`（切开 / 下放 / 新根不挂）
3. 向更上层插入分隔键；无上层则新根
4. `version → 偶数`

### 9.5 并发

- 乐观读见奇数 version 或指针变化 → 重试
- `PLACEHOLDER` / `ABSENT` 随 key 迁移；回填时 version 失败则整次读重试

### 9.6 不变量

- 分裂后任意 key 仍可通过分隔键唯一下钻到正确叶子
- 缓存条目落在所属节点职责范围内；高度 ≥ 3 节点不持有 `CacheAttachment`
- 上层同 key 仍新于下层（权威顺序见 §2）

---

## 10. 暂缓实现的接口

### 10.1 删除 / 合并

```
Status remove(Key key);
Status try_merge(Node* node);
Status rebalance(Node* node);
```

v1 返回 `NOT_IMPLEMENTED`。

### 10.2 WAL（ARIES 风格 physiological logging）

```
log_insert / log_update / log_compensate / checkpoint / recover
```

粒度：生理日志（页内槽 / 缓存槽变更）。v1 为 no-op sink；崩溃恢复不保证。

### 10.3 自适应概率

```
struct Probabilities { double P_parent; double P_placeholder; };
Probabilities AdaptivePolicy::update(const Stats& stats);
```

`Stats` 建议包含：各级缓存命中率、CLOCK 驱逐率、SSD IO 次数、冷热占比估计。  
v1：返回常量默认值；后续按负载调节 `P_parent`、`P_placeholder`。

---

## 11. 模块边界

| 模块 | 职责 |
|---|---|
| `Tree` | 路由、分裂、version、点查/范围查编排 |
| `CacheAttachment` | 槽数组、指纹、slot 锁、CLOCK、`sorted_flag` |
| `KeyLockTable` | 同 key 写锁 |
| `SsDPageStore` | 定长页分配 / 读写 / 地址 |
| `WalSink` | stub：ARIES API |
| `AdaptivePolicy` | stub：概率调节 |
| `DeleteOps` | stub：删除与合并 |

---

## 12. 测试关注点（设计级，非实现计划）

- 同 key 并发写：互斥与子树阻挡
- 写撞占位 / 读回填与写填充的竞态
- SSD 后仅内存二次校验能看到并发写
- 父→叶下推与叶→SSD 刷盘的 dirty 不变量
- 分裂中乐观读重试；缓存按 `mid` 切开正确性
- 范围查同 key 上层优先；`sorted_flag` 失效时机

---

## 13. 决议记录（对话摘要）

- 并发方案选 **B**（乐观读 + 槽写锁），非 latch-coupling 主路径，也非粗粒度节点 RW 锁
- 拓扑选无叶子链表的类 B+，非 B-link
- SSD 二次校验**不**第二次进盘；用于补救「下盘过程中上层并发写 / 缓存可见性」竞态
- 权威顺序：父缓存 > 叶缓存 > SSD；叶子索引不高于缓存
- 叶子索引**延迟登记**：仅刷盘时添加/更新映射，缓存插入时不登记
- 驱逐出口仅两种：父→叶，或叶→SSD；脏数据不会被直接丢弃
- 热数据留上层：**无** CLOCK 提升，仅 `P_parent` + 路径原地更新
- 范围查不要求与点查同等线性一致
- 分裂按高度处理缓存：h=1 保留；h≥3 下放给子节点
- 查不存在可回填 `ABSENT` 否定缓存，语义与普通回填相同
- **KeyLockTable：** 每 `CacheAttachment` 独立持有一张条带锁表（`std::mutex`），非 Tree 全局表；若未来同 key 竞争异常高，可演进为 per-key 无锁等待队列
- **分裂 key 锁策略（v1）：** 等待本节点所有 key 写锁释放后再进入结构临界区；后续可演进为迁移锁表项
