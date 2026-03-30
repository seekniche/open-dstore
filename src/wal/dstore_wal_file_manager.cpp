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
 * dstore_wal_file_manager.cpp
 *
 * Description:
 * src/wal/dstore_wal_file_manager.cpp
 *
 * ---------------------------------------------------------------------------------------
 *
 */
#include <future>
#include "port/dstore_port.h"
#include "common/log/dstore_log.h"
#include "common/instrument/perf/dstore_perf.h"
#include "buffer/dstore_checkpointer.h"
#include "wal/dstore_wal_perf_unit.h"
#include "wal/dstore_wal_file_manager.h"
#include "framework/dstore_watchdog.h"

namespace DSTORE {
static constexpr uint32 WAIT_COUNT_FOR_REPORT_WARNING = 5000;
static constexpr uint64 WAIT_CHECKPOINT_INIT_MILLISEC_INTERVAL = 1000;
static constexpr uint32 WAIT_PAUSE_RECYCLE_MICROSEC = 2000;
static constexpr uint32 WAIT_PAUSE_REQUEST_TIMEOUT_MILLISEC = 10;

WalFile::WalFile(WalFileInitPara para)
    : m_vfs(para.vfs),
      m_startPlsn(para.startPlsn),
      m_flushedPlsn(0),
      m_maxValidDataOffset(0),
      m_flushCallBackInfo(para.flushCallBackInfo),
      m_zeroCopyInfo(para.zeroCopyInfo),
      m_next(nullptr),
      m_fd(nullptr),
      m_onlyForRead(para.onlyForRead),
      m_diorw(para.diorw)
{
    int result = sprintf_s(m_path, MAXPGPATH, "%s", para.path);
    storage_securec_check_ss(result)
}

WalFile::~WalFile()
{
    Close();
    m_vfs = nullptr;
    m_next = nullptr;
    m_fd = nullptr;
}

uint64 WalFile::GetStartPlsn()
{
    return GsAtomicReadU64(&m_startPlsn);
}

uint64 WalFile::GetFlushedPlsn()
{
    return GsAtomicReadU64(&m_flushedPlsn);
}

bool WalFile::SetFlushedPlsn(uint64 flushPlsn)
{
    return WalUtils::TryAtomicSetBiggerU64(&m_flushedPlsn, flushPlsn);
}

uint64 WalFile::GetMaxValidDataOffset()
{
    return GsAtomicReadU64(&m_maxValidDataOffset);
}

void WalFile::SetMaxValidDataOffset(uint64 validDataOffset, bool reset)
{
    if (unlikely(reset)) {
        GsAtomicWriteU64(&m_maxValidDataOffset, validDataOffset);
    } else {
        (void)WalUtils::TryAtomicSetBiggerU64(&m_maxValidDataOffset, validDataOffset);
    }
}

RetStatus WalFile::CreateFile(const FileParameter &filePara)
{
    RetStatus retStatus = m_vfs->CreateFile(m_path, filePara, &m_fd);
    StorageExit1(STORAGE_FUNC_FAIL(retStatus), MODULE_WAL,
                 ErrMsg("Creating wal file failed, filePath(%s), exit gaussdb process", m_path));
    if (!m_onlyForRead) {
        if (unlikely(FileControl(m_fd, SET_FILE_FLUSH_CALLBACK, &m_flushCallBackInfo) != 0)) {
            ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("FileControl set wal file flush callback failed."));
        }
        ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("FileControl set wal file flush callback."));
        if (USE_PAGE_STORE &&
            unlikely(FileControl(m_fd, SET_FILE_ZCOPY_MEMORY_KEY, &m_zeroCopyInfo) != 0)) {
            ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("FileControl set the wal file zcopy memory key failed."));
        }
    }
    return retStatus;
}

void WalFile::Close() noexcept
{
    if (m_fd != nullptr) {
        m_vfs->CloseFile(m_fd);
        m_fd = nullptr;
    }
}

RetStatus WalFile::ReadFileHeaderInfo(WalFileHeaderData *header, uint16 headerLen, DstoreMemoryContext memoryContext)
{
    RetStatus retStatus;
    ssize_t readSize = 0;

    if (m_diorw) {
        WalDioReadAdaptor dioReadAdaptor{memoryContext, WAL_BLCKSZ};
        retStatus = dioReadAdaptor.Read(STATIC_CAST_PTR_TYPE(header, uint8 *), headerLen, this, 0, &readSize);
    } else {
        retStatus = Read(header, headerLen, 0, &readSize);
    }
    if (STORAGE_FUNC_FAIL(retStatus)) {
        ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("Pread wal file %s failed.", m_path));
        return DSTORE_FAIL;
    }
    if (static_cast<uint64>(readSize) != headerLen) {
        ErrLog(DSTORE_WARNING, MODULE_WAL,
               ErrMsg("Wal file %s len(%ld) is not enough for header len.", m_path, readSize));
        return DSTORE_FAIL;
    }
    if (unlikely(header->magicNum != WAL_FILE_HEAD_MAGIC)) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("Wal file %s header magic num invalid.", m_path));
        return DSTORE_FAIL;
    }
    if (unlikely(header->crc != WalFileHeaderData::ComputeHdrCrc(static_cast<uint8 *>(static_cast<void *>(header))))) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("Wal file %s crc invalid.", m_path));
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

uint64 WalFile::GetFirstWalGroupPlsn(DstoreMemoryContext memoryContext)
{
    WalFileHeaderData header{};
    if (STORAGE_FUNC_FAIL(ReadFileHeaderInfo(&header, WAL_FILE_HDR_SIZE, memoryContext))) {
        return INVALID_PLSN;
    }
    return GetFirstGroupStartPlsn(header);
}

uint64 WalFile::GetFirstGroupStartPlsn(const WalFileHeaderData &walFileHeaderData)
{
    return walFileHeaderData.startPlsn + walFileHeaderData.lastRecordRemainLen + WAL_FILE_HDR_SIZE;
}

