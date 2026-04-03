# Dstore LSN 全景分析

## 1. 概述

### 1.1 PLSN 与 GLSN 的区别

Dstore 中存在两种 LSN 体系：

| 类型 | 全称 | 类型定义 | 作用域 | 说明 |
|------|------|---------|--------|------|
| **PLSN** | Physical LSN | `uint64` / `WalPlsn`（`uint64_t`） | 单个 WalStream 内 | 表示 WAL 日志在物理文件中的字节偏移位置，在单个 WalStream 内单调递增。是 WAL 回放、脏页刷写、checkpoint 等机制的核心定位依据 |
| **GLSN** | Global LSN | `uint64` | 跨 WalStream 全局 | 全局逻辑序列号，用于跨 WalStream 的顺序排序。在分布式/多 WalStream 场景下保证全局有序性 |
| **WalId** | WAL Stream ID | `uint64` / `uint16` | 标识 WalStream | PLSN 只在同一 WalId 内有意义，不同 WalId 的 PLSN 不可比较 |

**关键常量**：
- `INVALID_PLSN = 0`：无效 PLSN
- `INVALID_END_PLSN = UINT64_MAX`：无效结束 PLSN
- `INVALID_WAL_GLSN = UINT64_MAX`：无效 GLSN
- `INVALID_WAL_ID = UINT64_MAX`：无效 WAL ID

### 1.2 PageVersion 结构

每个页面通过 `PageVersion` 记录其当前版本：

```cpp
struct PageVersion {
    uint64 glsn;  // 页面最后修改的 GLSN
    uint64 plsn;  // 页面最后修改的 PLSN
};
```

页面头部（`PageHeader`）中存储 `m_glsn`、`m_plsn`、`m_walId`，通过 `SetLsn()` 原子更新，并有 `LsnSanityCheck()` 校验新 LSN 必须大于当前 LSN。

---

## 2. LSN 分类详解

### 2.1 WAL 写入流水线 LSN

这些 LSN 反映一条 WAL 记录从产生到持久化的流水线进度：

| LSN 名称 | 所属类/结构 | 字段 | 含义 | 重要性 |
|----------|-----------|------|------|--------|
| **maxAppendedPlsn** | `WalStream` | `GetMaxAppendedPlsn()` | WAL 缓冲区中已追加的最大连续 PLSN（所有数据已拷贝到内存缓冲区） | **核心** — 代表写入进度的上界 |
| **maxContinuousPlsn** | `WalStreamBuffer` | `m_maxContinuousPlsn` | WAL 缓冲区中最大连续已完成拷贝的 PLSN（所有小于此值的数据都已写入缓冲区，可供刷写） | **核心** — 控制刷写线程可读范围 |
| **maxWrittenToFilePlsn** | `WalStream` | `m_maxWrittenToFilePlsn` / `GetMaxWrittenToFilePlsn()` | 已写入 WAL 文件但可能尚未 fsync 的最大 PLSN | 中等 — 写入进度 |
| **maxFlushFinishPlsn** | `WalStream` | `m_maxFlushFinishPlsn` / `GetMaxFlushedPlsn()` | 已持久化到磁盘（fsync 完成）的最大 PLSN | **核心** — 持久化保证的边界 |
| **maxFlushedPlsn** | `WalBufferCtlData` | `maxFlushedPlsn` | WAL 缓冲区层面记录的已刷新 PLSN | 内部 |
| **lastScannedPlsn** | `WalStreamBuffer` | `m_lastScannedPlsn` | 后台刷写线程上次扫描到的 PLSN 位置 | 内部 |

**流水线关系**：
```
maxAppendedPlsn >= maxContinuousPlsn >= maxWrittenToFilePlsn >= maxFlushFinishPlsn
```

如果 `maxAppendedPlsn` 远大于 `maxFlushFinishPlsn`，说明 WAL 刷写存在积压。

### 2.2 Checkpoint 相关 LSN

