// SPDX-License-Identifier: LicenseRef-norxs-RI-1.0
// SPDX-FileCopyrightText: (c) 2026 norxs Technology LLC <contact@norxs.com>
//
/**
 * =====================================================================================
 * @file        SwitchHal.hpp
 * @brief       Pure-virtual Hardware Abstraction Layer for Automotive Ethernet Switch
 *              ASIC register access via MDIO, SPI, or PCIe management interface.
 *              Defines the Admin→Oper double-buffer swap semantics required by
 *              IEEE 802.1Qbv §8.6.9.4 for race-free GCL updates, and exposes
 *              interfaces for VLAN, TCAM (802.1Qci), FRER (802.1CB), and MACsec
 *              (802.1AE) configuration planes.
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, IEEE 802.1Qbv, IEEE 802.1CB,
 *              IEEE 802.1Qci, IEEE 802.1AE, ISO 26262 ASIL-D
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 *              Contact: contact@norxs.com | https://www.norxs.com
 * =====================================================================================
 */

#ifndef NORXS_TSN_SWITCH_HAL_HPP
#define NORXS_TSN_SWITCH_HAL_HPP

#include "TsnTypes.hpp"
#include <array>

namespace norxs {
namespace tsn {

// ─────────────────────────────────────────────────────────────────────────────
// Register address and data types
// ─────────────────────────────────────────────────────────────────────────────

using RegAddr = u32;
using RegData = u32;

/// @brief Port index sentinel for chip-global registers (not port-specific).
static constexpr u8 kGlobalPort = 0xFFU;

// ─────────────────────────────────────────────────────────────────────────────
// MACsec Key Material  (IEEE 802.1AE §10.7)
// ─────────────────────────────────────────────────────────────────────────────

/// @brief 128-bit Secure Association Key (SAK) for MACsec AES-GCM-128.
using MacsecSak128 = std::array<u8, 16U>;

/// @brief MACsec Secure Channel configuration.
struct MacsecChannelConfig
{
    u8          channelIndex;           ///< HAL channel slot [0, kMaxSecureChannels)
    u8          sci[8];                 ///< Secure Channel Identifier (IEEE 802.1AE §6.4)
    MacsecSak128 sak;                   ///< Session key (never logged; zeroised after load)
    u32         replayWindow;           ///< Anti-replay window size in frames
    bool        encryptionEnabled;      ///< True = encrypt+authenticate; false = auth only
    bool        isReceiveChannel;       ///< True = SecY RX path; false = TX path
};

// ─────────────────────────────────────────────────────────────────────────────
// SwitchHal — Abstract Interface
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Abstract interface encapsulating all switch ASIC hardware operations.
///
///        Concrete implementations (Sja1110Hal, Q5050Hal) map these calls onto
///        the physical management bus.  The interface is partitioned into five
///        functional planes mirroring the TSN standard stack:
///
///          Plane 1 — Initialisation & Raw Register Access
///          Plane 2 — IEEE 802.1Qbv  Time-Aware Shaper (TAS / GCL)
///          Plane 3 — IEEE 802.1Qci  Per-Stream Filtering & Policing
///          Plane 4 — IEEE 802.1CB   Frame Replication & Elimination (FRER)
///          Plane 5 — IEEE 802.1AE   MACsec Line-Rate Encryption
///          Plane 6 — Diagnostics
///
///        AUTOSAR / MISRA constraints applied to all implementations:
///          - No dynamic memory allocation in any method body (M18-4-1)
///          - No C++ exceptions thrown (M15-3-1 applied to all overrides)
///          - All methods noexcept
///          - Thread-safety is the responsibility of the concrete implementor
class SwitchHal
{
public:
    SwitchHal()                            = default;
    virtual ~SwitchHal()                   = default;
    SwitchHal(const SwitchHal&)            = delete;
    SwitchHal& operator=(const SwitchHal&) = delete;
    SwitchHal(SwitchHal&&)                 = delete;
    SwitchHal& operator=(SwitchHal&&)      = delete;

    // =========================================================================
    // Plane 1 — Initialisation & Raw Register Access
    // =========================================================================

    /// @brief One-time hardware initialisation.
    ///        Verifies chip ID, asserts/releases hardware reset, applies base
    ///        configuration (VLAN defaults, port enables, clock configuration).
    ///
    /// @return Status::Ok()               on success.
    /// @return kHalBusTimeout             management bus unresponsive.
    /// @return kHalWriteFailed            initial register write failed.
    virtual Status Init() noexcept = 0;

