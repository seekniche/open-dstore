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
 * dstore_watchdog.cpp
 *
 * IDENTIFICATION
 *        src/framework/dstore_watchdog.cpp
 *
 * ---------------------------------------------------------------------------------------
 */
#include "framework/dstore_watchdog.h"
#include "diagnose/dstore_watchdog_diagnose.h"
#include "common/log/dstore_log.h"
#include "framework/dstore_instance.h"

namespace DSTORE {

WatchDogMgr::WatchDogMgr()
    : m_entryCount(0), m_bgThread(nullptr), m_needStop(false)
{
    for (uint32 i = 0; i < WATCHDOG_MAX_ENTRIES; i++) {
        m_entries[i] = nullptr;
    }
}

WatchDogMgr::~WatchDogMgr()
{
    Stop();
}

void WatchDogMgr::Register(WatchDogEntry *entry)
{
    StorageAssert(entry != nullptr);
    std::lock_guard<std::mutex> lock(m_entriesMutex);
    StorageAssert(m_entryCount < WATCHDOG_MAX_ENTRIES);
    m_entries[m_entryCount++] = entry;
    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog registered thread: %s, pdbId=%u, timeout=%lus.",
               entry->threadName, entry->pdbId, entry->timeoutSeconds));
}

void WatchDogMgr::Unregister(WatchDogEntry *entry)
{
    StorageAssert(entry != nullptr);
    std::lock_guard<std::mutex> lock(m_entriesMutex);
    for (uint32 i = 0; i < m_entryCount; i++) {
        if (m_entries[i] == entry) {
            /* shift remaining entries down */
            for (uint32 j = i; j + 1 < m_entryCount; j++) {
                m_entries[j] = m_entries[j + 1];
            }
            m_entries[--m_entryCount] = nullptr;
            ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
                ErrMsg("WatchDog unregistered thread: %s, pdbId=%u.", entry->threadName, entry->pdbId));
            return;
        }
    }
}

void WatchDogMgr::Start(PdbId pdbId)
{
    m_needStop.store(false, std::memory_order_relaxed);
    m_bgThread = new std::thread(&WatchDogMgr::WatchDogMain, this, pdbId);
    ErrLog(DSTORE_LOG, MODULE_WATCHDOG, ErrMsg("WatchDog started, pdbId=%u.", pdbId));
}

void WatchDogMgr::Stop()
{
    if (m_bgThread == nullptr) {
        return;
    }
    m_needStop.store(true, std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lock(m_sleepMutex);
        m_sleepCv.notify_all();
    }
    m_bgThread->join();
    delete m_bgThread;
    m_bgThread = nullptr;
    ErrLog(DSTORE_LOG, MODULE_WATCHDOG, ErrMsg("WatchDog stopped."));
}

void WatchDogMgr::WatchDogMain(PdbId pdbId)
{
    InitSignalMask();
    (void)pthread_setname_np(pthread_self(), "WatchDog");

    ErrLog(DSTORE_LOG, MODULE_WATCHDOG, ErrMsg("WatchDog thread started, pdbId=%u.", pdbId));

    while (!m_needStop.load(std::memory_order_relaxed)) {
        /* sleep for WATCHDOG_CHECK_INTERVAL_S seconds, but wake early if Stop() is called */
        {
            std::unique_lock<std::mutex> lock(m_sleepMutex);
            m_sleepCv.wait_for(lock, std::chrono::seconds(WATCHDOG_CHECK_INTERVAL_S),
                [this]() { return m_needStop.load(std::memory_order_relaxed); });
        }
        if (m_needStop.load(std::memory_order_relaxed)) {
            break;
        }
        CheckAllEntries();
    }

    ErrLog(DSTORE_LOG, MODULE_WATCHDOG, ErrMsg("WatchDog thread stopped."));
}

