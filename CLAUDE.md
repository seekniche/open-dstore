# CLAUDE.md - Dstore 项目指南

## 项目概述

Dstore 是一个可独立编译、独立测试的数据库 **TP（事务处理）存储引擎**，类似于 PostgreSQL 的存储引擎。Dstore 向上可对接 PostgreSQL/MySQL 的 server 层，中间通过 handler 层适配。使用 C++11 编写，GCC 7.3 编译，GPLv2+ 协议。

**当前只关注单节点模式。** 分布式/多节点相关逻辑（如 `distribute`、`cdb` 模块）可忽略。

## 目录结构

```
dstore/
├── src/              # 主要源代码（25 个模块）
├── include/          # 内部头文件
├── interface/        # 对外公开 API 头文件（纯虚类接口）
├── tests/            # 单元测试（gtest）和 TPC-C 测试
├── tools/            # 诊断工具（pagedump、waldump 等）
├── utils/            # 工具库（独立编译，生成 libgsutils.so）
├── cmake/            # CMake 模块（build_options、set_lib_path）
├── scripts/          # 编译辅助脚本
├── build.sh          # 主编译脚本
├── buildenv          # 环境配置文件（编译前需 source）
└── CMakeLists.txt    # 根 CMake 配置
```

---

## 核心模块详解

### Framework 框架（src/framework/）

**核心类：**
- **`StorageInstance`**：全局单例（`g_storageInstance`），存储引擎入口。管理 PDB 数组、缓冲池、锁管理器、CSN 管理器等全局资源。
  - 生命周期：`Bootstrap()` / `StartupInstance()` → `ShutdownInstance()`
  - 初始化顺序：GucInit → TypecacheMgrInit → ThreadCoreMgrInit → BufMgrInit → InitAllLockMgrs → InitPdbSlots → OpenOnePdb
- **`StoragePdb`**：可插拔数据库（Pluggable Database），每个 PDB 拥有独立的表空间、事务上下文、WAL、checkpoint、undo 管理。
  - PDB ID 范围：PDB_ROOT_ID=3（默认），FIRST_USER_PDB_ID=17，PDB_MAX_ID=32
  - 生命周期：`CreatePdb()` / `InitPdb()` → `OpenPdb()` → `ClosePdb()`
  - 每个 PDB 包含：VFSAdapter、ControlFile、TransactionMgr、UndoMgr、WalManager、CheckpointMgr、BgPageWriterMgr、TablespaceMgr
  - 后台线程：checkpoint 线程、undo 回收线程、异步恢复 undo 线程
- **`ThreadContext`**：线程本地上下文（`thread_local ThreadContext *thrd`），每个工作线程持有一个。
  - 包含：ThreadCorePayload（core + xact）、WAL 写入上下文、事务列表、内存管理器、session
  - 初始化：`InitializeBasic()` → `InitStorageContext(pdbId)` → `InitTransactionRuntime(pdbId, callback)`
- **`ThreadCoreMgr`**：线程核心管理器，管理所有线程的 ThreadCore/ThreadXact 数组。
  - ThreadCore 和 ThreadXact 分离，各自 cacheline 对齐（64 字节），减少 false sharing
  - ThreadXact 存储：csnMin、currentActiveXid、toBeCommitedCsn、pdbId
- **`VFSAdapter`**：虚拟文件系统抽象层，支持 LOCAL（本地文件系统）、PAGESTORE（分布式存储，可忽略）等模式。
  - 文件操作：ReadPageSync/WritePageSync/WritePageAsync/Fsync/Extend/Truncate
  - 文件描述符哈希表管理，分区锁减少竞争

### Buffer Pool 缓冲池（src/buffer/）

**核心架构：**

```
BufMgr（缓冲池管理器）
├── BufTable（哈希表，BufferTag → BufferDesc 快速查找）
├── BufferMemChunk（物理内存分块管理）
├── BufLruList[N]（分区 LRU，减少锁竞争）
│   ├── LruHotList（热页面，固定大小，高频访问保护）
│   ├── LruList（工作集，活跃页面）
│   ├── LruCandidateList（候选页面，可复用）
│   └── LruInvalidationList（待清理）
├── BgDiskPageMasterWriter（每个 WAL stream 一个，监控/调度线程）
│   └── BgDiskPageSlaveWriter[1-16]（实际刷脏线程）
└── CheckpointMgr（checkpoint 管理器）
```

