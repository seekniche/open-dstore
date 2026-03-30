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

#include "diagnose/dstore_watchdog_diagnose.h"
#include "framework/dstore_instance.h"
#include "common/memory/dstore_mctx.h"
#include "common/algorithm/dstore_string_info.h"
#include "securec.h"

#ifdef UT
#include "common/fault_injection/dstore_watchdog_fault_injection.h"
#endif

namespace DSTORE {

static const char *ThreadTypeToString(WatchDogThreadType type)
{
    switch (type) {
        case WatchDogThreadType::BG_PAGE_MASTER_WRITER:
            return "BG_PAGE_MASTER";
        case WatchDogThreadType::BG_PAGE_SLAVE_WRITER:
            return "BG_PAGE_SLAVE";
        case WatchDogThreadType::CHECKPOINTER:
            return "CHECKPOINTER";
        case WatchDogThreadType::WAL_FILE_RECYCLE:
            return "WAL_FILE_RECYCLE";
        case WatchDogThreadType::UNDO_RECYCLE:
            return "UNDO_RECYCLE";
        case WatchDogThreadType::BTREE_RECYCLE:
            return "BTREE_RECYCLE";
        default:
            return "UNKNOWN";
    }
}

static const char *RunStateToString(ThreadRunState state)
{
    switch (state) {
        case ThreadRunState::NOT_STARTED:
            return "NOT_STARTED";
        case ThreadRunState::RUNNING:
            return "RUNNING";
        case ThreadRunState::SLEEPING:
            return "SLEEPING";
        case ThreadRunState::STUCK:
            return "STUCK";
        case ThreadRunState::STOPPED:
            return "STOPPED";
        default:
            return "UNKNOWN";
    }
}

static void FillThreadStatus(WatchDogThreadStatus *status, const WatchDogHandle *hb,
                             uint64 nowUs, WatchDogMgr *mgr)
{
    error_t rc = strncpy_s(status->threadName, sizeof(status->threadName),
                           hb->threadName, sizeof(status->threadName) - 1);
    storage_securec_check(rc, "\0", "\0");
    status->threadType = hb->threadType;
    status->threadIndex = hb->threadIndex;
    status->runState = hb->runState.load(std::memory_order_relaxed);
    status->lastHeartbeatUs = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
    status->elapsedUs = nowUs - status->lastHeartbeatUs;
    status->timeoutThresholdUs = mgr->GetTimeoutUsFromGuc(hb->threadType);
    status->isTimeout = (status->runState == ThreadRunState::RUNNING ||
                         status->runState == ThreadRunState::STUCK) &&
                        (status->elapsedUs > status->timeoutThresholdUs);
}

RetStatus WatchDogDiagnose::GetAllThreadStatus(PdbId pdbId,
                                               WatchDogThreadStatus **outArray,
                                               uint32_t *outCount)
{
    if (outArray == nullptr || outCount == nullptr) {
        return DSTORE_FAIL;
    }

    *outArray = nullptr;
    *outCount = 0;

    if (g_storageInstance == nullptr) {
        return DSTORE_FAIL;
    }

    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
#ifdef UT
    FAULT_INJECTION_ACTION(DstoreWatchDogFI::GET_PDB_FAIL, pdb = nullptr);
#endif

    if (pdb == nullptr) {
        return DSTORE_FAIL;
    }

    WatchDogMgr *mgr = pdb->GetWatchDogMgr();
#ifdef UT
    FAULT_INJECTION_ACTION(DstoreWatchDogFI::GET_WATCHDOG_MGR_FAIL, mgr = nullptr);
#endif

    if (mgr == nullptr) {
        return DSTORE_FAIL;
    }

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = mgr->GetHeartbeatSnapshot(snapshot, WatchDogMgr::MAX_WATCHED_THREADS);

    if (count == 0) {
        return DSTORE_SUCC;
    }

    WatchDogThreadStatus *result = static_cast<WatchDogThreadStatus *>(
        DstorePalloc(sizeof(WatchDogThreadStatus) * count));
#ifdef UT
    FAULT_INJECTION_ACTION(DstoreWatchDogFI::PALLOC_FAIL, { DstorePfree(result); result = nullptr; });
#endif
    if (result == nullptr) {
        return DSTORE_FAIL;
    }

    uint64 nowUs = WatchDogMgr::GetSteadyClockUs();
    for (uint32 i = 0; i < count; i++) {
        FillThreadStatus(&result[i], &snapshot[i], nowUs, mgr);
    }

    *outArray = result;
    *outCount = count;
    return DSTORE_SUCC;
}

RetStatus WatchDogDiagnose::GetThreadStatusByType(PdbId pdbId,
                                                   WatchDogThreadType type,
                                                   WatchDogThreadStatus **outArray,
                                                   uint32_t *outCount)
{
    if (outArray == nullptr || outCount == nullptr) {
        return DSTORE_FAIL;
    }

    *outArray = nullptr;
    *outCount = 0;

    if (g_storageInstance == nullptr) {
        return DSTORE_FAIL;
    }

    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
    if (pdb == nullptr) {
        return DSTORE_FAIL;
    }

    WatchDogMgr *mgr = pdb->GetWatchDogMgr();
    if (mgr == nullptr) {
        return DSTORE_FAIL;
    }

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = mgr->GetHeartbeatsByType(type, snapshot, WatchDogMgr::MAX_WATCHED_THREADS);

    if (count == 0) {
        return DSTORE_SUCC;
    }

    WatchDogThreadStatus *result = static_cast<WatchDogThreadStatus *>(
        DstorePalloc(sizeof(WatchDogThreadStatus) * count));
    if (result == nullptr) {
        return DSTORE_FAIL;
    }

    uint64 nowUs = WatchDogMgr::GetSteadyClockUs();
    for (uint32 i = 0; i < count; i++) {
        FillThreadStatus(&result[i], &snapshot[i], nowUs, mgr);
    }

    *outArray = result;
    *outCount = count;
    return DSTORE_SUCC;
}

void WatchDogDiagnose::FreeThreadStatusArray(WatchDogThreadStatus *arr)
{
    if (arr != nullptr) {
        DstorePfree(arr);
    }
}

char *WatchDogDiagnose::GetFormattedSummary(PdbId pdbId)
{
    if (g_storageInstance == nullptr) {
        return nullptr;
    }
    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
    if (pdb == nullptr) {
        return nullptr;
    }

    WatchDogMgr *mgr = pdb->GetWatchDogMgr();
    if (mgr == nullptr) {
        return nullptr;
    }

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = mgr->GetHeartbeatSnapshot(snapshot, WatchDogMgr::MAX_WATCHED_THREADS);

    StringInfoData str;
    if (unlikely(!str.init())) {
        return nullptr;
    }

    str.append("================== WatchDog Thread Status ==================\n");
    str.append("PDB: %u\n\n", pdbId);
    str.append("%-20s %-20s %-6s %-10s %-9s %-11s %-12s\n",
               "Thread Name", "Type", "Index", "State", "Timeout?", "Elapsed(s)", "Threshold(s)");
    str.append("--------------------+--------------------+------+----------+---------+-----------+-------------\n");

    uint64 nowUs = WatchDogMgr::GetSteadyClockUs();
    uint32 stuckCount = 0;

    for (uint32 i = 0; i < count; i++) {
        const WatchDogHandle *hb = &snapshot[i];
        ThreadRunState state = hb->runState.load(std::memory_order_relaxed);
        uint64 lastHbUs = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
        uint64 elapsedUs = nowUs - lastHbUs;
        uint64 timeoutUs = mgr->GetTimeoutUsFromGuc(hb->threadType);
        bool isTimeout = (state == ThreadRunState::RUNNING || state == ThreadRunState::STUCK) &&
                         (elapsedUs > timeoutUs);

        if (state == ThreadRunState::STUCK) {
            stuckCount++;
        }

        str.append("%-20s %-20s %-6u %-10s %-9s %-11.1f %-12.1f\n",
                   hb->threadName,
                   ThreadTypeToString(hb->threadType),
                   hb->threadIndex,
                   RunStateToString(state),
                   isTimeout ? "YES" : "NO",
                   static_cast<double>(elapsedUs) / 1000000.0,
                   static_cast<double>(timeoutUs) / 1000000.0);
    }

    str.append("\nStuck Threads: %u\n", stuckCount);
    str.append("================================================================\n");

    return str.data;
}

char *WatchDogDiagnose::GetFormattedStatusByType(PdbId pdbId, WatchDogThreadType type)
{
    if (g_storageInstance == nullptr) {
        return nullptr;
    }
    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
    if (pdb == nullptr) {
        return nullptr;
    }

    WatchDogMgr *mgr = pdb->GetWatchDogMgr();
    if (mgr == nullptr) {
        return nullptr;
    }

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = mgr->GetHeartbeatsByType(type, snapshot, WatchDogMgr::MAX_WATCHED_THREADS);

    StringInfoData str;
    if (unlikely(!str.init())) {
        return nullptr;
    }

    str.append("WatchDog Status for type: %s (PDB=%u)\n", ThreadTypeToString(type), pdbId);

    uint64 nowUs = WatchDogMgr::GetSteadyClockUs();

    for (uint32 i = 0; i < count; i++) {
        const WatchDogHandle *hb = &snapshot[i];
        ThreadRunState state = hb->runState.load(std::memory_order_relaxed);
        uint64 lastHbUs = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
        uint64 elapsedUs = nowUs - lastHbUs;
        uint64 timeoutUs = mgr->GetTimeoutUsFromGuc(hb->threadType);
        bool isTimeout = (state == ThreadRunState::RUNNING || state == ThreadRunState::STUCK) &&
                         (elapsedUs > timeoutUs);

        str.append("  [%u] %s: state=%s, timeout=%s, elapsed=%.1fs, threshold=%.1fs\n",
                   hb->threadIndex,
                   hb->threadName,
                   RunStateToString(state),
                   isTimeout ? "YES" : "NO",
                   static_cast<double>(elapsedUs) / 1000000.0,
                   static_cast<double>(timeoutUs) / 1000000.0);
    }

    if (count == 0) {
        str.append("  (no threads registered for this type)\n");
    }

    return str.data;
}

}  /* namespace DSTORE */
