# Tickets — Willpower.Application cross-platform (Linux first)

Source of truth: [`PLAN.md`](../PLAN.md). Goal: make `Willpower.Application` (and its
prerequisite `Willpower.Common`) build and pass tests on Linux (Ubuntu 22.04+, GCC 12+
and Clang 15+) while keeping the Windows/MSVC build green throughout.

Success (Phase 0):

```
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

## Index

All tickets are published as GitHub issues on [`ajare/willpower`](https://github.com/ajare/willpower/issues)
(labelled `cross-platform`, `priority/P0|P1`, `difficulty/easy|medium|hard`) with the
"blocked by" relationships below wired in GitHub. WP-0NN maps to issue #NN.

| Ticket | Issue | Phase | Priority | Difficulty | Title | Depends on | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [WP-001](WP-001.md) | [#1](https://github.com/ajare/willpower/issues/1) | 1 — Common | P0 | medium | Portable stack walking and unconditional `ASSERT_TRACE` | — | done |
| [WP-002](WP-002.md) | [#2](https://github.com/ajare/willpower/issues/2) | 1 — Common | P0 | easy | Portable local time in `Logger.cpp` | — | open |
| [WP-003](WP-003.md) | [#3](https://github.com/ajare/willpower/issues/3) | 1 — Common | P1 | easy | Feature-test guard for `source_location` in `Exceptions.h` | — | open |
| [WP-004](WP-004.md) | [#4](https://github.com/ajare/willpower/issues/4) | 1 — Common | P0 | medium | Platform-aware Common CMake (defines, sources, warnings) | WP-001 | done |
| [WP-005](WP-005.md) | [#5](https://github.com/ajare/willpower/issues/5) | 2 — Application | P0 | hard | Portable `Scheduler`/`SchedulerTask` timing (QPC → `steady_clock`) | WP-001 | done |
| [WP-006](WP-006.md) | [#6](https://github.com/ajare/willpower/issues/6) | 2 — Application | P1 | easy | Drop the Windows gate on the FMOD audio backend | — | open |
| [WP-007](WP-007.md) | [#7](https://github.com/ajare/willpower/issues/7) | 2 — Application | P0 | medium | `Scheduler` unit test (ordering + budget scaling) | WP-005 | done |
| [WP-008](WP-008.md) | [#8](https://github.com/ajare/willpower/issues/8) | 3 — CMake | P0 | easy | Root CMake: drop MSVC-only gates | — | done |
| [WP-009](WP-009.md) | [#9](https://github.com/ajare/willpower/issues/9) | 3 — CMake | P0 | medium | `cmake/Helpers.cmake`: platform-aware target helpers | WP-008 | done |
| [WP-010](WP-010.md) | [#10](https://github.com/ajare/willpower/issues/10) | 3 — CMake | P0 | medium | `cmake/Dependencies.cmake`: branch MPP import; keep FMOD WIN32 gate | WP-008 | done |
| [WP-011](WP-011.md) | [#11](https://github.com/ajare/willpower/issues/11) | 3 — CMake | P1 | easy | Application module CMake: compiler-aware warnings | WP-009 | open |
| [WP-012](WP-012.md) | [#12](https://github.com/ajare/willpower/issues/12) | 4 — Verify | P0 | medium | Linux end-to-end build + `ctest` (acceptance gate) | all above | open |
| [WP-013](WP-013.md) | [#13](https://github.com/ajare/willpower/issues/13) | 4 — Verify | P1 | easy | CI: Linux job (GCC + Clang) | WP-012 | open |
| [WP-014](WP-014.md) | [#14](https://github.com/ajare/willpower/issues/14) | 4 — Verify | P1 | easy | Docs: Linux prerequisites + README update | WP-012 | open |

## Suggested execution order

1. **Phase 1 (Common prerequisite):** WP-001, WP-002, WP-003 in parallel; WP-004 after WP-001.
   Common must compile on Linux before Application work is verifiable.
2. **Phase 2 (Application sources):** WP-005 (after WP-001), then WP-007; WP-006 any time.
3. **Phase 3 (CMake):** WP-008 first, then WP-009/WP-010 in parallel; WP-011 after WP-009.
4. **Phase 4:** WP-012 once everything above is in; then WP-013 and WP-014 in parallel.

## Out of scope for this pass (Phase 5 follow-ups)

- `willpower.viz` (renderer; same treatment, larger effort).
- macOS (`WP_PLATFORM_APPLE` exists but is untested).
- FMOD backend on Linux (needs the proprietary Linux SDK).
- Static-library build mode / `WP_*_STATIC_LIB`.

## Risks (from PLAN.md)

- **MPP import paths:** exact Linux artifact locations need confirming against a real
  MPP build before wiring `IMPORTED_*` properties (tracked in WP-010).
- **GLEW/OpenGL on Linux:** GLEW 2.3.1's CMake supports Linux, but the CI image needs
  X11/GL dev packages (`libgl1-mesa-dev`).
- **Scheduler behaviour drift:** µs budget logic is timing-sensitive — WP-007 plus a
  byte-for-byte identical algorithm should keep this safe.
- **Warning flood:** `/W4` → `-Wall -Wextra` will surface new warnings in
  Common/Application on GCC/Clang; budget time to fix (or selectively suppress) without
  changing behaviour (WP-004, WP-011).
