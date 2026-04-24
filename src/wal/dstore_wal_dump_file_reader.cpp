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
 * dstore_wal_dump_file_reader.cpp
 *
 * Description:
 * src/wal/dstore_wal_dump_file_reader.cpp
 *
 * ---------------------------------------------------------------------------------------
 *
 */
#include <securec.h>
#include <dirent.h>
#include "page/dstore_page_diagnose.h"
#include "framework/dstore_vfs_interface.h"
#include "framework/dstore_config_interface.h"
#include "framework/dstore_instance_interface.h"
#include "framework/dstore_pdb.h"
#include "tablespace/dstore_tablespace.h"
#include "control/dstore_control_struct.h"
#include "wal/dstore_wal_dump_file_reader.h"

namespace DSTORE {

using namespace PageDiagnose;

constexpr int ERROR_BUF_LEN = 256;
constexpr uint16 MAX_DUMP_WAL_STREAM_COUNT = 0xFF;
constexpr uint32 DEFAULT_FILE_ARRAY_LEN = 10;
constexpr uint64 COMMU_LIMIT_BYTES = 512 * 1024U;
constexpr uint16 CONTROL_PAGE_NUM = 2;
constexpr uint16 CONTROL_FILE1 = 0;
constexpr uint16 CONTROL_FILE2 = 1;

WalDumpFileReader::WalDumpFileReader(char *path, StorageType vfsType, uint64 checkpointPlsn)
    : m_vfsType(vfsType),
    m_fileReader(vfsType, path),
    m_walDir{nullptr},
    m_walFileInfo{INVALID_WAL_ID, 0},
    m_fileNum(0),
    m_fileArrayLen(0),
    m_fileStartPlsnArray(nullptr),
    m_walFileHeaders(nullptr),
    m_fileDescArrays(nullptr),
    m_startPlsn(INVALID_END_PLSN),
    m_endPlsn(0),
    m_checkpointPlsn(checkpointPlsn),
    m_walStreamEmpty(true)
{
    m_walFileInfo = {INVALID_WAL_ID, 0};
    m_pageStoreConfig = {};

    if (m_vfsType == StorageType::LOCAL || m_vfsType == StorageType::TENANT_ISOLATION) {
        m_walDir = path;
    }
    m_tenantConfig = {};
}

WalDumpFileReader::~WalDumpFileReader()
{
    DstorePfreeExt(m_fileStartPlsnArray);
    DstorePfreeExt(m_walFileHeaders);
    DstorePfreeExt(m_fileDescArrays);

    m_walFileHeaders = nullptr;
    m_walDir = nullptr;
}

static const ControlPageTypeInfo *GetPageTypeInfo(const uint16 controlPageType)
{
    uint8_t index;
    for (index = 0; index < sizeof(CONTROL_PAGE_TYPE_INFOS) / sizeof(CONTROL_PAGE_TYPE_INFOS[0]); ++index) {
        if (CONTROL_PAGE_TYPE_INFOS[index].type == static_cast<ControlPageType>(controlPageType)) {
            break;
        }
    }
    if (index == 0 || (index >= (sizeof(CONTROL_PAGE_TYPE_INFOS) / sizeof(CONTROL_PAGE_TYPE_INFOS[0])))) {
        (void)fprintf(DumpToolHelper::dumpPrint, "GetFileIdFromControlFile failed for read unknown PageType %hu\n",
            controlPageType);
        return nullptr;
    }
    return &(CONTROL_PAGE_TYPE_INFOS[index]);
}

static bool GetPageRangeFromControlFile(ControlPageType pageType, uint32_t *firstPage, uint32_t *lastPage)
{
    switch (pageType) {
        case CONTROL_WAL_STREAM_DATAPAGE_TYPE:
            *firstPage = CONTROLFILE_PAGEMAP_WALSTREAM_START;
            *lastPage = CONTROLFILE_PAGEMAP_WALSTREAM_MAX;
            break;
        case CONTROL_PDBINFO_DATAPAGE_TYPE:
            *firstPage = CONTROLFILE_PAGEMAP_PDBINFO_START;
            *lastPage = CONTROLFILE_PAGEMAP_PDBINFO_MAX;
            break;
        default:
            (void)fprintf(DumpToolHelper::dumpPrint, "Get page type [%u] from meta page fail, invalid type\n",
                pageType);
            return false;
    }

    (void)fprintf(DumpToolHelper::dumpPrint, "Get page type [%u] range [%u, %u]from meta page success\n",
        pageType, *firstPage, *lastPage);
    return true;
}

struct WalStreamExtendPageContext {
    uint32_t firstPage;
    uint32_t lastPage;
};

static RetStatus GetWalCheckpoint(ControlPage *controlPage, uint32 curPageId,
    void *args, bool *finishGetInfo)
{
    WalCheckPointInfoArgs *checkpointInfo = static_cast<WalCheckPointInfoArgs *>(args);
    uint8_t *controlPageData = (uint8_t *)(controlPage->m_data);
    uint8_t *start = controlPageData;
    uint8_t *end = controlPageData + controlPage->m_pageHeader.m_writeOffset;
    const ControlPageTypeInfo* pageTypeInfo = GetPageTypeInfo(controlPage->GetControlPageType());
    if (pageTypeInfo == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "GetPageTypeInfo failed.\n");
        return DSTORE_FAIL;
    }
    while (start < end) {
        ControlWalStreamPageItemData *walStreamPage = STATIC_CAST_PTR_TYPE(start, ControlWalStreamPageItemData *);
        if (walStreamPage->walId != checkpointInfo->walId) {
            start += pageTypeInfo->size;
            continue;
        }
        *(checkpointInfo->checkpoint) = walStreamPage->lastWalCheckpoint;
        *finishGetInfo = true;
        (void)fprintf(DumpToolHelper::dumpPrint,
            "GetWalCheckpoint find checkpoint at page:%u, time:%ld diskPlsn:%lu memPlsn:%lu.\n",
            curPageId, walStreamPage->lastWalCheckpoint.time, walStreamPage->lastWalCheckpoint.diskRecoveryPlsn,
            walStreamPage->lastWalCheckpoint.memoryCheckpoint.memRecoveryPlsn);
        const WalCheckPointHistory &history = walStreamPage->checkpointHistory;
        (void)fprintf(DumpToolHelper::dumpPrint,
            "GetWalCheckpoint history: slotCount:%hhu newestSlotIdx:%hhu.\n",
            history.slotCount, history.newestSlotIdx);
        for (uint8 i = 0; i < history.slotCount; ++i) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "  slot[%hhu] time:%ld diskPlsn:%lu memPlsn:%lu.\n", i,
                history.slots[i].time, history.slots[i].diskRecoveryPlsn,
                history.slots[i].memoryCheckpoint.memRecoveryPlsn);
        }
        break;
    }
    return DSTORE_SUCC;
}

