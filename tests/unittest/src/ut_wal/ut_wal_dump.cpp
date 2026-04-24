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
#include "ut_wal/ut_wal_basic.h"
#include "common/algorithm/dstore_checksum_impl.h"
#include "control/dstore_control_file_page.h"
#include "control/dstore_control_walinfo.h"
#include "heap/dstore_heap_wal_struct.h"
#include "index/dstore_btree_wal.h"
#include "index/dstore_btree_recycle_wal.h"
#include "tablespace/dstore_tablespace_wal.h"
#include "undo/dstore_undo_wal.h"
#include "diagnose/dstore_wal_diagnose.h"
#include "wal/dstore_wal_dump.h"
#include "ut_mock/ut_mock.h"
#include "systable/dstore_systable_wal.h"
#include "patch_control_checkpoint.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace DSTORE {

static constexpr uint32 WAL_GROUP_NUM = 20;

class WalDumpTest : public WALBASICTEST {
protected:
    void SetUp() override
    {
        DSTORETEST::SetUp();
        WALBASICTEST::Prepare();
        PrepareControlFileContent();
        NodeId selfNode = 0;
        m_segSize = g_storageInstance->GetGuc()->walFileSize;
        RetStatus retStatus = m_walStreamManager->Init(m_walControlFile);
        ASSERT_EQ(retStatus, DSTORE_SUCC);

        uint32 dataLen = 2048;
        char *data = (char *)calloc(dataLen, sizeof(char));
        m_bufferMgr = g_storageInstance->GetBufferMgr();
        tablespace = DstoreNew((m_ut_memory_context))
            TableSpace(g_storageInstance->GetPdb(g_defaultPdbId)->GetControlFile(), 1);
        FileSize fileSize;
        fileSize.initialFileSize = INIT_FILE_SIZE;
        fileSize.maxFileSize = MAX_FILE_SIZE;
        retStatus = tablespace->AddFile(UT_WAL_FILE0_ID, fileSize, nullptr, EXT_SIZE_8192);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        PageId page_id = {UT_WAL_FILE0_ID, 2};
        BufferDesc *bufferDesc = m_bufferMgr->Read(g_defaultPdbId, page_id, LW_EXCLUSIVE);
        m_bufferMgr->MarkDirty(bufferDesc);
        for (uint32 i = 0; i < WAL_GROUP_NUM; i++) {
            uint32 size = sizeof(WalRecordHeapInsert) + dataLen;
            WalRecordHeapInsert *redoData = (WalRecordHeapInsert *) DstorePalloc(size);
            ItemPointerData ctid = {};
            WalType walType;
            if (i <= WAL_GROUP_NUM / 5) {
                walType = MODULE_DESC_TABLE[0].recordTypeLowerBound;
            } else if (i <= WAL_GROUP_NUM / 5 * 2) {
                walType = MODULE_DESC_TABLE[1].recordTypeLowerBound;
            } else if (i <= WAL_GROUP_NUM / 5 * 3) {
                walType = MODULE_DESC_TABLE[2].recordTypeLowerBound;
            } else if (i <= WAL_GROUP_NUM / 5 * 4) {
                walType = MODULE_DESC_TABLE[3].recordTypeLowerBound;
            } else if (i <= WAL_GROUP_NUM / 5 * 5) {
                walType = MODULE_DESC_TABLE[4].recordTypeLowerBound;
            }
            m_walWriter->BeginAtomicWal(INVALID_XID);
            m_walWriter->RememberPageNeedWal(bufferDesc);
            WalPageHeaderContext heapHeader = {walType, size, page_id, 0, 0, 0, false, EXCLUDED_FILE_VERSION};
            redoData->SetHeader(heapHeader, ctid.GetOffset(), INVALID_UNDO_RECORD_PTR);
            redoData->SetData(data, dataLen);
            m_walWriter->PutNewWalRecord(redoData);
            m_groupLsnInfos[i] = m_walWriter->EndAtomicWal();
            DstorePfree(redoData);
        }
        m_bufferMgr->UnlockAndRelease(bufferDesc);
        free(data);
        m_walWriter->WaitTargetPlsnPersist(m_groupLsnInfos[WAL_GROUP_NUM - 1]);
        ASSERT_EQ(m_walStreamManager->IsSelfWritingWalStream(m_walWriter->GetWalId()), true);
        ASSERT_EQ(m_walStreamManager->GetWalStream(m_walWriter->GetWalId())->GetMaxAppendedPlsn(),
                  m_groupLsnInfos[WAL_GROUP_NUM - 1].m_endPlsn);
        ASSERT_EQ(m_walStreamManager->GetWalStream(m_walWriter->GetWalId())->GetMaxFlushedPlsn(),
                  m_groupLsnInfos[WAL_GROUP_NUM - 1].m_endPlsn);
        InitWalDumpConfig();
        m_walDumper = new WalDumper(&m_walDumpConfig);

        fflush(stderr);
        setvbuf(stderr, nullptr, _IONBF, 0);
        m_saveErrFd = dup(STDERR_FILENO);
        m_fileFd = open("wal_dump_ut.log",(O_RDWR | O_CREAT), 0644);
        dup2(m_fileFd, STDERR_FILENO);
    }
    void TearDown() override
    {
        m_walDumper->Destroy();
        delete m_walDumper;

        dup2(m_saveErrFd, STDERR_FILENO);
        close(m_fileFd);
        close(m_saveErrFd);
        unlink("wal_dump_ut.log");

        WALBASICTEST::TearDown();
    }

