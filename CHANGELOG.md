# Changelog

All notable changes to the **norxs Deterministic TSN Zonal Backbone** are documented here.
This project follows [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

*(Changes staged for the next release go here)*

---

## [1.1.0] — 2026-06-12

### Fixed — Resolves both open observations from v1.0.1

- **OBS-001 (Medium):** The gPTP convergence watchdog could never fire in slave
  mode before the first sync, because `Tick()` derived time exclusively from the
  PHC, which is unreadable in `kFreeRun`. The orchestrator now maintains a
  **PHC-independent monotonic clock** derived from the `Tick()` period contract
  (`monotonicNs_ += tickPeriodUs × 1000` per tick) and drives the convergence
  watchdog from it. A slave that never receives Sync now correctly enters
  `kFault` with `kPtpNotSynchronised` after `ptpConvergenceTimeoutMs`.
  Regression tests: `OrchTest.Tick_SlaveNeverSyncs_ConvergenceWatchdogEntersFault`,
  `OrchTest.Tick_GrandmasterLocksBeforeTimeout_NoSpuriousFault`.
- **OBS-002 (Low):** `ScheduleParams::entryCount` widened `u8` → `u16` so the
  documented [1, 256] range — and the full SJA1110/88Q5050 GCL SRAM depth — is
  representable. All iteration variables widened accordingly. **API change**
  (struct layout) — hence the minor version bump per SemVer.
  Regression tests: `OrchTest.Init_Full256EntryGcl_PassesPrevalidation`,
  `OrchTest.Init_EntryCountAboveMax_RejectedAsGclEmpty`.

### Added — kDegraded → kSafeState escalation (previously documented, not implemented)

- The FSM documentation promised `kDegraded → kSafeState` when the ASIL-D
  deadline can no longer be guaranteed, but no code path ever set
  `kFaultNcDeadline` — the safe state was unreachable. Implemented: if the
  hardware-schedule integrity fault (`VerifyHardwareSchedule()` mismatch)
  **persists while degraded**, the Network Calculus guarantees proven against
  the validated schedule are void for the operative GCL, so the orchestrator
  escalates to `kSafeState` with trigger `kDeadlineViolation` and re-applies the
  TC7-exclusive safe-state schedule on all ports.
  Tests: `OrchTest.Tick_PersistentGclIntegrityFault_EscalatesToSafeState`,
  `OrchTest.AuditLog_SafeStateEntry_RecordsDeadlineViolationTrigger`.

### Added — Verification depth

- **Phase 0 configuration validation** in `Init()`: rejects `tickPeriodUs == 0`
  (would disable the monotonic watchdog) and `schedulePortCount > kMaxSwitchPorts`
  with `kInvalidArgument`, before any hardware is touched.
- **5 holdover runtime-path tests** (`PtpTest`): sync-silence → holdover entry,
  TCXO drift-compensation PHC writes, budget expiry → `kFreeRun` +
  `kPtpHoldoverExpired`, 10× GM-silence detection, and re-sync recovery —
  previously a fully uncovered ASIL-D continuity mechanism.
- **Measured structural coverage (gcov, host):** 96.1% lines overall —
  PtpClockManager 100.0%, TsnOrchestrator 97.9%, GateControlManager 96.0%,
  FrerManager 88.5%. Per-module branch data in the test report.
- Total suite: **113 tests, 113 passing** (GcmTest 26, PtpTest 27, FrerTest 21,
  OrchTest 39). Report: `docs/test_reports/unit_test_report_v1.1.0.md`.

### Added — Demonstration & tooling

- `examples/zonal_gateway_demo.cpp` + `-DNORXS_BUILD_EXAMPLES=ON` CMake target:
  runnable end-to-end lifecycle demo (Init → kRunning → fault injection →
  kDegraded → kSafeState → audit-trail dump) against a simulated switch ASIC.
- `.github/workflows/codeql.yml` — CodeQL semantic security analysis
  (push/PR + weekly), per ISO/IEC 18974 §3.1.5.
- CI Job 8 — Doxygen API documentation build; blocks on undocumented public API;
  publishes the HTML as a build artifact.
- `.clang-format` and `.clang-tidy` — local profiles mirroring CI.
- `.github/CODEOWNERS` — dual review (safety + security) on production code;
  OSPO ownership of compliance artifacts.
- `CODE_OF_CONDUCT.md`; `feature_request` issue template with standards-impact
  checklist; issue `config.yml` routing security reports away from public issues.

### Changed

- Project version 1.0.1 → 1.1.0; SBOM regenerated as
  `sbom/tsn-zonal-backbone-1.1.0.spdx.json` (CI version-sync gate updated).

---

## [1.0.1] — 2026-06-12

### Fixed

- **DEF-001 (Critical):** `TsnOrchestrator::PrevalidateAllSchedules()` narrowed
  `kMaxGclEntries` (256) to `u8`, truncating the bound to 0 — every non-empty
  schedule was rejected with `kGclEmpty` and `Init()` could never succeed. The
  comparison is now widened to `std::size_t`, matching the correct pattern in
  `GateControlManager.cpp`. Covered by regression test
  `OrchTest.Init_ValidConfig_ReturnsOk_StateInitializing`.
- **DEF-002 (Minor):** `TsnOrchestrator::GetHealthMetrics()` now aggregates
  per-stream 802.1Qci drop counters as documented (previously always returned 0).
  Covered by `OrchTest.GetHealthMetrics_AggregatesMacsecAndDropCounters`.

### Added — Verification

- `tests/test_TsnOrchestrator.cpp` — 28 new unit tests for the previously untested
  master coordinator: 8-phase Init fail-fast semantics, Start/Stop lifecycle,
  kPtpConverging → kRunning transition (BMCA self-election path), ASIL-D
  degradation FSM (kRunning → kDegraded on MACsec auth failures; kFault latch),
  circular audit log content (ISO 26262 §7.4.2), runtime schedule update guards,
  and consolidated health metrics. Total suite: **97 tests, 97 passing**.
- `docs/test_reports/unit_test_report_v1.0.1.md` — formal unit test report
  (environment, per-suite results, defect log, open observations) plus the raw
  execution log `unit_test_log_v1.0.1.txt`.

### Added — Compliance (OpenChain ISO/IEC 5230 · ISO/IEC 18974 · NIST CSF 2.0)

- `docs/compliance/openchain-iso5230.md` — open source license compliance program
  documentation (OpenChain self-certification route).
- `docs/compliance/openchain-iso18974.md` — security assurance program
  documentation: known-vulnerability detection, intake SLAs, standard practices.
- `docs/compliance/nist-csf-mapping.md` — NIST CSF 2.0 mapping across all six
  functions, at both the process layer and the product layer.
- `sbom/tsn-zonal-backbone-1.0.1.spdx.json` — SPDX 2.3 Software Bill of Materials
  (zero third-party runtime dependencies; GoogleTest declared test-scope only).
- `NOTICE` — third-party attribution file; `LICENSES/LicenseRef-norxs-RI-1.0.txt`
  — REUSE-style license text directory.
- CI Job 6 — SBOM validation, version-sync check, compliance artifact presence,
  and a machine-enforced zero-third-party-includes gate on production code.
- CI Job 7 — cppcheck security static analysis (blocking on errors), complementing
  clang-tidy `cert-*`/`bugprone-*` (ISO/IEC 18974 §3.1.5).
- `.github/dependabot.yml` — weekly CI toolchain vulnerability/freshness monitoring.
- Pull request template extended with an ISO/IEC 5230 / 18974 compliance checklist.
- `SECURITY.md` extended with the Security Assurance Program section (SBOM link,
  monitoring, NIST CSF mapping reference).

### Changed

- `README.md`: corrected the test inventory (the structure listing previously
  described only `test_GateControlManager.cpp` with an inaccurate case count),
  added the Verification & Test Evidence and Compliance Programs sections, and
  refreshed badges.
- `CMakeLists.txt`: project version 1.0.0 → 1.0.1; orchestrator tests added to the
  `norxs-tsn-tests` target.

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

#### `test_GateControlManager.cpp` — 26 GoogleTest Cases
*(count corrected in v1.0.1 — originally misstated as 28; the 1.0.0 release also shipped `test_PtpClockManager.cpp` (22 cases) and `test_FrerManager.cpp` (21 cases), omitted from this entry)*
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
