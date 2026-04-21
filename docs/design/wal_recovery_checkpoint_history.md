# Checkpoint 历史位置链 —— recovery 起点容错设计

## 1. 背景

当前 dstore 拉起时的 recovery 流程（仅描述与本设计相关的部分）：

1. **读取起点**
   `WalRecovery::Init` (`src/wal/dstore_wal_recovery.cpp:211`)
   → `InitRecoveryPlsn` (`src/wal/dstore_wal_recovery.cpp:1969`)
   从 `ControlWalStreamPageItemData::lastWalCheckpoint`（`include/control/dstore_control_walinfo.h:58`）读出 `diskRecoveryPlsn`，写入 `m_recoveryStartPlsn`。

2. **执行恢复**
   `WalStream::Recovery` (`src/wal/dstore_wal_logstream.cpp:1052`)
   → `WalRecovery::Recovery` → `Redo` → `RedoLoopReadAndRedoWalRecord` (`src/wal/dstore_wal_recovery.cpp:327`)
   循环 `walRecordReader->ReadNext` 解析一个个 atomic group，最后通过出参 `lastGroupEndPlsn` 回传"已成功 redo 的最后一组的结束位置"。

3. **截断尾部 WAL**
   `WalStream::Recovery` (`src/wal/dstore_wal_logstream.cpp:1096`)
   → `Truncate(lastGroupEndPlsn)` 把 `lastGroupEndPlsn` 之后的 WAL 文件物理截断。

### 1.1 问题

整条链路对 control file 中那一份 `lastWalCheckpoint.diskRecoveryPlsn` **完全信任**。一旦该字段因任何原因（部分写、磁盘静默错误、bug 写入了错误值等）落到了错误位置，会发生：

- `m_recoveryStartPlsn` 起点错误。
- `WalRecordReader::ReadNext` 在该错误位置上无法解析出合法的 atomic group（`ValidWalGroupLen` / `ValidWalGroup` CRC 校验失败），返回空 group，`RedoLoopReadAndRedoWalRecord` 直接 `break`。
- `lastGroupEndPlsn` 仍等于错误的 `redoStartPlsn`。
- `Truncate(lastGroupEndPlsn)` 把后面**本应回放的合法 WAL**当成"多余尾部"物理截断 —— **数据丢失，且不可恢复**。

### 1.2 目标

在不改变正常路径性能的前提下：让 recovery 启动阶段能识别出"checkpoint 记录的位置不是一个合法的 WAL group 起点"，并自动回退到一个更老的、合法的 checkpoint 位置，从而避免错误地 truncate 合法 WAL。

### 1.3 非目标

- 不解决"checkpoint 位置看似合法但语义错误"（CRC 撞上 + groupLen 也合法）的极端场景 —— 该场景需要业务层的 LSN 单调性校验或冗余备份，不在本设计范围内。
- 不改变正常 redo / dispatch / 并行回放流程。
- 不改变 control file 的 page 布局（仅扩展单条 `ControlWalStreamPageItemData` 内的字段，向后兼容）。

## 2. 设计概览

把 control file 中"单槽 checkpoint"扩展为**长度为 N 的环形历史链**（**N = 3，编译期常量，不做 GUC**）。

- **写**：每次 checkpoint 完成后，**只覆盖一个槽位**（环形游标 +1 mod N）。最旧的那条历史被覆盖，其余 N-1 条保留。
- **读**：recovery 启动时，按"从新到旧"遍历这 N 条 checkpoint，对每个 `diskRecoveryPlsn` 调用 group header 校验；**第一个通过校验的位置**即作为 `m_recoveryStartPlsn`。
- **回收**：WAL 文件回收门槛由"最新 checkpoint 的 diskRecoveryPlsn"放宽为"**N 槽中最旧的 diskRecoveryPlsn**"，确保所有历史槽对应的 WAL 物理上仍然存在。

### 2.1 为什么"回退到老 checkpoint"是安全的

历史 checkpoint 的 `diskRecoveryPlsn` 比当前 checkpoint 更小（更靠前）。redo 是幂等的：从更早的位置开始回放，会把 [old_plsn, latest_plsn] 这段 WAL 多回放一遍，对页面的最终状态没有影响（`WalRecovery::IsLsnError` / `GetWalRecordReplayType` 已具备幂等过滤逻辑）。代价仅是**recovery 时间变长**（多回放一段 WAL），换得的是"checkpoint 位置写坏时仍能恢复"。

