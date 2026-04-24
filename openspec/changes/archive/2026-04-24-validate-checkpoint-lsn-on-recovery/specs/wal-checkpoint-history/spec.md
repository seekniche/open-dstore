## ADDED Requirements

### Requirement: Fixed-size ring buffer of checkpoint slots

The system SHALL maintain a checkpoint history per WAL stream as a ring buffer of exactly `WAL_CHECKPOINT_HISTORY_SLOTS = 3` `WalCheckPoint` slots, with `slotCount` (number of populated slots) and `newestSlotIdx` (index of the most recently written slot). `WAL_CHECKPOINT_HISTORY_SLOTS` MUST be a compile-time constant and MUST NOT be exposed as a GUC.

#### Scenario: Empty history reports zero populated slots
- **WHEN** a `WalCheckPointHistory` is value-initialized
- **THEN** `slotCount` SHALL equal `0` and `GetOldestDiskRecoveryPlsn()` SHALL return `INVALID_PLSN` (i.e. `0`)

#### Scenario: Push fills slots in increasing index order until saturation
- **WHEN** the caller pushes 3 distinct checkpoints into a fresh history
- **THEN** `slots[0]`, `slots[1]`, `slots[2]` SHALL hold the three pushed checkpoints in push order
- **AND** `newestSlotIdx` SHALL advance to `0`, `1`, `2` respectively
- **AND** `slotCount` SHALL grow `1 → 2 → 3` and saturate at `3`

#### Scenario: Push past saturation overwrites the positionally-oldest slot
- **WHEN** a 4th checkpoint is pushed into an already-full ring (`newestSlotIdx == 2`)
- **THEN** slot `0` SHALL be overwritten with the new checkpoint
- **AND** `newestSlotIdx` SHALL become `0`
- **AND** `slotCount` SHALL remain `3`

### Requirement: Index lookup by age

`WalCheckPointHistory::GetSlotIdxByAge(uint8 k)` SHALL return the slot index of the k-th most recent checkpoint, where `k = 0` is the newest and `k = slotCount - 1` is the oldest. The function SHALL compute `(newestSlotIdx + WAL_CHECKPOINT_HISTORY_SLOTS - k) % WAL_CHECKPOINT_HISTORY_SLOTS` so that callers never write the modular index by hand.

#### Scenario: Age lookup before wrap returns push-order positions
- **WHEN** three checkpoints (100, 200, 300) have been pushed and no wrap has occurred (`newestSlotIdx == 2`)
- **THEN** `GetSlotIdxByAge(0)` SHALL return `2` (newest, plsn 300)
- **AND** `GetSlotIdxByAge(1)` SHALL return `1` (plsn 200)
- **AND** `GetSlotIdxByAge(2)` SHALL return `0` (oldest, plsn 100)

#### Scenario: Age lookup after wrap correctly walks the ring
- **WHEN** five checkpoints (100, 200, 300, 400, 500) have been pushed (`newestSlotIdx == 1`, slots = [400, 500, 300])
- **THEN** `GetSlotIdxByAge(0)` SHALL return `1` (newest, plsn 500)
- **AND** `GetSlotIdxByAge(1)` SHALL return `0` (plsn 400)
- **AND** `GetSlotIdxByAge(2)` SHALL return `2` (oldest, plsn 300)

### Requirement: Oldest disk-recovery plsn across populated slots

`GetOldestDiskRecoveryPlsn()` SHALL return the numerical minimum of `diskRecoveryPlsn` over the `slotCount` populated slots, ignoring uninitialized tail slots. When `slotCount == 0`, the function SHALL return `INVALID_PLSN`.

#### Scenario: Partially-populated history returns the smallest seen plsn
- **WHEN** the history contains exactly `{700}` and then `{700, 800}` after a second push
- **THEN** `GetOldestDiskRecoveryPlsn()` SHALL return `700` in both states

#### Scenario: Saturated and wrapped history returns minimum of remaining slots
- **WHEN** the history holds (100, 200, 300), then is pushed with 400 (overwrites slot 0 = 100)
- **THEN** `GetOldestDiskRecoveryPlsn()` SHALL return `200`
- **WHEN** then pushed with 500 (overwrites slot 1 = 200)
- **THEN** `GetOldestDiskRecoveryPlsn()` SHALL return `300`

#### Scenario: Non-monotonic checkpoint plsn still yields numeric minimum
- **WHEN** checkpoints are pushed out of plsn order, e.g. 500 then 100 then 300
- **THEN** `GetOldestDiskRecoveryPlsn()` SHALL return `100`, not the positionally-oldest slot

### Requirement: Backwards-compatible empty history

When a control page produced by an older binary (without the history field) is loaded, the resulting `WalCheckPointHistory` SHALL satisfy `slotCount == 0`, and `GetOldestDiskRecoveryPlsn()` SHALL return `INVALID_PLSN` so that callers can fall back to legacy `lastWalCheckpoint`-based behavior.

#### Scenario: Empty history reports invalid plsn on read
- **WHEN** a freshly value-initialized `WalCheckPointHistory` is queried
- **THEN** `slotCount` SHALL be `0`
- **AND** `GetOldestDiskRecoveryPlsn()` SHALL be `INVALID_PLSN` (`0`)

### Requirement: Compatibility field invariant `lastWalCheckpoint == slots[newestSlotIdx]`

The system SHALL keep `ControlWalStreamPageItemData::lastWalCheckpoint` byte-equal to `checkpointHistory.slots[checkpointHistory.newestSlotIdx]` whenever a checkpoint is written, so existing readers (`WalDiagnose`, backup/restore, legacy UTs) continue to work and a future downgrade can keep reading the legacy field.

#### Scenario: After every checkpoint push the compat field equals the newest slot
- **WHEN** a new `WalCheckPoint` is pushed into the history during `CheckpointMgr::DoCheckpoint`
- **THEN** `walStreamInfo.lastWalCheckpoint` SHALL equal `walStreamInfo.checkpointHistory.slots[checkpointHistory.newestSlotIdx]` immediately before `UpdateWalStreamForCheckPointWithBarrier` is invoked
