# WatchDog 后台线程监控设计方案

## 1. 需求概述

实现 WatchDog 功能，监控 dstore 后台线程是否卡死，并提供两种告警方式：
1. **打印 ERROR 日志**：当检测到线程卡死时，输出 ErrLog
2. **Diagnose 接口**：上层 server 层可直接调用 `WatchDogDiagnose` 的 static 方法查询各后台线程状态（参考 `HeapDiagnose::GetPageFreespace` 模式）

### 1.1 需要监控的后台线程

| 线程 | 类 | 主循环位置 | 休眠方式 |
|------|-----|-----------|---------|
| Buffer Master Writer | `BgDiskPageMasterWriter` | `Run()` | `SmartSleep()`（自适应，基于 GUC `bgPageWriterSleepMilliSecond`） |
| Buffer Slave Writer | `BgDiskPageSlaveWriter` | `Run()` | `std::condition_variable` 等待 master 唤醒 |
| Checkpoint | `CheckpointMgr` | `CheckpointerMain()` | 1 秒轮询（1ms 分片） |
| WAL 日志回收 | `WalFileManager` | `RecycleWalFileWorkerMain()` | 10ms 轮询 |
| Undo 回收 | `StoragePdb` | `RecycleUndoThreadMain()` | 内部循环休眠 |
| B-tree Recycle | `BtreeRecycleWorker` | `BtreeRecycleThreadMain()` | futex 等待任务 |

## 2. 整体架构

```
                          StoragePdb
                             |
                        WatchDogMgr（每个 PDB 一个）
                        /          \
              监控线程(WatchDogThread)   WatchDogDiagnose（static 接口）
                  |                        |
          周期性检查各线程心跳            上层 server 直接调用 static 方法
                  |                        |
          超时 → ErrLog(ERROR)        返回结构化数据 / 格式化字符串
```

### 2.1 核心设计思路

采用**心跳（Heartbeat）+ 看门狗线程（WatchDog Thread）**模式：

- 每个被监控线程在主循环每次迭代时更新自身的**心跳时间戳**（原子写入 `steady_clock::now()`）
- WatchDog 线程周期性检查所有被监控线程的心跳，如果心跳超时则告警
- 心跳更新是轻量操作（一次原子 store），对被监控线程几乎零开销

### 2.2 为什么不用其他方案

| 方案 | 缺点 |
|------|------|
| 被监控线程主动上报 | 卡死时无法上报，失去监控意义 |
| 共享 counter 递增 | 需要 WatchDog 记住上次值比较，不如时间戳直观 |
| 信号机制（SIGUSR） | 侵入性强，可能干扰线程正常逻辑 |

## 3. 详细设计

### 3.1 心跳数据结构

```cpp
// include/framework/dstore_watchdog.h

// 被监控线程的类型枚举
enum class WatchDogThreadType : uint8 {
    BG_PAGE_MASTER_WRITER = 0,
    BG_PAGE_SLAVE_WRITER,
    CHECKPOINTER,
    WAL_FILE_RECYCLE,
    UNDO_RECYCLE,
    BTREE_RECYCLE,
    WATCHDOG_THREAD_TYPE_COUNT   // 哨兵值，用于数组大小
};

// 线程运行状态
enum class ThreadRunState : uint8 {
    NOT_STARTED = 0,    // 线程未启动
    RUNNING,            // 正常运行中
    SLEEPING,           // 正常休眠中（等待任务/定时唤醒）
    STUCK,              // 疑似卡死（心跳超时）
    STOPPED             // 已停止
};

// 单个线程的心跳信息
struct WatchDogHandle {
    std::atomic<uint64> lastHeartbeatUs;         // 上次心跳时间（微秒，steady_clock）
    std::atomic<ThreadRunState> runState;         // 当前运行状态
    WatchDogThreadType threadType;                // 线程类型
    uint32 threadIndex;                           // 同类型线程的索引（如 slave writer 0/1/2...）
    char threadName[64];                           // 线程名称（固定数组，Register 时 strncpy 拷贝）
    std::atomic<bool> registered;                 // 是否已注册（用于遍历时跳过空槽位）
};
```

### 3.2 心跳注册与更新接口

```cpp
// include/framework/dstore_watchdog.h

class WatchDogMgr {
public:
    WatchDogMgr();
    ~WatchDogMgr();
    DISALLOW_COPY_AND_MOVE(WatchDogMgr);

    // 生命周期
    RetStatus Init(PdbId pdbId);
    void StartThread();     // 启动 WatchDog 线程，阻塞等待线程就绪后返回
    void WaitReady() const; // 阻塞等待 m_ready 标志
    void Destroy();

    // === 被监控线程调用的接口 ===

    // 注册一个被监控线程，返回心跳句柄（指针）
    // 线程启动时调用，传入类型、索引、名称
    // 超时阈值从 GUC 参数读取
    WatchDogHandle *Register(WatchDogThreadType type, uint32 index,
                                const char *threadName);

    // 注销，线程退出时调用
    void Unregister(WatchDogHandle *heartbeat);

    // 心跳更新（被监控线程在主循环中调用）
    // 内联实现，尽量轻量
    static inline void TouchHeartbeat(WatchDogHandle *hb);

    // 设置线程状态（可选，用于区分 RUNNING/SLEEPING）
    static inline void SetRunState(WatchDogHandle *hb, ThreadRunState state);

    // === WatchDog 线程内部方法 ===

    // WatchDog 主循环（作为后台线程运行）
    void WatchDogThreadMain();

    // === 查询方法（供 WatchDogDiagnose 及外部调用） ===

    // 根据线程类型从 GUC 获取超时阈值（微秒）
    uint64 GetTimeoutUsFromGuc(WatchDogThreadType type) const;

    // 获取当前稳定时钟时间（微秒）
    static uint64 GetSteadyClockUs();

    // 获取当前所有心跳的快照
    uint32 GetHeartbeatSnapshot(WatchDogHandle *outArray, uint32 maxCount) const;

    // 获取指定类型的心跳
    uint32 GetHeartbeatsByType(WatchDogThreadType type,
                               WatchDogHandle *outArray, uint32 maxCount) const;

    // 查询接口
    PdbId GetPdbId() const;
    bool IsStopped() const;
    bool IsReady() const;
    uint32 GetRegisteredCount() const;

    // 心跳数组（固定大小，预分配）
    static constexpr uint32 MAX_WATCHED_THREADS = 64;

#ifndef UT
private:
#endif
    // 检查所有心跳，对超时的打印 ERROR 日志
    void CheckAllHeartbeats();

    PdbId m_pdbId;
    std::atomic<bool> m_stop;
    std::atomic<bool> m_ready;        // WatchDog 线程就绪标志，StartThread 阻塞等待此标志
    std::thread *m_watchdogThread;

    WatchDogHandle m_heartbeats[MAX_WATCHED_THREADS];
    std::atomic<uint32> m_registeredCount;
};
```

