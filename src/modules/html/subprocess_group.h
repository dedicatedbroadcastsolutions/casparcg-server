#pragma once

#if defined(__linux__)
#include <pthread.h>
#include <unistd.h>

namespace caspar::html {

inline bool isolate_subprocess_group()
{
    // Keep terminal group signals from killing Chromium children before CEF closes them.
    return getpgrp() == getpid() || setpgid(0, 0) == 0;
}

inline void isolate_forked_subprocess()
{
    if (!isolate_subprocess_group()) {
        constexpr char message[] = "[html] Failed to isolate forked subprocess process group\n";
        const auto     written   = write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)written;
        _exit(1);
    }
}

inline int install_subprocess_group_isolation()
{
    // No allocation, locks or CEF calls are allowed in the child-side fork hook.
    // Isolate before exec: loading libcef can take long enough to receive Ctrl+C.
    static const int result = pthread_atfork(nullptr, nullptr, isolate_forked_subprocess);
    return result;
}

} // namespace caspar::html
#endif
