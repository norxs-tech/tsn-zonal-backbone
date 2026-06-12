# Unit Test Report — norxs Deterministic TSN Zonal Backbone v1.0.1

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

| | |
|---|---|
| **Report ID** | NTR-UT-2026-001 |
| **Software under test** | norxs-tsn-zonal-backbone v1.0.1 (static library, 4 translation units) |
| **Test framework** | GoogleTest-compatible harness (`TEST_F` fixtures, EXPECT/ASSERT macros) |
| **Standard reference** | ISO 26262-6:2018 §9.4 (unit verification), §10 (testing) |
| **Raw execution log** | [`unit_test_log_v1.0.1.txt`](unit_test_log_v1.0.1.txt) |

> **Independence note.** This report documents a host-based (off-target) unit test
> execution. It is *one* element of the ISO 26262 verification argument and does not
> by itself constitute target verification, MC/DC structural coverage measurement on
> the production toolchain, or tool qualification (ISO 26262-8 §11). Those artifacts
> are part of the commercial safety evidence package.

---

## 1. Test Environment

| Item | Value |
|------|-------|
| Host | x86_64 Linux (Ubuntu 24.04) |
| Compiler | GCC 13.3.0, `-std=c++14 -fno-exceptions -fno-rtti` |
| Library build flags | `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef -Werror -O2` |
| Build result | **0 warnings, 0 errors** with `-Werror` |
| Test isolation | Mock `SwitchHal` per fixture; no hardware, no network, no filesystem |

## 2. Summary

| Suite | File | Tests | Passed | Failed |
|-------|------|------:|-------:|-------:|
| `GcmTest` — GateControlManager | `tests/test_GateControlManager.cpp` | 26 | 26 | 0 |
| `PtpTest` — PtpClockManager | `tests/test_PtpClockManager.cpp` | 22 | 22 | 0 |
| `FrerTest` — FrerManager | `tests/test_FrerManager.cpp` | 21 | 21 | 0 |
| `OrchTest` — TsnOrchestrator | `tests/test_TsnOrchestrator.cpp` | 28 | 28 | 0 |
| **Total** | | **97** | **97** | **0** |

Exit status: `0`. Total wall time: < 1 ms (pure in-memory logic, no I/O).

## 3. Coverage of Safety Mechanisms

| Safety mechanism (ISO 26262 reference) | Verified by |
|----------------------------------------|-------------|
| 7-stage GCL validation pipeline (Part 6 §9.4) | `GcmTest.Stage1_*` … `Stage7_*` |
| ASIL-D guard band ≥ frame-drain bound (IEEE 802.1Qbv §8.6.8.4) | `GcmTest` Stage 5 cases |
| Network Calculus WCD ≤ 500 µs deadline (Part 4 §6.4) | `GcmTest` Stage 7 cases |
| gPTP PI servo clamp ±500 000 ppb | `PtpTest.InjectSync_LargeOffset_AdjustmentClamped` |
| Holdover entry/expiry state machine | `PtpTest` holdover cases |
| FRER O(1) duplicate elimination (802.1CB §10.4) | `FrerTest` duplicate cases |
| FRER latent error detection (802.1CB §7.4.5) | `FrerTest` LED cases |
| 8-phase Init fail-fast, no partial hardware state | `OrchTest.Init_*` |
| kPtpConverging → kRunning transition + GCL load | `OrchTest.Tick_ReachesRunning_*` |
| Degradation FSM: kRunning → kDegraded on MACsec auth failure (UN R155) | `OrchTest.Tick_MacsecAuthFailures_DegradesToDegraded` |
| kFault latch: Tick rejected with `kOperationNotPermitted` | `OrchTest.Tick_InFaultState_*` |
| Circular audit log records transitions + trigger codes (§7.4.2) | `OrchTest.AuditLog_*` |
| Runtime schedule update only permitted in kRunning | `OrchTest.UpdatePortSchedule_*` |

## 4. Defects Found and Resolved in This Cycle

| ID | Severity | Description | Resolution |
|----|----------|-------------|------------|
| DEF-001 | **Critical** | `TsnOrchestrator::PrevalidateAllSchedules()` narrowed `kMaxGclEntries` (256) to `u8`, truncating to 0. The comparison `entryCount > 0` therefore rejected **every non-empty schedule**, making `Init()` permanently fail with `kGclEmpty`. The orchestrator could never start. Latent because TsnOrchestrator previously had no unit tests. | Widened comparison to `std::size_t` (matching the correct pattern already used in `GateControlManager.cpp`). Regression test: `OrchTest.Init_ValidConfig_ReturnsOk_StateInitializing`. |
| DEF-002 | Minor | `TsnOrchestrator::GetHealthMetrics()` documented aggregation of per-stream drop counters but always returned `outDropTotal = 0`. | Implemented aggregation over configured stream filters. Regression test: `OrchTest.GetHealthMetrics_AggregatesMacsecAndDropCounters`. |

## 5. Open Observations (not fixed in this cycle)

| ID | Severity | Observation |
|----|----------|-------------|
| OBS-001 | Medium | **gPTP convergence timeout cannot trip in slave mode before first sync.** `TsnOrchestrator::Tick()` derives monotonic time from `PtpClockManager::GetCurrentTime()`, which returns an error while the clock is in `kFreeRun`. A slave node that never receives a Sync therefore reads `nowNs = 0` forever, and the `ptpConvergenceTimeoutMs` watchdog never fires — the orchestrator can remain in `kPtpConverging` indefinitely instead of entering `kFault`. Recommendation: source the watchdog from a PHC-independent monotonic clock (e.g., a `SwitchHal::GetMonotonicNs()` extension or `CLOCK_MONOTONIC`). Tracked for v1.1. |
| OBS-002 | Low | `ScheduleParams::entryCount` is `u8` but its documentation states a valid range of [1, 256]; a full 256-entry GCL is unrepresentable. Either widen to `u16` or document the effective limit as 255. |

## 6. Verdict

All 97 unit tests **PASS** on the host configuration above. The two defects found
during test development were fixed and are covered by regression tests. OBS-001 and
OBS-002 are tracked as open items for the next minor release.

---
*(c) 2026 norxs Technology LLC. All rights reserved.*
