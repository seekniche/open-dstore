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
 * dstore_wal_dump.cpp
 *
 * Description:
 * src/wal/dstore_wal_dump.cpp
 *
 * ---------------------------------------------------------------------------------------
 *
 */
#include <pwd.h>

#include "framework/dstore_instance_interface.h"
#include "framework/dstore_pdb.h"
#include "heap/dstore_heap_wal_struct.h"
#include "index/dstore_btree_wal.h"
#include "index/dstore_btree_recycle_wal.h"
#include "systable/dstore_systable_wal.h"
#include "tablespace/dstore_tablespace_wal.h"
#include "undo/dstore_undo_wal.h"
#include "page/dstore_page_diagnose.h"
#include "wal/dstore_wal_dump.h"

namespace DSTORE {

using namespace PageDiagnose;

constexpr uint32 WAL_DUMP_EXPECT_RECORD_NUM = 20000000;

using GetRecordFunc = void (*)(WalDumpRecordHashEntry *, void *, WalDumpRecordHashEntry **);

struct GetRecordFuncTable {
    WalDumpGetRecordType getType;
    GetRecordFunc getFunc;
};

struct GetTargetRecordFuncParam {
    uint64 endPlsn;
};

RetStatus WalDumper::InitWalDumpConfig(WalDumpConfig &config)
{
    char dir[MAXPGPATH] = "./";
    config.timelineId = 0;
    int rc = memcpy_s(config.dir, MAXPGPATH, dir, MAXPGPATH);
    if (rc != 0) {
        (void)fprintf(stderr, "Memcpy config dir failed.\n");
        return DSTORE_FAIL;
    }
    config.reuseVfs = false;
    config.vfs = nullptr;
    config.vfsType = StorageType::LOCAL;
    config.vfsConfigPath = nullptr;
    config.pdbId = INVALID_PDB_ID;
    config.pdbVfsName = static_cast<char *>(malloc(DSTORE_VFS_NAME_MAX_LEN));
    if (config.pdbVfsName == nullptr) {
        (void)fprintf(stderr, "Malloc pdb vfs name failed.\n");
        return DSTORE_FAIL;
    }
    rc = memset_s(config.pdbVfsName, DSTORE_VFS_NAME_MAX_LEN, 0, DSTORE_VFS_NAME_MAX_LEN);
    if (rc != 0) {
        (void)fprintf(stderr, "Memcpy config pdbVfsName failed.\n");
        return DSTORE_FAIL;
    }
    config.dumpWalAfterCheckpoint = false;
    config.walId = INVALID_WAL_ID;
    config.walFileSize = 0;
    config.startPlsn = 0;
    config.endPlsn = WAL_DUMP_INVALID_PLSN;
    config.recordNumPerInputLimit = 0;
    config.displayedRecordNum = 0;
    config.moduleFilter = WAL_DUMP_INVALID_FILTER;
    config.typeFilter = WAL_DUMP_INVALID_FILTER;
    config.xidFilter = { WAL_DUMP_INVALID_XID_FILTER, WAL_DUMP_INVALID_XID_FILTER };
    config.pageIdFilter = INVALID_PAGE_ID;
    config.commandType = WalDumpCommandType::DUMP_WAL_RECORD;
    char dumpDir[MAXPGPATH] = "./waldump_result";
    rc = memcpy_s(config.dumpDir, MAXPGPATH, dumpDir, MAXPGPATH);
    if (rc != 0) {
        (void)fprintf(stderr, "Memcpy config dumpDir failed.\n");
        return DSTORE_FAIL;
    }
    PageDiagnose::InitCommConfig(&config.commConfig);
    config.checkPageError = false;
    config.scanValidCheckpoint = false;
    return DSTORE_SUCC;
}

static RetStatus AddWalInfo(WalId **walIdArray, uint32 **walFileSizeArray, uint32 *walIdCount, WalId walId)
{
    uint32 newWalIdArrayLen = *walIdCount + 1;
    WalId *newWalIdArray = static_cast<WalId *>(DstorePalloc0(newWalIdArrayLen * sizeof(WalId)));
    if (newWalIdArray == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Could not malloc new fileHeaders or file descriptor array \n");
        return DSTORE_FAIL;
    }
    uint32 *newWalFileSizeArray = static_cast<uint32 *>(DstorePalloc0(newWalIdArrayLen * sizeof(uint32)));
    if (newWalFileSizeArray == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Could not malloc new fileHeaders or wal file size array \n");
        return DSTORE_FAIL;
    }

    if (*walIdArray != nullptr) {
        int rc = memcpy_s(newWalIdArray, newWalIdArrayLen * sizeof(WalId), *walIdArray, *walIdCount * sizeof(WalId));
        storage_securec_check(rc, "\0", "\0");
        rc = memcpy_s(newWalFileSizeArray, newWalIdArrayLen * sizeof(uint32), *walFileSizeArray,
            *walIdCount * sizeof(uint32));
        storage_securec_check(rc, "\0", "\0");
    }

    newWalIdArray[newWalIdArrayLen - 1] = walId;
    /* file size in local file mode is unused */
    newWalFileSizeArray[newWalIdArrayLen - 1] = 0;

    DstorePfreeExt(*walIdArray);
    DstorePfreeExt(*walFileSizeArray);

    *walIdArray = newWalIdArray;
    *walFileSizeArray = newWalFileSizeArray;
    *walIdCount = newWalIdArrayLen;
    return DSTORE_SUCC;
}

static bool WalIdIsExisted(WalId *walIdArray, uint32 walIdCount, WalId walId)
{
    for (uint32 i = 0; i < walIdCount; ++i) {
        if (walIdArray[i] == walId) {
            return true;
        }
    }
    return false;
}

static RetStatus GetWalIdsArray(WalDumpConfig *config, WalId **walIdArray, uint32 **walFileSizeArray,
    uint32 *walIdCount)
{
    RetStatus result = DSTORE_SUCC;
    if (config->vfsType == StorageType::PAGESTORE) {
        PageDiagnose::DumpToolHelperInitParam param = {
            .reuseVfs = config->reuseVfs,
            .vfs = config->vfs,
            .pdbVfsName = config->pdbVfsName,
            .commConfig = &config->commConfig
        };
        WalInfoArgs walInfoArgs = {
            .walIdArray = walIdArray,
            .walFileSizeArray = walFileSizeArray,
            .walIdCount = walIdCount
        };
        result = WalDumpFileReader::GetPageInfoFromControlFile(&param, config->vfsConfigPath,
            GetPageInfoType::GET_WAL_ID_ARRAY, &walInfoArgs);
        if (result != DSTORE_SUCC) {
            ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("GetPageInfoFromControlFile fail."));
            return result;
        }
        return DSTORE_SUCC;
    }

    /* below is local file mode */
    DIR *dir;
    struct dirent *filePtr;
    dir = opendir(config->dir);
    if (dir == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Open dir failed, path(%s).\n", config->dir);
        return DSTORE_FAIL;
    }
    WalId walId;
    while ((filePtr = readdir(dir)) != nullptr) {
        if (strlen(filePtr->d_name) != WAL_FILE_NAME_LEN ||
            strspn(filePtr->d_name, "0123456789ABCDEF_") != WAL_FILE_NAME_LEN) {
            continue;
        }
        char *saveStr;
        char *walIdStr = strtok_r(filePtr->d_name, "_", &saveStr);
        if (walIdStr == nullptr) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Parse file name's walId failed, name(%s).\n",
                filePtr->d_name);
            result = DSTORE_FAIL;
            break;
        }
        if (sscanf_s(walIdStr, "%llx", &walId) != 1) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Invalid walId in file name \"%s\"\n", walIdStr);
            result = DSTORE_FAIL;
            break;
        }
        if (!WalIdIsExisted(*walIdArray, *walIdCount, walId)) {
            result = AddWalInfo(walIdArray, walFileSizeArray, walIdCount, walId);
            if (result != DSTORE_SUCC) {
                (void)fprintf(DumpToolHelper::dumpPrint, "Invalid walId in file name \"%s\"\n", walIdStr);
                break;
            }
        }
    }
    int ret = closedir(dir);
    if (ret != 0) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Close dir(path:%s) fail\n", config->dir);
    }
    return result;
}

static RetStatus UpdateVfsInfo(WalDumpConfig *config)
{
    if (config->vfsType == StorageType::PAGESTORE && !config->reuseVfs && config->pdbId != INVALID_PDB_ID) {
        PageDiagnose::DumpToolHelperInitParam param = {
            .reuseVfs = config->reuseVfs,
            .vfs = config->vfs,
            .pdbVfsName = config->pdbVfsName,
            .commConfig = &config->commConfig
        };
        VfsInfoArgs vfsInfoArgs = {
            .pdbId = config->pdbId,
            .vfsName = config->pdbVfsName,
            .vfsNameSize = DSTORE_VFS_NAME_MAX_LEN
        };
        RetStatus result = WalDumpFileReader::GetPageInfoFromControlFile(&param, config->vfsConfigPath,
            GetPageInfoType::GET_VFS_NAME, &vfsInfoArgs);
        if (result != DSTORE_SUCC) {
            ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("GetPageInfoFromControlFile fail."));
            return result;
        }
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("UpdateVfsInfo get vfs name:%s by pdb id %u.", config->pdbVfsName,
            config->pdbId));
        return DSTORE_SUCC;
    }
    return DSTORE_SUCC;
}