    void InitWalDumpConfig()
    {
        char *pdbWalPath = StorageInstance::GetPdbWalPath(g_storageInstance->GetGuc()->dataDir, g_defaultPdbId);
        int rc = sprintf_s(m_walPath, MAXPGPATH, "%s/", pdbWalPath);
        ASSERT_GT(rc, 0);
        rc = sprintf_s(m_walDumpConfig.dir, MAXPGPATH, "%s/", pdbWalPath);
        ASSERT_GT(rc, 0);
        m_walDumpConfig.vfsType = StorageType::LOCAL;
        m_walDumpConfig.timelineId = TIME_LINE_ID;
        m_walDumpConfig.startPlsn = WAL_DUMP_INVALID_PLSN;
        m_walDumpConfig.endPlsn = WAL_DUMP_INVALID_PLSN;
        m_walDumpConfig.recordNumPerInputLimit = 0;
        m_walDumpConfig.displayedRecordNum = 0;
        m_walDumpConfig.vfsConfigPath = nullptr;
        m_walDumpConfig.moduleFilter = WAL_DUMP_INVALID_FILTER;
        m_walDumpConfig.typeFilter = WAL_DUMP_INVALID_FILTER;
        m_walDumpConfig.xidFilter = { WAL_DUMP_INVALID_XID_FILTER, WAL_DUMP_INVALID_XID_FILTER };
        m_walDumpConfig.pageIdFilter = INVALID_PAGE_ID;
        m_walDumpConfig.checkPageError = false;
        rc = sprintf_s(m_walDumpConfig.dumpDir, MAXPGPATH, "%s/", pdbWalPath);
        ASSERT_GT(rc, 0);
    }
    void DumpRecord(UNUSE_PARAM StringInfo str, const WalRecord *record, uint64 recordEndPlsn, Xid xid)
    {
        const WalRecord *walRecord = m_walDumper->DecompressProc(record);
        FILE *fp = fopen("waldump.txt", "w+");
        (void)fprintf(fp, "Wal record @ record end plsn:%lu; xid:(%d, %lu); ", recordEndPlsn,
                      static_cast<int>(xid.m_zoneId), xid.m_logicSlotId);
        WalType type = walRecord->m_type;
        for (uint16 i = 0; i < MAX_MODULE_ID; i++) {
            if (type >= MODULE_DESC_TABLE[i].recordTypeLowerBound && type <= MODULE_DESC_TABLE[i].recordTypeUpperBound) {
                switch (MODULE_DESC_TABLE[i].type) {
                    case ModuleType::HEAP:
                        WalRecordHeap::DumpHeapRecord(static_cast<const WalRecordHeap *>(walRecord), fp);
                        break;
                    case ModuleType::INDEX:
                        WalRecordIndex::DumpIndexRecord(static_cast<const WalRecordIndex *>(walRecord), fp);
                        break;
                    case ModuleType::BTREERECYCLE:
                        WalRecordBtrRecycle::DumpBtrRecycleRecord(
                            static_cast<const WalRecordBtrRecycle *>(walRecord), fp);
                        break;
                    case ModuleType::TABLESPACE:
                        WalRecordTbs::DumpTbsRecord(static_cast<const WalRecordTbs *>(walRecord), fp);
                        break;
                    case ModuleType::UNDO:
                        WalRecordUndo::DumpUndoRecord(static_cast<const WalRecordUndo *>(walRecord), fp);
                        break;
                    case ModuleType::CHECKPOINT:
                        (void)fprintf(fp, "CheckPoint no wal now.\n");
                        break;
                    case ModuleType::SYSTABLE:
                        WalRecordSystable::DumpSystableRecord(static_cast<const WalRecordSystable *>(walRecord), fp);
                        break;
                    default:
                        (void)fprintf(stderr, "Invalid wal type.\n");
                }
                break;
            }
        }
        (void)fprintf(fp, "\n");
        fclose(fp);
    }

