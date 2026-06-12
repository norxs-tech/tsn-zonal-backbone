## Summary

<!-- One paragraph describing what this PR does and why. -->

---

## Motivation

<!-- What problem does this solve? Reference the issue. -->

Closes #

---

## Module(s) Changed

- [ ] `TsnTypes.hpp`
- [ ] `SwitchHal.hpp`
- [ ] `GateControlManager`
- [ ] `PtpClockManager`
- [ ] `FrerManager`
- [ ] `TsnOrchestrator`
- [ ] Build system / CMake
- [ ] CI pipeline
- [ ] Tests
- [ ] Documentation

---

## AUTOSAR C++14 Compliance Checklist

- [ ] No `throw` / `catch` / `try` introduced
- [ ] No `new` / `malloc` / `calloc` in production code (placement new with static storage only)
- [ ] No `std::vector` / `std::list` / `std::map` introduced
- [ ] No `std::shared_ptr` / `std::unique_ptr` introduced
- [ ] No `float` / `double` introduced — integer arithmetic only
- [ ] No `goto` introduced
- [ ] No recursive functions introduced
- [ ] All new public methods are `noexcept`
- [ ] All new headers have include guards
- [ ] Doxygen header present on every new file (`@file`, `@brief` ≥ 2 sentences, `@standards`, `@copyright`)

---

## ISO 26262 / Safety Checklist

- [ ] Any new validation stage adds a corresponding `ErrorCode` entry in `TsnTypes.hpp`
- [ ] Any new fault condition is handled by `TsnOrchestrator::TickDegradationMachine()`
- [ ] Any state transition is logged via `TsnOrchestrator::LogStateTransition()`
- [ ] Guard band constants updated if frame size assumptions change
- [ ] Network Calculus deadline constants updated if safety requirements change
- [ ] No change reduces the ASIL-D TC7 window minimum width

---

## Testing

<!-- Paste GoogleTest output for the test run. All tests must pass. -->

```
[==========] Running X tests from Y test suites.
[----------] ...
[  PASSED  ] X tests.
```

- [ ] New tests added for all changed decision points
- [ ] MC/DC coverage achieved on all new branches
- [ ] `MockSwitchHal` failure injection tested for all new HAL calls
- [ ] CI Jobs 1–8 + CodeQL all passing (link to Actions run):

---

## License & Security Compliance (OpenChain ISO/IEC 5230 · ISO/IEC 18974)

- [ ] All new/modified files carry the `SPDX-License-Identifier` header
- [ ] No third-party code introduced into the production library
      (or: OSPO approval obtained and SBOM updated — link approval)
- [ ] `sbom/` updated if any build/test dependency changed
- [ ] I have read [`SECURITY.md`](../SECURITY.md) and this change introduces no
      new attack surface, **or** the new surface is documented there

---

## Breaking Changes

<!-- Does this PR change any public API in TsnTypes.hpp, SwitchHal.hpp, or any manager header? -->

- [ ] No breaking changes
- [ ] Breaking changes — describe migration path below:

---

*(c) 2026 norxs Technology LLC. All rights reserved.*