static RetStatus UpdateWalFileDir(WalDumpConfig *config)
{
    if (config->vfsType == StorageType::TENANT_ISOLATION) {
        char dirPath[MAXPGPATH];
        int rc = sprintf_s(dirPath, MAXPGPATH, "%s/%s", config->dir, "wal");
        if (rc == -1) {
            ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("Sprint_f wal file directory fail."));
            return DSTORE_FAIL;
        }
        rc = memcpy_s(config->dir, MAXPGPATH, dirPath, MAXPGPATH);
        storage_securec_check(rc, "\0", "\0");
    }
    return DSTORE_SUCC;
}

DumpType WalDumper::GetDumpType(WalDumpConfig *config)
{
    /* Only designate page id filter is DUMP_ONE_PAGE */
    if (config->pageIdFilter != INVALID_PAGE_ID && config->walId == INVALID_WAL_ID &&
        config->xidFilter.zoneId == WAL_DUMP_INVALID_XID_FILTER) {
        if (config->checkPageError) {
            return DumpType::CHECK_ONE_PAGE;
        } else {
            return DumpType::DUMP_ONE_PAGE;
        }
    }
    return DumpType::DUMP_ONE_STREAM;
}

RetStatus WalDumper::StartErrorLog(bool *loggerAlreadyStarted)
{
    if (IsLoggerStarted()) {
        (void)fprintf(stderr, "Error logger already started.\n");
        *loggerAlreadyStarted = true;
        return DSTORE_SUCC;
    }
    SetErrLogDirectory("./wal_dump_error_log");

    ErrorCode err = StartLogger();
    if (err != STORAGE_OK) {
        (void)fprintf(stderr, "StartLogger fail, error code is %lld.\n", err);
        return DSTORE_FAIL;
    }

    err = OpenLogger();
    if (err != STORAGE_OK) {
        (void)fprintf(stderr, "OpenLogger fail, error code is %lld.\n", err);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus WalDumper::SetWalDumpPath(WalDumpConfig *config)
{
    char *realPath = realpath(config->dumpDir, nullptr);
    if (realPath == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("Dump path(%s) is invalid.", config->dumpDir));
        return DSTORE_FAIL;
    }
    char outputFileName[MAXPGPATH];
    RetStatus result = GenerateDumpFileName(config, realPath, outputFileName);
    if (result != DSTORE_SUCC) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("Generate dump file name failed."));
        free(realPath);
        return DSTORE_FAIL;
    }
    FILE *outputFile = fopen(outputFileName, "w");
    if (outputFile == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("Open output dump file %s failed.", outputFileName));
        free(realPath);
        return DSTORE_FAIL;
    }
    DumpToolHelper::SetPrintTarget(outputFile);
    free(realPath);
    return DSTORE_SUCC;
}

RetStatus WalDumper::DumpByConfig(WalDumpConfig *config)
{
    DstoreSetMemoryOutOfControl();
    WalId *walIdArray = nullptr;
    uint32 *walFileSizeArray = nullptr;
    uint32 walIdCount = 0;
    WalId targetWalId = config->walId;
    bool loggerAlreadyStarted = false;
    DumpType dumpType = WalDumper::GetDumpType(config);

    RetStatus result = StartErrorLog(&loggerAlreadyStarted);
    if (result != DSTORE_SUCC) {
        (void)fprintf(stderr, "StartErrorLog fail.\n");
        goto EXIT;
    }
    if (mkdir(config->dumpDir, S_IRWXU) < 0 && errno != EEXIST) {
        ErrLog(DSTORE_ERROR, MODULE_WAL,
               ErrMsg("Failed to create path(%s) for waldump, errno: %s.", config->dumpDir, strerror(errno)));
        result = DSTORE_FAIL;
        goto EXIT;
    }
    if (SetWalDumpPath(config) != DSTORE_SUCC) {
        result = DSTORE_FAIL;
        goto EXIT;
    }
    result = CreateMemoryContextForTool("waldump memcontext");
    if (result != DSTORE_SUCC) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("CreateMemoryContextForTool fail.\n"));
        goto EXIT;
    }
    result = UpdateWalFileDir(config);
    if (result != DSTORE_SUCC) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("UpdateWalFileDir fail."));
        goto EXIT;
    }
    result = UpdateVfsInfo(config);
    if (result != DSTORE_SUCC) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("UpdateVfsInfo fail."));
        goto EXIT;
    }
    result = GetWalIdsArray(config, &walIdArray, &walFileSizeArray, &walIdCount);
    if (result != DSTORE_SUCC) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("GetWalIdsArray fail."));
        goto EXIT;
    }
    if (walIdArray == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("walIdArray array is null."));
        result = DSTORE_FAIL;
        goto EXIT;
    }

    /* dump one page in all stream */
    if (dumpType == DumpType::DUMP_ONE_PAGE || dumpType == DumpType::CHECK_ONE_PAGE) {
        if (SetWalDumpPath(config) != DSTORE_SUCC) {
            result = DSTORE_FAIL;
            goto EXIT;
        }
        WalDumper *walDumper = new WalDumper(config);
        StorageAssert(walDumper != nullptr);
        result = walDumper->Dump(dumpType, walIdArray, walFileSizeArray, walIdCount);
        fclose(DumpToolHelper::dumpPrint);
        delete walDumper;
        goto EXIT;
    }

    /* dump one page in one stream, or dump one or all stream */
    for (uint32 index = 0; index < walIdCount; index++) {
        uint32 walFileSize = config->vfsType == StorageType::PAGESTORE ? walFileSizeArray[index] : 0;
        config->walId = targetWalId != INVALID_WAL_ID ? targetWalId : walIdArray[index];
        config->walFileSize = walFileSize;
        if (SetWalDumpPath(config) != DSTORE_SUCC) {
            result = DSTORE_FAIL;
            goto EXIT;
        }
        (void)fprintf(DumpToolHelper::dumpPrint,
            "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump WalId %lu Start~~~~~~~~~~~~~~~~~~~~~~~~~~~~~.\n", config->walId);
        WalDumper *walDumper = new WalDumper(config);
        StorageAssert(walDumper != nullptr);
        RetStatus oneStreamResult = walDumper->Dump(dumpType,
            targetWalId != INVALID_WAL_ID ? &targetWalId : &walIdArray[index], &walFileSize, 1);
        (void)fprintf(DumpToolHelper::dumpPrint,
            "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump WalId %lu Finish~~~~~~~~~~~~~~~~~~~~~~~~~~~~.\n", config->walId);
        if (oneStreamResult != DSTORE_SUCC) {
            result = DSTORE_FAIL;
        }
        fclose(DumpToolHelper::dumpPrint);
        delete walDumper;
        /* If target wal id is not invalid, just dump one time. */
        if (targetWalId != INVALID_WAL_ID) {
            break;
        }
    }

EXIT:
    if (result != DSTORE_SUCC) {
        (void)fprintf(stderr, "There were some errors, see wal_dump_error_log for more details\n.");
    }
    DstorePfreeExt(walIdArray);
    DstorePfreeExt(walFileSizeArray);
    if (!loggerAlreadyStarted) {
        CloseLogger();
        StopLogger();
    }
    DstoreSetMemoryInControl();
    return result;
}

