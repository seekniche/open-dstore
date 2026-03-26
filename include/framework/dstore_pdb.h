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
 * dstore_pdb.h
 *
 * IDENTIFICATION
 *        include/framework/dstore_pdb.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSTORE_PDB_H
#define DSTORE_PDB_H

#include <thread>
#include <unistd.h>
#include "port/dstore_port.h"
#include "common/dstore_datatype.h"
#include "control/dstore_control_file.h"
#include "tablespace/dstore_relation_space.h"
#include "tablespace/dstore_tablespace.h"
#include "buffer/dstore_bg_disk_page_writer.h"
#include "dstore_vfs_adapter.h"
#include "pdb/dstore_pdb_interface.h"
#include "wal/dstore_wal_redo_manager.h"
#include "framework/dstore_watchdog.h"
namespace DSTORE {

#ifdef UT
const uint64 INIT_FILE_SIZE = (8UL << 20);                  // 8M
#else
const uint64 INIT_FILE_SIZE = (128UL << 20);                // 128M
#endif

const uint64 TEMPLATE_PDB_TBS_INIT_FILE_SIZE = (8UL << 20); // 8M

const unsigned int OBJSPACEMGR_SLEEP_TIME_IN_MS = 1;
constexpr char PDB_ROOT_NAME[] = "postgres";
constexpr char PDB_TEMPLATE1_NAME[] = "template1";
constexpr char PDB_TEMPLATE0_NAME[] = "template0";
constexpr uint32 BACKUP_RESTORE_SLEEP_TIME = 1000000;
#define RESTORE_FILE_NAME "restore_info_file"
#define RESTORE_CONTROL_FILE_NAME_1 "restore_control_file_1"
#define RESTORE_CONTROL_FILE_NAME_2 "restore_control_file_2"

/* max count of nodes in a page, The value must be the same as the value of MAX_MAPPINGS_4K in the SQLENGINE. */
constexpr int RELMAP_NODE_MAX = 510;
struct RelMap {
    std::atomic<bool> isValid;
    int32_t count;
    ControlSysTableItemData relmaps[RELMAP_NODE_MAX];
};
struct BuiltinRelMap {
    RWLock relMapRWLock;
    RelMap sharedRelMap;
    RelMap localRelMap;
};

struct StandbyPdbSyncMode {
    uint32 standbyClusterId;
    PdbSyncMode syncMode;
};

struct BarrierInfo {
    CommitSeqNo barrierCsn;
    PdbSyncMode syncMode;
    bool isRedoFinish;
};

struct NodeBarrierInfos {
    uint32 nodeId;
    uint32 walStreamCount;
    BarrierInfo *barrierInfos;
};

struct NodeTxnNeedRollback {
    uint32 nodeId;
    uint64 totalRollbackNum;
    TransactionNeedRollbackInfo *rollbackArray;
};


struct NoSendBarrierInfo {
    CommitSeqNo barrierCsn;
    uint64 plsn;
};

struct NoSendBarrierListNode {
    dlist_node dlistNode;
    NoSendBarrierInfo info;
};

struct WalBarrierCsn : public WalRecord {
    CommitSeqNo barrierCsn;
    uint32 nodeCnt;
    uint64 term;
    uint32 pdbCount;
    StandbyPdbSyncMode syncModeArray[0];

    inline void SetBarrierCsn(CommitSeqNo setBarrierCsn)
    {
        barrierCsn = setBarrierCsn;
    }

    inline CommitSeqNo GetBarrierCsn() const
    {
        return barrierCsn;
    }

    inline void SetNodeCnt(uint32 setNodeCnt)
    {
        nodeCnt = setNodeCnt;
    }

    inline uint32 GetNodeCnt() const
    {
        return nodeCnt;
    }

    inline void SetTerm(uint64 setTerm)
    {
        term = setTerm;
    }

    inline uint64 GetTerm() const
    {
        return term;
    }

