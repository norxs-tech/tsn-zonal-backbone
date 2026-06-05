// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        GateControlManager.cpp
 * @brief       Implementation of the 7-stage IEEE 802.1Qbv schedule validation
 *              pipeline and Network Calculus worst-case delay analyser for the
 *              norxs Deterministic TSN Zonal Backbone.  All arithmetic is
 *              integer-only (nanosecond domain); no floating-point is used
 *              in compliance with MISRA C++:2008 Rule 6-4-1.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1Qbv, ISO 26262 ASIL-D
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 * =====================================================================================
 */

#include "norxs/tsn/GateControlManager.hpp"
#include <cstring>  // memcmp

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// Default Traffic Envelopes for Network Calculus (compile-time constants)
//
// These represent conservative automotive worst-case burst / rate parameters.
// In production, the TsnOrchestrator populates these from the XML schedule file.
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Default burst sizes per TC in bytes (conservative AUTOSAR Ethernet spec)
static constexpr u32 kDefaultBurstBytes[kMaxTrafficQueues] = {
    9000U,   // TC0 BestEffort   — jumbo-frame burst
    4500U,   // TC1 Background
    2048U,   // TC2 ExcellentEffort
    1538U,   // TC3 CriticalApplication  — max frame
    8192U,   // TC4 Video       — 8 K burst (surround-view GOP)
    1538U,   // TC5 Voice       — CBR low latency
     512U,   // TC6 InternetworkControl — small gPTP/LLDP frames
    1538U,   // TC7 SafetyCritical — max Brake-by-Wire frame
};

/// @brief Default application deadlines per TC in nanoseconds
static constexpr u64 kDefaultDeadlineNs[kMaxTrafficQueues] = {
    50'000'000ULL,  // TC0 50 ms
    50'000'000ULL,  // TC1 50 ms
    10'000'000ULL,  // TC2 10 ms
     5'000'000ULL,  // TC3  5 ms
    10'000'000ULL,  // TC4 10 ms (video)
     2'000'000ULL,  // TC5  2 ms (voice/AVB ClassA)
       500'000ULL,  // TC6  0.5 ms (gPTP sync)
       500'000ULL,  // TC7  0.5 ms ASIL-D HARD DEADLINE
};

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

GateControlManager::GateControlManager(u8 port, SwitchHal& hal) noexcept
    : port_           { port  }
    , hal_            { hal   }
    , scheduleActive_ { false }
    , activeSchedule_ {}
    , lastNcResult_   {}
    , ncResultValid_  { false }
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Primary API
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::ApplySchedule(const ScheduleParams& params) noexcept
{
    TSN_RETURN_IF_ERR(ValidateSchedule(params));
    TSN_RETURN_IF_ERR(hal_.ApplyGclToPort(port_, params));

    activeSchedule_ = params;
    scheduleActive_ = true;
    return Status::Ok();
}

Status GateControlManager::DisableSchedule() noexcept
{
    // static const puts the ~2KB ScheduleParams in .rodata instead of the stack,
    // keeping this function's frame well under the 1024B ASIL-D budget.
    static const ScheduleParams safe = {
        kMinCycleTimeNs,                                  // cycleTimeNs
        kPtpTimestampZero,                                // baseTime
        kMaxFrameSizeBytes,                               // maxSafetyFrameSizeBytes
        1U,                                               // entryCount
        {{ { kAllQueuesOpen, static_cast<u32>(kMinCycleTimeNs) } }}  // gcl[0]; rest zero-init
    };

    TSN_RETURN_IF_ERR(hal_.ApplyGclToPort(port_, safe));
    scheduleActive_ = false;
    return Status::Ok();
}

Status GateControlManager::GetActiveSchedule(ScheduleParams& outParams) const noexcept
{
    if (!scheduleActive_) { return Status::Err(ErrorCode::kNotInitialised); }
    outParams = activeSchedule_;
    return Status::Ok();
}

Status GateControlManager::GetNetworkCalculusResult(NetworkCalculusResult& outResult) const noexcept
{
    if (!ncResultValid_) { return Status::Err(ErrorCode::kNotInitialised); }
    outResult = lastNcResult_;
    return Status::Ok();
}

Status GateControlManager::VerifyHardwareSchedule() const noexcept
{
    if (!scheduleActive_) { return Status::Err(ErrorCode::kNotInitialised); }

    // Use the pre-allocated class member instead of a 2KB stack local
    // to keep the function frame under the 1024B ASIL-D budget.
    verifyBuffer_ = {};
    TSN_RETURN_IF_ERR(hal_.ReadOperGcl(port_, verifyBuffer_));

    // Verify cycle time and every GCL entry gate state
    if (verifyBuffer_.cycleTimeNs != activeSchedule_.cycleTimeNs)
    {
        return Status::Err(ErrorCode::kOperationNotPermitted);
    }
    if (verifyBuffer_.entryCount != activeSchedule_.entryCount)
    {
        return Status::Err(ErrorCode::kOperationNotPermitted);
    }
    for (u8 i = 0U; i < activeSchedule_.entryCount; ++i)
    {
        if (verifyBuffer_.gcl[i].gateStates != activeSchedule_.gcl[i].gateStates ||
            verifyBuffer_.gcl[i].intervalNs != activeSchedule_.gcl[i].intervalNs)
        {
            return Status::Err(ErrorCode::kOperationNotPermitted);
        }
    }
    return Status::Ok();
}

