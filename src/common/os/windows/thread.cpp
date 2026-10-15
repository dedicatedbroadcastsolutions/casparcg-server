#include "../thread.h"

#include <thread>

#include <windows.h>

#include "../../utf.h"
#include "../../log.h"

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

    GROUP_AFFINITY group;
    if (!GetThreadGroupAffinity(GetCurrentThread(), &group)) {
        CASPAR_LOG(warning) << "Cannot query thread processor group: Windows error " << GetLastError();
        return false;
    }
    const auto active_cpus = GetActiveProcessorCount(group.Group);
    DWORD_PTR mask = 0;
    for (auto cpu_id : cpu_ids) {
        if (cpu_id < 0 || cpu_id >= thread_affinity_capacity() || static_cast<DWORD>(cpu_id) >= active_cpus) {
            CASPAR_LOG(warning) << "Cannot set thread CPU affinity: CPU ID " << cpu_id
                               << " is unavailable in processor group " << group.Group;
            return false;
        }
        mask |= (static_cast<DWORD_PTR>(1) << cpu_id);
    }

    if (SetThreadAffinityMask(GetCurrentThread(), mask) != 0)
        return true;
    CASPAR_LOG(warning) << "Cannot set thread CPU affinity: Windows error " << GetLastError();
    return false;
}

int thread_affinity_capacity() { return static_cast<int>(sizeof(DWORD_PTR) * 8); }

} // namespace caspar
