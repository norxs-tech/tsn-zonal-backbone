// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        FrerManager.hpp
 * @brief       IEEE 802.1CB Frame Replication and Elimination for Reliability (FRER)
 *              manager implementing the sequence generation, individual recovery,
 *              and latent error detection state machines for zero-recovery-time
 *              redundancy across disjoint 1000BASE-T1 network paths.
 *              Provides ASIL-D path monitoring and automatic path-failure detection
 *              within one recovery timeout window (configurable, typically 1 ms).
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1CB, ISO 26262 ASIL-D,
 *              UN R155 (cybersecurity: duplicate stream injection prevention)
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 *              Contact: contact@norxs.com | https://www.norxs.com
 * =====================================================================================
 */

#ifndef NORXS_TSN_FRER_MANAGER_HPP
#define NORXS_TSN_FRER_MANAGER_HPP

#include "TsnTypes.hpp"
#include "SwitchHal.hpp"

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// FRER State Machine States  (IEEE 802.1CB §7.4.3)
// ─────────────────────────────────────────────────────────────────────────────

enum class FrerRecoveryState : u8
{
    kPassRecovering = 0U,  ///< Normal: receiving frames, discarding duplicates
    kDiscardRecovering = 1U, ///< Transitional: waiting for sequence reset
    kReset          = 2U,  ///< Recovery function reset in progress
};

// ─────────────────────────────────────────────────────────────────────────────
// FRER Stream Runtime State (one per registered stream)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Runtime monitoring state for one FRER stream.
struct FrerStreamState
{
    u16              streamHandle;
    FrerRecoveryState recoveryState;

    // Sequence tracking
    u16              lastSeqNum;           ///< Last accepted sequence number
    u16              expectedSeqNum;       ///< Next expected sequence number
    u32              seqNumHistoryMask;    ///< 32-entry bitmask window

    // Path health (one bit per member path, up to 8 paths)
    u8               pathHealthMask;       ///< 1 = healthy, 0 = path silent
    u8               lastPathReceived;     ///< Index of path that delivered last frame

    // Counters (monotonic; readable via GetFrerStats())
    u64              framesAccepted;       ///< First-copy frames forwarded
    u64              framesDiscardedDuplicate; ///< Dropped duplicates (healthy sign)
    u64              framesOutOfOrder;     ///< Out-of-window sequence numbers
    u64              latentErrorEvents;    ///< IEEE 802.1CB §7.4.5 LED triggers

    // Timestamps (nanoseconds monotonic)
    u64              lastFrameNs;          ///< Time of last accepted frame
    u64              pathSilenceDeadlineNs;///< Path declared failed after this
};

// ─────────────────────────────────────────────────────────────────────────────
// FrerManager
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Manages all IEEE 802.1CB FRER streams for the TSN backbone.
///
///        Design:
///          - Up to kMaxFrerEntries (64) streams tracked in static arrays.
///          - Per-stream state machine runs in the Tick() method called by
///            TsnOrchestrator every 125 µs.
///          - Latent Error Detection (LED): compares duplicate-drop rate against
///            expected rate.  If too few duplicates arrive, a path may be
///            silently broken → LED event → kDegraded orchestrator state.
///          - Path silence detection: if any path carries no frames within
///            recoveryTimeoutNs, it is declared failed and reported.
///          - All decisions are implemented in software state machine;
///            the SwitchHal handles the hardware duplicate-elimination SRAM.
///
///        FRER sequence space (IEEE 802.1CB §10.4):
///          Uses a 32-bit history bitmask for O(1) duplicate detection:
///            bit = (seqNum - lastSeqNum) & 0x1F
///            if bit already set → duplicate; else set bit, accept frame
class FrerManager final
{
public:
    explicit FrerManager(SwitchHal& hal) noexcept;
    ~FrerManager() = default;
    FrerManager(const FrerManager&)            = delete;
    FrerManager& operator=(const FrerManager&) = delete;
    FrerManager(FrerManager&&)                 = delete;
    FrerManager& operator=(FrerManager&&)      = delete;

