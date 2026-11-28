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

#include "device_stream.h"

#include <common/except.h>
#include <common/log.h>
#include <common/utf.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace caspar { namespace proaudio {

using namespace std::chrono_literals;

namespace {

// A driver that has not called back for this long is considered stopped (an ASIO reset, the
// soundcard service restarting, a network interface going away) and the stream is reopened.
constexpr auto stall_timeout = 1000ms;
constexpr auto retry_interval = 2000ms;
constexpr auto watchdog_tick  = 250ms;
// A missing device is looked for again by re-reading PortAudio's device list (which loads every
// ASIO driver), so that is done less often than the open retry; repeated failures are logged
// only this often.
constexpr auto rescan_interval      = 10s;
constexpr auto failure_log_interval = 60s;

std::string settings_key(const stream_settings& s) { return clean_name(s.device_name) + "|" + clean_name(s.host_api); }

} // namespace

std::shared_ptr<device_stream> device_stream::acquire(const stream_settings& settings)
{
    std::lock_guard guard{registry_mutex_};

    const auto key = settings_key(settings);
    registry_.erase(std::remove_if(registry_.begin(), registry_.end(), [](auto& w) { return w.expired(); }),
                    registry_.end());

    for (auto& weak : registry_) {
        if (auto shared = weak.lock(); shared && settings_key(shared->settings_) == key) {
            const auto& s = shared->settings_;
            if (s.sample_rate != settings.sample_rate)
                CASPAR_THROW_EXCEPTION(invalid_operation()
                                       << msg_info("proaudio: device '" + settings.device_name + "' is already open at " +
                                                   std::to_string(s.sample_rate) + " Hz; every channel sharing it must "
                                                   "run at the same audio sample rate"));
            if (s.buffer_size_frames != settings.buffer_size_frames || s.latency_ms != settings.latency_ms)
                CASPAR_LOG(warning) << L"proaudio: device '" << u16(settings.device_name)
                                    << L"' is shared; its buffer size and latency are those of the first consumer";
            return shared;
        }
    }

    auto created = std::shared_ptr<device_stream>(new device_stream(settings));
    registry_.push_back(created);
    return created;
}

device_stream::device_stream(const stream_settings& settings)
    : settings_(settings)
    , library_(pa_library::acquire())
{
    // A network soundcard's ASIO driver (Dante Virtual Soundcard) only works once its service is
    // up, which at boot can be well after CasparCG has started. A device that is not there yet is
    // therefore waited for, not refused: the watchdog keeps looking, and the consumers start
    // playing as soon as it appears. A misspelt name shows as a device that never appears, with
    // the available devices in the log and proaudio/running false.
    bool found = false;
    {
        std::lock_guard pa{pa_mutex_};
        try {
            auto device = find_device(device_direction::output, settings_.device_name, settings_.host_api);
            if (auto info = Pa_GetDeviceInfo(device)) {
                auto api         = Pa_GetHostApiInfo(info->hostApi);
                device_label_    = std::string(info->name) + " (" + (api ? api->name : "?") + ")";
                output_channels_ = info->maxOutputChannels;
                claimed_.assign(static_cast<std::size_t>(output_channels_), false);
                found = true;
            }
        } catch (...) {
            found = false;
        }
    }

    if (!found) {
        device_label_ = settings_.device_name + " (" + settings_.host_api + ")";
        claimed_.assign(static_cast<std::size_t>(max_pending_channels), false);
        last_failure_log_ = std::chrono::steady_clock::now();
        CASPAR_LOG(warning) << L"proaudio: " << u16(device_label_)
                            << L" is not available yet; waiting for it and retrying every "
                            << retry_interval.count() << L" ms";
    }

    watchdog_thread_ = std::thread([this] { watchdog(); });
}

device_stream::~device_stream()
{
    {
        std::lock_guard guard{watchdog_mutex_};
        stopping_ = true;
    }
    watchdog_cv_.notify_all();
    if (watchdog_thread_.joinable())
        watchdog_thread_.join();

    std::lock_guard guard{control_mutex_};
    close();
}

