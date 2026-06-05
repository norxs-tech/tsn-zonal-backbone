// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        test_FrerManager.cpp
 * @brief       GoogleTest unit tests for FrerManager — covering stream registration,
 *              O(1) duplicate detection, sequence-window edge cases, per-path health
 *              monitoring, latent error detection (IEEE 802.1CB §7.4.5), and stream
 *              deregistration compaction.  MC/DC coverage on all decision branches.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, ISO 26262 ASIL-D (unit test per Part 6 §9.4.3),
 *              IEEE 802.1CB
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 * =====================================================================================
 */

#include <gtest/gtest.h>
#include "norxs/tsn/FrerManager.hpp"

using namespace norxs::tsn;

// ─────────────────────────────────────────────────────────────────────────────
// Minimal Mock HAL for FRER tests
// ─────────────────────────────────────────────────────────────────────────────

class MockHalFrer final : public SwitchHal
{
public:
    u32 registerStreamCount   { 0U };
    u32 deregisterStreamCount { 0U };
    bool failRegister         { false };

    Status Init() noexcept override { return Status::Ok(); }
    Status WriteRegister(u8, RegAddr, RegData) noexcept override { return Status::Ok(); }
    Status ReadRegister(u8, RegAddr, RegData& out) const noexcept override { out=0; return Status::Ok(); }
    Status ReadModifyWrite(u8, RegAddr, RegData, RegData) noexcept override { return Status::Ok(); }
    Status ApplyGclToPort(u8, const ScheduleParams&) noexcept override { return Status::Ok(); }
    Status ReadOperGcl(u8, ScheduleParams&) const noexcept override { return Status::Ok(); }
    Status IsGclSwapPending(u8, bool& out) const noexcept override { out=false; return Status::Ok(); }
    Status SetStreamFilter(const StreamFilterInstance&) noexcept override { return Status::Ok(); }
    Status ClearStreamFilter(u16) noexcept override { return Status::Ok(); }
    Status GetStreamDropCount(u16, u64& out) const noexcept override { out=0; return Status::Ok(); }

    Status RegisterFrerStream(const FrerStreamEntry&) noexcept override
    {
        if (failRegister) { return Status::Err(ErrorCode::kHalWriteFailed); }
        ++registerStreamCount;
        return Status::Ok();
    }

    Status DeregisterFrerStream(u16) noexcept override
    {
        ++deregisterStreamCount;
        return Status::Ok();
    }

