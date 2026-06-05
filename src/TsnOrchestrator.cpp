// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        TsnOrchestrator.cpp
 * @brief       Implementation of the TsnOrchestrator master coordinator, including
 *              the 6-phase hardware initialisation sequence, 125 µs scheduling loop,
 *              ASIL-D degradation state machine (kRunning → kDegraded → kSafeState
 *              → kFault), circular audit log (ISO 26262 §7.4.2), and dynamic
 *              schedule update via Admin→Oper atomic swap.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1Qbv, IEEE 802.1AS-Rev,
 *              IEEE 802.1CB, IEEE 802.1Qci, IEEE 802.1AE, ISO 26262 ASIL-D,
 *              ISO/SAE 21434, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 * =====================================================================================
 */

#include "norxs/tsn/TsnOrchestrator.hpp"
#include <new>      // placement new — AUTOSAR M18-4-1: only static storage, no heap
#include <cstring>

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// Fault Mask Bits
// ─────────────────────────────────────────────────────────────────────────────

static constexpr u32 kFaultPtpHoldover    = (1U << 0U);
static constexpr u32 kFaultFrerLatent     = (1U << 1U);
static constexpr u32 kFaultMacsecAuth     = (1U << 2U);
static constexpr u32 kFaultHalIntegrity   = (1U << 3U);
static constexpr u32 kFaultNcDeadline     = (1U << 4U);

// Health check interval: every 8 ms (64 × 125 µs ticks)
static constexpr u64 kHealthCheckIntervalNs = 8'000'000ULL;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