    /// @brief Write a 32-bit register.
    ///
    /// @param port     Physical port [0, kMaxSwitchPorts) or kGlobalPort.
    /// @param address  Register address.
    /// @param data     Value to write.
    virtual Status WriteRegister(u8 port, RegAddr address, RegData data) noexcept = 0;

    /// @brief Read a 32-bit register.
    ///
    /// @param port     Physical port or kGlobalPort.
    /// @param address  Register address.
    /// @param outData  Populated with register contents on success.
    virtual Status ReadRegister(u8 port, RegAddr address, RegData& outData) const noexcept = 0;

    /// @brief Atomic read–modify–write on a register field.
    ///
    /// @param port     Physical port or kGlobalPort.
    /// @param address  Register address.
    /// @param mask     Field bitmask.
    /// @param value    New field value (pre-shifted into position).
    virtual Status ReadModifyWrite(u8 port, RegAddr address, RegData mask, RegData value) noexcept = 0;

    // =========================================================================
    // Plane 2 — IEEE 802.1Qbv  Time-Aware Shaper
    //
    // Double-Buffer / Admin→Oper Swap Semantics (IEEE 802.1Qbv §8.6.9.4):
    //
    //   The hardware maintains two GCL banks:
    //     - AdminGateStates  : the "pending" configuration being written
    //     - OperGateStates   : the "live" configuration currently executing
    //
    //   The swap from Admin→Oper happens atomically at a PTP baseTime boundary,
    //   ensuring the currently executing schedule is never torn mid-cycle.
    //   Implementations must:
    //     1. Write all GCL entries to the Admin bank (no bus locking required)
    //     2. Write AdminCycleTime and AdminBaseTime registers
    //     3. Set the ConfigChange bit to trigger the Admin→Oper swap
    //     4. Poll ConfigPending until the hardware clears it (swap done)
    //   Steps 3–4 must complete before the next cycle boundary.
    // =========================================================================

    /// @brief Load and atomically activate a validated Gate Control List.
    ///
    ///        This method performs the complete Admin→Oper swap sequence.
    ///        The caller (GateControlManager) guarantees @p params is validated.
    ///        The new schedule becomes OperGateStates at @p params.baseTime.
    ///
    /// @param port    Egress port to configure.
    /// @param params  Validated schedule (cycle time, base time, GCL entries).
    ///
    /// @return Status::Ok()            swap committed successfully.
    /// @return kHalShadowBufferBusy   previous Admin→Oper swap still pending.
    /// @return kHalPortOutOfRange     port index out of range.
    /// @return kHalWriteFailed        Admin bank write failed.
    /// @return kHalBusTimeout         ConfigPending poll timed out.
    virtual Status ApplyGclToPort(u8 port, const ScheduleParams& params) noexcept = 0;

    /// @brief Read back the currently active (Oper) GCL from hardware.
    ///        Used by diagnostics to verify the schedule was applied correctly.
    ///
    /// @param port       Port to query.
    /// @param outParams  Populated with the live Oper schedule.
    virtual Status ReadOperGcl(u8 port, ScheduleParams& outParams) const noexcept = 0;

    /// @brief Poll whether an Admin→Oper swap is still pending on a port.
    ///
    /// @param port      Port to query.
    /// @param outPending True if hardware ConfigPending bit is set.
    virtual Status IsGclSwapPending(u8 port, bool& outPending) const noexcept = 0;

    // =========================================================================
    // Plane 3 — IEEE 802.1Qci  Per-Stream Filtering and Policing
    // =========================================================================

    /// @brief Install a Per-Stream Filter and Policing rule at ingress.
    ///
    ///        The hardware drops frames that exceed the token-bucket rate or
    ///        the maximum frame size defined in @p sfi.  If blockOnLateFrame
    ///        is set, all subsequent frames from the stream are dropped once
    ///        the first violation occurs (DDoS / Broadcast Storm containment).
    ///
    /// @param sfi  Stream filter rule to install.
    virtual Status SetStreamFilter(const StreamFilterInstance& sfi) noexcept = 0;

    /// @brief Remove a previously installed stream filter by stream handle.
    virtual Status ClearStreamFilter(u16 streamHandle) noexcept = 0;

    /// @brief Read the hardware ingress drop counter for a stream.
    ///
    /// @param streamHandle  Target stream.
    /// @param outDrops      Populated with cumulative drop count.
    virtual Status GetStreamDropCount(u16 streamHandle, u64& outDrops) const noexcept = 0;

    // =========================================================================
    // Plane 4 — IEEE 802.1CB  Frame Replication and Elimination for Reliability
    // =========================================================================