| LSN 名称 | 所属类/结构 | 字段 | 含义 | 重要性 |
|----------|-----------|------|------|--------|
| **lastCheckPointRecoveryPlsn** | `WalCheckpointInfoData` | `lastCheckPointRecoveryPlsn` | 上次 checkpoint 时记录的恢复起点 PLSN | **核心** — 崩溃恢复从此处开始回放 |
| **lastCheckpointPLsn** | `WalStreamInfoData`（控制文件） | `lastCheckpointPLsn` | 上次 checkpoint WAL 记录的起始位置 | **核心** — 定位 checkpoint 记录本身 |
| **diskRecoveryPlsn** | `WalCheckPoint` | `diskRecoveryPlsn` | checkpoint 时所有脏页已刷到磁盘的最小 PLSN，崩溃恢复可从此处开始 redo | **核心** — 恢复起点 |
| **memRecoveryPlsn** | `MemoryCheckpoint` | `memRecoveryPlsn` | 远程内存节点上已刷新的恢复 PLSN（分布式场景，单节点可忽略） | 低 |
| **maxFlushedPlsn** | `WalCheckpointStatInfo` | `maxFlushedPlsn` | checkpoint 统计中的最大已刷 PLSN | 诊断用 |
| **maxAppendedPlsn** | `WalCheckpointStatInfo` | `maxAppendedPlsn` | checkpoint 统计中的最大已追加 PLSN | 诊断用 |

### 2.3 脏页队列 / Buffer LSN

| LSN 名称 | 所属类/结构 | 字段 | 含义 | 重要性 |
|----------|-----------|------|------|--------|
| **recoveryPlsn**（Buffer） | `BufferDesc` | `recoveryPlsn[N]` | 每个 BufferDesc 关联的恢复 PLSN 数组（每个 WAL stream 槽位一个），记录该脏页被推入脏页队列时的 recoveryPlsn | **核心** — checkpoint 依赖此值确定最小恢复点 |
| **recoveryPlsn**（队列） | `DirtyPageQueue` / `QueueInfo` | `recoveryPlsn` | 脏页队列的整体最小恢复 PLSN | **核心** — `GetMinRecoveryPlsn()` 返回 |
| **recoveryPlsn**（Master Writer） | `BgDiskPageMasterWriter` | `m_recoveryPlsn` | Master Writer 当前管理的恢复 PLSN | 中等 |
| **pageVersionOnDisk** | `BufferDesc` | `pageVersionOnDisk` | 页面已刷到磁盘的 PageVersion（glsn + plsn），用于检测 missing dirty | 诊断用 |

### 2.4 Recovery（恢复）相关 LSN

| LSN 名称 | 所属类/结构 | 字段/方法 | 含义 | 重要性 |
|----------|-----------|----------|------|--------|
| **recoveryStartPlsn** | `WalRecovery` | `m_recoveryStartPlsn` / `GetRecoveryStartPlsn()` | 本次恢复的起始 PLSN（从 checkpoint 记录中获取） | **核心** |
| **recoveryEndPlsn** | `WalRecovery` | `m_recoveryEndPlsn` / `GetRecoveryEndPlsn()` | 本次恢复的结束 PLSN（WAL 数据的末尾） | **核心** |
| **diskRecoveryStartPlsn** | `WalRecovery` | `m_diskRecoveryStartPlsn` / `GetDiskRecoveryStartPlsn()` | 从磁盘 WAL 开始恢复的起始 PLSN（可能与 recoveryStartPlsn 不同，因为部分 WAL 可能在内存中） | 中等 |
| **curRedoFinishedPlsn** | `WalRecovery` | `m_curRedoFinishedPlsn` | 当前已完成 redo 的最大 PLSN | **核心** — 恢复进度 |
| **lastGroupEndPlsn** | `WalRecovery` | `m_lastGroupEndPlsn` / `GetLastGroupEndPlsn()` | 最后一个已回放的 WAL 原子组的结束 PLSN | **核心** |
| **redoFinishedPlsn** | `WalStream` | `m_redoFinishedPlsn` | WalStream 级别的 redo 完成 PLSN | 中等 |
| **standbyRedoFinishPlsn** | `WalStream` | `m_standbyRedoFinishPlsn` / `GetStandbyRedoFinishPlsn()` | 备机 redo 完成的 PLSN | 低（单节点可忽略） |
| **maxParseredPlsn** | `WalRecovery` | `GetMaxParseredPlsn()` | 并行恢复中已解析到的最大 PLSN | 低（单节点串行恢复） |
| **curBarrierEndPlsn** | `WalRecovery` | `m_curBarrierEndPlsn` / `GetBarrierEndPlsn()` | 当前 barrier 的结束 PLSN（分布式一致性相关） | 低（单节点可忽略） |

