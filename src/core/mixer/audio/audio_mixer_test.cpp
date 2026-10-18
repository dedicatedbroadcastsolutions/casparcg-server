#include "audio_mixer.h"

#include <common/diagnostics/graph.h>
#include <core/frame/frame.h>
#include <core/frame/frame_transform.h>
#include <core/frame/pixel_format.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {

using namespace caspar;
using namespace caspar::core;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct captured_graph : diagnostics::spi::graph_sink
{
    std::map<std::string, int> warnings;

    void activate() override {}
    void set_text(const std::wstring&) override {}
    void set_value(const std::string&, double) override {}
    void set_color(const std::string&, int) override {}
    void auto_reset() override {}
    void set_tag(diagnostics::tag_severity severity, const std::string& name) override
    {
        if (severity == diagnostics::tag_severity::WARNING)
            ++warnings[name];
    }
};

video_format_desc format(bool variable)
{
    return video_format_desc(video_format::x1080p2500,
                             1,
                             1920,
                             1080,
                             1920,
                             1080,
                             25000,
                             1000,
                             L"test",
                             variable ? std::vector<int>{4, 3} : std::vector<int>{4});
}

void submit(audio_mixer& mixer, const std::vector<int32_t>& samples, const void* tag, double volume = 1.0)
{
    frame_transform transform;
    transform.audio_transform.volume = volume;
    mixer.push(transform);
    mixer.visit(const_frame(tag, {}, array<const int32_t>(samples), pixel_format_desc{}, nullptr, {}));
    mixer.pop();
}

void check_buffers(const spl::shared_ptr<captured_graph>& sink)
{
    for (bool variable : {false, true}) {
        for (bool tagged : {false, true}) {
            const auto desc     = format(variable);
            const auto channels = desc.audio_channels;
            const int  tag      = 0;
            for (int size = 0; size <= 5 * channels + 1; ++size) {
                audio_mixer          mixer(spl::make_shared<diagnostics::graph>());
                std::vector<int32_t> input(size);
                for (int n = 0; n < size; ++n)
                    input[n] = 100 + n;

                const auto invalid_before = sink->warnings["audio-invalid-buffer"];
                const auto under_before   = sink->warnings["audio-buffer-underflow"];
                submit(mixer, input, tagged ? &tag : nullptr);
                const auto output   = mixer(desc, 4);
                const auto complete = size - size % channels;
                const auto prefix   = variable && tagged && complete ? channels : 0;

                require(output.size() == 4 * channels, "Mixer changed output length");
                for (size_t n = 0; n < output.size(); ++n) {
                    int32_t expected = 0;
                    if (complete && n >= prefix) {
                        const auto source = n - prefix;
                        expected = source < complete ? input[source] : input[complete - channels + n % channels];
                    }
                    require(output.data()[n] == expected, "Incomplete channel data leaked or hold behavior changed");
                }
                require(sink->warnings["audio-invalid-buffer"] - invalid_before == (size % channels != 0),
                        "Malformed buffer diagnostic missing or spurious");
                require(sink->warnings["audio-buffer-underflow"] - under_before ==
                            (complete > 0 && complete + prefix < output.size()),
                        "Underflow diagnostic missing or spurious");
            }
        }
    }
}

void check_cadence_and_recovery()
{
    const auto  desc = format(true);
    const auto  c    = desc.audio_channels;
    const int   tag  = 0;
    audio_mixer mixer(spl::make_shared<diagnostics::graph>());

    std::vector<int32_t> first(4 * c + 1, 100);
    first.back() = 999;
    submit(mixer, first, &tag);
    auto output = mixer(desc, 4);
    require(std::all_of(output.begin(), output.begin() + c, [](auto v) { return v == 0; }),
            "Cadence startup silence changed");
    require(std::all_of(output.begin() + c, output.end(), [](auto v) { return v == 100; }),
            "First complete audio changed");

    submit(mixer, std::vector<int32_t>(3 * c, 200), &tag);
    output = mixer(desc, 4);
    require(std::all_of(output.begin(), output.begin() + c, [](auto v) { return v == 100; }),
            "Cadence carry-over changed");
    require(std::all_of(output.begin() + c, output.end(), [](auto v) { return v == 200; }),
            "Incomplete sample contaminated cadence buffer");

    submit(mixer, {999}, &tag);
    output = mixer(desc, 4);
    require(std::all_of(output.begin(), output.end(), [](auto v) { return v == 0; }),
            "Malformed-only frame was not discarded");

    submit(mixer, std::vector<int32_t>(4 * c, 300), &tag);
    output = mixer(desc, 4);
    require(std::all_of(output.begin(), output.begin() + c, [](auto v) { return v == 0; }),
            "Discarded stream did not reset cadence startup");
    require(std::all_of(output.begin() + c, output.end(), [](auto v) { return v == 300; }),
            "Valid audio did not recover after malformed input");
}

void check_volume_ramp()
{
    const auto                 desc = format(false);
    const int                  tag  = 0;
    audio_mixer                mixer(spl::make_shared<diagnostics::graph>());
    const std::vector<int32_t> samples(4 * desc.audio_channels, 1200);
    submit(mixer, samples, &tag);
    mixer(desc, 4);
    submit(mixer, samples, &tag, 0.5);
    const auto output = mixer(desc, 4);
    for (size_t n = 0; n < output.size(); ++n)
        require(output.data()[n] == 1200 - 200 * (n / desc.audio_channels), "Volume ramp changed");
}

} // namespace

int main()
{
    try {
        const auto sink = spl::make_shared<captured_graph>();
        diagnostics::spi::register_sink_factory([sink] { return sink; });
        check_buffers(sink);
        check_cadence_and_recovery();
        check_volume_ramp();
        std::cout << "Audio mixer regressions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
