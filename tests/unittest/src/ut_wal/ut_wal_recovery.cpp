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
#include <random>
#include <vector>
#include "ut_utilities/ut_dstore_framework.h"
#include "ut_mock/ut_mock.h"
#include "ut_tablespace/ut_segment.h"
#include "buffer/dstore_checkpointer.h"
#include "control/dstore_control_file.h"
#include "framework/dstore_pdb.h"
#include "vfs/vfs_interface.h"
#include "heap/dstore_heap_wal_struct.h"
#include "tablespace/dstore_tablespace_wal.h"
#include "wal/dstore_wal_recovery.h"
#include "ut_wal/ut_wal_basic.h"
#include "diagnose/dstore_wal_diagnose.h"
using namespace DSTORE;
/* define it means using pagestore */
/* #define DYNAMIC_LINK_VFS_LIB */

extern char g_utTopDir[MAXPGPATH];
static constexpr double TEST_TIME_SECONDS = 10 * 60 * 60;
static uint32 g_walGroupNum = 2000;
static FileId g_walUtFileId = 0;
static constexpr uint32 MAX_WAL_GROUP_NUM = 1000;
static constexpr uint32 WAL_RECORDS_NUM = 20;
static constexpr uint64 WAL_READ_BUFFER_SIZE = 1024 * 1024 * 1024UL;
static constexpr uint32 MAX_PAGE_NUM = 10;
static constexpr uint32 WAL_RECORD_APPEND_DATA_LEN = 2024;
static constexpr uint32 INSERT_DDL_COUNT = 10;

void WalRecordReadCallBackForTest(const WalRecord *walRecord, void *recordReadCallBackContext)
{
    GsAtomicFetchAddU64((uint64 *)recordReadCallBackContext, 1);
}

class WalRecoveryTest : public WALBASICTEST {
protected:

    void SetUp() override
    {
        SetDefaultPdbId(PDB_TEMPLATE1_ID);
        DSTORETEST::SetUp();
#ifdef DYNAMIC_LINK_VFS_LIB
        /* Get the config file. */
        memset(m_configFilePath, 0, MAXPGPATH);
        char *execPath = m_configFilePath;
        ret = readlink("/proc/self/exe", execPath, MAXPGPATH);
        ASSERT_GT(ret, 0);
        char *lastSlashPtr = strrchr(execPath, '/');
        ASSERT_NE(lastSlashPtr, nullptr);

        snprintf(lastSlashPtr + 1, MAXPGPATH / 2, "dynamic_vfs.conf.init");
        ASSERT_EQ(access(m_configFilePath, F_OK), 0); /* It must exist. */

        DSTORETEST::m_guc.vfsConfigPath = m_configFilePath;
#endif
        WALBASICTEST::Prepare(16 * 1024);
        m_instance = g_storageInstance;
        m_testBufMgr = g_storageInstance->GetBufferMgr();
        /* use default tablespace */
        m_testTbs = UtMockModule::UtGetTableSpace(static_cast<TablespaceId>(TBS_ID::DEFAULT_TABLE_SPACE_ID), nullptr);
        PrepareControlFileContent();
        NodeId selfNode = 0;
        RetStatus retStatus = m_walStreamManager->Init(m_walControlFile);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        m_controlFile = g_storageInstance->GetPdb(g_defaultPdbId)->GetControlFile();
        m_segSize = g_storageInstance->GetGuc()->walFileSize;
        m_walStream = m_walStreamManager->GetWritingWalStream();
        m_startReadPlsn = m_walStream->GetMaxWrittenToFilePlsn();
        m_walRecovery = m_walStream->GetWalRecovery();
    }

    void UpdateControlFileCheckPoint(uint64 recoveryStartPlsn)
    {
        RetStatus retStatus;
        ControlWalStreamPageItemData *walStreamInfo = nullptr;
        retStatus = m_controlFile->GetWalStreamInfo(0, &walStreamInfo);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        WalCheckPoint checkPoint = {0, recoveryStartPlsn};
        walStreamInfo->lastCheckpointPLsn = 0;
        walStreamInfo->lastWalCheckpoint = checkPoint;
        retStatus = m_controlFile->UpdateWalStream(*walStreamInfo);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        m_controlFile->FreeWalStreamsInfo(walStreamInfo);
    }
    WalCheckPoint BuildCheckpoint(uint64 diskRecoveryPlsn) const
    {
        WalCheckPoint checkpoint = {};
        checkpoint.time = static_cast<Timestamp>(diskRecoveryPlsn);
        checkpoint.diskRecoveryPlsn = diskRecoveryPlsn;
        checkpoint.memoryCheckpoint.memRecoveryPlsn = diskRecoveryPlsn;
        return checkpoint;
    }

    std::vector<WalGroupLsnInfo> CollectWalGroupInfos(uint32 expectedMinGroupNum)
    {
        WalReaderConf conf = {m_walStream->GetWalId(), m_startReadPlsn, m_walStream, nullptr, m_segSize,
                              WalReadSource::WAL_READ_FROM_DISK};
        WalRecordReader *walRecordReader = DstoreNew(m_ut_memory_context) WalRecordReader(m_ut_memory_context, conf);
        std::vector<WalGroupLsnInfo> groupInfos;
        if (walRecordReader == nullptr) {
            ADD_FAILURE() << "failed to allocate WalRecordReader";
            return groupInfos;
        }
        if (walRecordReader->Init() != DSTORE_SUCC) {
            ADD_FAILURE() << "failed to initialize WalRecordReader";
            delete walRecordReader;
            return groupInfos;
        }

        const WalRecordAtomicGroup *walGroup = nullptr;
        while (true) {
            if (walRecordReader->ReadNext(&walGroup) != DSTORE_SUCC) {
                ADD_FAILURE() << "failed to read wal group from disk";
                delete walRecordReader;
                return groupInfos;
            }
            if (walGroup == nullptr) {
                break;
            }
            groupInfos.push_back({m_walStream->GetWalId(), walRecordReader->GetCurGroupStartPlsn(),
                                  walRecordReader->GetCurGroupEndPlsn()});
            while (walRecordReader->GetNextWalRecord() != nullptr) {
            }
        }
        delete walRecordReader;
        EXPECT_GE(groupInfos.size(), expectedMinGroupNum);
        return groupInfos;
    }