RetStatus WalDumper::GenerateDumpFileName(WalDumpConfig *config, const char *dumpDir, char *outputFileName)
{
    int rc = memset_s(outputFileName, MAXPGPATH, 0, MAXPGPATH);
    storage_securec_check(rc, "\0", "\0");
    const char *dumpType = (config->pageIdFilter != INVALID_PAGE_ID && config->walId == INVALID_WAL_ID)
                               ? (config->checkPageError ? "check_page" : "dump_page")
                               : "dump_stream";
    rc = sprintf_s(outputFileName, MAXPGPATH, "%s/%s_%lu_%u_%u", dumpDir, dumpType,
                   config->walId == INVALID_WAL_ID ? 0 : config->walId, config->pageIdFilter.m_fileId,
                   config->pageIdFilter.m_blockId == DSTORE_INVALID_BLOCK_NUMBER ? 0 : config->pageIdFilter.m_blockId);
    if (rc == -1) {
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

void WalDumper::DumpToLocalFile(WalDumpConfig *config, StringInfoData *dumpInfo)
{
    char dirPath[MAXPGPATH];
    if (config->vfsType == StorageType::LOCAL || config->vfsType == StorageType::TENANT_ISOLATION) {
        int rc = memcpy_s(dirPath, MAXPGPATH, config->dir, MAXPGPATH);
        if (rc != 0) {
            dumpInfo->append("Memcpy directory path failed. \n");
            return;
        }
    } else if (config->vfsType == StorageType::PAGESTORE) {
        char logPath[MAXPGPATH];
        GetErrLogDirectory(logPath, MAXPGPATH);
        bool res = CanonicalizePath(logPath, dirPath);
        if (res == false) {
            dumpInfo->append("Get log absolute path to output failed. \n");
            return;
        }
    } else {
        dumpInfo->append("Vfs type invalid. \n");
        return;
    }
    int rc = sprintf_s(config->dumpDir, MAXPGPATH, "%s/%s", dirPath, "waldump_result");
    if (rc == -1) {
        dumpInfo->append("Set dump path failed, rc = %d. \n", rc);
        return;
    }

    RetStatus result = DumpByConfig(config);
    if (result != DSTORE_SUCC) {
        dumpInfo->append("Dump by config failed. \n");
        dumpInfo->append("Dump result output to path: %s \n", config->dumpDir);
        return;
    }

    dumpInfo->append("Dump result output to path: %s \n", config->dumpDir);
    return;
}

RetStatus WalDumper::Dump(DumpType dumpType, WalId *walIdArray, uint32 *walFileSizeArray, uint32 walIdCount)
{
    RetStatus retStatus = Init(walIdArray, walFileSizeArray, walIdCount);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Init WalDump fail.\n");
        Destroy();
        return retStatus;
    }

    if (dumpType == DumpType::DUMP_ONE_STREAM) {
        retStatus = DumpOneWalStream();
        if (STORAGE_FUNC_FAIL(retStatus)) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Wal dump stream fail.\n");
        }
        Destroy();
        return retStatus;
    }

    if (dumpType == DumpType::CHECK_ONE_PAGE) {
        retStatus = DumpOnePageWithCheck();
        if (STORAGE_FUNC_FAIL(retStatus)) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Wal check page fail.\n");
        }
        return retStatus;
    }

    retStatus = DumpOnePage();
    if (STORAGE_FUNC_FAIL(retStatus)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal dump page fail.\n");
    }

    Destroy();
    return retStatus;
}

WalDumper::WalDumper(const WalDumpConfig *config)
    : m_walStreamCount(0),
    m_walStreamReader(nullptr),
    m_config(*config),
    m_inited(false),
    m_memoryContext(nullptr),
    m_bufferForDecompress(),
    m_recordHtab(nullptr),
    m_prevRecordHtab(nullptr),
    m_pageErrorInfo({WAL_DUMP_INVALID_RECORD_KEY, WAL_DUMP_INVALID_RECORD_KEY, WAL_DUMP_INVALID_RECORD_KEY})
{}

WalDumper::~WalDumper()
{
    for (uint32 i = 0; i < m_walStreamCount; ++i) {
        if (m_walStreamReader[i].walDumpFileReader != nullptr) {
            delete m_walStreamReader[i].walDumpFileReader;
            m_walStreamReader[i].walDumpFileReader = nullptr;
        }
        if (m_walStreamReader[i].walRecordReader != nullptr) {
            delete m_walStreamReader[i].walRecordReader;
            m_walStreamReader[i].walRecordReader = nullptr;
        }
    }
    DstorePfreeExt(m_walStreamReader);

    m_memoryContext = nullptr;
    DstorePfreeExt(m_bufferForDecompress.buffer);
    m_inited = false;
}

void WalDumper::AdjustStartPlsnByCheckpoint(uint64 *startPlsn, uint64 checkpointPlsn)
{
    if (*startPlsn == WAL_DUMP_INVALID_PLSN || *startPlsn < checkpointPlsn) {
        /* try to move startplsn to diskRecoveryPlsn */
        (void)fprintf(DumpToolHelper::dumpPrint, "Modify start plsn from %lu to checkpoint plsn %lu.\n", *startPlsn,
            checkpointPlsn);
        *startPlsn = checkpointPlsn;
    } else {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "No need to modify start plsn %lu which is not smaller than checkpoint plsn %lu.\n", *startPlsn,
            checkpointPlsn);
    }
}

RetStatus WalDumper::HashInit()
{
    constexpr uint32 expectPageNum = WAL_DUMP_EXPECT_RECORD_NUM;
    HASHCTL info;
    info.keysize = sizeof(WalDumpRecordHashKey);
    info.entrysize = sizeof(WalDumpRecordHashEntry);
    info.dsize = hash_select_dirsize(expectPageNum);
    info.hash = tag_hash;
    info.hcxt = m_memoryContext;

    m_recordHtab = hash_create("m_recordHtab", expectPageNum, &info, (HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION));
    if (unlikely(m_recordHtab == nullptr)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "create m_recordHtab failed.\n");
        return DSTORE_FAIL;
    }

    if (m_config.checkPageError) {
        info.keysize = sizeof(WalDumpRecordHashKey);
        info.entrysize = sizeof(WalDumpErrorRecordHashEntry);
        info.dsize = hash_select_dirsize(expectPageNum);
        info.hash = tag_hash;
        info.hcxt = m_memoryContext;
        m_prevRecordHtab =
            hash_create("m_prevRecordHtab", expectPageNum, &info, (HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION));
        if (unlikely(m_prevRecordHtab == nullptr)) {
            (void)fprintf(DumpToolHelper::dumpPrint, "create m_prevRecordHtab failed.\n");
            return DSTORE_FAIL;
        }
    }

    return DSTORE_SUCC;
}

WalDumpRecordHashEntry *WalDumper::HashAdd(WalDumpRecordHashKey recordKey, uint64 groupStartPlsn,
    const WalRecordAtomicGroup *walGroup, WalRecord *record)
{
    bool isFound;
    WalDumpRecordHashEntry *entry = static_cast<WalDumpRecordHashEntry *>(
        hash_search(m_recordHtab, &recordKey, HASH_ENTER, &isFound));
    if (unlikely(isFound)) {
        return nullptr;
    }
    entry->recordKey = recordKey;
    entry->groupStartPlsn = groupStartPlsn;
    entry->record = record;
    entry->group = *walGroup;
    return entry;
}

WalDumpRecordHashEntry *WalDumper::HashGet(WalDumpRecordHashKey recordKey)
{
    bool isFound;
    return static_cast<WalDumpRecordHashEntry *>(hash_search(m_recordHtab, &recordKey, HASH_FIND, &isFound));
}

void WalDumper::HashDelete(WalDumpRecordHashKey recordKey)
{
    WalDumpRecordHashEntry *entry = HashGet(recordKey);
    if (entry == nullptr) {
        return;
    }
    DstorePfreeExt(entry->record);
    (void)hash_search(m_recordHtab, &recordKey, HASH_REMOVE, nullptr);
}

void WalDumper::HashDestroy()
{
    if (m_recordHtab != nullptr) {
        HASH_SEQ_STATUS hashStatus;
        WalDumpRecordHashEntry *entry = nullptr;
        hash_seq_init(&hashStatus, m_recordHtab);
        while ((entry = static_cast<WalDumpRecordHashEntry *>(hash_seq_search(&hashStatus))) != nullptr) {
            DstorePfreeExt(entry->record);
        }
        hash_destroy(m_recordHtab);
        m_recordHtab = nullptr;
    }
    if (m_prevRecordHtab != nullptr) {
        HASH_SEQ_STATUS hashStatus;
        WalDumpErrorRecordHashEntry *entry = nullptr;
        hash_seq_init(&hashStatus, m_prevRecordHtab);
        while ((entry = static_cast<WalDumpErrorRecordHashEntry *>(hash_seq_search(&hashStatus))) != nullptr) {
            DstorePfreeExt(entry->record);
        }
        hash_destroy(m_prevRecordHtab);
        m_prevRecordHtab = nullptr;
    }
}

RetStatus WalDumper::InitPlsn(uint64 checkpointStartPlsn, uint64 minPlsn, uint64 maxPlsn, uint64 *startPlsn,
    uint64 *endPlsn)
{
    /* Init start and end plsn according to config */
    *startPlsn = m_config.startPlsn;
    *endPlsn = m_config.endPlsn;
    if (m_config.vfsType == StorageType::PAGESTORE) {
        if (m_config.dumpWalAfterCheckpoint) {
            AdjustStartPlsnByCheckpoint(startPlsn, checkpointStartPlsn);
        }
    } else {
        if (m_config.dumpWalAfterCheckpoint) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "Temporarily not support dump log after checkpoint in local mode for now.\n");
            return DSTORE_FAIL;
        }
    }

    /* Update start and end plsn according to wal file in storage */
    if (!CheckPlsn(minPlsn, maxPlsn, startPlsn, endPlsn)) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "Check plsn failed, min plsn %lu, max plsn %lu, start plsn %lu, end plsn %lu.\n",
            minPlsn, maxPlsn, *startPlsn, *endPlsn);
        return DSTORE_FAIL;
    }

    return DSTORE_SUCC;
}

