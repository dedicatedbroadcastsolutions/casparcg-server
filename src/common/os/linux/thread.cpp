#include "../thread.h"
#include "../../utf.h"
#include "../../log.h"
#include <cstring>
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
    for (auto cpu_id : cpu_ids) {
        if (cpu_id < 0 || cpu_id >= CPU_SETSIZE) {
            CASPAR_LOG(warning) << "Cannot set thread CPU affinity: CPU ID " << cpu_id
                               << " is outside the supported range 0.." << CPU_SETSIZE - 1;
            return false;
        }
        CPU_SET(cpu_id, &set);
    }

    cpu_set_t previous;
    auto      error = pthread_getaffinity_np(pthread_self(), sizeof(previous), &previous);
    if (error == 0)
        error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (error != 0) {
        CASPAR_LOG(warning) << "Cannot set thread CPU affinity: " << std::strerror(error);
        return false;
    }

    cpu_set_t effective;
    error = pthread_getaffinity_np(pthread_self(), sizeof(effective), &effective);
    if (error == 0 && CPU_EQUAL(&effective, &set))
        return true;

    CASPAR_LOG(warning) << "Cannot apply the complete thread CPU affinity mask; restoring the previous mask"
                       << (error != 0 ? std::string(": ") + std::strerror(error) : "");
    error = pthread_setaffinity_np(pthread_self(), sizeof(previous), &previous);
    if (error != 0)
        CASPAR_LOG(error) << "Cannot restore thread CPU affinity: " << std::strerror(error);
    return false;
}

int thread_affinity_capacity() { return CPU_SETSIZE; }

} // namespace caspar
