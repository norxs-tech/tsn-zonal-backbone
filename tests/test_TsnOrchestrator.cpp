// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        test_TsnOrchestrator.cpp
 * @brief       GoogleTest unit tests for TsnOrchestrator — covering the 8-phase Init
 *              sequence (fail-fast semantics, no partial hardware state), Start/Stop
 *              lifecycle, the kPtpConverging → kRunning transition, the ASIL-D
 *              degradation state machine (kRunning → kDegraded, → kFault), the
 *              circular audit log (ISO 26262 §7.4.2), runtime schedule updates, and
 *              consolidated health metrics aggregation.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, ISO 26262 ASIL-D (unit test per Part 6 §9.4.3)
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * =====================================================================================
 */

#include <gtest/gtest.h>
#include "norxs/tsn/TsnOrchestrator.hpp"

using namespace norxs::tsn;

// ─────────────────────────────────────────────────────────────────────────────
// Mock SwitchHal — call recording, failure injection, controllable PHC clock
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// PHC register addresses mirrored from PtpClockManager.cpp (hardware map)
constexpr RegAddr kPhcSecHighReg = 0x0408U;
constexpr RegAddr kPhcSecLowReg  = 0x040CU;
constexpr RegAddr kPhcNsReg      = 0x0410U;

class MockHalOrch final : public SwitchHal
{
public:
    // ── Failure injection ────────────────────────────────────────────────
    bool failInit          { false };
    bool failVlan          { false };
    bool failStreamFilter  { false };
    bool failApplyGcl      { false };
    bool failFrerRegister  { false };

    // ── Controllable PHC time (drives Orchestrator's notion of "now") ────
    u32  phcSeconds        { 0U };
    u32  phcNanoseconds    { 0U };

    // ── Simulated security telemetry ─────────────────────────────────────
    u64  macsecFailsPerChannel { 0ULL };
    u64  dropsPerStream        { 0ULL };

    // ── Call record ──────────────────────────────────────────────────────
    u32  initCallCount         { 0U };
    u32  vlanCallCount         { 0U };
    u32  streamFilterCount     { 0U };
    u32  applyGclCallCount     { 0U };
    u32  disableGclCallCount   { 0U };
    u32  frerRegisterCount     { 0U };

    ScheduleParams storedSchedule{};

    Status Init() noexcept override
    {
        ++initCallCount;
        return failInit ? Status::Err(ErrorCode::kHalBusTimeout) : Status::Ok();
    }

    Status WriteRegister(u8, RegAddr, RegData) noexcept override { return Status::Ok(); }

    Status ReadRegister(u8, RegAddr address, RegData& outData) const noexcept override
    {
        switch (address)
        {
            case kPhcSecHighReg: outData = 0U;             break;
            case kPhcSecLowReg:  outData = phcSeconds;     break;
            case kPhcNsReg:      outData = phcNanoseconds; break;
            default:             outData = 0U;             break;
        }
        return Status::Ok();
    }

    Status ReadModifyWrite(u8, RegAddr, RegData, RegData) noexcept override { return Status::Ok(); }

    Status ApplyGclToPort(u8, const ScheduleParams& params) noexcept override
    {
        if (failApplyGcl) { return Status::Err(ErrorCode::kHalWriteFailed); }
        ++applyGclCallCount;
        storedSchedule = params;
        return Status::Ok();
    }

    Status ReadOperGcl(u8, ScheduleParams& out) const noexcept override
        { out = storedSchedule; return Status::Ok(); }

    Status IsGclSwapPending(u8, bool& out) const noexcept override
        { out = false; return Status::Ok(); }

    Status SetStreamFilter(const StreamFilterInstance&) noexcept override
    {
        if (failStreamFilter) { return Status::Err(ErrorCode::kHalWriteFailed); }
        ++streamFilterCount;
        return Status::Ok();
    }

    Status ClearStreamFilter(u16) noexcept override { return Status::Ok(); }

    Status GetStreamDropCount(u16, u64& out) const noexcept override
        { out = dropsPerStream; return Status::Ok(); }

    Status RegisterFrerStream(const FrerStreamEntry&) noexcept override
    {
        if (failFrerRegister) { return Status::Err(ErrorCode::kHalWriteFailed); }
        ++frerRegisterCount;
        return Status::Ok();
    }

    Status DeregisterFrerStream(u16) noexcept override { return Status::Ok(); }

    Status GetFrerStats(u16, u64& a, u64& b, u64& c) const noexcept override
        { a = b = c = 0ULL; return Status::Ok(); }

