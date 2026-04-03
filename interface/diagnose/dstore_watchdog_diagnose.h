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
 * IDENTIFICATION
 *        interface/diagnose/dstore_watchdog_diagnose.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSTORE_WATCHDOG_DIAGNOSE_H
#define DSTORE_WATCHDOG_DIAGNOSE_H

#include "framework/dstore_watchdog.h"
#include "diagnose/dstore_diagnose.h"

namespace DSTORE {

struct WatchDogThreadStatus : public DiagnoseItem {
    char threadName[64];
    WatchDogThreadType threadType;
    uint32_t threadIndex;
    ThreadRunState runState;           /* 线程自报状态 */
    ThreadHealthState healthState;     /* WatchDog 推断的健康状态 */
    bool isTimeout;
    uint64_t lastHeartbeatUs;
    uint64_t elapsedUs;
    uint64_t timeoutThresholdUs;
    char progressMsg[WATCHDOG_PROGRESS_MSG_LEN];  /* 线程最后汇报的进度描述 */
};

#pragma GCC visibility push(default)

class WatchDogDiagnose {
public:
    static RetStatus GetAllThreadStatus(PdbId pdbId,
                                        WatchDogThreadStatus **outArray,
                                        uint32_t *outCount);

    static RetStatus GetThreadStatusByType(PdbId pdbId,
                                           WatchDogThreadType type,
                                           WatchDogThreadStatus **outArray,
                                           uint32_t *outCount);

    static void FreeThreadStatusArray(WatchDogThreadStatus *arr);

    static char *GetFormattedSummary(PdbId pdbId);

    static char *GetFormattedStatusByType(PdbId pdbId, WatchDogThreadType type);
};

#pragma GCC visibility pop

}  /* namespace DSTORE */

#endif  /* DSTORE_WATCHDOG_DIAGNOSE_H */
