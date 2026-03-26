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
 * ut_distributed_undo_mgr.cpp
 *
 * IDENTIFICATION
 *        include/undo/dstore_rollback_trx_task_mgr.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSTORE_ROLLBACK_TRX_TASK_MGR_H
#define DSTORE_ROLLBACK_TRX_TASK_MGR_H

#include <atomic>
#include <mutex>
#include <condition_variable>

#include "common/algorithm/dstore_ilist.h"
#include "common/memory/dstore_mctx.h"
#include "transaction/dstore_transaction_types.h"
#include "framework/dstore_instance.h"
#include "undo/dstore_undo_zone.h"
#include "framework/dstore_watchdog.h"

namespace DSTORE {

#ifdef UT
#define private public
#endif

constexpr uint32 MAX_ROLLBACK_WORKER_NUM = 10;
struct RollbackTrxTask : public BaseObject {
    dlist_node nodeInList;
    Xid rollbackXid;
    UndoZone *rollbackUndoZone;

    bool operator==(const RollbackTrxTask &task) const
    {
        return rollbackXid == task.rollbackXid && rollbackUndoZone == task.rollbackUndoZone;
    }

    bool operator!=(const RollbackTrxTask &task) const
    {
        return !(*this == task);
    }

    RollbackTrxTask() : rollbackUndoZone(nullptr)
    {
        DListNodeInit(&nodeInList);
    }
    RollbackTrxTask(Xid xid, UndoZone *undoZone) : rollbackXid(xid), rollbackUndoZone(undoZone)
    {
        DListNodeInit(&nodeInList);
    }

    dlist_node *GetNodeInList();
    static RollbackTrxTask *GetRollbackTrxTaskFromNodeInList(dlist_node *node);
};

class RollbackTrxWorker;

class RollbackTrxTaskMgr : public BaseObject {
public:
    RollbackTrxTaskMgr(PdbId pdbId);
    RetStatus Initialize();
    void Destroy();
    ~RollbackTrxTaskMgr();

    PdbId GetPdbId();

    RetStatus AddRollbackTrxTask(Xid rollbackXid, UndoZone *rollbackUndoZone);
    bool IsDispatching() const
    {
        return m_isDispatching;
    }

    void StartDispatch();
    void StopDispatch();
    void WakeupDispatch();

    bool IsAllTaskFinished();

private:
    DstoreMemoryContext m_memoryContext;

    dlist_head m_rollbackTrxTaskQueue;
    DstoreSpinLock m_queueSpinlock;

    uint32 m_maxWorkerNum;
    RollbackTrxWorker **m_workers;
    uint32 m_nextWorkerNumForScan;

    std::mutex m_notifyMtx;
    std::condition_variable m_notifyCv;

    static const uint32 m_defaultSleepSeconds = 1;
    uint32 m_currSleepSeconds;
    static const uint32 m_maxSleepSeconds = 300;

    std::thread *m_dispatchThread;

    std::atomic<bool> m_needStop;
    std::atomic<bool> m_isDispatching;
    PdbId m_pdbId;
    /* WatchDog heartbeat: updated each main-loop iteration (0 = not running) */
    std::atomic<uint64> m_lastHeartbeatTime;
    WatchDogEntry m_watchdogEntry;

    void DispatchMain(PdbId pdbId);
    void DoDispatch();
    RollbackTrxWorker *GetNextIdleWorker();
    RollbackTrxTask *GetNextRollbackTrxTask();
    bool IsAllWorkerIdle() const;
};

#ifdef UT
#undef private
#endif

} /* namespace DSTORE */

#endif /* STORAGE_ROLLBACK_TRX_TASK_MGR_H */
