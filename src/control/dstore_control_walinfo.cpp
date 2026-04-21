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
 * dstore_control_walinfo.cpp
 *
 *
 *
 * IDENTIFICATION
 *        dstore/src/control/dstore_control_walinfo.cpp
 *
 * ---------------------------------------------------------------------------------------
 */
#include "control/dstore_control_walinfo.h"
#include "control/dstore_control_file_lock.h"
#include "framework/dstore_instance.h"
#include "perfcounter/dstore_perf_item.h"
#include "framework/dstore_framework_perf_unit.h"
#include "wal/dstore_wal_logstream.h"
#include "common/error/dstore_error.h"

namespace DSTORE {
inline RetStatus ControlWalInfo::AddWalStreamInfo(const ControlWalStreamPageItemData &streamInfo)
{
    uint32 maxItemSize = sizeof(ControlWalStreamPageItemData);
    return AddOneItem(static_cast<const char *>(static_cast<const void *>(&streamInfo)), maxItemSize,
                      CONTROL_WAL_STREAM_DATAPAGE_TYPE);
}

RetStatus ControlWalInfo::AddWalStream(const ControlWalStreamPageItemData &streamInfo)
{
    StorageAssert(m_isInitialized);
    if (STORAGE_FUNC_FAIL(m_lock->Lock(CFLockMode::CF_EXCLUSIVE))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Acquire control file lock fail."));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_EXCLUSIVE};
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }

    if (STORAGE_FUNC_FAIL(AddWalStreamInfo(streamInfo))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("group(%hhu) add wal stream info fail.", GetGroupType()));
        return DSTORE_FAIL;
    }
    if (STORAGE_FUNC_FAIL(PostGroup())) {
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus ControlWalInfo::UpdateWalStream(const ControlWalStreamPageItemData &streamInfo)
{
    StorageAssert(m_isInitialized);
    if (STORAGE_FUNC_FAIL(m_lock->Lock(CFLockMode::CF_EXCLUSIVE))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Acquire control file lock fail."));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_EXCLUSIVE};
    return UpdateWalStreamInternal(streamInfo);
}

RetStatus ControlWalInfo::UpdateWalStreamForCheckPoint(const ControlWalStreamPageItemData &streamInfo)
{
    WalId walId = streamInfo.walId;
    if (STORAGE_FUNC_FAIL(m_lock->Lock(CFLockMode::CF_EXCLUSIVE))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Acquire control file lock fail."));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_EXCLUSIVE};
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }

    uint16 itemSize = sizeof(ControlWalStreamPageItemData);
    ControlPageIterator<ControlWalStreamPageItemData> iterator{this, CONTROLFILE_PAGEMAP_WALSTREAM_START, itemSize};
    bool isExist = false;

    while (iterator.NextItem()) {
        ControlWalStreamPageItemData *itemData = iterator.GetItem();
        if (STORAGE_VAR_NULL(itemData)) {
            return DSTORE_FAIL;
        }
        if (itemData->walId == walId) {
            isExist = true;
            itemData->lastCheckpointPLsn = streamInfo.lastCheckpointPLsn;
            itemData->lastWalCheckpoint = streamInfo.lastWalCheckpoint;
            errno_t rc = memcpy_s(&itemData->checkpointHistory, sizeof(WalCheckPointHistory),
                                  &streamInfo.checkpointHistory, sizeof(WalCheckPointHistory));
            storage_securec_check(rc, "\0", "\0");
            MarkPageDirty(iterator.GetCurrentBlock());
            break;
        }
    }

    if (!isExist) {
        ErrLog(DSTORE_WARNING, MODULE_WAL,
               ErrMsg("Do not found wal stream %lu when update for checkpoint with diskRecoveryPlsn:%lu.", walId,
                      streamInfo.lastWalCheckpoint.diskRecoveryPlsn));
        return DSTORE_FAIL;
    }
    if (STORAGE_FUNC_FAIL(PostGroup())) {
        ErrLog(
            DSTORE_ERROR, MODULE_WAL,
            ErrMsg("Post write control file fail when update wal stream %lu for checkpoint with diskRecoveryPlsn:%lu.",
                   walId, streamInfo.lastWalCheckpoint.diskRecoveryPlsn));
        return DSTORE_FAIL;
    }
    ErrLog(DSTORE_LOG, MODULE_WAL,
           ErrMsg("Update wal stream %lu for checkpoint with diskRecoveryPlsn:%lu success.", walId,
                  streamInfo.lastWalCheckpoint.diskRecoveryPlsn));
    return DSTORE_SUCC;
}

RetStatus ControlWalInfo::UpdateWalStreamForCheckPointWithBarrier(const ControlWalStreamPageItemData &streamInfo)
{
    WalId walId = streamInfo.walId;
    if (STORAGE_FUNC_FAIL(m_lock->Lock(CFLockMode::CF_EXCLUSIVE))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Acquire control file lock fail."));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_EXCLUSIVE};
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }

    uint16 itemSize = sizeof(ControlWalStreamPageItemData);
    ControlPageIterator<ControlWalStreamPageItemData> iterator{this, CONTROLFILE_PAGEMAP_WALSTREAM_START, itemSize};
    bool isExist = false;

    while (iterator.NextItem()) {
        ControlWalStreamPageItemData *itemData = iterator.GetItem();
        if (STORAGE_VAR_NULL(itemData)) {
            return DSTORE_FAIL;
        }
        if (itemData->walId == walId) {
            isExist = true;
            itemData->lastCheckpointPLsn = streamInfo.lastCheckpointPLsn;
            itemData->lastWalCheckpoint = streamInfo.lastWalCheckpoint;
            errno_t rc = memcpy_s(&itemData->checkpointHistory, sizeof(WalCheckPointHistory),
                                  &streamInfo.checkpointHistory, sizeof(WalCheckPointHistory));
            storage_securec_check(rc, "\0", "\0");
            itemData->barrier = streamInfo.barrier;
            MarkPageDirty(iterator.GetCurrentBlock());
            break;
        }
    }

    if (!isExist) {
        ErrLog(DSTORE_WARNING, MODULE_WAL,
               ErrMsg("Do not found wal stream %lu when update for checkpoint with diskRecoveryPlsn:%lu.", walId,
                      streamInfo.lastWalCheckpoint.diskRecoveryPlsn));
        return DSTORE_FAIL;
    }
    if (STORAGE_FUNC_FAIL(PostGroup())) {
        ErrLog(
            DSTORE_ERROR, MODULE_WAL,
            ErrMsg("Post write control file fail when update wal stream %lu for checkpoint with diskRecoveryPlsn:%lu.",
                   walId, streamInfo.lastWalCheckpoint.diskRecoveryPlsn));
        return DSTORE_FAIL;
    }
    ErrLog(DSTORE_LOG, MODULE_WAL,
           ErrMsg("Update wal stream %lu for checkpoint with diskRecoveryPlsn:%lu success.", walId,
                  streamInfo.lastWalCheckpoint.diskRecoveryPlsn));
    return DSTORE_SUCC;
}

RetStatus ControlWalInfo::DeleteWalStream(WalId walId)
{
    StorageAssert(m_isInitialized);

    /* acquire exclusive file lock, make sure the file will not extend or change by others */
    if (STORAGE_FUNC_FAIL(m_lock->Lock(CFLockMode::CF_EXCLUSIVE))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Acquire control file lock fail."));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_EXCLUSIVE};
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }

    if (STORAGE_FUNC_FAIL(DeleteWalStreamInfo(walId))) {
        return DSTORE_FAIL;
    }

    if (STORAGE_FUNC_FAIL(PostGroup())) {
        return DSTORE_FAIL;
    }
    ErrLog(DSTORE_LOG, MODULE_CONTROL, ErrMsg("DeleteWalStream %lu in control success.", walId));
    return DSTORE_SUCC;
}

uint32 ControlWalInfo::GetAllWalStreams(WalId **walIdArray, WalStreamItemFilter filter)
{
    LatencyStat::Timer timer(&FrameworkPerfUnit::GetInstance().m_getAllWalStreams);

    UNUSE_PARAM AutoMemCxtSwitch autoSwitch{thrd->m_memoryMgr->GetGroupContext(MEMORY_CONTEXT_LONGLIVE)};
    if (m_lock->Lock(CFLockMode::CF_SHARE) == DSTORE_FAIL) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("GetAllWalStreams acquire control file lock fail."));
        return 0;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_SHARE};

    return GetAllWalStreamsWithoutLock(walIdArray, filter);
}

uint32 ControlWalInfo::GetAllWalStreamsWithoutLock(WalId **walIdArray, WalStreamItemFilter filter)
{
    LatencyStat::Timer timerIterate(&FrameworkPerfUnit::GetInstance().m_controlPageIterate);
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return 0;
    }
    uint32 count = 0;
    uint16 itemSize = sizeof(ControlWalStreamPageItemData);
    ControlPageIterator<ControlWalStreamPageItemData> iterator{this, CONTROLFILE_PAGEMAP_WALSTREAM_START, itemSize};

