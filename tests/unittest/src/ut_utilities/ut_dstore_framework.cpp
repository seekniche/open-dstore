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
#include "framework/dstore_pdb.h"
#include "framework/dstore_instance.h"
#include "framework/dstore_config_interface.h"
#include "buffer/dstore_bg_page_writer_mgr.h"
#include "buffer/dstore_bg_disk_page_writer.h"
#include "ut_mock/ut_mock.h"
#include "ut_utilities/ut_time_monitor.h"
#include "framework/dstore_session_interface.h"
#include "pdb/dstore_pdb_interface.h"

using namespace DSTORE;

extern char g_utTopDir[MAXPGPATH];
extern char g_utDir[MAXPGPATH];
extern char g_utLogDir[MAXPGPATH];
extern char g_utDataDir[MAXPGPATH];

UTConfigFile DSTORETEST::m_config;
char DSTORETEST::dstoreDir[MAXPGPATH] = {'0'};
char DSTORETEST::pdbTemplatePath[MAXPGPATH] = {'0'};
char DSTORETEST::srcDataFile[MAXPGPATH] = {'0'};
char DSTORETEST::imageDataFile[MAXPGPATH] = {'0'};
char DSTORETEST::undoSrcDataFile[MAXPGPATH] = {'0'};
char DSTORETEST::undoImageDataFile[MAXPGPATH] = {'0'};
StorageGUC DSTORETEST::m_guc = {0};
DSTORE::PdbId DSTORETEST::m_pdbId = PDB_TEMPLATE1_ID;
TenantConfig DSTORETEST::m_tenantConfig = {0};

template <typename T>
char DSTOREParamTest<T>::dstoreDir[MAXPGPATH] = {'0'};

template <typename T>
char DSTOREParamTest<T>::pdbTemplatePath[MAXPGPATH] = {'0'};

template <typename T>
StorageGUC DSTOREParamTest<T>::m_guc = {0};
template <typename T>
TenantConfig DSTOREParamTest<T>::m_tenantConfig = {0};

void create_thread_and_register()
{
    if (thrd == nullptr) {
        thrd = DstoreNew(g_storageInstance->GetMemoryMgr()->GetGroupContext(DSTORE::MEMORY_CONTEXT_LONGLIVE))
            ThreadContext();
        DSTORE::StorageSession *sc = DSTORE::CreateStorageSession(1LL);
        thrd->AttachSessionToThread(sc);
    }
    if (thrd->m_memoryMgr == nullptr) {
        (void)thrd->InitializeBasic();
    }
    (void)thrd->InitStorageContext(g_defaultPdbId);
    g_storageInstance->AddVisibleThread(thrd, g_defaultPdbId);
}

void ut_init_transaction_runtime()
{
    if (g_storageInstance->GetType() == StorageInstanceType::DISTRIBUTE_MEMORY) {
        return;
    }
    (void)thrd->InitTransactionRuntime(g_defaultPdbId, nullptr, nullptr);
}

void destory_thread()
{
    thrd->Destroy();
    DstorePfree(thrd);
    thrd = nullptr;
}

void unregister_thread()
{
    if (thrd != nullptr) {
        g_storageInstance->RemoveVisibleThread(thrd);
        DSTORE::StorageSession *sc = thrd->GetSession();
        thrd->DetachSessionFromThread();
        DSTORE::CleanUpSession(sc);
        thrd->Destroy();
        DstorePfree(thrd);
        thrd = nullptr;
    }
}

int RemoveDir(const char *dirPath)
{
    char *curDir = (char *)".";
    char *upperDir = (char *)"..";
    DIR *dirHandle = nullptr;
    dirent *dirContext = nullptr;
    struct stat dirStat;
    char subDirPath[MAXPGPATH];
    if (access(dirPath, F_OK) != 0) {
        return 0;
    }

    if (stat(dirPath, &dirStat) < 0) {
        StorageAssert(0);
    }
    if (S_ISDIR(dirStat.st_mode)) {
        dirHandle = opendir(dirPath);
        StorageAssert(dirHandle);
        while ((dirContext = readdir(dirHandle)) != nullptr) {
            if ((strcmp(dirContext->d_name, curDir) == 0) || strcmp(dirContext->d_name, upperDir) == 0) {
                continue;
            }
            int rc = sprintf_s(subDirPath, MAXPGPATH, "%s/%s", dirPath, dirContext->d_name);
            storage_securec_check_ss(rc);
            RemoveDir(subDirPath);
        }
        closedir(dirHandle);
        rmdir(dirPath);
    } else {
        remove(dirPath);
    }
    return 0;
}