### 3.3 心跳更新（内联，零开销）

```cpp
inline void WatchDogMgr::TouchHeartbeat(WatchDogHandle *hb)
{
    if (hb == nullptr) {
        return;
    }
    auto now = std::chrono::steady_clock::now();
    uint64 nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    hb->lastHeartbeatUs.store(nowUs, std::memory_order_relaxed);
}

inline void WatchDogMgr::SetRunState(WatchDogHandle *hb, ThreadRunState state)
{
    if (hb != nullptr) {
        hb->runState.store(state, std::memory_order_relaxed);
    }
}
```

### 3.4 各线程心跳埋点位置

#### 3.4.1 Buffer Master Writer（`BgDiskPageMasterWriter::Run()`）

```cpp
void BgDiskPageMasterWriter::Run()
{
    m_watchdogHandle = watchdogMgr->Register(
        WatchDogThreadType::BG_PAGE_MASTER_WRITER, static_cast<uint32>(m_slotId), "MasterWriter");

    for (;;) {
        if (IsStop()) { break; }

        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::RUNNING);
        // ... 扫描脏页、唤醒 slave、等待完成 ...
        WatchDogMgr::TouchHeartbeat(m_watchdogHandle);

        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::SLEEPING);
        SmartSleep();
    }

    watchdogMgr->Unregister(m_watchdogHandle);
}
```

#### 3.4.2 Buffer Slave Writer（`BgDiskPageSlaveWriter::Run()`）

```cpp
void BgDiskPageSlaveWriter::Run()
{
    m_watchdogHandle = watchdogMgr->Register(
        WatchDogThreadType::BG_PAGE_SLAVE_WRITER, m_slaveIndex, "SlaveWriter");

    while (true) {
        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::SLEEPING);
        WaitNextFlush();

        if (IsStop()) { break; }

        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::RUNNING);
        // ... SeizeDirtyPageListForFlush + FlushCandidateDirtyPage ...
        WatchDogMgr::TouchHeartbeat(m_watchdogHandle);
    }

    watchdogMgr->Unregister(m_watchdogHandle);
}
```

#### 3.4.3 Checkpointer（`CheckpointMgr::CheckpointerMain()`）

```cpp
void CheckpointMgr::CheckpointerMain()
{
    m_watchdogHandle = watchdogMgr->Register(
        WatchDogThreadType::CHECKPOINTER, 0, "Checkpointer");

    while (true) {
        if (m_shutdownRequested) { break; }

        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::RUNNING);
        // ... 遍历 WAL stream 做 checkpoint ...
        WatchDogMgr::TouchHeartbeat(m_watchdogHandle);

        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::SLEEPING);
        // 1 秒轮询休眠
    }

    watchdogMgr->Unregister(m_watchdogHandle);
}
```

#### 3.4.4 WAL 日志回收（`WalFileManager::RecycleWalFileWorkerMain()`）

```cpp
void WalFileManager::RecycleWalFileWorkerMain(bool isDropping)
{
    m_watchdogHandle = watchdogMgr->Register(
        WatchDogThreadType::WAL_FILE_RECYCLE,
        static_cast<uint32>(m_initWalFilesPara.walId), "WalRecycler");

    while (!m_stopBgRecycleThread.load()) {
        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::RUNNING);
        // ... 回收逻辑 ...
        WatchDogMgr::TouchHeartbeat(m_watchdogHandle);

        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::SLEEPING);
        // 10ms 休眠等待循环
        WatchDogMgr::TouchHeartbeat(m_watchdogHandle);
    }

    watchdogMgr->Unregister(m_watchdogHandle);
}
```

#### 3.4.5 Undo 回收（`StoragePdb::RecycleUndoThreadMain()`）

```cpp
void StoragePdb::RecycleUndoThreadMain()
{
    m_undoRecycleWdHandle = watchdogMgr->Register(
        WatchDogThreadType::UNDO_RECYCLE, 0, "UndoRecycler");

    // ... 等待备份恢复完成 ...
    // RecycleUndo() 内部循环中需要埋入心跳
    RecycleUndo();

    watchdogMgr->Unregister(m_undoRecycleWdHandle);
}
```

#### 3.4.6 B-tree Recycle（`BtreeRecycleWorker::BtreeRecycleThreadMain()`）

```cpp
void BtreeRecycleWorker::BtreeRecycleThreadMain()
{
    m_watchdogHandle = watchdogMgr->Register(
        WatchDogThreadType::BTREE_RECYCLE, m_workeId, "BtreeRecycler");

    while (!m_stopRecyleThread) {
        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::SLEEPING);
        thrd->Sleep();

        if (m_stopRecyleThread) { break; }

        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::RUNNING);
        // ... 执行回收任务 ...
        WatchDogMgr::TouchHeartbeat(m_watchdogHandle);
    }

    watchdogMgr->Unregister(m_watchdogHandle);
}
```

### 3.5 超时阈值设计（GUC 参数）

超时阈值不在代码中写死，统一通过 GUC 参数配置。在 `StorageGUC` 结构体中新增以下字段：

```cpp
// interface/framework/dstore_instance_interface.h  StorageGUC 结构体中新增：

uint32_t watchdogCheckIntervalSec;              // WatchDog 检查周期（秒），默认 5
uint32_t watchdogMasterWriterTimeoutSec;        // Master Writer 超时（秒），默认 30
uint32_t watchdogSlaveWriterTimeoutSec;         // Slave Writer 超时（秒），默认 60
uint32_t watchdogCheckpointerTimeoutSec;        // Checkpointer 超时（秒），默认 600
uint32_t watchdogWalRecycleTimeoutSec;          // WAL 日志回收超时（秒），默认 30
uint32_t watchdogUndoRecycleTimeoutSec;         // Undo 回收超时（秒），默认 120
uint32_t watchdogBtreeRecycleTimeoutSec;        // B-tree Recycle 超时（秒），默认 1800
```

