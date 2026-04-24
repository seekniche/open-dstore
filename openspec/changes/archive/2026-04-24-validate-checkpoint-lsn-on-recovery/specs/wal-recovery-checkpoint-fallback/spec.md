## ADDED Requirements

### Requirement: Probe-then-decision recovery start plsn

`WalRecovery::InitRecoveryPlsn` SHALL choose `m_recoveryStartPlsn` by probing candidate `diskRecoveryPlsn` values with `ProbeWalGroupAtPlsn`, which performs the same `ValidWalGroupLen` and `ValidWalGroup` (CRC) checks used during normal redo. The function SHALL NOT trust any `diskRecoveryPlsn` solely because it is non-`INVALID_PLSN`.

#### Scenario: Newest checkpoint passes probe — fast path returns immediately
- **WHEN** `lastWalCheckpoint.diskRecoveryPlsn` points at a location that parses as a valid atomic group
- **THEN** `m_recoveryStartPlsn` and `m_diskRecoveryStartPlsn` SHALL be set to that plsn and the function SHALL return without inspecting any history slot

#### Scenario: Newest checkpoint fails probe — slow path picks first valid older slot
- **WHEN** the newest checkpoint's plsn fails `ProbeWalGroupAtPlsn` but a strictly older slot (k ≥ 1 by age) passes
- **THEN** `m_recoveryStartPlsn` and `m_diskRecoveryStartPlsn` SHALL be set from that older slot
- **AND** a `DSTORE_WARNING` log SHALL record the rolled-back step count and the chosen slot

### Requirement: Fast-path-then-slow-path ordering

The fast path SHALL execute first using `lastWalCheckpoint`. The slow path (history-walk) SHALL execute only when the fast path's probe fails. The slow path SHALL iterate by age starting at `k = 1`, skipping `k = 0` because invariant I3 (`lastWalCheckpoint == slots[newestSlotIdx]`) guarantees it has already been probed.

#### Scenario: Healthy startup leaves new code path dormant
- **WHEN** the latest checkpoint is healthy
- **THEN** the fast path SHALL return without entering the history-walk loop
- **AND** the externally observable behavior of `InitRecoveryPlsn` SHALL be byte-identical to the pre-change implementation

#### Scenario: Slow path does not re-probe the newest slot
- **WHEN** the slow path is entered after a fast-path failure
- **THEN** the loop SHALL start at `k = 1` and SHALL NOT call `ProbeWalGroupAtPlsn` on `slots[newestSlotIdx]` again

### Requirement: All-slots-corrupted handling

If every populated slot fails its probe, `WalRecovery::HandleAllSlotsCorrupted` SHALL log all three slot contents, the corresponding WAL file list, and the currently visible WAL plsn range, then call `ErrLog(DSTORE_PANIC, ...)` to terminate the process. The system SHALL NOT silently fall through to "use the oldest slot anyway", because doing so could truncate even more legitimate WAL.

#### Scenario: All three populated slots fail probing
- **WHEN** `slotCount == 3` and every probe call returns `DSTORE_FAIL`
- **THEN** the process SHALL panic via `ErrLog(DSTORE_PANIC, ...)`
- **AND** the panic log SHALL include each slot's `diskRecoveryPlsn`, the WAL file list, and the WAL min/max plsn

#### Scenario: Backwards-compatible empty history with corrupted lastWalCheckpoint
- **WHEN** `slotCount == 0` and `lastWalCheckpoint`'s probe fails
- **THEN** the slow-path loop SHALL execute zero iterations and the system SHALL panic via `HandleAllSlotsCorrupted`

### Requirement: Probe is read-only

`WalRecovery::ProbeWalGroupAtPlsn` SHALL NOT mutate any state used by the actual redo path: `m_redoReadBuffer`, `m_curRedoFinishedPlsn`, the `WalRecordReader`'s position, or the `WalGroupParser` state owned by the running redo. The probe SHALL use a temporary memory context and read buffer that are released before the function returns. The probe SHALL also gracefully handle the case where the requested plsn lies outside any currently mounted WAL file (return `DSTORE_FAIL`, no log spam).

#### Scenario: Probe leaves redo state untouched
- **WHEN** `ProbeWalGroupAtPlsn` is called multiple times during recovery startup
- **THEN** `m_redoReadBuffer`, `m_curRedoFinishedPlsn`, and the `WalRecordReader` cursor SHALL remain at their pre-probe values

#### Scenario: Probe target is outside any mounted WAL file
- **WHEN** `GetWalFileByPlsn(candidate)` returns `nullptr`
- **THEN** `ProbeWalGroupAtPlsn` SHALL return `DSTORE_FAIL` immediately
- **AND** SHALL NOT attempt to instantiate a `WalGroupParser`
