#include <common/diagnostics/graph.h>
#include <core/frame/frame.h>
#include <core/frame/pixel_format.h>
#include <core/mixer/audio/audio_mixer.h>

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void verify_channel_width(int source_channels, int output_channels)
{
    using namespace caspar;
    using namespace caspar::core;

    video_format_repository formats;
    auto                    format = formats.find(L"1080p5000");
    format.audio_channels          = output_channels;

    std::vector<int32_t> samples(static_cast<std::size_t>(source_channels));
    for (int channel = 0; channel < source_channels; ++channel)
        samples[static_cast<std::size_t>(channel)] = channel + 1;

    const_frame input(nullptr,
                      {},
                      array<const int32_t>(std::move(samples)),
                      pixel_format_desc(pixel_format::invalid),
                      nullptr,
                      source_channels);

    audio_mixer mixer(spl::make_shared<diagnostics::graph>(), false);
    mixer.visit(input);
    const auto output = mixer(format, 1, false);

    check(output.size() == static_cast<std::size_t>(output_channels),
          "mixer output size did not match configured channel width");
    for (int channel = 0; channel < output_channels; ++channel) {
        const auto expected = channel < source_channels ? channel + 1 : 0;
        check(output.data()[channel] == expected, "mixer did not correctly pad or truncate source PCM");
    }
}

} // namespace

int main()
{
    try {
        verify_channel_width(24, 24);
        verify_channel_width(16, 24);
        verify_channel_width(24, 8);
        std::cout << "Audio channel-width mixer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
