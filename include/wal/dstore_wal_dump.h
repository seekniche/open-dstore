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
 * dstore_wal_dump.h
 *
 * Description:
 * Define class WalDumper, mainly support read and dump wal record in given range.
 * ---------------------------------------------------------------------------------------
 */
#ifndef DSTORE_DSTORE_WAL_DUMP_H
#define DSTORE_DSTORE_WAL_DUMP_H

#include "common/dstore_datatype.h"
#include "wal/dstore_wal_dump_file_reader.h"
#include "wal/dstore_wal_reader.h"

namespace DSTORE {

enum class ModuleType {
    HEAP,
    INDEX,
    BTREERECYCLE,
    TABLESPACE,
    CHECKPOINT,
    UNDO,
    SYSTABLE
};
struct ModuleDescData {
    ModuleType type;
    const char *name;
    WalType recordTypeLowerBound;
    WalType recordTypeUpperBound;
};

struct WalDumpStreamReader {
    WalDumpFileReader *walDumpFileReader;
    WalRecordReader *walRecordReader;
    uint64 startPlsn;
    uint64 endPlsn;
    WalDumpWalFileInfo walFileInfo;
};

enum class DumpType {
    DUMP_ONE_STREAM,
    DUMP_ONE_PAGE,
    CHECK_ONE_PAGE
};

struct XidFilter {
    uint64 zoneId;
    uint64 logicSlotId;
};

struct WalDumpConfig {
    uint32 timelineId;
    StorageType vfsType;
    bool reuseVfs;
    VFSAdapter *vfs;
    char dir[MAXPGPATH];
    char *vfsConfigPath;
    PdbId pdbId;
    char *pdbVfsName;
    bool dumpWalAfterCheckpoint;
    WalId walId;
    uint32 walFileSize;
    uint64 startPlsn;
    uint64 endPlsn;
    uint64 recordNumPerInputLimit;
    uint64 displayedRecordNum;
    uint32 moduleFilter;
    uint32 typeFilter;
    XidFilter xidFilter;
    PageId pageIdFilter;
    WalDumpCommandType commandType;
    char dumpDir[MAXPGPATH];
    PageDiagnose::DumpCommConfig commConfig;
    bool checkPageError;
    /*
     * When true, DumpOneWalStream prints one line per valid wal group header
     * instead of decoding records, and prints the final last_valid_group_end_plsn.
     * Set by `waldump --scan-valid-checkpoint`.
     */
    bool scanValidCheckpoint;
};

enum class WalDumpGetRecordType {
    LATEST_RECORD = 0,
    TARGET_RECORD
};

struct WalDumpGetRecordParam {
    WalDumpGetRecordType getType;
    void *args;
};

struct WalDumpRecordHashKey {
    WalId walId;
    uint64 endPlsn;
};

struct WalDumpRecordHashEntry {
    WalDumpRecordHashKey recordKey;
    uint64 groupStartPlsn;
    WalRecord *record;
    WalRecordAtomicGroup group;
};

struct WalDumpErrorRecordHashEntry {
    WalDumpRecordHashKey recordKey;
    WalDumpRecordHashKey currRecordKey;
    uint64 groupStartPlsn;
    WalRecord *record;
    WalRecordAtomicGroup group;
};

struct PageErrorInfo {
    WalDumpRecordHashKey prevRecordKey;
    WalDumpRecordHashKey errorRecordKey_1;
    WalDumpRecordHashKey errorRecordKey_2;
};


constexpr uint16 MAX_MODULE_ID = 7;
const ModuleDescData MODULE_DESC_TABLE[MAX_MODULE_ID] = {
    {ModuleType::HEAP, "heap", WAL_HEAP_INSERT, WAL_HEAP_FORCE_UPDATE_TUPLE_DATA_NO_TRX},
    {ModuleType::INDEX, "index", WAL_BTREE_BUILD, WAL_BTREE_ERASE_INS_FOR_DEL_FLAG},
    {ModuleType::BTREERECYCLE, "btreerecycle", WAL_BTREE_RECYCLE_PARTITION_INIT_PAGE,
        WAL_BTREE_RECYCLE_ROOT_META_SET_PARTITION_META},
    {ModuleType::TABLESPACE, "tablespace", WAL_TBS_INIT_BITMAP_META_PAGE, WAL_TBS_ALTER_TABLESPACE},
    {ModuleType::CHECKPOINT, "checkpoint", WAL_CHECKPOINT_SHUTDOWN, WAL_CHECKPOINT_ONLINE},
    {ModuleType::UNDO, "undo", WAL_UNDO_INIT_MAP_SEGMENT, WAL_TXN_ABORT},
    {ModuleType::SYSTABLE, "systable", WAL_SYSTABLE_WRITE_BUILTIN_RELMAP, WAL_SYSTABLE_WRITE_BUILTIN_RELMAP}
};
constexpr uint64 WAL_DUMP_INVALID_PLSN = UINT64_MAX;
constexpr uint32 WAL_DUMP_INVALID_FILTER = UINT32_MAX;
constexpr uint64 WAL_DUMP_INVALID_XID_FILTER = UINT64_MAX;
constexpr uint32 WAL_DUMP_INIT_TEMP_BUFF_SIZE = 10240;
constexpr WalDumpRecordHashKey WAL_DUMP_INVALID_RECORD_KEY = {UINT64_MAX, UINT64_MAX};

#pragma GCC visibility push(default)
class WalDumper {
public:
    static RetStatus InitWalDumpConfig(WalDumpConfig &config);

    static RetStatus DumpByConfig(WalDumpConfig *config);

    static void DumpToLocalFile(WalDumpConfig *config, StringInfoData *dumpInfo);

    static RetStatus StartErrorLog(bool *loggerAlreadyStarted);