    // ─────────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Initialise FRER subsystem. Clears all stream state.
    Status Init() noexcept;

    /// @brief Periodic tick — advance all stream state machines.
    ///        Must be called every 125 µs by TsnOrchestrator.
    ///        Detects path silence, fires latent error events.
    ///
    /// @param nowNs  Current monotonic time in nanoseconds.
    /// @return kFrerLatentError if any stream's LED fires this tick.
    Status Tick(u64 nowNs) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Stream Registration
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Register a new FRER stream and configure hardware replication/recovery.
    ///
    /// @param entry  Stream descriptor.
    /// @return kFrerTableFull if kMaxFrerEntries already registered.
    Status RegisterStream(const FrerStreamEntry& entry) noexcept;

    /// @brief Deregister a stream and release its hardware slot.
    Status DeregisterStream(u16 streamHandle) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Frame Notification (called by receive path)
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Notify the state machine that a frame arrived for @p streamHandle.
    ///
    ///        Implements the IEEE 802.1CB Individual Recovery Algorithm:
    ///          1. Extract sequence number from R-TAG.
    ///          2. Check history bitmask for duplicate.
    ///          3. If duplicate: increment framesDiscardedDuplicate, return.
    ///          4. If out-of-window: check if reset needed, update lastSeqNum.
    ///          5. Update pathHealthMask for the source path.
    ///
    /// @param streamHandle  Target stream.
    /// @param sequenceNum   R-TAG sequence number from the received frame.
    /// @param pathIndex     Source member path index [0, memberPaths).
    /// @param nowNs         Current monotonic time in nanoseconds.
    ///
    /// @return Result::Ok(true)  Frame should be forwarded (first copy).
    /// @return Result::Ok(false) Frame is a duplicate — discard.
    /// @return kFrerStreamUnknown  streamHandle not registered.
    Result<bool, ErrorCode> OnFrameReceived(u16 streamHandle,
                                             u16 sequenceNum,
                                             u8  pathIndex,
                                             u64 nowNs) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Statistics & Diagnostics
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Read cumulative statistics for a stream.
    Status GetStreamStats(u16  streamHandle,
                          u64& outAccepted,
                          u64& outDuplicates,
                          u64& outOutOfOrder,
                          u64& outLatentErrors) const noexcept;

    /// @brief Read the path health bitmask for a stream.
    ///        Bit N set = path N healthy. Clear bit = path declared silent.
    Status GetPathHealth(u16 streamHandle, u8& outHealthMask) const noexcept;

    /// @brief Return the number of currently registered streams.
    u8 GetStreamCount() const noexcept;

private:
    // ─────────────────────────────────────────────────────────────────────
    // Internal helpers
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Find the internal state slot for @p streamHandle.
    ///        Returns kMaxFrerEntries if not found.
    u8 FindStreamSlot(u16 streamHandle) const noexcept;

    /// @brief Implement the 32-bit history-bitmask duplicate check.
    ///        Returns true if @p seqNum is a duplicate of a recently seen frame.
    static bool IsDuplicate(FrerStreamState& state, u16 seqNum) noexcept;

    /// @brief Update latent error detection for @p state.
    ///        IEEE 802.1CB §7.4.5: if duplicates < threshold over LED window,
    ///        a path is silently broken → fire latent error event.
    void CheckLatentError(FrerStreamState& state, u64 nowNs) noexcept;

    // ─────────────────────────────────────────────────────────────────────
    // Static storage
    // ─────────────────────────────────────────────────────────────────────

    SwitchHal& hal_;
    bool       initialised_;
    u8         streamCount_;
    std::array<FrerStreamState,   kMaxFrerEntries> states_;
    std::array<FrerStreamEntry,   kMaxFrerEntries> entries_;
};

} // namespace tsn
} // namespace norxs

#endif // NORXS_TSN_FRER_MANAGER_HPP