static RetStatus PreGetWalInfo(void *args)
{
    WalInfoArgs *walInfoArgs = static_cast<WalInfoArgs *>(args);
    *(walInfoArgs->walIdCount) = 0;
    *(walInfoArgs->walIdArray) =
        static_cast<WalId *>(DstorePalloc0(sizeof(WalId) * MAX_DUMP_WAL_STREAM_COUNT));
    if (*(walInfoArgs->walIdArray) == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "PreGetPageInfo failed for malloc WalIdArrry memory failed.\n");
        return DSTORE_FAIL;
    }
    *(walInfoArgs->walFileSizeArray) =
        static_cast<uint32 *>(DstorePalloc0(sizeof(uint32) * MAX_DUMP_WAL_STREAM_COUNT));
    if (*(walInfoArgs->walFileSizeArray) == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "PreGetPageInfo failed for malloc walFileSizeArray memory failed.\n");
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

static RetStatus FillAllWalIds(ControlPage *controlPage, uint32 curPageId,
    void *args, UNUSE_PARAM bool *finishGetInfo)
{
    WalInfoArgs *walInfoArgs = static_cast<WalInfoArgs *>(args);
    uint8_t *controlPageData = (uint8_t *)(controlPage->m_data);
    uint8_t *start = controlPageData;
    uint8_t *end = controlPageData + controlPage->m_pageHeader.m_writeOffset;
    const ControlPageTypeInfo* pageTypeInfo = GetPageTypeInfo(controlPage->GetControlPageType());
    if (pageTypeInfo == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "GetPageTypeInfo failed.\n");
        return DSTORE_FAIL;
    }
    while (start < end) {
        ControlWalStreamPageItemData *walStreamPage = STATIC_CAST_PTR_TYPE(start, ControlWalStreamPageItemData *);
        (void)fprintf(DumpToolHelper::dumpPrint, "FillAllWalIds found walId:%lu at page:%u\n",
            walStreamPage->walId, curPageId);
        if ((*walInfoArgs->walIdCount) == MAX_DUMP_WAL_STREAM_COUNT) {
            (void)fprintf(DumpToolHelper::dumpPrint, "FillAllWalIds failed for exceed max count %u\n",
                MAX_DUMP_WAL_STREAM_COUNT);
            return DSTORE_FAIL;
        }
        (*walInfoArgs->walIdArray)[(*walInfoArgs->walIdCount)] = walStreamPage->walId;
        (*walInfoArgs->walFileSizeArray)[(*walInfoArgs->walIdCount)] = walStreamPage->walFileSize;
        (*walInfoArgs->walIdCount)++;
        start += pageTypeInfo->size;
    }
    return DSTORE_SUCC;
}

static RetStatus PostGetWalInfo(RetStatus res, void *args)
{
    if (res == DSTORE_SUCC) {
        return DSTORE_SUCC;
    }
    WalInfoArgs *walInfoArgs = static_cast<WalInfoArgs *>(args);
    *(walInfoArgs->walIdCount) = 0;
    DstorePfreeExt(*(walInfoArgs->walIdArray));
    *(walInfoArgs->walIdArray) = nullptr;
    DstorePfreeExt(*(walInfoArgs->walFileSizeArray));
    *(walInfoArgs->walFileSizeArray) = nullptr;
    return DSTORE_SUCC;
}

static RetStatus GetVfsNameByPdbId(ControlPage *controlPage, uint32 curPageId, void *args,
    bool *finishGetInfo)
{
    VfsInfoArgs *vfsInfoArgs = static_cast<VfsInfoArgs *>(args);
    uint8_t *controlPageData = (uint8_t *)(controlPage->m_data);
    uint8_t *start = controlPageData;
    uint8_t *end = controlPageData + controlPage->m_pageHeader.m_writeOffset;
    const ControlPageTypeInfo* pageTypeInfo = GetPageTypeInfo(controlPage->GetControlPageType());
    if (pageTypeInfo == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "GetPageTypeInfo failed.\n");
        return DSTORE_FAIL;
    }
    while (start < end) {
        ControlPdbInfoPageItemData *pdbInfoItem = STATIC_CAST_PTR_TYPE(start, ControlPdbInfoPageItemData *);
        if (pdbInfoItem->pdbId != vfsInfoArgs->pdbId) {
            start += pageTypeInfo->size;
            continue;
        }
        int rc = memcpy_s(vfsInfoArgs->vfsName, vfsInfoArgs->vfsNameSize, pdbInfoItem->vfsName,
            sizeof(pdbInfoItem->vfsName));
        if (rc != 0) {
            (void)fprintf(DumpToolHelper::dumpPrint, "GetVfsNameByPdbId memcpy failed at page:%u\n", curPageId);
            return DSTORE_FAIL;
        }
        (void)fprintf(DumpToolHelper::dumpPrint, "GetVfsNameByPdbId found pdb id:%u, vfsName:%s, at page:%u\n",
            vfsInfoArgs->pdbId, pdbInfoItem->vfsName, curPageId);
        *finishGetInfo = true;
        break;
    }
    return DSTORE_SUCC;
}