    void InstallCheckpointHistory(ControlWalStreamPageItemData *streamInfo, const std::vector<uint64> &slotPlsns,
                                  uint64 latestPlsn = INVALID_PLSN) const
    {
        ASSERT_NE(streamInfo, nullptr);
        streamInfo->lastCheckpointPLsn = 0;
        streamInfo->checkpointHistory = {};
        for (uint64 slotPlsn : slotPlsns) {
            streamInfo->checkpointHistory.Push(BuildCheckpoint(slotPlsn));
        }
        if (latestPlsn == INVALID_PLSN && !slotPlsns.empty()) {
            latestPlsn = slotPlsns.back();
        }
        if (latestPlsn != INVALID_PLSN) {
            streamInfo->lastWalCheckpoint = BuildCheckpoint(latestPlsn);
            if (streamInfo->checkpointHistory.slotCount > 0) {
                streamInfo->checkpointHistory.slots[streamInfo->checkpointHistory.newestSlotIdx] =
                    streamInfo->lastWalCheckpoint;
            }
        }
    }

    void PersistCheckpointHistory(const std::vector<uint64> &slotPlsns, uint64 latestPlsn = INVALID_PLSN,
                                  bool syncCheckpointMgr = false)
    {
        ControlWalStreamPageItemData *streamInfo = nullptr;
        ASSERT_EQ(m_controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &streamInfo), DSTORE_SUCC);
        InstallCheckpointHistory(streamInfo, slotPlsns, latestPlsn);
        ASSERT_EQ(m_controlFile->UpdateWalStream(*streamInfo), DSTORE_SUCC);
        if (syncCheckpointMgr) {
            CheckpointMgr *checkpointMgr = g_storageInstance->GetPdb(g_defaultPdbId)->GetCheckpointMgr();
            ASSERT_NE(checkpointMgr, nullptr);
            bool found = false;
            dlist_iter iter;
            dlist_foreach(iter, checkpointMgr->GetCheckpointInfoList()) {
                WalCheckpointInfoData *checkpointInfo = dlist_container(WalCheckpointInfoData, node, iter.cur);
                if (checkpointInfo->walId != streamInfo->walId) {
                    continue;
                }
                checkpointInfo->lastCheckPointRecoveryPlsn = streamInfo->lastCheckpointPLsn;
                checkpointInfo->lastCheckPoint = streamInfo->lastWalCheckpoint;
                checkpointInfo->lastCheckPointHistory = streamInfo->checkpointHistory;
                found = true;
                break;
            }
            if (!found) {
                checkpointMgr->Init();
                dlist_foreach(iter, checkpointMgr->GetCheckpointInfoList()) {
                    WalCheckpointInfoData *checkpointInfo = dlist_container(WalCheckpointInfoData, node, iter.cur);
                    if (checkpointInfo->walId != streamInfo->walId) {
                        continue;
                    }
                    checkpointInfo->lastCheckPointRecoveryPlsn = streamInfo->lastCheckpointPLsn;
                    checkpointInfo->lastCheckPoint = streamInfo->lastWalCheckpoint;
                    checkpointInfo->lastCheckPointHistory = streamInfo->checkpointHistory;
                    found = true;
                    break;
                }
            }
            ASSERT_TRUE(found);
        }
        m_controlFile->FreeWalStreamsInfo(streamInfo);
    }

    void CorruptWalGroupLen(uint64 groupStartPlsn)
    {
        WalFile *walFile = m_walStream->GetWalFileManager()->GetWalFileByPlsn(groupStartPlsn);
        ASSERT_NE(walFile, nullptr);
        WalRecordAtomicGroup walGroup = {};
        int64 readSize = 0;
        int64 offset = static_cast<int64>(groupStartPlsn - walFile->GetStartPlsn());
        ASSERT_EQ(walFile->Read(&walGroup, sizeof(walGroup), offset, &readSize), DSTORE_SUCC);
        ASSERT_EQ(static_cast<uint64>(readSize), sizeof(walGroup));
        walGroup.groupLen = sizeof(WalRecordAtomicGroup) - 1;
        ASSERT_EQ(walFile->PwriteSync(&walGroup, sizeof(walGroup), offset), DSTORE_SUCC);
        ASSERT_EQ(walFile->Fsync(), DSTORE_SUCC);
    }
    void WriteWalGroupsOfManyTypes(uint32 walGroupNum, bool needDDL = false)
    {
        ReStartWal();
        uint64 startPlsn = m_walStream->GetMaxWrittenToFilePlsn();
        g_walGroupNum = walGroupNum;
        TableSpace *tablespace = (TableSpace *)UtMockModule::UtGetTableSpace(2);
        RetStatus retStatus = tablespace->AllocAndAddDataFile(g_defaultPdbId, &g_walUtFileId, EXT_SIZE_8192, false);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        BufMgrInterface *bufferMgr = g_storageInstance->GetBufferMgr();
        Page *pages[MAX_PAGE_NUM] = {nullptr};
        char *data = static_cast<char *>(DstorePalloc(WAL_RECORD_APPEND_DATA_LEN));
        for (uint32 i = 0; i < g_walGroupNum; i++) {
            m_walWriter->BeginAtomicWal(INVALID_XID);
            if (needDDL && i % INSERT_DDL_COUNT == 0) {
                WalRecordTbsLogical walRecordTbsLogical;
                walRecordTbsLogical.m_type = WAL_EMPTY_DDL_REDO;
                walRecordTbsLogical.m_size = sizeof(WalRecordTbsLogical);
                walRecordTbsLogical.tablespaceId = i;
                walRecordTbsLogical.preReuseVersion = WalUtils::GetTbsVersion(g_defaultPdbId, i);
                m_walWriter->PutNewWalRecord(&walRecordTbsLogical);
                m_walWriter->Append(data, WAL_RECORD_APPEND_DATA_LEN);
                m_ddlLsnInfo = m_walWriter->EndAtomicWal();
            } else {
                for (uint32 j = 0; j < MAX_PAGE_NUM; j++) {
                    PageId pageId = {g_walUtFileId, j};
                    BufferDesc *bufferDesc = bufferMgr->Read(g_defaultPdbId, pageId, LW_EXCLUSIVE);
                    pages[j] = bufferDesc->GetPage();
                    pages[j]->m_header.m_myself = pageId;
                    bufferMgr->MarkDirty(bufferDesc);
                    WalRecordForPage walRecordForPage;
                    WalPageHeaderContext header = {WAL_EMPTY_REDO,
                                                   sizeof(WalRecordForPage),
                                                   pageId,
                                                   pages[j]->m_header.m_walId,
                                                   pages[j]->m_header.m_plsn,
                                                   pages[j]->m_header.m_glsn,
                                                   false,
                                                   WalUtils::GetFileVersion(g_defaultPdbId, pageId.m_fileId)};
                    walRecordForPage.SetWalPageHeader(header);
                    m_walWriter->RememberPageNeedWal(bufferDesc);
                    m_walWriter->PutNewWalRecord(&walRecordForPage);
                    m_walWriter->Append(data, WAL_RECORD_APPEND_DATA_LEN);
                    bufferMgr->UnlockAndRelease(bufferDesc);
                }
                (void)m_walWriter->EndAtomicWal();
                for (uint32 j = 0; j < MAX_PAGE_NUM; j++) {
                    m_groupLsnInfos[j][i] = {pages[j]->GetWalId(), startPlsn, pages[j]->GetPlsn()};
                }
            }
        }
        m_walWriter->WaitTargetPlsnPersist(m_groupLsnInfos[MAX_PAGE_NUM - 1][g_walGroupNum - 1].m_walId,
                                           m_groupLsnInfos[MAX_PAGE_NUM - 1][g_walGroupNum - 1].m_endPlsn);
        if (needDDL) {
            m_walWriter->WaitTargetPlsnPersist(m_ddlLsnInfo);
        }
        DstorePfree(data);

        UtMockModule::UtDropTableSpace(tablespace);
    }
    void WriteWalRecords()
    {
        ReStartWal(16 * 1024);
        char *data = (char *)calloc(m_segSize, sizeof(char));
        WalRecord *record = reinterpret_cast<WalRecord *>(data);
        WalGroupLsnInfo walRecordGroupPtr = {INVALID_WAL_ID, INVALID_PLSN, INVALID_PLSN};
        m_walWriter->BeginAtomicWal(INVALID_XID);
        record->m_size = m_segSize / 2;
        record->SetType(WAL_EMPTY_REDO);
        m_walWriter->PutNewWalRecord(record);
        m_recordsEndPlsn[0] = sizeof(WalRecordAtomicGroup) + m_segSize / 2 + WAL_FILE_HDR_SIZE;
        record->m_size = m_segSize / 2 - 1000;
        m_walWriter->PutNewWalRecord(record);
        m_recordsEndPlsn[1] = (m_recordsEndPlsn[0] + m_segSize / 2 - 1000);
        record->m_size = m_segSize / 2 - 2000;
        m_walWriter->PutNewWalRecord(record);
        m_recordsEndPlsn[2] = (m_recordsEndPlsn[1] + m_segSize / 2 - 2000 + WAL_FILE_HDR_SIZE);
        m_walWriter->EndAtomicWal();
        m_walWriter->BeginAtomicWal(INVALID_XID);
        record->m_size = m_segSize - 1000;
        m_walWriter->PutNewWalRecord(record);
        m_recordsEndPlsn[3] = (m_recordsEndPlsn[2] + sizeof(WalRecordAtomicGroup) +
                              m_segSize - 1000 + WAL_FILE_HDR_SIZE);
        walRecordGroupPtr = m_walWriter->EndAtomicWal();
        ASSERT_EQ(walRecordGroupPtr.m_endPlsn, m_recordsEndPlsn[3]);
        m_walWriter->WaitTargetPlsnPersist(walRecordGroupPtr.m_walId, walRecordGroupPtr.m_endPlsn);
        free(data);
    }

    void WriteWalRecordsFullFile()
    {
        (void)fprintf(stderr, "WriteWalRecordsFullFile start\n");
        ReStartWal(16 * 1024);
        char *data = static_cast<char *>(DstorePalloc(2 * g_storageInstance->GetGuc()->walFileSize));
        WalRecord *record = reinterpret_cast<WalRecord *>(data);
        WalGroupLsnInfo walRecordGroupPtr = {INVALID_WAL_ID, INVALID_PLSN, INVALID_PLSN};
        m_walStream->PauseRecycle();
        m_walWriter->BeginAtomicWal(INVALID_XID);
        uint16 fileCount = m_walStream->m_walFileManager->GetAllWalFileCount();
        record->m_size = fileCount * g_storageInstance->GetGuc()->walFileSize - sizeof(WalRecordAtomicGroup) -
            fileCount * WAL_FILE_HDR_SIZE;
        record->SetType(WAL_EMPTY_REDO);
        m_walWriter->PutNewWalRecord(record);
        walRecordGroupPtr = m_walWriter->EndAtomicWal();
        (void)fprintf(stderr, "WriteWalRecordsFullFile m_endPlsn %lu, expect %lu\n", walRecordGroupPtr.m_endPlsn,
            fileCount * g_storageInstance->GetGuc()->walFileSize);
        ASSERT_EQ(walRecordGroupPtr.m_endPlsn, fileCount * g_storageInstance->GetGuc()->walFileSize);
        m_walWriter->WaitTargetPlsnPersist(walRecordGroupPtr.m_walId, walRecordGroupPtr.m_endPlsn);
        (void)fprintf(stderr, "WriteWalRecordsFullFile start %u files\n", fileCount);
        DstorePfree(data);
    }

    void ReStartWal(uint32 walFileSize = 16 * 1024 * 1024)
    {
        char pdbWalPath[MAXPGPATH] = {};
        int rc;
        if (g_defaultPdbId == PDB_TEMPLATE1_ID) {
            rc = sprintf_s(pdbWalPath, MAXPGPATH, "%s/%s/%s/wal",  g_storageInstance->GetGuc()->dataDir, BASE_DIR, g_storageInstance->m_guc->tenantConfig->storageConfig.template1VfsName);
        } else {
            rc = sprintf_s(pdbWalPath, MAXPGPATH, "%s/%s/%s/wal", g_storageInstance->GetGuc()->dataDir, BASE_DIR, g_storageInstance->m_guc->tenantConfig->storageConfig.rootpdbVfsName);
        }
        storage_securec_check_ss(rc);
        char cmd[MAXPGPATH] = {};
        rc = sprintf_s(cmd, MAXPGPATH, "rm -r %s/*", pdbWalPath);
        storage_securec_check_ss(rc);
        rc = system(cmd);
        ASSERT_EQ(rc, 0);

        g_storageInstance->GetGuc()->walFileSize = walFileSize;
        g_storageInstance->GetGuc()->walFileNumber = 1;
        m_walManager->Destroy();
        m_walManager->Init(m_walControlFile);

        m_walStreamManager = const_cast<WalStreamManager *>(m_walManager->GetWalStreamManager());
        delete thrd->m_walWriterContext;
        thrd->m_walWriterContext = DstoreNew(thrd->m_memoryMgr->GetRoot())
                AtomicWalWriterContext(thrd->m_memoryMgr->GetRoot(), m_pdbId,
                                       g_storageInstance->GetPdb(m_pdbId)->GetWalMgr());

        if (thrd->m_walWriterContext != nullptr) {
            UNUSE_PARAM RetStatus walWriterInitResult = thrd->m_walWriterContext->Init();
            StorageAssert(walWriterInitResult == DSTORE_SUCC);
        }
        m_walWriter = thrd->m_walWriterContext;

        m_walStream = m_walStreamManager->GetWritingWalStream();

        NodeId selfNode = 0;
        m_walStreamManager->InitWalStreamBgWriter(m_walStream);

        m_walRecovery = m_walStream->GetWalRecovery();
        m_startReadPlsn = m_walStream->GetMaxWrittenToFilePlsn();
    }

    void TearDown() override
    {
        WALBASICTEST::TearDown();
    }
    char m_configFilePath[MAXPGPATH];
    WalGroupLsnInfo m_groupLsnInfos[MAX_PAGE_NUM][MAX_WAL_GROUP_NUM] = {};
    WalGroupLsnInfo m_ddlLsnInfo;
    uint64 m_recordsEndPlsn[WAL_RECORDS_NUM] = {};
    ControlFile *m_controlFile;
    uint64 m_segSize;
    StorageInstance *m_instance;
    TableSpaceInterface *m_testTbs;
    BufMgrInterface *m_testBufMgr;
    WalRecovery *m_walRecovery;
    WalStream *m_walStream;
    uint64 m_startReadPlsn;

    void GetPageWalRecordInfoListAndCheck(std::atomic_bool *needStop)
    {
        (void)g_storageInstance->CreateThreadAndRegister();
        (void)pthread_setname_np(pthread_self(), "GetPageWalRecordInfoListAndCheck Worker");

        RetStatus retStatus;
        bool pageChecked[MAX_PAGE_NUM] = {false};
        uint32 pageCheckedNum = 0;
        while (pageCheckedNum < MAX_PAGE_NUM) {
            for (uint32 i = 0; i < MAX_PAGE_NUM; i++) {
                if (pageChecked[i]) {
                    continue;
                }
                PageId pageId = {g_walUtFileId, i};
                bool isRecoveryDone;
                BigArray::Iter walRecordIter = BigArray::Iter();
                retStatus = m_walManager->GetPageWalRecordInfoListAndLock(pageId, {m_groupLsnInfos[0][0].m_walId, 0, 0},
                                                                          false, &walRecordIter, &isRecoveryDone);
                ASSERT_EQ(retStatus, DSTORE_SUCC);
                if (walRecordIter.curNode != nullptr) {
                    pageChecked[i] = true;
                    pageCheckedNum++;
                    dlist_iter iter;
                    uint32 recordCnt = 0;
                    for (; !walRecordIter.IsEnd(); walRecordIter++) {
                        WalRecordInfoListNode *listNode = *walRecordIter;
                        bool needFreeWalRecord = false;
                        WalRecovery *walRecovery =
                            m_walManager->GetWalStreamManager()->GetWalStream(m_groupLsnInfos[0][0].m_walId)->GetWalRecovery();
                        WalRecord* walRecord = walRecovery->GetWalRecordForPageByPlsn(listNode->startPlsn,
                            listNode->walRecordSize, pageId, &needFreeWalRecord);
                        if (walRecord == nullptr) {
                            ErrLog(DSTORE_PANIC, MODULE_WAL,
                                ErrMsg("Get wal record fail, pageId(%hu,%u)",
                                pageId.m_fileId, pageId.m_blockId));
                            walRecovery->ReleasePageWalRecordsLwlock();
                            return;
                        }
                        if (walRecord->m_type != WAL_EMPTY_REDO) {
                            continue;
                        }
                        WalRecordForPage *walRecordForPage = reinterpret_cast<WalRecordForPage *>(walRecord);
                        ASSERT_EQ(walRecordForPage->m_pageId, pageId);
                        ASSERT_EQ(walRecordForPage->m_size, sizeof(WalRecordForPage) + WAL_RECORD_APPEND_DATA_LEN);
                        ASSERT_EQ(walRecordForPage->m_type, WAL_EMPTY_REDO);
                        ASSERT_EQ(listNode->walRecordSize, sizeof(WalRecordForPage) + WAL_RECORD_APPEND_DATA_LEN);
                        if (needFreeWalRecord) {
                            DstorePfreeExt(walRecordForPage);
                        }
                        ASSERT_EQ(listNode->redoContext.walId, m_groupLsnInfos[i][recordCnt].m_walId);
                        ASSERT_EQ(listNode->redoContext.recordEndPlsn, m_groupLsnInfos[i][recordCnt].m_endPlsn);
                        ASSERT_EQ(listNode->redoContext.xid, INVALID_XID);
                        recordCnt++;
                    }
                    ASSERT_EQ(recordCnt, g_walGroupNum);
                    m_walManager->ReleasePageWalRecordInfoListLock(m_groupLsnInfos[0][0].m_walId, false);
                    ErrLog(DSTORE_WARNING, MODULE_WAL, ErrMsg("Do one GetPageWalRecordInfoListAndCheck"
                        " pageId(%hu, %u)", pageId.m_fileId, pageId.m_blockId));
                }
            }
        }
        needStop->store(true);
        g_storageInstance->UnregisterThread();
    }
};