    /// @brief Register a FRER stream and configure replication or recovery.
    ///
    ///        For generator side (isGenerator = true):
    ///          Hardware replicates outgoing frames onto all memberPaths.
    ///        For receiver side (isGenerator = false):
    ///          Hardware accepts the first copy of each sequence number and
    ///          discards duplicates within recoveryTimeoutNs.
    ///
    /// @param entry  Stream descriptor including MAC, VLAN, and path count.
    virtual Status RegisterFrerStream(const FrerStreamEntry& entry) noexcept = 0;

    /// @brief Remove a FRER stream registration.
    virtual Status DeregisterFrerStream(u16 streamHandle) noexcept = 0;

    /// @brief Read FRER sequence recovery statistics for a stream.
    ///
    /// @param streamHandle    Target stream.
    /// @param outDuplicates   Frames discarded as duplicates (healthy redundancy).
    /// @param outOutOfOrder   Frames received out of sequence (path skew indicator).
    /// @param outLatentErrors Latent error detection events (IEEE 802.1CB §7.4).
    virtual Status GetFrerStats(u16  streamHandle,
                                u64& outDuplicates,
                                u64& outOutOfOrder,
                                u64& outLatentErrors) const noexcept = 0;

    // =========================================================================
    // Plane 5 — IEEE 802.1AE  MACsec Line-Rate Encryption
    // =========================================================================

    /// @brief Programme a MACsec Secure Channel with a new Session Key.
    ///
    ///        Key material in @p cfg.sak is loaded into hardware key storage
    ///        and then zeroed in the struct (caller's copy is also zeroed via
    ///        the returned reference — implementation responsibility).
    ///        This call must complete within one AES-GCM key-rotation window.
    ///
    /// @param cfg  Secure Channel configuration including SAK.
    virtual Status ConfigureMacsecChannel(const MacsecChannelConfig& cfg) noexcept = 0;

    /// @brief Rotate the Session Key on an active Secure Channel (hitless).
    ///
    ///        Uses the hardware's dual-SAK mechanism to rotate keys without
    ///        dropping frames — new SAK pre-loaded while old SAK still active.
    ///
    /// @param channelIndex  Channel to re-key.
    /// @param newSak        New 128-bit Session Key.
    virtual Status RotateMacsecKey(u8 channelIndex, const MacsecSak128& newSak) noexcept = 0;

    /// @brief Read MACsec integrity-failure counters for a channel.
    ///
    /// @param channelIndex   Target channel.
    /// @param outAuthFails   Frames that failed authentication (replay / spoof).
    virtual Status GetMacsecAuthFailures(u8 channelIndex, u64& outAuthFails) const noexcept = 0;

    // =========================================================================
    // Plane 6 — VLAN & TCAM Management
    // =========================================================================

    /// @brief Write a VLAN forwarding entry.
    ///
    /// @param vlanId         802.1Q VLAN [1, 4094].
    /// @param memberPortMask Ports that belong to this VLAN.
    /// @param untagPortMask  Ports that egress without a VLAN tag.
    virtual Status SetVlanEntry(u16 vlanId, u16 memberPortMask, u16 untagPortMask) noexcept = 0;

    /// @brief Install a Layer-2 ingress TCAM rule.
    ///
    /// @param ruleIndex   TCAM slot index.
    /// @param matchMac    Destination MAC pattern.
    /// @param matchVlan   VLAN pattern (0 = wildcard).
    /// @param actionDrop  True = drop matching frames at ingress.
    virtual Status SetTcamRule(u8                         ruleIndex,
                               const std::array<u8, 6U>& matchMac,
                               u16                        matchVlan,
                               bool                       actionDrop) noexcept = 0;

    // =========================================================================
    // Plane 7 — Diagnostics
    // =========================================================================

    /// @brief Read total port-level drop counter.
    virtual Status GetPortDropCount(u8 port, u64& outCount) const noexcept = 0;

    /// @brief Read hardware link status for a port.
    ///
    /// @param port      Target port.
    /// @param outUp     True if 1000BASE-T1 link is established.
    /// @param outSpeedMbps  Negotiated speed in Mbit/s.
    virtual Status GetLinkStatus(u8 port, bool& outUp, u32& outSpeedMbps) const noexcept = 0;

    /// @brief Return a null-terminated string literal identifying this HAL.
    ///        Must NOT allocate. Example: "NXP SJA1110 Rev C — SPI HAL v2.1"
    virtual const char* GetDeviceIdentifier() const noexcept = 0;

    /// @brief Return the number of physical ports on this device.
    virtual u8 GetPortCount() const noexcept = 0;
};

} // namespace tsn
} // namespace norxs

#endif // NORXS_TSN_SWITCH_HAL_HPP
