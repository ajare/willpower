 Analysis: Windows-specific code in Willpower.Application

 1. Source code (the module itself)

 The module is mostly already portable — the resource system, input, states, and service location use only std::filesystem, <fstream>, std::chrono, and forward-slash paths. The
 Windows coupling is concentrated in four spots:

 ### 🔴 Hard blockers — Scheduler / SchedulerTask

 - include/willpower/application/Scheduler.h, include/willpower/application/SchedulerTask.h — #include <windows.h>, store LARGE_INTEGER, and the whole class is wrapped in #if
   WP_PLATFORM == WP_PLATFORM_WINDOWS with a literal #error "…supported only on Windows." in the #else branch.
 - src/Scheduler.cpp, src/SchedulerTask.cpp — use QueryPerformanceFrequency() / QueryPerformanceCounter() (Win32 QPC) for the timeslice budgeting.
 - SchedulerTask.cpp also calls ASSERT_TRACE from willpower/common/WillpowerWalker.h — see §3, that macro is only defined on Windows, so it's a second-order blocker.

 ### 🟡 FMOD audio — platform-gated unnecessarily

 - AudioSystem.h/.cpp, resourcesystem/AudioBankResource.h/.cpp — the FMOD backend is gated on WP_PLATFORM == WP_PLATFORM_WINDOWS && defined(WP_APPLICATION_USE_FMOD). FMOD itself
   ships Linux/macOS SDKs, so the Windows check is gratuitous. The no-op fallback for non-FMOD builds is a good pattern to keep.

 ### 🟡 ImageResource.cpp

 - #include <GL/glew.h> and GL constants — portable API, but on Linux the build side must provide GLEW + link OpenGL (MPP's GLEW build already does; just needs the include dir
   and OpenGL::GL to propagate).

 ### ✅ Already cross-platform

 - Platform.h (OGRE-style) — has WP_PLATFORM_LINUX/WP_PLATFORM_APPLE, visibility attributes, and is fine.
 - Resource system (Resource, ResourceManager, ResourceLocation, DirectoryResourceLocation, DataStream, all factories), input, State*, ApplicationSettings, ServiceLocator, tests
   — nothing platform-specific (test YAML uses forward slashes).

 2. CMake build system

 ┌────────────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
 │ Location                           │ Windows-specific item                                                                                                                    │
 ├────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
 │ CMakeLists.txt (root)              │ if(NOT MSVC) FATAL_ERROR — the primary gate; MSVC-only MemCheck config (ASan runtime DLL staging, /RTC1 surgery); global UNICODE         │
 │                                    │ _UNICODE defines                                                                                                                         │
 ├────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
 │ cmake/Helpers.cmake                │ willpower_target_defaults: /MP, MSVC_RUNTIME_LIBRARY, PDB_OUTPUT_DIRECTORY; willpower_enable_sdl_checks: /sdl (MSVC-only flag, breaks    │
 │                                    │ GCC/Clang); willpower_deploy_runtime_dlls: $<TARGET_RUNTIME_DLLS> (MSVC-only genex)                                                      │
 ├────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
 │ cmake/Dependencies.cmake           │ willpower_import_mpp() hard-codes IMPORTED_IMPLIB … .lib / IMPORTED_LOCATION … .dll under bin/{Debug,Release,MemCheck}; FMOD section:    │
 │                                    │ if(NOT WIN32) FATAL_ERROR + api/*/lib/x64 VC paths                                                                                       │
 ├────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
 │ willpower.application/CMakeLists.t │ /W4 (MSVC-only warning level)                                                                                                            │
 │ xt                                 │                                                                                                                                          │
 ├────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
 │ willpower.common/CMakeLists.txt    │ unconditionally defines WIN32 _WINDOWS _USRDLL, compiles StackWalker.cpp, /W4                                                            │
 └────────────────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

 3. Upstream dependency Willpower.Common (must be fixed first — Application links it)

 - StackWalker.cpp/.h + WillpowerWalker.h — full Win32 DBH/PSAPI stack walker. Critically, WillpowerWalker.h defines ASSERT_TRACE (and WillpowerWalker/StackWalkerInstance) only
   inside #if WP_PLATFORM == WP_PLATFORM_WINDOWS, so on Linux the macro is simply undefined → SchedulerTask.cpp won't compile even after the QPC work.
 - src/Logger.cpp — localtime_s() (MSVC CRT; not in glibc).
 - include/willpower/common/Exceptions.h — #if _MSC_VER >= 1930 gates <source_location> (compiles on GCC but should use __cpp_lib_source_location).
 - Already portable: Timer (std::chrono), FileSystem (std::filesystem), Platform.h.

 4. massive-poly-pusher submodule (good news)

 Its CMake is already cross-platform: WIN32 only gates the GUI app targets (MppAppSupport, PipelineEditor), GLEW is hash-pinned and built from source (works on Linux), draco
 target naming is handled, and the library targets Willpower needs (MassivePolyPusher, MppMesh, MppProgram, Utils) contain no direct windows.h in their public headers/sources.
 Its outputs land in <mpp-build>/bin/*.so on Linux (single-config) vs bin/<Config>/*.dll on MSVC multi-config — the import logic in Dependencies.cmake needs to branch on this.

 ────────────────────────────────────────────────────────────────────────────────

 Plan

 ### Phase 0 — Scope & success criteria

 - Target: Linux first (Ubuntu 22.04+, GCC 12+ and Clang 15+), keeping the Windows/MSVC build green throughout.
 - Success = cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure passes on Linux for the Application module and its tests.
 - Decision: keep shared-library build (match WP_APPLICATION_API visibility attributes) — no static-lib mode work yet.

 ### Phase 1 — Willpower.Common (prerequisite)

 1. Stack walking: gate StackWalker.{h,cpp} compilation to Windows; provide a portable WillpowerWalker fallback on Linux (no-op or backtrace()-based). Always define ASSERT_TRACE
    (plain assert fallback off-Windows) — this unblocks SchedulerTask.
 2. Logger.cpp: portable local time — localtime_r on POSIX, localtime_s on MSVC.
 3. Exceptions.h: switch the source_location guard to __cpp_lib_source_location.
 4. CMake: compile StackWalker.cpp and define WIN32 _WINDOWS _USRDLL only if(WIN32); make warning flags compiler-aware (/W4 → -Wall -Wextra on GCC/Clang).

 ### Phase 2 — Willpower.Application sources

 1. Scheduler/SchedulerTask: replace QPC with std::chrono::steady_clock (ns resolution on glibc is fine for µs budgeting). Add a small internal helper (e.g. HighResTimer in a new
    src/PlatformTimer.{h,cpp} or reuse patterns) so the class bodies stay clean. Delete <windows.h>, the LARGE_INTEGER members, and both #error guards — the classes become
    unconditional. Keep the budget-scaling algorithm byte-for-byte.
 2. Audio: change the gate in AudioSystem/AudioBankResource from WP_PLATFORM == WP_PLATFORM_WINDOWS && defined(WP_APPLICATION_USE_FMOD) to just defined(WP_APPLICATION_USE_FMOD).
    Non-FMOD no-op stays the default on all platforms.
 3. Add a Scheduler unit test (task ordering + budget scaling) so the rewrite is verified, mirroring the existing test layout in willpower.application/tests.
 4. No changes expected in Platform.h or the resource system.

 ### Phase 3 — CMake cross-platformization

 1. Root CMakeLists.txt: drop the MSVC FATAL_ERROR (accept MSVC/GNU/Clang, 64-bit check stays); wrap the whole MemCheck/ASan-DLL block in if(MSVC AND CMAKE_CONFIGURATION_TYPES);
    scope UNICODE _UNICODE to WIN32 (harmless but tidy).
 2. cmake/Helpers.cmake:
     - willpower_target_defaults: generator-express the MSVC-only properties (MSVC_RUNTIME_LIBRARY, PDB_OUTPUT_DIRECTORY, /MP, /fsanitize=address) behind if(MSVC); add -Wall
       -Wextra for GNU/Clang.
     - willpower_enable_sdl_checks: no-op off MSVC.
     - willpower_deploy_runtime_dlls: keep $<TARGET_RUNTIME_DLLS> staging on MSVC; on GCC/Clang rely on build-tree RPATH (default) and add a post-build copy of linked .sos next
       to test executables if RPATH proves insufficient.
 3. cmake/Dependencies.cmake: platform-branch willpower_import_mpp() — Windows keeps the current .lib/.dll + config layout; Linux uses single-config IMPORTED_LOCATION
    "${_mpp_bin}/${stem}.so" (verify exact path after first Linux configure, since MPP outputs to flat bin/). FMOD: either extend find_* paths per-platform (api/core/lib/linux,
    etc.) or keep the explicit WIN32 error — recommend the latter for now, since the no-op default already covers Linux.
 4. Module CMakeLists (application, common): compiler-aware warning flags as in step 2.

 ### Phase 4 — Verification & docs

 - CI: add a Linux job (Ubuntu, GCC and Clang) running configure + build + ctest; keep the Windows job.
 - Document Linux prerequisites: cmake, g++/clang++, libgl1-mesa-dev (OpenGL for GLEW), git submodules.
 - Update README.md requirements section.

 ### Phase 5 — Follow-ups (explicitly out of scope for this pass)

 - willpower.viz (renderer; same treatment, larger effort).
 - macOS (WP_PLATFORM_APPLE exists but is untested).
 - FMOD backend on Linux (needs the proprietary Linux SDK).
 - Static-library build mode / WP_*_STATIC_LIB.

 ### Risks

 - MPP import paths: exact Linux artifact locations need confirming against a real MPP build before wiring IMPORTED_* properties.
 - GLEW/OpenGL on Linux: GLEW 2.3.1's CMake supports Linux, but the CI image needs X11/GL dev packages.
 - Behaviour drift in Scheduler: µs budget logic is timing-sensitive — the new unit test plus identical algorithm should keep this safe.
 - /W4 → -Wall -Wextra will surface a wave of new warnings in Common/Application on GCC/Clang; budget time to fix (or selectively suppress) without changing behaviour.
