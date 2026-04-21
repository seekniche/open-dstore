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
 */

#include "gtest/gtest.h"
#include "wal/dstore_wal_struct.h"

using namespace DSTORE;

namespace {

WalCheckPoint MakeCp(uint64 diskRecoveryPlsn)
{
    WalCheckPoint cp{};
    cp.diskRecoveryPlsn = diskRecoveryPlsn;
    return cp;
}

class WalCheckPointHistoryTest : public ::testing::Test {};

TEST_F(WalCheckPointHistoryTest, EmptyHistory)
{
    WalCheckPointHistory h{};
    EXPECT_EQ(h.slotCount, 0u);
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 0u);
}

TEST_F(WalCheckPointHistoryTest, PushFillsSlotsInOrder)
{
    WalCheckPointHistory h{};
    h.Push(MakeCp(100));
    EXPECT_EQ(h.slotCount, 1u);
    EXPECT_EQ(h.newestSlotIdx, 0u);
    EXPECT_EQ(h.slots[0].diskRecoveryPlsn, 100u);

    h.Push(MakeCp(200));
    EXPECT_EQ(h.slotCount, 2u);
    EXPECT_EQ(h.newestSlotIdx, 1u);
    EXPECT_EQ(h.slots[1].diskRecoveryPlsn, 200u);

    h.Push(MakeCp(300));
    EXPECT_EQ(h.slotCount, WAL_CHECKPOINT_HISTORY_SLOTS);
    EXPECT_EQ(h.newestSlotIdx, 2u);
    EXPECT_EQ(h.slots[2].diskRecoveryPlsn, 300u);
}

/*
 * RotateWrite: pushing N+2 checkpoints into an N-slot ring overwrites the
 * oldest entries and keeps newestSlotIdx tracking the latest write.
 */
TEST_F(WalCheckPointHistoryTest, RotateWriteOverwritesOldest)
{
    WalCheckPointHistory h{};
    h.Push(MakeCp(100));
    h.Push(MakeCp(200));
    h.Push(MakeCp(300));
    /* Now full: slots = [100, 200, 300], newestSlotIdx = 2. */

    h.Push(MakeCp(400));
    /* Slot 0 (oldest, plsn 100) is overwritten by 400. */
    EXPECT_EQ(h.slotCount, WAL_CHECKPOINT_HISTORY_SLOTS);
    EXPECT_EQ(h.newestSlotIdx, 0u);
    EXPECT_EQ(h.slots[0].diskRecoveryPlsn, 400u);
    EXPECT_EQ(h.slots[1].diskRecoveryPlsn, 200u);
    EXPECT_EQ(h.slots[2].diskRecoveryPlsn, 300u);

    h.Push(MakeCp(500));
    /* Slot 1 (now oldest, plsn 200) is overwritten by 500. */
    EXPECT_EQ(h.newestSlotIdx, 1u);
    EXPECT_EQ(h.slots[0].diskRecoveryPlsn, 400u);
    EXPECT_EQ(h.slots[1].diskRecoveryPlsn, 500u);
    EXPECT_EQ(h.slots[2].diskRecoveryPlsn, 300u);
}

/*
 * GetSlotIdxByAge: k = 0 returns the newest slot, k = slotCount - 1 the
 * oldest, regardless of where the ring head currently sits.
 */
TEST_F(WalCheckPointHistoryTest, GetSlotIdxByAgeBeforeWrap)
{
    WalCheckPointHistory h{};
    h.Push(MakeCp(100));
    h.Push(MakeCp(200));
    h.Push(MakeCp(300));
    /* newestSlotIdx = 2, slots = [100, 200, 300]. */
    EXPECT_EQ(h.GetSlotIdxByAge(0), 2u); /* newest -> 300 */
    EXPECT_EQ(h.GetSlotIdxByAge(1), 1u); /* second -> 200 */
    EXPECT_EQ(h.GetSlotIdxByAge(2), 0u); /* oldest -> 100 */
    EXPECT_EQ(h.slots[h.GetSlotIdxByAge(0)].diskRecoveryPlsn, 300u);
    EXPECT_EQ(h.slots[h.GetSlotIdxByAge(2)].diskRecoveryPlsn, 100u);
}

TEST_F(WalCheckPointHistoryTest, GetSlotIdxByAgeAfterWrap)
{
    WalCheckPointHistory h{};
    h.Push(MakeCp(100));
    h.Push(MakeCp(200));
    h.Push(MakeCp(300));
    h.Push(MakeCp(400));
    h.Push(MakeCp(500));
    /* newestSlotIdx = 1, slots = [400, 500, 300]. */
    EXPECT_EQ(h.GetSlotIdxByAge(0), 1u); /* newest  -> 500 */
    EXPECT_EQ(h.GetSlotIdxByAge(1), 0u); /* second  -> 400 */
    EXPECT_EQ(h.GetSlotIdxByAge(2), 2u); /* oldest  -> 300 */
    EXPECT_EQ(h.slots[h.GetSlotIdxByAge(0)].diskRecoveryPlsn, 500u);
    EXPECT_EQ(h.slots[h.GetSlotIdxByAge(1)].diskRecoveryPlsn, 400u);
    EXPECT_EQ(h.slots[h.GetSlotIdxByAge(2)].diskRecoveryPlsn, 300u);
}

/*
 * GetOldestDiskRecoveryPlsn returns the minimum diskRecoveryPlsn across
 * the populated slots; this is what GetRecyclablePlsn relies on so that
 * fall-back targets are not accidentally recycled.
 */
TEST_F(WalCheckPointHistoryTest, GetOldestDiskRecoveryPlsnPartial)
{
    WalCheckPointHistory h{};
    h.Push(MakeCp(700));
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 700u);

    h.Push(MakeCp(800));
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 700u);
}

TEST_F(WalCheckPointHistoryTest, GetOldestDiskRecoveryPlsnFullAndWrapped)
{
    WalCheckPointHistory h{};
    h.Push(MakeCp(100));
    h.Push(MakeCp(200));
    h.Push(MakeCp(300));
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 100u);

    /* Wrap once: slot 0 (=100) is overwritten with 400. */
    h.Push(MakeCp(400));
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 200u);

    /* Wrap again: slot 1 (=200) is overwritten with 500. */
    h.Push(MakeCp(500));
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 300u);
}

/*
 * The ring tolerates non-monotonic diskRecoveryPlsn values: GetOldest still
 * returns the numerical minimum of the populated slots. This protects the
 * recycling decision when checkpoints momentarily go backwards (e.g. forced
 * checkpoint after a restart).
 */
TEST_F(WalCheckPointHistoryTest, GetOldestPicksMinimumNotPositionalOldest)
{
    WalCheckPointHistory h{};
    h.Push(MakeCp(500));
    h.Push(MakeCp(100));
    h.Push(MakeCp(300));
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 100u);
}

/*
 * Backwards-compat path: a stream that was upgraded from a binary that did
 * not write history will arrive with slotCount == 0. GetOldest reports
 * INVALID_PLSN (0) so that callers fall back to the legacy lastWalCheckpoint.
 */
TEST_F(WalCheckPointHistoryTest, BackwardCompatEmptyReportsZero)
{
    WalCheckPointHistory h{};
    EXPECT_EQ(h.slotCount, 0u);
    EXPECT_EQ(h.GetOldestDiskRecoveryPlsn(), 0u);
}

}  // namespace
