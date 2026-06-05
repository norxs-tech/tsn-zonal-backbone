---
name: Bug Report
about: Report a defect in the norxs Deterministic TSN Zonal Backbone
title: "[BUG] <module>: <one-line description>"
labels: bug
assignees: ''
---

## Module

<!-- Tick all affected modules -->

- [ ] `TsnTypes.hpp` — Types, Result monad, constants
- [ ] `SwitchHal.hpp` — Hardware Abstraction Layer interface
- [ ] `GateControlManager` — IEEE 802.1Qbv schedule validation / Network Calculus
- [ ] `PtpClockManager` — IEEE 802.1AS-Rev gPTP clock / BMCA / holdover
- [ ] `FrerManager` — IEEE 802.1CB FRER duplicate detection / LED
- [ ] `TsnOrchestrator` — Master coordinator / degradation state machine
- [ ] `CMakeLists.txt` / build system
- [ ] `.github/workflows/ci.yml` — CI pipeline
- [ ] `tests/` — Unit test suite
- [ ] `docs/` — Documentation

---

## Description

<!-- A clear and concise description of the defect. -->

---

## Standard Clause Reference

<!-- Reference the relevant standard clause if applicable. -->
<!-- Examples: IEEE 802.1Qbv §8.6.8.4 · ISO 26262 Part 6 §9.4.3 · MISRA C++:2008 Rule 9-6-4 -->

**Standard / Clause:**

---

## Expected Behaviour

<!-- What should happen according to the standard or specification? -->

---

## Actual Behaviour

<!-- What actually happens? Include error codes, assertion failures, or incorrect values. -->

---

## Reproduction Steps

<!-- Minimal example to reproduce the defect. Include ScheduleParams values if relevant. -->

```cpp
// Minimal reproduction case
ScheduleParams p{};
p.cycleTimeNs = ...;
// ...
```

---

## Environment

| Field | Value |
|-------|-------|
| Compiler | (e.g. GCC 13.2, Clang 17) |
| Host arch | (e.g. x86_64, aarch64) |
| CMake version | |
| Toolchain | (e.g. aarch64-poky-linux-g++) |
| OS / BSP | (e.g. Ubuntu 22.04, NXP Yocto kirkstone) |

---

## Additional Context

<!-- Stack traces, register dumps, oscilloscope captures, or other diagnostic data. -->

---

> **Security note:** Do NOT use this form to report security vulnerabilities.
> Contact norxs Technology LLC directly via https://norxs.com/contact
