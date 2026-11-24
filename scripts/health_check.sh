#!/usr/bin/env bash
# Demo health check hook for deploy-cli: exit 0 if the app in <target_dir> is
# running and reports itself healthy.
#
#   usage: health_check.sh <target_dir>
#
# deploy-cli runs this after deploy.sh succeeded, retries it a few times
# (health_check_attempts / health_check_interval_seconds in deploy.yaml) and
# rolls back if it never passes. $DEPLOYGUARD_ATTEMPT is the attempt number.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

TARGET="${1:?usage: health_check.sh <target_dir>}"
TARGET="${TARGET%/}"

log "attempt ${DEPLOYGUARD_ATTEMPT:-1}: checking $TARGET"
app_is_running "$TARGET" || die "the app is not running"
"$TARGET/app.sh" --health || die "the app reports that it is unhealthy"
log "healthy"
