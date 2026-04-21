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
 * dstore_wal_struct.h
 *
 * Description:
 * Wal public header file, contains all Wal Struct definition exposed to caller.
 * ---------------------------------------------------------------------------------------
 */
#ifndef DSTORE_WAL_STRUCT_H
#define DSTORE_WAL_STRUCT_H

#include "common/error/dstore_error.h"
#include "common/log/dstore_log.h"
#include "common/dstore_datatype.h"
#include "common/dstore_common_utils.h"
#include "common/algorithm/dstore_type_compress.h"
#include "framework/dstore_modules.h"
#include "tuple/dstore_data_tuple.h"
#include "tuple/dstore_index_tuple.h"
#include "undo/dstore_undo_types.h"
#include "page/dstore_page.h"
#include "dstore_wal_utils.h"
#include "buffer/dstore_buf_mgr.h"
#include "errorcode/dstore_wal_error_code.h"
#include "undo/dstore_transaction_slot.h"
#include "page/dstore_data_page.h"
#include "common/algorithm/dstore_checksum_impl.h"
#include "common/memory/dstore_memory_allocator_stack.h"
#include "common/memory/dstore_mctx.h"
#include "port/dstore_port.h"

namespace DSTORE {
#define WAL_FILE_HDR_SIZE MAXALIGN(sizeof(WalFileHeaderData))
#define WAL_BLCKSZ BLCKSZ /* keep the same as BLCKSZ in GaussdbServer, because the unit of guc wal_buffers is BLCKSZ */

constexpr uint32 WAL_FILE_HEAD_MAGIC = 0xD2A8F347;
/* when no file reuse, the version of file will always equals to 1, no need to record it in WAL */
constexpr uint64 EXCLUDED_FILE_VERSION = 1;
constexpr uint64 EXCLUDED_TBS_VERSION = 1;

enum WalType : uint16 {
    /* Heap wal type */
    WAL_HEAP_INSERT,
    WAL_HEAP_BATCH_INSERT,
    WAL_HEAP_DELETE,
    WAL_HEAP_INPLACE_UPDATE,
    WAL_HEAP_SAME_PAGE_APPEND,
    WAL_HEAP_ANOTHER_PAGE_APPEND_UPDATE_NEW_PAGE,
    WAL_HEAP_ANOTHER_PAGE_APPEND_UPDATE_OLD_PAGE,
    WAL_HEAP_ALLOC_TD,
    WAL_HEAP_PRUNE,
    WAL_HEAP_FORCE_UPDATE_TUPLE_DATA_NO_TRX,

    /* Index wal type */
    WAL_BTREE_BUILD,
    WAL_BTREE_INIT_META_PAGE,
    WAL_BTREE_UPDATE_META_ROOT,
    WAL_BTREE_UPDATE_LOWEST_SINGLE_PAGE,
    WAL_BTREE_NEW_INTERNAL_ROOT,
    WAL_BTREE_NEW_LEAF_ROOT,
    WAL_BTREE_INSERT_ON_INTERNAL,
    WAL_BTREE_INSERT_ON_LEAF,
    WAL_BTREE_SPLIT_INTERNAL,
    WAL_BTREE_SPLIT_LEAF,
    WAL_BTREE_SPLIT_INSERT_INTERNAL,
    WAL_BTREE_SPLIT_INSERT_LEAF,
    WAL_BTREE_NEW_INTERNAL_RIGHT,
    WAL_BTREE_NEW_LEAF_RIGHT,
    WAL_BTREE_DELETE_ON_INTERNAL,
    WAL_BTREE_DELETE_ON_LEAF,
    WAL_BTREE_PAGE_PRUNE,
    WAL_BTREE_ALLOC_TD,
    WAL_BTREE_UPDATE_LIVESTATUS,
    WAL_BTREE_UPDATE_SPLITSTATUS,
    WAL_BTREE_UPDATE_LEFT_SIB_LINK,
    WAL_BTREE_UPDATE_RIGHT_SIB_LINK,
    WAL_BTREE_UPDATE_DOWNLINK,
    WAL_BTREE_ERASE_INS_FOR_DEL_FLAG,
    /* Btree recycle queue wal type */
    WAL_BTREE_RECYCLE_PARTITION_INIT_PAGE,
    WAL_BTREE_RECYCLE_PARTITION_PUSH,
    WAL_BTREE_RECYCLE_PARTITION_BATCH_PUSH,
    WAL_BTREE_RECYCLE_PARTITION_POP,
    WAL_BTREE_RECYCLE_PARTITION_ALLOC_SLOT,
    WAL_BTREE_RECYCLE_PARTITION_WRITE_SLOT,
    WAL_BTREE_RECYCLE_QUEUE_PAGE_META_SET_NEXT,
    WAL_BTREE_RECYCLE_PARTITION_META_INIT,
    WAL_BTREE_RECYCLE_PARTITION_META_SET_HEAD,
    WAL_BTREE_RECYCLE_PARTITION_META_TIMESTAMP_UPDATE,
    WAL_BTREE_RECYCLE_ROOT_META_INIT,
    WAL_BTREE_RECYCLE_ROOT_META_SET_PARTITION_META,