唯一硬约束：**老 checkpoint 对应的 WAL 文件必须还在**。这通过 §3.4 调整回收门槛实现。

## 3. 详细设计

### 3.1 数据结构变更

#### 3.1.1 新增 `WalCheckPointHistory`

新增"历史链"结构，与现有 `WalCheckPoint` 解耦（保留旧字段以便 §5 兼容性方案）：

```cpp
// include/wal/dstore_wal_struct.h
constexpr uint8 WAL_CHECKPOINT_HISTORY_SLOTS = 3; // 编译期常量，不做 GUC

struct WalCheckPointHistory {
    uint8  slotCount;                                  // 实际有效槽数（启动初期可能 < N）
    uint8  newestSlotIdx;                              // 最新一条所在的槽下标 [0, N)
    uint16 _pad;
    WalCheckPoint slots[WAL_CHECKPOINT_HISTORY_SLOTS]; // 环形数组

    // —— 辅助函数（详见 §3.4）——
    uint64 GetOldestDiskRecoveryPlsn() const;          // N 槽中最旧的 diskRecoveryPlsn
    uint8  GetSlotIdxByAge(uint8 k) const;             // 第 k 老槽的下标，k=0 即最新
};
```

`slots[newestSlotIdx]` 是最新的一条；按 `(newestSlotIdx + N - k) % N` 倒序遍历得到第 k 老的一条。

#### 3.1.2 修改 `ControlWalStreamPageItemData`

```cpp
// include/control/dstore_control_walinfo.h
struct ControlWalStreamPageItemData {
    // ... 原有字段保持不变 ...
    WalCheckPoint lastWalCheckpoint;        // 兼容字段：始终等于 checkpointHistory.slots[newestSlotIdx]
    WalCheckPointHistory checkpointHistory; // 新增字段
    // ...
};
```

`lastWalCheckpoint` **保留**且其值始终等同于"最新一条历史"，避免改动其它读路径（`WalDiagnose`、备份恢复、UT 等）。新逻辑只在 recovery 启动时走 `checkpointHistory`。

`Copy` / `Dump` 函数同步增加新字段处理。

#### 3.1.3 控制文件 page 布局

`ControlWalStreamPageItemData` 体积增大 `2 * sizeof(WalCheckPoint) + 4` 字节（新增 N-1 个 `WalCheckPoint`，N=3）。

需要校验单条 item 是否仍能放进一个 control page。如果超出，分两步：

- 优先方案：把 `WalCheckPoint::time` 等可派生字段从历史槽中剔除，只保留 recovery 关键的 `diskRecoveryPlsn` / `memRecoveryPlsn` / `term`（即新增 `WalCheckPointSlim` 结构体）。
- 兜底方案：`WalCheckPointHistory` 拆到独立的 control page 段，由新的 `ControlWalCheckpointHistoryInfo` 类管理，与 `ControlWalStreamPageItemData` 通过 `walId` 关联。

具体走哪条路，等到拍版前用 `sizeof(ControlWalStreamPageItemData)` 与 `CONTROLFILE_PAGE_USABLE_SIZE` 实测对比后决定。

### 3.2 写路径：checkpoint 时旋转更新

`CheckpointMgr::DoCheckpoint`（`src/buffer/dstore_checkpointer.cpp:230` 段附近）当前的逻辑：

```cpp
walStreamInfo->lastWalCheckpoint = checkPoint;
controlFile->UpdateWalStreamForCheckPointWithBarrier(walId, ..., walStreamInfo->lastWalCheckpoint, ...);
```

改为：

```cpp
WalCheckPointHistory &hist = walStreamInfo->checkpointHistory;
uint8 nextSlot = (hist.slotCount == 0) ? 0
                                       : (hist.newestSlotIdx + 1) % WAL_CHECKPOINT_HISTORY_SLOTS;
hist.slots[nextSlot] = checkPoint;
hist.newestSlotIdx = nextSlot;
hist.slotCount = std::min<uint8>(hist.slotCount + 1, WAL_CHECKPOINT_HISTORY_SLOTS);
walStreamInfo->lastWalCheckpoint = checkPoint;  // 兼容字段同步
controlFile->UpdateWalStreamForCheckPointWithBarrier(walId, ..., *walStreamInfo);
```

> 写入仍然是一次 control page 的原子刷盘（`PostGroup`，受 `CFLockMode::CF_EXCLUSIVE` 保护，crc 校验），不引入新的并发问题。

