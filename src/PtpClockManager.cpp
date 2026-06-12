// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        PtpClockManager.cpp
 * @brief       Implementation of the IEEE 802.1AS-Rev gPTP clock manager including
 *              integer-scaled PI servo, BMCA dataset comparison, TCXO-compensated
 *              holdover state machine, and hardware PHC register interaction via
 *              SwitchHal.  All arithmetic is integer-only to comply with
 *              MISRA C++:2008 Rule 6-4-1 (no floating-point arithmetic).
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1AS-Rev, ISO 26262 ASIL-D
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 * =====================================================================================
 */

#include "norxs/tsn/PtpClockManager.hpp"
#include <cstring>

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// PI Servo Constants (scaled integer, no FP)
//
//   Kp = 1/4   → implemented as arithmetic right-shift by 2
//   Ki = 1/32  → implemented as arithmetic right-shift by 5
//
//   These values are appropriate for a TCXO oscillator with ±2 ppm accuracy
//   operating on a 125 µs tick period.  Adjust for customer oscillator spec.
//
//   Anti-windup clamp: ±500 000 000 ns·tick (≈ 500 ms accumulation)
// ─────────────────────────────────────────────────────────────────────────────

static constexpr i64 kKpShift              = 2;   // Kp = 2^(-2)  = 0.250
static constexpr i64 kKiShift              = 5;   // Ki = 2^(-5)  = 0.031
static constexpr i64 kIntegralClamp        = 500'000'000LL;
static constexpr i64 kAdjustmentClampPpb   = 500'000LL;  // ±500 000 ppb = ±500 ppm

// PHC register addresses (i.MX8X enet_qos — normalised; real addresses in BSP)
static constexpr RegAddr kPhcFreqAdjReg    = 0x0414U; // ENET_QOS_MAC_SYSTEM_TIME_NANOSECONDS_UPDATE
static constexpr RegAddr kPhcSecHighReg    = 0x0408U;
static constexpr RegAddr kPhcSecLowReg     = 0x040CU;
static constexpr RegAddr kPhcNsReg         = 0x0410U;

// BMCA: unreachable clock class (IEEE 802.1AS §10.3.2)
static constexpr u8 kClockClassUnreachable = 255U;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

PtpClockManager::PtpClockManager(SwitchHal& hal) noexcept
    : hal_              { hal }
    , cfg_              {}
    , initialised_      { false }
    , syncState_        { SyncState::kFreeRun }
    , clockRole_        { ClockRole::kSlave }
    , electedGm_        {}
    , gmValid_          { false }
    , offsetFromGmNs_   { 0LL }
    , meanPathDelayNs_  { 0 }
    , integral_         { 0LL }
    , lastSyncNs_       { 0ULL }
    , holdoverStartNs_  { 0ULL }
    , holdoverUsedNs_   { 0ULL }
    , ports_            {}
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

