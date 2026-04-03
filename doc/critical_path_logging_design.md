# 关键路径日志与错误码加固 — 设计文档

## 1. 背景与目标

当前 dstore 在关键流程中的日志覆盖不够完整，部分错误路径缺少日志或日志信息不足以定位问题。本文档梳理以下关键流程的日志现状，识别不足之处，并提出加固方案：

- 实例启动（Bootstrap / StartupInstance）
- 实例关闭（ShutdownInstance）
- PDB 打开 / 关闭（OpenPdb / ClosePdb）
- 各后台线程的启动、运行、停止
- 错误码体系

---

## 2. 实例启动流程日志分析

### 2.1 当前现状

`StorageInstance::Bootstrap()` 和 `StorageInstance::StartupInstance()` 是两个主要启动入口。

**Bootstrap() (dstore_instance.cpp:398)**

```
GucInit → TypecacheMgrInit → ThreadCoreMgrInit → InitCpuRes → BufMgrInit
→ InitAllLockMgrs → InitPdbLwLocks → InitPdbSlots → InitPdbInfoCache → CreateTemplatePDB
```

**StartupInstance() (dstore_instance.cpp:474)**

```
GucInit → TypecacheMgrInit → ThreadCoreMgrInit → InitCpuRes → InitPerfCounter
→ BufMgrInit → InitAllLockMgrs → InitPdbLwLocks → InitPdbSlots → InitPdbInfoCache → OpenOnePdb
```

### 2.2 问题清单

| 编号 | 问题 | 位置 | 严重程度 |
|------|------|------|---------|
| S-1 | **Bootstrap/StartupInstance 入口无日志**：整个启动流程没有"开始启动"和"启动完成"的日志，无法从日志中确认启动是否发起、何时完成 | dstore_instance.cpp:398, 474 | 高 |
| S-2 | **各 Init 步骤失败只返回 DSTORE_FAIL，不打印是哪一步失败**：例如 `GucInit` 失败后直接 `return DSTORE_FAIL`（行 406-408），调用者无法从日志中区分是 GUC 初始化失败还是 BufMgr 初始化失败 | dstore_instance.cpp:406-438 | 高 |
| S-3 | **前置条件检查无日志**：`m_memoryMgr == nullptr` 或 `m_instanceState != NOT_ACTIVE` 时直接返回失败，无任何日志说明原因 | dstore_instance.cpp:401-402, 477-478 | 高 |
| S-4 | **InitCpuRes 成功有日志（行312），但其他 Init 步骤成功无日志**：无法确认每个步骤的耗时和完成状态 | dstore_instance.cpp | 中 |

### 2.3 加固方案

```
[DS_INIT] StorageInstance::Bootstrap started.
[DS_INIT] GucInit completed.                          // 每个步骤成功后打印
[DS_INIT] TypecacheMgrInit completed.
[DS_INIT] ThreadCoreMgrInit completed.
[DS_INIT] InitCpuRes completed.
[DS_INIT] BufMgrInit completed, buffer=%u.            // 附带关键参数
[DS_INIT] InitAllLockMgrs completed.
[DS_INIT] InitPdbSlots completed.
[DS_INIT] CreateTemplatePDB completed, pdbId=%u.
[DS_INIT] StorageInstance::Bootstrap completed.
```

每个 Init 步骤失败时，增加具体的错误日志：
```cpp
ret = GucInit(guc);
if (STORAGE_FUNC_FAIL(ret)) {
    ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("[DS_INIT] GucInit failed."));
    return DSTORE_FAIL;
}
```

---

## 3. 实例关闭流程日志分析

### 3.1 当前现状

**ShutdownInstance() (dstore_instance.cpp:539)**

```
m_instanceState = NOT_ACTIVE → StopBgThreads → FlushAll → CloseAllPdb
→ BufMgrDestroy → DestroyCsnMgr → DestroyTableLockMgr → DestroyXactLockMgr
→ DestroyLockMgr → DestroyTransactionRuntime → UnregisterThread → ResourcesCleanUp
```

### 3.2 问题清单