**GUC 参数与线程类型的映射：**

`WatchDogMgr::GetTimeoutUsFromGuc()` 根据 `WatchDogThreadType` 读取对应 GUC 参数并转为微秒：

```cpp
uint64 WatchDogMgr::GetTimeoutUsFromGuc(WatchDogThreadType type) const
{
    const StorageGUC *guc = g_storageInstance->GetGuc();
    uint32 timeoutSec = 0;
    switch (type) {
        case WatchDogThreadType::BG_PAGE_MASTER_WRITER:
            timeoutSec = guc->watchdogMasterWriterTimeoutSec;
            break;
        case WatchDogThreadType::BG_PAGE_SLAVE_WRITER:
            timeoutSec = guc->watchdogSlaveWriterTimeoutSec;
            break;
        case WatchDogThreadType::CHECKPOINTER:
            timeoutSec = guc->watchdogCheckpointerTimeoutSec;
            break;
        case WatchDogThreadType::WAL_FILE_RECYCLE:
            timeoutSec = guc->watchdogWalRecycleTimeoutSec;
            break;
        case WatchDogThreadType::UNDO_RECYCLE:
            timeoutSec = guc->watchdogUndoRecycleTimeoutSec;
            break;
        case WatchDogThreadType::BTREE_RECYCLE:
            timeoutSec = guc->watchdogBtreeRecycleTimeoutSec;
            break;
        default:
            timeoutSec = 60; // 默认 60 秒
            break;
    }
    return static_cast<uint64>(timeoutSec) * 1000000ULL;
}
```

**默认值与建议范围：**

| GUC 参数 | 默认值 | 建议范围 | 理由 |
|----------|--------|---------|------|
| `watchdogCheckIntervalSec` | 5 | [1, 60] | 检查频率，5 秒足以及时发现异常 |
| `watchdogMasterWriterTimeoutSec` | 30 | [10, 300] | 正常迭代 < 1 秒 |
| `watchdogSlaveWriterTimeoutSec` | 60 | [10, 600] | 刷脏可能稍慢 |
| `watchdogCheckpointerTimeoutSec` | 600 | [60, 3600] | checkpoint 可能持续较长 |
| `watchdogWalRecycleTimeoutSec` | 30 | [10, 300] | 回收操作很快 |
| `watchdogUndoRecycleTimeoutSec` | 120 | [30, 600] | 涉及较多页面扫描 |
| `watchdogBtreeRecycleTimeoutSec` | 1800 | [60, 7200] | 任务本身可能较慢 |

**注意**：线程处于 `SLEEPING` 状态时，WatchDog 不检测超时（因为等待任务/定时唤醒是正常行为）。只有 `RUNNING` 状态下心跳超时才告警。

### 3.6 WatchDog 线程主循环

```cpp
void WatchDogMgr::WatchDogThreadMain()
{
    while (!m_stop.load(std::memory_order_relaxed)) {
        CheckAllHeartbeats();

        uint32 intervalSec = 5;
        const StorageGUC *guc = (g_storageInstance != nullptr) ? g_storageInstance->GetGuc() : nullptr;
        if (guc != nullptr) {
            intervalSec = guc->watchdogCheckIntervalSec;
        }
        if (intervalSec == 0) {
            intervalSec = 1;
        }

        uint32 totalChunks = (intervalSec * 1000) / SLEEP_CHUNK_MS;
        for (uint32 i = 0; i < totalChunks; i++) {
            if (m_stop.load(std::memory_order_relaxed)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_CHUNK_MS));
        }
    }
}

void WatchDogMgr::CheckAllHeartbeats()
{
    uint64 nowUs = GetSteadyClockUs();
    uint32 count = m_registeredCount.load(std::memory_order_acquire);

    for (uint32 i = 0; i < count; i++) {
        WatchDogHandle *hb = &m_heartbeats[i];

        if (!hb->registered.load(std::memory_order_relaxed)) {
            continue;
        }

        // 只检查 RUNNING 状态的线程
        ThreadRunState state = hb->runState.load(std::memory_order_relaxed);
        if (state != ThreadRunState::RUNNING) {
            continue;
        }

        uint64 lastHb = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
        uint64 elapsed = nowUs - lastHb;
        uint64 timeoutUs = GetTimeoutUsFromGuc(hb->threadType);

        if (elapsed > timeoutUs) {
            ErrLog(DSTORE_ERROR, MODULE_WATCHDOG,
                ErrMsg("WatchDog: thread '%s' (type=%d, index=%u) appears STUCK. "
                       "Last heartbeat was %lu us ago (timeout=%lu us).",
                       hb->threadName, (int)hb->threadType, hb->threadIndex,
                       elapsed, timeoutUs));

            hb->runState.store(ThreadRunState::STUCK, std::memory_order_relaxed);
        }
    }
}
```

### 3.7 Diagnose 接口

遵循现有 diagnose 模式（参考 `HeapDiagnose::GetPageFreespace`、`BufMgrDiagnose::GetBufDescPrintInfo`），新增 `WatchDogDiagnose` 类。所有方法为 **static**，server 层直接调用，无需通过 `StorageInstanceInterface`。

#### 3.7.1 结构化返回数据

