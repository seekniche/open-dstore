## Why

Recovery 启动时对 control file 中的 `lastWalCheckpoint.diskRecoveryPlsn` 完全信任：一旦该字段因部分写、磁盘静默错误或 bug 落到非法位置，`WalRecordReader::ReadNext` 会在错位上无法解析合法 atomic group，`lastGroupEndPlsn` 退化为错误起点，随后的 `Truncate` 会把本该回放的合法 WAL 物理截断，造成**数据丢失且不可恢复**。本次改造在不影响正常路径的前提下，让 recovery 能识别"checkpoint 不是合法 group 起点"并自动回退到更老的合法 checkpoint。

## What Changes

- 把 control file 中的"单槽 checkpoint"扩展为长度为 N=3 的环形历史链 `WalCheckPointHistory`（编译期常量，不做 GUC）。
- Checkpoint 写路径每次只覆盖一个槽位，最旧的历史被覆盖、其余 N-1 条保留；兼容字段 `lastWalCheckpoint` 始终等于最新槽。
- Recovery 读路径先按老逻辑快路径用 `lastWalCheckpoint`，仅当对该位置的 group header 校验（`ValidWalGroupLen` + `ValidWalGroup` CRC）失败时，才按"从新到旧"遍历 N 槽，落到第一个通过校验的位置。
- WAL 回收门槛由"最新 checkpoint 的 diskRecoveryPlsn"放宽为"N 槽中最旧的 diskRecoveryPlsn"，确保所有历史槽对应的 WAL 文件物理上仍然存在。
- 全部 N 槽都失败时直接 `DSTORE_PANIC` 退出并 dump 现场，**不**自动选老槽强行恢复，避免扩大破坏面。
- `waldump` 工具新增 `--scan-valid-checkpoint` / `--patch-control-checkpoint` 子命令，承接全槽失败后的人工恢复链路。
- **BREAKING（控制文件结构）**：`ControlWalStreamPageItemData` 新增 `WalCheckPointHistory checkpointHistory` 字段；`UpdateWalStreamForCheckPoint{,WithBarrier}` 入参由 `const WalCheckPoint&` 改为 `const ControlWalStreamPageItemData&`。本期不考虑升降级场景。

## Capabilities

### New Capabilities
- `wal-checkpoint-history`: control file 中维护 N=3 槽的 checkpoint 环形历史链，封装 push / 按年龄索引 / 取最旧 plsn 等基本操作。
- `wal-recovery-checkpoint-fallback`: recovery 启动时对 checkpoint plsn 做 group header 校验并按从新到旧的顺序回退，确保不会从坏点起恢复。
- `wal-recycle-history-floor`: WAL 文件回收下界改为历史链中最旧的 `diskRecoveryPlsn`，保证所有可回退槽对应的 WAL 物理可达。
- `waldump-checkpoint-recovery-tools`: `waldump` 工具支持扫描合法 checkpoint 起点与离线改写 control file，作为全槽失败后的人工兜底链路。

### Modified Capabilities
<!-- 当前仓库 openspec/specs/ 为空，本次没有需要修改的既有 capability。 -->

## Impact

- **代码改动**：
  - `include/wal/dstore_wal_struct.h`：新增 `WalCheckPointHistory` 与 `WAL_CHECKPOINT_HISTORY_SLOTS=3`。
  - `include/control/dstore_control_walinfo.h` / `src/control/dstore_control_walinfo.cpp`：`ControlWalStreamPageItemData` 增字段；`Copy/Dump`、`UpdateWalStreamForCheckPoint{,WithBarrier}` 同步调整。
  - `include/control/dstore_control_file.h`、`include/buffer/dstore_checkpointer.h` / `src/buffer/dstore_checkpointer.cpp`：签名调整 + 新增 `GetWalCheckpointHistory(walId, &hist)`。
  - `include/wal/dstore_wal_recovery.h` / `src/wal/dstore_wal_recovery.cpp`：`InitRecoveryPlsn` 改签名 + 新增 `ProbeWalGroupAtPlsn` / `HandleAllSlotsCorrupted`。
  - `src/wal/dstore_wal_file_manager.cpp`：`GetRecyclablePlsn` 改取 N 槽最旧。
  - `tools/waldump/`：新增 `--scan-valid-checkpoint` / `--patch-control-checkpoint` 子命令。
- **运行时代价**：
  - WAL 占用上界增加 ≈ `(N-1) × checkpoint_interval × 平均写入速率`（N=3 时分钟级周期下多保留几百 MB 到几 GB）。
  - Recovery 启动多读 ≤ N 个 group header，相比整段 redo 可忽略。
  - Control page 容量需重新核对（`ControlWalStreamPageItemData` 增大 `2 × sizeof(WalCheckPoint) + 4` 字节），如不够再走"瘦身槽 / 拆 page"兜底。
- **不变式**：`lastWalCheckpoint == slots[newestSlotIdx]` 始终成立；探测路径只读不污染正式 redo 状态。
- **兼容性**：本期不考虑升降级；`lastWalCheckpoint` 字段保留并继续被双写，作为未来降级钩子。