RetStatus WalFile::PwriteAsync(const void *buf, uint64 count, int64 offset, const AsyncIoContext *aioContext)
{
    OpenFileIfNot();
    StorageStat *stat = g_storageInstance->GetStat();
    stat->m_reportWaitEvent(
        static_cast<uint32_t>(GsStatWaitEvent::WAIT_EVENT_WALFILE_WRITE_ASYNC));
    RetStatus retStatus = m_vfs->PwriteAsync(m_fd, buf, count, offset, aioContext);
    stat->m_reportWaitEvent(static_cast<uint32_t>(OPTUTIL_GSSTAT_WAIT_EVENT_END));
    StorageExit1(STORAGE_FUNC_FAIL(retStatus), MODULE_WAL,
                 ErrMsg("WalFile PwriteAsync failure, fileName(%s), exit gaussdb process.", m_path));
    return DSTORE_SUCC;
}

RetStatus WalFile::PwriteSync(const void *buf, uint64 count, int64 offset)
{
    OpenFileIfNot();
    RetStatus ret = m_vfs->PwriteSync(GetFileDescriptor(), buf, count, offset);
    StorageReleasePanic(STORAGE_FUNC_FAIL(ret), MODULE_FRAMEWORK,
                        ErrMsg("WalFile %s PwriteSync failure.", m_path));
    return DSTORE_SUCC;
}

void WalFile::OpenFileIfNot()
{
    if (m_fd == nullptr) {
        RetStatus retStatus = m_vfs->OpenFile(m_path, DSTORE_WAL_FILE_OPEN_FLAG, &m_fd);
        StorageExit1(STORAGE_FUNC_FAIL(retStatus), MODULE_WAL,
                     ErrMsg("Open wal file failed, filePath(%s), exit gaussdb process.", m_path));
        if (!m_onlyForRead) {
            if (unlikely(FileControl(m_fd, SET_FILE_FLUSH_CALLBACK, &m_flushCallBackInfo) != 0)) {
                storage_set_error(WAL_ERROR_UNREACHABLE_CODE);
                ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("FileControl set wal file flush callback failed."));
            }
            ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("FileControl set wal file flush callback."));
            if (USE_PAGE_STORE &&
                unlikely(FileControl(m_fd, SET_FILE_ZCOPY_MEMORY_KEY, &m_zeroCopyInfo) != 0)) {
                storage_set_error(WAL_ERROR_UNREACHABLE_CODE);
                ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("FileControl set the wal file zcopy memory key failed."));
            }
        }
        ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("Open wal file succ, filePath(%s).", m_path));
    }
}

bool WalFile::IsDioRw()
{
    return m_diorw;
}

RetStatus WalFile::Read(void *buf, uint64 count, int64 offset, int64 *readSize)
{
    OpenFileIfNot();
    StorageStat *stat = g_storageInstance->GetStat();
    stat->m_reportWaitEvent(
        static_cast<uint32_t>(GsStatWaitEvent::WAIT_EVENT_WALFILE_READ));
    RetStatus retStatus = m_vfs->Pread(m_fd, buf, count, offset, readSize);
    stat->m_reportWaitEvent(static_cast<uint32_t>(OPTUTIL_GSSTAT_WAIT_EVENT_END));
    if (STORAGE_FUNC_FAIL(retStatus)) {
        ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK, ErrMsg("Wal file %s Read failure.", m_path));
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus WalFile::Fsync()
{
    OpenFileIfNot();
    g_storageInstance->GetStat()->m_reportWaitEvent(
        static_cast<uint32_t>(GsStatWaitEvent::WAIT_EVENT_WALFILE_FSYNC));
    RetStatus retStatus = m_vfs->Fsync(m_fd);
    g_storageInstance->GetStat()->m_reportWaitEvent(static_cast<uint32_t>(OPTUTIL_GSSTAT_WAIT_EVENT_END));
    if (STORAGE_FUNC_FAIL(retStatus)) {
        ErrLog(DSTORE_ERROR,
               MODULE_FRAMEWORK, ErrMsg("Wal file %s Fsync failure.", m_path));
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus WalFile::Truncate(int64 length)
{
    OpenFileIfNot();
    constexpr uint32 retryTimes = 1000;
    RetStatus retStatus;
    for (uint16 i = 0; i < retryTimes; i++) {
        retStatus = m_vfs->Truncate(m_fd, length);
        if (STORAGE_FUNC_FAIL(retStatus)) {
            ErrLog(DSTORE_WARNING, MODULE_WAL, ErrMsg("Truncate wal file %s failed. retry times:%d", m_path, i));
        } else {
            break;
        }
        std::chrono::milliseconds dura(1);
        std::this_thread::sleep_for(dura);
    }

    StorageExit1(STORAGE_FUNC_FAIL(retStatus), MODULE_WAL,
                 ErrMsg("Truncate wal file %s final failed, exit gaussdb process.", m_path));
    SetMaxValidDataOffset(length, true);
    return DSTORE_SUCC;
}

RetStatus WalFile::RenameFile(const char *destPathName)
{
    m_vfs->CloseFile(m_fd);
    RetStatus retStatus = m_vfs->RenameFile(m_path, destPathName);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        ErrLog(DSTORE_ERROR, MODULE_FRAMEWORK,
               ErrMsg("Wal RenameFile failure, srcPathName %s, destPathName %s.", m_path, destPathName));
        return DSTORE_FAIL;
    }
    int result = sprintf_s(m_path, MAXPGPATH, "%s", destPathName);
    storage_securec_check_ss(result)
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("Wal RenameFile succ, srcPathName %s, destPathName %s.",
        m_path, destPathName));
    m_fd = nullptr;
    OpenFileIfNot();
    return DSTORE_SUCC;
}

RetStatus WalFile::RemoveFile()
{
    m_vfs->CloseFile(m_fd);
    m_fd = nullptr;
    RetStatus retStatus = m_vfs->RemoveFile(m_path);
    if (STORAGE_FUNC_FAIL(retStatus)) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("Wal RemoveFile failure, srcPathName %s.", m_path));
        return DSTORE_FAIL;
    }
    SetMaxValidDataOffset(0, true);
    return DSTORE_SUCC;
}

RetStatus WalFile::PreadAsync(void *buf, uint64_t count, int64_t offset, const AsyncIoContext *aioContext)
{
    OpenFileIfNot();
    g_storageInstance->GetStat()->m_reportWaitEvent(
        static_cast<uint32_t>(GsStatWaitEvent::WAIT_EVENT_WALFILE_READ_ASYNC));
    RetStatus retStatus = m_vfs->PreadAsync(m_fd, buf, count, offset, aioContext);
    g_storageInstance->GetStat()->m_reportWaitEvent(static_cast<uint32_t>(OPTUTIL_GSSTAT_WAIT_EVENT_END));
    StorageExit1(STORAGE_FUNC_FAIL(retStatus), MODULE_WAL,
                 ErrMsg("PreadAsync wal file failure, fileName %s, exit gaussdb process", m_path));
    return DSTORE_SUCC;
}