    while (iterator.NextItem()) {
        count++;
    }
    if (count == 0) {
        return 0;
    }

    uint32 index = 0;
    iterator = ControlPageIterator<ControlWalStreamPageItemData>{this, CONTROLFILE_PAGEMAP_WALSTREAM_START, itemSize};
    *walIdArray = static_cast<WalId *>(DstorePalloc(sizeof(WalId) * count));
    while (STORAGE_VAR_NULL(*walIdArray)) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("GetAllWalStreams alloc memory for walIdArray fail, retry it"));
        GaussUsleep(WAL_WAIT_MEMORY_AVAILABLE_TIME);
        *walIdArray = static_cast<WalId *>(DstorePalloc(sizeof(WalId) * count));
    }
    while (iterator.NextItem()) {
        ControlWalStreamPageItemData *itemData = iterator.GetItem();
        StorageAssert(index < count);

        if ((likely(itemData != nullptr))) {
            if (filter == nullptr || filter(*itemData)) {
                (*walIdArray)[index++] = itemData->walId;
            }
        } else {
            break;
        }
    }
    return index;
}

RetStatus ControlWalInfo::GetWalStreamInfo(WalId walId, ControlWalStreamPageItemData **streamInfo)
{
    StorageAssert(streamInfo != nullptr);
    StorageAssert(m_isInitialized);

    /* acquire share file lock, make sure the file will not extend or change by others */
    if (m_lock->Lock(CFLockMode::CF_SHARE) == DSTORE_FAIL) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Acquire control file lock fail."));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_SHARE};
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }
    return GetWalStreamInfoBaseOnWalId(walId, streamInfo);
}