RetStatus WalDumper::InitFileReader(uint64 checkpointPlsn, WalId walId, uint32_t walFileSize,
    WalDumpFileReader **fileReader)
{
    char *path = m_config.vfsType == StorageType::PAGESTORE ? m_config.vfsConfigPath : m_config.dir;
    *fileReader = DstoreNew(m_memoryContext)WalDumpFileReader(path, m_config.vfsType, checkpointPlsn);
    if (unlikely(*fileReader == nullptr)) {
        return DSTORE_FAIL;
    }

    (*fileReader)->SetWalDumpWalFileInfo(walId, walFileSize);
    RetStatus retStatus =
        (*fileReader)->Init(m_config.reuseVfs, m_config.vfs, m_config.pdbVfsName, &m_config.commConfig);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal dump file reader init fail\n");
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus WalDumper::InitRecordReader(WalDumpFileReader *fileReader, uint64 *startPlsn, uint64 fileSize,
    WalRecordReader **recordReader)
{
    const WalRecordAtomicGroup *walGroup = nullptr;
    WalReaderConf walReaderConf = {
        .walId = INVALID_WAL_ID,
        .startPlsn = *startPlsn,
        .fileReader = fileReader,
        .walReadBuffer = nullptr,
        .walFileSize = fileSize,
        .walReadSource = WalReadSource::WAL_READ_FROM_DISK
    };
    *recordReader = DstoreNew(m_memoryContext) WalRecordReader(m_memoryContext, walReaderConf);
    if (unlikely(*recordReader == nullptr)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Create wal record reader fail\n");
        return DSTORE_FAIL;
    }
    RetStatus retStatus = (*recordReader)->Init();
    if (STORAGE_FUNC_FAIL(retStatus)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Init wal record reader fail.\n");
        return DSTORE_FAIL;
    }

    retStatus = (*recordReader)->ReadNext(&walGroup);
    if (retStatus != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal record reader read fail.\n");
        return DSTORE_FAIL;
    }
    if (walGroup == nullptr) {
        uint64 validStartPlsn = FindValidGroupStartPlsn(startPlsn, fileReader, *recordReader);
        if (validStartPlsn == WAL_DUMP_INVALID_PLSN) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "Find valid read start plsn fail. Config start plsn:%lu, might reach end\n", *startPlsn);
            return DSTORE_FAIL;
        }
        *startPlsn = validStartPlsn;
    }
    (*recordReader)->SetReadStartPlsn(*startPlsn);
    return DSTORE_SUCC;
}

RetStatus WalDumper::Init(WalId *walIdArray, uint32 *walFileSizeArray, uint32 walIdCount)
{
    m_memoryContext = g_dstoreCurrentMemoryContext;
    RetStatus retStatus = HashInit();
    if (retStatus != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Init hash table fail\n");
        return DSTORE_FAIL;
    }

    WalCheckPoint *checkpoint = static_cast<WalCheckPoint *>(DstorePalloc0(walIdCount * sizeof(WalCheckPoint)));
    if (unlikely(checkpoint == nullptr)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Malloc checkpoint array fail\n");
        goto ERROR_EXIT;
    }

    m_walStreamReader = static_cast<WalDumpStreamReader *>(DstorePalloc0(walIdCount * sizeof(WalDumpStreamReader)));
    if (unlikely(m_walStreamReader == nullptr)) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Malloc wal stream reader fail\n");
        goto ERROR_EXIT;
    }
    m_walStreamCount = walIdCount;

    if (m_config.vfsType == StorageType::PAGESTORE) {
        for (uint32 i = 0; i < walIdCount; ++i) {
            PageDiagnose::DumpToolHelperInitParam param = {
                .reuseVfs = m_config.reuseVfs,
                .vfs = m_config.vfs,
                .pdbVfsName = m_config.pdbVfsName,
                .commConfig = &m_config.commConfig
            };
            WalCheckPointInfoArgs args = {
                .walId = walIdArray[i],
                .checkpoint = &checkpoint[i]
            };
            if (WalDumpFileReader::GetPageInfoFromControlFile(&param, m_config.vfsConfigPath, GET_WAL_CHECKPOINT,
                &args) != DSTORE_SUCC) {
                goto ERROR_EXIT;
            }
            if (checkpoint[i].diskRecoveryPlsn == INVALID_END_PLSN) {
                (void)fprintf(DumpToolHelper::dumpPrint,
                    "WalDumper init wal:%lu failed for get invalid checkpointPlsn:%lu.\n", m_config.walId,
                    checkpoint[i].diskRecoveryPlsn);
                goto ERROR_EXIT;
            }
        }
    }

    for (uint32 i = 0; i < walIdCount; ++i) {
        WalDumpStreamReader *stream = &m_walStreamReader[i];
        /* If user do not designate checkpoint, that config start plsn is 0, checkpoint plsn will be inited below:
         * 1.using disk recovery plsn in page store mode.
         * 2.using INVALID_END_PLSN in loacal mode.
         */
        uint64 checkpointPlsn = m_config.startPlsn;
        if (checkpointPlsn == 0) {
            checkpointPlsn =
                (m_config.vfsType == StorageType::PAGESTORE) ? checkpoint[i].diskRecoveryPlsn : INVALID_END_PLSN;
        }

        retStatus = InitFileReader(checkpointPlsn, walIdArray[i], walFileSizeArray[i], &stream->walDumpFileReader);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Init wal stream %lu file reader failed.\n", walIdArray[i]);
            goto ERROR_EXIT;
        }

        if (stream->walDumpFileReader->WalStreamIsEmpty()) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Wal Stream %lu is empty.\n", walIdArray[i]);
            continue;
        }

        stream->walFileInfo = stream->walDumpFileReader->GetWalFileInfo();

        uint64 minPlsn;
        uint64 maxPlsn;
        stream->walDumpFileReader->GetWalFilePlsnRange(&minPlsn, &maxPlsn);
        retStatus =
            InitPlsn(checkpoint[i].diskRecoveryPlsn, minPlsn, maxPlsn, &stream->startPlsn, &stream->endPlsn);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Init wal stream %lu plsn failed.\n", walIdArray[i]);
            goto ERROR_EXIT;
        }

        retStatus = InitRecordReader(stream->walDumpFileReader, &stream->startPlsn, stream->walFileInfo.fileSize,
            &stream->walRecordReader);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Init wal stream %lu record reader failed.\n", walIdArray[i]);
            goto ERROR_EXIT;
        }
    }

    m_bufferForDecompress.buffer = DstorePalloc(WAL_DUMP_INIT_TEMP_BUFF_SIZE);
    if (m_bufferForDecompress.buffer == nullptr) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Malloc buffer for decompress fail\n");
        goto ERROR_EXIT;
    }
    m_bufferForDecompress.bufferSize = WAL_DUMP_INIT_TEMP_BUFF_SIZE;
    m_inited = true;
    DstorePfreeExt(checkpoint);
    return DSTORE_SUCC;
ERROR_EXIT:
    HashDestroy();
    DstorePfreeExt(checkpoint);
    for (uint32 i = 0; i < m_walStreamCount; ++i) {
        if (m_walStreamReader[i].walDumpFileReader != nullptr) {
            m_walStreamReader[i].walDumpFileReader->Destroy();
            delete m_walStreamReader[i].walDumpFileReader;
            m_walStreamReader[i].walDumpFileReader = nullptr;
        }
        if (m_walStreamReader[i].walRecordReader != nullptr) {
            delete m_walStreamReader[i].walRecordReader;
            m_walStreamReader[i].walRecordReader = nullptr;
        }
    }
    m_walStreamCount = 0;
    DstorePfreeExt(m_walStreamReader);
    m_walStreamReader = nullptr;
    return DSTORE_FAIL;
}

void WalDumper::Destroy()
{
    HashDestroy();
    for (uint32 i = 0; i < m_walStreamCount; ++i) {
        if (m_walStreamReader[i].walDumpFileReader != nullptr) {
            m_walStreamReader[i].walDumpFileReader->CloseAllFiles();
        }
    }
    for (uint32 i = 0; i < m_walStreamCount; ++i) {
        if (m_walStreamReader[i].walDumpFileReader != nullptr) {
            m_walStreamReader[i].walDumpFileReader->Destroy();
            m_walStreamReader[i].walDumpFileReader = nullptr;
        }
    }
}

bool WalDumper::CheckPlsn(uint64 minPlsn, uint64 maxPlsn, uint64 *startPlsn, uint64 *endPlsn)
{
    if (*startPlsn < minPlsn) {
        *startPlsn = minPlsn;
    } else if (*startPlsn > maxPlsn) {
        return false;
    }

    if (*endPlsn > maxPlsn) {
        *endPlsn = maxPlsn;
    } else if (*endPlsn < minPlsn) {
        return false;
    }

    if (*startPlsn == WAL_DUMP_INVALID_PLSN) {
        *startPlsn = minPlsn;
    }
    if (*startPlsn >= *endPlsn) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "Argument startPlsn %lu greater or equal than endPlsn %lu, invalid.\n", *startPlsn, *endPlsn);
        return false;
    }
    return true;
}

uint64 WalDumper::FindValidGroupStartPlsn(uint64 *startPlsn, WalDumpFileReader *fileReader,
    WalRecordReader *recordReader)
{
    uint64 targetPlsn = *startPlsn;
    WalFileHeaderData headerData = {};
    RetStatus retStatus = fileReader->GetWalFileHeader(targetPlsn, &headerData);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        return WAL_DUMP_INVALID_PLSN;
    }
    recordReader->SetReadStartPlsn(WalFile::GetFirstGroupStartPlsn(headerData));
    const WalRecordAtomicGroup *walGroup = nullptr;
    uint64 curGroupPlsn;
    do {
        retStatus = recordReader->ReadNext(&walGroup);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Wal record reader read next fail.\n");
            return WAL_DUMP_INVALID_PLSN;
        }
        if (walGroup == nullptr) {
            return WAL_DUMP_INVALID_PLSN;
        }
        curGroupPlsn = recordReader->GetCurGroupStartPlsn();
        if (curGroupPlsn >= targetPlsn) {
            return curGroupPlsn;
        }
    } while (true);
}