    Status ConfigureMacsecChannel(const MacsecChannelConfig&) noexcept override { return Status::Ok(); }
    Status RotateMacsecKey(u8, const MacsecSak128&) noexcept override { return Status::Ok(); }

    Status GetMacsecAuthFailures(u8, u64& out) const noexcept override
        { out = macsecFailsPerChannel; return Status::Ok(); }

    Status SetVlanEntry(u16, u16, u16) noexcept override
    {
        if (failVlan) { return Status::Err(ErrorCode::kHalWriteFailed); }
        ++vlanCallCount;
        return Status::Ok();
    }

    Status SetTcamRule(u8, const std::array<u8, 6U>&, u16, bool) noexcept override { return Status::Ok(); }
    Status GetPortDropCount(u8, u64& out) const noexcept override { out = 0ULL; return Status::Ok(); }
    Status GetLinkStatus(u8, bool& up, u32& spd) const noexcept override
        { up = true; spd = 1000U; return Status::Ok(); }
    const char* GetDeviceIdentifier() const noexcept override { return "MockHalOrch"; }
    u8 GetPortCount() const noexcept override { return 8U; }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Test Fixture
// ─────────────────────────────────────────────────────────────────────────────

class OrchTest : public ::testing::Test
{
protected:
    MockHalOrch hal;
    TsnOrchestrator orch{ hal };

    /// @brief Valid 1 ms schedule with TC7-exclusive safety window
    ///        (mirrors GcmTest::MakeValidSchedule layout).
    static ScheduleParams MakeValidSchedule()
    {
        ScheduleParams p{};
        p.cycleTimeNs             = 1'000'000ULL;
        p.baseTime                = kPtpTimestampZero;
        p.maxSafetyFrameSizeBytes = 100U;
        p.entryCount              = 4U;
        p.gcl[0] = GclEntry{ kAllQueuesClosed,  20'000U };
        p.gcl[1] = GclEntry{ kSafetyOnlyOpen,   30'000U };
        p.gcl[2] = GclEntry{ 0x30U,            100'000U };
        p.gcl[3] = GclEntry{ kAllQueuesOpen,   850'000U };
        return p;
    }

    /// @brief Configuration whose local clock qualifies for BMCA self-election
    ///        as grandmaster (clockClass < 135) → gPTP locks autonomously.
    static OrchestratorConfig MakeGrandmasterConfig()
    {
        OrchestratorConfig cfg{};

        cfg.ptp.localClockQuality.clockClass    = 6U;     // GM-grade clock
        cfg.ptp.localClockQuality.clockAccuracy = 0x20U;
        cfg.ptp.priority1          = 128U;
        cfg.ptp.priority2          = 128U;
        cfg.ptp.domainNumber       = 0U;
        cfg.ptp.portCount          = 2U;
        cfg.ptp.syncTimeoutNs      = 10'000'000ULL;
        cfg.ptp.holdoverBudgetNs   = 100'000'000ULL;
        cfg.ptp.driftCompensationNs = 1'000ULL;
        cfg.ptp.ports[0].portIndex = 0U;
        cfg.ptp.ports[0].enabled   = true;
        cfg.ptp.ports[1].portIndex = 1U;
        cfg.ptp.ports[1].enabled   = true;

        cfg.schedulePortCount = 2U;
        cfg.schedules[0]      = MakeValidSchedule();
        cfg.schedules[1]      = MakeValidSchedule();

        cfg.frerStreamCount = 1U;
        cfg.frerStreams[0].streamHandle = 7U;
        cfg.frerStreams[0].memberPaths  = 2U;

        cfg.streamFilterCount = 1U;
        cfg.streamFilters[0].streamHandle = 7U;

        cfg.safeStateSchedule = MakeValidSchedule();

        cfg.tickPeriodUs            = 125U;
        cfg.ptpConvergenceTimeoutMs = 500U;
        return cfg;
    }

    /// @brief Drive the orchestrator from kInitializing to kRunning.
    ///        Tick #1: BMCA self-election locks gPTP; the converging check in
    ///        the same tick observes IsSynchronised() and loads all GCLs.
    void DriveToRunning()
    {
        ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsOk());
        ASSERT_TRUE(orch.Start().IsOk());
        for (int i = 0; i < 3 && !orch.IsFullyOperational(); ++i)
        {
            (void)orch.Tick();
        }
        ASSERT_TRUE(orch.IsFullyOperational());
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction & Initial State
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Constructor_InitialState_Uninitialized)
{
    EXPECT_EQ(orch.GetState(), OrchestratorState::kUninitialized);
    EXPECT_FALSE(orch.IsFullyOperational());
}

TEST_F(OrchTest, Constructor_AuditLog_Empty)
{
    std::array<AuditEntry, kAuditLogSize> log{};
    u8 count = 0xFFU;
    ASSERT_TRUE(orch.GetAuditLog(log, count).IsOk());
    EXPECT_EQ(count, 0U);
}

// ─────────────────────────────────────────────────────────────────────────────
// Init — 8-phase sequence, fail-fast semantics
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Init_ValidConfig_ReturnsOk_StateInitializing)
{
    ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsOk());
    EXPECT_EQ(orch.GetState(), OrchestratorState::kInitializing);
}

TEST_F(OrchTest, Init_ConfiguresVlanAndStreamFilters)
{
    ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsOk());
    EXPECT_EQ(hal.vlanCallCount,     1U);
    EXPECT_EQ(hal.streamFilterCount, 1U);
}

TEST_F(OrchTest, Init_HalChipFailure_Propagates_StateUnchanged)
{
    hal.failInit = true;
    const Status s = orch.Init(MakeGrandmasterConfig());
    ASSERT_TRUE(s.IsError());
    EXPECT_EQ(s.Error(), ErrorCode::kHalBusTimeout);
    EXPECT_EQ(orch.GetState(), OrchestratorState::kUninitialized);
}

TEST_F(OrchTest, Init_VlanFailure_FailsFast_NoStreamFiltersTouched)
{
    hal.failVlan = true;
    ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsError());
    // Phase ordering: VLAN (phase 2) fails → stream filters (phase 3) never run
    EXPECT_EQ(hal.streamFilterCount, 0U);
    EXPECT_EQ(orch.GetState(), OrchestratorState::kUninitialized);
}

TEST_F(OrchTest, Init_EmptySchedule_FailsPrevalidation_NoGclApplied)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedules[1].entryCount = 0U;
    const Status s = orch.Init(cfg);
    ASSERT_TRUE(s.IsError());
    EXPECT_EQ(s.Error(), ErrorCode::kGclEmpty);
    EXPECT_EQ(hal.applyGclCallCount, 0U);   // Fail fast before touching GCL HW
}

