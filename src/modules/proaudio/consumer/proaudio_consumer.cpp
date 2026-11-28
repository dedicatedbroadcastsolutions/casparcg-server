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

// Multi-channel audio output to a PortAudio device (ASIO on Windows), held in lip sync with the
// channel's video output.
//
// Timing model. The channel is paced by its video output (DeckLink): every tick, the frame that
// is sent here leaves the SDI output a fixed number of frames later. This consumer never paces the
// channel and never blocks it. It queues each tick's audio and the device plays the queue out on
// its own clock. DELAY_MS is the time from the video tick to the moment that tick's first audio
// sample leaves the audio driver (for Dante Virtual Soundcard: is handed to the network). It is
// measured every tick from the driver's own output timestamps and held constant, so audio and
// video keep a fixed offset, set once by measurement against the SDI output.
//
// The two clocks differ (the video card's crystal or genlock against the audio network's PTP
// clock): even 10 ppm is 36 ms an hour. A fixed queue therefore needs the audio stretched or
// shrunk by those few ppm, continuously, as asynchronous bridges between clock domains do
// (zita-ajbridge, PipeWire's adaptive resampler, FFmpeg's aresample=async). The latency is
// measured every tick, smoothed, and a PI controller sets the resampler's ratio so the average
// latency sits exactly on the target. The ratio change is inaudible (hundredths of a cent); the
// result is a sample-accurate, drift-free offset, measured rather than assumed.

#include "proaudio_consumer.h"

#include "../device/device_stream.h"
#include "../util/device_util.h"
#include "../util/sync_probe.h"

#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/log.h>
#include <common/param.h>
#include <common/utf.h>

#include <core/consumer/channel_info.h>
#include <core/consumer/output_timeline.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/monitor/monitor.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>
#include <boost/property_tree/ptree.hpp>

// FFmpeg's headers do not build warning-free under MSVC's /WX; the OAL consumer does the same.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace caspar { namespace proaudio {

namespace {

// Resampler ratio limits. Real clock pairs differ by tens of ppm; the bound only matters while the
// controller pulls in after a start, and 500 ppm (0.9 cents) is still inaudible.
constexpr double max_correction_ppm = 500.0;
// The ratio is handed to swresample as a whole number of samples over this distance, so its
// resolution is 1e6 / distance ppm (about 2 ppm).
constexpr int compensation_distance = 480000;
// The audio is held against the picture, not against the moment frames are handed over: the
// video output publishes, for every frame it schedules, how long that frame takes from handover to
// leaving the card, and the controller works on the difference between that and the audio's own
// handover-to-output delay. Buffering anywhere before the outputs moves both alike and cancels.
//
// A stall (a decoder opening or seeking) needs no correction: every frame carries exactly its own
// audio, so while the channel stops, picture and audio stop together, and both resume together.
// Measured against the SDI audio at the mixing desk, the alignment does not move through a stall.
// The measurement, however, jumps about for several seconds while the video card's buffers drain
// and refill. So once aligned, nothing is ever stepped on a short disturbance: only ticks within
// outlier_ms of the held level feed the slow drift controller, and a different level is taken (in
// one exact move) only if it holds for accept_level_s, which only a real fault does, such as the
// audio device restarting. Ticks off the frame period by more than tick_jitter_ms mark the channel
// unsettled and are not used at all.
constexpr double tick_jitter_ms   = 3.0;
constexpr double settle_s         = 0.5;
constexpr double align_settle_s   = 2.0;
constexpr double step_window_s    = 0.5;
constexpr double align_window_s   = 2.0;
constexpr double align_spread_ms  = 1.0;
constexpr double outlier_ms       = 1.0;
constexpr double accept_level_s   = 20.0;
// Lock is declared once the smoothed fill stays within this of the target for lock_seconds.
constexpr double lock_tolerance_ms = 0.25;
constexpr double lock_seconds      = 3.0;

struct swr_deleter
{
    void operator()(SwrContext* s) const { swr_free(&s); }
};

} // namespace