void device_stream::attach(output_port* port)
{
    std::unique_lock lock{control_mutex_};

    // Until the device has appeared its size is unknown; the channels are then checked when it
    // opens.
    const auto limit = output_channels_ > 0 ? output_channels_ : max_pending_channels;
    for (auto ch : port->channels) {
        if (ch < 0 || ch >= limit)
            CASPAR_THROW_EXCEPTION(invalid_operation()
                                   << msg_info("proaudio: device channel " + std::to_string(ch + 1) +
                                               " does not exist on '" + device_label_ + "', which has " +
                                               std::to_string(limit) + " output channels"));
    }

    // A consumer that was just removed (REMOVE, then ADD with new settings) is destroyed
    // asynchronously, a little after the REMOVE has been acknowledged. Give it time to hand its
    // channels back before treating the overlap as a real conflict.
    auto taken = [&] {
        return std::any_of(port->channels.begin(), port->channels.end(), [&](int ch) {
            return claimed_[static_cast<std::size_t>(ch)];
        });
    };
    if (!released_.wait_for(lock, 2s, [&] { return !taken(); })) {
        for (auto ch : port->channels)
            if (claimed_[static_cast<std::size_t>(ch)])
                CASPAR_THROW_EXCEPTION(invalid_operation()
                                       << msg_info("proaudio: device channel " + std::to_string(ch + 1) + " on '" +
                                                   device_label_ + "' is already used by another consumer"));
    }

    auto slot = std::find_if(ports_.begin(), ports_.end(), [](auto& p) { return p.load() == nullptr; });
    if (slot == ports_.end())
        CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("proaudio: too many consumers on one device"));

    for (auto ch : port->channels)
        claimed_[static_cast<std::size_t>(ch)] = true;
    slot->store(port, std::memory_order_release);

    if (!stream_)
        open(); // Failure is logged; the watchdog keeps retrying.
    lock.unlock();
}

void device_stream::detach(output_port* port)
{
    {
        std::lock_guard guard{control_mutex_};

        for (auto& slot : ports_) {
            if (slot.load() != port)
                continue;
            slot.store(nullptr, std::memory_order_release);
            // A callback that read the slot before it was cleared may still be rendering this
            // port. Two further callbacks guarantee it has finished.
            wait_for_callbacks(callbacks_.load() + 2);
            for (auto ch : port->channels)
                claimed_[static_cast<std::size_t>(ch)] = false;
            break;
        }
    }
    released_.notify_all();
}

void device_stream::attach(probe_tap* tap)
{
    std::lock_guard guard{control_mutex_};

    if (tap->channel_a >= input_channels_ || tap->channel_b >= input_channels_)
        CASPAR_LOG(warning) << L"proaudio: probe input channels " << tap->channel_a + 1 << L"," << tap->channel_b + 1
                            << L" are not (yet) available on " << u16(device_label_) << L", which has "
                            << input_channels_ << L" input channels open";

    auto slot = std::find_if(probes_.begin(), probes_.end(), [](auto& p) { return p.load() == nullptr; });
    if (slot == probes_.end())
        CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("proaudio: too many sync probes on one device"));
    slot->store(tap, std::memory_order_release);
}

void device_stream::detach(probe_tap* tap)
{
    std::lock_guard guard{control_mutex_};

    for (auto& slot : probes_) {
        if (slot.load() != tap)
            continue;
        slot.store(nullptr, std::memory_order_release);
        wait_for_callbacks(callbacks_.load() + 2);
        return;
    }
}

