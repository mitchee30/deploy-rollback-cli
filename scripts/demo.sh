#!/usr/bin/env bash
# End-to-end demo of deploy-cli:
#
#   1. install v1.0                                       (setup_v1.sh)
#   2. deploy v1.1: healthy, committed                    -> exit code 0
#   3. deploy v2.0: health check fails, rolled back to 1.1 -> exit code 1
#
#   usage: scripts/demo.sh            (build first: scripts/build.sh)
#          DEPLOY_CLI=/path/to/deploy-cli scripts/demo.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT="$(dirname "$SCRIPT_DIR")"
CLI="${DEPLOY_CLI:-$ROOT/build/deploy-cli}"
TARGET="$ROOT/test_app_target"
[ -x "$CLI" ] || die "deploy-cli not found at $CLI - run scripts/build.sh first"
cd "$ROOT"

if [ -t 1 ]; then BOLD=$'\033[1m'; RESET=$'\033[0m'; else BOLD=""; RESET=""; fi
step() { printf '\n%s==== %s ====%s\n' "$BOLD" "$*" "$RESET"; }

# deploy <release>: runs deploy-cli and records its exit code in $CODE.
deploy() {
    CODE=0
    DEMO_RELEASE="$1" "$CLI" --config deploy.yaml || CODE=$?
    printf '%s-> deploy-cli exit code: %s%s\n' "$BOLD" "$CODE" "$RESET"
}

step "1/3  Install v1.0"
"$SCRIPT_DIR/setup_v1.sh"

step "2/3  Deploy v1.1 (a healthy release)"
deploy v1.1
CODE_V11=$CODE

step "3/3  Deploy v2.0 (starts, but fails its health check)"
deploy v2.0
CODE_V20=$CODE

step "Result"
echo "deploy v1.1 -> exit $CODE_V11 (expected 0: deployed)"
echo "deploy v2.0 -> exit $CODE_V20 (expected 1: failed, rolled back)"
echo "running now  -> version $("$TARGET/app.sh" --version)"
echo "full log     -> $ROOT/deploy_guard.log"

stop_app "$TARGET" >/dev/null
[ "$CODE_V11" -eq 0 ] && [ "$CODE_V20" -eq 1 ]