void DSTORETEST::SetUpTestCase()
{
    char m_tenantConfigFilePath[MAXPGPATH];
    /* Get tenant config file. */
    errno_t rc = memset_s(m_tenantConfigFilePath, MAXPGPATH, 0, MAXPGPATH);
    storage_securec_check(rc, "\0", "\0");
    char *execPath4tenant = m_tenantConfigFilePath;
    int ret = readlink("/proc/self/exe", execPath4tenant, MAXPGPATH);
    ASSERT_GT(ret, 0);
    char *lastSlashPtr4tenant = strrchr(execPath4tenant, '/');
    ASSERT_NE(lastSlashPtr4tenant, nullptr);
    ret = snprintf(lastSlashPtr4tenant + 1, MAXPGPATH / 2, "ut_isoland_start.json");
    ASSERT_GT(ret, 0);
    ASSERT_EQ(access(m_tenantConfigFilePath, F_OK), 0); /* It must exist. */
    TenantConfig tenantConfig;
    DSTORE::RetStatus err = TenantConfigInterface::GetTenantConfig(m_tenantConfigFilePath, &tenantConfig);
    ASSERT_EQ(err, DSTORE_SUCC);
    ret = memcpy_s(static_cast<void *>(&m_tenantConfig), sizeof(TenantConfig), &tenantConfig, sizeof(TenantConfig));
    storage_securec_check(ret, "\0", "\0");

    m_config.SetUpFilePath();

    m_guc = {.selfNodeId = 0,
             .buffer = 4096,
             .bufferLruPartition = 16,
             .checkpointTimeout = 60,
             .defaultIsolationLevel = 0,
             .maintenanceWorkMem = 65536,
             .dataDir = g_utDataDir,
             .ncores = 800,
             .logMinMessages = DSTORE_ERROR,
             .foldPeriod = 20,
             .foldThreshold = 10,
             .foldLevel = DSTORE_WARNING,
             .csnAssignmentIncrement = 10000,
             .moduleLoggingConfigure = nullptr,
             .lockHashTableSize = 256,
             .lockTablePartitionNum = 256,
             .enableLazyLock = false,
             .vfsTenantIsolationConfigPath = nullptr,
             .updateCsnMinInterval = 500 * 1000,
             .numObjSpaceMgrWorkers = 1,
             .minFreePagePercentageThreshold1 = 0,
             .minFreePagePercentageThreshold2 = 0,
             .probOfExtensionThreshold = 0,
             .walLevel = 0,
             .recoveryWorkerNum = 1,
             .synchronousCommit = true,
             .walStreamCount = 10,
             .walFileNumber = 1,
             .walFileSize = 16777216, /* Byte */
             .walBuffers = 49152,
             .walFlushTimeout = 500,
             .walReadBufferSize = 1073741824, /* Byte */
             .walRedoBufferSize = 268435456, /* Byte */
             .walwriterCpuBind = 0,
             .walKeepSegments = 2,
             .redoBindCpuAttr = "nobind",
             .bgWalWriterMinBytes = 51200,
             .walEachWriteLenghthLimit = 524288,
             .numaNodeNum = 1,
             .disableBtreePageRecycle = false,
             .deadlockTimeInterval = 20000,
             .recycleFsmTimeInterval = 10,
             .probOfUpdateFsmTimestamp = 0,
             .probOfRecycleFsm = 0,
             .probOfRecycleBtree = 10};

    m_guc.selfNodeId = -1;
    m_guc.distLockNumBuckets = 128;
    m_guc.distLockMaxRingSize = 128;
    m_guc.csnMode = DSTORE::CsnMode::DEFAULT;
    /* Grand read authority to a page whenever there is no write operation for 5 seconds for unit tests. */
    m_guc.pdReadAuthResetPeriod = 5;
    /* Initialize default guc values for disaster recovery. */
    m_guc.defaultHeartbeatTimeoutInterval = 1;
    m_guc.defaultWalSizeThreshold = 1;
    m_guc.bgDiskWriterSlaveNum = 1;
    m_guc.bgPageWriterSleepMilliSecond = 2000;
    m_guc.walThrottlingSize = 1048576;
    m_guc.maxIoCapacityKb = 10000;

    m_guc.csnThreadBindCpu = -1;
    m_guc.commConfigStr = NULL;
    m_guc.commThreadMin = 30;
    m_guc.commThreadMax = 30;
    m_guc.clusterId = 1;
    m_guc.commProtocolType = 0;
    srand(time(nullptr));
    m_guc.ctrlPlanePort = (rand() % 30000) + 10000;
    m_guc.csnMode = DSTORE::CsnMode::DEFAULT;
    m_guc.commProtocolTypeStr = const_cast<char *>("TCP_TYPE");
    m_guc.gclockOverlapWaitTimeOptimization = true;
    m_guc.enableQuickStartUp = false;
    m_guc.tenantConfig = &m_tenantConfig;
    m_guc.bgMemWriterSlaveNum = 1;
    m_guc.rnSleepTimeoutSeconds = 1;
    m_guc.enableRemoteCrConstruction = false;
    m_guc.walKeepTimeAfterRecovery = 0;
    m_guc.enableWatchDog = true;
    m_guc.watchdogCheckIntervalSec = 5;
    m_guc.watchdogMasterWriterTimeoutSec = 30;
    m_guc.watchdogSlaveWriterTimeoutSec = 60;
    m_guc.watchdogCheckpointerTimeoutSec = 600;
    m_guc.watchdogWalRecycleTimeoutSec = 30;
    m_guc.watchdogUndoRecycleTimeoutSec = 120;
    m_guc.watchdogBtreeRecycleTimeoutSec = 1800;
    rc = strcpy_s(m_guc.tenantConfig->tenantName, DSTORE_TENANT_NAME_MAX_LEN, "tenant1");
    storage_securec_check(rc, "\0", "\0");
    m_guc.tenantConfig->storeSpaceCnt = 1;
    rc = strcpy_s(m_guc.tenantConfig->storeSpaces[0].storeSpaceName, DSTORE_TENANT_NAME_MAX_LEN,
        UT_DEFAULT_STORESPACE_NAME);
    storage_securec_check(rc, "\0", "\0");

    rc = sprintf_s(dstoreDir, MAXPGPATH, "%s/%s/", g_utDataDir, DSTORE::BASE_DIR);
    storage_securec_check_ss(rc);

    rc = sprintf_s(pdbTemplatePath, MAXPGPATH, "%s/%s/", dstoreDir, m_guc.tenantConfig->storageConfig.template1VfsName);
    storage_securec_check_ss(rc);
}