| 编号 | 问题 | 位置 | 严重程度 |
|------|------|------|---------|
| D-1 | **ShutdownInstance 入口/出口无日志**：无法从日志确认关闭流程何时开始、何时结束 | dstore_instance.cpp:539 | 高 |
| D-2 | **各 Destroy 步骤无日志**：BufMgrDestroy、DestroyLockMgr 等均无日志，如果关闭流程卡住，无法从日志判断卡在哪一步 | dstore_instance.cpp:551-560 | 高 |
| D-3 | **BootstrapDestroy / BootstrapResDestroy 无日志**：这两个函数完全没有日志 | dstore_instance.cpp:449, 462 | 中 |
| D-4 | **FlushAll 失败无单独日志**：FlushAll 在 `#ifndef UT` 块中调用，失败不会被记录 | dstore_instance.cpp:547 | 中 |

### 3.3 加固方案

```
[DS_SHUTDOWN] StorageInstance::ShutdownInstance started.
[DS_SHUTDOWN] StopBgThreads completed.
[DS_SHUTDOWN] FlushAll completed.                     // 或 FlushAll failed
[DS_SHUTDOWN] CloseAllPdb completed.
[DS_SHUTDOWN] BufMgrDestroy completed.
[DS_SHUTDOWN] All lock managers destroyed.
[DS_SHUTDOWN] StorageInstance::ShutdownInstance completed.
```

---

## 4. PDB 生命周期日志分析

### 4.1 当前现状

**OpenPdb() (dstore_pdb.cpp:527)**

```
BgPageWriterMgrInit → CheckPdbInfo → InitControlFile → InitTableSpaceMgr
→ InitWatchDogMgr → InitWalMgr → InitCheckpointMgr → InitUndoMgr
→ InitTransactionMgr → InitObjSpaceMgr → InitRecoveryThreadAndWaitDone → StartBgThread
```

**ClosePdb() (dstore_pdb.cpp:595)**

```
StopBgThread → ResetPdb
```

### 4.2 问题清单

| 编号 | 问题 | 位置 | 严重程度 |
|------|------|------|---------|
| P-1 | **OpenPdb 入口/出口无日志**：无法确认 PDB 何时开始打开、何时完成 | dstore_pdb.cpp:527 | 高 |
| P-2 | **OpenPdb 中间步骤无日志**：InitTableSpaceMgr、InitWalMgr、InitCheckpointMgr、InitUndoMgr、InitTransactionMgr 等成功后无日志，失败路径也只有部分有日志 | dstore_pdb.cpp:569-582 | 高 |
| P-3 | **ClosePdb 入口无日志**：只有 StopBgThread 内部有日志，ClosePdb 本身没有开始/结束日志 | dstore_pdb.cpp:595 | 中 |
| P-4 | **OpenPdb 的 IsInit() 检查直接返回成功无日志**：如果 PDB 已经初始化，静默返回，无法区分是正常还是重复打开 | dstore_pdb.cpp:531-532 | 低 |

### 4.3 加固方案

```
[PDB_OPEN] OpenPdb started, pdbId=%u.
[PDB_OPEN] ControlFile initialized, pdbId=%u.
[PDB_OPEN] WatchDogMgr initialized, pdbId=%u.
[PDB_OPEN] WalMgr initialized, pdbId=%u.
[PDB_OPEN] CheckpointMgr initialized, pdbId=%u.
[PDB_OPEN] UndoMgr initialized, pdbId=%u.
[PDB_OPEN] TransactionMgr initialized, pdbId=%u.
[PDB_OPEN] ObjSpaceMgr initialized, pdbId=%u.
[PDB_OPEN] Recovery completed, pdbId=%u.
[PDB_OPEN] Background threads started, pdbId=%u.
[PDB_OPEN] OpenPdb completed, pdbId=%u.

[PDB_CLOSE] ClosePdb started, pdbId=%u.
[PDB_CLOSE] ClosePdb completed, pdbId=%u.
```

---

## 5. 后台线程日志分析

### 5.1 后台线程总览

