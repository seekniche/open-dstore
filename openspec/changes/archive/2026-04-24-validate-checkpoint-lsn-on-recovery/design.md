## Context

Recovery 启动链路（与本设计相关的部分）：

1. `WalRecovery::Init` → `InitRecoveryPlsn` 从 `ControlWalStreamPageItemData::lastWalCheckpoint` 读出 `diskRecoveryPlsn`，写入 `m_recoveryStartPlsn`。
2. `WalStream::Recovery` → `WalRecovery::Recovery` → `Redo` → `RedoLoopReadAndRedoWalRecord` 循环 `walRecordReader->ReadNext` 解析 atomic group，最后通过出参 `lastGroupEndPlsn` 回传"已成功 redo 的最后一组的结束位置"。
3. `WalStream::Recovery` 调用 `Truncate(lastGroupEndPlsn)` 把后续 WAL 文件物理截断。

整条链路对那一份 `lastWalCheckpoint.diskRecoveryPlsn` 完全信任。一旦该字段因部分写、磁盘静默错误或 bug 落到错误位置：`ReadNext` 在错位上无法解析合法 group（`ValidWalGroupLen` / `ValidWalGroup` CRC 失败）→ `lastGroupEndPlsn` 退化为错误的起点 → `Truncate` 把本应回放的合法 WAL 当"多余尾部"截断 → **数据丢失，不可恢复**。

详细背景与公式化前提见 `docs/design/wal_recovery_checkpoint_history.md` §1。

## Goals / Non-Goals

**Goals:**

- 在不改变正常路径性能与行为的前提下，让 recovery 启动阶段能识别"checkpoint 不是合法 group 起点"。
- 自动回退到一个更老的、合法的 checkpoint 位置，避免错误 truncate 合法 WAL。
- 提供运维兜底链路（`waldump` 子命令），承接"全部历史槽都失败"的人工恢复。
- 关键不变式可由单元测试覆盖：`slots[newestSlotIdx]` 是最新；`slotCount` 单调饱和到 N；`lastWalCheckpoint == slots[newestSlotIdx]`；最旧槽的 `diskRecoveryPlsn ≤ WAL 物理保留下界`；探测路径只读不污染并发 redo。

**Non-Goals:**

- 不解决"checkpoint 位置看似合法但语义错误"（CRC 撞上 + groupLen 也合法）的极端场景 —— 该场景需业务层 LSN 单调性校验或冗余备份，留给 §8 后续扩展。
- 不改变正常 redo / dispatch / 并行回放流程。
- 不改变 control file 的 page 布局（仅扩展单条 `ControlWalStreamPageItemData` 的字段）。
- 本期不考虑升降级场景；预留 `lastWalCheckpoint` 双写作为未来降级钩子。

## Flow Overview

### Overall Closure

```mermaid
flowchart TD
    A[Checkpoint completes] --> B[Push new WalCheckPoint into history ring]
    B --> C[Sync lastWalCheckpoint = newest slot]
    C --> D[Single control-page write with CRC]
    D --> E[WAL retention lower bound = oldest slot diskRecoveryPlsn]

    F[Recovery starts] --> G[Read lastWalCheckpoint and checkpointHistory]
    G --> H{Fast-path probe latest slot}
    H -->|success| I[Use latest diskRecoveryPlsn]
    H -->|fail| J[Probe older slots from new to old]
    J -->|first success| K[Roll back N checkpoint(s) and continue redo]
    J -->|all fail| L[Panic and dump slot/WAL diagnostics]

    L --> M[Operator runs waldump --scan-valid-checkpoint]
    M --> N[Find valid group start/end candidates]
    N --> O[Operator runs waldump --patch-control-checkpoint]
    O --> P[Restart and recover from repaired checkpoint]
```

## Decisions

### Decision 1：N=3 槽环形历史，编译期常量

把"单槽 checkpoint"扩展为长度 N=3 的环形数组 `WalCheckPointHistory`：