void DSTORETEST::TearDownTestCase()
{
#ifdef ENABLE_FAULT_INJECTION

    DestroyFaultInjectionHash(FI_GLOBAL);

#endif
}

void DSTORETEST::PrintTestCaseName()
{
    const ::testing::TestInfo* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
    char logMsg[1024] = {0};
    int rc = sprintf_s(logMsg, 1024, "---------------------------------------------------------------\n"
                            "     TEST:%s.%s     START\n"
                            "---------------------------------------------------------------\n",
                            testInfo->test_suite_name(), testInfo->name());
    storage_securec_check_ss(rc);
    ErrLog(DSTORE_INFO, MODULE_ALL, ErrMsg("%s.",logMsg));
}

void DSTORETEST::SetUp()
{
    SetDefaultPdbId(PDB_TEMPLATE1_ID);
    /*
     * As there will be a timer which counts the time each test case takes, we have to reset its count down every time
     * we set up a new case.
     */
    m_guc.selfNodeId = 1;
    UTTimeoutMonitor::GetInstance()->ResetCountDown();
    ut_memory_context_init();
    DstoreMemoryContextSwitchTo(m_ut_memory_context);
    RemoveDir(g_utDataDir);
    int ret = mkdir(g_utDataDir, 0777);
    StorageAssert(ret == 0);
    chdir(g_utDataDir);
    ret = mkdir(dstoreDir, 0777);
    StorageAssert(ret == 0);

    InitLogAdapterInstance(m_guc.logMinMessages, g_utLogDir, m_guc.foldPeriod, m_guc.foldThreshold, m_guc.foldLevel);
    PrintTestCaseName();
    CreateDefaultVfs(DSTORETEST::m_guc.tenantConfig);
}

void DSTORETEST::TearDown()
{
    StopLogAdapterInstance();
    ut_memory_context_free();
    chdir(g_utTopDir);
    /* release vfs lib from CreateDefaultVfs */
    VfsInterface::ModuleInitialize();
    ::OffloadVfsLib(GetVfsLibHandle());
}

void DSTORETEST::Bootstrap()
{
    g_storageInstance = static_cast<StorageInstance *>(StorageInstanceInterface::Create(StorageInstanceType::SINGLE));
    g_storageInstance->InitWorkingVersionNum(&GRAND_VERSION_NUM);
    thrd = dynamic_cast<ThreadContext*>(ThreadContextInterface::Create());
    StorageSession *sc = CreateStorageSession(1ULL);
    thrd->AttachSessionToThread(sc);
    (void)thrd->InitializeBasic();
    (void)g_storageInstance->Bootstrap(&m_guc);
    
    (void)thrd->InitStorageContext(DSTORE::g_defaultPdbId);
    g_storageInstance->AddVisibleThread(thrd, DSTORE::g_defaultPdbId);

    DSTORE::CreateTemplateTablespace(DSTORE::g_defaultPdbId);
    DSTORE::CreateUndoMapSegment(DSTORE::g_defaultPdbId);

    StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);

    StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);
    g_storageInstance->UnregisterThread();
    (void)DstoreMemoryContextSwitchTo(g_storageInstance->GetMemoryMgr()->GetRoot());
    g_storageInstance->BootstrapDestroy();
    g_storageInstance->BootstrapResDestroy();
    DSTORE::CleanUpSession(sc);

    StorageInstanceInterface::DestoryInstance();
    g_storageInstance = nullptr;
}

