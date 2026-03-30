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
 * dstore_watchdog.h
 *
 * IDENTIFICATION
 *        include/framework/dstore_watchdog.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSTORE_WATCHDOG_H
#define DSTORE_WATCHDOG_H

#include <atomic>
#include <chrono>
#include <thread>
#include "common/dstore_datatype.h"

namespace DSTORE {

enum class WatchDogThreadType : uint8 {
    BG_PAGE_MASTER_WRITER = 0,
    BG_PAGE_SLAVE_WRITER,
    CHECKPOINTER,
    WAL_FILE_RECYCLE,
    UNDO_RECYCLE,
    BTREE_RECYCLE,
    WATCHDOG_THREAD_TYPE_COUNT
};

enum class ThreadRunState : uint8 {
    NOT_STARTED = 0,
    RUNNING,
    SLEEPING,
    STUCK,
    STOPPED
};

struct WatchDogHandle {
    std::atomic<uint64> lastHeartbeatUs{0};
    std::atomic<ThreadRunState> runState{ThreadRunState::NOT_STARTED};
    WatchDogThreadType threadType = WatchDogThreadType::WATCHDOG_THREAD_TYPE_COUNT;
    uint32 threadIndex = 0;
    char threadName[64] = {'\0'};
    std::atomic<bool> registered{false};
};

class WatchDogMgr {
public:
    WatchDogMgr();
    ~WatchDogMgr();
    DISALLOW_COPY_AND_MOVE(WatchDogMgr);

    RetStatus Init(PdbId pdbId);
    void StartThread();
    void WaitReady() const;
    void Destroy();

    WatchDogHandle *Register(WatchDogThreadType type, uint32 index, const char *threadName);
    void Unregister(WatchDogHandle *heartbeat);

    static inline void TouchHeartbeat(WatchDogHandle *hb)
    {
        if (hb == nullptr) {
            return;
        }
        uint64 nowUs = static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        hb->lastHeartbeatUs.store(nowUs, std::memory_order_relaxed);
    }

    static inline void SetRunState(WatchDogHandle *hb, ThreadRunState state)
    {
        if (hb != nullptr) {
            hb->runState.store(state, std::memory_order_relaxed);
        }
    }

    void WatchDogThreadMain();

    uint32 GetHeartbeatSnapshot(WatchDogHandle *outArray, uint32 maxCount) const;
    uint32 GetHeartbeatsByType(WatchDogThreadType type, WatchDogHandle *outArray, uint32 maxCount) const;

    uint64 GetTimeoutUsFromGuc(WatchDogThreadType type) const;

    static uint64 GetSteadyClockUs();

    static constexpr uint32 MAX_WATCHED_THREADS = 64;

    PdbId GetPdbId() const { return m_pdbId; }
    bool IsStopped() const { return m_stop.load(std::memory_order_relaxed); }
    bool IsReady() const { return m_ready.load(std::memory_order_acquire); }
    uint32 GetRegisteredCount() const { return m_registeredCount.load(std::memory_order_acquire); }

#ifndef UT
private:
#endif
    void CheckAllHeartbeats();

    PdbId m_pdbId;
    std::atomic<bool> m_stop;
    std::atomic<bool> m_ready;
    std::thread *m_watchdogThread;

    WatchDogHandle m_heartbeats[MAX_WATCHED_THREADS];
    std::atomic<uint32> m_registeredCount;
};

}  /* namespace DSTORE */

#endif  /* DSTORE_WATCHDOG_H */
