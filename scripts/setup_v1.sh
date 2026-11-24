#!/usr/bin/env bash
# Resets the demo: a target directory with release v1.0 installed and running.
#
#   usage: setup_v1.sh [target_dir]      (default: <repo>/test_app_target)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

ROOT="$(dirname "$SCRIPT_DIR")"
TARGET="${1:-$ROOT/test_app_target}"
TARGET="${TARGET%/}"

mkdir -p "$TARGET"
stop_app "$TARGET"

# Start from a clean slate: no leftover backup or crash-recovery state.
# (The lock file is left alone; deleting a lock file is never safe.)
_parent_and_name "$TARGET"
rm -rf "$_PARENT/.deploy_guard_${_NAME}.backup" "$_PARENT/.deploy_guard_${_NAME}.backup.tmp"
rm -f "$_PARENT/.deploy_guard_${_NAME}.state" "$_PARENT/.deploy_guard_${_NAME}.state.tmp"

clear_dir "$TARGET"
cp -a "$ROOT/releases/v1.0/." "$TARGET/"
start_app "$TARGET"
wait_until_healthy "$TARGET" 5 || die "v1.0 did not start"
log "v1.0 is installed and running in $TARGET"
