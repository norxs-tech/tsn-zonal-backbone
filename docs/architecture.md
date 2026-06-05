# Architecture Deep Dive — norxs Deterministic TSN Zonal Backbone

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

---

## Table of Contents

1. [Problem Statement](#1-problem-statement)
2. [Target Hardware Platform](#2-target-hardware-platform)
3. [Network Architecture](#3-network-architecture)
4. [Software Stack Design Principles](#4-software-stack-design-principles)
5. [Module Architecture](#5-module-architecture)
6. [IEEE 802.1Qbv Gate Control — 7-Stage Validation Pipeline](#6-ieee-8021qbv-gate-control--7-stage-validation-pipeline)
7. [Network Calculus Analysis Engine](#7-network-calculus-analysis-engine)
8. [gPTP Clock Synchronisation](#8-gptp-clock-synchronisation)
9. [IEEE 802.1CB FRER — Zero-Recovery-Time Redundancy](#9-ieee-8021cb-frer--zero-recovery-time-redundancy)
10. [ASIL-D Degradation State Machine](#10-asil-d-degradation-state-machine)
11. [Memory Model](#11-memory-model)
12. [125 µs Real-Time Scheduling Loop](#12-125-µs-real-time-scheduling-loop)
13. [Admin→Oper GCL Swap Semantics](#13-adminoper-gcl-swap-semantics)
14. [Standards Compliance Matrix](#14-standards-compliance-matrix)

---

## 1. Problem Statement

Modern Software-Defined Vehicles (SDV) use Zonal Electrical/Electronic Architectures
in which a small number of powerful Zonal Gateway ECUs aggregate sensors, actuators,
and domain controllers previously connected via dedicated CAN/FlexRay buses.

The fundamental challenge is **deterministic co-existence**: the same 1000BASE-T1
Ethernet link must simultaneously carry:

| Traffic Class | Example | Latency Budget | Loss Tolerance |
|---------------|---------|---------------|----------------|
| TC7 — ASIL-D Safety | Brake-by-Wire, Steering | **≤ 500 µs** | **Zero** |
| TC5/TC6 — AVB | Surround-view video (60 fps) | ≤ 2 ms | < 1 frame |
| TC4 — Control | ADAS perception fusion | ≤ 5 ms | < 0.01% |
| TC0–TC3 — Best-effort | OTA, diagnostics, logging | ≤ 50 ms | Lossy OK |

Without active scheduling, a single 9 KB jumbo frame from a video stream introduces
72 µs of transmission time at 1 Gbps — 14% of the entire TC7 budget — making
deterministic co-existence impossible.

IEEE 802.1Qbv Time-Aware Scheduling (TAS) solves this via hardware gate control,
but existing open-source implementations lack:

- Formal mathematical worst-case delay verification (Network Calculus)
- ASIL-D guard band enforcement at the schedule validation layer
- Integrated FRER redundancy management with latent error detection
- gPTP holdover continuity for ASIL-D applications

This stack provides all four.

---

## 2. Target Hardware Platform

### Primary: NXP i.MX8X + SJA1110

```
NXP i.MX8X SoC
├── 4× Cortex-A35 @ 1.2 GHz (application cores)
│   └── TSN stack runs here (SCHED_FIFO, CPU-affinity core 3)
├── Cortex-M4 (safety monitor — ISO 26262 ASIL-B)
└── enet_qos (Ethernet DMA with IEEE 1588 hardware timestamps)
    └── MII/RMII → SJA1110

NXP SJA1110 (10-port automotive Ethernet switch)
├── 8× 1000BASE-T1 ports (OPEN Alliance TC10 sleep/wake)
├── 2× 2.5GBASE-T1 uplinks
├── 802.1Qbv TAS — 256 GCL entries per port
├── 802.1CB FRER — hardware sequence generation/recovery
├── 802.1Qci PSFS — per-stream token bucket (1024 filters)
├── 802.1AE MACsec — AES-GCM-128 line-rate @ 10 Gbps aggregate
└── Management: SPI (fast path) + MDIO (slow path)
```

### Alternate: Marvell 88Q5050

Drop-in HAL replacement. Same API surface, different register map.
`Q5050Hal` implements `SwitchHal` for Marvell targets.

---

## 3. Network Architecture

```
Physical Layer: IEEE 802.3bp 1000BASE-T1 (single twisted pair, automotive EMC)

┌─────────────────────────────────────────────────────────┐
│  Front Zonal Gateway                                     │
│  ┌────────────┐  ┌─────────────┐  ┌──────────────────┐  │
│  │  LiDAR MCU │  │  Radar MCU  │  │  Brake-by-Wire   │  │
│  │  TC4 · 50M │  │  TC4 · 10M  │  │  TC7 · 100 kbps  │  │
│  └──────┬─────┘  └──────┬──────┘  └────────┬─────────┘  │
│         └───────────────┴───────────────────┘            │
│                       1000BASE-T1                        │
│  ┌───────────────────────────────────────────────────┐   │
│  │  SJA1110 — 8-port switch                          │   │
│  │  ┌─ GCL Cycle = 1 ms ───────────────────────────┐ │   │
│  │  │ [guard 20µs][TC7 30µs][AVB 100µs][BE 850µs] │ │   │
│  │  └───────────────────────────────────────────────┘ │   │
│  └──────────────────────┬────────────────────────────┘   │
│                  1000BASE-T1 backbone link                 │
└──────────────────────────┼──────────────────────────────┘
                           │
┌──────────────────────────┼──────────────────────────────┐
│  Central Gateway (SDV backbone)                          │
│  ┌───────────────────────────────────────────────────┐   │
│  │  NXP i.MX8X  ·  norxs TSN Stack (this codebase)  │   │
│  └───────────────────────────────────────────────────┘   │
└──────────────────────────┼──────────────────────────────┘
                           │
┌──────────────────────────┼──────────────────────────────┐
│  Rear Zonal Gateway       │                              │
│  (Camera · Ultrasonic · Door actuators · HVAC)           │
└─────────────────────────────────────────────────────────┘
```

---

## 4. Software Stack Design Principles

### P1 — Exception-Free Error Monad

All functions return `Status` (`Result<void, ErrorCode>`) or `Result<T, ErrorCode>`.
No `throw`. No `catch`. The `TSN_RETURN_IF_ERR` macro propagates errors identically
to Rust's `?` operator, enabling clean error-chain composition without stack unwinding.

```cpp
Status GateControlManager::ApplySchedule(const ScheduleParams& p) noexcept {
    TSN_RETURN_IF_ERR(CheckEntryCount(p));      // Stage 1
    TSN_RETURN_IF_ERR(CheckCycleTime(p));       // Stage 2
    TSN_RETURN_IF_ERR(CheckNoZeroIntervals(p)); // Stage 3
    TSN_RETURN_IF_ERR(CheckIntervalSum(p));     // Stage 4
    TSN_RETURN_IF_ERR(CheckGuardBand(p));       // Stage 5
    TSN_RETURN_IF_ERR(CheckSafetyCriticalWindow(p)); // Stage 6
    TSN_RETURN_IF_ERR(RunNetworkCalculus(p));   // Stage 7
    return hal_.ApplyGclToPort(port_, p);
}
```

### P2 — Zero Dynamic Allocation

No `new`, `malloc`, or STL dynamic containers in production code.
Arrays are `std::array<T, N>` with compile-time bounds.
`GateControlManager` instances use placement-new into a static byte array
in `TsnOrchestrator` — a bounded, deterministic pattern acceptable under
AUTOSAR M18-4-1.

### P3 — Integer-Only Arithmetic

No `float` or `double` anywhere.  All timing computations are in nanoseconds
using `u64`.  The PI servo uses arithmetic right-shifts for Kp/Ki scaling.
Network Calculus uses `u64 × u64` with appropriate ordering to avoid overflow
at realistic cycle times (≤ 1 s = 10⁹ ns, well within u64 range).

### P4 — Hardware Isolation via Pure-Virtual HAL

All register accesses are routed through `SwitchHal`. Unit tests inject
`MockSwitchHal` with failure simulation. Production ships `Sja1110Hal` (not
included in this reference). This pattern enables full offline validation of
the scheduling logic before any hardware is available.

### P5 — ASIL-D by Architecture, Not by Annotation

Safety properties are enforced structurally:
- TC7 exclusive window is **mandatory** (Stage 6 rejects any schedule without it)
- Guard band is **mandatory** (Stage 5 rejects any schedule where TC7 window is too narrow)
- Network Calculus deadline is **mandatory** (Stage 7 rejects any schedule exceeding 500 µs WCD)
- Safe-state GCL is **pre-validated** and stored at init time — it is applied unconditionally
  in `EnterSafeState()` without re-running the validation pipeline

---

## 5. Module Architecture

```
TsnOrchestrator
│  ├── owns PtpClockManager  (value member)
│  ├── owns FrerManager      (value member)
│  ├── owns GateControlManager[N]  (placement-new in gcmStorage_[])
│  └── uses SwitchHal&       (injected reference)
│
GateControlManager
│  ├── uses SwitchHal&
│  └── owns NetworkCalculusResult  (value member, written by RunNetworkCalculus)
│
PtpClockManager
│  └── uses SwitchHal&  (PHC register access via enet_qos MDIO proxy)
│
FrerManager
│  └── uses SwitchHal&  (hardware FRER SRAM configuration)
│
SwitchHal  (pure virtual)
│  ├── Sja1110Hal  (production — not in this reference)
│  └── MockSwitchHal  (unit tests)
```

---

## 6. IEEE 802.1Qbv Gate Control — 7-Stage Validation Pipeline

Every call to `GateControlManager::ApplySchedule()` runs all 7 stages
**before any hardware is touched**.  A failure at any stage returns an error
and leaves the currently executing schedule undisturbed.

```
Stage 1 — EntryCount
  ├── entryCount ∈ [1, 256]
  └── Errors: kGclEmpty, kGclTooManyEntries

Stage 2 — CycleTime
  ├── cycleTimeNs ∈ [100 000, 1 000 000 000]
  └── Errors: kCycleTimeTooShort, kCycleTimeTooLong

Stage 3 — Zero Intervals
  ├── ∀ entry: intervalNs > 0
  └── Error: kZeroIntervalEntry

Stage 4 — Interval Sum
  ├── Σ(gcl[i].intervalNs) == cycleTimeNs  (u64, no FP)
  └── Error: kIntervalSumMismatch

Stage 5 — ASIL-D Guard Band  ← NEW (previously missing)
  ├── For each entry where gateStates == kSafetyOnlyOpen:
  │     intervalNs ≥ FrameNs(maxSafetyFrameBytes) + FrameNs(1538)
  │   = maxSafetyFrame×8 + 12 304 ns  at 1 Gbps
  └── Error: kGuardBandViolation

Stage 6 — Safety Critical Window
  ├── ∃ entry: gateStates == 0x80  (TC7 exclusively open)
  └── Error: kSafetyCriticalWindowAbsent

Stage 7 — Network Calculus  ← NEW (unique to this implementation)
  ├── For each TC i: WCD_i = T_i + B_i + Σ_{j>i} B_j
  ├── ASIL-D hard gate: WCD_TC7 ≤ 500 000 ns
  └── Error: kDeadlineViolation
```

### Guard Band Derivation (Stage 5)

IEEE 802.1Qbv §8.6.8.4 specifies that a gate must remain closed for at least
one maximum-frame transmission time before the TC7 window opens, to prevent
a large best-effort frame from overlapping with the safety window.

At 1000BASE-T1 (1 Gbps, 1 bit/ns):

```
guardBandNs = ceil(maxFrameSizeBytes × 8 bits × 1 ns/bit)
            = 1538 B × 8 = 12 304 ns
```

This means: the TC7 exclusive window must be at least:

```
minWindowNs = guardBandNs + FrameNs(maxSafetyFrameSizeBytes)
```

A schedule with a TC7 window of 10 000 ns would fail Stage 5 because
`10 000 < 12 304 + safetyFrameNs`.

---

## 7. Network Calculus Analysis Engine

### Theory: Latency-Rate (LR) Server Model

Reference: Le Boudec & Thiran, "Network Calculus: A Theory of Deterministic
Queuing Systems for the Internet", Springer LNCS 2050, 2001.

For a TAS-scheduled port, each Traffic Class is served by a Latency-Rate server:

```
Latency  L_i = time TC_i is blocked per cycle = Σ{ intervals where bit_i == 0 }
Rate     R_i = C × (open fraction)  where C = link capacity = 1 Gbps

For arrival curve (σ, ρ) and service curve (L, R):
  WCD_i ≤ L_i + σ_i / R_i + Σ_{j > priority(i)} σ_j / R
```

The implementation uses a conservative approximation suitable for automotive
worst-case analysis (IEC/TR 61907):

```
T_i   = Σ{ intervalNs : (gateStates & (1 << i)) == 0 }
burstNs_i = kDefaultBurstBytes[i] × 8                      (at 1 Gbps)
WCD_i = T_i + burstNs_i + Σ_{j > i} burstNs_j
```

### ASIL-D Contract

```
WCD_TC7 ≤ kSafetyDeadlineNs  (500 000 ns)

Typical example with 1 ms cycle:
  TC7 closed time T_7 = 970 000 ns  (970 µs, during BE + AVB slots)
  TC7 burst       B_7 = 1538 × 8 = 12 304 ns  (max Brake-by-Wire frame)
  Higher classes  j > 7: none

  WCD_TC7 = 970 000 + 12 304 = 982 304 ns  → 982 µs

  ⚠ This EXCEEDS the 500 µs deadline — the schedule would be rejected.

Corrected design: TC7 cycle must be short enough that T_7 ≤ 487 696 ns:
  cycleTime = 500 µs, TC7 window = 30 µs → T_7 ≈ 470 µs → WCD ≈ 482 µs ✓
```

This demonstrates why Network Calculus verification is **mandatory** for ASIL-D:
a geometrically valid schedule (all intervals sum correctly) can still violate
the safety timing contract.

---

## 8. gPTP Clock Synchronisation

### State Machine

```
kFreeRun ──────────────────────────────── (no synchronisation)
    │
    │  First Sync received (InjectSyncMessage)
    ▼
kLocking ──────────────────────────────── (servo running, |offset| > 1 µs)
    │
    │  |offset| < kPtpSyncTargetNs (1 µs) for one tick
    ▼
kLocked ───────────────────────────────── (fully synchronised)
    │                           ▲
    │  Sync silence > syncTimeoutNs        │ New Sync received
    ▼                           │
kHoldover ─────────────────────┘          (TCXO compensating)
    │
    │  holdoverUsedNs > holdoverBudgetNs
    ▼
kFreeRun  ──────────────────────────────── (kPtpHoldoverExpired → kFault)
```

### Integer PI Servo

```
offset_ns = (t2 + correction) - t1 - meanPathDelay
integral += offset_ns

// No FP: Kp = >>2 (0.25), Ki = >>5 (0.031)
adjustment_ppb = -(offset_ns >> 2) - (integral >> 5)
adjustment_ppb = clamp(adjustment_ppb, ±500 000)

// Write to i.MX8X enet_qos ENET_QOS_MAC_SYSTEM_TIME_NANOSECONDS_UPDATE
hal_.WriteRegister(kGlobalPort, kPhcFreqAdjReg, adjustment_ppb)
```

Servo convergence: < 100 ms from power-on to < 1 µs accuracy with TCXO oscillator.

### Holdover Drift Compensation

During holdover, the TCXO drifts at `kHoldoverDriftNsPerSec` (1 µs/s).
Every tick, a correction is applied:

```
driftNs = (driftRate × elapsedNs) / 1_000_000_000
         = (1000 ns/s × elapsedNs ns) / 10^9
         → integer nanoseconds added to PHC via update register
```

This maintains TAS baseTime validity for up to `holdoverBudgetNs` without a GM.

---

## 9. IEEE 802.1CB FRER — Zero-Recovery-Time Redundancy

### Concept

Each safety-critical frame is replicated onto N ≥ 2 **disjoint physical paths**.
The receiver accepts the first copy and discards duplicates. If one path fails,
the other path's frames arrive without any recovery action — zero recovery time.

```
Generator (sender)              Member paths         Receiver
┌──────────────┐               ┌────────────┐       ┌─────────────┐
│ Original     │──── Path A ──►│ Copy 1     │──────►│  Accept     │
│ Frame        │               └────────────┘       │  Copy 1     │
│ SeqNum = 42  │               ┌────────────┐       │             │
│              │──── Path B ──►│ Copy 2     │──────►│  Discard    │
└──────────────┘               └────────────┘       │  (dup)      │
                                                    └─────────────┘
```

### O(1) Duplicate Detection

```cpp
// History bitmask: 32-entry sliding window
u32 bit = static_cast<u32>(seqNum) % 32U;
if (historyMask & (1U << bit)) {
    return DUPLICATE;  // O(1) — no loop, no search
}
historyMask |= (1U << bit);
return ACCEPT;
```

### Latent Error Detection (IEEE 802.1CB §7.4.5)

A path can fail silently — no error frames, just silence. The LED detects this
by monitoring the ratio of duplicate frames received:

```
If duplicate_rate < 5% over 10 000 frames:
  → One path is not delivering copies → LATENT ERROR
  → FrerManager fires kFrerLatentError
  → TsnOrchestrator transitions to kDegraded
```

This is the difference between **fault detection** (observing errors) and
**latent fault monitoring** (detecting absence of expected events).

---

## 10. ASIL-D Degradation State Machine

```
                    ┌──────────────────┐
                    │  kUninitialized  │
                    └────────┬─────────┘
                   Init() OK │
                    ┌────────▼─────────┐
                    │  kInitializing   │
                    └────────┬─────────┘
                  Start() OK │
                    ┌────────▼─────────┐
                    │  kPtpConverging  │◄────── First gPTP Sync
                    └────────┬─────────┘        messages
           gPTP locked;      │
           GCL loaded;       │         PTP convergence timeout
           FRER registered   │         ──────────────────────►
                    ┌────────▼─────────┐             ┌────────┐
    ┌───────────────┤    kRunning      │             │ kFault │
    │ FRER latent   └────────┬─────────┘◄────────────┤        │
    │ MACsec fail            │                       └────────┘
    │ GCL mismatch           │ PTP holdover expired       ▲
    ▼                        │ NC deadline exceeded        │
┌───────────┐      ┌─────────▼─────────┐               kFault triggers
│ kDegraded │      │    kSafeState     │─────────────────┘
│           │──────►    (TC7 only)     │  (requires hw reset)
└───────────┘      └───────────────────┘
  │ paths restored
  └───────────────► kRunning
```

### Safe State Definition

In `kSafeState`, the `safeStateSchedule` is applied on all ports:

```
GCL:
  Entry 0: gateStates = 0xFF, intervalNs = cycleTime - 30000  (all open — flush)
  Entry 1: gateStates = 0x80, intervalNs = 30000               (TC7 only)
```

Only Brake-by-Wire and safety-critical flows can transmit.
All video, voice, and best-effort traffic is gated out.

---

## 11. Memory Model

| Region | Size | Content |
|--------|------|---------|
| `.text` | ~64 KB | All method implementations |
| `.rodata` | ~4 KB | `constexpr` constants, string literals |
| `.data` / `.bss` | ~32 KB | `TsnOrchestrator` instance (incl. sub-managers, static GCM storage, FRER state arrays, audit log) |
| Stack | ≤ 1024 B/function | ISO 26262 Part 6 §9.4.3 verified by CI Job 3 |
| Heap | **0 bytes** | No dynamic allocation post-init |

Key static sizes:
- `FrerStreamState[64]` = 64 × ~80 B = 5 120 B
- `AuditEntry[64]` = 64 × 32 B = 2 048 B
- `GateControlManager[16]` = placement-new into 16 × ~256 B = 4 096 B
- `ScheduleParams[16]` = 16 × (256 GclEntry × 5 B + 24 B) ≈ 20 KB

---

## 12. 125 µs Real-Time Scheduling Loop

The `TsnOrchestrator::Tick()` method is designed to execute within a single
125 µs period (8 kHz), matching the IEEE 802.1AS gPTP Sync message rate.

**Linux POSIX RT deployment:**

```cpp
// CPU affinity: pin to core 3 (isolated from Linux scheduler)
cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(3, &cpus);
sched_setaffinity(0, sizeof(cpus), &cpus);

// SCHED_FIFO priority 90 (highest non-kernel RT priority)
struct sched_param sp{ .sched_priority = 90 };
sched_setscheduler(0, SCHED_FIFO, &sp);

// 125 µs periodic timer
struct itimerspec ts{
    .it_interval = { .tv_sec = 0, .tv_nsec = 125'000 },
    .it_value    = { .tv_sec = 0, .tv_nsec = 125'000 },
};
timer_settime(timer_id, 0, &ts, nullptr);

// Loop
while (running) {
    wait_for_timer();
    orchestrator.Tick();
}
```

**Worst-case execution time budget per tick:**
- `PtpClockManager::Tick()` ≈ 2 µs (PHC register reads + servo)
- `FrerManager::Tick(64 streams)` ≈ 10 µs (LED checks, path silence)
- `TickIntegrityCheck()` (every 64 ticks) ≈ 30 µs amortised
- `TickDegradationMachine()` ≈ 1 µs
- **Total: ≤ 50 µs** (40% of 125 µs budget)

---

## 13. Admin→Oper GCL Swap Semantics

IEEE 802.1Qbv §8.6.9.4 defines a **double-buffer** mechanism for race-free
schedule updates.  The SJA1110 implements this as follows:

```
Step 1:  Write all new GCL entries to Admin bank registers
         (can be done at any time — Admin bank is not live)

Step 2:  Write AdminCycleTime register

Step 3:  Write AdminBaseTime register
         (PTP timestamp of the next cycle boundary to switch on)

Step 4:  Set the ConfigChange bit in the GateConfigChange register
         → hardware arms the Admin→Oper swap

Step 5:  Hardware atomically swaps Admin→Oper at the AdminBaseTime
         boundary (aligned to the PTP grandmaster clock)
         ConfigPending bit is cleared by hardware when swap completes

Step 6:  Software polls IsGclSwapPending() until ConfigPending == 0
```

The `SwitchHal::ApplyGclToPort()` contract encapsulates all 6 steps.
The abstract interface makes this completely transparent to
`GateControlManager`, which only calls `hal_.ApplyGclToPort()`.

---

## 14. Standards Compliance Matrix

| Standard | Clause / Requirement | Implementation |
|----------|---------------------|----------------|
| IEEE 802.1Qbv | §8.6.8.4 Gate Control List | `GateControlManager` — 7-stage pipeline |
| IEEE 802.1Qbv | §8.6.9.4 Admin→Oper swap | `SwitchHal::ApplyGclToPort()` |
| IEEE 802.1Qbv | Guard Band | Stage 5: `CheckGuardBand()` |
| IEEE 802.1AS-Rev | §10.6 Timestamp format | `PtpTimestamp` (u64 seconds, no bitfield) |
| IEEE 802.1AS-Rev | §11.3.2 Offset computation | `PtpClockManager::InjectSyncMessage()` |
| IEEE 802.1AS-Rev | §10.3.5 BMCA | `PtpClockManager::RunBmca()` |
| IEEE 802.1CB | §10.4 Sequence recovery | `FrerManager::OnFrameReceived()` |
| IEEE 802.1CB | §7.4.5 Latent error detection | `FrerManager::CheckLatentError()` |
| IEEE 802.1Qci | §8.28 Stream filter instances | `SwitchHal::SetStreamFilter()` |
| IEEE 802.1AE | §10.7 Secure Channel | `SwitchHal::ConfigureMacsecChannel()` |
| ISO 26262 | Part 4 §6.4.6 Safe state | `TsnOrchestrator::EnterSafeState()` |
| ISO 26262 | §7.4.2 Audit trail | `AuditEntry[64]` circular log |
| ISO 26262 | Part 6 §9.4.3 Stack analysis | CI Job 3: stack ≤ 1024 B/function |
| AUTOSAR C++14 | M15-3-1 No exceptions | `Result<T,E>` + `TSN_RETURN_IF_ERR` |
| AUTOSAR C++14 | M18-4-1 No heap post-init | Static arrays + placement-new |
| AUTOSAR C++14 | M6-4-1 No FP | Integer ns arithmetic throughout |
| MISRA C++:2008 | Rule 9-6-4 Bitfield types | `PtpTimestamp` uses plain `u64 seconds` |
| UN R155 | Attack surface: replay | MACsec replay window + auth failure monitoring |
| UN R155 | Attack surface: flooding | 802.1Qci `blockOnLateFrame` DDoS containment |

---

*(c) 2026 norxs Technology LLC. All rights reserved.*
*Safety Engineering, Built from the Ground Up. | https://norxs.com*