RetStatus ControlWalInfo::GetWalStreamInfoBaseOnWalId(WalId walId, ControlWalStreamPageItemData **streamInfo)
{
    uint16 itemSize = sizeof(ControlWalStreamPageItemData);
    ControlPageIterator<ControlWalStreamPageItemData> iterator{this, CONTROLFILE_PAGEMAP_WALSTREAM_START, itemSize};

    while (iterator.NextItem()) {
        ControlWalStreamPageItemData *itemData = iterator.GetItem();
        if (STORAGE_VAR_NULL(itemData)) {
            StorageAssert(0); /* should not be null. */
            return DSTORE_FAIL;
        }
        if (itemData->walId == walId) {
            *streamInfo = static_cast<ControlWalStreamPageItemData *>(DstorePalloc0(itemSize));
            if (STORAGE_VAR_NULL(*streamInfo)) {
                ErrLog(DSTORE_ERROR, MODULE_CONTROL,
                       ErrMsg("GetWalStreamInfoBaseOnWalId for Wal:%lu, out of memory when palloc streaminfo.", walId));
                storage_set_error(CONTROL_ERROR_MEMORY_NOT_ENOUGH);
                return DSTORE_FAIL;
            }
            ControlWalStreamPageItemData::Copy(**streamInfo, *itemData);
            return DSTORE_SUCC;
        }
    }
    ErrLog(DSTORE_ERROR, MODULE_CONTROL,
           ErrMsg("GetWalStreamInfoBaseOnWalId for Wal:%lu, find wal stream item fail.", walId));
    return DSTORE_FAIL;
}

