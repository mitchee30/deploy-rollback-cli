#!/usr/bin/env bash
# Crash recovery: deploy-cli is SIGKILLed in the middle of a deployment (or of
# a rollback) - no destructor, no signal handler, no cleanup runs - and the
# next run must detect it and repair the target.
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"

T="$WORK/app"
mkdir "$T"
echo "v1" >"$T/version"
STATE="$WORK/.deploy_guard_app.state"
SAVED="$WORK/saved_version"   # the hooks' own tiny "backup"

# $PPID of a hook is deploy-cli itself (hooks are exec'd directly, no shell).
write_hook "$WORK/deploy_then_die.sh" "
echo deploy >> '$EVENTS'
cp \"\$1/version\" '$SAVED'
echo half-installed > \"\$1/version\"
kill -9 \$PPID
exit 0"
write_hook "$WORK/deploy_v2.sh" "
echo deploy >> '$EVENTS'
cp \"\$1/version\" '$SAVED'
echo v2 > \"\$1/version\""
write_hook "$WORK/deploy_fail.sh" "
echo deploy >> '$EVENTS'
cp \"\$1/version\" '$SAVED'
echo broken > \"\$1/version\"
exit 1"
write_hook "$WORK/rollback.sh" "
echo rollback >> '$EVENTS'
if [ -f '$SAVED' ]; then cp '$SAVED' \"\$1/version\"; rm -f '$SAVED'; fi"
# Dies the first time it runs (a power cut halfway through the rollback);
# when it is run again it does the real work.
write_hook "$WORK/rollback_then_die.sh" "
echo rollback >> '$EVENTS'
if [ ! -f '$WORK/died_once' ]; then touch '$WORK/died_once'; kill -9 \$PPID; exit 0; fi
if [ -f '$SAVED' ]; then cp '$SAVED' \"\$1/version\"; rm -f '$SAVED'; fi"

section "deploy-cli killed with SIGKILL mid-deployment leaves INSTALLING behind"
write_config "$WORK/crash.yaml" "$T" "$WORK/deploy_then_die.sh" "$WORK/rollback.sh"
run_cli --config "$WORK/crash.yaml" --log-file "$LOG"
[ "$EXIT" -eq 137 ] || fail "expected deploy-cli to die from SIGKILL (137), got $EXIT"
expect_first_line "$STATE" "INSTALLING"
[ "$(cat "$T/version")" = "half-installed" ] || fail "the hook did not run as expected"
pass

section "next run: the dead run's lock is free, its deployment is rolled back, then the new one runs"
reset_events
write_config "$WORK/good.yaml" "$T" "$WORK/deploy_v2.sh" "$WORK/rollback.sh"
run_cli --config "$WORK/good.yaml" --log-file "$LOG"
expect_exit 0
expect_output "interrupted deployment"
expect_events "rollback deploy"
[ "$(cat "$T/version")" = "v2" ] || fail "expected v2 installed, found $(cat "$T/version")"
expect_missing "$STATE"
pass

section "deploy-cli killed with SIGKILL mid-rollback leaves ROLLING_BACK behind"
reset_events
write_config "$WORK/crash_rb.yaml" "$T" "$WORK/deploy_fail.sh" "$WORK/rollback_then_die.sh"
run_cli --config "$WORK/crash_rb.yaml" --log-file "$LOG"
[ "$EXIT" -eq 137 ] || fail "expected deploy-cli to die from SIGKILL (137), got $EXIT"
expect_first_line "$STATE" "ROLLING_BACK"
[ "$(cat "$T/version")" = "broken" ] || fail "expected the broken version to be in place"
pass

section "next run resumes the rollback with the hook the dead run recorded, then deploys"
# good.yaml names rollback.sh, but the state file says the interrupted run
# was using rollback_then_die.sh - and that is the hook that knows how to
# undo it.
reset_events
run_cli --config "$WORK/good.yaml" --log-file "$LOG"
expect_exit 0
expect_output "interrupted rollback"
expect_output "rollback_then_die.sh; using it instead of the one in the current configuration"
expect_events "rollback deploy"
[ "$(cat "$T/version")" = "v2" ] || fail "expected v2, found $(cat "$T/version")"
expect_missing "$STATE"
pass

section "a corrupt state file stops deploy-cli (exit 2) instead of being ignored"
reset_events
echo "garbage" >"$STATE"
run_cli --config "$WORK/good.yaml" --log-file "$LOG"
expect_exit 2
expect_output "corrupt"
expect_events ""
rm -f "$STATE"
pass
