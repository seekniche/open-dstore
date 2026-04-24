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
 * patch_control_checkpoint.cpp
 *
 * ---------------------------------------------------------------------------------------
 */
#include "patch_control_checkpoint.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <securec.h>

#include "common/algorithm/dstore_checksum_impl.h"
#include "common/dstore_common_utils.h"
#include "control/dstore_control_file_page.h"
#include "control/dstore_control_struct.h"
#include "control/dstore_control_walinfo.h"
#include "page/dstore_page.h"
#include "wal/dstore_wal_struct.h"

namespace DSTORE {

namespace {

constexpr const char *PROG = "waldump";

bool LockFileExists(const char *controlFilePath, char *lockPathOut, size_t lockPathLen)
{
    int rc = snprintf_s(lockPathOut, lockPathLen, lockPathLen - 1, "%s.lock", controlFilePath);
    if (rc < 0) {
        return false;
    }
    struct stat st;
    return stat(lockPathOut, &st) == 0;
}

RetStatus CreateBackup(const char *controlFilePath)
{
    char bakPath[PATH_MAX];
    int rc = snprintf_s(bakPath, sizeof(bakPath), sizeof(bakPath) - 1, "%s.bak", controlFilePath);
    if (rc < 0) {
        (void)fprintf(stderr, "%s: failed to format backup path for \"%s\".\n", PROG, controlFilePath);
        return DSTORE_FAIL;
    }

    int srcFd = open(controlFilePath, O_RDONLY);
    if (srcFd < 0) {
        (void)fprintf(stderr, "%s: open(%s) for backup failed: %s\n", PROG, controlFilePath, strerror(errno));
        return DSTORE_FAIL;
    }
    int dstFd = open(bakPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (dstFd < 0) {
        (void)fprintf(stderr, "%s: open(%s) for backup write failed: %s\n", PROG, bakPath, strerror(errno));
        (void)close(srcFd);
        return DSTORE_FAIL;
    }

    char buf[BLCKSZ];
    ssize_t n;
    RetStatus result = DSTORE_SUCC;
    while ((n = read(srcFd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(dstFd, buf + written, n - written);
            if (w < 0) {
                (void)fprintf(stderr, "%s: write to backup %s failed: %s\n", PROG, bakPath, strerror(errno));
                result = DSTORE_FAIL;
                break;
            }
            written += w;
        }
        if (result != DSTORE_SUCC) {
            break;
        }
    }
    if (n < 0) {
        (void)fprintf(stderr, "%s: read from %s during backup failed: %s\n", PROG, controlFilePath, strerror(errno));
        result = DSTORE_FAIL;
    }
    if (fsync(dstFd) != 0) {
        (void)fprintf(stderr, "%s: fsync backup %s failed: %s\n", PROG, bakPath, strerror(errno));
        result = DSTORE_FAIL;
    }
    (void)close(srcFd);
    (void)close(dstFd);
    if (result == DSTORE_SUCC) {
        (void)fprintf(stdout, "%s: backup written to %s\n", PROG, bakPath);
    }
    return result;
}

bool IsWalStreamDataPage(const ControlBasePage *page)
{
    return page->m_pageHeader.m_magic == CONTROL_DATA_MAGIC_NUMBER &&
           page->m_pageHeader.m_pageType == static_cast<uint16>(CONTROL_WAL_STREAM_DATAPAGE_TYPE);
}

void RecomputePageCrc(uint8 *pageBuf)
{
    ControlBasePage *page = reinterpret_cast<ControlBasePage *>(pageBuf);
    page->m_pageHeader.m_checksum = 0;
    page->m_pageHeader.m_checksum = CompChecksum(pageBuf, BLCKSZ, CHECKSUM_CRC);
}

/*
 * Walk the items packed into one WALSTREAM data page. Returns true when the
 * matching walId is found; on match, mutates both compatibility-mirror and
 * history-newest-slot fields, then prints a before/after diff.
 */
bool TryPatchPage(uint8 *pageBuf, BlockNumber blockNumber, const PatchControlCheckpointArgs &args, bool &corrupt)
{
    corrupt = false;
    ControlBasePage *page = reinterpret_cast<ControlBasePage *>(pageBuf);
    if (!IsWalStreamDataPage(page)) {
        return false;
    }
    if (page->m_pageHeader.m_writeOffset < page->m_pageHeader.m_dataOffset ||
        page->m_pageHeader.m_writeOffset > sizeof(page->m_data)) {
        (void)fprintf(stderr, "%s: page %u writeOffset=%u out of range; skipping.\n",
            PROG, blockNumber, page->m_pageHeader.m_writeOffset);
        corrupt = true;
        return false;
    }
    char *itemStart = page->m_data + page->m_pageHeader.m_dataOffset;
    char *itemEnd = page->m_data + page->m_pageHeader.m_writeOffset;
    constexpr size_t itemSize = sizeof(ControlWalStreamPageItemData);
    while (itemStart + itemSize <= itemEnd) {
        ControlWalStreamPageItemData *item = reinterpret_cast<ControlWalStreamPageItemData *>(itemStart);
        if (item->walId == args.walId) {
            uint64 oldLast = item->lastWalCheckpoint.diskRecoveryPlsn;
            uint64 oldHist = INVALID_PLSN;
            uint8 newestIdx = item->checkpointHistory.newestSlotIdx;
            bool histPopulated = item->checkpointHistory.slotCount > 0 &&
                                 newestIdx < WAL_CHECKPOINT_HISTORY_SLOTS;
            if (histPopulated) {
                oldHist = item->checkpointHistory.slots[newestIdx].diskRecoveryPlsn;
            }
            (void)fprintf(stdout, "%s: page=%u walId=%lu match found.\n", PROG, blockNumber, args.walId);
            (void)fprintf(stdout, "  lastWalCheckpoint.diskRecoveryPlsn: %lu -> %lu\n", oldLast, args.plsn);
            if (histPopulated) {
                (void)fprintf(stdout, "  checkpointHistory.slots[%hhu].diskRecoveryPlsn: %lu -> %lu\n",
                    newestIdx, oldHist, args.plsn);
            } else {
                (void)fprintf(stdout, "  checkpointHistory empty (slotCount=%hhu); only mirror field is patched.\n",
                    item->checkpointHistory.slotCount);
            }
            if (!args.dryRun) {
                item->lastWalCheckpoint.diskRecoveryPlsn = args.plsn;
                if (histPopulated) {
                    item->checkpointHistory.slots[newestIdx].diskRecoveryPlsn = args.plsn;
                }
            }
            return true;
        }
        itemStart += itemSize;
    }
    return false;
}

}  // namespace

RetStatus RunPatchControlCheckpoint(const PatchControlCheckpointArgs &args)
{
    if (args.controlFilePath == nullptr || args.controlFilePath[0] == '\0') {
        (void)fprintf(stderr, "%s: --control-file is required.\n", PROG);
        return DSTORE_FAIL;
    }

    char lockPath[PATH_MAX];
    if (LockFileExists(args.controlFilePath, lockPath, sizeof(lockPath))) {
        (void)fprintf(stderr,
            "%s: refuse to patch — lock file exists at %s. Stop the dstore process first, "
            "or remove the lock file if you have verified no process is alive.\n",
            PROG, lockPath);
        return DSTORE_FAIL;
    }

    if (!args.dryRun) {
        if (STORAGE_FUNC_FAIL(CreateBackup(args.controlFilePath))) {
            return DSTORE_FAIL;
        }
    }

    int flags = args.dryRun ? O_RDONLY : O_RDWR;
    int fd = open(args.controlFilePath, flags);
    if (fd < 0) {
        (void)fprintf(stderr, "%s: open(%s) failed: %s\n", PROG, args.controlFilePath, strerror(errno));
        return DSTORE_FAIL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        (void)fprintf(stderr, "%s: fstat(%s) failed: %s\n", PROG, args.controlFilePath, strerror(errno));
        (void)close(fd);
        return DSTORE_FAIL;
    }
    BlockNumber pageCount = static_cast<BlockNumber>(st.st_size / BLCKSZ);
    BlockNumber endPage = static_cast<BlockNumber>(CONTROLFILE_PAGEMAP_WALSTREAM_MAX);
    if (pageCount < endPage) {
        endPage = pageCount;
    }

    uint8 pageBuf[BLCKSZ];
    bool found = false;
    RetStatus result = DSTORE_SUCC;

    for (BlockNumber block = static_cast<BlockNumber>(CONTROLFILE_PAGEMAP_WALSTREAM_START);
         block < endPage; ++block) {
        off_t offset = static_cast<off_t>(block) * BLCKSZ;
        ssize_t got = pread(fd, pageBuf, BLCKSZ, offset);
        if (got != BLCKSZ) {
            (void)fprintf(stderr, "%s: pread page %u failed (got=%zd, errno=%d).\n",
                PROG, block, got, errno);
            result = DSTORE_FAIL;
            break;
        }
        bool corrupt = false;
        if (!TryPatchPage(pageBuf, block, args, corrupt)) {
            continue;
        }
        found = true;
        if (args.dryRun) {
            break;
        }
        RecomputePageCrc(pageBuf);
        ssize_t put = pwrite(fd, pageBuf, BLCKSZ, offset);
        if (put != BLCKSZ) {
            (void)fprintf(stderr, "%s: pwrite page %u failed (put=%zd, errno=%d).\n",
                PROG, block, put, errno);
            result = DSTORE_FAIL;
            break;
        }
        if (fsync(fd) != 0) {
            (void)fprintf(stderr, "%s: fsync %s failed: %s\n", PROG, args.controlFilePath, strerror(errno));
            result = DSTORE_FAIL;
            break;
        }
        break;
    }

    (void)close(fd);

    if (!found && result == DSTORE_SUCC) {
        (void)fprintf(stderr, "%s: walId=%lu not found in any WALSTREAM data page of %s.\n",
            PROG, args.walId, args.controlFilePath);
        return DSTORE_FAIL;
    }
    if (result == DSTORE_SUCC) {
        if (args.dryRun) {
            (void)fprintf(stdout, "%s: dry-run complete; control file unchanged.\n", PROG);
        } else {
            (void)fprintf(stdout, "%s: patch complete. Note: dstore double-writes control files; "
                "if a sibling file (e.g. database_control_2) exists, run this command against it too "
                "before restarting.\n", PROG);
        }
    }
    return result;
}

}  // namespace DSTORE
