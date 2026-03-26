# dstore 项目指南

## 项目概述

dstore 是一个**可独立编译、独立测试的数据库存储引擎组件**，定位类似 PostgreSQL 的存储引擎，但可以独立于上层 Server 层运行。上层可对接 MySQL、PostgreSQL 的 Server 层和 Handler 层代码。

- 支持多写节点（分布式计算节点），但当前主要关注**单节点**模式
- 使用 C++11/14 编写，依赖 gcc7.3 工具链
- 强制使用旧 C++ ABI：`-D_GLIBCXX_USE_CXX11_ABI=0`

---

## 目录结构

```
dstore/
├── buildenv               # 环境变量配置（BUILD_ROOT 需改为当前路径）
├── build_script/          # 构建脚本和配置（guc.json, dstore_conf.json 等）
├── cmake/                 # CMake 辅助脚本（set_lib_path.cmake 等）
├── include/               # 内部头文件（仅 src/ 使用）
├── interface/             # 对外暴露的公开 API 头文件（上层 Server 调用）
├── src/                   # 所有源代码
├── utils/                 # 独立子项目（gsutils 库），需先单独编译
├── tools/                 # 运维诊断工具（pagedump, waldump, buflookup 等）
├── tests/                 # 测试代码
│   ├── unittest/          # 单元测试
│   └── tpcctest/          # TPC-C 基准测试
└── output/                # 编译输出（libdstore.so, libdstore.a）
```

---

## 核心模块说明

### `src/framework/` — 框架层（核心入口）

| 文件 | 职责 |
|------|------|
| `dstore_instance.cpp` | `StorageInstance` 单例，全局存储引擎实例，管理所有 PDB |
| `dstore_pdb.cpp` | `StoragePdb`，每个数据库（PDB）的生命周期管理，包含所有子模块 |
| `dstore_thread.cpp` | 线程上下文（`ThreadContext`）管理，每线程持有事务、内存、锁上下文 |
| `dstore_session_interface.cpp` | Session 生命周期接口 |
| `dstore_wal.cpp` | WAL 管理器初始化 |
| `dstore_wal_bgwriter.cpp`（在 wal/） | WAL 刷盘后台线程 |

全局变量：
- `g_storageInstance` — 全局存储实例指针（`StorageInstance*`）
- `g_defaultPdbId` — 默认 PDB ID
- `thrd` — 线程局部变量，当前线程的 `ThreadContext*`

### `src/buffer/` — Buffer Pool

| 类 | 职责 |
|----|------|
| `BufMgr` | Buffer Pool 主管理器，实现 `Read/Write/MarkDirty/Release` 等 |
| `BufferDesc` | Buffer 描述符，含 page 指针、引用计数、状态标志、LRU 节点 |
| `BgDiskPageMasterWriter` | Buffer 刷脏调度线程（Master），每个 WAL stream 一个；负责扫描脏页队列、决定刷哪些页、唤醒 Slave、等待 Slave 完成、推进 recoveryPlsn，**本身不写磁盘** |
| `BgDiskPageSlaveWriter` | Buffer 刷脏执行线程（Slave），由 Master 管理；从共享的 `CandidateFlushCxt` 抢占一批脏页后调用 `FlushCandidateDirtyPage()` 实际写磁盘（支持 AIO）；Slave 数量由 GUC `bgDiskWriterSlaveNum` 控制，每个 Master 可有多个 Slave |
| `BgPageWriterMgr` | 管理所有 BgPageWriter 实例 |
| `CheckpointMgr` | Checkpoint 后台线程，定期推进 checkpoint |
| `BufferRing` | 批量读/写时使用的环形 Buffer 复用策略 |

Buffer 状态标志（`Buffer::BUF_*`）：32位引用计数 + 高位 flags，原子 CAS 操作。

**BgDiskPageMasterWriter / BgDiskPageSlaveWriter 协作流程**：
1. Master `Run()` 主循环：调用 `ScanDirtyListForFlush()` 从脏页队列扫出候选页，填入共享的 `CandidateFlushCxt`
2. Master 调用 `WakeUpSlaveWriter()` 唤醒所有 Slave
3. 每个 Slave 调用 `SeizeDirtyPageListForFlush()` 用原子操作从 `CandidateFlushCxt` 抢占一批（最多 1000 页），再调用 `FlushCandidateDirtyPage()` 实际写磁盘（同步或 AIO）
4. Master 调用 `WaitSlaveWriterFlushFinish()` 等所有 Slave 完成
5. Master 调用 `AdvanceHeadAfterFlush()` 推进脏页队列头指针和 `recoveryPlsn`
6. Checkpoint 时，外部调用 `FlushAllDirtyPages()`：设置 `m_flushAll=true`，等待 `recoveryPlsn >= maxAppendedPlsn`

### `src/wal/` — Write-Ahead Log

