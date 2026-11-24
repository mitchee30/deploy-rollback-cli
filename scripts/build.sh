#!/usr/bin/env bash
# Configures and builds deploy-cli and its tests in ./build.
#
#   usage: scripts/build.sh [extra CMake options]
#   e.g.   scripts/build.sh -DDEPLOYGUARD_SANITIZE=ON
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cmake -S "$ROOT" -B "$ROOT/build" "$@"
cmake --build "$ROOT/build" --parallel

echo
echo "Built:      $ROOT/build/deploy-cli"
echo "Run tests:  (cd build && ctest --output-on-failure -j 8)"
echo "Run demo:   scripts/demo.sh"
