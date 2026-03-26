# WatchDog 后台线程卡死监控 — 设计文档

## 1. 背景与目标

dstore 存储引擎包含多个关键后台线程，这些线程若发生卡死（死锁、死循环、IO 长时间阻塞等），会导致整个存储引擎停止推进，最终引起业务超时或数据库不可用。

**目标：**
1. 监控关键后台线程是否处于活跃状态
2. 若检测到线程疑似卡死（超过配置的超时阈值未上报心跳），自动打印告警日志
3. 提供诊断接口，上层 Server 可主动查询各后台线程的健康状态和最近心跳时间

**监控对象（5 类后台线程）：**

| 线程名 | 类 | 主函数 | 正常行为 |
|--------|----|--------|----------|
| WAL 刷盘线程 | `BgWalWriter` | `BgFlushMain()` | 持续循环调用 `m_stream->Flush()` |
| Checkpoint 推进线程 | `CheckpointMgr` | `CheckpointerMain()` | 定期做 checkpoint，间隔 checkpointTimeout |
| Buffer 刷脏线程 | `BgDiskPageMasterWriter` | `Run()` | 持续扫描脏页队列并触发刷盘 |
| WAL 文件回收线程 | `WalFileManager` | `RecycleWalFileWorkerMain()` | 定期检查 WAL 文件是否可回收 |
| Undo 回收调度线程 | `RollbackTrxTaskMgr` | `DispatchMain()`→`DoDispatch()` | 定期扫描并调度事务回滚任务 |

---

## 2. 设计方案

### 2.1 总体架构

```
┌─────────────────────────────────────────────────────┐
│                   StoragePdb                         │
│                                                      │
│  ┌────────────┐  ┌──────────────┐  ┌─────────────┐  │
│  │ BgWalWriter│  │CheckpointMgr │  │BgDiskPage   │  │
│  │            │  │              │  │MasterWriter │  │
│  │ heartbeat  │  │  heartbeat   │  │  heartbeat  │  │
│  └─────┬──────┘  └──────┬───────┘  └──────┬──────┘  │
│        │                │                 │          │
│  ┌─────┴────────────────┴─────────────────┴──────┐   │
│  │              WatchDogMgr                       │   │
│  │  - 注册所有监控项 (WatchDogEntry)                │   │
│  │  - 后台检测线程（每 1s 醒来检查一次）             │   │
│  │  - 超时 → ErrLog(DSTORE_ERROR, ...) 告警        │   │
│  └────────────────────┬───────────────────────────┘   │
│                       │                               │
└───────────────────────┼───────────────────────────────┘
                        │ 上层调用
               ┌────────▼────────┐
               │WatchDogDiagnose │ (interface/diagnose/)
               │ GetHealthStatus()│
               └─────────────────┘
```

### 2.2 心跳机制

每个被监控线程在其主循环中，每次迭代都更新一个原子时间戳（秒级精度）：

```cpp
// 在主循环每次迭代中调用：
m_lastHeartbeatTime.store(time(nullptr), std::memory_order_relaxed);
```

WatchDog 线程定期读取该时间戳，与当前时间对比。若差值超过 `timeoutSeconds`，则判定线程疑似卡死并发出告警。

**心跳语义**：
- `m_lastHeartbeatTime = 0`：线程尚未启动或已停止，不检查
- `m_lastHeartbeatTime > 0`：线程运行中，检查心跳是否新鲜

### 2.3 超时阈值设计

不同线程有不同的正常工作节奏，因此采用不同的默认超时：

| 线程 | 默认超时 | 理由 |
|------|----------|------|
| WAL 刷盘 | 30s | 循环很紧密，正常情况每次 sleep ≤ 10ms |
| Checkpoint | `checkpointTimeout * 3`（最小 180s） | checkpoint 本身耗时较长 |
| Buffer 刷脏 | 60s | 可能有 IO 等待，但循环频率较高 |
| WAL 文件回收 | 60s | 每 10ms 一次循环，但 IO 可能慢 |
| Undo 回收 | 120s | 事务回滚可能耗时较长 |

超时阈值未来可通过 GUC 参数配置（当前版本使用编译期常量）。

### 2.4 WatchDog 线程

WatchDog 监控线程：
- 每 **1 秒** 唤醒一次
- 遍历所有已注册的 `WatchDogEntry`
- 对每个 entry：若 `(now - lastHeartbeat) > timeout` 且 `lastHeartbeat != 0`，则告警
- 连续告警时，每 `reportIntervalSeconds`（默认 10s）打印一次，避免日志爆炸

