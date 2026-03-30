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
#include "ut_watchdog/ut_watchdog_mgr.h"
#include <thread>
#include <chrono>

using namespace DSTORE;

/* ========================================================================
 * Register / Unregister tests
 * ======================================================================== */
TEST_F(WatchDogMgrTest, Register_Basic_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MasterWriter");
    ASSERT_NE(nullptr, hb);
    EXPECT_TRUE(hb->registered.load(std::memory_order_relaxed));
    EXPECT_EQ(WatchDogThreadType::BG_PAGE_MASTER_WRITER, hb->threadType);
    EXPECT_EQ(0u, hb->threadIndex);
    EXPECT_STREQ("MasterWriter", hb->threadName);
    EXPECT_EQ(ThreadRunState::NOT_STARTED, hb->runState.load(std::memory_order_relaxed));
    EXPECT_GT(hb->lastHeartbeatUs.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(m_baseCount + 1, m_mgr->GetRegisteredCount());
}

TEST_F(WatchDogMgrTest, Register_NullName_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::CHECKPOINTER, 0, nullptr);
    ASSERT_NE(nullptr, hb);
    EXPECT_EQ('\0', hb->threadName[0]);
}

TEST_F(WatchDogMgrTest, Register_MultipleTypes_level0)
{
    WatchDogHandle *hb1 = m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    WatchDogHandle *hb2 = m_mgr->Register(WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 0, "SW0");
    WatchDogHandle *hb3 = m_mgr->Register(WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 1, "SW1");
    WatchDogHandle *hb4 = m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "CKP");
    WatchDogHandle *hb5 = m_mgr->Register(WatchDogThreadType::WAL_FILE_RECYCLE, 0, "WAL");
    WatchDogHandle *hb6 = m_mgr->Register(WatchDogThreadType::UNDO_RECYCLE, 0, "UNDO");
    WatchDogHandle *hb7 = m_mgr->Register(WatchDogThreadType::BTREE_RECYCLE, 0, "BTREE");

    ASSERT_NE(nullptr, hb1);
    ASSERT_NE(nullptr, hb2);
    ASSERT_NE(nullptr, hb3);
    ASSERT_NE(nullptr, hb4);
    ASSERT_NE(nullptr, hb5);
    ASSERT_NE(nullptr, hb6);
    ASSERT_NE(nullptr, hb7);
    EXPECT_EQ(m_baseCount + 7, m_mgr->GetRegisteredCount());
}

TEST_F(WatchDogMgrTest, Register_MaxSlots_level0)
{
    uint32 remaining = WatchDogMgr::MAX_WATCHED_THREADS - m_baseCount;
    for (uint32 i = 0; i < remaining; i++) {
        WatchDogHandle *hb = m_mgr->Register(WatchDogThreadType::BG_PAGE_SLAVE_WRITER, i, "SW");
        ASSERT_NE(nullptr, hb);
    }
    EXPECT_EQ(WatchDogMgr::MAX_WATCHED_THREADS, m_mgr->GetRegisteredCount());

    WatchDogHandle *overflow = m_mgr->Register(WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 99, "OVERFLOW");
    EXPECT_EQ(nullptr, overflow);
    EXPECT_EQ(WatchDogMgr::MAX_WATCHED_THREADS, m_mgr->GetRegisteredCount());
}

TEST_F(WatchDogMgrTest, Unregister_Basic_level0)
{
    WatchDogHandle *hb = m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "CKP");
    ASSERT_NE(nullptr, hb);
    EXPECT_TRUE(hb->registered.load(std::memory_order_relaxed));

    m_mgr->Unregister(hb);
    EXPECT_FALSE(hb->registered.load(std::memory_order_relaxed));
    EXPECT_EQ(ThreadRunState::STOPPED, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogMgrTest, Unregister_Nullptr_level0)
{
    m_mgr->Unregister(nullptr);
}

/* ========================================================================
 * TouchHeartbeat / SetRunState tests
 * ======================================================================== */
TEST_F(WatchDogMgrTest, TouchHeartbeat_Basic_level0)
{
    WatchDogHandle *hb = m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    ASSERT_NE(nullptr, hb);

    uint64 before = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    WatchDogMgr::TouchHeartbeat(hb);
    uint64 after = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
    EXPECT_GT(after, before);
}

