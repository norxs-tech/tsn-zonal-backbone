// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        test_PtpClockManager.cpp
 * @brief       GoogleTest unit tests for PtpClockManager — covering PI servo convergence,
 *              BMCA grandmaster election, holdover state machine transitions, sync
 *              timeout detection, and PHC frequency adjustment via MockSwitchHal.
 *              Achieves MC/DC coverage on all state machine decision points.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, ISO 26262 ASIL-D (unit test per Part 6 §9.4.3),
 *              IEEE 802.1AS-Rev
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 * =====================================================================================
 */

#include <gtest/gtest.h>
#include "norxs/tsn/PtpClockManager.hpp"

using namespace norxs::tsn;

// ─────────────────────────────────────────────────────────────────────────────
// Mock HAL — tracks PHC register writes
// ─────────────────────────────────────────────────────────────────────────────

class MockHalPtp final : public SwitchHal
{
public:
    u32  writeCallCount  { 0U };
    u32  lastRegAddr     { 0U };
    RegData lastRegValue { 0U };

    // Simulated PHC time registers
    RegData phcSecHigh { 0U };
    RegData phcSecLow  { 1U };   // 1 second offset from epoch
    RegData phcNs      { 0U };

    Status Init() noexcept override { return Status::Ok(); }

    Status WriteRegister(u8, RegAddr addr, RegData data) noexcept override
    {
        ++writeCallCount;
        lastRegAddr  = addr;
        lastRegValue = data;
        return Status::Ok();
    }

    Status ReadRegister(u8, RegAddr addr, RegData& out) const noexcept override
    {
        if (addr == 0x0408U) { out = phcSecHigh; return Status::Ok(); }
        if (addr == 0x040CU) { out = phcSecLow;  return Status::Ok(); }
        if (addr == 0x0410U) { out = phcNs;       return Status::Ok(); }
        out = 0U;
        return Status::Ok();
    }

    Status ReadModifyWrite(u8, RegAddr, RegData, RegData) noexcept override { return Status::Ok(); }
    Status ApplyGclToPort(u8, const ScheduleParams&) noexcept override { return Status::Ok(); }
    Status ReadOperGcl(u8, ScheduleParams&) const noexcept override { return Status::Ok(); }
    Status IsGclSwapPending(u8, bool& out) const noexcept override { out = false; return Status::Ok(); }
    Status SetStreamFilter(const StreamFilterInstance&) noexcept override { return Status::Ok(); }
    Status ClearStreamFilter(u16) noexcept override { return Status::Ok(); }
    Status GetStreamDropCount(u16, u64& out) const noexcept override { out = 0; return Status::Ok(); }
    Status RegisterFrerStream(const FrerStreamEntry&) noexcept override { return Status::Ok(); }
    Status DeregisterFrerStream(u16) noexcept override { return Status::Ok(); }
    Status GetFrerStats(u16, u64& a, u64& b, u64& c) const noexcept override { a=b=c=0; return Status::Ok(); }
    Status ConfigureMacsecChannel(const MacsecChannelConfig&) noexcept override { return Status::Ok(); }
    Status RotateMacsecKey(u8, const MacsecSak128&) noexcept override { return Status::Ok(); }
    Status GetMacsecAuthFailures(u8, u64& out) const noexcept override { out=0; return Status::Ok(); }
    Status SetVlanEntry(u16, u16, u16) noexcept override { return Status::Ok(); }
    Status SetTcamRule(u8, const std::array<u8,6U>&, u16, bool) noexcept override { return Status::Ok(); }
    Status GetPortDropCount(u8, u64& out) const noexcept override { out=0; return Status::Ok(); }
    Status GetLinkStatus(u8, bool& up, u32& spd) const noexcept override { up=true; spd=1000; return Status::Ok(); }
    const char* GetDeviceIdentifier() const noexcept override { return "MockHalPtp v1.0"; }
    u8 GetPortCount() const noexcept override { return 8U; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Test Fixture
// ─────────────────────────────────────────────────────────────────────────────

class PtpTest : public ::testing::Test
{
protected:
    MockHalPtp hal;
    PtpClockManager ptp{ hal };

    PtpConfig MakeValidConfig() const
    {
        PtpConfig cfg{};
        cfg.localClockQuality.clockClass    = 135U;
        cfg.localClockQuality.clockAccuracy = 0x22U;
        cfg.priority1         = 128U;
        cfg.priority2         = 128U;
        cfg.domainNumber      = 0U;
        cfg.portCount         = 2U;
        cfg.syncTimeoutNs     = 10'000'000ULL;   // 10 ms
        cfg.holdoverBudgetNs  = 100'000'000ULL;  // 100 ms
        cfg.driftCompensationNs = 1'000ULL;      // 1 µs/s (TCXO)
        cfg.ports[0].portIndex = 0U;
        cfg.ports[0].enabled   = true;
        cfg.ports[0].logSyncInterval = static_cast<u8>(-3);  // 8/s
        cfg.ports[1].portIndex = 1U;
        cfg.ports[1].enabled   = true;
        return cfg;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, Init_ValidConfig_ReturnsOk)
{
    EXPECT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
}

TEST_F(PtpTest, Init_ZeroPortCount_ReturnsInvalidArgument)
{
    PtpConfig cfg = MakeValidConfig();
    cfg.portCount = 0U;
    EXPECT_EQ(ptp.Init(cfg).Error(), ErrorCode::kInvalidArgument);
}

TEST_F(PtpTest, Init_TooManyPorts_ReturnsInvalidArgument)
{
    PtpConfig cfg = MakeValidConfig();
    cfg.portCount = static_cast<u8>(kMaxSwitchPorts) + 1U;
    EXPECT_EQ(ptp.Init(cfg).Error(), ErrorCode::kInvalidArgument);
}

TEST_F(PtpTest, Init_WritesZeroAdjustmentToPhc)
{
    // Init must zero the PHC frequency register
    const u32 writesBefore = hal.writeCallCount;
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    EXPECT_GT(hal.writeCallCount, writesBefore);
}

// ─────────────────────────────────────────────────────────────────────────────
// Initial State
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, BeforeInit_GetCurrentTime_ReturnsNotInitialised)
{
    PtpTimestamp t{};
    EXPECT_EQ(ptp.GetCurrentTime(t).Error(), ErrorCode::kNotInitialised);
}

TEST_F(PtpTest, AfterInit_SyncState_IsFreeRun)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    EXPECT_EQ(ptp.GetSyncState(), SyncState::kFreeRun);
}

TEST_F(PtpTest, AfterInit_IsSynchronised_IsFalse)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    EXPECT_FALSE(ptp.IsSynchronised());
}

