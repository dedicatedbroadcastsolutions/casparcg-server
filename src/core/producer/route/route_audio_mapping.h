#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace caspar { namespace core {

inline std::wstring_view trim_route_audio_map_entry(std::wstring_view value)
{
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) {
        return {};
    }

    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline int parse_route_audio_map_index(std::wstring_view value)
{
    if (value.empty()) {
        throw std::invalid_argument("empty audio channel index");
    }

    int result = 0;
    for (const auto digit : value) {
        if (digit < L'0' || digit > L'9') {
            throw std::invalid_argument("audio channel indices must be non-negative integers");
        }

        const int number = digit - L'0';
        if (result > (std::numeric_limits<int>::max() - number) / 10) {
            throw std::invalid_argument("audio channel index is too large");
        }
        result = result * 10 + number;
    }
    return result;
}

inline std::vector<int> identity_route_audio_mapping(int channels)
{
    if (channels <= 0)
        throw std::invalid_argument("audio channel count must be positive");
    std::vector<int> mapping(static_cast<std::size_t>(channels));
    for (int channel = 0; channel < channels; ++channel)
        mapping[channel] = channel;
    return mapping;
}

// Each entry patches destination=source; -1 explicitly selects silence.
inline std::vector<int> parse_route_audio_mapping(const std::wstring&     value,
                                                  int                     source_channels,
                                                  int                     destination_channels,
                                                  const std::vector<int>& current)
{
    if (source_channels <= 0 || destination_channels <= 0) {
        throw std::invalid_argument("audio channel counts must be positive");
    }
    if (value.empty()) {
        throw std::invalid_argument("audio mapping cannot be empty");
    }

    if (current.size() != static_cast<std::size_t>(destination_channels))
        throw std::invalid_argument("invalid current audio mapping dimensions");
    auto              mapping = current;
    std::vector<bool> updated(static_cast<std::size_t>(destination_channels), false);
    std::size_t       entry_start = 0;
    while (entry_start < value.size()) {
        const auto comma  = value.find(L',', entry_start);
        auto       entry  = trim_route_audio_map_entry(std::wstring_view(value).substr(
            entry_start, comma == std::wstring::npos ? std::wstring::npos : comma - entry_start));
        const auto equals = entry.find(L'=');
        if (equals == std::wstring_view::npos || entry.find(L'=', equals + 1) != std::wstring_view::npos) {
            throw std::invalid_argument("each audio mapping entry must be destination=source");
        }

        const auto destination  = parse_route_audio_map_index(trim_route_audio_map_entry(entry.substr(0, equals)));
        const auto source_value = trim_route_audio_map_entry(entry.substr(equals + 1));
        const bool silence =
            source_value.size() == 7 &&
            std::equal(source_value.begin(), source_value.end(), L"SILENCE", [](wchar_t left, wchar_t right) {
                return left == right || left == right + (L'a' - L'A');
            });
        const auto source = silence ? -1 : parse_route_audio_map_index(source_value);
        if (destination >= destination_channels || source >= source_channels) {
            throw std::invalid_argument("audio channel index is outside the source or destination channel count");
        }
        if (updated[static_cast<std::size_t>(destination)]) {
            throw std::invalid_argument("audio mapping contains a duplicate destination");
        }
        mapping[static_cast<std::size_t>(destination)] = source;
        updated[static_cast<std::size_t>(destination)] = true;

        if (comma == std::wstring::npos) {
            entry_start = value.size();
        } else {
            entry_start = comma + 1;
            if (entry_start == value.size()) {
                throw std::invalid_argument("audio mapping contains an empty entry");
            }
        }
    }
    return mapping;
}

inline std::vector<int>
parse_route_audio_mapping(const std::wstring& value, int source_channels, int destination_channels)
{
    return parse_route_audio_mapping(
        value, source_channels, destination_channels, identity_route_audio_mapping(destination_channels));
}

inline std::vector<std::int32_t> remap_route_audio(const std::int32_t*     source,
                                                   std::size_t             source_size,
                                                   int                     source_channels,
                                                   int                     destination_channels,
                                                   const std::vector<int>& mapping)
{
    if (source_channels <= 0 || destination_channels <= 0 ||
        mapping.size() != static_cast<std::size_t>(destination_channels)) {
        throw std::invalid_argument("invalid route audio mapping dimensions");
    }

    const auto samples = source_size / static_cast<std::size_t>(source_channels) +
                         (source_size % static_cast<std::size_t>(source_channels) != 0 ? 1 : 0);
    if (samples > std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(destination_channels)) {
        throw std::length_error("route audio buffer is too large");
    }

    std::vector<std::int32_t> result(samples * static_cast<std::size_t>(destination_channels), 0);
    for (std::size_t sample = 0; sample < samples; ++sample) {
        for (int destination = 0; destination < destination_channels; ++destination) {
            const auto source_channel = mapping[static_cast<std::size_t>(destination)];
            if (source_channel < 0 || source_channel >= source_channels) {
                continue;
            }

            const auto source_index =
                sample * static_cast<std::size_t>(source_channels) + static_cast<std::size_t>(source_channel);
            if (source_index < source_size) {
                result[sample * static_cast<std::size_t>(destination_channels) +
                       static_cast<std::size_t>(destination)] = source[source_index];
            }
        }
    }
    return result;
}

}} // namespace caspar::core
