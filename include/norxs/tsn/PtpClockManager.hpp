// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        PtpClockManager.hpp
 * @brief       IEEE 802.1AS-Rev Generalized Precision Time Protocol (gPTP) clock
 *              manager providing hardware-timestamped synchronisation, Best Master
 *              Clock Algorithm (BMCA) grandmaster election, and TCXO-compensated
 *              holdover mode for ASIL-D continuity when the grandmaster is lost.
 *              Achieves < 1 µs synchronisation accuracy via MAC-layer hardware
 *              timestamps on the NXP i.MX8X enet_qos peripheral.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1AS-Rev, ISO 26262 ASIL-D,
 *              IEEE 802.1Qbv (clock dependency for baseTime scheduling)
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 *              Contact: contact@norxs.com | https://www.norxs.com
 * =====================================================================================
 */

#ifndef NORXS_TSN_PTP_CLOCK_MANAGER_HPP
#define NORXS_TSN_PTP_CLOCK_MANAGER_HPP

#include "TsnTypes.hpp"
#include "SwitchHal.hpp"

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// gPTP Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief IEEE 802.1AS-Rev clock quality descriptor (ClockQuality TLV).
struct ClockQuality
{
    u8  clockClass;          ///< 6 = primary reference, 135 = application-specific
    u8  clockAccuracy;       ///< 0x21 = within 100 ns, 0x22 = within 250 ns, etc.
    u16 offsetScaledLogVariance; ///< Precision metric (lower = better)
};

/// @brief gPTP port dataset — one entry per 802.1AS-capable physical port.
struct PtpPortDataset
{
    u8           portIndex;          ///< Physical port [0, kMaxSwitchPorts)
    bool         enabled;            ///< True = participating in gPTP
    i32          peerDelayNs;        ///< Peer mean path delay in ns (signed)
    u8           logSyncInterval;    ///< Sync message rate: -3 = 8/s, 0 = 1/s
    u8           logAnnounceInterval;///< Announce message rate
    bool         asCapable;          ///< IEEE 802.1AS §10.3.2 AS_capable flag
};

/// @brief Grandmaster identity (IEEE 802.1AS §8.6.2.1).
struct GrandmasterIdentity
{
    u8           clockId[8];         ///< 64-bit EUI-64 Clock Identity
    ClockQuality quality;
    u8           priority1;          ///< BMCA tiebreak priority 1 [0..255]
    u8           priority2;          ///< BMCA tiebreak priority 2 [0..255]
    u16          stepsRemoved;       ///< Topology hops from GM to this clock
};

/// @brief Configuration passed to PtpClockManager::Init().
struct PtpConfig
{
    ClockQuality   localClockQuality;  ///< This node's clock quality
    u8             priority1;          ///< BMCA priority1 for this node
    u8             priority2;          ///< BMCA priority2 for this node
    u8             domainNumber;        ///< gPTP domain (typically 0)
    u8             portCount;           ///< Number of ports to manage
    u64            syncTimeoutNs;       ///< Max interval without Sync before holdover
    u64            holdoverBudgetNs;    ///< Max holdover duration before kFreeRun
    u64            driftCompensationNs; ///< Oscillator drift rate ns/s (TCXO spec)
    std::array<PtpPortDataset, kMaxSwitchPorts> ports;
};

// ─────────────────────────────────────────────────────────────────────────────
// PtpClockManager
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Manages IEEE 802.1AS-Rev gPTP clock synchronisation for the TSN backbone.
///
///        Responsibilities:
///          1. Hardware-timestamped Sync/Follow_Up message processing
///             (MAC-layer ingress/egress timestamps from i.MX8X enet_qos)
///          2. Best Master Clock Algorithm (BMCA) grandmaster election and
///             failover with configurable convergence timeout
///          3. Local oscillator (PHC — PTP Hardware Clock) rate adjustment
///             using proportional-integral servo (no FP; integer scaled math)
///          4. Holdover mode with TCXO drift compensation when GM is lost,
///             ensuring TAS baseTime remains valid for ASIL-D continuity
///          5. Providing the current PTP time to GateControlManager for
///             schedule baseTime validation (CheckBaseTimeInFuture)
///
///        State machine:
///          kFreeRun → kLocking → kLocked ⇄ kHoldover
///
///        Clock servo (integer PI, no std::float):
///          offset_ns = (t2 + correction) - t1_adjusted
///          integral  += offset_ns × Ki
///          adjustment = offset_ns × Kp + integral
///          phc_rate  -= adjustment (written to PHC frequency register)
class PtpClockManager final
{
public:
    explicit PtpClockManager(SwitchHal& hal) noexcept;
    ~PtpClockManager() = default;
    PtpClockManager(const PtpClockManager&)            = delete;
    PtpClockManager& operator=(const PtpClockManager&) = delete;
    PtpClockManager(PtpClockManager&&)                 = delete;
    PtpClockManager& operator=(PtpClockManager&&)      = delete;

    // ─────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Initialise the gPTP stack with @p cfg and start BMCA on all ports.
    ///
    /// @return Status::Ok()            initialisation successful.
    /// @return kInvalidArgument        cfg.portCount == 0 or > kMaxSwitchPorts.
    /// @return kHalWriteFailed         PHC frequency register write failed.
    Status Init(const PtpConfig& cfg) noexcept;