static RetStatus PostGetVfsNameByPdbId(UNUSE_PARAM RetStatus res, void *args)
{
    VfsInfoArgs *vfsInfoArgs = static_cast<VfsInfoArgs *>(args);
    if (strlen(vfsInfoArgs->vfsName) == 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "GetVfsNameByPdbId do not find vfs name by pdb id %u\n",
            vfsInfoArgs->pdbId);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

static GetInfoProcessFunctionTableForWaldump getInfoFunctionTable[] = {
    { GET_WAL_ID_ARRAY,   CONTROL_WAL_STREAM_DATAPAGE_TYPE, PreGetWalInfo, FillAllWalIds,     PostGetWalInfo        },
    { GET_WAL_CHECKPOINT, CONTROL_WAL_STREAM_DATAPAGE_TYPE, nullptr,       GetWalCheckpoint,  nullptr               },
    { GET_VFS_NAME,       CONTROL_PDBINFO_DATAPAGE_TYPE,    nullptr,       GetVfsNameByPdbId, PostGetVfsNameByPdbId }
};

static ControlPageType GetPageType(GetPageInfoType getType)
{
    for (uint32 i = 0; i < sizeof(getInfoFunctionTable) / sizeof(getInfoFunctionTable[0]); ++i) {
        if (getInfoFunctionTable[i].getType == getType) {
            return getInfoFunctionTable[i].pageType;
        }
    }
    return CONTROL_PAGE_TYPE_INVALID;
}

RetStatus WalDumpFileReader::PreGetPageInfo(GetPageInfoType getType, void *args)
{
    for (uint32 i = 0; i < sizeof(getInfoFunctionTable) / sizeof(getInfoFunctionTable[0]); ++i) {
        if (getInfoFunctionTable[i].getType == getType) {
            if (getInfoFunctionTable[i].preGetInfoFunc == nullptr) {
                (void)fprintf(DumpToolHelper::dumpPrint,
                    "PreGetPageInfo get type:%u pre get page information function is null, just return.\n", getType);
                return DSTORE_SUCC;
            }
            return getInfoFunctionTable[i].preGetInfoFunc(args);
        }
    }
    return DSTORE_FAIL;
}

RetStatus WalDumpFileReader::GetPageInfo(ControlPage *controlPage, uint32 curPageId, GetPageInfoType getType,
    void *args, bool *finishGetInfo)
{
    for (uint32 i = 0; i < sizeof(getInfoFunctionTable) / sizeof(getInfoFunctionTable[0]); ++i) {
        if (getInfoFunctionTable[i].getType == getType) {
            if (getInfoFunctionTable[i].getInfoFunc == nullptr) {
                (void)fprintf(DumpToolHelper::dumpPrint,
                    "GetPageInfo get type:%u get page information function is null, just return.\n", getType);
                return DSTORE_SUCC;
            }
            return getInfoFunctionTable[i].getInfoFunc(controlPage, curPageId, args, finishGetInfo);
        }
    }
    return DSTORE_FAIL;
}

RetStatus WalDumpFileReader::PostGetPageInfo(GetPageInfoType getType, RetStatus res, void *args)
{
    for (uint32 i = 0; i < sizeof(getInfoFunctionTable) / sizeof(getInfoFunctionTable[0]); ++i) {
        if (getInfoFunctionTable[i].getType == getType) {
            if (getInfoFunctionTable[i].postGetInfoFunc == nullptr) {
                (void)fprintf(DumpToolHelper::dumpPrint,
                    "PostGetPageInfo get type:%u post get page information function is null, just return.\n", getType);
                return res;
            }
            return getInfoFunctionTable[i].postGetInfoFunc(res, args);
        }
    }
    return DSTORE_FAIL;
}

RetStatus ControlFileMetaPageCheck(PageDiagnose::DumpToolHelper *fileReader, FileDescriptor *fd1, FileDescriptor *fd2,
    ControlPageType pageType, FileDescriptor **validFd)
{
    uint8_t firstMetaPageBuf[BLCKSZ];
    uint8_t secondMetaPageBuf[BLCKSZ];
    uint8_t *firstMetaPagePtr = firstMetaPageBuf;
    uint8_t *secondMetaPagePtr = secondMetaPageBuf;
    uint32_t metaPageId;

    if (fileReader == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "ControlFileMetaPageCheck fileReader is nullptr.\n");
        return DSTORE_FAIL;
    }
    if (pageType == CONTROL_WAL_STREAM_DATAPAGE_TYPE) {
        metaPageId = CONTROLFILE_PAGEMAP_WALSTREAM_META;
    } else if (pageType == CONTROL_PDBINFO_DATAPAGE_TYPE) {
        metaPageId = CONTROLFILE_PAGEMAP_PDBINFO_META;
    } else {
        (void)fprintf(DumpToolHelper::dumpPrint, "ControlFileMetaPageCheck pageType is invalid.\n");
        return DSTORE_FAIL;
    }

    if (fd1 == nullptr) {
        firstMetaPagePtr = nullptr;
    } else {
        if (STORAGE_FUNC_FAIL(fileReader->ReadPage(fd1, metaPageId, firstMetaPageBuf, BLCKSZ))) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "ControlFileMetaPageCheck failed for read control file 1 mete page.\n");
            firstMetaPagePtr = nullptr;
        }
    }

    if (fd2 == nullptr) {
        secondMetaPagePtr = nullptr;
    } else {
        if (STORAGE_FUNC_FAIL(fileReader->ReadPage(fd2, metaPageId, secondMetaPageBuf, BLCKSZ))) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "ControlFileMetaPageCheck failed for read control file 2 mete page.\n");
            secondMetaPagePtr = nullptr;
        }
    }

    MetaPageCheckResult metaPageCheckResult = DSTORE::CheckMetaPage(firstMetaPagePtr, secondMetaPagePtr);
    if (metaPageCheckResult == FIRST_META_PAGE_IS_VALID || metaPageCheckResult == BOTH_META_PAGES_ARE_VALID) {
        *validFd = fd1;
        (void)fprintf(DumpToolHelper::dumpPrint, "ControlFileMetaPageCheck control file 1 is valid.\n");
        return DSTORE_SUCC;
    } else if (metaPageCheckResult == SECOND_META_PAGE_IS_VALID) {
        *validFd = fd2;
        (void)fprintf(DumpToolHelper::dumpPrint, "ControlFileMetaPageCheck control file 2 is valid.\n");
        return DSTORE_SUCC;
    } else {
        (void)fprintf(DumpToolHelper::dumpPrint,
                "ControlFileMetaPageCheck failed, both control files' metaPage are invalid.\n");
        return DSTORE_FAIL;
    }
}