Status PtpClockManager::Init(const PtpConfig& cfg) noexcept
{
    if (cfg.portCount == 0U || cfg.portCount > static_cast<u8>(kMaxSwitchPorts))
    {
        return Status::Err(ErrorCode::kInvalidArgument);
    }

    cfg_ = cfg;
    for (u8 i = 0U; i < cfg.portCount; ++i)
    {
        ports_[i] = cfg.ports[i];
    }

    // Reset PHC frequency adjustment register to 0 ppb
    TSN_RETURN_IF_ERR(ApplyPhcAdjustment(0LL));

    syncState_   = SyncState::kFreeRun;
    clockRole_   = ClockRole::kSlave;
    gmValid_     = false;
    integral_    = 0LL;
    initialised_ = true;

    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — called every 125 µs by TsnOrchestrator
// ─────────────────────────────────────────────────────────────────────────────

Status PtpClockManager::Tick() noexcept
{
    if (!initialised_) { return Status::Err(ErrorCode::kNotInitialised); }

    // Read current PHC time to compute elapsed since last Sync
    PtpTimestamp now{};
    {
        RegData secHigh = 0U, secLow = 0U, ns = 0U;
        TSN_RETURN_IF_ERR(hal_.ReadRegister(kGlobalPort, kPhcSecHighReg,  secHigh));
        TSN_RETURN_IF_ERR(hal_.ReadRegister(kGlobalPort, kPhcSecLowReg,   secLow));
        TSN_RETURN_IF_ERR(hal_.ReadRegister(kGlobalPort, kPhcNsReg,       ns));
        now.seconds     = (static_cast<u64>(secHigh) << 32U) | static_cast<u64>(secLow);
        now.nanoseconds = ns;
    }

    const u64 nowNs = now.seconds * kNsPerSecond + static_cast<u64>(now.nanoseconds);

    // ── Holdover state management ─────────────────────────────────────────
    if (syncState_ == SyncState::kHoldover)
    {
        const u64 holdoverElapsedNs = nowNs - holdoverStartNs_;
        TSN_RETURN_IF_ERR(ApplyHoldoverDrift(holdoverElapsedNs));

        holdoverUsedNs_ = holdoverElapsedNs;
        if (holdoverUsedNs_ > cfg_.holdoverBudgetNs)
        {
            syncState_ = SyncState::kFreeRun;
            return Status::Err(ErrorCode::kPtpHoldoverExpired);
        }

        // No new Sync received; check if GM silence timeout exceeded
        const u64 silenceNs = nowNs - lastSyncNs_;
        if (silenceNs > cfg_.syncTimeoutNs * 10U)  // 10× timeout = GM lost
        {
            return Status::Err(ErrorCode::kPtpHoldoverExpired);
        }
        return Status::Ok();
    }

    // ── Sync timeout detection ────────────────────────────────────────────
    if (lastSyncNs_ != 0ULL)
    {
        const u64 silenceNs = nowNs - lastSyncNs_;
        if (silenceNs > cfg_.syncTimeoutNs && syncState_ == SyncState::kLocked)
        {
            EnterHoldover();
            return Status::Ok();
        }
    }

    // ── BMCA evaluation ──────────────────────────────────────────────────
    RunBmca();

    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Sync Message Injection (real path: called from NIC driver ISR via callback)
// ─────────────────────────────────────────────────────────────────────────────

Status PtpClockManager::InjectSyncMessage(u64 rxTimestampNs,
                                           u64 txTimestampNs,
                                           i64 correctionNs) noexcept
{
    if (!initialised_) { return Status::Err(ErrorCode::kNotInitialised); }

    // IEEE 802.1AS §11.3.2: offsetFromMaster = t2 - t1 - delay - correction
    // Using signed arithmetic; meanPathDelayNs_ is measured separately via Pdelay
    const i64 rawOffset = static_cast<i64>(rxTimestampNs)
                        - static_cast<i64>(txTimestampNs)
                        - static_cast<i64>(meanPathDelayNs_)
                        - correctionNs;

    offsetFromGmNs_ = rawOffset;
    lastSyncNs_     = rxTimestampNs;

    // Run PI servo and apply PHC adjustment
    const i64 adjustmentPpb = RunServo(rawOffset);
    TSN_RETURN_IF_ERR(ApplyPhcAdjustment(adjustmentPpb));

    // Transition from kLocking → kLocked when offset within target
    const i64 absOffset = (rawOffset < 0LL) ? -rawOffset : rawOffset;
    if (syncState_ == SyncState::kLocking &&
        static_cast<u64>(absOffset) < kPtpSyncTargetNs)
    {
        syncState_ = SyncState::kLocked;
    }
    else if (syncState_ == SyncState::kFreeRun || syncState_ == SyncState::kHoldover)
    {
        syncState_      = SyncState::kLocking;
        holdoverUsedNs_ = 0ULL;
    }

    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// PI Servo — integer scaled
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Portable arithmetic (sign-extending, floor) right shift for i64.
///
///        In C++14, right-shifting a negative signed value is
///        implementation-defined (resolved only in C++20) — flagged by
///        cppcheck [shiftNegativeLHS] and MISRA C++:2008 Rule 5-8-1 territory.
///        This helper reproduces the arithmetic-shift result exactly using
///        well-defined unsigned operations: for v < 0,
///        ~((~v) >> s) sign-extends, matching floor(v / 2^s).
static i64 ArithmeticShiftRight(i64 value, i64 shift) noexcept
{
    if (value >= 0)
    {
        return static_cast<i64>(static_cast<u64>(value) >> shift);
    }
    return static_cast<i64>(~((~static_cast<u64>(value)) >> shift));
}

i64 PtpClockManager::RunServo(i64 offsetNs) noexcept
{
    // Proportional term: offset / 2^kKpShift (arithmetic shift, portable)
    const i64 proportional = ArithmeticShiftRight(offsetNs, kKpShift);

    // Integral accumulation with anti-windup clamp
    integral_ += offsetNs;
    if (integral_ >  kIntegralClamp) { integral_ =  kIntegralClamp; }
    if (integral_ < -kIntegralClamp) { integral_ = -kIntegralClamp; }

    const i64 integralTerm = ArithmeticShiftRight(integral_, kKiShift);

    // Total adjustment (negated: positive offset → slow down clock)
    i64 adjustment = -(proportional + integralTerm);

    // Clamp to safe PHC register range
    if (adjustment >  kAdjustmentClampPpb) { adjustment =  kAdjustmentClampPpb; }
    if (adjustment < -kAdjustmentClampPpb) { adjustment = -kAdjustmentClampPpb; }

    return adjustment;
}

Status PtpClockManager::ApplyPhcAdjustment(i64 adjustmentPpb) noexcept
{
    // Write signed ppb value to i.MX8X enet_qos frequency adjust register
    // Register is a 32-bit 2's-complement value in ppb
    const RegData regVal = static_cast<RegData>(adjustmentPpb & 0xFFFF'FFFFL);
    return hal_.WriteRegister(kGlobalPort, kPhcFreqAdjReg, regVal);
}

// ─────────────────────────────────────────────────────────────────────────────
// BMCA — Best Master Clock Algorithm  (IEEE 802.1AS §10.3.5)
// ─────────────────────────────────────────────────────────────────────────────

void PtpClockManager::RunBmca() noexcept
{
    // Simplified BMCA: compare local clock quality against any received Announce.
    // In production, Announce message parsing feeds into this function from the
    // receive path; here we model the state transitions.

    if (!gmValid_)
    {
        // No GM announced: check if we qualify as GM
        if (cfg_.priority1 == 0U ||
            cfg_.localClockQuality.clockClass < 135U)
        {
            clockRole_ = ClockRole::kGrandmaster;
            // Self-elect: initialise electedGm_ from local config
            electedGm_.priority1 = cfg_.priority1;
            electedGm_.priority2 = cfg_.priority2;
            electedGm_.quality   = cfg_.localClockQuality;
            gmValid_             = true;
            syncState_           = SyncState::kLocked;
        }
    }
    // If GM is valid and we are Slave, syncState_ is managed by InjectSyncMessage.
}

// ─────────────────────────────────────────────────────────────────────────────
// Holdover
// ─────────────────────────────────────────────────────────────────────────────

void PtpClockManager::EnterHoldover() noexcept
{
    syncState_      = SyncState::kHoldover;
    holdoverStartNs_= lastSyncNs_;  // Approximation; real impl reads PHC
    holdoverUsedNs_ = 0ULL;
    // Freeze servo integral — do not accumulate while in holdover
}

Status PtpClockManager::ApplyHoldoverDrift(u64 elapsedNs) noexcept
{
    // TCXO drift compensation:
    //   driftCorrection_ns = cfg_.driftCompensationNs × elapsedNs / 1e9
    //   (scaled integer to avoid FP: multiply first, then divide)
    const u64 driftNs = (cfg_.driftCompensationNs * (elapsedNs / 1'000U)) / 1'000'000U;

    // Apply as a one-shot nanosecond adjustment to the PHC via the update register
    const RegData nsAdj = static_cast<RegData>(driftNs & 0xFFFF'FFFFU);
    return hal_.WriteRegister(kGlobalPort, kPhcNsReg, nsAdj);
}

// ─────────────────────────────────────────────────────────────────────────────
// Queries
// ─────────────────────────────────────────────────────────────────────────────

Status PtpClockManager::GetCurrentTime(PtpTimestamp& outTime) const noexcept
{
    if (!initialised_) { return Status::Err(ErrorCode::kNotInitialised); }

    if (syncState_ == SyncState::kFreeRun)
        { return Status::Err(ErrorCode::kPtpNotSynchronised); }

    RegData secHigh = 0U, secLow = 0U, ns = 0U;
    TSN_RETURN_IF_ERR(hal_.ReadRegister(kGlobalPort, kPhcSecHighReg, secHigh));
    TSN_RETURN_IF_ERR(hal_.ReadRegister(kGlobalPort, kPhcSecLowReg,  secLow));
    TSN_RETURN_IF_ERR(hal_.ReadRegister(kGlobalPort, kPhcNsReg,       ns));

    outTime.seconds     = (static_cast<u64>(secHigh) << 32U) | static_cast<u64>(secLow);
    outTime.nanoseconds = ns;
    return Status::Ok();
}

SyncState  PtpClockManager::GetSyncState()            const noexcept { return syncState_;       }
ClockRole  PtpClockManager::GetClockRole()             const noexcept { return clockRole_;       }
i32        PtpClockManager::GetMeanPathDelayNs()       const noexcept { return meanPathDelayNs_; }
i64        PtpClockManager::GetOffsetFromGrandmasterNs() const noexcept { return offsetFromGmNs_; }
bool       PtpClockManager::IsSynchronised()           const noexcept
{
    const i64 abs = (offsetFromGmNs_ < 0LL) ? -offsetFromGmNs_ : offsetFromGmNs_;
    return (syncState_ == SyncState::kLocked) &&
           (static_cast<u64>(abs) < kPtpSyncTargetNs);
}

u64 PtpClockManager::GetHoldoverRemainingNs() const noexcept
{
    if (syncState_ != SyncState::kHoldover) { return 0ULL; }
    if (holdoverUsedNs_ >= cfg_.holdoverBudgetNs) { return 0ULL; }
    return cfg_.holdoverBudgetNs - holdoverUsedNs_;
}

Status PtpClockManager::GetGrandmasterIdentity(GrandmasterIdentity& outGm) const noexcept
{
    if (!gmValid_) { return Status::Err(ErrorCode::kBmcaNoGrandmaster); }
    outGm = electedGm_;
    return Status::Ok();
}

Status PtpClockManager::ForceGrandmasterRole() noexcept
{
    cfg_.priority1 = 0U;  // Ensures BMCA will always elect this node
    clockRole_     = ClockRole::kGrandmaster;
    gmValid_       = true;
    syncState_     = SyncState::kLocked;
    return Status::Ok();
}

} // namespace tsn
} // namespace norxs