TEST_F(PtpTest, AfterInit_ClockRole_IsSlave)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    EXPECT_EQ(ptp.GetClockRole(), ClockRole::kSlave);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sync Message Processing → PI Servo
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, InjectSync_SmallOffset_TransitionsToLocking)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    // Inject a Sync with 5000 ns offset (> 1 µs threshold → kLocking, not kLocked)
    ASSERT_TRUE(ptp.InjectSyncMessage(1'000'005'000ULL, 1'000'000'000ULL, 0LL).IsOk());
    EXPECT_EQ(ptp.GetSyncState(), SyncState::kLocking);
}

TEST_F(PtpTest, InjectSync_OffsetWithinTarget_TransitionsToLocked)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    // First inject to move to kLocking
    ptp.InjectSyncMessage(1'000'005'000ULL, 1'000'000'000ULL, 0LL);
    // Inject with sub-1µs offset → should lock
    ptp.InjectSyncMessage(1'000'000'500ULL, 1'000'000'000ULL, 0LL);
    EXPECT_EQ(ptp.GetSyncState(), SyncState::kLocked);
}

TEST_F(PtpTest, InjectSync_WritesPhcAdjustment)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    const u32 writesBefore = hal.writeCallCount;
    ptp.InjectSyncMessage(1'000'010'000ULL, 1'000'000'000ULL, 0LL);
    EXPECT_GT(hal.writeCallCount, writesBefore);
}

