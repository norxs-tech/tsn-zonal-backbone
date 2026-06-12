# Open Source Security Assurance Program — OpenChain ISO/IEC 18974:2023

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

| | |
|---|---|
| **Scope** | The `tsn-zonal-backbone` repository and the `norxs-tsn-backbone` static library supplied from it. |
| **Specification** | ISO/IEC 18974:2023 (OpenChain Security Assurance Specification 1.1) |
| **Conformance route** | OpenChain Self-Certification |
| **Program owner** | Product Security Officer (PSO), norxs Technology LLC — contact@norxs.com, subject `[SECURITY]` |
| **Related policies** | [`SECURITY.md`](../../SECURITY.md) (vulnerability disclosure), ISO/SAE 21434 CSMS, UN R155 |

Section numbering mirrors ISO/IEC 18974:2023 §3.

---

## 3.1 Program foundation

### 3.1.1 Security assurance policy
norxs maintains a documented process to identify, track, and remediate **known
vulnerabilities** in the supplied software and its dependencies:

1. **Dependency surface minimisation.** The production library has zero third-party
   runtime dependencies (see SBOM); the known-vulnerability surface is therefore the
   norxs code itself plus the build/test toolchain.
2. **Known-vulnerability detection.**
   - SBOM components (GoogleTest, GitHub Actions) are monitored: Dependabot keeps
     GitHub Actions pinned versions current (`.github/dependabot.yml`); GoogleTest
     advisories are reviewed against the SBOM at each release and when published.
   - First-party code is continuously scanned: clang-tidy `bugprone-*`/`cert-*`
     checks (CI Job 4), cppcheck (CI Job 7), and the AUTOSAR pattern gate (CI Job 5).
3. **Intake.** Vulnerability reports follow the coordinated disclosure process in
   `SECURITY.md` (acknowledgement ≤ 2 business days, triage ≤ 5, critical fix
   target ≤ 30 days, CVE requested for confirmed issues).
4. **Newly published vulnerabilities** affecting already-shipped versions are
   assessed against the supported-versions matrix in `SECURITY.md`; affected
   commercial customers are notified under ISO/SAE 21434 CSMS obligations.

### 3.1.2 Competence & 3.1.3 Awareness
The PSO and release engineers are trained on the secure development standards
applied here (CERT C++, MISRA C++:2008, ISO/SAE 21434). All contributors are
pointed to `SECURITY.md` from the README and PR template.

### 3.1.4 Program scope
This repository. Reviewed at every release and at least every 18 months.

### 3.1.5 Standard practice implementation
Method examples in place:

| Practice | Implementation |
|----------|----------------|
| Software composition analysis | SPDX SBOM, zero-runtime-dependency gate |
| Static analysis | clang-tidy (cert-*, bugprone-*), cppcheck, `-Werror` strict warnings |
| Secure-by-construction rules | No heap, no exceptions, integer-only arithmetic, bounded stack (CI Job 3, ≤ 1024 B/function) |
| Security architecture review | Threat surfaces documented in `SECURITY.md` (gPTP spoofing, MACsec replay, FRER injection, GCL manipulation) |
| Runtime detection of attack indicators | `TickIntegrityCheck()` — MACsec auth-failure monitoring, GCL hardware readback, FRER latent error detection — feeding the ASIL-D degradation FSM |

## 3.2 Relevant tasks defined and supported

- **External inquiries:** security reports → `SECURITY.md` channel; acknowledged
  within 2 business days.
- **Resourcing:** PSO on-call rotation for triage; CI automates recurring detection.

## 3.3 Security assurance review

Before each release, the PSO reviews: open vulnerability reports, static analysis
deltas, SBOM component advisories, and the unit/regression test results
(`docs/test_reports/`). Releases are blocked on unresolved critical findings.

## 3.4 Adherence

norxs Technology LLC affirms that the program documented here satisfies the
requirements of ISO/IEC 18974:2023 for the stated scope, following the OpenChain
self-certification route, and is re-affirmed at every program review.

---
*(c) 2026 norxs Technology LLC. All rights reserved.*