    /* Tablespace wal type */
    WAL_TBS_INIT_BITMAP_META_PAGE,
    WAL_TBS_INIT_TBS_FILE_META_PAGE,
    WAL_TBS_INIT_TBS_SPACE_META_PAGE,
    WAL_TBS_UPDATE_TBS_FILE_META_PAGE,
    WAL_TBS_INIT_ONE_BITMAP_PAGE,
    WAL_TBS_INIT_BITMAP_PAGES,
    WAL_TBS_ADD_BITMAP_PAGES,
    WAL_TBS_BITMAP_ALLOC_BIT_START,
    WAL_TBS_BITMAP_ALLOC_BIT_END,
    WAL_TBS_BITMAP_FREE_BIT_START,
    WAL_TBS_BITMAP_FREE_BIT_END,
    WAL_TBS_EXTEND_FILE,
    WAL_TBS_INIT_UNDO_SEGMENT_META,
    WAL_TBS_INIT_DATA_SEGMENT_META,
    WAL_TBS_INIT_HEAP_SEGMENT_META,
    WAL_TBS_INIT_FSM_META,
    WAL_TBS_INIT_FSM_PAGE,
    WAL_TBS_INIT_EXT_META,
    WAL_TBS_MODIFY_EXT_META_NEXT,
    WAL_TBS_MODIFY_FSM_INDEX,
    WAL_TBS_ADD_FSM_SLOT,
    WAL_TBS_MOVE_FSM_SLOT,
    WAL_TBS_SEG_ADD_EXT,
    WAL_TBS_DATA_SEG_ADD_EXT,
    WAL_TBS_SEG_META_ASSIGN_DATA_PAGES,
    WAL_TBS_SEG_UNLINK_EXT,
    WAL_TBS_SEG_META_ADD_FSM_TREE,
    WAL_TBS_SEG_META_RECYCLE_FSM_TREE,
    WAL_TBS_SEG_META_ADJUST_DATA_PAGES_INFO,
    WAL_TBS_FSM_META_UPDATE_FSM_TREE,
    WAL_TBS_FSM_META_UPDATE_NUM_USED_PAGES,
    WAL_TBS_FSM_META_UPDATE_EXTENSION_STAT,
    WAL_TBS_INIT_ONE_DATA_PAGE,
    WAL_TBS_INIT_MULTIPLE_DATA_PAGES,
    WAL_TBS_UPDATE_FIRST_FREE_BITMAP_PAGE,
    /* Tablespace DDL wal type */
    WAL_TBS_CREATE_TABLESPACE,
    WAL_TBS_CREATE_DATA_FILE,
    WAL_TBS_ADD_FILE_TO_TABLESPACE,
    WAL_TBS_DROP_TABLESPACE,
    WAL_TBS_DROP_DATA_FILE,
    WAL_TBS_ALTER_TABLESPACE,

    /* Checkpoint wal type */
    WAL_CHECKPOINT_SHUTDOWN,
    WAL_CHECKPOINT_ONLINE,

    /* Undo wal type */
    WAL_UNDO_INIT_MAP_SEGMENT,
    WAL_UNDO_SET_ZONE_SEGMENT_ID,
    WAL_UNDO_INSERT_RECORD,
    WAL_UNDO_INIT_RECORD_SPACE,
    WAL_UNDO_EXTEND_PAGE_RING_PREV_PAGE,
    WAL_UNDO_EXTEND_PAGE_RING_NEXT_PAGE,
    WAL_UNDO_EXTEND_PAGE_RING_NEW_PAGE,
    WAL_UNDO_INIT_TXN_PAGE,
    WAL_UNDO_UPDATE_TXN_SLOT_PTR,
    WAL_UNDO_ALLOCATE_TXN_SLOT,
    WAL_UNDO_SET_TXN_PAGE_INITED,
    WAL_UNDO_HEAP,
    WAL_UNDO_BTREE,
    WAL_UNDO_HEAP_PAGE_ROLL_BACK,
    WAL_UNDO_BTREE_PAGE_ROLL_BACK,
    WAL_UNDO_RECYCLE_TXN_SLOT,
    /* Transaction wal type */
    WAL_TXN_COMMIT,
    WAL_TXN_ABORT,

    /* Logical replication needed wal type */
    WAL_NEXT_CSN,

    /* barrier wal type */
    WAL_BARRIER_CSN,

    /* systable wal type */
    WAL_SYSTABLE_WRITE_BUILTIN_RELMAP,

#ifdef UT
    /* wal reserved wal type */
    WAL_EMPTY_REDO,
    WAL_EMPTY_DDL_REDO,
#endif

    /* add new enum above this */
    WAL_TYPE_BUTTOM
};

struct MemoryCheckpoint {
    uint64 term;
    uint32 memoryNodeCnt;
    uint64 memRecoveryPlsn; /* memory Recovery Plsn where have flush to memory node where we can reply redo log start
                               from */