**恢复进度可通过以下公式计算**：
```
progress = (curRedoFinishedPlsn - recoveryStartPlsn) / (recoveryEndPlsn - recoveryStartPlsn) × 100%
```

### 2.5 控制文件中的 LSN

| LSN 名称 | 所属结构 | 字段 | 含义 | 重要性 |
|----------|---------|------|------|--------|
| **walMinRecoveryPlsn** | `WalStreamInfoData` | `walMinRecoveryPlsn` | 该 WalStream 的最小恢复点，崩溃后从此处开始回放 | **核心** |
| **archivePlsn** | `WalStreamInfoData` | `archivePlsn` | 已归档的 PLSN 位置 | 中等 |
| **lastCheckpointPLsn** | `WalStreamInfoData` | `lastCheckpointPLsn` | 上次 checkpoint WAL 记录的位置 | **核心** |
| **diskRecoveryPlsn** | `WalCheckPoint`（嵌入控制文件） | `lastWalCheckpoint.diskRecoveryPlsn` | 同 2.2 中 diskRecoveryPlsn | **核心** |
| **barrierEndPlsn** | `WalBarrier`（嵌入控制文件） | `barrier.barrierEndPlsn` | barrier 结束 PLSN | 低（单节点可忽略） |

### 2.6 WAL 记录级别 LSN

| LSN 名称 | 所属结构 | 字段 | 含义 | 重要性 |
|----------|---------|------|------|--------|
| **endPlsn** | `WalGroupLsnInfo` | `endPlsn` | 一个 WAL 记录组的结束 PLSN | 内部 |
| **glsn** | `WalGroupLsnInfo` | `glsn` | 一个 WAL 记录组的 GLSN | 内部 |
| **m_startPlsn** | `WalAtomicGroupPlsn` | `m_startPlsn` | WAL 原子组在文件中的起始 PLSN | 内部 |
| **m_endPlsn** | `WalAtomicGroupPlsn` | `m_endPlsn` | WAL 原子组在文件中的结束 PLSN | 内部 |
| **m_pagePrePlsn** | `WalRecordForPage` | `m_pagePrePlsn` | 该页面的前一条 WAL 记录的 PLSN（用于构建页面 WAL 链） | 内部 |
| **m_pagePreGlsn** | `WalRecordForPage` | `m_pagePreGlsn` | 该页面的前一条 WAL 记录的 GLSN | 内部 |

### 2.7 WAL 文件级别 LSN

| LSN 名称 | 所属结构 | 字段/方法 | 含义 | 重要性 |
|----------|---------|----------|------|--------|
| **startPlsn** | `WalFile` | `m_startPlsn` / `GetStartPlsn()` | 该 WAL 文件包含的第一条记录的 PLSN | 中等 |
| **flushedPlsn** | `WalFile` | `m_flushedPlsn` / `GetFlushedPlsn()` | 该 WAL 文件中已刷新到磁盘的 PLSN | 中等 |

### 2.8 事务相关 LSN

| LSN 名称 | 所属结构 | 字段 | 含义 | 重要性 |
|----------|---------|------|------|--------|
| **commitEndPlsn** | `TransactionSlot` | `commitEndPlsn` | 事务提交 WAL 记录的结束 PLSN | 中等 |
| **walId** | `TransactionSlot` | `walId` | 事务提交 WAL 所在的 WalStream ID | 中等 |

### 2.9 页面/Segment 中的 LSN

