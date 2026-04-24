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
 * dstore_control_walinfo.h
 *  Record walstream info in the control file..
 *
 *
 * IDENTIFICATION
 *        dstore/include/control/dstore_control_walinfo.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef DSTORE_CONTROL_WALINFO_H
#define DSTORE_CONTROL_WALINFO_H
#include "dstore_control_file_lock.h"
#include "wal/dstore_wal_struct.h"
#include "common/algorithm/dstore_string_info.h"
#include "control/dstore_control_file_mgr.h"
#include "common/dstore_common_utils.h"
#include "control/dstore_control_group.h"
namespace DSTORE {
constexpr uint32 MAX_WAL_STREAM_COUNT = 1024;
struct ControlWalInfoMeta {
    uint32 m_version;
    uint32 m_pad;
    uint64 m_maxWalId;
    static void Dump(void *item, StringInfoData &dumpInfo)
    {
        ControlWalInfoMeta *data = static_cast<ControlWalInfoMeta *>(item);
        dumpInfo.append("version %u, maxWalId %lu\n", data->m_version, data->m_maxWalId);
    }
} PACKED;

struct ControlWalStreamPageItemData {
    uint32 version;
    uint8 streamState;               /* refer to WalStreamState */
    WalId walId;                     /* each log stream has one wal id on each running node */
    uint32 walBlockSize;             /* wal seg file block size */
    uint32 walFileSize;              /* wal segment file size */
    uint64 walMinRecoveryPlsn;       /* wal stream min recovery point */
    uint64 archivePlsn;              /* wal stream archive point */
    uint64 lastCheckpointPLsn;       /* the start location of last checkpoint Wal record */
    /*
     * lastWalCheckpoint mirrors checkpointHistory.slots[checkpointHistory.newestSlotIdx] and is kept
     * for backward compatibility with code paths (diagnostics / legacy readers) that still consume it.
     */
    WalCheckPoint lastWalCheckpoint;
    WalCheckPointHistory checkpointHistory; /* recent N checkpoint slots, recovery-time fallback */
    FileParameter createFilePara;    /* the parameter of creating walFile for backup using */
    uint16 initWalFileCount;         /* default wal file count, not updated in real time */
    WalBarrier barrier;              /* barrier info include csn, end plsn and sync mode */

    static void Copy(ControlWalStreamPageItemData &destPage, const ControlWalStreamPageItemData &srcPage)
    {
        destPage.version = srcPage.version;
        destPage.streamState = srcPage.streamState;
        destPage.walId = srcPage.walId;
        destPage.walBlockSize = srcPage.walBlockSize;
        destPage.walFileSize = srcPage.walFileSize;
        destPage.walMinRecoveryPlsn = srcPage.walMinRecoveryPlsn;
        destPage.archivePlsn = srcPage.archivePlsn;
        destPage.lastCheckpointPLsn = srcPage.lastCheckpointPLsn;
        destPage.lastWalCheckpoint.time = srcPage.lastWalCheckpoint.time;
        destPage.lastWalCheckpoint.diskRecoveryPlsn = srcPage.lastWalCheckpoint.diskRecoveryPlsn;
        destPage.lastWalCheckpoint.memoryCheckpoint = srcPage.lastWalCheckpoint.memoryCheckpoint;
        errno_t rc = memcpy_s(&destPage.checkpointHistory, sizeof(WalCheckPointHistory),
                              &srcPage.checkpointHistory, sizeof(WalCheckPointHistory));
        storage_securec_check(rc, "\0", "\0");
        rc = memcpy_s(&destPage.createFilePara, sizeof(FileParameter), &srcPage.createFilePara,
                      sizeof(FileParameter));
        storage_securec_check(rc, "\0", "\0");
        destPage.initWalFileCount = srcPage.initWalFileCount;
        destPage.barrier.barrierCsn = srcPage.barrier.barrierCsn;
        destPage.barrier.barrierEndPlsn = srcPage.barrier.barrierEndPlsn;
        destPage.barrier.barrierSyncMode = srcPage.barrier.barrierSyncMode;
    }

