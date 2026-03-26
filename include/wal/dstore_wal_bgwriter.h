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
 * dstore_wal_bgwriter.h
 *
 * Description:
 * Wal private header file, define interface about wal background writer
 * ---------------------------------------------------------------------------------------
 *
 */
#ifndef DSTORE_DSTORE_WAL_BGWRITER_H
#define DSTORE_DSTORE_WAL_BGWRITER_H

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>

#include "dstore_wal_buffer.h"
#include "framework/dstore_watchdog.h"

namespace DSTORE {
class WalStream;
/**
 * BgWalWriter is a thread which is in duty of do wal flush work.
 * And BgWalWriter temporarily bound to fixed WalStream to support multi XLosStream parallel flush
 *
 * Notes:
 * 1. writer support restart by calling BgWalWriter::Run again
 * 2. writer can't stop before write fetched data from WalBuffer to WalFile
 * 2. writer don't support sleeping mechanism
 */
class BgWalWriter : public BaseObject {
public:
    BgWalWriter(DstoreMemoryContext mct, WalStream *stream, PdbId pdbId);
    virtual ~BgWalWriter();
    DISALLOW_COPY_AND_MOVE(BgWalWriter)

    /*
     * Init wal bgwriter by register thread resource and
     *
     * @return: init result
     */
    RetStatus Init();

    /*
     * Start work thread, do nothing if there is already running one.
     */
    void Run();

    /*
     * Try interrupt now running thread, just return success if no thread is running.
     * After this function return, not influence next thread normally start.
     */
    void Stop() noexcept;

    /*
     * Process the Wal flushing task.
     */
    void BgFlushMain();

    /*
     * Wake up bg writer if in sleeping state
     */
    void WakeUpIfSleeping();

    inline void ThrottleIfNeed()
    {
        constexpr uint64 reportStep = 1000; /* 1000 * 10ms = 10s report once */
        uint64 counter = 0;
        while (unlikely(m_needThrottleTrxn)) {
            if (unlikely(++counter % reportStep == 0)) {
                ErrLog(DSTORE_ERROR, MODULE_WAL,
                        ErrMsg("BgWalWriter is in throttling, sleep %lu times.", counter));
            }
            GaussUsleep(10 * 1000); /* 10 ms */
        }
    }

#ifndef UT
private:
#endif
    void WalThrottling();
    void SleepIfNecessary(uint64 flushedDataLen);
    bool IsInvalidTime(const timespec &time) const;

    std::atomic<bool> m_needStop;
    bool m_isRunning;
    std::atomic<bool> m_isSleeping;
    /* WatchDog heartbeat: updated each main-loop iteration (seconds since epoch, 0 = not running) */
    std::atomic<uint64> m_lastHeartbeatTime;
    WatchDogEntry m_watchdogEntry;

    std::thread *m_bgThread;
    std::mutex m_runningStateMtx;
    std::condition_variable m_runningStateCv;
    std::mutex m_sleepMtx;
    std::condition_variable m_sleepCv;
    struct timespec m_firstEmptyFlushTime;
    DstoreMemoryContext m_memoryContext;
    WalStream *m_stream;
    PdbId m_pdbId;
    bool m_isInThrottling;
    bool m_needThrottleTrxn{false};
};
}

#endif  // DSTORE_STORAGE_WAL_BGWRITER_H