    MemoryCheckpoint &operator=(const MemoryCheckpoint &memCheckpoint)
    {
        if (this == &memCheckpoint) {
            return *this;
        }
        term = memCheckpoint.term;
        memoryNodeCnt = memCheckpoint.memoryNodeCnt;
        memRecoveryPlsn = memCheckpoint.memRecoveryPlsn;
        StorageAssert(memCheckpoint.memoryNodeCnt <= REMOTE_MEMORY_NODE_MAX);
        return *this;
    }
};

/*
 * Body of Checkpoint WalRecord.
 */
struct WalCheckPoint {
    Timestamp time;
    uint64 diskRecoveryPlsn; /* disk Recovery Plsn where have flushed to disk and we can reply redo log start from */
    MemoryCheckpoint memoryCheckpoint;
};

/*
 * Ring buffer of recent WalCheckPoint entries.
 *
 * Recovery falls back to an older slot when the newest checkpoint plsn fails
 * to parse a valid wal group; WAL recycling honors the oldest slot to make sure
 * fall-back targets are physically still present on disk.
 *
 * Invariants (maintained by the checkpoint write path):
 *   - slots[newestSlotIdx] always equals the most recent checkpoint.
 *   - slotCount monotonically grows from 0 up to WAL_CHECKPOINT_HISTORY_SLOTS.
 *   - slotCount == 0 means "no history yet" (fresh stream or legacy upgrade).
 */
constexpr uint8 WAL_CHECKPOINT_HISTORY_SLOTS = 3;

struct WalCheckPointHistory {
    uint8 slotCount;
    uint8 newestSlotIdx;
    uint16 _pad;
    WalCheckPoint slots[WAL_CHECKPOINT_HISTORY_SLOTS];

    /* k = 0 returns the newest slot, k = slotCount-1 the oldest. */
    inline uint8 GetSlotIdxByAge(uint8 k) const
    {
        return static_cast<uint8>((newestSlotIdx + WAL_CHECKPOINT_HISTORY_SLOTS - k) %
                                  WAL_CHECKPOINT_HISTORY_SLOTS);
    }

    /* Returns INVALID_PLSN (0) when no valid slot is available. */
    inline uint64 GetOldestDiskRecoveryPlsn() const
    {
        if (slotCount == 0) {
            return 0;
        }
        uint64 oldest = UINT64_MAX;
        for (uint8 i = 0; i < slotCount; ++i) {
            if (slots[i].diskRecoveryPlsn < oldest) {
                oldest = slots[i].diskRecoveryPlsn;
            }
        }
        return oldest;
    }

    /* Append `cp` as the new newest slot; rotates over the oldest one once full. */
    inline void Push(const WalCheckPoint &cp)
    {
        uint8 nextSlot = (slotCount == 0) ? 0
                                          : static_cast<uint8>((newestSlotIdx + 1) %
                                                               WAL_CHECKPOINT_HISTORY_SLOTS);
        slots[nextSlot] = cp;
        newestSlotIdx = nextSlot;
        if (slotCount < WAL_CHECKPOINT_HISTORY_SLOTS) {
            slotCount++;
        }
    }
};

/*
 * Barrier info for control info.
 */
struct WalBarrier {
    CommitSeqNo barrierCsn;
    uint64 barrierEndPlsn;
    PdbSyncMode barrierSyncMode;
};

/*
 * Representing one detail log record inserted -by AtomicWalWriterContext::put_new_log_entry
 */
/* The maximum size added after walRecord compression */
constexpr uint16 MAX_WAL_RECORD_FOR_PAGE_COMPRESSED_SIZE =
    (COMPRESSED32_MAX_BYTE - sizeof(uint32)) * 2 + (COMPRESSED64_MAX_BYTE - sizeof(uint64)) * 4;
constexpr uint16 MAX_WAL_RECORD_FOR_PAGE_DECOMPRESSED_SIZE = COMPRESSED32_MAX_BYTE * 2 + COMPRESSED64_MAX_BYTE * 4;
constexpr uint16 MAX_WAL_RECORD_SIZE = UINT16_MAX - MAX_WAL_RECORD_FOR_PAGE_COMPRESSED_SIZE;
constexpr uint32 MAX_PAGES_COUNT_PER_WAL_GROUP = 1030;
constexpr uint32 WAL_GROUP_MAX_SIZE = MAX_PAGES_COUNT_PER_WAL_GROUP * MAX_WAL_RECORD_SIZE;
constexpr uint16 MAX_UNDO_WAL_TYPE_SIZE = static_cast<uint16>(WAL_TXN_ABORT - WAL_UNDO_INIT_MAP_SEGMENT + 1);

struct WalRecord {
    uint16 m_size;
    WalType m_type;

    inline void SetType(WalType type)
    {
        m_type = type;
    }

    inline WalType GetType() const
    {
        return m_type;
    }

    inline void SetSize(Size size)
    {
        StorageAssert(size <= MAX_WAL_RECORD_SIZE);
        m_size = static_cast<uint16>(size);
    }