RetStatus WalDumpFileReader::GetPageInfoFromControlFile(PageDiagnose::DumpToolHelperInitParam *param,
    char *vfsConfigPath, GetPageInfoType getType, void *args)
{
    /* Step1: Try initialize fileReader */
    PageDiagnose::DumpToolHelper *fileReader = new PageDiagnose::DumpToolHelper(StorageType::PAGESTORE, vfsConfigPath);
    if (unlikely(fileReader == nullptr)) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("GetPageInfoFromControlFile new DumpToolHelper fail."));
        return DSTORE_FAIL;
    }
    if (STORAGE_FUNC_FAIL(fileReader->Init(param))) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("GetPageInfoFromControlFile init control file reader failed."));
        delete fileReader;
        return DSTORE_FAIL;
    }

    ControlPageType pageType = GetPageType(getType);
    if (pageType == CONTROL_PAGE_TYPE_INVALID) {
        ErrLog(DSTORE_ERROR, MODULE_WAL,
               ErrMsg("GetPageInfoFromControlFile get page type by get type %u failed.\n", getType));
        delete fileReader;
        return DSTORE_FAIL;
    }
    uint8_t pageBuf[BLCKSZ];
    uint32_t curPageId = 0;
    uint32_t startPageId = 0;
    uint32_t endPageId = 0;
    bool needStop = false;
    FileDescriptor *fd[CONTROL_PAGE_NUM] = {nullptr};
    FileDescriptor *validFd = nullptr;

    RetStatus result = PreGetPageInfo(getType, args);
    if (result != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "GetPageInfoFromControlFile failed for pre process args failed.\n");
        goto EXIT;
    }

    if (STORAGE_FUNC_FAIL(fileReader->Open(DATABASE_CONTROL_FILE_1_NAME, FILE_READ_ONLY_FLAG, &fd[CONTROL_FILE1]))) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "GetPageInfoFromControlFile failed for init open control file 1 page failed.\n");
        fd[CONTROL_FILE1] = nullptr;
    }
    if (STORAGE_FUNC_FAIL(fileReader->Open(DATABASE_CONTROL_FILE_2_NAME, FILE_READ_ONLY_FLAG, &fd[CONTROL_FILE2]))) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "GetPageInfoFromControlFile failed for init open control file 2 page failed.\n");
        fd[CONTROL_FILE2] = nullptr;
    }
    if (fd[CONTROL_FILE1] == nullptr && fd[CONTROL_FILE2] == nullptr) {
        result = DSTORE_FAIL;
        goto EXIT;
    }
    if (STORAGE_FUNC_FAIL(ControlFileMetaPageCheck(fileReader, fd[CONTROL_FILE1], fd[CONTROL_FILE2],
        pageType, &validFd))) {
        result = DSTORE_FAIL;
        goto EXIT;
    }

    /* Step2: find and load target type page */
    if (!GetPageRangeFromControlFile(pageType, &startPageId, &endPageId)) {
        result = DSTORE_FAIL;
        goto EXIT;
    }
    curPageId = startPageId;
    do {
        if (STORAGE_FUNC_FAIL(fileReader->ReadPage(validFd, curPageId, pageBuf, BLCKSZ))) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "GetPageInfoFromControlFile failed for read control file page %u failed.\n", curPageId);
            result = DSTORE_FAIL;
            goto EXIT;
        }
        ControlPage *controlPage = STATIC_CAST_PTR_TYPE(pageBuf, ControlPage *);
        if (!IsTargetTypePage(controlPage, pageType, curPageId)) {
            continue;
        }
        bool finishGetInfo = false;
        if (STORAGE_FUNC_FAIL(GetPageInfo(controlPage, curPageId, getType, args, &finishGetInfo))) {
            result = DSTORE_FAIL;
            goto EXIT;
        }
        needStop = (curPageId == endPageId) || finishGetInfo;
        curPageId = controlPage->GetNextPage();
    } while (!needStop && curPageId != DSTORE_INVALID_BLOCK_NUMBER);
    result = DSTORE_SUCC;