TEST_F (WalRecoveryTest, BuildDirtyPageSetWhenAllFilesFull)
{
    WriteWalRecordsFullFile();
    ControlFile *controlFile = g_storageInstance->GetPdb(g_defaultPdbId)->GetControlFile();
    ControlWalStreamPageItemData *walStreamInfo = nullptr;
    RetStatus ret = controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &walStreamInfo);
    ASSERT_EQ(ret, DSTORE_SUCC);
    walStreamInfo->lastCheckpointPLsn = 0;
    ret = controlFile->UpdateWalStream(*walStreamInfo);
    ASSERT_EQ(ret, DSTORE_SUCC);
    controlFile->FreeWalStreamsInfo(walStreamInfo);
    m_walStream->DestroyBgWalWriter();
    m_walRecovery->Init(RedoMode::RECOVERY_REDO, PDB_TEMPLATE1_ID);
    ASSERT_EQ(m_walRecovery->IsDirtyPageSetBuilt(), false);
    ASSERT_EQ(m_walRecovery->BuildDirtyPageSetAndPageWalRecordListHtab(), DSTORE_SUCC);
    ASSERT_EQ(m_walRecovery->IsDirtyPageSetBuilt(), true);
    uint64 walStreamEndPlsn = GsAtomicReadU64(&(m_walRecovery->m_getDirtyPageReadBuffer->m_walStreamEndPlsn));
    uint16 fileCount = m_walStream->m_walFileManager->GetAllWalFileCount();
    (void)fprintf(stderr, "BuildDirtyPageSetWhenAllFilesFull walStreamEndPlsn %lu, expect %lu\n", walStreamEndPlsn,
            fileCount * g_storageInstance->GetGuc()->walFileSize);
    ASSERT_EQ(walStreamEndPlsn, fileCount * g_storageInstance->GetGuc()->walFileSize);
    (void)fprintf(stderr, "BuildDirtyPageSetWhenAllFilesFull arraySize %ld\n", m_walRecovery->m_dirtyPageEntryArraySize);
    ASSERT_EQ(m_walRecovery->m_dirtyPageEntryArraySize, 1);
}

