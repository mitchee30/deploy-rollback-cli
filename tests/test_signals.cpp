#include <doctest/doctest.h>

#include "deployguard/signals.h"

#include <csignal>

using namespace DeployGuard;

namespace {

// Saves the dispositions of the signals installSignalHandlers() touches and
// restores them afterwards, so these tests do not affect other tests that
// run in the same process.
class SignalDispositionGuard {
public:
    SignalDispositionGuard() {
        for (int i = 0; i < kCount; ++i) {
            ::sigaction(kSignals[i], nullptr, &saved_[i]);
        }
        resetCancellationRequest();
    }
    ~SignalDispositionGuard() {
        for (int i = 0; i < kCount; ++i) {
            ::sigaction(kSignals[i], &saved_[i], nullptr);
        }
        resetCancellationRequest();
    }
    SignalDispositionGuard(const SignalDispositionGuard&) = delete;
    SignalDispositionGuard& operator=(const SignalDispositionGuard&) = delete;

private:
    static constexpr int kCount = 4;
    static constexpr int kSignals[kCount] = {SIGINT, SIGTERM, SIGHUP, SIGPIPE};
    struct sigaction saved_[kCount] {};
};

void setDisposition(int sig, void (*handler)(int)) {
    struct sigaction action {};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    ::sigaction(sig, &action, nullptr);
}

} // namespace

TEST_CASE("signals: SIGTERM requests cancellation instead of killing the process") {
    SignalDispositionGuard guard;
    installSignalHandlers();
    CHECK_FALSE(cancellationRequested());

    ::raise(SIGTERM);  // would terminate the test binary without the handler

    CHECK(cancellationRequested());
    CHECK(cancellationSignal() == SIGTERM);
    resetCancellationRequest();
    CHECK_FALSE(cancellationRequested());
    CHECK(cancellationSignal() == 0);
}

TEST_CASE("signals: SIGINT and SIGHUP request cancellation too") {
    SignalDispositionGuard guard;
    setDisposition(SIGINT, SIG_DFL);
    setDisposition(SIGHUP, SIG_DFL);
    installSignalHandlers();

    ::raise(SIGINT);
    CHECK(cancellationSignal() == SIGINT);
    resetCancellationRequest();

    ::raise(SIGHUP);
    CHECK(cancellationSignal() == SIGHUP);
}

TEST_CASE("signals: SIGHUP and SIGINT that were ignored at start stay ignored (nohup)") {
    SignalDispositionGuard guard;
    setDisposition(SIGHUP, SIG_IGN);
    setDisposition(SIGINT, SIG_IGN);
    installSignalHandlers();

    ::raise(SIGHUP);
    ::raise(SIGINT);
    CHECK_FALSE(cancellationRequested());
}

TEST_CASE("signals: SIGTERM is handled even if it was ignored at start") {
    SignalDispositionGuard guard;
    setDisposition(SIGTERM, SIG_IGN);
    installSignalHandlers();
    ::raise(SIGTERM);
    CHECK(cancellationSignal() == SIGTERM);
}

TEST_CASE("signals: SIGPIPE is ignored, so a closed stdout cannot kill deploy-cli") {
    SignalDispositionGuard guard;
    installSignalHandlers();
    ::raise(SIGPIPE);  // would terminate the test binary if not ignored
    struct sigaction current {};
    ::sigaction(SIGPIPE, nullptr, &current);
    CHECK(current.sa_handler == SIG_IGN);
    CHECK_FALSE(cancellationRequested());
}