**关键数据结构：**
- **`BufferTag`**（12 字节）：PageId（fileId + blockId）+ PdbId，唯一标识缓冲池中一个页面
- **`BufferDesc`**（cacheline 对齐）：缓冲区描述符
  - `state`：原子 64 位，低 18 位为引用计数，高位为标志位（BUF_LOCKED、BUF_CONTENT_DIRTY、BUF_VALID、BUF_TAG_VALID、BUF_IO_IN_PROGRESS 等）
  - `contentLwLock`：页面内容轻量级锁
  - `nextDirtyPagePtr[5]`：脏页队列链表指针（每个 WAL stream 一个槽位）
  - `recoveryPlsn[5]`：恢复 LSN
  - `lruNode`：LRU 节点，包含 4 种状态（LN_PENDING、LN_CANDIDATE、LN_LRU、LN_HOT）和 usage 计数（0-5）

**两级引用计数：**
- 线程私有引用计数（`BufPrivateRefCount`，thread-local）：避免缓存行竞争
- 共享引用计数（BufferDesc.state 低位）：仅在线程首次 pin 时原子递增

**LRU 四状态转换：**
```
分配 → LN_PENDING → 添加到 LRU 尾部 → LN_LRU（usage=1）
访问 → usage 递增，移至 LRU 头部
usage ≥ 5 → 提升到 LN_HOT（热列表满时淘汰最旧到 LRU）
淘汰 → LRU 尾部 → LN_CANDIDATE（可复用）
复用 → LN_CANDIDATE → LN_PENDING → 重新分配
```

**Master/Slave 刷脏架构：**
- **Master Writer（BgDiskPageMasterWriter）**：监控/调度线程，每个 WAL stream 一个
  - 主循环：扫描脏页队列 → 构建 CandidateFlushCxt → 唤醒 Slave → 等待 Slave 完成 → 推进队列头 → 休眠
  - `ScanDirtyListForFlush()`：从 DirtyPageQueue 收集脏页到 candidateFlushArray
- **Slave Writer（BgDiskPageSlaveWriter）**：实际执行 I/O 的工作线程（1-16 个）
  - 通过原子操作无锁竞争工作分配：`CandidateFlushCxt.ScrambleLoc()` 原子递增获取工作片段
  - 支持同步写入和异步 AIO

**脏页队列（DirtyPageQueue）**：MPSC（多生产者单消费者）有界队列
- 多线程通过 `MarkDirty()` 推入脏页（设置 BUF_CONTENT_DIRTY 标志 + 推入队列）
- 关键不变量：recovery PLSN < 脏页的 PLSN（先获取 recovery PLSN，再插入 WAL 记录）

**Checkpoint：**
- `CheckpointMgr`：协调 checkpoint 流程
- 流程：扫描所有 WAL stream → 获取 minRecoveryPlsn → 刷脏页 → 记录 checkpoint LSN → 更新控制文件

### WAL 预写日志（src/wal/）

**核心架构：**

```
WalStreamManager（管理多个 WalStream）
└── WalStream（每个 PDB 一个写入流）
    ├── WalStreamBuffer（环形缓冲区）
    ├── BgWalWriter（后台刷写线程）
    └── WalFileManager（WAL 文件管理）
        └── WalFile 链表（head → ... → tail）
```

**WalStream 管理：**
- `WalStreamManager`：管理 PDB 下所有 WalStream 的生命周期
- `WalStream`：单个日志流，状态机（CREATING → USING → SYNC_DONE → CLOSE_DROPPING）
- 用途模式：`WAL_STREAM_USAGE_WRITE_WAL`（主动写入）或 `WAL_STREAM_USAGE_ONLY_READ`（只读）

**WAL Buffer（WalStreamBuffer）：**
- 多 block 环形缓冲区，每个 block 为 WAL_BLCKSZ（通常 8KB）
- NUMA 感知的 group insert lock，减少跨 NUMA 锁竞争
- 关键操作：
  - `ReserveInsertByteLocation()`：原子预留缓冲区空间
  - `MarkInsertFinish()`：标记数据写入完成，可供刷写
  - `GetNextFlushData()`：获取连续可刷写数据