    inline void SetPdbCount(uint32 count)
    {
        pdbCount = count;
    }

    inline uint32 GetPdbCount() const
    {
        return pdbCount;
    }
} PACKED;

class StoragePdb : public BaseObject {
public:
    explicit StoragePdb(PdbId pdbId);
    explicit StoragePdb(PdbId pdbId, const char *pdbUuid);
    virtual ~StoragePdb();
    DISALLOW_COPY_AND_MOVE(StoragePdb);

    FUNCTION_DELETE_LATER virtual RetStatus Create();
    FUNCTION_DELETE_LATER virtual RetStatus Initialize(bool initCsn);
    FUNCTION_DELETE_LATER virtual RetStatus Destroy();

    virtual RetStatus CreatePdb(bool initCsn);
    virtual RetStatus CreateVfsAndFiles(PdbId pdbId, const char *vfsName = nullptr);
    virtual RetStatus InitPdb(bool initCsn);
    virtual RetStatus ResetPdb(const char *vfsName, bool dropData);
    virtual RetStatus OpenPdb(void *pdbInfoData);
    virtual RetStatus ClosePdb(bool needFullCheckpoint = false);
    virtual bool IsReadFileOk() const;

    class VFSAdapter *GetVFS();
    void SetVFSName(const char *vfsName);
    const char *GetVFSName();
    virtual class ControlFile *GetControlFile();
    virtual class TransactionMgr *GetTransactionMgr();
    virtual class UndoMgr *GetUndoMgr();
    virtual class WalManager *GetWalMgr();
    virtual class LogicalReplicaMgr *GetLogicalReplicaMgr();
    virtual class ObjSpaceMgr *GetObjSpaceMgr();
    virtual class CheckpointMgr *GetCheckpointMgr();
    virtual RetStatus FullCheckpoint();

    bool IsInit() const;
    bool IsWalMgrInit() const;
#ifdef UT
    void SetInit();
    void SetWalMgrInit();
#endif
    void CreateTemplateTablespace();
    virtual void LoadUndoMapSegment();
    virtual void CreateUndoMapSegment();
    virtual void StartBgThread();
    virtual void StopBgThread(bool needFullCheckpoint = false, bool usedToDrPdbreplica = false);
    virtual void StopSQLDependBgThread();
    virtual void InitRecoveryThreadAndWaitDone();

    int64 GetBgWriterSlotId(uint64 walId = INVALID_WAL_ID) const;
    void SetBgWriterSlotId(const int64 slotId);

    /* Call vfs GetSize with file open to ensure vfs have had its handle. */
    RetStatus GetFileSizeWithOpen(const char *fileName, int64 &fileSize);

    void TryMarkSelfWalStreamDropping();
    virtual RetStatus InitControlFile();
    RetStatus GetControlFileSizeVfs(int64 &partSize);
    RetStatus GetDecodeDictFileSizeVfs(int64 &partSize);
    RetStatus GetFileSizeVfs(int64 &partSize);
    virtual RetStatus CreateNewVFS();
    virtual RetStatus MountExistingVFS(const char *vfsName);
    virtual class BgPageWriterMgr *GetBgPageWriterMgr();
    virtual class WatchDogMgr *GetWatchDogMgr();
    virtual RetStatus BgPageWriterMgrInit();
    virtual void BgPageWriterMgrDestroy();
    RetStatus DropWalPath(char *pdbPath) const;
    virtual void DestroyControlFile();
    RetStatus DeleteWalFileFromVfs(ControlWalStreamPageItemData *pageInfo);
    RetStatus GetWalFileSizeVfs(int64 &partSize);
    RetStatus GetSizeVfs(int64 &totalSize);

    RetStatus CopyFilesFromPdb(StoragePdb *srcPdb);
    RetStatus CopyFileFromVfs(FileId fileId, const char *fileName, VFSAdapter *srcVfs, VFSAdapter *newVfs);

