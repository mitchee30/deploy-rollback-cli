# shellcheck shell=bash
# Helpers shared by the demo hooks (deploy.sh, rollback.sh, health_check.sh)
# and setup_v1.sh. Sourced, not executed. Works with the bash 3.2 that ships
# with macOS.

log()  { echo "[$(basename "$0")] $*"; }
warn() { echo "[$(basename "$0")] WARNING: $*" >&2; }
die()  { echo "[$(basename "$0")] ERROR: $*" >&2; exit 1; }

# Files kept *next to* the target directory, never inside it (so wiping or
# restoring the target cannot touch them):
#
#   .deploy_guard_<name>.backup/   pre-deployment backup. deploy-cli passes
#                                  this path as $DEPLOYGUARD_BACKUP_DIR
#   .demo_app_<name>.pid           pid of the running demo app
#   .demo_app_<name>.log           output of the running demo app
_parent_and_name() {
    local target="${1%/}"
    _PARENT="$(cd "$(dirname "$target")" && pwd)"
    _NAME="$(basename "$target")"
}

backup_dir_for() {
    if [ -n "${DEPLOYGUARD_BACKUP_DIR:-}" ]; then
        echo "$DEPLOYGUARD_BACKUP_DIR"   # the location deploy-cli manages
    else
        _parent_and_name "$1"            # standalone run: same naming rule
        echo "$_PARENT/.deploy_guard_${_NAME}.backup"
    fi
}

# Does this backup belong to the deployment being rolled back? deploy.sh
# tags each backup with $DEPLOYGUARD_RUN_ID in "<backup>.run_id" (next to
# the backup, so restoring it never copies the tag into the target). With no
# id on either side - deploy.sh run by hand, or a state file written by
# deploy-cli < 0.4 - the backup is trusted.
backup_matches_run() {
    local backup="$1" tagged
    [ -n "${DEPLOYGUARD_RUN_ID:-}" ] || return 0
    [ -f "$backup.run_id" ] || return 0
    tagged="$(cat "$backup.run_id")"
    [ -z "$tagged" ] || [ "$tagged" = "$DEPLOYGUARD_RUN_ID" ]
}

pid_file_for() { _parent_and_name "$1"; echo "$_PARENT/.demo_app_${_NAME}.pid"; }
app_log_for()  { _parent_and_name "$1"; echo "$_PARENT/.demo_app_${_NAME}.log"; }

# Deletes everything inside a directory, hidden files included
# (`rm -rf dir/*` would skip them).
clear_dir() {
    find "$1" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
}

# Is the app of this target running? (Uses its pid file, so it never mixes
# up two targets - unlike `pgrep -f app.sh`, which matches every app.sh on
# the machine.)
app_is_running() {
    local pid_file pid
    pid_file="$(pid_file_for "$1")"
    [ -f "$pid_file" ] || return 1
    pid="$(cat "$pid_file")"
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

start_app() {
    local target pid_file log_file
    target="$(cd "$1" && pwd)"
    pid_file="$(pid_file_for "$target")"
    log_file="$(app_log_for "$target")"
    # Fully detached: stdin from /dev/null, output to a log file. A daemon
    # that kept deploy-cli's pipes as its stdout would be killed by SIGPIPE
    # the first time it printed something after deploy-cli exited.
    nohup "$target/app.sh" --daemon </dev/null >>"$log_file" 2>&1 &
    echo "$!" >"$pid_file"
    log "started $target/app.sh (pid $!, output: $log_file)"
}

# Stops only this target's app: SIGTERM, then SIGKILL after 5 seconds.
stop_app() {
    local pid_file pid
    pid_file="$(pid_file_for "$1")"
    [ -f "$pid_file" ] || return 0
    pid="$(cat "$pid_file")"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.5
        done
        if kill -0 "$pid" 2>/dev/null; then
            warn "app (pid $pid) ignored SIGTERM for 5s; sending SIGKILL"
            kill -9 "$pid" 2>/dev/null || true
        fi
        log "stopped app (pid $pid)"
    fi
    rm -f "$pid_file"
}

# Waits up to $2 seconds for the app to report healthy (used after a start).
wait_until_healthy() {
    local target="$1" tries=$(( $2 * 4 ))
    while [ "$tries" -gt 0 ]; do
        if app_is_running "$target" && "$target/app.sh" --health >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.25
        tries=$(( tries - 1 ))
    done
    return 1
}