void device_stream::wait_for_callbacks(std::uint64_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (running_.load(std::memory_order_acquire) && callbacks_.load(std::memory_order_acquire) < count &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
}

bool device_stream::rescan_devices()
{
    if (open_streams_ > 0)
        return false;

    // PortAudio counts initialisations; the shared library holds exactly one, so this really
    // terminates and starts it again, enumerating the drivers afresh. Device indices change, but
    // every open looks its device up by name.
    Pa_Terminate();
    auto err = Pa_Initialize();
    if (err != paNoError) {
        CASPAR_LOG(error) << L"proaudio: re-initialising PortAudio failed: " << u16(Pa_GetErrorText(err));
        return false;
    }
    return true;
}

void device_stream::open()
{
    close();

    std::lock_guard pa{pa_mutex_};
    try {
        const auto    now      = std::chrono::steady_clock::now();
        const bool    log_list = now - last_failure_log_ >= failure_log_interval;
        PaDeviceIndex device   = paNoDevice;
        try {
            device = find_device(device_direction::output, settings_.device_name, settings_.host_api, false);
        } catch (...) {
            // Not in the list PortAudio made when it started: the driver may have become ready
            // since (or come back after its service restarted). Read the list again and retry.
            if (now - last_rescan_ < rescan_interval || !rescan_devices())
                throw;
            last_rescan_ = now;
            device = find_device(device_direction::output, settings_.device_name, settings_.host_api, log_list);
        }
        auto info = Pa_GetDeviceInfo(device);
        if (!info)
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("device disappeared"));

        if (output_channels_ == 0) {
            // First appearance of a device that was missing at start-up: the ports were accepted
            // without knowing its size, so check them now.
            for (auto& slot : ports_) {
                auto port = slot.load(std::memory_order_acquire);
                if (!port)
                    continue;
                for (auto ch : port->channels)
                    if (ch >= info->maxOutputChannels)
                        CASPAR_THROW_EXCEPTION(invalid_operation()
                                               << msg_info("device channel " + std::to_string(ch + 1) +
                                                           " does not exist; the device has " +
                                                           std::to_string(info->maxOutputChannels) +
                                                           " output channels"));
            }
            output_channels_ = info->maxOutputChannels;
            if (claimed_.size() < static_cast<std::size_t>(output_channels_))
                claimed_.resize(static_cast<std::size_t>(output_channels_), false);
            CASPAR_LOG(info) << L"proaudio: " << u16(device_label_) << L" is now available";
        }

        if (info->maxOutputChannels != output_channels_) {
            // The channel count is a property of the driver (DVS: its channel setting). Ports were
            // validated against the old count, so a change needs the consumers re-added.
            CASPAR_THROW_EXCEPTION(invalid_operation()
                                   << msg_info("output channel count changed from " + std::to_string(output_channels_) +
                                               " to " + std::to_string(info->maxOutputChannels) +
                                               "; remove and re-add the proaudio consumers"));
        }

        PaStreamParameters out{};
        out.device           = device;
        out.channelCount     = output_channels_;
        out.sampleFormat     = paFloat32;
        out.suggestedLatency = settings_.latency_ms >= 0.0 ? settings_.latency_ms / 1000.0 : info->defaultLowOutputLatency;

        // Inputs are opened with the outputs (ASIO is full duplex on one device) so a sync probe
        // can be attached at any time without reopening the stream.
        PaStreamParameters in{};
        const bool         with_input = info->maxInputChannels > 0;
        if (with_input) {
            in.device           = device;
            in.channelCount     = info->maxInputChannels;
            in.sampleFormat     = paFloat32;
            in.suggestedLatency = out.suggestedLatency;
        }

        auto err = Pa_OpenStream(&stream_,
                                 with_input ? &in : nullptr,
                                 &out,
                                 settings_.sample_rate,
                                 settings_.buffer_size_frames > 0 ? static_cast<unsigned long>(settings_.buffer_size_frames)
                                                                  : paFramesPerBufferUnspecified,
                                 // Samples arrive as 24-bit-exact floats and are passed through
                                 // untouched: no dither, no clipping stage.
                                 paClipOff | paDitherOff,
                                 &device_stream::callback,
                                 this);
        bool duplex = with_input && err == paNoError;
        if (err != paNoError && with_input) {
            // Some drivers refuse full duplex; output alone is what matters.
            stream_ = nullptr;
            CASPAR_LOG(warning) << L"proaudio: " << u16(device_label_) << L" refused full duplex ("
                                << u16(Pa_GetErrorText(err)) << L"); opening output only, sync probe unavailable";
            err = Pa_OpenStream(&stream_,
                                nullptr,
                                &out,
                                settings_.sample_rate,
                                settings_.buffer_size_frames > 0
                                    ? static_cast<unsigned long>(settings_.buffer_size_frames)
                                    : paFramesPerBufferUnspecified,
                                paClipOff | paDitherOff,
                                &device_stream::callback,
                                this);
        }
        if (err != paNoError) {
            stream_ = nullptr;
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info(std::string("Pa_OpenStream: ") + Pa_GetErrorText(err)));
        }
        ++open_streams_;
        input_channels_ = duplex ? info->maxInputChannels : 0;

        err = Pa_StartStream(stream_);
        if (err != paNoError)
            CASPAR_THROW_EXCEPTION(invalid_operation()
                                   << msg_info(std::string("Pa_StartStream: ") + Pa_GetErrorText(err)));

        output_latency_ms_.store(Pa_GetStreamInfo(stream_)->outputLatency * 1000.0, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_acq_rel);

        CASPAR_LOG(info) << L"proaudio: opened " << u16(device_label_) << L", " << output_channels_ << L" out / "
                         << input_channels_ << L" in, " << settings_.sample_rate << L" Hz, driver output latency "
                         << output_latency_ms() << L" ms";
        // A later failure (the soundcard service restarting) is logged at once again.
        last_failure_log_ = std::chrono::steady_clock::time_point{};
    } catch (...) {
        // The first failure is logged in full; while the device stays away, a reminder now and then.
        const auto now = std::chrono::steady_clock::now();
        if (now - last_failure_log_ >= failure_log_interval) {
            last_failure_log_ = now;
            CASPAR_LOG_CURRENT_EXCEPTION();
            CASPAR_LOG(error) << L"proaudio: could not open " << u16(device_label_) << L"; retrying every "
                              << retry_interval.count() << L" ms";
        }
        close_locked();
    }
}