    void DumpGroupInfo(StringInfo str, const WalRecordAtomicGroup *group, uint64 startPlsn)
    {
        str->append("\nRecord atomic group @ start plsn:%lu; xid:(%d, %lu); len:%u; record num:%hu\n", startPlsn,
                    static_cast<int>(group->xid.m_zoneId), group->xid.m_logicSlotId, group->groupLen, group->recordNum);
    }
    char *GetTargetGroupsDumpInfo(WalDumpConfig config, uint32 startGroupIndex, uint32 lastGroupIndex)
    {
        WalReaderConf readerConf = {0, m_groupLsnInfos[startGroupIndex].m_startPlsn,
            m_walStreamManager->GetWalStream(0), nullptr, m_segSize, WalReadSource::WAL_READ_FROM_DISK};
        WalRecordReader *reader = DstoreNew(m_ut_memory_context) WalRecordReader(m_ut_memory_context, readerConf);
        reader->Init();
        const WalRecordAtomicGroup *readGroup;
        const WalRecord *curRecord;
        RetStatus retStatus;
        StringInfoData str;
        str.init();
        for (uint32 i = startGroupIndex; i <= lastGroupIndex; i++) {
            retStatus = reader->ReadNext(&readGroup);
            EXPECT_EQ(retStatus, DSTORE_SUCC);
            EXPECT_NE(readGroup, nullptr);
            curRecord = reader->GetNextWalRecord();
            EXPECT_NE(curRecord, nullptr);
            if (config.moduleFilter != WAL_DUMP_INVALID_FILTER &&
                !(curRecord->m_type >= MODULE_DESC_TABLE[config.moduleFilter].recordTypeLowerBound &&
                curRecord->m_type <= MODULE_DESC_TABLE[config.moduleFilter].recordTypeUpperBound)) {
                continue;
            }
            if (config.typeFilter != WAL_DUMP_INVALID_FILTER && config.typeFilter != curRecord->m_type) {
                continue;
            }
            DumpGroupInfo(&str, readGroup, m_groupLsnInfos[i].m_startPlsn);
            DumpRecord(&str, curRecord, m_groupLsnInfos[i].m_endPlsn, readGroup->xid);
        }
        delete reader;
        return str.data;
    }
    void WalDumpToStr(WalDumpConfig config, char **str, uint64 *size)
    {
        ReInitWalDump(config, DSTORE_SUCC);
        fflush(stdout);
        setvbuf(stdout, nullptr, _IONBF, 0);
        int saveFd = dup(STDOUT_FILENO);
        int fd = open("waldump.txt",(O_RDWR | O_CREAT), 0644);
        dup2(fd, STDOUT_FILENO);
        WalId walId = 0;
        uint32 walFileSize = m_segSize;
        RetStatus retStatus = m_walDumper->Dump(DumpType::DUMP_ONE_STREAM, &walId, &walFileSize, 1);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        dup2(saveFd, STDOUT_FILENO);

        FILE *fp = fdopen(fd , "r");
        fseek(fp, 0, SEEK_END);
        uint64 fileSize = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        char *dumpStr = (char *)malloc(fileSize * sizeof(char) + 10);
        fread(dumpStr, fileSize, sizeof(char), fp);
        dumpStr[fileSize] = '\0';

        fclose(fp);
        close(fd);
        close(saveFd);
        unlink("waldump.txt");
        *str = dumpStr;
        *size = fileSize;
    }

