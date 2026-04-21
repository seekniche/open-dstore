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
 */

#include "buffer/dstore_checkpointer.h"
#include "common/log/dstore_log.h"
#include "port/dstore_port.h"
#include "lock/dstore_lwlock.h"
#include "framework/dstore_thread.h"
#include "buffer/dstore_buf_mgr.h"
#include "buffer/dstore_bg_page_writer_mgr.h"
#include "buffer/dstore_bg_disk_page_writer.h"

namespace DSTORE {

void CheckpointMgr::CheckpointerMain()
{
    AutoMemCxtSwitch autoSwitch{m_checkpointContext};

    while (true) {
LOOP:
        Timestamp now;
        Timestamp elapsedTime;

        /* exit when m_shutdownRequested is set */
        if (m_shutdownRequested.load(std::memory_order_acquire)) {
            break;
        }
        /*
         * step1: check the flag or time to decide if start a checkpoint for a WalStream
         *
         * We loop through all the WalStream, and see if the flag is set by other thread.
         * If the flag of the WalStream is set, then we start to checkpoint this WalStream.
         *
         * Besides the flag, we also calculate the elapsed time since last checkpoint of this WalStream.
         * If the elapsed time exceed the CheckpointTimeout, we also checkpoint this WalStream.
         *
         * If the flag of the WalStream is zero and the elapsed time don't exceed the CheckpointTimeout,
         * we skip to checkpoint this WalStream.
         */
        StoragePdb *storagePdb = g_storageInstance->GetPdb(m_pdbId);
        if (storagePdb == nullptr) {
            ErrLog(DSTORE_PANIC, MODULE_BUFMGR, ErrMsg("storagePdb is nullptr."));
        }
        WalStreamManager *walStreamMgr = m_walMgr->GetWalStreamManager();
        dlist_mutable_iter iter = {};
        WalStreamNode *walStreamNode = nullptr;
        WalStream *walStream = nullptr;
        WalStreamFilter filter = WalStream::IsWalStreamNeedCkpt;
        while ((walStreamNode = walStreamMgr->GetNextWalStream(&iter, filter)) != nullptr) {
            walStream = walStreamNode->walStream;
            WalId walId = walStream->GetWalId();
            CheckpointFlag flag = CHECKPOINT_INVALID_FLAG;
            bool isPerformed = false;

            /* check if the elapsed seconds of the WalStream exceed timeout since last checkpoint */
            WalCheckpointInfoData *walCheckpointInfo = FindCheckpointInfo(walId);
            if (STORAGE_VAR_NULL(walCheckpointInfo)) {
                ErrLog(DSTORE_PANIC, MODULE_BUFMGR, ErrMsg("WalCheckpointInfo is nullptr, wal (%lu), pdb (%u).",
                    walId, m_pdbId));
            }
            now = static_cast<Timestamp>(time(nullptr));

            /* get the checkpoint request flag, and advance wal start count */
            flag = walCheckpointInfo->checkpointStreamRequest.StartCheckpoint();
            if (STORAGE_FUNC_FAIL(CheckpointOneWalStream(walId, flag, &isPerformed))) {
                goto LOOP;
            }

            /* advance wal done count */
            walCheckpointInfo->checkpointStreamRequest.FinishCheckpoint();

            if (isPerformed) {
                walCheckpointInfo->lastCheckpointTime = now;
                if (!g_storageInstance->IsBootstrapping()) {
                    int64 slotId = storagePdb->GetBgWriterSlotId(walId);
                    if (slotId < 0 || slotId >= DIRTY_PAGE_QUEUE_MAX_SIZE) {
                        ErrLog(DSTORE_ERROR, MODULE_BUFMGR, ErrMsg(
                            "CheckpointerMain, cannot find this slotId %ld.", slotId));
                        continue;
                    }
                    dynamic_cast<BufMgr *>(g_storageInstance->GetBufferMgr())->ReportBgwriter(m_pdbId, slotId);
                }
            }
        }

        /* step2: decide if sleep or process next turn checkpoint request */
        now = static_cast<Timestamp>(time(nullptr));
        elapsedTime = now - m_lastRequestTime;
        elapsedTime = elapsedTime < 0 ? 0 : elapsedTime;
        if (elapsedTime > g_storageInstance->GetGuc()->checkpointTimeout) {
            m_lastRequestTime = now;
            continue;
        }

        /* step3: check if there are dropping walstreams needs to be delete */
        walStreamMgr->DeleteDroppedWalStream();
        /* step4: wait enough time but break if requested to stop */
        /* WaitLatch */
        int waitTimeInMs = 1000;
        for (int i = 0; i < waitTimeInMs && !m_shutdownRequested; i++) {
            GaussUsleep(STORAGE_USECS_PER_MSEC);
        }
    }
}

RetStatus CheckpointMgr::FullCheckpoint(PdbId pdbId)
{
    m_isFullCkpting.store(true);
    if (!m_inited) {
        ErrLog(DSTORE_ERROR, MODULE_BUFMGR, ErrMsg("CheckpointMgr is not inited."));
        m_isFullCkpting.store(false);
        return DSTORE_FAIL;
    }

    StoragePdb *storagePdb = g_storageInstance->GetPdb(pdbId);
    StorageReleasePanic(storagePdb == nullptr, MODULE_BUFMGR, ErrMsg("PDB is nullptr."));
    StorageReleasePanic(!storagePdb->IsInit(), MODULE_BUFMGR, ErrMsg("PDB is not initialized."));
    if (storagePdb->GetPdbRoleMode() != PdbRoleMode::PDB_PRIMARY) {
        ErrLog(DSTORE_ERROR, MODULE_BUFMGR, ErrMsg("not supported on standby PDB."));
        m_isFullCkpting.store(false);
        return DSTORE_FAIL;
    }

    WalStreamManager *walStreamMgr = m_walMgr->GetWalStreamManager();
    StorageAssert(walStreamMgr);
    WalStream *walStream = walStreamMgr->GetWritingWalStream();
    if (walStream == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_BUFMGR, ErrMsg("WalStream is nullptr."));
        m_isFullCkpting.store(false);
        return DSTORE_FAIL;
    }

