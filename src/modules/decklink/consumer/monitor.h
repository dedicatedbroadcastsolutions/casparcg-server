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

#pragma once

#include "../decklink_api.h"
#include "config.h"

#include <core/monitor/monitor.h>

#include <common/diagnostics/graph.h>

#include <cstdint>
#include <optional>

namespace caspar { namespace decklink {

core::monitor::state get_state_for_config(const configuration& config, const core::video_format_desc& channel_format);

class audio_schedule_monitor
{
    std::optional<HRESULT> last_error_;
    std::uint64_t         failures_ = 0;
    std::wstring          label_;

  public:
    explicit audio_schedule_monitor(std::wstring label);

    /// Counts every failed submission while logging only error transitions and resumed SDK acceptance.
    void report(HRESULT result, BMDTimeValue stream_time, int sample_frames, diagnostics::graph& graph);
};

}} // namespace caspar::decklink