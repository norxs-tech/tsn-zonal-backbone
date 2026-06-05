// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        GateControlManager.hpp
 * @brief       IEEE 802.1Qbv Gate Control List manager implementing a five-stage
 *              mathematical validation pipeline — including ASIL-D guard band
 *              verification and Network Calculus worst-case delay analysis —
 *              followed by atomic Admin→Oper hardware serialisation via SwitchHal.
 *              All schedule decisions are justified by Network Calculus theory
 *              (Le Boudec & Thiran, 2001) using integer-only arithmetic.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1Qbv, ISO 26262 ASIL-D
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 *              Contact: contact@norxs.com | https://www.norxs.com
 * =====================================================================================
 */

#ifndef NORXS_TSN_GATE_CONTROL_MANAGER_HPP
#define NORXS_TSN_GATE_CONTROL_MANAGER_HPP

#include "TsnTypes.hpp"
#include "SwitchHal.hpp"

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// Network Calculus Analysis Result
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Per-class worst-case delay (WCD) computed by the Network Calculus engine.
///        Stored for post-validation audit; all values in nanoseconds.
struct NetworkCalculusResult
{
    u64 worstCaseDelayNs[kMaxTrafficQueues]; ///< WCD per traffic class [TC0..TC7]
    u64 safetyClassBurstNs;                  ///< TC7 maximum burst duration
    u64 totalGuardBandUsedNs;                ///< Sum of all guard bands in one cycle
    u64 scheduleUtilisationPercent;          ///< Link utilisation × 100 (0–100)
    bool deadlineMet[kMaxTrafficQueues];     ///< true iff WCD ≤ deadline for each TC
};

// ─────────────────────────────────────────────────────────────────────────────
// GateControlManager
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Manages the complete lifecycle of a IEEE 802.1Qbv Time-Aware schedule
///        on a single physical switch egress port.
///
///        Validation Pipeline (executed in order before any HAL access):
///          Stage 1 — EntryCount:    bounds check on GCL array
///          Stage 2 — CycleTime:     [kMinCycleTimeNs, kMaxCycleTimeNs]
///          Stage 3 — ZeroIntervals: no entry with intervalNs == 0
///          Stage 4 — IntervalSum:   Σ(intervalNs) == cycleTimeNs (u64, no FP)
///          Stage 5 — GuardBand:     TC7 window ≥ maxFrameNs + kMinGuardBandNs
///          Stage 6 — SafetyWindow:  at least one entry with gateStates == 0x80
///          Stage 7 — NetworkCalculus: WCD[TC7] ≤ kSafetyDeadlineNs (500 µs)
///
///        Memory model: Zero heap allocation. All schedule state in members.
///        Thread-safety: NOT thread-safe. TsnOrchestrator serialises access.
class GateControlManager final
{
public:
    // ─────────────────────────────────────────────────────────────────────
    // Construction
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Construct manager bound to a port and HAL.
    ///
    /// @param port   Physical switch port [0, kMaxSwitchPorts).
    /// @param hal    Live SwitchHal reference — must outlive this object.
    explicit GateControlManager(u8 port, SwitchHal& hal) noexcept;

    ~GateControlManager() = default;
    GateControlManager(const GateControlManager&)            = delete;
    GateControlManager& operator=(const GateControlManager&) = delete;
    GateControlManager(GateControlManager&&)                 = delete;
    GateControlManager& operator=(GateControlManager&&)      = delete;

    // ─────────────────────────────────────────────────────────────────────
    // Primary API
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Validate and apply a new Time-Aware schedule atomically.
    ///
    ///        Runs all 7 validation stages, then calls hal_.ApplyGclToPort().
    ///        On success, updates internal state.  On any validation failure,
    ///        the currently active schedule is NOT disturbed.
    ///
    /// @param params  Candidate schedule with cycle time, base time, GCL, and
    ///                maxSafetyFrameSizeBytes for guard band computation.
    ///
    /// @return Status::Ok()                          Schedule active in hardware.
    /// @return kGclEmpty / kGclTooManyEntries        Stage 1 failure.
    /// @return kCycleTimeTooShort/Long               Stage 2 failure.
    /// @return kZeroIntervalEntry                    Stage 3 failure.
    /// @return kIntervalSumMismatch                  Stage 4 failure.
    /// @return kGuardBandViolation                   Stage 5 failure.
    /// @return kSafetyCriticalWindowAbsent           Stage 6 failure.
    /// @return kDeadlineViolation                    Stage 7 failure.
    /// @return kHal*                                 HAL serialisation failure.
    Status ApplySchedule(const ScheduleParams& params) noexcept;

