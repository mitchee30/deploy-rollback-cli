#ifndef DEPLOYGUARD_SIGNALS_H
#define DEPLOYGUARD_SIGNALS_H

namespace DeployGuard {

// Installs handlers so that SIGINT (Ctrl+C), SIGTERM and SIGHUP request a
// *graceful* cancellation instead of killing deploy-cli on the spot (which
// would leave a half-installed target behind and the hook script running
// unsupervised). The handlers only set a flag - the deployment loop checks it,
// stops the running hook, and rolls back.
//
// SIGINT/SIGHUP that were already ignored when we started (e.g. under nohup,
// or as a background job of a non-interactive shell) are left ignored.
//
// SIGPIPE is ignored, so a closed stdout (e.g. `deploy-cli ... | head`) turns
// into a failed write instead of killing us halfway through a deployment.
// ProcessManager restores the default SIGPIPE behaviour for hook scripts.
void installSignalHandlers();

bool cancellationRequested();

// The signal that requested cancellation, or 0.
int cancellationSignal();

// For tests only.
void resetCancellationRequest();

} // namespace DeployGuard

#endif // DEPLOYGUARD_SIGNALS_H
