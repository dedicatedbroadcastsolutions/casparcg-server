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
 * Author: Robert Nagy, ronag89@gmail.com
 */

#include "ffmpeg_consumer.h"

#include "../util/av_assert.h"
#include "../util/av_util.h"

#include <common/bit_depth.h>
#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/memory.h>
#include <common/scope_exit.h>
#include <common/timer.h>

#include <core/consumer/channel_info.h>
#include <core/frame/frame.h>
#include <core/video_format.h>

#if defined(__GNUC__) && __GNUC__ == 14
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/regex.hpp>
#if defined(__GNUC__) && __GNUC__ == 14
#pragma GCC diagnostic pop
#endif

#include <boost/crc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <tbb/concurrent_queue.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace caspar { namespace ffmpeg {

// TODO multiple output streams
// TODO multiple output files
// TODO realtime with smaller buffer?

struct Stream
{
    std::shared_ptr<AVFilterGraph> graph  = nullptr;
    AVFilterContext*               sink   = nullptr;
    AVFilterContext*               source = nullptr;

    std::shared_ptr<AVCodecContext> enc = nullptr;
    AVStream*                       st  = nullptr;

    Stream(AVFormatContext*                    oc,
           std::string                         suffix,
           AVCodecID                           codec_id,
           const core::video_format_desc&      format_desc,
           bool                                realtime,
           common::bit_depth                   depth,
           std::map<std::string, std::string>& options)
    {
        std::map<std::string, std::string> stream_options;

        {
            auto tmp = std::move(options);
            for (auto& p : tmp) {
                if (boost::algorithm::ends_with(p.first, suffix)) {
                    const auto key = p.first.substr(0, p.first.size() - suffix.size());
                    stream_options.emplace(key, std::move(p.second));
                } else {
                    options.insert(std::move(p));
                }
            }
        }

        std::string filter_spec = "";
        {
            const auto it = stream_options.find("filter");
            if (it != stream_options.end()) {
                filter_spec = std::move(it->second);
                stream_options.erase(it);
            }
        }

        auto codec = avcodec_find_encoder(codec_id);
        {
            const auto it = stream_options.find("codec");
            if (it != stream_options.end()) {
                codec = avcodec_find_encoder_by_name(it->second.c_str());
                stream_options.erase(it);
            }
        }

        if (!codec) {
            FF_RET(AVERROR(EINVAL), "avcodec_find_encoder");
        }

        AVFilterInOut* outputs = nullptr;
        AVFilterInOut* inputs  = nullptr;

        CASPAR_SCOPE_EXIT
        {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        };

        graph = std::shared_ptr<AVFilterGraph>(avfilter_graph_alloc(),
                                               [](AVFilterGraph* ptr) { avfilter_graph_free(&ptr); });

        if (!graph) {
            FF_RET(AVERROR(ENOMEM), "avfilter_graph_alloc");
        }

        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            if (filter_spec.empty()) {
                filter_spec = "null";
            }
        } else {
            if (filter_spec.empty()) {
                filter_spec = "anull";
            }
        }

        FF(avfilter_graph_parse2(graph.get(), filter_spec.c_str(), &inputs, &outputs));

        {
            auto cur = inputs;

            if (!cur || cur->next) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter graph input count"));
            }

