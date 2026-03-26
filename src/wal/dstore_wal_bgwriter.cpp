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
 * dstore_wal_bgwriter.cpp
 *
 * IDENTIFICATION
 * src/gausskernel/dstore/src/wal/dstore_wal_bgwriter.cpp
 *
 * ---------------------------------------------------------------------------------------
 *
 */
#include "common/log/dstore_log.h"
#include "framework/dstore_thread_autobinder_interface.h"
#include "wal/dstore_wal_logstream.h"
#include "wal/dstore_wal_utils.h"
#include "wal/dstore_wal_perf_unit.h"
#include "wal/dstore_wal_bgwriter.h"

namespace DSTORE {
static constexpr timespec INVALID_TIME = {0, 0};
static constexpr timespec MAX_NO_FLUSH_TIME = {0, 1000 * 1000 * 10}; // more than this time no flush data lead to sleep.
static constexpr int SLEEP_DURATION_MILLISECONDS = 10;

BgWalWriter::BgWalWriter(DstoreMemoryContext mct, WalStream *stream, PdbId pdbId)
    : m_needStop(false),
    m_isRunning(false),
    m_isSleeping(false),
    m_bgThread(nullptr),
    m_firstEmptyFlushTime(INVALID_TIME),
    m_memoryContext(mct),
    m_stream(stream),
    m_pdbId(pdbId),
    m_isInThrottling(false),
    m_lastHeartbeatTime(0),
    m_watchdogEntry("BgWalWriter", &m_lastHeartbeatTime, WATCHDOG_WAL_BGWRITER_TIMEOUT_S, pdbId)
{
}

BgWalWriter::~BgWalWriter()
{
    m_bgThread = nullptr;
    m_memoryContext = nullptr;
    m_stream = nullptr;
}

RetStatus BgWalWriter::Init()
{
    if (m_memoryContext == nullptr || m_stream == nullptr) {
        storage_set_error(WAL_ERROR_INVALID_PARAM);
        return DSTORE_FAIL;
    }
    /* Step 1: allocate thread related resource */

    /* Step 2: register into ThreadCoreMgr */
    return DSTORE_SUCC;
}

void FlushWorkThd(BgWalWriter *bgWalWriter, PdbId pdbId)
{
    WalUtils::SignalBlock();

    (void)g_storageInstance->CreateThreadAndRegister(pdbId, false, "BgWalWriter", true,
                                                     ThreadMemoryLevel::THREADMEM_HIGH_PRIORITY);
    (void)pthread_setname_np(pthread_self(), "BgWalWriter");
    if (!IsTemplate(pdbId)) {
        if (STORAGE_FUNC_FAIL(RegisterThreadToBind(pthread_self(), BindType::CPU_BIND, CoreBindLevel::HIGH))) {
            ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("BgWalWriter RegisterThreadToBind fail"));
        }

        int bindCpu = g_storageInstance->GetGuc()->walwriterCpuBind;
        cpu_set_t walWriterSet;
        if (bindCpu >= 0) {
            Size binCpuTarget = static_cast<Size>(bindCpu);
            CPU_ZERO(&walWriterSet);
            CPU_SET(binCpuTarget, &walWriterSet);
            int rc = sched_setaffinity(0, sizeof(cpu_set_t), &walWriterSet);
            if (rc == -1) {
                ErrLog(DSTORE_ERROR, MODULE_WAL,
                       ErrMsg("BgWalWriter bind cpu:%lu failed, ErrorCode:%d.", binCpuTarget, errno));
            }
            CPU_ZERO(&walWriterSet);
            if (pthread_getaffinity_np(pthread_self(), sizeof(walWriterSet), &walWriterSet) != 0) {
                ErrLog(DSTORE_WARNING, MODULE_WAL, ErrMsg("BgWalWriter get affinity failed."));
            }
            if (!CPU_ISSET(binCpuTarget, &walWriterSet)) {
                ErrLog(DSTORE_WARNING, MODULE_WAL, ErrMsg("BgWalWriter bind cpu:%lu failed.", binCpuTarget));
            }
        }
    }
    {
        StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
        WatchDogMgr *watchdogMgr = (pdb != nullptr) ? pdb->GetWatchDogMgr() : nullptr;
        if (watchdogMgr != nullptr) {
            watchdogMgr->Register(&bgWalWriter->m_watchdogEntry);
        }
        bgWalWriter->BgFlushMain();
        if (watchdogMgr != nullptr) {
            watchdogMgr->Unregister(&bgWalWriter->m_watchdogEntry);
        }
    }
    if (STORAGE_FUNC_FAIL(UnRegisterThreadToBind(pthread_self(), BindType::CPU_BIND))) {
        ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("BgWalWriter UnRegisterThreadToBind fail"));
    }
    g_storageInstance->UnregisterThread();
}