RetStatus ControlWalInfo::CreateAndAllocateOneWalStream(ControlWalStreamPageItemData **walStreamItemData,
    WalId walId)
{
    StorageAssert(m_isInitialized);
    if (STORAGE_FUNC_FAIL(m_lock->Lock(CFLockMode::CF_EXCLUSIVE))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Acquire control file lock fail."));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_EXCLUSIVE};
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }
    uint8_t state = (walId == INVALID_WAL_ID ? static_cast<uint8_t>(WalStreamState::CREATING)
                                             : static_cast<uint8_t>(WalStreamState::USING));
    if (STORAGE_FUNC_FAIL(GenerateWalStreamNoLock(walStreamItemData, state, walId))) {
        return DSTORE_FAIL;
    }

    /* If wal streams info exists in control file, there is no need write wal stream. */
    ControlWalStreamPageItemData *itemData;
    if (STORAGE_FUNC_SUCC(GetWalStreamInfoBaseOnWalId((*walStreamItemData)->walId, &itemData))) {
        /* write metapage */
        if (STORAGE_FUNC_FAIL(PostGroup())) {
            return DSTORE_FAIL;
        }
        ErrLog(DSTORE_LOG, MODULE_CONTROL,
           ErrMsg("Get exist walid %lu in control success.", (*walStreamItemData)->walId));
        FreeWalStreamsInfo(itemData);
        return DSTORE_SUCC;
    }

    RetStatus retStatus = AddWalStreamInfo(**walStreamItemData);
    if (retStatus != DSTORE_SUCC) {
        DstorePfreeExt(*walStreamItemData);
        storage_set_error(WAL_ERROR_UNREACHABLE_CODE);
        ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("ControlFile AddWalStream fail."));
        return DSTORE_FAIL;
    }
    if (STORAGE_FUNC_FAIL(PostGroup())) {
        return DSTORE_FAIL;
    }
    ErrLog(DSTORE_LOG, MODULE_CONTROL,
           ErrMsg("CreateAndAllocateOneWalStream %lu in control success.", (*walStreamItemData)->walId));
    return DSTORE_SUCC;
}

RetStatus ControlWalInfo::GenerateWalStreamNoLock(ControlWalStreamPageItemData **walStreamPageItemData,
                                                  uint8_t initState, WalId walId)
{
    if (walStreamPageItemData == nullptr) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Invalid input walStreamPageItemData."));
        return DSTORE_FAIL;
    }
    ControlWalInfoMeta *metadata = static_cast<ControlWalInfoMeta *>(GetMetaData());
    if (metadata == nullptr) {
        storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
        return DSTORE_FAIL;
    }
    ControlWalStreamPageItemData *itemData =
        static_cast<ControlWalStreamPageItemData *>(DstorePalloc0(sizeof(ControlWalStreamPageItemData)));
    if (STORAGE_VAR_NULL(itemData)) {
        storage_set_error(CONTROL_ERROR_MEMORY_NOT_ENOUGH);
        return DSTORE_FAIL;
    }
    /* walId starts from 0 in a new pdb */
    if (metadata->m_maxWalId == UINT64_MAX) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Allocate WalStreamId failed for metaPage maxWalId = UINT64_MAX."));
        DstorePfreeExt(itemData);
        return DSTORE_FAIL;
    }
    if (walId != INVALID_WAL_ID) {
        itemData->walId = walId;
    } else {
        itemData->walId = metadata->m_maxWalId + 1;
    }

    itemData->version  = 0;
    itemData->streamState = initState;
    WalCheckPoint walCheckPoint = {};
    itemData->lastWalCheckpoint = walCheckPoint;
    itemData->createFilePara.streamId = itemData->walId;
    itemData->createFilePara.flag = APPEND_WRITE_FILE;
    itemData->createFilePara.fileSubType = WAL_LOG_FILE_TYPE;
    itemData->createFilePara.rangeSize = WAL_FILE_RANGE_SIZE;
    if (IsTemplate(m_pdbId)) {
        itemData->createFilePara.maxSize = TEMPLATE_WAL_FILE_SIZE;
        itemData->walFileSize = TEMPLATE_WAL_FILE_SIZE;
        itemData->initWalFileCount = TEMPLATE_WAL_FILE_COUNT;
    } else {
        itemData->createFilePara.maxSize = static_cast<uint64>(g_storageInstance->GetGuc()->walFileSize);
        itemData->walFileSize = static_cast<uint32>(g_storageInstance->GetGuc()->walFileSize);
        itemData->initWalFileCount = static_cast<uint16>(g_storageInstance->GetGuc()->walFileNumber);
    }
    itemData->createFilePara.recycleTtl = 0;
    itemData->createFilePara.mode = FILE_READ_AND_WRITE_MODE;
    itemData->createFilePara.isReplayWrite = false;
    TenantConfig *tenantConfig = g_storageInstance->GetGuc()->tenantConfig;
    StorageAssert(tenantConfig);
    errno_t rc = strcpy_s(itemData->createFilePara.storeSpaceName, STORESPACE_NAME_MAX_LEN,
                          tenantConfig->storeSpaces[0].storeSpaceName);
    storage_securec_check(rc, "\0", "\0");

    if (walId != INVALID_WAL_ID) {
        metadata->m_maxWalId = DstoreMax(itemData->walId, metadata->m_maxWalId);
    } else {
        metadata->m_maxWalId = itemData->walId;
    }
    MarkPageDirty(CONTROLFILE_PAGEMAP_WALSTREAM_META);

    *walStreamPageItemData = itemData;
    return DSTORE_SUCC;
}

