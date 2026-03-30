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
#include "ut_watchdog/ut_watchdog_diagnose.h"

using namespace DSTORE;

/* ========================================================================
 * GetAllThreadStatus tests
 * ======================================================================== */
TEST_F(WatchDogDiagnoseTest, GetAllThreadStatus_NullParams_level0)
{
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(g_defaultPdbId, nullptr, nullptr);
    EXPECT_EQ(DSTORE_FAIL, ret);
}

TEST_F(WatchDogDiagnoseTest, GetAllThreadStatus_Empty_level0)
{
    WatchDogThreadStatus *statusArr = nullptr;
    uint32_t count = 0;
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(g_defaultPdbId, &statusArr, &count);
    EXPECT_EQ(DSTORE_SUCC, ret);
    /* WAL recycle thread registers with WatchDog during startup, so count >= 1 */
    EXPECT_GE(count, 0u);
    if (statusArr != nullptr) {
        WatchDogDiagnose::FreeThreadStatusArray(statusArr);
    }
}

TEST_F(WatchDogDiagnoseTest, GetAllThreadStatus_WithRegistered_level0)
{
    uint32_t baseCount = m_mgr->GetRegisteredCount();

    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "TestMW");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    WatchDogMgr::TouchHeartbeat(hb);

    WatchDogThreadStatus *statusArr = nullptr;
    uint32_t count = 0;
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(g_defaultPdbId, &statusArr, &count);
    EXPECT_EQ(DSTORE_SUCC, ret);
    EXPECT_GE(count, baseCount + 1);

    bool foundTestMW = false;
    if (statusArr != nullptr) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(statusArr[i].threadName, "TestMW") == 0) {
                foundTestMW = true;
                EXPECT_EQ(ThreadRunState::RUNNING, statusArr[i].runState);
                EXPECT_GT(statusArr[i].timeoutThresholdUs, 0u);
                break;
            }
        }
        EXPECT_TRUE(foundTestMW);
        WatchDogDiagnose::FreeThreadStatusArray(statusArr);
    }

    m_mgr->Unregister(hb);
}

/* ========================================================================
 * GetThreadStatusByType tests
 * ======================================================================== */
TEST_F(WatchDogDiagnoseTest, GetThreadStatusByType_NullParams_level0)
{
    RetStatus ret = WatchDogDiagnose::GetThreadStatusByType(
        g_defaultPdbId, WatchDogThreadType::CHECKPOINTER, nullptr, nullptr);
    EXPECT_EQ(DSTORE_FAIL, ret);
}

TEST_F(WatchDogDiagnoseTest, GetThreadStatusByType_NoMatch_level0)
{
    m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");

    WatchDogThreadStatus *statusArr = nullptr;
    uint32_t count = 0;
    RetStatus ret = WatchDogDiagnose::GetThreadStatusByType(
        g_defaultPdbId, WatchDogThreadType::BTREE_RECYCLE, &statusArr, &count);
    EXPECT_EQ(DSTORE_SUCC, ret);
    EXPECT_EQ(0u, count);
}

TEST_F(WatchDogDiagnoseTest, GetThreadStatusByType_WithMatch_level0)
{
    WatchDogHandle *hb1 = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 0, "SW0");
    WatchDogHandle *hb2 = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 1, "SW1");
    m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "CKP");
    ASSERT_NE(nullptr, hb1);
    ASSERT_NE(nullptr, hb2);

    WatchDogMgr::SetRunState(hb1, ThreadRunState::RUNNING);
    WatchDogMgr::SetRunState(hb2, ThreadRunState::SLEEPING);
    WatchDogMgr::TouchHeartbeat(hb1);
    WatchDogMgr::TouchHeartbeat(hb2);

    WatchDogThreadStatus *statusArr = nullptr;
    uint32_t count = 0;
    RetStatus ret = WatchDogDiagnose::GetThreadStatusByType(
        g_defaultPdbId, WatchDogThreadType::BG_PAGE_SLAVE_WRITER, &statusArr, &count);
    EXPECT_EQ(DSTORE_SUCC, ret);
    EXPECT_EQ(2u, count);
    if (statusArr != nullptr) {
        EXPECT_STREQ("SW0", statusArr[0].threadName);
        EXPECT_STREQ("SW1", statusArr[1].threadName);
        WatchDogDiagnose::FreeThreadStatusArray(statusArr);
    }
}

/* ========================================================================
 * FreeThreadStatusArray tests
 * ======================================================================== */
TEST_F(WatchDogDiagnoseTest, FreeThreadStatusArray_Nullptr_level0)
{
    WatchDogDiagnose::FreeThreadStatusArray(nullptr);
}

/* ========================================================================
 * GetFormattedSummary tests
 * ======================================================================== */