            if (codec->type == AVMEDIA_TYPE_VIDEO) {
                const auto sar = boost::rational<int>(format_desc.square_width, format_desc.square_height) /
                                 boost::rational<int>(format_desc.width, format_desc.height);

                const auto pix_fmt = (depth == common::bit_depth::bit8) ? AV_PIX_FMT_BGRA : AV_PIX_FMT_BGRA64LE;

                auto args = (boost::format("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:sar=%d/%d:frame_rate=%d/%d") %
                             format_desc.width % format_desc.height % pix_fmt % format_desc.duration %
                             (format_desc.time_scale * format_desc.field_count) % sar.numerator() % sar.denominator() %
                             (format_desc.framerate.numerator() * format_desc.field_count) %
                             format_desc.framerate.denominator())
                                .str();
                auto name = (boost::format("in_%d") % 0).str();

                FF(avfilter_graph_create_filter(
                    &source, avfilter_get_by_name("buffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
            } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
                auto args = (boost::format("time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%#x") % 1 %
                             format_desc.audio_sample_rate % format_desc.audio_sample_rate % AV_SAMPLE_FMT_S32 %
                             get_channel_layout_mask_for_channels(format_desc.audio_channels))
                                .str();
                auto name = (boost::format("in_%d") % 0).str();

                FF(avfilter_graph_create_filter(
                    &source, avfilter_get_by_name("abuffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
            } else {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter input media type"));
            }
        }

        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            // TODO codec->profiles
            // TODO codec->supported_framerates
            sink = create_buffersink(graph.get(), "out", get_supported_pixel_formats(nullptr, codec));
        } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
            // TODO codec->profiles
            sink = create_abuffersink(graph.get(),
                                      "out",
                                      get_supported_sample_formats(nullptr, codec),
                                      get_supported_sample_rates(nullptr, codec),
                                      get_supported_channel_layouts(nullptr, codec));
        } else {
            CASPAR_THROW_EXCEPTION(ffmpeg_error_t()
                                   << boost::errinfo_errno(EINVAL) << msg_info_t("invalid output media type"));
        }
        {
            const auto cur = outputs;

            if (!cur || cur->next) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter graph output count"));
            }

            if (avfilter_pad_get_type(cur->filter_ctx->output_pads, cur->pad_idx) != codec->type) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter output media type"));
            }

            FF(avfilter_link(cur->filter_ctx, cur->pad_idx, sink, 0));
        }

        FF(avfilter_graph_config(graph.get(), nullptr));

        st = avformat_new_stream(oc, nullptr);
        if (!st) {
            FF_RET(AVERROR(ENOMEM), "avformat_new_stream");
        }

        enc = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(codec),
                                              [](AVCodecContext* ptr) { avcodec_free_context(&ptr); });

        if (!enc) {
            FF_RET(AVERROR(ENOMEM), "avcodec_alloc_context3")
        }

        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            st->time_base = av_inv_q(av_buffersink_get_frame_rate(sink));

            // Ensure the frame_rate is set in a way that rtmp will find it
            st->avg_frame_rate = av_buffersink_get_frame_rate(sink);

            enc->width               = av_buffersink_get_w(sink);
            enc->height              = av_buffersink_get_h(sink);
            enc->framerate           = av_buffersink_get_frame_rate(sink);
            enc->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(sink);
            enc->time_base           = st->time_base;
            enc->pix_fmt             = static_cast<AVPixelFormat>(av_buffersink_get_format(sink));
        } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
            st->time_base = {1, av_buffersink_get_sample_rate(sink)};

            enc->sample_fmt  = static_cast<AVSampleFormat>(av_buffersink_get_format(sink));
            enc->sample_rate = av_buffersink_get_sample_rate(sink);
            enc->time_base   = st->time_base;

            FF(av_buffersink_get_ch_layout(sink, &enc->ch_layout));

        } else {
            // TODO
        }

        if (realtime && codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
            enc->thread_type = FF_THREAD_SLICE;
        }

        if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        auto dict = to_dict(std::move(stream_options));
        CASPAR_SCOPE_EXIT { av_dict_free(&dict); };
        FF(avcodec_open2(enc.get(), codec, &dict));
        for (auto& p : to_map(&dict)) {
            options[p.first] = p.second + suffix;
        }

        FF(avcodec_parameters_from_context(st->codecpar, enc.get()));

        if (codec->type == AVMEDIA_TYPE_AUDIO && !(codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) {
            av_buffersink_set_frame_size(sink, enc->frame_size);
        }
    }

    // Each Stream (video or audio) now only ever needs its own pts, since admission is no
    // longer coupled between the two (see ffmpeg_consumer below).
    void send(std::tuple<core::const_frame, std::int64_t>& data,
              const core::video_format_desc&               format_desc,
              std::function<void(std::shared_ptr<AVPacket>)> cb)
    {
        std::shared_ptr<AVFrame>  frame;
        std::shared_ptr<AVPacket> pkt;

        const auto [in_frame, pts] = data;

        if (in_frame) {
            if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
                frame      = make_av_video_frame(in_frame, format_desc);
                frame->pts = pts;
            } else if (enc->codec_type == AVMEDIA_TYPE_AUDIO) {
                frame      = make_av_audio_frame(in_frame, format_desc);
                frame->pts = pts;
            } else {
                // TODO
            }
            FF(av_buffersrc_write_frame(source, frame.get()));
        } else {
            FF(av_buffersrc_close(source, AV_NOPTS_VALUE, 0));
        }

        while (true) {
            pkt     = alloc_packet();
            int ret = avcodec_receive_packet(enc.get(), pkt.get());

            if (ret == AVERROR(EAGAIN)) {
                frame = alloc_frame();
                ret   = av_buffersink_get_frame(sink, frame.get());
                if (ret == AVERROR(EAGAIN)) {
                    return;
                }
                if (ret == AVERROR_EOF) {
                    FF(avcodec_send_frame(enc.get(), nullptr));
                } else {
                    FF_RET(ret, "av_buffersink_get_frame");
                    FF(avcodec_send_frame(enc.get(), frame.get()));
                }
            } else if (ret == AVERROR_EOF) {
                return;
            } else {
                FF_RET(ret, "avcodec_receive_packet");
                pkt->stream_index = st->index;
                av_packet_rescale_ts(pkt.get(), enc->time_base, st->time_base);
                cb(std::move(pkt));
            }
        }
    }
};

