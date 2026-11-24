#!/usr/bin/env bash
# SIGTERM / SIGINT (Ctrl+C) during a deployment stop the hook and everything
# it started, then run the rollback - which itself cannot be interrupted.
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"

T="$WORK/app"
mkdir "$T"
STATE="$WORK/.deploy_guard_app.state"
write_hook "$WORK/deploy.sh" "
echo deploy >> '$EVENTS'
sleep 60 &
echo \$! > '$WORK/grandchild.pid'
touch '$WORK/started'
wait"
write_hook "$WORK/rollback.sh" "
touch '$WORK/rollback_started'
sleep 1
echo rollback >> '$EVENTS'"
write_config "$WORK/c.yaml" "$T" "$WORK/deploy.sh" "$WORK/rollback.sh"

section "SIGTERM: the hook and its children are stopped, the rollback runs, exit 1"
start_bg "$WORK/cli.out" "$CLI" --config "$WORK/c.yaml" --log-file "$LOG"
CLI_PID=$BG_PID
wait_for_file "$WORK/started"
kill -TERM "$CLI_PID"
wait_bg "$CLI_PID"
expect_exit 1
expect_events "deploy rollback"
expect_file_has "$LOG" "Received SIGTERM"
wait_until_dead "$(cat "$WORK/grandchild.pid")" || fail "the hook's background child is still running"
expect_missing "$STATE"
pass

section "another SIGTERM during the rollback does not interrupt it"
reset_events
rm -f "$WORK/started" "$WORK/rollback_started"
start_bg "$WORK/cli.out" "$CLI" --config "$WORK/c.yaml" --log-file "$LOG"
CLI_PID=$BG_PID
wait_for_file "$WORK/started"
kill -TERM "$CLI_PID"
wait_for_file "$WORK/rollback_started"
kill -TERM "$CLI_PID"
wait_bg "$CLI_PID"
expect_exit 1
expect_events "deploy rollback"
expect_file_has "$LOG" "never interrupted"
pass

section "SIGINT (Ctrl+C) behaves the same"
# Background jobs of a non-interactive shell ignore SIGINT; job control
# (set -m) starts them normally, like a shell would for a foreground job.
reset_events
rm -f "$WORK/started"
set -m
start_bg "$WORK/cli.out" "$CLI" --config "$WORK/c.yaml" --log-file "$LOG"
set +m
CLI_PID=$BG_PID
wait_for_file "$WORK/started"
kill -INT "$CLI_PID"
wait_bg "$CLI_PID"
expect_exit 1
expect_events "deploy rollback"
expect_file_has "$LOG" "Received SIGINT"
pass

section "under nohup, SIGHUP is ignored and the deployment completes"
reset_events
rm -f "$WORK/started"
write_hook "$WORK/deploy_short.sh" "echo deploy >> '$EVENTS'; touch '$WORK/started'; sleep 2"
write_config "$WORK/short.yaml" "$T" "$WORK/deploy_short.sh" "$WORK/rollback.sh"
start_bg "$WORK/cli.out" nohup "$CLI" --config "$WORK/short.yaml" --log-file "$LOG"
CLI_PID=$BG_PID
wait_for_file "$WORK/started"
kill -HUP "$CLI_PID"
wait_bg "$CLI_PID"
expect_exit 0
expect_events "deploy"
pass