TEST_F(WalRecoveryTest, PlsnSyncerTest)
{
    g_storageInstance->GetGuc()->recoveryWorkerNum = 10;
    m_walRecovery->Init(RedoMode::RECOVERY_REDO, g_defaultPdbId);
    m_walRecovery->PrepareBgThreads();
    BatchPutParaForWalReplay parallelEntrys;
    parallelEntrys.entryNum = 0;
    uint64 targetPlsn = 100;
    m_walRecovery->AddPlsnSyncer(parallelEntrys.redoWalRecordEntry[0], parallelEntrys.entryNum, targetPlsn);
    parallelEntrys.redoWalRecordEntry[parallelEntrys.entryNum++].recordEndPlsn = INVALID_PLSN;
    m_walRecovery->BatchPutToDispatchQueue(&parallelEntrys.redoWalRecordEntry[0], &parallelEntrys.entryNum, 1);
    m_walRecovery->m_dispatchWorkerThd->join();
    m_walRecovery->m_dispatchWorkerThd = nullptr;
    m_walRecovery->WaitAllWorkerRedoFinish();
    ASSERT_EQ(m_walRecovery->GetWorkersRedoFinishedPlsn(), targetPlsn);
    m_walRecovery->CleanUpBgThreads();
    g_storageInstance->GetGuc()->recoveryWorkerNum = 1;
}