void BgWalWriter::Run()
{
    {
        std::unique_lock<std::mutex> lock(m_runningStateMtx);
        /* Step 1: return if already running */
        if (m_isRunning) {
            return;
        }
        /* Step 2: first set isRunning to true, then later Stop can feel correct state */
        m_isRunning = true;
    }
    /* Step 3: Register FlushWork to thread */
    m_bgThread = new std::thread(FlushWorkThd, this, m_pdbId);
    StorageReleasePanic(
        m_bgThread == nullptr, MODULE_WAL,
        ErrMsg("Start BgWalWriter thread failed, pdb %u walstream %lu.", m_pdbId, m_stream->GetWalId()));
    ErrLog(DSTORE_WARNING, MODULE_FRAMEWORK, ErrMsg("Start BgWalWriter thread %lu for pdb %u walstream %lu",
        m_bgThread->native_handle(), m_pdbId, m_stream->GetWalId()));

    /* Step 4: thread start */
}

void BgWalWriter::Stop() noexcept
{
    std::unique_lock<std::mutex> lock(m_runningStateMtx);
    /* Step 1: return if not running */
    if (!m_isRunning) {
        return;
    }

    /* Step 2: set needStop to true */
    m_needStop.store(true, std::memory_order_relaxed);
    ErrLog(DSTORE_LOG, MODULE_WAL,
           ErrMsg("[PDB:%u, WAL:%lu]BgWalWriter set needStop ture", m_pdbId, m_stream->GetWalId()));
    /* Step 3: wait for thread stop, even need wait it start first if thread main not running  */
    while (m_isRunning) {
        m_runningStateCv.wait(lock); /* wait until thread stop */
    }
    if (m_bgThread != nullptr) {
        m_bgThread->join();
        delete m_bgThread;
        m_bgThread = nullptr;
    }
    /* Step 4: set needStop to false, not influence next time run */
    m_needStop.store(false, std::memory_order_relaxed);
    ErrLog(DSTORE_LOG, MODULE_WAL,
           ErrMsg("[PDB:%u, WAL:%lu]BgWalWriter set needStop false after stopping", m_pdbId, m_stream->GetWalId()));
}

