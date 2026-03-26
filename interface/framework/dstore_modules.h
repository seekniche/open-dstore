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
 * IDENTIFICATION
 *        interface/framework/dstore_modules.h
 *
 * ---------------------------------------------------------------------------------------
 */

#ifndef DSTORE_MODULES_H
#define DSTORE_MODULES_H

namespace DSTORE {

enum ModuleId : unsigned char {
    MODULE_ALL,
    MODULE_BUFFER,
    MODULE_BUFMGR,
    MODULE_CATALOG,
    MODULE_COMMON,
    MODULE_HEAP,
    MODULE_CONTROL,
    MODULE_FRAMEWORK,
    MODULE_INDEX,
    MODULE_LOCK,
    MODULE_PAGE,
    MODULE_PORT,
    MODULE_WAL,
    MODULE_BGPAGEWRITER,
    MODULE_TABLESPACE,
    MODULE_SEGMENT,
    MODULE_TRANSACTION,
    MODULE_TUPLE,
    MODULE_UNDO,
    MODULE_RPC,
    MODULE_SYSTABLE,
    MODULE_MEMNODE,
    MODULE_PDBREPLICA,
    MODULE_RECOVERY,
    MODULE_LOGICAL_REPLICATION,
    MODULE_PDB,
    MODULE_WATCHDOG,
    MODULE_MAX
};

#define MODULE_DISTRIBUTE_OFFSET        (MODULE_MAX + 1)

} /* namespace DSTORE */

#endif