TEST_F(WalRecoveryTest, WalRedoTest)
{
    WriteWalGroupsOfManyTypes(20, true);
    ControlFile *controlFile = g_storageInstance->GetPdb(g_defaultPdbId)->GetControlFile();
    ControlWalStreamPageItemData *walStreamInfo = nullptr;
    RetStatus ret = controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &walStreamInfo);
    ASSERT_EQ(ret, DSTORE_SUCC);
    walStreamInfo->lastCheckpointPLsn = 0;
    walStreamInfo->lastWalCheckpoint.diskRecoveryPlsn = m_groupLsnInfos[0][0].m_endPlsn;
    ret = controlFile->UpdateWalStream(*walStreamInfo);
    ASSERT_EQ(ret, DSTORE_SUCC);
    controlFile->FreeWalStreamsInfo(walStreamInfo);
    m_walStream->DestroyBgWalWriter();
    ASSERT_EQ(m_walManager->Recovery(m_walStream->GetWalId(), g_defaultPdbId), DSTORE_SUCC);
}

TEST_F(WalRecoveryTest, ParallelWalRedoTest)
{
    g_storageInstance->GetGuc()->recoveryWorkerNum = 10;
    WriteWalGroupsOfManyTypes(200, true);
    ControlFile *controlFile = g_storageInstance->GetPdb(g_defaultPdbId)->GetControlFile();
    ControlWalStreamPageItemData *walStreamInfo = nullptr;
    RetStatus ret = controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &walStreamInfo);
    ASSERT_EQ(ret, DSTORE_SUCC);
    walStreamInfo->lastCheckpointPLsn = 0;
    walStreamInfo->lastWalCheckpoint.diskRecoveryPlsn = m_groupLsnInfos[0][0].m_endPlsn;
    ret = controlFile->UpdateWalStream(*walStreamInfo);
    ASSERT_EQ(ret, DSTORE_SUCC);
    controlFile->FreeWalStreamsInfo(walStreamInfo);
    m_walStream->DestroyBgWalWriter();
    ASSERT_EQ(m_walManager->Recovery(m_walStream->GetWalId(), g_defaultPdbId), DSTORE_SUCC);
}

