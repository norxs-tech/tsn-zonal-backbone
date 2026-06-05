# Changelog

All notable changes to the **norxs Deterministic TSN Zonal Backbone** are documented here.
This project follows [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

*(Changes staged for the next release go here)*

---

## [1.0.0] — 2026-06-02

### Initial public release — norxs Technology LLC

Production-grade AUTOSAR C++14 IEEE TSN stack for next-generation Software-Defined Vehicle
Zonal Architectures. Enforces zero-packet-loss, sub-microsecond latency guarantees for
ASIL-D safety-critical payloads with mathematically verified worst-case delay bounds via
Network Calculus. Targets NXP i.MX8X SoC paired with SJA1110 / 88Q5050 switch ASICs.

---

### Added — TsnTypes

#### `TsnTypes.hpp` — Foundational Types and Error Monad
- Fixed-width integer aliases (`u8`/`u16`/`u32`/`u64`/`i32`/`i64`) conforming to AUTOSAR A3-9-1
- Complete `ErrorCode` enumeration with reserved numeric ranges per domain:
  Schedule Validation [100–199], HAL [200–299], Clock [300–399], FRER [400–499],
  Stream Policing [500–599], MACsec [600–699], General [900–999]
- `Result<T,E>` exception-free error monad with `IsOk()`, `IsError()`, `Value()`, `Error()`
  — Rust-equivalent `?` operator via `TSN_RETURN_IF_ERR` macro (MISRA C++:2008 Rule 16-0-4)
- `PtpTimestamp` corrected to use plain `u64 seconds` (avoids MISRA C++:2008 Rule 9-6-4
  implementation-defined bitfield layout; IEEE 802.1AS §10.6 compliant 48-bit seconds field)
- `GclEntry` with `GateStateBitmask` packed 8-bit gate mask (TC[7:0])
- `ScheduleParams` including `maxSafetyFrameSizeBytes` for guard band computation
- `NetworkCalculusResult` struct carrying per-class WCD, deadline met flags, utilisation
- `TrafficEnvelope` (σ, ρ) token-bucket arrival curve for Network Calculus input
- `FrerStreamEntry` — IEEE 802.1CB stream descriptor with disjoint path count
- `StreamFilterInstance` — IEEE 802.1Qci per-stream filter and policing rule
- `OrchestratorState`, `ClockRole`, `SyncState` enumerations for system-wide FSM
- System-wide constants: `kMinGuardBandNs` (12 304 ns at 1 Gbps), `kSafetyDeadlineNs` (500 µs),
  `kPtpSyncTargetNs` (1 µs), `kHoldoverDriftNsPerSec` (1 µs/s TCXO grade)

---

### Added — SwitchHal

#### `SwitchHal.hpp` — Pure-Virtual 6-Plane Hardware Abstraction Layer
- **Plane 1 — Init & Register Access**: `Init()`, `WriteRegister()`, `ReadRegister()`,
  `ReadModifyWrite()` — supports MDIO, SPI, and PCIe management interfaces
- **Plane 2 — IEEE 802.1Qbv TAS (GCL)**: `ApplyGclToPort()`, `ReadOperGcl()`,
  `IsGclSwapPending()` with full IEEE 802.1Qbv §8.6.9.4 Admin→Oper double-buffer
  swap semantics documented inline — prevents race conditions during GCL update
- **Plane 3 — IEEE 802.1Qci Stream Policing**: `SetStreamFilter()`, `ClearStreamFilter()`,
  `GetStreamDropCount()` — token-bucket CIR/CBS per stream, `blockOnLateFrame` DDoS containment
- **Plane 4 — IEEE 802.1CB FRER**: `RegisterFrerStream()`, `DeregisterFrerStream()`,
  `GetFrerStats()` — replication and recovery sides, latent error event counters
- **Plane 5 — IEEE 802.1AE MACsec**: `ConfigureMacsecChannel()`, `RotateMacsecKey()`,
  `GetMacsecAuthFailures()` — dual-SAK hitless key rotation, AES-GCM-128, replay window
- **Plane 6 — VLAN & Diagnostics**: `SetVlanEntry()`, `SetTcamRule()`,
  `GetPortDropCount()`, `GetLinkStatus()`, `GetDeviceIdentifier()`, `GetPortCount()`
- AUTOSAR constraints: all methods `noexcept`, no dynamic allocation, no exceptions,
  deleted copy/move constructors and assignment operators

---

### Added — GateControlManager

#### `GateControlManager.hpp/.cpp` — IEEE 802.1Qbv Schedule Manager
- **7-Stage Validation Pipeline** executed before any HAL access:
  - Stage 1: EntryCount bounds check [1, 256]
  - Stage 2: CycleTime bounds [100 µs, 1 s]
  - Stage 3: No zero-interval entries
  - Stage 4: Σ(intervalNs) == cycleTimeNs (u64 accumulation, no floating-point)
  - Stage 5 *(new)*: **ASIL-D Guard Band** — TC7 window ≥ `FrameNs(maxSafetyFrame) + FrameNs(1538B)` = 12 304 ns minimum at 1 Gbps (IEEE 802.1Qbv §8.6.8.4)
  - Stage 6: Safety window — at least one entry with `gateStates == 0x80` (TC7 exclusively)
  - Stage 7 *(new)*: **Network Calculus** — Latency-Rate server model WCD analysis
- **Network Calculus Engine** (`RunNetworkCalculus()`):
  - LR server model: `WCD_i = T_i + B_i + Σ_{j>i} B_j`
  - Integer-only arithmetic (ns domain, u64) — MISRA C++:2008 Rule 6-4-1 compliant
  - Per-class blocking time computed by `ComputeBlockingTimeNs()`
  - ASIL-D hard gate: `WCD_TC7 ≤ kSafetyDeadlineNs (500 000 ns)`
  - Results stored in `NetworkCalculusResult` for post-validation audit
- `ApplySchedule()` — validates then commits via `hal_.ApplyGclToPort()`
- `DisableSchedule()` — atomic fallback to all-queues-open pass-through
- `VerifyHardwareSchedule()` — reads back Oper GCL and compares cycle time + all gate states
- `GetNetworkCalculusResult()` — exposes last WCD analysis for diagnostic telemetry

---

### Added — PtpClockManager

#### `PtpClockManager.hpp/.cpp` — IEEE 802.1AS-Rev gPTP Clock Manager
- **Integer PI Servo** (no floating-point): Kp = 2^(-2) (shift >>2), Ki = 2^(-5) (shift >>5)
  — anti-windup clamp ±500 000 000 ns·tick; PHC adjustment clamped ±500 000 ppb
- **Sync offset computation**: `offset = t2 - t1 - meanPathDelay - correction` per IEEE 802.1AS §11.3.2
- **State machine**: `kFreeRun → kLocking → kLocked ⇄ kHoldover`
  — kLocked when `|offset| < kPtpSyncTargetNs (1 µs)`
- **BMCA** (Best Master Clock Algorithm — IEEE 802.1AS §10.3.5):
  — Priority1/Priority2/ClockQuality dataset comparison; grandmaster election;
  — `ForceGrandmasterRole()` for bench/HIL configurations (priority1 = 0)
- **Holdover mode**: activated when Sync silence exceeds `syncTimeoutNs`;
  TCXO drift compensation: `driftNs = compensationRate × elapsed / 1e9` (integer scaled);
  budget tracked against `holdoverBudgetNs`; returns `kPtpHoldoverExpired` on expiry
- `InjectSyncMessage()` for HIL/unit test Sync simulation
- `GetCurrentTime()`, `IsSynchronised()`, `GetHoldoverRemainingNs()`

---

### Added — FrerManager

#### `FrerManager.hpp/.cpp` — IEEE 802.1CB FRER State Machine
- **O(1) Bitmask Duplicate Detection**: 32-entry history bitmask per stream
  — `bit = seqNum % 32; if (mask & (1<<bit)) → DUPLICATE` — constant-time regardless of window position
- **Individual Recovery Algorithm** (IEEE 802.1CB §10.4):
  sequence extraction, duplicate check, out-of-window detection, history reset
- **Per-path Silence Monitoring**: `pathHealthMask` per stream updated on every frame receipt;
  path declared failed after `recoveryTimeoutNs` of silence on that path index
- **Latent Error Detection (LED)** (IEEE 802.1CB §7.4.5):
  over a 10 000-frame window, if duplicate rate < 5% → a redundant path is silently broken;
  fires `kFrerLatentError` to TsnOrchestrator → `kDegraded` state transition
- `RegisterStream()` / `DeregisterStream()` — compact static array, no fragmentation
- `OnFrameReceived()` — returns `Result<bool>` (true = forward, false = discard)
- `GetStreamStats()`, `GetPathHealth()` — full diagnostic telemetry

---

### Added — TsnOrchestrator

#### `TsnOrchestrator.hpp/.cpp` — Master Coordinator and ASIL-D State Machine
- **8-Phase Hardware Initialisation** (in `Init()`):
  HAL init, VLAN table, stream filters (802.1Qci), MACsec channels,
  pre-validation of all port schedules (Network Calculus fast-fail),
  GateControlManager placement-new into static storage, gPTP init, FRER registration
- **125 µs Scheduling Loop** (`Tick()`):
  a) `PtpClockManager::Tick()` — servo + BMCA
  b) `FrerManager::Tick()` — path health + LED
  c) `TickIntegrityCheck()` — GCL hardware readback, MACsec auth failure polling (8 ms interval)
  d) `TickDegradationMachine()` — fault-mask evaluation, state transitions
  e) `kPtpConverging → kRunning` — GCL load + FRER registration on first gPTP lock
