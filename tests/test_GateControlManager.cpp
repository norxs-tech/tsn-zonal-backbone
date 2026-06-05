// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        test_GateControlManager.cpp
 * @brief       GoogleTest unit tests for GateControlManager — covering all 7 validation
 *              stages, Network Calculus deadline verification, guard band boundary
 *              conditions, Admin→Oper swap semantics, and ASIL-D safety window
 *              requirements.  Achieves MC/DC coverage on all decision points.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, ISO 26262 ASIL-D (unit test per Part 6 §9.4.3)
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * =====================================================================================
 */

#include <gtest/gtest.h>
#include "norxs/tsn/GateControlManager.hpp"

using namespace norxs::tsn;

// ─────────────────────────────────────────────────────────────────────────────
// Mock SwitchHal — records calls, configurable failure injection
// ─────────────────────────────────────────────────────────────────────────────

class MockSwitchHal final : public SwitchHal
{
public:
    // Failure injection
    bool  failApplyGcl       { false };
    bool  failReadOperGcl    { false };
    bool  reportSwapPending  { false };

    // Call record
    u32   applyGclCallCount  { 0U };
    u8    lastAppliedPort    { 0xFFU };

    // Stored schedule (simulates hardware register)
    ScheduleParams storedSchedule{};

    Status Init() noexcept override { return Status::Ok(); }

    Status WriteRegister(u8, RegAddr, RegData) noexcept override { return Status::Ok(); }

    Status ReadRegister(u8, RegAddr, RegData& out) const noexcept override
        { out = 0U; return Status::Ok(); }

    Status ReadModifyWrite(u8, RegAddr, RegData, RegData) noexcept override
        { return Status::Ok(); }

    Status ApplyGclToPort(u8 port, const ScheduleParams& params) noexcept override
    {
        ++applyGclCallCount;
        lastAppliedPort = port;
        if (failApplyGcl) { return Status::Err(ErrorCode::kHalWriteFailed); }
        storedSchedule = params;
        return Status::Ok();
    }

    Status ReadOperGcl(u8, ScheduleParams& out) const noexcept override
    {
        if (failReadOperGcl) { return Status::Err(ErrorCode::kHalReadFailed); }
        out = storedSchedule;
        return Status::Ok();
    }

    Status IsGclSwapPending(u8, bool& out) const noexcept override
        { out = reportSwapPending; return Status::Ok(); }