bool WalDumper::CheckFilterInvalid(const WalRecord *walRecord)
{
    if (m_config.moduleFilter != WAL_DUMP_INVALID_FILTER &&
        !(walRecord->m_type >= MODULE_DESC_TABLE[m_config.moduleFilter].recordTypeLowerBound &&
            walRecord->m_type <= MODULE_DESC_TABLE[m_config.moduleFilter].recordTypeUpperBound)) {
        return true;
    }
    if (m_config.typeFilter != WAL_DUMP_INVALID_FILTER && static_cast<uint16>(walRecord->m_type) !=
        m_config.typeFilter) {
        return true;
    }
    return false;
}

bool WalDumper::CheckRecordFilter(const WalRecord *walRecord)
{
    if (CheckFilterInvalid(walRecord) ||
        (m_config.pageIdFilter != INVALID_PAGE_ID && !WalRecordContainPageId(walRecord, m_config.pageIdFilter))) {
        return true;
    }
    return false;
}

bool WalDumper::CheckGroupFilter(const WalRecordAtomicGroup *walGroup)
{
    if (m_config.xidFilter.zoneId != WAL_DUMP_INVALID_XID_FILTER && !WalGroupContainXid(walGroup, m_config.xidFilter)) {
        return true;
    }
    return false;
}

static const char *PdbSyncModeToString(PdbSyncMode pdbSyncMode)
{
    const char *pdbSyncModeStr;
    switch (pdbSyncMode) {
        case PdbSyncMode::MAX_PERFORMANCE_MODE:
            pdbSyncModeStr = "max_performance_mode";
            break;
        case PdbSyncMode::MAX_RELIABILITY_MODE:
            pdbSyncModeStr = "max_reliability_mode";
            break;
        case PdbSyncMode::MAX_AVAILABILITY_MODE:
            pdbSyncModeStr = "max_availability_mode";
            break;
        case PdbSyncMode::INVALID_SYNC_MODE:
        default:
            pdbSyncModeStr = "invalid";
    }
    return pdbSyncModeStr;
}

RetStatus WalDumper::DumpOneWalStream()
{
    if (m_walStreamReader->walDumpFileReader->WalStreamIsEmpty()) {
        return DSTORE_SUCC;
    }
    const WalRecordAtomicGroup *walGroup = nullptr;
    const WalRecord *walRecord;
    uint64 recordEndPlsn = 0;
    RetStatus retStatus;
    WalRecordReader *walRecordReader = m_walStreamReader->walRecordReader;

    if (m_config.scanValidCheckpoint) {
        uint64 lastValidEndPlsn = INVALID_PLSN;
        uint64 validGroupCount = 0;
        do {
            retStatus = walRecordReader->ReadNext(&walGroup);
            if (retStatus != DSTORE_SUCC || walGroup == nullptr) {
                break;
            }
            uint64 startPlsn = walRecordReader->GetCurGroupStartPlsn();
            uint64 endPlsn = walRecordReader->GetCurGroupEndPlsn();
            (void)fprintf(DumpToolHelper::dumpPrint,
                "valid_group plsn=%lu groupLen=%u crc=0x%08x endPlsn=%lu\n",
                startPlsn, walGroup->groupLen, walGroup->crc, endPlsn);
            lastValidEndPlsn = endPlsn;
            validGroupCount++;
            if (m_walStreamReader->endPlsn != WAL_DUMP_INVALID_PLSN &&
                endPlsn >= m_walStreamReader->endPlsn) {
                break;
            }
        } while (true);
        (void)fprintf(DumpToolHelper::dumpPrint,
            "scan_summary valid_group_count=%lu last_valid_group_end_plsn=%lu\n",
            validGroupCount, lastValidEndPlsn);
        return DSTORE_SUCC;
    }

    do {
        retStatus = walRecordReader->ReadNext(&walGroup);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Wal record reader read next fail.\n");
            return DSTORE_FAIL;
        }
        if (walGroup == nullptr) {
            break;
        }
        if (CheckGroupFilter(walGroup)) {
            continue;
        }
        bool groupInfoDisplayed = false;
        uint32 recordNum = walGroup->recordNum;
        do {
            walRecord = walRecordReader->GetNextWalRecord();
            if (walRecord == nullptr) {
                StorageAssert(recordNum == 0);
                break;
            }
            recordNum--;
            recordEndPlsn = walRecordReader->GetCurRecordEndPlsn();

            if (CheckRecordFilter(walRecord)) {
                continue;
            }

            if (!groupInfoDisplayed) {
                DisplayGroupInfo(walRecordReader->GetCurGroupStartPlsn(), walGroup);
                groupInfoDisplayed = true;
            }
            DisplayRecord(DecompressProc(walRecord), recordEndPlsn, walGroup->xid);
            if (walRecord->GetType() == WAL_BARRIER_CSN) {
                (void)fprintf(DumpToolHelper::dumpPrint, "Wal record @ record end plsn:%lu; xid:(%d, %lu);",
                              recordEndPlsn, static_cast<int>(walGroup->xid.m_zoneId), walGroup->xid.m_logicSlotId);

                const WalBarrierCsn *walBarrierCsn = static_cast<const WalBarrierCsn *>(walRecord);
                (void)fprintf(DumpToolHelper::dumpPrint,
                              "type: WAL_BARRIER_CSN. csn: %lu, node count: %u, term: %lu, sync mode{",
                              walBarrierCsn->barrierCsn, walBarrierCsn->nodeCnt, walBarrierCsn->term);
                for (uint32 i = 0; i < walBarrierCsn->pdbCount; i++) {
                    (void)fprintf(DumpToolHelper::dumpPrint, "%u: %s, ",
                                  walBarrierCsn->syncModeArray[i].standbyClusterId,
                                  PdbSyncModeToString(walBarrierCsn->syncModeArray[i].syncMode));
                }
                (void)fprintf(DumpToolHelper::dumpPrint, "}\n");
            }
            if (m_walStreamReader->endPlsn != WAL_DUMP_INVALID_PLSN && recordEndPlsn >= m_walStreamReader->endPlsn) {
                return DSTORE_SUCC;
            }
            m_config.displayedRecordNum++;
            if (CheckInputRecordNumExceedLimit()) {
                return DSTORE_SUCC;
            }
        } while (true);
    } while (true);

    return DSTORE_SUCC;
}

static void GetLatesetRecord(WalDumpRecordHashEntry *recordEntry, UNUSE_PARAM void *args,
    WalDumpRecordHashEntry **entry)
{
    /* In one wal stream, the after record is always newer than forward */
    *entry = recordEntry;
}

static void GetTargetRecord(WalDumpRecordHashEntry *recordEntry, void *args, WalDumpRecordHashEntry **entry)
{
    GetTargetRecordFuncParam *param = static_cast<GetTargetRecordFuncParam *>(args);
    if (param->endPlsn == recordEntry->recordKey.endPlsn) {
        *entry = recordEntry;
    }
}

static GetRecordFuncTable g_getRecordFuncTable[] = {
    { WalDumpGetRecordType::LATEST_RECORD,  GetLatesetRecord },
    { WalDumpGetRecordType::TARGET_RECORD,  GetTargetRecord  }
};