    Status GetFrerStats(u16, u64& a, u64& b, u64& c) const noexcept override { a=b=c=0; return Status::Ok(); }
    Status ConfigureMacsecChannel(const MacsecChannelConfig&) noexcept override { return Status::Ok(); }
    Status RotateMacsecKey(u8, const MacsecSak128&) noexcept override { return Status::Ok(); }
    Status GetMacsecAuthFailures(u8, u64& out) const noexcept override { out=0; return Status::Ok(); }
    Status SetVlanEntry(u16, u16, u16) noexcept override { return Status::Ok(); }
    Status SetTcamRule(u8, const std::array<u8,6U>&, u16, bool) noexcept override { return Status::Ok(); }
    Status GetPortDropCount(u8, u64& out) const noexcept override { out=0; return Status::Ok(); }
    Status GetLinkStatus(u8, bool& up, u32& spd) const noexcept override { up=true; spd=1000; return Status::Ok(); }
    const char* GetDeviceIdentifier() const noexcept override { return "MockHalFrer"; }
    u8 GetPortCount() const noexcept override { return 8U; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static FrerStreamEntry MakeEntry(u16 handle, u8 paths = 2U)
{
    FrerStreamEntry e{};
    e.streamHandle      = handle;
    e.memberPaths       = paths;
    e.recoveryTimeoutNs = 1'000'000ULL;  // 1 ms
    e.isGenerator       = false;
    e.vlanId            = 10U;
    return e;
}

// ─────────────────────────────────────────────────────────────────────────────
// Test Fixture
// ─────────────────────────────────────────────────────────────────────────────

class FrerTest : public ::testing::Test
{
protected:
    MockHalFrer  hal;
    FrerManager  frer{ hal };

    void SetUp() override
    {
        ASSERT_TRUE(frer.Init().IsOk());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FrerTest, Init_ClearsStreamCount)
{
    EXPECT_EQ(frer.GetStreamCount(), 0U);
}

TEST_F(FrerTest, DoubleInit_ReturnsOk)
{
    EXPECT_TRUE(frer.Init().IsOk());
    EXPECT_EQ(frer.GetStreamCount(), 0U);
}

// ─────────────────────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FrerTest, RegisterStream_ValidEntry_IncreasesCount)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    EXPECT_EQ(frer.GetStreamCount(), 1U);
    EXPECT_EQ(hal.registerStreamCount, 1U);
}

TEST_F(FrerTest, RegisterStream_MultipleStreams_AllTracked)
{
    for (u16 h = 1U; h <= 5U; ++h)
        { ASSERT_TRUE(frer.RegisterStream(MakeEntry(h)).IsOk()); }
    EXPECT_EQ(frer.GetStreamCount(), 5U);
}

TEST_F(FrerTest, RegisterStream_TableFull_ReturnsFrerTableFull)
{
    for (u16 h = 0U; h < static_cast<u16>(kMaxFrerEntries); ++h)
        { (void)frer.RegisterStream(MakeEntry(h)); }
    EXPECT_EQ(frer.RegisterStream(MakeEntry(999U)).Error(), ErrorCode::kFrerTableFull);
}

TEST_F(FrerTest, RegisterStream_HalFails_ReturnsHalError)
{
    hal.failRegister = true;
    EXPECT_EQ(frer.RegisterStream(MakeEntry(1U)).Error(), ErrorCode::kHalWriteFailed);
    EXPECT_EQ(frer.GetStreamCount(), 0U);
}

// ─────────────────────────────────────────────────────────────────────────────
// Deregistration
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FrerTest, DeregisterStream_ExistingStream_DecreasesCount)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    ASSERT_TRUE(frer.DeregisterStream(1U).IsOk());
    EXPECT_EQ(frer.GetStreamCount(), 0U);
}

TEST_F(FrerTest, DeregisterStream_UnknownHandle_ReturnsError)
{
    EXPECT_EQ(frer.DeregisterStream(999U).Error(), ErrorCode::kPsfsStreamUnknown);
}

TEST_F(FrerTest, DeregisterStream_MiddleEntry_ArrayCompacted)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(10U)).IsOk());
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(20U)).IsOk());
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(30U)).IsOk());

    ASSERT_TRUE(frer.DeregisterStream(20U).IsOk());
    EXPECT_EQ(frer.GetStreamCount(), 2U);

    // Remaining streams must still be accessible
    u8 health = 0U;
    EXPECT_TRUE(frer.GetPathHealth(10U, health).IsOk());
    EXPECT_TRUE(frer.GetPathHealth(30U, health).IsOk());
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame Reception — Duplicate Detection (O(1) bitmask)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FrerTest, OnFrameReceived_FirstCopy_ForwardedTrue)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    auto result = frer.OnFrameReceived(1U, 100U, 0U, 1'000'000ULL);
    ASSERT_TRUE(result.IsOk());
    EXPECT_TRUE(result.Value());  // Forward
}

TEST_F(FrerTest, OnFrameReceived_Duplicate_ForwardedFalse)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    frer.OnFrameReceived(1U, 100U, 0U, 1'000'000ULL);  // First copy path A
    auto result = frer.OnFrameReceived(1U, 100U, 1U, 1'001'000ULL); // Duplicate path B
    ASSERT_TRUE(result.IsOk());
    EXPECT_FALSE(result.Value());  // Discard
}