    RetStatus CopyWalFilesFromPdb(StoragePdb *srcPdb);
    RetStatus CopyWalFileFromVfs(const char *fileName, VFSAdapter *srcVfs, VFSAdapter *newVfs, WalId walId);

    RetStatus CopyControlFile(StoragePdb *templatePdb);
    RetStatus CopyFiles(StoragePdb *templatePdb);
    RetStatus MarkAllControlWalStreamDropping();

    void StoreMaxAppendPlsnOfWalStreams(class PdbsPlsnRecords &plsnRecords);
    RetStatus CreateCheckpoint();
    RetStatus CreateCheckpoint(const class PdbsPlsnRecords &diskRecoveryPlsnRecords);
    RetStatus CreateCheckpointForWalStream(const class WalStream *walStream, uint64 diskRecoveryPlsn);
    RetStatus CreateBgWriterThrdForWalStream(const class WalStream *walStream, bool primarySlot);
    RetStatus UpdateDatabaseSysTable(HeapTuple *sysDatabaseTuple);

    void CreateThreadAndRegister(bool isNeedInitStorageContext = true, bool needCommBuffer = true,
                                 ThreadMemoryLevel level = ThreadMemoryLevel::THREADMEM_HIGH_PRIORITY,
                                 const char *threadName = nullptr);
    void UnregisterThread();
    void InitSigForModule(const char *moduleName) const;

    RetStatus StartBgThreadWhenPromoting();
    RetStatus StartBgThreadWhenDemoting();
    RetStatus CleanTempFile(bool isStartUp = false);
    virtual RetStatus InitLogicalReplicaMgr();
    virtual void DestroyLogicalReplicaMgr();
    static void InitWalFileParam(FileParameter &filePara, WalId walId, PdbId pdbId);

    /* Only UT directly access these functions */
    virtual void InitTransactionMgr();
    virtual void DestroyTransactionMgr();
    virtual void DestoryTablespaceMgr();

    virtual RetStatus InitUndoMgr(); /* prepare undo table space and map segment after template table space created */
    virtual void DestroyUndoMgr();

    static bool IsValidPdbId(PdbId pdbId)
    {
        return pdbId >= PDB_START_ID && pdbId <= PDB_MAX_ID;
    }

    static void GenerateVfsName(PdbId pdbId, char* vfsName, uint32 len);

    inline PdbId GetPdbId() const
    {
        return m_pdbId;
    }
    inline char *GetPdbUuid()
    {
        return m_pdbUuid;
    }

    inline void SetPdbUuid(const char *pdbUuid)
    {
        if (strlen(pdbUuid) > 0) {
            error_t rc = strncpy_s(m_pdbUuid, FORMATTED_UUID_ARR_LEN, pdbUuid, strlen(pdbUuid));
            storage_securec_check(rc, "\0", "\0");
        }
    }

    inline void SetPdbName(const char *pdbName)
    {
        if (strlen(pdbName) > 0) {
            error_t rc = strncpy_s(m_pdbName, DB_NAME_LEN, pdbName, strlen(pdbName));
            storage_securec_check(rc, "\0", "\0");
        }
    }

    inline void SetPdbStatus(PdbStatus status)
    {
        m_pdbStatus.store(static_cast<uint8>(status), std::memory_order_release);
    }

    inline PdbStatus GetPdbStatus()
    {
        return static_cast<PdbStatus>(m_pdbStatus.load(std::memory_order_acquire));
    }

    inline void SetPdbTerm(uint64_t term)
    {
        GsAtomicWriteU64(&m_pdbTerm, term);
    }

    inline uint64_t GetPdbTerm()
    {
        return GsAtomicReadU64(const_cast<uint64_t *>(&m_pdbTerm));
    }

    inline bool CheckCanInnerAccess()
    {
        return GetPdbStatus() == PdbStatus::PDB_STATUS_OPENED_READ_WRITE ||
               GetPdbStatus() == PdbStatus::PDB_STATUS_CLOSING ||
               (GetPdbStatus() == PdbStatus::PDB_STATUS_CLOSED);
    }

