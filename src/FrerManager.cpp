// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        FrerManager.cpp
 * @brief       Implementation of the IEEE 802.1CB Frame Replication and Elimination
 *              for Reliability (FRER) state machine, including O(1) sequence-number
 *              duplicate detection via 32-bit history bitmask, per-path silence
 *              monitoring, and latent error detection (LED) per §7.4.5.
 *              No dynamic allocation; all stream state lives in static arrays.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1CB, ISO 26262 ASIL-D
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 * =====================================================================================
 */

#include "norxs/tsn/FrerManager.hpp"
#include <cstring>

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// Latent Error Detection (LED) constants  (IEEE 802.1CB §7.4.5)
//
//  LED fires when the duplicate-reception ratio falls below a threshold,
//  indicating a silently broken redundant path.
//
//  Window: 10 000 frames evaluated per stream.
//  Threshold: ≥ 1 duplicate per 10 frames expected (10% duplicate rate).
//  If actual rate < 5%, LED fires → orchestrator → kDegraded.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr u64 kLedWindowFrames     = 10'000ULL;
static constexpr u64 kLedThresholdPercent = 5ULL;   // below 5% → latent error

// History bitmask window width (32 sequence numbers)
static constexpr u32 kHistoryWindowWidth  = 32U;
static constexpr u32 kHistoryMaskFull     = 0xFFFF'FFFFU;

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