struct consumer_settings
{
    std::string      device_name;
    std::string      host_api;
    std::vector<int> channel_map; // 1-based device channel per source channel; empty: all, 1:1
    double           delay_ms           = 0.0; // audio out minus picture out, ms
    int              buffer_size_frames = 0;
    double           latency_ms         = -1.0;
    std::vector<int> probe; // empty, or two 1-based device input channels
};

struct proaudio_consumer : public core::frame_consumer
{
    const consumer_settings settings_;

    spl::shared_ptr<diagnostics::graph> graph_;
    int                                 channel_index_ = -1;
    core::video_format_desc             format_desc_;

    // The channel's output timing, from channel_info. Read by state() on other threads, so it is
    // swapped atomically; until initialize() it is an instance nobody publishes to.
    std::atomic<std::shared_ptr<core::output_timeline>> timeline_{std::make_shared<core::output_timeline>()};

    std::shared_ptr<device_stream>       stream_;
    std::unique_ptr<output_port>         port_;
    std::unique_ptr<probe_tap>           tap_;
    std::unique_ptr<sync_probe>          probe_;
    std::unique_ptr<SwrContext, swr_deleter> swr_;

    std::vector<int>   channel_map_; // resolved against the channel's audio channel count
    std::vector<float> mapped_;      // one tick of source audio, mapped and converted
    std::vector<float> resampled_;

    // Controller state, executor thread only.
    double        target_frames_    = 0.0; // video tick to audio out of the driver
    double        smoothed_latency_ = 0.0;
    bool          have_latency_     = false;
    std::uint64_t underrun_baseline_ = 0;
    std::int64_t  last_tick_ns_      = 0;
    int           regular_ticks_     = 0;
    std::uint64_t video_updates_seen_ = 0;
    std::vector<double> align_window_;
    int           video_stale_ticks_  = 0;
    bool          relative_           = false;
    double        outlier_s_          = 0.0;
    double        step_sum_          = 0.0;
    int           step_count_        = 0;
    double        integral_ppm_   = 0.0;
    double        alpha_          = 0.0;
    double        kp_             = 0.0;
    double        ki_             = 0.0;
    int           locked_ticks_   = 0;
    std::uint64_t seen_generation_ = 0;
    bool          centred_        = false;
    std::uint64_t last_underruns_ = 0;

    // Published for state() and the diagnostics graph; read from other threads.
    std::atomic<double>        delay_ms_{0.0};
    std::atomic<double>        fill_ms_{0.0};
    std::atomic<double>        correction_ppm_{0.0};
    std::atomic<double>        latency_ms_{0.0};
    std::atomic<bool>          locked_{false};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> overrun_frames_{0};
    std::atomic<std::uint64_t> resyncs_{0};
    // Diagnostics: the raw measurement and why the controller is or is not using it.
    std::atomic<double>        raw_latency_ms_{0.0};
    std::atomic<int>           regular_ticks_out_{0};
    std::atomic<bool>          measured_out_{false};
    std::atomic<bool>          aligned_out_{false};
    std::atomic<bool>          failed_{false};

    executor executor_{L"proaudio_consumer"};

    explicit proaudio_consumer(consumer_settings settings)
        : settings_(std::move(settings))
    {
        using diagnostics::color;
        graph_->set_color("fill", color(0.2f, 0.9f, 0.9f));
        graph_->set_color("correction", color(1.0f, 0.8f, 0.1f));
        graph_->set_color("underrun", color(0.9f, 0.3f, 0.3f));
        graph_->set_color("overrun", color(0.6f, 0.3f, 0.9f));
        graph_->set_color("resync", color(1.0f, 0.1f, 0.1f));
        diagnostics::register_graph(graph_);
    }

