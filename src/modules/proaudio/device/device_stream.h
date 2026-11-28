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

#include "../util/device_util.h"
#include "../util/ring_buffer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace caspar { namespace proaudio {

// The part of one consumer that the realtime callback reads: its queue of output samples and the
// device channels they go to. Owned by the consumer; the stream only borrows it between attach()
// and detach().
struct output_port
{
    output_port(std::vector<int> device_channels, std::size_t capacity_frames)
        : channels(std::move(device_channels))
        , ring(capacity_frames, channels.size())
        , scratch(max_block_frames * channels.size())
    {
    }

    // Largest block the callback renders at once; larger host blocks are rendered in pieces.
    static constexpr std::size_t max_block_frames = 4096;

    const std::vector<int>   channels; // 0-based device channel per port channel
    frame_ring_buffer<float> ring;
    std::vector<float>       scratch; // touched only by the realtime callback

    // Frames the writer asks the reader to throw away, to re-centre the queue after a fault
    // without the writer touching the read index.
    std::atomic<std::size_t>   drop_request{0};
    std::atomic<std::uint64_t> underrun_frames{0};

    // When the frame at ring read index `index` leaves the driver for the device (the DAC, or for
    // a network soundcard the network), on the steady clock. Written by every callback, read by
    // the consumer's controller; a sequence counter makes the pair consistent (seqlock).
    struct output_time
    {
        std::size_t   index;
        std::int64_t  steady_ns;
    };
    void publish(output_time t)
    {
        seq_.fetch_add(1, std::memory_order_acq_rel); // odd: writing
        out_index_.store(t.index, std::memory_order_relaxed);
        out_ns_.store(t.steady_ns, std::memory_order_relaxed);
        seq_.fetch_add(1, std::memory_order_acq_rel); // even: stable
    }
    bool read_output_time(output_time& t) const
    {
        for (int attempt = 0; attempt < 8; ++attempt) {
            auto before = seq_.load(std::memory_order_acquire);
            if (before & 1)
                continue;
            t.index     = out_index_.load(std::memory_order_relaxed);
            t.steady_ns = out_ns_.load(std::memory_order_relaxed);
            if (seq_.load(std::memory_order_acquire) == before)
                return before != 0;
        }
        return false;
    }

  private:
    std::atomic<std::uint64_t> seq_{0};
    std::atomic<std::size_t>   out_index_{0};
    std::atomic<std::int64_t>  out_ns_{0};
};

// Optional capture of two device input channels for the sync probe. The callback only copies;
// the analysis runs on the probe's own thread.
struct probe_tap
{
    probe_tap(int a, int b, std::size_t capacity_frames)
        : channel_a(a)
        , channel_b(b)
        , ring(capacity_frames, 2)
    {
    }

    const int                channel_a; // 0-based device input channels
    const int                channel_b;
    frame_ring_buffer<float> ring;
};

struct stream_settings
{
    std::string device_name;
    std::string host_api;
    int         sample_rate        = 48000;
    int         buffer_size_frames = 0;    // 0: the driver's preferred size
    double      latency_ms         = -1.0; // <0: the device's default low latency
};

// One open PortAudio stream on one device, shared by every consumer that uses the device.
//
// ASIO drivers accept a single client stream, so several CasparCG channels writing to one Dante
// Virtual Soundcard must share a stream, each filling its own device channels. The stream is
// opened with every output (and input) channel the device has, so consumers can come and go
// without reopening it, and is restarted by a watchdog if the driver stops calling back (for
// example when the soundcard service restarts).
class device_stream
{
  public:
    static std::shared_ptr<device_stream> acquire(const stream_settings& settings);

    ~device_stream();

    // Adds a port; it is rendered from the next callback on. Throws if a channel is out of range
    // or already used by another port.
    void attach(output_port* port);
    // Removes a port and returns only once the callback can no longer touch it.
    void detach(output_port* port);

    void attach(probe_tap* tap);
    void detach(probe_tap* tap);

    int  output_channels() const { return output_channels_; }
    int  input_channels() const { return input_channels_; }
    int  sample_rate() const { return settings_.sample_rate; }
    bool running() const { return running_.load(std::memory_order_acquire); }

    // Driver-reported output latency in milliseconds (0 while not running).
    double output_latency_ms() const { return output_latency_ms_.load(std::memory_order_relaxed); }
    // Counts of callbacks and of callbacks the driver flagged as having underflowed.
    std::uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }
    std::uint64_t device_underflows() const { return device_underflows_.load(std::memory_order_relaxed); }
    // Increments every time the stream is (re)started, so a consumer can re-centre its queue.
    std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

    const std::string& device_label() const { return device_label_; }

  private:
    explicit device_stream(const stream_settings& settings);

    void open();
    void close();
    void close_locked(); // with pa_mutex_ held
    void watchdog();
    void wait_for_callbacks(std::uint64_t count);
    // Re-reads PortAudio's device list, so a driver that was not ready when PortAudio started can
    // be found. Only possible while no stream is open on any device; called under pa_mutex_.
    bool rescan_devices();

    static int callback(const void*                     input,
                        void*                           output,
                        unsigned long                   frame_count,
                        const PaStreamCallbackTimeInfo* time_info,
                        PaStreamCallbackFlags           status_flags,
                        void*                           user_data);

    static constexpr std::size_t max_ports  = 32;
    static constexpr std::size_t max_probes = 4;
    // Channel range accepted while the device is not available yet and its size is unknown.
    static constexpr int max_pending_channels = 512;

    const stream_settings       settings_;
    std::shared_ptr<pa_library> library_;
    std::string                 device_label_;

    std::mutex              control_mutex_; // serialises attach/detach/open/close
    std::condition_variable released_;      // a port gave its channels back
    PaStream*  stream_          = nullptr;
    int        output_channels_ = 0;
    int        input_channels_  = 0;

    std::vector<bool> claimed_; // device output channels in use, under control_mutex_

    std::array<std::atomic<output_port*>, max_ports> ports_{};
    std::array<std::atomic<probe_tap*>, max_probes>  probes_{};

    std::atomic<bool>          running_{false};
    std::atomic<std::uint64_t> callbacks_{0};
    std::atomic<std::uint64_t> device_underflows_{0};
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<double>        output_latency_ms_{0.0};

    std::mutex              watchdog_mutex_;
    std::condition_variable watchdog_cv_;
    bool                    stopping_ = false;
    std::thread             watchdog_thread_;

    // Throttles for a device that stays missing: the retry itself runs every few seconds.
    std::chrono::steady_clock::time_point last_failure_log_{};
    std::chrono::steady_clock::time_point last_rescan_{};

    inline static std::mutex                                         registry_mutex_;
    inline static std::vector<std::weak_ptr<device_stream>>          registry_;

    // PortAudio's device list is process-wide: opening, closing and rescanning on any device
    // take this lock (always after a stream's control_mutex_), and a rescan needs zero open streams.
    inline static std::mutex pa_mutex_;
    inline static int        open_streams_ = 0;
};

}} // namespace caspar::proaudio