TEST_F(OrchTest, Init_IntervalSumMismatch_FailsPrevalidation)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedules[0].gcl[3].intervalNs = 850'001U;  // sum ≠ cycleTimeNs
    EXPECT_EQ(orch.Init(cfg).Error(), ErrorCode::kIntervalSumMismatch);
}

TEST_F(OrchTest, Init_MissingSafetyWindow_FailsPrevalidation)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedules[0].gcl[1].gateStates = 0xC0U;  // No TC7-exclusive window left
    EXPECT_EQ(orch.Init(cfg).Error(), ErrorCode::kSafetyCriticalWindowAbsent);
}

// ─────────────────────────────────────────────────────────────────────────────
// Start / Stop lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Start_BeforeInit_ReturnsNotInitialised)
{
    EXPECT_EQ(orch.Start().Error(), ErrorCode::kNotInitialised);
}

TEST_F(OrchTest, Start_AfterInit_EntersPtpConverging)
{
    ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsOk());
    ASSERT_TRUE(orch.Start().IsOk());
    EXPECT_EQ(orch.GetState(), OrchestratorState::kPtpConverging);
}

TEST_F(OrchTest, Stop_FromRunning_DisablesAndReturnsToUninitialized)
{
    DriveToRunning();
    ASSERT_TRUE(orch.Stop().IsOk());
    EXPECT_EQ(orch.GetState(), OrchestratorState::kUninitialized);
    EXPECT_FALSE(orch.IsFullyOperational());
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — converging → running, guard conditions
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Tick_BeforeInit_ReturnsNotInitialised)
{
    EXPECT_EQ(orch.Tick().Error(), ErrorCode::kNotInitialised);
}

TEST_F(OrchTest, Tick_GrandmasterSelfElection_ReachesRunning)
{
    DriveToRunning();
    EXPECT_EQ(orch.GetState(), OrchestratorState::kRunning);
}

TEST_F(OrchTest, Tick_ReachesRunning_AppliesGclOnAllScheduledPorts)
{
    DriveToRunning();
    EXPECT_EQ(hal.applyGclCallCount, 2U);   // schedulePortCount = 2
}