RetStatus ControlWalInfo::UpdateWalStreamState(WalId walId, const uint8 state)
{
    StorageAssert(m_isInitialized);
    if (STORAGE_FUNC_FAIL(m_lock->Lock(CFLockMode::CF_EXCLUSIVE))) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg(
            "Acquire control file lock fail, walId %lu, pbdId %u.", walId, m_pdbId));
        return DSTORE_FAIL;
    }
    ControlFileLockAutoUnlock autoUnlock{m_lock, CFLockMode::CF_EXCLUSIVE};
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }

    uint16 itemSize = sizeof(ControlWalStreamPageItemData);
    ControlPageIterator<ControlWalStreamPageItemData> iterator{this, CONTROLFILE_PAGEMAP_WALSTREAM_START, itemSize};
    bool isExist = false;
    bool isUpdated = false;

    while (iterator.NextItem()) {
        ControlWalStreamPageItemData *itemData = iterator.GetItem();
        if (STORAGE_VAR_NULL(itemData)) {
            return DSTORE_FAIL;
        }
        if (itemData->walId == walId) {
            isExist = true;
            if (state == static_cast<uint8>(WalStreamState::SYNC_DONE) &&
                itemData->streamState > static_cast<uint8>(WalStreamState::USING)) {
                /* do not rollback WalStreamState. */
                ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg(
                    "Current wal stream %lu state is %u, no need update to %u, pbdId %u.",
                    walId, static_cast<uint32>(itemData->streamState), static_cast<uint32>(state), m_pdbId));
            } else {
                itemData->streamState = state;
                isUpdated = true;
            }
            MarkPageDirty(iterator.GetCurrentBlock());
            break;
        }
    }

    if (!isExist) {
        ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg(
            "Do not found wal stream %lu when update state to %u, pbdId %u.",
            walId, static_cast<uint32>(state), m_pdbId));
        return DSTORE_FAIL;
    }
    if (STORAGE_FUNC_FAIL(PostGroup())) {
        ErrLog(DSTORE_ERROR, MODULE_WAL,
               ErrMsg("Post write control file fail when update wal stream %lu state to %u, pbdId %u.", walId,
                      static_cast<uint32>(state), m_pdbId));
        return DSTORE_FAIL;
    }
    if (isUpdated) {
        ErrLog(DSTORE_LOG, MODULE_WAL, ErrMsg(
            "Update wal stream %lu state to %u success, pbdId %u.", walId, static_cast<uint32>(state), m_pdbId));
    }
    return DSTORE_SUCC;
}

RetStatus ControlWalInfo::UpdateWalStreamInternal(const ControlWalStreamPageItemData &streamInfo)
{
    if (unlikely(LoadGroup() == DSTORE_FAIL)) {
        return DSTORE_FAIL;
    }

    if (STORAGE_FUNC_FAIL(DeleteWalStreamInfo(streamInfo.walId))) {
        if (StorageGetErrorCode() != CONTROL_ERROR_ITEM_NOT_FIND) {
            return DSTORE_FAIL;
        }
    }

    if (STORAGE_FUNC_FAIL(AddWalStreamInfo(streamInfo)) || STORAGE_FUNC_FAIL(PostGroup())) {
        return DSTORE_FAIL;
    }
    return DSTORE_SUCC;
}