- `m_maxContinuousPlsn`：追踪最大连续已写入 PLSN

**WAL Flush（BgWalWriter）：**
- 每个 WalStream 一个后台刷写线程
- 触发条件：缓冲区阈值（`bgWalWriterMinBytes`）、定时触发、手动唤醒
- 刷写流程：`GetNextFlushData()` → 写入 WalFile（同步/异步 I/O）→ 回调更新 flushedPlsn → 通知等待线程
- 限流机制：`WalThrottling()` 在缓冲区满时限制事务插入速率

**WAL File 管理（WalFileManager）：**
- WalFile 链表管理，每个文件大小可配置（默认 128MB）
- 文件轮转：`GetWalFileByPlsn()` 定位文件，`GetNextWalFile()` 切换新文件
- 后台回收：`RecycleWalFileWorkerMain()` 在 checkpoint 后回收旧文件

**WAL 记录格式：**
- `WalRecordAtomicGroup`：事务原子组（groupLen + crc + xid + recordNum + WalRecord[]）
- `WalRecord`（4 字节基础）：m_size + m_type
- `WalRecordForPage`（页面修改记录）：继承 WalRecord，增加 pageId、pagePrePlsn、pagePreGlsn、filePreVersion
- WAL 类型（190+ 种）：堆操作、B-tree 操作、B-tree 回收、表空间 DDL、Undo、事务提交/回滚、系统表等

**WAL 写入流程：**
```
AtomicWalWriterContext::BeginAtomicWal()
→ WalStreamBuffer::ReserveInsertByteLocation()
→ PutNewWalRecord()（拷贝记录到缓冲区）
→ EndAtomicWal() → MarkInsertFinish()
→ BgWalWriter 后台异步刷写到 WalFile
```

### Recovery 恢复（src/wal/，WalRecovery 相关）

**当前只关注串行回放（recoveryWorkerNum == 1）。**

**恢复阶段（WalRecoveryStage）：**
```
RECOVERY_NO_START → RECOVERY_STARTING → RECOVERY_GET_DIRTY_PAGE_SET
→ RECOVERY_GET_DIRTY_PAGE_SET_DONE → RECOVERY_REDO_STARTED
→ RECOVERY_REDO_STOPPING → RECOVERY_REDO_DONE → RECOVERY_DIRTY_PAGE_FLUSHED
```

**串行回放流程：**
1. **初始化**：`WalRecovery::Init()` 设置 `m_enableParallelRedo = false`（当 recoveryWorkerNum == 1）
2. **构建脏页集**：`BuildDirtyPageSetAndPageWalRecordListHtab()` 扫描所有 WAL 记录，构建 PageId → WAL 记录列表的哈希表
3. **串行 Redo**：`RedoLoopReadAndRedoWalRecord()` 主循环
   - 读取下一个原子 WAL 组（`WalRecordReader::ReadNext()`）
   - 遍历组中每条记录，直接调用 `RedoSingle()`（不走并行分发队列）
   - `RedoSingle()`：解压（如需要）→ 判断可回放性 → 读取受影响页面 → 调用 `RedoWalRecordByType()` 分发
   - 更新 `m_curRedoFinishedPlsn`
4. **类型分发**：根据 WalType 范围分发到具体 Redo 函数
   - 堆操作 → `WalRecordHeap::RedoHeapRecord()`
   - B-tree → `WalRecordIndex::RedoIndexRecord()`
   - B-tree 回收 → `WalRecordBtrRecycle::RedoBtrRecycleRecord()`
   - 表空间 → `WalRecordTbs::RedoTbsRecord()`
   - Undo/事务 → `WalRecordUndo::RedoUndoRecord()`
   - 系统表 → `WalRecordSystable::RedoSystableRecord()`

**并行回放（recoveryWorkerNum > 1，当前不关注）：**
- `ParallelRedoWorker`（PageRedoWorker / DDLRedoWorker）通过 `BlockSpscQueue`（SPSC 队列）消费分发的 WAL 记录

**WalRedoManager：**
- 管理整个 PDB 的 redo 任务生命周期
- `RedoTask` 描述恢复任务（term、待恢复的 WAL ID）
- 后台线程 `CleanFinishRedoTask` 监控完成状态