char *DSTORETEST::GetTemplatePdbPath()
{
    return pdbTemplatePath;
}

void DSTORETEST::CopyDataFile()
{
    StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);
    StoragePdbInterface::FlushAllDirtyPages(DSTORE::g_defaultPdbId);

    uint16 startDefaultFileId = 5124;
    for (uint16 i = 0; i < EXTENT_TYPE_COUNT; i++) {
        (void) memset_s(srcDataFile, MAXPGPATH, 0, MAXPGPATH);
        int rc = sprintf_s(srcDataFile, MAXPGPATH, "%s%d", pdbTemplatePath, startDefaultFileId);
        storage_securec_check_ss(rc);
        rc = sprintf_s(imageDataFile, MAXPGPATH, "%s%d_image", pdbTemplatePath, startDefaultFileId);
        storage_securec_check_ss(rc);

        /* do copy */
        std::ifstream  src(srcDataFile, std::ios::binary);
        std::ofstream  image(imageDataFile, std::ios::binary);
        image << src.rdbuf();

        startDefaultFileId++;
    }

    uint16 startUndoFileId = 5121;
    for (uint16 i = 0; i < 3; i++) {
        int rc = sprintf_s(undoSrcDataFile, MAXPGPATH, "%s%d", pdbTemplatePath, startUndoFileId);
        storage_securec_check_ss(rc);
        rc = sprintf_s(undoImageDataFile, MAXPGPATH, "%s%d_image", pdbTemplatePath, startUndoFileId);
        storage_securec_check_ss(rc);

        std::ifstream undoSrc(undoSrcDataFile, std::ios::binary);
        std::ofstream undoImage(undoImageDataFile, std::ios::binary);
        undoImage << undoSrc.rdbuf();

        startUndoFileId++;
    }
}

void DSTORETEST::ReadDataFileImage(PageId pageId, char *outBuffer, size_t bufferSize)
{
    uint32 offset = pageId.m_blockId * BLCKSZ;
    int fd;
    int rc;
    if (pageId.m_fileId >= 2052) {
        (void) memset_s(imageDataFile, MAXPGPATH, 0, MAXPGPATH);
        rc = sprintf_s(imageDataFile, MAXPGPATH, "%s%d_image", pdbTemplatePath, pageId.m_fileId);
        storage_securec_check_ss(rc);
        fd = open(imageDataFile, O_RDONLY);
    } else  {
        (void) memset_s(undoImageDataFile, MAXPGPATH, 0, MAXPGPATH);
        rc = sprintf_s(undoImageDataFile, MAXPGPATH, "%s%d_image", pdbTemplatePath, pageId.m_fileId);
        storage_securec_check_ss(rc);
        fd = open(undoImageDataFile, O_RDONLY);
    }
    size_t len = pread(fd, outBuffer, bufferSize, offset);
    if (len != bufferSize) {
        ASSERT(0);
    }
    close(fd);
}

char **DSTORETEST::AllocBufferForImage(bool isDataImage, uint32 *blockCount, FileId defaultFileId, FileId undoFileId)
{
    struct stat statBuf;
    (void) memset_s(imageDataFile, MAXPGPATH, 0, MAXPGPATH);
    TablespaceId tablespaceId = isDataImage ? static_cast<TablespaceId>(TBS_ID::DEFAULT_TABLE_SPACE_ID) :
        static_cast<TablespaceId>(TBS_ID::UNDO_TABLE_SPACE_ID);
    FileId fileId = isDataImage ? defaultFileId : undoFileId;
    int rc = sprintf_s(imageDataFile, MAXPGPATH, "%s%d_image", pdbTemplatePath, fileId);
    storage_securec_check_ss(rc);
    stat(imageDataFile, &statBuf);
    uint64 fileSize = statBuf.st_size;
    *blockCount = fileSize / BLCKSZ;

    char  **imageBuffer = (char **)DstorePalloc0(*blockCount * sizeof(char *));
    for (uint32 i = 0 ; i < *blockCount; i++) {
        imageBuffer[i] = (char *)DstorePalloc0(BLCKSZ);
        ReadDataFileImage({fileId, i}, imageBuffer[i], BLCKSZ);
    }
    return imageBuffer;
}

