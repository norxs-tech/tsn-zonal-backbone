# Open Source License Compliance Program — OpenChain ISO/IEC 5230:2020

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

| | |
|---|---|
| **Scope (per §1.3 "program")** | The `tsn-zonal-backbone` repository and the supplied software derived from it: the `norxs-tsn-backbone` static library and its accompanying documentation, tests, and build files. |
| **Specification** | ISO/IEC 5230:2020 (the OpenChain Specification 2.1) |
| **Conformance route** | OpenChain Self-Certification |
| **Program owner** | Open Source Program Officer, norxs Technology LLC — contact@norxs.com, subject `[OSPO]` |
| **Review cadence** | Every tagged release, and at minimum every 18 months (per §3.1.3 / §4.1) |

This document is the program documentation required by the specification. The
section numbering below mirrors ISO/IEC 5230:2020 §3.

---

## 3.1 Program foundation

### 3.1.1 Policy
norxs maintains a written open source policy:

1. **Inbound:** No third-party component may be added to the supplied software
   (production library) without OSPO approval. The production library currently has
   **zero third-party runtime dependencies by design** — this is an architectural
   property enforced by the AUTOSAR C++14 constraints (no heap, no exceptions, only
   `<cstdint>`, `<cstddef>`, `<array>`, `<limits>` from the standard library) and
   verified by CI Job 5 (compliance scan).
2. **Test/build-time:** Tools and frameworks (GoogleTest BSD-3-Clause, CMake
   BSD-3-Clause, GCC/Clang toolchains) are tracked in the SBOM but are never part of
   the distributed artifact.
3. **Outbound:** All distributed files carry SPDX headers
   (`SPDX-License-Identifier: LicenseRef-norxs-RI-1.0`); the complete license text is
   in `LICENSE` and `LICENSES/LicenseRef-norxs-RI-1.0.txt`.
4. All contributions follow `CONTRIBUTING.md`; contributor awareness of this policy
   is confirmed in the pull request template.

### 3.1.2 Competence
The program identifies the roles below; competence is documented in internal
training records (available under NDA):

| Role | Responsibility |
|------|----------------|
| Open Source Program Officer | Policy ownership, license review, SBOM sign-off |
| Release Engineer | SBOM regeneration at each tag, SPDX header CI gate |
| All committers | Policy awareness acknowledged per PR (template checkbox) |

### 3.1.3 Awareness & 3.1.4 Program scope
This document, `LICENSE`, `NOTICE`, and `CONTRIBUTING.md` are published in-repo.
Scope is this repository (see table above). Review occurs at every release.

### 3.1.5 License obligations
The OSPO reviews the identified licenses of all components in the SBOM and
satisfies their obligations (e.g., BSD-3-Clause attribution for GoogleTest is
carried in `NOTICE`, although GoogleTest is not distributed).

## 3.2 Relevant tasks defined and supported

- **External inquiries (§3.2.1):** License/compliance questions →
  contact@norxs.com, subject `[OSPO]`. Public contact is published in `NOTICE`
  and this document. Inquiries acknowledged within 5 business days.
- **Resourcing (§3.2.2):** OSPO effort is allocated per release; CI automates the
  recurring checks (SPDX headers — CI Job 5; SBOM presence/validity — CI Job 6).

## 3.3 Open source content review and approval

- **Bill of materials (§3.3.1):** SPDX 2.3 SBOM at
  [`sbom/tsn-zonal-backbone-1.1.0.spdx.json`](../../sbom/tsn-zonal-backbone-1.1.0.spdx.json),
  regenerated and reviewed at every tagged release. CI Job 6 validates JSON
  well-formedness and that the SBOM version matches the project version.
- **License compliance artifacts (§3.3.2):** SPDX headers on **every** source and
  test file (machine-enforced, CI fails otherwise); `LICENSE`; `LICENSES/` directory
  (REUSE-style layout); `NOTICE` with third-party attributions.

## 3.4 Compliance artifact creation and delivery

Each release archives: the SBOM, the `LICENSE`/`NOTICE` files, and the CI run
demonstrating the SPDX/compliance gates passed. Artifacts are retained for the
support lifetime of the release (see `SECURITY.md` supported versions) and no less
than the duration required by customer agreements.

## 3.5 Understanding open source community engagement

Upstream contributions (e.g., patches to GoogleTest or toolchains) require OSPO
approval and follow the upstream project's contribution process and license.

## 3.6 Adherence to the specification requirements

norxs Technology LLC affirms that the program documented here satisfies all
requirements of ISO/IEC 5230:2020 for the stated scope and follows the OpenChain
self-certification route. Conformance is re-affirmed at every program review.

---
*(c) 2026 norxs Technology LLC. All rights reserved.*