EXIT:
    /* Step4: free fileReader and related resources */
    fileReader->Close(fd[CONTROL_FILE1]);
    fileReader->Close(fd[CONTROL_FILE2]);
    fileReader->Destroy();
    result = PostGetPageInfo(getType, result, args);
    delete fileReader;
    return result;
}

bool WalDumpFileReader::IsTargetTypePage(ControlPage *pageBuf, ControlPageType pageType, uint32_t curPageId)
{
    ControlPage *controlPage = STATIC_CAST_PTR_TYPE(pageBuf, ControlPage *);
    const ControlPageTypeInfo *pageTypeInfo = GetPageTypeInfo(controlPage->GetControlPageType());
    if (pageTypeInfo == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "IsTargetTypePage get pageTypeInfo nullptr.\n");
        return false;
    }
    if (pageTypeInfo->type != pageType) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "Judge page type failed for Page:%u, type %hhu wrong, expect %hhu\n", curPageId,
            static_cast<uint8_t>(pageTypeInfo->type), static_cast<uint8_t>(pageType));
        return false;
    }
    return true;
}

inline bool IsValidFileIds(FileId *fileId)
{
    return fileId[0] != INVALID_VFS_FILE_ID;
}

RetStatus WalDumpFileReader::Init(bool reuseVfs, VFSAdapter *vfs, char *pdbVfsName,
    PageDiagnose::DumpCommConfig *commConfig)
{
    PageDiagnose::DumpToolHelperInitParam param = {
        .reuseVfs = reuseVfs,
        .vfs = vfs,
        .pdbVfsName = pdbVfsName,
        .commConfig = commConfig
    };
    if (STORAGE_FUNC_FAIL(m_fileReader.Init(&param))) {
        return DSTORE_FAIL;
    }

    if (STORAGE_FUNC_FAIL(InitWalFileInfo())) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Init wal files info fail.\n");
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

void WalDumpFileReader::SetWalDumpWalFileInfo(WalId walId, uint32 walFileSize)
{
    m_walFileInfo = {walId, walFileSize};
}

RetStatus WalDumpFileReader::InitWalFileInfo()
{
    if (m_vfsType == DSTORE::StorageType::PAGESTORE) {
        if (InitPageStoreWalFileInfo() != DSTORE_SUCC) {
            return DSTORE_FAIL;
        }
    } else {
        if (InitLocalWalFileInfo() != DSTORE_SUCC) {
            return DSTORE_FAIL;
        }
    }
    if (m_fileNum == 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "WalDumpFileReader init fail for wal file number is 0.\n");
        return DSTORE_FAIL;
    }

    return DSTORE_SUCC;
}

void WalDumpFileReader::CloseAllFiles()
{
    for (uint32 i = 0; i < m_fileNum; i++) {
        if (m_fileDescArrays[i] != nullptr) {
            m_fileReader.Close(m_fileDescArrays[i]);
            m_fileDescArrays[i] = nullptr;
        }
    }
}

void WalDumpFileReader::Destroy()
{
    m_fileReader.Destroy();
    m_walDir = nullptr;
}

RetStatus WalDumpFileReader::Read(uint64 plsn, uint8 *data, uint64 readLen, uint64 *resultLen)
{
    StorageAssert(m_walFileInfo.fileSize != 0);
    uint64 fileStartPlsn = static_cast<uint64>((plsn / m_walFileInfo.fileSize) * m_walFileInfo.fileSize);
    uint64 offset = plsn % m_walFileInfo.fileSize;
    uint64 limitedReadBytes = m_vfsType == DSTORE::StorageType::PAGESTORE ? COMMU_LIMIT_BYTES : m_walFileInfo.fileSize;
    uint64 curReadBytes = 0;
    uint64 restSpace = 0;
    int64 resultSize = 0;
    *resultLen = 0;
    WalFileHeaderData walFileHeader = {};

    if (STORAGE_FUNC_FAIL(GetWalFileHeader(plsn, &walFileHeader))) {
        return DSTORE_SUCC;
    }
    if (!(plsn >= walFileHeader.startPlsn && plsn < walFileHeader.startPlsn + walFileHeader.fileSize)) {
        return DSTORE_SUCC;
    }

    while (readLen > 0) {
        restSpace = DstoreMin(m_walFileInfo.fileSize - offset, limitedReadBytes);
        curReadBytes = readLen < restSpace ? readLen : restSpace;
        while (curReadBytes > 0) {
            FileDescriptor *fileDesc = GetFileDescriptor(fileStartPlsn);
            if (fileDesc == nullptr) {
                (void)fprintf(DumpToolHelper::dumpPrint, "Get file descriptor at %lu failed\n", plsn);
                return DSTORE_FAIL;
            }
            RetStatus retStatus =
                m_fileReader.ReadOffset(fileDesc, curReadBytes, static_cast<off_t>(offset), data, &resultSize);
            if (STORAGE_FUNC_FAIL(retStatus)) {
                (void)fprintf(DumpToolHelper::dumpPrint, "Read file of plsn %lu at offset %lu failed\n", plsn, offset);
                return DSTORE_FAIL;
            }
            if (resultSize <= 0) {
                return DSTORE_SUCC;
            }
            uint64 tmpSize = static_cast<uint64>(resultSize);
            curReadBytes -= tmpSize;
            data += tmpSize;
            offset += tmpSize;
            plsn += tmpSize;
            readLen -= tmpSize;
            (*resultLen) += tmpSize;
        }
        if (offset == m_walFileInfo.fileSize) {
            offset = 0;
            if (STORAGE_FUNC_FAIL(GetWalFileHeader(plsn, &walFileHeader))) {
                return DSTORE_SUCC;
            }
            if (plsn != walFileHeader.startPlsn) {
                break;
            }
        }
    }
    return DSTORE_SUCC;
}