```cpp
// interface/diagnose/dstore_watchdog_diagnose.h

#include "framework/dstore_watchdog.h"
#include "diagnose/dstore_diagnose.h"

namespace DSTORE {

// 单个线程的状态信息（结构化数据，供 server 层使用）
struct WatchDogThreadStatus : public DiagnoseItem {
    char threadName[64];                // 线程名称
    WatchDogThreadType threadType;      // 线程类型
    uint32_t threadIndex;               // 同类型线程索引
    ThreadRunState runState;            // 当前状态（RUNNING/SLEEPING/STUCK/...）
    bool isTimeout;                     // 是否超时
    uint64_t lastHeartbeatUs;           // 上次心跳时间戳（微秒，steady_clock）
    uint64_t elapsedUs;                 // 距上次心跳已过时间（微秒）
    uint64_t timeoutThresholdUs;        // 超时阈值（微秒，从 GUC 读取）
};

class WatchDogDiagnose {
public:
    // === 结构化数据接口（server 层用于展示各线程状态） ===

    // 获取所有被监控线程的结构化状态信息
    // outArray: 输出数组指针（内部分配，调用方通过 FreeThreadStatusArray 释放）
    // outCount: 输出数组长度
    // 返回 RetStatus 表示成功/失败
    static RetStatus GetAllThreadStatus(PdbId pdbId,
                                        WatchDogThreadStatus **outArray,
                                        uint32_t *outCount);

    // 获取指定类型线程的结构化状态信息
    static RetStatus GetThreadStatusByType(PdbId pdbId,
                                           WatchDogThreadType type,
                                           WatchDogThreadStatus **outArray,
                                           uint32_t *outCount);

    // 释放 GetAllThreadStatus / GetThreadStatusByType 返回的数组
    static void FreeThreadStatusArray(WatchDogThreadStatus *arr);

    // === 格式化字符串接口（用于日志/文本展示） ===

    // 获取所有线程状态的格式化摘要字符串
    // 返回 char*（调用方负责 pfree）
    static char *GetFormattedSummary(PdbId pdbId);

    // 获取指定类型线程的格式化字符串
    static char *GetFormattedStatusByType(PdbId pdbId, WatchDogThreadType type);
};

}  // namespace DSTORE
```

#### 3.7.2 结构化接口实现思路

```cpp
// src/framework/dstore_watchdog_diagnose.cpp

RetStatus WatchDogDiagnose::GetAllThreadStatus(PdbId pdbId,
                                               WatchDogThreadStatus **outArray,
                                               uint32_t *outCount)
{
    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
    if (pdb == nullptr || pdb->GetWatchDogMgr() == nullptr) {
        *outArray = nullptr;
        *outCount = 0;
        return DSTORE_FAIL;
    }

    WatchDogMgr *mgr = pdb->GetWatchDogMgr();

    // 获取心跳快照
    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = mgr->GetHeartbeatSnapshot(snapshot, WatchDogMgr::MAX_WATCHED_THREADS);

    if (count == 0) {
        *outArray = nullptr;
        *outCount = 0;
        return DSTORE_SUCC;
    }

    // 分配输出数组
    auto *result = (WatchDogThreadStatus *)DstorePalloc(sizeof(WatchDogThreadStatus) * count);
    uint64 nowUs = WatchDogMgr::GetSteadyClockUs();

    for (uint32 i = 0; i < count; i++) {
        const WatchDogHandle &hb = snapshot[i];
        WatchDogThreadStatus &status = result[i];

        // 填充结构化数据
        errno_t rc = strncpy_s(status.threadName, sizeof(status.threadName),
                               hb.threadName, sizeof(status.threadName) - 1);
        storage_securec_check(rc, "", "");
        status.threadType = hb.threadType;
        status.threadIndex = hb.threadIndex;
        status.runState = hb.runState.load(std::memory_order_relaxed);
        status.lastHeartbeatUs = hb.lastHeartbeatUs.load(std::memory_order_relaxed);
        status.elapsedUs = nowUs - status.lastHeartbeatUs;
        status.timeoutThresholdUs = mgr->GetTimeoutUsFromGuc(hb.threadType);
        status.isTimeout = (status.runState == ThreadRunState::RUNNING ||
                            status.runState == ThreadRunState::STUCK) &&
                           (status.elapsedUs > status.timeoutThresholdUs);
    }

    *outArray = result;
    *outCount = count;
    return DSTORE_SUCC;
}

void WatchDogDiagnose::FreeThreadStatusArray(WatchDogThreadStatus *arr)
{
    if (arr != nullptr) {
        DstorePfree(arr);
    }
}
```

#### 3.7.3 格式化输出示例

`GetFormattedSummary()` 返回的字符串：

```
================== WatchDog Thread Status ==================
PDB: 3

Thread Name          Type                 Index  State      Timeout?  Elapsed(s)  Threshold(s)
--------------------+--------------------+------+----------+---------+-----------+-------------
MasterWriter         BG_PAGE_MASTER       0      RUNNING    NO        0.5         30
SlaveWriter          BG_PAGE_SLAVE        0      SLEEPING   NO        2.1         60
SlaveWriter          BG_PAGE_SLAVE        1      RUNNING    NO        0.3         60
Checkpointer         CHECKPOINTER         0      RUNNING    NO        1.2         600
WalRecycler          WAL_FILE_RECYCLE     0      SLEEPING   NO        5.0         30
UndoRecycler         UNDO_RECYCLE         0      RUNNING    NO        3.4         120
BtreeRecycler        BTREE_RECYCLE        0      SLEEPING   NO        120.5       1800

Stuck Threads: 0
================================================================
```

告警时的 ERROR 日志：

```
[ERROR] WatchDog: thread 'MasterWriter' (type=BG_PAGE_MASTER_WRITER, index=0) appears STUCK.
        Last heartbeat was 45000000 us ago (timeout=30000000 us).
```

### 3.8 Diagnose 接口调用方式

server 层直接调用 `WatchDogDiagnose` 的 static 方法，无需修改 `StorageInstanceInterface`（参考 `HeapDiagnose::GetPageFreespace` 模式）：

```cpp
// server 层示例用法 —— 获取结构化数据
WatchDogThreadStatus *statusArr = nullptr;
uint32_t count = 0;
RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(pdbId, &statusArr, &count);
if (STORAGE_FUNC_SUCC(ret)) {
    for (uint32_t i = 0; i < count; i++) {
        // 访问 statusArr[i].threadName, isTimeout, elapsedUs 等
        // 可用于 server 层自定义展示格式（如系统视图、命令行输出等）
    }
    WatchDogDiagnose::FreeThreadStatusArray(statusArr);
}

// server 层示例用法 —— 获取格式化字符串（直接打印/返回）
char *summary = WatchDogDiagnose::GetFormattedSummary(pdbId);
if (summary != nullptr) {
    // 输出到客户端或日志
    DstorePfree(summary);
}
```

## 4. 文件组织