### Transaction 事务（src/transaction/）

**关键数据结构：**
- **`Xid`（事务 ID）**：64 位联合体，20 位 zoneId + 44 位 logicSlotId
- **`TransactionState`**：TRANS_DEFAULT → TRANS_START → TRANS_INPROGRESS → TRANS_COMMIT / TRANS_ABORT

**核心类：**
- **`Transaction`**：每连接事务状态
  - 成员：TransactionStateData（xid、state、blockState）、SnapshotData（csn、cid）、isolationLevel、savepoints、cursors
  - 关键方法：`Start()`、`Commit()`、`Abort()`、`AllocTransactionSlot()`、`SetSnapshotCsn()`、`InsertUndoRecord()`、`CreateSavepoint()`、`RollbackToSavepoint()`
  - MVCC 可见性：`XidVisibleToSnapshot()`、`ConstructCrPage()`（构建一致性读页面）
- **`TransactionMgr`**：PDB 级事务管理器
  - 依赖：UndoMgr、CsnMgr、RollbackTrxTaskMgr
  - 关键方法：`AllocTransactionSlot()`、`CommitTransactionSlot()`、`RollbackTransactionSlot()`、`InsertUndoRecord()`、`FetchUndoRecord()`、`AsyncRollback()`
- **`CsnMgr`**：全局 CSN（Commit Sequence Number）管理器
  - `m_nextCsn`：下一个待分配 CSN（原子递增）
  - `GetNextCsn()`：分配 CSN
  - `UpdateLocalCsnMin()`：计算最小可见 CSN
  - `GetRecycleCsnMin()`：获取可回收阈值

**CSN 机制：**
- 全局单调递增计数器，事务提交时分配
- 快照捕获当前 CSN：CSN < snapshot.csn 的已提交事务可见
- 无需扫描活跃事务列表即可判断可见性
- 回收机制：比最老快照更早的 CSN 可被回收

**事务生命周期：**
1. **开始**：`AllocTransactionSlot()` → 从 UndoMgr 获取 Xid（zoneId + logicSlotId）
2. **DML 操作**：Insert/Update/Delete 生成 UndoRecord 写入 undo zone
3. **设置快照**：`SetSnapshotCsn()` 捕获当前 CSN 用于 MVCC
4. **提交**：设置 PENDING_COMMIT → 从 CsnMgr 获取 CSN → 写入提交 WAL → 更新事务槽状态为 COMMITTED
5. **回滚**：反向遍历 undo 链 → 生成回滚 WAL → 物理撤销页面修改 → 标记 ABORTED

**Savepoint（保存点）：**
- `Savepoint`：记录 lastUndoPtr 和 lastLockPos
- `SavepointList`：双向链表栈，支持命名保存点
- 支持：`CreateSavepoint()` → `RollbackToSavepoint()` → `ReleaseSavepoint()`

### Undo 回滚（src/undo/）

**核心架构：**

```
UndoMgr（PDB 级管理器）
├── UndoZone[N]（undo 区域，每个事务区域一个）
│   ├── UndoZoneTrxManager（管理事务槽）
│   │   └── TransactionSlot[]（事务状态：curTailUndoPtr, csn, status）
│   └── UndoRecord 链（通过 txnPreUndoPtr 链接）
└── AllUndoZoneTxnInfoCache（事务信息缓存）
```

**关键数据结构：**
- **`UndoRecPtr`**：物理地址（16 位 fileID + 32 位 pageID + 16 位 offset）
- **`UndoRecord`**：undo 日志条目
  - 头部：undoType、cid、tdPreInfo（前一个 TD 的 xid/csn/undoPtr）、txnPreUndoPtr（事务 undo 链）、ctid、fileVersion
  - 数据：变长 payload（StringInfoData），使用 varint 压缩序列化
- **`TransactionSlot`**：事务在 undo zone 中的状态
  - curTailUndoPtr / spaceTailUndoPtr：undo 链尾指针
  - csn、status（IN_PROGRESS/PENDING_COMMIT/COMMITTED/ABORTED/FAILED/PREPARED）
  - commitEndPlsn、walId