void DSTORETEST::FreeBufferForImage(char **buffer, uint32 blockCount)
{
    for (uint32 i = 0 ; i < blockCount; i++) {
        DstorePfreeExt(buffer[i]);
    }
    DstorePfreeExt(buffer);
}

bool DSTORETEST::CmpPages(void *page1, void *page2, size_t pageSize, bool ignorePlsn)
{
    /* currently, ignore m_header.m_checksum, m_glsn */
    ((Page *)page1)->m_header.m_checksum = ((Page *)page2)->m_header.m_checksum;
    ((Page *)page1)->m_header.m_glsn = ((Page *)page2)->m_header.m_glsn;
    ((Page *)page1)->m_header.m_walId = ((Page *)page2)->m_header.m_walId;
    if (ignorePlsn) {
        ((Page *)page1)->m_header.m_plsn = ((Page *)page2)->m_header.m_plsn;
    }
    return memcmp(page1, page2, pageSize) == 0;
}

template class DSTOREParamTest<std::vector<Size>>;

template <typename T>
void DSTOREParamTest<T>::SetUpTestCase()
{
    m_guc = {.selfNodeId = 0,
             .buffer = 8192,
             .bufferLruPartition = 4,
             .checkpointTimeout = 60,
             .defaultIsolationLevel = 0,
             .maintenanceWorkMem = 65536,
             .dataDir = g_utDataDir,
             .ncores = 800,
             .logMinMessages = DSTORE_DEBUG1,
             .foldPeriod = 20,
             .foldThreshold = 10,
             .foldLevel = DSTORE_WARNING,
             .csnAssignmentIncrement = 10000,
             .moduleLoggingConfigure = nullptr,
             .lockHashTableSize = 256,
             .lockTablePartitionNum = 256,
             .enableLazyLock = false,
             .vfsTenantIsolationConfigPath = nullptr,
             .updateCsnMinInterval = 500 * 1000,
             .numObjSpaceMgrWorkers = 1,
             .minFreePagePercentageThreshold1 = 0,
             .minFreePagePercentageThreshold2 = 0,
             .probOfExtensionThreshold = 0,
             .walLevel = 0,
             .recoveryWorkerNum = 1,
             .synchronousCommit = true,
             .walStreamCount = 10,
             .walFileNumber = 1,
             .walFileSize = 16777216, /* Byte */
             .walBuffers = 49152,
             .walFlushTimeout = 500,
             .walReadBufferSize = 1073741824, /* Byte */
             .walRedoBufferSize = 268435456, /* Byte */
             .walwriterCpuBind = 0,
             .walKeepSegments = 2,
             .redoBindCpuAttr = "nobind",
             .bgWalWriterMinBytes = 51200,
             .walEachWriteLenghthLimit = 524288,
             .numaNodeNum = 1,
             .disableBtreePageRecycle = false,
             .deadlockTimeInterval = 20000,
             .recycleFsmTimeInterval = 10,
             .probOfUpdateFsmTimestamp = 0,
             .probOfRecycleFsm = 0};
    m_guc.tenantConfig = &m_tenantConfig;
    m_guc.bgDiskWriterSlaveNum = 1;
    m_guc.bgPageWriterSleepMilliSecond = 2000;
    m_guc.walThrottlingSize = 1048576;
    m_guc.maxIoCapacityKb = 10000;
    m_guc.bgMemWriterSlaveNum = 1;
    m_guc.rnSleepTimeoutSeconds = 1;
    m_guc.enableRemoteCrConstruction = false;
    m_guc.walKeepTimeAfterRecovery = 0;
    m_guc.enableStmtTrack = false;
    m_guc.enableWatchDog = true;
    m_guc.watchdogCheckIntervalSec = 5;
    m_guc.watchdogMasterWriterTimeoutSec = 30;
    m_guc.watchdogSlaveWriterTimeoutSec = 60;
    m_guc.watchdogCheckpointerTimeoutSec = 600;
    m_guc.watchdogWalRecycleTimeoutSec = 30;
    m_guc.watchdogUndoRecycleTimeoutSec = 120;
    m_guc.watchdogBtreeRecycleTimeoutSec = 1800;

    char m_tenantConfigFilePath[MAXPGPATH];
    /* Get tenant config file. */
    errno_t rc = memset_s(m_tenantConfigFilePath, MAXPGPATH, 0, MAXPGPATH);
    storage_securec_check(rc, "\0", "\0");
    char *execPath4tenant = m_tenantConfigFilePath;
    int ret = readlink("/proc/self/exe", execPath4tenant, MAXPGPATH);
    ASSERT_GT(ret, 0);
    char *lastSlashPtr4tenant = strrchr(execPath4tenant, '/');
    ASSERT_NE(lastSlashPtr4tenant, nullptr);
    ret = snprintf(lastSlashPtr4tenant + 1, MAXPGPATH / 2, "ut_isoland_start.json");
    ASSERT_GT(ret, 0);
    ASSERT_EQ(access(m_tenantConfigFilePath, F_OK), 0); /* It must exist. */
    TenantConfig tenantConfig;
    DSTORE::RetStatus err = TenantConfigInterface::GetTenantConfig(m_tenantConfigFilePath, &tenantConfig);
    ASSERT_EQ(err, DSTORE_SUCC);
    ret = memcpy_s(static_cast<void *>(&m_tenantConfig), sizeof(TenantConfig), &tenantConfig, sizeof(TenantConfig));
    storage_securec_check(ret, "\0", "\0");

    rc = strcpy_s(m_guc.tenantConfig->tenantName, DSTORE_TENANT_NAME_MAX_LEN, "tenant1");
    storage_securec_check(rc, "\0", "\0");
    m_guc.tenantConfig->storeSpaceCnt = 1;
    rc = strcpy_s(m_guc.tenantConfig->storeSpaces[0].storeSpaceName, DSTORE_TENANT_NAME_MAX_LEN, UT_DEFAULT_STORESPACE_NAME);
    storage_securec_check(rc, "\0", "\0");
    rc = sprintf_s(dstoreDir, MAXPGPATH, "%s/%s/", g_utDataDir, DSTORE::BASE_DIR);
    storage_securec_check_ss(rc);

    rc = sprintf_s(pdbTemplatePath, MAXPGPATH, "%s/%s/", dstoreDir, m_guc.tenantConfig->storageConfig.template1VfsName);
    storage_securec_check_ss(rc);
}

