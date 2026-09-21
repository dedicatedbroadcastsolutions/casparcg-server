#include "../thread.h"
#include "../../utf.h"
#include <pthread.h>
#include <sched.h>

namespace caspar {

void set_thread_name(const std::wstring& name) { pthread_setname_np(pthread_self(), u8(name).c_str()); }

void set_thread_realtime_priority()
{
    pthread_t          handle = pthread_self();
    int                policy;
    struct sched_param param;
    if (pthread_getschedparam(handle, &policy, &param) != 0)
        return;
    param.sched_priority = 2;
    pthread_setschedparam(handle, SCHED_FIFO, &param);
}

bool set_thread_affinity(const std::vector<int>& cpu_ids)
{
    if (cpu_ids.empty())
        return true;

    cpu_set_t set;
    CPU_ZERO(&set);
    for (auto cpu_id : cpu_ids)
        CPU_SET(cpu_id, &set);

    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

} // namespace caspar