namespace {

// Parses "-<name> <value>" out of the raw ffmpeg-consumer argument string. Used for the handful
// of options that size queues before initialize()'s background thread has even started parsing
// the rest of the arguments (mirroring how realtime_ already picks a queue capacity eagerly in
// the ffmpeg_consumer constructor today).
int parse_consumer_int_option(const std::string& args, const std::string& name, int default_value, int min_value, int max_value)
{
    const boost::regex expr("-" + name + "\\s+(\\d+)");
    boost::smatch      match;
    if (boost::regex_search(args, match, expr)) {
        try {
            return std::clamp(std::stoi(match[1].str()), min_value, max_value);
        } catch (...) {
            // Malformed value; fall through to the default.
        }
    }
    return default_value;
}

// Tracks whether a producer (frame admission or packet write) is in the middle of a drop streak,
// so that a rate-limited, clearly-worded log line is only emitted once a stall is sustained for a
// meaningful amount of time, rather than for every isolated transient drop (which is already
// visible, cheaply, as a diagnostics graph tag).
struct drop_tracker
{
    bool                                   dropping = false;
    std::chrono::steady_clock::time_point  streak_start;
    std::chrono::steady_clock::time_point  last_log;
    std::int64_t                           streak_units = 0; // e.g. dropped audio samples, or packets

    template <typename Consumer>
    void note_drop(const Consumer& self, const wchar_t* kind, std::int64_t units = 0)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!dropping) {
            dropping     = true;
            streak_start = now;
            streak_units = 0;
        }
        streak_units += units;

        constexpr auto sustained_threshold = std::chrono::milliseconds(250);
        constexpr auto log_rate_limit      = std::chrono::seconds(10);

        if (now - streak_start > sustained_threshold &&
            (last_log.time_since_epoch().count() == 0 || now - last_log > log_rate_limit)) {
            last_log = now;
            CASPAR_LOG(warning)
                << self.print() << L" Sustained " << kind << L" overload for "
                << std::chrono::duration_cast<std::chrono::milliseconds>(now - streak_start).count()
                << L"ms (" << streak_units << L" units lost so far) - the consumer cannot keep up with "
                << kind << L", this is not a momentary hiccup.";
        }
    }

    void note_ok() { dropping = false; }
};

} // namespace

struct ffmpeg_consumer : public core::frame_consumer
{
    core::monitor::state    state_;
    mutable std::mutex      state_mutex_;
    int                     channel_index_ = -1;
    core::video_format_desc format_desc_;
    bool                    realtime_ = false;
    std::int64_t            video_pts = 0;
    std::int64_t            audio_pts = 0;

    spl::shared_ptr<diagnostics::graph> graph_;

    std::string path_;
    std::string args_;

    std::exception_ptr exception_;
    std::mutex         exception_mutex_;