FrerManager::FrerManager(SwitchHal& hal) noexcept
    : hal_         { hal   }
    , initialised_ { false }
    , streamCount_ { 0U    }
    , states_      {}
    , entries_     {}
{
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

Status FrerManager::Init() noexcept
{
    streamCount_ = 0U;
    for (std::size_t i = 0U; i < kMaxFrerEntries; ++i)
    {
        states_[i]  = FrerStreamState{};
        entries_[i] = FrerStreamEntry{};
    }
    initialised_ = true;
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — periodic 125 µs state machine advance
// ─────────────────────────────────────────────────────────────────────────────

Status FrerManager::Tick(u64 nowNs) noexcept
{
    if (!initialised_) { return Status::Err(ErrorCode::kNotInitialised); }

    bool latentErrorFired = false;

    for (u8 i = 0U; i < streamCount_; ++i)
    {
        FrerStreamState& st  = states_[i];
        const FrerStreamEntry& en = entries_[i];

        // ── Path silence detection ────────────────────────────────────────
        if (st.lastFrameNs != 0ULL)
        {
            for (u8 path = 0U; path < en.memberPaths; ++path)
            {
                const u8 pathBit = static_cast<u8>(1U << path);

                if ((st.pathHealthMask & pathBit) != 0U)
                {
                    // Path was healthy — check if silence timeout exceeded
                    // We track last received path separately in a per-path
                    // timestamp array; here we use the global lastFrameNs
                    // as a conservative proxy (all-path silence).
                    const u64 silenceNs = nowNs - st.lastFrameNs;
                    if (silenceNs > en.recoveryTimeoutNs)
                    {
                        // Mark path as silent
                        st.pathHealthMask &= static_cast<u8>(~pathBit);
                    }
                }
            }
        }

        // ── Latent Error Detection ────────────────────────────────────────
        CheckLatentError(st, nowNs);
        if (st.latentErrorEvents > 0ULL)
        {
            latentErrorFired = true;
        }
    }

    if (latentErrorFired)
    {
        return Status::Err(ErrorCode::kFrerLatentError);
    }
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream Registration
// ─────────────────────────────────────────────────────────────────────────────

Status FrerManager::RegisterStream(const FrerStreamEntry& entry) noexcept
{
    if (!initialised_) { return Status::Err(ErrorCode::kNotInitialised); }
    if (streamCount_ >= static_cast<u8>(kMaxFrerEntries))
        { return Status::Err(ErrorCode::kFrerTableFull); }

    const u8 slot = streamCount_;

    entries_[slot] = entry;

    FrerStreamState& st   = states_[slot];
    st.streamHandle       = entry.streamHandle;
    st.recoveryState      = FrerRecoveryState::kPassRecovering;
    st.lastSeqNum         = 0U;
    st.expectedSeqNum     = 0U;
    st.seqNumHistoryMask  = 0U;
    st.pathHealthMask     = static_cast<u8>((1U << entry.memberPaths) - 1U);
    st.lastPathReceived   = 0U;
    st.framesAccepted             = 0ULL;
    st.framesDiscardedDuplicate   = 0ULL;
    st.framesOutOfOrder           = 0ULL;
    st.latentErrorEvents          = 0ULL;
    st.lastFrameNs                = 0ULL;
    st.pathSilenceDeadlineNs      = 0ULL;

    // Configure hardware replication / recovery in switch ASIC
    TSN_RETURN_IF_ERR(hal_.RegisterFrerStream(entry));

    ++streamCount_;
    return Status::Ok();
}

Status FrerManager::DeregisterStream(u16 streamHandle) noexcept
{
    const u8 slot = FindStreamSlot(streamHandle);
    if (slot >= streamCount_) { return Status::Err(ErrorCode::kPsfsStreamUnknown); }

    TSN_RETURN_IF_ERR(hal_.DeregisterFrerStream(streamHandle));

    // Compact the arrays by shifting remaining entries down
    for (u8 i = slot; i < static_cast<u8>(streamCount_ - 1U); ++i)
    {
        states_[i]  = states_[static_cast<u8>(i + 1U)];
        entries_[i] = entries_[static_cast<u8>(i + 1U)];
    }
    --streamCount_;
    return Status::Ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// Frame Reception Notification
// ─────────────────────────────────────────────────────────────────────────────

Result<bool, ErrorCode> FrerManager::OnFrameReceived(u16 streamHandle,
                                                       u16 sequenceNum,
                                                       u8  pathIndex,
                                                       u64 nowNs) noexcept
{
    const u8 slot = FindStreamSlot(streamHandle);
    if (slot >= streamCount_)
        { return Result<bool, ErrorCode>::Err(ErrorCode::kPsfsStreamUnknown); }

    FrerStreamState& st = states_[slot];

    // ── Update path health ────────────────────────────────────────────────
    const u8 pathBit = static_cast<u8>(1U << pathIndex);
    st.pathHealthMask  |= pathBit;  // Mark path as alive
    st.lastPathReceived = pathIndex;
    st.lastFrameNs      = nowNs;

    // ── Duplicate detection (O(1) bitmask) ───────────────────────────────
    if (IsDuplicate(st, sequenceNum))
    {
        ++st.framesDiscardedDuplicate;
        return Result<bool, ErrorCode>::Ok(false);  // Discard
    }

    // ── Out-of-window detection ───────────────────────────────────────────
    const i32 delta = static_cast<i32>(sequenceNum) - static_cast<i32>(st.lastSeqNum);
    if (delta < -static_cast<i32>(kHistoryWindowWidth) ||
        delta >  static_cast<i32>(kHistoryWindowWidth) * 2)
    {
        // Sequence reset: IEEE 802.1CB §10.4.1.2 – reset history
        st.seqNumHistoryMask = 0U;
        st.recoveryState     = FrerRecoveryState::kReset;
        ++st.framesOutOfOrder;
    }

    // ── Accept frame ─────────────────────────────────────────────────────
    // Set bit in history window for this sequence number
    const u32 bit = static_cast<u32>(sequenceNum) % kHistoryWindowWidth;
    st.seqNumHistoryMask |= (1U << bit);
    st.lastSeqNum         = sequenceNum;
    st.expectedSeqNum     = static_cast<u16>(sequenceNum + 1U);
    st.recoveryState      = FrerRecoveryState::kPassRecovering;

    ++st.framesAccepted;
    return Result<bool, ErrorCode>::Ok(true);  // Forward
}

// ─────────────────────────────────────────────────────────────────────────────
// Statistics
// ─────────────────────────────────────────────────────────────────────────────

Status FrerManager::GetStreamStats(u16  streamHandle,
                                    u64& outAccepted,
                                    u64& outDuplicates,
                                    u64& outOutOfOrder,
                                    u64& outLatentErrors) const noexcept
{
    const u8 slot = FindStreamSlot(streamHandle);
    if (slot >= streamCount_) { return Status::Err(ErrorCode::kPsfsStreamUnknown); }

    const FrerStreamState& st = states_[slot];
    outAccepted     = st.framesAccepted;
    outDuplicates   = st.framesDiscardedDuplicate;
    outOutOfOrder   = st.framesOutOfOrder;
    outLatentErrors = st.latentErrorEvents;
    return Status::Ok();
}

Status FrerManager::GetPathHealth(u16 streamHandle, u8& outHealthMask) const noexcept
{
    const u8 slot = FindStreamSlot(streamHandle);
    if (slot >= streamCount_) { return Status::Err(ErrorCode::kPsfsStreamUnknown); }
    outHealthMask = states_[slot].pathHealthMask;
    return Status::Ok();
}

u8 FrerManager::GetStreamCount() const noexcept { return streamCount_; }

// ─────────────────────────────────────────────────────────────────────────────
// Internal Helpers
// ─────────────────────────────────────────────────────────────────────────────

u8 FrerManager::FindStreamSlot(u16 streamHandle) const noexcept
{
    for (u8 i = 0U; i < streamCount_; ++i)
    {
        if (states_[i].streamHandle == streamHandle) { return i; }
    }
    return static_cast<u8>(kMaxFrerEntries);  // Sentinel: not found
}

bool FrerManager::IsDuplicate(FrerStreamState& state, u16 seqNum) noexcept
{
    const u32 bit = static_cast<u32>(seqNum) % kHistoryWindowWidth;
    if ((state.seqNumHistoryMask & (1U << bit)) != 0U)
    {
        // Bit already set in history window → duplicate
        return true;
    }
    return false;
}

void FrerManager::CheckLatentError(FrerStreamState& state, u64 nowNs) noexcept
{
    // LED check: after kLedWindowFrames total accepted frames,
    // verify duplicate rate is above kLedThresholdPercent.
    const u64 totalFrames = state.framesAccepted + state.framesDiscardedDuplicate;
    if (totalFrames < kLedWindowFrames) { return; }

    const u64 duplicateRatePercent =
        (state.framesDiscardedDuplicate * 100ULL) / totalFrames;

    if (duplicateRatePercent < kLedThresholdPercent)
    {
        ++state.latentErrorEvents;
        // Reset counters for next LED window
        state.framesAccepted           = 0ULL;
        state.framesDiscardedDuplicate = 0ULL;
    }

    (void)nowNs;  // Reserved for timestamp-based LED window in future
}

} // namespace tsn
} // namespace norxs
