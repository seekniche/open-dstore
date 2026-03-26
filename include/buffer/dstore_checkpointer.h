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

#ifndef DSTORE_DSTORE_CHECKPOINTER_H
#define DSTORE_DSTORE_CHECKPOINTER_H

#include <atomic>
#include "common/dstore_datatype.h"
#include "common/memory/dstore_mctx.h"
#include "lock/dstore_lwlock.h"
#include "wal/dstore_wal.h"
#include "wal/dstore_wal_struct.h"
#include "framework/dstore_watchdog.h"

namespace DSTORE {
class WalStream;

/*
 * Checkpoint request flags which will affect the behavior of checkpoint.
 * The flag is uint32 which
 * [0:7]  : use for indicate the cause of checkpoint;
 * [8:15] : use for some specifics actions will be executed when do the checkpoint;
 * [16:31]: reserve;
 */
enum CheckpointFlag : uint8 {
    CHECKPOINT_INVALID_FLAG,
    CHECKPOINT_CAUSE_TIME, /* Elapsed time */
};

/*----------
 * Shared data structure for communication between checkpointer and backends,
 * each WalStream will have a WalCheckpointInfoData.
 *
 * The ckpt counters allow backends to watch for completion of a checkpoint
 * request they send.  Here's how it works:
 *  * At start of a checkpoint, checkpointer reads (and clears) the request
 *    flags and increments m_checkpointStart, while holding ckptLock.
 *  * On completion of a checkpoint, checkpointer sets m_checkpointDone to
 *    equal m_checkpointStart.
 *  * On failure of a checkpoint, checkpointer increments m_checkpointFail
 *    and sets m_checkpointDone to equal m_checkpointStart.
 *
 * The algorithm for backends is:
 *  1. Record current values of m_checkpointFail and m_checkpointStart, and
 *     set request flags, while holding ckptLock.
 *  2. Send signal to request checkpoint.
 *  3. Sleep until m_checkpointStart changes. Now you know a checkpoint has
 *     begun since you started this algorithm (although *not* that it was
 *     specifically initiated by your signal), and that it is using your flags.
 *  4. Record new value of m_checkpointStart.
 *  5. Sleep until m_checkpointDone >= saved value of m_checkpointStart.  (Use modulo
 *     arithmetic here in case counters wrap around.)  Now you know a
 *     checkpoint has started and completed, but not whether it was
 *     successful.
 *  6. If m_checkpointFail is different from the originally saved value,
 *     assume request failed; otherwise it was definitely successful.
 *
 * m_checkpointFlag holds the OR of the checkpoint request flags sent by all
 * requesting backends since the last checkpoint start.  The flags are
 * chosen so that OR'ing is the correct way to combine multiple requests.
 *
 */
class CheckpointRequest : public BaseObject {
public:
    CheckpointRequest();
    ~CheckpointRequest() = default;
    /*
     * start to process checkpoint and return the checkpoint request flag
     */
    CheckpointFlag StartCheckpoint();
    /*
     * indicate that this turn checkpoint has finished.
     */
    void FinishCheckpoint();
    /*
     * request a checkpoint and set checkpoint request flag.
     */
    void Request(uint32 flag, uint32 *startTime, uint32 *failTime);

    uint32 GetCheckpointStartCnt() const;
    uint32 GetCheckpointDoneCnt() const;
private:
    DstoreSpinLock m_checkpointLock;
    /* ckptLock protect */
    uint32 m_checkpointStart;
    uint32 m_checkpointDone;
    uint32 m_checkpointFail;
    CheckpointFlag m_checkpointFlag;
};

struct WalCheckpointInfoData {
    dlist_node node;
    WalId walId;
    CheckpointRequest checkpointStreamRequest;

    LWLock checkpointLwLock;
    Timestamp lastCheckpointTime;

    DstoreSpinLock recoveryLock; /* protect the field below */
    uint64 lastCheckPointRecoveryPlsn;
    WalCheckPoint lastCheckPoint;
};

struct WalCheckpointStatInfo {
    WalId walId;
    Timestamp lastCheckpointTime;
    uint32 checkpointStart;
    uint32 checkpointDone;
    uint64 maxFlushedPlsn;
    uint64 maxAppendedPlsn;
    WalCheckPoint lastCheckPoint;
};

struct CheckpointStatInfo {
    uint32 pdbId;
    ThreadId checkpointPid;
    int64 lastRequestTime;
    uint32 requestedCheckpoints;
    uint32 walStreamNum;
    WalCheckpointStatInfo *walCkptStatInfo;

