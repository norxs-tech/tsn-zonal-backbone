// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        TsnOrchestrator.hpp
 * @brief       Central TSN Backbone orchestrator coordinating the complete IEEE TSN
 *              stack lifecycle: gPTP clock convergence, GCL schedule activation,
 *              FRER stream registration, 802.1Qci stream policing, and MACsec
 *              channel setup — with an ASIL-D degradation state machine that
 *              transitions between kRunning, kDegraded, kSafeState, and kFault.
 *              Operates as a deterministic 125 µs scheduling loop on the NXP i.MX8X
 *              POSIX environment with CPU-affinity pinned to a real-time core.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1Qbv, IEEE 802.1AS-Rev,
 *              IEEE 802.1CB, IEEE 802.1Qci, IEEE 802.1AE, ISO 26262 ASIL-D,
 *              ISO/SAE 21434, UN R155
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 *              Contact: contact@norxs.com | https://www.norxs.com
 * =====================================================================================
 */

#ifndef NORXS_TSN_ORCHESTRATOR_HPP
#define NORXS_TSN_ORCHESTRATOR_HPP

#include "TsnTypes.hpp"
#include "SwitchHal.hpp"
#include "GateControlManager.hpp"
#include "PtpClockManager.hpp"
#include "FrerManager.hpp"

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// Orchestrator Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Top-level configuration for the TSN stack.
///        Parsed from a validated XML/JSON network schedule file by the caller.
struct OrchestratorConfig
{
    // PTP
    PtpConfig ptp;

    // Per-port schedules (index = port number)
    u8                  schedulePortCount;
    std::array<ScheduleParams, kMaxSwitchPorts> schedules;

    // FRER streams
    u8                  frerStreamCount;
    std::array<FrerStreamEntry, kMaxFrerEntries> frerStreams;

    // Stream policing (802.1Qci)
    u8                  streamFilterCount;
    std::array<StreamFilterInstance, kMaxStreamFilters> streamFilters;

    // Degraded-mode schedule: applied when kDegraded → kSafeState
    ScheduleParams      safeStateSchedule;  ///< Only TC7 open in all slots

    // Timing loop
    u32                 tickPeriodUs;        ///< Scheduling loop period in µs (125)
    u32                 ptpConvergenceTimeoutMs; ///< Max wait for gPTP lock before fault
};

// ─────────────────────────────────────────────────────────────────────────────
// Audit Log Entry  (ISO 26262 §7.4.2 — circular event trail)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Single entry in the orchestrator's circular audit log.
struct AuditEntry
{
    u64              timestampNs;      ///< Monotonic ns when event occurred
    OrchestratorState previousState;  ///< State before transition
    OrchestratorState newState;       ///< State after transition
    ErrorCode        triggerCode;     ///< ErrorCode that caused the transition
    u8               portOrStream;    ///< Affected port or stream handle
};

static constexpr std::size_t kAuditLogSize = 64U;  ///< Circular log depth

// ─────────────────────────────────────────────────────────────────────────────
// TsnOrchestrator
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Master coordinator for the norxs Deterministic TSN Zonal Backbone.
///
///        Startup sequence (must succeed in order):
///          1. Init()       — validate config, initialise HAL, configure VLAN/TCAM
///          2. Start()      — enter kPtpConverging; wait for gPTP lock
///          3. (internal)   — on gPTP lock: load GCL on all scheduled ports
///          4. (internal)   — register FRER streams and stream filters
///          5. (internal)   — transition to kRunning
///
///        125 µs scheduling loop (Tick()):
///          a. PtpClockManager::Tick()   — servo + BMCA
///          b. FrerManager::Tick()       — path health + latent error detection
///          c. For each port: GateControlManager::VerifyHardwareSchedule()
///          d. Degradation state machine evaluation
///          e. Periodic integrity check (MACsec auth failures, stream drops)
///
///        ASIL-D Degradation State Machine:
///          kRunning
///            │ gPTP holdover expired / HAL fault       → kFault
///            │ FRER path failure / MAC auth failure     → kDegraded
///          kDegraded
///            │ kSafetyDeadlineNs exceeded (NC re-eval) → kSafeState
///            │ recovery: all paths restored             → kRunning
///          kSafeState
///            │ Only TC7 GCL active; all other queues
///            │ closed; only Brake-by-Wire traffic flows
///            │ manual reset required                   → kFault
///          kFault
///            │ Requires hardware reset + re-Init()
///
///        Memory model: All state in members. Zero heap. Zero exceptions.
///        Thread-safety: Tick() is called from a single RT thread.
///                       GetState() / GetAuditLog() may be called from other threads
///                       (caller provides external lock if needed).
class TsnOrchestrator final
{
public:
    /// @brief Construct orchestrator with concrete HAL and all sub-managers.
    ///
    /// @param hal  Concrete SwitchHal implementation (Sja1110Hal, Q5050Hal, etc.)
    explicit TsnOrchestrator(SwitchHal& hal) noexcept;