| LSN 名称 | 所属结构 | 字段 | 含义 | 重要性 |
|----------|---------|------|------|--------|
| **m_plsn** | `PageHeader` | `m_plsn` | 页面最后一次修改的 PLSN | 中等 |
| **m_glsn** | `PageHeader` | `m_glsn` | 页面最后一次修改的 GLSN | 中等 |
| **m_walId** | `PageHeader` | `m_walId` | 页面最后一次修改的 WAL Stream ID | 中等 |
| **plsn/glsn** | `SegmentMetaPageHeader` | `plsn` / `glsn` | Segment 元数据页面的 LSN | 低 |
| **plsn/glsn** | `TablespaceDiagnoseInfo` | `plsn` / `glsn` | 表空间诊断信息中的 LSN | 诊断用 |

### 2.10 逻辑复制相关 LSN

| LSN 名称 | 所属类 | 字段 | 含义 | 重要性 |
|----------|-------|------|------|--------|
| **m_minLogicalRepRequired** | `WalStreamManager` | `m_minLogicalRepRequired` | 逻辑复制所需的最小 WAL 位置，阻止此位置之前的 WAL 被回收 | 低（单节点可忽略） |

---

## 3. 现有 Diagnose 接口已暴露的 LSN

### 3.1 WalDiagnose（`interface/diagnose/dstore_wal_diagnose.h`）

**WalStreamStateInfo** — 通过 `WalDiagnose::GetWalStreamInfoLocally()` / `GetAllWalStreamInfo()` 获取：

| 已暴露字段 | 含义 |
|-----------|------|
| `maxAppendedPlsn` | WAL 缓冲区最大追加 PLSN |
| `maxWrittenToFilePlsn` | 已写入文件的最大 PLSN |
| `maxFlushFinishPlsn` | 已持久化的最大 PLSN |
| `redoStartPlsn` | 恢复起始 PLSN |
| `redoFinishedPlsn` | 恢复完成 PLSN |
| `redoDonePlsn` | 当前 redo 已完成的 PLSN |

**WalRedoInfo** — 通过 `WalDiagnose::GetWalRedoInfoLocally()` 获取：

| 已暴露字段 | 含义 |
|-----------|------|
| `recovery_start_plsn` | 恢复起始 PLSN |
| `recovery_end_plsn` | 恢复结束 PLSN |
| `curr_redo_plsn` | 当前 redo 进度 PLSN |
| `progress` | 恢复进度百分比 |

**WalRecoveryInfo** — 内部恢复诊断信息：

| 已暴露字段 | 含义 |
|-----------|------|
| `recoveryStartPlsn` | 恢复起始 PLSN |
| `recoveryEndPlsn` | 恢复结束 PLSN |
| `lastGroupEndPlsn` | 最后回放的 WAL 组结束 PLSN |

### 3.2 TablespaceDiagnose（`interface/diagnose/dstore_tablespace_diagnose.h`）

| 已暴露字段 | 含义 |
|-----------|------|
| `plsn` | 表空间的 PLSN |
| `glsn` | 表空间的 GLSN |

### 3.3 BackupRestore（`interface/diagnose/dstore_backup_restore_function.h`）

| 已暴露字段 | 含义 |
|-----------|------|
| `plsn`（in checkpoint info） | checkpoint 的 PLSN |
| `restoreStartPlsn` | 恢复起始 PLSN |
| `restoreEndPlsn` | 恢复结束 PLSN |

---

## 4. 建议新增暴露的 LSN

以下 LSN 当前仅在内部使用，建议通过 Diagnose 接口暴露给上层，便于问题排查：

### 4.1 P0（高优先级）— 核心运行状态

