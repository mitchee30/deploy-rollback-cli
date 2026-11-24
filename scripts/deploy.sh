#!/usr/bin/env bash
# Demo deploy hook for deploy-cli: installs a release into the target and
# starts it.
#
#   usage: deploy.sh <target_dir>
#   release: $DEMO_RELEASE (default v2.0, the broken one), from ../releases/
#
# Contract with deploy-cli: exit 0 = installed and started. Anything else
# (or a crash, or a timeout) makes deploy-cli run rollback.sh. Whether the
# new version is *healthy* is decided afterwards by health_check.sh.
#
# The invariant that makes rollback safe from any point of this script:
#
#   The target is not modified until a complete backup of it has been
#   committed (copied to a temporary directory, then renamed into place).
#
# So if rollback.sh finds a backup, it is complete; if it finds none, the
# target was never touched. The backup is also tagged with the deployment's
# $DEPLOYGUARD_RUN_ID, so rollback.sh can check it is restoring *this*
# deployment's backup and not a leftover (deploy-cli deletes leftovers
# before each deployment too - two independent safeguards).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

TARGET="${1:?usage: deploy.sh <target_dir>}"
TARGET="${TARGET%/}"
RELEASE="${DEMO_RELEASE:-v2.0}"
RELEASE_DIR="$(dirname "$SCRIPT_DIR")/releases/$RELEASE"

[ -d "$TARGET" ] || die "target directory $TARGET does not exist"
[ -x "$RELEASE_DIR/app.sh" ] || die "release $RELEASE not found in $(dirname "$RELEASE_DIR")"

BACKUP="$(backup_dir_for "$TARGET")"
log "deploying $RELEASE to $TARGET"

# 1. Back up the current version - hidden files included - tag it with this
#    deployment's run id, and commit it with an atomic rename. (Also clears
#    leftovers when this script is run on its own, without deploy-cli.)
rm -rf "$BACKUP" "$BACKUP.tmp" "$BACKUP.run_id"
mkdir -p "$BACKUP.tmp"
cp -a "$TARGET/." "$BACKUP.tmp/"
printf '%s\n' "${DEPLOYGUARD_RUN_ID:-}" >"$BACKUP.run_id"
mv "$BACKUP.tmp" "$BACKUP"
log "backup committed: $BACKUP${DEPLOYGUARD_RUN_ID:+ (deployment $DEPLOYGUARD_RUN_ID)}"

# 2. Stop the running version (this target's app only, via its pid file).
stop_app "$TARGET"

# 3. Replace the files.
clear_dir "$TARGET"
cp -a "$RELEASE_DIR/." "$TARGET/"
log "installed $RELEASE"

# 4. Start the new version and check that the process survives startup.
start_app "$TARGET"
sleep 1
app_is_running "$TARGET" || die "$RELEASE exited during startup; see $(app_log_for "$TARGET")"
log "$RELEASE is running"