| 线程 | ThreadMain 入口 | 启动日志 | 退出日志 | 运行时日志 | 说明 |
|------|-----------------|---------|---------|-----------|------|
| **BgDiskPageMasterWriter** | BgPageWriterMain → Run() | ✅ Run():100 | ✅ BgPageWriterExit():57 | ✅ DEBUG1 级刷脏统计 | 退出日志在基类，缺少 pdbId/walId |
| **BgDiskPageSlaveWriter** | SlavePageWriterMain → Run() | ✅ Run():622 | ✅ Run():669 + BgPageWriterExit() | ✅ DEBUG1 级 | 退出日志缺少 slaveIndex |
| **Checkpointer** | CheckpointThreadMain → CheckpointerMain() | ✅ CheckpointThreadMain:2448 | ✅ CheckpointThreadMain:2452 | 部分有 | CheckpointerMain 内部无启停日志，但外层 wrapper 有 |
| **BgWalWriter** | BgFlushMain() | ✅ 161 | ✅ 199 | ✅ | 日志较完整 |
| **WAL Recycle Worker** | RecycleWalFileWorkerMain() | ✅ 554 | ❌ **无** | 部分有 | RECYCLE_WAL_END 标签处无退出日志 |
| **BtreeRecycleWorker** | BtreeRecycleThreadMain() | ❌ **无** | ❌ **无** | 部分有（错误路径） | 注册了 WatchDog 但没打启动日志 |
| **UndoRecycler** | RecycleUndoThreadMain() | ✅ 2412 | ✅ 2418 | ❌ **无运行时日志** | 主循环 RecycleUndo() 内无任何日志 |
| **AsyncRecoverUndo** | AsyncRecoverUndoThreadMain() | ✅ 2436 | ✅ 2438 | ❌ **无** | 仅有启停，无进度/错误 |
| **ObjSpaceMgr Worker** | ObjSpaceMgrWorkerThreadMain() | ❌ **无启动日志** | ✅ 2687 | ✅ 有（错误+慢任务） | 有 DEBUG1 级任务日志和 5s 慢任务告警，但无启动日志 |
| **Recovery** | RecoveryThreadMain() | ❌ **无** | ❌ **无** | 部分有（失败 PANIC） | 只有 InitRecoveryThreadAndWaitDone 的外层日志，线程自身无启停日志 |
| **WatchDog** | WatchDogThreadMain() | ✅ | ✅ | ✅ | 日志完整 |

### 5.2 问题清单

| 编号 | 问题 | 位置 | 严重程度 |
|------|------|------|---------|
| T-1 | **BtreeRecycleWorker 无启动日志**：BtreeRecycleThreadMain() 注册了 WatchDog 但没有打印"已启动"日志 | dstore_btree_page_recycle.cpp:495-504 | 中 |
| T-2 | **BtreeRecycleWorker 无退出日志**：EXIT 标签处直接释放资源退出，不打印 | dstore_btree_page_recycle.cpp:524 | 中 |
| T-3 | **WAL Recycle Worker 无退出日志**：RECYCLE_WAL_END 标签处 Unregister WatchDog 后直接退出 | dstore_wal_file_manager.cpp:610-616 | 中 |
| T-4 | **ObjSpaceMgr Worker 无启动日志**：线程函数入口无"已启动"日志，仅在退出时有日志 | dstore_pdb.cpp:2627 | 中 |
| T-5 | **RecoveryThreadMain 无启停日志**：线程自身不打印启动/完成日志，只有外层 InitRecoveryThreadAndWaitDone 有一条 start 日志，且无完成日志 | dstore_pdb.cpp:2530-2551 | 高 |
| T-6 | **UndoRecycler 运行时无任何日志**：RecycleUndo() 主循环每 50ms 执行一次回收，但成功/失败/回收量均无日志 | dstore_pdb.cpp:2369-2391 | 中 |
| T-7 | **Checkpointer 中 checkpoint 操作无耗时日志**：CheckpointOneWalStream 执行后不打印耗时和刷脏页数量 | dstore_checkpointer.cpp:95-100 | 中 |
| T-8 | **BgPageWriterExit 退出日志缺少关键上下文**：只打印了 pid 和 shutdownRequest，缺少 pdbId、walId、slotId 等信息 | dstore_bg_page_writer_base.cpp:57 | 中 |
| T-9 | **BgPageWriterMain Init 失败不打印原因**：`ret = ... ? bgPageWriter->Init() : ret`，失败后直接退出，无错误日志 | dstore_bg_page_writer_mgr.cpp:271-277 | 中 |
| T-10 | **SlavePageWriterMain Init 失败只打印 retStatus 值**：`ErrLog("Disk SlavePageWriterMain retStatus = %d")`，未说明是哪个 Init 步骤失败 | dstore_bg_disk_page_writer.cpp:308 | 低 |
| T-11 | **StartBgThread() 无总括日志**：不知道 PDB 启动了哪些后台线程 | dstore_pdb.cpp:2170 | 中 |
| T-12 | **BgWalWriter 启动日志用了 WARNING 级别**：`ErrLog(DSTORE_WARNING, ...)` 打印启动信息不合理，应为 LOG 级别 | dstore_wal_bgwriter.cpp:126 | 低 |
| T-13 | **RecycleWalFileForDropping 失败无具体原因**：主循环中 drop 场景失败只 sleep 重试，不打印原因 | dstore_wal_file_manager.cpp:573-575 | 低 |