    void SetPdbInfo(ControlPdbInfoPageItemData *item);

    inline void SetPdbRoleMode(PdbRoleMode pdbRoleMode)
    {
        m_pdbRoleMode.store(static_cast<uint8>(pdbRoleMode), std::memory_order_release);
    }
    inline void SetPdbRecycleCsnMin(CommitSeqNo pdbRecycleCsnMin)
    {
        ErrLog(DSTORE_LOG, MODULE_FRAMEWORK, ErrMsg("[PdbRecycleCsnMin] pdb %u set m_pdbRecycleCsnMin from %lu to %lu.",
            m_pdbId, m_pdbRecycleCsnMin.load(std::memory_order_acquire), pdbRecycleCsnMin));
        m_pdbRecycleCsnMin.store(pdbRecycleCsnMin, std::memory_order_release);
    }
    bool IsPrimaryOrBeginPromote();
    PdbRoleMode GetPdbRoleMode();
    CommitSeqNo GetPdbRecycleCsnMin();
    inline void SetDisableSqlTransaction(bool disableSqlTransaction)
    {
        m_disableSqlTransaction.store(disableSqlTransaction, std::memory_order_release);
    }
    inline bool DisableSqlTransaction() const
    {
        return m_disableSqlTransaction.load(std::memory_order_acquire);
    }

    inline const char *GetPdbName() const
    {
        return m_pdbName;
    }
    inline void StartupAsyncRecoverUndoThread()
    {
        m_asyncRecoverUndoThread = new std::thread(&StoragePdb::AsyncRecoverUndoThreadMain, this);
    }
    inline void StartupUndoRecycleThread()
    {
        m_recycleUndoThread = new std::thread(&StoragePdb::RecycleUndoThreadMain, this);
    }
    inline void StartupSpaceMgrWorkerThread()
    {
        if (m_numObjSpaceMgrWorkers == 0) {
            return;
        }
        /* Allocate the worker thread list dynamically */
        StorageReleasePanic(m_numObjSpaceMgrWorkers == 0, MODULE_FRAMEWORK, ErrMsg("m_numObjSpaceMgrWorkers is 0."));
        m_objSpaceMgrWorkerList =
            static_cast<std::thread **>(DstorePalloc(m_numObjSpaceMgrWorkers * sizeof(std::thread *)));
        if (STORAGE_VAR_NULL(m_objSpaceMgrWorkerList)) {
            ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("Failed to malloc for objSpaceMgrWorkerList."));
            return;
        }
        for (uint32 i = 0; i < m_numObjSpaceMgrWorkers; i++) {
            m_objSpaceMgrWorkerList[i] = new std::thread(&StoragePdb::ObjSpaceMgrWorkerThreadMain, this, i);
        }
    }
    inline void StartupBgPageWriterAndCkptThread()
    {
        InitBgWriterThread();
        m_checkpointThread = new std::thread(&StoragePdb::CheckpointThreadMain, this);
    }
    inline void StartupStandbyMonitorThread()
    {
        std::lock_guard<std::mutex> lock(m_standbyMonitorThreadMutex);
        if (m_standbyMonitorThread == nullptr) {
            m_standbyMonitorThread = new std::thread(&StoragePdb::StandbyMonitorThreadMain, this);
        }
    }
    inline void WaitStandbyMonitorExit()
    {
        std::lock_guard<std::mutex> lock(m_standbyMonitorThreadMutex);
        if (m_standbyMonitorThread != nullptr) {
            m_standbyMonitorThread->join();
            delete m_standbyMonitorThread;
            m_standbyMonitorThread = nullptr;
        }
    }
    inline void StartupBarrierCreatorThread()
    {
        if (IsTemplate(GetPdbId()) || GetPdbId() == PDB_ROOT_ID) {
            return;
        }
        m_barrierCreatorThread = new std::thread(&StoragePdb::BarrierCreatorThreadMain, this);
    }
    inline void StartupCollectBarrierThread()
    {
        if (IsTemplate(GetPdbId()) || GetPdbId() == PDB_ROOT_ID) {
            return;
        }
        m_collectMinBarrierThread = new std::thread(&StoragePdb::CollectMinBarrierThreadMain, this);
    }
    inline void StartUpdateBarrierCsnThread()
    {
        if (m_pdbId < FIRST_USER_PDB_ID) {
            return;
        }
        m_updateBarrierCsnThread = new (std::nothrow) std::thread(&StoragePdb::UpdateBarrierCsnThreadMain, this);
    }
    inline void StopUpdateBarrierCsnThread()
    {
        if (m_updateBarrierCsnThread == nullptr) {
            return;
        }
        m_stopPdbUpdateBarrier = true;
        if (m_updateBarrierCsnThread != nullptr) {
            m_updateBarrierCsnThread->join();
            delete m_updateBarrierCsnThread;
            m_updateBarrierCsnThread = nullptr;
        }
        m_stopPdbUpdateBarrier = false;
    }
    static inline PdbId GetPdbIndex(PdbId pdbId)
    {
        return pdbId - PDB_START_ID;
    }

