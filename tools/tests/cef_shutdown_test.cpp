#include <modules/html/browser_lifecycle.h>
#include <modules/html/subprocess_group.h>

#include <iostream>
#include <stdexcept>

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#endif

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void lifecycle_tests()
{
    using caspar::html::browser_lifecycle;
    for (int repeat = 0; repeat < 100; ++repeat) {
        int               quit_count = 0;
        browser_lifecycle empty([&] { ++quit_count; });
        empty.shutdown();
        empty.shutdown();
        require(quit_count == 1, "Empty shutdown must complete exactly once");

        int a = 0, b = 0, pending = 0;
        int close_a = 0, close_b = 0, close_pending = 0;
        quit_count = 0;
        browser_lifecycle browsers([&] { ++quit_count; });
        browsers.add(&a, [&] { ++close_a; });
        browsers.add(&b, [&] { ++close_b; });
        browsers.add(&pending, [&] { ++close_pending; });
        browsers.shutdown();
        require(close_a == 1 && close_b == 1 && close_pending == 1, "Close all active and pending clients");
        require(quit_count == 0, "Close requests alone must not quit the loop");
        browsers.remove(&b);
        browsers.remove(&a);
        require(quit_count == 0, "Pending creation must keep the loop running");
        browsers.remove(&pending);
        require(quit_count == 1, "Quit only after the final close or failed creation");
        browsers.shutdown();
        require(quit_count == 1, "Repeated shutdown must not quit twice");
        try {
            browsers.add(&a, [] {});
            throw std::runtime_error("Creation during shutdown must be rejected");
        } catch (const std::logic_error&) {
        }

        quit_count = 0;
        browser_lifecycle replaced([&] { ++quit_count; });
        replaced.add(&a, [] {});
        replaced.remove(&a);
        require(quit_count == 0, "Closing a template during normal operation must not quit CEF");
        replaced.add(&a, [&] { replaced.remove(&a); });
        replaced.add(&b, [&] { replaced.remove(&b); });
        replaced.shutdown();
        require(quit_count == 1, "Synchronous close callbacks must not invalidate shutdown iteration");
    }
}

#if defined(__linux__)
volatile std::sig_atomic_t received_signal = 0;

void record_signal(int signal) { received_signal = signal; }

bool read_byte(int fd)
{
    pollfd ready{fd, POLLIN, 0};
    int    status;
    do {
        status = poll(&ready, 1, 5000);
    } while (status < 0 && errno == EINTR);
    if (status <= 0) {
        return false;
    }
    char    value;
    ssize_t count;
    do {
        count = read(fd, &value, 1);
    } while (count < 0 && errno == EINTR);
    return count == 1;
}

void signal_case(int signal, bool whole_group, bool isolate, bool at_fork = false)
{
    int ready[2], finish[2];
    require(pipe(ready) == 0 && pipe(finish) == 0, "Create test synchronization pipes");
    const auto browser = fork();
    require(browser >= 0, "Fork test browser process");
    if (browser == 0) {
        close(ready[0]);
        close(finish[1]);
        if (setpgid(0, 0) != 0) {
            _exit(10);
        }
        struct sigaction action
        {
        };
        action.sa_handler = record_signal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
            _exit(11);
        }
        int child_ready[2], child_finish[2];
        if (pipe(child_ready) != 0 || pipe(child_finish) != 0) {
            _exit(12);
        }
        if (at_fork && caspar::html::install_subprocess_group_isolation() != 0) {
            _exit(20);
        }
        const auto subprocess = fork();
        if (subprocess < 0) {
            _exit(13);
        }
        if (subprocess == 0) {
            close(child_ready[0]);
            close(child_finish[1]);
            if (isolate && !at_fork && !caspar::html::isolate_subprocess_group()) {
                _exit(14);
            }
            if (isolate && !at_fork && !caspar::html::isolate_subprocess_group()) {
                _exit(15);
            }
            if (write(child_ready[1], "r", 1) != 1) {
                _exit(16);
            }
            const bool finished        = read_byte(child_finish[0]);
            const auto expected_signal = whole_group && !isolate ? signal : 0;
            _exit(finished && received_signal == expected_signal ? 0 : 17);
        }
        close(child_ready[1]);
        close(child_finish[0]);
        if (!read_byte(child_ready[0]) || write(ready[1], "r", 1) != 1) {
            kill(subprocess, SIGKILL);
            waitpid(subprocess, nullptr, 0);
            _exit(18);
        }
        const bool finished       = read_byte(finish[0]);
        const bool child_finished = write(child_finish[1], "x", 1) == 1;
        int        status         = 0;
        pid_t      waited;
        do {
            waited = waitpid(subprocess, &status, 0);
        } while (waited < 0 && errno == EINTR);
        _exit(finished && child_finished && waited == subprocess && WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                      received_signal == signal
                  ? 0
                  : 19);
    }
    close(ready[1]);
    close(finish[0]);
    const bool ready_ok  = read_byte(ready[0]);
    const bool signal_ok = signal == 0 || (ready_ok && kill(whole_group ? -browser : browser, signal) == 0);
    const bool finish_ok = write(finish[1], "x", 1) == 1;
    close(ready[0]);
    close(finish[1]);
    int status = 0;
    require(waitpid(browser, &status, 0) == browser, "Reap test browser");
    require(ready_ok && signal_ok && finish_ok && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Group signals must reach unisolated children, but not isolated CEF children");
}
#endif

} // namespace

int main()
{
    try {
        lifecycle_tests();
#if defined(__linux__)
        std::signal(SIGPIPE, SIG_IGN);
        for (const int signal : {0, SIGINT, SIGTERM}) {
            for (const bool isolate : {false, true}) {
                signal_case(signal, false, isolate);
                signal_case(signal, true, isolate);
            }
            signal_case(signal, true, true, true);
        }
#endif
        std::cout << "CEF lifecycle and subprocess-group regression tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