void WalFile::ResetState(uint64 startPlsn)
{
    m_startPlsn = startPlsn;
    m_flushedPlsn = 0;
    m_next = nullptr;
}

static CheckpointMgr *TryGetCheckpointMgr(PdbId pdbId)
{
    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
    if (unlikely(pdb == nullptr || !pdb->IsInit())) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("get ChekckpointMgr failed for find pdb:%d failed", pdbId));
        return nullptr;
    }
    CheckpointMgr *ckpMgr = pdb->GetCheckpointMgr();
    if (ckpMgr == nullptr) {
        ErrLog(DSTORE_WARNING, MODULE_WAL, ErrMsg("get ChekckpointMgr of pdb:%d fail", pdbId));
    }
    return ckpMgr;
}

WalFileManager::WalFileManager(DstoreMemoryContext memoryContext)
    : m_headFile(nullptr),
      m_writingFile(nullptr),
      m_tailFile(nullptr),
      m_recycleThread(nullptr),
      m_stopBgRecycleThread(false),
      m_pauseRecycleStart(false),
      m_pauseRecycleFinish(false),
      m_walFileIsDropping(false),
      m_memoryContext(memoryContext),
      m_workerRunning(false),
      m_numPauseRequests(0),
      m_recoveryRecycleFinish(false),
      m_dioRw(false),
      m_pauseWalFileRecycle(false),
      m_pauseWalFileRecycleCnt(0)
{
    m_initWalFilesPara = {};
    RWLockInit(&m_fileLock, RWLOCK_PREFER_READER_NP);
}

WalFileManager::~WalFileManager()
{
    Destroy();
}

RetStatus WalFileManager::InitWalFiles(const InitWalFilesPara &para, bool dioRw)
{
    if (unlikely(m_memoryContext == nullptr || strlen(para.dir) > MAXPGPATH ||
                 para.vfs == nullptr || para.walId == INVALID_WAL_ID || para.initWalFileCount < 1 ||
                 para.walFileSize % WAL_BLCKSZ != 0 || para.walFileSize <= WAL_FILE_HDR_SIZE ||
                 para.maxWalFileCount <= 1)) {
        ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("WalFileManager InitWalFiles invalid para"));
    }
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("WalFileManager InitWalFiles para pdbId:%u, walId:%lu, initWalFileCount:%u, "
        "maxWalFileCount:%u, maxFreeWalFileCount:%u, walFileSize:%lu, startPlsn:%lu, onlyForRead:%s, isForStandBy:%s",
        para.pdbId, para.walId, para.initWalFileCount, para.maxWalFileCount, para.maxFreeWalFileCount, para.walFileSize,
        para.startPlsn, para.onlyForRead ? "true" : "false", para.isForStandBy ? "true" : "false"));
    /* Step 1: Get startPlsn from para (normally equal to disk checkpoint recovery plsn), and get wal file
     * name "dir + walId + startPLsn", judge the wal file if exist by name.
     * starPlsn + walFileSize to get next wal file name and update m_tailFile until the file do not exist */
    m_initWalFilesPara = para;
    m_dioRw = dioRw;
    uint32 cnt = 0;
    uint64 firstStartPlsn = 0;
    char **fileNames = GetWalFilesName({para.pdbId, para.walId, para.walFileSize, para.startPlsn, false},
                                       cnt, firstStartPlsn);

    /* when recovery, it's possible that walfiles are already deleted
     * when standby redo, it's possible that walfiles are not received yet */
    if (cnt == 0 && para.startPlsn != INVALID_PLSN) {
        ErrLog((para.onlyForRead || para.isForStandBy) ? DSTORE_ERROR : DSTORE_PANIC, MODULE_WAL,
               ErrMsg("WalFileManager init startPlsn %lu, but the wal file not exist", para.startPlsn));
    }
    if (cnt > 0) {
        /* Initialize existing WalFiles recorded in control file */
        for (uint32 i = 0; i < cnt; i++) {
            WalFile *walFile = CreateWalFileObject(firstStartPlsn, fileNames[i]);
            DstorePfree(fileNames[i]);
            AppendWalFile(walFile);
            firstStartPlsn += para.walFileSize;
        }
        DstorePfree(fileNames);
    }

    /* Init the rest of WalFiles required here */
    int newInitWalFileCnt = para.initWalFileCount > cnt ? static_cast<int>(para.initWalFileCount - cnt) : 0;
    if (!para.onlyForRead) {
        CreateWalFiles(firstStartPlsn, newInitWalFileCnt);
        m_recycleThread = new std::thread(&WalFileManager::RecycleWalFileWorkerMain, this, false);
    }
    return DSTORE_SUCC;
}

void WalFileManager::CloseAllWalFiles()
{
    WalFile *walFile = m_headFile;
    while (walFile) {
        WalFile *next = GetNextWalFile(walFile, false);
        walFile->Close();
        walFile = next;
    }
}

WalFile *WalFileManager::GetWalFileByPlsn(uint64 plsn)
{
    /* Step 1: iterate the wal file list from m_headFile, find the wal file, its plsn range include target plsn */
    WalFile *walFile = m_headFile;
    while (walFile) {
        uint64 fileStartPlsn = walFile->GetStartPlsn();
        if (plsn >= fileStartPlsn && plsn < fileStartPlsn + m_initWalFilesPara.walFileSize) {
            break;
        }
        walFile = GetNextWalFile(walFile, false);
    }
    return walFile;
}