`UpdateWalStreamForCheckPointWithBarrier` 的签名调整：原签名中的 `const WalCheckPoint &lastWalCheckpoint` 入参**直接替换为整个 `walStreamInfo` 的引用**（即 `const ControlWalStreamPageItemData &streamInfo`），这样函数内部既能拿到 `lastWalCheckpoint` 也能拿到 `checkpointHistory`，避免再加第二个参数（`src/control/dstore_control_walinfo.cpp:126`）。`UpdateWalStreamForCheckPoint` 同步调整。

### 3.3 读路径：recovery 启动时按新到旧验证

`WalRecovery::InitRecoveryPlsn` 当前签名：

```cpp
void WalRecovery::InitRecoveryPlsn(WalId walId, WalCheckPoint *lastWalCheckpoint);
```

改为接收整个 `ControlWalStreamPageItemData`（拿到 `checkpointHistory`）并按"**先走老逻辑快路径，老逻辑失败再走新逻辑**"的顺序判断：

```cpp
void WalRecovery::InitRecoveryPlsn(WalId walId, const ControlWalStreamPageItemData *streamInfo)
{
    // —— 快路径：与改造前完全一致，直接用 lastWalCheckpoint。 ——
    // 只有当 lastWalCheckpoint 对应位置无法解析出合法 group 时，才走 N 槽回退。
    const WalCheckPoint &latest = streamInfo->lastWalCheckpoint;
    uint64 latestCandidate = ChooseRecoveryPlsn(latest);
    if (ProbeWalGroupAtPlsn(walId, latestCandidate) == DSTORE_SUCC) {
        m_recoveryStartPlsn     = latestCandidate;
        m_diskRecoveryStartPlsn = latest.diskRecoveryPlsn;
        return;  // 与原有行为完全等价，新逻辑零侵入
    }

    ErrLog(DSTORE_WARNING, MODULE_WAL,
           ErrMsg("[PDB:%u WAL:%lu] lastWalCheckpoint plsn=%lu probe failed, "
                  "fallback to checkpointHistory.", m_pdbId, walId, latestCandidate));

    // —— 慢路径：N 槽从新到旧逐个尝试。 ——
    const WalCheckPointHistory &hist = streamInfo->checkpointHistory;
    // 注意：slot[newestSlotIdx] 等价于 lastWalCheckpoint，已在上面试过 —— 从 k=1 开始即可。
    for (uint8 k = 1; k < hist.slotCount; ++k) {
        uint8 idx = hist.GetSlotIdxByAge(k);
        const WalCheckPoint &cp = hist.slots[idx];
        uint64 candidate = ChooseRecoveryPlsn(cp);

        if (ProbeWalGroupAtPlsn(walId, candidate) == DSTORE_SUCC) {
            m_recoveryStartPlsn     = candidate;
            m_diskRecoveryStartPlsn = cp.diskRecoveryPlsn;
            ErrLog(DSTORE_LOG, MODULE_WAL,
                   ErrMsg("[PDB:%u WAL:%lu] recovery rolled back %u checkpoint(s) "
                          "to slot %u plsn=%lu", m_pdbId, walId, k, idx, candidate));
            return;
        }
        ErrLog(DSTORE_WARNING, MODULE_WAL,
               ErrMsg("[PDB:%u WAL:%lu] checkpoint slot %u plsn=%lu probe failed, "
                      "fallback to older slot.", m_pdbId, walId, idx, candidate));
    }
    // 所有槽都失败 —— 走 §3.5 fallback。
    HandleAllSlotsCorrupted(walId, hist);
}
```

**为什么先走老逻辑快路径（设计加固）**

1. **零侵入**：99.999% 情况下 `lastWalCheckpoint` 是合法的，走快路径后直接 return，新代码路径完全不被触发，老逻辑的行为与改造前完全一致 —— **任何 N 槽侧的潜在 bug（探测函数误判、环形索引算错、兼容字段未同步等）都不会污染正常恢复路径**。
2. **故障半径收敛**：新逻辑只在"老逻辑已经失败"时才被启用；这本身就是异常路径，多走几次探测、记录更多日志、走更保守的回退策略都是可接受的。
3. **与不变式 I3 自洽**：因为 `lastWalCheckpoint == slots[newestSlotIdx]`，快路径 + 慢路径从 k=1 开始遍历，**不会重复探测同一位置**，也不会丢漏任何槽。

#### 3.3.1 `ProbeWalGroupAtPlsn` —— 校验函数

复用现有的 `WalGroupParser`：