void device_stream::close()
{
    std::lock_guard pa{pa_mutex_};
    close_locked();
}

void device_stream::close_locked()
{
    running_.store(false, std::memory_order_release);
    if (!stream_)
        return;

    auto err = Pa_AbortStream(stream_);
    if (err != paNoError && err != paStreamIsStopped)
        CASPAR_LOG(warning) << L"proaudio: Pa_AbortStream: " << u16(Pa_GetErrorText(err));
    err = Pa_CloseStream(stream_);
    if (err != paNoError)
        CASPAR_LOG(warning) << L"proaudio: Pa_CloseStream: " << u16(Pa_GetErrorText(err));
    stream_         = nullptr;
    --open_streams_;
    input_channels_ = 0;
    output_latency_ms_.store(0.0, std::memory_order_relaxed);
}

void device_stream::watchdog()
{
    auto last_count    = callbacks_.load();
    auto last_progress = std::chrono::steady_clock::now();
    auto last_attempt  = std::chrono::steady_clock::time_point{};

    std::unique_lock lock{watchdog_mutex_};
    while (!watchdog_cv_.wait_for(lock, watchdog_tick, [this] { return stopping_; })) {
        lock.unlock();

        const auto now   = std::chrono::steady_clock::now();
        const auto count = callbacks_.load(std::memory_order_acquire);
        if (count != last_count) {
            last_count    = count;
            last_progress = now;
        }

        {
            std::lock_guard guard{control_mutex_};
            const bool      wanted = std::any_of(ports_.begin(), ports_.end(), [](auto& p) { return p.load() != nullptr; });

            if (wanted && stream_ && now - last_progress > stall_timeout) {
                CASPAR_LOG(error) << L"proaudio: " << u16(device_label_) << L" stopped calling back; reopening";
                close();
            }
            if (wanted && !stream_ && now - last_attempt > retry_interval) {
                last_attempt  = now;
                last_progress = now;
                open();
            }
        }

        lock.lock();
    }
}

