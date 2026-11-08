#include "audio_matrix.h"

#include <common/diagnostics/graph.h>
#include <common/except.h>
#include <core/frame/draw_frame.h>
#include <core/producer/route/route_audio_mapping.h>

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>

namespace caspar { namespace core {

namespace {

std::size_t tap_capacity(const video_format_desc& format, int maximum_read_samples, int required_source_samples)
{
    if (format.audio_channels <= 0 || format.audio_sample_rate <= 0 || format.audio_cadence.empty()) {
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid AUDIO_MATRIX source audio format."));
    }
    const auto maximum = *std::max_element(format.audio_cadence.begin(), format.audio_cadence.end());
    if (maximum <= 0) {
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid AUDIO_MATRIX source audio cadence."));
    }
    const auto largest_block = std::max({maximum, maximum_read_samples, required_source_samples});
    return static_cast<std::size_t>(largest_block) * 2 + static_cast<std::size_t>(maximum) - 1;
}

} // namespace

audio_tap::transport::transport(const video_format_desc& format, int maximum_read_samples, int required_source_samples)
    : buffer(tap_capacity(format, maximum_read_samples, required_source_samples),
             static_cast<std::size_t>(format.audio_channels))
    , capacity_samples(std::max({*std::max_element(format.audio_cadence.begin(), format.audio_cadence.end()),
                                 maximum_read_samples,
                                 required_source_samples}))
{
}

std::vector<audio_matrix_source> parse_audio_matrix_sources(const std::wstring& value)
{
    std::vector<std::wstring> entries;
    boost::split(entries, value, boost::is_any_of(L","));
    if (entries.empty() || entries.size() > 64) {
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"AUDIO_MATRIX accepts 1..64 inputs per output."));
    }

    std::vector<audio_matrix_source> result;
    for (auto& entry : entries) {
        boost::trim(entry);
        std::vector<std::wstring> fields;
        boost::split(fields, entry, boost::is_any_of(L":"));
        if (fields.size() != 5) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Use channel:layer:audio:PRE|POST:gain."));
        }
        try {
            audio_matrix_source source{};
            source.channel       = parse_route_audio_map_index(fields[0]);
            source.layer         = fields[1] == L"*" ? -1 : parse_route_audio_map_index(fields[1]);
            source.audio_channel = parse_route_audio_map_index(fields[2]);
            if (!boost::iequals(fields[3], L"PRE") && !boost::iequals(fields[3], L"POST")) {
                throw std::invalid_argument("tap must be PRE or POST");
            }
            source.post          = boost::iequals(fields[3], L"POST");
            std::size_t consumed = 0;
            source.gain          = std::stod(fields[4], &consumed);
            if (consumed != fields[4].size() || !std::isfinite(source.gain) || std::abs(source.gain) > 16) {
                throw std::invalid_argument("gain must be finite and within -16..16");
            }
            result.push_back(source);
        } catch (const std::invalid_argument&) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid AUDIO_MATRIX input or gain."));
        } catch (const std::out_of_range&) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"AUDIO_MATRIX gain is out of range."));
        }
    }
    return result;
}

audio_tap::audio_tap(int                                         channel,
                     int                                         layer,
                     bool                                        post,
                     const video_format_desc&                    format,
                     spl::shared_ptr<caspar::diagnostics::graph> graph,
                     int                                         maximum_read_samples)
    : channel(channel)
    , layer(layer)
    , post(post)
    , format(format)
    , maximum_read_samples(maximum_read_samples)
    , graph_(std::move(graph))
    , mixer_(spl::make_shared<diagnostics::graph>(), false)
    , buffer_(std::make_shared<transport>(format, maximum_read_samples))
    , source_cadence_(format.audio_cadence)
{
}

void audio_tap::publish(const draw_frame& frame, const video_format_desc& current_format, int nb_samples)
{
    const bool valid = format.audio_channels == current_format.audio_channels &&
                       format.audio_sample_rate == current_format.audio_sample_rate;
    if (!valid) {
        if (format_valid_.exchange(false, std::memory_order_acq_rel)) {
            mixer_.reset();
        }
        discontinuity_.store(true, std::memory_order_release);
        graph_->set_tag(diagnostics::tag_severity::WARNING, "audio-matrix-format");
        return;
    }
    format_valid_.store(true, std::memory_order_release);

    const auto cadence_changed = source_cadence_ != current_format.audio_cadence;
    const auto current_maximum =
        current_format.audio_cadence.empty()
            ? 0
            : *std::max_element(current_format.audio_cadence.begin(), current_format.audio_cadence.end());
    auto buffer = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
    if (cadence_changed || nb_samples > buffer->capacity_samples || current_maximum > buffer->capacity_samples) {
        if (current_maximum <= 0) {
            discontinuity_.store(true, std::memory_order_release);
            format_valid_.store(false, std::memory_order_release);
            graph_->set_tag(diagnostics::tag_severity::WARNING, "audio-matrix-format");
            mixer_.reset();
            return;
        }
        source_cadence_  = current_format.audio_cadence;
        auto replacement = std::make_shared<transport>(current_format, maximum_read_samples, nb_samples);
        discontinuity_.store(true, std::memory_order_release);
        mixer_.reset();
        std::atomic_store_explicit(&buffer_, std::move(replacement), std::memory_order_release);
        buffer = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
    }

    const auto audio_frame = !post && layer >= 0 ? draw_frame::pop(frame) : frame;
    audio_frame.accept(mixer_);
    const auto audio = mixer_(current_format, nb_samples);
    if (buffer->buffer.push(audio.data(), static_cast<std::size_t>(nb_samples)) !=
        static_cast<std::size_t>(nb_samples)) {
        discontinuity_.store(true, std::memory_order_release);
        graph_->set_tag(diagnostics::tag_severity::WARNING, "audio-matrix-overflow");
    }
}

