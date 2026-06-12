# Security Policy — norxs Deterministic TSN Zonal Backbone

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

This project implements safety-critical automotive network infrastructure subject to
**ISO/SAE 21434**, **UN R155**, and **UN R156** cybersecurity obligations.
Responsible disclosure is taken seriously and handled with urgency.

---

## Supported Versions

| Version | Supported          |
|---------|--------------------|
| 1.x     | ✅ Active support  |
| < 1.0   | ❌ Not supported   |

---

## Reporting a Vulnerability

**Do NOT use the public GitHub issue tracker for security vulnerabilities.**
Public disclosure of a vulnerability before a fix is available could endanger
vehicles and systems built on this technology.

### Contact

| Channel | Details |
|---------|---------|
| **Email** | contact@norxs.com |
| **Subject line** | `[SECURITY] tsn-zonal-backbone — <brief one-line description>` |
| **Web** | https://norxs.com/contact |

All security reports are handled under **Non-Disclosure Agreement**.
We do not require you to sign an NDA before submitting a report —
we will propose one as part of the coordinated disclosure process.

---

## What to Include in Your Report

To help us triage and reproduce efficiently, please provide:

1. **Affected component** — module name, file, function (e.g. `FrerManager::OnFrameReceived`)
2. **Vulnerability class** — e.g. integer overflow, race condition, authentication bypass
3. **Attack surface** — local, network, physical access required?
4. **Reproduction steps** — minimal code or network capture demonstrating the issue
5. **Impact assessment** — which safety or security properties are violated
6. **Standard reference** — relevant clause (e.g. IEEE 802.1AE §10.7, UN R155 Annex 5)
7. **Suggested fix** (optional but appreciated)

---

## Response Timeline

| Stage | Target |
|-------|--------|
| Acknowledgement | Within **2 business days** |
| Initial triage | Within **5 business days** |
| Fix development | Within **30 days** for critical issues |
| Coordinated disclosure | Agreed jointly with reporter |
| CVE assignment | Requested for confirmed vulnerabilities |
| Public disclosure | After fix released; typically 90-day embargo |

For vulnerabilities affecting production vehicles or live deployments,
we will escalate immediately and may contact relevant automotive OEM/Tier-1 partners
under our ISO/SAE 21434 CSMS obligations.

---

## Scope

### In Scope

- All source files in `include/norxs/tsn/` and `src/`
- Cryptographic protocol implementation (IEEE 802.1AE MACsec, key rotation)
- Authentication / replay attack vulnerabilities (MACsec replay window)
- Denial of service via malformed TSN schedule or stream injection
- FRER sequence number manipulation enabling duplicate injection
- gPTP spoofing enabling baseTime manipulation (schedule disruption)
- Buffer / integer overflow in GCL validation pipeline
- Race conditions in Admin→Oper GCL swap

### Out of Scope

- Vulnerabilities in third-party dependencies (GoogleTest, CMake)
- Attacks requiring direct physical access to the switch ASIC management bus
  (these are handled by hardware security in the vehicle design)
- Social engineering or phishing

---

## Security Assurance Program

Vulnerability handling in this project operates under a documented
**OpenChain ISO/IEC 18974** security assurance program — see
[`docs/compliance/openchain-iso18974.md`](docs/compliance/openchain-iso18974.md).
Key elements:

- **SBOM:** A machine-readable SPDX 2.3 bill of materials is published at
  [`sbom/tsn-zonal-backbone-1.1.0.spdx.json`](sbom/tsn-zonal-backbone-1.1.0.spdx.json)
  and validated by CI on every push. The production library has **zero third-party
  runtime dependencies** (CI-enforced), which minimises the known-vulnerability surface.
- **Known-vulnerability monitoring:** Dependabot tracks CI toolchain components;
  SBOM entries are reviewed against published advisories at every release.
- **Continuous detection:** clang-tidy (`cert-*`, `bugprone-*`), cppcheck, the
  AUTOSAR pattern gate, and bounded-stack verification run on every push.
- **Framework alignment:** Controls are mapped to **NIST CSF 2.0** in
  [`docs/compliance/nist-csf-mapping.md`](docs/compliance/nist-csf-mapping.md).

---

## Security Architecture Reference

The security design of this stack covers the following UN R155 attack surfaces:

| Attack Surface | Mitigation | Standard |
|----------------|------------|----------|
| Network flooding / DDoS | 802.1Qci `blockOnLateFrame` per-stream policing | UN R155 Annex 5.3.3 |
| Frame injection | 802.1AE MACsec AES-GCM-128 authentication | UN R155 Annex 5.3.5 |
| Replay attacks | MACsec replay window, FRER sequence tracking | UN R155 Annex 5.3.5 |
| gPTP spoofing | BMCA dataset comparison + hardware timestamps | IEEE 802.1AS-Rev |
| Schedule manipulation | 7-stage GCL validation, hardware integrity readback | ISO 26262 ASIL-D |
| Latent path failure | FRER Latent Error Detection §7.4.5 | IEEE 802.1CB |

---

## Disclosure Credit

norxs Technology LLC acknowledges responsible disclosures in the CHANGELOG
under the reporter's preferred name/handle, unless anonymity is requested.

---

*(c) 2026 norxs Technology LLC. All rights reserved.*
*Safety Engineering, Built from the Ground Up. | https://norxs.com*
*All engagements conducted under NDA.*
