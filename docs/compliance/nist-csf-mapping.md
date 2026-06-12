# NIST Cybersecurity Framework (CSF) 2.0 Mapping

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

This document maps the engineering and process controls of the
`tsn-zonal-backbone` project to the six functions of **NIST CSF 2.0**
(NIST CSWP 29, February 2024). Two layers are mapped:

- **Process layer** — how this repository is developed, reviewed, and released.
- **Product layer** — security capabilities the TSN stack itself provides to the
  vehicle network it protects (defence-in-depth for a UN R155 / ISO 21434 item).

| CSF 2.0 Function | Category (sel.) | Process-layer control (this repo) | Product-layer capability (the stack) |
|---|---|---|---|
| **GOVERN (GV)** | GV.OC, GV.RR, GV.PO | Security policy (`SECURITY.md`); OSPO/PSO roles and review cadence (`docs/compliance/openchain-iso5230.md`, `openchain-iso18974.md`); license & contribution governance (`LICENSE`, `CONTRIBUTING.md`) | Security requirements traced to UN R155 Annex 5 and ISO/SAE 21434 work products (commercial evidence package) |
| **IDENTIFY (ID)** | ID.AM, ID.RA | Asset inventory via SPDX SBOM (`sbom/`); risk analysis of attack surfaces documented in `SECURITY.md`; defect/observation tracking in test reports (`docs/test_reports/`) | TARA-derived threat model: gPTP spoofing, MACsec replay, FRER sequence injection, GCL manipulation, flooding/DoS |
| **PROTECT (PR)** | PR.AA, PR.DS, PR.PS, PR.IR | Branch protection + mandatory CI gates (build, tests, static analysis, AUTOSAR scan, SPDX headers, SBOM validation); memory-safe-by-construction coding rules (no heap, no exceptions, integer-only, bounded stack ≤ 1024 B verified in CI) | IEEE 802.1AE MACsec AES-GCM-128 authentication & encryption with key rotation; 802.1Qci per-stream filtering/policing (`blockOnLateFrame`); 7-stage GCL validation rejects malformed schedules before hardware commit; ASIL-D guard bands preserve safety bandwidth under attack load |
| **DETECT (DE)** | DE.CM, DE.AE | CI static analysis on every push (clang-tidy cert-*/bugprone-*, cppcheck); Dependabot monitoring of pinned action versions; SBOM-vs-advisory review each release | `TickIntegrityCheck()` every 8 ms: MACsec authentication-failure counters, GCL hardware readback vs. intended schedule, per-stream drop counters; FRER Latent Error Detection (802.1CB §7.4.5) detects silently failed redundant paths; BMCA dataset comparison resists rogue grandmaster announcements |
| **RESPOND (RS)** | RS.MA, RS.AN, RS.CO | Coordinated vulnerability disclosure with SLAs (ack ≤ 2 d, triage ≤ 5 d, critical fix ≤ 30 d, CVE assignment, 90-day embargo); customer notification under ISO/SAE 21434 CSMS | ASIL-D degradation state machine responds autonomously within one 125 µs tick: kRunning → kDegraded on MACsec/FRER/integrity faults; kDegraded → kSafeState (TC7 safety traffic only) on deadline risk; all transitions recorded with trigger codes in the 64-entry circular audit log (forensics) |
| **RECOVER (RC)** | RC.RP, RC.CO | Patched releases follow SemVer with documented CHANGELOG; supported-versions matrix in `SECURITY.md`; release artifacts retained with SBOM for traceability | Recovery paths in the FSM: kDegraded → kRunning when all redundant paths restore; gPTP holdover (TCXO drift-compensated) maintains schedule validity through grandmaster loss; kSafeState/kFault require deliberate reset + re-`Init()`, preventing oscillation |

## Profile statement

The project's target profile prioritises **PROTECT and DETECT at the product layer**
(the stack is itself a protective network control for ASIL-D traffic) and
**GOVERN/IDENTIFY at the process layer** (SBOM-driven, policy-backed development).
Gaps tracked toward the target profile are recorded as observations in
`docs/test_reports/` (e.g., OBS-001: PHC-independent watchdog time source).

## Maintenance

This mapping is reviewed at every release alongside the ISO/IEC 5230 and
ISO/IEC 18974 program reviews.

---
*(c) 2026 norxs Technology LLC. All rights reserved.*
