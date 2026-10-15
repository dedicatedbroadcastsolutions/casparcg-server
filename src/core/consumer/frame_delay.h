#pragma once

#include "../frame/frame.h"

#include <common/timespan.h>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <utility>

namespace caspar { namespace core {

class frame_delay
{
    std::deque<const_frame> frames_;
    std::size_t             delay_fields_ = 0;
    int                     field_count_  = 1;

  public:
    void reset(const timespan& delay, double frame_rate, int field_count)
    {
        const auto requested = delay.in_frames(frame_rate);
        const auto frames    = std::clamp(requested, 0, static_cast<int>(frame_rate));
        if (requested != frames) {
            CASPAR_LOG(warning) << "Consumer output delay clamped from " << requested << " to " << frames << " frames";
        }
        frames_.clear();
        field_count_  = field_count;
        delay_fields_ = static_cast<std::size_t>(frames) * field_count_;
    }

    const_frame push(const_frame frame)
    {
        if (delay_fields_ == 0)
            return frame;

        frames_.push_back(std::move(frame));
        if (frames_.size() <= delay_fields_) {
            // Repeat the first frame's matching field while the delay fills.
            return frames_[(frames_.size() - 1) % field_count_];
        }

        auto result = frames_.front();
        frames_.pop_front();
        return result;
    }
};

}} // namespace caspar::core
