#!/usr/bin/env bash
# Two deploy-cli processes against the same target. Before v0.2 the second
# one saw the first one's live INSTALLING marker, took it for a crash, and
# rolled the target back underneath the running deployment.
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"

T="$WORK/app"
T2="$WORK/other"
mkdir "$T" "$T2"
write_hook "$WORK/slow_deploy.sh" "echo deploy >> '$EVENTS'; touch '$WORK/started'; sleep 3"
write_hook "$WORK/fast_deploy.sh" "echo deploy-other >> '$EVENTS'"
write_hook "$WORK/rollback.sh" "echo rollback >> '$EVENTS'"
write_config "$WORK/slow.yaml" "$T" "$WORK/slow_deploy.sh" "$WORK/rollback.sh"
write_config "$WORK/other.yaml" "$T2" "$WORK/fast_deploy.sh" "$WORK/rollback.sh"

section "a second run on the same target is refused at once (exit 3) and rolls nothing back"
start_bg "$WORK/first.out" "$CLI" --config "$WORK/slow.yaml" --log-file ""
FIRST=$BG_PID
wait_for_file "$WORK/started"
run_cli --config "$WORK/slow.yaml" --log-file ""
expect_exit 3
expect_output "already running against this target (pid $FIRST)"
expect_faster_than 3
wait_bg "$FIRST"
expect_exit 0
expect_events "deploy"
pass

section "once the first run has finished, the target can be deployed again"
reset_events
rm -f "$WORK/started"
run_cli --config "$WORK/slow.yaml" --log-file ""
expect_exit 0
expect_events "deploy"
pass

section "a run on a different target is not blocked"
reset_events
rm -f "$WORK/started"
start_bg "$WORK/first.out" "$CLI" --config "$WORK/slow.yaml" --log-file ""
FIRST=$BG_PID
wait_for_file "$WORK/started"
run_cli --config "$WORK/other.yaml" --log-file ""
expect_exit 0
wait_bg "$FIRST"
expect_exit 0
pass

section "a daemon started by a hook does not inherit the lock"
# If the lock descriptor leaked into the daemon, every later run would be
# refused for as long as the daemon lives.
reset_events
write_hook "$WORK/deploy_daemon.sh" "echo deploy >> '$EVENTS'; sleep 60 & echo \$! > '$WORK/daemon.pid'"
write_config "$WORK/daemon.yaml" "$T" "$WORK/deploy_daemon.sh" "$WORK/rollback.sh"
run_cli --config "$WORK/daemon.yaml" --log-file ""
expect_exit 0
process_running "$(cat "$WORK/daemon.pid")" || fail "the daemon should still be running"
run_cli --config "$WORK/daemon.yaml" --log-file ""
expect_exit 0
pass