    WalId walId = walStream->GetWalId();
    CheckpointFlag flag = CHECKPOINT_INVALID_FLAG;
    bool isPerformed = false;
    RetStatus ret = CreateCheckpoint(walId, flag, &isPerformed);
    m_isFullCkpting.store(false);
    m_requestedCheckpoints++;
    if (!g_storageInstance->IsBootstrapping()) {
        int64 slotId = storagePdb->GetBgWriterSlotId(walId);
        if (slotId < 0 || slotId >= DIRTY_PAGE_QUEUE_MAX_SIZE) {
            ErrLog(DSTORE_ERROR, MODULE_BUFMGR, ErrMsg("FullCheckpoint, cannot find this slotId %ld.", slotId));
            return DSTORE_FAIL;
        }
        dynamic_cast<BufMgr *>(g_storageInstance->GetBufferMgr())->ReportBgwriter(m_pdbId, slotId);
    }
    return ret;
}

RetStatus CheckpointMgr::GetWalCheckpoint(WalId walId, WalCheckPoint &walCheckpoint)
{
    if (!m_inited.load(std::memory_order_acquire)) {
        return DSTORE_FAIL;
    }
    WalCheckpointInfoData *info = FindCheckpointInfo(walId);
    if (info == nullptr) {
        return DSTORE_FAIL;
    }
    DstoreLWLockAcquire(&info->checkpointLwLock, LW_SHARED);
    walCheckpoint = info->lastCheckPoint;
    LWLockRelease(&info->checkpointLwLock);
    return DSTORE_SUCC;
}

RetStatus CheckpointMgr::GetWalCheckpointHistory(WalId walId, WalCheckPointHistory &history)
{
    if (!m_inited.load(std::memory_order_acquire)) {
        return DSTORE_FAIL;
    }
    WalCheckpointInfoData *info = FindCheckpointInfo(walId);
    if (info == nullptr) {
        return DSTORE_FAIL;
    }
    DstoreLWLockAcquire(&info->checkpointLwLock, LW_SHARED);
    errno_t rc = memcpy_s(&history, sizeof(WalCheckPointHistory), &info->lastCheckPointHistory,
                          sizeof(WalCheckPointHistory));
    LWLockRelease(&info->checkpointLwLock);
    storage_securec_check(rc, "\0", "\0");
    return DSTORE_SUCC;
}

RetStatus CheckpointMgr::CheckpointOneWalStream(WalId walId, CheckpointFlag flags, bool *isPerformed)
{
    return CreateCheckpoint(walId, flags, isPerformed);
}