TEST_F(WalRecoveryTest, UTWalRecovery_CheckpointHistory_NewestValid)
{
    WriteWalGroupsOfManyTypes(3);
    std::vector<WalGroupLsnInfo> groups = CollectWalGroupInfos(3);
    ASSERT_GE(groups.size(), 3U);

    ControlWalStreamPageItemData *streamInfo = nullptr;
    ASSERT_EQ(m_controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &streamInfo), DSTORE_SUCC);
    InstallCheckpointHistory(streamInfo,
        {groups[0].m_startPlsn + 1, groups[1].m_startPlsn + 1, groups[2].m_startPlsn},
        groups[2].m_startPlsn);

    m_walRecovery->InitRecoveryPlsn(m_walStream->GetWalId(), streamInfo);
    ASSERT_EQ(m_walRecovery->GetRecoveryStartPlsn(), groups[2].m_startPlsn);
    ASSERT_EQ(m_walRecovery->GetDiskRecoveryStartPlsn(), groups[2].m_startPlsn);
    m_controlFile->FreeWalStreamsInfo(streamInfo);
}

TEST_F(WalRecoveryTest, UTWalRecovery_CheckpointHistory_NewestCorrupted)
{
    WriteWalGroupsOfManyTypes(3);
    std::vector<WalGroupLsnInfo> groups = CollectWalGroupInfos(3);
    ASSERT_GE(groups.size(), 3U);
    CorruptWalGroupLen(groups[2].m_startPlsn);

    ControlWalStreamPageItemData *streamInfo = nullptr;
    ASSERT_EQ(m_controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &streamInfo), DSTORE_SUCC);
    InstallCheckpointHistory(streamInfo,
        {groups[0].m_startPlsn, groups[1].m_startPlsn, groups[2].m_startPlsn},
        groups[2].m_startPlsn);

    m_walRecovery->InitRecoveryPlsn(m_walStream->GetWalId(), streamInfo);
    ASSERT_EQ(m_walRecovery->GetRecoveryStartPlsn(), groups[1].m_startPlsn);
    ASSERT_EQ(m_walRecovery->GetDiskRecoveryStartPlsn(), groups[1].m_startPlsn);
    m_controlFile->FreeWalStreamsInfo(streamInfo);
}

TEST_F(WalRecoveryTest, UTWalRecovery_CheckpointHistory_OnlyOldestValid)
{
    WriteWalGroupsOfManyTypes(3);
    std::vector<WalGroupLsnInfo> groups = CollectWalGroupInfos(3);
    ASSERT_GE(groups.size(), 3U);
    CorruptWalGroupLen(groups[2].m_startPlsn);
    CorruptWalGroupLen(groups[1].m_startPlsn);

    ControlWalStreamPageItemData *streamInfo = nullptr;
    ASSERT_EQ(m_controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &streamInfo), DSTORE_SUCC);
    InstallCheckpointHistory(streamInfo,
        {groups[0].m_startPlsn, groups[1].m_startPlsn, groups[2].m_startPlsn},
        groups[2].m_startPlsn);

    m_walRecovery->InitRecoveryPlsn(m_walStream->GetWalId(), streamInfo);
    ASSERT_EQ(m_walRecovery->GetRecoveryStartPlsn(), groups[0].m_startPlsn);
    ASSERT_EQ(m_walRecovery->GetDiskRecoveryStartPlsn(), groups[0].m_startPlsn);
    m_controlFile->FreeWalStreamsInfo(streamInfo);
}

TEST_F(WalRecoveryTest, UTWalRecovery_CheckpointHistory_AllCorrupted)
{
    WriteWalGroupsOfManyTypes(3);
    std::vector<WalGroupLsnInfo> groups = CollectWalGroupInfos(3);
    ASSERT_GE(groups.size(), 3U);
    CorruptWalGroupLen(groups[2].m_startPlsn);
    CorruptWalGroupLen(groups[1].m_startPlsn);
    CorruptWalGroupLen(groups[0].m_startPlsn);

    ControlWalStreamPageItemData *streamInfo = nullptr;
    ASSERT_EQ(m_controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &streamInfo), DSTORE_SUCC);
    InstallCheckpointHistory(streamInfo,
        {groups[0].m_startPlsn, groups[1].m_startPlsn, groups[2].m_startPlsn},
        groups[2].m_startPlsn);

    EXPECT_DEATH(m_walRecovery->InitRecoveryPlsn(m_walStream->GetWalId(), streamInfo), "");
    m_controlFile->FreeWalStreamsInfo(streamInfo);
}

TEST_F(WalRecoveryTest, UTWalRecovery_CheckpointHistory_Truncate)
{
    WriteWalGroupsOfManyTypes(3);
    std::vector<WalGroupLsnInfo> groups = CollectWalGroupInfos(3);
    ASSERT_GE(groups.size(), 3U);

    PersistCheckpointHistory({groups[0].m_startPlsn, groups[1].m_startPlsn, groups[2].m_startPlsn + 1},
        groups[2].m_startPlsn + 1);
    m_walStream->DestroyBgWalWriter();

    ASSERT_EQ(m_walManager->Recovery(m_walStream->GetWalId(), g_defaultPdbId), DSTORE_SUCC);
    ASSERT_EQ(m_walRecovery->ProbeWalGroupAtPlsn(m_walStream->GetWalId(), groups[2].m_startPlsn), DSTORE_SUCC);
}

TEST_F(WalRecoveryTest, UTWalFileManager_RecyclablePlsn_TakesOldest)
{
    PersistCheckpointHistory({300, 100, 200}, 200, true);
    ASSERT_EQ(m_walStream->GetWalFileManager()->GetRecyclablePlsn(), 100U);

    PersistCheckpointHistory({}, 777, true);
    ASSERT_EQ(m_walStream->GetWalFileManager()->GetRecyclablePlsn(), 777U);
}

