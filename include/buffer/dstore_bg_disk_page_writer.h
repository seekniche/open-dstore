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
 * dstore_bg_disk_page_writer.h
 *     This file defines the building blocks that bg page writer needs.
 *
 * IDENTIFICATION
 *        include/buffer/dstore_bg_disk_page_writer.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSTORE_BG_DISK_PAGE_WRITER_H
#define DSTORE_BG_DISK_PAGE_WRITER_H

#include "buffer/dstore_bg_page_writer_mgr.h"
#include "buffer/dstore_buf_mgr.h"
#include "buffer/dstore_bg_page_writer_base.h"
#include "framework/dstore_watchdog.h"

namespace DSTORE {

struct BgSlavePageWriterEntry {
    class BgDiskPageSlaveWriter *bgSlaveDiskPageWriter;
    std::thread *bgSlaveDiskThread;
};

class BgDiskPageMasterWriter final : public BgPageWriterBase {
public:
    BgDiskPageMasterWriter(const WalStream *walStream, DirtyPageQueue *dirtyPageQueue, PdbId pdbId, int64 slotId);
    ~BgDiskPageMasterWriter() final = default;
    DISALLOW_COPY_AND_MOVE(BgDiskPageMasterWriter);

    RetStatus Init() final;

    void Run();

    void FlushAllDirtyPages();

    void Stop() override;

    /*
     * Obtain the smallest PLSN in all dirty page queues which coule be checkpoint plsn.
     */
    uint64 GetMinRecoveryPlsn() const;

    const WalStream *GetWalStream() { return m_walStream; }

    bool WaitNowAllDirtyPageFlushed() const;

    bool NeedFlushAll();

    char *Dump();

    void Destroy();

    uint32 GetSlaveNum(){ return m_slaveNum; }

    PdbId GetPdbId(){ return m_pdbId; }

    void GetSlavePageWriters(AioCompleterInfo *aioCompleterInfo, uint32 startIndex);

    uint32 GetSlaveSlotInfo(AioSlotUsageInfo *aioSlotUsageInfo, uint32 writerId, uint32 startIndex);

    int64 GetWalStreamSlotId()
    {
        return m_slotId;
    }

private:
    RetStatus StartSlavePageWriters();

    void StopSlavePageWriters();

    void RefreshNextFlushTime();

    static void SlavePageWriterMain(PdbId pdbId, class BgDiskPageSlaveWriter *slaveWriter, RetStatus &ret);

    void SmartSleep() const;

    uint32 ScanDirtyListForFlush(uint64 &advanceNum, const int64 slotId);

    void WakeUpSlaveWriter();

    void WaitSlaveWriterFlushFinish() const;

    BgSlavePageWriterEntry *m_slaveWriterArray;
    CandidateFlushCxt m_flushCxt;
    const WalStream *m_walStream;
    DirtyPageQueue *m_dirtyPageQueue;
    BufDescVector m_tmpDirtyPageVec;
    std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds> m_nextFlushTime;
    std::atomic<uint64> m_recoveryPlsn;
    uint32 m_slaveNum;
    PdbId m_pdbId;
    int64 m_slotId;

    std::atomic<bool> m_flushAll;
    std::mutex m_mtx;
    static constexpr long m_waitStep = 100 * 1000;
    WatchDogHandle *m_watchdogHandle = nullptr;
};

class BgDiskPageSlaveWriter final : public BgPageSlaveWriter {
public:
    explicit BgDiskPageSlaveWriter(CandidateFlushCxt *flushCxt, BgDiskPageMasterWriter* master, uint32 slaveIndex);
    ~BgDiskPageSlaveWriter() final = default;
    DISALLOW_COPY_AND_MOVE(BgDiskPageSlaveWriter);

    void SeizeDirtyPageListForFlush() override;

    RetStatus Init() override;

    void Run();

    void WakeupIfSleeping()
    {
        std::unique_lock<std::mutex> waitLock(m_mutex);

        if (!m_isFlushing.load(std::memory_order_acquire)) {
            /*
             * avoid master set m_isFlushing true after slave has exited (set it false).
             * then master maybe hang at WaitSlaveWriterFlushFinish when exit.
             */
            if (!IsStop()) {
                m_isFlushing.store(true, std::memory_order_release);
            }
            m_cv.notify_all();
        }
    }

    BatchBufferAioContextMgr* GetBatchBufferAioContextMgr();

private:

    inline void WaitNextFlush()
    {
        std::unique_lock<std::mutex> waitLock(m_mutex);
        if (m_flushCxt->GetStartFlushLoc() >= m_flushCxt->GetValidSize()) {
            m_isFlushing.store(false, std::memory_order_release);
            (void)m_cv.wait(waitLock);
        }
    }

    void FlushCandidateDirtyPage(BatchBufferAioContextMgr *batchCtxMgr = nullptr);
    bool m_useAio;
    BgDiskPageMasterWriter* m_master;
    BatchBufferAioContextMgr* batchCtxMgr;
    uint32 m_slaveIndex;
    WatchDogHandle *m_watchdogHandle = nullptr;
};

} /* namespace DSTORE */

#endif /* STORAGE_BG_DISK_PAGE_WRITER_H */
