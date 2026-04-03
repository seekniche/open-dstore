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

#include "framework/dstore_watchdog.h"
#include "framework/dstore_instance.h"
#include "common/log/dstore_log.h"
#include "securec.h"

#ifdef UT
#include "common/fault_injection/dstore_watchdog_fault_injection.h"
#endif

namespace DSTORE {

constexpr uint32 WatchDogMgr::MAX_WATCHED_THREADS;

static const uint32 DEFAULT_TIMEOUT_SEC = 60;
static const uint32 US_PER_SEC = 1000000;
static const uint32 SLEEP_CHUNK_MS = 500;

WatchDogMgr::WatchDogMgr()
    : m_pdbId(INVALID_PDB_ID), m_stop(false), m_ready(false), m_watchdogThread(nullptr), m_registeredCount(0)
{
    for (uint32 i = 0; i < MAX_WATCHED_THREADS; i++) {
        m_heartbeats[i].lastHeartbeatUs.store(0, std::memory_order_relaxed);
        m_heartbeats[i].runState.store(ThreadRunState::NOT_STARTED, std::memory_order_relaxed);
        m_heartbeats[i].healthState.store(ThreadHealthState::HEALTHY, std::memory_order_relaxed);
        m_heartbeats[i].threadType = WatchDogThreadType::WATCHDOG_THREAD_TYPE_COUNT;
        m_heartbeats[i].threadIndex = 0;
        m_heartbeats[i].threadName[0] = '\0';
        m_heartbeats[i].progressMsg[0] = '\0';
        m_heartbeats[i].registered.store(false, std::memory_order_relaxed);
    }
}

WatchDogMgr::~WatchDogMgr()
{
    Destroy();
}

RetStatus WatchDogMgr::Init(PdbId pdbId)
{
    m_pdbId = pdbId;
    m_stop.store(false, std::memory_order_relaxed);
    m_registeredCount.store(0, std::memory_order_relaxed);

    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog: Init completed, pdbId=%u.", pdbId));

    return DSTORE_SUCC;
}

void WatchDogMgr::StartThread()
{
    if (m_watchdogThread == nullptr) {
        ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
            ErrMsg("WatchDog: starting watchdog thread, pdbId=%u.", m_pdbId));
        m_ready.store(false, std::memory_order_relaxed);
        m_watchdogThread = new std::thread(&WatchDogMgr::WatchDogThreadMain, this);
        WaitReady();
        ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
            ErrMsg("WatchDog: watchdog thread is ready, pdbId=%u.", m_pdbId));
    }
}

void WatchDogMgr::WaitReady() const
{
    while (!m_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void WatchDogMgr::Destroy()
{
    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog: Destroy started, pdbId=%u, registeredCount=%u.",
               m_pdbId, m_registeredCount.load(std::memory_order_relaxed)));

    m_stop.store(true, std::memory_order_release);
    if (m_watchdogThread != nullptr) {
        if (m_watchdogThread->joinable()) {
            m_watchdogThread->join();
        }
        delete m_watchdogThread;
        m_watchdogThread = nullptr;
    }

    for (uint32 i = 0; i < MAX_WATCHED_THREADS; i++) {
        m_heartbeats[i].registered.store(false, std::memory_order_relaxed);
    }
    m_registeredCount.store(0, std::memory_order_relaxed);
    m_ready.store(false, std::memory_order_relaxed);

    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog: Destroy completed, pdbId=%u.", m_pdbId));
}

WatchDogHandle *WatchDogMgr::Register(WatchDogThreadType type, uint32 index, const char *threadName)
{
#ifdef UT
    FAULT_INJECTION_RETURN(DstoreWatchDogFI::REGISTER_FAIL, nullptr);
#endif

    uint32 slot = m_registeredCount.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= MAX_WATCHED_THREADS) {
        m_registeredCount.fetch_sub(1, std::memory_order_relaxed);
        return nullptr;
    }

    WatchDogHandle *hb = &m_heartbeats[slot];
    hb->threadType = type;
    hb->threadIndex = index;

    if (threadName != nullptr) {
        error_t rc = strncpy_s(hb->threadName, sizeof(hb->threadName), threadName, sizeof(hb->threadName) - 1);
        storage_securec_check(rc, "\0", "\0");
    } else {
        hb->threadName[0] = '\0';
    }

    uint64 nowUs = GetSteadyClockUs();
    hb->lastHeartbeatUs.store(nowUs, std::memory_order_relaxed);
    hb->runState.store(ThreadRunState::NOT_STARTED, std::memory_order_relaxed);
    hb->healthState.store(ThreadHealthState::HEALTHY, std::memory_order_relaxed);
    hb->progressMsg[0] = '\0';
    hb->registered.store(true, std::memory_order_release);

    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog: registered thread '%s' (type=%d, index=%u, slot=%u), pdbId=%u.",
               hb->threadName, static_cast<int>(type), index, slot, m_pdbId));

    return hb;
}

