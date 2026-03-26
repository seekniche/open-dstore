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
 * dstore_watchdog_diagnose.h
 *
 * Public diagnose interface for WatchDog. The server layer can call
 * WatchDogDiagnose to query the health status of background threads.
 *
 * IDENTIFICATION
 *        interface/diagnose/dstore_watchdog_diagnose.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef DSTORE_WATCHDOG_DIAGNOSE_H
#define DSTORE_WATCHDOG_DIAGNOSE_H

#include <cstdint>
#include "common/dstore_common_utils.h"

namespace DSTORE {

#pragma GCC visibility push(default)

/*
 * Health status of a single monitored background thread.
 */
struct WatchDogThreadStatus {
    char name[64];               /* thread name */
    uint32_t pdbId;              /* PDB that the thread belongs to */
    uint64_t lastHeartbeatSecs;  /* seconds since last heartbeat (0 = not started) */
    uint64_t timeoutSecs;        /* configured timeout threshold */
    bool isStuck;                /* true if lastHeartbeatSecs > timeoutSecs */
};

/*
 * WatchDogDiagnose provides a snapshot of all monitored thread health states.
 *
 * Usage (from server layer):
 *   DSTORE::WatchDogDiagnose diag(pdbId);
 *   char *status = diag.GetHealthStatus();
 *   // ... use status ...
 *   DSTORE::DstorePfreeExt(status);   // free when done
 *
 *   // OR get structured data:
 *   DSTORE::WatchDogThreadStatus *arr = nullptr;
 *   uint32_t cnt = diag.GetThreadStatusArray(&arr);
 *   // ... use arr[0..cnt-1] ...
 *   diag.FreeThreadStatusArray(arr);
 */
class WatchDogDiagnose {
public:
    explicit WatchDogDiagnose(uint32_t pdbId);
    ~WatchDogDiagnose() = default;

    /*
     * Returns a human-readable string listing every monitored thread and its health.
     * Caller must free the returned pointer with DstorePfreeExt().
     * Returns nullptr if WatchDog is not initialized.
     */
    char *GetHealthStatus();

    /*
     * Fills *statusArr with a heap-allocated array of WatchDogThreadStatus.
     * Returns the number of entries. Caller must call FreeThreadStatusArray().
     */
    uint32_t GetThreadStatusArray(WatchDogThreadStatus **statusArr);

    /*
     * Frees the array returned by GetThreadStatusArray().
     */
    void FreeThreadStatusArray(WatchDogThreadStatus *statusArr);

private:
    uint32_t m_pdbId;
};

#pragma GCC visibility pop

}  /* namespace DSTORE */

#endif  /* DSTORE_WATCHDOG_DIAGNOSE_H */