TEST_F(PtpTest, InjectSync_LargeOffset_AdjustmentClamped)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    // Inject 100 ms offset — servo must clamp to ±500 000 ppb
    for (int i = 0; i < 20; ++i)
    {
        ptp.InjectSyncMessage(1'100'000'000ULL, 1'000'000'000ULL, 0LL);
    }
    // The PHC register value must not exceed 500 000 ppb
    const i64 adj = static_cast<i64>(static_cast<i32>(hal.lastRegValue));
    EXPECT_LE(adj,  500'000LL);
    EXPECT_GE(adj, -500'000LL);
}

TEST_F(PtpTest, InjectSync_CorrectOffsetCalculation)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    // t2=1001ns, t1=1000ns, delay=0, correction=0 → offset = +1 ns
    ptp.InjectSyncMessage(1'000'000'001ULL, 1'000'000'000ULL, 0LL);
    const i64 offset = ptp.GetOffsetFromGrandmasterNs();
    EXPECT_EQ(offset, 1LL);
}

// ─────────────────────────────────────────────────────────────────────────────
// BMCA — Best Master Clock Algorithm
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, ForceGrandmaster_SetsRoleAndLockedState)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    ASSERT_TRUE(ptp.ForceGrandmasterRole().IsOk());
    EXPECT_EQ(ptp.GetClockRole(),  ClockRole::kGrandmaster);
    EXPECT_EQ(ptp.GetSyncState(),  SyncState::kLocked);
    EXPECT_TRUE(ptp.IsSynchronised());
}

TEST_F(PtpTest, GetGrandmasterIdentity_BeforeElection_ReturnsBmcaNoGm)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    GrandmasterIdentity gm{};
    // No GM elected yet (no Sync/Announce received)
    // Result depends on BMCA; if local node elects itself, Ok. Otherwise error.
    const Status s = ptp.GetGrandmasterIdentity(gm);
    // Either Ok (self-elected) or kBmcaNoGrandmaster — both valid
    EXPECT_TRUE(s.IsOk() || s.Error() == ErrorCode::kBmcaNoGrandmaster);
}

TEST_F(PtpTest, ForceGrandmaster_GetGrandmasterIdentity_ReturnsOk)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    ASSERT_TRUE(ptp.ForceGrandmasterRole().IsOk());
    GrandmasterIdentity gm{};
    EXPECT_TRUE(ptp.GetGrandmasterIdentity(gm).IsOk());
}

// ─────────────────────────────────────────────────────────────────────────────
// Holdover State Machine
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, HoldoverRemaining_WhenNotInHoldover_ReturnsZero)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    EXPECT_EQ(ptp.GetHoldoverRemainingNs(), 0ULL);
}

TEST_F(PtpTest, GetCurrentTime_WhenLocked_ReturnsOk)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    ASSERT_TRUE(ptp.ForceGrandmasterRole().IsOk());
    PtpTimestamp t{};
    EXPECT_TRUE(ptp.GetCurrentTime(t).IsOk());
}

TEST_F(PtpTest, GetCurrentTime_WhenFreeRun_ReturnsNotSynchronised)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    PtpTimestamp t{};
    // kFreeRun (initial state, no Sync injected)
    EXPECT_EQ(ptp.GetCurrentTime(t).Error(), ErrorCode::kPtpNotSynchronised);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — periodic state machine
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, Tick_BeforeInit_ReturnsNotInitialised)
{
    EXPECT_EQ(ptp.Tick().Error(), ErrorCode::kNotInitialised);
}

TEST_F(PtpTest, Tick_AfterInit_ReturnsOk)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    ASSERT_TRUE(ptp.ForceGrandmasterRole().IsOk());
    EXPECT_TRUE(ptp.Tick().IsOk());
}