    void CompareDumpResAndTargetGroups(WalDumpConfig config, uint32 startGroup, uint32 endGroup)
    {
        char *dumpStr;
        uint64 size;
        WalDumpToStr(config, &dumpStr, &size);
        char *targetStr = GetTargetGroupsDumpInfo(config, startGroup, endGroup);
        targetStr[size] = '\0';
        ASSERT_EQ(memcmp(dumpStr, targetStr, size), 0);
        DstorePfree(targetStr);
        free(dumpStr);
    }
    void ReInitWalDump(WalDumpConfig config, RetStatus expectedRes)
    {
        m_walDumper->Destroy();
        delete m_walDumper;
        m_walDumper = new WalDumper(&config);
        WalId walId = 0;
        uint32 walFileSize = m_segSize;
        RetStatus retStatus = m_walDumper->Init(&walId, &walFileSize, 1);
        ASSERT_EQ(retStatus, expectedRes);
    }
    void BuildWalRecord(bool usePageInfo, WalId walId, uint64 plsn, uint64 glsn)
    {
        Page *page = nullptr;
        char *data = static_cast<char *>(DstorePalloc(2024));

        m_walWriter->BeginAtomicWal(INVALID_XID);
        PageId pageId = {UT_WAL_FILE0_ID, 0};
        BufferDesc *bufferDesc = m_bufferMgr->Read(g_defaultPdbId, pageId, LW_EXCLUSIVE);
        page = bufferDesc->GetPage();
        page->m_header.m_myself = pageId;
        m_bufferMgr->MarkDirty(bufferDesc);
        WalRecordForPage walRecordForPage;
        WalPageHeaderContext header = {WAL_HEAP_INSERT,
                                       sizeof(WalRecordForPage),
                                       pageId,
                                       usePageInfo ? walId : page->m_header.m_walId,
                                       usePageInfo ? plsn : page->m_header.m_plsn,
                                       usePageInfo ? glsn : page->m_header.m_glsn,
                                       false,
                                       WalUtils::GetFileVersion(g_defaultPdbId, pageId.m_fileId)};
        walRecordForPage.SetWalPageHeader(header);
        m_walWriter->RememberPageNeedWal(bufferDesc);
        m_walWriter->PutNewWalRecord(&walRecordForPage);
        m_walWriter->Append(data, 2024);
        m_bufferMgr->UnlockAndRelease(bufferDesc);
        (void)m_walWriter->EndAtomicWal();
        DstorePfree(data);
    }

    std::string MakeTempControlFilePath()
    {
        char pathTemplate[] = "/tmp/ut_waldump_control_XXXXXX";
        int fd = mkstemp(pathTemplate);
        if (fd < 0) {
            ADD_FAILURE() << "mkstemp failed for temporary control file path";
            return "";
        }
        close(fd);
        unlink(pathTemplate);
        return std::string(pathTemplate);
    }

    void CreatePatchableControlFile(const std::string &path, uint64 walId, uint64 diskRecoveryPlsn)
    {
        int fd = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        ASSERT_GE(fd, 0);
        off_t fileSize = static_cast<off_t>(CONTROLFILE_PAGEMAP_WALSTREAM_START + 1) * BLCKSZ;
        ASSERT_EQ(ftruncate(fd, fileSize), 0);

        ControlDataPage page = {};
        page.InitDataPage(CONTROL_WAL_STREAM_DATAPAGE_TYPE);
        ControlWalStreamPageItemData item = {};
        item.walId = walId;
        item.lastWalCheckpoint.diskRecoveryPlsn = diskRecoveryPlsn;
        item.checkpointHistory.Push(item.lastWalCheckpoint);
        ASSERT_EQ(page.AddItem(&item, sizeof(item)), DSTORE_SUCC);
        page.m_pageHeader.m_checksum = CompChecksum(reinterpret_cast<const uint8 *>(&page), BLCKSZ, CHECKSUM_CRC);

        off_t offset = static_cast<off_t>(CONTROLFILE_PAGEMAP_WALSTREAM_START) * BLCKSZ;
        ASSERT_EQ(pwrite(fd, &page, BLCKSZ, offset), BLCKSZ);
        close(fd);
    }

    ControlWalStreamPageItemData ReadPatchedWalStreamItem(const std::string &path, uint32 *pageChecksum = nullptr)
    {
        ControlWalStreamPageItemData item = {};
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            ADD_FAILURE() << "open failed for control file " << path;
            return item;
        }
        ControlDataPage page = {};
        off_t offset = static_cast<off_t>(CONTROLFILE_PAGEMAP_WALSTREAM_START) * BLCKSZ;
        if (pread(fd, &page, BLCKSZ, offset) != BLCKSZ) {
            ADD_FAILURE() << "pread failed for control file " << path;
            close(fd);
            return item;
        }
        close(fd);

