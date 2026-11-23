#include "audio_resampler.h"
#include "av_assert.h"

extern "C" {
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <stdexcept>

namespace caspar::ffmpeg {

AudioResampler::AudioResampler(int sample_rate, AVSampleFormat in_sample_fmt, int channels)
    : channels_(channels)
{
    if (channels_ <= 0)
        CASPAR_THROW_EXCEPTION(std::invalid_argument("AudioResampler requires a positive output channel count."));

    AVChannelLayout channel_layout     = AV_CHANNEL_LAYOUT_7POINT1;
    AVChannelLayout channel_layout_out{};
    av_channel_layout_default(&channel_layout_out, channels_);

    SwrContext* raw_ctx = nullptr;
    const auto  status  = swr_alloc_set_opts2(&raw_ctx,
                                            &channel_layout_out,
                                            AV_SAMPLE_FMT_S32,
                                            sample_rate,
                                            &channel_layout,
                                            in_sample_fmt,
                                            sample_rate,
                                            0,
                                            nullptr);
    av_channel_layout_uninit(&channel_layout_out);
    ctx = std::shared_ptr<SwrContext>(raw_ctx, [](SwrContext* ptr) { swr_free(&ptr); });

    FF(status);
    FF_RET(swr_init(ctx.get()), "swr_init");
}

caspar::array<int32_t> AudioResampler::convert(int frames, const void** src)
{
    auto result = caspar::array<int32_t>(static_cast<std::size_t>(frames) * channels_ * sizeof(int32_t));
    auto ptr    = result.data();
    swr_convert(ctx.get(), (uint8_t**)&ptr, frames, reinterpret_cast<const uint8_t**>(src), frames);

    return result;
}

}; // namespace caspar::ffmpeg
