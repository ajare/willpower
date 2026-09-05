#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: ./build_from_scratch.sh [options]

Configure and build Willpower and its required dependencies from scratch.

Options:
  --with-mpp-lfs       Download MassivePolyPusher's Git LFS files.
  --build-type TYPE    CMake build type (default: Release).
  --build-dir DIR      Build directory, relative to the repository root unless
                       absolute (default: build).
  -h, --help           Show this help.

Environment:
  CC, CXX               Select the C and C++ compilers during configuration.
  CMAKE_GENERATOR       Select a CMake generator.
  CMAKE_BUILD_PARALLEL_LEVEL
                        Limit the number of parallel build jobs.
EOF
}

fail() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

WITH_MPP_LFS=false
BUILD_TYPE=Release
BUILD_DIR=build

while (($#)); do
    case "$1" in
        --with-mpp-lfs)
            WITH_MPP_LFS=true
            shift
            ;;
        --build-type)
            (($# >= 2)) || fail "--build-type requires a value"
            BUILD_TYPE=$2
            shift 2
            ;;
        --build-dir)
            (($# >= 2)) || fail "--build-dir requires a value"
            BUILD_DIR=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "unknown option: $1 (run with --help for usage)"
            ;;
    esac
done

command -v git >/dev/null 2>&1 || fail "Git is required but was not found on PATH"
command -v cmake >/dev/null 2>&1 || fail "CMake is required but was not found on PATH"

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
cd "$ROOT_DIR"

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || fail "$ROOT_DIR is not a Git checkout"
[[ -f CMakeLists.txt && -f .gitmodules ]] || fail "run this script from the Willpower checkout"

if [[ "$WITH_MPP_LFS" == true ]] && ! git lfs version >/dev/null 2>&1; then
    cat >&2 <<'EOF'
error: --with-mpp-lfs requires Git LFS, but 'git lfs' is not installed.

Install Git LFS, then run this script again:
  Ubuntu/Debian: sudo apt update && sudo apt install git-lfs
  Fedora:        sudo dnf install git-lfs
  macOS:         brew install git-lfs
  Windows:       winget install GitHub.GitLFS

For other systems, see https://git-lfs.com/.
EOF
    exit 1
fi

printf 'Synchronizing and checking out all submodules...\n'
git submodule sync --recursive
git submodule update --init --recursive

SUBMODULE_STATUS=$(git submodule status --recursive)
printf '%s\n' "$SUBMODULE_STATUS"
if grep -Eq '^[+-U]' <<<"$SUBMODULE_STATUS"; then
    fail "one or more submodules are not checked out at the commits recorded by their parent"
fi

MPP_DIR="$ROOT_DIR/ext/massive-poly-pusher"
[[ -f "$MPP_DIR/CMakeLists.txt" ]] || fail "MassivePolyPusher was not checked out correctly"

if [[ "$WITH_MPP_LFS" == true ]]; then
    printf 'Downloading MassivePolyPusher Git LFS files...\n'
    git -C "$MPP_DIR" lfs install --local
    git -C "$MPP_DIR" lfs pull
fi

if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$ROOT_DIR/$BUILD_DIR"
fi

[[ -n "$BUILD_DIR" && "$BUILD_DIR" != / && "$BUILD_DIR" != "$ROOT_DIR" ]] || \
    fail "refusing to remove unsafe build directory: $BUILD_DIR"

printf 'Removing previous build output...\n'
rm -rf -- "$BUILD_DIR" "$MPP_DIR/build"

printf 'Configuring %s build in %s...\n' "$BUILD_TYPE" "$BUILD_DIR"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

printf 'Building Willpower and dependencies...\n'
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel

printf 'Build completed successfully.\n'