    inline uint16 GetSize() const
    {
        return m_size;
    }
} PACKED;

STATIC_ASSERT_TRIVIAL(WalRecord);
constexpr uint16 MIN_WAL_RECORD_SIZE = sizeof(WalRecord);

/*
 * Each time user call AtomicWalWriterContext::EndAtomicWal will insert one WalGroup,
 * containing multi WalRecords.
 */
struct WalRecordAtomicGroup {
    uint32 groupLen; /* total length of WalRecordGroup */
    uint32 crc; /* crc of WalGroup */
    Xid xid; /* transaction id */
    uint16 recordNum; /* element number of m_group_data */
    WalRecord walRecords[]; /* all log record data */
} PACKED;

STATIC_ASSERT_TRIVIAL(WalRecordAtomicGroup);

/*
 * Lsn info for one WalRecord.
 * Note:
 * 1. glsn only contained and useful in WalRecordForPage and sub struct, is meaningless otherwise
 * 2. this struct is usually used to describe WalRecord lsn info and will not write to disk so on
 */
struct WalRecordLsnInfo {
    WalId walId; /* wal id */
    uint64 endPlsn; /* end plsn of this Wal record */
    uint64 glsn; /* glsn of this Wal record */

    void SetValue(Page *page)
    {
        glsn = page->GetGlsn();
        endPlsn = page->GetPlsn();
        walId = page->GetWalId();
    }

    bool operator==(const WalRecordLsnInfo& val)
    {
        return (val.walId == this->walId) && (val.endPlsn == this->endPlsn) && (val.glsn == this->glsn);
    }

    bool operator>(const WalRecordLsnInfo &val) const
    {
        return (this->glsn > val.glsn) || (val.glsn == this->glsn && this->endPlsn > val.endPlsn);
    }
};

/*
 * Lsn info for one WalAtomicGroup.
 * Note:
 * 1. this struct is usually used to describe WalGroup lsn info and will not write to disk so on
 */
struct WalGroupLsnInfo {
    WalId m_walId; /* wal id */
    uint64 m_startPlsn; /* start plsn of this Wal group in corresponding WalFile */
    uint64 m_endPlsn; /* end plsn of this Wal group in corresponding WalFile */
};

extern const char* g_walTypeForPrint[WAL_TYPE_BUTTOM];

struct WalRecordFlag {
    uint8 glsnChangeFlag : 1;
    uint8 containLogicalInfoFlag : 1; /* wal constains logical replication info */
    uint8 decodeDictChangeFlag : 1; /* catalog change which logical decode interested in */
    uint8 heapDeleteContainsReplicaKeyFlag : 1; /* wal of heap tuple delete contains pkey */
    uint8 heapUpdateContainsReplicaKeyFlag : 1; /* wal of heap tuple update contains pkey */
    uint8 containFileVersionFlag : 1; /* wal constains file version */
    uint8 unused : 2;
};

struct WalPageHeaderContext {
    WalType type;
    uint32 size;
    PageId pageId;
    WalId preWalId;
    uint64 prePlsn;
    uint64 preGlsn;
    bool glsnChangedFlag;
    uint64 preVersion;
};

struct WalRecordForPage : public WalRecord {
    PageId m_pageId;
    union {
        uint8  m_placeHolder;
        WalRecordFlag m_flag;
    } m_flags;
    WalId m_pagePreWalId;
    uint64 m_pagePrePlsn;
    uint64 m_pagePreGlsn;
    uint64 m_filePreVersion;

    uint16 Compress(char *walRecordForPageOnDisk) const noexcept
    {
        char *tempdiskData = walRecordForPageOnDisk;
        StorageReleasePanic(m_size < sizeof(WalRecordForPage), MODULE_WAL, ErrMsg("Wal serialize m_size too small."));

        errno_t rc =
            memcpy_s(tempdiskData, sizeof(WalRecord),
                static_cast<const char*>(static_cast<const void*>(this)), sizeof(WalRecord));
        storage_securec_check(rc, "\0", "\0");
        tempdiskData += sizeof(WalRecord);

        uint8 thisSize = VarintCompress::CompressUnsigned32(m_pageId.m_fileId, tempdiskData);
        tempdiskData += thisSize;
        thisSize = VarintCompress::CompressUnsigned32(m_pageId.m_blockId, tempdiskData);
        tempdiskData += thisSize;

        rc = memcpy_s(tempdiskData, sizeof(m_flags),
            static_cast<const char*>(static_cast<const void*>(&(m_flags))), sizeof(m_flags));
        storage_securec_check(rc, "\0", "\0");
        tempdiskData += sizeof(m_flags);
        thisSize = VarintCompress::CompressUnsigned64(m_pagePreWalId, tempdiskData);
        tempdiskData += thisSize;
        thisSize = VarintCompress::CompressUnsigned64(m_pagePrePlsn, tempdiskData);
        tempdiskData += thisSize;
        thisSize = VarintCompress::CompressUnsigned64(m_pagePreGlsn, tempdiskData);
        tempdiskData += thisSize;
        if (m_flags.m_flag.containFileVersionFlag) {
            thisSize = VarintCompress::CompressUnsigned64(m_filePreVersion, tempdiskData);
            tempdiskData += thisSize;
        }

        return static_cast<uint16>(tempdiskData - walRecordForPageOnDisk);
    }