TEST_F(WatchDogMgrTest, TouchHeartbeat_Nullptr_level0)
{
    WatchDogMgr::TouchHeartbeat(nullptr);
}

TEST_F(WatchDogMgrTest, SetRunState_Basic_level0)
{
    WatchDogHandle *hb = m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    ASSERT_NE(nullptr, hb);

    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    EXPECT_EQ(ThreadRunState::RUNNING, hb->runState.load(std::memory_order_relaxed));

    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    EXPECT_EQ(ThreadRunState::SLEEPING, hb->runState.load(std::memory_order_relaxed));

    WatchDogMgr::SetRunState(hb, ThreadRunState::STUCK);
    EXPECT_EQ(ThreadRunState::STUCK, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogMgrTest, SetRunState_Nullptr_level0)
{
    WatchDogMgr::SetRunState(nullptr, ThreadRunState::RUNNING);
}

/* ========================================================================
 * GetSteadyClockUs test
 * ======================================================================== */
TEST_F(WatchDogMgrTest, GetSteadyClockUs_level0)
{
    uint64 t1 = WatchDogMgr::GetSteadyClockUs();
    EXPECT_GT(t1, 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64 t2 = WatchDogMgr::GetSteadyClockUs();
    EXPECT_GT(t2, t1);
}

/* ========================================================================
 * GetTimeoutUsFromGuc tests
 * ======================================================================== */
TEST_F(WatchDogMgrTest, GetTimeoutUsFromGuc_AllTypes_level0)
{
    EXPECT_GT(m_mgr->GetTimeoutUsFromGuc(WatchDogThreadType::BG_PAGE_MASTER_WRITER), 0u);
    EXPECT_GT(m_mgr->GetTimeoutUsFromGuc(WatchDogThreadType::BG_PAGE_SLAVE_WRITER), 0u);
    EXPECT_GT(m_mgr->GetTimeoutUsFromGuc(WatchDogThreadType::CHECKPOINTER), 0u);
    EXPECT_GT(m_mgr->GetTimeoutUsFromGuc(WatchDogThreadType::WAL_FILE_RECYCLE), 0u);
    EXPECT_GT(m_mgr->GetTimeoutUsFromGuc(WatchDogThreadType::UNDO_RECYCLE), 0u);
    EXPECT_GT(m_mgr->GetTimeoutUsFromGuc(WatchDogThreadType::BTREE_RECYCLE), 0u);
    EXPECT_GT(m_mgr->GetTimeoutUsFromGuc(WatchDogThreadType::WATCHDOG_THREAD_TYPE_COUNT), 0u);
}

/* ========================================================================
 * GetHeartbeatSnapshot tests
 * ======================================================================== */
TEST_F(WatchDogMgrTest, GetHeartbeatSnapshot_Empty_level0)
{
    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = m_mgr->GetHeartbeatSnapshot(snapshot, WatchDogMgr::MAX_WATCHED_THREADS);
    EXPECT_EQ(m_baseCount, count);
}

TEST_F(WatchDogMgrTest, GetHeartbeatSnapshot_WithThreads_level0)
{
    m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "CKP");

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = m_mgr->GetHeartbeatSnapshot(snapshot, WatchDogMgr::MAX_WATCHED_THREADS);
    EXPECT_EQ(m_baseCount + 2, count);
    EXPECT_STREQ("MW", snapshot[m_baseCount].threadName);
    EXPECT_STREQ("CKP", snapshot[m_baseCount + 1].threadName);
}

TEST_F(WatchDogMgrTest, GetHeartbeatSnapshot_SkipUnregistered_level0)
{
    WatchDogHandle *hb1 = m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "CKP");
    m_mgr->Unregister(hb1);

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = m_mgr->GetHeartbeatSnapshot(snapshot, WatchDogMgr::MAX_WATCHED_THREADS);
    EXPECT_EQ(m_baseCount + 1, count);

    bool foundCKP = false;
    for (uint32 i = 0; i < count; i++) {
        if (strcmp(snapshot[i].threadName, "CKP") == 0) {
            foundCKP = true;
            break;
        }
    }
    EXPECT_TRUE(foundCKP);
}

