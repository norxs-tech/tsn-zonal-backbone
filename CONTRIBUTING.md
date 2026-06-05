# Contributing to norxs Deterministic TSN Zonal Backbone

**norxs Technology LLC** | Safety Engineering, Built from the Ground Up.

This repository is a **reference implementation** published for technical demonstration.
External contributions are accepted for bug reports, documentation improvements, and
test coverage additions under the terms below.

---

## Table of Contents

1. [Code of Conduct](#1-code-of-conduct)
2. [Reporting Bugs](#2-reporting-bugs)
3. [Reporting Security Vulnerabilities](#3-reporting-security-vulnerabilities)
4. [Submitting Pull Requests](#4-submitting-pull-requests)
5. [Coding Standard — AUTOSAR C++14](#5-coding-standard--autosar-c14)
6. [MISRA C++:2008 Mandatory Rules](#6-misra-c2008-mandatory-rules)
7. [Naming Conventions](#7-naming-conventions)
8. [Doxygen Comment Requirements](#8-doxygen-comment-requirements)
9. [Commit Message Convention](#9-commit-message-convention)
10. [Test Requirements](#10-test-requirements)
11. [CI Pipeline — All Jobs Must Pass](#11-ci-pipeline--all-jobs-must-pass)

---

## 1. Code of Conduct

All contributors are expected to engage professionally and respectfully.
Safety-critical engineering demands precision, rigour, and clear communication.

---

## 2. Reporting Bugs

Use the **Bug Report** issue template (`.github/ISSUE_TEMPLATE/bug_report.md`).

Required fields:
- Affected module (checkboxes in the template)
- Standard clause reference (e.g. IEEE 802.1Qbv §8.6.8.4)
- Reproduction steps with minimal schedule parameters
- Expected vs actual behaviour
- Compiler version, platform, and toolchain

---

## 3. Reporting Security Vulnerabilities

**Do NOT use the public issue tracker for security vulnerabilities.**

Contact norxs Technology LLC directly:
- Email: contact@norxs.com
- Web: https://norxs.com/contact
- Subject line: `[SECURITY] tsn-zonal-backbone — <brief description>`

All security reports are handled under NDA and coordinated disclosure.
This project is subject to ISO/SAE 21434 and UN R155 obligations.

---

## 4. Submitting Pull Requests

1. Fork the repository and create a branch from `develop` (not `main`).
2. Branch naming: `fix/<module>-<issue>` or `feat/<module>-<description>`
3. All CI jobs must pass before review.
4. Fill in the PR template completely — especially the compliance checklist.
5. PRs that introduce floating-point arithmetic, dynamic allocation, or exceptions
   will be closed without review.

---

## 5. Coding Standard — AUTOSAR C++14

All production code (`src/`, `include/`) must conform to AUTOSAR C++14.
The following are **hard rejections** — no exceptions, no waivers:

| Forbidden | Rule | Reason |
|-----------|------|--------|
| `throw` / `catch` / `try` | M15-3-1 | No exception mechanism |
| `new` / `delete` / `malloc` / `free` | M18-4-1 | No heap allocation |
| `std::vector` / `std::list` / `std::map` | A9-5-1 | Dynamic containers |
| `std::shared_ptr` / `std::unique_ptr` | A20-8-1 | Smart pointer overhead |
| `float` / `double` | M6-4-1 | Use integer nanosecond arithmetic |
| `goto` | M6-6-1 | Unstructured flow |
| Recursive functions | M7-5-4 | Unbounded stack |
| `reinterpret_cast` (production) | M5-2-8 | Type system bypass |
| `#define` constants | A2-13-4 | Use `constexpr` |
| Non-`noexcept` public methods | — | All HAL and manager methods must be `noexcept` |

The CI compliance scan (Job 5) enforces these patterns automatically.

---

## 6. MISRA C++:2008 Mandatory Rules

Key mandatory rules applied in this codebase:

- **Rule 0-1-1**: No unreachable code.
- **Rule 2-10-1**: Identifiers in the same scope shall differ.
- **Rule 5-0-15**: No array indexing via pointer arithmetic.
- **Rule 6-4-1**: No implicit conversion from integer to float.
- **Rule 6-5-2**: Loop counters shall not be modified inside the loop.
- **Rule 7-3-1**: All declarations at namespace, class, or function scope.
- **Rule 9-6-4**: Bit-fields shall only be of type `unsigned int` or `int` — not `u64`.
- **Rule 12-1-3**: Constructors shall not call virtual functions.
- **Rule 15-3-1**: Exceptions shall only be used for error handling (applied as: no exceptions).
- **Rule 16-0-4**: Macros shall not be `#define`'d; `TSN_RETURN_IF_ERR` uses `do-while(false)`.
- **Rule 18-4-1**: No dynamic heap memory after initialisation.

---

## 7. Naming Conventions

| Element | Convention | Example |
|---------|------------|---------|
| Types, Classes, Structs | `PascalCase` | `GateControlManager`, `ScheduleParams` |
| Enum values | `kPascalCase` | `kSafetyOnlyOpen`, `kDeadlineViolation` |
| Member variables (private) | `snake_case_` | `scheduleActive_`, `hal_` |
| Local variables | `camelCase` | `blockingNs`, `rawOffset` |
| Constants (`constexpr`) | `kUpperCamelCase` | `kMinCycleTimeNs`, `kMaxGclEntries` |
| Public methods | `PascalCase` | `ApplySchedule()`, `GetCurrentTime()` |
| Private methods | `PascalCase` | `CheckGuardBand()`, `RunNetworkCalculus()` |
| Namespaces | `lowercase` | `norxs::tsn` |
| Files | `PascalCase` | `GateControlManager.cpp` |
| Test files | `test_PascalCase` | `test_GateControlManager.cpp` |

---

## 8. Doxygen Comment Requirements

Every file **must** have a complete Doxygen header:

```cpp
/**
 * =====================================================================================
 * @file        FileName.hpp
 * @brief       [Minimum 2 sentences. First: what the file does.
 *               Second: key algorithm or design decision.]
 * @project     Deterministic TSN Zonal Backbone
 * @standards   AUTOSAR C++14, MISRA C++:2008, [applicable IEEE standards], ISO 26262 ASIL-D
 * @author      norxs-lab
 * @copyright   (c) 2026 norxs Technology LLC. All rights reserved.
 * @note        Reference implementation for Automotive TSN Network Orchestration.
 * =====================================================================================
 */
```

Every public method must have:
- `@brief` (≥ 1 sentence explaining purpose)
- `@param` for every non-obvious parameter
- `@return` for every possible return value (all `ErrorCode` variants)

The CI Doxygen check (Job 5) enforces `@file`, `@brief`, `@standards`, `@copyright`.

---

## 9. Commit Message Convention

```
<type>: <short summary> (max 72 chars)

[optional body — bullet points for multi-file changes]
```

| Type | Use |
|------|-----|
| `feat` | New module, feature, or file |
| `fix` | Bug fix (reference issue number) |
| `docs` | README, CHANGELOG, comments only |
| `refactor` | Restructuring without behaviour change |
| `test` | Adding or modifying tests |
| `chore` | CI, build system, `.gitignore` |
| `style` | Formatting, naming, whitespace |
| `perf` | Performance improvement |

**Example:**
```
fix: GateControlManager guard band check uses correct frame size

Stage 5 (CheckGuardBand) now includes preamble + IFG in the frame
transmission time calculation, matching IEEE 802.1Qbv §8.6.8.4.
Previous value 12 176 ns corrected to 12 304 ns.

Fixes #42
```

---

## 10. Test Requirements

Any PR modifying `src/` or `include/` must:

- Add or update GoogleTest cases in `tests/test_<Module>.cpp`
- Achieve **MC/DC coverage** on all modified decision points
  (ISO 26262 Part 6 Table 10 — ASIL-D requires MC/DC)
- Include at least one **boundary test** per validation stage added
- Use `MockSwitchHal` with failure injection for HAL interaction tests
- Pass all 5 CI jobs before requesting review

Test naming convention:
```
TEST_F(FixtureName, Stage<N>_<Condition>_<ExpectedResult>)
TEST_F(FixtureName, <Method>_<Scenario>_<ExpectedResult>)
```

---

## 11. CI Pipeline — All Jobs Must Pass

| Job | Description | Failure Means |
|-----|-------------|---------------|
| 1 — build-test | Host build + GoogleTest | Code does not compile or tests fail |
| 2 — cross-compile | aarch64 i.MX8X target | Code is not cross-compilable |
| 3 — stack-analysis | ISO 26262 stack depth | A function exceeds 1024 B stack |
| 4 — lint | clang-tidy static analysis | Code quality violation |
| 5 — compliance | AUTOSAR pattern scan + Doxygen | Forbidden construct or missing header |

PRs with any failing CI job will not be reviewed.

---

*(c) 2026 norxs Technology LLC. All rights reserved.*
*Safety Engineering, Built from the Ground Up. | https://norxs.com*
