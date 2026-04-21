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
 * dstore_control_file.h
 *  ControlFiles records tablespace metadata (files contained in a tablespace), and logStream information
 *  (storage location and recovery point for each LogStream)
 *
 *
 * IDENTIFICATION
 *        storage/include/control/dstore_control_file.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSTORE_DSTORE_CONTROL_FILE_H
#define DSTORE_DSTORE_CONTROL_FILE_H

#include "control/dstore_control_file_lock.h"
#include "control/dstore_control_file_mgr.h"
#include "control/dstore_control_pdbinfo.h"
#include "control/dstore_control_walinfo.h"
#include "control/dstore_control_relmap.h"
#include "control/dstore_control_csninfo.h"
#include "control/dstore_control_tablespace.h"
#include "control/dstore_control_logicrep.h"
#include "control/dstore_control_file_mgr.h"
#include "framework/dstore_instance_interface.h"
#include "common/error/dstore_error.h"
#include "errorcode/dstore_control_error_code.h"

namespace DSTORE {
constexpr const char* DATABASE_CONTROL_FILE_1_NAME = "database_control_1";
constexpr const char* DATABASE_CONTROL_FILE_2_NAME = "database_control_2";
constexpr uint16 INVALID_ELEMENT_OFFSET = 0xFFFF;
constexpr uint16 INVALID_ELEMENT_SIZE = 0xFFFF;
constexpr uint16 CONTROL_FILE_HEADER_SIZE = 512;
constexpr uint16 INIT_FILE_ELEMENT_ARRAY_SIZE = 8;

using WalStreamItemFilter = bool (*)(const ControlWalStreamPageItemData &walStreamItem);

enum CtrlFileMessageType : uint8 {
    CTRL_FILE_REQUEST_PAGE_BUFFER_INVALIDATION = 1, /* Broadcast to everyone when the control file gets modified. */
    CTRL_FILE_REQUEST_PDB_ROLE_MODE_INVALIDATION, /* During intra-cluster PDB status update,
        notify all nodes to invalidate cached PDB status */
    CTRL_FILE_REQUEST_PDB_ROLE_MODE_RELOAD, /* During intra-cluster PDB status update,
        notify all nodes to update cached PDB status */
    CTRL_FILE_REQUEST_PDB_SYNC_MODE_INVALIDATION, /* Notify all nodes to invalidate a cached standby pdb sync mode */
    CTRL_FILE_REQUEST_PDB_SYNC_MODE_RELOAD, /* Notify all nodes to reload a standby pdb sync mode */
    CTRL_FILE_REQUEST_PDB_RECYCLE_CSN_MIN_INVALIDATION, /* Notify all nodes to invalidate cached PDB recycleCsnMin */
    CTRL_FILE_REQUEST_PDB_RECYCLE_CSN_MIN_RELOAD, /* Notify all nodes to reload cached PDB recycleCsnMin */
};

struct CreateFileParameter {
    FileId fileId;
    FileParameter filePara;
    const char *filePath;
    const char *storeSpaceName;
};

class ControlFile : public BaseObject {
public:
    ControlFile(PdbId pdbId, VFSAdapter *vfs = nullptr, DeployType deployType =
        DeployType::CONTROL_FILE_DEPLOYTYPE_INVALID);

    ~ControlFile();

    DISALLOW_COPY_AND_MOVE(ControlFile);

    void GetDataFilePageItemCtid(FileId fileId, BlockNumber *blkno, OffsetNumber *offset)
    {
        m_controlTablespace->GetDataFilePageItemCtid(fileId, blkno, offset);
    }
    void GetTbsPageItemCtid(TablespaceId tablespaceId, BlockNumber *blkno, OffsetNumber *offset)
    {
        m_controlTablespace->GetTbsPageItemCtid(tablespaceId, blkno, offset);
    }

    RetStatus InvalidateFileId(FileId fileId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->InvalidateFileId(fileId);
    }

    RetStatus AllocAndCreateDataFile(FileId *fileId, TablespaceId tablespaceId,
        ExtentSize extentSize, bool needWal)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->AllocAndCreateDataFile(fileId, tablespaceId, extentSize, needWal);
    }