    // Video and audio are admitted into independent bounded queues and processed on independent
    // threads. A single shared queue (the previous design) coupled the two: once it filled up -
    // e.g. because a video encode or a packet write briefly stalled - admitting a *video* frame
    // failed, which silently discarded that same tuple's *audio* samples too, while video_pts/
    // audio_pts still advanced to keep subsequent timestamps contiguous. That produced audible
    // dropouts with no trace in packet timing, sequence numbers or decoder error counts, because
    // only the *content* was lost, not the timestamp continuity. See send() below.
    tbb::concurrent_bounded_queue<std::tuple<core::const_frame, std::int64_t>> video_frame_buffer_;
    tbb::concurrent_bounded_queue<std::tuple<core::const_frame, std::int64_t>> audio_frame_buffer_;
    std::thread                                                                frame_thread_;

    int audio_queue_capacity_  = 0;
    int packet_queue_capacity_ = 0;

    drop_tracker video_frame_drops_;
    drop_tracker audio_frame_drops_;

    std::int64_t video_queue_high_water_ = 0;
    std::int64_t audio_queue_high_water_ = 0;

    common::bit_depth depth_;

  public:
    ffmpeg_consumer(std::string path, std::string args, bool realtime, common::bit_depth depth)
        : channel_index_([&] {
            boost::crc_16_type result;
            result.process_bytes(path.data(), path.length());
            return result.checksum();
        }())
        , realtime_(realtime)
        , path_(std::move(path))
        , args_(std::move(args))
        , depth_(depth)
    {
        state_["file/path"] = u8(path_);

        // Video is allowed to fall behind and drop frames under transient load, unchanged from
        // before. Audio gets its own, independently sized queue, so that no longer also discards
        // audio content. A deeper audio queue trades a bounded amount of extra worst-case
        // monitoring latency for audio continuity under a transient stall; -audio-buffer <frames>
        // lets an operator tune that trade-off (default: ~8 frames/fields in realtime mode, i.e.
        // roughly 130ms at 59.94Hz). -packet-buffer <packets> similarly sizes the encoded-packet
        // queue feeding the muxer/network writer. Neither is a substitute for fixing a sustained
        // overload, which is reported via drop_tracker rather than hidden behind an ever-growing
        // backlog.
        audio_queue_capacity_  = parse_consumer_int_option(args_, "audio-buffer", realtime_ ? 8 : 64, 1, 256);
        packet_queue_capacity_ = parse_consumer_int_option(args_, "packet-buffer", realtime_ ? 16 : 128, 1, 1024);

        video_frame_buffer_.set_capacity(realtime_ ? 1 : 64);
        audio_frame_buffer_.set_capacity(audio_queue_capacity_);

        diagnostics::register_graph(graph_);
        graph_->set_color("video-frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("audio-frame-time", diagnostics::color(0.1f, 0.8f, 1.0f));
        graph_->set_color("dropped-video-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        graph_->set_color("dropped-audio-frame", diagnostics::color(1.0f, 0.2f, 0.2f));
        graph_->set_color("dropped-video-packet", diagnostics::color(0.6f, 0.5f, 0.1f));
        graph_->set_color("dropped-audio-packet", diagnostics::color(1.0f, 0.5f, 0.0f));
        graph_->set_color("video-input", diagnostics::color(0.7f, 0.4f, 0.4f));
        graph_->set_color("audio-input", diagnostics::color(0.4f, 0.4f, 0.9f));
    }

    ~ffmpeg_consumer()
    {
        if (frame_thread_.joinable()) {
            video_frame_buffer_.push({core::const_frame{}, -1});
            audio_frame_buffer_.push({core::const_frame{}, -1});
            frame_thread_.join();
        }
    }

    // frame consumer

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        if (frame_thread_.joinable()) {
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Cannot reinitialize ffmpeg-consumer."));
        }

        format_desc_   = format_desc;
        channel_index_ = channel_info.index;

        graph_->set_text(print());

        frame_thread_ = std::thread([=, this] {
            try {
                std::map<std::string, std::string> options;
                {
                    static boost::regex opt_exp("-(?<NAME>[^\\s]+)(\\s+(?<VALUE>[^\\s]+))?");
                    for (auto it = boost::sregex_iterator(args_.begin(), args_.end(), opt_exp);
                         it != boost::sregex_iterator();
                         ++it) {
                        options[(*it)["NAME"].str().c_str()] =
                            (*it)["VALUE"].matched ? (*it)["VALUE"].str().c_str() : "";
                    }
                }

                // Already consumed eagerly in the constructor (see audio_queue_capacity_ /
                // packet_queue_capacity_); strip them here so they are neither forwarded to
                // avcodec/avformat nor reported as unused below.
                options.erase("audio-buffer");
                options.erase("packet-buffer");

                boost::filesystem::path full_path = path_;

                static boost::regex prot_exp("^.+:.*");
                if (!boost::regex_match(path_, prot_exp)) {
                    if (!full_path.is_absolute()) {
                        full_path = u8(env::media_folder()) + path_;
                    }

                    // TODO -y?
                    if (boost::filesystem::exists(full_path)) {
                        boost::filesystem::remove(full_path);
                    }

                    boost::filesystem::create_directories(full_path.parent_path());
                }

                AVFormatContext* oc = nullptr;

                {
                    std::string format;
                    {
                        const auto format_it = options.find("format");
                        if (format_it != options.end()) {
                            format = std::move(format_it->second);
                            options.erase(format_it);
                        }
                    }

                    FF(avformat_alloc_output_context2(
                        &oc, nullptr, !format.empty() ? format.c_str() : nullptr, path_.c_str()));
                }

                CASPAR_SCOPE_EXIT { avformat_free_context(oc); };

                std::optional<Stream> video_stream;
                if (oc->oformat->video_codec != AV_CODEC_ID_NONE) {
                    if (oc->oformat->video_codec == AV_CODEC_ID_H264 && options.find("preset:v") == options.end()) {
                        options["preset:v"] = "veryfast";
                    }
                    video_stream.emplace(oc, ":v", oc->oformat->video_codec, format_desc, realtime_, depth_, options);

                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state_["file/fps"] = av_q2d(av_buffersink_get_frame_rate(video_stream->sink));
                    }
                }

                std::optional<Stream> audio_stream;
                if (oc->oformat->audio_codec != AV_CODEC_ID_NONE) {
                    audio_stream.emplace(oc, ":a", oc->oformat->audio_codec, format_desc, realtime_, depth_, options);
                }

                if (!(oc->oformat->flags & AVFMT_NOFILE)) {
                    // TODO (fix) interrupt_cb
                    auto dict = to_dict(std::move(options));
                    CASPAR_SCOPE_EXIT { av_dict_free(&dict); };
                    FF(avio_open2(&oc->pb, full_path.string().c_str(), AVIO_FLAG_WRITE, nullptr, &dict));
                    options = to_map(&dict);
                }

                {
                    auto dict = to_dict(std::move(options));
                    CASPAR_SCOPE_EXIT { av_dict_free(&dict); };
                    FF(avformat_write_header(oc, &dict));
                    options = to_map(&dict);
                }

                {
                    for (auto& p : options) {
                        CASPAR_LOG(warning) << print() << " Unused option " << p.first << "=" << p.second;
                    }
                }

                tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>> packet_buffer;
                packet_buffer.set_capacity(packet_queue_capacity_);
                auto packet_thread = std::thread([&] {
                    try {
                        CASPAR_SCOPE_EXIT
                        {
                            if (!(oc->oformat->flags & AVFMT_NOFILE)) {
                                FF(avio_closep(&oc->pb));
                            }
                        };

                        std::map<int, int64_t> count;

                        std::shared_ptr<AVPacket> pkt;
                        while (true) {
                            packet_buffer.pop(pkt);
                            if (!pkt) {
                                break;
                            }
                            count[pkt->stream_index] += 1;
                            FF(av_interleaved_write_frame(oc, pkt.get()));
                        }

                        auto video_st = video_stream ? video_stream->st : nullptr;
                        auto audio_st = audio_stream ? audio_stream->st : nullptr;

                        if ((!video_st || count[video_st->index]) && (!audio_st || count[audio_st->index])) {
                            FF(av_write_trailer(oc));
                        }

                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                        // TODO
                        packet_buffer.abort();
                    }
                });
                CASPAR_SCOPE_EXIT
                {
                    if (packet_thread.joinable()) {
                        // TODO Is nullptr needed?
                        packet_buffer.push(nullptr);
                        packet_buffer.abort();
                        packet_thread.join();
                    }
                };

                // Writing into the muxer never blocks the encode threads below: a stalled or
                // momentarily backed-up write (e.g. transient network backpressure on the UDP
                // socket) used to propagate straight back through a blocking push() here, into
                // the single combined per-frame queue below it, and from there into dropped
                // *admission* of new frames (including audio - see ffmpeg_consumer above). With
                // try_push + per-media diagnostics, a muxer stall can now only ever drop that
                // media type's own already-encoded packets.
                drop_tracker video_packet_drops;
                drop_tracker audio_packet_drops;
                auto         video_packet_cb = [&](std::shared_ptr<AVPacket>&& pkt) {
                    if (packet_buffer.try_push(pkt)) {
                        video_packet_drops.note_ok();
                    } else {
                        graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-video-packet");
                        video_packet_drops.note_drop(*this, L"video packet-write", 1);
                    }
                };
                auto audio_packet_cb = [&](std::shared_ptr<AVPacket>&& pkt) {
                    if (packet_buffer.try_push(pkt)) {
                        audio_packet_drops.note_ok();
                    } else {
                        graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-audio-packet");
                        audio_packet_drops.note_drop(*this, L"audio packet-write", 1);
                    }
                };

                // Video and audio always run their own loop on their own thread (even if one of
                // them turns out not to exist for this output format - that thread then just
                // drains and discards). Exactly one nullptr must reach packet_thread once *both*
                // have finished, regardless of which one finishes first.
                std::atomic<int> active_producers{2};
                auto             finish_producer = [&] {
                    if (--active_producers == 0) {
                        packet_buffer.push(nullptr);
                    }
                };

                std::thread video_proc_thread([&] {
                    try {
                        std::int64_t frame_number = 0;
                        while (true) {
                            std::tuple<core::const_frame, std::int64_t> data;
                            video_frame_buffer_.pop(data);
                            {
                                std::lock_guard<std::mutex> lock(state_mutex_);
                                state_["file/video-frame"] = frame_number++;
                            }
                            graph_->set_value("video-input",
                                              static_cast<double>(video_frame_buffer_.size() + 0.001) /
                                                  video_frame_buffer_.capacity());

                            if (video_stream) {
                                caspar::timer frame_timer;
                                video_stream->send(data, format_desc, video_packet_cb);
                                graph_->set_value("video-frame-time", frame_timer.elapsed() * format_desc.fps);
                            }

                            if (!std::get<0>(data)) {
                                break;
                            }
                        }
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(exception_mutex_);
                        if (!exception_) {
                            exception_ = std::current_exception();
                        }
                    }
                    finish_producer();
                });
                CASPAR_SCOPE_EXIT
                {
                    // Guards against std::terminate() if anything between here and the
                    // video_proc_thread.join() below throws (e.g. audio_proc_thread's own
                    // construction failing under resource exhaustion) while this thread is still
                    // joinable, mirroring the existing packet_thread guard above.
                    if (video_proc_thread.joinable()) {
                        video_frame_buffer_.push({core::const_frame{}, -1});
                        video_proc_thread.join();
                    }
                };

                // Detects any remaining gap between consecutive admitted audio chunks, regardless
                // of cause - a cheap, independent cross-check that keeps working even if some
                // future change reintroduces a discontinuity by a different path than the one
                // fixed here.
                bool         have_expected_audio_pts = false;
                std::int64_t expected_audio_pts      = 0;

                std::thread audio_proc_thread([&] {
                    try {
                        std::int64_t frame_number = 0;
                        while (true) {
                            std::tuple<core::const_frame, std::int64_t> data;
                            audio_frame_buffer_.pop(data);
                            {
                                std::lock_guard<std::mutex> lock(state_mutex_);
                                state_["file/audio-frame"] = frame_number++;
                            }
                            graph_->set_value("audio-input",
                                              static_cast<double>(audio_frame_buffer_.size() + 0.001) /
                                                  audio_frame_buffer_.capacity());

                            const auto& in_frame = std::get<0>(data);
                            if (in_frame) {
                                const auto pts = std::get<1>(data);
                                if (have_expected_audio_pts && pts != expected_audio_pts) {
                                    CASPAR_LOG(warning) << print() << L" Audio timestamp discontinuity: expected pts "
                                                        << expected_audio_pts << L", got " << pts << L" (gap of "
                                                        << (pts - expected_audio_pts) << L" samples).";
                                }
                                have_expected_audio_pts = true;
                                expected_audio_pts =
                                    pts + static_cast<std::int64_t>(in_frame.audio_data().size()) /
                                              std::max<std::size_t>(1, format_desc.audio_channels);
                            }

                            if (audio_stream) {
                                caspar::timer frame_timer;
                                audio_stream->send(data, format_desc, audio_packet_cb);
                                graph_->set_value("audio-frame-time", frame_timer.elapsed() * format_desc.fps);
                            }

                            if (!in_frame) {
                                break;
                            }
                        }
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(exception_mutex_);
                        if (!exception_) {
                            exception_ = std::current_exception();
                        }
                    }
                    finish_producer();
                });
                CASPAR_SCOPE_EXIT
                {
                    if (audio_proc_thread.joinable()) {
                        audio_frame_buffer_.push({core::const_frame{}, -1});
                        audio_proc_thread.join();
                    }
                };

                video_proc_thread.join();
                audio_proc_thread.join();

                packet_thread.join();
            } catch (...) {
                std::lock_guard<std::mutex> lock(exception_mutex_);
                exception_ = std::current_exception();
            }
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        // TODO - field alignment

        {
            std::lock_guard<std::mutex> lock(exception_mutex_);
            if (exception_ != nullptr) {
                std::rethrow_exception(exception_);
            }
        }

        // Video and audio are admitted independently so that a transient stall on one side (e.g.
        // a video encode briefly falling behind under system contention) can never also discard
        // audio content for the same frame - see the comment on video_frame_buffer_ above. Both
        // queues only ever drop, never block: send() runs on the shared channel thread and must
        // never stall channel/SDI playout regardless of how this consumer is keeping up.
        if (!video_frame_buffer_.try_push({frame, video_pts})) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-video-frame");
            video_frame_drops_.note_drop(*this, L"video frame-admission", 1);
        } else {
            video_frame_drops_.note_ok();
        }

        if (!audio_frame_buffer_.try_push({frame, audio_pts})) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-audio-frame");
            audio_frame_drops_.note_drop(
                *this, L"audio frame-admission", frame.audio_data().size() / format_desc_.audio_channels);
        } else {
            audio_frame_drops_.note_ok();
        }

        video_pts += 1;
        audio_pts += frame.audio_data().size() / format_desc_.audio_channels;

        const auto video_size = video_frame_buffer_.size();
        const auto audio_size = audio_frame_buffer_.size();

        graph_->set_value("video-input", static_cast<double>(video_size + 0.001) / video_frame_buffer_.capacity());
        graph_->set_value("audio-input", static_cast<double>(audio_size + 0.001) / audio_frame_buffer_.capacity());

        if (video_size > video_queue_high_water_ || audio_size > audio_queue_high_water_) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (video_size > video_queue_high_water_) {
                video_queue_high_water_            = video_size;
                state_["file/video-queue-high-water"] = video_queue_high_water_;
            }
            if (audio_size > audio_queue_high_water_) {
                audio_queue_high_water_            = audio_size;
                state_["file/audio-queue-high-water"] = audio_queue_high_water_;
            }
        }