RetStatus ControlWalInfo::DeleteWalStreamInfo(WalId walId)
{
    BlockNumber curBlockNumber = CONTROLFILE_PAGEMAP_WALSTREAM_START;
    uint16 itemSize = sizeof(ControlWalStreamPageItemData);
    ControlPageIterator<ControlWalStreamPageItemData> iterator{this, CONTROLFILE_PAGEMAP_WALSTREAM_START, itemSize};

    /* Read ControlPage one by one */
    while (iterator.NextItem()) {
        ControlWalStreamPageItemData *itemData = iterator.GetItem();
        if (unlikely(itemData == nullptr)) {
            return DSTORE_FAIL;
        }
        if (itemData->walId == walId) {
            size_t itemLength = sizeof(ControlWalStreamPageItemData);
            ControlPage *controlPage = GetPage(iterator.GetCurrentBlock());
            if (unlikely(controlPage == nullptr)) {
                return DSTORE_FAIL;
            }
            RetStatus rc = controlPage->RemoveItem(static_cast<size_t>(iterator.GetCurrentOffset()), itemLength);
            iterator.MarkItemDelete();
            BlockNumber lastPage = GetLastPageBlockNumber();
            curBlockNumber = iterator.GetCurrentBlock();
            SetLastPageBlockNumber(DstoreMin(curBlockNumber, lastPage));
            MarkPageDirty(curBlockNumber);
            return rc;
        }
    }

    storage_set_error(CONTROL_ERROR_ITEM_NOT_FIND);
    return DSTORE_FAIL;
}

void ControlWalInfo::FreeWalStreamsInfoArray(ControlWalStreamPageItemData **walStreamInfoArray,
                                             uint32 streamCount) const
{
    if (walStreamInfoArray == nullptr) {
        return;
    }
    for (uint32 i = 0; i < streamCount; i++) {
        if (walStreamInfoArray[i] != nullptr) {
            DstorePfreeExt(walStreamInfoArray[i]);
        }
    }
    DstorePfreeExt(walStreamInfoArray);
}

void ControlWalInfo::FreeWalStreamsInfo(ControlWalStreamPageItemData *walStreamInfo) const
{
    if (walStreamInfo != nullptr) {
        DstorePfreeExt(walStreamInfo);
    }
}

RetStatus ControlWalInfo::Create()
{
    /* step 1, init metapage */
    ControlMetaPage *metaPage = GetMetaPage();
    if (unlikely(metaPage == nullptr)) {
        return DSTORE_FAIL;
    }
    metaPage->InitMetaPage(CONTROL_WAL_STREAM_METAPAGE_TYPE, sizeof(ControlWalInfoMeta));
    ControlWalInfoMeta metaData;
    metaData.m_version = 0;
    metaData.m_maxWalId = 0;
    if (unlikely(metaPage->SetMetaData(static_cast<void *>(&metaData), sizeof(ControlWalInfoMeta)) == DSTORE_FAIL)) {
        ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("Failed to set metadata(grouptype:%hhu).", GetGroupType()));
        return DSTORE_FAIL;
    }

    ControlMetaHeader *metaHeader = metaPage->GetControlMetaHeader();
    metaHeader->m_lastPageId = CONTROLFILE_PAGEMAP_WALSTREAM_START;
    ControlPageRange *pageRange = &metaHeader->m_pageRange[0];
    pageRange[0].m_start = CONTROLFILE_PAGEMAP_WALSTREAM_START;
    pageRange[0].m_end = CONTROLFILE_PAGEMAP_WALSTREAM_MAX;
    for (uint32 i = 1; i < CONTROL_MAX_PAGERANGE_NUM; i++) {
        pageRange[i].m_start = DSTORE_INVALID_BLOCK_NUMBER;
        pageRange[i].m_end = DSTORE_INVALID_BLOCK_NUMBER;
    }
    MarkPageDirty(CONTROLFILE_PAGEMAP_WALSTREAM_META);

    /* step 2, init sharedpage */
    ControlDataPage *page = GetPage(CONTROLFILE_PAGEMAP_WALSTREAM_START);
    if (unlikely(page == nullptr)) {
        return DSTORE_FAIL;
    }
    page->InitDataPage(CONTROL_WAL_STREAM_DATAPAGE_TYPE);
    MarkPageDirty(CONTROLFILE_PAGEMAP_WALSTREAM_START);

    return DSTORE_SUCC;
}
}  // namespace DSTORE