- **ASIL-D Degradation FSM**:
  — `kRunning → kDegraded`: FRER latent error, MACsec auth failures, GCL integrity mismatch
  — `kRunning/kDegraded → kSafeState`: PTP holdover expired, Network Calculus deadline exceeded
  — Safe state: `cfg_.safeStateSchedule` applied on all ports (TC7-exclusive GCL)
  — `kAny → kFault`: unrecoverable HAL failure; all schedules disabled; requires hw reset
- **64-entry Circular Audit Log** (ISO 26262 §7.4.2): `AuditEntry[]` with monotonic timestamp,
  from/to states, trigger ErrorCode, affected port/stream
- `UpdatePortSchedule()` — runtime GCL update with full 7-stage re-validation
- `GetHealthMetrics()` — consolidated: PTP offset, fault mask, total drops, MACsec failures

---

### Added — Tests

#### `test_GateControlManager.cpp` — 28 GoogleTest Cases
- `MockSwitchHal` with failure injection (`failApplyGcl`, `failReadOperGcl`, `reportSwapPending`)
- Full MC/DC coverage on all 7 validation stages
- Guard band boundary conditions: exact minimum, below minimum, above minimum
- Network Calculus result availability before/after `ApplySchedule()`
- HAL interaction: call count, port ID, failure isolation (validation failure → no HAL call)
- State management: `GetActiveSchedule()` before/after, `DisableSchedule()` effect
- Hardware integrity: `VerifyHardwareSchedule()` match, HAL read failure, before-apply guard
- Successive updates: second schedule overwrites first correctly