bool WalFileManager::CreateWalStreamFileObj(uint64 plsn, uint64 walFileSize, PdbId pdbId, WalId walId)
{
    uint32 walTimeLineId = 0;
    char fileName[MAXPGPATH];
    uint64 walFileStartPlsn = plsn - plsn % walFileSize;
    StorageReleasePanic(m_tailFile != nullptr &&
        m_tailFile->GetStartPlsn() + m_initWalFilesPara.walFileSize != walFileStartPlsn,
        MODULE_WAL,
        ErrMsg("WalFile Log inconsecutive, last plsn %lu, cur plsn %lu!", m_tailFile->GetStartPlsn(),
        walFileStartPlsn));
    char *pdbWalPath = StorageInstance::GetPdbWalPath(g_storageInstance->GetGuc()->dataDir, pdbId);
    StorageReleasePanic(pdbWalPath == nullptr, MODULE_WAL, ErrMsg("alloc memory for pdbWalPath fail!"));
    StoragePdb *pdb = g_storageInstance->GetPdb(pdbId);
    StorageReleasePanic(pdb == nullptr, MODULE_WAL, ErrMsg("CreateWalStreamFileObj get pdb failed, pdbId(%u).", pdbId));
    VFSAdapter *vfs = pdb->GetVFS();
    int result =
        sprintf_s(fileName, MAXPGPATH, "%s/%08hX_%08X_%016llX", pdbWalPath, walId, walTimeLineId, walFileStartPlsn);
    storage_securec_check_ss(result);
    ErrLog(DSTORE_LOG, MODULE_WAL,
        ErrMsg("[PDB:%hhu WAL:%lu]CreateWalStreamFileObj startPlsn %lu", pdbId, walId, walFileStartPlsn));
    if (vfs->FileExists(fileName)) {
        WalFile *walFile = CreateWalFileObject(walFileStartPlsn, fileName);
        AppendWalFile(walFile);
        DstorePfree(pdbWalPath);
        return true;
    }
    DstorePfree(pdbWalPath);
    return false;
}

WalFile *WalFileManager::GetHeadWalFile()
{
    return m_headFile;
}

WalFile *WalFileManager::GetTailWalFile()
{
    return m_tailFile;
}

WalFile *WalFileManager::GetNextWalFile(WalFile *walFile, bool needWait)
{
    /* Step 1: Get walFile's next file, if used for write, needWait one free wal file */
    constexpr long sleepTime = 100;
    WalFile *nextFile = walFile->GetNext();
    if (!needWait) {
        return nextFile;
    }
    uint64 retryTimes = 0;
    while (nextFile == nullptr) {
        nextFile = walFile->GetNext();
        GaussUsleep(sleepTime);
        if (retryTimes++ % WAIT_COUNT_FOR_REPORT_WARNING == 0) {
            ErrLog(DSTORE_LOG, MODULE_WAL,
                ErrMsg("[PDB:%u WAL:%lu]GetNextWalFile wait for next file with start plsn %lu",
                m_initWalFilesPara.pdbId, m_initWalFilesPara.walId,
                walFile->GetStartPlsn() + m_initWalFilesPara.walFileSize));
        }
    }
    return nextFile;
}

RetStatus WalFileManager::RecycleWalFileForDropping()
{
    StoragePdb *pdb = g_storageInstance->GetPdb(m_initWalFilesPara.pdbId);
    StorageReleasePanic(pdb == nullptr, MODULE_WAL,
                        ErrMsg("RecycleWalFileForDropping get pdb failed, pdbId(%u).", m_initWalFilesPara.pdbId));
    ControlFile *controlFile = pdb->GetControlFile();
    ControlWalStreamPageItemData *walStreamInfo = nullptr;
    if (STORAGE_FUNC_FAIL(controlFile->GetWalStreamInfo(m_initWalFilesPara.walId, &walStreamInfo))) {
        ErrLog(DSTORE_WARNING, MODULE_BUFFER,
            ErrMsg("[Wal:%lu]Can't get wal stream info from control file.", m_initWalFilesPara.walId));
        if (STORAGE_FUNC_SUCC(RemoveAllFiles())) {
            m_recoveryRecycleFinish = true;
            return DSTORE_SUCC;
        }
        return DSTORE_FAIL;
    }

    if (unlikely(walStreamInfo->streamState != static_cast<uint8>(WalStreamState::RECOVERY_DROPPING)) &&
        STORAGE_FUNC_FAIL(controlFile->
        UpdateWalStreamState(m_initWalFilesPara.walId, static_cast<uint8>(WalStreamState::RECOVERY_DROPPING)))) {
        ErrLog(DSTORE_ERROR, MODULE_WAL,
            ErrMsg("[Wal:%lu]Mark wal stream state to RECOVERY_DROPPING failed", m_initWalFilesPara.walId));
    }
    Timestamp now = static_cast<Timestamp>(time(nullptr));
    Timestamp elapsedTime = now - walStreamInfo->lastWalCheckpoint.time;
    if (elapsedTime > g_storageInstance->GetGuc()->walKeepTimeAfterRecovery * SECONDS_ONE_MINUTE &&
        STORAGE_FUNC_SUCC(RemoveAllFiles()) &&
        STORAGE_FUNC_SUCC(controlFile->DeleteWalStream(m_initWalFilesPara.walId))) {
        ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("[Wal:%lu]RecycleWalFileForDropping complete with "
            "lastWalCheckpoint.time:%ld, walKeepTimeAfterRecovery: %d", m_initWalFilesPara.walId,
            walStreamInfo->lastWalCheckpoint.time, g_storageInstance->GetGuc()->walKeepTimeAfterRecovery));
        m_recoveryRecycleFinish = true;
        return DSTORE_SUCC;
    }
    controlFile->FreeWalStreamsInfo(walStreamInfo);
    return DSTORE_FAIL;
}