```cpp
RetStatus WalRecovery::ProbeWalGroupAtPlsn(WalId walId, uint64 candidate)
{
    // 1. 该 plsn 必须仍在 walFile 物理可达范围内
    WalFile *file = m_walStream->GetWalFileManager()->GetWalFileByPlsn(candidate);
    if (file == nullptr) {
        return DSTORE_FAIL;
    }

    // 2. 用最小代价的 WalGroupParser 实例化 + ReadCurrent
    WalGroupParser parser(...);
    parser.SetReadStartPlsn(candidate);
    parser.SetNeedCheckGroupHeader(true);  // 强制走 ValidWalGroupLen + ValidWalGroup
    const WalRecordAtomicGroup *group = nullptr;
    return parser.ReadCurrent(group);      // 内部已包含 groupLen 范围 + CRC 校验
}
```

`ReadCurrent` 失败的两种来源：

- `ValidWalGroupLen` 失败：`groupLen < WAL_GROUP_HEADER_SIZE || groupLen >= WAL_GROUP_MAX_SIZE`（`src/wal/dstore_wal_reader.cpp:225`）。
- `ValidWalGroup` 失败：CRC 不匹配（`src/wal/dstore_wal_reader.cpp:298`）。

只要任一失败就视为该位置不可信。

> 注意：`ProbeWalGroupAtPlsn` 是**只读探测**，不能修改 `m_redoReadBuffer` / `m_curRedoFinishedPlsn` 等正式 redo 用的状态。建议给它一个独立的临时 memory context 与读 buffer，探测完毕立即销毁。

### 3.4 WAL 回收门槛调整（关键约束）

#### 3.4.1 抽出 `WalCheckPointHistory::GetOldestDiskRecoveryPlsn`

把"遍历 N 槽取最旧 plsn"的逻辑封装成 `WalCheckPointHistory` 的成员函数，避免 `WalFileManager` / `WalRecovery` / 诊断工具各自重复实现：

```cpp
// include/wal/dstore_wal_struct.h
uint64 WalCheckPointHistory::GetOldestDiskRecoveryPlsn() const
{
    if (slotCount == 0) {
        return INVALID_PLSN;
    }
    uint64 oldest = UINT64_MAX;
    for (uint8 i = 0; i < slotCount; ++i) {
        oldest = std::min(oldest, slots[i].diskRecoveryPlsn);
    }
    return oldest;
}
```

> 配套的 `GetSlotIdxByAge(uint8 k)` 同样作为成员函数，统一封装环形索引计算（k=0 即最新）：
> `return (newestSlotIdx + WAL_CHECKPOINT_HISTORY_SLOTS - k) % WAL_CHECKPOINT_HISTORY_SLOTS;`
> 这样 §3.3 读路径与 §3.4 回收路径都不再裸写环形下标，单元测试也只需针对成员函数本身。

#### 3.4.2 `GetRecyclablePlsn` 调整

修改 `WalFileManager::GetRecyclablePlsn` (`src/wal/dstore_wal_file_manager.cpp:763`)：

```cpp
uint64 WalFileManager::GetRecyclablePlsn() const
{
    // ... 取 ckpMgr / pdb 同前 ...
    WalCheckPointHistory hist;
    if (STORAGE_FUNC_FAIL(ckpMgr->GetWalCheckpointHistory(m_initWalFilesPara.walId, hist))) {
        return INVALID_PLSN;
    }
    return hist.GetOldestDiskRecoveryPlsn();   // 用 N 槽中最旧的 diskRecoveryPlsn 作为回收下界
}
```

这意味着**WAL 文件多保留 N-1 个 checkpoint 周期的内容**。

实际 WAL 占用的增长上界 ≈ `(N-1) × checkpoint_interval × 平均 wal 写入速率`。
N=3、checkpoint 周期分钟级别时多保留几百 MB 到几 GB —— 这是为换取容错能力付出的固定代价，**N 不可调**。

### 3.5 兜底：N 槽全部失败

如果所有槽都探测失败，说明问题已经超出本设计能覆盖的范围（要么三个槽都被同时写坏，要么对应的 WAL 文件已被异常删除）。处理策略：

- **不**自动选择最老一条强行恢复 —— 否则可能截断更多合法 WAL，扩大破坏面。
- 直接 `ErrLog(DSTORE_PANIC, ...)` 退出进程，并把三个槽的内容、对应 WAL 文件列表、当前可见的 WAL 起止 plsn 全部 dump 到日志。
- 留给运维：通过下面 §3.5.1 扩展后的 `waldump` 工具完成"扫出合法起点 + 写回 control file"，再次拉起。