void BgWalWriter::BgFlushMain()
{
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("[PDB:%u, WAL:%lu]BgWalWriter start", m_pdbId, m_stream->GetWalId()));
    /* Step 1: keep doing main work if not needStop */
    while (likely(!m_needStop.load(std::memory_order_relaxed))) {
        /* Update watchdog heartbeat at the start of each iteration */
        m_lastHeartbeatTime.store(static_cast<uint64>(time(nullptr)), std::memory_order_relaxed);
        /* record the start time */
        double initStart = static_cast<double>(GetSystemTimeInMicrosecond());
        uint64 flushedDataLen = m_stream->Flush();
        double spendTimeInMs = (static_cast<double>(GetSystemTimeInMicrosecond()) - initStart) / 1000;
        StoragePdb *pdb = g_storageInstance->GetPdb(m_pdbId);
        StorageReleasePanic(pdb == nullptr, MODULE_WAL, ErrMsg("BgFlushMain get pdb failed, pdbId(%u).", m_pdbId));
        /* only standby print the flushing speed log */
        if (pdb != nullptr && pdb->IsInit() && pdb->GetPdbRoleMode() == PdbRoleMode::PDB_STANDBY
            && flushedDataLen > 0) {
            ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("Flush Wal speed: %f plsn / ms.",
                static_cast<double>(flushedDataLen) / spendTimeInMs));
        }
        SleepIfNecessary(flushedDataLen);
    }
    /* Step 2: wait async flush finish */
    uint64 maxWrittenToFilePlsn = m_stream->GetMaxWrittenToFilePlsn();
    uint64 maxFlushedPlsn = m_stream->GetMaxFlushedPlsn();
    uint64 count = 0;
    const int64 reportCount = 100000;
    while (maxWrittenToFilePlsn > maxFlushedPlsn) {
        if (count++ % reportCount == 0) {
            ErrLog(DSTORE_LOG, MODULE_WAL,
                   ErrMsg("[PDB:%u, WAL:%lu]BgWalWriter wait async flush finish, maxWrittenToFilePlsn %lu, "
                          "maxFlushedPlsn %lu",
                          m_pdbId, m_stream->GetWalId(), maxWrittenToFilePlsn, maxFlushedPlsn));
        }
        GaussUsleep(SLEEP_DURATION_MILLISECONDS);
        maxWrittenToFilePlsn = m_stream->GetMaxWrittenToFilePlsn();
        maxFlushedPlsn = m_stream->GetMaxFlushedPlsn();
    }
    /* Step 3: finally set isRunning to false */
    {
        std::unique_lock<std::mutex> lock(m_runningStateMtx);
        m_isRunning = false;
    }
    /* Clear heartbeat so watchdog knows the thread has stopped */
    m_lastHeartbeatTime.store(0, std::memory_order_relaxed);
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("[PDB:%u, WAL:%lu]BgWalWriter stop", m_pdbId, m_stream->GetWalId()));
    /* Step 3: notify caller of BgWalWriter::Stop */
    m_runningStateCv.notify_all();
}

