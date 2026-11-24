#!/usr/bin/env bash
# A hook that hangs is stopped after timeout_seconds (the whole process group,
# with SIGKILL if SIGTERM is ignored) and the deployment is rolled back.
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"

T="$WORK/app"
mkdir "$T"
STATE="$WORK/.deploy_guard_app.state"
write_hook "$WORK/hang.sh" "
echo deploy >> '$EVENTS'
sleep 60 &
echo \$! > '$WORK/grandchild.pid'
wait"
write_hook "$WORK/stubborn.sh" "echo deploy >> '$EVENTS'; trap '' TERM; sleep 60"
write_hook "$WORK/deploy_fail.sh" "echo deploy >> '$EVENTS'; exit 1"
write_hook "$WORK/rollback.sh" "echo rollback >> '$EVENTS'"
write_hook "$WORK/rollback_hang.sh" "echo rollback >> '$EVENTS'; sleep 60"

section "a hung deploy hook times out, its children are killed, and it is rolled back (exit 1)"
write_config "$WORK/t.yaml" "$T" "$WORK/hang.sh" "$WORK/rollback.sh" "timeout_seconds: 1"
run_cli --config "$WORK/t.yaml" --log-file "$LOG"
expect_exit 1
expect_faster_than 10
expect_events "deploy rollback"
expect_file_has "$LOG" "timed out"
wait_until_dead "$(cat "$WORK/grandchild.pid")" || fail "the hook's background child is still running"
pass

section "--timeout on the command line overrides the config file"
reset_events
write_config "$WORK/long.yaml" "$T" "$WORK/hang.sh" "$WORK/rollback.sh" "timeout_seconds: 600"
run_cli --config "$WORK/long.yaml" --timeout 1 --log-file "$LOG"
expect_exit 1
expect_faster_than 10
pass

section "a hook that ignores SIGTERM is SIGKILLed after the grace period"
reset_events
write_config "$WORK/stubborn.yaml" "$T" "$WORK/stubborn.sh" "$WORK/rollback.sh" "timeout_seconds: 1"
run_cli --config "$WORK/stubborn.yaml" --log-file "$LOG"
expect_exit 1
expect_faster_than 15
expect_file_has "$LOG" "SIGKILL"
expect_events "deploy rollback"
pass

section "a hung rollback times out too: exit 2, state kept for the next run"
reset_events
write_config "$WORK/rbhang.yaml" "$T" "$WORK/deploy_fail.sh" "$WORK/rollback_hang.sh" "timeout_seconds: 1"
run_cli --config "$WORK/rbhang.yaml" --log-file "$LOG"
expect_exit 2
expect_faster_than 10
expect_first_line "$STATE" "ROLLING_BACK"
rm -f "$STATE"
pass
