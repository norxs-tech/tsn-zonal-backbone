# norxs Deterministic TSN Zonal Backbone
### AUTOSAR C++14 IEEE 802.1Qbv/CB/Qci/AE stack with Network Calculus latency verification for NXP i.MX8X

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

[![CI](https://github.com/norxs-tech/tsn-zonal-backbone/actions/workflows/ci.yml/badge.svg)](https://github.com/norxs-tech/tsn-zonal-backbone/actions)
[![License](https://img.shields.io/badge/license-norxs%20RI%20v1.0-blue)](LICENSE)
[![Standard](https://img.shields.io/badge/standard-IEEE%20802.1Qbv%20%7C%20802.1CB%20%7C%20802.1Qci%20%7C%20802.1AE-green)]()
[![Safety](https://img.shields.io/badge/safety-ISO%2026262%20ASIL--D-red)]()
[![Cybersecurity](https://img.shields.io/badge/cybersecurity-UN%20R155%20%7C%20ISO%2021434-orange)]()

---

## What This Is

The norxs Deterministic TSN Zonal Backbone is a production-grade, hard real-time
network scheduling IP stack for next-generation Software-Defined Vehicle (SDV) Zonal
Architectures.  It enforces **zero-packet-loss, sub-microsecond latency guarantees**
for ASIL-D safety-critical payloads sharing a 1000BASE-T1 link with high-bandwidth
infotainment streams — with mathematically verified worst-case delay bounds via
Network Calculus (Le Boudec & Thiran, 2001).

**This is the software we build for our clients — shown here as a reference.**

---

## System Architecture

```
[ Front Zonal Gateway ]            [ Rear Zonal Gateway ]
   LiDAR · Radar                     Camera · Sensors
         │ 1000BASE-T1                      │ 1000BASE-T1
─────────┼──────────────────────────────────┼──────────────────
         │                                  │
  ┌──────▼──────────────────────────────────▼──────┐
  │           Automotive TSN Switch ASIC             │
  │       NXP SJA1110  ·  Marvell 88Q5050           │
  │                                                  │
  │  [ 802.1Qbv TAS Gates ]  [ 802.1CB FRER ]        │
  │  [ 802.1Qci PSFS Policing ]  [ 802.1AE MACsec ]  │
  └──────────────────┬───────────────────────────────┘
                     │ MDIO / SPI / PCIe management
  ┌──────────────────▼───────────────────────────────┐
  │         NXP i.MX8X SoC — norxs TSN Stack         │
  │──────────────────────────────────────────────────│
  │                                                  │
  │  TsnOrchestrator  (125 µs SCHED_FIFO loop)       │
  │    ├─ PtpClockManager  (IEEE 802.1AS-Rev gPTP)   │
  │    │    ├─ PI Servo (integer-scaled, no FP)       │
  │    │    ├─ BMCA grandmaster election              │
  │    │    └─ TCXO holdover compensation             │
  │    │                                              │
  │    ├─ GateControlManager × N ports               │
  │    │    ├─ 7-stage schedule validation pipeline   │
  │    │    ├─ Network Calculus WCD analyser          │
  │    │    ├─ ASIL-D guard band enforcement          │
  │    │    └─ Admin→Oper atomic GCL swap             │
  │    │                                              │
  │    ├─ FrerManager  (IEEE 802.1CB)                 │
  │    │    ├─ O(1) bitmask duplicate detection       │
  │    │    ├─ Per-path silence monitoring            │
  │    │    └─ Latent error detection (LED §7.4.5)   │
  │    │                                              │
  │    └─ SwitchHal  (pure-virtual MDIO/SPI/PCIe)    │
  │         ├─ Sja1110Hal  (NXP SJA1110)             │
  │         └─ Q5050Hal    (Marvell 88Q5050)          │
  └──────────────────────────────────────────────────┘
```

---

## Module Inventory

| Module | Files | Purpose | Standard |
|--------|-------|---------|----------|
| **TsnOrchestrator** | `TsnOrchestrator.hpp/.cpp` | Master coordinator, 125 µs loop, ASIL-D state machine | ISO 26262 ASIL-D |
| **GateControlManager** | `GateControlManager.hpp/.cpp` | 7-stage GCL validation + Network Calculus + Admin→Oper swap | IEEE 802.1Qbv |
| **PtpClockManager** | `PtpClockManager.hpp/.cpp` | gPTP PI servo, BMCA, holdover state machine | IEEE 802.1AS-Rev |
| **FrerManager** | `FrerManager.hpp/.cpp` | Frame replication/elimination, O(1) dup detection, LED | IEEE 802.1CB |
| **SwitchHal** | `SwitchHal.hpp` | Pure-virtual 6-plane HAL (TAS, PSFS, FRER, MACsec, VLAN, diag) | IEEE 802.1Qbv/CB/Qci/AE |
| **TsnTypes** | `TsnTypes.hpp` | Types, `Result<T,E>` monad, `PtpTimestamp`, `ScheduleParams` | AUTOSAR C++14 |

---

## Key Algorithms

### 1. Network Calculus Worst-Case Delay Analyser (`GateControlManager.cpp`)

Implements the Latency-Rate (LR) server model from Le Boudec & Thiran (2001)
for IEEE 802.1Qbv Time-Aware Scheduling.  For each Traffic Class i:

```
T_i  = Σ{ intervalNs : bit_i == 0 }          (blocking time per cycle)
B_i  = burstSizeBytes × 8 ns/byte             (burst drain at 1 Gbps)

WCD_i = T_i + B_i + Σ_{j > i} B_j            (worst-case end-to-end delay)

ASIL-D contract: WCD_TC7 ≤ 500 000 ns
```

All arithmetic is **integer-only** (nanosecond domain, u64).
No floating-point. No heap allocation.

### 2. ASIL-D Guard Band Enforcement  (`GateControlManager.cpp` Stage 5)

```
guardBandNs = maxFrameSizeBytes × 8 + safetyFrameBytes × 8
            = 1538 × 8 + safetyFrame × 8
            = 12 304 ns + safetyFrameNs

Required: TC7 window intervalNs ≥ guardBandNs
```

Prevents long best-effort frames from straddling the TC7 window edge,
which would violate the ASIL-D determinism guarantee at the hardware level.

### 3. Integer PI Servo  (`PtpClockManager.cpp`)

```
Kp = 2^(-2)    Ki = 2^(-5)     (no FP — shift arithmetic only)

offset_ns  = (t2 + correction) - t1 - meanPathDelay
integral  += offset_ns
adjustment = -(offset_ns >> 2) - (integral >> 5)
adjustment  = clamp(adjustment, ±500 000 ppb)
```

Achieves < 1 µs synchronisation accuracy with TCXO oscillator.

### 4. O(1) FRER Duplicate Detection  (`FrerManager.cpp`)

```
bit = seqNum % 32
if (historyMask & (1 << bit)):  DUPLICATE — discard
else: set bit, accept frame
```

IEEE 802.1CB §10.4 compliant.  Latent Error Detection fires
when duplicate rate < 5% over 10 000-frame window (path silently broken).

---

## AUTOSAR C++14 Compliance

| Check | Result |
|-------|--------|
| `try` / `catch` / `throw` | **0** |
| `std::vector` / `std::list` / `std::map` | **0** |
| `new` / `malloc` (production code) | **0** — placement new only in Orchestrator static storage |
| `float` / `double` | **0** — integer nanosecond arithmetic throughout |
| `goto` | **0** |
| `shared_ptr` / `unique_ptr` | **0** |
| Doxygen headers | **All files** |
| Include guards | **All headers** |

## ISO 26262 Compliance

| Measure | Status | Reference |
|---------|--------|-----------|
| ASIL-D exclusive TC7 window mandatory | ✅ Stage 6 validation | IEEE 802.1Qbv §8.6.8.4 |
| Guard band ≥ 12 304 ns at 1 Gbps | ✅ Stage 5 validation | IEEE 802.1Qbv §8.6.8.4 |
| Network Calculus WCD ≤ 500 µs (TC7) | ✅ Stage 7 validation | ISO 26262 Part 4 §6.4 |
| Hardware schedule integrity check | ✅ VerifyHardwareSchedule() | ISO 26262 Part 6 §9.4 |
| ASIL-D degradation state machine | ✅ TsnOrchestrator | ISO 26262 Part 4 §6.4.6 |
| Circular audit log (64 entries) | ✅ AuditEntry[] | ISO 26262 §7.4.2 |
| gPTP holdover continuity | ✅ PtpClockManager | IEEE 802.1AS-Rev |
| FRER latent error detection | ✅ FrerManager LED | IEEE 802.1CB §7.4.5 |
| MACsec authentication monitoring | ✅ TickIntegrityCheck() | IEEE 802.1AE · UN R155 |
| No dynamic memory post-init | ✅ AUTOSAR M18-4-1 | MISRA C++:2008 |
| No exceptions | ✅ `Result<T,E>` monad | AUTOSAR M15-3-1 |
| Stack usage bounded | ✅ CI Job 3 | ISO 26262 Part 6 §9.4.3 |

---

## Build Instructions

### Prerequisites

- CMake ≥ 3.21
- GCC ≥ 11 or Clang ≥ 14 (C++14 with `-fno-exceptions -fno-rtti`)
- For cross-compile: `aarch64-poky-linux-g++` (NXP Yocto SDK)
- For tests: GoogleTest ≥ 1.13

### Host unit tests

```bash
cmake -B build-host -DNORXS_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host --parallel
ctest --test-dir build-host -V
```

### Cross-compile for NXP i.MX8X

```bash
source /opt/fsl-imx-wayland/6.1-mickledore/environment-setup-armv8a-poky-linux
cmake -B build-imx8x \
      -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-aarch64-imx8x.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build-imx8x --parallel
```

### Coverage report (host)

```bash
cmake -B build-cov -DNORXS_BUILD_TESTS=ON -DNORXS_ENABLE_COVERAGE=ON
cmake --build build-cov
ctest --test-dir build-cov
lcov --capture --directory build-cov -o coverage.info
genhtml coverage.info --output-directory coverage-report
```

---

## Repository Structure

```
tsn-zonal-backbone/
├── include/norxs/tsn/
│   ├── TsnTypes.hpp              Foundational types, Result<T,E>, constants
│   ├── SwitchHal.hpp             Pure-virtual 6-plane HAL interface
│   ├── GateControlManager.hpp   IEEE 802.1Qbv schedule manager
│   ├── PtpClockManager.hpp      IEEE 802.1AS-Rev gPTP clock manager
│   ├── FrerManager.hpp          IEEE 802.1CB FRER state machine
│   └── TsnOrchestrator.hpp      Master coordinator + degradation FSM
├── src/
│   ├── GateControlManager.cpp   7-stage validation + Network Calculus
│   ├── PtpClockManager.cpp      PI servo + BMCA + holdover
│   ├── FrerManager.cpp          Duplicate detection + LED
│   └── TsnOrchestrator.cpp      125 µs loop + state machine
├── tests/
│   └── test_GateControlManager.cpp   28 GoogleTest cases (MC/DC coverage)
├── docs/
│   └── architecture.md          Deep-dive architecture documentation
├── cmake/
│   └── Toolchain-aarch64-imx8x.cmake  NXP i.MX8X cross-compilation
├── .github/
│   ├── workflows/ci.yml          5-job CI pipeline
│   └── ISSUE_TEMPLATE/
│       └── bug_report.md
├── CMakeLists.txt
├── LICENSE
├── CHANGELOG.md
├── CONTRIBUTING.md
└── README.md
```

---

## Commercial Licensing & Services

This reference implementation is published under the **norxs Reference Implementation License v1.0**.
Commercial use requires a separate license agreement.

**norxs Technology LLC** offers:
- Full production source rights for ASIL-D deployment
- ISO 26262 safety evidence package (HARA, FMEA, FTA, DFA, Safety Case)
- UN R155 / ISO 21434 TARA and CSMS documentation
- ASPICE-aligned development process documentation
- SJA1110 / 88Q5050 concrete HAL implementation
- Long-term engineering support and maintenance


All engagements conducted under NDA.

---

## Standards

IEEE 802.1Qbv · IEEE 802.1AS-Rev · IEEE 802.1CB · IEEE 802.1Qci · IEEE 802.1AE ·
AUTOSAR C++14 · MISRA C++:2008 · ISO 26262 ASIL-D · ISO/SAE 21434 · UN R155 · ASPICE

---

*(c) 2026 norxs Technology LLC. All rights reserved.*

---

## Safety Documentation

| Document | Path | Description |
|----------|------|-------------|
| Safety Case Summary | `docs/safety_case_summary.md` | ASIL-D hazard analysis, SSR allocation, DFA summary |
| Architecture Deep Dive | `docs/architecture.md` | Algorithm derivations, state machines, standards matrix |
| API Reference | `docs/doxygen/html/` | Generated via `doxygen Doxyfile` |

Full safety evidence package (HARA, FMEA, FTA, DFA, FSC, Safety Case)
available to commercial customers under NDA.

---

## Security

This project follows coordinated vulnerability disclosure.
**Do not report security vulnerabilities via public issues.**
See [SECURITY.md](SECURITY.md) for the full policy.

Relevant attack surfaces: gPTP spoofing · MACsec replay · FRER sequence injection · GCL manipulation.