RetStatus WalDumper::PushRecordToHashTable(WalDumpRecordHashKey recordKey, uint64 groupStartPlsn,
    const WalRecordAtomicGroup *walGroup, const WalRecord *walRecord, WalDumpRecordHashEntry **entry)
{
    /* Push compressd record and group to hash table, do not decompress, or it will waste much memory */
    WalRecord *recordInHashTab = static_cast<WalRecord *>(DstorePalloc0(walRecord->GetSize()));
    if (recordInHashTab == nullptr) {
        return DSTORE_FAIL;
    }
    int rc = memcpy_s(recordInHashTab, walRecord->GetSize(), walRecord, walRecord->GetSize());
    storage_securec_check(rc, "\0", "\0");
    *entry = HashAdd(recordKey, groupStartPlsn, walGroup, recordInHashTab);
    if (*entry == nullptr) {
        DstorePfreeExt(recordInHashTab);
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus WalDumper::PushRecordToPrevHashTable(WalDumpRecordHashKey prevRecordKey, WalDumpRecordHashKey currRecordKey,
                                               uint64 groupStartPlsn, const WalRecordAtomicGroup *walGroup,
                                               const WalRecord *walRecord)
{
    /* Push compressd record and group to hash table, do not decompress, or it will waste much memory */
    WalRecord *recordInHashTab = static_cast<WalRecord *>(DstorePalloc0(walRecord->GetSize()));
    if (recordInHashTab == nullptr) {
        ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("Allocate memory for recordInHashTab fail."));
    }
    int rc = memcpy_s(recordInHashTab, walRecord->GetSize(), walRecord, walRecord->GetSize());
    storage_securec_check(rc, "\0", "\0");

    bool isFound;
    WalDumpErrorRecordHashEntry *entry =
        static_cast<WalDumpErrorRecordHashEntry *>(hash_search(m_prevRecordHtab, &prevRecordKey, HASH_ENTER, &isFound));
    if (unlikely(isFound || entry == nullptr)) {
        DstorePfreeExt(recordInHashTab);
        return DSTORE_FAIL;
    }

    entry->recordKey = prevRecordKey;
    entry->currRecordKey = currRecordKey;
    entry->groupStartPlsn = groupStartPlsn;
    entry->record = recordInHashTab;
    entry->group = *walGroup;

    return DSTORE_SUCC;
}

RetStatus WalDumper::ReadGroupAndGetEntry(WalDumpStreamReader *reader, const WalRecordAtomicGroup *walGroup,
    WalDumpGetRecordParam *getRecordParam, WalDumpRecordHashEntry **entry)
{
    uint32 recordNum = walGroup->recordNum;
    const WalRecord *walRecord;
    WalRecordReader *walRecordReader = reader->walRecordReader;
    uint64 recordEndPlsn = 0;

    do {
        walRecord = walRecordReader->GetNextWalRecord();
        if (walRecord == nullptr) {
            StorageAssert(recordNum == 0);
            break;
        }
        recordNum--;
        recordEndPlsn = walRecordReader->GetCurRecordEndPlsn();

        if (CheckFilterInvalid(walRecord) || !WalRecordContainPageId(walRecord, m_config.pageIdFilter)) {
            continue;
        }

        /* Below, the record modified the target block */
        WalDumpRecordHashEntry *tempEntry = nullptr;
        RetStatus retStatus = PushRecordToHashTable({ reader->walFileInfo.walId, recordEndPlsn },
            walRecordReader->GetCurGroupStartPlsn(), walGroup, walRecord, &tempEntry);
        if (tempEntry == nullptr) {
            continue;
        }
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "Push record to hash table failed, wal id: %lu, end plsn: %lu.\n", reader->walFileInfo.walId,
                recordEndPlsn);
            return DSTORE_FAIL;
        }
        for (uint32_t i = 0; i < sizeof(g_getRecordFuncTable) / sizeof(g_getRecordFuncTable[0]); ++i) {
            if (g_getRecordFuncTable[i].getType == getRecordParam->getType) {
                g_getRecordFuncTable[i].getFunc(tempEntry, getRecordParam->args, entry);
            }
        }
    } while (true);
    return DSTORE_SUCC;
}

RetStatus WalDumper::ReadWalFileAndGetEntry(WalDumpStreamReader *reader, uint64 fileStartPlsn, const uint64 endPlsn,
    WalDumpGetRecordParam *getRecordParam, WalDumpRecordHashEntry **entry)
{
    const WalRecordAtomicGroup *walGroup = nullptr;
    WalRecordReader *recordReader = reader->walRecordReader;
    WalFileHeaderData fileHeader = {};
    RetStatus retStatus = reader->walDumpFileReader->GetWalFileHeader(fileStartPlsn, &fileHeader);
    if (retStatus != DSTORE_SUCC) {
        /* Do not need to return failed, reason: file maybe not existed, so user need to judge by entry is nullptr or
         * not,but not error code
         */
        return DSTORE_SUCC;
    }
    recordReader->SetReadStartPlsn(WalFile::GetFirstGroupStartPlsn(fileHeader));

    retStatus = recordReader->ReadNext(&walGroup);
    if (retStatus != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint,
            "Wal record reader read failed, wal id: %lu, file start plsn: %lu.\n", reader->walFileInfo.walId,
            fileStartPlsn);
        return DSTORE_FAIL;
    }
    while (walGroup != nullptr) {
        retStatus = ReadGroupAndGetEntry(reader, walGroup, getRecordParam, entry);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Read group and get entry failed.\n");
            return DSTORE_FAIL;
        }

        if (recordReader->GetCurGroupEndPlsn() >= endPlsn) {
            break;
        }

        retStatus = recordReader->ReadNext(&walGroup);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "Wal record reader read failed, wal id: %lu, file start plsn: %lu.\n", reader->walFileInfo.walId,
                fileStartPlsn);
            return DSTORE_FAIL;
        }
    }
    return DSTORE_SUCC;
}

RetStatus WalDumper::ReadWalFileAndGetLatesetRecord(WalDumpStreamReader *reader, uint64 targetEndPlsn,
    WalDumpRecordHashEntry **entry)
{
    *entry = nullptr;
    WalDumpWalFileInfo fileInfo = reader->walDumpFileReader->GetWalFileInfo();
    uint64 fileStartPlsn = targetEndPlsn - (targetEndPlsn % fileInfo.fileSize);
    uint64 endPlsn = fileStartPlsn + fileInfo.fileSize;

    WalDumpGetRecordParam param = {
        .getType = WalDumpGetRecordType::LATEST_RECORD,
        .args = nullptr
    };
    RetStatus retStatus = ReadWalFileAndGetEntry(reader, fileStartPlsn, endPlsn, &param, entry);
    if (retStatus != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Read wal file and get lateset end plsn %lu entry failed.\n",
            targetEndPlsn);
        return DSTORE_FAIL;
    }

    /* if not found record which modify target block in this wal file, continue find it in forward wal file */
    if (*entry == nullptr) {
        if (fileStartPlsn == 0) {
            /* Read at wal stream head */
            return DSTORE_SUCC;
        }
        return ReadWalFileAndGetLatesetRecord(reader, fileStartPlsn - fileInfo.fileSize, entry);
    }

    return DSTORE_SUCC;
}

uint64 WalDumper::GetPrevGlsn(const WalRecord *record)
{
    if (record->m_type == WAL_TBS_INIT_MULTIPLE_DATA_PAGES || record->m_type == WAL_TBS_INIT_BITMAP_PAGES) {
        return INVALID_WAL_GLSN;
    }
    const WalRecordForPage *page = static_cast<const WalRecordForPage *>(static_cast<const void *>(record));
    return page->m_pagePreGlsn;
}

bool WalDumper::RecordIsNewer(WalDumpRecordHashEntry *record, WalDumpRecordHashEntry *comparedRecord)
{
    if (record == nullptr) {
        return false;
    }
    if (comparedRecord == nullptr) {
        return true;
    }
    uint64 prevGlsn = GetPrevGlsn(DecompressProc(record->record));
    uint64 comparedPrevGlsn = GetPrevGlsn(DecompressProc(comparedRecord->record));
    return prevGlsn > comparedPrevGlsn;
}

RetStatus WalDumper::GetLatesetRecordEntry(WalDumpRecordHashEntry **entry)
{
    for (uint32 i = 0; i < m_walStreamCount; ++i) {
        WalDumpStreamReader *reader = &m_walStreamReader[i];
        if (reader->walDumpFileReader->WalStreamIsEmpty()) {
            continue;
        }
        WalDumpRecordHashEntry *latestRecord = nullptr;
        RetStatus retStatus = ReadWalFileAndGetLatesetRecord(reader, reader->endPlsn, &latestRecord);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint, "Read wal stream %lu and get lateset record fail.\n",
                reader->walFileInfo.walId);
            return DSTORE_FAIL;
        }
        if (RecordIsNewer(latestRecord, *entry)) {
            *entry = latestRecord;
        }
    }
    return DSTORE_SUCC;
}

bool WalDumper::WalFileAllIsRemain(WalFileHeaderData &fileHeader)
{
    /* If the whole file data is last record remain data */
    if (unlikely(fileHeader.lastRecordRemainLen + WAL_FILE_HDR_SIZE == fileHeader.fileSize)) {
        return true;
    }
    return false;
}

RetStatus WalDumper::ReadWalFileAndGetTargetRecord(WalDumpStreamReader *reader, uint64 fileStartPlsn,
    const uint64 targetEndPlsn, WalDumpRecordHashEntry **entry)
{
    /* Check target record whether already read in hash table */
    WalDumpRecordHashEntry *recordEntry = HashGet({reader->walFileInfo.walId, targetEndPlsn});
    if (recordEntry != nullptr) {
        *entry = recordEntry;
        return DSTORE_SUCC;
    }

    WalFileHeaderData fileHeader = {};
    RetStatus retStatus = reader->walDumpFileReader->GetWalFileHeader(fileStartPlsn, &fileHeader);
    if (retStatus != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Get file start plsn %lu file header failed.\n", fileStartPlsn);
        return DSTORE_FAIL;
    }
    /* If target record in forward wal file */
    if (targetEndPlsn <= WalFile::GetFirstGroupStartPlsn(fileHeader) || WalFileAllIsRemain(fileHeader)) {
        if (fileStartPlsn == 0) {
            /* Can't find forward file */
            return DSTORE_FAIL;
        }
        /* Target record in forward wal file */
        return ReadWalFileAndGetTargetRecord(reader, fileStartPlsn - fileHeader.fileSize, targetEndPlsn, entry);
    }

    /* Target record in this wal file, read file until reach end plsn and return target record */
    GetTargetRecordFuncParam funcParam = {
        .endPlsn = targetEndPlsn
    };
    WalDumpGetRecordParam param = {
        .getType = WalDumpGetRecordType::TARGET_RECORD,
        .args = &funcParam
    };

    retStatus = ReadWalFileAndGetEntry(reader, fileStartPlsn, targetEndPlsn, &param, entry);
    if (retStatus != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Wal record reader read failed.\n");
        return DSTORE_FAIL;
    }

    return DSTORE_SUCC;
}

