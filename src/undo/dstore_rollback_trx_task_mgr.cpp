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
 * dstore_rollback_trx_task_mgr.cpp
 *     This file defines the building blocks that bg page writer needs.
 *
 * IDENTIFICATION
 *        src/undo/dstore_rollback_trx_task_mgr.cpp
 *
 * ---------------------------------------------------------------------------------------
 */

#include <csignal>

#include "types/data_types.h"
#include "undo/dstore_rollback_trx_worker.h"
#include "undo/dstore_rollback_trx_task_mgr.h"

namespace DSTORE {

dlist_node *RollbackTrxTask::GetNodeInList()
{
    return &nodeInList;
}

RollbackTrxTask *RollbackTrxTask::GetRollbackTrxTaskFromNodeInList(dlist_node *node)
{
    return static_cast<RollbackTrxTask *>(dlist_container(RollbackTrxTask, nodeInList, node));
}

RollbackTrxTaskMgr::RollbackTrxTaskMgr(PdbId pdbId)
    : m_memoryContext(nullptr),
      m_rollbackTrxTaskQueue(),
      m_queueSpinlock(),
      m_maxWorkerNum{MAX_ROLLBACK_WORKER_NUM},
      m_workers(nullptr),
      m_nextWorkerNumForScan(0),
      m_notifyMtx(),
      m_notifyCv(),
      m_currSleepSeconds(m_defaultSleepSeconds),
      m_dispatchThread(nullptr),
      m_needStop(false),
      m_isDispatching(false),
      m_pdbId(pdbId),
      m_lastHeartbeatTime(0),
      m_watchdogEntry("RollbackTrxMgr", &m_lastHeartbeatTime, WATCHDOG_UNDO_RECYCLE_TIMEOUT_S, pdbId)
{}

RollbackTrxTaskMgr::~RollbackTrxTaskMgr()
{
    m_dispatchThread = nullptr;
    m_workers = nullptr;
    m_memoryContext = nullptr;
}

RetStatus RollbackTrxTaskMgr::Initialize()
{
    m_memoryContext = DstoreAllocSetContextCreate(
        g_storageInstance->GetMemoryMgr()->GetGroupContext(MEMORY_CONTEXT_TRANSACTION),
        "RollbackTrxRequestMgrMemoryContext",
        ALLOCSET_DEFAULT_MINSIZE, ALLOCSET_DEFAULT_INITSIZE, ALLOCSET_DEFAULT_INITSIZE,
        MemoryContextType::SHARED_CONTEXT);
    if (STORAGE_VAR_NULL(m_memoryContext)) {
        ErrLog(DSTORE_ERROR, MODULE_UNDO, ErrMsg("Alloc memory for rollback task mgr memory context fail."));
        return DSTORE_FAIL;
    }

    DListInit(&m_rollbackTrxTaskQueue);
    m_queueSpinlock.Init();

    m_workers = static_cast<RollbackTrxWorker **>(
        DstoreMemoryContextAlloc(m_memoryContext, m_maxWorkerNum * sizeof(RollbackTrxWorker *)));
    if (STORAGE_VAR_NULL(m_workers)) {
        DstoreMemoryContextDelete(m_memoryContext);
        m_memoryContext = nullptr;
        ErrLog(DSTORE_ERROR, MODULE_UNDO, ErrMsg("Alloc memory for rollback task worker fail."));
        return DSTORE_FAIL;
    }
    uint32 i = 0;
    for (; i < m_maxWorkerNum; ++i) {
        m_workers[i] = DstoreNew(m_memoryContext) RollbackTrxWorker(m_memoryContext, this, m_pdbId);
        if (STORAGE_VAR_NULL(m_workers[i])) {
            ErrLog(DSTORE_WARNING, MODULE_UNDO, ErrMsg("Alloc memory for rollback task worker(%u) fail.", i));
            break;
        }
    }
    if (unlikely(i == 0)) {
        DstorePfree(m_workers);
        m_workers = nullptr;
        DstoreMemoryContextDelete(m_memoryContext);
        m_memoryContext = nullptr;
        return DSTORE_FAIL;
    }
    if (unlikely(i < m_maxWorkerNum)) {
        m_maxWorkerNum = i;
        ErrLog(DSTORE_WARNING, MODULE_UNDO, ErrMsg("Init rollback task worker num(%u).", i));
    }
    return DSTORE_SUCC;
}

void RollbackTrxTaskMgr::Destroy()
{
    for (uint32 i = 0; i < m_maxWorkerNum; ++i) {
        if (m_workers == nullptr) {
            break;
        }

        if (m_workers[i] == nullptr) {
            continue;
        }
        delete m_workers[i];
        m_workers[i] = nullptr;
    }

    if (m_workers != nullptr) {
        DstorePfree(m_workers);
        m_workers = nullptr;
    }

    if (m_memoryContext != nullptr) {
        DstoreMemoryContextDelete(m_memoryContext);
        m_memoryContext = nullptr;
    }

    if (m_dispatchThread != nullptr) {
        delete m_dispatchThread;
        m_dispatchThread = nullptr;
    }
}

PdbId RollbackTrxTaskMgr::GetPdbId()
{
    return m_pdbId;
}

RetStatus RollbackTrxTaskMgr::AddRollbackTrxTask(Xid rollbackXid, UndoZone *rollbackUndoZone)
{
    RollbackTrxTask *rollbackTask = DstoreNew(m_memoryContext) RollbackTrxTask(rollbackXid, rollbackUndoZone);
    if (unlikely(rollbackTask == nullptr)) {
        ErrLog(DSTORE_ERROR, MODULE_UNDO,
               ErrMsg("Alloc memory for rollback task fail, xid(%d, %lu)", static_cast<int>(rollbackXid.m_zoneId),
                      rollbackXid.m_logicSlotId));
        return DSTORE_FAIL;
    }

    m_queueSpinlock.Acquire();
    DListPushTail(&m_rollbackTrxTaskQueue, rollbackTask->GetNodeInList());
    m_queueSpinlock.Release();
    WakeupDispatch();
    ErrLog(DSTORE_DEBUG1, MODULE_UNDO, ErrMsg("Async rollback add rollback task, xid(%d, %lu).",
        static_cast<int>(rollbackXid.m_zoneId), rollbackXid.m_logicSlotId));
    return DSTORE_SUCC;
}

void RollbackTrxTaskMgr::StartDispatch()
{
    m_isDispatching.store(true, std::memory_order_release);
    StorageAssert(m_dispatchThread == nullptr);
    m_dispatchThread = new std::thread(&RollbackTrxTaskMgr::DispatchMain, this, m_pdbId);
    m_dispatchThread->detach();
}

void RollbackTrxTaskMgr::StopDispatch()
{
    m_needStop.store(true, std::memory_order_release);
}

void RollbackTrxTaskMgr::WakeupDispatch()
{
    std::unique_lock<std::mutex> lock(m_notifyMtx);
    m_notifyCv.notify_one();
}

void RollbackTrxTaskMgr::DispatchMain(PdbId pdbId)
{
    InitSignalMask();

    StorageAssert(thrd == nullptr);
    thrd = DstoreNew(m_memoryContext) ThreadContext();
    StorageReleasePanic(thrd == nullptr, MODULE_UNDO, ErrMsg("alloc memory for thrd fail!"));
    thrd->SetThreadMemLevel(ThreadMemoryLevel::THREADMEM_HIGH_PRIORITY);
    if (STORAGE_FUNC_FAIL(thrd->InitializeBasic())) {
        g_storageInstance->RemoveVisibleThread(thrd);
        thrd->Destroy();
        DstorePfree(thrd);
        thrd = nullptr;
        ErrLog(DSTORE_ERROR, MODULE_COMMON, ErrMsg("Failed to initialize basic thread context."));
        return;
    }
    if (STORAGE_FUNC_FAIL(thrd->InitStorageContext(pdbId))) {
        g_storageInstance->RemoveVisibleThread(thrd);
        thrd->Destroy();
        DstorePfree(thrd);
        thrd = nullptr;
        ErrLog(DSTORE_ERROR, MODULE_COMMON, ErrMsg("Failed to initialize sotrage context."));
        return;
    }
    g_storageInstance->AddVisibleThread(thrd, pdbId, "RollbackTrxMgr");

    (void)pthread_setname_np(pthread_self(), "RollbackTrxMgr");

    {
        StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
        WatchDogMgr *watchdogMgr = (pdb != nullptr) ? pdb->GetWatchDogMgr() : nullptr;
        if (watchdogMgr != nullptr) {
            watchdogMgr->Register(&m_watchdogEntry);
        }
        DoDispatch();
        /* Clear heartbeat so watchdog knows the thread has stopped */
        m_lastHeartbeatTime.store(0, std::memory_order_relaxed);
        if (watchdogMgr != nullptr) {
            watchdogMgr->Unregister(&m_watchdogEntry);
        }
    }

    if (thrd != nullptr) {
        g_storageInstance->RemoveVisibleThread(thrd);
        thrd->Destroy();
        DstorePfree(thrd);
        thrd = nullptr;
    }
    m_isDispatching.store(false, std::memory_order_release);
}

void RollbackTrxTaskMgr::DoDispatch()
{
Dispatch:
    /* Update watchdog heartbeat at the start of each iteration */
    m_lastHeartbeatTime.store(static_cast<uint64>(time(nullptr)), std::memory_order_relaxed);
    if (m_needStop.load(std::memory_order_acquire) && IsAllTaskFinished()) {
        return;
    }
    RollbackTrxWorker *idleWorker = nullptr;
    RollbackTrxTask *rollbackTask = nullptr;
    idleWorker = GetNextIdleWorker();
    if (idleWorker == nullptr) {
        goto DispatchSleep;
    }
    rollbackTask = GetNextRollbackTrxTask();
    if (rollbackTask == nullptr) {
        goto DispatchSleep;
    }
    idleWorker->SetTask(rollbackTask);
    idleWorker->Run();
    m_currSleepSeconds = m_defaultSleepSeconds;

DispatchSleep:
    std::unique_lock<std::mutex> lock(m_notifyMtx);
    (void)m_notifyCv.wait_for(lock, std::chrono::seconds(m_currSleepSeconds));
    m_currSleepSeconds = Min(2 * m_currSleepSeconds, m_maxSleepSeconds);
    goto Dispatch;
}

RollbackTrxWorker *RollbackTrxTaskMgr::GetNextIdleWorker()
{
    for (uint32 i = m_nextWorkerNumForScan; i < m_maxWorkerNum; ++i) {
        if (!m_workers[i]->IsRunning()) {
            m_nextWorkerNumForScan = i;
            return m_workers[i];
        }
    }
    m_nextWorkerNumForScan = 0;
    return nullptr;
}

RollbackTrxTask *RollbackTrxTaskMgr::GetNextRollbackTrxTask()
{
    m_queueSpinlock.Acquire();
    if (DListIsEmpty(&m_rollbackTrxTaskQueue)) {
        m_queueSpinlock.Release();
        return nullptr;
    }
    dlist_node *node = DListPopHeadNode(&m_rollbackTrxTaskQueue);
    m_queueSpinlock.Release();
    return RollbackTrxTask::GetRollbackTrxTaskFromNodeInList(node);
}

bool RollbackTrxTaskMgr::IsAllWorkerIdle() const
{
    for (uint32 i = 0; i < m_maxWorkerNum; ++i) {
        if (m_workers[i]->IsRunning()) {
            return false;
        }
    }
    return true;
}

bool RollbackTrxTaskMgr::IsAllTaskFinished()
{
    m_queueSpinlock.Acquire();
    bool ret = DListIsEmpty(&m_rollbackTrxTaskQueue);
    m_queueSpinlock.Release();
    return ret && IsAllWorkerIdle();
}

}  // namespace DSTORE