### 5.3 加固方案

#### 5.3.1 补充缺失的启停日志

**BtreeRecycleWorker**（dstore_btree_page_recycle.cpp）：
```cpp
// 在 WatchDog 注册之后（~505行），while 循环之前
ErrLog(DSTORE_LOG, MODULE_INDEX,
    ErrMsg("[BG_THREAD] BtreeRecycler started, pdbId=%u, workerId=%u.", m_pdbId, m_workeId));

// EXIT 标签处，ReleaseSQLThreadContext 之前
ErrLog(DSTORE_LOG, MODULE_INDEX,
    ErrMsg("[BG_THREAD] BtreeRecycler stopped, pdbId=%u, workerId=%u.", m_pdbId, m_workeId));
```

**WAL Recycle Worker**（dstore_wal_file_manager.cpp）：
```cpp
// RECYCLE_WAL_END 标签处，Unregister 之后
ErrLog(DSTORE_LOG, MODULE_WAL,
    ErrMsg("[BG_THREAD] WalRecycler stopped, pdbId=%u, walId=%lu.",
           m_initWalFilesPara.pdbId, m_initWalFilesPara.walId));
```

**ObjSpaceMgr Worker**（dstore_pdb.cpp）：
```cpp
// ObjSpaceMgrWorkerThreadMain 入口，while 循环之前
ErrLog(DSTORE_LOG, MODULE_SEGMENT,
    ErrMsg("[BG_THREAD] ObjSpaceMgr worker started, pdbId=%u, workerId=%u.", GetPdbId(), workerId));
```

**RecoveryThreadMain**（dstore_pdb.cpp）：
```cpp
// CreateThreadAndRegister 之后
ErrLog(DSTORE_LOG, MODULE_FRAMEWORK,
    ErrMsg("[BG_THREAD] Recovery thread started, pdbId=%u.", m_pdbId));
// UnregisterThread 之前
ErrLog(DSTORE_LOG, MODULE_FRAMEWORK,
    ErrMsg("[BG_THREAD] Recovery thread stopped, pdbId=%u.", m_pdbId));
```

#### 5.3.2 补充运行时关键日志

**UndoRecycler — RecycleUndo()**（dstore_pdb.cpp:2369）：

当前主循环完全无日志。建议在每次回收完成后（或按时间间隔采样）打印回收进度：
```cpp
// m_undoMgr->Recycle(recycleMinCsn) 之后，以一定间隔打印
ErrLog(DSTORE_LOG, MODULE_UNDO,
    ErrMsg("[UNDO_RECYCLE] UndoRecycler completed one round, pdbId=%u, recycleMinCsn=%lu.",
           m_pdbId, recycleMinCsn));
```
> 注意：RecycleUndo 每 50ms 执行一次，不宜每次都打印。可用计数器控制，如每 100 轮打印一次（约 5 秒），或仅在回收了实际页面时打印。

**Checkpointer — checkpoint 耗时**（dstore_checkpointer.cpp:95-100）：
```cpp
// CheckpointOneWalStream 执行前后计时
uint64 ckptStart = GetSystemTimeInMicrosecond();
if (STORAGE_FUNC_FAIL(CheckpointOneWalStream(walId, flag, &isPerformed))) {
    goto LOOP;
}
if (isPerformed) {
    uint64 ckptElapsed = (GetSystemTimeInMicrosecond() - ckptStart) / 1000;
    ErrLog(DSTORE_LOG, MODULE_BUFMGR,
        ErrMsg("[CHECKPOINT] walId=%lu, pdbId=%u completed, elapsed=%lu ms.",
               walId, m_pdbId, ckptElapsed));
}
```