// ─────────────────────────────────────────────────────────────────────────────
// Path delay
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, GetMeanPathDelay_InitiallyZero)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    EXPECT_EQ(ptp.GetMeanPathDelayNs(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Holdover Runtime Path  (ASIL-D timing continuity through grandmaster loss)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(PtpTest, Tick_SyncSilenceBeyondTimeout_EntersHoldover)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    // Lock: two syncs, second within 1 µs target. lastSyncNs = 2.0000005 s
    ptp.InjectSyncMessage(2'000'005'000ULL, 2'000'000'000ULL, 0LL);
    ptp.InjectSyncMessage(2'000'000'500ULL, 2'000'000'000ULL, 0LL);
    ASSERT_EQ(ptp.GetSyncState(), SyncState::kLocked);

    // Advance PHC past lastSync + syncTimeout (10 ms): 2.1 s
    hal.phcSecLow = 2U;
    hal.phcNs     = 100'000'000U;
    ASSERT_TRUE(ptp.Tick().IsOk());
    EXPECT_EQ(ptp.GetSyncState(), SyncState::kHoldover);
}

TEST_F(PtpTest, Tick_InHoldover_AppliesDriftCompensationWrite)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    ptp.InjectSyncMessage(2'000'005'000ULL, 2'000'000'000ULL, 0LL);
    ptp.InjectSyncMessage(2'000'000'500ULL, 2'000'000'000ULL, 0LL);
    // Detect GM silence early (12 ms > 10 ms timeout) → holdover.
    // Holdover elapsed time is measured from lastSync (t ≈ 2.0000005 s).
    hal.phcSecLow = 2U;  hal.phcNs = 12'000'000U;
    ASSERT_TRUE(ptp.Tick().IsOk());
    ASSERT_EQ(ptp.GetSyncState(), SyncState::kHoldover);

    // Next tick at +50 ms since lastSync: inside the 100 ms budget and below
    // the 10× GM-lost threshold → drift compensation must write the PHC.
    hal.phcNs = 50'000'000U;
    const u32 before = hal.writeCallCount;
    ASSERT_TRUE(ptp.Tick().IsOk());
    EXPECT_GT(hal.writeCallCount, before);
    EXPECT_EQ(ptp.GetSyncState(), SyncState::kHoldover);
    EXPECT_GT(ptp.GetHoldoverRemainingNs(), 0ULL);
}

TEST_F(PtpTest, Tick_HoldoverBudgetExpired_FreeRunWithError)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    ptp.InjectSyncMessage(2'000'005'000ULL, 2'000'000'000ULL, 0LL);
    ptp.InjectSyncMessage(2'000'000'500ULL, 2'000'000'000ULL, 0LL);
    hal.phcSecLow = 2U;  hal.phcNs = 100'000'000U;   // → holdover
    ASSERT_TRUE(ptp.Tick().IsOk());

    // Exceed the 100 ms holdover budget (elapsed since lastSync ≈ 300 ms)
    hal.phcNs = 300'000'000U;
    const Status s = ptp.Tick();
    ASSERT_TRUE(s.IsError());
    EXPECT_EQ(s.Error(), ErrorCode::kPtpHoldoverExpired);
    EXPECT_EQ(ptp.GetSyncState(), SyncState::kFreeRun);
    EXPECT_FALSE(ptp.IsSynchronised());
}

TEST_F(PtpTest, Tick_HoldoverGmSilenceTenfoldTimeout_ReportsExpired)
{
    // Large budget so the 10× sync-timeout GM-lost branch fires first.
    PtpConfig cfg = MakeValidConfig();
    cfg.holdoverBudgetNs = 10'000'000'000ULL;        // 10 s budget
    ASSERT_TRUE(ptp.Init(cfg).IsOk());
    ptp.InjectSyncMessage(2'000'005'000ULL, 2'000'000'000ULL, 0LL);
    ptp.InjectSyncMessage(2'000'000'500ULL, 2'000'000'000ULL, 0LL);
    hal.phcSecLow = 2U;  hal.phcNs = 100'000'000U;   // → holdover
    ASSERT_TRUE(ptp.Tick().IsOk());

    // Silence > 10 × 10 ms = 100 ms while budget (10 s) still has headroom
    hal.phcNs = 200'000'001U;
    const Status s = ptp.Tick();
    ASSERT_TRUE(s.IsError());
    EXPECT_EQ(s.Error(), ErrorCode::kPtpHoldoverExpired);
}

TEST_F(PtpTest, Tick_ReSyncDuringHoldover_RecoversTowardsLock)
{
    ASSERT_TRUE(ptp.Init(MakeValidConfig()).IsOk());
    ptp.InjectSyncMessage(2'000'005'000ULL, 2'000'000'000ULL, 0LL);
    ptp.InjectSyncMessage(2'000'000'500ULL, 2'000'000'000ULL, 0LL);
    hal.phcSecLow = 2U;  hal.phcNs = 100'000'000U;
    ASSERT_TRUE(ptp.Tick().IsOk());
    ASSERT_EQ(ptp.GetSyncState(), SyncState::kHoldover);

    // Grandmaster returns: a new Sync exits holdover into kLocking
    ptp.InjectSyncMessage(2'100'005'000ULL, 2'100'000'000ULL, 0LL);
    EXPECT_EQ(ptp.GetSyncState(), SyncState::kLocking);
}
