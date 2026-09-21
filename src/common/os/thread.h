#pragma once

#include <string>
#include <vector>

namespace caspar {

void set_thread_name(const std::wstring& name);
void set_thread_realtime_priority();

// Pins the calling thread to the given set of logical CPU ids. A no-op (returns true) if cpu_ids is empty, so call
// sites can pass a possibly-unconfigured affinity list unconditionally. Returns false if the underlying OS call
// failed.
bool set_thread_affinity(const std::vector<int>& cpu_ids);

} // namespace caspar