| LSN | 来源 | 建议接口 | 诊断价值 |
|-----|------|---------|---------|
| **DirtyPageQueue minRecoveryPlsn** | `DirtyPageQueue::GetMinRecoveryPlsn()` | 新增 `BufMgrDiagnose::GetDirtyPageQueueInfo()` | 反映脏页队列的恢复点进度，如果此值长期不推进，说明脏页刷写可能卡住 |
| **控制文件 walMinRecoveryPlsn** | `WalStreamInfoData.walMinRecoveryPlsn` | 新增 `ControlFileDiagnose::GetWalStreamLsnInfo()` | 崩溃恢复的起始点，关键的持久化状态信息 |
| **控制文件 lastCheckpointPLsn** | `WalStreamInfoData.lastCheckpointPLsn` | 同上 | 上次 checkpoint 位置，可判断 checkpoint 是否正常推进 |
| **控制文件 diskRecoveryPlsn** | `WalCheckPoint.diskRecoveryPlsn` | 同上 | checkpoint 时的磁盘恢复点 |
| **maxContinuousPlsn** | `WalStreamBuffer.m_maxContinuousPlsn` | 扩展 `WalStreamStateInfo` | 反映 WAL 缓冲区的连续写入进度，与 maxAppendedPlsn 的差值反映并发写入的碎片程度 |

### 4.2 P1（中优先级）— 辅助定位

| LSN | 来源 | 建议接口 | 诊断价值 |
|-----|------|---------|---------|
| **Master Writer recoveryPlsn** | `BgDiskPageMasterWriter.m_recoveryPlsn` | 扩展现有 Buffer Diagnose | 每个 WAL stream 的刷脏进度 |
| **BufferDesc recoveryPlsn** | `BufferDesc.recoveryPlsn[]` | 已有 `BufDescPrintInfo`（包含 `recoveryPlsn`） | 单个 buffer 的恢复 PLSN，已在 BufDescPrintInfo 中暴露 |
| **BufferDesc pageVersionOnDisk** | `BufferDesc.pageVersionOnDisk` | 扩展 `BufDescPrintInfo` | 页面在磁盘上的版本，与当前 PLSN 比较可检测 missing dirty |
| **WalFile startPlsn/flushedPlsn** | `WalFile.m_startPlsn` / `m_flushedPlsn` | 新增 `WalDiagnose::GetWalFileList()` | WAL 文件级别的 LSN 分布，帮助理解 WAL 文件回收情况 |
| **archivePlsn** | `WalStreamInfoData.archivePlsn` | 扩展控制文件诊断 | 归档进度 |

### 4.3 P2（低优先级）— 深度调试

| LSN | 来源 | 建议接口 | 诊断价值 |
|-----|------|---------|---------|
| **lastScannedPlsn** | `WalStreamBuffer.m_lastScannedPlsn` | 扩展 WAL 诊断 | 刷写线程的扫描进度 |
| **commitEndPlsn** | `TransactionSlot.commitEndPlsn` | 扩展事务诊断 | 事务提交的 WAL 位置 |
| **diskRecoveryStartPlsn** | `WalRecovery.m_diskRecoveryStartPlsn` | 扩展恢复诊断 | 区分从磁盘/内存恢复的起始位置 |
| **页面 m_plsn/m_glsn** | `PageHeader` | 已有 pagedump 工具 | 单个页面的 LSN，pagedump 已可查看 |

---

## 5. 关键 LSN 关系图

### 5.1 WAL 写入 → 刷新 → Checkpoint 流水线

```
事务写入 WAL 记录
    │
    ▼
WalStreamBuffer.ReserveInsertLocation()
    │  分配 startPlsn ~ endPlsn 区间
    ▼
maxAppendedPlsn  ← 缓冲区中已追加的最大位置
    │
    ▼ (数据拷贝完成后)
maxContinuousPlsn  ← 连续可刷写的最大位置
    │
    ▼ (BgWalWriter 后台刷写)
maxWrittenToFilePlsn  ← 已写入 WAL 文件
    │
    ▼ (fsync 完成)
maxFlushFinishPlsn  ← 已持久化到磁盘
    │
    ▼ (事务提交要求)
commitEndPlsn  ← 事务提交时等待此 PLSN 持久化
```

### 5.2 Checkpoint → Recovery 关系

