#!/usr/bin/env bash
# The bundled demo, end to end: real hooks, real background service, real
# health check. Runs on a copy, so the repository itself is never modified.
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"

cp -R "$REPO/scripts" "$REPO/releases" "$REPO/deploy.yaml" "$WORK/"
cd "$WORK"
T="$WORK/test_app_target"
BACKUP="$WORK/.deploy_guard_test_app_target.backup"
STATE="$WORK/.deploy_guard_test_app_target.state"
PIDFILE="$WORK/.demo_app_test_app_target.pid"

version() { "$T/app.sh" --version; }
app_running() { [ -f "$PIDFILE" ] && process_running "$(cat "$PIDFILE")"; }

section "setup_v1.sh installs v1.0 and starts it"
./scripts/setup_v1.sh >/dev/null
[ "$(version)" = "1.0" ] || fail "expected v1.0"
app_running || fail "v1.0 is not running"
pass

section "deploying v1.1 (healthy): exit 0, v1.1 running"
DEMO_RELEASE=v1.1 run_cli --config deploy.yaml --log-file "$LOG"
expect_exit 0
[ "$(version)" = "1.1" ] || fail "expected v1.1, found $(version)"
app_running || fail "v1.1 is not running"
expect_file_has "$LOG" "Health check passed"
expect_missing "$STATE"
pass

section "deploying v2.0 (unhealthy): retried, rolled back to v1.1 - hidden files included - exit 1"
echo "SECRET=from-v1.1" >"$T/.env"
echo "only in v2" >releases/v2.0/.v2_only    # our private copy of the releases
DEMO_RELEASE=v2.0 run_cli --config deploy.yaml --log-file "$LOG"
expect_exit 1
expect_file_has "$LOG" "Health check failed after 3 attempt(s)"
expect_file_has "$LOG" "[STDERR]"
expect_file_has "$LOG" "Database schema mismatch"
[ "$(version)" = "1.1" ] || fail "expected v1.1 after the rollback, found $(version)"
[ "$(cat "$T/.env")" = "SECRET=from-v1.1" ] || fail "the hidden .env file was not restored"
expect_missing "$T/.v2_only"
app_running || fail "v1.1 is not running after the rollback"
"$T/app.sh" --health >/dev/null || fail "v1.1 is not healthy after the rollback"
expect_missing "$BACKUP"
expect_missing "$STATE"
pass

section "rollback.sh leaves alone a backup that belongs to another deployment"
# As if a crash had left the state of deployment "mine" (killed before it
# committed its own backup) next to a backup tagged with another run id.
mkdir -p "$BACKUP"
cp -a releases/v1.0/. "$BACKUP/"
echo "someone-else" >"$BACKUP.run_id"
printf 'INSTALLING\nrun_id=mine\nrollback=%s\n' "$WORK/scripts/rollback.sh" >"$STATE"
DEMO_RELEASE=v1.1 run_cli --config deploy.yaml --log-file "$LOG"
expect_exit 0
expect_file_has "$LOG" "belongs to deployment 'someone-else'"
[ "$(version)" = "1.1" ] || fail "the other deployment's backup (v1.0) was restored"
expect_missing "$STATE"
pass

section "--path uses the bundled hooks, health check included"
DEMO_RELEASE=v2.0 run_cli --path ./test_app_target --log-file ""
expect_exit 1
[ "$(version)" = "1.1" ] || fail "expected v1.1, found $(version)"
pass

section "first deployment into an empty directory works"
E="$WORK/empty_app"
mkdir "$E"
demo_config() {  # demo_config <file> <target>: the bundled hooks, fast retries
    printf 'deployment:\n  target_path: "%s"\n  scripts:\n    deploy: "./scripts/deploy.sh"\n    rollback: "./scripts/rollback.sh"\n    health_check: "./scripts/health_check.sh"\n  health_check_interval_seconds: 0\n' \
        "$2" >"$1"
}
demo_config "$WORK/empty.yaml" "./empty_app"
DEMO_RELEASE=v1.1 run_cli --config "$WORK/empty.yaml" --log-file "$LOG"
expect_exit 0
[ "$("$E/app.sh" --version)" = "1.1" ] || fail "v1.1 not installed into the empty directory"
pass

section "a failed first deployment rolls back to an empty directory"
F="$WORK/fresh_app"
mkdir "$F"
demo_config "$WORK/fresh.yaml" "./fresh_app"
DEMO_RELEASE=v2.0 run_cli --config "$WORK/fresh.yaml" --log-file "$LOG"
expect_exit 1
[ -z "$(ls -A "$F")" ] || fail "expected $F to be empty again, found: $(ls -A "$F")"
[ ! -f "$WORK/.demo_app_fresh_app.pid" ] || fail "no app should be running for the fresh target"
pass