    ~TsnOrchestrator() = default;
    TsnOrchestrator(const TsnOrchestrator&)            = delete;
    TsnOrchestrator& operator=(const TsnOrchestrator&) = delete;
    TsnOrchestrator(TsnOrchestrator&&)                 = delete;
    TsnOrchestrator& operator=(TsnOrchestrator&&)      = delete;

    // ─────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Validate configuration and initialise all hardware subsystems.
    ///
    ///        Performs:
    ///          - HAL Init() (chip reset, register verification)
    ///          - VLAN table configuration for all ports
    ///          - 802.1Qci stream filter installation
    ///          - MACsec channel configuration
    ///          - gPTP stack initialisation
    ///          - Network Calculus pre-validation on all port schedules
    ///            (fails fast before any hardware is touched if any schedule
    ///             violates ASIL-D timing requirements)
    ///
    ///        On any error, the orchestrator remains in kUninitialized.
    ///        No partial state is left in hardware.
    ///
    /// @param cfg  Complete TSN stack configuration.
    /// @return Status::Ok()               Ready to call Start().
    /// @return kInvalidArgument           Configuration semantically invalid.
    /// @return kDeadlineViolation         Pre-validation: NC analysis failed.
    /// @return kHal*                      Hardware initialisation failed.
    Status Init(const OrchestratorConfig& cfg) noexcept;

    /// @brief Begin the TSN stack — enter kPtpConverging and wait for gPTP lock.
    ///
    ///        Non-blocking. The caller must call Tick() at kTickPeriodUs intervals.
    ///        The orchestrator transitions to kRunning autonomously once all
    ///        convergence criteria are met.
    ///
    /// @return kNotInitialised if Init() has not been called successfully.
    Status Start() noexcept;

    /// @brief Gracefully shut down — disable all port schedules, deregister streams.
    Status Stop() noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Scheduling Loop
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Execute one orchestration tick — must be called every tickPeriodUs µs.
    ///
    ///        The caller is responsible for real-time scheduling (e.g. SCHED_FIFO
    ///        on Linux, or a bare-metal timer ISR on QNX/FreeRTOS).
    ///        This method never blocks.  It reads monotonic time internally.
    ///
    /// @return Status::Ok()      tick processed, state unchanged or improved.
    /// @return kPtpHoldoverExpired  holdover expired → transitioned to kFault.
    /// @return kFrerLatentError     latent error detected → kDegraded.
    /// @return kOperationNotPermitted orchestrator is in kFault.
    Status Tick() noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Dynamic Schedule Management
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Update the GCL schedule on a specific port at runtime.
    ///
    ///        Validates the new schedule (full 7-stage pipeline) and applies it
    ///        via Admin→Oper swap at the next cycle boundary.  The current schedule
    ///        continues running until the swap is confirmed.
    ///
    ///        Only permitted in kRunning state.
    ///
    /// @param port    Target port.
    /// @param params  New schedule parameters.
    Status UpdatePortSchedule(u8 port, const ScheduleParams& params) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // State & Diagnostics
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Return the current orchestrator state.
    OrchestratorState GetState() const noexcept;

    /// @brief Return true if the orchestrator is in kRunning (fully operational).
    bool IsFullyOperational() const noexcept;

    /// @brief Copy the audit log into @p outLog (most recent kAuditLogSize entries).
    ///
    /// @param outLog   Buffer to receive the log.
    /// @param outCount Populated with the number of valid entries.
    Status GetAuditLog(std::array<AuditEntry, kAuditLogSize>& outLog,
                       u8& outCount) const noexcept;

