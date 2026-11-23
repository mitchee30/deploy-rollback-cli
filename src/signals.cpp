#include "deployguard/signals.h"

#include <csignal>

namespace DeployGuard {

namespace {

// The only thing a signal handler may safely do here is store to a
// volatile sig_atomic_t. Everything else happens in the main loop.
volatile std::sig_atomic_t g_cancel_signal = 0;

void onCancelSignal(int sig) {
    g_cancel_signal = sig;
}

void installCancelHandler(int sig, bool keep_if_ignored) {
    struct sigaction current {};
    if (keep_if_ignored && ::sigaction(sig, nullptr, &current) == 0 && current.sa_handler == SIG_IGN) {
        return;  // e.g. SIGHUP under nohup: the user asked us to survive it
    }
    struct sigaction action {};
    action.sa_handler = onCancelSignal;
    sigemptyset(&action.sa_mask);
    // No SA_RESTART: a blocking call (poll, read) returns EINTR right away,
    // so the main loop notices the request without waiting.
    action.sa_flags = 0;
    ::sigaction(sig, &action, nullptr);
}

} // namespace

void installSignalHandlers() {
    installCancelHandler(SIGINT, /*keep_if_ignored=*/true);
    installCancelHandler(SIGHUP, /*keep_if_ignored=*/true);
    installCancelHandler(SIGTERM, /*keep_if_ignored=*/false);

    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    ::sigaction(SIGPIPE, &ignore, nullptr);
}

bool cancellationRequested() {
    return g_cancel_signal != 0;
}

int cancellationSignal() {
    return static_cast<int>(g_cancel_signal);
}

void resetCancellationRequest() {
    g_cancel_signal = 0;
}

} // namespace DeployGuard