| 类 | 职责 |
|----|------|
| `WalManager` | WAL 总管理器，管理多个 `WalStream` |
| `WalStream` | 一个 WAL 流，含 buffer、file manager、bgwriter |
| `WalStreamManager` | 管理多个 `WalStream` 的集合 |
| `BgWalWriter` | WAL 刷盘后台线程，绑定到 `WalStream` |
| `WalFileManager` | WAL 文件管理（创建/回收/归档），含回收后台线程 |
| `CheckpointMgr` | Checkpoint 推进线程（在 buffer/） |
| `WalRecovery` | WAL 重放/恢复 |

### `src/transaction/` — 事务管理

| 类 | 职责 |
|----|------|
| `TransactionMgr` | 事务管理器，管理 XID、CSN 分配 |
| `CsnMgr` | Commit Sequence Number 管理（MVCC 可见性） |
| `TransactionContext` | 线程局部事务状态 |
| `Savepoint` | 保存点管理 |

事务隔离级别：Read Committed / Repeatable Read / Serializable

### `src/index/` — BTree 索引

| 文件 | 职责 |
|------|------|
| `dstore_btree.cpp` | BTree 主逻辑（查找、分裂等） |
| `dstore_btree_insert.cpp` | 插入 |
| `dstore_btree_scan.cpp` | 扫描 |
| `dstore_btree_delete.cpp` | 删除 |
| `dstore_btree_build.cpp` | 索引构建（包括并行构建） |
| `dstore_btree_split.cpp` | 页面分裂 |
| `dstore_btree_undo.cpp` | 索引 Undo |
| `dstore_btree_vacuum.cpp` | Vacuum 清理 |
| `dstore_btree_page_recycle.cpp` | 空页回收 |

### `src/heap/` — Heap 表存储

| 文件 | 职责 |
|------|------|
| `dstore_heap_insert.cpp` | Tuple 插入 |
| `dstore_heap_scan.cpp` | 顺序扫描 |
| `dstore_heap_update.cpp` | Tuple 更新 |
| `dstore_heap_delete.cpp` | Tuple 删除 |
| `dstore_heap_prune.cpp` | 死版本清理 |
| `dstore_heap_vacuum.cpp` | Vacuum |
| `dstore_heap_lob.cpp` | LOB 大对象 |

### `src/undo/` — Undo 日志

| 类 | 职责 |
|----|------|
| `UndoMgr` | Undo 总管理器，管理 UndoZone |
| `UndoZone` | Undo zone（每线程一个 zone），存放 undo 记录 |
| `RollbackTrxTaskMgr` | Undo 回收调度线程（DispatchMain），负责异步事务回滚 |
| `RollbackTrxWorker` | Undo 回收工作线程 |

### `src/lock/` — 锁管理

- `LockMgr`：表级锁、行级锁管理
- `LWLock`：轻量级锁（读写锁）
- `DeadlockDetector`：死锁检测

### `src/tablespace/` — 表空间 & 段管理

管理数据文件的物理组织，包括数据段、索引段、Heap 段、FSM（空闲空间映射）。

### `src/page/` — Page 结构

- `Page`：基础 page 结构，包含 header（checksum、LSN、page type、page id 等）
- `HeapPage`：Heap 页面，含 ItemId 数组和 Tuple 数据
- `IndexPage`：BTree 索引页面

### `src/common/` — 公共基础设施

- `memory/`：内存上下文（`DstoreMemoryContext`），类似 PG 的内存池
- `error/`：错误码和错误信息管理
- `log/`：日志模块（`ErrLog` 宏）
- `instrument/`：wait_event、trace、perf 统计

---

## 关键数据结构

### `StorageGUC` — 配置参数
`interface/framework/dstore_instance_interface.h`
所有可配置参数（buffer size、WAL 参数、checkpoint timeout 等）

### `Page` — 页面 header
`include/page/dstore_page.h`
```cpp
struct Page {
    uint16 m_checksum;
    uint64 m_glsn, m_plsn;    // Global/Physical LSN
    uint16 m_lower, m_upper;  // 空闲空间边界
    uint16 m_type;            // 页面类型
    PageId m_myself;          // 自身 (fileId, blockId)
};
```

### `BufferDesc` — Buffer 描述符
`include/buffer/dstore_buf.h`
```cpp
struct BufferDesc {
    std::atomic<uint64> state;  // refcount(低32) + flags(高32)
    BufferTag bufTag;           // (pdbId, fileId, blockId)
    LWLock contentLwLock;       // 内容读写锁
    BufferDescController *controller;  // IO锁、CR锁等
    LruNode lruNode;            // LRU 链表节点
};
```

### `ThreadContext` — 线程上下文
`include/framework/dstore_thread.h`
每线程局部变量 `thrd`，包含事务状态、内存上下文、锁上下文等。

---

## 对外接口（interface/ 目录）

上层 Server/Handler 层通过 `interface/` 目录下的头文件调用：

| 接口文件 | 说明 |
|---------|------|
| `framework/dstore_instance_interface.h` | `StorageInstanceInterface`，引擎生命周期（初始化/销毁/PDB管理） |
| `transaction/dstore_transaction_interface.h` | 事务开始/提交/回滚/Savepoint |
| `heap/dstore_heap_interface.h` | HeapInterface：Insert/Update/Delete/Scan |
| `index/dstore_index_interface.h` | IndexInterface：Build/Insert/Delete/ScanBegin/ScanNext |
| `pdb/dstore_pdb_interface.h` | PDB 上下文切换、Checkpoint |
| `diagnose/dstore_diagnose.h` | 诊断迭代器基类 |
| `diagnose/dstore_wal_diagnose.h` | WAL 状态诊断 |
| `diagnose/dstore_checkpointer_diagnose.h` | Checkpoint 诊断 |
| `diagnose/dstore_bg_page_writer_diagnose.h` | Buffer 刷脏诊断 |