---

### Added — Build System & CI

#### `CMakeLists.txt` — CMake Build Configuration
- `norxs-tsn-backbone` static library target with AUTOSAR flags: `-fno-exceptions -fno-rtti -fstack-usage`
- `norxs-tsn-tests` GoogleTest binary with `gtest_discover_tests`
- `NORXS_BUILD_TESTS`, `NORXS_ENABLE_COVERAGE`, `NORXS_STRICT_WARNINGS` options
- Post-build stack usage report for ISO 26262 Part 6 §9.4.3 verification
- CMake install rules with namespace `norxs::` for downstream consumption

#### `cmake/Toolchain-aarch64-imx8x.cmake` — NXP i.MX8X Cross-Compilation
- Cortex-A35 arch flags: `-mcpu=cortex-a35 -mtune=cortex-a35 -march=armv8-a+crc`
- `aarch64-poky-linux-gcc` toolchain with NXP Yocto sysroot support
- `NORXS_SYSROOT` env var override for CI environments

#### `.github/workflows/ci.yml` — 5-Job CI Pipeline
- **Job 1** Build + GoogleTest (x86_64 host)
- **Job 2** Cross-compile aarch64, `objdump` architecture verification
- **Job 3** Stack usage analysis, ASIL-D enforcement: no function > 1024 B
- **Job 4** `clang-tidy` static analysis: `bugprone-*`, `cert-*`, `cppcoreguidelines-*`
- **Job 5** AUTOSAR compliance pattern scan (13 forbidden patterns) + Doxygen header check

---

[1.0.0]: https://github.com/norxs-tech/tsn-zonal-backbone/releases/tag/v1.0.0