    uint16 Decompress(const WalRecord *origRecord) noexcept
    {
        const char *tempdiskData = static_cast<const char *>(static_cast<const void *>(origRecord));
        const char *start = tempdiskData;
        SetSize(origRecord->m_size);
        SetType(origRecord->m_type);
        tempdiskData += sizeof(WalRecord);

        uint8 thisSize;
        m_pageId.m_fileId = static_cast<uint16>(VarintCompress::DecompressUnsigned32(tempdiskData, thisSize));
        tempdiskData += thisSize;
        m_pageId.m_blockId = VarintCompress::DecompressUnsigned32(tempdiskData, thisSize);
        tempdiskData += thisSize;

        m_flags.m_placeHolder = *(static_cast<const uint8 *>(static_cast<const void *>(tempdiskData)));
        tempdiskData += sizeof(m_flags);

        m_pagePreWalId = VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
        tempdiskData += thisSize;
        m_pagePrePlsn = VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
        tempdiskData += thisSize;
        m_pagePreGlsn = VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
        tempdiskData += thisSize;
        if (m_flags.m_flag.containFileVersionFlag) {
            m_filePreVersion = VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
            tempdiskData += thisSize;
        } else {
            m_filePreVersion = EXCLUDED_FILE_VERSION;
        }

        uint16 compressSize = static_cast<uint16>(tempdiskData - start);
        StorageAssert(m_size >= compressSize);
        m_size = static_cast<uint16>((m_size - compressSize) + static_cast<uint16>(sizeof(WalRecordForPage)));
        return compressSize;
    }

    uint16 GetMaxCompressedSize() const noexcept
    {
        const uint8 uint32Count = 2;
        const uint8 uint64Count = 4;
        return (COMPRESSED32_MAX_BYTE - sizeof(uint32)) * uint32Count +
               (COMPRESSED64_MAX_BYTE - sizeof(uint64)) * uint64Count;
    }

    inline void SetWalPageHeader(const WalPageHeaderContext &pageHeader)
    {
        m_flags.m_placeHolder = 0;
        SetType(pageHeader.type);
        SetSize(pageHeader.size);
        SetPagePreWalInfo(pageHeader.preWalId, pageHeader.prePlsn, pageHeader.preGlsn);
        SetPageGlsnChangeFlag(pageHeader.glsnChangedFlag);
        SetFileVersionAndFlag(pageHeader.preVersion);
        m_pageId = pageHeader.pageId;
    }

