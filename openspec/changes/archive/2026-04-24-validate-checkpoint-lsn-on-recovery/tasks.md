## 1. 数据结构骨架

- [x] 1.1 在 `include/wal/dstore_wal_struct.h` 新增 `WAL_CHECKPOINT_HISTORY_SLOTS = 3` 与 `WalCheckPointHistory{ slotCount, newestSlotIdx, slots[N] }`，结构体外层用 `#pragma pack(push, 1)` 紧凑布局，去掉 `slots[]` 前的对齐空洞
- [x] 1.2 实现 `WalCheckPointHistory::Push(const WalCheckPoint&)`、`GetSlotIdxByAge(uint8 k)`、`GetOldestDiskRecoveryPlsn()`（含空槽返回 `INVALID_PLSN` 的兼容路径）
- [x] 1.3 在 `include/control/dstore_control_walinfo.h` 的 `ControlWalStreamPageItemData` 加 `WalCheckPointHistory checkpointHistory` 字段；保留 `lastWalCheckpoint` 作为兼容字段
- [x] 1.4 在 cmake 配置阶段静态断言 `sizeof(ControlWalStreamPageItemData) <= CONTROLFILE_PAGE_USABLE_SIZE`；超出则按 design.md Decision 1 记录的兜底（`WalCheckPointSlim` 或拆 page）讨论后再实现 *(改为 C++ `static_assert` 落在 `include/control/dstore_control_walinfo.h`，bound 用 `BLCKSZ - sizeof(ControlPageHeader)`)*

## 2. Control file 读写适配

- [x] 2.1 在 `src/control/dstore_control_walinfo.cpp` 同步 `Copy` / `Dump` / 序列化函数处理新字段
- [x] 2.2 把 `UpdateWalStreamForCheckPoint` / `UpdateWalStreamForCheckPointWithBarrier` 入参由 `const WalCheckPoint&` 改为 `const ControlWalStreamPageItemData&`，并同步 `include/control/dstore_control_file.h` 头声明
- [x] 2.3 在 `include/buffer/dstore_checkpointer.h` / `src/buffer/dstore_checkpointer.cpp` 新增 `GetWalCheckpointHistory(walId, WalCheckPointHistory &out)`，从内存中的 `walStreamInfo` 拷贝出最新历史
- [x] 2.4 反向兼容路径：读到 `slotCount == 0` 的旧 page 时不报错，让所有依赖路径走"空历史 → 退到 `lastWalCheckpoint`"的兜底分支

## 3. Checkpoint 写路径

- [x] 3.1 修改 `CheckpointMgr::DoCheckpoint`（`src/buffer/dstore_checkpointer.cpp`）：先 `hist.Push(checkPoint)`，再把 `walStreamInfo->lastWalCheckpoint = checkPoint` 同步设为最新槽，最后调用新签名的 `UpdateWalStreamForCheckPointWithBarrier(walId, ..., *walStreamInfo)`
- [x] 3.2 验证写入仍走单次 `PostGroup` 原子刷盘，没有引入新的并发临界区

## 4. Recovery 读路径

- [x] 4.1 在 `include/wal/dstore_wal_recovery.h` 把 `InitRecoveryPlsn` 入参改为 `const ControlWalStreamPageItemData *streamInfo`，新增 `ProbeWalGroupAtPlsn` / `HandleAllSlotsCorrupted` 声明（注意：跨 TU 调用的成员函数不要标 `inline`）
- [x] 4.2 实现 `ProbeWalGroupAtPlsn`：只读探测、独立临时 memory context + 读 buffer、复用 `WalGroupParser::ReadCurrent`，并对 `GetWalFileByPlsn(candidate) == nullptr` 直接返回 `DSTORE_FAIL`
- [x] 4.3 实现 `InitRecoveryPlsn` 的"快路径 + 慢路径"逻辑：先用 `lastWalCheckpoint` 探测；失败则按 `k = 1..slotCount-1` 顺序探测；首个成功的 slot 设置 `m_recoveryStartPlsn` / `m_diskRecoveryStartPlsn` 并 warn 日志记录回退步数
- [x] 4.4 实现 `HandleAllSlotsCorrupted`：dump 三槽内容、WAL 文件列表、当前 WAL 起止 plsn，调用 `ErrLog(DSTORE_PANIC, ...)` 退出
- [x] 4.5 校验：探测路径不修改 `m_redoReadBuffer` / `m_curRedoFinishedPlsn` / `WalRecordReader` 游标（grep 一遍 + UT 验证）

## 5. WAL 回收门槛

- [x] 5.1 修改 `WalFileManager::GetRecyclablePlsn`（`src/wal/dstore_wal_file_manager.cpp`）调用 `ckpMgr->GetWalCheckpointHistory(walId, hist)` 后返回 `hist.GetOldestDiskRecoveryPlsn()`
- [x] 5.2 兼容路径：`slotCount == 0` 时返回 `lastWalCheckpoint.diskRecoveryPlsn`，避免空历史卡住回收
- [x] 5.3 grep 全仓，确保没有第二条直接读 `lastWalCheckpoint.diskRecoveryPlsn` 决定回收下界的路径；如有，统一改走 `GetRecyclablePlsn` *(grep 已审；其余调用点是日志/redo 起点 lookup/dump 工具，不是回收下界)*

## 6. 单元测试（`tests/unittest/src/ut_wal/`）