```cpp
constexpr uint8 WAL_CHECKPOINT_HISTORY_SLOTS = 3;
#pragma pack(push, 1)
struct WalCheckPointHistory {
    uint8  slotCount;       // 启动初期可能 < N
    uint8  newestSlotIdx;   // 最新一条所在的槽下标 [0, N)
    WalCheckPoint slots[WAL_CHECKPOINT_HISTORY_SLOTS];
    uint64 GetOldestDiskRecoveryPlsn() const;
    uint8  GetSlotIdxByAge(uint8 k) const;
};
#pragma pack(pop)
```

> 用 `#pragma pack(push, 1)` 紧凑布局，省掉 `slots[]` 前面那 2 字节的自然对齐空洞，避免显式塞 `_pad` 字段又得在每次 memcpy/memset 时小心处理它的语义。

**为什么 N=3 而不是 GUC：**
- N=2 时容错能力等于"最新一条坏，落到次新"，无法应对"最新两条都坏"；N=3 已经覆盖到"连续两次 checkpoint 部分写"这种已知的硬件场景，再大收益边际递减。
- 做成 GUC 会带来：①额外的运行期分支 ②槽数变化时的回收下界跳跃 ③故障注入测试矩阵爆炸。容错能力是"设计决策"而非"运行时调参点"。

**替代方案：**
- 链表/动态分配：拒绝。control file 必须是定长结构。
- 把整张 control page 双写：拒绝。control file 已经做了双写；本设计要解决的是"checkpoint 字段语义错误"而非"page 物理写损"。

### Decision 2：写路径只覆盖一个槽位

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

写入仍然是一次 control page 的原子刷盘（`PostGroup`，`CFLockMode::CF_EXCLUSIVE` + CRC 校验），不引入新的并发问题。

`UpdateWalStreamForCheckPoint{,WithBarrier}` 入参由 `const WalCheckPoint&` 改为 `const ControlWalStreamPageItemData&`，让函数同时拿到 `lastWalCheckpoint` 与 `checkpointHistory`，避免新增第二个参数。

```mermaid
flowchart TD
    A[CheckpointMgr::DoCheckpoint] --> B[Load walStreamInfo.checkpointHistory]
    B --> C{slotCount == 0?}
    C -->|yes| D[nextSlot = 0]
    C -->|no| E[nextSlot = newestSlotIdx + 1 mod N]
    D --> F[slots[nextSlot] = checkPoint]
    E --> F
    F --> G[newestSlotIdx = nextSlot]
    G --> H[slotCount = min(slotCount + 1, N)]
    H --> I[lastWalCheckpoint = checkPoint]
    I --> J[UpdateWalStreamForCheckPointWithBarrier(..., walStreamInfo)]
    J --> K[Control page persisted once with barrier/CRC]
```

### Decision 3：读路径"老逻辑快路径 + 新逻辑慢路径"

`InitRecoveryPlsn` 的执行顺序：

1. 用 `lastWalCheckpoint` 走 `ProbeWalGroupAtPlsn`；通过即返回，**与改造前完全等价**。
2. 仅当快路径失败时，按"从新到旧"遍历 `checkpointHistory.slots`（k 从 1 开始，因为 `slots[newestSlotIdx]` 已经在快路径试过）。
3. 第一个通过 `ProbeWalGroupAtPlsn` 的位置即作为 `m_recoveryStartPlsn`。
4. 全部失败 → `HandleAllSlotsCorrupted` 进 panic 并 dump 现场。

**为什么先走老逻辑：**
- **零侵入**：99.999% 情况下 `lastWalCheckpoint` 合法，新代码路径不被触发，不会污染正常恢复。
- **故障半径收敛**：新逻辑只在异常路径执行；多走几次探测、记录更多日志、走更保守策略都可接受。
- **与不变式 I3 自洽**：`lastWalCheckpoint == slots[newestSlotIdx]`，慢路径从 k=1 开始，不会重复探测同一位置，也不会丢漏槽。