int device_stream::callback(const void*                     input,
                            void*                           output,
                            unsigned long                   frame_count,
                            const PaStreamCallbackTimeInfo* time_info,
                            PaStreamCallbackFlags           status_flags,
                            void*                           user_data)
{
    // Realtime thread: no locks, no allocation, no logging.
    auto self = static_cast<device_stream*>(user_data);
    auto out  = static_cast<float*>(output);
    auto och  = static_cast<std::size_t>(self->output_channels_);

    // When this buffer's first frame leaves the driver, moved onto the steady clock that the
    // consumers stamp video ticks with. PortAudio gives the time on its own stream clock, so only
    // the difference to its "now" is used. A driver that reports no time falls back to "now plus
    // the stream's output latency".
    const auto steady_now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
    double ahead_s = self->output_latency_ms_.load(std::memory_order_relaxed) / 1000.0;
    if (time_info && time_info->outputBufferDacTime > 0.0 && time_info->currentTime > 0.0) {
        auto reported = time_info->outputBufferDacTime - time_info->currentTime;
        if (reported >= 0.0 && reported < 1.0)
            ahead_s = reported;
    }
    const auto first_frame_ns = steady_now + static_cast<std::int64_t>(ahead_s * 1e9);

    if (status_flags & paOutputUnderflow)
        self->device_underflows_.fetch_add(1, std::memory_order_relaxed);

    std::memset(out, 0, frame_count * och * sizeof(float));

    for (auto& slot : self->ports_) {
        auto port = slot.load(std::memory_order_acquire);
        if (!port)
            continue;

        if (auto drop = port->drop_request.exchange(0, std::memory_order_acq_rel))
            port->ring.discard(drop);

        port->publish({port->ring.read_index(), first_frame_ns});

        const auto pch = port->channels.size();
        for (unsigned long offset = 0; offset < frame_count;) {
            const auto n   = std::min<std::size_t>(frame_count - offset, output_port::max_block_frames);
            const auto got = port->ring.pop(port->scratch.data(), n);
            if (got < n)
                port->underrun_frames.fetch_add(n - got, std::memory_order_relaxed);

            for (std::size_t i = 0; i < n; ++i) {
                auto dst = out + (offset + i) * och;
                auto src = port->scratch.data() + i * pch;
                for (std::size_t c = 0; c < pch; ++c)
                    dst[port->channels[c]] = src[c];
            }
            offset += static_cast<unsigned long>(n);
        }
    }

    if (input) {
        auto in  = static_cast<const float*>(input);
        auto ich = static_cast<std::size_t>(self->input_channels_);
        for (auto& slot : self->probes_) {
            auto tap = slot.load(std::memory_order_acquire);
            if (!tap || tap->channel_a >= self->input_channels_ || tap->channel_b >= self->input_channels_)
                continue;

            float pair[2 * 256];
            for (unsigned long offset = 0; offset < frame_count;) {
                const auto n = std::min<unsigned long>(frame_count - offset, 256);
                for (unsigned long i = 0; i < n; ++i) {
                    pair[2 * i]     = in[(offset + i) * ich + static_cast<std::size_t>(tap->channel_a)];
                    pair[2 * i + 1] = in[(offset + i) * ich + static_cast<std::size_t>(tap->channel_b)];
                }
                tap->ring.push(pair, n); // A full ring drops the newest audio; the probe only samples.
                offset += n;
            }
        }
    }

    self->callbacks_.fetch_add(1, std::memory_order_release);
    return paContinue;
}

}} // namespace caspar::proaudio