#### 3.5.1 `waldump` 工具能力扩展

当前 `waldump` 仅支持解析 WAL 文件、打印 group 内容（`tools/waldump/`）。为承接 §3.5 的人工恢复，新增两个子命令：

| 子命令 | 输入 | 行为 |
|--------|------|------|
| `waldump --scan-valid-checkpoint --wal-dir <dir> [--from-plsn <p>]` | WAL 文件目录、可选起始 plsn | 从指定位置（缺省为最早 WAL 文件起点）顺序扫描，打印所有通过 `ValidWalGroupLen` + `ValidWalGroup` 校验的 group header 列表，以及最末一个合法 group 的结束 plsn —— 这就是手工选起点的依据 |
| `waldump --patch-control-checkpoint --control-file <path> --wal-id <id> --plsn <p> [--dry-run]` | control file 路径、wal id、要写入的 plsn | 加载 control file，将指定 wal 流的 `lastWalCheckpoint.diskRecoveryPlsn` 与 `checkpointHistory.slots[newestSlotIdx].diskRecoveryPlsn` **同时**改写为指定值，重算 control page CRC 后回写。`--dry-run` 模式只打印 diff 不落盘 |

> **安全约束**：`--patch-control-checkpoint` 必须在进程未拉起、且 control file 没有被独占（无 `.lock` 文件）时才允许执行；执行前自动 `cp` 一份 `.bak` 备份。
> **复用代码**：扫描子命令复用 §3.3.1 的 `ProbeWalGroupAtPlsn` / `WalGroupParser::ReadCurrent`；改写子命令复用 `ControlFile::WriteSinglePage` 的底层接口，避免实现两套写盘逻辑。

这样兜底链路是：**进程 panic → 运维拿日志中的"最早可见 WAL plsn" → `waldump --scan-valid-checkpoint` 找到合法起点 → `waldump --patch-control-checkpoint --dry-run` 预览 → 真正落盘 → 重新拉起**。无需手写脚本、不需要离线编译辅助工具。

### 3.6 关键不变式

| # | 不变式 | 由谁保证 |
|---|--------|----------|
| I1 | `slots[newestSlotIdx]` 始终是最新一条 checkpoint | §3.2 写路径 |
| I2 | `slotCount` 单调递增直到饱和 N | §3.2 写路径 |
| I3 | `lastWalCheckpoint == slots[newestSlotIdx]`（兼容字段） | §3.2 写路径 |
| I4 | 最旧槽的 `diskRecoveryPlsn` ≤ WAL 物理保留下界 | §3.4 回收门槛 |
| I5 | 探测路径不修改任何持久化状态 / 不影响并发 redo | §3.3.1 隔离的 memctx |

## 4. 代码改动点清单

| 模块 | 文件 | 改动 |
|------|------|------|
| 数据结构 | `include/wal/dstore_wal_struct.h` | 新增 `WalCheckPointHistory` 与 `WAL_CHECKPOINT_HISTORY_SLOTS` |
| 控制文件 | `include/control/dstore_control_walinfo.h` | `ControlWalStreamPageItemData` 增字段、`Copy/Dump` 同步 |
| 控制文件 | `src/control/dstore_control_walinfo.cpp` | `UpdateWalStreamForCheckPoint{,WithBarrier}` 接收/写入 `WalCheckPointHistory` |
| 控制文件接口 | `include/control/dstore_control_file.h` | 同上签名调整 |
| Checkpoint 写 | `src/buffer/dstore_checkpointer.cpp` | `DoCheckpoint` 旋转写入新槽 |
| Checkpoint 缓存 | `include/buffer/dstore_checkpointer.h` & cpp | 新增 `GetWalCheckpointHistory(walId, &hist)` |
| Recovery 读 | `include/wal/dstore_wal_recovery.h` | `InitRecoveryPlsn` 签名 + 新增 `ProbeWalGroupAtPlsn` / `HandleAllSlotsCorrupted` |
| Recovery 读 | `src/wal/dstore_wal_recovery.cpp` | 实现按新到旧探测 |
| WAL 回收 | `src/wal/dstore_wal_file_manager.cpp` | `GetRecyclablePlsn` 改为取 N 槽最旧 |
| WAL 诊断工具 | `src/wal/dstore_wal_diagnose.cpp`、`src/wal/dstore_wal_dump*.cpp` | 同步打印 N 槽信息（可选） |
| waldump 工具 | `tools/waldump/` | 新增 `--scan-valid-checkpoint` / `--patch-control-checkpoint` 子命令（§3.5.1） |