const std::vector<int32_t>& audio_tap::read(int nb_samples)
{
    scratch_.resize(static_cast<std::size_t>(nb_samples) * static_cast<std::size_t>(format.audio_channels));
    if (discontinuity_.exchange(false, std::memory_order_acq_rel) || !format_valid()) {
        std::atomic_load_explicit(&buffer_, std::memory_order_acquire)->buffer.discard();
        std::fill(scratch_.begin(), scratch_.end(), 0);
        return scratch_;
    }
    const auto buffer = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
    const auto read   = buffer->buffer.pop(scratch_.data(), static_cast<std::size_t>(nb_samples));
    if (!format_valid() || discontinuity_.exchange(false, std::memory_order_acq_rel)) {
        std::atomic_load_explicit(&buffer_, std::memory_order_acquire)->buffer.discard();
        std::fill(scratch_.begin(), scratch_.end(), 0);
        return scratch_;
    }
    if (read != static_cast<std::size_t>(nb_samples)) {
        graph_->set_tag(diagnostics::tag_severity::WARNING, "audio-matrix-underflow");
    }
    return scratch_;
}

std::wstring audio_matrix_config::text() const
{
    std::wstringstream result;
    result << std::setprecision(std::numeric_limits<double>::max_digits10);
    bool first_output = true;
    for (std::size_t destination = 0; destination < outputs.size(); ++destination) {
        if (!outputs[destination]) {
            continue;
        }
        if (!first_output) {
            result << L";";
        }
        first_output = false;
        result << destination << L"=";
        if (outputs[destination]->empty()) {
            result << L"SILENCE";
        }
        bool first = true;
        for (const auto& input : *outputs[destination]) {
            if (!first) {
                result << L",";
            }
            first              = false;
            const auto& source = input.source;
            result << source.channel << L":";
            if (source.layer < 0) {
                result << L"*";
            } else {
                result << source.layer;
            }
            result << L":" << source.audio_channel << L":" << (source.post ? L"POST" : L"PRE") << L":" << source.gain;
        }
    }
    return first_output ? L"DEFAULT" : result.str();
}

array<const int32_t> mix_audio_matrix(const array<const int32_t>& legacy,
                                      const video_format_desc&    format,
                                      int                         nb_samples,
                                      const audio_matrix_config&  config,
                                      double                      master_volume)
{
    const auto expected = static_cast<std::size_t>(nb_samples) * static_cast<std::size_t>(format.audio_channels);
    if (legacy.size() != expected) {
        CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info(L"AUDIO_MATRIX received an invalid PCM block."));
    }
    std::map<audio_tap*, const std::vector<int32_t>*> buses;
    for (const auto& output : config.outputs) {
        if (output) {
            for (const auto& input : *output) {
                if (!buses.contains(input.tap.get())) {
                    buses.emplace(input.tap.get(), &input.tap->read(nb_samples));
                }
            }
        }
    }

    std::vector<int32_t> result(legacy.begin(), legacy.end());
    struct input_pcm
    {
        const int32_t* data;
        int            stride;
        double         gain;
    };
    std::vector<input_pcm> inputs;
    for (int destination = 0; destination < format.audio_channels; ++destination) {
        if (static_cast<std::size_t>(destination) >= config.outputs.size() || !config.outputs[destination]) {
            continue;
        }
        inputs.clear();
        for (const auto& input : *config.outputs[destination]) {
            if (input.tap->format.audio_sample_rate != format.audio_sample_rate || !input.tap->format_valid() ||
                input.source.audio_channel < 0 || input.source.audio_channel >= input.tap->format.audio_channels) {
                continue;
            }
            inputs.push_back({buses.at(input.tap.get())->data() + input.source.audio_channel,
                              input.tap->format.audio_channels,
                              input.source.gain});
        }
        for (int sample = 0; sample < nb_samples; ++sample) {
            double mixed = 0;
            for (const auto& input : inputs) {
                mixed += input.data[static_cast<std::size_t>(sample) * input.stride] * input.gain;
            }
            mixed *= master_volume;
            result[static_cast<std::size_t>(sample) * format.audio_channels + destination] =
                static_cast<int32_t>(std::clamp(mixed,
                                                static_cast<double>(std::numeric_limits<int32_t>::min()),
                                                static_cast<double>(std::numeric_limits<int32_t>::max())));
        }
    }
    return array<const int32_t>(std::move(result));
}

}} // namespace caspar::core