```
include/framework/
    dstore_watchdog.h                  # WatchDogMgr、WatchDogHandle、枚举等定义

src/framework/
    dstore_watchdog.cpp                # WatchDogMgr 实现（Init/Destroy/Register/Unregister/
                                       #   CheckAllHeartbeats/WatchDogThreadMain/GetHeartbeatSnapshot）
    dstore_watchdog_diagnose.cpp       # WatchDogDiagnose 实现（GetAllThreadStatus/
                                       #   GetFormattedSummary 等格式化逻辑）

interface/diagnose/
    dstore_watchdog_diagnose.h         # WatchDogDiagnose 对外 static 接口 + WatchDogThreadStatus 结构体

interface/framework/
    dstore_instance_interface.h        # StorageGUC 新增 watchdog* 字段（GUC 参数）

tests/unittest/src/ut_watchdog/
    ut_watchdog_mgr.cpp                # WatchDogMgr 核心逻辑测试
    ut_watchdog_diagnose.cpp           # WatchDogDiagnose 接口测试
    ut_watchdog_integration.cpp        # 后台线程生命周期集成测试
```

## 5. 集成点

### 5.1 WatchDogMgr 生命周期（挂在 StoragePdb 上）

```
StoragePdb::InitPdb() / OpenPdb()
    → InitWatchDogMgr()（在 InitWalMgr 之前调用，确保先于所有后台线程）
        → 创建 WatchDogMgr，调用 Init(pdbId) + StartThread()
        → StartThread() 内部阻塞等待 m_ready 标志，确保 WatchDog 线程已进入主循环
    → InitWalMgr()（WAL 回收线程在此启动，此时 WatchDog 已在运行）
    → ... 其他 Manager 初始化 ...
    → StartBgThread()（启动其他后台线程，WatchDog 已在监控中）

StoragePdb::StopBgThread()
    → m_stopBgThread = true
    → 停止各后台线程

StoragePdb::DestroyMgr()（在 StopBgThread 之后调用）
    → ... 销毁各 Manager（DestroyWalMgr 停止 WAL 回收线程等）...
    → DestroyWatchDogMgr()（作为 LAST action，确保所有后台线程已停止后再销毁 WatchDog）
```

**启动校验机制**：`WatchDogMgr::StartThread()` 创建线程后调用 `WaitReady()` 阻塞等待，
`WatchDogThreadMain()` 入口处设置 `m_ready = true`（memory_order_release），
`WaitReady()` 循环检查 `m_ready`（memory_order_acquire），确保 WatchDog 线程确实已启动后才返回。

### 5.2 StoragePdb 新增成员

```cpp
class StoragePdb {
    // 新增
    WatchDogMgr *m_watchdogMgr;
    WatchDogHandle *m_undoRecycleWdHandle = nullptr;

public:
    WatchDogMgr *GetWatchDogMgr() const { return m_watchdogMgr; }
};
```

### 5.3 被监控线程修改清单

| 文件 | 修改内容 |
|------|---------|
| `include/buffer/dstore_bg_disk_page_writer.h` | BgDiskPageMasterWriter / SlaveWriter 新增 `WatchDogHandle *m_watchdogHandle` 成员 |
| `src/buffer/dstore_bg_disk_page_writer.cpp` | Run() 中埋入 Register/TouchHeartbeat/SetRunState/Unregister |
| `include/buffer/dstore_checkpointer.h` | CheckpointMgr 新增 `WatchDogHandle *m_watchdogHandle` 成员 |
| `src/buffer/dstore_checkpointer.cpp` | CheckpointerMain() 中埋入心跳 |
| `include/wal/dstore_wal_file_manager.h` | WalFileManager 新增 `WatchDogHandle *m_watchdogHandle` 成员 |
| `src/wal/dstore_wal_file_manager.cpp` | RecycleWalFileWorkerMain() 中埋入心跳 |
| `include/framework/dstore_pdb.h` | StoragePdb 新增 `WatchDogHandle *m_undoRecycleWdHandle` 成员 |
| `src/framework/dstore_pdb.cpp` | RecycleUndoThreadMain() 中埋入心跳 |
| `include/index/dstore_btree_page_recycle.h` | BtreeRecycleWorker 新增 `WatchDogHandle *m_watchdogHandle` 成员 |
| `src/index/dstore_btree_page_recycle.cpp` | BtreeRecycleThreadMain() 中埋入心跳 |
| `include/framework/dstore_pdb.h` | StoragePdb 新增 m_watchdogMgr / m_undoRecycleWdHandle / GetWatchDogMgr() |
| `src/framework/dstore_pdb.cpp` | OpenPdb/StartBgThread/StopBgThread 中集成 WatchDogMgr |
| `interface/framework/dstore_instance_interface.h` | StorageGUC 新增 watchdog* GUC 参数字段 |

## 6. 单元测试设计

目标：**行覆盖率 >= 90%，分支覆盖率 >= 90%**

### 6.1 故障注入点定义

为覆盖 WatchDog 代码中的错误处理分支，需要定义故障注入点。参考现有模式（如 `DstoreTransactionFI`、`DstoreLockMgrFI`），新增 WatchDog 故障注入枚举：

```cpp
// include/common/fault_injection/dstore_watchdog_fault_injection.h

namespace DSTORE {

enum class DstoreWatchDogFI {
    REGISTER_FAIL,               // Register 时模拟分配失败
    GET_PDB_FAIL,                // GetPdb 返回 nullptr，覆盖 Diagnose 中 pdb == nullptr 分支
    GET_WATCHDOG_MGR_FAIL,       // GetWatchDogMgr 返回 nullptr
    PALLOC_FAIL,                 // palloc 分配失败，覆盖 Diagnose 内存分配异常
    GET_GUC_FAIL,                // GetGuc 返回异常值，覆盖 GetTimeoutUsFromGuc 的 default 分支
    CHECK_HEARTBEAT_SKIP,        // 跳过一次心跳检查，用于验证检查周期逻辑
};

}  // namespace DSTORE
```

**在生产代码中埋入故障注入点：**