        if (pageChecksum != nullptr) {
            *pageChecksum = page.m_pageHeader.m_checksum;
        }
        errno_t rc = memcpy_s(&item, sizeof(item), page.m_data, sizeof(item));
        storage_securec_check(rc, "\0", "\0");
        return item;
    }
    WalDumper *m_walDumper = nullptr;
    WalDumpConfig m_walDumpConfig = {};
    uint64 m_segSize;
    WalGroupLsnInfo m_groupLsnInfos[WAL_GROUP_NUM] = {};
    char m_walPath[MAXPGPATH];
    int m_fileFd;
    int m_saveErrFd;
    BufMgrInterface *m_bufferMgr;
    TableSpace *tablespace;
};

TEST_F(WalDumpTest, DISABLED_WalDumpConfigCheckTest)
{
    m_walDumpConfig.startPlsn = m_segSize * 11;
    ReInitWalDump(m_walDumpConfig, DSTORE_FAIL);

    InitWalDumpConfig();
    m_walDumpConfig.startPlsn = m_segSize;
    m_walDumpConfig.endPlsn = 0;
    ReInitWalDump(m_walDumpConfig, DSTORE_FAIL);

    InitWalDumpConfig();
    m_walDumpConfig.startPlsn = 0;
    m_walDumpConfig.endPlsn = m_segSize - 1;
    ReInitWalDump(m_walDumpConfig, DSTORE_SUCC);

    InitWalDumpConfig();
    ReInitWalDump(m_walDumpConfig, DSTORE_SUCC);

    InitWalDumpConfig();
    m_walDumpConfig.startPlsn = m_groupLsnInfos[1].m_startPlsn;
    m_walDumpConfig.endPlsn = WAL_DUMP_INVALID_PLSN;
    ReInitWalDump(m_walDumpConfig, DSTORE_SUCC);

    InitWalDumpConfig();
    m_walDumpConfig.endPlsn = m_groupLsnInfos[0].m_endPlsn;
    ReInitWalDump(m_walDumpConfig, DSTORE_SUCC);
}

TEST_F(WalDumpTest, DISABLED_WalDumpPlsnRangeTest)
{
    m_walDumpConfig.startPlsn = WAL_DUMP_INVALID_PLSN;
    m_walDumpConfig.endPlsn = WAL_DUMP_INVALID_PLSN;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 1);

    m_walDumpConfig.startPlsn = m_groupLsnInfos[2].m_startPlsn - 10;
    m_walDumpConfig.endPlsn = WAL_DUMP_INVALID_PLSN;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 2, WAL_GROUP_NUM - 1);

    m_walDumpConfig.startPlsn = m_groupLsnInfos[3].m_startPlsn - 100;
    m_walDumpConfig.endPlsn = m_groupLsnInfos[WAL_GROUP_NUM - 2].m_endPlsn - 100;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 3, WAL_GROUP_NUM - 2);

    m_walDumpConfig.startPlsn = WAL_DUMP_INVALID_PLSN;
    m_walDumpConfig.endPlsn = m_groupLsnInfos[WAL_GROUP_NUM - 3].m_endPlsn - 100;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 3);
}

TEST_F(WalDumpTest, DISABLED_WalDumpFilterTest)
{
    m_walDumpConfig.typeFilter = 0;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 1);

    m_walDumpConfig.typeFilter = 1;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 1);

    m_walDumpConfig.typeFilter = MODULE_DESC_TABLE[1].recordTypeLowerBound;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 1);

    InitWalDumpConfig();
    m_walDumpConfig.moduleFilter = 0;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 1);

    m_walDumpConfig.moduleFilter = 1;
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 1);
}

TEST_F(WalDumpTest, DISABLED_WalDumpDefaultDirTest)
{
    char *pdbWalPath = StorageInstance::GetPdbWalPath(g_storageInstance->GetGuc()->dataDir, g_defaultPdbId);
    chdir(pdbWalPath);
    int rc = sprintf_s(m_walPath, MAXPGPATH, "./");
    ASSERT_GT(rc, 0);
    CompareDumpResAndTargetGroups(m_walDumpConfig, 0, WAL_GROUP_NUM - 1);
}