#### 5.3.3 丰富已有日志的上下文

**BgPageWriterExit()**（dstore_bg_page_writer_base.cpp:57）：

当前只打印 pid 和 shutdownRequest。建议增加 pdbId，但由于基类不一定有 pdbId 字段，可由子类在调用 BgPageWriterExit 前打印更详细的退出日志。

**BgPageWriterMain Init 失败**（dstore_bg_page_writer_mgr.cpp:271）：
```cpp
ret = STORAGE_FUNC_SUCC(ret) ? bgPageWriter->Init() : ret;
if (STORAGE_FUNC_FAIL(ret)) {
    ErrLog(DSTORE_ERROR, MODULE_BGPAGEWRITER,
        ErrMsg("[BG_THREAD] BgPageWriter Init failed, pdbId=%u.", pdbId));
}
```

#### 5.3.4 StartBgThread 总括日志

```cpp
void StoragePdb::StartBgThread()
{
    if (g_defaultPdbId == PDB_ROOT_ID && IsTemplate(GetPdbId())) {
        return;
    }
    ErrLog(DSTORE_LOG, MODULE_FRAMEWORK,
        ErrMsg("[BG_THREAD] Starting background threads for pdbId=%u, role=%d.",
               GetPdbId(), static_cast<int>(GetPdbRoleMode())));
    // ... existing code ...
    ErrLog(DSTORE_LOG, MODULE_FRAMEWORK,
        ErrMsg("[BG_THREAD] All background threads started for pdbId=%u.", GetPdbId()));
}
```

---

## 6. 错误码体系分析

### 6.1 当前现状

```cpp
enum RetStatus : int {
    DSTORE_FAIL = -1,
    DSTORE_SUCC = 0,
};
```

整个系统只有两个返回值：成功和失败。所有失败场景统一返回 `DSTORE_FAIL`。

### 6.2 问题清单

| 编号 | 问题 | 影响 | 严重程度 |
|------|------|------|---------|
| E-1 | **错误码过于笼统**：所有错误都是 DSTORE_FAIL，调用方无法区分是参数错误、资源不足、I/O 错误还是状态异常 | 上层 server 无法做差异化处理（如重试 vs 告警 vs panic） | 高 |
| E-2 | **错误信息依赖日志而非返回值**：当前的做法是 ErrLog 记录原因 + 返回 DSTORE_FAIL。如果中间有调用链，内层的错误原因在外层被丢失 | 深层调用的错误细节在上层不可见 | 高 |
| E-3 | **部分关键路径返回失败时无日志**：Bootstrap/StartupInstance 中多个 Init 步骤失败时直接返回 DSTORE_FAIL 无日志（见 S-2） | 完全无法定位是哪个步骤失败 | 高 |

### 6.3 加固方案（分两阶段）

**第一阶段（短期）— 日志加固，不改错误码体系**：

在所有返回 `DSTORE_FAIL` 的路径上，确保有 `ErrLog` 记录：
- 失败的操作名称
- 关键上下文（pdbId、walId、threadName 等）
- 如果有底层错误码（如 VFS 的 ErrorCode），打印出来

重点关注：
1. Bootstrap/StartupInstance 中所有 Init 步骤的失败路径（S-2）
2. OpenPdb 中各 Init 步骤的失败路径（P-2）
3. 后台线程 Init 失败路径（T-8、T-9）

**第二阶段（中期）— 扩展错误码**：

```cpp
enum RetStatus : int {
    DSTORE_FAIL = -1,
    DSTORE_SUCC = 0,
    DSTORE_ERR_INVALID_PARAM = -2,     // 参数非法
    DSTORE_ERR_OUT_OF_MEMORY = -3,     // 内存分配失败
    DSTORE_ERR_IO = -4,                // I/O 错误
    DSTORE_ERR_STATE = -5,             // 状态异常（如未初始化就操作）
    DSTORE_ERR_RESOURCE_BUSY = -6,     // 资源被占用
    DSTORE_ERR_NOT_FOUND = -7,         // 资源不存在
    DSTORE_ERR_TIMEOUT = -8,           // 操作超时
};
```

