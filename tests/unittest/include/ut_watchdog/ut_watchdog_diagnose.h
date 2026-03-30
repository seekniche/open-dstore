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
#ifndef UT_WATCHDOG_DIAGNOSE_H
#define UT_WATCHDOG_DIAGNOSE_H

#include <gtest/gtest.h>
#include "ut_utilities/ut_dstore_framework.h"
#include "ut_mock/ut_instance_mock.h"
#include "framework/dstore_watchdog.h"
#include "diagnose/dstore_watchdog_diagnose.h"
#include "common/fault_injection/dstore_watchdog_fault_injection.h"

class WatchDogDiagnoseTest : public DSTORETEST {
protected:
    void SetUp() override
    {
        DSTORETEST::SetUp();
        MockStorageInstance *inst = DstoreNew(m_ut_memory_context) MockStorageInstance();
        inst->Install(&DSTORETEST::m_guc, m_ut_memory_context);
        inst->Startup(&DSTORETEST::m_guc);

        DSTORE::StoragePdb *pdb = g_storageInstance->GetPdb(g_defaultPdbId);
        ASSERT_NE(nullptr, pdb);
        m_mgr = pdb->GetWatchDogMgr();
        ASSERT_NE(nullptr, m_mgr);
    }

    void TearDown() override
    {
        m_mgr = nullptr;
        MockStorageInstance *inst = (MockStorageInstance *)g_storageInstance;
        inst->Shutdown();
        delete inst;
        DSTORETEST::TearDown();
    }

    DSTORE::WatchDogMgr *m_mgr;
};

#endif // UT_WATCHDOG_DIAGNOSE_H
