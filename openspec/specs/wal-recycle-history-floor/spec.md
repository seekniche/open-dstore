# wal-recycle-history-floor Specification

## Purpose
TBD - created by archiving change validate-checkpoint-lsn-on-recovery. Update Purpose after archive.
## Requirements
### Requirement: WAL recycling floor uses oldest history slot

`WalFileManager::GetRecyclablePlsn` SHALL return the minimum `diskRecoveryPlsn` over all populated slots in the per-stream `WalCheckPointHistory` (i.e. `WalCheckPointHistory::GetOldestDiskRecoveryPlsn()`), instead of using only the latest checkpoint's `diskRecoveryPlsn`. This guarantees that every WAL byte covered by any retained history slot is still physically present on disk, which is the precondition for the recovery fallback path to be useful.

#### Scenario: Three distinct slot plsns — recycler picks the smallest
- **WHEN** the history holds three slots with `diskRecoveryPlsn` values 100, 500, 300 (in any positional order)
- **THEN** `GetRecyclablePlsn` SHALL return `100`

#### Scenario: After a wraparound, recycler honors the new oldest
- **WHEN** the history starts as (100, 200, 300), then 400 is pushed (overwriting slot 0 = 100)
- **THEN** `GetRecyclablePlsn` SHALL return `200`
- **WHEN** then 500 is pushed (overwriting slot 1 = 200)
- **THEN** `GetRecyclablePlsn` SHALL return `300`

### Requirement: Backwards-compatible recycling on empty history

When `slotCount == 0` (e.g. control file produced by an older binary that did not write history), `GetRecyclablePlsn` SHALL fall back to the legacy behavior of using `lastWalCheckpoint.diskRecoveryPlsn` so that recycling does not stall and so that the upgrade path remains safe until the first new-format checkpoint is written.

#### Scenario: Pre-upgrade control page with empty history
- **WHEN** `slotCount == 0` and `lastWalCheckpoint.diskRecoveryPlsn = 700`
- **THEN** `GetRecyclablePlsn` SHALL return `700`

#### Scenario: First post-upgrade checkpoint switches recycler to history mode
- **WHEN** the first checkpoint after upgrade has been pushed (now `slotCount == 1`)
- **THEN** `GetRecyclablePlsn` SHALL return `slots[0].diskRecoveryPlsn`, equal to `lastWalCheckpoint.diskRecoveryPlsn` by invariant I3

### Requirement: No path may recycle past the oldest history slot

No code path SHALL physically delete or truncate WAL bytes at a plsn smaller than `WalCheckPointHistory::GetOldestDiskRecoveryPlsn()` while that history slot is still populated. Any new recycling caller MUST go through `GetRecyclablePlsn` (or another helper that ultimately calls it) rather than computing its own floor.

#### Scenario: Bug-protection against direct recycling
- **WHEN** a new caller asks "how much WAL can I delete?"
- **THEN** the answer SHALL come from `GetRecyclablePlsn`, never from `lastWalCheckpoint.diskRecoveryPlsn` directly