template <typename T>
void DSTOREParamTest<T>::TearDownTestCase()
{}

template <typename T>
void DSTOREParamTest<T>::SetUp()
{
    SetDefaultPdbId(PDB_TEMPLATE1_ID);

    UTTimeoutMonitor::GetInstance()->ResetCountDown();
    ut_memory_context_init();
    DstoreMemoryContextSwitchTo(m_ut_memory_context);
    RemoveDir(g_utDataDir);
    int ret = mkdir(g_utDataDir, 0777);
    StorageAssert(ret == 0);
    chdir(g_utDataDir);
    ret = mkdir(dstoreDir, 0777);
    StorageAssert(ret == 0);
    m_guc.csnAssignmentIncrement = 2;
    m_guc.synchronousCommit = true;
    m_guc.walFileNumber = 1;
    m_guc.walFileSize = 16777216; /* Byte */
    m_guc.walReadBufferSize = 1073741824;
    m_guc.walRedoBufferSize = 268435456;
    m_guc.walBuffers = 49152;
    m_guc.walwriterCpuBind = 0;
    m_guc.redoBindCpuAttr = "nobind";
    m_guc.tenantConfig = &m_tenantConfig;
    m_guc.bgWalWriterMinBytes = 51200;
    m_guc.walEachWriteLenghthLimit = 524288;
    CreateDefaultVfs(m_guc.tenantConfig);
}

template <typename T>
void DSTOREParamTest<T>::TearDown()
{
    ut_memory_context_free();
    chdir(g_utTopDir);
}

void DSTORETEST::ProcEnter()
{
    UTTimeoutMonitor::GetInstance()->Start();
}

void DSTORETEST::ProcExit(int exitCode)
{
    UTTimeoutMonitor::GetInstance()->Interrupt();
    UTTimeoutMonitor::Destroy();
    exit(exitCode);
}

void DSTORETEST::InitTenantConfig(StorageGUC *guc) {
    char m_tenantConfigFilePath[MAXPGPATH];
    errno_t rc = memset_s(m_tenantConfigFilePath, MAXPGPATH, 0, MAXPGPATH);
    storage_securec_check(rc, "\0", "\0");
    char *execPath4tenant = m_tenantConfigFilePath;
    int ret = readlink("/proc/self/exe", execPath4tenant, MAXPGPATH);
    ASSERT_GT(ret, 0);
    char *lastSlashPtr4tenant = strrchr(execPath4tenant, '/');
    ASSERT_NE(lastSlashPtr4tenant, nullptr);
    ret = snprintf(lastSlashPtr4tenant + 1, MAXPGPATH / 2, "ut_isoland_start.json");
    ASSERT_GT(ret, 0);
    ASSERT_EQ(access(m_tenantConfigFilePath, F_OK), 0); /* It must exist. */
    DSTORE::RetStatus err = TenantConfigInterface::GetTenantConfig(m_tenantConfigFilePath, guc->tenantConfig);
    ASSERT_EQ(err, DSTORE_SUCC);
    ret = strcpy_s(guc->tenantConfig->tenantName, DSTORE_TENANT_NAME_MAX_LEN, "tenant1");
    ASSERT_EQ(ret, 0);
}