TEST_F(OrchTest, Tick_ReachesRunning_RegistersFrerStreams)
{
    DriveToRunning();
    EXPECT_EQ(hal.frerRegisterCount, 1U);
}

TEST_F(OrchTest, Tick_GclApplyFailsDuringConvergence_EntersFault)
{
    ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsOk());
    ASSERT_TRUE(orch.Start().IsOk());
    hal.failApplyGcl = true;
    const Status s = orch.Tick();
    ASSERT_TRUE(s.IsError());
    EXPECT_EQ(orch.GetState(), OrchestratorState::kFault);
}

TEST_F(OrchTest, Tick_InFaultState_ReturnsOperationNotPermitted)
{
    ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsOk());
    ASSERT_TRUE(orch.Start().IsOk());
    hal.failApplyGcl = true;
    (void)orch.Tick();                       // → kFault
    EXPECT_EQ(orch.Tick().Error(), ErrorCode::kOperationNotPermitted);
}

// ─────────────────────────────────────────────────────────────────────────────
// ASIL-D Degradation State Machine
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Tick_MacsecAuthFailures_DegradesToDegraded)
{
    DriveToRunning();

    // Advance the PHC ≥ 8 ms so the periodic integrity check executes,
    // then report MACsec authentication failures on every secure channel.
    hal.phcSeconds            = 10U;
    hal.macsecFailsPerChannel = 3ULL;

    ASSERT_TRUE(orch.Tick().IsOk());
    EXPECT_EQ(orch.GetState(), OrchestratorState::kDegraded);
    EXPECT_FALSE(orch.IsFullyOperational());
}

TEST_F(OrchTest, Tick_NoFaults_RemainsRunning)
{
    DriveToRunning();
    hal.phcSeconds = 10U;       // Integrity check executes — but clean telemetry
    ASSERT_TRUE(orch.Tick().IsOk());
    EXPECT_EQ(orch.GetState(), OrchestratorState::kRunning);
}

// ─────────────────────────────────────────────────────────────────────────────
// Audit Log (ISO 26262 §7.4.2)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, AuditLog_RecordsTransitionToRunning)
{
    DriveToRunning();
    std::array<AuditEntry, kAuditLogSize> log{};
    u8 count = 0U;
    ASSERT_TRUE(orch.GetAuditLog(log, count).IsOk());
    ASSERT_TRUE(count >= 1U);
    EXPECT_EQ(log[count - 1U].previousState, OrchestratorState::kPtpConverging);
    EXPECT_EQ(log[count - 1U].newState,      OrchestratorState::kRunning);
    EXPECT_EQ(log[count - 1U].triggerCode,   ErrorCode::kOk);
}

TEST_F(OrchTest, AuditLog_RecordsDegradationWithTriggerCode)
{
    DriveToRunning();
    hal.phcSeconds            = 10U;
    hal.macsecFailsPerChannel = 1ULL;
    (void)orch.Tick();

    std::array<AuditEntry, kAuditLogSize> log{};
    u8 count = 0U;
    ASSERT_TRUE(orch.GetAuditLog(log, count).IsOk());
    ASSERT_TRUE(count >= 2U);
    EXPECT_EQ(log[count - 1U].newState,    OrchestratorState::kDegraded);
    EXPECT_EQ(log[count - 1U].triggerCode, ErrorCode::kMacsecReplayAttack);
}

// ─────────────────────────────────────────────────────────────────────────────
// Runtime Schedule Update
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, UpdatePortSchedule_NotRunning_ReturnsOperationNotPermitted)
{
    ASSERT_TRUE(orch.Init(MakeGrandmasterConfig()).IsOk());
    EXPECT_EQ(orch.UpdatePortSchedule(0U, MakeValidSchedule()).Error(),
              ErrorCode::kOperationNotPermitted);
}

TEST_F(OrchTest, UpdatePortSchedule_RunningValidSchedule_AppliesGcl)
{
    DriveToRunning();
    const u32 before = hal.applyGclCallCount;
    ASSERT_TRUE(orch.UpdatePortSchedule(1U, MakeValidSchedule()).IsOk());
    EXPECT_EQ(hal.applyGclCallCount, before + 1U);
}

TEST_F(OrchTest, UpdatePortSchedule_UnmanagedPort_ReturnsPortOutOfRange)
{
    DriveToRunning();
    EXPECT_EQ(orch.UpdatePortSchedule(9U, MakeValidSchedule()).Error(),
              ErrorCode::kHalPortOutOfRange);
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics & Accessors
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, GetGateControlManager_ManagedPort_ReturnsManager)
{
    DriveToRunning();
    const GateControlManager* gcm = nullptr;
    ASSERT_TRUE(orch.GetGateControlManager(0U, gcm).IsOk());
    ASSERT_TRUE(gcm != nullptr);
    EXPECT_EQ(gcm->GetPort(), 0U);
}

