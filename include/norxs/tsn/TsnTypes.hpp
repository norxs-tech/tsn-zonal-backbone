// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        TsnTypes.hpp
 * @brief       Foundational types, enumerations, PTP timestamp structures, and the
 *              Result<T,E> error monad for the norxs Deterministic TSN Zonal Backbone.
 *              All types conform to AUTOSAR C++14 (MISRA C++:2008) constraints:
 *              no dynamic allocation, no exceptions, fixed-width integers throughout.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1Qbv, IEEE 802.1AS-Rev,
 *              IEEE 802.1CB, IEEE 802.1Qci, IEEE 802.1AE, ISO 26262 ASIL-D
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 *              Contact: contact@norxs.com | https://www.norxs.com
 * =====================================================================================
 */

#ifndef NORXS_TSN_TYPES_HPP
#define NORXS_TSN_TYPES_HPP

#include <cstdint>
#include <cstddef>
#include <array>
#include <limits>

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// §1  Platform Integer Aliases
// ─────────────────────────────────────────────────────────────────────────────

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// ─────────────────────────────────────────────────────────────────────────────
// §2  IEEE 802.1Qbv / TSN Dimensional Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Maximum egress traffic queues per port (IEEE 802.1Qbv §8.6.8.4)
static constexpr std::size_t kMaxTrafficQueues  = 8U;

/// Maximum GCL entries per port (hardware SRAM limit for SJA1110 / 88Q5050)
static constexpr std::size_t kMaxGclEntries     = 256U;

/// Maximum physical switch ports supported
static constexpr std::size_t kMaxSwitchPorts    = 16U;

/// Maximum Per-Stream Filter instances (IEEE 802.1Qci §8.28)
static constexpr std::size_t kMaxStreamFilters  = 128U;

/// Maximum FRER sequence recovery entries (IEEE 802.1CB §7.4)
static constexpr std::size_t kMaxFrerEntries    = 64U;

/// Maximum MACsec Secure Channels (IEEE 802.1AE §10.7)
static constexpr std::size_t kMaxSecureChannels = 32U;

/// Nanoseconds per second
static constexpr u64 kNsPerSecond               = 1'000'000'000ULL;

/// Minimum admissible cycle time: 100 µs
static constexpr u64 kMinCycleTimeNs            = 100'000ULL;

/// Maximum admissible cycle time: 1 s
static constexpr u64 kMaxCycleTimeNs            = 1'000'000'000ULL;

/// 1000BASE-T1 link speed in bits/nanosecond (= 1 bit/ns)
static constexpr u64 kLinkSpeedBitsPerNs        = 1U;

/// Maximum Ethernet frame size including preamble + IFG: 1538 bytes
static constexpr u32 kMaxFrameSizeBytes         = 1538U;

/// Minimum guard band for 1000BASE-T1: ceil(1538B * 8b / 1Gbps) = 12304 ns
/// IEEE 802.1Qbv §8.6.8.4 – guard band prevents frame preemption at window edge
static constexpr u32 kMinGuardBandNs            = 12'304U;

/// ASIL-D worst-case Brake-by-Wire end-to-end deadline: 500 µs
static constexpr u64 kSafetyDeadlineNs          = 500'000ULL;

/// gPTP synchronisation accuracy target: < 1 µs
static constexpr u64 kPtpSyncTargetNs           = 1'000ULL;

/// Holdover oscillator drift budget: 1 µs/s (TCXO grade)
static constexpr u64 kHoldoverDriftNsPerSec     = 1'000ULL;

// ─────────────────────────────────────────────────────────────────────────────
// §3  Traffic Class  (IEEE 802.1Q PCP mapping)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief IEEE 802.1Q Traffic Class — maps 1:1 to egress queue index [0..7].
enum class TrafficClass : u8
{
    kBestEffort          = 0U,  ///< Standard best-effort background
    kBackground          = 1U,  ///< Low-priority bulk data
    kExcellentEffort     = 2U,  ///< OEM-reserved
    kCriticalApplication = 3U,  ///< ADAS non-time-critical diagnostics
    kVideo               = 4U,  ///< Surround-view / AVB Class B (CBS)
    kVoice               = 5U,  ///< AVB Class A (CBS, lower latency)
    kInternetworkControl = 6U,  ///< gPTP, LLDP, network management
    kSafetyCritical      = 7U,  ///< Brake-by-Wire – ASIL-D TAS exclusive
};