    RetStatus FreeDataFileId(FileId fileId, TablespaceId tablespaceId, uint16 slotId, Xid ddlXid, bool needWal)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->FreeDataFileId(fileId, tablespaceId, slotId, ddlXid, needWal);
    }

    RetStatus InitTbsId(TablespaceId tablespaceId, uint64 tbsMaxSize)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->InitTbsId(tablespaceId, tbsMaxSize);
    }

    RetStatus AllocTbsId(TablespaceId *tablespaceId, uint64 tbsMaxSize, bool needWal)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->AllocTbsId(tablespaceId, tbsMaxSize, needWal);
    }

    RetStatus GetDataFilePageItemData(FileId fileId, ControlDataFilePageItemData *filePageItem)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->GetDataFilePageItemData(fileId, filePageItem);
    }

    RetStatus GetALLDataFilePageItemDatasForTemp(ControlDataFilePageItemData *filePageItems, uint32 count)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->GetALLDataFilePageItemDatasForTemp(filePageItems, count);
    }

    RetStatus GetTbsPageItemData(TablespaceId tablespaceId, ControlTablespacePageItemData *tbsItem)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->GetTbsPageItemData(tablespaceId, tbsItem);
    }
    RetStatus FreeTbsId(TablespaceId tablespaceId, Xid ddlXid, bool needWal)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->FreeTbsId(tablespaceId, ddlXid, needWal);
    }
    RetStatus AlterTbsMaxSize(TablespaceId tablespaceId, uint64 tbsMaxSize, bool needWal)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->AlterTbsMaxSize(tablespaceId, tbsMaxSize, needWal);
    }

#ifdef UT
    ControlDataFilePageItemData *GetDataFilePageItemPtr(FileId fileId, BlockNumber *blkno)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlTablespace->GetDataFilePageItemPtr(fileId, blkno);
    }

    ControlTablespacePageItemData *GetTbsPageItemPtr(TablespaceId tablespaceId, BlockNumber *blkno)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlTablespace->GetTbsPageItemPtr(tablespaceId, blkno);
    }
