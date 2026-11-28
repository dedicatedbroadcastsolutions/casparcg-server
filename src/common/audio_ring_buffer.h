#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace caspar {

// One writer and one reader. Counts are interleaved PCM frames, not scalar samples.
template <typename Sample>
class audio_ring_buffer
{
    static_assert(std::is_trivially_copyable_v<Sample>);

  public:
    audio_ring_buffer(std::size_t capacity_frames, std::size_t channels)
        : channels_(channels)
        , capacity_(storage_capacity(capacity_frames, channels))
        , buffer_(capacity_ * channels)
    {
    }

    std::size_t channels() const { return channels_; }
    std::size_t capacity() const { return capacity_; }

    // Running frame counts: everything ever written and read. Frame n of the stream is the n-th
    // frame written, so these place any queued frame on the stream's timeline.
    std::size_t write_index() const { return write_.load(std::memory_order_acquire); }
    std::size_t read_index() const { return read_.load(std::memory_order_acquire); }

    // Frames currently queued, exact from either side at the moment of the call.
    std::size_t size() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }

    std::size_t push(const Sample* frames, std::size_t frame_count)
    {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read  = read_.load(std::memory_order_acquire);
        const auto count = std::min(frame_count, capacity_ - (write - read));
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(&buffer_[((write + i) & (capacity_ - 1)) * channels_],
                        &frames[i * channels_],
                        channels_ * sizeof(Sample));
        }
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    // Writer only: appends silence, for pre-roll and for re-centring after a fault.
    std::size_t push_silence(std::size_t frame_count)
    {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read  = read_.load(std::memory_order_acquire);
        const auto count = std::min(frame_count, capacity_ - (write - read));
        for (std::size_t i = 0; i < count; ++i) {
            std::fill_n(&buffer_[((write + i) & (capacity_ - 1)) * channels_], channels_, Sample{});
        }
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    std::size_t pop(Sample* frames, std::size_t frame_count)
    {
        const auto read  = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto count = std::min(frame_count, write - read);
        for (std::size_t i = 0; i < count; ++i) {
            std::memcpy(&frames[i * channels_],
                        &buffer_[((read + i) & (capacity_ - 1)) * channels_],
                        channels_ * sizeof(Sample));
        }
        if (count < frame_count) {
            std::memset(&frames[count * channels_], 0, (frame_count - count) * channels_ * sizeof(Sample));
        }
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    // Reader only: discard queued PCM after a transport discontinuity.
    void discard() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

    // Reader only: discards up to frame_count queued frames and returns how many went.
    std::size_t discard(std::size_t frame_count)
    {
        const auto read  = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto count = std::min(frame_count, write - read);
        read_.store(read + count, std::memory_order_release);
        return count;
    }

  private:
    static std::size_t storage_capacity(std::size_t requested, std::size_t channels)
    {
        if (requested == 0 || channels == 0 || requested == std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument("invalid audio ring dimensions");
        }
        std::size_t capacity = 1;
        while (capacity <= requested) {
            if (capacity > std::numeric_limits<std::size_t>::max() / 2) {
                throw std::length_error("audio ring is too large");
            }
            capacity *= 2;
        }
        if (capacity > std::numeric_limits<std::size_t>::max() / channels / sizeof(Sample)) {
            throw std::length_error("audio ring is too large");
        }
        return capacity;
    }

    const std::size_t   channels_;
    const std::size_t   capacity_;
    std::vector<Sample> buffer_;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

} // namespace caspar
