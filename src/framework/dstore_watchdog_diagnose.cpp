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
 * dstore_watchdog_diagnose.cpp
 *
 * IDENTIFICATION
 *        src/framework/dstore_watchdog_diagnose.cpp
 *
 * ---------------------------------------------------------------------------------------
 */
#include "diagnose/dstore_watchdog_diagnose.h"
#include "framework/dstore_watchdog.h"
#include "framework/dstore_pdb.h"
#include "framework/dstore_instance.h"
#include "common/algorithm/dstore_string_info.h"
#include "common/memory/dstore_mctx.h"
#include "common/log/dstore_log.h"

namespace DSTORE {

WatchDogDiagnose::WatchDogDiagnose(uint32_t pdbId) : m_pdbId(pdbId)
{}

char *WatchDogDiagnose::GetHealthStatus()
{
    StoragePdb *storagePdb = g_storageInstance->GetPdb(m_pdbId);
    if (STORAGE_VAR_NULL(storagePdb)) {
        ErrLog(DSTORE_ERROR, MODULE_WATCHDOG, ErrMsg("WatchDogDiagnose: pdb %u is nullptr.", m_pdbId));
        return nullptr;
    }

    WatchDogMgr *watchdogMgr = storagePdb->GetWatchDogMgr();
    if (STORAGE_VAR_NULL(watchdogMgr)) {
        ErrLog(DSTORE_LOG, MODULE_WATCHDOG, ErrMsg("WatchDogDiagnose: WatchDogMgr not initialized for pdb %u.",
                                                     m_pdbId));
        return nullptr;
    }

    StringInfoData info;
    if (!info.init()) {
        return nullptr;
    }

    /* Ask WatchDogMgr to fill a temporary buffer, then append to StringInfoData */
    constexpr uint32 STATUS_BUF_SIZE = 4096;
    char tmpBuf[STATUS_BUF_SIZE] = {0};
    uint32 written = watchdogMgr->GetStatus(tmpBuf, STATUS_BUF_SIZE);

    info.append("WatchDog status for pdb %u:\n", m_pdbId);
    if (written > 0) {
        info.append("%s", tmpBuf);
    } else {
        info.append("  (no threads registered)\n");
    }

    return info.data;
}

uint32_t WatchDogDiagnose::GetThreadStatusArray(WatchDogThreadStatus **statusArr)
{
    if (statusArr == nullptr) {
        return 0;
    }
    *statusArr = nullptr;

    StoragePdb *storagePdb = g_storageInstance->GetPdb(m_pdbId);
    if (STORAGE_VAR_NULL(storagePdb)) {
        return 0;
    }

    WatchDogMgr *watchdogMgr = storagePdb->GetWatchDogMgr();
    if (STORAGE_VAR_NULL(watchdogMgr)) {
        return 0;
    }

    /* Use the text-based GetStatus to build the array: use a larger temp buffer approach.
     * We allocate a WatchDogThreadStatus array large enough for WATCHDOG_MAX_ENTRIES.
     * Then parse through the watchdog entries via the existing GetStatus text output is
     * not ideal; instead we add a structured method on WatchDogMgr.
     */
    constexpr uint32 MAX = WATCHDOG_MAX_ENTRIES;
    WatchDogThreadStatus *arr = static_cast<WatchDogThreadStatus *>(
        DstorePalloc0(sizeof(WatchDogThreadStatus) * MAX));
    if (arr == nullptr) {
        return 0;
    }

    uint32_t cnt = watchdogMgr->GetStatusArray(arr, MAX);
    if (cnt == 0) {
        DstorePfree(arr);
        return 0;
    }

    *statusArr = arr;
    return cnt;
}

void WatchDogDiagnose::FreeThreadStatusArray(WatchDogThreadStatus *statusArr)
{
    if (statusArr != nullptr) {
        DstorePfree(statusArr);
    }
}

}  /* namespace DSTORE */
