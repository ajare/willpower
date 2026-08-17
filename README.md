# Willpower

Willpower is a standalone collection of C++20 shared libraries for 2D geometry, collision simulation, application services, resource loading, and debug visualisation. Public APIs live under `include/willpower` in the `wp` namespace.

## Modules

| CMake target | Directory | Purpose |
| --- | --- | --- |
| `Willpower.Common` | `willpower.common` | Maths and 2D primitives, bounds, splines, acceleration grids, structured data, logging, files, and timers. |
| `Willpower.Geometry` | `willpower.geometry` | Polygon and mesh representation, queries, validation, triangulation, offsets, CSG helpers, and mesh-editing operations. |
| `Willpower.Collide` | `willpower.collide` | AABB/circle colliders, static lines, spatial indexing, sweep tests, and collision simulation. |
| `Willpower.Application` | `willpower.application` | Scheduling, input, optional audio, application state, service location, and YAML-driven resource management. |
| `WillPower.Viz` | `willpower.viz` | Renderers for geometry meshes, collision simulations, acceleration grids, lines, quads, and triangles. |

```text
Common ──> Geometry ──> Collide ──> Viz
   └────────────────────> Application
```

## Requirements

- Windows x64
- Microsoft Visual C++ with C++20 support
- CMake 3.25 or newer
- Git, for obtaining submodules

The build currently rejects non-MSVC and 32-bit configurations because the libraries retain Windows DLL interfaces and platform-specific implementation code.

## Cloning

Clone recursively so that both Willpower's dependencies and MassivePolyPusher's nested dependencies are populated:

```powershell
git clone --recurse-submodules <repository-url>
cd willpower
```

For an existing clone:

```powershell
git submodule update --init --recursive
```

The repository declares these direct submodules:

- [MassivePolyPusher](https://github.com/ajare/massive-poly-pusher) — rendering APIs and Utils; it owns nested SDL, GLEW, Assimp, Utils, and yaml-cpp dependencies
- [earcut.hpp](https://github.com/mapbox/earcut.hpp) — header-only polygon triangulation
- [SplineLibrary](https://github.com/ejmahler/SplineLibrary) — header-only spline implementations

## Building

From a Visual Studio developer shell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
```

Use `--config Release` for a release build. On the first build, CMake configures MassivePolyPusher in an independent build tree and builds the library targets required by Willpower. All generated files remain under the selected Willpower build directory.

Build outputs are placed under:

```text
build/bin/<Config>/<Target>/   # DLLs, executables, and staged runtime DLLs
build/lib/<Config>/<Target>/   # Import libraries
```

### Optional FMOD support

Audio compiles to a no-op backend by default, so a proprietary SDK is not needed for a complete build. To enable the FMOD backend, install the FMOD Studio API separately and configure with:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DWILLPOWER_ENABLE_FMOD=ON `
  -DWILLPOWER_FMOD_ROOT="C:/path/to/FMOD Studio API Windows"
```

FMOD cannot be distributed as a public Git submodule. The configured root must contain the standard `api/core` and `api/studio` SDK directories. Required runtime DLLs are staged beside test executables.

## Tests

Build and run the CTest suite with:

```powershell
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

The suite covers acceleration-grid set operations, removal of legacy geometry helpers, static-line clipping, and YAML resource manifests. Test data is self-contained under `willpower.application/tests/data`; no parent project or external resource tree is required.

Disable test targets at configure time with `-DBUILD_TESTING=OFF`.

## Layout

```text
cmake/                          # Standalone dependency and build helpers
ext/                            # Git submodules
willpower.<module>/
├── include/willpower/<module>/ # Public headers
├── src/                        # Implementations
├── tests/                      # Tests, where present
└── CMakeLists.txt              # Shared-library target
```

TinyXML2, PolyPartition, and Clipper are retained as vendored source implementations. All other required third-party source is supplied by the declared submodules.