void WalFileManager::RecycleWalFileWorkerMain(bool isDropping)
{
    if (g_defaultPdbId == PDB_ROOT_ID && IsTemplate(m_initWalFilesPara.pdbId)) {
        return;
    }
    InitSignalMask();
    (void)g_storageInstance->CreateThreadAndRegister(m_initWalFilesPara.pdbId, false, "RecWalWorker", false,
                                                     ThreadMemoryLevel::THREADMEM_HIGH_PRIORITY);

    StoragePdb *pdb = g_storageInstance->GetPdb(m_initWalFilesPara.pdbId);
    if (STORAGE_VAR_NULL(pdb)) {
        ErrLog(DSTORE_ERROR, MODULE_WAL,
            ErrMsg("[RecycleWalFileWorkerMain] Pdb object is null, pdbId: %u.", m_initWalFilesPara.pdbId));
        return;
    }
    uint64 standbyWaitCount = 0;
    /* standby pdb's walFile is created by primary, wait files ready before proceeding */
    while (pdb->GetPdbRoleMode() == PdbRoleMode::PDB_STANDBY && (m_headFile == nullptr || m_tailFile == nullptr) &&
        !m_stopBgRecycleThread.load(std::memory_order_relaxed)) {
        GaussUsleep(WAIT_PAUSE_RECYCLE_MICROSEC);
        if (++standbyWaitCount % WAIT_COUNT_FOR_REPORT_WARNING == 0) {
            ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("Standby pdb's recycle thread waiting for wal files, "
                "pdbName: %s, walId: %lu.", pdb->GetPdbName(), m_initWalFilesPara.walId));
        }
    }
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("RecycleWalFileWorker starts, pdbName: %s, walId: %lu.",
        pdb->GetPdbName(), m_initWalFilesPara.walId));

    WatchDogMgr *watchdogMgr = pdb->GetWatchDogMgr();
    if (watchdogMgr != nullptr) {
        m_watchdogHandle = watchdogMgr->Register(
            WatchDogThreadType::WAL_FILE_RECYCLE, static_cast<uint32>(m_initWalFilesPara.walId), "WalRecycler");
        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::RUNNING);
    } else {
        ErrLog(DSTORE_LOG, MODULE_WATCHDOG,
            ErrMsg("WatchDogMgr is null when registering WalRecycler, pdbId=%u.",
                   m_initWalFilesPara.pdbId));
    }

    constexpr long sleepTimeInMs = 10;
    while (!m_stopBgRecycleThread.load(std::memory_order_relaxed)) {
        PauseRecycleIfNeed();

        if (isDropping) {
            if (RecycleWalFileForDropping() == DSTORE_FAIL) {
                GaussUsleep(STORAGE_USECS_PER_SEC);
                continue;
            } else {
                break;
            }
        }

        /* Step 1: wait m_headFile recyclable */
        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::SLEEPING);
        while (!WalFileRecyclable(m_headFile)) {
            /* Step 1.1: N = maxFreeWalFileCount - curFreeWalFileCount if N > 0, create N wal files and append */
            if (pdb->GetPdbRoleMode() != PdbRoleMode::PDB_STANDBY &&
                m_initWalFilesPara.maxFreeWalFileCount > GetFreeWalFileCount() &&
                GetAllWalFileCount() < m_initWalFilesPara.maxWalFileCount && !m_initWalFilesPara.onlyForRead) {
                CreateNextWalFile();
            }
            if (m_stopBgRecycleThread.load(std::memory_order_relaxed)) {
                goto RECYCLE_WAL_END;
            }
            PauseRecycleIfNeed();
            /* Step 1.2 sleep for 10ms in total or wait to be notified */
            for (int i = 0; i < sleepTimeInMs && !m_stopBgRecycleThread.load(std::memory_order_relaxed); i++) {
                GaussUsleep(STORAGE_USECS_PER_MSEC);
            }
        }
        WatchDogMgr::SetRunState(m_watchdogHandle, ThreadRunState::RUNNING);
        /* Step 2: N = maxFreeWalFileCount - curFreeWalFileCount
         * if N > 0, rename and append this wal file; else, remove it */
        if (GetFreeWalFileCount() < m_initWalFilesPara.maxFreeWalFileCount) {
            RecycleHeadFile();
        } else if (GetAllWalFileCount() > m_initWalFilesPara.initWalFileCount) {
            RemoveHeadFile();
        }
        WatchDogMgr::TouchHeartbeat(m_watchdogHandle);
    }

RECYCLE_WAL_END:
    if (watchdogMgr != nullptr) {
        watchdogMgr->Unregister(m_watchdogHandle);
        m_watchdogHandle = nullptr;
    }
    g_storageInstance->UnregisterThread();
}

void WalFileManager::CreateNextWalFile()
{
    /* Step 1: Get new file's startPlsn = tail file's startPlsn + walFileSize */
    uint64 newStartPlsn = m_tailFile != nullptr
            ? m_tailFile->GetStartPlsn() + m_initWalFilesPara.walFileSize : 0;

    /* Step 2: Create next file now */
    CreateWalFiles(newStartPlsn, 1);
}

WalFile *WalFileManager::CreateOnePendingWalFile(uint64 startPlsn)
{
    WalFile *walFile = CreateWalFileObject(startPlsn, nullptr);
    StorageReleasePanic(walFile == nullptr, MODULE_WAL, ErrMsg("CreateOnePendingWalFile get walFile failed."));
    if (STORAGE_FUNC_FAIL(walFile->CreateFile(m_initWalFilesPara.filePara))) {
        StorageExit1(true, MODULE_WAL,
                     ErrMsg("WalFileManager InitWalFiles create wal file %s fail, exit gaussdb process",
                            walFile->GetFileName()));
    }
#ifndef UT
    PrintWalFilesInfo(true, walFile->GetFileName());
#endif
    return walFile;
}

void WalFileManager::CreateWalFiles(uint64 startPlsn, uint16 fileCount)
{
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("Create %u wal files start for walstream %lu pdb %u, wal file size %lu.",
        fileCount, m_initWalFilesPara.walId, m_initWalFilesPara.pdbId, m_initWalFilesPara.walFileSize));
    std::future<WalFile *> *walFiles = static_cast<std::future<WalFile *> *>(
        DstorePalloc(fileCount * sizeof(std::future<WalFile *>)));
    while (STORAGE_VAR_NULL(walFiles)) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("CreateWalFiles alloc memory fail, retry it"));
        GaussUsleep(WAL_WAIT_MEMORY_AVAILABLE_TIME);
        walFiles = static_cast<std::future<WalFile *> *>(DstorePalloc(fileCount * sizeof(std::future<WalFile *>)));
    }
    for (uint32 i = 0; i < fileCount; i++) {
        new (&walFiles[i]) std::future<WalFile *>();
    }

    uint64 curStartPlsn = startPlsn;
    for (uint32 i = 0; i < fileCount; i++) {
        walFiles[i] = std::async(std::launch::async,
            [&](WalFileManager *walFileManager, uint64_t thisStartPlsn, PdbId pdbId) -> WalFile* {
            InitSignalMask();
            (void)g_storageInstance->CreateThreadAndRegister(pdbId, false, "WalFileAsync", false);
            WalFile *file = walFileManager->CreateOnePendingWalFile(thisStartPlsn);
            g_storageInstance->UnregisterThread();
            return file; /* return from lambda expression */
        }, this, curStartPlsn, m_initWalFilesPara.pdbId);

        curStartPlsn += m_initWalFilesPara.walFileSize;
    }

    for (uint32 i = 0; i < fileCount; i++) {
        AppendWalFile(walFiles[i].get());
        (&walFiles[i])->~future<WalFile *>();
    }
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("Create %u wal files succ for walstream %lu pdb %u, wal file size %lu, sum of"
        " wal files is %u.", fileCount, m_initWalFilesPara.walId, m_initWalFilesPara.pdbId,
        m_initWalFilesPara.walFileSize, GetAllWalFileCount()));
    DstorePfree(walFiles);
}