RetStatus WalDumper::GetTargetRecordEntry(WalId recordWalId, uint64 recordEndPlsn, WalDumpRecordHashEntry **entry)
{
    *entry = nullptr;
    for (uint32 i = 0; i < m_walStreamCount; ++i) {
        if (m_walStreamReader[i].walFileInfo.walId != recordWalId) {
            continue;
        }
        WalDumpWalFileInfo fileInfo = m_walStreamReader[i].walDumpFileReader->GetWalFileInfo();
        uint64 fileStartPlsn = recordEndPlsn - (recordEndPlsn % fileInfo.fileSize);
        return ReadWalFileAndGetTargetRecord(&m_walStreamReader[i], fileStartPlsn, recordEndPlsn, entry);
    }
    return DSTORE_FAIL;
}

void WalDumper::GetPrevRecordInfo(const WalRecord *record, WalId *prevWalId, uint64 *prevEndPlsn)
{
    if (record == nullptr || record->m_type == WAL_TBS_INIT_MULTIPLE_DATA_PAGES ||
        record->m_type == WAL_TBS_INIT_BITMAP_PAGES) {
        return;
    }
    const WalRecordForPage *page = static_cast<const WalRecordForPage *>(static_cast<const void *>(record));
    *prevWalId = page->m_pagePreWalId;
    *prevEndPlsn = page->m_pagePrePlsn;
}

RetStatus WalDumper::DumpOnePage()
{
    (void)fprintf(DumpToolHelper::dumpPrint,
        "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump Page(%u, %u) Start~~~~~~~~~~~~~~~~~~~~~~~~~~~~~.\n",
        m_config.pageIdFilter.m_fileId, m_config.pageIdFilter.m_blockId);
    WalDumpRecordHashEntry *recordEntry = nullptr;
    WalId prevRecordWalId = 0;
    uint64 prevRecordEndPlsn = INVALID_END_PLSN;

    /* Read each wal stream file until find the stream's latest record */
    RetStatus retStatus = GetLatesetRecordEntry(&recordEntry);
    if (retStatus != DSTORE_SUCC) {
        (void)fprintf(DumpToolHelper::dumpPrint, "Get lateset record entry failed.\n");
        return DSTORE_FAIL;
    }

    while (recordEntry != nullptr) {
        const WalRecord *decompressedRecord = DecompressProc(recordEntry->record);
        DisplayGroupInfo(recordEntry->groupStartPlsn, &recordEntry->group);
        DisplayRecord(decompressedRecord, recordEntry->recordKey.endPlsn, recordEntry->group.xid);
        GetPrevRecordInfo(decompressedRecord, &prevRecordWalId, &prevRecordEndPlsn);
        HashDelete(recordEntry->recordKey);

        if (prevRecordWalId == 0 || prevRecordEndPlsn == 0) {
            break;
        }

        m_config.displayedRecordNum++;
        if (CheckInputRecordNumExceedLimit()) {
            return DSTORE_SUCC;
        }

        retStatus = GetTargetRecordEntry(prevRecordWalId, prevRecordEndPlsn, &recordEntry);
        if (retStatus != DSTORE_SUCC) {
            (void)fprintf(DumpToolHelper::dumpPrint,
                "Get target record entry wal id: %lu, end plsn: %lu, failed.\n", prevRecordWalId, prevRecordEndPlsn);
            return DSTORE_FAIL;
        }
    }
    (void)fprintf(DumpToolHelper::dumpPrint,
        "\n~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump Page(%u, %u) Finish~~~~~~~~~~~~~~~~~~~~~~~~~~~~~.\n",
        m_config.pageIdFilter.m_fileId, m_config.pageIdFilter.m_blockId);
    return DSTORE_SUCC;
}

/* record this page error, print it later */
void WalDumper::SetPageError(WalDumpRecordHashKey prevRecord, WalDumpRecordHashKey curRecord)
{
    bool isFound = false;
    WalDumpErrorRecordHashEntry *entry =
        static_cast<WalDumpErrorRecordHashEntry *>(hash_search(m_prevRecordHtab, &prevRecord, HASH_ENTER, &isFound));
    if (!isFound || (entry->recordKey.walId == curRecord.walId && entry->recordKey.endPlsn == curRecord.endPlsn)) {
        return;
    } else {
        (void)fprintf(DumpToolHelper::dumpPrint,
                      "[Found Wal Error] it contain error in page (%u, %u), the same prevRecord = (%lu, %lu), the next "
                      "records are (%lu, %lu) and (%lu, %lu)\n",
                      m_config.pageIdFilter.m_fileId, m_config.pageIdFilter.m_blockId, prevRecord.walId,
                      prevRecord.endPlsn, entry->currRecordKey.walId, entry->currRecordKey.endPlsn, curRecord.walId,
                      curRecord.endPlsn);
        m_pageErrorInfo.prevRecordKey = prevRecord;
        m_pageErrorInfo.errorRecordKey_1 = curRecord;
        m_pageErrorInfo.errorRecordKey_2 = {entry->currRecordKey.walId, entry->currRecordKey.endPlsn};
    }
}

/* Get all record info in all walstreams, push to hash table, mark error */
RetStatus WalDumper::GetAllRecordWithPageFilter()
{
    for (uint32 i = 0; i < m_walStreamCount; ++i) {
        WalDumpStreamReader *reader = &m_walStreamReader[i];
        if (reader->walDumpFileReader->WalStreamIsEmpty()) {
            continue;
        }
        const WalRecordAtomicGroup *walGroup = nullptr;
        const WalRecord *walRecord;
        uint64 recordEndPlsn = 0;
        RetStatus retStatus;
        WalRecordReader *walRecordReader = reader->walRecordReader;
        WalId prevRecordWalId = 0;
        uint64 prevRecordEndPlsn = INVALID_END_PLSN;

        do {
            retStatus = walRecordReader->ReadNext(&walGroup);
            if (retStatus != DSTORE_SUCC) {
                (void)fprintf(DumpToolHelper::dumpPrint, "Wal record reader read next fail.\n");
                return DSTORE_FAIL;
            }
            if (walGroup == nullptr) {
                break;
            }
            uint32 recordNum = walGroup->recordNum;
            do {
                walRecord = walRecordReader->GetNextWalRecord();
                if (walRecord == nullptr) {
                    StorageAssert(recordNum == 0);
                    break;
                }
                recordNum--;
                recordEndPlsn = walRecordReader->GetCurRecordEndPlsn();

                if (CheckRecordFilter(walRecord)) {
                    continue;
                }
                const WalRecord *decompressedRecord = DecompressProc(walRecord);
                GetPrevRecordInfo(decompressedRecord, &prevRecordWalId, &prevRecordEndPlsn);
                retStatus = PushRecordToPrevHashTable(
                    {prevRecordWalId, prevRecordEndPlsn}, {reader->walFileInfo.walId, recordEndPlsn},
                    walRecordReader->GetCurGroupStartPlsn(), walGroup, walRecord);
                if (retStatus != DSTORE_SUCC) {
                    SetPageError({prevRecordWalId, prevRecordEndPlsn}, {reader->walFileInfo.walId, recordEndPlsn});
                }
                if (m_walStreamReader->endPlsn != WAL_DUMP_INVALID_PLSN &&
                    recordEndPlsn >= m_walStreamReader->endPlsn) {
                    return DSTORE_SUCC;
                }
            } while (true);
        } while (true);
    }
    return DSTORE_SUCC;
}

/*
 * m_prevRecordHtab use in check page, it contails preRecordLsn -> currRecord
 * m_recordHtab, it contains currRecordLsn -> currRecord
 */
