/*
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
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace caspar { namespace proaudio {

// Single-producer/single-consumer lock-free ring buffer of interleaved audio frames.
//
// push() and pop() never block, allocate or take a lock, so either may run on a realtime audio
// callback. push() must only ever be called from one fixed thread and pop() from another; size()
// may be read from either.
//
// One "frame" is `channels` interleaved samples of type T.
template <typename T>
class frame_ring_buffer
{
  public:
    frame_ring_buffer(std::size_t capacity_frames, std::size_t channels)
        : channels_(channels)
        , capacity_(next_pow2(capacity_frames + 1))
        , mask_(capacity_ - 1)
        , buffer_(capacity_ * channels)
    {
    }

    std::size_t channels() const { return channels_; }
    std::size_t capacity() const { return capacity_; }

    // Running frame indices: everything ever written / read. Sample n of the stream is the n-th
    // frame written, so these place any queued frame on the stream's timeline.
    std::size_t write_index() const { return write_.load(std::memory_order_acquire); }
    std::size_t read_index() const { return read_.load(std::memory_order_acquire); }

    // Frames currently queued. Exact from either side at the moment of the call.
    std::size_t size() const
    {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }

    // Writer side. Returns the number of whole frames written (less than requested if full).
    std::size_t push(const T* frames, std::size_t frame_count)
    {
        auto write = write_.load(std::memory_order_relaxed);
        auto read  = read_.load(std::memory_order_acquire);

        auto to_write = std::min(frame_count, capacity_ - (write - read));
        copy_in(write, frames, to_write);
        write_.store(write + to_write, std::memory_order_release);
        return to_write;
    }

    // Writer side. Appends silence; used to pre-roll and to re-centre after a fault.
    std::size_t push_silence(std::size_t frame_count)
    {
        auto write = write_.load(std::memory_order_relaxed);
        auto read  = read_.load(std::memory_order_acquire);

        auto to_write = std::min(frame_count, capacity_ - (write - read));
        for (std::size_t i = 0; i < to_write; ++i)
            std::fill_n(&buffer_[((write + i) & mask_) * channels_], channels_, T{});
        write_.store(write + to_write, std::memory_order_release);
        return to_write;
    }

    // Reader side. Missing frames (underrun) are zero-filled. Returns the number of frames that
    // were real audio, so the caller can count an underrun.
    std::size_t pop(T* out_frames, std::size_t frame_count)
    {
        auto read  = read_.load(std::memory_order_relaxed);
        auto write = write_.load(std::memory_order_acquire);

        auto to_read = std::min(frame_count, write - read);
        for (std::size_t i = 0; i < to_read; ++i)
            std::memcpy(&out_frames[i * channels_], &buffer_[((read + i) & mask_) * channels_], channels_ * sizeof(T));
        if (to_read < frame_count)
            std::fill_n(&out_frames[to_read * channels_], (frame_count - to_read) * channels_, T{});

        read_.store(read + to_read, std::memory_order_release);
        return to_read;
    }

    // Reader side. Discards up to frame_count frames; used to re-centre after a fault.
    std::size_t discard(std::size_t frame_count)
    {
        auto read  = read_.load(std::memory_order_relaxed);
        auto write = write_.load(std::memory_order_acquire);

        auto to_drop = std::min(frame_count, write - read);
        read_.store(read + to_drop, std::memory_order_release);
        return to_drop;
    }

  private:
    void copy_in(std::size_t write, const T* frames, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
            std::memcpy(&buffer_[((write + i) & mask_) * channels_], &frames[i * channels_], channels_ * sizeof(T));
    }

    static std::size_t next_pow2(std::size_t v)
    {
        std::size_t p = 1;
        while (p < v)
            p <<= 1;
        return p;
    }

    const std::size_t channels_;
    const std::size_t capacity_;
    const std::size_t mask_;

    std::vector<T> buffer_;

    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

}} // namespace caspar::proaudio
