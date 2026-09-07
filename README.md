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

Willpower supports 64-bit Windows and Linux builds. A 32-bit configuration is rejected.

- CMake 3.25 or newer
- Git, for obtaining submodules
- A C++20 compiler:
  - Windows: Microsoft Visual C++
  - Linux: GCC 12 or newer, or Clang 15 or newer
- Linux OpenGL development packages required by GLEW

On Ubuntu 22.04 or newer, install the Linux prerequisites with:

```bash
sudo apt update
sudo apt install build-essential cmake git libgl1-mesa-dev libglu1-mesa-dev
```

For a Clang build, also install `clang`.

## Cloning

Clone recursively so that both Willpower's dependencies and MassivePolyPusher's nested dependencies are populated:

```bash
git clone --recurse-submodules <repository-url>
cd willpower
```

For an existing clone:

```bash
git submodule update --init --recursive
```

The repository declares these direct submodules:

- [MassivePolyPusher](https://github.com/ajare/massive-poly-pusher) — rendering APIs and Utils; it owns nested SDL, GLEW, Assimp, Utils, and yaml-cpp dependencies
- [earcut.hpp](https://github.com/mapbox/earcut.hpp) — header-only polygon triangulation
- [SplineLibrary](https://github.com/ejmahler/SplineLibrary) — header-only spline implementations

## Building

### Complete build from scratch

The following sequence starts with a new checkout and builds Willpower and every dependency it needs. Do not build the submodules in separate build trees: the top-level CMake build configures MassivePolyPusher, and MassivePolyPusher in turn adds its nested dependencies in the required order.

After cloning, the root-level script automates the complete process:

```bash
./build_from_scratch.sh
```

Pass `--with-mpp-lfs` to also download MassivePolyPusher's Git LFS assets. This option reports platform-specific installation instructions and exits if Git LFS is unavailable. Run `./build_from_scratch.sh --help` for build type, build directory, and compiler selection details.

To perform the same process manually:

1. Clone Willpower and enter the checkout:

   ```bash
   git clone <repository-url> willpower
   cd willpower
   ```

2. Populate the submodules, including all nested submodules:

   ```bash
   git submodule sync --recursive
   git submodule update --init --recursive
   ```

   The recursive command resolves the checkout hierarchy in this order:

   1. `ext/SplineLibrary` and `ext/earcut.hpp` (header-only)
   2. `ext/massive-poly-pusher`
   3. MassivePolyPusher's `ext/sdl`, `ext/glew`, `ext/assimp`, and `ext/utils`
   4. Utils' `vendor/yaml-cpp`

   To initialize the same hierarchy explicitly, which can be useful when diagnosing a failed recursive checkout, run:

   ```bash
   git submodule update --init ext/SplineLibrary ext/earcut.hpp ext/massive-poly-pusher
   git -C ext/massive-poly-pusher submodule update --init ext/sdl ext/glew ext/assimp ext/utils
   git -C ext/massive-poly-pusher/ext/utils submodule update --init vendor/yaml-cpp
   ```

3. Configure a fresh build tree and build from the repository root:

   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --parallel
   ```

   On the first build, CMake configures MassivePolyPusher in `build/_deps`. CMake's target dependencies then build yaml-cpp and Utils, GLEW, the required MassivePolyPusher libraries, and finally the Willpower modules. SplineLibrary and earcut.hpp are header-only; Assimp supplies the Poly2Tri sources used by Willpower. No manual dependency build or install step is required.

For a completely clean rebuild of an existing checkout, remove the generated tree, refresh all submodules, and repeat configuration:

```bash
rm -rf build
# Remove ignored build output produced inside the MPP checkout, if present.
rm -rf ext/massive-poly-pusher/build
git submodule sync --recursive
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

On PowerShell, use `Remove-Item -Recurse -Force build, ext/massive-poly-pusher/build` in place of the two `rm` commands (omit paths that do not exist).

### Linux

Configure and build with GCC:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

To use Clang, select it when creating a fresh build directory:

```bash
CC=clang CXX=clang++ cmake -S . -B build-clang -DCMAKE_BUILD_TYPE=Release
cmake --build build-clang --parallel
```

Use `-DCMAKE_BUILD_TYPE=Debug` for a debug build. Use
`-DCMAKE_BUILD_TYPE=Shipping` for maximum Release optimisations; on Windows,
Shipping also statically links the Visual C++ runtime.

### Windows

From a Visual Studio developer shell:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug --parallel
```

Use `--config Release` for a release build, or `--config Shipping` for maximum
Release optimisations and a statically linked Visual C++ runtime. Shipping
executables therefore do not require the Visual C++ Redistributable.

Build outputs are placed under:

```text
build/bin/<Config>/<Target>/   # Shared libraries and executables
build/lib/<Config>/<Target>/   # Import/static library artifacts
```

For single-config Linux generators, `<Config>` is the value of `CMAKE_BUILD_TYPE`.

### Optional FMOD support

Audio compiles to a no-op backend by default, so a proprietary SDK is not needed for a complete build. To enable the FMOD backend, supply the FMOD Engine API separately and name each path it is made of:

```powershell
$sdk = "C:/path/to/FMOD Studio API Windows"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DWILLPOWER_ENABLE_FMOD=ON `
  -DWILLPOWER_FMOD_CORE_INCLUDE="$sdk/api/core/inc" `
  -DWILLPOWER_FMOD_STUDIO_INCLUDE="$sdk/api/studio/inc" `
  -DWILLPOWER_FMOD_CORE_LIBRARY="$sdk/api/core/lib/x64/fmod_vc.lib" `
  -DWILLPOWER_FMOD_STUDIO_LIBRARY="$sdk/api/studio/lib/x64/fmodstudio_vc.lib" `
  -DWILLPOWER_FMOD_CORE_DLL="$sdk/api/core/lib/x64/fmod.dll" `
  -DWILLPOWER_FMOD_STUDIO_DLL="$sdk/api/studio/lib/x64/fmodstudio.dll"
```

On Linux, use the shared objects from the Linux SDK; separate runtime paths are not required:

```bash
sdk="/path/to/FMOD Studio API Linux"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DWILLPOWER_ENABLE_FMOD=ON \
  -DWILLPOWER_FMOD_CORE_INCLUDE="$sdk/api/core/inc" \
  -DWILLPOWER_FMOD_STUDIO_INCLUDE="$sdk/api/studio/inc" \
  -DWILLPOWER_FMOD_CORE_LIBRARY="$sdk/api/core/lib/x86_64/libfmod.so" \
  -DWILLPOWER_FMOD_STUDIO_LIBRARY="$sdk/api/studio/lib/x86_64/libfmodstudio.so"
```

FMOD cannot be distributed as a public Git submodule. The paths are given individually rather than as a single root because the Engine API is not always laid out as the SDK installer leaves it — a project that vendors it into its own tree works just as well. Configuring with any required path missing lists exactly which. On Windows, runtime DLLs are staged beside test executables; Linux executables use CMake's build-tree runtime path.

## Resource Manifest validation

`Willpower.Application` validates every YAML Resource Manifest while
`ResourceLocation::scan()` or `rescan()` loads it. YAML is parsed once and checked before
it is converted to `StructuredData`, before Resource records are published, and before
filesystem or cross-Resource checks. A failed initial scan publishes no records; a
failed rescan retains the last successful records and can be retried after the manifest
is corrected.

The canonical JSON Schema Draft 7 sources are in
[`willpower.application/schemas`](willpower.application/schemas). Start with
[`resource-manifest.schema.json`](willpower.application/schemas/resource-manifest.schema.json),
which composes the common declaration schema and all nine built-in Resource Type
schemas. CMake embeds this catalog into `Willpower.Application`; deployed programs do
not need schema files beside the executable and schema resolution performs no network
access.

CMake also generates the language-neutral **Resource Schema Bundle** target
`willpower_resource_schema_bundle`. Its catalog and self-contained schema documents are
available in the build tree at
`<build-tree>/willpower.application/resource-schema-bundle`. `cmake --install` installs
the bundle to `<prefix>/<datadir>/willpower/resource-schema-bundle` (normally
`<prefix>/share/willpower/resource-schema-bundle`). External tools begin with
`catalog.json`; each entry identifies its Resource Type and optional factory, document,
stable schema ID, and SHA-256 digest. The complete format, offline-resolution rules,
lookup fallback, and compatibility policy are specified in
[`docs/specifications/resource-schema-bundle.md`](docs/specifications/resource-schema-bundle.md).

The schemas preserve the loader's compatibility forms:

- `Resource`, `Namespace`, options, dependencies, Definitions, and nested collections
  accept either one mapping or a non-empty sequence where the loader historically did;
- numeric and boolean fields accept native YAML scalars and the documented quoted forms;
- default Definitions are checked against their built-in Resource Type, while a
  Definition with a non-empty `factory` is checked only as a generic specialized
  Definition until that factory publishes a schema; and
- unknown custom Resource Types receive common declaration validation so plugin
  manifests remain loadable.

Structural validation covers document shape, required and unknown fields, supported
enums, and scalar syntax/ranges. Semantic validation remains separate and later:
`validateResourceDefinitions()` and `ResourceManager` check source-file existence,
duplicate declarations, dependency resolution and cycles, loaded image bounds, and
references into other Resources. Source asset contents (for example XML, image, shader,
or audio data) are validated only when the corresponding Resource is created or loaded.

Validation failures throw `ResourceManifestValidationException`. Malformed YAML is
reported as `YAML syntax`; schema-invalid YAML reports up to 100 failures in deterministic
document order. Each available diagnostic includes the manifest path, namespace and
Resource identity, JSON instance path, and one-based YAML line and column. To fix a
manifest, go to the reported position/path, correct the structural rule, and rerun the
scan; if structural loading succeeds but semantic validation fails, fix the referenced
file or Resource relationship instead.

The repository's [VS Code settings](.vscode/settings.json) associate
`**/Resources.yaml` and `**/Resources.yml` with the canonical root schema when the Red
Hat YAML extension (or another setting-compatible YAML language server) is installed.
This editor support adds no production dependency.

## Tests

Tests are enabled by default. Build and run the suite on Linux with:

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On Windows, specify the selected multi-config configuration and disable
interactive system-debug dialogs so crashes cannot block unattended runs:

```powershell
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --interactive-debug-mode 0 --output-on-failure
```

The suite covers acceleration-grid set operations, removal of legacy geometry helpers, static-line clipping, scheduling, input state, and YAML resource manifests. Test data is self-contained under `willpower.application/tests/data`; no parent project or external resource tree is required.

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
