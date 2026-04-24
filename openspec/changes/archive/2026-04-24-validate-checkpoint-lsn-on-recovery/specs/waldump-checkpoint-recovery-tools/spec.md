## ADDED Requirements

### Requirement: `--scan-valid-checkpoint` lists usable WAL group starts

`waldump --scan-valid-checkpoint --wal-dir <dir> [--from-plsn <p>]` SHALL sequentially scan the given WAL directory starting at `--from-plsn` (or the earliest WAL file when omitted) and print every group header position that passes both `ValidWalGroupLen` and `ValidWalGroup` (CRC) — i.e. the same checks used by the runtime `ProbeWalGroupAtPlsn`. The command SHALL also print the end-plsn of the last valid group, which is the recommended candidate for `--patch-control-checkpoint`. The command SHALL be read-only.

#### Scenario: Healthy WAL directory dump
- **WHEN** the operator runs `waldump --scan-valid-checkpoint --wal-dir <dir>`
- **THEN** the tool SHALL print one line per valid group with `(plsn, groupLen, crc)`
- **AND** SHALL print a final line `last_valid_group_end_plsn = <plsn>`
- **AND** SHALL exit 0 without modifying any file

#### Scenario: Optional `--from-plsn` narrows scan range
- **WHEN** the operator passes `--from-plsn <p>`
- **THEN** the scan SHALL start at `<p>` instead of the earliest WAL file
- **AND** results before `<p>` SHALL be omitted

#### Scenario: Implementation reuses runtime probe code
- **WHEN** the scan parses a candidate group
- **THEN** it SHALL go through the same `WalGroupParser::ReadCurrent` path as `ProbeWalGroupAtPlsn`, so a checkpoint plsn that is rejected by the tool is also rejected by the runtime, and vice versa

### Requirement: `--patch-control-checkpoint` rewrites checkpoint plsn

`waldump --patch-control-checkpoint --control-file <path> --wal-id <id> --plsn <p> [--dry-run]` SHALL load the given control file, locate the entry for `--wal-id`, and overwrite **both** `lastWalCheckpoint.diskRecoveryPlsn` AND `checkpointHistory.slots[newestSlotIdx].diskRecoveryPlsn` with `--plsn`. The control page CRC SHALL be recomputed before the page is written back. With `--dry-run` the tool SHALL only print a diff and exit 0 without writing.

#### Scenario: Dry run prints diff and writes nothing
- **WHEN** the operator passes `--dry-run`
- **THEN** the tool SHALL print before/after values for both fields and exit 0
- **AND** the control file `mtime` SHALL be unchanged

#### Scenario: Live patch updates both fields and CRC
- **WHEN** the operator runs without `--dry-run`
- **THEN** both `lastWalCheckpoint.diskRecoveryPlsn` and `checkpointHistory.slots[newestSlotIdx].diskRecoveryPlsn` SHALL equal `<p>` after the call returns
- **AND** the page CRC SHALL pass on a subsequent re-read

### Requirement: Patch tool refuses to corrupt a live control file

`--patch-control-checkpoint` SHALL refuse to run when the dstore process holding the control file is alive (detected via the existing `.lock` file convention) and SHALL automatically write a `.bak` sibling copy of the control file before any in-place modification.

#### Scenario: Live process detected — refuse to patch
- **WHEN** a `<control-file>.lock` file exists or appears held
- **THEN** the tool SHALL exit non-zero with an error message naming the lock file
- **AND** SHALL NOT modify the control file

#### Scenario: Backup is created before patch
- **WHEN** a non-dry-run patch is allowed to proceed
- **THEN** a copy of the original control file SHALL exist at `<control-file>.bak` after the patch returns
- **AND** the `.bak` content SHALL match the pre-patch bytes

### Requirement: End-to-end manual recovery flow

The two subcommands together SHALL support the documented manual recovery sequence: panic → operator reads "earliest visible WAL plsn" from logs → `waldump --scan-valid-checkpoint` finds a valid group start → `waldump --patch-control-checkpoint --dry-run` previews the change → live patch → restart dstore. No additional offline tooling SHALL be required.

#### Scenario: Operator-only recovery without rebuilding the binary
- **WHEN** the runtime panics with `HandleAllSlotsCorrupted`
- **THEN** the operator SHALL be able to complete the recovery flow above using only the shipped `waldump` binary plus the panic log contents