WalFile *WalFileManager::CreateWalFileObject(uint64 startPlsn, char *fileName)
{
    uint32 walTimeLineId = 0;
    char path[MAXPGPATH];
    if (fileName == nullptr) {
        int result = sprintf_s(path, MAXPGPATH, "%s/%08hX_%08X_%016llX", m_initWalFilesPara.dir,
                               m_initWalFilesPara.walId, walTimeLineId, startPlsn);
        storage_securec_check_ss(result);
        fileName = path;
    }

    WalFile *walFile = DstoreNew(m_memoryContext)
        WalFile({m_initWalFilesPara.vfs, fileName, m_initWalFilesPara.flushCallBackInfo,
                m_initWalFilesPara.zeroCopyInfo, startPlsn, m_initWalFilesPara.onlyForRead, m_dioRw});
    if (STORAGE_VAR_NULL(walFile)) {
        ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("WalFileManager InitWalFiles alloc WalFile fail"));
    }
    return walFile;
}

RetStatus WalFileManager::RecycleHeadFile()
{
    if (STORAGE_FUNC_FAIL(m_headFile->Truncate(0))) {
        return DSTORE_FAIL;
    }
    /* Step 1: Get new file's startPlsn = tail file's startPlsn + walFileSize, and rename the file and create
     * new wal file object */
    uint64 newStartPlsn = m_tailFile->GetStartPlsn() + m_initWalFilesPara.walFileSize;
    char newFileName[MAXPGPATH];
    int result = sprintf_s(newFileName, MAXPGPATH, "%s/%08hX_%08X_%016llX", m_initWalFilesPara.dir,
                           m_initWalFilesPara.walId, 0, newStartPlsn);
    storage_securec_check_ss(result);
    if (STORAGE_FUNC_FAIL(m_headFile->RenameFile(newFileName))) {
        return DSTORE_FAIL;
    }
    /* Step 2: Append new wal file to tail */
    GetFileUniqueLock();
    WalFile *nextHead = m_headFile->GetNext();
    m_headFile->ResetState(newStartPlsn);
    AppendWalFile(m_headFile);
    m_headFile = nextHead != nullptr ? nextHead : m_tailFile;
    ReleaseUniqueFileLock();
#ifndef UT
    PrintWalFilesInfo(false, m_headFile->GetFileName());
#endif
    return DSTORE_SUCC;
}

void WalFileManager::RemoveHeadFile()
{
    /* Step 1: remove the wal file */
    m_headFile->RemoveFile();
    WalFile *nextHead = m_headFile->GetNext();
    GetFileUniqueLock();
    delete m_headFile;
    m_headFile = nextHead;
    ReleaseUniqueFileLock();
#ifndef UT
    PrintWalFilesInfo(false, m_headFile->GetFileName());
#endif
}

RetStatus WalFileManager::RemoveAllFiles()
{
    GetFileUniqueLock();
    ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("[Wal:%lu]Start delete all wal files", m_initWalFilesPara.walId));
    int file_count = 0;
    while (m_headFile != nullptr) {
        if (STORAGE_FUNC_FAIL(m_headFile->RemoveFile())) {
            ReleaseUniqueFileLock();
            return DSTORE_FAIL;
        }
        WalFile *nextHead = m_headFile->GetNext();
        ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("[Wal:%lu]Delete wal file %s succ, headFile start plsn %lu",
            m_initWalFilesPara.walId, m_headFile->GetFileName(), m_headFile->GetStartPlsn()));
        delete m_headFile;
        m_headFile = nextHead;
        file_count++;
    }
    ReleaseUniqueFileLock();
    ErrLog(DSTORE_LOG, MODULE_WAL,
        ErrMsg("[Wal:%lu]Delete all wal files successfully, file count %d", m_initWalFilesPara.walId, file_count));
    return DSTORE_SUCC;
}

void WalFileManager::AppendWalFile(WalFile *walFile)
{
    /* Step 1: update m_tailFile->next = walFile and m_tailFile = walFile,
     * use atomic to support concurrency */
    if (m_tailFile == nullptr || m_headFile == nullptr) {
        m_tailFile = walFile;
        m_headFile = walFile;
        return;
    }
    m_tailFile->SetNext(walFile);

    /* m_tailFile is only used by RecycleWalFileWorker, no concurrency */
    m_tailFile = walFile;
}

uint64 WalFileManager::GetRecyclablePlsn() const
{
    /* get disk checkpoint */
    CheckpointMgr *ckpMgr = TryGetCheckpointMgr(m_initWalFilesPara.pdbId);
    if (unlikely(ckpMgr == nullptr)) {
        return INVALID_PLSN;
    }
    StoragePdb *pdb = g_storageInstance->GetPdb(m_initWalFilesPara.pdbId);
    if (unlikely(pdb == nullptr)) {
        return INVALID_PLSN;
    }

    WalCheckPoint walCheckpoint;
    if (STORAGE_FUNC_FAIL(ckpMgr->GetWalCheckpoint(m_initWalFilesPara.walId, walCheckpoint))) {
        return INVALID_PLSN;
    }

    uint64 minRecyclablePlsn = walCheckpoint.diskRecoveryPlsn;
    return minRecyclablePlsn;
}

bool WalFileManager::WalFileRecyclable(WalFile *walFile)
{
    /* when build standby, pause wal file recycle in primary */
    if (GetPauseWalFileRecycleFlag()) {
        if (++m_pauseWalFileRecycleCnt % WAIT_COUNT_FOR_REPORT_WARNING == 0) {
            ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg("Primary pdb's walFile recycle is paused when creating standby, "
                "pdbId: %u, walId: %lu.", m_initWalFilesPara.pdbId, m_initWalFilesPara.walId));
        }
        return false;
    }
    m_pauseWalFileRecycleCnt = 0;
    /* 1. If current wal file count less than walKeepSegments, there is no need to recycle.
     * 2. If GetRecyclablePlsn < walFile->startPlsn + walFileSize, there is no need to recycle.
     */
    return ((GetAllWalFileCount() > g_storageInstance->GetGuc()->walKeepSegments) &&
        (GetRecyclablePlsn() >= walFile->GetStartPlsn() + m_initWalFilesPara.walFileSize));
}

