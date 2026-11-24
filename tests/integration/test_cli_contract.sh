#!/usr/bin/env bash
# The command-line contract: options, and the meaning of each exit code.
# shellcheck source=lib.sh
source "$(dirname "$0")/lib.sh"

T="$WORK/app"
mkdir "$T"
STATE="$WORK/.deploy_guard_app.state"
write_hook "$WORK/deploy_ok.sh"   "echo deploy >> '$EVENTS'"
write_hook "$WORK/deploy_fail.sh" "echo deploy >> '$EVENTS'; exit 1"
write_hook "$WORK/rb_ok.sh"       "echo rollback >> '$EVENTS'"
write_hook "$WORK/rb_fail.sh"     "echo rollback >> '$EVENTS'; exit 1"

section "--help prints usage and the exit codes, exit 0"
run_cli --help
expect_exit 0
expect_output "Exit codes:"
pass

section "--version, exit 0"
run_cli --version
expect_exit 0
expect_output "deploy-cli 0."
pass

section "unknown option -> exit 3"
run_cli --confg deploy.yaml
expect_exit 3
expect_output "unknown option '--confg'"
pass

section "--config and --path together -> exit 3"
run_cli --config a.yaml --path "$T"
expect_exit 3
pass

section "invalid --timeout -> exit 3"
run_cli --path "$T" --timeout soon
expect_exit 3
pass

section "missing config file -> exit 3"
run_cli --config "$WORK/nope.yaml" --log-file ""
expect_exit 3
expect_output "Cannot read configuration file"
pass

section "malformed YAML -> exit 3"
echo "deployment: [unclosed" >"$WORK/bad.yaml"
run_cli --config "$WORK/bad.yaml" --log-file ""
expect_exit 3
pass

section "exit 0: the deploy hook succeeds"
reset_events
write_config "$WORK/ok.yaml" "$T" "$WORK/deploy_ok.sh" "$WORK/rb_ok.sh"
run_cli --config "$WORK/ok.yaml" --log-file "$LOG"
expect_exit 0
expect_events "deploy"
expect_missing "$STATE"
pass

section "exit 1: the deploy hook fails, the rollback succeeds"
reset_events
write_config "$WORK/rb.yaml" "$T" "$WORK/deploy_fail.sh" "$WORK/rb_ok.sh"
run_cli --config "$WORK/rb.yaml" --log-file "$LOG"
expect_exit 1
expect_events "deploy rollback"
expect_missing "$STATE"
pass

section "exit 2: the rollback fails too; the state file is kept"
reset_events
write_config "$WORK/crit.yaml" "$T" "$WORK/deploy_fail.sh" "$WORK/rb_fail.sh"
run_cli --config "$WORK/crit.yaml" --log-file "$LOG"
expect_exit 2
expect_events "deploy rollback"
expect_first_line "$STATE" "ROLLING_BACK"
pass

section "after exit 2, the next run retries the rollback first, then deploys"
reset_events
run_cli --config "$WORK/ok.yaml" --log-file "$LOG"
expect_exit 0
expect_events "rollback deploy"
expect_output "interrupted rollback"
expect_missing "$STATE"
pass

section "exit 3: target directory missing - no hook runs"
reset_events
write_config "$WORK/missing.yaml" "$WORK/no_such_dir" "$WORK/deploy_ok.sh" "$WORK/rb_ok.sh"
run_cli --config "$WORK/missing.yaml" --log-file ""
expect_exit 3
expect_output "does not exist"
expect_events ""
pass

section "exit 3: rollback hook missing - found before the deploy hook runs"
reset_events
write_config "$WORK/norb.yaml" "$T" "$WORK/deploy_ok.sh" "$WORK/no_such_rollback.sh"
run_cli --config "$WORK/norb.yaml" --log-file ""
expect_exit 3
expect_output "rollback hook"
expect_events ""
pass

section "--dry-run: exit 0, runs nothing, creates no file"
reset_events
D="$WORK/dry"
mkdir "$D"
write_config "$WORK/dry.yaml" "$D" "$WORK/deploy_ok.sh" "$WORK/rb_ok.sh"
run_cli --config "$WORK/dry.yaml" --dry-run --log-file ""
expect_exit 0
expect_output "Plan:"
expect_events ""
expect_missing "$WORK/.deploy_guard_dry.state"
expect_missing "$WORK/.deploy_guard_dry.lock"
pass

section "--dry-run of a broken config: exit 3"
run_cli --config "$WORK/norb.yaml" --dry-run --log-file ""
expect_exit 3
pass

section "the log file gets every line, including the hooks' stderr"
write_hook "$WORK/noisy.sh" "echo to-stdout; echo to-stderr >&2"
write_config "$WORK/noisy.yaml" "$T" "$WORK/noisy.sh" "$WORK/rb_ok.sh"
rm -f "$LOG"
run_cli --config "$WORK/noisy.yaml" --log-file "$LOG"
expect_exit 0
expect_file_has "$LOG" "[STDOUT]  [noisy.sh] to-stdout"
expect_file_has "$LOG" "[STDERR]  [noisy.sh] to-stderr"
pass
