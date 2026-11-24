#!/usr/bin/env bash
# Demo rollback hook for deploy-cli: puts the target back the way it was
# before the current deployment.
#
#   usage: rollback.sh <target_dir>
#
# deploy-cli runs this after a failed deployment, and again on its next start
# if it was killed in the middle of a deployment or of a rollback. So this
# script must work no matter how far deploy.sh got, and must be safe to run
# more than once.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/common.sh
source "$SCRIPT_DIR/lib/common.sh"

TARGET="${1:?usage: rollback.sh <target_dir>}"
TARGET="${TARGET%/}"
BACKUP="$(backup_dir_for "$TARGET")"
log "rolling back $TARGET"

if [ -d "$BACKUP" ]; then
    # deploy.sh committed a backup, so it may have changed the target.
    stop_app "$TARGET"
    mkdir -p "$TARGET"
    clear_dir "$TARGET"
    cp -a "$BACKUP/." "$TARGET/"
    log "restored the files from $BACKUP"
else
    # deploy.sh never modifies the target before committing a backup.
    log "no backup from this deployment: the target was not modified"
fi

if [ ! -x "$TARGET/app.sh" ]; then
    # The target was empty before this deployment: nothing to run.
    stop_app "$TARGET"
    rm -rf "$BACKUP"
    log "rollback complete: the target holds no app, as before the deployment"
    exit 0
fi

# Make sure the restored version is running and healthy before deleting the
# backup: if anything below fails, deploy-cli reports it and re-runs this
# hook next time, and it will need the backup again.
if ! app_is_running "$TARGET"; then
    start_app "$TARGET"
fi
wait_until_healthy "$TARGET" 10 ||
    die "the restored app is not healthy; the backup is kept at $BACKUP for manual recovery"

rm -rf "$BACKUP"
log "rollback complete: version $("$TARGET/app.sh" --version) is running and healthy"