## 5. 兼容性 & 升级

**本期暂不考虑升降级场景**，留作后续迭代。

仅记录一条已知约束：当前 dstore 还是单一二进制独立编译/独立测试的组件，没有平滑升级流程；接入到上游产品后再补"旧版本 control file → 新版本 control file"的迁移路径与降级回退方案。

设计上预留的钩子是：`lastWalCheckpoint` 字段保留并继续被双写（§3.1.2），未来若需要降级到旧版本，旧逻辑可以无感继续读这个字段。

## 6. 测试计划

### 6.1 单元测试（`tests/unittest/src/ut_wal/`）

- `UTWalRecovery_CheckpointHistory_RotateWrite` —— 连续 checkpoint N+2 次，验证 `newestSlotIdx`、`slotCount`、被覆盖槽的内容。
- `UTWalRecovery_CheckpointHistory_NewestValid` —— 最新槽合法 → recovery 选中最新槽，`m_recoveryStartPlsn` 等于最新 `diskRecoveryPlsn`。
- `UTWalRecovery_CheckpointHistory_NewestCorrupted` —— 在最新 `diskRecoveryPlsn` 对应位置注入坏数据（破坏 `groupLen` 或 CRC）→ recovery 选中次新槽。
- `UTWalRecovery_CheckpointHistory_OnlyOldestValid` —— 最新与次新都坏 → 选中最老槽。
- `UTWalRecovery_CheckpointHistory_AllCorrupted` —— 全坏 → 进程 panic（`EXPECT_DEATH`）。
- `UTWalRecovery_CheckpointHistory_Truncate` —— 验证 fallback 后 `Truncate` 使用的是回退后槽的 `lastGroupEndPlsn`，**不会**误删合法 WAL。
- `UTWalFileManager_RecyclablePlsn_TakesOldest` —— 三槽 plsn 各异，`GetRecyclablePlsn` 返回最小者。
- `UTControlWalInfo_BackwardCompat` —— 模拟旧版本 page（`slotCount==0`），首次拉起后正确填充槽 0。

### 6.2 故障注入测试（`ENABLE_FAULT_INJECTION`）

复用现有 `DstorePdbReplicaFI` 框架，新增注入点：

- `BEFORE_CHECKPOINT_WRITE_HISTORY` —— 在写 control file 之前篡改最新 `diskRecoveryPlsn` 为非法值。
- `AFTER_CHECKPOINT_WRITE_HISTORY` —— 写后立即 kill，模拟部分写。

随后拉起进程，断言 redo 完成 + WAL 未被错误截断。

### 6.3 集成测试

- TPC-C 跑 30 分钟，期间触发 checkpoint ≥ 5 次，停库后用 `pagedump` / `waldump` 手动篡改 control file 最新槽 plsn 为乱值，重新拉起，验证业务数据完整。

## 7. 风险与权衡

| 风险 | 评估 | 缓解 |
|------|------|------|
| WAL 占用增长 | 增加 (N-1) × ckpt_interval 周期的 WAL，最坏几 GB | N=3 写死，作为容错的固定代价接受 |
| Control page 容量 | 新增 ~2 × `sizeof(WalCheckPoint)`，可能挤压同 page 其它字段 | §3.1.3 分情况处理（瘦身 / 拆 page） |
| 探测路径误判 | CRC 撞库导致老坏槽被认为合法（理论极低） | 接受：本设计目标不含此场景，由更上层冗余保障 |
| 老槽 WAL 被异常删除 | 若回收逻辑有 bug 提前删了老槽对应文件，回退失败 | §3.5 panic + 日志，I4 由 §3.4 单点保证，加 UT 兜底 |
| 探测开销 | recovery 启动多读 ≤ N 个 group | N=3 时 ≤ 3 次小 IO，相比整段 redo 可忽略 |

## 8. 后续扩展（非本次实现）

- N 槽间增加 LSN 单调性校验（`slots[k+1].diskRecoveryPlsn > slots[k].diskRecoveryPlsn`），可识别"看似合法但实际乱序"的写坏场景。
- 探测时不仅校验 group header，还向后多读一个 group，验证两组之间 `prevGroupPlsn` 串得起来。
- 把"恢复时回退了几步"作为一个 perf counter 暴露，运维可监控。

