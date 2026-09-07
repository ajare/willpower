#!/usr/bin/env bash

set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: ./build_incremental.sh [--config CONFIG]

Build the existing Willpower build tree incrementally.

Options:
  --config CONFIG    CMake build configuration (default: Release).
  -h, --help         Show this help.
EOF
}

CONFIG=Release

while (($#)); do
    case "$1" in
        --config)
            (($# >= 2)) || { printf 'error: --config requires a value\n' >&2; exit 1; }
            CONFIG=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'error: unknown option: %s (run with --help for usage)\n' "$1" >&2
            exit 1
            ;;
    esac
done

ROOT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
cmake --build "$ROOT_DIR/build-linux" --config "$CONFIG" --parallel