    static inline void CopyData(char *dstBuf, uint32 destSize, const char *srcData, uint32 srcSize)
    {
        errno_t rc = memcpy_s(dstBuf, destSize, srcData, srcSize);
        storage_securec_check(rc, "\0", "\0");
    }
    inline void SetPagePreWalInfo(WalId walId, uint64 plsn)
    {
        m_pagePreWalId = walId;
        m_pagePrePlsn = plsn;
    }
    inline void SetPagePreWalInfo(WalId walId, uint64 plsn, uint64 glsn)
    {
        m_pagePreWalId = walId;
        m_pagePrePlsn = plsn;
        m_pagePreGlsn = glsn;
    }
    inline void SetPageGlsnChangeFlag(bool glsnChangedFlag)
    {
        if (glsnChangedFlag) {
            m_flags.m_flag.glsnChangeFlag = 1;
        } else {
            m_flags.m_flag.glsnChangeFlag = 0;
        }
    }
    inline void SetFileVersionAndFlag(uint64 fileVersion)
    {
        if (fileVersion == EXCLUDED_FILE_VERSION) {
            m_flags.m_flag.containFileVersionFlag = 0;
        } else {
            m_flags.m_flag.containFileVersionFlag = 1;
        }
        m_filePreVersion = fileVersion;
    }
    inline void Dump(FILE *fp) const
    {
        (void)fprintf(
            fp,
            "type:%s; len:%hu; pageId=(%hu, %u); page prev walId:%lu; page prev endPlsn:%lu; "
            "page prev glsn:%lu; file prev version:%lu; glsnChangeFlag:%s; ddlChangeFlag:%s; fileVersionFlag:%s; ",
            g_walTypeForPrint[static_cast<uint16>(m_type)], m_size, m_pageId.m_fileId, m_pageId.m_blockId,
            m_pagePreWalId, m_pagePrePlsn, m_pagePreGlsn, m_filePreVersion,
            m_flags.m_flag.glsnChangeFlag ? "true" : "false", m_flags.m_flag.decodeDictChangeFlag ? "true" : "false",
            m_flags.m_flag.containFileVersionFlag ? "true" : "false");
    }
} PACKED;

STATIC_ASSERT_TRIVIAL(WalRecordForPage);

struct WalRecordForPageOnDisk : public WalRecord {
/*
    compressData contains the content after compressed by order :
    |m_size|m_type|compressed m_pageId.m_fileId|compressed m_pageId.m_blockId|m_flags|
    |2Bytes|2Bytes|1Byte-----------------5Bytes|1Byte------------------5Bytes|1Byte  |
    |compressed m_pagePreWalId|compressed m_pagePrePlsn|compressed m_pagePreGlsn|compressed m_filePreVersion|
    |1Byte--------------5Bytes|1Byte------------10Bytes|1Byte------------10Bytes|1Byte---------------10Bytes|
*/
    char compressData[(sizeof(WalRecordForPage) - sizeof(WalRecord)) + MAX_WAL_RECORD_FOR_PAGE_COMPRESSED_SIZE];
    PageId GetCompressedPageId() const
    {
        switch (m_type) {
#ifdef UT
            case WAL_EMPTY_REDO:
#endif
            case WAL_TBS_INIT_ONE_DATA_PAGE:
            case WAL_TBS_INIT_ONE_BITMAP_PAGE: {
                PageId pageId;
                errno_t rc = memcpy_s(&pageId, sizeof(PageId), &compressData[0], sizeof(PageId));
                storage_securec_check(rc, "\0", "\0");
                return pageId;
            }
            case WAL_TBS_INIT_MULTIPLE_DATA_PAGES:
            case WAL_TBS_INIT_BITMAP_PAGES:
            case WAL_TBS_CREATE_TABLESPACE:
            case WAL_TBS_CREATE_DATA_FILE:
            case WAL_TBS_ADD_FILE_TO_TABLESPACE:
            case WAL_TBS_DROP_TABLESPACE:
            case WAL_TBS_DROP_DATA_FILE:
            case WAL_TBS_ALTER_TABLESPACE: {
                ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("Only for WalRecordForPage"));
                break;
            }
            default: {
                const char *tempdiskData = static_cast<const char*>(static_cast<const void*>(this));
                tempdiskData += sizeof(WalRecord);

                uint8 thisSize;
                PageId pageId;
                pageId.m_fileId = static_cast<uint16>(VarintCompress::DecompressUnsigned32(tempdiskData, thisSize));
                tempdiskData += thisSize;
                pageId.m_blockId = VarintCompress::DecompressUnsigned32(tempdiskData, thisSize);
                tempdiskData += thisSize;
                return pageId;
            }
        }
        return INVALID_PAGE_ID;
    }
    uint64 GetCompressedFilePreVersion() const
    {
        uint64 filePreVersion = EXCLUDED_FILE_VERSION;
        switch (m_type) {
#ifdef UT
            case WAL_EMPTY_REDO:
#endif
            case WAL_TBS_INIT_ONE_DATA_PAGE:
            case WAL_TBS_INIT_ONE_BITMAP_PAGE: {
                const WalRecordForPage *walRecordForPage =
                    static_cast<const WalRecordForPage *>(static_cast<const void *>(this));
                filePreVersion = walRecordForPage->m_filePreVersion;
                break;
            }
#ifdef UT
            case WAL_EMPTY_DDL_REDO:
#endif
            case WAL_TBS_CREATE_TABLESPACE:
            case WAL_TBS_CREATE_DATA_FILE:
            case WAL_TBS_ADD_FILE_TO_TABLESPACE:
            case WAL_TBS_DROP_TABLESPACE:
            case WAL_TBS_DROP_DATA_FILE:
            case WAL_TBS_ALTER_TABLESPACE: {
                const char *tempdiskData = static_cast<const char *>(static_cast<const void *>(this));
                tempdiskData += sizeof(WalRecord) + sizeof(TablespaceId);
                errno_t rc = memcpy_s(&filePreVersion, sizeof(uint64), tempdiskData, sizeof(uint64));
                storage_securec_check(rc, "\0", "\0");
                break;
            }
            case WAL_TBS_INIT_BITMAP_PAGES:
            case WAL_TBS_INIT_MULTIPLE_DATA_PAGES: {
                errno_t rc = memcpy_s(&filePreVersion, sizeof(uint64), &compressData[0], sizeof(uint64));
                storage_securec_check(rc, "\0", "\0");
                break;
            }
            default: {
                uint8 thisSize;
                const char *tempdiskData = static_cast<const char *>(static_cast<const void *>(this));
                tempdiskData += sizeof(WalRecord);
                // m_pageId.m_fileId
                VarintCompress::DecompressUnsigned32(tempdiskData, thisSize);
                tempdiskData += thisSize;
                // m_pageId.m_blockId
                VarintCompress::DecompressUnsigned32(tempdiskData, thisSize);
                tempdiskData += thisSize;
                // m_flags
                WalRecordFlag flag = *(static_cast<const WalRecordFlag *>(static_cast<const void *>(tempdiskData)));
                tempdiskData += sizeof(WalRecordForPage::m_flags);

                if (flag.containFileVersionFlag) {
                    // m_pagePreWalId
                    VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
                    tempdiskData += thisSize;
                    // m_pagePrePlsn
                    VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
                    tempdiskData += thisSize;
                    // m_pagePreGlsn
                    VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
                    tempdiskData += thisSize;
                    // m_filePreVersion
                    filePreVersion = VarintCompress::DecompressUnsigned64(tempdiskData, thisSize);
                } else {
                    filePreVersion = EXCLUDED_FILE_VERSION;
                }
                break;
            }
        }
        return filePreVersion;
    }
} PACKED;

struct WalRecordForDataPage : public WalRecordForPage {
public:
    struct AllocTdRecord {
        uint8 extendNum;
        char data[]; /* Store TrxSlotStatus, size is (tdNum) * sizeof(TrxSlotStatus) */
    } PACKED;
    /*
     * return size include :
     * extendNum(uint8), TrxSlotStatus(tdNum)
     */
    static inline uint32 GetAllocTdSize(const TDAllocContext &context)
    {
        return ((context.allocTd.isDirty) ?
            (sizeof(AllocTdRecord) + sizeof(TrxSlotStatus) * (context.allocTd.tdNum)) : 0);
    }