TEST_F(WalRecoveryTest, UTControlWalInfo_BackwardCompat)
{
    WriteWalGroupsOfManyTypes(2);
    std::vector<WalGroupLsnInfo> groups = CollectWalGroupInfos(2);
    ASSERT_GE(groups.size(), 2U);

    ControlWalStreamPageItemData *streamInfo = nullptr;
    ASSERT_EQ(m_controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &streamInfo), DSTORE_SUCC);
    streamInfo->checkpointHistory = {};
    streamInfo->lastWalCheckpoint = BuildCheckpoint(groups[0].m_startPlsn);
    ASSERT_EQ(m_controlFile->UpdateWalStream(*streamInfo), DSTORE_SUCC);
    m_controlFile->FreeWalStreamsInfo(streamInfo);

    StoragePdb *pdb = g_storageInstance->GetPdb(g_defaultPdbId);
    ASSERT_NE(pdb, nullptr);
    ASSERT_EQ(pdb->CreateCheckpointForWalStream(m_walStream, groups[1].m_endPlsn), DSTORE_SUCC);

    streamInfo = nullptr;
    ASSERT_EQ(m_controlFile->GetWalStreamInfo(m_walStream->GetWalId(), &streamInfo), DSTORE_SUCC);
    ASSERT_EQ(streamInfo->checkpointHistory.slotCount, 1);
    ASSERT_EQ(streamInfo->checkpointHistory.newestSlotIdx, 0);
    ASSERT_EQ(streamInfo->checkpointHistory.slots[0].diskRecoveryPlsn, groups[1].m_endPlsn);
    ASSERT_EQ(streamInfo->lastWalCheckpoint.diskRecoveryPlsn,
              streamInfo->checkpointHistory.slots[streamInfo->checkpointHistory.newestSlotIdx].diskRecoveryPlsn);
    m_controlFile->FreeWalStreamsInfo(streamInfo);
}

TEST_F (WalRecoveryTest, DISABLED_BuildDirtyPageSetAndPageWalRecordListHtabTest)
{
    WriteWalGroupsOfManyTypes(20);
    WalRecovery *walRecovery = DstoreNew(m_ut_memory_context) WalRecovery(
        m_ut_memory_context, m_walStream, g_storageInstance->GetGuc()->walFileSize, m_groupLsnInfos[0][0].m_walId);
    walRecovery->Init(RedoMode::RECOVERY_REDO, PDB_TEMPLATE1_ID);
    ASSERT_EQ(walRecovery->IsDirtyPageSetBuilt(), false);
    ASSERT_EQ(walRecovery->BuildDirtyPageSetAndPageWalRecordListHtab(), DSTORE_SUCC);
    ASSERT_EQ(walRecovery->IsDirtyPageSetBuilt(), true);

    long arraySize = 0;
    WalDirtyPageEntry *array = walRecovery->GetDirtyPageEntryArrayCopy(arraySize);
    ASSERT_NE(arraySize, 0);
    DstorePfree(array);
    array = nullptr;
    walRecovery->Destroy();
}

TEST_F (WalRecoveryTest, GetRecordEndPlsnTest)
{
    WriteWalRecords();
    WalReaderConf conf = {m_walStream->GetWalId(), m_startReadPlsn, m_walStream, nullptr, m_segSize,
                          DSTORE::WalReadSource::WAL_READ_FROM_DISK};
    WalRecordReader *walRecordReader = DstoreNew(m_ut_memory_context) WalRecordReader(m_ut_memory_context, conf);
    walRecordReader->Init();

    const WalRecordAtomicGroup *walGroup = nullptr;
    const WalRecord *walRecord = nullptr;
    uint32 recordsNum = 0;
    RetStatus retStatus;
    do {
        retStatus = walRecordReader->ReadNext(&walGroup);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        if (walGroup == nullptr) {
            break;
        }
        do {
            walRecord = walRecordReader->GetNextWalRecord();
            if (walRecord == nullptr) {
                break;
            }
            ASSERT_EQ(walRecordReader->GetCurRecordEndPlsn(), m_recordsEndPlsn[recordsNum++]);
        } while (true);
    } while (true);

    delete walRecordReader;
}

TEST_F (WalRecoveryTest, ParallelTbsWalRedoTest)
{
    HeapNormalSegment *segment = (HeapNormalSegment*)SegmentTest::UTAllocSegment(g_defaultPdbId, m_testTbs->GetTablespaceId(), m_testBufMgr, SegmentType::HEAP_SEGMENT_TYPE);
    ASSERT_TRUE(SegmentIsValid(segment));
    for (uint32 i = 0; i < 22; i++) {
        ASSERT_NE(segment->GetNewPage(), INVALID_PAGE_ID);
    }
    m_walWriter->WaitTargetPlsnPersist(m_walStream->GetWalId(), m_walStream->GetMaxAppendedPlsn());

    ReStartWal();
    g_storageInstance->GetGuc()->recoveryWorkerNum = 1;
    g_traceSwitch = WAL_TRACE_SWITCH;
    m_walStream->DestroyBgWalWriter();
    ASSERT_EQ(m_walManager->Recovery(m_walStream->GetWalId(), g_defaultPdbId), DSTORE_SUCC);
    ReStartWal();
    g_storageInstance->GetGuc()->recoveryWorkerNum = 10;
    m_walStream->DestroyBgWalWriter();
    ASSERT_EQ(m_walManager->Recovery(m_walStream->GetWalId(), g_defaultPdbId), DSTORE_SUCC);
    g_traceSwitch = 0;
}

TEST_F (WalRecoveryTest, GetPageWalRecordInfoListTest)
{
    WriteWalGroupsOfManyTypes(MAX_WAL_GROUP_NUM);
    std::atomic_bool needStop1{false};
    std::atomic_bool needStop2{false};
    BufMgrInterface *bufferMgr = g_storageInstance->GetBufferMgr();
    BufferDesc *bufferDescs[MAX_PAGE_NUM] = {nullptr};
    for (uint32 j = 0; j < MAX_PAGE_NUM; j++) {
        bufferDescs[j] = bufferMgr->Read(g_defaultPdbId, {g_walUtFileId, j}, LW_EXCLUSIVE);
    }
    std::thread getWalThd1 = std::thread(&WalRecoveryTest::GetPageWalRecordInfoListAndCheck, this, &needStop1);
    std::thread getWalThd2 = std::thread(&WalRecoveryTest::GetPageWalRecordInfoListAndCheck, this, &needStop2);
    std::thread recoveryThd = std::thread([this]() {
        (void)g_storageInstance->CreateThreadAndRegister();
        m_walStream->DestroyBgWalWriter();
        this->m_walManager->Recovery(m_walStream->GetWalId(), g_defaultPdbId);
        g_storageInstance->UnregisterThread();
    });
    while (!needStop1.load() || !needStop2.load()) {
        GaussUsleep(1000);
    }
    for (uint32 j = 0; j < MAX_PAGE_NUM; j++) {
        bufferMgr->UnlockAndRelease(bufferDescs[j]);
    }
    getWalThd1.join();
    getWalThd2.join();
    recoveryThd.join();
}