TEST_F(OrchTest, GetGateControlManager_UnmanagedPort_ReturnsError)
{
    DriveToRunning();
    const GateControlManager* gcm = nullptr;
    EXPECT_EQ(orch.GetGateControlManager(9U, gcm).Error(),
              ErrorCode::kHalPortOutOfRange);
}

TEST_F(OrchTest, GetHealthMetrics_AggregatesMacsecAndDropCounters)
{
    DriveToRunning();
    hal.macsecFailsPerChannel = 2ULL;   // × kMaxSecureChannels (32) = 64
    hal.dropsPerStream        = 5ULL;   // × 1 configured stream filter = 5

    i64 offsetNs = 0LL;
    u32 faultMask = 0U;
    u64 dropTotal = 0ULL;
    u64 macsecFails = 0ULL;
    ASSERT_TRUE(orch.GetHealthMetrics(offsetNs, faultMask, dropTotal, macsecFails).IsOk());

    EXPECT_EQ(macsecFails, 2ULL * static_cast<u64>(kMaxSecureChannels));
    EXPECT_EQ(dropTotal,   5ULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// Regression — OBS-001: convergence watchdog must fire without a readable PHC
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Tick_SlaveNeverSyncs_ConvergenceWatchdogEntersFault)
{
    // Slave-grade clock (clockClass ≥ 135, priority1 ≠ 0): BMCA will NOT
    // self-elect, no Sync is ever injected → PtpClockManager stays kFreeRun
    // and GetCurrentTime() keeps failing. The watchdog must still fire from
    // the PHC-independent monotonic clock.
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.ptp.localClockQuality.clockClass = 248U;   // slave-only quality
    cfg.ptp.priority1                    = 200U;
    cfg.ptpConvergenceTimeoutMs          = 1U;     // 1 ms
    cfg.tickPeriodUs                     = 125U;   // → watchdog after 9 ticks

    ASSERT_TRUE(orch.Init(cfg).IsOk());
    ASSERT_TRUE(orch.Start().IsOk());

    Status last = Status::Ok();
    for (int i = 0; i < 20 && orch.GetState() != OrchestratorState::kFault; ++i)
    {
        last = orch.Tick();
    }
    EXPECT_EQ(orch.GetState(), OrchestratorState::kFault);
    ASSERT_TRUE(last.IsError());
    EXPECT_EQ(last.Error(), ErrorCode::kPtpNotSynchronised);
}

TEST_F(OrchTest, Tick_GrandmasterLocksBeforeTimeout_NoSpuriousFault)
{
    // Tight 1 ms timeout, but the GM self-elects on the first tick —
    // the watchdog must not produce a spurious fault.
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.ptpConvergenceTimeoutMs = 1U;
    ASSERT_TRUE(orch.Init(cfg).IsOk());
    ASSERT_TRUE(orch.Start().IsOk());
    for (int i = 0; i < 3 && !orch.IsFullyOperational(); ++i) { (void)orch.Tick(); }
    EXPECT_EQ(orch.GetState(), OrchestratorState::kRunning);
}

// ─────────────────────────────────────────────────────────────────────────────
// Regression — OBS-002: full 256-entry GCL must be representable & accepted
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Init_Full256EntryGcl_PassesPrevalidation)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedulePortCount = 1U;

    ScheduleParams& p = cfg.schedules[0];
    p = ScheduleParams{};
    p.cycleTimeNs             = 1'024'000ULL;       // 256 × 4 000 ns
    p.baseTime                = kPtpTimestampZero;
    p.maxSafetyFrameSizeBytes = 100U;
    p.entryCount              = 256U;               // u16 now — representable
    for (u16 i = 0U; i < 256U; ++i)
    {
        p.gcl[i] = GclEntry{ kAllQueuesOpen, 4'000U };
    }
    // Hardware-mandated TC7-exclusive safety window (interval > guard band:
    // 100×8 + 1538×8 = 13 104 ns < 4 000 ns... enlarge one window)
    p.gcl[1] = GclEntry{ kSafetyOnlyOpen, 4'000U };

    // Prevalidation (Stages 1–5 inline) must accept entryCount == 256.
    // Note: 4 000 ns < guard band, so the FULL pipeline at ApplySchedule time
    // would reject Stage 5 — prevalidation scope here is the count/sum/safety
    // window checks that OBS-002 affected.
    EXPECT_TRUE(orch.Init(cfg).IsOk());
}

TEST_F(OrchTest, Init_EntryCountAboveMax_RejectedAsGclEmpty)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedules[0].entryCount = 257U;             // > kMaxGclEntries
    EXPECT_EQ(orch.Init(cfg).Error(), ErrorCode::kGclEmpty);
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration validation (Phase 0)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Init_ZeroTickPeriod_ReturnsInvalidArgument)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.tickPeriodUs = 0U;
    EXPECT_EQ(orch.Init(cfg).Error(), ErrorCode::kInvalidArgument);
    EXPECT_EQ(orch.GetState(), OrchestratorState::kUninitialized);
}