**UndoType 类型：**
- 堆操作：UNDO_HEAP_INSERT、DELETE、INPLACE_UPDATE、SAME_PAGE_APPEND_UPDATE、ANOTHER_PAGE_APPEND_UPDATE_OLD_PAGE/NEW_PAGE
- B-tree 操作：UNDO_BTREE_INSERT、DELETE
- 临时表变体：以上所有类型的 _TMP 后缀版本

**回滚机制：**
- `RollbackUndoZone()`：从 undo 链尾部反向遍历到头部
- `RollbackTrxTaskMgr`：分发异步回滚任务
- `RollbackTrxWorker`：执行回滚并生成 WAL 记录
- Undo 页面以环形结构组织，所有记录来自已提交事务后可回收

### Heap 堆表（src/heap/）

**核心类：**
- **`HeapHandler`**（基类）：管理 StorageRelation、BufferDesc、TD 上下文、BufferRing（环形缓冲策略）
- **`HeapInsertHandler`**：插入操作（支持批量插入）
- **`HeapUpdateHandler`**：更新操作（in-place / same-page append / cross-page append）
- **`HeapDeleteHandler`**：删除操作
- **`HeapScanHandler`**：扫描操作（支持 MVCC 可见性过滤、谓词下推、采样扫描）

**TD（Tuple Descriptor，16 字节）：** 每个元组的 MVCC 元数据
- `m_xid`：最后修改事务 ID
- `m_csn`：该事务的 CSN
- `m_undoRecPtr`：指向 undo 记录的指针
- `m_lockerXid`：持锁事务 ID
- `m_status`：UNOCCUPY_AND_PRUNEABLE / OCCUPY_TRX_IN_PROGRESS / OCCUPY_TRX_END

**Heap 页面布局：**
```
PageHeader → HeapPageHeader → [TD 数组 | ItemId 数组 | 元组数据]
                                ↑ lower 向下增长    ↑ upper 向上增长
```
- HeapPageHeader：potentialDelSize（可删除空间）、fsmIndex、recentDeadTupleMinCsn

**DML 操作流程：**
- **Insert**：FSM 查找空闲空间 → 分配 TD → 创建 HeapDiskTuple → 生成 UNDO_HEAP_INSERT → 写入 undo zone → AddTuple → 更新 FSM 和 WAL
- **Update**：锁定元组 → 检查可见性/冲突 → 分配新 TD → 根据情况生成不同 undo 类型 → 更新或新增元组
- **Delete**：锁定元组 → 检查可见性 → 分配新 TD → 生成 UNDO_HEAP_DELETE（含元组镜像）→ 标记删除
- **Scan**：`Begin()` → `SeqScan()` 循环获取可见元组 → `End()`
  - MVCC 可见性：比较 snapshot.csn 与 TD.csn
  - 不可见时通过 undo 链构建一致性读页面（CR page）

**Undo 数据结构（per DML type）：**
- `UndoDataHeapDelete`：完整元组镜像
- `UndoDataHeapInplaceUpdate`：列级差异（oldTdId + 变更位置 + 旧数据）
- `UndoDataHeapSamePageAppendUpdate`：旧元组镜像
- `UndoDataHeapAnotherPageAppendUpdate`：新 ctid + 旧元组镜像

**LOB 大对象支持：** `FetchLobValue()`、`FetchTupleWithLob()`，LOB 引用存储在元组中

### Index B-tree 索引（src/index/）

**核心类继承：**
```
Btree（基类：m_indexRel, m_indexInfo, m_scanKeyValues）
├── BtreeSplit（分裂：m_leafStack, m_splitBuf, m_newRightBuf）
│   ├── BtreeInsert（插入：InsertTuple, SearchBtreeForInsert, CheckUnique, FindInsertLoc）
│   └── BtreeDelete（删除：DeleteTuple, DoDelete, FindDeleteLoc, DeleteFromLeaf/Internal）
├── BtreeScan（扫描：BeginScan, GetNextTuple, EndScan, 正向/反向, IN/ANY 数组条件）
├── BtreePageRecycle（页面回收：PutIntoRecycleQueueIfEmpty, BatchRecycleBtreePage）
├── BtreeVacuum（清理：BtreeLazyVacuum）
└── BtreeStorageMgr（存储管理：GetNewPage, 回收队列, meta 缓存）
```

