/*
 * Copyright (C) 2026 Huawei Technologies Co.,Ltd.
 *
 * dstore is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * dstore is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. if not, see <https://www.gnu.org/licenses/>.
 *
 * ---------------------------------------------------------------------------------------
 *
 * patch_control_checkpoint.h
 *
 * Implements `waldump --patch-control-checkpoint`. See openspec change
 * validate-checkpoint-lsn-on-recovery, capability waldump-checkpoint-recovery-tools.
 * ---------------------------------------------------------------------------------------
 */
#ifndef DSTORE_WALDUMP_PATCH_CONTROL_CHECKPOINT_H
#define DSTORE_WALDUMP_PATCH_CONTROL_CHECKPOINT_H

#include "common/dstore_datatype.h"

namespace DSTORE {

struct PatchControlCheckpointArgs {
    const char *controlFilePath;
    uint64 walId;
    uint64 plsn;
    bool dryRun;
};

/*
 * Implements the --patch-control-checkpoint subcommand. Returns DSTORE_SUCC on
 * success (including dry-run). The caller is expected to translate non-success
 * into a non-zero process exit code.
 */
RetStatus RunPatchControlCheckpoint(const PatchControlCheckpointArgs &args);

}  // namespace DSTORE
#endif
