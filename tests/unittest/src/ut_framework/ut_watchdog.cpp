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
 * ut_watchdog.cpp
 *     Unit tests for WatchDogMgr.
 *
 * ---------------------------------------------------------------------------------------
 */

#include "ut_utilities/ut_dstore_framework.h"
#include "framework/dstore_watchdog.h"
#include "diagnose/dstore_watchdog_diagnose.h"

#include <chrono>
#include <thread>

using namespace DSTORE;

/* ===================================================================
 * Test fixture: lightweight, no engine startup required.
 * WatchDogMgr only needs signal-mask setup and std::thread; it does NOT
 * require g_storageInstance, g_dstoreCurrentMemoryContext or thrd.
 * =================================================================== */
class WatchDogTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

/* ===================================================================
 * 1. Basic lifecycle: Start / Stop
 * =================================================================== */
TEST_F(WatchDogTest, StartAndStop)
{
    WatchDogMgr watchdog;
    watchdog.Start(0);
    /* Brief pause so the thread actually starts */
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    watchdog.Stop();
    /* Second Stop() should be a no-op (m_bgThread is nullptr after first stop) */
    watchdog.Stop();
}

/* ===================================================================
 * 2. Register / Unregister entries
 * =================================================================== */
TEST_F(WatchDogTest, RegisterAndUnregister)
{
    WatchDogMgr watchdog;

    std::atomic<uint64> hb1(0);
    std::atomic<uint64> hb2(0);
    WatchDogEntry e1("Thread1", &hb1, WATCHDOG_WAL_BGWRITER_TIMEOUT_S, 0);
    WatchDogEntry e2("Thread2", &hb2, WATCHDOG_CHECKPOINTER_TIMEOUT_S, 0);

    watchdog.Register(&e1);
    watchdog.Register(&e2);

    /* GetStatus on two registered but not-yet-started entries */
    char buf[4096] = {0};
    uint32 written = watchdog.GetStatus(buf, sizeof(buf));
    EXPECT_GT(written, 0u);
    EXPECT_NE(nullptr, strstr(buf, "Thread1"));
    EXPECT_NE(nullptr, strstr(buf, "Thread2"));

    watchdog.Unregister(&e1);

    char buf2[4096] = {0};
    watchdog.GetStatus(buf2, sizeof(buf2));
    /* e1 should no longer appear */
    EXPECT_EQ(nullptr, strstr(buf2, "Thread1"));
    EXPECT_NE(nullptr, strstr(buf2, "Thread2"));

    watchdog.Unregister(&e2);
}

/* ===================================================================
 * 3. GetStatus correctly reports NOT_STARTED when heartbeat == 0
 * =================================================================== */
TEST_F(WatchDogTest, StatusNotStartedWhenHeartbeatZero)
{
    WatchDogMgr watchdog;

    std::atomic<uint64> hb(0);
    WatchDogEntry entry("IdleThread", &hb, 30, 0);
    watchdog.Register(&entry);

    char buf[4096] = {0};
    watchdog.GetStatus(buf, sizeof(buf));
    EXPECT_NE(nullptr, strstr(buf, "NOT_STARTED"));

    watchdog.Unregister(&entry);
}

/* ===================================================================
 * 4. GetStatus correctly reports OK when heartbeat is recent
 * =================================================================== */
TEST_F(WatchDogTest, StatusOKWhenHeartbeatRecent)
{
    WatchDogMgr watchdog;

    std::atomic<uint64> hb(static_cast<uint64>(time(nullptr)));
    WatchDogEntry entry("ActiveThread", &hb, 60, 0);
    watchdog.Register(&entry);

    char buf[4096] = {0};
    watchdog.GetStatus(buf, sizeof(buf));
    EXPECT_NE(nullptr, strstr(buf, "OK"));

    watchdog.Unregister(&entry);
}

/* ===================================================================
 * 5. GetStatus correctly reports STUCK when heartbeat is stale
 * =================================================================== */
TEST_F(WatchDogTest, StatusStuckWhenHeartbeatStale)
{
    WatchDogMgr watchdog;

    /* Use a heartbeat far in the past (1000 seconds ago) */
    uint64 staleTime = static_cast<uint64>(time(nullptr)) - 1000;
    std::atomic<uint64> hb(staleTime);
    WatchDogEntry entry("StuckThread", &hb, 5, 0);  /* 5 second timeout */
    watchdog.Register(&entry);

    char buf[4096] = {0};
    watchdog.GetStatus(buf, sizeof(buf));
    EXPECT_NE(nullptr, strstr(buf, "STUCK"));

    watchdog.Unregister(&entry);
}

/* ===================================================================
 * 6. GetStatusArray fills structured output correctly
 * =================================================================== */