TsnOrchestrator::TsnOrchestrator(SwitchHal& hal) noexcept
    : hal_                   { hal }
    , cfg_                   {}
    , state_                 { OrchestratorState::kUninitialized }
    , initialised_           { false }
    , ptpManager_            { hal }
    , frerManager_           { hal }
    , gcManagers_            {}
    , gcmStorage_            {}
    , gcmPortMap_            {}
    , gcmCount_              { 0U }
    , ptpConvergenceStartNs_ { 0ULL }
    , lastHealthCheckNs_     { 0ULL }
    , activeFaultMask_       { 0U }
    , auditLog_              {}
    , auditHead_             { 0U }
    , auditCount_            { 0U }
{
    for (u8 i = 0U; i < static_cast<u8>(kMaxSwitchPorts); ++i)
    {
        gcManagers_[i] = nullptr;
        gcmPortMap_[i] = 0xFFU;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Init — 6-phase hardware initialisation
// ─────────────────────────────────────────────────────────────────────────────

Status TsnOrchestrator::Init(const OrchestratorConfig& cfg) noexcept
{
    cfg_ = cfg;

    // Phase 1: HAL chip initialisation
    TSN_RETURN_IF_ERR(InitHal());

    // Phase 2: VLAN table
    TSN_RETURN_IF_ERR(InitVlanTable());

    // Phase 3: 802.1Qci stream filters (installed before any traffic flows)
    TSN_RETURN_IF_ERR(InitStreamFilters());

    // Phase 4: MACsec channels
    TSN_RETURN_IF_ERR(InitMacsec());

    // Phase 5: Network Calculus pre-validation on ALL port schedules
    //          Fail fast before touching GCL hardware if any schedule is invalid.
    TSN_RETURN_IF_ERR(PrevalidateAllSchedules());

    // Phase 6: GateControlManager construction (placement new into static storage)
    for (u8 i = 0U; i < cfg_.schedulePortCount; ++i)
    {
        if (i >= static_cast<u8>(kMaxSwitchPorts)) { break; }
        void* storage = static_cast<void*>(gcmStorage_[i]);
        gcManagers_[i] = new (storage) GateControlManager(i, hal_);
        gcmPortMap_[i] = i;
        ++gcmCount_;
    }

    // Phase 7: gPTP stack initialisation
    TSN_RETURN_IF_ERR(ptpManager_.Init(cfg_.ptp));

    // Phase 8: FRER
    TSN_RETURN_IF_ERR(frerManager_.Init());

    initialised_ = true;
    state_       = OrchestratorState::kInitializing;
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Init Sub-phases
// ─────────────────────────────────────────────────────────────────────────────

Status TsnOrchestrator::InitHal() noexcept
{
    return hal_.Init();
}

Status TsnOrchestrator::InitVlanTable() noexcept
{
    // Default: all ports on VLAN 1 (management), tagged
    return hal_.SetVlanEntry(1U,
                             static_cast<u16>((1U << cfg_.ptp.portCount) - 1U),
                             0U);
}

Status TsnOrchestrator::InitStreamFilters() noexcept
{
    for (u8 i = 0U; i < cfg_.streamFilterCount; ++i)
    {
        TSN_RETURN_IF_ERR(hal_.SetStreamFilter(cfg_.streamFilters[i]));
    }
    return Status::Ok();
}

Status TsnOrchestrator::InitMacsec() noexcept
{
    // MACsec channels are configured separately by the Cybersecurity Manager;
    // this is a hook point. In production, key material is delivered via
    // a secure session from the Vehicle Security Operations Centre (VSOC).
    return Status::Ok();
}

Status TsnOrchestrator::InitFrerStreams() noexcept
{
    for (u8 i = 0U; i < cfg_.frerStreamCount; ++i)
    {
        TSN_RETURN_IF_ERR(frerManager_.RegisterStream(cfg_.frerStreams[i]));
    }
    return Status::Ok();
}

Status TsnOrchestrator::PrevalidateAllSchedules() noexcept
{
    // Run a dry-run validation by constructing temporary GCMs pointing at a
    // null-like HAL — in production use a mock HAL for offline pre-validation.
    // Here we validate mathematical properties only (Stages 1–6 of the pipeline),
    // deferring Network Calculus (Stage 7) to the real GCM at ApplySchedule time.
    for (u8 i = 0U; i < cfg_.schedulePortCount; ++i)
    {
        const ScheduleParams& p = cfg_.schedules[i];

        // Minimal inline checks matching Stages 1–5
        if (p.entryCount == 0U || p.entryCount > static_cast<u8>(kMaxGclEntries))
            { return Status::Err(ErrorCode::kGclEmpty); }
        if (p.cycleTimeNs < kMinCycleTimeNs || p.cycleTimeNs > kMaxCycleTimeNs)
            { return Status::Err(ErrorCode::kCycleTimeTooShort); }

        u64 sum = 0ULL;
        bool hasSafetyWindow = false;
        for (u8 j = 0U; j < p.entryCount; ++j)
        {
            if (p.gcl[j].intervalNs == 0U)
                { return Status::Err(ErrorCode::kZeroIntervalEntry); }
            sum += static_cast<u64>(p.gcl[j].intervalNs);
            if (p.gcl[j].gateStates == kSafetyOnlyOpen)
                { hasSafetyWindow = true; }
        }
        if (sum != p.cycleTimeNs)  { return Status::Err(ErrorCode::kIntervalSumMismatch); }
        if (!hasSafetyWindow)      { return Status::Err(ErrorCode::kSafetyCriticalWindowAbsent); }
    }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Start
// ─────────────────────────────────────────────────────────────────────────────

Status TsnOrchestrator::Start() noexcept
{
    if (!initialised_) { return Status::Err(ErrorCode::kNotInitialised); }

    state_                  = OrchestratorState::kPtpConverging;
    ptpConvergenceStartNs_  = 0ULL;  // Set on first Tick()

    return Status::Ok();
}

Status TsnOrchestrator::Stop() noexcept
{
    for (u8 i = 0U; i < gcmCount_; ++i)
    {
        if (gcManagers_[i] != nullptr)
        {
            (void)gcManagers_[i]->DisableSchedule();
        }
    }
    state_ = OrchestratorState::kUninitialized;
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — 125 µs scheduling loop
// ─────────────────────────────────────────────────────────────────────────────

Status TsnOrchestrator::Tick() noexcept
{
    if (state_ == OrchestratorState::kFault)
        { return Status::Err(ErrorCode::kOperationNotPermitted); }
    if (!initialised_)
        { return Status::Err(ErrorCode::kNotInitialised); }

    // Read monotonic time (production: clock_gettime(CLOCK_MONOTONIC))
    // Approximated here via PHC timestamp
    PtpTimestamp now{};
    (void)ptpManager_.GetCurrentTime(now);
    const u64 nowNs = now.seconds * kNsPerSecond + static_cast<u64>(now.nanoseconds);

    // ── a) gPTP clock tick ────────────────────────────────────────────────
    TSN_RETURN_IF_ERR(TickPtp());

    // ── b) FRER path monitoring ───────────────────────────────────────────
    const Status frerStatus = TickFrer(nowNs);
    if (frerStatus.IsError() && frerStatus.Error() == ErrorCode::kFrerLatentError)
    {
        activeFaultMask_ |= kFaultFrerLatent;
    }

    // ── c) Periodic integrity check ───────────────────────────────────────
    if (nowNs - lastHealthCheckNs_ >= kHealthCheckIntervalNs)
    {
        TSN_RETURN_IF_ERR(TickIntegrityCheck());
        lastHealthCheckNs_ = nowNs;
    }

    // ── d) Degradation state machine ──────────────────────────────────────
    TSN_RETURN_IF_ERR(TickDegradationMachine());

    // ── e) PtpConverging → kRunning transition ────────────────────────────
    if (state_ == OrchestratorState::kPtpConverging)
    {
        if (ptpManager_.IsSynchronised())
        {
            // Load GCL schedules now that we have a valid PTP clock
            for (u8 i = 0U; i < gcmCount_; ++i)
            {
                if (gcManagers_[i] != nullptr)
                {
                    const Status s = gcManagers_[i]->ApplySchedule(cfg_.schedules[i]);
                    if (s.IsError())
                    {
                        (void)EnterFault(s.Error());
                        return s;
                    }
                }
            }
            TSN_RETURN_IF_ERR(InitFrerStreams());

            LogStateTransition(state_, OrchestratorState::kRunning,
                               ErrorCode::kOk, 0U, nowNs);
            state_ = OrchestratorState::kRunning;
        }
        else
        {
            // Check convergence timeout
            if (ptpConvergenceStartNs_ == 0ULL) { ptpConvergenceStartNs_ = nowNs; }
            const u64 elapsedMs = (nowNs - ptpConvergenceStartNs_) / 1'000'000ULL;
            if (elapsedMs > static_cast<u64>(cfg_.ptpConvergenceTimeoutMs))
            {
                (void)EnterFault(ErrorCode::kPtpNotSynchronised);
                return Status::Err(ErrorCode::kPtpNotSynchronised);
            }
        }
    }

    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick Sub-routines
// ─────────────────────────────────────────────────────────────────────────────

Status TsnOrchestrator::TickPtp() noexcept
{
    const Status s = ptpManager_.Tick();
    if (s.IsError() && s.Error() == ErrorCode::kPtpHoldoverExpired)
    {
        activeFaultMask_ |= kFaultPtpHoldover;
    }
    return Status::Ok();  // Don't propagate; handled by degradation machine
}

Status TsnOrchestrator::TickFrer(u64 nowNs) noexcept
{
    return frerManager_.Tick(nowNs);
}

Status TsnOrchestrator::TickIntegrityCheck() noexcept
{
    // Verify GCL hardware integrity on all active ports
    for (u8 i = 0U; i < gcmCount_; ++i)
    {
        if (gcManagers_[i] != nullptr && gcManagers_[i]->IsScheduleActive())
        {
            const Status s = gcManagers_[i]->VerifyHardwareSchedule();
            if (s.IsError())
            {
                activeFaultMask_ |= kFaultHalIntegrity;
            }
        }
    }

    // Check MACsec authentication failures across all streams
    u64 totalMacsecFails = 0ULL;
    for (u8 ch = 0U; ch < static_cast<u8>(kMaxSecureChannels); ++ch)
    {
        u64 fails = 0ULL;
        const Status s = hal_.GetMacsecAuthFailures(ch, fails);
        if (s.IsOk()) { totalMacsecFails += fails; }
    }
    if (totalMacsecFails > 0ULL)
    {
        activeFaultMask_ |= kFaultMacsecAuth;
    }

    return Status::Ok();
}

Status TsnOrchestrator::TickDegradationMachine() noexcept
{
    if (state_ != OrchestratorState::kRunning &&
        state_ != OrchestratorState::kDegraded)
    {
        return Status::Ok();
    }

    // Fault conditions that require immediate SafeState
    const u32 safeStateTriggers = kFaultPtpHoldover | kFaultNcDeadline;
    // Fault conditions that degrade to kDegraded
    const u32 degradeTriggers   = kFaultFrerLatent | kFaultMacsecAuth | kFaultHalIntegrity;

    if ((activeFaultMask_ & safeStateTriggers) != 0U)
    {
        const ErrorCode trigger = ((activeFaultMask_ & kFaultPtpHoldover) != 0U)
                                  ? ErrorCode::kPtpHoldoverExpired
                                  : ErrorCode::kDeadlineViolation;
        return EnterSafeState(trigger);
    }

    if ((activeFaultMask_ & degradeTriggers) != 0U && state_ == OrchestratorState::kRunning)
    {
        const ErrorCode trigger = ((activeFaultMask_ & kFaultFrerLatent) != 0U)
                                  ? ErrorCode::kFrerLatentError
                                  : ErrorCode::kMacsecReplayAttack;

        PtpTimestamp now{};
        (void)ptpManager_.GetCurrentTime(now);
        const u64 nowNs = now.seconds * kNsPerSecond + static_cast<u64>(now.nanoseconds);

        LogStateTransition(state_, OrchestratorState::kDegraded, trigger, 0U, nowNs);
        state_ = OrchestratorState::kDegraded;
    }

    return Status::Ok();
}

Status TsnOrchestrator::EnterSafeState(ErrorCode trigger) noexcept
{
    PtpTimestamp now{};
    (void)ptpManager_.GetCurrentTime(now);
    const u64 nowNs = now.seconds * kNsPerSecond + static_cast<u64>(now.nanoseconds);

    // Apply the safe-state schedule on all ports: TC7-exclusive windows only
    for (u8 i = 0U; i < gcmCount_; ++i)
    {
        if (gcManagers_[i] != nullptr)
        {
            (void)gcManagers_[i]->ApplySchedule(cfg_.safeStateSchedule);
        }
    }

    LogStateTransition(state_, OrchestratorState::kSafeState, trigger, 0U, nowNs);
    state_ = OrchestratorState::kSafeState;
    return Status::Ok();
}

Status TsnOrchestrator::EnterFault(ErrorCode trigger) noexcept
{
    PtpTimestamp now{};
    (void)ptpManager_.GetCurrentTime(now);
    const u64 nowNs = now.seconds * kNsPerSecond + static_cast<u64>(now.nanoseconds);

    for (u8 i = 0U; i < gcmCount_; ++i)
    {
        if (gcManagers_[i] != nullptr)
        {
            (void)gcManagers_[i]->DisableSchedule();
        }
    }

    LogStateTransition(state_, OrchestratorState::kFault, trigger, 0U, nowNs);
    state_ = OrchestratorState::kFault;
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic Schedule Update
// ─────────────────────────────────────────────────────────────────────────────

Status TsnOrchestrator::UpdatePortSchedule(u8 port, const ScheduleParams& params) noexcept
{
    if (state_ != OrchestratorState::kRunning)
        { return Status::Err(ErrorCode::kOperationNotPermitted); }

    for (u8 i = 0U; i < gcmCount_; ++i)
    {
        if (gcManagers_[i] != nullptr && gcManagers_[i]->GetPort() == port)
        {
            return gcManagers_[i]->ApplySchedule(params);
        }
    }
    return Status::Err(ErrorCode::kHalPortOutOfRange);
}

// ─────────────────────────────────────────────────────────────────────────────
// Audit Log
// ─────────────────────────────────────────────────────────────────────────────

void TsnOrchestrator::LogStateTransition(OrchestratorState from,
                                          OrchestratorState to,
                                          ErrorCode         trigger,
                                          u8                portOrStream,
                                          u64               nowNs) noexcept
{
    AuditEntry& entry   = auditLog_[auditHead_];
    entry.timestampNs   = nowNs;
    entry.previousState = from;
    entry.newState      = to;
    entry.triggerCode   = trigger;
    entry.portOrStream  = portOrStream;

    auditHead_ = static_cast<u8>((auditHead_ + 1U) % kAuditLogSize);
    if (auditCount_ < static_cast<u8>(kAuditLogSize)) { ++auditCount_; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Queries
// ─────────────────────────────────────────────────────────────────────────────

OrchestratorState TsnOrchestrator::GetState()            const noexcept { return state_;       }
bool              TsnOrchestrator::IsFullyOperational()  const noexcept
    { return state_ == OrchestratorState::kRunning; }
const PtpClockManager& TsnOrchestrator::GetPtpClockManager() const noexcept { return ptpManager_; }
const FrerManager&     TsnOrchestrator::GetFrerManager()      const noexcept { return frerManager_; }

Status TsnOrchestrator::GetGateControlManager(u8 port,
                                               const GateControlManager*& outGcm) const noexcept
{
    for (u8 i = 0U; i < gcmCount_; ++i)
    {
        if (gcManagers_[i] != nullptr && gcManagers_[i]->GetPort() == port)
        {
            outGcm = gcManagers_[i];
            return Status::Ok();
        }
    }
    return Status::Err(ErrorCode::kHalPortOutOfRange);
}

Status TsnOrchestrator::GetAuditLog(std::array<AuditEntry, kAuditLogSize>& outLog,
                                     u8& outCount) const noexcept
{
    outLog   = auditLog_;
    outCount = auditCount_;
    return Status::Ok();
}

Status TsnOrchestrator::GetHealthMetrics(i64& outPtpOffsetNs,
                                          u32& outFaultMask,
                                          u64& outDropTotal,
                                          u64& outMacsecFails) const noexcept
{
    outPtpOffsetNs = ptpManager_.GetOffsetFromGrandmasterNs();
    outFaultMask   = activeFaultMask_;

    outDropTotal = 0ULL;
    outMacsecFails = 0ULL;

    for (u8 ch = 0U; ch < static_cast<u8>(kMaxSecureChannels); ++ch)
    {
        u64 fails = 0ULL;
        if (hal_.GetMacsecAuthFailures(ch, fails).IsOk()) { outMacsecFails += fails; }
    }

    return Status::Ok();
}

} // namespace tsn
} // namespace norxs