    /*
     * wal: the pointer of start position to record AllocTd Info
     * size: the buffer size
     */
    inline void SetAllocTdWal(TDAllocContext &context, char* wal, uint32 size) const
    {
        /* if page is not dirty , no need to record alloc td */
        if (!context.allocTd.isDirty) {
            return;
        }

        uint32 trxStatusSize = sizeof(TrxSlotStatus) * (context.allocTd.tdNum);
        StorageReleasePanic(size != (sizeof(AllocTdRecord) + trxStatusSize), MODULE_WAL,
            ErrMsg("Wal size(%u) not equal cal size(%lu).", size, sizeof(AllocTdRecord) + trxStatusSize));
        StorageReleasePanic(wal == nullptr, MODULE_WAL, ErrMsg("Input parm wal is nullptr."));

        AllocTdRecord* record = static_cast<AllocTdRecord*>(static_cast<void*>(wal));
        StorageAssert(record != nullptr);
        record->extendNum = context.allocTd.extendNum;
        errno_t rc = memcpy_s(record->data, trxStatusSize, context.allocTd.xidStatus, trxStatusSize);
        storage_securec_check(rc, "\0", "\0");
        context.allocTd.isDirty = false;
    }

    inline void RedoExtendTdWal(DataPage* page, uint8 extendNum) const
    {
        uint32 freeSpace = page->GetFreeSpace<FreeSpaceCondition::RAW>();
        uint8 tdNum = static_cast<uint8>(page->dataHeader.tdCount);
        uint8 numExtended = extendNum;
        StorageReleasePanic((tdNum + numExtended > MAX_TD_COUNT), MODULE_WAL,
            ErrMsg("cur page td num %hhu, extend td num %hhu error.", tdNum, numExtended));
        StorageReleasePanic((numExtended  * sizeof(TD) > freeSpace), MODULE_WAL,
            ErrMsg("cur page free space %u, extend td num %hhu error.", freeSpace, numExtended));

        /*
        * Move the line pointers ahead in the page to make room for
        * added transaction slots.
        */
        char *start = page->GetItemIdArrayStartPtr();
        char *end = page->GetItemIdArrayEndPtr();
        uint32 size = static_cast<uint32>(end - start);
        if (size != 0) {
            errno_t ret = memmove_s(start + (numExtended * sizeof(TD)), size, start, size);
            storage_securec_check(ret, "", "");
        }

        /* Initialize the new TD slots */
        for (uint8 i = tdNum; i < tdNum + numExtended; i++) {
            TD *td = page->GetTd(i);
            td->Reset();
        }

        /* Reinitialize number of TD slots and begining of free space in the header */
        page->dataHeader.tdCount = tdNum + numExtended;
        page->m_header.m_lower += numExtended * sizeof(TD);

        StorageAssert(page->CheckSanity());
    }
    /*
     * wal: the strat position of AllocTd Wal
     * size: the buffer size
     */
    inline void RedoAllocTdWal(DataPage* page, PageType pageType, const char* wal, uint32 size) const
    {
        /* if size is zero , there is no AllocTd Wal , return */
        if (size == 0) {
            return;
        }
        StorageReleasePanic(wal == nullptr, MODULE_WAL, ErrMsg("Input parm wal is nullptr."));

        const AllocTdRecord* record = static_cast<const AllocTdRecord*>(static_cast<const void*>(wal));
        uint8 tdNum = page->GetTdCount();
        uint32 calSize = sizeof(AllocTdRecord) + sizeof(TrxSlotStatus) * (tdNum);
        StorageReleasePanic(size != calSize, MODULE_WAL, ErrMsg("Wal size(%u) != cal size(%u).", size, calSize));

        /* get Td Recycle Info */
        AutoMemCxtSwitch autoMemCxtSwitch(thrd->m_memoryMgr->GetGroupContext(MEMORY_CONTEXT_LONGLIVE));
        TdRecycleStatus *tdRecycle = static_cast<TdRecycleStatus *>(DstorePalloc(sizeof(TdRecycleStatus) * tdNum));
        while (STORAGE_VAR_NULL(tdRecycle)) {
            ErrLog(DSTORE_ERROR, MODULE_WAL, ErrMsg("RedoAllocTdWal alloc memory for size %lu failed, retry it.",
                sizeof(TdRecycleStatus) * tdNum));
            GaussUsleep(WAL_WAIT_MEMORY_AVAILABLE_TIME);
            tdRecycle = static_cast<TdRecycleStatus *>(DstorePalloc(sizeof(TdRecycleStatus) * tdNum));
        }
        uint8 tdRecycleNum = 0;
        bool existTxnInProgress = false;
        GetTdRecycleInfo(page, record, tdRecycle, tdRecycleNum, existTxnInProgress);
        /* refresh Tuple Td status */
        if (tdRecycleNum > 0) {
            if (pageType == PageType::HEAP_PAGE_TYPE) {
                page->RefreshTupleTdStatus<HeapDiskTuple>(tdRecycleNum, tdRecycle);
            } else if (pageType == PageType::INDEX_PAGE_TYPE) {
                page->RefreshTupleTdStatus<IndexTuple>(tdRecycleNum, tdRecycle);
            } else {
                ErrLog(DSTORE_PANIC, MODULE_WAL, ErrMsg("Only for HEAP_PAGE_TYPE and INDEX_PAGE_TYPE"));
            }
            TdId tdId = page->GetAvailableTd(tdRecycleNum, tdRecycle, existTxnInProgress);
            if (tdId != INVALID_TD_SLOT) {
                TD* td = page->GetTd(tdId);
                td->SetCsn(INVALID_CSN);
                td->SetCsnStatus(IS_INVALID);
            }
        }
        DstorePfree(tdRecycle);

        /* ExtendTd */
        if (record->extendNum) {
            RedoExtendTdWal(page, record->extendNum);
        }
    }