    BgDiskPageMasterWriter *GetBgDiskPageMasterWriter() const;

    inline void AddActiveTransaction()
    {
        (void)GsAtomicAddFetchU32(&m_activeTransactionNum, 1);
    }
    inline void SubActiveTransaction()
    {
        (void)GsAtomicSubFetchU32(&m_activeTransactionNum, 1);
    }
    inline uint32 GetActiveTransactionNum()
    {
        return GsAtomicReadU32(&m_activeTransactionNum);
    }

    inline TablespaceMgr *GetTablespaceMgr() const
    {
        return m_tablespaceMgr;
    }

    inline bool IsRollbackEnd() const
    {
        return m_rollbackEnd.load(std::memory_order_acquire);
    }

    void AsyncRecoverUndo();

    void AsyncRecoverUndoThreadMain();

    RetStatus GetBuiltinRelMap(RelMapType type, RelMapNode *nodes, int *count);

    RetStatus WriteBuiltinRelMap(RelMapType type, RelMapNode *nodes, int count);
    RetStatus WriteBuiltinRelMapRedo(RelMapType type, RelMapNode *nodes, int count);
    void GenerateWriteBuiltinRelMapWal(RelMapType type, RelMapNode *nodes, int count);
    void TryMarkSelfUndoStopping();
    void ResetSelfUndoStopping();

    /**
     * Gets a new oid for a database object of current pdb.
     *
     * @param isInitDb Indicates whether the current phase is in phase initdb.
     * @param oid the new oid, this is a out param
     * @return return DSTORE_SUCC if get oid successfully, otherwise return DSTORE_FAIL.
     */
    RetStatus GetNewObjectId(bool isInitDb, Oid &oid, bool isInplaceUpgrade);

    void StopBarrierThread();
    void PushTransactionIntoBarrierRollbackList(TransactionsNeedRollbackListNode *node);
    void UpdateCsnInBarrierRollbackListByXid(Xid xid, CommitSeqNo csn);
    void DeleteTrxnsFromBarrierRollbackListBeforeCsn(CommitSeqNo csn);
    void DeleteAllTrxnsFromBarrierRollbackList();
    void DeleteAndUpdateLastBarrierCsnBeforePlsn(uint64 plsn);
    void DeleteAllFromNoSendBarrierList();
    void PushBarrierIntoNoSendBarrierList(CommitSeqNo barrierCsn, uint64 plsn);
    RetStatus GetAllTransactionsNeedRollbackByBarrier(CommitSeqNo barrierCsn,
                                                      TransactionNeedRollbackInfo **rollbackArray,
                                                      uint64 &totalRollbackNum);
    inline void SetNeedRollbackBarrierInFailover(bool needRollback)
    {
        m_needRollbackBarrierInFailover = needRollback;
    }