void ThrdWriteWal(WalRecord *data, double *microSumDuration, uint64 *flushCallTimes,
                   double *flushMinDuration, double *flushMaxDuration)
{
    create_thread_and_register();
    thrd->m_walWriterContext->Init();

    auto start = std::chrono::high_resolution_clock::now();
    unsigned seed = start.time_since_epoch().count();
    std::default_random_engine gen(seed);
    std::normal_distribution<double> dis(500, 1.5);
    std::chrono::duration<double> spendTime{};
    WalGroupLsnInfo walRecordGroupPtr{};
    RetStatus retStatus;
    uint64 times = 0;
    while(true) {
        thrd->m_walWriterContext->BeginAtomicWal(INVALID_XID);
        thrd->m_walWriterContext->PutNewWalRecord(data);
        walRecordGroupPtr = thrd->m_walWriterContext->EndAtomicWal();
        times++;
        if (times % 100 == 0) {
            auto flushStart = std::chrono::high_resolution_clock::now();
            thrd->m_walWriterContext->WaitTargetPlsnPersist(walRecordGroupPtr.m_walId, walRecordGroupPtr.m_endPlsn);
            std::chrono::duration<double, std::milli> flushTime = std::chrono::high_resolution_clock::now() - flushStart;
            double curTime = flushTime.count();
            (*microSumDuration) += curTime;
            (*flushCallTimes)++;
            if (curTime > *flushMaxDuration) {
                *flushMaxDuration = curTime;
            }
            if (curTime < *flushMinDuration) {
                *flushMinDuration = curTime;
            }
        }
        spendTime = std::chrono::high_resolution_clock::now() - start;
        if (spendTime.count() >= TEST_TIME_SECONDS) {
            break;
        }
    }
    unregister_thread();
}

TEST_F(WalRecoveryTest, DISABLED_WalMaxWriteSpeedTest)
{
    int recordLen = 10000;
    WalRecord *record = (WalRecord *)calloc(recordLen, sizeof(char));
    char *record_data = reinterpret_cast<char *>(record);
    for (uint32 i = 0; i < recordLen; i++) {
        record_data[i] = 'a' + i % 26;
    }
    record->SetSize(recordLen);
    int threadCount = 100;
    std::thread walContext[threadCount];

    double microSumDuration[threadCount];
    uint64 flushCallTimes[threadCount];
    double flushMinDuration[threadCount];
    double flushMaxDuration[threadCount];

    double sumAllFlushDuration = 0;
    uint64 sumAllFlushCallTimes = 0;

    double avgFlushDuration = 0;
    double minFlushDuration = 10000;
    double maxFlushDuration = 0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < threadCount; i++) {
        microSumDuration[i] = 0;
        flushCallTimes[i] = 0;
        flushMinDuration[i] = 10000;
        flushMaxDuration[i] = 0;
        walContext[i] = std::thread(ThrdWriteWal, record, &microSumDuration[i], &flushCallTimes[i],
                                    &flushMinDuration[i], &flushMaxDuration[i]);
    }

    for (int i = 0; i < threadCount; i++) {
        walContext[i].join();
        sumAllFlushDuration += microSumDuration[i];
        sumAllFlushCallTimes += flushCallTimes[i];
        avgFlushDuration = sumAllFlushDuration / sumAllFlushCallTimes;
        if (flushMinDuration[i] < minFlushDuration) {
            minFlushDuration = flushMinDuration[i];
        }
        if (flushMaxDuration[i] > maxFlushDuration) {
            maxFlushDuration = flushMaxDuration[i];
        }
    }
    std::chrono::duration<double> allTimeSecs = std::chrono::high_resolution_clock::now() - start;

    record->SetSize(MIN_WAL_RECORD_SIZE);
    m_walWriter->BeginAtomicWal(INVALID_XID);
    m_walWriter->PutNewWalRecord(record);
    WalGroupLsnInfo walRecordGroupPtr = m_walWriter->EndAtomicWal();
    uint32 sumFlushBytes = walRecordGroupPtr.m_startPlsn / 1024 / 1024;
    double speed =  sumFlushBytes / allTimeSecs.count();
    std::cout << "MB: " << sumFlushBytes << '\n'
              << "Time(s): " << allTimeSecs.count() << '\n'
              <<"Write speed: " << speed << "MB/S" << '\n'
              << " wait flush avg: " << avgFlushDuration<< '\n'
              << " wait flush min: " << minFlushDuration << '\n'
              << "wait flush max: " << maxFlushDuration<< '\n'
              << "wait flush sum: " << sumAllFlushDuration << '\n'
              << "wait flush call times: " << sumAllFlushCallTimes << '\n'
              << std::endl;
    free(record);
}

WalRecord *BuildWalRecordForPage(uint16 len, WalRecordLsnInfo preLsn)
{
    StorageAssert(len > sizeof(WalRecord) + sizeof(WalId) + sizeof(uint64) + sizeof(uint64));
    WalRecord *record = (WalRecord *)DstorePalloc(len);
    WalRecordForPage *record_data = reinterpret_cast<WalRecordForPage *>(record);
    record_data->m_pagePreWalId = preLsn.walId;
    record_data->m_pagePreGlsn = preLsn.glsn;
    record_data->m_pagePrePlsn = preLsn.endPlsn;
    record->SetSize(len);
    record->SetType(WalType::WAL_EMPTY_REDO);
    return record;
}
