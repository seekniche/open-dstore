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
 * dstore_log.cpp
 *
 *
 * IDENTIFICATION
 *        src/common/log/dstore_log.cpp
 *
 * ---------------------------------------------------------------------------------------
 */

#include "common/log/dstore_log.h"
#include <syslog.h>
#include <cstring>
#include <cerrno>
#include <climits> /* for PIPE_BUF */
#include <ctime>
#include "framework/dstore_instance.h"
#include "framework/dstore_thread.h"
#include "common/algorithm/dstore_string_info.h"
#include "common/memory/dstore_mctx.h"

namespace DSTORE {

const ModuleData moduleMap[] = {
    {MODULE_ALL, "ALL"},
    /* add your module name following */
    {MODULE_BUFFER, "BUFFER"},
    {MODULE_BUFMGR, "BUFMGR"},
    {MODULE_CATALOG, "CATALOG"},
    {MODULE_COMMON, "COMMON"},
    {MODULE_HEAP, "HEAP"},
    {MODULE_CONTROL, "CONTROL"},
    {MODULE_FRAMEWORK, "FRAMEWORK"},
    {MODULE_INDEX, "INDEX"},
    {MODULE_LOCK, "LOCK"},
    {MODULE_PAGE, "PAGE"},
    {MODULE_PORT, "PORT"},
    {MODULE_WAL, "WAL"},
    {MODULE_BGPAGEWRITER,        "BGPAGEWRITER"},
    {MODULE_TABLESPACE,          "TABLESPACE"},
    {MODULE_SEGMENT,             "SEGMENT"},
    {MODULE_TRANSACTION,         "TRANSACTION"},
    {MODULE_TUPLE,               "TUPLE"},
    {MODULE_UNDO,                "UNDO"},
    {MODULE_RPC,                 "RPC"},
    {MODULE_SYSTABLE,            "SYSTABLE"},
    {MODULE_MEMNODE,             "MEMNODE"},
    {MODULE_PDBREPLICA,          "PDBREPLICA"},
    {MODULE_RECOVERY,            "RECOVERY"},
    {MODULE_LOGICAL_REPLICATION, "MODULE_LOGICAL_REPLICATION"},
    {MODULE_PDB,                 "PDB"},
    {MODULE_WATCHDOG,            "WATCHDOG"},
    {MODULE_MAX,                 "MAX"},
};
static_assert(MODULE_MAX == sizeof(moduleMap) / sizeof(moduleMap[0]) - 1, "invalid moduleMap size.");

const char *GetValidModuleName(ModuleId moduleId)
{
    return moduleMap[static_cast<uint16>(moduleId)].m_modName;
}

/*
 * Version of GetValidModuleName that works with the whatiserrcode standalone
 * tool that has no access to thrd storage.
 */
const char *GetValidModuleNameForTool(ModuleId moduleId)
{
    if (moduleId > MODULE_MAX) {
        return nullptr;
    }
    char *name = static_cast<char *>(DstorePalloc0(DSTORE_MODULE_NAME_MAXLEN));
    if (STORAGE_VAR_NULL(name)) {
        return nullptr;
    }
    const char *prefix = (moduleId == MODULE_ALL) ? "DSTORE" : "D";
    errno_t rc = snprintf_s(name, DSTORE_MODULE_NAME_MAXLEN, DSTORE_MODULE_NAME_MAXLEN - 1, "%s_%s", prefix,
                            moduleMap[moduleId].m_modName);
    storage_securec_check_ss(rc);
    return name;
}

ModuleId GetModuleId(const char *moduleName)
{
    char name[DSTORE_MODULE_NAME_MAXLEN];
    const char *prefix = nullptr;
    errno_t rc;
    for (int i = 0; i < (int)MODULE_MAX; ++i) {
        prefix = (i == (int)MODULE_ALL) ? "DSTORE" : "D";
        rc = snprintf_s(name, DSTORE_MODULE_NAME_MAXLEN, DSTORE_MODULE_NAME_MAXLEN - 1, "%s_%s", prefix,
                        moduleMap[i].m_modName);
        storage_securec_check_ss(rc);
        name[rc] = 0;
        if (strncasecmp(name, moduleName, DSTORE_MODULE_NAME_MAXLEN) == 0) {
            return (ModuleId)i;
        }
    }
    /* invalid module id */
    return MODULE_MAX;
}

void ModuleLoggingInit(bool turnOn)
{
    StorageAssert(g_storageInstance != nullptr);
    StorageAssert(g_storageInstance->GetGuc() != nullptr);
    StorageAssert(g_storageInstance->GetGuc()->moduleLoggingConfigure != nullptr);
    const unsigned char v = turnOn ? 0xFF : 0x00;
    for (int i = 0; i < (int)DSTORE_MODULE_BITMAP_SIZE; ++i) {
        g_storageInstance->GetGuc()->moduleLoggingConfigure[i] = v;
    }
}

void EnableModuleLogging(ModuleId moduleId)
{
    if (moduleId == MODULE_ALL) {
        ModuleLoggingInit(true);
        return;
    }
    char *confMap = g_storageInstance->GetGuc()->moduleLoggingConfigure;
    confMap[GetModuleBitmapPos(moduleId)] =
        (unsigned char)(confMap[GetModuleBitmapPos(moduleId)]) | GetModuleMask(moduleId);
}

void DisableModuleLogging(ModuleId moduleId)
{
    if (moduleId == MODULE_ALL) {
        ModuleLoggingInit(false);
        return;
    }
    char *confMap = g_storageInstance->GetGuc()->moduleLoggingConfigure;
    confMap[GetModuleBitmapPos(moduleId)] =
        (unsigned char)(confMap[GetModuleBitmapPos(moduleId)]) & (~GetModuleMask(moduleId));
}

bool ModuleLoggingIsOn(ModuleId moduleId)
{
    StorageAssert(g_storageInstance != nullptr);
    StorageAssert(g_storageInstance->GetGuc() != nullptr);
    StorageAssert(g_storageInstance->GetGuc()->moduleLoggingConfigure != nullptr);
    /* MODULE_MAX is a special id. at default it's on.
     * after 'off(ALL)' is set, it is turned off;
     * after 'on(ALL)' is set, it is switched to be on again.
     */
    char *confMap = g_storageInstance->GetGuc()->moduleLoggingConfigure;
    return ((GetModuleMask(moduleId) & (unsigned char)(confMap[GetModuleBitmapPos(moduleId)])) != 0);
}

bool IsLogStarted()
{
    return IsLoggerStarted();
}

void SetLogLevelAndFoldParam(int logLevel, int foldPeriod, int foldThreshold, int foldLevel)
{
    /* fold config */
    SetErrLogFoldConfig(foldPeriod, foldThreshold, foldLevel);

    /* log level */
    SetErrLogServerLevel(logLevel);
}

void InitLogAdapterInstance(int logLevel, const char* logDir, int foldPeriod, int foldThreshold, int foldLevel)
{
    /* Skip processing effort if non-error message will not be output */
    if (IsLoggerStarted()) {
        return;
    }

    /* These assert conditions validate that DstoreErrLevelCheck() has been called. */
    bool result = SetErrLogFoldConfig(static_cast<uint32>(foldPeriod), static_cast<uint32>(foldThreshold), foldLevel);
    STORAGE_RELEASE_EXIT(!result, "set err log fold config failed, foldPeriod=%d, foldThreshold=%d, foldLevel=%d.\n",
        foldPeriod, foldThreshold, foldLevel);

    /* Initialize data for this error frame */
#define ERROR_LOG_PREFIX \
    (LOG_LINE_PREFIX_HIGH_PRECISION_TIMESTAMP | LOG_LINE_PREFIX_SEVERITY | \
    LOG_LINE_PREFIX_RUNNING_CONTEXT_TID | LOG_LINE_PREFIX_COMPILATION_CONTEXT_FILE_NAME | \
    LOG_LINE_PREFIX_COMPILATION_CONTEXT_LINENO | LOG_LINE_PREFIX_QUERY_STRING)

    uint32 logPrefix = ERROR_LOG_PREFIX;
    SetErrLogLinePrefixSuffix(logPrefix);

        /* keep only base name, useful especially for vpath builds */
    SetErrLogServerLevel(logLevel);

    /* the default text domain is the backend's */
    uint32 logDestination = ((logDir != nullptr) ? LOG_DESTINATION_LOCAL_FILE : LOG_DESTINATION_LOCAL_STDERR);
    SetErrLogDestination(logDestination);

    /* set log total space and per file space */
    const uint32 perFileSpace = 256 * 1024;
    const uint32 totalFileSpace = 100 * perFileSpace;
    SetErrLogSpaceSize(totalFileSpace, perFileSpace);

    /* return value does not matter */
    if (logDir != nullptr) {
        SetErrLogDirectory(logDir);

        ErrorCode err = StartLogger();
        STORAGE_RELEASE_EXIT((err != ERROR_SYS_OK), "StartLogger failed, err = %lld.\n", err);

        err = OpenLogger();
        STORAGE_RELEASE_EXIT((err != ERROR_SYS_OK), "OpenLogger failed, err = %lld.\n", err);
    }

    ErrLog(DSTORE_LOG, MODULE_COMMON, ErrMsg("Init log success, logLevel:%d, logDir:%s, foldPeriod:%d, "
        "foldThreshold:%d, foldLevel:%d.", logLevel, ((logDir != nullptr) ? logDir : "stderr"),
        foldPeriod, foldThreshold, foldLevel));
}

void StopLogAdapterInstance()
{
    if (!IsLoggerStarted()) {
        return;
    }
    CloseLogger();
    StopLogger();
    ResetErrLogFoldConfig();
}

void OpenLoggerThread()
{
    ErrorCode err = OpenLogger();
    StorageReleasePanic((err != ERROR_SYS_OK), MODULE_COMMON, ErrMsg("OpenLogger failed, err = %lld.", err));
}

void CloseLoggerThread()
{
    CloseLogger();
}

}  // namespace DSTORE
