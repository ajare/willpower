# Tickets

## Willpower.Application cross-platform initiative (Linux first)

Source of truth: [`PLAN.md`](../../PLAN.md). Goal: make `Willpower.Application` (and its
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
| [WP-003](WP-003.md) | [#3](https://github.com/ajare/willpower/issues/3) | 1 — Common | P1 | easy | Feature-test guard for `source_location` in `Exceptions.h` | — | done |
| [WP-004](WP-004.md) | [#4](https://github.com/ajare/willpower/issues/4) | 1 — Common | P0 | medium | Platform-aware Common CMake (defines, sources, warnings) | WP-001 | done |
| [WP-005](WP-005.md) | [#5](https://github.com/ajare/willpower/issues/5) | 2 — Application | P0 | hard | Portable `Scheduler`/`SchedulerTask` timing (QPC → `steady_clock`) | WP-001 | done |
| [WP-006](WP-006.md) | [#6](https://github.com/ajare/willpower/issues/6) | 2 — Application | P1 | easy | Drop the Windows gate on the FMOD audio backend | — | done |
| [WP-007](WP-007.md) | [#7](https://github.com/ajare/willpower/issues/7) | 2 — Application | P0 | medium | `Scheduler` unit test (ordering + budget scaling) | WP-005 | done |
| [WP-008](WP-008.md) | [#8](https://github.com/ajare/willpower/issues/8) | 3 — CMake | P0 | easy | Root CMake: drop MSVC-only gates | — | done |
| [WP-009](WP-009.md) | [#9](https://github.com/ajare/willpower/issues/9) | 3 — CMake | P0 | medium | `cmake/Helpers.cmake`: platform-aware target helpers | WP-008 | done |
| [WP-010](WP-010.md) | [#10](https://github.com/ajare/willpower/issues/10) | 3 — CMake | P0 | medium | `cmake/Dependencies.cmake`: branch MPP import; keep FMOD WIN32 gate | WP-008 | done |
| [WP-011](WP-011.md) | [#11](https://github.com/ajare/willpower/issues/11) | 3 — CMake | P1 | easy | Application module CMake: compiler-aware warnings | WP-009 | done |
| [WP-012](WP-012.md) | [#12](https://github.com/ajare/willpower/issues/12) | 4 — Verify | P0 | medium | Linux end-to-end build + `ctest` (acceptance gate) | all above | done |
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

---

## Resource Manifest schema-validation initiative

Source of truth: [`resource-manifest-schema-validation.md`](../specifications/resource-manifest-schema-validation.md).
Goal: validate every YAML Resource Manifest during `ResourceLocation::scan()` and
`rescan()`, before conversion to `StructuredData` or publication of resource records.
This work supports, but does not block on, [issue #24](https://github.com/ajare/willpower/issues/24)
for a Resource Manifest Editor.

| Ticket | Issue | Phase | Priority | Difficulty | Title | Blocked by | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [WP-025](WP-025.md) | [#25](https://github.com/ajare/willpower/issues/25) | Infrastructure | P1 | medium | Add Valijson and an embedded schema catalog | — | done |
| [WP-026](WP-026.md) | [#26](https://github.com/ajare/willpower/issues/26) | Loader integration | P1 | hard | Validate Resource Manifests atomically during scan/rescan | WP-025 | done |
| [WP-027](WP-027.md) | [#27](https://github.com/ajare/willpower/issues/27) | Built-in schemas | P1 | medium | Source-backed Resource Type schemas | WP-025, WP-026 | done |
| [WP-028](WP-028.md) | [#28](https://github.com/ajare/willpower/issues/28) | Built-in schemas | P1 | hard | ImageSet and AnimationSet schemas | WP-025, WP-026 | done |
| [WP-029](WP-029.md) | [#29](https://github.com/ajare/willpower/issues/29) | Built-in schemas | P1 | hard | Program and Material schemas | WP-025, WP-026 | done |
| [WP-030](WP-030.md) | [#30](https://github.com/ajare/willpower/issues/30) | Verification & docs | P1 | medium | Regression suite and documentation | WP-026–WP-029 | done |

### Suggested execution order

1. WP-025 establishes raw-YAML validation, embedding, and the schema catalog.
2. WP-026 integrates common validation and atomic state publication into the loader.
3. WP-027, WP-028, and WP-029 can then proceed in parallel.
4. WP-030 is the end-to-end acceptance and documentation gate.

---

## Resource schema export initiative

Goal: distribute the built-in Resource Type schemas as a portable Resource Schema Bundle,
allow downstream applications to compose and export schemas for their own Resource
subclasses, and make the same catalog available to runtime validation and external tools.
Static bundles are the interoperability contract; native schema plugins are optional.

| Ticket | Issue | Phase | Priority | Difficulty | Title | Blocked by | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [WP-031](WP-031.md) | [#31](https://github.com/ajare/willpower/issues/31) | Bundle contract | P1 | medium | Define the bundle format and package built-in schemas | — | open |
| [WP-032](WP-032.md) | [#32](https://github.com/ajare/willpower/issues/32) | Catalog API | P1 | hard | Add a public Resource Schema Catalog API | WP-031 | open |
| [WP-033](WP-033.md) | [#33](https://github.com/ajare/willpower/issues/33) | Build integration | P1 | hard | Compose downstream application schema bundles | WP-031 | open |
| [WP-034](WP-034.md) | [#34](https://github.com/ajare/willpower/issues/34) | Runtime integration | P1 | hard | Validate custom Resource Types from the catalog | WP-032 | open |
| [WP-035](WP-035.md) | [#35](https://github.com/ajare/willpower/issues/35) | Tooling | P1 | medium | Export and compose Resource Schema Bundles | WP-032, WP-033 | open |
| [WP-036](WP-036.md) | [#36](https://github.com/ajare/willpower/issues/36) | Dynamic discovery | P2 | hard | Add an optional C ABI for schema plugins | WP-031, WP-032 | done |
| [WP-037](WP-037.md) | [#37](https://github.com/ajare/willpower/issues/37) | Verification & docs | P1 | medium | Integration tests and documentation | WP-033–WP-035 | done |

### Suggested execution order

1. WP-031 fixes the portable bundle contract and exports the built-in catalog.
2. WP-032 and WP-033 can then implement the public catalog and downstream build integration in parallel.
3. WP-034 integrates custom schemas into runtime validation; WP-035 adds export tooling after the catalog and composition APIs exist.
4. WP-037 is the end-to-end acceptance and documentation gate.
5. WP-036 is optional P2 work and can proceed after WP-032 without blocking static bundle support.

---

## Resource Manifest Editor initiative

The parent product is [#24](https://github.com/ajare/willpower/issues/24). Its shared
document foundation remains usable headlessly while the desktop shell and later editing
work build on the same validation contract.

| Ticket | Issue | Phase | Priority | Difficulty | Title | Blocked by | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| [WP-038](WP-038.md) | [#38](https://github.com/ajare/willpower/issues/38) | Foundation | P1 | hard | Validate and round-trip manifests from the CLI | — | done |
| [WP-039](WP-039.md) | [#39](https://github.com/ajare/willpower/issues/39) | Desktop shell | P1 | hard | Create, open, and save manifests in the desktop shell | WP-038 | done |
| [WP-040](WP-040.md) | [#40](https://github.com/ajare/willpower/issues/40) | Authoring | P1 | hard | Author file-backed Resources from built-in schemas | WP-039 | done |
| [WP-041](WP-041.md) | [#41](https://github.com/ajare/willpower/issues/41) | Organization | P1 | hard | Organize Resources in flat namespaces safely | WP-040 | done |
| [WP-042](WP-042.md) | [#42](https://github.com/ajare/willpower/issues/42) | Dependencies | P1 | hard | Author dependencies and inline Resources | WP-040, WP-041 | done |
| [WP-043](WP-043.md) | [#43](https://github.com/ajare/willpower/issues/43) | Composite authoring | P1 | hard | Author ImageSet and AnimationSet Definitions | WP-042 | done |
| [WP-044](WP-044.md) | [#44](https://github.com/ajare/willpower/issues/44) | Advanced authoring | P1 | hard | Author Program, Material, and specialized Definitions | WP-043 | done |
| [WP-045](WP-045.md) | [#45](https://github.com/ajare/willpower/issues/45) | Schema deployment | P1 | hard | Load and reload application Resource Schema Bundles | WP-041, WP-044 | done |
| [WP-046](WP-046.md) | [#46](https://github.com/ajare/willpower/issues/46) | Semantic repair | P1 | hard | Repair semantic errors and enforce save validity | WP-042, WP-045 | done |