**B-tree 页面结构：**
```
Page → DataPage → BtrPage
├── TD 数组（仅叶子页面分配）
├── ItemId 数组
├── 索引元组数据
└── BtrPageLinkAndStatus（special 区域）
    - prev/next（兄弟链接）、level（0=叶子）
    - type（LEAF/INTERNAL/META）、isRoot、liveStat、splitStat
```

**BtrMeta（元数据页面）：**
- rootPage、rootLevel、lowestSinglePage/Level（瘦树优化，跳过单页面层级直接定位）
- 属性信息：nkeyAtts、attTypeIds、opcinTypes、functionOids
- 操作统计：每层的 split/recycle 计数

**页面分裂流程：**
1. 页面满时触发 `SplitAndAddDownlink()`
2. 创建新右页面，重分配元组
3. 向父节点插入 downlink（父节点满则递归分裂）
4. 分裂状态：SPLIT_INCOMPLETE → SPLIT_COMPLETE

**页面回收状态机：**
```
NORMAL_USING → EMPTY_HAS_PARENT_HAS_SIB → EMPTY_NO_PARENT_HAS_SIB → EMPTY_NO_PARENT_NO_SIB（可复用）
```
- `BtreeRecycleWorker` 后台处理 `PendingFreePageQueue`
- `BtreePageUnlink` 从父节点/兄弟链中解除链接

**lowestSinglePage 优化：** 跳过只有单页面的层级，减少瘦树的搜索深度

### Page 页面（src/page/）

**页面层次：**
```
Page（基类）
├── PageHeader：checksum、special offset、flags、lower、upper、type、GLSN、PLSN、WAL ID、PageId
└── DataPage
    ├── DataPageHeader：tdCount、versionNum、headerOffset、segmentCreateXid
    ├── HeapPage（堆页面）
    │   └── HeapPageHeader：potentialDelSize、fsmIndex、recentDeadTupleMinCsn
    └── BtrPage（B-tree 页面）
        └── BtrPageHeader + BtrPageLinkAndStatus
```

**ItemId（行指针）：**
- 正常：flags(2) + offset(15) + len(15)
- 无存储（重定向）：flags(2) + tdId(8) + tdStatus(2) + tupLiveMode(3)

**空间管理：** lower 指针向下增长（ItemId 数组），upper 指针向上增长（元组数据），空闲空间 = upper - lower

### Lock 锁（src/lock/）

**轻量级锁（LWLock）：**
- 64 位原子状态：低 24 位共享计数 + VAL_SHARED + VAL_EXCLUSIVE + DISALLOW_PREEMPT + HAS_WAITERS 等标志
- 模式：LW_EXCLUSIVE、LW_SHARED、LW_WAIT_UNTIL_FREE
- 获取协议：原子 CAS 尝试 → 入队等待 → futex 休眠 → 被唤醒后重试
- 释放：原子递减/清除标志 → 唤醒等待者

**锁管理器（LockMgr）：**
```
LockMgr（高层 API）
├── ThreadLocalLock（线程本地锁跟踪，快速路径判断已持有）
├── LockHashTable（主锁表，分区设计）
│   ├── LockEntry（每个被锁对象：LockTag + granted 队列 + wait 队列）
│   │   └── Skip List 实现（O(log N) 查找）
│   └── LWLockPadded[] 分区锁
└── DeadlockDetector
```

**死锁检测：**
- `WaitForGraph`：顶点（ThreadVertex）+ 边（wait-for 关系）
- 算法：从当前锁等待状态构建等待图 → 查找环 → 选择牺牲者（最晚启动的事务）→ 注入 DEADLOCK 错误
- `DeadlockThrdState`：记录等待开始时间、事务启动时间、死锁报告

### 其他模块

| 模块 | 说明 |
|------|------|
| **config** | 配置参数管理（GUC 参数） |
| **control** | 控制文件管理（数据库状态、checkpoint 信息、CSN 信息） |
| **tablespace** | 表空间与 segment 管理 |
| **tuple** | 元组操作与格式处理 |
| **fsm** | 空闲空间管理 |
| **flashback** | 闪回恢复 |
| **perfcounter** | 性能统计 |
| **catalog** | 系统目录管理 |
| **common** | 公共工具：错误处理、内存管理（MemoryContext）、数据类型、日志、算法 |
| **cdb** | 分布式数据库（单节点模式下可忽略） |
| **logical_replication** | 逻辑复制（单节点模式下可忽略） |

