/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Julian Waller, julian@superfly.tv
 */

#include "monitor.h"

#include <common/log.h>

#include <iomanip>
#include <utility>

namespace caspar { namespace decklink {

audio_schedule_monitor::audio_schedule_monitor(std::wstring label)
    : label_(std::move(label))
{
}

void audio_schedule_monitor::report(HRESULT             result,
                                    BMDTimeValue        stream_time,
                                    int                 sample_frames,
                                    diagnostics::graph& graph)
{
    if (FAILED(result)) {
        ++failures_;
        graph.set_tag(diagnostics::tag_severity::WARNING, "audio-schedule-failed");
        if (!last_error_ || *last_error_ != result) {
            CASPAR_LOG(error) << label_ << L" ScheduleAudioSamples failed (HRESULT=0x" << std::hex
                              << static_cast<std::uint32_t>(result) << std::dec << L", stream-time=" << stream_time
                              << L", sample-frames=" << sample_frames << L"). Audio may be missing; "
                              << L"continuing the output timeline without restarting the device.";
        }
        last_error_ = result;
    } else if (last_error_) {
        CASPAR_LOG(info) << label_ << L" ScheduleAudioSamples is accepting audio again after " << failures_
                         << L" failed submissions. This does not confirm audible output.";
        last_error_.reset();
        failures_ = 0;
    }
}

core::monitor::state get_state_for_port(const port_configuration&      port_config,
                                        const core::video_format_desc& channel_format)
{
    core::monitor::state state;

    state["index"]    = port_config.device_index;
    state["key-only"] = port_config.key_only;

    if (port_config.format.format == core::video_format::invalid) {
        state["video-mode"] = channel_format.name;
    } else {
        state["video-mode"] = port_config.format.name;
    }

    if (port_config.has_subregion_geometry()) {
        state["subregion/src-x"]  = port_config.src_x;
        state["subregion/src-y"]  = port_config.src_y;
        state["subregion/src-x"]  = port_config.dest_x;
        state["subregion/dest-y"] = port_config.dest_y;
        state["subregion/width"]  = port_config.region_w;
        state["subregion/height"] = port_config.region_h;
    }

    return state;
}

core::monitor::state get_state_for_config(const configuration& config, const core::video_format_desc& channel_format)
{
    core::monitor::state state;

    state["decklink"] = get_state_for_port(config.primary, channel_format);

    state["decklink/embedded-audio"] = config.embedded_audio;
    state["decklink/buffer-depth"]   = config.base_buffer_depth;

    if (config.keyer == configuration::keyer_t::external_keyer) {
        state["decklink/keyer"] = std::wstring(L"external");
    } else if (config.keyer == configuration::keyer_t::internal_keyer) {
        state["decklink/keyer"] = std::wstring(L"internal");
    } else if (config.keyer == configuration::keyer_t::disabled_keyer) {
        state["decklink/keyer"] = std::wstring(L"disabled");
    }

    if (config.latency == configuration::latency_t::low_latency) {
        state["decklink/latency"] = std::wstring(L"low");
    } else if (config.latency == configuration::latency_t::normal_latency) {
        state["decklink/latency"] = std::wstring(L"normal");
    }

    int index = 0;
    for (auto& port_config : config.secondaries) {
        state["decklink/ports"][index++] = get_state_for_port(port_config, channel_format);
    }

    return state;
}

}} // namespace caspar::decklink