void WatchDogMgr::Unregister(WatchDogHandle *heartbeat)
{
    if (heartbeat == nullptr) {
        return;
    }

    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog: unregistered thread '%s' (type=%d, index=%u), pdbId=%u.",
               heartbeat->threadName, static_cast<int>(heartbeat->threadType),
               heartbeat->threadIndex, m_pdbId));

    heartbeat->runState.store(ThreadRunState::STOPPED, std::memory_order_relaxed);
    heartbeat->registered.store(false, std::memory_order_release);
}

uint64 WatchDogMgr::GetSteadyClockUs()
{
    return static_cast<uint64>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64 WatchDogMgr::GetTimeoutUsFromGuc(WatchDogThreadType type) const
{
    const StorageGUC *guc = nullptr;
    if (g_storageInstance != nullptr) {
        guc = g_storageInstance->GetGuc();
    }
    uint32 timeoutSec = DEFAULT_TIMEOUT_SEC;

    if (guc != nullptr) {
        switch (type) {
            case WatchDogThreadType::BG_PAGE_MASTER_WRITER:
                timeoutSec = guc->watchdogMasterWriterTimeoutSec;
                break;
            case WatchDogThreadType::BG_PAGE_SLAVE_WRITER:
                timeoutSec = guc->watchdogSlaveWriterTimeoutSec;
                break;
            case WatchDogThreadType::CHECKPOINTER:
                timeoutSec = guc->watchdogCheckpointerTimeoutSec;
                break;
            case WatchDogThreadType::WAL_FILE_RECYCLE:
                timeoutSec = guc->watchdogWalRecycleTimeoutSec;
                break;
            case WatchDogThreadType::UNDO_RECYCLE:
                timeoutSec = guc->watchdogUndoRecycleTimeoutSec;
                break;
            case WatchDogThreadType::BTREE_RECYCLE:
                timeoutSec = guc->watchdogBtreeRecycleTimeoutSec;
                break;
            default:
                timeoutSec = DEFAULT_TIMEOUT_SEC;
                break;
        }
    }

    if (timeoutSec == 0) {
        timeoutSec = DEFAULT_TIMEOUT_SEC;
    }
    return static_cast<uint64>(timeoutSec) * US_PER_SEC;
}

void WatchDogMgr::ReportProgress(WatchDogHandle *hb, const char *msg)
{
    if (hb == nullptr) {
        return;
    }
    TouchHeartbeat(hb);
    if (msg != nullptr) {
        error_t rc = strncpy_s(hb->progressMsg, sizeof(hb->progressMsg), msg, sizeof(hb->progressMsg) - 1);
        storage_securec_check(rc, "\0", "\0");
    } else {
        hb->progressMsg[0] = '\0';
    }
}

void WatchDogMgr::CheckAllHeartbeats()
{
#ifdef UT
    FAULT_INJECTION_ACTION(DstoreWatchDogFI::CHECK_HEARTBEAT_SKIP, return);
#endif

    uint64 nowUs = GetSteadyClockUs();
    uint32 count = m_registeredCount.load(std::memory_order_acquire);

    for (uint32 i = 0; i < count && i < MAX_WATCHED_THREADS; i++) {
        WatchDogHandle *hb = &m_heartbeats[i];

        if (!hb->registered.load(std::memory_order_acquire)) {
            continue;
        }

        ThreadRunState state = hb->runState.load(std::memory_order_relaxed);
        if (state != ThreadRunState::RUNNING) {
            /* 非 RUNNING 状态(SLEEPING/NOT_STARTED/STOPPED)恢复健康 */
            hb->healthState.store(ThreadHealthState::HEALTHY, std::memory_order_relaxed);
            continue;
        }

        uint64 lastHb = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
        uint64 elapsed = nowUs - lastHb;
        uint64 timeoutUs = GetTimeoutUsFromGuc(hb->threadType);

        if (elapsed > timeoutUs) {
            /* 仅在首次检测到超时时打印告警（HEALTHY → TIMEOUT） */
            ThreadHealthState prevHealth = hb->healthState.load(std::memory_order_relaxed);
            if (prevHealth != ThreadHealthState::TIMEOUT) {
                ErrLog(DSTORE_ERROR, MODULE_WATCHDOG,
                    ErrMsg("WatchDog TIMEOUT DETECTED: thread '%s' (type=%d, index=%u) "
                           "no heartbeat for %lu us (timeout=%lu us), lastProgress='%s', pdbId=%u.",
                           hb->threadName, static_cast<int>(hb->threadType), hb->threadIndex,
                           elapsed, timeoutUs, hb->progressMsg, m_pdbId));
            }
            hb->healthState.store(ThreadHealthState::TIMEOUT, std::memory_order_relaxed);
        } else {
            hb->healthState.store(ThreadHealthState::HEALTHY, std::memory_order_relaxed);
        }
    }
}

void WatchDogMgr::WatchDogThreadMain()
{
    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog: watchdog thread started, pdbId=%u.", m_pdbId));
    m_ready.store(true, std::memory_order_release);
    while (!m_stop.load(std::memory_order_acquire)) {
        CheckAllHeartbeats();

        uint32 intervalSec = 5;
        const StorageGUC *guc = (g_storageInstance != nullptr) ? g_storageInstance->GetGuc() : nullptr;
        if (guc != nullptr) {
            intervalSec = guc->watchdogCheckIntervalSec;
        }
        if (intervalSec == 0) {
            intervalSec = 1;
        }

        uint32 totalChunks = (intervalSec * 1000) / SLEEP_CHUNK_MS;
        for (uint32 i = 0; i < totalChunks; i++) {
            if (m_stop.load(std::memory_order_relaxed)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_CHUNK_MS));
        }
    }
    ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
        ErrMsg("WatchDog: watchdog thread exiting, pdbId=%u.", m_pdbId));
}