---

## 架构分层

```
Framework 层
│  StorageInstance（全局单例）→ StoragePdb（可插拔数据库）→ ThreadContext（线程上下文）
│
事务与锁层
│  Transaction（事务生命周期）→ CsnMgr（CSN 分配与可见性）→ LockMgr（锁管理与死锁检测）
│
存储访问层
│  HeapHandler（堆表 CRUD）→ BtreeInsert/Delete/Scan（B-tree 索引操作）
│
Undo 层
│  UndoMgr → UndoZone → UndoRecord 链（事务回滚与旧版本重建）
│
缓冲池
│  BufMgr（分区 LRU + 两级引用计数）→ MasterWriter + SlaveWriter（脏页刷写）→ CheckpointMgr
│
WAL 与恢复
│  WalStream（日志流）→ WalStreamBuffer（环形缓冲）→ BgWalWriter（后台刷写）→ WalRecovery（串行/并行 redo）
│
物理存储
│  VFSAdapter（文件系统抽象）→ TablespaceMgr（表空间）→ Page（页面格式）→ ControlFile（控制文件）
```

---

## 编译指南

### 依赖项

依赖库需放置在 dstore 同级的 `../local_libs/` 目录下：
- **gcc 7.3**（位于 `local_libs/buildtools/gcc7.3/`）
- **secure** v3.0.9（华为安全 C 库）
- **lz4** v1.10.0
- **cjson** v1.7.17
- **gtest** v1.10.0

### 编译步骤

```bash
# 1. 配置环境（先修改 buildenv 中 BUILD_ROOT 为 dstore 目录的实际路径）
source buildenv

# 2. 先编译 utils 库
cd utils && bash build.sh -m release && cd ..

# 3. 编译 dstore
bash build.sh -m release    # 可选: -m debug / memcheck / coverage
```

编译产物：`output/lib/libdstore.so`、`output/lib/libdstore.a`

### 编译选项

```bash
bash build.sh -m <mode>     # release | debug | memcheck | coverage
bash build.sh -vb           # 显示详细编译日志
bash build.sh clean          # 清理编译目录
```

## 测试

### 单元测试（gtest）

```bash
cd dstore && rm -rf tmp_build && mkdir tmp_build && cd tmp_build
cmake .. -DUTILS_PATH=$(pwd)/../utils/output -DENABLE_UT=ON
make run_dstore_ut_all                      # 全部单元测试
make run_dstore_ut_asan                     # 带 AddressSanitizer
make run_dstore_datamanager_unittest        # heap、tablespace、control、PDB、VFS
make run_dstore_index_and_undo_unittest     # B-tree 和 undo
make run_dstore_xact_and_lock_unittest      # 事务、锁、并发
make run_dstore_buffer_unittest             # 缓冲池
make run_dstore_ha_unittest                 # WAL、复制、恢复
```

### TPC-C 测试

```bash
cd dstore && rm -rf tmp_build && mkdir tmp_build && cd tmp_build
cmake .. -DUTILS_PATH=$(pwd)/../utils/output
make run_dstore_tpcctest
```

### 测试组织

测试代码位于 `tests/unittest/`，共 27 个测试模块（如 `ut_buffer`、`ut_btree`、`ut_heap`、`ut_wal`、`ut_transaction`、`ut_lock`）。使用 gtest/gmock 框架，通过 mock 实现测试隔离。

## 诊断工具

- **pagedump** - 数据库页面转储与分析
- **waldump** - WAL 日志转储与分析
- **whatiserrcode** - 错误码查询
- **htablookup** - 哈希表分析
- **buflookup** - 缓冲池分析

## 代码规范

- C++11 标准，GCC 7.3 编译
- 命名：源文件统一使用 `dstore_` 前缀（如 `dstore_buf_mgr.cpp`、`dstore_btree.cpp`）
- 函数和变量使用 snake_case 命名风格
- 头文件：对外公开 API 放在 `interface/`（纯虚类），内部头文件放在 `include/`
- 支持平台：x86_64（EulerOS 2.5）、aarch64（EulerOS 2.9）
- cacheline 对齐（64 字节）用于高频并发访问的数据结构