void WatchDogMgr::CheckAllEntries()
{
    uint64 now = static_cast<uint64>(time(nullptr));

    std::lock_guard<std::mutex> lock(m_entriesMutex);
    for (uint32 i = 0; i < m_entryCount; i++) {
        WatchDogEntry *entry = m_entries[i];
        if (entry == nullptr || entry->heartbeatTimePtr == nullptr) {
            continue;
        }

        uint64 lastHb = entry->heartbeatTimePtr->load(std::memory_order_relaxed);
        if (lastHb == 0) {
            /* thread not yet started or already stopped — skip */
            continue;
        }

        /* Detect time going backwards (e.g. NTP adjustment) — treat as OK */
        if (now < lastHb) {
            continue;
        }

        uint64 elapsed = now - lastHb;
        if (elapsed <= entry->timeoutSeconds) {
            continue;
        }

        /* Thread is potentially stuck. Rate-limit alarms. */
        if (now - entry->lastAlarmTime < WATCHDOG_ALARM_INTERVAL_SECONDS) {
            continue;
        }
        entry->lastAlarmTime = now;

        ErrLog(DSTORE_ERROR, MODULE_WATCHDOG,
            ErrMsg("WatchDog ALARM: thread '%s' (pdbId=%u) may be stuck! "
                   "Last heartbeat %lus ago (threshold=%lus). "
                   "lastHeartbeat=%lu, now=%lu.",
                   entry->threadName, entry->pdbId,
                   elapsed, entry->timeoutSeconds,
                   lastHb, now));
    }
}

uint32 WatchDogMgr::GetStatus(char *buf, uint32 bufLen) const
{
    if (buf == nullptr || bufLen == 0) {
        return 0;
    }

    uint64 now = static_cast<uint64>(time(nullptr));
    uint32 written = 0;

    std::lock_guard<std::mutex> lock(m_entriesMutex);
    for (uint32 i = 0; i < m_entryCount; i++) {
        WatchDogEntry *entry = m_entries[i];
        if (entry == nullptr || entry->heartbeatTimePtr == nullptr) {
            continue;
        }

        uint64 lastHb = entry->heartbeatTimePtr->load(std::memory_order_relaxed);
        bool isStuck = false;
        uint64 elapsed = 0;

        if (lastHb != 0 && now >= lastHb) {
            elapsed = now - lastHb;
            isStuck = (elapsed > entry->timeoutSeconds);
        }

        int ret = snprintf_s(buf + written, bufLen - written, bufLen - written - 1,
            "  [%s] pdbId=%u lastHeartbeat=%lus ago timeout=%lus status=%s\n",
            entry->threadName, entry->pdbId,
            elapsed, entry->timeoutSeconds,
            (lastHb == 0) ? "NOT_STARTED" : (isStuck ? "STUCK" : "OK"));
        if (ret < 0 || static_cast<uint32>(ret) >= bufLen - written) {
            break;
        }
        written += static_cast<uint32>(ret);
    }
    return written;
}

uint32 WatchDogMgr::GetStatusArray(WatchDogThreadStatus *statusArr, uint32 maxEntries) const
{
    if (statusArr == nullptr || maxEntries == 0) {
        return 0;
    }

    uint64 now = static_cast<uint64>(time(nullptr));
    uint32 cnt = 0;

    std::lock_guard<std::mutex> lock(m_entriesMutex);
    for (uint32 i = 0; i < m_entryCount && cnt < maxEntries; i++) {
        WatchDogEntry *entry = m_entries[i];
        if (entry == nullptr || entry->heartbeatTimePtr == nullptr) {
            continue;
        }

        WatchDogThreadStatus &s = statusArr[cnt];
        errno_t rc = strncpy_s(s.name, sizeof(s.name), entry->threadName, sizeof(s.name) - 1);
        storage_securec_check(rc, "\0", "\0");
        s.pdbId = entry->pdbId;
        s.timeoutSecs = entry->timeoutSeconds;

        uint64 lastHb = entry->heartbeatTimePtr->load(std::memory_order_relaxed);
        if (lastHb == 0) {
            s.lastHeartbeatSecs = 0;
            s.isStuck = false;
        } else if (now >= lastHb) {
            s.lastHeartbeatSecs = now - lastHb;
            s.isStuck = (s.lastHeartbeatSecs > entry->timeoutSeconds);
        } else {
            s.lastHeartbeatSecs = 0;
            s.isStuck = false;
        }
        cnt++;
    }
    return cnt;
}

}  /* namespace DSTORE */