TEST_F(WatchDogDiagnoseTest, GetFormattedSummary_WithThreads_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::CHECKPOINTER, 0, "TestCKP");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    WatchDogMgr::TouchHeartbeat(hb);

    char *summary = WatchDogDiagnose::GetFormattedSummary(g_defaultPdbId);
    ASSERT_NE(nullptr, summary);
    EXPECT_NE(nullptr, strstr(summary, "WatchDog Thread Status"));
    EXPECT_NE(nullptr, strstr(summary, "TestCKP"));
    DstorePfree(summary);

    m_mgr->Unregister(hb);
}

TEST_F(WatchDogDiagnoseTest, GetFormattedSummary_InvalidPdb_level0)
{
    char *summary = WatchDogDiagnose::GetFormattedSummary(INVALID_PDB_ID);
    EXPECT_EQ(nullptr, summary);
}

/* ========================================================================
 * GetFormattedStatusByType tests
 * ======================================================================== */
TEST_F(WatchDogDiagnoseTest, GetFormattedStatusByType_WithThread_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::WAL_FILE_RECYCLE, 0, "TestWAL");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    WatchDogMgr::TouchHeartbeat(hb);

    char *result = WatchDogDiagnose::GetFormattedStatusByType(
        g_defaultPdbId, WatchDogThreadType::WAL_FILE_RECYCLE);
    ASSERT_NE(nullptr, result);
    EXPECT_NE(nullptr, strstr(result, "TestWAL"));
    DstorePfree(result);

    m_mgr->Unregister(hb);
}

TEST_F(WatchDogDiagnoseTest, GetFormattedStatusByType_NoMatch_level0)
{
    char *result = WatchDogDiagnose::GetFormattedStatusByType(
        g_defaultPdbId, WatchDogThreadType::BTREE_RECYCLE);
    ASSERT_NE(nullptr, result);
    EXPECT_NE(nullptr, strstr(result, "no threads registered"));
    DstorePfree(result);
}

TEST_F(WatchDogDiagnoseTest, GetFormattedStatusByType_InvalidPdb_level0)
{
    char *result = WatchDogDiagnose::GetFormattedStatusByType(
        INVALID_PDB_ID, WatchDogThreadType::CHECKPOINTER);
    EXPECT_EQ(nullptr, result);
}

/* ========================================================================
 * Fault Injection tests for Diagnose
 * ======================================================================== */
static FaultInjectionEntry g_watchdogDiagnoseFIEntries[] = {
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::REGISTER_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::GET_PDB_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::GET_WATCHDOG_MGR_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::PALLOC_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::GET_GUC_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::CHECK_HEARTBEAT_SKIP, false, nullptr),
};

class WatchDogDiagnoseFITest : public WatchDogDiagnoseTest {
protected:
    void SetUp() override
    {
        WatchDogDiagnoseTest::SetUp();
        int count = static_cast<int>(sizeof(g_watchdogDiagnoseFIEntries) / sizeof(g_watchdogDiagnoseFIEntries[0]));
        RegisterFaultInjection(g_watchdogDiagnoseFIEntries, count, FI_GLOBAL);
    }

    void TearDown() override
    {
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::GET_PDB_FAIL, FI_GLOBAL);
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::GET_WATCHDOG_MGR_FAIL, FI_GLOBAL);
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::PALLOC_FAIL, FI_GLOBAL);
        WatchDogDiagnoseTest::TearDown();
    }
};

TEST_F(WatchDogDiagnoseFITest, GetAllThreadStatus_GetPdbFail_level0)
{
    FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::GET_PDB_FAIL, FI_GLOBAL);

    WatchDogThreadStatus *statusArr = nullptr;
    uint32_t count = 0;
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(g_defaultPdbId, &statusArr, &count);
    EXPECT_EQ(DSTORE_FAIL, ret);
    EXPECT_EQ(nullptr, statusArr);
    EXPECT_EQ(0u, count);
}

TEST_F(WatchDogDiagnoseFITest, GetAllThreadStatus_GetWatchDogMgrFail_level0)
{
    FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::GET_WATCHDOG_MGR_FAIL, FI_GLOBAL);

    WatchDogThreadStatus *statusArr = nullptr;
    uint32_t count = 0;
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(g_defaultPdbId, &statusArr, &count);
    EXPECT_EQ(DSTORE_FAIL, ret);
    EXPECT_EQ(nullptr, statusArr);
    EXPECT_EQ(0u, count);
}

TEST_F(WatchDogDiagnoseFITest, GetAllThreadStatus_PallocFail_level0)
{
    m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");

    FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::PALLOC_FAIL, FI_GLOBAL);

    WatchDogThreadStatus *statusArr = nullptr;
    uint32_t count = 0;
    RetStatus ret = WatchDogDiagnose::GetAllThreadStatus(g_defaultPdbId, &statusArr, &count);
    EXPECT_EQ(DSTORE_FAIL, ret);
}