void UtInstance::Install(StorageGUC *guc, DstoreMemoryContext utMctx)
{
    InstallPrepare(guc, utMctx);

    /* the prepare work is done here, we can modify system table and generate redo & undo log in general.
     * But in UT test, we skip these steps.
     */

    InstallFinish();
}

void UtInstance::InstallPrepare(StorageGUC *guc, DstoreMemoryContext utMctx)
{
    SetDefaultPdbId(PDB_TEMPLATE1_ID);
    g_storageInstance = this;
    g_storageInstance->InitWorkingVersionNum(&GRAND_VERSION_NUM);
    (void)CreateMemMgr();
    thrd = DstoreNew(GetMemoryMgr()->GetGroupContext(DSTORE::MEMORY_CONTEXT_LONGLIVE)) ThreadContext();
    (void)thrd->InitializeBasic();
    StorageSession *sc = CreateStorageSession(1ULL);
    thrd->AttachSessionToThread(sc);
    (void)Bootstrap(guc);

    (void)thrd->InitStorageContext(g_defaultPdbId);

    AddVisibleThread(thrd, g_defaultPdbId);
    StoragePdb *templatePdb = GetPdb(g_defaultPdbId);
    templatePdb->CreateTemplateTablespace();
    templatePdb->CreateUndoMapSegment();

    (void)thrd->InitTransactionRuntime(g_defaultPdbId, nullptr, nullptr);
}

void UtInstance::InstallFinish()
{
    GetBufferMgr()->FlushAll(true);
    StorageSession *sc = thrd->GetSession();
    thrd->DetachSessionFromThread();
    RemoveVisibleThread(thrd);
    CleanUpSession(sc);
    thrd->Destroy();
    delete thrd;
    thrd = nullptr;
    BootstrapDestroy();
    BootstrapResDestroy();
    g_storageInstance = nullptr;
}

void UtInstance::Startup(StorageGUC *guc)
{
    g_storageInstance = this;
    g_storageInstance->InitWorkingVersionNum(&GRAND_VERSION_NUM);
    (void)CreateMemMgr();
    ThreadContextInterface::Create();
    (void)thrd->InitializeBasic();
    StorageSession *sc = CreateStorageSession(1ULL, 128);
    thrd->AttachSessionToThread(sc);
    (void)StartupInstance(guc);
    (void)thrd->InitStorageContext(g_defaultPdbId);
    /* temporary fix for aarch64 WalStream::BatchAppend::GetWalInsertLock, in which numaId is -1 */
#ifdef __aarch64__
    thrd->SetNumaId(1);
#endif
    (void)thrd->InitTransactionRuntime(g_defaultPdbId, nullptr, nullptr);
}

void UtInstance::Shutdown()
{
    StorageSession *sc = thrd->GetSession();
    CleanUpSession(sc);
    ShutdownInstance();
    g_storageInstance = nullptr;
}

RetStatus UtInstance::CreatePDB(PdbInfo *pdbInfo)
{
    return StorageInstance::CreatePDB(pdbInfo);
}

void UtInstance::ThreadSetupAndRegister()
{
    StorageAssert(thrd == nullptr);
    StorageAssert(g_storageInstance != nullptr);

    thrd = DstoreNew(g_storageInstance->GetMemoryMgr()->GetGroupContext(DSTORE::MEMORY_CONTEXT_LONGLIVE))
        ThreadContext();
    DSTORE::StorageSession *sc = DSTORE::CreateStorageSession(1LL);
    thrd->AttachSessionToThread(sc);
    (void)thrd->InitializeBasic();
    (void)thrd->InitStorageContext(g_defaultPdbId);
    g_storageInstance->AddVisibleThread(thrd, g_defaultPdbId);
    (void)thrd->InitTransactionRuntime(g_defaultPdbId, nullptr, nullptr); /* do this ?*/
}

void UtInstance::ThreadUnregisterAndExit()
{
    StorageAssert(thrd != nullptr);
    StorageAssert(g_storageInstance != nullptr);

    g_storageInstance->RemoveVisibleThread(thrd);
    DSTORE::StorageSession *sc = thrd->GetSession();
    thrd->DetachSessionFromThread();
    DSTORE::CleanUpSession(sc);
    thrd->Destroy();
    delete thrd;
    thrd = nullptr;
}