> **重要规范**：所有 `FAULT_INJECTION_RETURN`、`FAULT_INJECTION_ACTION` 等故障注入宏在生产代码中**必须**用 `#ifdef UT ... #endif` 包裹。这是因为当编译时未开启 UT（即 `-DENABLE_UT=OFF`），故障注入相关的头文件和符号不会被引入，直接使用会导致编译失败。

```cpp
// src/framework/dstore_watchdog.cpp

WatchDogHandle *WatchDogMgr::Register(WatchDogThreadType type, uint32 index,
                                          const char *threadName)
{
#ifdef UT
    FAULT_INJECTION_RETURN(DstoreWatchDogFI::REGISTER_FAIL, nullptr);
#endif

    uint32 slot = m_registeredCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= MAX_WATCHED_THREADS) {
        m_registeredCount.fetch_sub(1, std::memory_order_relaxed);
        return nullptr;
    }
    // ... 初始化 heartbeat ...
}

// src/framework/dstore_watchdog_diagnose.cpp

RetStatus WatchDogDiagnose::GetAllThreadStatus(PdbId pdbId, ...)
{
    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
#ifdef UT
    FAULT_INJECTION_ACTION(DstoreWatchDogFI::GET_PDB_FAIL, pdb = nullptr);
#endif

    if (pdb == nullptr || pdb->GetWatchDogMgr() == nullptr) {
        *outArray = nullptr;
        *outCount = 0;
        return DSTORE_FAIL;
    }

    // ... 分配内存 ...
    auto *result = (WatchDogThreadStatus *)DstorePalloc(sizeof(WatchDogThreadStatus) * count);
#ifdef UT
    FAULT_INJECTION_ACTION(DstoreWatchDogFI::PALLOC_FAIL, { DstorePfree(result); result = nullptr; });
#endif
    if (result == nullptr) {
        *outArray = nullptr;
        *outCount = 0;
        return DSTORE_FAIL;
    }
    // ...
}
```

### 6.2 测试文件

```
tests/unittest/src/ut_watchdog/
    ut_watchdog_mgr.cpp                # WatchDogMgr 核心逻辑测试
    ut_watchdog_diagnose.cpp           # WatchDogDiagnose 接口测试
    ut_watchdog_integration.cpp        # 后台线程生命周期集成测试
```

### 6.3 测试 Fixture 与故障注入注册

```cpp
// ut_watchdog_mgr.cpp

#include "fault_injection/fault_injection.h"
#include "common/fault_injection/dstore_watchdog_fault_injection.h"

class WatchDogMgrTest : public DSTORETEST {
protected:
    void SetUp() override {
        DSTORETEST::SetUp();
        MockStorageInstance *inst = DstoreNew(m_ut_memory_context) MockStorageInstance();
        inst->Install(&DSTORETEST::m_guc, m_ut_memory_context);
        inst->Startup(&DSTORETEST::m_guc);
        DSTORE::StoragePdb *pdb = g_storageInstance->GetPdb(g_defaultPdbId);
        ASSERT_NE(nullptr, pdb);
        m_mgr = pdb->GetWatchDogMgr();
        ASSERT_NE(nullptr, m_mgr);
    }

    void TearDown() override {
        m_mgr = nullptr;
        MockStorageInstance *inst = (MockStorageInstance *)g_storageInstance;
        inst->Shutdown();
        delete inst;
        DSTORETEST::TearDown();
    }

    DSTORE::WatchDogMgr *m_mgr;
};
```

### 6.4 ut_watchdog.cpp 测试用例清单

#### 6.4.1 Init / Destroy 生命周期

| 用例 | 覆盖点 |
|------|--------|
| `InitAndDestroy_Normal` | Init 成功，Destroy 成功，m_registeredCount == 0 |
| `Init_MultipleTimes` | 重复 Init 的幂等性或错误处理 |
| `Destroy_WithoutInit` | 未 Init 时 Destroy 不崩溃 |
| `Destroy_WithRegisteredThreads` | 存在已注册线程时 Destroy，验证清理逻辑 |

#### 6.4.2 Register / Unregister

| 用例 | 覆盖点 | 故障注入 |
|------|--------|---------|
| `Register_SingleThread` | 注册一个线程，返回非空指针，字段正确 | - |
| `Register_MultipleThreads` | 注册多个不同类型线程，各自独立 | - |
| `Register_SameTypeDifferentIndex` | 注册多个同类型线程（如多个 SlaveWriter），index 区分 | - |
| `Register_MaxCapacity` | 注册达到 MAX_WATCHED_THREADS 上限 | - |
| `Register_ExceedCapacity` | 超出上限，返回 nullptr | - |
| `Register_FaultInjection` | 故障注入导致 Register 失败，返回 nullptr | `FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::REGISTER_FAIL, FI_GLOBAL)` |
| `Unregister_Normal` | 正常注销，registered 标志清除 | - |
| `Unregister_Nullptr` | 传 nullptr，不崩溃 | - |
| `Unregister_AlreadyUnregistered` | 重复注销，不崩溃 | - |

**故障注入用例示例：**

```cpp
TEST_F(WatchDogTestBase, Register_FaultInjection)
{
    // 激活故障注入，模拟 Register 内部失败
    FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::REGISTER_FAIL, FI_GLOBAL);

    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::CHECKPOINTER, 0, "Checkpointer");
    EXPECT_EQ(hb, nullptr);  // 故障注入导致失败

    FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::REGISTER_FAIL, FI_GLOBAL);

    // 关闭故障注入后恢复正常
    hb = m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "Checkpointer");
    EXPECT_NE(hb, nullptr);
    m_mgr->Unregister(hb);
}
```

#### 6.4.3 TouchHeartbeat / SetRunState

| 用例 | 覆盖点 |
|------|--------|
| `TouchHeartbeat_UpdatesTimestamp` | 调用后 lastHeartbeatUs 更新为当前时间附近 |
| `TouchHeartbeat_Nullptr` | 传 nullptr，不崩溃（分支覆盖） |
| `TouchHeartbeat_MultipleCalls` | 多次调用，时间戳递增 |
| `SetRunState_AllStates` | 遍历所有 ThreadRunState 枚举值，逐一设置并验证 |
| `SetRunState_Nullptr` | 传 nullptr，不崩溃（分支覆盖） |

#### 6.4.4 CheckAllHeartbeats（核心检测逻辑）