RetStatus WalDumpFileReader::ParseFileHeader(uint64 fileStartPlsn)
{
    if (GetFileIndex(fileStartPlsn) >= 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "ParseFileHeader:%lu duplicated and return \n", fileStartPlsn);
        return DSTORE_SUCC;
    }
    if (m_fileNum >= m_fileArrayLen) {
        if (STORAGE_FUNC_FAIL(TryExpandWalFilesArray())) {
            return DSTORE_FAIL;
        }
    }
    m_fileStartPlsnArray[m_fileNum] = fileStartPlsn;
    m_fileDescArrays[m_fileNum] = nullptr;
    m_walFileHeaders[m_fileNum] = {};

    char name[MAXPGPATH];
    RetStatus retStatus = MakeWalFileName(name, MAXPGPATH, fileStartPlsn);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        return DSTORE_FAIL;
    }

    FileDescriptor *fileDesc = nullptr;
    int32_t fileIndex = -1;
    retStatus = OpenFile(fileStartPlsn, name, FILE_READ_ONLY_FLAG, &fileDesc, &fileIndex);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        return DSTORE_FAIL;
    }
    if (fileDesc == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "ParseFileHeader:%lu failed for GetFileDescriptor failed after open\n",
            fileStartPlsn);
        return DSTORE_FAIL;
    }
    if (fileIndex < 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "ParseFileHeader:%lu failed for GetFileIndex failed after open\n",
            fileStartPlsn);
        return DSTORE_FAIL;
    }

    /* If this wal stream parse at least one wal file header, this wal stream is not empty */
    int64 fileSize = m_fileReader.Size(fileDesc);
    if (fileSize == 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal file %s size is 0, empty\n", name);
        return DSTORE_SUCC;
    }
    m_walStreamEmpty = false;

    WalFileHeaderData *walFileHeaderData = &m_walFileHeaders[fileIndex];
    int64 resultSize;
    retStatus = m_fileReader.ReadOffset(fileDesc, sizeof(WalFileHeaderData), 0, walFileHeaderData, &resultSize);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Read file %s failed\n", name);
        return DSTORE_FAIL;
    }
    if (static_cast<uint64>(resultSize) != sizeof(WalFileHeaderData)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal file %s read result size(%ld), reach the end wal file\n",
                      name, resultSize);
        return DSTORE_FAIL;
    }
    if (walFileHeaderData->magicNum != WAL_FILE_HEAD_MAGIC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal file %s header magic num invalid, reach the end wal file.\n",
            name);
        return DSTORE_FAIL;
    }
    uint8 *headerData = STATIC_CAST_PTR_TYPE(walFileHeaderData, uint8*);
    uint64 checkSum = WalFileHeaderData::ComputeHdrCrc(headerData);
    if (walFileHeaderData->crc != checkSum) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal file %s header crc check invalid, reach the end wal file.\n",
            name);
        return DSTORE_FAIL;
    }

    UpdateStartEndPlsn(fileStartPlsn, walFileHeaderData->fileSize);
    return DSTORE_SUCC;
}

WalDumpWalFileInfo WalDumpFileReader::GetWalFileInfo() const
{
    return m_walFileInfo;
}

uint64 WalDumpFileReader::GetPrevReadStartPoint(UNUSE_PARAM uint64 plsn)
{
    (void)fprintf(DumpToolHelper::dumpPrint, "WalDumpFileReader not support GetPrevReadStartPoint\n");
    return INVALID_PLSN;
}

void WalDumpFileReader::GetWalFilePlsnRange(uint64 *startPlsn, uint64 *endPlsn)
{
    *startPlsn = m_startPlsn;
    *endPlsn = m_endPlsn;
}

bool WalDumpFileReader::WalStreamIsEmpty()
{
    return m_walStreamEmpty;
}

inline RetStatus WalDumpFileReader::MakeWalFileName(char *name, uint32 maxLen, uint64 startPlsn) const
{
    int ret;
    /* Only PAGESTORE mode is relative path in the pdb directory, other mode is absolute path. */
    if (m_vfsType == StorageType::LOCAL || m_vfsType == StorageType::TENANT_ISOLATION) {
        ret = sprintf_s(name, maxLen, "%s/%08hX_%08X_%016llX", m_walDir, m_walFileInfo.walId, 0U, startPlsn);
    } else if (m_vfsType == StorageType::PAGESTORE) {
        ret = sprintf_s(name, maxLen, "%s/%08hX_%08X_%016llX", "wal", m_walFileInfo.walId, 0U, startPlsn);
    } else {
        (void)fprintf(DumpToolHelper::dumpPrint, "Vfs type invalid.\n");
        return DSTORE_FAIL;
    }
    if (ret == -1) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Sprint_f wal file name fail.\n");
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus WalDumpFileReader::OpenFile(
    uint64 startPlsn, const char *fileName, int flags, FileDescriptor **fd, int32_t *index)
{
    *fd = GetFileDescriptor(startPlsn);
    if (*fd != nullptr) {
        *index = GetFileIndex(startPlsn);
        if (*index < 0) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Opened file:%s not found in inner index, startPlsn:%lu.\n",
                fileName, startPlsn);
            return DSTORE_FAIL;
        }
        return DSTORE_SUCC;
    }

    ErrorCode vfsRet = m_fileReader.Open(fileName, flags, &(m_fileDescArrays[m_fileNum]));
    if (vfsRet != 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Open file failed, filePath(%s), vfsRet(%lld).\n", fileName, vfsRet);
        return DSTORE_FAIL;
    }
    *fd = m_fileDescArrays[m_fileNum];
    *index = m_fileNum;
    ++m_fileNum;

    return DSTORE_SUCC;
}