> **注意**：第二阶段改动面较大，需要逐模块替换。建议先完成第一阶段的日志加固，再视需要推进第二阶段。

---

## 7. 统一日志前缀规范

为方便 grep 和日志分析，建议各关键路径使用统一的前缀标识：

| 前缀 | 用途 | 示例 |
|------|------|------|
| `[DS_INIT]` | 实例初始化流程 | `[DS_INIT] BufMgrInit completed, buffer=300000.` |
| `[DS_SHUTDOWN]` | 实例关闭流程 | `[DS_SHUTDOWN] CloseAllPdb completed.` |
| `[PDB_OPEN]` | PDB 打开流程 | `[PDB_OPEN] OpenPdb started, pdbId=3.` |
| `[PDB_CLOSE]` | PDB 关闭流程 | `[PDB_CLOSE] ClosePdb completed, pdbId=3.` |
| `[BG_THREAD]` | 后台线程启停 | `[BG_THREAD] Checkpointer started, pdbId=3.` |
| `[CHECKPOINT]` | Checkpoint 操作 | `[CHECKPOINT] Checkpoint completed, walId=1, elapsed=120ms.` |
| `[StopBgThread]` | 后台线程停止（已有） | 保持现有格式不变 |
| `WatchDog:` | WatchDog 相关（已有） | 保持现有格式不变 |
| `WatchDog STUCK DETECTED:` | 卡死告警（已有） | 保持现有格式不变 |

---

## 8. 优先级排序

### P0 — 必须修复（影响问题定位）

1. **S-1/S-2/S-3**：Bootstrap/StartupInstance 添加启动入口日志 + 每步失败日志
2. **D-1/D-2**：ShutdownInstance 添加关闭入口/出口日志 + 每步完成日志
3. **P-1/P-2**：OpenPdb 添加入口/出口日志 + 各 Init 步骤日志
4. **T-5**：RecoveryThreadMain 补充启停日志（恢复流程问题定位关键）
5. **E-3**：所有返回 DSTORE_FAIL 但无 ErrLog 的路径补充日志

### P1 — 建议修复（提升可观测性）

6. **T-1/T-2**：BtreeRecycler 补充启停日志
7. **T-3**：WAL Recycle Worker 补充退出日志
8. **T-4**：ObjSpaceMgr Worker 补充启动日志
9. **T-6**：UndoRecycler 运行时增加采样日志
10. **T-7**：Checkpoint 操作增加耗时统计
11. **T-8**：BgPageWriterExit 丰富上下文（pdbId、walId）
12. **T-9**：BgPageWriterMain Init 失败补充日志
13. **T-11**：StartBgThread 增加总括日志
14. **S-4**：Init 步骤成功日志

### P2 — 低优先级 / 长期规划

15. **T-10**：SlavePageWriterMain Init 失败日志优化
16. **T-12**：BgWalWriter 启动日志级别修正（WARNING → LOG）
17. **T-13**：RecycleWalFileForDropping 失败增加原因日志
18. **E-1/E-2**：错误码体系扩展

---

## 9. 改动范围估算

| 文件 | 改动内容 |
|------|---------|
| `src/framework/dstore_instance.cpp` | Bootstrap/StartupInstance/ShutdownInstance 添加日志 |
| `src/framework/dstore_pdb.cpp` | OpenPdb/ClosePdb/StartBgThread/RecoveryThreadMain/RecycleUndo/ObjSpaceMgrWorkerThreadMain 添加日志 |
| `src/buffer/dstore_checkpointer.cpp` | Checkpoint 操作耗时日志 |
| `src/buffer/dstore_bg_page_writer_mgr.cpp` | BgPageWriterMain Init 失败日志 |
| `src/buffer/dstore_bg_page_writer_base.cpp` | BgPageWriterExit 丰富上下文 |
| `src/wal/dstore_wal_file_manager.cpp` | RecycleWalFileWorkerMain 添加退出日志 |
| `src/wal/dstore_wal_bgwriter.cpp` | BgWalWriter 启动日志级别修正 |
| `src/index/dstore_btree_page_recycle.cpp` | BtreeRecycleThreadMain 添加启停日志 |
