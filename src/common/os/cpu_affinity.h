#pragma once

#include "../except.h"

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace caspar {

inline std::vector<int> parse_cpu_list(const std::wstring& spec, int capacity)
{
    if (boost::trim_copy(spec).empty())
        return {};

    std::vector<int> result;
    std::vector<std::wstring> parts;
    boost::split(parts, spec, boost::is_any_of(L","));
    for (auto part : parts) {
        boost::trim(part);
        auto invalid = [&] {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid cpu-affinity entry \"" + part +
                                                           L"\"; expected CPU IDs or ascending ranges within 0.." +
                                                           std::to_wstring(capacity - 1)));
        };
        auto parse_id = [&](const std::wstring& text) {
            if (text.empty())
                invalid();
            int value = 0;
            for (auto c : text) {
                if (c < L'0' || c > L'9')
                    invalid();
                const int digit = c - L'0';
                if (value > (capacity - 1) / 10 ||
                    (value == (capacity - 1) / 10 && digit > (capacity - 1) % 10))
                    invalid();
                value = value * 10 + digit;
            }
            return value;
        };

        const auto dash  = part.find(L'-');
        const int  first = parse_id(part.substr(0, dash));
        const int  last  = dash == std::wstring::npos ? first : parse_id(part.substr(dash + 1));
        if (last < first)
            invalid();
        for (int cpu = first; cpu <= last; ++cpu)
            result.push_back(cpu);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

} // namespace caspar