RetStatus WalDumper::DumpWalList(WalDumpRecordHashKey prevRecord, WalDumpRecordHashKey curRecord)
{
    (void)fprintf(
        DumpToolHelper::dumpPrint,
        "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump Wal List %lu/%lu ->  %lu/%lu Start~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n",
        prevRecord.walId, prevRecord.endPlsn, curRecord.walId, curRecord.endPlsn);
    /* prevRecord & curRecord not in m_prevRecordHtab, use GetTargetRecordEntry */
    WalDumpRecordHashEntry *recordEntry = nullptr;
    GetTargetRecordEntry(prevRecord.walId, prevRecord.endPlsn, &recordEntry);
    const WalRecord *decompressedRecord = DecompressProc(recordEntry->record);
    DisplayGroupInfo(recordEntry->groupStartPlsn, &recordEntry->group);
    DisplayRecord(decompressedRecord, recordEntry->recordKey.endPlsn, recordEntry->group.xid);

    GetTargetRecordEntry(curRecord.walId, curRecord.endPlsn, &recordEntry);
    decompressedRecord = DecompressProc(recordEntry->record);
    DisplayGroupInfo(recordEntry->groupStartPlsn, &recordEntry->group);
    DisplayRecord(decompressedRecord, recordEntry->recordKey.endPlsn, recordEntry->group.xid);
    WalDumpRecordHashKey tmpRecordKey = curRecord;
    do {
        bool isFound = false;
        WalDumpErrorRecordHashEntry *entry = static_cast<WalDumpErrorRecordHashEntry *>(
            hash_search(m_prevRecordHtab, &tmpRecordKey, HASH_ENTER, &isFound));
        if (!isFound) {
            break;
        }
        decompressedRecord = DecompressProc(entry->record);
        DisplayGroupInfo(entry->groupStartPlsn, &entry->group);
        DisplayRecord(decompressedRecord, entry->currRecordKey.endPlsn, entry->group.xid);
        tmpRecordKey = {entry->currRecordKey.walId, entry->currRecordKey.endPlsn};
    } while (true);

    (void)fprintf(
        DumpToolHelper::dumpPrint,
        "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump Wal List %lu/%lu ->  %lu/%lu End~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n",
        prevRecord.walId, prevRecord.endPlsn, curRecord.walId, curRecord.endPlsn);

    return DSTORE_SUCC;
}

/* check page error */
RetStatus WalDumper::DumpOnePageWithCheck()
{
    (void)fprintf(DumpToolHelper::dumpPrint,
                  "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump Page(%u, %u) Error Start~~~~~~~~~~~~~~~~~~~~~~~~~~~~~.\n",
                  m_config.pageIdFilter.m_fileId, m_config.pageIdFilter.m_blockId);

    GetAllRecordWithPageFilter();
    if (m_pageErrorInfo.prevRecordKey.walId != INVALID_WAL_ID) {
        DumpWalList(m_pageErrorInfo.prevRecordKey, m_pageErrorInfo.errorRecordKey_1);
        DumpWalList(m_pageErrorInfo.prevRecordKey, m_pageErrorInfo.errorRecordKey_2);
    } else {
        (void)fprintf(DumpToolHelper::dumpPrint, "There is no page error in page [%u, %u].\n",
                      m_config.pageIdFilter.m_fileId, m_config.pageIdFilter.m_blockId);
    }

    (void)fprintf(DumpToolHelper::dumpPrint,
                  "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~Dump Page(%u, %u) Error End~~~~~~~~~~~~~~~~~~~~~~~~~~~~~.\n",
                  m_config.pageIdFilter.m_fileId, m_config.pageIdFilter.m_blockId);
    return DSTORE_SUCC;
}

bool WalDumper::CheckInputRecordNumExceedLimit()
{
    if (m_config.recordNumPerInputLimit > 0 && m_config.displayedRecordNum >= m_config.recordNumPerInputLimit) {
        int input;
        bool needExit = false;
        while (!needExit) {
            (void)printf("Press enter to continue or type 'q' to quit: ");
            input = getchar();
            if (input == '\n') {
                m_config.displayedRecordNum = 0;
                needExit = true;
                continue;
            } else if (input == 'q' || input == 'Q') {
                return true;
            } else {
                (void)printf("Invalid input. Please try again.");
                continue;
            }
        }
    }
    return false;
}

void WalDumper::ReAllocBuffForDecompress(uint32 newBuffSize, uint32 oldBuffSize)
{
    char *newBuff = static_cast<char *>(DstorePalloc0(newBuffSize));
    if (newBuff == nullptr) {
        storage_set_error(WAL_ERROR_INTERNAL_ERROR);
        ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("RedoWalRecord new m_buffForDecompress error!"));
    }
    errno_t rc = memcpy_s(newBuff, newBuffSize, static_cast<char *>(m_bufferForDecompress.buffer), oldBuffSize);
    storage_securec_check(rc, "\0", "\0");
    DstorePfreeExt(m_bufferForDecompress.buffer);
    m_bufferForDecompress.buffer = newBuff;
}


const WalRecord *WalDumper::DecompressProc(const WalRecord *walRecord)
{
    if (!WalRecovery::IsSupportCompress(walRecord->GetType())) {
        return walRecord;
    }
    return static_cast<const WalRecord *>(WalRecovery::DecompressProc(walRecord, &m_bufferForDecompress));
}

void WalDumper::DisplayRecord(const WalRecord *record, uint64 recordEndPlsn, Xid xid)
{
    StorageReleasePanic(record == nullptr, MODULE_WAL, ErrMsg("Waldump get invalid record."));
    const WalRecordForPage *walRecordForPage = static_cast<const WalRecordForPage *>(record);
    (void)fprintf(DumpToolHelper::dumpPrint, "Wal record @ record end plsn:%lu; xid:(%d, %lu); ", recordEndPlsn,
        static_cast<int>(xid.m_zoneId), xid.m_logicSlotId);
    WalType type = walRecordForPage->m_type;
    for (uint16 i = 0; i < MAX_MODULE_ID; i++) {
        if (type >= MODULE_DESC_TABLE[i].recordTypeLowerBound && type <= MODULE_DESC_TABLE[i].recordTypeUpperBound) {
            switch (MODULE_DESC_TABLE[i].type) {
                case ModuleType::HEAP:
                    WalRecordHeap::DumpHeapRecord(
                        static_cast<const WalRecordHeap *>(record), DumpToolHelper::dumpPrint);
                    break;
                case ModuleType::INDEX:
                    WalRecordIndex::DumpIndexRecord(
                        static_cast<const WalRecordIndex *>(record), DumpToolHelper::dumpPrint);
                    break;
                case ModuleType::BTREERECYCLE:
                    WalRecordBtrRecycle::DumpBtrRecycleRecord(
                        static_cast<const WalRecordBtrRecycle *>(record), DumpToolHelper::dumpPrint);
                    break;
                case ModuleType::TABLESPACE:
                    WalRecordTbs::DumpTbsRecord(static_cast<const WalRecordTbs *>(record), DumpToolHelper::dumpPrint);
                    break;
                case ModuleType::UNDO:
                    WalRecordUndo::DumpUndoRecord(
                        static_cast<const WalRecordUndo *>(record), DumpToolHelper::dumpPrint);
                    break;
                case ModuleType::CHECKPOINT:
                    (void)fprintf(DumpToolHelper::dumpPrint, "CheckPoint no wal now.\n");
                    break;
                case ModuleType::SYSTABLE:
                    WalRecordSystable::DumpSystableRecord(
                        static_cast<const WalRecordSystable *>(record), DumpToolHelper::dumpPrint);
                    break;
                default:
                    (void)fprintf(DumpToolHelper::dumpPrint, "Invalid wal type.\n");
            }
            break;
        }
    }
    (void)fprintf(DumpToolHelper::dumpPrint, "\n");
}

void WalDumper::DisplayGroupInfo(uint64 groupStartPlsn, const WalRecordAtomicGroup *group) const
{
    (void)fprintf(DumpToolHelper::dumpPrint,
        "\nRecord atomic group @ start plsn:%lu; xid:(%d, %lu); len:%u; record num:%hu\n",
        groupStartPlsn, static_cast<int>(group->xid.m_zoneId), group->xid.m_logicSlotId,
        group->groupLen, group->recordNum);
}

bool WalDumper::WalRecordContainPageId(const WalRecord *walRecord, const PageId pageId)
{
    PageId firstPageId{};
    uint16 pageCount = 1;
    if (walRecord->m_type == WAL_TBS_INIT_MULTIPLE_DATA_PAGES) {
        static_cast<const WalRecordTbsInitDataPages *>(static_cast<const void *>(walRecord))
            ->GetPageIdRange(firstPageId, pageCount);
    } else if (walRecord->m_type == WAL_TBS_INIT_BITMAP_PAGES) {
        static_cast<const WalRecordTbsInitBitmapPages *>(static_cast<const void *>(walRecord))
            ->GetPageIdRange(firstPageId, pageCount);
    } else if (walRecord->m_type == WAL_TBS_CREATE_TABLESPACE || walRecord->m_type == WAL_TBS_CREATE_DATA_FILE ||
               walRecord->m_type == WAL_TBS_ADD_FILE_TO_TABLESPACE || walRecord->m_type == WAL_TBS_DROP_TABLESPACE ||
               walRecord->m_type == WAL_TBS_DROP_DATA_FILE || walRecord->m_type == WAL_TBS_ALTER_TABLESPACE) {
        return false;
    } else {
        firstPageId =
            static_cast<const WalRecordForPageOnDisk *>(static_cast<const void *>(walRecord))->GetCompressedPageId();
    }
    for (uint16 i = 0; i < pageCount; i++) {
        PageId curPageId = {firstPageId.m_fileId, firstPageId.m_blockId + i};
        if (curPageId == pageId) {
            return true;
        }
    }
    return false;
}

bool WalDumper::WalGroupContainXid(const WalRecordAtomicGroup *group, const XidFilter xid)
{
    if (group->xid.m_zoneId == xid.zoneId && group->xid.m_logicSlotId == xid.logicSlotId) {
        return true;
    }
    return false;
}

}