    static void Dump(void *item, StringInfoData &dumpInfo)
    {
        ControlWalStreamPageItemData *data = static_cast<ControlWalStreamPageItemData *>(item);
        dumpInfo.append(
            "streamState %hhu, walId %lu, walBlockSize %u, walFileSize %u, walMinRecoveryPlsn %lu"
            "archivePlsn %lu, lastCheckpointPLsn %lu, lastWalCheckpoint.time %ld, "
            "lastWalCheckpoint.diskRecoveryPlsn %lu, lastWalCheckpoint.memRecoveryPlsn %lu, "
            "storeSpaceName %s streamId %u flag %hu subtype %d rangeSize %u maxSize %lu recycleTtl %lu mode %d"
            "isReplayWrite %d initWalFileCount %d, "
            "barrierCsn %lu barrierEndPlsn %lu barrierSyncMode %d version %u, "
            "checkpointHistory.slotCount %hhu newestSlotIdx %hhu.\n",
            data->streamState, data->walId, data->walBlockSize, data->walFileSize, data->walMinRecoveryPlsn,
            data->archivePlsn, data->lastCheckpointPLsn, data->lastWalCheckpoint.time,
            data->lastWalCheckpoint.diskRecoveryPlsn, data->lastWalCheckpoint.memoryCheckpoint.memRecoveryPlsn,
            data->createFilePara.storeSpaceName, data->createFilePara.streamId, data->createFilePara.flag,
            data->createFilePara.fileSubType, data->createFilePara.rangeSize, data->createFilePara.maxSize,
            data->createFilePara.recycleTtl, data->createFilePara.mode, data->createFilePara.isReplayWrite,
            data->initWalFileCount, data->barrier.barrierCsn, data->barrier.barrierEndPlsn,
            static_cast<int32>(data->barrier.barrierSyncMode), data->version,
            data->checkpointHistory.slotCount, data->checkpointHistory.newestSlotIdx);
        for (uint8 i = 0; i < data->checkpointHistory.slotCount; ++i) {
            dumpInfo.append(
                "  checkpointHistory.slots[%hhu] time %ld diskRecoveryPlsn %lu memRecoveryPlsn %lu.\n", i,
                data->checkpointHistory.slots[i].time,
                data->checkpointHistory.slots[i].diskRecoveryPlsn,
                data->checkpointHistory.slots[i].memoryCheckpoint.memRecoveryPlsn);
        }
    }
};

/*
 * One ControlWalStreamPageItemData must fit in a single control page after
 * accounting for the page header. If a future change pushes the struct beyond
 * this limit, see openspec change validate-checkpoint-lsn-on-recovery
 * design Decision 1 for fallback options (WalCheckPointSlim or page split).
 */
static_assert(sizeof(ControlWalStreamPageItemData) <= (BLCKSZ - sizeof(ControlPageHeader)),
              "ControlWalStreamPageItemData no longer fits in a single control page");

using VnodeControlWalStreamPageItemDatas = ControlWalStreamPageItemData **;
using WalStreamItemFilter = bool (*)(const ControlWalStreamPageItemData &walStreamItem);
class ControlWalInfo : public ControlGroup {
public:
    ControlWalInfo(ControlFileMgr *controlFileMgr, DstoreMemoryContext memCtx, PdbId pdbId)
        : ControlGroup(controlFileMgr, memCtx, CONTROL_GROUP_TYPE_WALSTREAM, CONTROLFILE_PAGEMAP_WALSTREAM_META, pdbId)
    {}
    ~ControlWalInfo() {}
    DISALLOW_COPY_AND_MOVE(ControlWalInfo);
    RetStatus Init(DeployType deployType)
    {
        if (unlikely(ControlGroup::Init(deployType) == DSTORE_SUCC)) {
            m_isInitialized = true;
            return DSTORE_SUCC;
        }
        return DSTORE_FAIL;
    }
    RetStatus Create();
    void Reload()
    {}
    /**
     * Add WalStream element.
     *
     * @param streamInfo is target element info
     * @return add result
     */
    RetStatus AddWalStream(const ControlWalStreamPageItemData &streamInfo);
    /**
     * Update WalStream element.
     *
     * NOTE: if the WalStream element is not exist, then insert a new one.
     *
     * @param streamInfo is target element info
     * @return update result
     */
    RetStatus UpdateWalStream(const ControlWalStreamPageItemData &streamInfo);
    /**
     * Update WalStream element for checkpoint usage.
     *
     * NOTE: This function only update partial elements for checkpoint usage. Checkpoint invokes rather than
     * UpdateWalStream to avoid concurrent updating problems.
     *
     * The fields written to the control page are: lastCheckpointPLsn, lastWalCheckpoint and checkpointHistory.
     *
     * @param streamInfo carries walId plus the new checkpoint state to persist
     * @return update result
     */
    RetStatus UpdateWalStreamForCheckPoint(const ControlWalStreamPageItemData &streamInfo);
    /**
     * Update WalStream element for checkpoint usage with barrier.
     *
     * NOTE: This function only update partial elements for checkpoint usage. Checkpoint invokes rather than
     * UpdateWalStream to avoid concurrent updating problems.
     *
     * The fields written to the control page are: lastCheckpointPLsn, lastWalCheckpoint, checkpointHistory and
     * barrier.
     *
     * @param streamInfo carries walId plus the new checkpoint state and barrier to persist
     * @return update result
     */
    RetStatus UpdateWalStreamForCheckPointWithBarrier(const ControlWalStreamPageItemData &streamInfo);
    /**
     * Delete WalStream element.
     *
     * NOTE: if the element is not exist, then return DSTORE_FAIL.
     *
     * @param walId is target WalStream wal Id
     * @return delete result
     */
    RetStatus DeleteWalStream(WalId walId);
    /**
     * Get all the WAL streams in the control file. Note the caller has to release the memory of walIdArray.
     *
     * @param[out] walIdArray the array of WAL IDs to be returned
     * @param[in] filter is user-defined filter
     * @return the number of active WAL streams
     */
    uint32 GetAllWalStreams(WalId **walIdArray, WalStreamItemFilter filter = nullptr);
    /**
     * Get all the WAL streams in the control file without acquiring control file lock.
     * The control file lock is expected to be held by the caller separately.
     *
     * @param[out] walIdArray the array of WAL IDs to be returned
     * @param[in] filter is user-defined filter
     * @return the number of active WAL streams
     */
    uint32 GetAllWalStreamsWithoutLock(WalId **walIdArray, WalStreamItemFilter filter);
    /**
     * Get target WalStream info.
     *
     * @param walId is target WalStream ID
     * @param[out] streamInfo is pointer to fetched stream info, which is constructed and freed by caller,
     * @return get result
     */
    RetStatus GetWalStreamInfo(WalId walId, ControlWalStreamPageItemData **streamInfo);
    /**
     * create and allocate one wal stream
     *
     * @param[out] walStream is alloacted wal stream
     * @return DSTORE_SUCC if create successfully, otherwise return DSTORE_FAIL.
     */
    RetStatus CreateAndAllocateOneWalStream(ControlWalStreamPageItemData **walStreamItemData, WalId walId);
    BlockNumber GetFirstWalStreamBlockNumber()
    {
        return static_cast<BlockNumber>(ControlFilePageMap::CONTROLFILE_PAGEMAP_WALSTREAM_START);
    }
    RetStatus UpdateWalStreamState(WalId walId, const uint8 state);
    inline void SetWalStreamReusable(bool canWalStreamReusable)
    {
        m_canWalStreamReusable.store(canWalStreamReusable, std::memory_order_release);
    }

    inline bool CanWalStreamReusable()
    {
        return m_canWalStreamReusable.load(std::memory_order_acquire);
    }
    RetStatus UpdateWalStreamInternal(const ControlWalStreamPageItemData &streamInfo);

    RetStatus DeleteWalStreamInfo(WalId walId);

    inline RetStatus AddWalStreamInfo(const ControlWalStreamPageItemData &streamInfo);

    RetStatus GetWalStreamInfoBaseOnWalId(WalId walId, ControlWalStreamPageItemData **streamInfo);

    RetStatus GenerateWalStreamNoLock(ControlWalStreamPageItemData **walStreamPageItemData, uint8_t initState,
        WalId walId);
    void FreeWalStreamsInfoArray(ControlWalStreamPageItemData **walStreamInfoArray, uint32 streamCount) const;
    void FreeWalStreamsInfo(ControlWalStreamPageItemData *walStreamInfo) const;

private:
    std::atomic<bool> m_canWalStreamReusable;
};
}  // namespace DSTORE
#endif  // DSTORE_CONTROL_WALINFO_H