// ─────────────────────────────────────────────────────────────────────────────
// §4  Gate States  (IEEE 802.1Qbv §8.6.8.4)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Single queue gate open/closed state.
enum class GateState : u8
{
    kOpen   = 1U,
    kClosed = 0U,
};

/// @brief Packed 8-bit gate bitmask. Bit N → TrafficClass N. '1' = Open.
///        Example: 0x80 = only TC7 (SafetyCritical) open – ASIL-D exclusive window.
using GateStateBitmask = u8;

/// Bitmask constant: all queues open (pass-through / TAS disabled)
static constexpr GateStateBitmask kAllQueuesOpen   = 0xFFU;

/// Bitmask constant: only TC7 open – mandatory ASIL-D safety window
static constexpr GateStateBitmask kSafetyOnlyOpen  = 0x80U;

/// Bitmask constant: all queues closed (guard band interval)
static constexpr GateStateBitmask kAllQueuesClosed = 0x00U;

// ─────────────────────────────────────────────────────────────────────────────
// §5  Error Codes
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Canonical error codes for the entire TSN Backbone stack.
///        Numeric ranges are reserved per domain to allow future extension.
enum class ErrorCode : u32
{
    kOk                            = 0U,

    // ── Schedule Validation  [100–199] ─────────────────────────────────────
    kGclEmpty                      = 100U,
    kGclTooManyEntries             = 101U,
    kCycleTimeTooShort             = 102U,
    kCycleTimeTooLong              = 103U,
    kIntervalSumMismatch           = 104U,
    kOverlappingWindows            = 105U,
    kSafetyCriticalWindowAbsent    = 106U,
    kZeroIntervalEntry             = 107U,
    kGuardBandViolation            = 108U,  ///< TC7 window < minGuardBand + maxFrameNs
    kDeadlineViolation             = 109U,  ///< Network Calculus WCD > ASIL-D deadline

    // ── Hardware Abstraction Layer  [200–299] ──────────────────────────────
    kHalWriteFailed                = 200U,
    kHalReadFailed                 = 201U,
    kHalPortOutOfRange             = 202U,
    kHalNullPointer                = 203U,
    kHalBusTimeout                 = 204U,
    kHalRegisterLocked             = 205U,
    kHalShadowBufferBusy           = 206U,  ///< Admin→Oper swap still pending
    kHalDmaError                   = 207U,

    // ── Clock Synchronisation  [300–399] ───────────────────────────────────
    kPtpNotSynchronised            = 300U,
    kBaseTimeInPast                = 301U,
    kClockDriftExceeded            = 302U,
    kPtpHoldoverExpired            = 303U,  ///< Holdover budget exhausted
    kBmcaNoGrandmaster             = 304U,  ///< BMCA cannot elect a GM

    // ── FRER (802.1CB)  [400–499] ──────────────────────────────────────────
    kFrerTableFull                 = 400U,
    kFrerSequenceReset             = 401U,  ///< Sequence number wrapped
    kFrerLatentError               = 402U,  ///< Latent error detection triggered
    kFrerMemberInvalid             = 403U,

    // ── Stream Policing (802.1Qci)  [500–599] ─────────────────────────────
    kPsfsTableFull                 = 500U,
    kPsfsBurstExceeded             = 501U,
    kPsfsStreamUnknown             = 502U,

    // ── MACsec (802.1AE)  [600–699] ────────────────────────────────────────
    kMacsecKeyExpired              = 600U,
    kMacsecReplayAttack            = 601U,
    kMacsecChannelFull             = 602U,

    // ── General  [900–999] ─────────────────────────────────────────────────
    kInvalidArgument               = 900U,
    kNotInitialised                = 901U,
    kOperationNotPermitted         = 902U,
    kBufferTooSmall                = 903U,
    kTimeout                       = 904U,
};

