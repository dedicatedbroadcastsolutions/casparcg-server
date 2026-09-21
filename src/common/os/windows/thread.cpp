#include "../thread.h"

#include <thread>

#include <windows.h>

#include "../../utf.h"

namespace caspar {

typedef struct tagTHREADNAME_INFO
{
    DWORD  dwType;     // must be 0x1000
    LPCSTR szName;     // pointer to name (in user addr space)
    DWORD  dwThreadID; // thread ID (-1=caller thread)
    DWORD  dwFlags;    // reserved for future use, must be zero
} THREADNAME_INFO;

inline void SetThreadName(DWORD dwThreadID, LPCSTR szThreadName)
{
    THREADNAME_INFO info;
    {
        info.dwType     = 0x1000;
        info.szName     = szThreadName;
        info.dwThreadID = dwThreadID;
        info.dwFlags    = 0;
    }
    __try {
        RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    } __except (EXCEPTION_CONTINUE_EXECUTION) {
    }
}

void set_thread_name(const std::wstring& name) { SetThreadName(GetCurrentThreadId(), u8(name).c_str()); }

void set_thread_realtime_priority() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL); }

bool set_thread_affinity(const std::vector<int>& cpu_ids)
{
    if (cpu_ids.empty())
        return true;

    // Only handles CPUs within the calling thread's processor group (i.e. up to 64 logical CPUs on that group).
    DWORD_PTR mask = 0;
    for (auto cpu_id : cpu_ids) {
        if (cpu_id >= 0 && cpu_id < static_cast<int>(sizeof(DWORD_PTR) * 8))
            mask |= (static_cast<DWORD_PTR>(1) << cpu_id);
    }

    return mask != 0 && SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
}

} // namespace caspar