constexpr float HIGH_WATER = 0.75;
constexpr int BYTE_PER_KB = 1024;
constexpr int THROTTLING_LIMIT_STEP = 1000; /* 1ms */
constexpr int THROTTLING_LIMIT_TOTAL = 10 * 1000; /* 10ms */
void BgWalWriter::WalThrottling()
{
    StoragePdb *pdb = g_storageInstance->GetPdb(m_pdbId);
    if (STORAGE_VAR_NULL(pdb) || !pdb->IsInit() || pdb->GetPdbRoleMode() == PdbRoleMode::PDB_STANDBY) {
        return;
    }

    static uint64 count = 0;
    constexpr uint32 step = 1000;
    long totalTime = 0;
    constexpr double epsilon = 1e-3;
    constexpr double lsnPerCeil = 20.0;
    uint64 minLsn = INVALID_PLSN;
    uint64 curLsn = INVALID_PLSN;
    double lsnPercent = 1.0;
    while (lsnPercent >= 1.0 && totalTime < THROTTLING_LIMIT_TOTAL && !m_needStop.load(std::memory_order_acquire)) {
        BgDiskPageMasterWriter *pageWriter = pdb->GetBgDiskPageMasterWriter();
        if (STORAGE_VAR_NULL(pageWriter) || pageWriter->IsStop()) {
            m_needThrottleTrxn = false;
            return;
        }
        minLsn = pageWriter->GetMinRecoveryPlsn();
        curLsn = m_stream->GetMaxAppendedPlsn();
        lsnPercent = static_cast<double>(curLsn - minLsn) /
                     (static_cast<double>(g_storageInstance->GetGuc()->walThrottlingSize) * BYTE_PER_KB);
        if (lsnPercent > lsnPerCeil) {
            if (!m_needThrottleTrxn) {
                m_needThrottleTrxn = true;
                ErrLog(DSTORE_LOG, MODULE_WAL,
                       ErrMsg("Transaction Throttling startup: pdbid %u, curLsn %lu, minLsn %lu, wal size %ld KB, "
                              "lsnPercent: %.2f.",
                              m_pdbId, curLsn, minLsn, (curLsn - minLsn) / BYTE_PER_KB, lsnPercent));
            }
        } else {
            if (m_needThrottleTrxn) {
                m_needThrottleTrxn = false;
                ErrLog(DSTORE_LOG, MODULE_WAL,
                       ErrMsg("Transaction Throttling ended: pdbid %u, curLsn %lu, minLsn %lu, wal size %ld KB, "
                              "lsnPercent: %.2f.",
                              m_pdbId, curLsn, minLsn, (curLsn - minLsn) / BYTE_PER_KB, lsnPercent));
            }
        }
        if (unlikely(lsnPercent > HIGH_WATER && (lsnPercent - HIGH_WATER) > epsilon)) {
            /* linear interpolation */
            long sleepus = static_cast<long>((lsnPercent - HIGH_WATER) / (1 - HIGH_WATER) * THROTTLING_LIMIT_STEP);
            if (!m_isInThrottling) {
                ErrLog(DSTORE_LOG, MODULE_WAL,
                       ErrMsg("Throttling startup: pdbid %u, curLsn %lu, minLsn %lu, wal size %ld KB, "
                              "lsnPercent: %.2f.",
                              m_pdbId, curLsn, minLsn, (curLsn - minLsn) / BYTE_PER_KB, lsnPercent));
                m_isInThrottling = true;
                count = 0;
            }
            sleepus = DstoreMin(sleepus, THROTTLING_LIMIT_STEP);
            GaussUsleep(sleepus);
            if (count % step == 0) {
                ErrLog(DSTORE_LOG, MODULE_WAL,
                       ErrMsg("Throttling: pdbid %u, already sleep %ld us, curLsn %lu, minLsn %lu, wal size %ld KB, "
                              "lsnPercent: %.2f.",
                              m_pdbId, totalTime, curLsn, minLsn, (curLsn - minLsn) / BYTE_PER_KB, lsnPercent));
            }
            count++;
            totalTime += static_cast<int>(sleepus);
        } else if (unlikely(m_isInThrottling)) {
            ErrLog(
                DSTORE_LOG, MODULE_WAL,
                ErrMsg("Throttling ended: pdbid %u, eventually sleep %ld us, curLsn %lu, minLsn %lu,wal size %ld KB, "
                       "lsnPercent: %.2f.",
                       m_pdbId, totalTime, curLsn, minLsn, (curLsn - minLsn) / BYTE_PER_KB, lsnPercent));
            m_isInThrottling = false;
        }
    }
}

void BgWalWriter::SleepIfNecessary(uint64 flushedDataLen)
{
    WalThrottling();

    if (likely(flushedDataLen != 0)) {
        m_firstEmptyFlushTime = INVALID_TIME;
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        storage_set_error(WAL_ERROR_UNREACHABLE_CODE);
        ErrLog(DSTORE_WARNING, MODULE_WAL, ErrMsg("Get time failed, may lead wal bg writer don't sleep."));
        return;
    }
    if (IsInvalidTime(m_firstEmptyFlushTime)) {
        m_firstEmptyFlushTime = now;
        return;
    }
    if (WalUtils::IsOvertimed(m_firstEmptyFlushTime, now, MAX_NO_FLUSH_TIME)) {
        std::unique_lock<std::mutex> sleepLock(m_sleepMtx);
        m_isSleeping.store(true, std::memory_order_relaxed);
        (void)m_sleepCv.wait_for(sleepLock, std::chrono::milliseconds(SLEEP_DURATION_MILLISECONDS));
        m_isSleeping.store(false, std::memory_order_relaxed);
    }
}

void BgWalWriter::WakeUpIfSleeping()
{
    if (unlikely(m_isSleeping.load(std::memory_order_relaxed))) {
        std::unique_lock<std::mutex> sleepLock(m_sleepMtx);
        m_sleepCv.notify_all();
    }
}

bool BgWalWriter::IsInvalidTime(const timespec &time) const
{
    if (time.tv_sec == INVALID_TIME.tv_sec && time.tv_nsec == INVALID_TIME.tv_nsec) {
        return true;
    }
    return false;
}
}