void CreateClusterInfo(StorageGUC *guc, int nodeNum, std::vector<int> nodeType, int startNodeId)
{
    StorageAssert(guc);
    char path [MAXPGPATH] = {0};
    int rc = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, "%s/clusterInfo", guc->dataDir);
    storage_securec_check_ss(rc);
    if (access(path, F_OK) != -1) {
        return;
    }
    std::ofstream clusterInfoFile;
    clusterInfoFile.open(path, std::ios::out);
    clusterInfoFile<<"clusterId =  "<< guc->clusterId <<std::endl;
    clusterInfoFile<<"termId = 1 "<<std::endl;
    if (nodeNum == 0 || nodeNum != nodeType.size()) {
        clusterInfoFile<<"nodeCount = 3 "<<std::endl;
        clusterInfoFile<<"nodeId = 0, nodeType = 0, nodeStatus = 3, ip = 127.0.0.1 "<<std::endl;
        clusterInfoFile<<"nodeId = 1, nodeType = 0, nodeStatus = 3, ip = 127.0.0.2 "<<std::endl;
        clusterInfoFile<<"nodeId = 2, nodeType = 0, nodeStatus = 3, ip = 127.0.0.3 "<<std::endl;
    } else {
        clusterInfoFile<<"nodeCount = "<<nodeNum<<std::endl;
        for (int i = 0; i < nodeNum; ++i) {
            clusterInfoFile<<"nodeId = "<<startNodeId + i<<", nodeType = "<<nodeType[i]
                <<", nodeStatus = 3, ip = 127.0.0."<<startNodeId + i + 1<<std::endl;
        }
    }
    clusterInfoFile<<"# nodeCount & nodeId & ip are needed to read ip address from node id under integration mode. "<<std::endl;
    clusterInfoFile<<"# termId & nodeStatus are for dstore self test, use only under standalone mode. "<<std::endl;
    clusterInfoFile.close();
}

void CreateDefaultVfs(TenantConfig *config)
{
    bool ret = VfsInterface::ModuleInitialize();
    StorageReleasePanic(!ret, DSTORE::MODULE_FRAMEWORK, ErrMsg("ModuleInitialize fail."));

    char *path = (char *)malloc(MAXPGPATH);
    errno_t rc = sprintf_s(path, MAXPGPATH, "%s/%s", g_utDataDir, DSTORE::BASE_DIR);
    storage_securec_check_ss(rc);
    VfsInterface::SetupTenantIsoland(config, path);
    free(path);

    RetStatus retStatus = VfsInterface::CreateTenantDefaultVfs(config);
    StorageReleasePanic(STORAGE_FUNC_FAIL(retStatus), DSTORE::MODULE_FRAMEWORK, ErrMsg("CreateTenantDefaultVfs fail."));
}

int GetClusterInfoTerm()
{
    int ret = -1;
    char path [MAXPGPATH] = {0};
    int rc = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, "%s/clusterInfo", g_utDataDir);
    storage_securec_check_ss(rc);

    std::ifstream clusterInfoFile;
    clusterInfoFile.open(path, std::ios::in);
    char buf[256] = {0};
    clusterInfoFile.getline(buf, sizeof(buf));
    clusterInfoFile.getline(buf, sizeof(buf));
    sscanf_s((buf), "termId = %d", &ret);
    clusterInfoFile.close();
    return ret;
}

void UpdateClusterInfo(int nodeNum, std::vector<int> nodeType, std::vector<int> nodeStatus, int term)
{
    char path [MAXPGPATH] = {0};
    int rc = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, "%s/clusterInfo", g_utDataDir);
    storage_securec_check_ss(rc);
    unlink(path);

    std::ofstream clusterInfoFile;
    clusterInfoFile.open(path, std::ios::out);
    clusterInfoFile<<"clusterId = 1 "<<std::endl;
    clusterInfoFile<<"termId = "<<term + 1<<" "<<std::endl;
    if (nodeNum == 0 || nodeNum != nodeType.size()) {
        clusterInfoFile<<"nodeCount = 3 "<<std::endl;
        clusterInfoFile<<"nodeId = 0, nodeType = 0, nodeStatus = 3, ip = 127.0.0.1 "<<std::endl;
        clusterInfoFile<<"nodeId = 1, nodeType = 0, nodeStatus = 3, ip = 127.0.0.2 "<<std::endl;
        clusterInfoFile<<"nodeId = 2, nodeType = 0, nodeStatus = 3, ip = 127.0.0.3 "<<std::endl;
    } else {
        clusterInfoFile<<"nodeCount = "<<nodeNum<<std::endl;
        for (int i = 0; i < nodeNum; ++i) {
            clusterInfoFile<<"nodeId = "<<i<<", nodeType = "<<nodeType[i]<<", nodeStatus = "<<nodeStatus[i]<<", ip = 127.0.0."<<i + 1<<std::endl;
        }
    }
    clusterInfoFile<<"# nodeCount & nodeId & ip are needed to read ip address from node id under integration mode. "<<std::endl;
    clusterInfoFile<<"# termId & nodeStatus are for dstore self test, use only under standalone mode. "<<std::endl;
    clusterInfoFile.close();
}