```
Checkpoint 执行时:
    1. 获取当前 DirtyPageQueue.minRecoveryPlsn → 成为 diskRecoveryPlsn
    2. 将 diskRecoveryPlsn 写入 WalCheckPoint 记录
    3. 记录 checkpoint WAL 的位置 → lastCheckpointPLsn
    4. 更新控制文件:
       - walMinRecoveryPlsn = diskRecoveryPlsn
       - lastCheckpointPLsn = checkpoint WAL 位置
       - diskRecoveryPlsn = checkpoint 内容中的 diskRecoveryPlsn

崩溃恢复时:
    1. 读取控制文件 → 获取 lastCheckpointPLsn
    2. 从 lastCheckpointPLsn 读取 WalCheckPoint → 获取 diskRecoveryPlsn
    3. recoveryStartPlsn = diskRecoveryPlsn（恢复起点）
    4. recoveryEndPlsn = WAL 数据末尾（恢复终点）
    5. 逐条回放: curRedoFinishedPlsn 从 recoveryStartPlsn 向 recoveryEndPlsn 推进
```

### 5.3 LSN 健康度诊断参考

| 比较项 | 正常状态 | 异常提示 |
|--------|---------|---------|
| `maxAppendedPlsn - maxFlushFinishPlsn` | 较小（< 几个 WAL 文件大小） | 差值过大说明 WAL 刷写积压 |
| `maxAppendedPlsn - maxContinuousPlsn` | 接近 0 | 差值大说明有大量并发写入未完成拷贝 |
| `maxWrittenToFilePlsn - maxFlushFinishPlsn` | 接近 0 | 差值大说明 fsync 延迟 |
| `lastCheckpointPLsn` 推进 | 随 checkpoint 定期推进 | 长期不变说明 checkpoint 未执行或失败 |
| `walMinRecoveryPlsn` 推进 | 随 checkpoint 推进 | 长期不变说明恢复点未推进，崩溃恢复时间增长 |
| `DirtyPageQueue.minRecoveryPlsn` 推进 | 随脏页刷写推进 | 长期不变说明脏页刷写卡住 |
| `recoveryEndPlsn - curRedoFinishedPlsn` | 恢复中逐渐减小至 0 | 长期不变说明恢复卡住 |

---

## 6. 总结

### 6.1 需要重点关注的核心 LSN（单节点模式）

1. **WAL 写入流水线**：`maxAppendedPlsn` → `maxContinuousPlsn` → `maxWrittenToFilePlsn` → `maxFlushFinishPlsn`
2. **Checkpoint 状态**：`lastCheckpointPLsn`、`diskRecoveryPlsn`、`walMinRecoveryPlsn`
3. **脏页刷写进度**：`DirtyPageQueue.minRecoveryPlsn`
4. **恢复进度**：`recoveryStartPlsn`、`recoveryEndPlsn`、`curRedoFinishedPlsn`

### 6.2 现有接口覆盖情况

| 类别 | 已暴露 | 未暴露（建议新增） |
|------|--------|------------------|
| WAL 写入流水线 | maxAppendedPlsn, maxWrittenToFilePlsn, maxFlushFinishPlsn | **maxContinuousPlsn** |
| Checkpoint | — | **lastCheckpointPLsn, diskRecoveryPlsn, walMinRecoveryPlsn** |
| 脏页队列 | — | **DirtyPageQueue.minRecoveryPlsn** |
| 恢复 | recoveryStartPlsn, recoveryEndPlsn, redoDonePlsn | 已较完整 |
| 页面级 | pagedump 工具可查看 | 已足够 |

### 6.3 实施建议

1. **新增 `ControlFileDiagnose` 接口**：暴露控制文件中的 `walMinRecoveryPlsn`、`lastCheckpointPLsn`、`diskRecoveryPlsn` — 这是当前最大的盲区
2. **扩展 `WalStreamStateInfo`**：增加 `maxContinuousPlsn` 字段
3. **新增 `BufMgrDiagnose::GetDirtyPageQueueInfo()`**：暴露各 WAL stream 的脏页队列 `minRecoveryPlsn`
4. **提供 LSN 一览汇总接口**：一次调用返回所有关键 LSN 的快照，便于运维人员快速判断系统健康状态