| 用例 | 覆盖点 |
|------|--------|
| `Check_NoRegisteredThreads` | 空列表，无告警 |
| `Check_AllThreadsHealthy` | 所有线程心跳正常，无 STUCK |
| `Check_RunningThreadTimeout` | RUNNING 线程超时 → 状态变为 STUCK + ERROR 日志 |
| `Check_SleepingThreadNotChecked` | SLEEPING 线程即使心跳过期也不告警（关键分支） |
| `Check_NotStartedThreadNotChecked` | NOT_STARTED 线程不检查 |
| `Check_StoppedThreadNotChecked` | STOPPED 线程不检查 |
| `Check_StuckThreadAlreadyStuck` | 已经 STUCK 的线程不重复告警（STUCK != RUNNING，跳过检查） |
| `Check_MixedStates` | 混合状态：部分 RUNNING 正常、部分超时、部分 SLEEPING |
| `Check_ThreadRecoverFromStuck` | 线程从 STUCK 恢复后（更新心跳+SetRunState RUNNING），不再告警 |
| `Check_UnregisteredSlotSkipped` | 注销后的槽位被跳过（registered == false 分支） |

#### 6.4.5 GetTimeoutUsFromGuc

| 用例 | 覆盖点 | 故障注入 |
|------|--------|---------|
| `GetTimeout_AllThreadTypes` | 遍历所有 WatchDogThreadType，验证返回对应 GUC 值 | - |
| `GetTimeout_DefaultBranch` | 传入无效类型（如 WATCHDOG_THREAD_TYPE_COUNT），走 default 分支 | - |
| `GetTimeout_GucValueChange` | 修改 GUC 值后超时阈值跟随变化 | - |
| `GetTimeout_GucFail` | 故障注入使 GUC 返回异常，验证降级到默认值 | `FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::GET_GUC_FAIL, FI_GLOBAL)` |

#### 6.4.6 WatchDogThreadMain（主循环）

| 用例 | 覆盖点 |
|------|--------|
| `ThreadMain_StartAndStop` | 启动后设置 m_stop=true，线程正常退出 |
| `ThreadMain_DetectsStuckThread` | 主循环运行中检测到超时线程 |
| `ThreadMain_CheckIntervalFromGuc` | 验证检查周期从 GUC 读取 |

#### 6.4.7 GetHeartbeatSnapshot

| 用例 | 覆盖点 |
|------|--------|
| `Snapshot_Empty` | 无注册线程，返回 0 |
| `Snapshot_Normal` | 有注册线程，正确拷贝 |
| `Snapshot_FilterUnregistered` | 注销后的槽位不在快照中 |
| `Snapshot_MaxCountLimit` | outArray 大小不足时截断 |

### 6.5 ut_watchdog_diagnose.cpp 测试用例清单

#### 6.5.1 GetAllThreadStatus（结构化接口）

| 用例 | 覆盖点 | 故障注入 |
|------|--------|---------|
| `GetAll_NoPdb` | 无效 pdbId，返回错误 | - |
| `GetAll_NoPdb_FaultInjection` | 故障注入 GetPdb 返回 nullptr | `FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::GET_PDB_FAIL, FI_GLOBAL)` |
| `GetAll_NoWatchDogMgr` | PDB 存在但 WatchDogMgr 为空 | `FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::GET_WATCHDOG_MGR_FAIL, FI_GLOBAL)` |
| `GetAll_Empty` | 无注册线程，outCount == 0 | - |
| `GetAll_Normal` | 有注册线程，验证每个 WatchDogThreadStatus 字段正确 | - |
| `GetAll_TimeoutDetection` | 含超时线程，isTimeout == true | - |
| `GetAll_SleepingNotTimeout` | SLEEPING 线程即使 elapsed > threshold，isTimeout == false | - |
| `GetAll_VerifyAllFields` | 逐字段验证 | - |
| `GetAll_PallocFail` | 内存分配失败，返回错误 | `FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::PALLOC_FAIL, FI_GLOBAL)` |

**故障注入用例示例：**

```cpp
TEST_F(WatchDogDiagnoseTest, GetAll_PallocFail)
{
    // 先注册一个线程，使 count > 0
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::CHECKPOINTER, 0, "Checkpointer");
    ASSERT_NE(hb, nullptr);

    // 激活故障注入，模拟 palloc 失败
    FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::PALLOC_FAIL, FI_GLOBAL);

    WatchDogThreadStatus *outArray = nullptr;
    uint32_t outCount = 0;
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(TEST_PDB_ID, &outArray, &outCount);
    EXPECT_EQ(ret, DSTORE_FAIL);
    EXPECT_EQ(outArray, nullptr);
    EXPECT_EQ(outCount, 0u);

    FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::PALLOC_FAIL, FI_GLOBAL);
    m_mgr->Unregister(hb);
}

TEST_F(WatchDogDiagnoseTest, GetAll_NoPdb_FaultInjection)
{
    // 激活故障注入，模拟 GetPdb 返回 nullptr
    FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::GET_PDB_FAIL, FI_GLOBAL);

    WatchDogThreadStatus *outArray = nullptr;
    uint32_t outCount = 0;
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(TEST_PDB_ID, &outArray, &outCount);
    EXPECT_EQ(ret, DSTORE_FAIL);

    FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::GET_PDB_FAIL, FI_GLOBAL);
}
```

#### 6.5.2 GetThreadStatusByType

| 用例 | 覆盖点 |
|------|--------|
| `ByType_MasterWriter` | 过滤只返回 BG_PAGE_MASTER_WRITER 类型 |
| `ByType_SlaveWriterMultiple` | 多个 SlaveWriter，全部返回 |
| `ByType_NoMatch` | 无匹配类型，outCount == 0 |

#### 6.5.3 FreeThreadStatusArray

| 用例 | 覆盖点 |
|------|--------|
| `Free_Normal` | 正常释放，不崩溃 |
| `Free_Nullptr` | 传 nullptr，不崩溃 |

#### 6.5.4 GetFormattedSummary