TEST_F(OrchTest, Init_PortCountAboveMax_ReturnsInvalidArgument)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedulePortCount = 17U;                    // kMaxSwitchPorts = 16
    EXPECT_EQ(orch.Init(cfg).Error(), ErrorCode::kInvalidArgument);
}

// ─────────────────────────────────────────────────────────────────────────────
// kDegraded → kSafeState escalation  (NC guarantees void on persistent
// hardware schedule integrity fault — documented FSM transition)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Tick_PersistentGclIntegrityFault_EscalatesToSafeState)
{
    DriveToRunning();
    const u32 gclAppliesAtRunning = hal.applyGclCallCount;

    // Corrupt the simulated hardware GCL so VerifyHardwareSchedule() detects
    // a mismatch against the validated schedule at the next integrity check.
    hal.storedSchedule.gcl[2].intervalNs += 1U;
    hal.phcSeconds = 10U;                       // ≥ 8 ms → integrity check runs

    ASSERT_TRUE(orch.Tick().IsOk());            // integrity fault → kDegraded
    ASSERT_EQ(orch.GetState(), OrchestratorState::kDegraded);

    hal.phcSeconds = 11U;                       // next periodic check window
    ASSERT_TRUE(orch.Tick().IsOk());            // persistent fault → kSafeState
    EXPECT_EQ(orch.GetState(), OrchestratorState::kSafeState);

    // The TC7-exclusive safe-state schedule must have been pushed to all ports
    EXPECT_EQ(hal.applyGclCallCount, gclAppliesAtRunning + 2U);
}

TEST_F(OrchTest, AuditLog_SafeStateEntry_RecordsDeadlineViolationTrigger)
{
    DriveToRunning();
    hal.storedSchedule.gcl[2].intervalNs += 1U;
    hal.phcSeconds = 10U;
    (void)orch.Tick();
    hal.phcSeconds = 11U;
    (void)orch.Tick();
    ASSERT_EQ(orch.GetState(), OrchestratorState::kSafeState);

    std::array<AuditEntry, kAuditLogSize> log{};
    u8 count = 0U;
    ASSERT_TRUE(orch.GetAuditLog(log, count).IsOk());
    ASSERT_TRUE(count >= 3U);
    EXPECT_EQ(log[count - 1U].previousState, OrchestratorState::kDegraded);
    EXPECT_EQ(log[count - 1U].newState,      OrchestratorState::kSafeState);
    EXPECT_EQ(log[count - 1U].triggerCode,   ErrorCode::kDeadlineViolation);
}

// ─────────────────────────────────────────────────────────────────────────────
// Prevalidation branch completion + accessor smoke tests
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrchTest, Init_CycleTimeTooShort_FailsPrevalidation)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedules[0].cycleTimeNs = 50'000ULL;   // < kMinCycleTimeNs (100 µs)
    EXPECT_EQ(orch.Init(cfg).Error(), ErrorCode::kCycleTimeTooShort);
}

TEST_F(OrchTest, Init_ZeroIntervalEntry_FailsPrevalidation)
{
    OrchestratorConfig cfg = MakeGrandmasterConfig();
    cfg.schedules[0].gcl[2].intervalNs = 0U;
    EXPECT_EQ(orch.Init(cfg).Error(), ErrorCode::kZeroIntervalEntry);
}

TEST_F(OrchTest, SubManagerAccessors_ReflectRunningSystemState)
{
    DriveToRunning();
    EXPECT_TRUE(orch.GetPtpClockManager().IsSynchronised());
    // The single configured FRER stream was registered during convergence
    EXPECT_EQ(orch.GetFrerManager().GetStreamCount(), 1U);
}