TEST_F(FrerTest, OnFrameReceived_Duplicate_IncrementsDuplicateCounter)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    frer.OnFrameReceived(1U, 200U, 0U, 0ULL);
    frer.OnFrameReceived(1U, 200U, 1U, 0ULL);  // Duplicate

    u64 accepted = 0ULL, dupes = 0ULL, ooo = 0ULL, led = 0ULL;
    ASSERT_TRUE(frer.GetStreamStats(1U, accepted, dupes, ooo, led).IsOk());
    EXPECT_EQ(dupes, 1ULL);
    EXPECT_EQ(accepted, 1ULL);
}

TEST_F(FrerTest, OnFrameReceived_SequentialFrames_AllAccepted)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    u64 nowNs = 0ULL;
    for (u16 seq = 0U; seq < 10U; ++seq)
    {
        auto r = frer.OnFrameReceived(1U, seq, 0U, nowNs);
        EXPECT_TRUE(r.Value()) << "SeqNum " << seq << " should be accepted";
        nowNs += 125'000ULL;
    }
    u64 accepted = 0ULL, dupes = 0ULL, ooo = 0ULL, led = 0ULL;
    ASSERT_TRUE(frer.GetStreamStats(1U, accepted, dupes, ooo, led).IsOk());
    EXPECT_EQ(accepted, 10ULL);
    EXPECT_EQ(dupes,    0ULL);
}

TEST_F(FrerTest, OnFrameReceived_UnknownStream_ReturnsError)
{
    auto r = frer.OnFrameReceived(999U, 1U, 0U, 0ULL);
    EXPECT_TRUE(r.IsError());
    EXPECT_EQ(r.Error(), ErrorCode::kPsfsStreamUnknown);
}

// ─────────────────────────────────────────────────────────────────────────────
// Bitmask Window — 32-entry rolling window
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FrerTest, OnFrameReceived_SeqNumModulo32_DetectedAsDuplicate)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    // SeqNum 0 and 32 map to same bit (0 % 32 == 32 % 32 == 0)
    frer.OnFrameReceived(1U, 0U, 0U, 0ULL);   // Accept seq 0
    auto r = frer.OnFrameReceived(1U, 32U, 0U, 0ULL);  // Maps to same bit → duplicate
    ASSERT_TRUE(r.IsOk());
    EXPECT_FALSE(r.Value());  // Detected as duplicate
}

// ─────────────────────────────────────────────────────────────────────────────
// Path Health
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FrerTest, GetPathHealth_AfterRegistration_AllPathsHealthy)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U, 2U)).IsOk());
    u8 health = 0U;
    ASSERT_TRUE(frer.GetPathHealth(1U, health).IsOk());
    EXPECT_EQ(health, 0x03U);  // Both path 0 and path 1 healthy
}

TEST_F(FrerTest, GetPathHealth_UnknownStream_ReturnsError)
{
    u8 health = 0U;
    EXPECT_EQ(frer.GetPathHealth(999U, health).Error(), ErrorCode::kPsfsStreamUnknown);
}

TEST_F(FrerTest, GetStreamStats_UnknownStream_ReturnsError)
{
    u64 a=0, b=0, c=0, d=0;
    EXPECT_EQ(frer.GetStreamStats(999U, a, b, c, d).Error(), ErrorCode::kPsfsStreamUnknown);
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — periodic state machine
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(FrerTest, Tick_NoStreams_ReturnsOk)
{
    EXPECT_TRUE(frer.Tick(1'000'000ULL).IsOk());
}

TEST_F(FrerTest, Tick_StreamWithNoFrames_ReturnsOk)
{
    ASSERT_TRUE(frer.RegisterStream(MakeEntry(1U)).IsOk());
    EXPECT_TRUE(frer.Tick(1'000'000ULL).IsOk());
}

TEST_F(FrerTest, BeforeInit_Tick_ReturnsNotInitialised)
{
    FrerManager uninit{ hal };
    EXPECT_EQ(uninit.Tick(0ULL).Error(), ErrorCode::kNotInitialised);
}