| 用例 | 覆盖点 | 故障注入 |
|------|--------|---------|
| `Summary_Empty` | 无线程，返回空表头 | - |
| `Summary_Normal` | 有线程，包含正确表头和数据行 | - |
| `Summary_ContainsStuckCount` | 含 STUCK 线程，"Stuck Threads: N" 正确 | - |
| `Summary_NoPdb` | 无效 pdbId，返回 nullptr | - |
| `Summary_NoPdb_FaultInjection` | 故障注入 GetPdb 返回 nullptr | `FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::GET_PDB_FAIL, FI_GLOBAL)` |

#### 6.5.5 GetFormattedStatusByType

| 用例 | 覆盖点 |
|------|--------|
| `ByType_Formatted` | 过滤输出指定类型 |
| `ByType_NoMatch` | 无匹配，返回空表 |

### 6.6 故障注入点与覆盖分支的对应关系

| 故障注入点 | 生产代码位置 | 覆盖的分支 | UT 用例 |
|-----------|-------------|-----------|--------|
| `REGISTER_FAIL` | `WatchDogMgr::Register()` 入口 | 函数提前返回 nullptr | `Register_FaultInjection` |
| `GET_PDB_FAIL` | `WatchDogDiagnose::GetAllThreadStatus()` / `GetFormattedSummary()` | `pdb == nullptr` → return DSTORE_FAIL | `GetAll_NoPdb_FaultInjection`, `Summary_NoPdb_FaultInjection` |
| `GET_WATCHDOG_MGR_FAIL` | `WatchDogDiagnose::GetAllThreadStatus()` | `pdb->GetWatchDogMgr() == nullptr` → return DSTORE_FAIL | `GetAll_NoWatchDogMgr` |
| `PALLOC_FAIL` | `WatchDogDiagnose::GetAllThreadStatus()` 内存分配 | `result == nullptr` → return DSTORE_FAIL | `GetAll_PallocFail` |
| `GET_GUC_FAIL` | `WatchDogMgr::GetTimeoutUsFromGuc()` | GUC 异常时走 default 降级路径 | `GetTimeout_GucFail` |
| `CHECK_HEARTBEAT_SKIP` | `WatchDogMgr::CheckAllHeartbeats()` | 用于验证检查被跳过后不产生告警 | `ThreadMain_CheckIntervalFromGuc` |

### 6.7 故障注入高级用法

利用 `FAULT_INJECTION_ACTIVE_MODE_LEVEL` 的 skip/expect 参数实现更精细的控制：

```cpp
// 跳过前 2 次调用，第 3 次触发故障，只触发 1 次
FAULT_INJECTION_ACTIVE_MODE_LEVEL(DstoreWatchDogFI::REGISTER_FAIL, 0, FI_GLOBAL, 2, 1);

// 注册 3 个线程：前 2 个成功，第 3 个因故障注入失败
WatchDogHandle *hb1 = m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "Ckpt");
EXPECT_NE(hb1, nullptr);  // 第 1 次：跳过

WatchDogHandle *hb2 = m_mgr->Register(WatchDogThreadType::UNDO_RECYCLE, 0, "Undo");
EXPECT_NE(hb2, nullptr);  // 第 2 次：跳过

WatchDogHandle *hb3 = m_mgr->Register(WatchDogThreadType::BTREE_RECYCLE, 0, "Btr");
EXPECT_EQ(hb3, nullptr);  // 第 3 次：触发故障

WatchDogHandle *hb4 = m_mgr->Register(WatchDogThreadType::WAL_FILE_RECYCLE, 0, "Wal");
EXPECT_NE(hb4, nullptr);  // 第 4 次：expect 耗尽，恢复正常

FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::REGISTER_FAIL, FI_GLOBAL);
```

### 6.8 覆盖率保证策略

确保行覆盖率和分支覆盖率 >= 90% 的关键措施：

1. **所有 if 分支必须有正反两面用例**：
   - `hb == nullptr` 的 true/false 分支
   - `state != ThreadRunState::RUNNING` 的各种状态分支（NOT_STARTED/RUNNING/SLEEPING/STUCK/STOPPED）
   - `elapsed > timeoutUs` 的超时/未超时分支
   - `registered == false` 的跳过/不跳过分支
   - `GetTimeoutUsFromGuc` 的所有 switch-case + default

2. **通过故障注入覆盖错误路径**：
   - `Register` 内部失败 → `REGISTER_FAIL`
   - `GetPdb` 返回空 → `GET_PDB_FAIL`
   - `GetWatchDogMgr` 返回空 → `GET_WATCHDOG_MGR_FAIL`
   - 内存分配失败 → `PALLOC_FAIL`
   - GUC 参数异常 → `GET_GUC_FAIL`

3. **边界值测试**：
   - `elapsed == timeoutUs`（刚好等于，不超时）
   - `elapsed == timeoutUs + 1`（刚好超时）
   - `m_registeredCount == 0` / `== MAX_WATCHED_THREADS`

4. **故障注入的 skip/expect 控制**：
   - 使用 `FAULT_INJECTION_ACTIVE_MODE_LEVEL` 精确控制第 N 次调用才触发故障
   - 覆盖"部分成功部分失败"的混合场景

5. **故障注入宏必须用 `#ifdef UT` 保护**：
   - 生产代码中所有 `FAULT_INJECTION_RETURN`、`FAULT_INJECTION_ACTION` 等宏调用必须包裹在 `#ifdef UT ... #endif` 中
   - 原因：编译时若未开启 UT（`-DENABLE_UT=OFF`），故障注入相关头文件和符号不会被引入，裸写会导致编译失败
   - UT 测试代码（`tests/unittest/` 下）天然在 UT 宏定义下编译，无需额外包裹

6. **并发场景**（可选，根据测试框架能力）：
   - 多线程同时 Register
   - Register 和 CheckAllHeartbeats 并发
   - TouchHeartbeat 和 CheckAllHeartbeats 并发

## 7. 可扩展性

- **新增监控线程**：只需在新线程中调用 `Register()` / `TouchHeartbeat()` / `Unregister()`，在 `WatchDogThreadType` 枚举和 GUC 中新增对应项
- **超时阈值动态调整**：通过 GUC 参数实时生效，CheckAllHeartbeats 每次检查时从 GUC 重新读取
- **WatchDog 检查周期动态调整**：`watchdogCheckIntervalSec` 每次循环重新读取
- **告警方式可扩展**：当前支持 ErrLog + Diagnose，后续可增加回调接口
