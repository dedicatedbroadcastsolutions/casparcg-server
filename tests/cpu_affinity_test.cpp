#include <common/os/cpu_affinity.h>
#include <common/os/thread.h>
#include <common/executor.h>

#include <future>
#include <iostream>
#include <stdexcept>

#ifndef _WIN32
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

namespace {

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check_parser()
{
    using caspar::parse_cpu_list;
    require(parse_cpu_list(L"", 64).empty(), "Empty list is not a no-op");
    require(parse_cpu_list(L" \t\n ", 64).empty(), "Whitespace list is not a no-op");
    require(parse_cpu_list(L"0-3,6, 8-9 ", 64) == std::vector<int>({0, 1, 2, 3, 6, 8, 9}), "Incorrect range parsing");
    require(parse_cpu_list(L"3,1-3,0,1", 64) == std::vector<int>({0, 1, 2, 3}), "CPU IDs were not deduplicated");
    require(parse_cpu_list(L"63", 64) == std::vector<int>({63}), "Last supported CPU was rejected");
    require(parse_cpu_list(L"1023", 1024) == std::vector<int>({1023}), "Linux mask limit was rejected");

    for (const auto* invalid : {L"-1", L"0,-1", L"3-1", L"1x", L"1-2x", L"1-2-3", L"1-", L",1", L"1,",
                               L"1,,2", L"+1", L"1.5", L"1 2", L"64", L"0-64", L"2147483647",
                               L"9999999999999999999999999", L"0-2147483647"}) {
        bool rejected = false;
        try {
            static_cast<void>(parse_cpu_list(invalid, 64));
        } catch (const caspar::user_error&) {
            rejected = true;
        }
        require(rejected, "Malformed CPU list was accepted");
    }
}

#ifndef _WIN32
cpu_set_t current_mask()
{
    cpu_set_t mask;
    require(pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask) == 0, "Cannot inspect CPU affinity");
    return mask;
}

void check_linux_affinity()
{
    const auto original = current_mask();
    int cpu = 0;
    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &original))
        ++cpu;
    require(cpu < CPU_SETSIZE, "No usable CPU in current mask");
    require(caspar::thread_affinity_capacity() == CPU_SETSIZE, "Incorrect Linux affinity capacity");

    // Change only temporary worker threads, never the test runner's affinity.
    auto worker = std::async(std::launch::async, [cpu] {
        const auto before = current_mask();
        require(caspar::set_thread_affinity({}), "Empty mask failed");
        const auto unchanged = current_mask();
        require(CPU_EQUAL(&before, &unchanged), "Empty mask changed affinity");
        require(!caspar::set_thread_affinity({-1}), "Negative CPU ID was accepted");
        require(!caspar::set_thread_affinity({CPU_SETSIZE}), "Oversized CPU ID was accepted");
        require(!caspar::set_thread_affinity({cpu, CPU_SETSIZE}), "Invalid CPU was silently discarded");
        const auto after_invalid = current_mask();
        require(CPU_EQUAL(&before, &after_invalid), "Invalid mask changed affinity");

        require(caspar::set_thread_affinity({cpu, cpu}), "Valid CPU affinity failed");
        const auto pinned = current_mask();
        require(CPU_COUNT(&pinned) == 1 && CPU_ISSET(cpu, &pinned), "Worker did not run on the requested mask");
        auto child = std::async(std::launch::async, [] { return current_mask(); });
        const auto inherited = child.get();
        require(CPU_EQUAL(&pinned, &inherited), "Child worker did not inherit affinity");

        const auto unavailable = sysconf(_SC_NPROCESSORS_CONF);
        if (unavailable > 0 && unavailable < CPU_SETSIZE) {
            require(!caspar::set_thread_affinity({static_cast<int>(unavailable)}), "Unavailable CPU was accepted");
            require(!caspar::set_thread_affinity({cpu, static_cast<int>(unavailable)}), "Partial mask was accepted");
            const auto restored = current_mask();
            require(CPU_EQUAL(&pinned, &restored), "Partial mask did not restore previous affinity");
        }
    });
    worker.get();

    caspar::executor executor(L"affinity-test", {cpu});
    const auto actual = executor.begin_invoke([] { return current_mask(); }).get();
    require(CPU_COUNT(&actual) == 1 && CPU_ISSET(cpu, &actual), "Executor did not apply configured affinity");
    caspar::executor default_executor(L"default-affinity-test");
    const auto default_mask = default_executor.begin_invoke([] { return current_mask(); }).get();
    require(CPU_EQUAL(&original, &default_mask), "Unconfigured executor changed inherited affinity");
    const auto after = current_mask();
    require(CPU_EQUAL(&original, &after), "Tests changed the main thread's affinity");
}
#endif

} // namespace

int main()
{
    try {
        check_parser();
#ifndef _WIN32
        check_linux_affinity();
#endif
        std::cout << "CPU affinity regression checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