bool WalFileManager::WalFileWithPlsnRecyclable(uint64 plsn)
{
    return WalFileRecyclable(GetWalFileByPlsn(plsn));
}

void WalFileManager::RequestPauseWalFileManager()
{
    /* get the lock, add a pause request, wait for wal file mgr to pause if it's not pause */
    ErrLog(DSTORE_LOG, MODULE_WAL,
        ErrMsg("Start to pause WalFileManager worker thread, current number of pause request = %d",
            m_numPauseRequests));
    std::unique_lock<std::mutex> lock(m_workerMtx);
    StorageReleasePanic(m_numPauseRequests < 0, MODULE_WAL, ErrMsg("Current number of pause request is abnormal."));
    m_numPauseRequests++;
    while (m_workerRunning) {
        m_workerSleepCv.wait(lock);
    }
    ErrLog(DSTORE_LOG, MODULE_WAL,
        ErrMsg("WalFileManager worker thread paused, current number of pause request = %d",
            m_numPauseRequests));
}

void WalFileManager::RequestResumeWalFileManager()
{
    /* get the lock, remove a pause request, last pause request removed wakes wal file mgr */
    ErrLog(DSTORE_LOG, MODULE_WAL,
        ErrMsg("Start to resume WalFileManager worker thread, current number of pause request = %d",
            m_numPauseRequests));
    std::unique_lock<std::mutex> lock(m_workerMtx);
    StorageReleasePanic(m_numPauseRequests <= 0, MODULE_WAL, ErrMsg("Current number of pause request is abnormal."));
    m_numPauseRequests--;
    if (m_numPauseRequests == 0) {
        m_wakeWorkerCv.notify_all();
    }
    ErrLog(DSTORE_LOG, MODULE_WAL,
        ErrMsg("WalFileManager worker thread resumed, current number of pause request = %d",
            m_numPauseRequests));
}

void WalFileManager::PauseRecycleIfNeed()
{
    /*
     * get the lock, sleep when m_numPauseRequests is greater than 0,
     * set flag marking WalFileManager as asleep and notify other threads that waiting for the WalFileManager to sleep
     */
    std::unique_lock<std::mutex> lock(m_workerMtx);

    while (m_numPauseRequests > 0) {
        m_workerRunning = false;
        m_workerSleepCv.notify_all();
        (void)m_wakeWorkerCv.wait_for(lock, std::chrono::milliseconds(WAIT_PAUSE_REQUEST_TIMEOUT_MILLISEC));
    }

    m_workerRunning = true;
    lock.unlock();
    lock.release();

    uint64 loopCount = 0;
    while (m_pauseRecycleStart.load(std::memory_order_acquire) &&
        !m_stopBgRecycleThread.load(std::memory_order_relaxed)) {
        m_pauseRecycleFinish.store(true, std::memory_order_release);
        GaussUsleep(WAIT_PAUSE_RECYCLE_MICROSEC);
        if (++loopCount % WAIT_COUNT_FOR_REPORT_WARNING == 0) {
            ErrLog(DSTORE_WARNING, MODULE_WAL, ErrMsg("Wal recycle paused for walId: %lu.", m_initWalFilesPara.walId));
        }
    }
}

char **WalFileManager::GetWalFilesName(GetWalFilesNamePara para, uint32 &cnt, uint64 &firstStartPlsn)
{
    uint32 walTimeLineId = 0;
    uint64 walFileStartPlsn = para.diskRecoveryPlsn - para.diskRecoveryPlsn % para.walFileSize;
    uint64 nextFileStartPlsn = walFileStartPlsn;
    char fileName[MAXPGPATH];
    bool fileExist = true;
    uint32 existFileNum = 0;
    cnt = 0;
    char *pdbWalPath = StorageInstance::GetPdbWalPath(g_storageInstance->GetGuc()->dataDir, para.pdbId);
    if (STORAGE_VAR_NULL(g_storageInstance->GetPdb(para.pdbId))) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("GetWalFilesName get pdb failed, pdbId(%u).", para.pdbId));
        DstorePfreeExt(pdbWalPath);
        return nullptr;
    }
    VFSAdapter *vfs = g_storageInstance->GetPdb(para.pdbId)->GetVFS();
    while (fileExist) {
        int result = sprintf_s(fileName, MAXPGPATH, "%s/%08hX_%08X_%016llX", pdbWalPath, para.walId, walTimeLineId,
                               nextFileStartPlsn);
        storage_securec_check_ss(result);
        fileExist = vfs->FileExists(fileName);
        if (fileExist) {
            existFileNum++;
            nextFileStartPlsn += para.walFileSize;
        }
    }
    nextFileStartPlsn = walFileStartPlsn;
    while (!para.onlyAfter && nextFileStartPlsn >= para.walFileSize) {
        nextFileStartPlsn -= para.walFileSize;
        int result = sprintf_s(fileName, MAXPGPATH, "%s/%08hX_%08X_%016llX", pdbWalPath, para.walId, walTimeLineId,
                               nextFileStartPlsn);
        storage_securec_check_ss(result);
        fileExist = vfs->FileExists(fileName);
        if (fileExist) {
            existFileNum++;
            walFileStartPlsn = nextFileStartPlsn;
        } else {
            break;
        }
    }
    if (existFileNum == 0) {
        DstorePfreeExt(pdbWalPath);
        return nullptr;
    }
    char **res = static_cast<char **>(DstorePalloc(existFileNum * sizeof(char *)));
    while (STORAGE_VAR_NULL(res)) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("WalFileManager InitWalFiles alloc file name array fail, retry it"));
        GaussUsleep(WAL_WAIT_MEMORY_AVAILABLE_TIME);
        res = static_cast<char **>(DstorePalloc(existFileNum * sizeof(char *)));
    }
    firstStartPlsn = walFileStartPlsn;
    for (uint32 i = 0; i < existFileNum; i++) {
        res[i] = static_cast<char *>(DstorePalloc(sizeof(char) * MAXPGPATH));
        while (STORAGE_VAR_NULL(res[i])) {
            ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("WalFileManager InitWalFiles alloc file name fail, retry it"));
            GaussUsleep(WAL_WAIT_MEMORY_AVAILABLE_TIME);
            res[i] = static_cast<char *>(DstorePalloc(sizeof(char) * MAXPGPATH));
        }
        int result = sprintf_s(res[i], MAXPGPATH, "%s/%08hX_%08X_%016llX", pdbWalPath, para.walId, walTimeLineId,
                               walFileStartPlsn);
        storage_securec_check_ss(result);
        walFileStartPlsn += para.walFileSize;
    }
    cnt = existFileNum;
    DstorePfree(pdbWalPath);
    return res;
}