/* ========================================================================
 * GetHeartbeatsByType tests
 * ======================================================================== */
TEST_F(WatchDogMgrTest, GetHeartbeatsByType_Basic_level0)
{
    m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    m_mgr->Register(WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 0, "SW0");
    m_mgr->Register(WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 1, "SW1");
    m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "CKP");

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = m_mgr->GetHeartbeatsByType(WatchDogThreadType::BG_PAGE_SLAVE_WRITER,
                                               snapshot, WatchDogMgr::MAX_WATCHED_THREADS);
    EXPECT_EQ(2u, count);
    EXPECT_STREQ("SW0", snapshot[0].threadName);
    EXPECT_STREQ("SW1", snapshot[1].threadName);
}

TEST_F(WatchDogMgrTest, GetHeartbeatsByType_NoMatch_level0)
{
    m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = m_mgr->GetHeartbeatsByType(WatchDogThreadType::BTREE_RECYCLE,
                                               snapshot, WatchDogMgr::MAX_WATCHED_THREADS);
    EXPECT_EQ(0u, count);
}

/* ========================================================================
 * CheckAllHeartbeats tests
 * ======================================================================== */
TEST_F(WatchDogMgrTest, CheckHeartbeats_DetectsStuck_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MasterWriter");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    hb->lastHeartbeatUs.store(0, std::memory_order_relaxed);

    m_mgr->CheckAllHeartbeats();

    EXPECT_EQ(ThreadRunState::STUCK, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogMgrTest, CheckHeartbeats_SkipsSleeping_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 0, "SlaveWriter");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    hb->lastHeartbeatUs.store(0, std::memory_order_relaxed);

    m_mgr->CheckAllHeartbeats();

    EXPECT_EQ(ThreadRunState::SLEEPING, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogMgrTest, CheckHeartbeats_HealthyThreadNotStuck_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::CHECKPOINTER, 0, "Checkpointer");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    WatchDogMgr::TouchHeartbeat(hb);

    m_mgr->CheckAllHeartbeats();

    EXPECT_EQ(ThreadRunState::RUNNING, hb->runState.load(std::memory_order_relaxed));
}

/* ========================================================================
 * Fault Injection tests
 * ======================================================================== */
static FaultInjectionEntry g_watchdogFIEntries[] = {
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::REGISTER_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::GET_PDB_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::GET_WATCHDOG_MGR_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::PALLOC_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::GET_GUC_FAIL, false, nullptr),
    FAULT_INJECTION_ENTRY(DstoreWatchDogFI::CHECK_HEARTBEAT_SKIP, false, nullptr),
};

class WatchDogFITest : public WatchDogMgrTest {
protected:
    void SetUp() override
    {
        WatchDogMgrTest::SetUp();
        int count = static_cast<int>(sizeof(g_watchdogFIEntries) / sizeof(g_watchdogFIEntries[0]));
        RegisterFaultInjection(g_watchdogFIEntries, count, FI_GLOBAL);
    }

    void TearDown() override
    {
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::REGISTER_FAIL, FI_GLOBAL);
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::GET_PDB_FAIL, FI_GLOBAL);
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::GET_WATCHDOG_MGR_FAIL, FI_GLOBAL);
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::PALLOC_FAIL, FI_GLOBAL);
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::GET_GUC_FAIL, FI_GLOBAL);
        FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::CHECK_HEARTBEAT_SKIP, FI_GLOBAL);
        WatchDogMgrTest::TearDown();
    }
};

TEST_F(WatchDogFITest, Register_FaultInjection_level0)
{
    FAULT_INJECTION_ACTIVE(DstoreWatchDogFI::REGISTER_FAIL, FI_GLOBAL);
    WatchDogHandle *hb = m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    EXPECT_EQ(nullptr, hb);
    FAULT_INJECTION_INACTIVE(DstoreWatchDogFI::REGISTER_FAIL, FI_GLOBAL);

    hb = m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    EXPECT_NE(nullptr, hb);
}