RetStatus CheckpointMgr::CreateCheckpoint(WalId walId, UNUSE_PARAM CheckpointFlag flags, bool *isPerformed)
{
    WalCheckPoint checkPoint = {};
    *isPerformed = false;
    WalCheckpointInfoData *checkpointInfo = FindCheckpointInfo(walId);
    if (checkpointInfo == nullptr) {
        return DSTORE_FAIL;
    }
    bool isShutdown = false;

    /*
     * Acquire checkpointLwLock to ensure only one checkpoint happens at a time.
     * (This is just pro forma, since in the present system structure there is
     * only one process that is allowed to issue checkpoints at any given
     * time.)
     */
    DstoreLWLockAcquire(&checkpointInfo->checkpointLwLock, LW_EXCLUSIVE);

    /* Step 1: check if is database shutdown
     * if it's database shutdown, we need update state and time in the ControlFile
     */
    if (isShutdown) {
    }
    checkPoint.time = time(nullptr);

    /* Step 2: check it's an incremental checkpoint or full checkpoint.
     * if it's an incremental checkpoint, get the recovery LSN from the first page in the dirty page queue.
     * if it's a full checkpoint, get the recovery LSN from the Redo log current insert position.
     */
    WalStreamManager *walStreamMgr = m_walMgr->GetWalStreamManager();
    if (STORAGE_VAR_NULL(walStreamMgr)) {
        LWLockRelease(&checkpointInfo->checkpointLwLock);
        ErrLog(DSTORE_WARNING, MODULE_BUFFER, ErrMsg("walStreamMgr is nullptr."));
        return DSTORE_FAIL;
    }
    WalStream *stream = walStreamMgr->GetWalStream(walId);
    if (STORAGE_VAR_NULL(stream)) {
        LWLockRelease(&checkpointInfo->checkpointLwLock);
        ErrLog(DSTORE_WARNING, MODULE_BUFFER, ErrMsg("stream is nullptr."));
        return DSTORE_FAIL;
    }

    uint64 recoveryPlsn = INVALID_PLSN;
    /* Step1: get DirtyPageQueue by walId */
    StoragePdb *pdb = g_storageInstance->GetPdb(m_pdbId);
    StorageReleasePanic(pdb == nullptr, MODULE_BUFFER, ErrMsg("pdb %u is nullptr", m_pdbId));
    BgPageWriterMgr *bgPageWriterMgr = pdb->GetBgPageWriterMgr();
    if (STORAGE_VAR_NULL(bgPageWriterMgr)) {
        LWLockRelease(&checkpointInfo->checkpointLwLock);
        ErrLog(DSTORE_WARNING, MODULE_BUFFER, ErrMsg("failed to get bg page writer mgr"));
        return DSTORE_FAIL;
    }

    BgDiskPageMasterWriter *bgPageWriter = bgPageWriterMgr->GetBgPageWriter<BgDiskPageMasterWriter>(walId);
    if (STORAGE_VAR_NULL(bgPageWriter)) {
        LWLockRelease(&checkpointInfo->checkpointLwLock);
        ErrLog(DSTORE_WARNING, MODULE_BUFFER, ErrMsg("failed to get the bg page writer"));
        return DSTORE_FAIL;
    }
    recoveryPlsn = bgPageWriter->GetMinRecoveryPlsn();
    checkPoint.diskRecoveryPlsn = recoveryPlsn;

    /* step6: update Control File */
    ControlFile *controlFile = pdb->GetControlFile();
    StorageReleasePanic(controlFile == nullptr, MODULE_BUFFER, ErrMsg("Failed to get controlFile, pdb %u", m_pdbId));
    ControlWalStreamPageItemData *walStreamInfo = nullptr;
    RetStatus ret = controlFile->GetWalStreamInfo(walId, &walStreamInfo);
    if (STORAGE_FUNC_FAIL(ret)) {
        LWLockRelease(&checkpointInfo->checkpointLwLock);
        return DSTORE_FAIL;
    }
    if (walStreamInfo->lastWalCheckpoint.diskRecoveryPlsn >= checkPoint.diskRecoveryPlsn &&
        walStreamInfo->lastWalCheckpoint.memoryCheckpoint.memRecoveryPlsn >=
            checkPoint.memoryCheckpoint.memRecoveryPlsn) {
        controlFile->FreeWalStreamsInfo(walStreamInfo);
        LWLockRelease(&checkpointInfo->checkpointLwLock);
        return DSTORE_SUCC;
    }
    uint64 lastrecoveryPlsn = walStreamInfo->lastWalCheckpoint.diskRecoveryPlsn;
    walStreamInfo->lastCheckpointPLsn = 0;
    walStreamInfo->lastWalCheckpoint = checkPoint;
    /*
     * Rotate the new checkpoint into the history ring before persisting, so that the control
     * file write atomically reflects the invariant slots[newestSlotIdx] == lastWalCheckpoint.
     */
    walStreamInfo->checkpointHistory.Push(checkPoint);
    walStreamInfo->barrier.barrierCsn = stream->GetWalRecovery()->GetBarrierCsn();
    walStreamInfo->barrier.barrierEndPlsn = stream->GetWalRecovery()->GetBarrierEndPlsn();
    walStreamInfo->barrier.barrierSyncMode = stream->GetWalRecovery()->GetBarrierSyncMode();
    ret = controlFile->UpdateWalStreamForCheckPointWithBarrier(*walStreamInfo);
    if (STORAGE_FUNC_FAIL(ret)) {
        controlFile->FreeWalStreamsInfo(walStreamInfo);
        LWLockRelease(&checkpointInfo->checkpointLwLock);
        return DSTORE_FAIL;
    }
    checkpointInfo->lastCheckPoint = checkPoint;
    {
        errno_t rc = memcpy_s(&checkpointInfo->lastCheckPointHistory, sizeof(WalCheckPointHistory),
                              &walStreamInfo->checkpointHistory, sizeof(WalCheckPointHistory));
        storage_securec_check(rc, "\0", "\0");
    }
    controlFile->FreeWalStreamsInfo(walStreamInfo);

    LWLockRelease(&checkpointInfo->checkpointLwLock);
    ErrLog(DSTORE_LOG, MODULE_BUFFER,
        ErrMsg("wal stream %lu complete checkpoint, checkpoint diskRecoveryPlsn %lu, checkpoint memRecoveryPlsn %lu "
            "last checkpoint recoveryPlsn %lu, current Flush Max Plsn %lu, current Append Max Plsn %lu, pdbId %u",
            walId, checkPoint.diskRecoveryPlsn, checkPoint.memoryCheckpoint.memRecoveryPlsn, lastrecoveryPlsn,
            stream->GetMaxFlushedPlsn(), stream->GetMaxAppendedPlsn(), m_pdbId));
    *isPerformed = true;
    return DSTORE_SUCC;
}