**替代方案：**
- 一开始就遍历整个 `checkpointHistory`，找到最新合法的：拒绝。等价于把"探测 + 选择"作为正常路径，新代码任何 bug 都会污染常规恢复。
- 不要 fast path 的 probe，直接信任 `lastWalCheckpoint`：拒绝。等于把这次改造退化为"加了几个未被使用的字段"，根本不解决问题。

```mermaid
flowchart TD
    A[WalRecovery::Init] --> B[InitRecoveryPlsn(streamInfo)]
    B --> C[latest = lastWalCheckpoint]
    C --> D{ProbeWalGroupAtPlsn(latest) succeeds?}
    D -->|yes| E[Set m_recoveryStartPlsn = latest]
    E --> F[Continue normal redo]
    D -->|no| G[Warn and enter history fallback]
    G --> H[For k = 1 .. slotCount - 1]
    H --> I[Pick slot by age from new to old]
    I --> J{Probe slot succeeds?}
    J -->|yes| K[Adopt that slot and log rolled back k checkpoint(s)]
    K --> F
    J -->|no| L{More older slots?}
    L -->|yes| H
    L -->|no| M[HandleAllSlotsCorrupted]
    M --> N[Panic with slot dump + WAL file list]
```

### Decision 4：`ProbeWalGroupAtPlsn` 复用现有 group 校验

```cpp
RetStatus WalRecovery::ProbeWalGroupAtPlsn(WalId walId, uint64 candidate) {
    WalFile *file = m_walStream->GetWalFileManager()->GetWalFileByPlsn(candidate);
    if (file == nullptr) return DSTORE_FAIL;
    WalGroupParser parser(...);
    parser.SetReadStartPlsn(candidate);
    parser.SetNeedCheckGroupHeader(true);    // 强制 ValidWalGroupLen + ValidWalGroup
    const WalRecordAtomicGroup *group = nullptr;
    return parser.ReadCurrent(group);
}
```

校验失败的两种来源已经被 `WalGroupParser` 覆盖：
- `ValidWalGroupLen` 失败：`groupLen < WAL_GROUP_HEADER_SIZE || groupLen >= WAL_GROUP_MAX_SIZE`。
- `ValidWalGroup` 失败：CRC 不匹配。

**只读约束**：`ProbeWalGroupAtPlsn` 不能修改 `m_redoReadBuffer` / `m_curRedoFinishedPlsn` 等正式 redo 用的状态；用独立的临时 memory context 与读 buffer，探测完毕立即销毁。

### Decision 5：WAL 回收下界 = N 槽中最旧的 `diskRecoveryPlsn`

老逻辑只看最新 checkpoint 时，最旧槽对应的 WAL 文件早就被回收了；回退能力会失效。

```cpp
uint64 WalFileManager::GetRecyclablePlsn() const {
    WalCheckPointHistory hist;
    if (STORAGE_FUNC_FAIL(ckpMgr->GetWalCheckpointHistory(m_initWalFilesPara.walId, hist))) {
        return INVALID_PLSN;
    }
    return hist.GetOldestDiskRecoveryPlsn();
}
```

**代价**：WAL 占用上界增加 `(N-1) × checkpoint_interval × 平均写入速率`。N=3、分钟级 checkpoint 周期下多保留几百 MB 到几 GB —— 这是为容错付出的固定代价，N 不可调。

### Decision 6：全槽失败时 panic，不强行选老槽

如果三个槽全部探测失败，意味着问题已经超出本设计能覆盖的范围（要么三个槽同时被写坏，要么对应 WAL 文件已被异常删除）。

- **不**自动选择最老一条强行恢复 —— 否则可能截断更多合法 WAL，扩大破坏面。
- 直接 `ErrLog(DSTORE_PANIC, ...)` 退出进程，把三个槽的内容、对应 WAL 文件列表、当前可见的 WAL 起止 plsn 全部 dump 到日志。
- 留给运维：通过 `waldump` 工具完成"扫出合法起点 + 写回 control file"，再次拉起。

### Decision 7：`waldump` 工具承接人工兜底