WalFile *WalFileManager::GetWalFileByFileDescriptor(FileDescriptor *fd)
{
    WalFile *walFile = m_headFile;
    while (walFile) {
        if (fd == walFile->GetFileDescriptor()) {
            break;
        }
        walFile = GetNextWalFile(walFile, false);
    }
    return walFile;
}

void WalFileManager::Destroy()
{
    StopRecycleWalFileWorker();
    WalFile *walFile = m_headFile;
    while (walFile) {
        WalFile *next = GetNextWalFile(walFile, false);
        delete walFile;
        walFile = next;
    }
    m_headFile = m_tailFile = nullptr;
    RWLockDestroy(&m_fileLock);
}

void WalFileManager::PauseRecycle()
{
    m_pauseRecycleStart.store(true, std::memory_order_release);
    while (!m_pauseRecycleFinish.load(std::memory_order_acquire)) {
        GaussUsleep(WAIT_PAUSE_RECYCLE_MICROSEC);
    }
}

void WalFileManager::RerunRecycle()
{
    m_pauseRecycleStart.store(false, std::memory_order_release);
    m_pauseRecycleFinish.store(false, std::memory_order_release);
}

uint16 WalFileManager::GetAllWalFileCount()
{
    if (unlikely(m_headFile == nullptr || m_tailFile == nullptr)) {
        return 0;
    }
    return ((m_tailFile->GetStartPlsn() - m_headFile->GetStartPlsn()) / m_initWalFilesPara.walFileSize) + 1;
}

uint16 WalFileManager::GetFreeWalFileCount()
{
    WalFile *writingFile = m_writingFile.load(std::memory_order_acquire);
    uint64 writeStartPlsn = writingFile == nullptr ? 0 : writingFile->GetStartPlsn();
    return (m_tailFile->GetStartPlsn() - writeStartPlsn) / m_initWalFilesPara.walFileSize;
}

void WalFileManager::PrintWalFilesInfo(bool isCreate, char *fileName)
{
    StoragePdb *pdb = g_storageInstance->GetPdb(m_initWalFilesPara.pdbId);
    if (STORAGE_VAR_NULL(pdb)) {
        ErrLog(DSTORE_ERROR, MODULE_WAL,
            ErrMsg("PrintWalFilesInfo get pdb failed, pdbId(%u).", m_initWalFilesPara.pdbId));
        return;
    }
    WalStream *stream = pdb->GetWalMgr()->GetWalStreamManager()->GetWalStream(m_initWalFilesPara.walId);
    if (STORAGE_VAR_NULL(stream)) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("PrintWalFilesInfo get wal stream failed."));
        return;
    }
    if (isCreate) {
        ErrLog(DSTORE_LOG, MODULE_WAL,
               ErrMsg("WalFileManager InitWalFiles create wal file %s succ, maxFlushFinishPlsn %lu,"
                      "endBytePos %lu, maxWrittenToFilePlsn %lu", fileName, stream->GetMaxFlushedPlsn(),
                      stream->GetStreamBufferInsertEndPos(), stream->GetMaxWrittenToFilePlsn()));
    } else if (pdb->GetPdbRoleMode() == PdbRoleMode::PDB_STANDBY || m_initWalFilesPara.isForStandBy) {
        ErrLog(DSTORE_LOG, MODULE_WAL,
               ErrMsg("[StandbyPdb] RecycleHeadFile %s succ, checkpoint plsn %lu, headFile start plsn %lu, "
                      "maxFlushFinishPlsn %lu, sum of wal files %u",
                      fileName, GetRecyclablePlsn(), m_headFile->GetStartPlsn(), stream->GetStandbyMaxFlushedPlsn(),
                      GetAllWalFileCount()));
    } else {
        ErrLog(DSTORE_LOG, MODULE_WAL,
               ErrMsg("RecycleHeadFile %s succ, checkpoint plsn %lu, headFile start plsn %lu, maxFlushFinishPlsn %lu"
                      "endBytePos %lu, maxWrittenToFilePlsn %lu, sum of wal files %u", fileName,
                      GetRecyclablePlsn(), m_headFile->GetStartPlsn(), stream->GetMaxFlushedPlsn(),
                      stream->GetStreamBufferInsertEndPos(), stream->GetMaxWrittenToFilePlsn(), GetAllWalFileCount()));
    }
}

void WalFileManager::SetWritingFile(WalFile *walFile)
{
    m_writingFile.store(walFile, std::memory_order_release);
}

void WalFileManager::GetFileSharedLock()
{
    RWLockRdLock(&m_fileLock);
}

void WalFileManager::GetFileUniqueLock()
{
    RWLockWrLock(&m_fileLock);
}

void WalFileManager::ReleaseSharedFileLock()
{
    RWLockRdUnlock(&m_fileLock);
}

void WalFileManager::ReleaseUniqueFileLock()
{
    RWLockWrUnlock(&m_fileLock);
}

void WalFileManager::StopRecycleWalFileWorker()
{
    m_stopBgRecycleThread.store(true);
    if (m_recycleThread != nullptr) {
        m_recycleThread->join();
        delete m_recycleThread;
        m_recycleThread = nullptr;
    }
}

void WalFileManager::StartupRecycleWalFileWorker(bool isDropping)
{
    m_stopBgRecycleThread.store(false);
    m_walFileIsDropping.store(isDropping);
    m_recycleThread = new std::thread(&WalFileManager::RecycleWalFileWorkerMain, this, isDropping);
}

bool WalFileManager::IsRecoveryRecycleFinish(bool *isStopRecycle)
{
    if (m_stopBgRecycleThread.load(std::memory_order_relaxed) && isStopRecycle != nullptr) {
        *isStopRecycle = true;
    }
    if (m_recycleThread != nullptr && m_recoveryRecycleFinish) {
        return true;
    }
    return false;
}

}
