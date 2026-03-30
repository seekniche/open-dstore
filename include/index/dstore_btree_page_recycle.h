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
 * dstore_btree_page_recycle.h
 *
 * IDENTIFICATION
 *        dstore/include/index/dstore_btree_page_recycle.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef SRC_GAUSSKERNEL_INCLUDE_INDEX_DSTORE_BTREE_PAGE_RECYCLE_H
#define SRC_GAUSSKERNEL_INCLUDE_INDEX_DSTORE_BTREE_PAGE_RECYCLE_H

#include <future>
#include "index/dstore_btree_page_unlink.h"
#include "framework/dstore_watchdog.h"

namespace DSTORE {

static constexpr int64 BTREE_RECYCLE_MAX_WAIT_TIME_IN_SEC = 30 * 60; /* We would wait at most 30 min */

class BtreePageRecycle : public BaseObject {
public:
    explicit BtreePageRecycle(StorageRelation indexRel, TimestampTz startTime = 0L) : m_indexRel(indexRel),
        m_bufMgr((indexRel->btreeSmgr != nullptr && indexRel->btreeSmgr->IsGlobalTempIndex()) ?
        thrd->GetTmpLocalBufMgr() : g_storageInstance->GetBufferMgr()),
        m_startTime(startTime)
    {}
    BtreePageRecycle() = delete;
    ~BtreePageRecycle() = default;

    /*
     * PutIntoRecycleQueueIfEmpty
     *
     * Mark a leaf page as EMPTY_HAS_PARENT_HAS_SIB page before deleting of the last tuple on page.
     * EMPTY_HAS_PARENT_HAS_SIB page will be added into PendingFreePageQueue immediately, but will be completely
     * recycled when calling BatchRecycleBtreePage if the page is still empty and recyclable at that time.
     */
    RetStatus PutIntoRecycleQueueIfEmpty(BufferDesc *pageBuf);

    /*
     * BatchRecycleBtreePage
     *
     * Using the current recycle CSN min, get dead pages from the RecycleQueue
     * and attempt to unlink the pages from
     * their parent and sibling pages. If successfully, the unlinked pages
     * will become reusable once the recycle CSN min move pass the latest CSN
     * at the time when the page is unlinked
     */
    RetStatus BatchRecycleBtreePage(IndexInfo *indexInfo, ScanKey scanKey, bool *needRetry = nullptr);

    /*
     * TryRegisterRecycleBtreeTask
     *
     * Register a btree page recycle tack if there isn't one in the task list. Note that the return value DSTORE_SUCC
     * does not means we registered the task successfully. The function may skip register randomly to avoid access
     * the background task thread too ofen.
     */
    static RetStatus TryRegisterRecycleBtreeTask(const Xid createXid, IndexInfo *indexInfo, ScanKey scanKey,
                                                 TablespaceId tbsId, const PageId segmentId, PdbId pdbId);
    static RetStatus TryRegisterRecycleBtreeTask(ObjSpaceMgrTaskInfo *taskInfo);
    static RetStatus TryRegisterColdRecyclePartitionReclaimTask(TablespaceId tbsId, const PageId segmentId,
                                                                const Xid createdXid, PdbId pdbId);

    inline BtreeStorageMgr *GetBtreeSmgr() const
    {
        return m_indexRel->btreeSmgr;
    }

    inline PdbId GetPdbId()
    {
        return m_indexRel->m_pdbId;
    }

private:
    static bool NeedRegisterTask(const TablespaceId tbsId, const PageId segmentId, const Xid createdXid,
                                 ObjSpaceMgrTaskType type, PdbId pdbId);

    RetStatus TryRecycleBtreePage(BtreePageUnlink &btrUnlink, uint64 *numRecycledPages = nullptr);

    bool NeedWal()
    {
        return (!(m_indexRel && GetBtreeSmgr() && GetBtreeSmgr()->IsGlobalTempIndex()));
    }

    StorageRelation  m_indexRel;
    BufMgrInterface *m_bufMgr;
    TimestampTz m_startTime;
};

#define BTREE_RECYCLE_WORKER_WAIT_INTERVAL (MICRO_PER_MILLI_SEC * 1000 * 60)

class BtreeRecycleWorker : public BaseObject {
public:
    explicit BtreeRecycleWorker(uint32 workerId, PdbId pdbId)
    {
        m_workeId = workerId;
        m_pdbId = pdbId;
        m_stopRecyleThread = false;
        m_btreeRecycleResult = DSTORE_FAIL;
        m_btrRecycleTask = nullptr;
        ErrLog(DSTORE_LOG, MODULE_INDEX,
               ErrMsg("[BtrRecycle] BtreeRecycleWorker start workerId[%u], pdbId[%u]", m_workeId, m_pdbId));
        m_recycleThreadCore = nullptr;
        m_recycleThreadStartTime = 0L;
        m_recycleThreadId = INVALID_THREAD_ID;
        m_recycleThread = nullptr;
    }
    ~BtreeRecycleWorker() = default;
    inline void SetTask(ObjSpaceMgrTask *task)
    {
        m_waitCnt = 0;
        m_btreeRecycleResult = DSTORE_FAIL;
        m_btrRecycleTask.store(task, std::memory_order_release);
    }
    inline void BtreeRecycleWorkerStop()
    {
        m_stopRecyleThread = true;
        if (IsThreadValid()) {
            m_recycleThreadCore->immediateAbort = true;
            m_recycleThreadCore->futex.DstoreFutexPost();
            m_recycleThread->join();
            delete m_recycleThread;
        }
        m_recycleThread = nullptr;
        m_recycleThreadCore = nullptr;
        m_recycleThreadId = INVALID_THREAD_ID;
        m_recycleThreadStartTime = 0L;

        ErrLog(DSTORE_LOG, MODULE_INDEX,
               ErrMsg("[BtrRecycle] BtreeRecycleWorkerStop workerId[%u], pdbId[%u]", m_workeId, m_pdbId));
    }
    RetStatus BtreeRecycleExecute(ObjSpaceMgrTask *task);
private:
    uint32 m_workeId;
    PdbId m_pdbId;
    bool m_stopRecyleThread;
    RetStatus m_btreeRecycleResult;
    std::atomic<ObjSpaceMgrTask*> m_btrRecycleTask;
    std::thread *m_recycleThread;
    uint32 m_waitCnt = 0;
    ThreadId m_recycleThreadId;
    ThreadCore *m_recycleThreadCore;
    TimestampTz m_recycleThreadStartTime;

    inline bool IsThreadValid() const
    {
        if (STORAGE_VAR_NULL(m_recycleThread) || unlikely(!m_recycleThread->joinable())) {
            return false;
        }
        if (STORAGE_VAR_NULL(m_recycleThreadCore)) {
            return false;
        }
        return m_recycleThreadCore->pid == m_recycleThreadId &&
            m_recycleThreadCore->startTime == m_recycleThreadStartTime;
    }

    /* Index may need access SQL thrd, ERR_LEVEL_FATAL may happen when init SQL thrd, then current thread may be
     * killed, so need create sub-thrd to init SQL thrd to avoid dstore resource leaks. */
    void BtreeRecycleThreadMain();

    WatchDogHandle *m_watchdogHandle = nullptr;
};

}

#endif /* SRC_GAUSSKERNEL_INCLUDE_INDEX_STORAGE_BTREE_PAGE_RECYCLE_H */