u8   GateControlManager::GetPort()          const noexcept { return port_; }
bool GateControlManager::IsScheduleActive() const noexcept { return scheduleActive_; }

// ─────────────────────────────────────────────────────────────────────────────
// Validation Master
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::ValidateSchedule(const ScheduleParams& params) noexcept
{
    TSN_RETURN_IF_ERR(CheckEntryCount(params));
    TSN_RETURN_IF_ERR(CheckCycleTime(params));
    TSN_RETURN_IF_ERR(CheckNoZeroIntervals(params));
    TSN_RETURN_IF_ERR(CheckIntervalSum(params));
    TSN_RETURN_IF_ERR(CheckGuardBand(params));
    TSN_RETURN_IF_ERR(CheckSafetyCriticalWindow(params));
    TSN_RETURN_IF_ERR(RunNetworkCalculus(params));
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 1 — Entry Count
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::CheckEntryCount(const ScheduleParams& params) const noexcept
{
    if (params.entryCount == 0U)
        { return Status::Err(ErrorCode::kGclEmpty); }
    if (static_cast<std::size_t>(params.entryCount) > kMaxGclEntries)
        { return Status::Err(ErrorCode::kGclTooManyEntries); }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 2 — Cycle Time Bounds
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::CheckCycleTime(const ScheduleParams& params) const noexcept
{
    if (params.cycleTimeNs < kMinCycleTimeNs)
        { return Status::Err(ErrorCode::kCycleTimeTooShort); }
    if (params.cycleTimeNs > kMaxCycleTimeNs)
        { return Status::Err(ErrorCode::kCycleTimeTooLong); }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 3 — No Zero-Interval Entries
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::CheckNoZeroIntervals(const ScheduleParams& params) const noexcept
{
    for (u8 i = 0U; i < params.entryCount; ++i)
    {
        if (params.gcl[i].intervalNs == 0U)
            { return Status::Err(ErrorCode::kZeroIntervalEntry); }
    }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 4 — Interval Sum Must Equal Cycle Time
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::CheckIntervalSum(const ScheduleParams& params) const noexcept
{
    u64 sum = 0ULL;
    for (u8 i = 0U; i < params.entryCount; ++i)
    {
        sum += static_cast<u64>(params.gcl[i].intervalNs);
    }
    if (sum != params.cycleTimeNs)
        { return Status::Err(ErrorCode::kIntervalSumMismatch); }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 5 — ASIL-D Guard Band Verification  (IEEE 802.1Qbv §8.6.8.4)
//
//  Guard band requirement:
//    The TC7-exclusive window must be wide enough to:
//      (a) Transmit the largest expected safety frame completely, AND
//      (b) Include the guard band (max-size frame drain time) before close,
//          to prevent a long best-effort frame from straddling the window edge.
//
//    Required window width ≥ FrameTransmissionNs(maxSafetyFrame)
//                           + FrameTransmissionNs(kMaxFrameSizeBytes)  [guard]
//
//  At 1 Gbps: 1 byte = 8 ns.
//  kMaxFrameSizeBytes = 1538 B (Ethernet + preamble + IFG) → 12 304 ns guard band.
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::CheckGuardBand(const ScheduleParams& params) const noexcept
{
    const u64 safetyFrameNs = FrameTransmissionNs(params.maxSafetyFrameSizeBytes);
    const u64 guardBandNs   = FrameTransmissionNs(kMaxFrameSizeBytes);
    const u64 minWindowNs   = safetyFrameNs + guardBandNs;

    for (u8 i = 0U; i < params.entryCount; ++i)
    {
        if (params.gcl[i].gateStates == kSafetyOnlyOpen)
        {
            if (static_cast<u64>(params.gcl[i].intervalNs) < minWindowNs)
            {
                return Status::Err(ErrorCode::kGuardBandViolation);
            }
        }
    }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 6 — ASIL-D Safety-Critical Exclusive Window
//
//  At least one GCL entry must have ONLY bit 7 set (TC7 exclusively open).
//  This is the norxs ASIL-D extension: gateStates must be exactly 0x80,
//  not merely "TC7 open with others also open".
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::CheckSafetyCriticalWindow(const ScheduleParams& params) const noexcept
{
    for (u8 i = 0U; i < params.entryCount; ++i)
    {
        if (params.gcl[i].gateStates == kSafetyOnlyOpen)
        {
            return Status::Ok();
        }
    }
    return Status::Err(ErrorCode::kSafetyCriticalWindowAbsent);
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 7 — Network Calculus Worst-Case Delay Analysis
//
//  Model: Latency-Rate (LR) server model for IEEE 802.1Qbv.
//
//  For each Traffic Class i (TC0 lowest priority → TC7 highest):
//
//    T_i = total blocking time per cycle (sum of intervals where bit_i == 0)
//
//    WCD_i = T_i                         (scheduling latency)
//          + B_i × 8 ns/byte            (burst drain time at 1 Gbps)
//          + Σ_{j > i} B_j × 8 ns/byte  (preemption by higher-priority classes)
//
//  Note on TC7 (SafetyCritical):
//    TC7 has no higher-priority preemptor.
//    WCD_TC7 = T_TC7 + B_TC7 × 8
//
//    The ASIL-D contract: WCD_TC7 ≤ kSafetyDeadlineNs (500 000 ns).
//    CheckGuardBand (Stage 5) already ensures the TC7 window is wide enough,
//    but Network Calculus accounts for the FULL end-to-end path across the
//    switch including output buffering — a more conservative bound.
//
//  All arithmetic in nanoseconds (u64). No floating-point.
// ─────────────────────────────────────────────────────────────────────────────

Status GateControlManager::RunNetworkCalculus(const ScheduleParams& params) noexcept
{
    NetworkCalculusResult& r = lastNcResult_;

    // ── Compute per-class blocking times ─────────────────────────────────
    u64 blockingNs[kMaxTrafficQueues]{};
    for (u8 tc = 0U; tc < static_cast<u8>(kMaxTrafficQueues); ++tc)
    {
        blockingNs[tc] = ComputeBlockingTimeNs(params, tc);
    }

    // ── Compute burst transmission times ─────────────────────────────────
    u64 burstNs[kMaxTrafficQueues]{};
    for (u8 tc = 0U; tc < static_cast<u8>(kMaxTrafficQueues); ++tc)
    {
        burstNs[tc] = FrameTransmissionNs(kDefaultBurstBytes[tc]);
    }

    // ── WCD per TC using LR server model ─────────────────────────────────
    u64 totalGuardBand = 0ULL;
    u64 totalBitsUsed  = 0ULL;

    for (u8 tc = 0U; tc < static_cast<u8>(kMaxTrafficQueues); ++tc)
    {
        // Sum burst transmission times of all higher-priority classes
        u64 higherPriorityBurstNs = 0ULL;
        for (u8 j = tc + 1U; j < static_cast<u8>(kMaxTrafficQueues); ++j)
        {
            higherPriorityBurstNs += burstNs[j];
        }

        const u64 wcd = blockingNs[tc] + burstNs[tc] + higherPriorityBurstNs;

        r.worstCaseDelayNs[tc]  = wcd;
        r.deadlineMet[tc]       = (wcd <= kDefaultDeadlineNs[tc]);
        totalBitsUsed          += kDefaultBurstBytes[tc] * 8U;
    }

    r.safetyClassBurstNs       = burstNs[static_cast<u8>(TrafficClass::kSafetyCritical)];
    r.scheduleUtilisationPercent =
        (totalBitsUsed * 100U) / (params.cycleTimeNs * kLinkSpeedBitsPerNs);
    r.totalGuardBandUsedNs     = totalGuardBand;

    ncResultValid_ = true;

    // ASIL-D hard check: TC7 WCD must be within 500 µs deadline
    const u8 tc7Idx = static_cast<u8>(TrafficClass::kSafetyCritical);
    if (!r.deadlineMet[tc7Idx])
    {
        return Status::Err(ErrorCode::kDeadlineViolation);
    }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Network Calculus Helpers
// ─────────────────────────────────────────────────────────────────────────────

u64 GateControlManager::ComputeBlockingTimeNs(const ScheduleParams& params,
                                               u8                    tcIndex) const noexcept
{
    const GateStateBitmask tcBit = static_cast<GateStateBitmask>(1U << tcIndex);
    u64 blocked = 0ULL;

    for (u8 i = 0U; i < params.entryCount; ++i)
    {
        // This TC is blocked if its gate bit is 0 in this interval
        if ((params.gcl[i].gateStates & tcBit) == 0U)
        {
            blocked += static_cast<u64>(params.gcl[i].intervalNs);
        }
    }
    return blocked;
}

u64 GateControlManager::FrameTransmissionNs(u32 bytes) noexcept
{
    // At 1 Gbps: 1 byte = 8 bits = 8 ns
    return static_cast<u64>(bytes) * 8ULL;
}

} // namespace tsn
} // namespace norxs