#endif
    RetStatus AddFileIdToTbs(TablespaceId tablespaceId, FileId fileId, uint16 *slotId, bool needWal)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->AddFileIdToTbs(tablespaceId, fileId, slotId, needWal);
    }
    RetStatus FreeFileIdFromTbs(TablespaceId tablespaceId, FileId fileId, uint16 *slotId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->FreeFileIdFromTbs(tablespaceId, fileId, slotId);
    }

    RetStatus GetFilesFromTablespace(TablespaceId tablespaceId, dlist_head *fileIdList, uint32 *fileCount)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->GetFilesFromTablespace(tablespaceId, fileIdList, fileCount);
    }
    RetStatus GetFilesFromAllTablespace(dlist_head *fileIdList, uint32 *fileCount)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->GetFilesFromAllTablespace(fileIdList, fileCount);
    }

    void FreeFileIdList(dlist_head *fileIdListHead)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return;
        }
        m_controlTablespace->FreeFileIdList(fileIdListHead);
    }
    RetStatus UpdateCreateTablespace(TablespaceId tablespaceId, Xid ddlXid, uint64 tbsMaxSize, uint64 preReuseVersion)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->UpdateCreateTablespace(tablespaceId, ddlXid, tbsMaxSize, preReuseVersion);
    }
    RetStatus UpdateCreateDataFile(TablespaceId tablespaceId, FileId fileId, uint64 fileMaxSize, ExtentSize extentSize,
        uint64 preReuseVersion, Xid ddlXid)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->UpdateCreateDataFile(tablespaceId, fileId, fileMaxSize, extentSize,
                                                         preReuseVersion, ddlXid);
    }
    RetStatus UpdateAddFileToTbs(TablespaceId tablespaceId, FileId fileId, uint16 slotId, uint16 hwm,
        uint64 preReuseVersion, Xid ddlXid)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->UpdateAddFileToTbs(tablespaceId, fileId, slotId, hwm,
                                                       preReuseVersion, ddlXid);
    }
    RetStatus UpdateDropTablespace(TablespaceId tablespaceId, uint64 preReuseVersion, Xid ddlXid, uint16 hwm)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->UpdateDropTablespace(tablespaceId, preReuseVersion, ddlXid, hwm);
    }
    RetStatus UpdateDropDataFile(TablespaceId tablespaceId, FileId fileId, uint64 preReuseVersion,
        uint16 slotId, uint16 hwm)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->UpdateDropDataFile(tablespaceId, fileId, preReuseVersion, slotId, hwm);
    }
    RetStatus UpdateAlterTablespace(TablespaceId tablespaceId, uint64 tbsMaxSize, Xid ddlXid, uint64 preReuseVersion)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlTablespace->UpdateAlterTablespace(tablespaceId, tbsMaxSize, ddlXid, preReuseVersion);
    }

    ControlPdbInfo* GetCtlPdbInfo()
    {
        return m_controlPdbInfo;
    }

    RetStatus AllocPdbId(const char *pdbName, PdbId &pdbId, bool isSysPdb = false, const char* vfsName = nullptr)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->AllocPdbId(pdbName, pdbId, isSysPdb, vfsName);
    }

    RetStatus UpdatePdbItemData(PdbId pdbId, ControlPdbInfoPageItemData *pdbInfo, size_t pdbInfoLen)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbItemData(pdbId, pdbInfo, pdbInfoLen);
    }

    RetStatus GetPdbRoleMode(PdbId pdbId, PdbRoleMode &pdbRoleMode)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetPdbRoleMode(pdbId, pdbRoleMode);
    }

    RetStatus UpdatePdbRoleMode(PdbId pdbId, PdbRoleMode pdbRoleMode)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbRoleMode(pdbId, pdbRoleMode);
    }

    RetStatus GetPdbRecycleCsnMin(PdbId pdbId, CommitSeqNo &pdbRecycleCsnMin)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("[PdbRecycleCsnMin] ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetPdbRecycleCsnMin(pdbId, pdbRecycleCsnMin);
    }

    RetStatus UpdatePdbRecycleCsnMin(PdbId pdbId, CommitSeqNo pdbRecycleCsnMin)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("[PdbRecycleCsnMin] ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbRecycleCsnMin(pdbId, pdbRecycleCsnMin);
    }

    RetStatus UpdatePdbReplicaStatus(PdbId pdbId, PdbReplicaRebuildProgress newPdbReplicaStatus)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbReplicaStatus(pdbId, newPdbReplicaStatus);
    }

    RetStatus GetPdbReplicaStatus(PdbId pdbId, PdbReplicaRebuildProgress &rebuildProgress)
    {
        if (unlikely(!m_initialized)) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetPdbReplicaStatus(pdbId, rebuildProgress);
    }

    RetStatus UpdatePdbSwitchStatus(PdbId pdbId, PdbSwitchStatus newPdbSwitchStatus)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbSwitchStatus(pdbId, newPdbSwitchStatus);
    }

    RetStatus GetPdbSwitchStatus(PdbId pdbId, PdbSwitchStatus &pdbSwitchStatus)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetPdbSwitchStatus(pdbId, pdbSwitchStatus);
    }

    RetStatus GetStandbyPdbInfo(PdbId pdbId, uint32 standbyClusterId, StandbyPdbInfo &standbyPdbInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetStandbyPdbInfo(pdbId, standbyClusterId, standbyPdbInfo);
    }

    RetStatus UpdateStandbyPdbClusterName(PdbId pdbId, uint32 standbyClusterId, const char *clusterName)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdateStandbyPdbClusterName(pdbId, standbyClusterId, clusterName);
    }

    RetStatus UpdateStandbyPdbVfsName(PdbId pdbId, uint32 standbyClusterId, const char *vfsName)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdateStandbyPdbVfsName(pdbId, standbyClusterId, vfsName);
    }

    RetStatus UpdateStandbyPdbStoreSpaceName(PdbId pdbId, uint32 standbyClusterId, const char *storeSpaceName)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdateStandbyPdbStoreSpaceName(pdbId, standbyClusterId, storeSpaceName);
    }

    RetStatus UpdatePdbWalSyncMode(PdbId pdbId, uint32 standbyClusterId, PdbSyncMode syncMode,
        uint32 walSizeThreshold = INVALID_WAL_SIZE_THRESHOLD, bool updateActualSyncMode = false)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbWalSyncMode(pdbId, standbyClusterId, syncMode, walSizeThreshold,
            updateActualSyncMode);
    }

    RetStatus UpdateStandbySyncModeAndWalSize(PdbId pdbId, PdbSyncMode syncMode, uint32 walSizeThreshold)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdateStandbySyncModeAndWalSize(pdbId, syncMode, walSizeThreshold);
    }

    ControlPdbInfoPageItemData *GetDropablePdbInfo(const char *pdbName)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlPdbInfo->GetDropablePdbInfo(pdbName);
    }

    RetStatus RegisterNewStandby(PdbId pdbId, StandbyPdbInfo standbyPdbInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->RegisterNewStandby(pdbId, standbyPdbInfo);
    }

    RetStatus ClearStandbyPdbSlotByClusterId(PdbId pdbId, uint32 standbyClusterId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->ClearStandbyPdbSlotByClusterId(pdbId, standbyClusterId);
    }

    RetStatus SetDeleteFlag(PdbId pdbId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->SetDeleteFlag(pdbId);
    }

    RetStatus SetOpenedFlag(PdbId pdbId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->SetOpenedFlag(pdbId);
    }

    RetStatus FreePdbId(PdbId pdbId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->FreePdbId(pdbId);
    }

    RetStatus UpdateTotalPdbCount(bool isIncrease)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdateTotalPdbCount(isIncrease);
    }

    RetStatus GetPdbInfoById(PdbId pdbId, PdbInfo *pdbInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetPdbInfoById(pdbId, pdbInfo);
    }

    ControlPdbInfoPageItemData *GetPdbInfoById(PdbId pdbId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlPdbInfo->GetPdbInfoById(pdbId);
    }

    /**
     * Create ControlFile, the function is called when bootstrap
     * @param dataDir Specify where ControlFiles are stored.
     * @return
     */
    RetStatus Create(const char *dataDir);

    /**
     * Load the controlFile into memory, the function is called everytime database setup.
     * @param dataDir Specify where ControlFiles are stored.
     * @return
     */
    RetStatus Init(const char *dataDir);
    RetStatus InitFileMgrAndGroup(const char *dataDir);
    void DestroyFileMgrAndGroupMgr();

    /**
     * Add WalStream element.
     *
     * @param streamInfo is target element info
     * @return add result
     */
    RetStatus AddWalStream(const ControlWalStreamPageItemData &streamInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->AddWalStream(streamInfo);
    }

    /**
     * Update WalStream element.
     *
     * NOTE: if the WalStream element is not exist, then insert a new one.
     *
     * @param streamInfo is target element info
     * @return update result
     */
    RetStatus UpdateWalStream(const ControlWalStreamPageItemData &streamInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->UpdateWalStream(streamInfo);
    }

    /**
     * Update WalStream element for checkpoint usage.
     *
     * NOTE: This function only update partial elements for checkpoint usage. Checkpoint invokes rather than
     * UpdateWalStream to avoid concurrent updating problems. The fields lastCheckpointPLsn, lastWalCheckpoint and
     * checkpointHistory in `streamInfo` are persisted.
     *
     * @param streamInfo carries walId plus the new checkpoint state to persist
     * @return update result
     */
    RetStatus UpdateWalStreamForCheckPoint(const ControlWalStreamPageItemData &streamInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->UpdateWalStreamForCheckPoint(streamInfo);
    }

    /**
     * Update WalStream element for checkpoint usage with barrier.
     *
     * NOTE: This function only update partial elements for checkpoint usage. Checkpoint invokes rather than
     * UpdateWalStream to avoid concurrent updating problems. The fields lastCheckpointPLsn, lastWalCheckpoint,
     * checkpointHistory and barrier in `streamInfo` are persisted.
     *
     * @param streamInfo carries walId plus the new checkpoint state and barrier to persist
     * @return update result
     */
    RetStatus UpdateWalStreamForCheckPointWithBarrier(const ControlWalStreamPageItemData &streamInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->UpdateWalStreamForCheckPointWithBarrier(streamInfo);
    }
    /**
     * Delete WalStream element.
     *
     * NOTE: if the element is not exist, then return DSTORE_FAIL.
     *
     * @param walId is target WalStream wal Id
     * @return delete result
     */
    RetStatus DeleteWalStream(WalId walId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->DeleteWalStream(walId);
    }

    /**
     * Get all the WAL streams in the control file. Note the caller has to release the memory of walIdArray.
     *
     * @param[out] walIdArray the array of WAL IDs to be returned
     * @param[in] filter is user-defined filter
     * @return the number of active WAL streams
     */
    uint32 GetAllWalStreams(WalId **walIdArray, WalStreamItemFilter filter = nullptr)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return 0;
        }
        return m_controlWalInfo->GetAllWalStreams(walIdArray, filter);
    }

    /**
     * Get all the WAL streams in the control file without acquiring control file lock.
     * The control file lock is expected to be held by the caller separately.
     *
     * @param[out] walIdArray the array of WAL IDs to be returned
     * @param[in] filter is user-defined filter
     * @return the number of active WAL streams
     */
    uint32 GetAllWalStreamsWithoutLock(WalId **walIdArray, WalStreamItemFilter filter)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return 0;
        }
        return m_controlWalInfo->GetAllWalStreamsWithoutLock(walIdArray, filter);
    }

    /**
     * Get target WalStream info.
     *
     * @param walId is target WalStream ID
     * @param[out] streamInfo is pointer to fetched stream info, which is constructed and freed by caller,
     * @return get result
     */
    RetStatus GetWalStreamInfo(WalId walId, ControlWalStreamPageItemData **streamInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->GetWalStreamInfo(walId, streamInfo);
    }

    RetStatus SetMaxReservedCSN(CommitSeqNo csn, CommitSeqNo &newMaxReservedCsn)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlCsnInfo->SetMaxReservedCSN(csn, newMaxReservedCsn);
    }

    RetStatus GetMaxReservedCSN(CommitSeqNo &csn)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlCsnInfo->GetMaxReservedCSN(csn);
    }

    RetStatus SetUndoZoneMapSegmentId(const PageId segmentId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlCsnInfo->SetUndoZoneMapSegmentId(segmentId);
    }

    RetStatus GetUndoZoneMapSegmentId(PageId &segmentId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlCsnInfo->GetUndoZoneMapSegmentId(segmentId);
    }

    /**
     * Adds a systemTable item to the control file.
     *
     * @param sysTableOid the system table's oid
     * @param segmentId the first page of this system table
     * @return return DSTORE_SUCC if inited successfully, otherwise return DSTORE_FAIL.
     */
    RetStatus AddSysTableItem(const Oid sysTableOid, const PageId &segmentId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlRelmap->AddSysTableItem(sysTableOid, segmentId);
    }

    /**
     * Gets a systemTable item by systemTableOid in the control file.
     *
     * @param sysTableOid the system table's oid
     * @param segmentId the first page of this system table, this is a out param
     * @return return DSTORE_SUCC if inited successfully, otherwise return DSTORE_FAIL.
     */
    RetStatus GetSysTableItem(Oid sysTableOid, PageId &segmentId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlRelmap->GetSysTableItem(sysTableOid, segmentId);
    }

    /**
     * Get all the system tables in the control file.
     * @param type systable table type
     * @param systbItem Pointer to the control system table item data
     * @param count Reference to store the number of acquired system table items
     * @return Return the acquisition status, success or failure
     * @note This function is used to obtain all system table items and store the number in count.
     */
    RetStatus GetAllSysTableItem(int type, ControlSysTableItemData *systbItems, int &count)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlRelmap->GetAllSysTableItem(type, systbItems, count);
    }

    /**
     * Write all system table items
     * @param type systable table type
     * @param systbItems Pointer to system table item data
     * @param count The number of system table items to be written
     * @return Return writing status
     *
     * This function is used to write all system table item data to a specified location.
     */
    RetStatus WriteAllSysTableItem(int type, ControlSysTableItemData *systbItems, int &count)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlRelmap->WriteAllSysTableItem(type, systbItems, count);
    }

    void FreeWalStreamsInfoArray(ControlWalStreamPageItemData **walStreamInfoArray, uint32 streamCount) const
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return;
        }
        m_controlWalInfo->FreeWalStreamsInfoArray(walStreamInfoArray, streamCount);
    }

    void FreeWalStreamsInfo(ControlWalStreamPageItemData *walStreamInfo) const
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return;
        }
        m_controlWalInfo->FreeWalStreamsInfo(walStreamInfo);
    }

    bool IsInitialized() const
    {
        return m_initialized.load(std::memory_order_acquire);
    }

    /**
     * create and allocate one wal stream
     *
     * @param[out] walStream is alloacted wal stream
     * @return DSTORE_SUCC if create successfully, otherwise return DSTORE_FAIL.
     */
    RetStatus CreateAndAllocateOneWalStream(ControlWalStreamPageItemData **walStreamItemData, WalId walId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->CreateAndAllocateOneWalStream(walStreamItemData, walId);
    }

    RetStatus AddLogicalReplicationSlot(ControlLogicalReplicationSlotPageItemData *data)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlLogicRep->AddLogicalReplicationSlot(data);
    }

    RetStatus DeleteLogicalReplicationSlot(char *slotName, WalId walId, bool &isExist)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlLogicRep->DeleteLogicalReplicationSlot(slotName, walId, isExist);
    }

    RetStatus UpdateLogicalReplicationSlot(ControlLogicalReplicationSlotPageItemData *data)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlLogicRep->UpdateLogicalReplicationSlot(data);
    }

    RetStatus GetAllLogicalReplicationSlotBaseOnWalId(WalId targetWalId,
                                                      ControlLogicalReplicationSlotPageItemData **repSlotinfo,
                                                      int &slotCountNum)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlLogicRep->GetAllLogicalReplicationSlotBaseOnWalId(targetWalId, repSlotinfo, slotCountNum);
    }

    inline BlockNumber GetBlockCount() const
    {
        return m_controlFileMgr->GetBlockCount();
    }

    ControlPage *GetPage(BlockNumber blockNumber);

    ControlPdbInfoPageItemData *GetPdbInfo(const char *pdbName, PdbStatus pdbStatus)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlPdbInfo->GetPdbInfo(pdbName, pdbStatus);
    }

    ControlPdbInfoPageItemData *GetPdbInfo(PdbId pdbId, PdbStatus pdbStatus)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlPdbInfo->GetPdbInfo(pdbId, pdbStatus);
    }

    ControlPdbInfoPageItemData *GetPdbInfo(const char *pdbName)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlPdbInfo->GetPdbInfo(pdbName);
    }

    ControlPdbInfoPageItemData *GetAvailablePdbInfo(const char *pdbName)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlPdbInfo->GetAvailablePdbInfo(pdbName);
    }

    ControlPdbInfoPageItemData *GetAvailablePdbInfo(PdbId pdbId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return nullptr;
        }
        return m_controlPdbInfo->GetAvailablePdbInfo(pdbId);
    }

    FUNCTION_DELETE_LATER
    RetStatus GetPdbIdByName(const char *pdbName, PdbId *pdbId, PdbStatus pdbStatus)
    {
        return m_controlPdbInfo->GetPdbIdByName(pdbName, pdbId, pdbStatus);
    }

    RetStatus GetPdbIdByUuid(const char *pdbUuid, PdbId *pdbId, PdbStatus pdbStatus)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetPdbIdByUuid(pdbUuid, pdbId, pdbStatus);
    }

    RetStatus GetAvailablePdbIdByUuid(const char *pdbUuid, PdbId *pdbId)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetAvailablePdbIdByUuid(pdbUuid, pdbId);
    }

    RetStatus IsPdbInRestoreFromBackup(const char *pdbName, bool *isInRestoreFromBackup)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->IsPdbInRestoreFromBackup(pdbName, isInRestoreFromBackup);
    }
    RetStatus UpdatePdbInRestoreFromBackup(PdbId pdbId, bool isPdbInRestoreFromBackup)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbInRestoreFromBackup(pdbId, isPdbInRestoreFromBackup);
    }

    RetStatus IsPdbInFullRepair(const char *pdbName, bool *isFullRepair)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->IsPdbInFullRepair(pdbName, isFullRepair);
    }
    RetStatus UpdatePdbFullRepairStatus(PdbId pdbId, bool isFullRepair)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdatePdbFullRepairStatus(pdbId, isFullRepair);
    }

    PdbId GetPdbId()
    {
        return m_pdbId;
    }

    uint64 GetFileSize()
    {
        return m_controlFileMgr->GetFileSize();
    }

    inline void SetWalStreamReusable(bool canWalStreamReusable)
    {
        m_controlWalInfo->SetWalStreamReusable(canWalStreamReusable);
    }

    inline bool CanWalStreamReusable()
    {
        return m_controlWalInfo->CanWalStreamReusable();
    }

    RetStatus UpdateAllPdbInfoAfterRestore(void *allPdbInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->UpdateAllPdbInfoAfterRestore(allPdbInfo);
    }

    RetStatus RectifyPdbInfoAfterL2Restore()
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->RectifyPdbInfoAfterL2Restore();
    }

    RetStatus GetAllPdbIdForBackupRestore(PdbId *pdbIdArray, uint32 &count)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetAllPdbIdForBackupRestore(pdbIdArray, count);
    }

    inline void CleanPageValid(uint32 blockNumber)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            ErrLog(DSTORE_WARNING, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return;
        }
        m_controlFileMgr->CleanPageStateValid(blockNumber);
    }

    static RetStatus InvalidatePdbProc(ControlFile *controlFile, const void* data, uint32 dataLen)
    {
        if (!controlFile) {
            return DSTORE_SUCC;
        }
        return controlFile->InvalidatePdbInfo(data, dataLen);
    }
    RetStatus InvalidatePdbInfo(const void* data, uint32 dataLen)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->InvalidatePdbInfo(data, dataLen);
    }

    void InvalidatePdbIdxIfNeed(BlockNumber blkno)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return;
        }
        m_controlPdbInfo->InvalidatePdbIdxIfNeed(blkno);
    }

    static RetStatus InvalidateRelMapProc(ControlFile *controlFile, const void* data, uint32 dataLen)
    {
        if (!controlFile) {
            return DSTORE_SUCC;
        }
        return controlFile->InvalidateRelmapInfo(data, dataLen);
    }
    RetStatus InvalidateRelmapInfo(const void* data, uint32 dataLen)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlRelmap->InvalidateRelmapInfo(data, dataLen);
    }

    RetStatus UpdateWalStreamState(WalId walId, const uint8 state)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->UpdateWalStreamState(walId, state);
    }

    RetStatus UpdateWalStreamInternal(const ControlWalStreamPageItemData &streamInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->UpdateWalStreamInternal(streamInfo);
    }

    RetStatus GetWalStreamInfoBaseOnWalId(WalId walId, ControlWalStreamPageItemData **streamInfo)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlWalInfo->GetWalStreamInfoBaseOnWalId(walId, streamInfo);
    }

    inline void SetEnableCachePage(bool isEnable)
    {
        if (!m_initialized.load(std::memory_order_acquire)) {
            /* In openAllPdbPhase1 scenarios, the control file not be initialized. */
            m_enableCachePage.store(isEnable, std::memory_order_release);
            ErrLog(DSTORE_LOG, MODULE_CONTROL, ErrMsg("ControlFile is not initialized when set enable cache page."));
            return;
        }
        m_controlFileMgr->SetEnableCachePage(isEnable);
    }

    inline RetStatus GetControlFileSizeVfs(UNUSE_PARAM int64 &partSize)
    {
        return DSTORE_SUCC;
    }

    inline RetStatus ReadPdbCxtFromCtrlFile(PdbId pdbId)
    {
        /* The caller may be running in parallel with StartupInstance,
         * at which point communication has been initialized, but the
         * control file of rootPdb has not yet been loaded.
         */
        while (!m_initialized.load(std::memory_order_acquire)) {
            GaussUsleep(1000);
        }
        return m_controlPdbInfo->ReadPdbCxt(pdbId);
    }

    inline RetStatus GetVisiablePdbInfoAll(PdbBriefInfo *pdbIdInfos, uint32_t *pdbCount)
    {
        if (unlikely(!m_initialized.load(std::memory_order_acquire))) {
            storage_set_error(CONTROL_ERROR_NOT_INITIALIZED);
            ErrLog(DSTORE_ERROR, MODULE_CONTROL, ErrMsg("ControlFile is not initialized."));
            return DSTORE_FAIL;
        }
        return m_controlPdbInfo->GetVisiablePdbInfoAll(pdbIdInfos, pdbCount);
    }

    RetStatus ExtendFile()
    {
        return DSTORE_SUCC;
    }

    RetStatus CopyControlFile(ControlFile *srcControlFile);
    RetStatus OpenControlFile(char *dataDir);
    RetStatus LoadControlFile(bool reload = false);

#ifndef UT
private:
#endif
    VFSAdapter *m_vfs;
    DstoreMemoryContext m_ctx;
    PdbId m_pdbId;
    std::atomic<bool> m_initialized;
    std::atomic<bool> m_canWalStreamReusable;
    std::atomic<bool> m_enableCachePage;
    class ControlPdbInfo *m_controlPdbInfo;
    class ControlRelmap *m_controlRelmap;
    class ControlCsnInfo *m_controlCsnInfo;
    class ControlTablespace *m_controlTablespace;
    class ControlWalInfo *m_controlWalInfo;
    class ControlLogicRep *m_controlLogicRep;
    class ControlFileMgr *m_controlFileMgr;
    DeployType m_deployType;
};
}  // namespace DSTORE

#endif  // DSTORE_STORAGE_CONTROL_FILE_H