    /// @brief Process one gPTP synchronisation tick.
    ///
    ///        Must be called periodically (typically every 125 µs by the
    ///        TsnOrchestrator scheduling loop).  Reads hardware timestamps,
    ///        runs the PI servo, and advances the BMCA state machine.
    ///
    /// @return Status::Ok()             tick processed, clock state unchanged.
    /// @return kPtpHoldoverExpired      holdover budget exhausted → kFreeRun.
    /// @return kBmcaNoGrandmaster       no GM after convergence timeout.
    Status Tick() noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Clock State Queries
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Read the current PTP network time from the PHC.
    ///
    /// @param outTime  Populated with the current PTP timestamp.
    /// @return kPtpNotSynchronised if syncState_ == kFreeRun.
    Status GetCurrentTime(PtpTimestamp& outTime) const noexcept;

    /// @brief Return the current synchronisation state.
    SyncState GetSyncState() const noexcept;

    /// @brief Return the current clock role (Grandmaster / Boundary / Slave).
    ClockRole GetClockRole() const noexcept;

    /// @brief Return the identity of the currently elected grandmaster.
    ///
    /// @param outGm  Populated with GM identity.
    /// @return kBmcaNoGrandmaster if no GM is elected.
    Status GetGrandmasterIdentity(GrandmasterIdentity& outGm) const noexcept;

    /// @brief Return the current mean path delay to the GM in nanoseconds.
    ///        Negative value indicates measured delay is below the servo threshold.
    i32 GetMeanPathDelayNs() const noexcept;

    /// @brief Return the current clock offset from GM in nanoseconds.
    ///        Absolute value < kPtpSyncTargetNs (1000 ns) means kLocked.
    i64 GetOffsetFromGrandmasterNs() const noexcept;

    /// @brief True if the clock is within kPtpSyncTargetNs of the grandmaster.
    bool IsSynchronised() const noexcept;

    /// @brief Return remaining holdover budget in nanoseconds.
    ///        Returns 0 if not in holdover.
    u64 GetHoldoverRemainingNs() const noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // BMCA control
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Force this node to act as Grandmaster (override BMCA).
    ///        Used in bench configurations with no external GM.
    ///        Sets priority1 = 0 so BMCA will always elect this node.
    Status ForceGrandmasterRole() noexcept;

    /// @brief Inject a simulated Sync message (for HIL/unit testing).
    ///
    /// @param rxTimestampNs   Hardware ingress timestamp.
    /// @param txTimestampNs   GM transmit timestamp from Follow_Up TLV.
    /// @param correctionNs    Accumulated correction field.
    Status InjectSyncMessage(u64 rxTimestampNs,
                              u64 txTimestampNs,
                              i64 correctionNs) noexcept;

private:
    // ─────────────────────────────────────────────────────────────────────
    // PI Servo (integer scaled math, no floating-point)
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Run one PI servo iteration given the measured offset.
    ///
    ///        Uses scaled integer arithmetic to avoid FP (MISRA C++:2008 Rule 6-4-1):
    ///          Kp = 1/4  (implemented as >> 2)
    ///          Ki = 1/32 (implemented as >> 5)
    ///          adjustment_ppb = (offset_ns >> 2) + (integral_ >> 5)
    ///
    ///        Clamps the adjustment to ±500 000 ppb to protect the PHC register.
    ///
    /// @param offsetNs  Current clock offset relative to grandmaster.
    /// @return PHC frequency adjustment in parts-per-billion (ppb).
    i64 RunServo(i64 offsetNs) noexcept;

    /// @brief Write frequency adjustment to the PHC hardware register via HAL.
    Status ApplyPhcAdjustment(i64 adjustmentPpb) noexcept;

    /// @brief Run BMCA dataset comparison (IEEE 802.1AS §10.3.5).
    ///        Updates clockRole_ and electedGm_.
    void RunBmca() noexcept;

    /// @brief Enter holdover: freeze servo, start drift compensation countdown.
    void EnterHoldover() noexcept;

    /// @brief Apply TCXO drift compensation during holdover.
    ///        Adds (driftCompensationNs × elapsedMs / 1000) ns correction
    ///        to the local PHC to maintain schedule baseTime validity.
    Status ApplyHoldoverDrift(u64 elapsedNs) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Members (all static storage)
    // ─────────────────────────────────────────────────────────────────────

    SwitchHal&         hal_;
    PtpConfig          cfg_;
    bool               initialised_;

    SyncState          syncState_;
    ClockRole          clockRole_;
    GrandmasterIdentity electedGm_;
    bool               gmValid_;

    i64                offsetFromGmNs_;   ///< Last measured GM offset
    i32                meanPathDelayNs_;  ///< Running average peer delay
    i64                integral_;         ///< PI servo integral accumulator
    u64                lastSyncNs_;       ///< Monotonic ns of last Sync receipt
    u64                holdoverStartNs_;  ///< Monotonic ns when holdover began
    u64                holdoverUsedNs_;   ///< Accumulated holdover duration

    std::array<PtpPortDataset, kMaxSwitchPorts> ports_;
};

} // namespace tsn
} // namespace norxs

#endif // NORXS_TSN_PTP_CLOCK_MANAGER_HPP