- [x] 6.1 `ut_wal_checkpoint_history.cpp`：覆盖 `Push` / `GetSlotIdxByAge` / `GetOldestDiskRecoveryPlsn` / 空历史 / 非单调 plsn 的全部 spec scenario（已存在 9 个用例，按需补充）
- [x] 6.2 `UTWalRecovery_CheckpointHistory_NewestValid`：最新槽合法，验证走快路径、`m_recoveryStartPlsn == latest.diskRecoveryPlsn`、不进入 history 循环
- [x] 6.3 `UTWalRecovery_CheckpointHistory_NewestCorrupted`：最新槽位置注入坏 group（破坏 `groupLen` 或 CRC），验证选中次新槽
- [x] 6.4 `UTWalRecovery_CheckpointHistory_OnlyOldestValid`：最新+次新都坏，验证选中最老槽
- [x] 6.5 `UTWalRecovery_CheckpointHistory_AllCorrupted`：全坏，`EXPECT_DEATH` 断言 panic
- [x] 6.6 `UTWalRecovery_CheckpointHistory_Truncate`：fallback 后 `Truncate` 使用回退后槽的 `lastGroupEndPlsn`，断言合法 WAL 未被误删
- [x] 6.7 `UTWalFileManager_RecyclablePlsn_TakesOldest`：三槽 plsn 各异，`GetRecyclablePlsn` 返回最小者；空历史时返回 `lastWalCheckpoint.diskRecoveryPlsn`
- [x] 6.8 `UTControlWalInfo_BackwardCompat`：模拟旧版本 page（`slotCount == 0`），首次拉起后正确填充槽 0，且 `lastWalCheckpoint` 与 `slots[0]` 一致

## 7. 故障注入测试 *(本期延后，留待后续 change)*

> 延后理由：需要先在 `DstorePdbReplicaFI` 框架下铺新的注入点骨架，再配套多进程重启 fixture，与本期"数据结构 + 写读路径 + 工具"主线脱钩。当前已通过 §6 的 UT 矩阵覆盖了 `Push` / fallback / 全坏 panic / 兼容路径的代码逻辑；故障注入用于补全"真实 IO 部分写"语义，作为独立 change 推进更合适。
> 跟踪：在新 change 中重新立项。

- [ ] 7.1 在 `DstorePdbReplicaFI` 框架下新增注入点 `BEFORE_CHECKPOINT_WRITE_HISTORY`（写 control file 前篡改最新 `diskRecoveryPlsn` 为非法值）*(deferred)*
- [ ] 7.2 新增注入点 `AFTER_CHECKPOINT_WRITE_HISTORY`（写后立即 kill 模拟部分写）*(deferred)*
- [ ] 7.3 编写注入用例：触发后重新拉起进程，断言 redo 完成 + WAL 未被错误截断 + 日志包含"rolled back N checkpoint(s)" *(deferred)*

## 8. 集成测试 *(本期延后，留待后续 change)*

> 延后理由：依赖 30 分钟级别的 TPC-C 跑批 + 操作员手动用 `pagedump`/`waldump` 篡改 control file，无法纳入 CI；§9 的 `--patch-control-checkpoint` 工具已具备执行手段，剩余的是运行手册而非代码改动。计划在另一个偏运维/SRE 的 change 中以"恢复演练 playbook"形式落地。
> 跟踪：在新 change 中重新立项。

- [ ] 8.1 TPC-C 跑 30 分钟、触发 checkpoint ≥ 5 次后停库 *(deferred)*
- [ ] 8.2 用 `pagedump` / `waldump` 手动篡改 control file 最新槽 plsn 为乱值，重新拉起，断言业务数据完整、redo 走 fallback 路径 *(deferred)*
- [ ] 8.3 把"最新两槽"都改坏，重复一次，断言依然完整恢复 *(deferred)*

## 9. waldump 工具

- [x] 9.1 在 `tools/waldump/` 新增 `--scan-valid-checkpoint --wal-dir <dir> [--from-plsn <p>]` 子命令，复用 `WalGroupParser::ReadCurrent`，打印每个合法 group header 与最末合法 group 结束 plsn
- [x] 9.2 新增 `--patch-control-checkpoint --control-file <path> --wal-id <id> --plsn <p> [--dry-run]` 子命令：同时改写 `lastWalCheckpoint.diskRecoveryPlsn` 与 `checkpointHistory.slots[newestSlotIdx].diskRecoveryPlsn`，重算 page CRC 后写回；复用 `ControlFile::WriteSinglePage`
- [x] 9.3 安全约束：检测 `<control-file>.lock` 存在则拒绝；非 dry-run 前自动 `cp` 一份 `.bak` 备份
- [x] 9.4 工具自检 UT：dry-run 模式下 `mtime` 不变；live 模式下两个字段值一致 + page CRC 通过

## 10. 同步诊断与文档

- [x] 10.1 `src/wal/dstore_wal_diagnose.cpp` / `dstore_wal_dump*.cpp` 同步打印 N 槽信息（可选但建议） *(`ControlWalStreamPageItemData::Dump` 头部已打印；`dstore_wal_dump_file_reader.cpp` 同步打印 N 槽；`dstore_wal_diagnose.cpp` 不涉及 checkpoint dump)*
- [x] 10.2 在 `docs/design/wal_recovery_checkpoint_history.md` 末尾追加链接指回本 openspec change 与新增的 spec 文件，作为后续考古入口
- [x] 10.3 更新 `tests/unittest/CMakeLists.txt`（如新增 UT 文件需要纳入），并跑 `make run_dstore_ha_unittest` 确认全部用例通过 *(修复 `tests/CMakeLists.txt` 里用 `sh` 调 Bash 脚本导致的 target 失效后，`run_dstore_ha_unittest` 已通过：102 tests from 14 suites, 0 failed)*