---

## 后台线程一览

| 线程 | 类 | 主函数 | 归属 |
|------|----|--------|------|
| WAL 刷盘 | `BgWalWriter` | `BgFlushMain()` | 每个 `WalStream` 一个 |
| Checkpoint | `CheckpointMgr` | `CheckpointerMain()` | 每个 PDB 一个 |
| Buffer 刷脏（调度） | `BgDiskPageMasterWriter` | `Run()` | 每个 WAL stream 一个；扫脏页、协调 Slave、推进 recoveryPlsn |
| Buffer 刷脏（执行） | `BgDiskPageSlaveWriter` | `Run()` | 每个 Master 可多个（`bgDiskWriterSlaveNum`）；实际调用 Flush/AIO 写磁盘 |
| WAL 文件回收 | `WalFileManager` | `RecycleWalFileWorkerMain()` | 每个 `WalFileManager` 一个 |
| Undo 回收调度 | `RollbackTrxTaskMgr` | `DispatchMain()` | 每个 PDB 一个 |
| Undo 回收工作 | `RollbackTrxWorker` | 工作线程 | 多个（最多10个） |

---

## PDB（Pluggable Database）概念

- `PdbId`：数据库实例 ID（类似 PostgreSQL 的 database oid）
- `StoragePdb`：每个数据库的完整上下文（WAL、Buffer、Undo、Checkpoint 等）
- 单节点模式只有一个 PDB（`g_defaultPdbId`）
- 每个 PDB 有独立的 WAL stream、Buffer、Undo zone

---

## 编译说明

### 前置依赖（local_libs/）
- gcc7.3：编译工具链
- secure（Huawei SecureC）：安全字符串函数
- lz4：压缩
- cjson：JSON 解析
- gtest/mockcpp：单元测试（需用 `-D_GLIBCXX_USE_CXX11_ABI=0` 编译）

### 编译步骤
```bash
source dstore/buildenv          # 加载环境变量

# 1. 编译 utils
cd utils && bash build.sh -m release

# 2. 编译 dstore（不带UT）
cd .. && bash build.sh -m release

# 3. 编译 dstore（带UT）
bash build.sh -m debug -ut

# 4. TPC-C 测试
rm -rf tmp_build && mkdir tmp_build && cd tmp_build
cmake .. -DUTILS_PATH=dstore/utils/output
make run_dstore_tpcctest
```

### 已知构建问题及解决方案
1. **gcc7.3 的 libstdc++.so.6 版本过旧**：`cp /usr/lib/x86_64-linux-gnu/libstdc++.so.6 local_libs/buildtools/gcc7.3/gcc/lib64/`
2. **缺少 libaio.h**：`sudo apt-get install -y libaio-dev`
3. **mockcpp 未包含**：需从 GitHub 克隆 sinojelly/mockcpp 并用旧 ABI 编译
4. **gtest 只有 debug 版本**：需用 `-DCMAKE_CXX_FLAGS="-D_GLIBCXX_USE_CXX11_ABI=0"` 重新编译 Release 版本

---

## 诊断接口（Diagnose）模式

项目提供诊断接口供 Server 层调用，模式如下：
1. 诊断类（如 `CheckpointerDiagnose`）在 `interface/diagnose/` 中声明
2. 实现在 `src/buffer/dstore_checkpointer_diagnose.cpp` 等
3. 通过 `g_storageInstance->GetPdb(pdbId)->GetXxxMgr()` 获取内部对象
4. 返回 `char*`（通过 `StringInfoData` 构建）或结构体数组

---

## 代码规范

- 所有代码在 `namespace DSTORE` 内
- 公开 API 使用 `#pragma GCC visibility push(default)` / `pop`
- 内存分配：使用 `DstoreNew(ctx) T(...)` 和 `DstorePfree(ptr)`
- 断言：`StorageAssert(cond)`，panic：`ErrLog(DSTORE_PANIC, ...)`
- 错误日志：`ErrLog(DSTORE_ERROR/WARNING/LOG/DEBUG1, MODULE_XXX, ErrMsg("..."))`
- 错误返回值：`DSTORE_SUCC` / `DSTORE_FAIL`，宏 `STORAGE_FUNC_FAIL(expr)` 检测失败
- 禁止拷贝：`DISALLOW_COPY_AND_MOVE(ClassName)`
- 时间戳：`GetCurrentTimestamp()`（微秒），`time(nullptr)`（秒）
- `ThreadId` = `pthread_t`，`INVALID_THREAD_ID = (pthread_t)(-1)`

---

## 用户偏好

- 关注**单节点**模式，不需要关心多写节点功能
- 后续需要开发新特性，需要理解现有代码风格