TEST_F(WalDumpTest, WaldumpCheckPageError)
{
    BuildWalRecord(false, 0, 0, 0);
    BufferDesc *bufferDesc = m_bufferMgr->Read(g_defaultPdbId, {UT_WAL_FILE0_ID, 0}, LW_EXCLUSIVE);
    Page *page = bufferDesc->GetPage();
    WalId prevWalId = page->m_header.m_walId;
    uint64 prevPlsn = page->m_header.m_plsn;
    uint64 prevGlsn = page->m_header.m_glsn;
    m_bufferMgr->UnlockAndRelease(bufferDesc);

    for (uint32 i = 0; i < 2; i++) {
        BuildWalRecord(true, prevWalId, prevPlsn, prevGlsn);
        for (int i = 0; i < 10; i++) {
            BuildWalRecord(false, 0, 0, 0);
        }
    }

    bufferDesc = m_bufferMgr->Read(g_defaultPdbId, {UT_WAL_FILE0_ID, 0}, LW_EXCLUSIVE);
    page = bufferDesc->GetPage();
    m_walWriter->WaitTargetPlsnPersist(page->GetWalId(), page->GetPlsn());
    m_bufferMgr->UnlockAndRelease(bufferDesc);
    UtMockModule::UtDropTableSpace(tablespace);
}

TEST_F(WalDumpTest, PatchControlCheckpointDryRunAndLiveMode)
{
    constexpr uint64 WAL_ID = 17;
    constexpr uint64 OLD_PLSN = 0x12340;
    constexpr uint64 DRY_RUN_PLSN = 0x23450;
    constexpr uint64 LIVE_PLSN = 0x34560;

    std::string controlFilePath = MakeTempControlFilePath();
    std::string backupPath = controlFilePath + ".bak";
    CreatePatchableControlFile(controlFilePath, WAL_ID, OLD_PLSN);

    struct stat before = {};
    ASSERT_EQ(stat(controlFilePath.c_str(), &before), 0);

    PatchControlCheckpointArgs args = {
        .controlFilePath = controlFilePath.c_str(),
        .walId = WAL_ID,
        .plsn = DRY_RUN_PLSN,
        .dryRun = true,
    };
    ASSERT_EQ(RunPatchControlCheckpoint(args), DSTORE_SUCC);

    struct stat afterDryRun = {};
    ASSERT_EQ(stat(controlFilePath.c_str(), &afterDryRun), 0);
    ASSERT_EQ(before.st_mtim.tv_sec, afterDryRun.st_mtim.tv_sec);
    ASSERT_EQ(before.st_mtim.tv_nsec, afterDryRun.st_mtim.tv_nsec);
    ASSERT_EQ(access(backupPath.c_str(), F_OK), -1);

    ControlWalStreamPageItemData item = ReadPatchedWalStreamItem(controlFilePath);
    ASSERT_EQ(item.lastWalCheckpoint.diskRecoveryPlsn, OLD_PLSN);
    ASSERT_EQ(item.checkpointHistory.slotCount, 1);
    ASSERT_EQ(item.checkpointHistory.slots[item.checkpointHistory.newestSlotIdx].diskRecoveryPlsn, OLD_PLSN);

    args.plsn = LIVE_PLSN;
    args.dryRun = false;
    ASSERT_EQ(RunPatchControlCheckpoint(args), DSTORE_SUCC);
    ASSERT_EQ(access(backupPath.c_str(), F_OK), 0);

    uint32 pageChecksum = 0;
    item = ReadPatchedWalStreamItem(controlFilePath, &pageChecksum);
    ASSERT_EQ(item.lastWalCheckpoint.diskRecoveryPlsn, LIVE_PLSN);
    ASSERT_EQ(item.checkpointHistory.slotCount, 1);
    ASSERT_EQ(item.checkpointHistory.slots[item.checkpointHistory.newestSlotIdx].diskRecoveryPlsn, LIVE_PLSN);

    ControlDataPage page = {};
    int fd = open(controlFilePath.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0);
    off_t offset = static_cast<off_t>(CONTROLFILE_PAGEMAP_WALSTREAM_START) * BLCKSZ;
    ASSERT_EQ(pread(fd, &page, BLCKSZ, offset), BLCKSZ);
    close(fd);
    page.m_pageHeader.m_checksum = 0;
    ASSERT_EQ(pageChecksum, CompChecksum(reinterpret_cast<const uint8 *>(&page), BLCKSZ, CHECKSUM_CRC));

    unlink(controlFilePath.c_str());
    unlink(backupPath.c_str());
}
}