    /// @brief Return the PtpClockManager for external clock queries.
    const PtpClockManager& GetPtpClockManager() const noexcept;

    /// @brief Return the FrerManager for external stream diagnostics.
    const FrerManager& GetFrerManager() const noexcept;

    /// @brief Return the GateControlManager for a specific port.
    ///
    /// @param port    Target port.
    /// @param outGcm  Set to pointer to the manager on success.
    /// @return kHalPortOutOfRange if port is not managed.
    Status GetGateControlManager(u8 port, const GateControlManager*& outGcm) const noexcept;

    /// @brief Read consolidated system health metrics.
    ///
    /// @param outPtpOffsetNs    Current gPTP offset from GM.
    /// @param outFaultMask      Bitmask of active fault conditions.
    /// @param outDropTotal      Total frames dropped across all stream filters.
    /// @param outMacsecFails    Total MACsec authentication failures.
    Status GetHealthMetrics(i64& outPtpOffsetNs,
                            u32& outFaultMask,
                            u64& outDropTotal,
                            u64& outMacsecFails) const noexcept;

private:
    // ─────────────────────────────────────────────────────────────────────
    // Internal Startup Steps
    // ─────────────────────────────────────────────────────────────────────

    Status InitHal() noexcept;
    Status InitVlanTable() noexcept;
    Status InitStreamFilters() noexcept;
    Status InitMacsec() noexcept;
    Status InitFrerStreams() noexcept;
    Status PrevalidateAllSchedules() noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Tick Sub-routines
    // ─────────────────────────────────────────────────────────────────────

    Status TickPtp() noexcept;
    Status TickFrer(u64 nowNs) noexcept;
    Status TickIntegrityCheck() noexcept;
    Status TickDegradationMachine() noexcept;

    /// @brief Transition to kSafeState: apply safe-state GCL on all ports.
    ///        Safe-state schedule = TC7-exclusive window only.
    Status EnterSafeState(ErrorCode trigger) noexcept;

    /// @brief Transition to kFault: disable all schedules.
    Status EnterFault(ErrorCode trigger) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Audit Log
    // ─────────────────────────────────────────────────────────────────────

    void LogStateTransition(OrchestratorState from,
                            OrchestratorState to,
                            ErrorCode         trigger,
                            u8                portOrStream,
                            u64               nowNs) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Members (all static storage — zero heap)
    // ─────────────────────────────────────────────────────────────────────

    SwitchHal&          hal_;
    OrchestratorConfig  cfg_;
    OrchestratorState   state_;
    bool                initialised_;

    // Sub-managers (value members — no dynamic allocation)
    PtpClockManager     ptpManager_;
    FrerManager         frerManager_;
    std::array<GateControlManager*, kMaxSwitchPorts> gcManagers_;
    // GCM objects stored in separate static array to avoid default-constructing
    // all ports when only a subset are used.
    // Concrete instances live here; gcManagers_[i] points into this array.
    // This is a common AUTOSAR pattern for fixed-size polymorphic storage.
    // (GateControlManager is non-copyable; raw storage + placement is
    //  acceptable under AUTOSAR M18-4-1 when bounded at compile time.)
    alignas(GateControlManager)
    u8 gcmStorage_[kMaxSwitchPorts][sizeof(GateControlManager)];
    u8 gcmPortMap_[kMaxSwitchPorts]; ///< Maps storage slot → port index
    u8 gcmCount_;

    // Timing
    u64 ptpConvergenceStartNs_;
    u64 lastHealthCheckNs_;

    /// PHC-independent monotonic time, derived from the Tick() contract
    /// (caller invokes Tick() every cfg_.tickPeriodUs µs). Drives the gPTP
    /// convergence watchdog so it can fire even while the PHC is unreadable
    /// (PtpClockManager in kFreeRun before first sync). Resolves OBS-001.
    u64 monotonicNs_;

    // Fault tracking
    u32 activeFaultMask_;    ///< Bitmask of concurrent active faults

    // Audit log (circular buffer)
    std::array<AuditEntry, kAuditLogSize> auditLog_;
    u8  auditHead_;   ///< Next write index
    u8  auditCount_;  ///< Valid entries (saturates at kAuditLogSize)
};

} // namespace tsn
} // namespace norxs

#endif // NORXS_TSN_ORCHESTRATOR_HPP
