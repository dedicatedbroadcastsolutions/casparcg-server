#pragma once

#include <core/frame/frame.h>
#include <core/video_format.h>

#include <common/log.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace caspar { namespace newtek {

class ndi_frame_buffer
{
    mutable std::mutex            mutex_;
    std::condition_variable       condition_;
    std::queue<core::const_frame> frames_;
    int                           capacity_     = 16;
    int                           field_count_  = 1;
    bool                          accept_frame_ = true;
    bool                          stopped_      = true;

  public:
    void reset(int capacity, int field_count)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Reserve complete field pairs so overload cannot invert field order.
        capacity_ = std::max(field_count, capacity - capacity % field_count);
        if (capacity_ != capacity) {
            CASPAR_LOG(warning) << "NDI buffer size adjusted from " << capacity << " to " << capacity_ << " fields";
        }
        field_count_  = field_count;
        frames_       = {};
        accept_frame_ = true;
        stopped_      = false;
    }

    bool push(core::video_field field, core::const_frame frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return false;

        if (field != core::video_field::b)
            accept_frame_ = frames_.size() + field_count_ <= static_cast<std::size_t>(capacity_);
        if (!accept_frame_)
            return false;

        frames_.push(std::move(frame));
        condition_.notify_one();
        return true;
    }

    bool wait_for_preroll()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] {
            return stopped_ || frames_.size() >= static_cast<std::size_t>(std::min(8, capacity_));
        });
        return !stopped_;
    }

    core::const_frame pop()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return stopped_ || !frames_.empty(); });
        if (stopped_)
            return {};
        auto frame = frames_.front();
        frames_.pop();
        return frame;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        frames_  = {};
        condition_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.size();
    }

    int capacity() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }
};

}} // namespace caspar::newtek