    static DumpType GetDumpType(WalDumpConfig *config);

    explicit WalDumper(const WalDumpConfig *config);

    ~WalDumper();

    RetStatus Init(WalId *walIdArray, uint32 *walFileSizeArray, uint32 walIdCount);

    void Destroy();

    RetStatus Dump(DumpType dumpType, WalId *walIdArray, uint32 *walFileSizeArray, uint32 walIdCount);

    const WalRecord *DecompressProc(const WalRecord *walRecord);

private:

    /* Init functions */
    RetStatus InitPlsn(uint64 checkpointStartPlsn, uint64 minPlsn, uint64 maxPlsn, uint64 *startPlsn,
        uint64 *endPlsn);
    RetStatus InitFileReader(uint64 checkpointPlsn, WalId walId, uint32_t walFileSize, WalDumpFileReader **fileReader);
    RetStatus InitRecordReader(WalDumpFileReader *fileReader, uint64 *startPlsn, uint64 fileSize,
        WalRecordReader **recordReader);
    bool CheckPlsn(uint64 minPlsn, uint64 maxPlsn, uint64 *startPlsn, uint64 *endPlsn);
    void AdjustStartPlsnByCheckpoint(uint64 *startPlsn, uint64 checkpointPlsn);

    uint64 FindValidGroupStartPlsn(uint64 *startPlsn, WalDumpFileReader *fileReader, WalRecordReader *recordReader);

    /* Display functions */
    void DisplayRecord(const WalRecord *record, uint64 recordEndPlsn, Xid xid);
    void DisplayGroupInfo(uint64 groupStartPlsn, const WalRecordAtomicGroup *group) const;
    bool CheckFilterInvalid(const WalRecord *walRecord);
    bool CheckRecordFilter(const WalRecord *walRecord);
    bool CheckGroupFilter(const WalRecordAtomicGroup *walGroup);
    bool WalGroupContainXid(const WalRecordAtomicGroup *group, const XidFilter xid);
    bool CheckInputRecordNumExceedLimit();
    bool WalRecordContainPageId(const WalRecord *walRecord, const PageId pageId);

    void ReAllocBuffForDecompress(uint32 newBuffSize, uint32 oldBuffSize);

    /* Dump stream functions */
    RetStatus DumpOneWalStream();

    /* Dump page functions */
    RetStatus DumpOnePage();
    RetStatus DumpOnePageWithCheck();
    RetStatus GetAllRecordWithPageFilter();
    void SetPageError(WalDumpRecordHashKey prevRecord, WalDumpRecordHashKey curRecord);
    RetStatus DumpWalList(WalDumpRecordHashKey prevRecord, WalDumpRecordHashKey curRecord);
    void GetPrevRecordInfo(const WalRecord *record, WalId *prevWalId, uint64 *prevEndPlsn);
    RetStatus GetTargetRecordEntry(WalId recordWalId, uint64 recordEndPlsn, WalDumpRecordHashEntry **entry);
    bool WalFileAllIsRemain(WalFileHeaderData &fileHeader);
    RetStatus ReadWalFileAndGetTargetRecord(WalDumpStreamReader *reader, uint64 fileStartPlsn,
        const uint64 targetEndPlsn, WalDumpRecordHashEntry **entry);
    RetStatus GetLatesetRecordEntry(WalDumpRecordHashEntry **entry);
    bool RecordIsNewer(WalDumpRecordHashEntry *record, WalDumpRecordHashEntry *comparedRecord);
    uint64 GetPrevGlsn(const WalRecord *record);
    RetStatus ReadWalFileAndGetLatesetRecord(WalDumpStreamReader *reader, uint64 targetEndPlsn,
        WalDumpRecordHashEntry **entry);
    RetStatus ReadWalFileAndGetEntry(WalDumpStreamReader *reader, uint64 fileStartPlsn, uint64 endPlsn,
        WalDumpGetRecordParam *getRecordParam, WalDumpRecordHashEntry **entry);
    RetStatus ReadGroupAndGetEntry(WalDumpStreamReader *reader, const WalRecordAtomicGroup *walGroup,
        WalDumpGetRecordParam *getRecordParam, WalDumpRecordHashEntry **entry);
    RetStatus PushRecordToHashTable(WalDumpRecordHashKey recordKey, uint64 groupStartPlsn,
        const WalRecordAtomicGroup *walGroup, const WalRecord *walRecord, WalDumpRecordHashEntry **entry);
    RetStatus PushRecordToPrevHashTable(WalDumpRecordHashKey prevRecordKey, WalDumpRecordHashKey currRecordKey,
                                        uint64 groupStartPlsn, const WalRecordAtomicGroup *walGroup,
                                        const WalRecord *walRecord);

    /* Hash table functions */
    RetStatus HashInit();
    WalDumpRecordHashEntry *HashAdd(WalDumpRecordHashKey recordKey, uint64 groupStartPlsn,
        const WalRecordAtomicGroup *walGroup, WalRecord *record);
    WalDumpRecordHashEntry *HashGet(WalDumpRecordHashKey recordKey);
    void HashDelete(WalDumpRecordHashKey recordKey);
    void HashDestroy();
    static RetStatus GenerateDumpFileName(WalDumpConfig *config, const char *dirPath, char *outputFileName);
    static RetStatus SetWalDumpPath(WalDumpConfig *config);

    uint32 m_walStreamCount;
    WalDumpStreamReader *m_walStreamReader;

    WalDumpConfig m_config;

    bool m_inited;
    DstoreMemoryContext m_memoryContext;
    BuffForDecompress m_bufferForDecompress;

    HTAB *m_recordHtab;
    HTAB *m_prevRecordHtab;
    PageErrorInfo m_pageErrorInfo;
};
#pragma GCC visibility pop
}
#endif
