# dstore 日志补充分析报告

> 分析日期：2026-03-26
> 分析范围：单节点模式下的关键路径和后台线程
> 说明：本文档只列**缺失**或**不足**的日志，已有充分日志的地方不重复描述

---

## 一、启动 / 停止路径

### 1.1 `StorageInstance::ShutdownInstance()`
**文件**：`src/framework/dstore_instance.cpp:539`

**现状**：函数内**零条日志**，整个关闭流程完全不可观测。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 函数入口 | `"ShutdownInstance begin."` | LOG |
| `StopBgThreads()` 调用后 | `"StopBgThreads done."` | LOG |
| `CloseAllPdb()` 调用后 | `"CloseAllPdb done."` | LOG |
| `BufMgrDestroy()` 等各组件销毁后 | `"BufMgrDestroy / DestroyCsnMgr / DestroyLockMgr done."` | LOG |
| 函数末尾 | `"ShutdownInstance complete."` | LOG |

---

### 1.2 `StorageInstance::OpenOnePdb()`
**文件**：`src/framework/dstore_instance.cpp:1393`

**现状**：函数内**零条日志**，PDB 启动的完整过程不可观测。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 函数入口 | `"OpenOnePdb begin, pdbId=%u."` | LOG |
| `InitPdb()` 调用后（成功） | `"InitPdb success, pdbId=%u."` | LOG |
| `StartBgThreads()` 调用后 | `"StartBgThreads done, pdbId=%u."` | LOG |
| 函数末尾 | `"OpenOnePdb complete, pdbId=%u."` | LOG |

---

### 1.3 `StoragePdb::InitPdb()`
**文件**：`src/framework/dstore_pdb.cpp:394`

**现状**：只有 `InitVFS` / `InitObjSpaceMgr` 失败的 ERROR，以及末尾的耗时 LOG。各子模块初始化全程哑火。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 函数入口 | `"InitPdb begin, pdbId=%u."` | LOG |
| `InitWalMgr()` 之后 | `"InitWalMgr success, pdbId=%u."` | LOG |
| `InitUndoMgr()` 之后 | `"InitUndoMgr success, pdbId=%u."` | LOG |
| `InitTransactionMgr()` 之后 | `"InitTransactionMgr success, pdbId=%u."` | LOG |
| `InitRecoveryThreadAndWaitDone()` 之前/之后 | `"InitRecovery begin/done, pdbId=%u."` | LOG |
| `StartBgThread()` 之后 | `"StartBgThread done, pdbId=%u."` | LOG |

> **注**：`InitControlFile()` 失败走的是 PANIC，无需额外 LOG。

---

### 1.4 `StoragePdb::ClosePdb()`
**文件**：`src/framework/dstore_pdb.cpp:595`

**现状**：只有 `ResetPdb` 失败的 ERROR，正常关闭路径无任何日志。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 函数入口 | `"ClosePdb begin, pdbId=%u, needFullCheckpoint=%d."` | LOG |
| `StopBgThread()` 之后 | `"StopBgThread done, pdbId=%u."` | LOG |
| 函数末尾（成功） | `"ClosePdb success, pdbId=%u."` | LOG |

---

## 二、Checkpoint 线程

### 2.1 `CheckpointMgr::CheckpointerMain()`
**文件**：`src/buffer/dstore_checkpointer.cpp:30`

**现状**：有 checkpoint 完成时的详细 LOG（耗时、PLSN 推进量等），但线程本身无启动/退出日志，长时间无 checkpoint 触发时日志完全沉默。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 主循环进入前（注册 WatchDog 之后） | `"CheckpointerMain started, pdbId=%u."` | LOG |
| `m_shutdownRequested` 退出后 | `"CheckpointerMain stopped, pdbId=%u."` | LOG |
| 空闲等待超过一定周期（如 60s）无 checkpoint | `"CheckpointerMain idle for %us, pdbId=%u."` | DEBUG1 |

---

## 三、Buffer 刷脏线程

### 3.1 `BgPageWriterMgr::BgPageWriterMain()`（模板函数）
**文件**：`src/buffer/dstore_bg_page_writer_mgr.cpp:256`

**现状**：函数内**零条日志**。该函数是 `BgDiskPageMasterWriter` 的线程入口包装，负责线程注册、Init、启动 `Run()`，出错时完全不可见。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| `CreateThreadAndRegister` 失败 | `"BgPageWriterMain register thread fail, pdbId=%u."` | ERROR |
| `bgPageWriter->Init()` 失败 | `"BgPageWriter Init fail, pdbId=%u."` | ERROR |
| `bgPageWriter->Run()` 返回后 | `"BgPageWriter exited, pdbId=%u."` | LOG |

---

### 3.2 `BgDiskPageSlaveWriter::Run()`
**文件**：`src/buffer/dstore_bg_disk_page_writer.cpp`

**现状**：有启动/退出 LOG，有每批次抢占的 DEBUG1 日志。缺少以下内容：

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| `FlushCandidateDirtyPage` 失败（目前直接 PANIC）前 | 在 PANIC 之前打印 `"Flush page failed, bufTag=(%u,%hu,%u)."` | ERROR |
| 定期汇报（可选，每 N 次循环） | `"SlaveWriter flushed %u pages in this batch, pdbId=%u."` | DEBUG1（已有，可考虑提升到 LOG 级别） |

> Slave 整体日志已基本够用，优先级低。

---

## 四、WAL 相关线程