    /// @brief Revert to all-queues-open pass-through (TAS disabled).
    ///        Safe to call at any time, including from fault handlers.
    Status DisableSchedule() noexcept;

    /// @brief Retrieve the currently active schedule snapshot.
    ///
    /// @param outParams  Populated on success.
    /// @return kNotInitialised if no schedule is active.
    Status GetActiveSchedule(ScheduleParams& outParams) const noexcept;

    /// @brief Retrieve the Network Calculus result from the last ApplySchedule call.
    ///
    /// @param outResult  Populated on success.
    /// @return kNotInitialised if no schedule has been applied yet.
    Status GetNetworkCalculusResult(NetworkCalculusResult& outResult) const noexcept;

    /// @brief Verify that the hardware Oper GCL matches the last applied schedule.
    ///        Reads back via hal_.ReadOperGcl() and compares cycle time and
    ///        all entry gate states.  Used for periodic integrity monitoring.
    ///
    /// @return Status::Ok()          Hardware matches expected schedule.
    /// @return kHalReadFailed        Readback failed.
    /// @return kOperationNotPermitted Mismatch detected — integrity violation.
    Status VerifyHardwareSchedule() const noexcept;

    u8   GetPort()             const noexcept;
    bool IsScheduleActive()    const noexcept;

private:
    // ─────────────────────────────────────────────────────────────────────
    // Validation Stages (private)
    // ─────────────────────────────────────────────────────────────────────

    Status ValidateSchedule(const ScheduleParams& params) noexcept;

    /// Stage 1
    Status CheckEntryCount(const ScheduleParams& params) const noexcept;
    /// Stage 2
    Status CheckCycleTime(const ScheduleParams& params) const noexcept;
    /// Stage 3
    Status CheckNoZeroIntervals(const ScheduleParams& params) const noexcept;
    /// Stage 4 — u64 accumulation; no floating-point (MISRA C++:2008 Rule 6-4-1)
    Status CheckIntervalSum(const ScheduleParams& params) const noexcept;
    /// Stage 5 — Guard band: TC7 window ≥ ceil(maxFrameBytes × 8 / 1Gbps) + margin
    Status CheckGuardBand(const ScheduleParams& params) const noexcept;
    /// Stage 6 — Exactly gateStates == kSafetyOnlyOpen (0x80) in ≥ 1 entry
    Status CheckSafetyCriticalWindow(const ScheduleParams& params) const noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Network Calculus Engine (private)
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Stage 7 — Compute per-class worst-case delays using Network Calculus.
    ///
    ///        Model: Latency-Rate (LR) server per IEEE 802.1Qbv TAS.
    ///
    ///        For each Traffic Class i, the worst-case delay is bounded by:
    ///
    ///          WCD_i = T_i + (B_i + Σ_{j>i} B_j) / C
    ///
    ///        where:
    ///          T_i  = total closed-time for TC_i in one cycle (blocking latency)
    ///          B_i  = burst size of TC_i (bytes) × 8 / link_rate (ns)
    ///          B_j  = burst of higher-priority class j
    ///          C    = 1 Gbps link capacity (1 bit/ns)
    ///
    ///        All arithmetic is integer-only (nanoseconds).  Result populated
    ///        into lastNcResult_ for audit trail.
    ///
    /// @param params  Validated schedule (Stages 1–6 must have passed).
    /// @return kDeadlineViolation if WCD[TC7] > kSafetyDeadlineNs.
    Status RunNetworkCalculus(const ScheduleParams& params) noexcept;

    /// @brief Compute the total nanoseconds per cycle during which TC_i is blocked.
    ///        A TC is blocked during any GCL interval where its gate bit is 0.
    u64 ComputeBlockingTimeNs(const ScheduleParams& params, u8 tcIndex) const noexcept;

    /// @brief Compute transmission time for a frame of @p bytes at 1 Gbps.
    ///        frameNs = bytes × 8 bits/byte × 1 ns/bit = bytes × 8.
    static u64 FrameTransmissionNs(u32 bytes) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Member State (all static storage)
    // ─────────────────────────────────────────────────────────────────────

    u8                   port_;
    SwitchHal&           hal_;
    bool                 scheduleActive_;
    ScheduleParams       activeSchedule_;
    mutable ScheduleParams verifyBuffer_;   ///< Scratch for VerifyHardwareSchedule (keeps stack < 1024B)
    NetworkCalculusResult lastNcResult_;
    bool                 ncResultValid_;
};

} // namespace tsn
} // namespace norxs

#endif // NORXS_TSN_GATE_CONTROL_MANAGER_HPP