// ─────────────────────────────────────────────────────────────────────────────
// §6  Result<T,E> — Exception-free error monad  (AUTOSAR M15-3-1 exempt)
// ─────────────────────────────────────────────────────────────────────────────

template <typename T, typename E = ErrorCode>
struct Result final
{
    static Result Ok(T value) noexcept  { Result r; r.ok_ = true;  r.value_ = value; return r; }
    static Result Err(E error) noexcept { Result r; r.ok_ = false; r.error_ = error; return r; }

    bool IsOk()    const noexcept { return  ok_; }
    bool IsError() const noexcept { return !ok_; }
    T    Value()   const noexcept { return value_; }  // UB if IsError()
    E    Error()   const noexcept { return error_; }  // UB if IsOk()

private:
    bool ok_    { false };
    T    value_ {};
    E    error_ { E{} };
};

template <typename E>
struct Result<void, E> final
{
    static Result Ok()     noexcept { Result r; r.ok_ = true;  return r; }
    static Result Err(E e) noexcept { Result r; r.ok_ = false; r.error_ = e; return r; }

    bool IsOk()    const noexcept { return  ok_; }
    bool IsError() const noexcept { return !ok_; }
    E    Error()   const noexcept { return error_; }

private:
    bool ok_    { false };
    E    error_ { E{} };
};

/// Canonical void-success / error-code status type used throughout the stack.
using Status = Result<void, ErrorCode>;

// ─────────────────────────────────────────────────────────────────────────────
// §7  Error-propagation macro  (MISRA C++:2008 Rule 16-0-4 compliant wrapper)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief If @p expr yields an error Status, immediately return it to the caller.
///        Equivalent to Rust's `?` operator.  do-while(false) prevents dangling-else.
#define TSN_RETURN_IF_ERR(expr)                  \
    do {                                         \
        const ::norxs::tsn::Status _s_ = (expr); \
        if (_s_.IsError()) { return _s_; }       \
    } while (false)

// ─────────────────────────────────────────────────────────────────────────────
// §8  PTP Timestamp  (IEEE 802.1AS §10.6 — 80-bit: 48-bit seconds + 32-bit ns)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief IEEE 802.1AS network timestamp.
///
///        seconds holds a 48-bit value; the upper 16 bits are always zero.
///        Using a plain u64 avoids MISRA C++:2008 Rule 9-6-4 (bitfield on u64
///        is implementation-defined layout).
struct PtpTimestamp
{
    u64 seconds;      ///< 48-bit seconds field (bits [63:48] must be zero)
    u32 nanoseconds;  ///< Sub-second component in [0, 999 999 999]

    /// @brief Return true if this timestamp is strictly before @p other.
    bool IsBefore(const PtpTimestamp& other) const noexcept
    {
        if (seconds != other.seconds) { return seconds < other.seconds; }
        return nanoseconds < other.nanoseconds;
    }

    /// @brief Return the difference to @p other in nanoseconds.
    ///        Returns 0 if @p other is not strictly after this.
    u64 DeltaNs(const PtpTimestamp& other) const noexcept
    {
        if (!IsBefore(other)) { return 0ULL; }
        const u64 secDelta = (other.seconds - seconds) * kNsPerSecond;
        const u64 nsDelta  = (other.nanoseconds >= nanoseconds)
                             ? (other.nanoseconds - nanoseconds)
                             : (kNsPerSecond - nanoseconds + other.nanoseconds);
        return secDelta + nsDelta;
    }
};

/// Sentinel: zero timestamp (UNIX epoch, used as "not set")
static constexpr PtpTimestamp kPtpTimestampZero{ 0ULL, 0U };

// ─────────────────────────────────────────────────────────────────────────────
// §9  Gate Control List Entry  (IEEE 802.1Qbv §8.6.8.4 Table 8-7)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Single row of a Gate Control List.
struct GclEntry
{
    GateStateBitmask gateStates;  ///< Packed TC[7:0] gate mask
    u32              intervalNs;  ///< Duration in nanoseconds (> 0 always)
};

