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

#include "ut_utilities/ut_dstore_framework.h"
#include "control/dstore_control_file.h"
#include "ut_buffer/ut_buffer_fake_instance.h"
#include "systable/sys_database.h"
#include "common/datatype/dstore_uuid_utils.h"
#include <thread>
#include <chrono>
#include <string>
#include <unordered_set>

using namespace DSTORE;

const char *UT_CONTROL_FILE_1 = "ut_control_file_1";
const char *UT_TABLESPACE_NAME_1 = "ut_tablespace_1";
const char *UT_CONTROL_FILE_2 = "ut_control_file_2";
static const uint32 MAX_FILE_BLOCK_COUNT = (uint32)1024 * 1024 * 64;
FileParameter tbsFilePara;
ControlFile *controlFile;

class ControlFileTest : public DSTORETEST {

protected:
    void SetUp() override {
        DSTORETEST::SetUp();
        UtInstance *instance = DstoreNew(m_ut_memory_context)UtInstance();
        instance->Install(&DSTORETEST::m_guc, m_ut_memory_context);
        instance->Startup(&DSTORETEST::m_guc);
        controlFile = g_storageInstance->GetPdb(g_defaultPdbId)->GetControlFile();

        tbsFilePara.streamId = VFS_DEFAULT_FILE_STREAM_ID;
        tbsFilePara.flag = IN_PLACE_WRITE_FILE;
        tbsFilePara.fileSubType = DATA_FILE_TYPE;
        tbsFilePara.rangeSize = TBS_FILE_RANGE_SIZE;
        tbsFilePara.maxSize = static_cast<uint64>(DSTORE_MAX_BLOCK_NUMBER) * BLCKSZ;
        tbsFilePara.recycleTtl = 0;
        tbsFilePara.mode = FILE_READ_AND_WRITE_MODE;
        tbsFilePara.isReplayWrite = false;
        TenantConfig *tenantConfig = g_storageInstance->GetGuc()->tenantConfig;
        StorageAssert(tenantConfig);
        errno_t rc = strcpy_s(tbsFilePara.storeSpaceName, STORESPACE_NAME_MAX_LEN,
                            tenantConfig->storeSpaces[0].storeSpaceName);
        storage_securec_check(rc, "\0", "\0");
    }

    void TearDown() override {
        UtInstance *instance = (UtInstance *)g_storageInstance;
        instance->Shutdown();
        delete instance;
        DSTORETEST::TearDown();
    }
};

TEST_F(ControlFileTest, CheckSysTbsWriteCF_level0) {
    for (uint16 i = 1; i <= 6; i++) {
        ControlTablespacePageItemData *tbsItem = controlFile->GetTbsPageItemPtr(i, nullptr);
        ASSERT_EQ(tbsItem->tablespaceId, i);
        if (i == 5 || i == 1) {
            ASSERT_EQ(tbsItem->hwm, 0);
            ASSERT_EQ(tbsItem->tbsMaxSize, MAX_TABLESPACE_SIZE);
            ASSERT_EQ(tbsItem->reuseVersion, 1);
            ASSERT_EQ(tbsItem->ddlXid, INVALID_XID);
            ASSERT_EQ(tbsItem->used, 1);
        } else if (i == 4) {
            ASSERT_EQ(tbsItem->hwm, 2); // In the undo process, two files are created.
            ASSERT_EQ(tbsItem->tbsMaxSize, MAX_TABLESPACE_SIZE);
            ASSERT_EQ(tbsItem->reuseVersion, 1);
            ASSERT_EQ(tbsItem->ddlXid, INVALID_XID);
            ASSERT_EQ(tbsItem->used, 1);
        } else {
            ASSERT_EQ(tbsItem->hwm, 0); // if UT, no file is created when create systablespace.
            ASSERT_EQ(tbsItem->tbsMaxSize, MAX_TABLESPACE_SIZE);
            ASSERT_EQ(tbsItem->reuseVersion, 1);
            ASSERT_EQ(tbsItem->ddlXid, INVALID_XID);
            ASSERT_EQ(tbsItem->used, 1);
        }
    }
    for (uint16 i = 1; i <= 2; i++) {
        ControlDataFilePageItemData *fileItem = controlFile->GetDataFilePageItemPtr(5120+i, nullptr);
        ASSERT_EQ(fileItem->fileId, 5120+i);
        ASSERT_EQ(fileItem->used, 1);
        ASSERT_NE(fileItem->extentSize, 0);
        ASSERT_EQ(fileItem->reuseVersion, 1);
        ASSERT_EQ(fileItem->fileMaxSize, MAX_FILE_SIZE);
        ASSERT_NE(fileItem->tablespaceId, 0);
        ASSERT_EQ(fileItem->ddlXid, INVALID_XID);
    }
    
    ControlDataFilePageItemData *fileItem = controlFile->GetDataFilePageItemPtr(21, nullptr);
    ASSERT_EQ(fileItem->used, 0);
}

