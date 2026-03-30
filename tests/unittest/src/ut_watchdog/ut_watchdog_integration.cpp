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
#include "ut_watchdog/ut_watchdog_integration.h"
#include <thread>
#include <chrono>

using namespace DSTORE;

/* ========================================================================
 * Background thread lifecycle simulation tests
 *
 * Each test simulates the heartbeat lifecycle pattern of a specific
 * background thread: Register -> SetRunState(RUNNING) -> work loop
 * with TouchHeartbeat + SLEEPING/RUNNING transitions -> Unregister
 * ======================================================================== */

TEST_F(WatchDogIntegrationTest, MasterWriterLifecycle_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MasterWriter");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    EXPECT_EQ(ThreadRunState::RUNNING, hb->runState.load(std::memory_order_relaxed));

    uint64 t1 = hb->lastHeartbeatUs.load(std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    WatchDogMgr::TouchHeartbeat(hb);
    EXPECT_GT(hb->lastHeartbeatUs.load(std::memory_order_relaxed), t1);

    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    EXPECT_EQ(ThreadRunState::SLEEPING, hb->runState.load(std::memory_order_relaxed));

    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    EXPECT_EQ(ThreadRunState::RUNNING, hb->runState.load(std::memory_order_relaxed));

    m_mgr->Unregister(hb);
    EXPECT_EQ(ThreadRunState::STOPPED, hb->runState.load(std::memory_order_relaxed));
    EXPECT_FALSE(hb->registered.load(std::memory_order_relaxed));
}

TEST_F(WatchDogIntegrationTest, SlaveWriterLifecycle_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 0, "SlaveWriter");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);

    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    EXPECT_EQ(ThreadRunState::SLEEPING, hb->runState.load(std::memory_order_relaxed));

    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    WatchDogMgr::TouchHeartbeat(hb);

    m_mgr->Unregister(hb);
    EXPECT_EQ(ThreadRunState::STOPPED, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogIntegrationTest, CheckpointerLifecycle_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::CHECKPOINTER, 0, "Checkpointer");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);

    WatchDogMgr::TouchHeartbeat(hb);

    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);

    m_mgr->Unregister(hb);
    EXPECT_EQ(ThreadRunState::STOPPED, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogIntegrationTest, WalRecyclerLifecycle_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::WAL_FILE_RECYCLE, 0, "WalRecycler");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);

    WatchDogMgr::TouchHeartbeat(hb);

    m_mgr->Unregister(hb);
    EXPECT_EQ(ThreadRunState::STOPPED, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogIntegrationTest, UndoRecyclerLifecycle_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::UNDO_RECYCLE, 0, "UndoRecycler");
    ASSERT_NE(nullptr, hb);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);

    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    WatchDogMgr::TouchHeartbeat(hb);

    m_mgr->Unregister(hb);
    EXPECT_EQ(ThreadRunState::STOPPED, hb->runState.load(std::memory_order_relaxed));
}

TEST_F(WatchDogIntegrationTest, BtreeRecyclerLifecycle_level0)
{
    WatchDogHandle *hb = m_mgr->Register(
        WatchDogThreadType::BTREE_RECYCLE, 0, "BtreeRecycler");
    ASSERT_NE(nullptr, hb);

    WatchDogMgr::SetRunState(hb, ThreadRunState::SLEEPING);
    EXPECT_EQ(ThreadRunState::SLEEPING, hb->runState.load(std::memory_order_relaxed));

    WatchDogMgr::SetRunState(hb, ThreadRunState::RUNNING);
    WatchDogMgr::TouchHeartbeat(hb);

    m_mgr->Unregister(hb);
    EXPECT_EQ(ThreadRunState::STOPPED, hb->runState.load(std::memory_order_relaxed));
}

/* ========================================================================
 * All threads registered snapshot test
 * ======================================================================== */
TEST_F(WatchDogIntegrationTest, AllThreadsRegistered_Snapshot_level0)
{
    WatchDogHandle *hb1 = m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MasterWriter");
    WatchDogHandle *hb2 = m_mgr->Register(WatchDogThreadType::BG_PAGE_SLAVE_WRITER, 0, "SlaveWriter");
    WatchDogHandle *hb3 = m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "Checkpointer");
    WatchDogHandle *hb4 = m_mgr->Register(WatchDogThreadType::WAL_FILE_RECYCLE, 0, "WalRecycler");
    WatchDogHandle *hb5 = m_mgr->Register(WatchDogThreadType::UNDO_RECYCLE, 0, "UndoRecycler");
    WatchDogHandle *hb6 = m_mgr->Register(WatchDogThreadType::BTREE_RECYCLE, 0, "BtreeRecycler");

    ASSERT_NE(nullptr, hb1);
    ASSERT_NE(nullptr, hb2);
    ASSERT_NE(nullptr, hb3);
    ASSERT_NE(nullptr, hb4);
    ASSERT_NE(nullptr, hb5);
    ASSERT_NE(nullptr, hb6);

    WatchDogMgr::SetRunState(hb1, ThreadRunState::RUNNING);
    WatchDogMgr::SetRunState(hb2, ThreadRunState::SLEEPING);
    WatchDogMgr::SetRunState(hb3, ThreadRunState::RUNNING);
    WatchDogMgr::SetRunState(hb4, ThreadRunState::RUNNING);
    WatchDogMgr::SetRunState(hb5, ThreadRunState::SLEEPING);
    WatchDogMgr::SetRunState(hb6, ThreadRunState::SLEEPING);

    WatchDogHandle snapshot[WatchDogMgr::MAX_WATCHED_THREADS];
    uint32 count = m_mgr->GetHeartbeatSnapshot(snapshot, WatchDogMgr::MAX_WATCHED_THREADS);
    EXPECT_GE(count, 6u);

    uint32 runningCount = 0;
    uint32 sleepingCount = 0;
    for (uint32 i = 0; i < count; i++) {
        ThreadRunState state = snapshot[i].runState.load(std::memory_order_relaxed);
        if (state == ThreadRunState::RUNNING) {
            runningCount++;
        } else if (state == ThreadRunState::SLEEPING) {
            sleepingCount++;
        }
    }
    EXPECT_GE(runningCount, 3u);
    EXPECT_GE(sleepingCount, 3u);

    m_mgr->Unregister(hb1);
    m_mgr->Unregister(hb2);
    m_mgr->Unregister(hb3);
    m_mgr->Unregister(hb4);
    m_mgr->Unregister(hb5);
    m_mgr->Unregister(hb6);
}

/* ========================================================================
 * Destroy clears all test
 * ======================================================================== */
TEST_F(WatchDogIntegrationTest, Destroy_ClearsAll_level0)
{
    m_mgr->Register(WatchDogThreadType::BG_PAGE_MASTER_WRITER, 0, "MW");
    m_mgr->Register(WatchDogThreadType::CHECKPOINTER, 0, "CKP");
    EXPECT_EQ(m_baseCount + 2, m_mgr->GetRegisteredCount());

    m_mgr->Destroy();
    EXPECT_EQ(0u, m_mgr->GetRegisteredCount());
    EXPECT_TRUE(m_mgr->IsStopped());
}