// ─────────────────────────────────────────────────────────────────────────────
// §10  Schedule Parameters
// ─────────────────────────────────────────────────────────────────────────────

/// @brief High-level descriptor for a IEEE 802.1Qbv Time-Aware schedule.
struct ScheduleParams
{
    u64              cycleTimeNs;                         ///< Repeating period [ns]
    PtpTimestamp     baseTime;                            ///< Absolute PTP cycle start
    u32              maxSafetyFrameSizeBytes;             ///< Largest expected TC7 frame
    u8               entryCount;                          ///< Valid GCL entries [1, 256]
    std::array<GclEntry, kMaxGclEntries> gcl;            ///< Gate Control List
};

// ─────────────────────────────────────────────────────────────────────────────
// §11  Network Calculus — Per-Class Traffic Envelope
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Token-bucket arrival curve (σ, ρ) for one traffic class.
///        Used by NetworkCalculusAnalyzer to compute worst-case delays.
struct TrafficEnvelope
{
    TrafficClass tc;          ///< Traffic class this envelope describes
    u32          burstBytes;  ///< σ: maximum burst size in bytes
    u32          rateKbps;    ///< ρ: sustained rate in kbit/s
    u64          deadlineNs;  ///< Application-level end-to-end deadline
};

// ─────────────────────────────────────────────────────────────────────────────
// §12  FRER Stream Descriptor  (IEEE 802.1CB §7.4)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Identifies a replicated stream and its recovery parameters.
struct FrerStreamEntry
{
    u16                    streamHandle;      ///< Unique stream handle
    std::array<u8, 6U>    destinationMac;    ///< Destination MAC address
    u16                    vlanId;            ///< 802.1Q VLAN
    u8                     memberPaths;       ///< Number of disjoint paths (≥ 2)
    u32                    recoveryTimeoutNs; ///< Max inter-path skew before reset
    bool                   isGenerator;       ///< True = replication side
};

// ─────────────────────────────────────────────────────────────────────────────
// §13  Per-Stream Filter Instance  (IEEE 802.1Qci §8.28)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Flow policing rule applied at ingress per stream.
struct StreamFilterInstance
{
    u16                    streamHandle;       ///< Matches FrerStreamEntry or standalone
    std::array<u8, 6U>    sourceMac;          ///< Source MAC (all-zeros = wildcard)
    u16                    vlanId;             ///< VLAN (0 = wildcard)
    u32                    maxFrameSizeBytes;  ///< Frames above this size are dropped
    u32                    committedRateKbps;  ///< Token-bucket CIR
    u32                    committedBurstBytes;///< Token-bucket CBS
    bool                   blockOnLateFrame;  ///< Drop all frames after first late one
};

// ─────────────────────────────────────────────────────────────────────────────
// §14  System State Snapshot  (used by TsnOrchestrator for diagnostics)
// ─────────────────────────────────────────────────────────────────────────────

enum class OrchestratorState : u8
{
    kUninitialized   = 0U,
    kInitializing    = 1U,
    kPtpConverging   = 2U,  ///< Waiting for gPTP lock
    kScheduleArmed   = 3U,  ///< GCL loaded, awaiting baseTime
    kRunning         = 4U,  ///< Fully operational
    kDegraded        = 5U,  ///< Running with reduced function (e.g. one path failed)
    kSafeState       = 6U,  ///< ASIL-D safe state: safety traffic only
    kFault           = 7U,  ///< Unrecoverable fault, requires reset
};

enum class ClockRole : u8
{
    kGrandmaster = 0U,
    kBoundary    = 1U,
    kSlave       = 2U,
};

enum class SyncState : u8
{
    kFreeRun  = 0U,  ///< No synchronisation
    kLocking  = 1U,  ///< BMCA running, not yet within kPtpSyncTargetNs
    kLocked   = 2U,  ///< Within kPtpSyncTargetNs of grandmaster
    kHoldover = 3U,  ///< GM lost; local oscillator compensating
};

} // namespace tsn
} // namespace norxs

#endif // NORXS_TSN_TYPES_HPP
