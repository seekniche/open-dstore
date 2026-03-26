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
 * WatchDog: monitors background threads for hangs/deadlocks.
 * Each monitored thread updates a heartbeat timestamp (seconds) in its main loop.
 * The WatchDog thread wakes every second and checks all registered entries.
 * If a heartbeat is stale beyond the timeout, an error-level alarm is emitted.
 *
 * IDENTIFICATION
 *        include/framework/dstore_watchdog.h
 *
 * ---------------------------------------------------------------------------------------
 */
#ifndef DSTORE_WATCHDOG_H
#define DSTORE_WATCHDOG_H

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "common/dstore_datatype.h"
#include "common/memory/dstore_mctx.h"

namespace DSTORE {

/* Forward declaration of diagnose struct (defined in interface/diagnose/dstore_watchdog_diagnose.h) */
struct WatchDogThreadStatus;

/* Minimum interval (seconds) between repeated alarms for the same entry */
constexpr uint64 WATCHDOG_ALARM_INTERVAL_SECONDS = 10;

/* Default timeout thresholds (seconds) */
constexpr uint64 WATCHDOG_WAL_BGWRITER_TIMEOUT_S      = 30;
constexpr uint64 WATCHDOG_CHECKPOINTER_TIMEOUT_S      = 180;
constexpr uint64 WATCHDOG_BUF_PAGEWRITER_TIMEOUT_S    = 60;
constexpr uint64 WATCHDOG_WAL_RECYCLE_TIMEOUT_S       = 60;
constexpr uint64 WATCHDOG_UNDO_RECYCLE_TIMEOUT_S      = 120;

/* How often the watchdog thread wakes up (seconds) */
constexpr uint64 WATCHDOG_CHECK_INTERVAL_S = 1;

/* Max number of monitored entries per WatchDogMgr */
constexpr uint32 WATCHDOG_MAX_ENTRIES = 64;

/*
 * WatchDogEntry represents one registered background thread.
 * The entry is owned by the monitored object (not by WatchDogMgr).
 * The heartbeat pointer must remain valid for the full lifetime of the entry.
 */
struct WatchDogEntry {
    const char *threadName;                    /* static string: thread name for log messages */
    std::atomic<uint64> *heartbeatTimePtr;     /* pointer to the thread's heartbeat timestamp (seconds) */
    uint64 timeoutSeconds;                     /* alarm threshold */
    PdbId pdbId;                               /* PDB this thread belongs to */

    /* internal: last time an alarm was emitted for this entry (not protected by lock, only written by watchdog) */
    uint64 lastAlarmTime;

    WatchDogEntry(const char *name, std::atomic<uint64> *hbPtr, uint64 timeout, PdbId pdb)
        : threadName(name), heartbeatTimePtr(hbPtr), timeoutSeconds(timeout), pdbId(pdb), lastAlarmTime(0)
    {}
};

/*
 * WatchDogMgr manages a pool of WatchDogEntry objects and runs a background
 * thread that periodically checks whether each registered thread is alive.
 *
 * Lifecycle:
 *   1. WatchDogMgr::Register(entry) — called by each background thread at startup
 *   2. WatchDogMgr::Start(pdbId)    — called by StoragePdb after all threads are registered
 *   3. WatchDogMgr::Stop()          — called by StoragePdb during shutdown
 *   4. WatchDogMgr::Unregister(entry) — optional, called when a thread exits mid-run
 */
class WatchDogMgr : public BaseObject {
public:
    explicit WatchDogMgr();
    ~WatchDogMgr();
    DISALLOW_COPY_AND_MOVE(WatchDogMgr)

    /*
     * Register a WatchDogEntry. Thread-safe.
     * The entry's heartbeatTimePtr must point to an atomic owned by the monitored object.
     * Must be called before Start(), or while the watchdog thread is running.
     */
    void Register(WatchDogEntry *entry);

    /*
     * Unregister a previously registered entry. Thread-safe.
     * After return, the watchdog will no longer access the entry.
     */
    void Unregister(WatchDogEntry *entry);

    /*
     * Start the watchdog background thread.
     */
    void Start(PdbId pdbId);

    /*
     * Signal the watchdog thread to stop and wait for it to exit.
     */
    void Stop();

    /*
     * Fill buf with a human-readable status string.
     * Returns the number of bytes written (excluding null terminator).
     */
    uint32 GetStatus(char *buf, uint32 bufLen) const;

    /*
     * Fill statusArr (caller-allocated array of size maxEntries) with structured status.
     * Returns the number of entries written.
     */
    uint32 GetStatusArray(struct WatchDogThreadStatus *statusArr, uint32 maxEntries) const;

private:
    /* The main loop of the watchdog background thread */
    void WatchDogMain(PdbId pdbId);

    /* Check all entries once and emit alarms as needed */
    void CheckAllEntries();

    mutable std::mutex m_entriesMutex;
    WatchDogEntry *m_entries[WATCHDOG_MAX_ENTRIES];
    uint32 m_entryCount;

    std::thread *m_bgThread;
    std::mutex m_sleepMutex;
    std::condition_variable m_sleepCv;
    std::atomic<bool> m_needStop;
};

}  /* namespace DSTORE */

#endif  /* DSTORE_WATCHDOG_H */