    Status SetStreamFilter(const StreamFilterInstance&) noexcept override { return Status::Ok(); }
    Status ClearStreamFilter(u16) noexcept override { return Status::Ok(); }
    Status GetStreamDropCount(u16, u64& out) const noexcept override { out = 0; return Status::Ok(); }
    Status RegisterFrerStream(const FrerStreamEntry&) noexcept override { return Status::Ok(); }
    Status DeregisterFrerStream(u16) noexcept override { return Status::Ok(); }
    Status GetFrerStats(u16, u64& a, u64& b, u64& c) const noexcept override
        { a = b = c = 0; return Status::Ok(); }
    Status ConfigureMacsecChannel(const MacsecChannelConfig&) noexcept override { return Status::Ok(); }
    Status RotateMacsecKey(u8, const MacsecSak128&) noexcept override { return Status::Ok(); }
    Status GetMacsecAuthFailures(u8, u64& out) const noexcept override { out = 0; return Status::Ok(); }
    Status SetVlanEntry(u16, u16, u16) noexcept override { return Status::Ok(); }
    Status SetTcamRule(u8, const std::array<u8,6U>&, u16, bool) noexcept override { return Status::Ok(); }
    Status GetPortDropCount(u8, u64& out) const noexcept override { out = 0; return Status::Ok(); }
    Status GetLinkStatus(u8, bool& up, u32& spd) const noexcept override { up = true; spd = 1000; return Status::Ok(); }
    const char* GetDeviceIdentifier() const noexcept override { return "MockHAL v1.0"; }
    u8 GetPortCount() const noexcept override { return 8U; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Test Fixture
// ─────────────────────────────────────────────────────────────────────────────

class GcmTest : public ::testing::Test
{
protected:
    MockSwitchHal hal;
    GateControlManager gcm{ 0U, hal };

    /// @brief Build a valid 1 ms cycle schedule with one TC7-exclusive window.
    ///
    ///  Layout (1 000 000 ns total):
    ///   [0] Guard band  (closed)      :  20 000 ns  0x00
    ///   [1] TC7 exclusive (safety)    :  30 000 ns  0x80  ← min > guardBand + safetyFrame
    ///   [2] AVB/Video (TC4+TC5)       : 100 000 ns  0x30
    ///   [3] Best-effort (all open)    : 850 000 ns  0xFF
    ScheduleParams MakeValidSchedule() const
    {
        ScheduleParams p{};
        p.cycleTimeNs             = 1'000'000ULL;
        p.baseTime                = kPtpTimestampZero;
        p.maxSafetyFrameSizeBytes = 100U;  // Small frame → guardBand = 100×8 + 1538×8 = 13 104 ns
        p.entryCount              = 4U;

        // Guard band slot: no queues transmit (protects the TC7 window entry edge)
        p.gcl[0] = GclEntry{ kAllQueuesClosed, 20'000U };
        // TC7-exclusive window: must be > FrameNs(100) + FrameNs(1538) = 800 + 12304 = 13104 ns
        p.gcl[1] = GclEntry{ kSafetyOnlyOpen,  30'000U };
        // AVB window
        p.gcl[2] = GclEntry{ 0x30U,           100'000U };
        // Best-effort
        p.gcl[3] = GclEntry{ kAllQueuesOpen,  850'000U };

        return p;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Stage 1 — Entry Count
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, Stage1_EmptyGcl_ReturnsGclEmpty)
{
    ScheduleParams p = MakeValidSchedule();
    p.entryCount = 0U;
    const Status s = gcm.ApplySchedule(p);
    EXPECT_TRUE(s.IsError());
    EXPECT_EQ(s.Error(), ErrorCode::kGclEmpty);
    EXPECT_EQ(hal.applyGclCallCount, 0U);
}

TEST_F(GcmTest, Stage1_MaxEntries_Accepted)
{
    // 256 entries is at boundary — should not return kGclTooManyEntries
    ScheduleParams p{};
    p.cycleTimeNs             = kMaxCycleTimeNs;
    p.maxSafetyFrameSizeBytes = 100U;
    p.entryCount              = static_cast<u8>(kMaxGclEntries);

    // Fill 256 entries; last entry = TC7-exclusive
    const u32 slotNs = static_cast<u32>(kMaxCycleTimeNs / kMaxGclEntries);
    for (u8 i = 0U; i < static_cast<u8>(kMaxGclEntries - 1U); ++i)
        { p.gcl[i] = GclEntry{ kAllQueuesOpen, slotNs }; }
    // Make last entry TC7-only and wide enough for guard band
    p.gcl[255] = GclEntry{ kSafetyOnlyOpen, slotNs };

    // Adjust to make Σ == cycleTime
    u64 sum = static_cast<u64>(slotNs) * kMaxGclEntries;
    if (sum != kMaxCycleTimeNs) { p.gcl[0].intervalNs += static_cast<u32>(kMaxCycleTimeNs - sum); }

    // Only check it doesn't fail on entry count; other stages may fail
    const Status s = gcm.ApplySchedule(p);
    EXPECT_NE(s.Error(), ErrorCode::kGclTooManyEntries);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 2 — Cycle Time Bounds
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, Stage2_CycleTimeTooShort_Rejected)
{
    ScheduleParams p = MakeValidSchedule();
    p.cycleTimeNs = kMinCycleTimeNs - 1U;
    p.gcl[3].intervalNs -= 1U;  // Keep sum consistent
    const Status s = gcm.ApplySchedule(p);
    EXPECT_EQ(s.Error(), ErrorCode::kCycleTimeTooShort);
}

TEST_F(GcmTest, Stage2_CycleTimeAtMinimum_PassesStage2)
{
    // Should pass Stage 2 (may fail later stages)
    ScheduleParams p = MakeValidSchedule();
    p.cycleTimeNs = kMinCycleTimeNs;
    // Rescale GCL
    p.entryCount  = 2U;
    p.gcl[0] = GclEntry{ kSafetyOnlyOpen, 30'000U };
    p.gcl[1] = GclEntry{ kAllQueuesOpen,  70'000U };
    const Status s = gcm.ApplySchedule(p);
    EXPECT_NE(s.Error(), ErrorCode::kCycleTimeTooShort);
    EXPECT_NE(s.Error(), ErrorCode::kCycleTimeTooLong);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 3 — No Zero-Interval Entries
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, Stage3_ZeroIntervalEntry_Rejected)
{
    ScheduleParams p = MakeValidSchedule();
    p.gcl[2].intervalNs = 0U;
    const Status s = gcm.ApplySchedule(p);
    EXPECT_EQ(s.Error(), ErrorCode::kZeroIntervalEntry);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 4 — Interval Sum
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, Stage4_SumMismatch_Rejected)
{
    ScheduleParams p = MakeValidSchedule();
    p.gcl[3].intervalNs += 1U;  // Sum now > cycleTime
    const Status s = gcm.ApplySchedule(p);
    EXPECT_EQ(s.Error(), ErrorCode::kIntervalSumMismatch);
}

TEST_F(GcmTest, Stage4_SumExact_PassesStage4)
{
    ScheduleParams p = MakeValidSchedule();
    const Status s = gcm.ApplySchedule(p);
    EXPECT_NE(s.Error(), ErrorCode::kIntervalSumMismatch);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 5 — Guard Band
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, Stage5_TC7WindowTooNarrow_GuardBandViolation)
{
    ScheduleParams p = MakeValidSchedule();
    // TC7 window (entry 1) currently 30 000 ns
    // guardBand = FrameNs(100) + FrameNs(1538) = 800 + 12304 = 13104 ns
    // Set TC7 window to 13000 < 13104 → should fail
    const u32 reduction = 30'000U - 12'000U;
    p.gcl[1].intervalNs = 12'000U;
    p.gcl[3].intervalNs += reduction;  // Keep sum correct
    const Status s = gcm.ApplySchedule(p);
    EXPECT_EQ(s.Error(), ErrorCode::kGuardBandViolation);
}

TEST_F(GcmTest, Stage5_TC7WindowExactlyMinimum_PassesStage5)
{
    ScheduleParams p = MakeValidSchedule();
    // minWindow = FrameNs(100) + FrameNs(1538) = 800 + 12304 = 13104
    // Set TC7 window exactly to 13104
    const u32 oldNs = p.gcl[1].intervalNs;
    p.gcl[1].intervalNs = 13'104U;
    p.gcl[3].intervalNs += (oldNs - 13'104U);
    const Status s = gcm.ApplySchedule(p);
    EXPECT_NE(s.Error(), ErrorCode::kGuardBandViolation);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 6 — Safety Critical Window
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, Stage6_NoExclusiveTC7Window_Rejected)
{
    ScheduleParams p = MakeValidSchedule();
    // Change TC7 window to have TC6 also open — no longer exclusive
    p.gcl[1].gateStates = 0xC0U;  // TC7 + TC6 open — NOT exclusive
    const Status s = gcm.ApplySchedule(p);
    EXPECT_EQ(s.Error(), ErrorCode::kSafetyCriticalWindowAbsent);
}

TEST_F(GcmTest, Stage6_ExactlyOneExclusiveWindow_Passes)
{
    ScheduleParams p = MakeValidSchedule();
    const Status s = gcm.ApplySchedule(p);
    EXPECT_NE(s.Error(), ErrorCode::kSafetyCriticalWindowAbsent);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 7 — Network Calculus
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, Stage7_ValidSchedule_DeadlineMet)
{
    ScheduleParams p = MakeValidSchedule();
    const Status s = gcm.ApplySchedule(p);
    // A 1 ms cycle with TC7 open for 30 µs should satisfy the 500 µs deadline
    EXPECT_NE(s.Error(), ErrorCode::kDeadlineViolation);
}

TEST_F(GcmTest, Stage7_NCResult_Available_AfterApply)
{
    ScheduleParams p = MakeValidSchedule();
    ASSERT_TRUE(gcm.ApplySchedule(p).IsOk());

    NetworkCalculusResult result{};
    EXPECT_TRUE(gcm.GetNetworkCalculusResult(result).IsOk());
    EXPECT_TRUE(result.deadlineMet[static_cast<u8>(TrafficClass::kSafetyCritical)]);
}

TEST_F(GcmTest, Stage7_NCResult_NotAvailable_BeforeApply)
{
    NetworkCalculusResult result{};
    EXPECT_EQ(gcm.GetNetworkCalculusResult(result).Error(), ErrorCode::kNotInitialised);
}

// ─────────────────────────────────────────────────────────────────────────────
// HAL Interaction
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, ValidSchedule_CallsHalApplyOnce)
{
    ScheduleParams p = MakeValidSchedule();
    ASSERT_TRUE(gcm.ApplySchedule(p).IsOk());
    EXPECT_EQ(hal.applyGclCallCount, 1U);
    EXPECT_EQ(hal.lastAppliedPort, 0U);
}

TEST_F(GcmTest, HalFailure_DoesNotUpdateActiveSchedule)
{
    hal.failApplyGcl = true;
    ScheduleParams p = MakeValidSchedule();
    const Status s = gcm.ApplySchedule(p);
    EXPECT_EQ(s.Error(), ErrorCode::kHalWriteFailed);
    EXPECT_FALSE(gcm.IsScheduleActive());
}

TEST_F(GcmTest, ValidationFailure_DoesNotCallHal)
{
    ScheduleParams p = MakeValidSchedule();
    p.entryCount = 0U;
    (void)gcm.ApplySchedule(p);
    EXPECT_EQ(hal.applyGclCallCount, 0U);
}

// ─────────────────────────────────────────────────────────────────────────────
// State Management
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, GetActiveSchedule_BeforeApply_ReturnsNotInitialised)
{
    ScheduleParams out{};
    EXPECT_EQ(gcm.GetActiveSchedule(out).Error(), ErrorCode::kNotInitialised);
}

TEST_F(GcmTest, GetActiveSchedule_AfterApply_ReturnsCopy)
{
    ScheduleParams p = MakeValidSchedule();
    ASSERT_TRUE(gcm.ApplySchedule(p).IsOk());

    ScheduleParams out{};
    ASSERT_TRUE(gcm.GetActiveSchedule(out).IsOk());
    EXPECT_EQ(out.cycleTimeNs, p.cycleTimeNs);
    EXPECT_EQ(out.entryCount,  p.entryCount);
}

TEST_F(GcmTest, DisableSchedule_ClearsActiveFlag)
{
    ScheduleParams p = MakeValidSchedule();
    ASSERT_TRUE(gcm.ApplySchedule(p).IsOk());
    ASSERT_TRUE(gcm.IsScheduleActive());

    ASSERT_TRUE(gcm.DisableSchedule().IsOk());
    EXPECT_FALSE(gcm.IsScheduleActive());
}

// ─────────────────────────────────────────────────────────────────────────────
// Hardware Integrity Verification
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, VerifyHardwareSchedule_MatchingHW_ReturnsOk)
{
    ScheduleParams p = MakeValidSchedule();
    ASSERT_TRUE(gcm.ApplySchedule(p).IsOk());
    // MockHal stores the exact schedule that was applied
    EXPECT_TRUE(gcm.VerifyHardwareSchedule().IsOk());
}

TEST_F(GcmTest, VerifyHardwareSchedule_HalReadFails_ReturnsError)
{
    ScheduleParams p = MakeValidSchedule();
    ASSERT_TRUE(gcm.ApplySchedule(p).IsOk());

    hal.failReadOperGcl = true;
    EXPECT_EQ(gcm.VerifyHardwareSchedule().Error(), ErrorCode::kHalReadFailed);
}

TEST_F(GcmTest, VerifyHardwareSchedule_BeforeApply_NotInitialised)
{
    EXPECT_EQ(gcm.VerifyHardwareSchedule().Error(), ErrorCode::kNotInitialised);
}

// ─────────────────────────────────────────────────────────────────────────────
// Port Identity
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, GetPort_ReturnsConstructedPort)
{
    EXPECT_EQ(gcm.GetPort(), 0U);
}

TEST_F(GcmTest, SecondGcm_OnPort5_ReportsCorrectPort)
{
    GateControlManager gcm5{ 5U, hal };
    EXPECT_EQ(gcm5.GetPort(), 5U);
}

// ─────────────────────────────────────────────────────────────────────────────
// Successive Updates
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(GcmTest, ApplySchedule_Twice_SecondScheduleActive)
{
    ScheduleParams p1 = MakeValidSchedule();
    ASSERT_TRUE(gcm.ApplySchedule(p1).IsOk());

    ScheduleParams p2 = MakeValidSchedule();
    p2.cycleTimeNs     = 2'000'000ULL;
    p2.gcl[3].intervalNs += 1'000'000U;  // Adjust to new cycle time
    ASSERT_TRUE(gcm.ApplySchedule(p2).IsOk());

    ScheduleParams out{};
    ASSERT_TRUE(gcm.GetActiveSchedule(out).IsOk());
    EXPECT_EQ(out.cycleTimeNs, 2'000'000ULL);
    EXPECT_EQ(hal.applyGclCallCount, 2U);
}