    inline bool GetNeedRollbackBarrierInFailover() const
    {
        return m_needRollbackBarrierInFailover;
    }
    inline void SetBarrierLastEndPlsn(uint64 endPlsn)
    {
        m_barrierLastEndPlsn = endPlsn;
    }

    inline uint64 GetBarrierLastEndPlsn() const
    {
        return m_barrierLastEndPlsn;
    }

    inline void SetRollbackBarrierCsnInFailover(CommitSeqNo rollbackBarrierCsn)
    {
        m_rollbackBarrierCsn = rollbackBarrierCsn;
    }

    inline CommitSeqNo GetRollbackBarrierCsnInFailover() const
    {
        return m_rollbackBarrierCsn;
    }

    inline uint16 *GetStoppingStatus()
    {
        return &m_stoppingStatus;
    }

     inline bool IsNeedStopBgThread()
    {
        return m_stopBgThread.load(std::memory_order_acquire);
    }

    char m_pdbName[DB_NAME_LEN] = {0};

    class VFSAdapter *m_vfs;
    class ControlFile *m_controlFile;
    class TransactionMgr *m_transactionMgr;
    class UndoMgr *m_undoMgr;
    class WalManager *m_walMgr;
    class LogicalReplicaMgr *m_logicalReplicaMgr;
    class ObjSpaceMgr *m_objSpaceMgr;
    class CheckpointMgr *m_checkpointMgr;
    class BgPageWriterMgr *m_bgPageWriterMgr;
    class WatchDogMgr *m_watchdogMgr;
    Oid m_nextOid = FIRST_BOOTSTRAP_OBJECT_ID;  /* next OID to assign */
    uint32 m_cachedOidCnt = 0;     /* the number of unused cached OID */
    RWLock m_oidGenRWLock;         /* For concurrent application of OIDs in the same PDB */
    std::atomic<int32> m_asyncStopAndDropWalThrdCnt;  /* the number of running async StopAndDropWal thrds. */
    GsStatMsgBgWriter* GetGsStatMsgBgWriter();
    void InitTableSpaceMgr();

    class std::thread *GetStandbyMonitorThread()
    {
        return m_standbyMonitorThread;
    }

    int64 GetOrgBgWriterSlotId()
    {
        return m_bgWriterSlotId;
    }

protected:
    virtual RetStatus BootstrapVFS();
    virtual RetStatus InitVFS();
    virtual RetStatus DestroyVFS(const char *vfsName, bool dropData, PdbId pdbId);
    virtual RetStatus CreateControlFile(PdbId pdbId, const char* pdbPath);
    virtual RetStatus CreateDecodeDictFile(PdbId pdbId, const char* pdbPath);

    void InitCheckpointMgr();
    void DestroyCheckpointMgr();

    virtual RetStatus InitWalMgr();
    virtual void DestroyWalMgr();

    virtual RetStatus InitObjSpaceMgr();
    virtual void DestroyObjSpaceMgr();

    RetStatus CreateSysTablespace(TablespaceId tablespaceId);

    RetStatus CreateTablespace(const TablespaceId tablespaceId, const FileId fileId, uint64 maxSize = MAX_FILE_SIZE);
    RetStatus CreateTablespace(const TablespaceId tablespaceId, const char *tablespaceName,
        const FileId fileId, const char* storeSpaceName, uint64 maxSize = MAX_FILE_SIZE);
    RetStatus CreateTablespaceInternal(const TablespaceId tablespaceId, const char *tablespaceName, uint64 maxSize,
        FileId startFileId, const char *storeSpaceName);

    FUNCTION_DELETE_LATER void CreateTablespaceImpl(const TablespaceId tablespaceId, const FileId fileId);

    virtual void RecycleUndo();
    void RecycleUndoThreadMain();