        return make_ready_future(true);
    }

    std::wstring print() const override { return L"ffmpeg[" + u16(path_) + L"]"; }

    std::wstring name() const override { return L"ffmpeg"; }

    bool has_synchronization_clock() const override { return false; }

    int index() const override { return 100000 + channel_index_; }

    core::monitor::state state() const override
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&     params,
                                                      const core::video_format_repository& format_repository,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                      const core::channel_info& channel_info)
{
    if (params.size() < 2 || (!boost::iequals(params.at(0), L"STREAM") && !boost::iequals(params.at(0), L"FILE")))
        return core::frame_consumer::empty();

    auto                     path = u8(params.at(1));
    std::vector<std::string> args;
    for (auto n = 2; n < params.size(); ++n) {
        args.emplace_back(u8(params[n]));
    }
    return spl::make_shared<ffmpeg_consumer>(
        path, boost::join(args, " "), boost::iequals(params.at(0), L"STREAM"), channel_info.depth);
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    return spl::make_shared<ffmpeg_consumer>(u8(ptree.get<std::wstring>(L"path", L"")),
                                             u8(ptree.get<std::wstring>(L"args", L"")),
                                             ptree.get(L"realtime", false),
                                             channel_info.depth);
}
}} // namespace caspar::ffmpeg