FileDescriptor *WalDumpFileReader::GetFileDescriptor(uint64 fileStartPlsn)
{
    for (uint32 i = 0; i < m_fileNum; i++) {
        if (m_fileStartPlsnArray[i] == fileStartPlsn) {
            return m_fileDescArrays[i];
        }
    }
    return nullptr;
}

WalFileHeaderData *WalDumpFileReader::GetFileHeader(uint64 fileStartPlsn)
{
    for (uint32 i = 0; i < m_fileNum; i++) {
        if (m_fileStartPlsnArray[i] == fileStartPlsn) {
            return &(m_walFileHeaders[i]);
        }
    }
    return nullptr;
}

int32_t WalDumpFileReader::GetFileIndex(uint64 fileStartPlsn)
{
    for (uint32 i = 0; i < m_fileNum; i++) {
        if (m_fileStartPlsnArray[i] == fileStartPlsn) {
            return i;
        }
    }
    return -1;
}

RetStatus WalDumpFileReader::InitLocalWalFileInfo()
{
    DIR *dir;
    struct dirent *filePtr;
    dir = opendir(m_walDir);
    if (dir == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Open dir failed, path(%s).\n", m_walDir);
        return DSTORE_FAIL;
    }
    WalId walId;
    uint64 fileStartPlsn;
    RetStatus result = DSTORE_SUCC;
    while ((filePtr = readdir(dir)) != nullptr) {
        if (strlen(filePtr->d_name) != WAL_FILE_NAME_LEN ||
            strspn(filePtr->d_name, "0123456789ABCDEF_") != WAL_FILE_NAME_LEN) {
            continue;
        }
        char *saveStr;
        char *walIdStr = strtok_r(filePtr->d_name, "_", &saveStr);
        if (walIdStr == nullptr) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Parse file name's walId failed, name(%s).\n", filePtr->d_name);
            result = DSTORE_FAIL;
            break;
        }
        if (sscanf_s(walIdStr, "%llx", &walId) != 1) {
            (void)fprintf(DumpToolHelper::dumpPrint, "invalid walId in file name \"%s\"\n", walIdStr);
            result = DSTORE_FAIL;
            break;
        }
        if (m_walFileInfo.walId != walId) {
            continue;
        }
        char *timelineStr = strtok_r(nullptr, "_", &saveStr);
        if (timelineStr == nullptr) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Parse file name's timeline failed, name(%s).\n", filePtr->d_name);
            result = DSTORE_FAIL;
            break;
        }
        if (sscanf_s(saveStr, "%llx", &fileStartPlsn) != 1) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Could not parse file name startPlsn\"%s\"\n", walIdStr);
            result = DSTORE_FAIL;
            break;
        }

        if (ParseFileHeader(fileStartPlsn) == DSTORE_FAIL) {
            continue;
        }
    }
    int ret = closedir(dir);
    if (ret != 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Close dir(path:%s) fail\n", m_walDir);
    }

    for (uint32 i = 0; i < m_fileNum; ++i) {
        if (m_walFileHeaders[i].fileSize > 0) {
            m_walFileInfo.fileSize = DstoreMax(m_walFileInfo.fileSize, m_walFileHeaders[i].fileSize);
        }
    }

    return result;
}