    void ObjSpaceMgrWorkerThreadMain(uint32 workerId) noexcept;
    void CheckpointThreadMain();
    void StandbyMonitorThreadMain();
    void RecoveryThreadMain();

    void BarrierCreatorLoop();
    void BarrierCreatorThreadMain();
    void UpdateBarrierCsnThreadMain();
    void UpdateBarrierCsnThreadLoop();
    CommitSeqNo GetCurrentMinBarrierCsn();
    void CollectMinBarrierLoop();
    void CollectMinBarrierThreadMain();

    RetStatus InitBgWriterThread();
    RetStatus CreateWalPath(char *pdbPath) const;
    RetStatus CheckPdbInfo(const char *vfsName);
    void DestroyMgr();
    bool IsPdbInRestoreFromBackup();

    RetStatus OpenFileInVfsIfNeed(VFSAdapter *vfsAdapter, FileId fileId, const char *fileName);

    RetStatus OpenWalFileInVfsIfNeed(VFSAdapter *vfsAdapter, const char *fileName, FileDescriptor **walFileFd);

    RetStatus CopySpecifySizeInFile(VFSAdapter *newVfs, VFSAdapter *srcVfs, FileId fileId, uint32 readSize,
                                    int64 offset);

    RetStatus CopySpecifySizeInWalFile(VFSAdapter *srcVfs, FileDescriptor *srcFileFd, VFSAdapter *newVfs,
                                       FileDescriptor *destFileFd, uint32 readSize, int64 offset);

    inline void ObjSpaceMgrWorkerSleep() const
    {
        unsigned int timeout = OBJSPACEMGR_SLEEP_TIME_IN_MS;
        (void)usleep(timeout * 1000U);
    }

    class std::thread *m_recycleUndoThread;
    class std::thread *m_asyncRecoverUndoThread;
    class std::thread *m_checkpointThread;
    class std::thread **m_objSpaceMgrWorkerList;
    std::mutex m_standbyMonitorThreadMutex;
    class std::thread *m_standbyMonitorThread;
    class std::thread *m_barrierCreatorThread;
    class std::thread *m_collectMinBarrierThread;
    class std::thread *m_updateBarrierCsnThread;
    bool m_stopPdbUpdateBarrier;
    PdbId m_pdbId;
    std::atomic<uint8> m_pdbStatus;
    char m_pdbUuid[FORMATTED_UUID_ARR_LEN] = {0};
    bool m_hasInitialized;
    bool m_walMgrhasInitialized;
    bool m_inBootStrap;
    int64 m_bgWriterSlotId;
    std::atomic<bool> m_stopBgThread;
    std::atomic<bool> m_stopBarrierThread;
    uint16 m_numObjSpaceMgrWorkers;
    std::atomic<uint8> m_pdbRoleMode;
    std::atomic<CommitSeqNo> m_pdbRecycleCsnMin{MAX_COMMITSEQNO};
    std::atomic<bool> m_disableSqlTransaction;
    gs_atomic_uint32 m_activeTransactionNum;
    char m_vfsName[MAX_CONFIG_NAME_LENGTH] = {0};
    class TablespaceMgr *m_tablespaceMgr;
    BuiltinRelMap m_builtinRelMap;
    std::atomic<bool> m_rollbackEnd;
    GsStatMsgBgWriter m_statMsgBgWriter;
    dlist_head m_transactionsNeedRollbackBarrierListHead;
    uint64 m_numOfTransactionsNeedRollbackBarrier;
    dlist_head m_noSendBarrierListHead;
    uint64 m_numOfNoSendBarrier;
    bool m_needRollbackBarrierInFailover;
    CommitSeqNo m_rollbackBarrierCsn;
    uint64 m_barrierLastEndPlsn;
    pthread_rwlock_t m_rollbackBarrierListLock;
    gs_atomic_uint64 m_pdbTerm = 0;
    uint16 m_stoppingStatus = 0;
};

} /* namespace DSTORE */
#endif