void CheckpointMgr::Init()
{
    m_checkpointContext = DstoreAllocSetContextCreate(g_storageInstance->GetMemoryMgr()->GetGroupContext(
        DSTORE::MEMORY_CONTEXT_BUFFER), "Checkpoint", ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE,
        ALLOCSET_DEFAULT_MAXSIZE, MemoryContextType::SHARED_CONTEXT);
    StorageReleasePanic(m_checkpointContext == nullptr, MODULE_BUFFER, ErrMsg("failed to alloc checkpointContext."));
    AutoMemCxtSwitch amcs{m_checkpointContext};
    m_checkpointPid = thrd->GetCore()->pid;
    DListInit(&m_checkpointInfoList);
    WalId *walIds = nullptr;
    StoragePdb *storagePdb = g_storageInstance->GetPdb(m_pdbId);
    StorageReleasePanic(storagePdb == nullptr, MODULE_BUFFER, ErrMsg("pdb %u is nullptr", m_pdbId));
    WalStreamFilter filter;
    if (storagePdb->GetPdbRoleMode() == PdbRoleMode::PDB_STANDBY) {
        filter = WalStream::IsWalStreamNeedCkpt;
    } else {
        filter = WalStream::IsWalStreamForWrite;
    }
    m_walCheckpointDataNum = m_walMgr->GetWalStreamManager()->GetOwnedStreamIds(&walIds, filter);
    ErrLog(DSTORE_LOG, MODULE_BUFFER, ErrMsg("init checkpoint mgr success, wal num %u", m_walCheckpointDataNum));

    /* Get WalStream Info, and init one WalCheckpointInfoData for each WalStream */
    ControlFile *controlFile = g_storageInstance->GetPdb(m_pdbId)->GetControlFile();
    StorageReleasePanic(controlFile == nullptr, MODULE_BUFFER, ErrMsg("Failed to get controlFile, pdb %u", m_pdbId));
    ControlWalStreamPageItemData *walStreamInfo = nullptr;
    for (uint32 i = 0; i < m_walCheckpointDataNum; i++) {
        WalCheckpointInfoData *checkPointNode =
            static_cast<WalCheckpointInfoData *>(DstorePalloc0(sizeof(WalCheckpointInfoData)));
        StorageReleasePanic(checkPointNode == nullptr, MODULE_BUFFER,
                        ErrMsg("alloc memory for checkpointInfoList fail!"));
        RetStatus ret = controlFile->GetWalStreamInfo(walIds[i], &walStreamInfo);
        StorageReleasePanic(STORAGE_FUNC_FAIL(ret), MODULE_BUFFER,
            ErrMsg("Can't get wal_stream(%lu) info from control file.", walIds[i]));
        checkPointNode->walId = walIds[i];
        DstoreNew(&checkPointNode->checkpointStreamRequest) CheckpointRequest();
        LWLockInitialize(&checkPointNode->checkpointLwLock, LWLOCK_GROUP_CHECK_POINT);
        checkPointNode->lastCheckpointTime = time(nullptr);
        checkPointNode->recoveryLock.Init();
        checkPointNode->lastCheckPointRecoveryPlsn = walStreamInfo->lastCheckpointPLsn;
        checkPointNode->lastCheckPoint = walStreamInfo->lastWalCheckpoint;
        errno_t cpyRc = memcpy_s(&checkPointNode->lastCheckPointHistory, sizeof(WalCheckPointHistory),
                                 &walStreamInfo->checkpointHistory, sizeof(WalCheckPointHistory));
        storage_securec_check(cpyRc, "\0", "\0");
        DListPushTail(&m_checkpointInfoList, &(checkPointNode->node));
    }
    DstorePfreeExt(walIds);
    m_shutdownRequested = false;
    m_lastRequestTime = time(nullptr);
    m_inited.store(true);
}