    ~proaudio_consumer() override
    {
        executor_.invoke([this] { teardown(); });
    }

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int /*port_index*/) override
    {
        executor_.invoke([&] {
            teardown();
            format_desc_   = format_desc;
            channel_index_ = channel_info.index;
            if (channel_info.timeline) {
                timeline_.store(channel_info.timeline);
            }
            graph_->set_text(print());
            setup();
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        // Interlaced channels send one frame per field, each carrying that field's share of the
        // audio cadence, so every send is queued.
        if (!failed_.load(std::memory_order_acquire))
        {
            // The tick is stamped here, on the channel's output thread, as the frame is handed to
            // the outputs; the queue work below may run a little later on this consumer's thread.
            const auto tick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
            executor_.begin_invoke([this, frame = std::move(frame), tick_ns] { tick(frame, tick_ns); });
        }

        // Never pace or hold the channel: the video output owns the clock.
        return make_ready_future(true);
    }

    std::wstring print() const override
    {
        return L"proaudio[" + std::to_wstring(channel_index_) + L"|" +
               u16(stream_ ? stream_->device_label() : settings_.device_name) + L"]";
    }

    std::wstring name() const override { return L"proaudio"; }

    bool has_synchronization_clock() const override { return false; }

    // Only the audio is used, so the mixer need not read the picture back to the host.
    bool needs_host_frame() const override { return false; }

    int index() const override { return 520; }

    core::monitor::state state() const override
    {
        core::monitor::state state;
        state["proaudio/device"]         = stream_ ? stream_->device_label() : settings_.device_name;
        state["proaudio/running"]        = stream_ && stream_->running();
        state["proaudio/delay_ms"]       = delay_ms_.load();
        state["proaudio/latency_ms"]     = latency_ms_.load();
        state["proaudio/fill_ms"]        = fill_ms_.load();
        state["proaudio/correction_ppm"] = correction_ppm_.load();
        state["proaudio/locked"]         = locked_.load();
        state["proaudio/underruns"]      = underruns_.load();
        state["proaudio/overruns"]       = overrun_frames_.load();
        state["proaudio/resyncs"]        = resyncs_.load();
        {
            const auto tl                          = timeline_.load();
            state["proaudio/video/late_frames"]    = tl->late_frames.load();
            state["proaudio/video/dropped_frames"] = tl->dropped_frames.load();
        }
        state["proaudio/debug/raw_latency_ms"] = raw_latency_ms_.load();
        state["proaudio/debug/regular_ticks"]  = regular_ticks_out_.load();
        state["proaudio/debug/measured"]       = measured_out_.load();
        state["proaudio/debug/aligned"]        = aligned_out_.load();
        if (probe_) {
            auto r                            = probe_->latest();
            state["proaudio/probe/valid"]      = r.valid;
            state["proaudio/probe/offset_ms"]  = r.offset_ms;
            state["proaudio/probe/confidence"] = r.confidence;
            state["proaudio/probe/level_dbfs"] = {r.level_a_dbfs, r.level_b_dbfs};
        }
        return state;
    }

  private:
    void setup()
    {
        try {
            const auto rate     = format_desc_.audio_sample_rate;
            const auto cadence  = *std::max_element(format_desc_.audio_cadence.begin(), format_desc_.audio_cadence.end());
            const auto fps      = format_desc_.fps > 0 ? format_desc_.fps : 25.0;
            channel_map_ = settings_.channel_map;
            if (channel_map_.empty())
                for (int i = 1; i <= format_desc_.audio_channels; ++i)
                    channel_map_.push_back(i);
            const auto map_size = channel_map_.size();

            stream_settings ss;
            ss.device_name        = settings_.device_name;
            ss.host_api           = settings_.host_api;
            ss.sample_rate        = rate;
            ss.buffer_size_frames = settings_.buffer_size_frames;
            ss.latency_ms         = settings_.latency_ms;
            stream_               = device_stream::acquire(ss);

            std::vector<int> device_channels;
            for (auto ch : channel_map_)
                device_channels.push_back(ch - 1);

            // Audio out of the driver this long after the picture leaves the video card (may be
            // negative: audio ahead of the picture, to make up for a slower audio path after it).
            const double delay_ms = settings_.delay_ms;
            target_frames_        = delay_ms * rate / 1000.0;
            delay_ms_.store(delay_ms);

            // The queue must hold the target, a tick of headroom both ways, and the largest block
            // the driver may take at once.
            // The queue holds the picture's own delay plus the target (up to a few hundred ms),
            // a tick of headroom both ways, and the largest block the driver may take at once.
            const auto capacity = static_cast<std::size_t>(rate / 2 + std::abs(target_frames_)) +
                                  4 * static_cast<std::size_t>(cadence) + 2 * output_port::max_block_frames;
            port_ = std::make_unique<output_port>(device_channels, capacity);

            // Controller gains. The plant is an integrator: a ratio error of x ppm moves the fill
            // by x * 1e-6 * rate samples per second. kp sets the pull-in time constant (about
            // 5 s), ki is chosen for critical damping, and the latency is smoothed over about 1 s,
            // which is fast against the loop and averages out tick and driver-block jitter.
            const double g = 1e-6 * rate;
            kp_            = 4.0;
            const double wn = kp_ * g / 2.0;
            ki_            = wn * wn / g / fps;
            alpha_         = 1.0 / fps;

            SwrContext*     raw    = nullptr;
            AVChannelLayout layout = {};
            layout.order           = AV_CHANNEL_ORDER_UNSPEC;
            layout.nb_channels     = static_cast<int>(map_size);
            if (swr_alloc_set_opts2(&raw, &layout, AV_SAMPLE_FMT_FLT, rate, &layout, AV_SAMPLE_FMT_FLT, rate, 0, nullptr) < 0)
                CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("proaudio: swr_alloc_set_opts2 failed"));
            swr_.reset(raw);
            // Equal rates would bypass the resampler; force it so the ratio can be trimmed.
            av_opt_set_int(swr_.get(), "flags", SWR_FLAG_RESAMPLE, 0);
            // Linear interpolation between filter phases gives a smooth, continuously variable
            // ratio instead of stepping between the 1024 precomputed phases.
            av_opt_set_int(swr_.get(), "linear_interp", 1, 0);
            if (swr_init(swr_.get()) < 0)
                CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("proaudio: swr_init failed"));

            mapped_.resize(static_cast<std::size_t>(cadence) * 2 * map_size);
            resampled_.resize((static_cast<std::size_t>(cadence) * 2 + 256) * map_size);

            stream_->attach(port_.get());
            seen_generation_ = 0;
            centred_         = false;
            integral_ppm_    = 0.0;
            smoothed_latency_ = target_frames_;
            have_latency_     = false;

            if (settings_.probe.size() == 2) {
                tap_   = std::make_unique<probe_tap>(settings_.probe[0] - 1, settings_.probe[1] - 1, 1 << 17);
                stream_->attach(tap_.get());
                probe_ = std::make_unique<sync_probe>(*tap_, rate);
            }

            CASPAR_LOG(info) << print() << L" source channels 1-" << map_size << L" to device channels "
                             << u16(boost::algorithm::join(to_strings(channel_map_), ",")) << L", delay "
                             << delay_ms << L" ms";
        } catch (...) {
            CASPAR_LOG_CURRENT_EXCEPTION();
            failed_.store(true, std::memory_order_release);
            teardown();
            throw;
        }
    }

    void teardown()
    {
        probe_.reset();
        if (stream_ && tap_)
            stream_->detach(tap_.get());
        tap_.reset();
        if (stream_ && port_)
            stream_->detach(port_.get());
        port_.reset();
        stream_.reset();
        swr_.reset();
    }

    static std::vector<std::string> to_strings(const std::vector<int>& v)
    {
        std::vector<std::string> out;
        for (auto x : v)
            out.push_back(std::to_string(x));
        return out;
    }

    // Queued frames as the device will see them: what is in the ring, less any drop still pending.
    double queued_frames() const
    {
        return static_cast<double>(port_->ring.size()) - static_cast<double>(port_->drop_request.load());
    }

    // Shifts all audio still to be written by `frames` at once: silence in to delay it, a drop
    // request to bring it forward. Used to start on target and to recover from a fault; ordinary
    // drift is trimmed by the resampler instead.
    void shift(double frames)
    {
        const auto n = static_cast<std::size_t>(std::lround(std::abs(frames)));
        if (frames > 0)
            port_->ring.push_silence(n);
        else if (frames < 0)
            port_->drop_request.fetch_add(std::min<std::size_t>(n, static_cast<std::size_t>(std::max(0.0, queued_frames()))));
        locked_ticks_ = 0;
        locked_.store(false);
    }

    // Time, in frames, from the video tick to the moment the first sample of this tick's audio
    // leaves the driver, if the driver has reported its output timing yet.
    //
    // This is measured, not inferred from the queue fill: the driver states when the frame at a
    // given ring index goes out; the frame this tick writes first sits a known number of frames
    // after it. Measuring the fill instead would alias against the driver's block size (2.7 ms
    // blocks against 20 ms ticks repeat in a slowly shifting pattern) and let the controller
    // chase a phantom error worth up to a block.
    bool measure_latency(std::int64_t tick_ns, double pending_in_resampler, double& latency_frames) const
    {
        output_port::output_time t{};
        if (!port_->read_output_time(t))
            return false;
        const auto rate        = static_cast<double>(format_desc_.audio_sample_rate);
        const auto first_index = static_cast<double>(port_->ring.write_index()) + pending_in_resampler -
                                 static_cast<double>(port_->drop_request.load());
        const auto out_ns      = static_cast<double>(t.steady_ns) +
                            (first_index - static_cast<double>(t.index)) * 1e9 / rate;
        latency_frames = (out_ns - static_cast<double>(tick_ns)) * rate / 1e9;
        return true;
    }

    void tick(const core::const_frame& frame, std::int64_t tick_ns)
    {
        if (!port_ || !stream_)
            return;

        const auto rate = format_desc_.audio_sample_rate;
        const auto fps  = format_desc_.fps > 0 ? format_desc_.fps : 25.0;

        // While the device is not playing, nothing drains the queue: hold it and start fresh once
        // the device runs again (including after a driver restart).
        if (!stream_->running()) {
            centred_ = false;
            locked_.store(false);
            return;
        }
        if (const auto gen = stream_->generation(); !centred_ || gen != seen_generation_) {
            if (centred_)
                resyncs_.fetch_add(1);
            seen_generation_ = gen;
            centred_         = true;
            have_latency_    = false;
            step_sum_        = 0.0;
            step_count_      = 0;
            // A first estimate: the queue covers the target less the driver's own latency. The
            // measured latency takes over as soon as the driver reports its timing.
            const auto driver  = stream_->output_latency_ms() * rate / 1000.0;
            const auto tl      = timeline_.load();
            const auto picture = tl->video_delay_updates.load() != 0 ? tl->video_delay_ns.load() * rate / 1e9
                                                                     : 4.0 * rate / fps;
            shift(std::max(0.0, picture + target_frames_ - driver) - queued_frames());
        }

        // Is the channel ticking at its frame rate? Late and bunched ticks say nothing about the
        // audio path, only about the channel's own timing.
        const auto frame_ns = 1e9 / fps;
        const bool regular  = last_tick_ns_ != 0 &&
                             std::abs(static_cast<double>(tick_ns - last_tick_ns_) - frame_ns) < tick_jitter_ms * 1e6;
        last_tick_ns_ = tick_ns;
        if (regular) {
            ++regular_ticks_;
        } else {
            regular_ticks_ = 0;
            align_window_.clear();
        }
        const bool settled = regular_ticks_ >= static_cast<int>(settle_s * fps);
        const bool aligned_settle = regular_ticks_ >= static_cast<int>(align_settle_s * fps);

        const auto pending_in = static_cast<double>(swr_get_delay(swr_.get(), rate));
        double     latency    = 0.0;
        double     error      = 0.0;
        bool       measured   = measure_latency(tick_ns, pending_in, latency);
        // Hold the audio against the picture, not against the handover: subtract the video
        // output's own handover-to-display delay, published by the card for every frame it
        // schedules. Any change in the card's buffering then moves both terms alike. Without a
        // publishing video output (an audio-only channel) the audio is held against the handover.
        {
            const auto  timeline_ptr = timeline_.load();
            const auto& timeline     = *timeline_ptr;
            const auto  updates  = timeline.video_delay_updates.load();
            if (updates != video_updates_seen_) {
                video_updates_seen_ = updates;
                video_stale_ticks_  = 0;
            } else if (video_stale_ticks_ < 1000) {
                ++video_stale_ticks_;
            }
            const bool video_live = video_updates_seen_ != 0 && video_stale_ticks_ < static_cast<int>(fps);
            if (measured && video_live)
                latency -= timeline.video_delay_ns.load() * rate / 1e9;
            if (video_live != relative_)
                measured = false; // the reference just changed; let the next tick start clean
            relative_ = video_live;
        }
        measured_out_.store(measured);
        regular_ticks_out_.store(regular_ticks_);
        aligned_out_.store(have_latency_);
        if (measured)
            raw_latency_ms_.store(latency * 1000.0 / rate);

        if (measured && (have_latency_ ? settled : aligned_settle)) {
            if (!have_latency_) {
                // First alignment after a start. The measurement keeps settling for a while after
                // the channel starts or a clip opens; align only on a window in which it is steady
                // (spread under align_spread_ms), and then on that window's mean, in one step.
                align_window_.push_back(latency);
                const auto want = static_cast<std::size_t>(align_window_s * fps);
                if (align_window_.size() > want)
                    align_window_.erase(align_window_.begin());
                const auto [lo, hi] = std::minmax_element(align_window_.begin(), align_window_.end());
                if (align_window_.size() == want && (*hi - *lo) < align_spread_ms * rate / 1000.0) {
                    double mean = 0.0;
                    for (auto v : align_window_)
                        mean += v;
                    mean /= static_cast<double>(align_window_.size());
                    align_window_.clear();
                    shift(target_frames_ - mean);
                    smoothed_latency_   = target_frames_;
                    have_latency_       = true;
                    underrun_baseline_  = port_->underrun_frames.load();
                    step_sum_           = 0.0;
                    step_count_         = 0;
                }
            } else if (std::abs(latency - smoothed_latency_) <= outlier_ms * rate / 1000.0) {
                outlier_s_ = 0.0;
                step_sum_  = 0.0;
                step_count_ = 0;
                smoothed_latency_ += (latency - smoothed_latency_) * alpha_;
                error = smoothed_latency_ - target_frames_;
                // Late (error > 0) needs fewer output samples: a negative correction.
                integral_ppm_ = std::clamp(integral_ppm_ + ki_ * error, -max_correction_ppm, max_correction_ppm);
            } else {
                // Off the held level. Usually a refilling video buffer; only a level that holds
                // for accept_level_s is real, and is then taken in one exact step.
                outlier_s_ += 1.0 / fps;
                if (outlier_s_ >= accept_level_s - step_window_s) {
                    step_sum_ += latency;
                    if (++step_count_ >= static_cast<int>(step_window_s * fps)) {
                        const auto level = step_sum_ / step_count_;
                        shift(target_frames_ - level);
                        resyncs_.fetch_add(1);
                        CASPAR_LOG(warning) << print() << L" latency held at " << level * 1000.0 / rate << L" ms for "
                                            << accept_level_s << L" s against " << target_frames_ * 1000.0 / rate
                                            << L" ms; moved back in one step";
                        smoothed_latency_ = target_frames_;
                        outlier_s_        = 0.0;
                        step_sum_         = 0.0;
                        step_count_       = 0;
                    }
                }
            }
        }
        const auto ppm = std::clamp(-(kp_ * error + integral_ppm_), -max_correction_ppm, max_correction_ppm);
        swr_set_compensation(swr_.get(),
                             static_cast<int>(std::lround(ppm * 1e-6 * compensation_distance)),
                             compensation_distance);

        // Select and convert the mapped channels. CasparCG mixes in 32-bit integers; float keeps
        // 24 significant bits, all a 24-bit Dante channel carries.
        const auto& audio    = frame.audio_data();
        const auto  src_ch   = format_desc_.audio_channels;
        const auto  map_size = channel_map_.size();
        const auto  samples  = src_ch > 0 ? static_cast<int>(audio.size()) / src_ch : 0;
        const float scale    = 1.0f / 2147483648.0f;
        if (mapped_.size() < static_cast<std::size_t>(samples) * map_size)
            mapped_.resize(static_cast<std::size_t>(samples) * map_size);
        for (int s = 0; s < samples; ++s) {
            for (std::size_t c = 0; c < map_size; ++c) {
                mapped_[s * map_size + c] =
                    static_cast<int>(c) < src_ch ? static_cast<float>(audio.data()[s * src_ch + c]) * scale : 0.0f;
            }
        }

        const auto out_cap  = static_cast<int>(resampled_.size() / map_size);
        auto       in_ptr   = reinterpret_cast<const uint8_t*>(mapped_.data());
        auto       out_ptr  = reinterpret_cast<uint8_t*>(resampled_.data());
        const auto produced = swr_convert(swr_.get(), &out_ptr, out_cap, &in_ptr, samples);
        if (produced > 0) {
            const auto written = port_->ring.push(resampled_.data(), static_cast<std::size_t>(produced));
            if (written < static_cast<std::size_t>(produced)) {
                overrun_frames_.fetch_add(static_cast<std::size_t>(produced) - written);
                graph_->set_tag(diagnostics::tag_severity::WARNING, "overrun");
            }
        }

        // Lock: the smoothed latency has held within tolerance for lock_seconds.
        if (have_latency_ && settled &&
            std::abs(smoothed_latency_ - target_frames_) < lock_tolerance_ms * rate / 1000.0) {
            if (++locked_ticks_ >= static_cast<int>(lock_seconds * fps))
                locked_.store(true);
        } else {
            locked_ticks_ = 0;
            locked_.store(false);
        }

        fill_ms_.store(queued_frames() * 1000.0 / rate);
        correction_ppm_.store(ppm);
        latency_ms_.store(smoothed_latency_ * 1000.0 / rate);
        underruns_.store(have_latency_ ? port_->underrun_frames.load() - underrun_baseline_ : 0);

        graph_->set_value("fill", std::clamp(0.5 + error / (2.0 * target_frames_), 0.0, 1.0));
        graph_->set_value("correction", 0.5 + ppm / (2.0 * max_correction_ppm));
        const auto underruns = port_->underrun_frames.load();
        if (underruns != last_underruns_) {
            last_underruns_ = underruns;
            if (have_latency_)
                graph_->set_tag(diagnostics::tag_severity::WARNING, "underrun");
        }
    }
};