### 2.5 诊断接口

```cpp
// interface/diagnose/dstore_watchdog_diagnose.h
struct WatchDogThreadStatus {
    char name[64];            // 线程名称
    PdbId pdbId;              // 所属 PDB
    uint64_t lastHeartbeat;   // 最近心跳时间（Unix 时间戳，秒）
    uint64_t timeoutSeconds;  // 超时阈值
    bool isStuck;             // 是否判定为卡死
    int64_t stuckDurationSeconds; // 卡死持续时间（若 isStuck=true）
};

class WatchDogDiagnose {
public:
    explicit WatchDogDiagnose(PdbId pdbId);
    // 返回所有监控线程的健康状态字符串（供日志输出）
    char *GetHealthStatus();
    // 获取结构化状态数组（供程序化处理）
    uint32_t GetThreadStatusArray(WatchDogThreadStatus **statusArr);
    void FreeThreadStatusArray(WatchDogThreadStatus *statusArr);
};
```

---

## 3. 实现方案

### 3.1 新增文件

```
include/framework/dstore_watchdog.h          # WatchDogMgr 类定义
src/framework/dstore_watchdog.cpp            # WatchDogMgr 实现
interface/diagnose/dstore_watchdog_diagnose.h # 诊断公开接口
src/framework/dstore_watchdog_diagnose.cpp   # 诊断实现
```

### 3.2 修改文件（添加心跳）

| 文件 | 改动 |
|------|------|
| `include/wal/dstore_wal_bgwriter.h` | 新增 `std::atomic<uint64> m_lastHeartbeatTime` |
| `src/wal/dstore_wal_bgwriter.cpp` | `BgFlushMain()` 循环中更新心跳 |
| `include/buffer/dstore_checkpointer.h` | 新增 `std::atomic<uint64> m_lastHeartbeatTime` |
| `src/buffer/dstore_checkpointer.cpp` | `CheckpointerMain()` 循环中更新心跳 |
| `include/buffer/dstore_bg_page_writer_base.h` | 新增 `std::atomic<uint64> m_lastHeartbeatTime` |
| `src/buffer/dstore_bg_disk_page_writer.cpp` | `Run()` 循环中更新心跳 |
| `include/wal/dstore_wal_file_manager.h` | 新增 `std::atomic<uint64> m_lastHeartbeatTime` |
| `src/wal/dstore_wal_file_manager.cpp` | `RecycleWalFileWorkerMain()` 循环中更新心跳 |
| `include/undo/dstore_rollback_trx_task_mgr.h` | 新增 `std::atomic<uint64> m_lastHeartbeatTime` |
| `src/undo/dstore_rollback_trx_task_mgr.cpp` | `DoDispatch()` 循环中更新心跳 |
| `include/framework/dstore_pdb.h` | 新增 `WatchDogMgr *m_watchdogMgr` |
| `src/framework/dstore_pdb.cpp` | 初始化/启动/停止 WatchDog |

---

## 4. 关键接口说明

### WatchDogMgr

```cpp
class WatchDogMgr {
public:
    // 注册一个被监控线程，返回 entry 指针（生命周期由被监控对象持有）
    void Register(WatchDogEntry *entry);
    // 反注册
    void Unregister(WatchDogEntry *entry);
    // 启动监控线程
    void Start(PdbId pdbId);
    // 停止监控线程
    void Stop();
};

struct WatchDogEntry {
    const char *threadName;                   // 线程名称（字符串常量）
    std::atomic<uint64> *heartbeatTimePtr;   // 指向被监控线程的心跳时间戳
    uint64 timeoutSeconds;                    // 超时阈值
    PdbId pdbId;                              // 所属 PDB
    uint64 lastAlarmTime;                     // 上次告警时间（防止日志洪泛）
};
```

---

## 5. 告警示例

```
[ERROR] [MODULE_WATCHDOG] WatchDog detected thread possibly stuck:
  name=BgWalWriter, pdbId=3, walId=1,
  lastHeartbeat=1711000030 (30 seconds ago), timeout=30s
```

---

## 6. 未来扩展

1. 支持通过 GUC 参数动态调整超时阈值
2. 支持注册回调函数（线程卡死时自动触发 core dump 或重启）
3. 扩展监控到更多线程（Undo Worker、Redo Worker 等）
4. 提供全局 HTTP/RPC 健康检查接口