void CheckpointMgr::AddOneCheckPoint(WalId walid, uint64 lastcheckpointplsn)
{
    AutoMemCxtSwitch amcs{ m_checkpointContext };
    WalCheckpointInfoData *info = FindCheckpointInfo(walid);
    if (info != nullptr) {
        ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("AddOneCheckPoint: walId %lu has exits in checkPointNode", walid));
        return;
    }
    m_walCheckpointDataNum++;

    WalCheckpointInfoData *checkPointNode =
        static_cast<WalCheckpointInfoData *>(DstorePalloc0(sizeof(WalCheckpointInfoData)));
    StorageReleasePanic(checkPointNode == nullptr, MODULE_BUFFER, ErrMsg("alloc memory for checkPointNode fail!"));

    checkPointNode->walId = walid;
    DstoreNew(&checkPointNode->checkpointStreamRequest) CheckpointRequest();
    LWLockInitialize(&checkPointNode->checkpointLwLock, LWLOCK_GROUP_CHECK_POINT);
    checkPointNode->lastCheckpointTime = time(nullptr);
    checkPointNode->recoveryLock.Init();
    checkPointNode->lastCheckPointRecoveryPlsn = lastcheckpointplsn;
    m_queueSpinlock.Acquire();
    DListPushTail(&m_checkpointInfoList, &(checkPointNode->node));
    m_queueSpinlock.Release();
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("AddOneCheckPoint: walId %lu add in checkPointNode", walid));
}

void CheckpointMgr::Destroy()
{
    constexpr long waitStep = 1000;
    while (m_isFullCkpting) {
        GaussUsleep(waitStep);
    }

    if (m_checkpointContext != nullptr) {
        DstoreMemoryContextDelete(m_checkpointContext);
        m_checkpointContext = nullptr;
    }
}

void CheckpointMgr::StopCheckpointer()
{
    m_inited.store(false);
    m_shutdownRequested.store(true, std::memory_order_release);
}

WalCheckpointInfoData *CheckpointMgr::FindCheckpointInfo(WalId walId)
{
    dlist_iter iter;
    m_queueSpinlock.Acquire();
    dlist_foreach(iter, &m_checkpointInfoList) {
        WalCheckpointInfoData *checkPointNode = dlist_container(WalCheckpointInfoData, node, iter.cur);
        if (checkPointNode->walId == walId) {
            m_queueSpinlock.Release();
            return checkPointNode;
        }
    }
    m_queueSpinlock.Release();
    return nullptr;
}