namespace {

std::vector<int> parse_probe(const std::string& text)
{
    if (text.empty())
        return {};
    auto list = parse_channel_map(text, 0);
    if (list.size() != 2 || list[0] < 1 || list[1] < 1)
        CASPAR_THROW_EXCEPTION(invalid_argument()
                               << msg_info("proaudio: PROBE takes two 1-based device input channels, e.g. \"1,2\""));
    return list;
}

spl::shared_ptr<core::frame_consumer> make(consumer_settings s)
{
    for (auto ch : s.channel_map)
        if (ch < 1)
            CASPAR_THROW_EXCEPTION(invalid_argument()
                                   << msg_info("proaudio: CHANNEL_MAP entries are 1-based device channels, got " +
                                               std::to_string(ch)));
    return spl::make_shared<proaudio_consumer>(std::move(s));
}

} // namespace

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&     params,
                                                      const core::video_format_repository& format_repository,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                      const core::channel_info& channel_info)
{
    if (params.empty() || !boost::iequals(params.at(0), L"PROAUDIO"))
        return core::frame_consumer::empty();

    consumer_settings s;
    s.device_name        = u8(get_param(L"DEVICE_NAME", params, L""));
    s.host_api           = u8(get_param(L"HOST_API", params, L""));
    s.channel_map        = parse_channel_map(u8(get_param(L"CHANNEL_MAP", params, L"")), 0);
    s.delay_ms           = get_param(L"DELAY_MS", params, 0.0);
    s.buffer_size_frames = get_param(L"BUFFER_SIZE", params, 0);
    s.latency_ms         = get_param(L"LATENCY_MS", params, -1.0);
    s.probe              = parse_probe(u8(get_param(L"PROBE", params, L"")));
    return make(std::move(s));
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    consumer_settings s;
    s.device_name        = u8(ptree.get(L"device-name", L""));
    s.host_api           = u8(ptree.get(L"host-api", L""));
    s.channel_map        = parse_channel_map(u8(ptree.get(L"channel-map", L"")), 0);
    s.delay_ms           = ptree.get(L"delay-ms", 0.0);
    s.buffer_size_frames = ptree.get(L"buffer-size", 0);
    s.latency_ms         = ptree.get(L"latency-ms", -1.0);
    s.probe              = parse_probe(u8(ptree.get(L"probe", L"")));
    return make(std::move(s));
}

}} // namespace caspar::proaudio
