# Unit Test Report — norxs Deterministic TSN Zonal Backbone v1.1.0

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

| | |
|---|---|
| **Report ID** | NTR-UT-2026-002 (supersedes NTR-UT-2026-001) |
| **Software under test** | norxs-tsn-zonal-backbone v1.1.0 (static library, 4 translation units) |
| **Test framework** | GoogleTest-compatible harness (`TEST_F` fixtures, EXPECT/ASSERT macros) |
| **Standard reference** | ISO 26262-6:2018 §9.4 (unit verification), §10 (testing) |
| **Raw execution log** | [`unit_test_log_v1.1.0.txt`](unit_test_log_v1.1.0.txt) |

> **Independence note.** Host-based (off-target) unit verification. Coverage in §3
> is gcov line/branch measurement on the host toolchain — it is *not* MC/DC
> measurement on the qualified production toolchain. Target verification, MC/DC
> evidence, and tool qualification (ISO 26262-8 §11) are part of the commercial
> safety evidence package.

---

## 1. Test Environment

| Item | Value |
|------|-------|
| Host | x86_64 Linux (Ubuntu 24.04) |
| Compiler | GCC 13.3.0, `-std=c++14 -fno-exceptions -fno-rtti` |
| Library build flags | `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wundef -Werror -O2` |
| Build result | **0 warnings, 0 errors** with `-Werror` |
| Coverage build | `--coverage -O0 -g`, measured with gcov (GCC 13.3.0) |
| Test isolation | Mock `SwitchHal` per fixture; no hardware, network, or filesystem |

## 2. Summary

| Suite | File | Tests | Passed | Failed |
|-------|------|------:|-------:|-------:|
| `GcmTest` — GateControlManager | `tests/test_GateControlManager.cpp` | 26 | 26 | 0 |
| `PtpTest` — PtpClockManager | `tests/test_PtpClockManager.cpp` | 27 | 27 | 0 |
| `FrerTest` — FrerManager | `tests/test_FrerManager.cpp` | 21 | 21 | 0 |
| `OrchTest` — TsnOrchestrator | `tests/test_TsnOrchestrator.cpp` | 39 | 39 | 0 |
| **Total** | | **113** | **113** | **0** |

Exit status: `0`.

## 3. Measured Structural Coverage (gcov, host)

| Translation unit | Lines | Line coverage | Branches | Taken ≥ once |
|------------------|------:|--------------:|---------:|-------------:|
| `PtpClockManager.cpp` | 140 | **100.00%** | 76 | 77.63% |
| `TsnOrchestrator.cpp` | 234 | **97.86%** | 168 | 77.38% |
| `GateControlManager.cpp` | 124 | **95.97%** | 80 | 91.25% |
| `FrerManager.cpp` | 122 | **88.52%** | 52 | 71.15% |
| **Overall** | **620** | **96.13%** | — | — |

Residual uncovered code is concentrated in the FRER per-path silence-recovery
loop and latent-error reset path (`FrerManager.cpp` §Tick), which require
multi-stream frame-timing scenarios scheduled for integration-level testing.

## 4. Coverage of Safety Mechanisms

All v1.0.1 mechanism coverage is retained (7-stage GCL pipeline, guard bands,
Network Calculus deadlines, PI servo clamp, FRER duplicate elimination/LED,
Init fail-fast, degradation to kDegraded, kFault latch, audit log). Newly
verified in this release:

| Safety mechanism | Verified by |
|------------------|-------------|
| gPTP convergence watchdog fires **without a readable PHC** (slave, pre-sync) | `OrchTest.Tick_SlaveNeverSyncs_ConvergenceWatchdogEntersFault` |
| No spurious watchdog fault when GM locks in time | `OrchTest.Tick_GrandmasterLocksBeforeTimeout_NoSpuriousFault` |
| **kDegraded → kSafeState** escalation on persistent GCL integrity fault; TC7-exclusive schedule re-applied to all ports | `OrchTest.Tick_PersistentGclIntegrityFault_EscalatesToSafeState` |
| SafeState audit entry carries `kDeadlineViolation` trigger (ISO 26262 §7.4.2) | `OrchTest.AuditLog_SafeStateEntry_RecordsDeadlineViolationTrigger` |
| Holdover entry on GM silence > syncTimeout (802.1AS continuity) | `PtpTest.Tick_SyncSilenceBeyondTimeout_EntersHoldover` |
| TCXO drift compensation writes PHC while in budget | `PtpTest.Tick_InHoldover_AppliesDriftCompensationWrite` |
| Holdover budget expiry → `kFreeRun` + `kPtpHoldoverExpired` | `PtpTest.Tick_HoldoverBudgetExpired_FreeRunWithError` |
| 10× sync-timeout GM-lost detection inside holdover | `PtpTest.Tick_HoldoverGmSilenceTenfoldTimeout_ReportsExpired` |
| Re-sync during holdover recovers toward lock | `PtpTest.Tick_ReSyncDuringHoldover_RecoversTowardsLock` |
| Full 256-entry GCL representable and accepted (OBS-002) | `OrchTest.Init_Full256EntryGcl_PassesPrevalidation` |
| Phase 0 config validation (zero tick period, port count bound) | `OrchTest.Init_ZeroTickPeriod_*`, `Init_PortCountAboveMax_*` |

## 5. Defect / Observation Disposition

| ID | v1.0.1 status | v1.1.0 disposition |
|----|---------------|--------------------|
| DEF-001 (Critical, prevalidation truncation) | Fixed | Regression test retained — passing |
| DEF-002 (Minor, drop-counter aggregation) | Fixed | Regression test retained — passing |
| OBS-001 (Medium, watchdog dead in slave mode) | Open | **Fixed** — PHC-independent monotonic clock; 2 regression tests |
| OBS-002 (Low, entryCount u8 vs documented 256) | Open | **Fixed** — widened to u16; 2 regression tests; API change → 1.1.0 |
| GAP-001 (new) | — | **Fixed** — documented kDegraded → kSafeState transition was unimplemented (`kFaultNcDeadline` never set; safe state unreachable). Implemented as persistent-integrity-fault escalation; 2 tests |

## 6. Open Observations

| ID | Severity | Observation |
|----|----------|-------------|
| OBS-003 | Low | Degraded-state faults (`kFaultFrerLatent`, `kFaultMacsecAuth`, `kFaultHalIntegrity`) are latched by design — there is no automatic kDegraded → kRunning recovery even though the FSM documentation sketches one ("all paths restored"). Rationale: security-relevant events (MACsec auth failures) should not self-clear; recovery is an operator/diagnostic-session decision. Either implement guarded auto-recovery for the non-security faults or align the FSM documentation. Tracked for v1.2. |
| OBS-004 | Info | FRER silence-recovery and LED-reset paths (≈11% of `FrerManager.cpp`) need frame-timing scenarios beyond unit scope; planned for the HIL integration campaign. |

## 7. Verdict

All 113 unit tests **PASS**; measured line coverage 96.13% with PtpClockManager
at 100%. Both observations open from v1.0.1 are resolved with regression tests,
and one documented-but-unimplemented FSM transition (GAP-001) was implemented
and verified. OBS-003/OBS-004 are tracked for the next cycle.

---
*(c) 2026 norxs Technology LLC. All rights reserved.*