### 4.1 `WalFileManager::RecycleWalFileWorkerMain()`
**文件**：`src/wal/dstore_wal_file_manager.cpp:530`

**现状**：只有线程启动一条 LOG（主节点情况下），主循环内部完全无日志。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 主循环退出后 | `"RecycleWalFileWorker stopped, pdbId=%u, walId=%lu."` | LOG |
| 每次成功回收一个 WAL 文件后 | `"Recycled wal file %s, pdbId=%u, walId=%lu."` | LOG |
| 每次成功创建一个新 WAL 文件后 | `"Created new wal file, pdbId=%u, walId=%lu, current count=%u."` | LOG |
| 无文件可回收时（定期，可按次数限频） | `"RecycleWalFileWorker: nothing to recycle, current wal file count=%u."` | DEBUG1 |
| 线程异常退出（`pdb == nullptr` 之外的错误路径） | 补充对应 ERROR | ERROR |

---

## 五、Undo 回收相关线程

### 5.1 `RollbackTrxTaskMgr::DispatchMain()` 及 `DoDispatch()`
**文件**：`src/undo/dstore_rollback_trx_task_mgr.cpp`

**现状**：`DispatchMain` 线程初始化有错误日志，但线程启动/退出无日志；`DoDispatch()` 内部完全无日志。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| `DispatchMain` 主循环进入前 | `"RollbackTrxTaskMgr DispatchMain started, pdbId=%u."` | LOG |
| `DispatchMain` 退出后 | `"RollbackTrxTaskMgr DispatchMain stopped, pdbId=%u."` | LOG |
| `DoDispatch` 中每次成功分配一个任务给 worker | `"Dispatched rollback task xid(%d,%lu) to worker, pdbId=%u."` | DEBUG1 |
| `DoDispatch` 中任务队列长度超过阈值（如 > 5） | `"Rollback task queue depth=%u, pdbId=%u."` | WARNING |
| 无空闲 worker 时（限频打印） | `"No idle rollback worker available, pdbId=%u."` | WARNING |

---

### 5.2 `RollbackTrxWorker::WorkerMain()`
**文件**：`src/undo/dstore_rollback_trx_worker.cpp:96`

**现状**：有创建线程失败的 ERROR、有回滚失败后重新入队的 LOG、有 DEBUG1 级别的开始日志；缺少线程正常完成日志。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 回滚成功完成后 | `"Async rollback success, pdbId=%u, xid(%d,%lu)."` | LOG |
| `UnregisterThread()` 之前（线程正常退出） | `"RollbackTrxWorker exiting, pdbId=%u."` | DEBUG1 |

---

## 六、BTree 回收线程

### 6.1 `BtreeRecycleWorker::BtreeRecycleThreadMain()`
**文件**：`src/index/dstore_btree_page_recycle.cpp:457`

**现状**：有初始化失败的 ERROR、有获取事务失败的 ERROR；但线程启动成功、任务执行完成、线程退出均无日志。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| 初始化成功、进入主循环前 | `"BtreeRecycleWorker started, workerId=%u, pdbId=%u."` | LOG |
| 每次 `task->ExecuteRecycleBtreeTask()` 成功后 | `"BtreeRecycle task done, workerId=%u, pdbId=%u, result=%d."` | LOG |
| `ExecuteRecycleBtreeTask()` 失败后 | `"BtreeRecycle task failed, workerId=%u, pdbId=%u."` | ERROR |
| `goto EXIT` 正常退出时 | `"BtreeRecycleWorker stopped, workerId=%u, pdbId=%u."` | LOG |

---

## 七、其他线程

### 7.1 `StorageInstance::UpdateCsnMinThreadMain()`
**文件**：`src/framework/dstore_instance.cpp:1421`

**现状**：只有启动/停止各一条 LOG，`UpdateCsnMinThreadLoop()` 内部执行过程完全不可见。

| 缺失位置 | 建议内容 | 级别 |
|---------|---------|------|
| `UpdateCsnMinThreadLoop` 内每更新一次 CSN min（可限频） | `"Updated CsnMin to %lu."` | DEBUG1 |
| 更新失败时 | 对应 ERROR | ERROR |

> `UpdateCsnMinThreadLoop` 目前逻辑不明，建议阅读其实现后决定补充粒度。

---

## 优先级汇总

| 优先级 | 位置 | 问题 |
|--------|------|------|
| P0 | `ShutdownInstance` | 关闭流程完全无日志，问题定位困难 |
| P0 | `OpenOnePdb` | 启动流程完全无日志 |
| P0 | `BgPageWriterMgr::BgPageWriterMain` | 线程入口无任何日志，Init/Run 失败不可见 |
| P1 | `InitPdb` | 各子模块初始化无进度日志 |
| P1 | `ClosePdb` | 正常关闭路径无日志 |
| P1 | `RecycleWalFileWorkerMain` | 主循环无日志，WAL 回收操作不可见 |
| P1 | `DispatchMain` / `DoDispatch` | 线程无启动/退出日志，任务分配不可见 |
| P1 | `BtreeRecycleThreadMain` | 线程无启动/退出日志，任务完成不可见 |
| P2 | `CheckpointerMain` | 缺线程启动/退出日志，长时间空闲时沉默 |
| P2 | `RollbackTrxWorker::WorkerMain` | 缺回滚成功日志 |
| P2 | `UpdateCsnMinThreadMain` | 执行过程不可见 |
| P3 | `BgDiskPageSlaveWriter::Run` | 已有基本日志，Flush 失败前缺 ERROR |