TEST_F(WatchDogTest, GetStatusArray)
{
    WatchDogMgr watchdog;

    std::atomic<uint64> hb1(static_cast<uint64>(time(nullptr)));
    std::atomic<uint64> hb2(0);
    WatchDogEntry e1("Active", &hb1, 60, 1);
    WatchDogEntry e2("Idle",   &hb2, 30, 1);

    watchdog.Register(&e1);
    watchdog.Register(&e2);

    WatchDogThreadStatus statusArr[8];
    uint32 count = watchdog.GetStatusArray(statusArr, 8);

    EXPECT_EQ(count, 2u);

    /* First entry: Active — heartbeat is recent, should not be stuck */
    EXPECT_STREQ(statusArr[0].name, "Active");
    EXPECT_EQ(statusArr[0].pdbId, 1u);
    EXPECT_FALSE(statusArr[0].isStuck);

    /* Second entry: Idle — heartbeat is 0, not stuck (not started) */
    EXPECT_STREQ(statusArr[1].name, "Idle");
    EXPECT_FALSE(statusArr[1].isStuck);

    watchdog.Unregister(&e1);
    watchdog.Unregister(&e2);
}

/* ===================================================================
 * 7. Running watchdog thread doesn't fire alarm for healthy threads
 * =================================================================== */
TEST_F(WatchDogTest, RunningWatchdogNoAlarmForHealthyThread)
{
    WatchDogMgr watchdog;

    std::atomic<uint64> hb(static_cast<uint64>(time(nullptr)));
    WatchDogEntry entry("HealthyThread", &hb, 60, 0);
    watchdog.Register(&entry);

    watchdog.Start(0);

    /* Keep updating heartbeat for 200ms */
    for (int i = 0; i < 4; i++) {
        hb.store(static_cast<uint64>(time(nullptr)), std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    watchdog.Stop();

    /* Entry should still be OK */
    char buf[4096] = {0};
    watchdog.GetStatus(buf, sizeof(buf));
    EXPECT_NE(nullptr, strstr(buf, "OK"));

    watchdog.Unregister(&entry);
}

/* ===================================================================
 * 8. WatchDogEntry timeoutSeconds constants are sane
 * =================================================================== */
TEST_F(WatchDogTest, TimeoutConstantsSane)
{
    EXPECT_GT(WATCHDOG_WAL_BGWRITER_TIMEOUT_S,   0u);
    EXPECT_GT(WATCHDOG_CHECKPOINTER_TIMEOUT_S,   0u);
    EXPECT_GT(WATCHDOG_BUF_PAGEWRITER_TIMEOUT_S, 0u);
    EXPECT_GT(WATCHDOG_WAL_RECYCLE_TIMEOUT_S,    0u);
    EXPECT_GT(WATCHDOG_UNDO_RECYCLE_TIMEOUT_S,   0u);
    EXPECT_GT(WATCHDOG_CHECK_INTERVAL_S,         0u);
    EXPECT_LE(WATCHDOG_CHECK_INTERVAL_S,         WATCHDOG_WAL_BGWRITER_TIMEOUT_S);
}

/* ===================================================================
 * 9. Max entries: register up to WATCHDOG_MAX_ENTRIES
 * =================================================================== */
TEST_F(WatchDogTest, RegisterMaxEntries)
{
    WatchDogMgr watchdog;
    constexpr uint32 N = 10;  /* test a reasonable number, not the full 64 */

    std::atomic<uint64> hbs[N];
    WatchDogEntry *entries[N];
    for (uint32 i = 0; i < N; i++) {
        hbs[i].store(0);
        entries[i] = new WatchDogEntry("T", &hbs[i], 30, 0);
        watchdog.Register(entries[i]);
    }

    WatchDogThreadStatus statusArr[N];
    uint32 count = watchdog.GetStatusArray(statusArr, N);
    EXPECT_EQ(count, N);

    for (uint32 i = 0; i < N; i++) {
        watchdog.Unregister(entries[i]);
        delete entries[i];
    }
}

/* ===================================================================
 * 10. GetStatusArray respects maxEntries limit
 * =================================================================== */
TEST_F(WatchDogTest, GetStatusArrayRespectsMaxEntries)
{
    WatchDogMgr watchdog;

    std::atomic<uint64> hb1(1), hb2(1), hb3(1);
    WatchDogEntry e1("A", &hb1, 30, 0);
    WatchDogEntry e2("B", &hb2, 30, 0);
    WatchDogEntry e3("C", &hb3, 30, 0);
    watchdog.Register(&e1);
    watchdog.Register(&e2);
    watchdog.Register(&e3);

    WatchDogThreadStatus arr[2];
    uint32 count = watchdog.GetStatusArray(arr, 2);
    EXPECT_EQ(count, 2u);  /* capped at maxEntries=2 */

    watchdog.Unregister(&e1);
    watchdog.Unregister(&e2);
    watchdog.Unregister(&e3);
}
