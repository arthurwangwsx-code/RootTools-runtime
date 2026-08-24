#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p build/tests
clang -std=c11 -Wall -Wextra -Werror -I Daemon Tests/control_plane_test.c Daemon/control_plane.c -o build/tests/control-plane-test
build/tests/control-plane-test