RetStatus WalDumpFileReader::InitPageStoreWalFileInfo()
{
    uint64_t fileStartPlsn = static_cast<uint64>((m_checkpointPlsn / m_walFileInfo.fileSize) * m_walFileInfo.fileSize);

    /* Step1: Parse WalFile which checkpoint locate */
    char name[MAXPGPATH] = {0};
    if (STORAGE_FUNC_FAIL(MakeWalFileName(name, MAXPGPATH, fileStartPlsn))) {
        return DSTORE_FAIL;
    }
    bool fileExist = false;
    if (STORAGE_FUNC_FAIL(m_fileReader.FileIsExist(name, &fileExist))) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Vfs judge file:%s exist failed\n", name);
        return DSTORE_FAIL;
    }
    /* If this wal stream has no file, use wal stream empty flag to skip. */
    if (fileExist) {
        if (ParseFileHeader(fileStartPlsn) == DSTORE_FAIL) {
            (void)fprintf(DumpToolHelper::dumpPrint, "ParseFileHeader for WalFile(startPlsn:%lu) failed and return\n",
                fileStartPlsn);
            return DSTORE_FAIL;
        }
    } else {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "WalFile(startPlsn:%lu) which checkpoint or designate:%lu locate at not exist.\n", fileStartPlsn,
            m_checkpointPlsn);
    }

    /* Step2: find all WalFile before WalFile which checkpointPlsn locate */
    uint64_t nextForwardFileStartPlsn = fileStartPlsn;
    while (nextForwardFileStartPlsn >= m_walFileInfo.fileSize) {
        nextForwardFileStartPlsn -= m_walFileInfo.fileSize;
        if (STORAGE_FUNC_FAIL(MakeWalFileName(name, MAXPGPATH, nextForwardFileStartPlsn))) {
            return DSTORE_FAIL;
        }
        fileExist = false;
        if (STORAGE_FUNC_FAIL(m_fileReader.FileIsExist(name, &fileExist))) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Vfs judge file:%s exist failed\n", name);
            return DSTORE_FAIL;
        }
        if (!fileExist) {
            (void)fprintf(DumpToolHelper::dumpPrint, "WalFile(startPlsn:%lu) not exist and finish find forward\n",
                nextForwardFileStartPlsn);
            break;
        }
        if (STORAGE_FUNC_FAIL(ParseFileHeader(nextForwardFileStartPlsn))) {
            (void)fprintf(DumpToolHelper::dumpPrint, "WalFile(startPlsn:%lu) parse failed and finish find forward\n",
                nextForwardFileStartPlsn);
            break;
        }
    }

    /* Step3: find all WalFile after WalFile which checkpointPlsn locate */
    uint64_t nextAfterwardFileStartPlsn = fileStartPlsn;
    while (nextAfterwardFileStartPlsn < INVALID_END_PLSN) {
        nextAfterwardFileStartPlsn += m_walFileInfo.fileSize;
        if (STORAGE_FUNC_FAIL(MakeWalFileName(name, MAXPGPATH, nextAfterwardFileStartPlsn))) {
            return DSTORE_FAIL;
        }
        fileExist = false;
        if (STORAGE_FUNC_FAIL(m_fileReader.FileIsExist(name, &fileExist))) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Vfs judge file:%s exist failed\n", name);
            return DSTORE_FAIL;
        }
        if (!fileExist) {
            (void)fprintf(DumpToolHelper::dumpPrint, "WalFile(startPlsn:%lu) not exist and finish find afterward\n",
                nextAfterwardFileStartPlsn);
            break;
        }
        if (STORAGE_FUNC_FAIL(ParseFileHeader(nextAfterwardFileStartPlsn))) {
            (void)fprintf(DumpToolHelper::dumpPrint, "WalFile(startPlsn:%lu) parse failed and finish find afterward\n",
                nextAfterwardFileStartPlsn);
            break;
        }
    }

    return DSTORE_SUCC;
}

RetStatus WalDumpFileReader::TryExpandWalFilesArray()
{
    uint32 nextFileArrayLen = 0;
    if (m_fileArrayLen == 0) {
        nextFileArrayLen = DEFAULT_FILE_ARRAY_LEN;
    } else {
        nextFileArrayLen = m_fileArrayLen + m_fileArrayLen;
    }

    uint64 *newFileStartPlsns =
        static_cast<uint64 *>(DstorePalloc0(nextFileArrayLen * sizeof(uint64)));
    WalFileHeaderData *newWalFileHeaders =
        static_cast<WalFileHeaderData *>(DstorePalloc0(nextFileArrayLen * sizeof(WalFileHeaderData)));
    FileDescriptor **newFileDescriptors =
        static_cast<FileDescriptor **>(DstorePalloc0(nextFileArrayLen * sizeof(FileDescriptor *)));

    if (newFileStartPlsns == nullptr || newWalFileHeaders == nullptr || newFileDescriptors == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Could not malloc new fileHeaders or fileDescriptor array \n");
        return DSTORE_FAIL;
    }

    int rc;
    if (m_fileArrayLen == 0) {
        goto EXIT;
    }

    rc = memcpy_s(newFileStartPlsns, nextFileArrayLen * sizeof(uint64),
        m_fileStartPlsnArray, m_fileArrayLen * sizeof(uint64));
    storage_securec_check(rc, "\0", "\0");

    rc = memcpy_s(newWalFileHeaders, nextFileArrayLen * sizeof(WalFileHeaderData),
        m_walFileHeaders, m_fileArrayLen * sizeof(WalFileHeaderData));
    storage_securec_check(rc, "\0", "\0");

    rc = memcpy_s(newFileDescriptors, nextFileArrayLen * sizeof(FileDescriptor *),
        m_fileDescArrays, m_fileArrayLen * sizeof(FileDescriptor *));
    storage_securec_check(rc, "\0", "\0");

    DstorePfreeExt(m_fileStartPlsnArray);
    DstorePfreeExt(m_walFileHeaders);
    DstorePfreeExt(m_fileDescArrays);

EXIT:
    m_fileStartPlsnArray = newFileStartPlsns;
    m_walFileHeaders = newWalFileHeaders;
    m_fileDescArrays = newFileDescriptors;
    m_fileArrayLen = nextFileArrayLen;

    return DSTORE_SUCC;
}

void WalDumpFileReader::UpdateStartEndPlsn(uint64_t fileStartPlsn, uint64_t fileSize)
{
    if (fileSize != 0) {
        m_startPlsn = fileStartPlsn < m_startPlsn ? fileStartPlsn : m_startPlsn;
        uint64_t fileEndPlsn = fileStartPlsn + fileSize;
        m_endPlsn = fileEndPlsn > m_endPlsn ? fileEndPlsn : m_endPlsn;
    }
}

RetStatus WalDumpFileReader::GetWalFileHeader(uint64 plsn, WalFileHeaderData *walFileHeaderData)
{
    StorageAssert(m_walFileInfo.fileSize != 0);
    uint64 fileStartPlsn = static_cast<uint64>((plsn / m_walFileInfo.fileSize) * m_walFileInfo.fileSize);
    WalFileHeaderData *headerData = GetFileHeader(fileStartPlsn);
    if (headerData == nullptr) {
        return DSTORE_FAIL;
    }
    if (headerData->magicNum != WAL_FILE_HEAD_MAGIC) {
        return DSTORE_FAIL;
    }
    *walFileHeaderData = *headerData;
    return DSTORE_SUCC;
}
}