    inline void DescAllocTd(FILE *fp, const char* wal, uint32 size) const
    {
        const AllocTdRecord* allocTd = static_cast<const AllocTdRecord*>(static_cast<const void*>(wal));
        const TrxSlotStatus *status = static_cast<const TrxSlotStatus *>(static_cast<const void *>(allocTd->data));
        uint8 tdNum = static_cast<uint8>((size - sizeof(AllocTdRecord)) / sizeof(TrxSlotStatus));
        (void)fprintf(fp, "AllocTd{tdNum(%hhu), extendNum(%hhu), size(%u)}.",
            tdNum, allocTd->extendNum, size);
        for (TdId tdId = 0; tdId < tdNum; tdId++) {
            (void)fprintf(fp, "{tdId(%hhu), xidstatus(%d)} ", tdId, static_cast<int>(status[tdId]));
        }
        (void)fprintf(fp, ".");
    }

    inline PageId GetPageId() const
    {
        return m_pageId;
    }

private:
    inline void GetTdRecycleInfo(DataPage* page, const AllocTdRecord* record,
        TdRecycleStatus* tdRecycle, uint8 &tdRecycleNum, bool &existTxnInProgress) const
    {
        const TrxSlotStatus* xidStatus = static_cast<const TrxSlotStatus*>(static_cast<const void*>(record->data));
        for (uint8 tdId = 0; tdId < page->GetTdCount(); ++tdId) {
            switch (xidStatus[tdId]) {
                case TXN_STATUS_FROZEN:
                case TXN_STATUS_COMMITTED: {
                    tdRecycle[tdRecycleNum].unused = ((xidStatus[tdId] == TXN_STATUS_FROZEN) ? true : false);
                    tdRecycle[tdRecycleNum].id = tdId;
                    tdRecycleNum++;
                    break;
                }
                case TXN_STATUS_IN_PROGRESS: {
                    existTxnInProgress = true;
                    break;
                }
                case TXN_STATUS_PENDING_COMMIT:
                case TXN_STATUS_UNKNOWN:
                case TXN_STATUS_ABORTED:
                case TXN_STATUS_FAILED:
                default: {
                    break;
                }
            }
        }
    }
} PACKED;

STATIC_ASSERT_TRIVIAL(WalRecordForDataPage);

struct WalFileHeaderData {
    uint32 crc;
    uint32 version;
    uint64 startPlsn;
    uint64 fileSize;
    uint64 timelineId;
    uint32 lastRecordRemainLen;
    uint32 magicNum;

    static inline uint32 ComputeHdrCrc(const uint8 *hdrBuf)
    {
        uint32 offset = offsetof(WalFileHeaderData, version);
        return CompChecksum(hdrBuf + offset, sizeof(WalFileHeaderData) - offset, CHECKSUM_CRC);
    }
} PACKED;

STATIC_ASSERT_TRIVIAL(WalFileHeaderData);

struct WalRecordRedoContext {
    Xid xid;
    WalId walId;
    PdbId pdbId;
    uint64 recordEndPlsn;
};

/*
 * Description callback for each WalType;
 */
using WalRedoFuncPtr = RetStatus (*)(WalRecord *walRecord);

struct BuffForDecompress {
    void *buffer;
    uint32 bufferSize;
};

struct WalRecordCompressAndDecompressItem {
    WalType type;
    uint16 headerSize;
    std::function<uint16(WalRecord *)> getMaxCompressedSize;
    std::function<uint16(WalRecord *, char *walRecordForPageOnDisk)> compress;
    std::function<uint16(WalRecord *, const WalRecord *compressedRecord)> decompress;

    WalRecordCompressAndDecompressItem(
        WalType walType, uint16 size, const std::function<uint16(WalRecord *)> &getMaxCompressedSizeFunc,
        const std::function<uint16(WalRecord *, char *walRecordForPageOnDisk)> &compressFunc,
        std::function<uint16(WalRecord *, const WalRecord *compressedRecord)> decompressFunc) noexcept
        : type(walType),
          headerSize(size),
          getMaxCompressedSize(getMaxCompressedSizeFunc),
          compress(compressFunc),
          decompress(decompressFunc)
    {}
};

}  // namespace DSTORE
#endif  // STORAGE_WAL_STRUCT_H