TEST_F(ControlFileTest, AllocAndFreeFileId_level0) {
    FileId fileId = 100;
    BlockNumber blkno;
    OffsetNumber offset;
    RetStatus ret = DSTORE_FAIL;

    controlFile->GetDataFilePageItemCtid(fileId, &blkno, &offset);
    ASSERT_EQ(blkno, DEFAULT_DATAFILE_PAGE + 1);
    ASSERT_EQ(offset, 27);

    TablespaceId tablespaceId = 1;
    ExtentSize extentSize = EXT_SIZE_8;

    for (int i = 1; i <= 1000; i++) {
        ret = controlFile->AllocAndCreateDataFile(&fileId, tablespaceId, extentSize, false);
        ASSERT_EQ(ret, DSTORE_SUCC);
        ASSERT_EQ(fileId, 5120+2+i); // if UT, no file is created when create systablespace.
                                     // In the undo process, two files are created.
    }

    ControlDataFilePageItemData *fileItem = controlFile->GetDataFilePageItemPtr(fileId, nullptr);
    ASSERT_EQ(fileItem->fileId, fileId);
    ASSERT_EQ(fileItem->used, 1);
    ASSERT_EQ(fileItem->extentSize, extentSize);
    ASSERT_EQ(fileItem->reuseVersion, 1);
    ASSERT_EQ(fileItem->fileMaxSize, MAX_FILE_SIZE);
    ASSERT_EQ(fileItem->tablespaceId, tablespaceId);
    ASSERT_NE(fileItem->ddlXid.m_placeHolder, 0);

    ret = controlFile->FreeDataFileId(fileId, tablespaceId, MAX_SPACE_FILE_COUNT, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    fileItem = controlFile->GetDataFilePageItemPtr(fileId, nullptr);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(fileItem->fileId, fileId);
    ASSERT_EQ(fileItem->used, 0);
    ASSERT_EQ(fileItem->extentSize, INVALID_EXT_SIZE);
    ASSERT_EQ(fileItem->reuseVersion, 2);
    ASSERT_EQ(fileItem->fileMaxSize, 0);
    ASSERT_EQ(fileItem->tablespaceId, 0);
    ASSERT_EQ(fileItem->ddlXid, INVALID_XID);
    
    ret = controlFile->AllocAndCreateDataFile(&fileId, tablespaceId, extentSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(fileId, 5120+2+1000+1);
    fileItem = controlFile->GetDataFilePageItemPtr(fileId, nullptr);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(fileItem->fileId, fileId);
    ASSERT_EQ(fileItem->used, 1);
    ASSERT_EQ(fileItem->extentSize, extentSize);
    ASSERT_EQ(fileItem->reuseVersion, 1);
    ASSERT_EQ(fileItem->fileMaxSize, MAX_FILE_SIZE);
    ASSERT_EQ(fileItem->tablespaceId, tablespaceId);
    ASSERT_NE(fileItem->ddlXid.m_placeHolder, 0);

    for (int i = 1; i <= 999; i++) {
        ret = controlFile->FreeDataFileId(5120+2+i, tablespaceId, MAX_SPACE_FILE_COUNT, DSTORE::INVALID_XID, false);
        ASSERT_EQ(ret, DSTORE_SUCC);
    }
    ret = controlFile->FreeDataFileId(5120+2+1000+1, tablespaceId, MAX_SPACE_FILE_COUNT, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
}

TEST_F(ControlFileTest, AllocAndFreeTbsId1_level0) {
    TablespaceId tablespaceId = 100;
    BlockNumber blkno;
    OffsetNumber offset;
    RetStatus ret = DSTORE_FAIL;

    controlFile->GetTbsPageItemCtid(tablespaceId, &blkno, &offset);
    ASSERT_EQ(blkno, DEFAULT_TABLESPACE_PAGE + 34 - 1);
    ASSERT_EQ(offset, 1);

    uint64 tbsMaxSize = 1024 * 1024 * 600;
    for (int i = 1; i <= 1000; i++) {
        ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
        ASSERT_EQ(ret, DSTORE_SUCC);
        ASSERT_EQ(tablespaceId,24+i);
    }

    ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_FAIL);

    ControlTablespacePageItemData *tbsItem = controlFile->GetTbsPageItemPtr(888, nullptr);
    ASSERT_EQ(tbsItem->tablespaceId, 888);
    ASSERT_EQ(tbsItem->hwm, 0);
    ASSERT_EQ(tbsItem->tbsMaxSize, tbsMaxSize);
    ASSERT_EQ(tbsItem->reuseVersion, 1);
    ASSERT_NE(tbsItem->ddlXid.m_placeHolder, 0);
    ASSERT_EQ(tbsItem->used, 1);

    ret = controlFile->FreeTbsId(888, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    tbsItem = controlFile->GetTbsPageItemPtr(888, nullptr);
    ASSERT_EQ(tbsItem->tablespaceId, 888);
    ASSERT_EQ(tbsItem->hwm, 0);
    ASSERT_EQ(tbsItem->tbsMaxSize, 0);
    ASSERT_EQ(tbsItem->reuseVersion, 2);
    ASSERT_EQ(tbsItem->ddlXid, INVALID_XID);
    ASSERT_EQ(tbsItem->used, 0);

    ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(tablespaceId, 888);
    tbsItem = controlFile->GetTbsPageItemPtr(888, nullptr);
    ASSERT_EQ(tbsItem->tablespaceId, 888);
    ASSERT_EQ(tbsItem->hwm, 0);
    ASSERT_EQ(tbsItem->tbsMaxSize, tbsMaxSize);
    ASSERT_EQ(tbsItem->reuseVersion, 3);
    ASSERT_NE(tbsItem->ddlXid.m_placeHolder, 0);
    ASSERT_EQ(tbsItem->used, 1);

    for (int i = 1; i <= 1000; i++) {
        ret = controlFile->FreeTbsId(24+i, DSTORE::INVALID_XID, false);
        ASSERT_EQ(ret, DSTORE_SUCC);
    }
}

TEST_F(ControlFileTest, AllocAndFreeTbsId2_level0) {
    TablespaceId tablespaceId = 0;
    RetStatus ret = DSTORE_FAIL;

    uint64 tbsMaxSize = 1024 * 1024 * 600;
    for (int i = 1; i <= 100; i++) {
        ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
        ASSERT_EQ(ret, DSTORE_SUCC);
        ASSERT_EQ(tablespaceId,24+i);
    }

    ret = controlFile->FreeTbsId(60, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    for (int i = 1; i <= 900; i++) {
        ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
        ASSERT_EQ(ret, DSTORE_SUCC);
        ASSERT_EQ(tablespaceId,24+100+i);
    }

    ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(tablespaceId,60);

    ret = controlFile->FreeTbsId(60, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ret = controlFile->FreeTbsId(600, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(tablespaceId,600);

    for (int i = 1; i <= 1000; i++) {
        ret = controlFile->FreeTbsId(24+i, DSTORE::INVALID_XID, false);
        ASSERT_EQ(ret, DSTORE_SUCC);
    }
}

TEST_F(ControlFileTest, AddAndFreeFileIdToTbs_level0) {
    TablespaceId tablespaceId = 0;
    uint16 slotId;
    uint64 tbsMaxSize = 1024 * 1024 * 600;
    RetStatus ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(tablespaceId, 25);

    ret = controlFile->AddFileIdToTbs(tablespaceId, 3000, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AddFileIdToTbs(tablespaceId, 3001, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AddFileIdToTbs(tablespaceId, 3002, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AddFileIdToTbs(tablespaceId, 3003, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ControlTablespacePageItemData *tbsItem = controlFile->GetTbsPageItemPtr(tablespaceId, nullptr);
    ASSERT_EQ(tbsItem->tablespaceId, tablespaceId);
    ASSERT_EQ(tbsItem->hwm, 4);
    ASSERT_EQ(tbsItem->tbsMaxSize, tbsMaxSize);
    ASSERT_EQ(tbsItem->reuseVersion, 1);
    ASSERT_NE(tbsItem->ddlXid.m_placeHolder, 0);
    ASSERT_EQ(tbsItem->used, 1);
    ASSERT_EQ(tbsItem->fileIds[0], 3000);
    ASSERT_EQ(tbsItem->fileIds[1], 3001);
    ASSERT_EQ(tbsItem->fileIds[2], 3002);
    ASSERT_EQ(tbsItem->fileIds[3], 3003);

    ret = controlFile->FreeFileIdFromTbs(tablespaceId, 3002, &slotId);
    ASSERT_EQ(ret, DSTORE_SUCC);
    tbsItem = controlFile->GetTbsPageItemPtr(tablespaceId, nullptr);
    ASSERT_EQ(tbsItem->tablespaceId, tablespaceId);
    ASSERT_EQ(tbsItem->hwm, 4);
    ASSERT_EQ(tbsItem->tbsMaxSize, tbsMaxSize);
    ASSERT_EQ(tbsItem->reuseVersion, 1);
    ASSERT_NE(tbsItem->ddlXid.m_placeHolder, 0);
    ASSERT_EQ(tbsItem->used, 1);
    ASSERT_EQ(tbsItem->fileIds[0], 3000);
    ASSERT_EQ(tbsItem->fileIds[1], 3001);
    ASSERT_EQ(tbsItem->fileIds[2], 0);
    ASSERT_EQ(tbsItem->fileIds[3], 3003);

    ret = controlFile->AddFileIdToTbs(tablespaceId, 3004, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    tbsItem = controlFile->GetTbsPageItemPtr(tablespaceId, nullptr);
    ASSERT_EQ(tbsItem->tablespaceId, tablespaceId);
    ASSERT_EQ(tbsItem->hwm, 4);
    ASSERT_EQ(tbsItem->tbsMaxSize, tbsMaxSize);
    ASSERT_EQ(tbsItem->reuseVersion, 1);
    ASSERT_NE(tbsItem->ddlXid.m_placeHolder, 0);
    ASSERT_EQ(tbsItem->used, 1);
    ASSERT_EQ(tbsItem->fileIds[0], 3000);
    ASSERT_EQ(tbsItem->fileIds[1], 3001);
    ASSERT_EQ(tbsItem->fileIds[2], 3004);
    ASSERT_EQ(tbsItem->fileIds[3], 3003);

    ret = controlFile->FreeFileIdFromTbs(tablespaceId, 3001, &slotId);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->FreeFileIdFromTbs(tablespaceId, 3004, &slotId);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->FreeFileIdFromTbs(tablespaceId, 3000, &slotId);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->FreeFileIdFromTbs(tablespaceId, 3003, &slotId);
    ASSERT_EQ(ret, DSTORE_SUCC);

    tbsItem = controlFile->GetTbsPageItemPtr(tablespaceId, nullptr);
    ASSERT_EQ(tbsItem->tablespaceId, tablespaceId);
    ASSERT_EQ(tbsItem->hwm, 4);
    ASSERT_EQ(tbsItem->tbsMaxSize, tbsMaxSize);
    ASSERT_EQ(tbsItem->reuseVersion, 1);
    ASSERT_NE(tbsItem->ddlXid.m_placeHolder, 0);
    ASSERT_EQ(tbsItem->used, 1);
    ASSERT_EQ(tbsItem->fileIds[0], 0);
    ASSERT_EQ(tbsItem->fileIds[1], 0);
    ASSERT_EQ(tbsItem->fileIds[2], 0);
    ASSERT_EQ(tbsItem->fileIds[3], 0);

    ret = controlFile->FreeTbsId(tablespaceId, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
}

TEST_F(ControlFileTest, InitTbsId_level0) {
    TablespaceId tablespaceId = 99;
    uint64 tbsMaxSize = 1024 * 1024 * 600;
    RetStatus ret = controlFile->InitTbsId(tablespaceId, tbsMaxSize);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ControlTablespacePageItemData *tbsItem = controlFile->GetTbsPageItemPtr(tablespaceId, nullptr);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(tbsItem->tablespaceId, tablespaceId);
    ASSERT_EQ(tbsItem->hwm, 0);
    ASSERT_EQ(tbsItem->tbsMaxSize, tbsMaxSize);
    ASSERT_EQ(tbsItem->reuseVersion, 1);
    ASSERT_NE(tbsItem->ddlXid.m_placeHolder, 0);
    ASSERT_EQ(tbsItem->used, 1);

    ret = controlFile->FreeTbsId(tablespaceId, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
}

TEST_F(ControlFileTest, GetFilesFromTablespace_level0) {
    TablespaceId tablespaceId1 = 0;
    TablespaceId tablespaceId2 = 0;
    uint16 slotId;
    uint64 tbsMaxSize = 1024 * 1024 * 600;

    RetStatus ret = controlFile->AllocTbsId(&tablespaceId1, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AllocTbsId(&tablespaceId2, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ret = controlFile->AddFileIdToTbs(tablespaceId1, 1000, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AddFileIdToTbs(tablespaceId1, 1001, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ret = controlFile->AddFileIdToTbs(tablespaceId2, 2000, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AddFileIdToTbs(tablespaceId2, 2001, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    dlist_head fileIdListHead;
    DListInit(&fileIdListHead);
    uint32 fileCount = 0;
    ret = controlFile->GetFilesFromTablespace(tablespaceId1, &fileIdListHead, &fileCount);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(fileCount, 2);
    ASSERT_EQ(DListIsEmpty(&fileIdListHead), false);
    
    uint i=0;
    dlist_iter iter;
    dlist_foreach(iter, &fileIdListHead) {
        FileIdNode *fileIdNode = dlist_container(FileIdNode, node, iter.cur);
        ASSERT_EQ(fileIdNode->fileId, 1000+i);
        i++;
    }
    ASSERT_EQ(i, 2);
    controlFile->FreeFileIdList(&fileIdListHead);
    ASSERT_EQ(DListIsEmpty(&fileIdListHead), true);
    
    DListInit(&fileIdListHead);
    ret = controlFile->GetFilesFromAllTablespace(&fileIdListHead, &fileCount);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(fileCount, 4 + 2);
    controlFile->FreeFileIdList(&fileIdListHead);
    ASSERT_EQ(DListIsEmpty(&fileIdListHead), true);

    ret = controlFile->FreeTbsId(tablespaceId1, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->FreeTbsId(tablespaceId2, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
}

TEST_F(ControlFileTest, GetFilesFromTablespaceAfterFree_level0) {
    TablespaceId tablespaceId = 0;
    uint16 slotId;
    uint64 tbsMaxSize = 1024 * 1024 * 600;

    RetStatus ret = controlFile->AllocTbsId(&tablespaceId, tbsMaxSize, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ret = controlFile->AddFileIdToTbs(tablespaceId, 1000, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AddFileIdToTbs(tablespaceId, 1002, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ret = controlFile->AddFileIdToTbs(tablespaceId, 1001, &slotId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ret = controlFile->FreeFileIdFromTbs(tablespaceId, 1002, &slotId);
    ASSERT_EQ(ret, DSTORE_SUCC);

    dlist_head fileIdListHead;
    DListInit(&fileIdListHead);
    uint32 fileCount = 0;
    ret = controlFile->GetFilesFromTablespace(tablespaceId, &fileIdListHead, &fileCount);
    ASSERT_EQ(ret, DSTORE_SUCC);
    ASSERT_EQ(fileCount, 2);
    ASSERT_EQ(DListIsEmpty(&fileIdListHead), false);
    
    uint i=0;
    dlist_iter iter;
    dlist_foreach(iter, &fileIdListHead) {
        FileIdNode *fileIdNode = dlist_container(FileIdNode, node, iter.cur);
        ASSERT_EQ(fileIdNode->fileId, 1000+i);
        i++;
    }
    ASSERT_EQ(i, 2);
    controlFile->FreeFileIdList(&fileIdListHead);
    ASSERT_EQ(DListIsEmpty(&fileIdListHead), true);

    ret = controlFile->FreeTbsId(tablespaceId, DSTORE::INVALID_XID, false);
    ASSERT_EQ(ret, DSTORE_SUCC);
}

TEST_F(ControlFileTest, PdbIdTest_level0) {
    PdbId pdbId;
    RetStatus ret = controlFile->AllocPdbId("TestPdb", pdbId, false);
    ASSERT_EQ(ret, DSTORE_SUCC);

    ControlPdbInfoPageItemData pdbInfo;
    pdbInfo.pdbId = pdbId;
    pdbInfo.pdbStatus = PdbStatus::PDB_STATUS_OPENED_READ_WRITE;
    pdbInfo.dbaId = 12345;
    errno_t rc = strncpy_s(pdbInfo.pdbName, PDB_NAME_LEN, "TestPdb", strlen("TestPdb"));
    storage_securec_check(rc, "\0", "\0");
    rc = strncpy_s(pdbInfo.vfsName, MAX_CONFIG_NAME_LENGTH, "vfs", strlen("vfs"));
    storage_securec_check(rc, "\0", "\0");
    ret = controlFile->UpdatePdbItemData(pdbId, &pdbInfo, sizeof(ControlPdbInfoPageItemData));
    ASSERT_EQ(ret, DSTORE_SUCC);

    const ControlPdbInfoPageItemData *p = controlFile->GetPdbInfoById(pdbId);
    ASSERT_EQ(p->pdbId, pdbInfo.pdbId);
    ASSERT_EQ(p->pdbStatus, pdbInfo.pdbStatus);
    ASSERT_EQ(p->dbaId, pdbInfo.dbaId);
    ASSERT_EQ(strcasecmp(p->pdbName, pdbInfo.pdbName), 0);
    ASSERT_EQ(strcasecmp(p->vfsName, pdbInfo.vfsName), 0);

    /* test the case where arg1(pdbId) is not the same with the pdbId field of arg2(pdbInfo) when calling UpdatePdbId
     * function.
     */
    ControlPdbInfoPageItemData wrongPdbInfo;
    wrongPdbInfo.pdbId = pdbId + 1;
    ret = controlFile->UpdatePdbItemData(pdbId, &wrongPdbInfo, sizeof(ControlPdbInfoPageItemData));
    ASSERT_EQ(ret, DSTORE_FAIL);

    /* test if the pdbStatus field of arg2(pdbInfo) will be ignored when calling UpdatePdbId function. */
    wrongPdbInfo.pdbId = pdbId;
    wrongPdbInfo.pdbStatus = PdbStatus::PDB_STATUS_OPENED_READ_WRITE;
    ret = controlFile->UpdatePdbItemData(pdbId, &wrongPdbInfo, sizeof(ControlPdbInfoPageItemData));
    ASSERT_EQ(ret, DSTORE_SUCC);
    p = controlFile->GetPdbInfoById(pdbId);
    EXPECT_EQ(p->pdbStatus, PdbStatus::PDB_STATUS_OPENED_READ_WRITE);

    ret = controlFile->SetDeleteFlag(pdbId);
    ASSERT_EQ(ret, DSTORE_SUCC);
    p = controlFile->GetPdbInfoById(pdbId);
    ASSERT_EQ(p->pdbStatus, PdbStatus::PDB_STATUS_DROPPING);

    ret = controlFile->FreePdbId(pdbId);
    ASSERT_EQ(ret, DSTORE_SUCC);
    p = controlFile->GetPdbInfoById(pdbId);
    ASSERT_EQ(p->pdbStatus, PdbStatus::PDB_STATUS_UNCREATED);
    ret = controlFile->UpdatePdbItemData(pdbId, &pdbInfo, sizeof(ControlPdbInfoPageItemData));
    ASSERT_EQ(ret, DSTORE_FAIL);

    for (DSTORE::PdbId i = 0; i <= PDB_MAX_ID - FIRST_USER_PDB_ID; ++i) {
        char pdbName[PDB_NAME_LEN];
        rc = sprintf_s(pdbName, sizeof(pdbName), "pdb%d", i);
        storage_securec_check_ss(rc);
        ret = controlFile->AllocPdbId(pdbName, pdbId);
        ASSERT_EQ(ret, DSTORE_SUCC);
        ASSERT_EQ(pdbId, FIRST_USER_PDB_ID + i);
    }
    ret = controlFile->AllocPdbId("TestPdb33", pdbId);
    ASSERT_EQ(ret, DSTORE_FAIL);
}

TEST_F(ControlFileTest, PdbUuidTest_level0) {
    std::unordered_set<std::string> strSet;
    const int allocCount = 10000;
    char pdbUuid[FORMATTED_UUID_ARR_LEN] = {0};
    for (int i = 0; i < allocCount; i++) {
        errno_t rc = memset_s(pdbUuid, FORMATTED_UUID_ARR_LEN, 0, FORMATTED_UUID_ARR_LEN);
        storage_securec_check(rc, "\0", "\0");
        UuidGenerate(pdbUuid);
        std::string str(pdbUuid, pdbUuid + strlen(pdbUuid));
        ASSERT_EQ(strSet.find(str), strSet.end());
        strSet.insert(str);
    }
}

void InitCreateFilePara(FileParameter &filePara, uint64 maxFileSize)
{
    filePara.flag = APPEND_WRITE_FILE;
    filePara.rangeSize = WAL_FILE_RANGE_SIZE;
    filePara.maxSize = maxFileSize;
    filePara.mode = FILE_READ_AND_WRITE_MODE;
    filePara.isReplayWrite = false;
    TenantConfig *tenantConfig = g_storageInstance->GetGuc()->tenantConfig;
    StorageAssert(tenantConfig);
    errno_t rc = strcpy_s(filePara.storeSpaceName, STORESPACE_NAME_MAX_LEN,
                        tenantConfig->storeSpaces[0].storeSpaceName);
    storage_securec_check(rc, "\0", "\0");
}

void CheckCreateFilePara(FileParameter filePara, uint64 maxFileSize)
{
    TenantConfig *tenantConfig = g_storageInstance->GetGuc()->tenantConfig;
    StorageAssert(tenantConfig);
    ASSERT_EQ(filePara.flag, APPEND_WRITE_FILE);
    ASSERT_EQ(filePara.rangeSize, WAL_FILE_RANGE_SIZE);
    ASSERT_EQ(filePara.maxSize, maxFileSize);
    ASSERT_EQ(filePara.mode, FILE_READ_AND_WRITE_MODE);
    ASSERT_EQ(filePara.isReplayWrite, false);
}

/**
 * Add WalStream; that get them and check.
 */
TEST_F(ControlFileTest, AddWalStreamTest001_level0) {
    RetStatus retStatus;
    for (int i = 1; i < 32; i++) {
        ControlWalStreamPageItemData *itemData =
            (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
        itemData->streamState = 1;
        itemData->walId = i + 1; // walId1 has been created by templatePdb
        itemData->walBlockSize = 8192;
        itemData->walFileSize = 4 * 1024 * 1024;
        itemData->walMinRecoveryPlsn = 0x2022;
        itemData->archivePlsn = 0x07;
        itemData->lastCheckpointPLsn = 0x30;
        itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
        itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
        InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        itemData->initWalFileCount = 111;
        retStatus = controlFile->AddWalStream(*itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        DstorePfreeExt(itemData);
    }

    for (int i = 1; i < 32; i++) {
        ControlWalStreamPageItemData *itemData;
        retStatus = controlFile->GetWalStreamInfo(i + 1, &itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        ASSERT_EQ(itemData->streamState, 1);
        ASSERT_EQ(itemData->walId, i + 1);
        ASSERT_EQ(itemData->walBlockSize, 8192);
        ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
        ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
        ASSERT_EQ(itemData->archivePlsn, 0x07);
        ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
        ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
        ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
        CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        ASSERT_EQ(itemData->initWalFileCount, 111);
        controlFile->FreeWalStreamsInfo(itemData);
    }
}

TEST_F(ControlFileTest, DeleteNotExistWalStreamTest_level0) {
    RetStatus retStatus = controlFile->DeleteWalStream(0xff);
    ASSERT_EQ(retStatus, DSTORE_FAIL);
    ASSERT_EQ(StorageGetErrorCode(), CONTROL_ERROR_ITEM_NOT_FIND);
}

TEST_F(ControlFileTest, DeleteOneWalStreamTest_level0) {
    RetStatus retStatus;

    ControlWalStreamPageItemData *itemData =
        (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
    itemData->streamState = 1;
    itemData->walId = 0xff;
    itemData->walBlockSize = 8192;
    itemData->walFileSize = 4 * 1024 * 1024;
    itemData->walMinRecoveryPlsn = 0x2022;
    itemData->archivePlsn = 0x07;
    itemData->lastCheckpointPLsn = 0x30;
    itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
    itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
    InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    itemData->initWalFileCount = 111;
    retStatus = controlFile->AddWalStream(*itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    DstorePfreeExt(itemData);


    itemData = nullptr;
    retStatus = controlFile->GetWalStreamInfo(0xff, &itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    ASSERT_EQ(itemData->streamState, 1);
    ASSERT_EQ(itemData->walId, 0xff);
    ASSERT_EQ(itemData->walBlockSize, 8192);
    ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
    ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
    ASSERT_EQ(itemData->archivePlsn, 0x07);
    ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
    ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
    ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
    CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    ASSERT_EQ(itemData->initWalFileCount, 111);
    controlFile->FreeWalStreamsInfo(itemData);

    retStatus = controlFile->DeleteWalStream(0xff);
    ASSERT_EQ(retStatus, DSTORE_SUCC);

    itemData = nullptr;
    retStatus = controlFile->GetWalStreamInfo(0xff, &itemData);
    ASSERT_EQ(retStatus, DSTORE_FAIL);
    ASSERT_EQ(itemData, nullptr);
}

TEST_F(ControlFileTest, DeleteWalStreamInDifferentPageTest_TIER1_level0) {
    RetStatus retStatus;

    for (int i = 1; i <= 32; i++) {
        ControlWalStreamPageItemData *itemData =
            (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
        itemData->streamState = 1;
        itemData->walId = i + 1;
        itemData->walBlockSize = 8192;
        itemData->walFileSize = 4 * 1024 * 1024;
        itemData->walMinRecoveryPlsn = 0x2022;
        itemData->archivePlsn = 0x07;
        itemData->lastCheckpointPLsn = 0x30;
        itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
        itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
        InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        itemData->initWalFileCount = 111;
        retStatus = controlFile->AddWalStream(*itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        DstorePfreeExt(itemData);
    }

    for (int i = 1; i <= 32; i++) {
        ControlWalStreamPageItemData *itemData = nullptr;
        retStatus = controlFile->GetWalStreamInfo(i + 1, &itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        ASSERT_EQ(itemData->streamState, 1);
        ASSERT_EQ(itemData->walId, i + 1);
        ASSERT_EQ(itemData->walBlockSize, 8192);
        ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
        ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
        ASSERT_EQ(itemData->archivePlsn, 0x07);
        ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
        ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
        ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
        CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        ASSERT_EQ(itemData->initWalFileCount, 111);
        controlFile->FreeWalStreamsInfo(itemData);
    }

    for (int i = 1; i <= 32; i+=2) {
        retStatus = controlFile->DeleteWalStream(i + 1);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
    }

    for (int i = 1; i <= 32; i++) {
        ControlWalStreamPageItemData *itemData = nullptr;
        retStatus = controlFile->GetWalStreamInfo(i + 1, &itemData);
        if (i % 2 == 1) {
            ASSERT_EQ(retStatus, DSTORE_FAIL);
            ASSERT_EQ(itemData, nullptr);
        } else {
            ASSERT_EQ(retStatus, DSTORE_SUCC);
            ASSERT_EQ(itemData->streamState, 1);
            ASSERT_EQ(itemData->walId, i + 1);
            ASSERT_EQ(itemData->walBlockSize, 8192);
            ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
            ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
            ASSERT_EQ(itemData->archivePlsn, 0x07);
            ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
            ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
            ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
            ASSERT_EQ(itemData->initWalFileCount, 111);
        }
        controlFile->FreeWalStreamsInfo(itemData);
    }
}

TEST_F(ControlFileTest, UpdateNotExistWalStreamTest_TIER1_level0) {
    RetStatus retStatus;

    ControlWalStreamPageItemData *itemData =
        (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
    itemData->streamState = 1;
    itemData->walId = 0xff;
    itemData->walBlockSize = 8192;
    itemData->walFileSize = 4 * 1024 * 1024;
    itemData->walMinRecoveryPlsn = 0x2022;
    itemData->archivePlsn = 0x07;
    itemData->lastCheckpointPLsn = 0x30;
    itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
    itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
    InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    itemData->initWalFileCount = 123;
    retStatus = controlFile->UpdateWalStream(*itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    DstorePfreeExt(itemData);


    itemData = nullptr;
    retStatus = controlFile->GetWalStreamInfo(0xff, &itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    ASSERT_EQ(itemData->streamState, 1);
    ASSERT_EQ(itemData->walId, 0xff);
    ASSERT_EQ(itemData->walBlockSize, 8192);
    ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
    ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
    ASSERT_EQ(itemData->archivePlsn, 0x07);
    ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
    ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
    ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
    CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    ASSERT_EQ(itemData->initWalFileCount, 123);
    controlFile->FreeWalStreamsInfo(itemData);
}

TEST_F(ControlFileTest, UpdateOneWalStreamTest_level0) {
    RetStatus retStatus;

    ControlWalStreamPageItemData *itemData =
        (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
    itemData->streamState = 1;
    itemData->walId = 0xff;
    itemData->walBlockSize = 8192;
    itemData->walFileSize = 4 * 1024 * 1024;
    itemData->walMinRecoveryPlsn = 0x2022;
    itemData->archivePlsn = 0x07;
    itemData->lastCheckpointPLsn = 0x30;
    itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
    itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
    InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    itemData->initWalFileCount = 123;
    retStatus = controlFile->AddWalStream(*itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    DstorePfreeExt(itemData);


    itemData = nullptr;
    retStatus = controlFile->GetWalStreamInfo(0xff, &itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    ASSERT_EQ(itemData->streamState, 1);
    ASSERT_EQ(itemData->walId, 0xff);
    ASSERT_EQ(itemData->walBlockSize, 8192);
    ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
    ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
    ASSERT_EQ(itemData->archivePlsn, 0x07);
    ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
    ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
    ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
    CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    ASSERT_EQ(itemData->initWalFileCount, 123);
    controlFile->FreeWalStreamsInfo(itemData);

    itemData = (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
    itemData->streamState = 2;
    itemData->walId = 0xff;
    itemData->walBlockSize = 8192;
    itemData->walFileSize = 4 * 1024 * 1024;
    itemData->walMinRecoveryPlsn = 0x2022;
    itemData->archivePlsn = 0x08;
    itemData->lastCheckpointPLsn = 0x31;
    itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0ee0;
    itemData->lastWalCheckpoint.time = 1645427962;
    InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    itemData->initWalFileCount = 234;
    retStatus = controlFile->UpdateWalStream(*itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    DstorePfreeExt(itemData);


    itemData = nullptr;
    retStatus = controlFile->GetWalStreamInfo(0xff, &itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    ASSERT_EQ(itemData->streamState, 2);
    ASSERT_EQ(itemData->walId, 0xff);
    ASSERT_EQ(itemData->walBlockSize, 8192);
    ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
    ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
    ASSERT_EQ(itemData->archivePlsn, 0x08);
    ASSERT_EQ(itemData->lastCheckpointPLsn, 0x31);
    ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0ee0);
    ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
    CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    ASSERT_EQ(itemData->initWalFileCount, 234);
    controlFile->FreeWalStreamsInfo(itemData);
}

TEST_F(ControlFileTest, UpdateOneWalStreamTestConcurrently_level0)
{
    RetStatus retStatus;

    ControlWalStreamPageItemData *itemData =
        (ControlWalStreamPageItemData *)DstorePalloc0(sizeof(ControlWalStreamPageItemData));
    itemData->streamState = 1;
    itemData->walId = 0xff;
    itemData->walBlockSize = 8192;
    itemData->walFileSize = 4 * 1024 * 1024;
    itemData->walMinRecoveryPlsn = 0x2022;
    itemData->archivePlsn = 0x07;
    itemData->lastCheckpointPLsn = 0x30;
    itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
    itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
    InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    itemData->initWalFileCount = 123;
    retStatus = controlFile->AddWalStream(*itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    DstorePfreeExt(itemData);

    /* prepare update itemData1 */
    ControlWalStreamPageItemData *itemData1 = nullptr;
    retStatus = controlFile->GetWalStreamInfo(0xff, &itemData1);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    itemData1->lastCheckpointPLsn = 0xaa;
    itemData1->lastWalCheckpoint.diskRecoveryPlsn = 0x0ef0;
    itemData1->lastWalCheckpoint.time = 123456789;

    /* prepare and update itemData2 */
    ControlWalStreamPageItemData *itemData2 =
        (ControlWalStreamPageItemData *)DstorePalloc0(sizeof(ControlWalStreamPageItemData));
    itemData2->streamState = 2;
    itemData2->walId = 0xff;
    itemData2->walBlockSize = 8192;
    itemData2->walFileSize = 4 * 1024 * 1024;
    itemData2->walMinRecoveryPlsn = 0x2022;
    itemData2->archivePlsn = 0x08;
    itemData2->lastCheckpointPLsn = 0x31;
    itemData2->lastWalCheckpoint.diskRecoveryPlsn = 0x0ee0;
    itemData2->lastWalCheckpoint.time = 1645427962;
    InitCreateFilePara(itemData2->createFilePara, itemData2->walFileSize);
    itemData2->initWalFileCount = 234;
    retStatus = controlFile->UpdateWalStream(*itemData2);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    DstorePfreeExt(itemData2);

    /* update itemData1 */
    itemData1->walId = 0xff;
    retStatus = controlFile->UpdateWalStreamForCheckPoint(*itemData1);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    CheckCreateFilePara(itemData1->createFilePara, itemData1->walFileSize);
    controlFile->FreeWalStreamsInfo(itemData1);

    itemData = nullptr;
    retStatus = controlFile->GetWalStreamInfo(0xff, &itemData);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    ASSERT_EQ(itemData->streamState, 2);
    ASSERT_EQ(itemData->walId, 0xff);
    ASSERT_EQ(itemData->walBlockSize, 8192);
    ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
    ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
    ASSERT_EQ(itemData->archivePlsn, 0x08);
    ASSERT_EQ(itemData->lastCheckpointPLsn, 0xaa);
    ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0ef0);
    ASSERT_EQ(itemData->lastWalCheckpoint.time, 123456789);
    CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
    ASSERT_EQ(itemData->initWalFileCount, 234);
    controlFile->FreeWalStreamsInfo(itemData);
}

TEST_F(ControlFileTest, UpdateWalStreamInDifferentPageTest_TIER1_level0) {
    int itemCount = 64;
    RetStatus retStatus;

    for (int i = 1; i <= itemCount; i++) {
        ControlWalStreamPageItemData *itemData =
            (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
        itemData->streamState = 1;
        itemData->walId = i + 1;
        itemData->walBlockSize = 8192;
        itemData->walFileSize = 4 * 1024 * 1024;
        itemData->walMinRecoveryPlsn = 0x2022;
        itemData->archivePlsn = 0x07;
        itemData->lastCheckpointPLsn = 0x30;
        itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
        itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
        InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        itemData->initWalFileCount = 123;
        retStatus = controlFile->AddWalStream(*itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        DstorePfreeExt(itemData);
    }

    for (int i = 1; i <= itemCount; i++) {
        ControlWalStreamPageItemData *itemData = nullptr;
        retStatus = controlFile->GetWalStreamInfo(i + 1, &itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        ASSERT_EQ(itemData->streamState, 1);
        ASSERT_EQ(itemData->walId, i + 1);
        ASSERT_EQ(itemData->walBlockSize, 8192);
        ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
        ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
        ASSERT_EQ(itemData->archivePlsn, 0x07);
        ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
        ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
        ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
        CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        ASSERT_EQ(itemData->initWalFileCount, 123);
        controlFile->FreeWalStreamsInfo(itemData);
    }

    for (int i = 1; i <= itemCount; i+=2) {
        ControlWalStreamPageItemData *itemData =
            (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
        itemData->streamState = 2;
        itemData->walId = i + 1;
        itemData->walBlockSize = 8192;
        itemData->walFileSize = 4 * 1024 * 1024;
        itemData->walMinRecoveryPlsn = 0x2022;
        itemData->archivePlsn = 0x08;
        itemData->lastCheckpointPLsn = 0x31;
        itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0ee0;
        itemData->lastWalCheckpoint.time = 1645427962;
        InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        itemData->initWalFileCount = 234;
        retStatus = controlFile->UpdateWalStream(*itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
    }

    for (int i = 1; i <= itemCount; i++) {
        ControlWalStreamPageItemData *itemData = nullptr;
        retStatus = controlFile->GetWalStreamInfo(i + 1, &itemData);
        if (i % 2 == 1) {
            ASSERT_EQ(retStatus, DSTORE_SUCC);
            ASSERT_EQ(itemData->streamState, 2);
            ASSERT_EQ(itemData->walId, i + 1);
            ASSERT_EQ(itemData->walBlockSize, 8192);
            ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
            ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
            ASSERT_EQ(itemData->archivePlsn, 0x08);
            ASSERT_EQ(itemData->lastCheckpointPLsn, 0x31);
            ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0ee0);
            ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
            CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
            ASSERT_EQ(itemData->initWalFileCount, 234);
        } else {
            ASSERT_EQ(retStatus, DSTORE_SUCC);
            ASSERT_EQ(itemData->streamState, 1);
            ASSERT_EQ(itemData->walId, i + 1);
            ASSERT_EQ(itemData->walBlockSize, 8192);
            ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
            ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
            ASSERT_EQ(itemData->archivePlsn, 0x07);
            ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
            ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
            ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
            CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
            ASSERT_EQ(itemData->initWalFileCount, 123);
        }
        controlFile->FreeWalStreamsInfo(itemData);
    }
}

TEST_F(ControlFileTest, WalStreamItemSpaceReuseTest_level0) {
    int itemCount = 64;
    int repeat = 10;
    RetStatus retStatus;

    for (int i = 1; i <= itemCount; i++) {
        ControlWalStreamPageItemData *itemData =
            (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
        itemData->streamState = 1;
        itemData->walId = i + 1;
        itemData->walBlockSize = 8192;
        itemData->walFileSize = 4 * 1024 * 1024;
        itemData->walMinRecoveryPlsn = 0x2022;
        itemData->archivePlsn = 0x07;
        itemData->lastCheckpointPLsn = 0x30;
        itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0db0;
        itemData->lastWalCheckpoint.time = 1645427962; /* a special timestamp */
        InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        itemData->initWalFileCount = 123;
        retStatus = controlFile->AddWalStream(*itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        DstorePfreeExt(itemData);
    }

    for (int i = 1; i <= itemCount; i++) {
        ControlWalStreamPageItemData *itemData = nullptr;
        retStatus = controlFile->GetWalStreamInfo(i + 1, &itemData);
        ASSERT_EQ(retStatus, DSTORE_SUCC);
        ASSERT_EQ(itemData->streamState, 1);
        ASSERT_EQ(itemData->walId, i + 1);
        ASSERT_EQ(itemData->walBlockSize, 8192);
        ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
        ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
        ASSERT_EQ(itemData->archivePlsn, 0x07);
        ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
        ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
        ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
        CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
        ASSERT_EQ(itemData->initWalFileCount, 123);
        controlFile->FreeWalStreamsInfo(itemData);
    }

    uint32 pageCount = controlFile->m_controlFileMgr->UtGetPageCount();

    for (int iter = 0; iter < repeat; iter++) {
        if (iter % 2 == 0) {
            for (int i = 1; i <= itemCount; i+=2) {
                retStatus = controlFile->DeleteWalStream(i + 1);
                ASSERT_EQ(retStatus, DSTORE_SUCC);
            }
        } else {
            for (int i = 1; i <= itemCount; i+=2) {
                ControlWalStreamPageItemData *itemData =
                    (ControlWalStreamPageItemData *) DstorePalloc0(sizeof(ControlWalStreamPageItemData));
                itemData->streamState = 2;
                itemData->walId = i + 1;
                itemData->walBlockSize = 8192;
                itemData->walFileSize = 4 * 1024 * 1024;
                itemData->walMinRecoveryPlsn = 0x2022;
                itemData->archivePlsn = 0x08;
                itemData->lastCheckpointPLsn = 0x31;
                itemData->lastWalCheckpoint.diskRecoveryPlsn = 0x0ee0;
                itemData->lastWalCheckpoint.time = 1645427962;
                InitCreateFilePara(itemData->createFilePara, itemData->walFileSize);
                itemData->initWalFileCount = 123;
                retStatus = controlFile->UpdateWalStream(*itemData);
                ASSERT_EQ(retStatus, DSTORE_SUCC);
            }
        }
    }


    for (int i = 1; i <= itemCount; i++) {
        ControlWalStreamPageItemData *itemData = nullptr;
        retStatus = controlFile->GetWalStreamInfo(i + 1, &itemData);
        if (i % 2 == 1) {
            ASSERT_EQ(retStatus, DSTORE_SUCC);
            ASSERT_EQ(itemData->streamState, 2);
            ASSERT_EQ(itemData->walId, i + 1);
            ASSERT_EQ(itemData->walBlockSize, 8192);
            ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
            ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
            ASSERT_EQ(itemData->archivePlsn, 0x08);
            ASSERT_EQ(itemData->lastCheckpointPLsn, 0x31);
            ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0ee0);
            ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
            CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
            ASSERT_EQ(itemData->initWalFileCount, 123);
        } else {
            ASSERT_EQ(retStatus, DSTORE_SUCC);
            ASSERT_EQ(itemData->streamState, 1);
            ASSERT_EQ(itemData->walId, i + 1);
            ASSERT_EQ(itemData->walBlockSize, 8192);
            ASSERT_EQ(itemData->walFileSize, 4 * 1024 * 1024);
            ASSERT_EQ(itemData->walMinRecoveryPlsn, 0x2022);
            ASSERT_EQ(itemData->archivePlsn, 0x07);
            ASSERT_EQ(itemData->lastCheckpointPLsn, 0x30);
            ASSERT_EQ(itemData->lastWalCheckpoint.diskRecoveryPlsn, 0x0db0);
            ASSERT_EQ(itemData->lastWalCheckpoint.time, 1645427962);
            CheckCreateFilePara(itemData->createFilePara, itemData->walFileSize);
            ASSERT_EQ(itemData->initWalFileCount, 123);
        }
        controlFile->FreeWalStreamsInfo(itemData);
    }

    ASSERT_EQ(pageCount, controlFile->m_controlFileMgr->UtGetPageCount()); /* no more page extend */
}

TEST_F(ControlFileTest, CSNRecordTest_level0) {
    CommitSeqNo outCsn = MAX_COMMITSEQNO;
    PageId outUndoMapSegmentId = {1, 1};
    /* Test 1: Get the inited values. */
    controlFile->GetMaxReservedCSN(outCsn);
    controlFile->GetUndoZoneMapSegmentId(outUndoMapSegmentId);
    ASSERT_EQ(outCsn, INVALID_CSN);
    ASSERT_NE(outUndoMapSegmentId, INVALID_PAGE_ID);

    /* Test 2: Set and get csn and segment id. */
    CommitSeqNo csn = 1000;
    PageId undoMapSegmentId = {10, 1000};
    controlFile->SetMaxReservedCSN(csn, csn);
    controlFile->SetUndoZoneMapSegmentId(undoMapSegmentId);
    controlFile->GetMaxReservedCSN(outCsn);
    controlFile->GetUndoZoneMapSegmentId(outUndoMapSegmentId);
    ASSERT_EQ(outCsn, csn);
    ASSERT_EQ(outUndoMapSegmentId, undoMapSegmentId);

    /* Test 3: Reboot control file to check data consistency. */
    ControlFile *anotherFile = DstoreNew(m_ut_memory_context)ControlFile(g_defaultPdbId,
        g_storageInstance->GetPdb(g_defaultPdbId)->GetVFS());
    anotherFile->Init(DSTORETEST::pdbTemplatePath);
    outCsn = INVALID_CSN;
    outUndoMapSegmentId = INVALID_PAGE_ID;
    anotherFile->GetMaxReservedCSN(outCsn);
    anotherFile->GetUndoZoneMapSegmentId(outUndoMapSegmentId);
    ASSERT_EQ(outCsn, csn);
    ASSERT_EQ(outUndoMapSegmentId, undoMapSegmentId);
}

TEST_F(ControlFileTest, SysTableItemTest_level0) {
    Oid sysTableOid = 1;
    PageId segmentId = {2, 3};

    /* Test 1: Get the value before add item. */
    PageId retPageId;
    RetStatus ret = controlFile->GetSysTableItem(sysTableOid, retPageId);
    EXPECT_EQ(ret, DSTORE_FAIL);
    EXPECT_EQ(retPageId, INVALID_PAGE_ID);

    /* Test 2: Add systable item to control file. */
    ret = controlFile->AddSysTableItem(sysTableOid, segmentId);
    EXPECT_EQ(ret, DSTORE_SUCC);

    /* Test 3: Get the value after add item and compare it. */
    ret = controlFile->GetSysTableItem(sysTableOid, retPageId);
    EXPECT_EQ(ret, DSTORE_SUCC);
    EXPECT_EQ(retPageId, segmentId);
}

TEST_F(ControlFileTest, CreateAndAllocateOneWalStreamTest001_TIER1_level0) {
    RetStatus retStatus;
    ControlWalStreamPageItemData *walStreamItemData;
    retStatus = controlFile->CreateAndAllocateOneWalStream(&walStreamItemData, DSTORE::INVALID_WAL_ID);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    DstorePfreeExt(walStreamItemData);
}

TEST_F(ControlFileTest, ControlFileBlockCount_TIER1_level0) {
    RetStatus retStatus;
    BlockNumber blkCount = controlFile->GetBlockCount();
    ASSERT_EQ(blkCount, CONTROLFILE_PAGEMAP_MAX);
}

TEST_F(ControlFileTest, ControlFileCopyTest_level0) {
    RetStatus retStatus;
    /* Test 1: Copy ControlFile. */
    ControlFile *srcControlFile = g_storageInstance->GetPdb(g_defaultPdbId)->GetControlFile();
    ControlFile *newControlFile = DstoreNew(m_ut_memory_context)ControlFile(g_defaultPdbId,
        g_storageInstance->GetPdb(g_defaultPdbId)->GetVFS());
    newControlFile->Init(DSTORETEST::pdbTemplatePath);
    retStatus = newControlFile->CopyControlFile(srcControlFile);
    ASSERT_EQ(retStatus, DSTORE_SUCC);
    delete newControlFile;
    newControlFile = nullptr;
}