| 子命令 | 行为 |
|--------|------|
| `waldump --scan-valid-checkpoint --wal-dir <dir> [--from-plsn <p>]` | 顺序扫描，打印所有通过 `ValidWalGroupLen + ValidWalGroup` 的 group header，以及最末一个合法 group 的结束 plsn |
| `waldump --patch-control-checkpoint --control-file <path> --wal-id <id> --plsn <p> [--dry-run]` | 加载 control file，将指定 wal 流的 `lastWalCheckpoint.diskRecoveryPlsn` 与 `checkpointHistory.slots[newestSlotIdx].diskRecoveryPlsn` 同时改写为指定值，重算 page CRC 后回写 |

**安全约束**：`--patch-control-checkpoint` 必须在进程未拉起、且 control file 未被独占（无 `.lock`）时才允许执行；执行前自动 `cp` 一份 `.bak` 备份。

**复用代码**：扫描子命令复用 `ProbeWalGroupAtPlsn` / `WalGroupParser::ReadCurrent`；改写子命令复用 `ControlFile::WriteSinglePage` 的底层接口。

## Risks / Trade-offs

| 风险 | 评估 | 缓解 |
|------|------|------|
| WAL 占用增长 | 增加 (N-1) × ckpt_interval 周期的 WAL，最坏几 GB | N=3 写死，作为容错的固定代价接受 |
| Control page 容量挤压 | 新增 ~2 × `sizeof(WalCheckPoint)`，可能挤压同 page 其它字段 | 配置 cmake 阶段实测 `sizeof(ControlWalStreamPageItemData)` vs `CONTROLFILE_PAGE_USABLE_SIZE`，超出则走"瘦身槽 `WalCheckPointSlim` / 拆 page" 兜底 |
| 探测路径误判 | CRC 撞库导致老坏槽被认为合法（理论极低） | 接受：本设计目标不含此场景，由更上层冗余保障 |
| 老槽 WAL 被异常删除 | 若回收逻辑有 bug 提前删了老槽对应文件，回退失败 | §3.5 panic + 日志，I4 由 `GetRecyclablePlsn` 单点保证，加 UT 兜底 |
| 探测开销 | recovery 启动多读 ≤ N 个 group | N=3 时 ≤ 3 次小 IO，相比整段 redo 可忽略 |
| 新代码侵入正常路径 | 任何 N 槽侧的 bug 都可能污染常规恢复 | "老逻辑快路径 + 新逻辑慢路径" 顺序（Decision 3），把新代码限制在异常分支 |

## Migration Plan

本期不考虑升降级，仅记录已知约束与未来钩子：

- 当前 dstore 是单一二进制独立编译/独立测试的组件，没有平滑升级流程；接入到上游产品后再补"旧版本 control file → 新版本 control file"的迁移路径。
- 设计上预留：`lastWalCheckpoint` 字段保留并继续被双写。未来若需要降级到旧版本，旧逻辑可以无感继续读这个字段。
- 首次拉起新版本读到旧 page（`slotCount == 0`）时，按"backwards-compat path"处理：`GetOldestDiskRecoveryPlsn` 返回 `INVALID_PLSN`，回收下界回退到 `lastWalCheckpoint.diskRecoveryPlsn`；下一轮 checkpoint 写入后，`slotCount` 变 1，行为切换到新逻辑。

## Open Questions

- Control page 容量实测结果未定：等到 cmake 配置阶段对 `sizeof(ControlWalStreamPageItemData)` 与 `CONTROLFILE_PAGE_USABLE_SIZE` 实测后，决定是否走 `WalCheckPointSlim` 瘦身槽方案。
- 故障注入点 `BEFORE_CHECKPOINT_WRITE_HISTORY` / `AFTER_CHECKPOINT_WRITE_HISTORY` 在现有 `DstorePdbReplicaFI` 框架下的具体接入位置待确认。
- §8 的"LSN 单调性校验"与"探测时多读一个 group 验证 prevGroupPlsn 串接"作为后续扩展，本期不实现。
