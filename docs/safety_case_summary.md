# Safety Case Summary — norxs Deterministic TSN Zonal Backbone

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

**Document ID:** NORXS-TSN-SC-001 | **Version:** 1.0 | **Date:** 2026-06-02
**ASIL Level:** D | **Standard:** ISO 26262:2018 Parts 4, 6, 8

> **Note:** This document is a **summary** of the safety case for the reference
> implementation.  The full safety case (HARA, FSC, HSI, FMEA, FTA, DFA, Safety Case
> compilation) is available to customers under commercial licence and NDA.
> Contact: contact@norxs.com | https://norxs.com

---

## 1. Safety Goal

**SG-01:** The TSN backbone shall guarantee that Brake-by-Wire (BbW) network frames
are delivered end-to-end within **500 µs** with **zero frame loss** under all
single-point and latent fault conditions, in compliance with ISO 26262 ASIL-D.

| Attribute | Value |
|-----------|-------|
| ASIL | D |
| FTTI | < 5 ms (assumed; verify per vehicle-level HARA) |
| Safe State | TC7-exclusive GCL active; all non-safety queues gated out |
| Emergency Operation Interval | Duration of `holdoverBudgetNs` (configurable; default 100 ms) |

---

## 2. Hazard and Risk Analysis Summary

| Hazard ID | Hazard | ASIL | Safety Measure |
|-----------|--------|------|----------------|
| H-01 | BbW frame delayed > 500 µs due to non-safety traffic preemption | D | Network Calculus Stage 7 deadline verification |
| H-02 | BbW frame lost due to port buffer overflow | D | TC7-exclusive gate window (Stage 6) + guard band (Stage 5) |
| H-03 | Schedule applied before gPTP synchronisation | D | `kPtpConverging` state; GCL not loaded until clock locked |
| H-04 | Incorrect GCL applied due to Admin→Oper race | D | IEEE 802.1Qbv §8.6.9.4 atomic swap; ConfigPending polling |
| H-05 | Silent path failure causing single-path dependency | D | FRER Latent Error Detection §7.4.5; kDegraded transition |
| H-06 | GCL corrupted after load (hardware soft error) | D | `VerifyHardwareSchedule()` every 8 ms; kFault on mismatch |
| H-07 | Network attack spoofing gPTP → schedule disruption | D | MACsec AES-GCM-128 authentication on all management traffic |
| H-08 | gPTP grandmaster failure → clock drift → missed deadline | D | Holdover mode + TCXO compensation; kSafeState on expiry |

---

## 3. Safety Architecture

### 3.1 Software Safety Requirements Allocation

| SW Safety Requirement | Implemented In | Verification |
|-----------------------|----------------|-------------|
| SSR-01: GCL interval sum shall equal cycle time | `GateControlManager::CheckIntervalSum()` (Stage 4) | Unit test `Stage4_SumMismatch_Rejected` |
| SSR-02: TC7 exclusive window width ≥ guard band | `GateControlManager::CheckGuardBand()` (Stage 5) | Unit test `Stage5_TC7WindowTooNarrow_GuardBandViolation` |
| SSR-03: TC7 exclusive window mandatory | `GateControlManager::CheckSafetyCriticalWindow()` (Stage 6) | Unit test `Stage6_NoExclusiveTC7Window_Rejected` |
| SSR-04: WCD[TC7] ≤ 500 µs | `GateControlManager::RunNetworkCalculus()` (Stage 7) | Unit test `Stage7_ValidSchedule_DeadlineMet` |
| SSR-05: Hardware schedule integrity | `GateControlManager::VerifyHardwareSchedule()` | Unit test `VerifyHardwareSchedule_MatchingHW_ReturnsOk` |
| SSR-06: Clock locked before GCL activation | `TsnOrchestrator::TickPtp()` + `kPtpConverging` state | Integration test (see §5) |
| SSR-07: Safe state on holdover expiry | `TsnOrchestrator::EnterSafeState(kPtpHoldoverExpired)` | Unit test (see §5) |
| SSR-08: FRER latent error → kDegraded | `TsnOrchestrator::TickDegradationMachine()` | Unit test `FrerTest::Tick_*` |
| SSR-09: No dynamic memory post-init | AUTOSAR M18-4-1 enforcement | CI Job 5 compliance scan |
| SSR-10: No exceptions | AUTOSAR M15-3-1 enforcement | CI Job 5 compliance scan |
| SSR-11: Stack depth ≤ 1024 B/function | `-fstack-usage` compilation flag | CI Job 3 enforcement |

---

## 4. Functional Safety Concept

### 4.1 Degradation State Machine

```
          External fault
          (HAL, PTP, FRER)
                │
kRunning ───────┼───────────────────────────────► kFault
    │           │  HAL unrecoverable              (requires hw reset)
    │     ┌─────▼──────────────┐
    │     │ kDegraded          │
    │     │ (FRER path fail,   │──► kSafeState ──► kFault
    │     │  MACsec auth fail) │    (PTP holdover  (manual reset)
    │     └────────────────────┘     expired)
    │
    └── Recovery: all faults cleared → kRunning
```

### 4.2 Safe State Definition (SSR-07)

When `kSafeState` is entered, the following actions are taken atomically:

1. `cfg_.safeStateSchedule` is applied on ALL switch ports via `ApplyGclToPort()`
2. The safe-state schedule contains only one GCL entry type: `gateStates = 0x80` (TC7 only)
3. All non-safety queues (TC0–TC6) are permanently gated closed until manual reset
4. `AuditEntry` is written with monotonic timestamp, trigger code, and state transition

**Safe state is not recoverable without a full stack re-initialisation (`Init() → Start()`).**
This is intentional per ISO 26262 Part 4 §6.4.6 — safe state shall be stable.

---

## 5. Independence and Freedom from Interference

### 5.1 Software Partitioning

| Component | ASIL | Partitioning Measure |
|-----------|------|---------------------|
| `GateControlManager` (validation) | D | No shared mutable state with other managers |
| `PtpClockManager` (servo) | D | Accesses only PHC registers; no shared variables |
| `FrerManager` (path monitoring) | D | Static arrays per stream; no heap |
| `TsnOrchestrator` (coordination) | D | Serialises all sub-manager access in Tick() |
| `SwitchHal` (HAL) | D | Pure-virtual; concrete implementation isolated |

### 5.2 Freedom from Interference (FfI)

Per ISO 26262 Part 6 §7.4.7:

- **Memory:** No dynamic allocation. Static arrays prevent heap corruption contamination.
- **Execution time:** 125 µs Tick() bounded by stack analysis (CI Job 3). No recursion.
- **Control flow:** `TSN_RETURN_IF_ERR` provides deterministic error propagation; no exception unwinding.
- **Shared resources:** HAL access serialised by single-threaded Tick() loop (SCHED_FIFO).

---

## 6. Dependent Failure Analysis Summary

| Common Cause Failure | Effect | Mitigation |
|---------------------|--------|------------|
| Power supply failure (all ECUs) | Total network loss | Out of scope (vehicle architecture) |
| Single PHY failure | Path A or B down | FRER dual-path; zero recovery time |
| GM clock failure | gPTP holdover | Holdover + TCXO compensation; safe state on expiry |
| Switch ASIC hardware fault | GCL corruption | `VerifyHardwareSchedule()` every 8 ms |
| Firmware update (OTA) | Schedule interrupted | UN R156 update orchestration (out of scope) |

---

## 7. Hardware Safety Metrics (summary)

The following metrics apply to the **software** component of this stack.
Hardware metrics (SPFM, LFM) are computed by the vehicle-level safety integrator
per ISO 26262 Part 5 §8.

| Metric | Contribution of this Software |
|--------|------------------------------|
| Single-Point Fault Mitigation | GCL integrity check, FRER LED, MACsec auth monitoring |
| Latent Fault Detection | FRER LED (IEEE 802.1CB §7.4.5), holdover expiry |
| Diagnostic Coverage | VerifyHardwareSchedule() + Network Calculus Stage 7 |

---

## 8. Tool Confidence Level

| Tool | Version | TCL | Qualification Method |
|------|---------|-----|---------------------|
| GCC | ≥ 11.0 | TCL-2 | ISO 26262 Part 8 §11.4.7 — established tool |
| CMake | ≥ 3.21 | TCL-1 | Not safety-critical; build automation only |
| GoogleTest | ≥ 1.13 | TCL-2 | Test framework; output reviewed by engineer |
| clang-tidy | ≥ 14 | TCL-2 | Static analysis; reports reviewed by engineer |

---

## 9. Verification Summary

| Verification Activity | Method | Status |
|----------------------|--------|--------|
| Unit testing | GoogleTest (28+ cases per module, MC/DC) | ✅ CI Job 1 |
| Static analysis | clang-tidy (`bugprone-*`, `cert-*`, `cppcoreguidelines-*`) | ✅ CI Job 4 |
| AUTOSAR compliance | Pattern scan (13 forbidden patterns) | ✅ CI Job 5 |
| Stack depth | `-fstack-usage` ≤ 1024 B/function | ✅ CI Job 3 |
| Cross-compilation | aarch64-poky-linux-g++ for NXP i.MX8X | ✅ CI Job 2 |
| Doxygen completeness | All public APIs documented | ✅ CI Job 5 |
| Network Calculus verification | Stage 7 WCD ≤ 500 µs for every schedule | ✅ Per `ApplySchedule()` call |

---

## 10. Full Safety Evidence Package

The following documents are available to commercial customers under NDA:

| Document | Description |
|----------|-------------|
| NORXS-TSN-HARA-001 | Hazard Analysis and Risk Assessment |
| NORXS-TSN-FSC-001 | Functional Safety Concept |
| NORXS-TSN-SSC-001 | Software Safety Concept |
| NORXS-TSN-SSR-001 | Software Safety Requirements |
| NORXS-TSN-HSI-001 | Hardware-Software Interface Specification |
| NORXS-TSN-FMEA-001 | Failure Mode and Effects Analysis |
| NORXS-TSN-FTA-001 | Fault Tree Analysis |
| NORXS-TSN-DFA-001 | Dependent Failure Analysis |
| NORXS-TSN-UNIT-001 | Unit Test Specification and Results |
| NORXS-TSN-SC-001 | Safety Case (this document — summary only) |

All engagements conducted under NDA.

---

*(c) 2026 norxs Technology LLC. All rights reserved.*
*TÜV-certified Functional Safety Expert (ISO 26262 Expert Level)*
*Automotive Cyber-Security Professional (ISO/SAE 21434 · UN R155)*