    CheckpointStatInfo() : pdbId(INVALID_PDB_ID), checkpointPid(0), lastRequestTime(0), requestedCheckpoints(0),
        walStreamNum(0), walCkptStatInfo(nullptr) {};
    ~CheckpointStatInfo()
    {
        DstorePfreeExt(walCkptStatInfo);
    }
};

class CheckpointMgr : public BaseObject {
public:
    /* A main entry to process checkpoint request */
    void CheckpointerMain();

    explicit CheckpointMgr(WalManager *walMgr, PdbId pdbId)
        : m_pdbId(pdbId),
          m_checkpointPid(INVALID_THREAD_ID),
          m_lastRequestTime(time(nullptr)),
          m_walCheckpointDataNum(0),
          m_checkpointContext(nullptr),
          m_shutdownRequested(false),
          m_inited(false),
          m_isFullCkpting(false),
          m_requestedCheckpoints(0),
          m_walMgr(walMgr),
          m_lastHeartbeatTime(0),
          m_watchdogEntry("Checkpointer", &m_lastHeartbeatTime, WATCHDOG_CHECKPOINTER_TIMEOUT_S, pdbId)
    {
        m_queueSpinlock.Init();
    }
    ~CheckpointMgr() = default;

    /* stop the checkpoint thread */
    void StopCheckpointer();

    /*
     * GetWalCheckpoint info of target walStream
     * @param[in] walId for target WalStream
     * @param[out] walCheckPoint is fetched info
     *
     * @return SRORAGE_SUCC if fetch info success
     */
    RetStatus GetWalCheckpoint(WalId walId, WalCheckPoint &walCheckpoint);

    RetStatus FullCheckpoint(PdbId pdbId);

    uint32 GetRequestedCheckpointsCnt() const
    {
        return m_requestedCheckpoints;
    }

    uint32 GetWalCheckpointDataNum() const
    {
        return m_walCheckpointDataNum;
    }

    dlist_head *GetCheckpointInfoList()
    {
        return &m_checkpointInfoList;
    }

    WalManager *GetWalMgr() const
    {
        return m_walMgr;
    }

    bool IsInitialized() const
    {
        return m_inited.load(std::memory_order_acquire);
    }

    RetStatus GetCheckpointStatInfo(CheckpointStatInfo &ckptInfo);

    void Init();
    void AddOneCheckPoint(WalId walid, uint64 lastcheckpointplsn);
    void Destroy();
    DstoreSpinLock m_queueSpinlock;
    dlist_head m_checkpointInfoList;

private:
    PdbId m_pdbId;
    ThreadId m_checkpointPid;
    Timestamp m_lastRequestTime;
    uint32 m_walCheckpointDataNum;
    CheckpointRequest m_checkpointAllStreamRequest;

    DstoreMemoryContext m_checkpointContext;
    std::atomic_bool m_shutdownRequested;
    std::atomic_bool m_inited;
    std::atomic_bool m_isFullCkpting;
    uint32 m_requestedCheckpoints;

    WalManager *m_walMgr;
    /* WatchDog heartbeat: updated each main-loop iteration (0 = not running) */
    std::atomic<uint64> m_lastHeartbeatTime;
    WatchDogEntry m_watchdogEntry;

    WalCheckpointInfoData *FindCheckpointInfo(WalId walId);

    RetStatus CheckpointOneWalStream(WalId walId, CheckpointFlag flags, bool *isPerformed);

    RetStatus CreateCheckpoint(WalId walId, CheckpointFlag flags, bool *isPerformed);
};

void DumpAllWalSteamCkptInfo(StringInfoData &dumpInfo, uint32 walStreamNum, WalCheckpointStatInfo *walCkptStatInfo);

}  // namespace DSTORE
#endif  // DSTORE_STORAGE_CHECKPOINTER_H