uint32 WatchDogMgr::GetHeartbeatSnapshot(WatchDogHandle *outArray, uint32 maxCount) const
{
    uint32 count = m_registeredCount.load(std::memory_order_acquire);
    if (count > maxCount) {
        count = maxCount;
    }

    uint32 validCount = 0;
    for (uint32 i = 0; i < count && i < MAX_WATCHED_THREADS; i++) {
        const WatchDogHandle *src = &m_heartbeats[i];
        if (!src->registered.load(std::memory_order_acquire)) {
            continue;
        }

        WatchDogHandle *dst = &outArray[validCount];
        dst->lastHeartbeatUs.store(src->lastHeartbeatUs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->runState.store(src->runState.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->healthState.store(src->healthState.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->threadType = src->threadType;
        dst->threadIndex = src->threadIndex;
        error_t rc = strncpy_s(dst->threadName, sizeof(dst->threadName),
                               src->threadName, sizeof(dst->threadName) - 1);
        storage_securec_check(rc, "\0", "\0");
        rc = strncpy_s(dst->progressMsg, sizeof(dst->progressMsg),
                       src->progressMsg, sizeof(dst->progressMsg) - 1);
        storage_securec_check(rc, "\0", "\0");
        dst->registered.store(true, std::memory_order_relaxed);
        validCount++;
    }

    return validCount;
}

uint32 WatchDogMgr::GetHeartbeatsByType(WatchDogThreadType type,
                                         WatchDogHandle *outArray, uint32 maxCount) const
{
    uint32 count = m_registeredCount.load(std::memory_order_acquire);
    uint32 validCount = 0;

    for (uint32 i = 0; i < count && i < MAX_WATCHED_THREADS; i++) {
        const WatchDogHandle *src = &m_heartbeats[i];
        if (!src->registered.load(std::memory_order_acquire)) {
            continue;
        }
        if (src->threadType != type) {
            continue;
        }
        if (validCount >= maxCount) {
            break;
        }

        WatchDogHandle *dst = &outArray[validCount];
        dst->lastHeartbeatUs.store(src->lastHeartbeatUs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->runState.store(src->runState.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->healthState.store(src->healthState.load(std::memory_order_relaxed), std::memory_order_relaxed);
        dst->threadType = src->threadType;
        dst->threadIndex = src->threadIndex;
        error_t rc = strncpy_s(dst->threadName, sizeof(dst->threadName),
                               src->threadName, sizeof(dst->threadName) - 1);
        storage_securec_check(rc, "\0", "\0");
        rc = strncpy_s(dst->progressMsg, sizeof(dst->progressMsg),
                       src->progressMsg, sizeof(dst->progressMsg) - 1);
        storage_securec_check(rc, "\0", "\0");
        dst->registered.store(true, std::memory_order_relaxed);
        validCount++;
    }

    return validCount;
}

}  /* namespace DSTORE */