RetStatus CheckpointMgr::GetCheckpointStatInfo(CheckpointStatInfo &ckptInfo)
{
    if (!m_inited.load(std::memory_order_acquire)) {
        ErrLog(DSTORE_ERROR, MODULE_BUFFER, ErrMsg("checkpointer has not inited."));
        return DSTORE_FAIL;
    }

    dlist_iter iter;
    int cktnum = 0;
    ckptInfo.pdbId = m_pdbId;
    ckptInfo.checkpointPid = m_checkpointPid;
    ckptInfo.lastRequestTime = m_lastRequestTime;
    ckptInfo.requestedCheckpoints = m_requestedCheckpoints;
    ckptInfo.walStreamNum = m_walCheckpointDataNum;
    ckptInfo.walCkptStatInfo =
        static_cast<WalCheckpointStatInfo *>(DstorePalloc0(sizeof(WalCheckpointStatInfo) * m_walCheckpointDataNum));
    if (ckptInfo.walCkptStatInfo == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_BUFFER, ErrMsg("failed to alloc memory for wal ckpt stat info."));
        return DSTORE_FAIL;
    }

    m_queueSpinlock.Acquire();
    dlist_foreach(iter, &m_checkpointInfoList)
    {
        WalCheckpointInfoData *infoList = dlist_container(WalCheckpointInfoData, node, iter.cur);
        ckptInfo.walCkptStatInfo[cktnum].walId = infoList->walId;
        ckptInfo.walCkptStatInfo[cktnum].checkpointStart = infoList->checkpointStreamRequest.GetCheckpointStartCnt();
        ckptInfo.walCkptStatInfo[cktnum].checkpointDone = infoList->checkpointStreamRequest.GetCheckpointDoneCnt();
        ckptInfo.walCkptStatInfo[cktnum].lastCheckpointTime = infoList->lastCheckpointTime;
        ckptInfo.walCkptStatInfo[cktnum].maxFlushedPlsn =
            m_walMgr->GetWalStreamManager()->GetWalStream(infoList->walId)->GetMaxFlushedPlsn();
        ckptInfo.walCkptStatInfo[cktnum].maxAppendedPlsn =
            m_walMgr->GetWalStreamManager()->GetWalStream(infoList->walId)->GetMaxAppendedPlsn();
        DstoreLWLockAcquire(&(infoList->checkpointLwLock), LW_SHARED);
        ckptInfo.walCkptStatInfo[cktnum].lastCheckPoint = infoList->lastCheckPoint;
        LWLockRelease(&(infoList->checkpointLwLock));
        cktnum++;
    }
    m_queueSpinlock.Release();

    return DSTORE_SUCC;
}

CheckpointRequest::CheckpointRequest()
    : m_checkpointStart{0}, m_checkpointDone{0}, m_checkpointFail{0}, m_checkpointFlag{CHECKPOINT_INVALID_FLAG}
{
    m_checkpointLock.Init();
}

CheckpointFlag CheckpointRequest::StartCheckpoint()
{
    m_checkpointLock.Acquire();
    CheckpointFlag flag = CHECKPOINT_CAUSE_TIME;
    m_checkpointStart++;
    m_checkpointLock.Release();
    return flag;
}

void CheckpointRequest::FinishCheckpoint()
{
    m_checkpointLock.Acquire();
    m_checkpointDone++;
    m_checkpointLock.Release();
}

uint32 CheckpointRequest::GetCheckpointStartCnt() const
{
    return m_checkpointStart;
}

uint32 CheckpointRequest::GetCheckpointDoneCnt() const
{
    return m_checkpointDone;
}

void DumpAllWalSteamCkptInfo(StringInfoData &dumpInfo, uint32 walStreamNum, WalCheckpointStatInfo *walCkptStatInfo)
{
    for (uint32 i = 0; i < walStreamNum; i++) {
        dumpInfo.append("  walId:%ld checkpointStart:%u checkpointDone:%u lastCheckpointTime:%ld "
            "timeInCtrlFile:%ld diskRecoveryPlsn:%lu maxFlushedPlsn:%lu maxAppendedPlsn:%lu\n",
            walCkptStatInfo[i].walId, walCkptStatInfo[i].checkpointStart,
            walCkptStatInfo[i].checkpointDone, walCkptStatInfo[i].lastCheckpointTime,
            walCkptStatInfo[i].lastCheckPoint.time,
            walCkptStatInfo[i].lastCheckPoint.diskRecoveryPlsn,
            walCkptStatInfo[i].maxFlushedPlsn, walCkptStatInfo[i].maxAppendedPlsn);
        if (walCkptStatInfo[i].lastCheckPoint.memoryCheckpoint.memoryNodeCnt > 0) {
            dumpInfo.append("  term:%lu memRecoveryPlsn:%lu",
                walCkptStatInfo[i].lastCheckPoint.memoryCheckpoint.term,
                walCkptStatInfo[i].lastCheckPoint.memoryCheckpoint.memRecoveryPlsn);
            dumpInfo.append(" memnode:");
            dumpInfo.append("\n");
        }
    }
}

}  // namespace DSTORE
