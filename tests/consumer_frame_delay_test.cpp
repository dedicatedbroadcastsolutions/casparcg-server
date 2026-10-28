#include <core/consumer/frame_delay.h>
#include <core/frame/pixel_format.h>
#include <modules/newtek/consumer/ndi_frame_buffer.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using caspar::core::const_frame;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const_frame make_frame(int value)
{
    caspar::core::pixel_format_desc desc(caspar::core::pixel_format::bgra);
    desc.planes.emplace_back(1, 1, 4);
    std::vector<caspar::array<const std::uint8_t>> image;
    image.emplace_back(std::vector<std::uint8_t>{static_cast<std::uint8_t>(value), 0, 0, 255});
    return const_frame(nullptr,
                       std::move(image),
                       caspar::array<const std::int32_t>(std::vector<std::int32_t>{value, -value}),
                       desc);
}

void check_sequence(const caspar::timespan& delay, double frame_rate, int fields, int delayed_frames)
{
    caspar::core::frame_delay buffer;
    buffer.reset(delay, frame_rate, fields);
    std::vector<const_frame> inputs;
    // Drain the consumer immediately after every push: delay must not depend on worker speed.
    for (int n = 0; n < 180; ++n) {
        inputs.push_back(make_frame(n));
        auto output = buffer.push(inputs.back());
        auto index  = n < delayed_frames * fields ? n % fields : n - delayed_frames * fields;
        require(static_cast<bool>(output), "Delay returned an empty frame");
        require(output == inputs.at(index), "Incorrect frame/field delay");
        require(output.image_data(0).data()[0] == index, "Video did not follow delayed frame");
        require(output.audio_data().data()[0] == index, "Audio did not follow delayed frame");
    }
}

void check_reset()
{
    caspar::core::frame_delay buffer;
    buffer.reset(caspar::timespan{5}, 25, 1);
    buffer.push(make_frame(1));
    buffer.push(make_frame(2));
    buffer.reset(caspar::timespan{2}, 25, 2);
    auto field_a = make_frame(10);
    auto field_b = make_frame(11);
    require(buffer.push(field_a) == field_a, "Reset retained old frames");
    require(buffer.push(field_b) == field_b, "Reset lost field alignment");
    require(buffer.push(make_frame(12)) == field_a, "Reset did not apply new delay");
    require(buffer.push(make_frame(13)) == field_b, "Reset did not preserve field parity");
    buffer.reset(caspar::timespan{}, 50, 1);
    auto frame = make_frame(20);
    require(buffer.push(frame) == frame, "Reset to zero delay retained frames");
}

void check_ndi_capacity()
{
    using caspar::core::video_field;
    caspar::newtek::ndi_frame_buffer buffer;
    buffer.reset(16, 1);
    for (int n = 0; n < 1000; ++n) {
        require(buffer.push(video_field::progressive, make_frame(n % 200)) == (n < 16),
                "NDI did not reject incoming frames at capacity");
        require(buffer.size() <= 16, "NDI send queue grew beyond capacity");
    }
    for (int n = 0; n < 16; ++n)
        require(buffer.pop().audio_data().data()[0] == n, "NDI did not retain queued frames in order");

    buffer.reset(1, 1);
    require(buffer.push(video_field::progressive, make_frame(1)), "Minimum buffer rejected first frame");
    require(buffer.wait_for_preroll(), "Minimum buffer could not complete preroll");
    require(!buffer.push(video_field::progressive, make_frame(2)), "Minimum buffer accepted excess frame");
    buffer.reset(0, 1);
    require(buffer.capacity() == 1, "Invalid capacity was not clamped");
    buffer.reset(1, 2);
    require(buffer.capacity() == 2, "Interlaced capacity did not allow a field pair");
    buffer.reset(7, 2);
    require(buffer.capacity() == 6, "Odd interlaced capacity was not rounded down");
}

void check_ndi_field_pairs()
{
    using caspar::core::video_field;
    caspar::newtek::ndi_frame_buffer buffer;
    buffer.reset(4, 2);
    require(buffer.push(video_field::a, make_frame(0)), "NDI rejected field A");
    require(buffer.push(video_field::b, make_frame(1)), "NDI rejected field B");
    require(buffer.push(video_field::a, make_frame(2)), "NDI rejected second field A");
    require(buffer.push(video_field::b, make_frame(3)), "NDI rejected second field B");
    require(buffer.pop().audio_data().data()[0] == 0, "Incorrect first field");
    require(!buffer.push(video_field::a, make_frame(4)), "NDI accepted field A without room for field B");
    require(buffer.pop().audio_data().data()[0] == 1, "Incorrect second field");
    require(!buffer.push(video_field::b, make_frame(5)), "NDI accepted field B after dropping field A");
    require(buffer.pop().audio_data().data()[0] == 2, "NDI lost field alignment");
    require(buffer.pop().audio_data().data()[0] == 3, "NDI lost second field alignment");
    require(buffer.push(video_field::a, make_frame(6)), "NDI did not recover after overload");
    require(buffer.push(video_field::b, make_frame(7)), "NDI did not recover field B after overload");
    require(buffer.pop().audio_data().data()[0] == 6, "NDI field A inverted after overload");
    require(buffer.pop().audio_data().data()[0] == 7, "NDI field B inverted after overload");
}

void check_ndi_delayed_overload()
{
    using caspar::core::video_field;
    caspar::core::frame_delay delay;
    caspar::newtek::ndi_frame_buffer buffer;
    delay.reset(caspar::timespan{2}, 25, 2);
    buffer.reset(4, 2);
    for (int n = 0; n < 6; ++n) {
        const auto field = n % 2 ? video_field::b : video_field::a;
        require(buffer.push(field, delay.push(make_frame(n))) == (n < 4), "Unexpected delayed overload behavior");
    }
    for (int n = 0; n < 4; ++n)
        require(buffer.pop().audio_data().data()[0] == n % 2, "Preroll did not preserve the first field pair");

    buffer.push(video_field::a, delay.push(make_frame(6)));
    buffer.push(video_field::b, delay.push(make_frame(7)));
    require(buffer.pop().audio_data().data()[0] == 2, "Dropping a frame changed the configured delay");
    require(buffer.pop().audio_data().data()[0] == 3, "Dropping a frame changed the delayed field parity");
}

void check_ndi_shutdown()
{
    using namespace std::chrono_literals;
    caspar::newtek::ndi_frame_buffer buffer;
    buffer.reset(16, 1);
    auto preroll = std::async(std::launch::async, [&] { return buffer.wait_for_preroll(); });
    require(preroll.wait_for(20ms) == std::future_status::timeout, "NDI did not wait for preroll");
    buffer.stop();
    require(preroll.wait_for(1s) == std::future_status::ready, "Shutdown did not wake preroll");
    require(!preroll.get(), "Stopped preroll reported success");

    buffer.reset(1, 1);
    auto waiting = std::async(std::launch::async, [&] { return buffer.pop(); });
    require(waiting.wait_for(20ms) == std::future_status::timeout, "NDI did not wait for an incoming frame");
    buffer.stop();
    require(waiting.wait_for(1s) == std::future_status::ready, "Shutdown did not wake the send wait");
    require(!waiting.get(), "Shutdown returned a stale frame");

    buffer.reset(16, 1);
    buffer.push(caspar::core::video_field::progressive, make_frame(1));
    buffer.stop();
    require(buffer.size() == 0, "Shutdown retained pending NDI frames");
    buffer.reset(2, 2);
    require(buffer.push(caspar::core::video_field::a, make_frame(2)), "Format change did not reset field alignment");
    require(buffer.push(caspar::core::video_field::b, make_frame(3)), "Format change did not reset field B");
    require(buffer.wait_for_preroll(), "Small interlaced buffer did not complete preroll");
}

void check_ndi_preroll()
{
    using namespace std::chrono_literals;
    using caspar::core::video_field;
    caspar::newtek::ndi_frame_buffer buffer;
    buffer.reset(16, 1);
    for (int n = 0; n < 7; ++n)
        require(buffer.push(video_field::progressive, make_frame(n)), "Preroll rejected an incoming frame");
    auto ready = std::async(std::launch::async, [&] { return buffer.wait_for_preroll(); });
    require(ready.wait_for(20ms) == std::future_status::timeout, "NDI started before eight buffered fields");
    buffer.push(video_field::progressive, make_frame(7));
    require(ready.wait_for(1s) == std::future_status::ready, "NDI did not start after eight buffered fields");
    require(ready.get(), "NDI preroll unexpectedly stopped");
    for (int n = 8; n < 10; ++n)
        require(buffer.push(video_field::progressive, make_frame(n)), "Default buffer dropped a ten-frame burst");

    buffer.stop();
    buffer.reset(64, 2);
    for (int n = 0; n < 8; ++n)
        buffer.push(n % 2 ? video_field::b : video_field::a, make_frame(n));
    require(buffer.wait_for_preroll(), "Increasing capacity changed the existing startup cushion");
}

} // namespace

int main()
{
    try {
        check_sequence(caspar::timespan{}, 25, 1, 0);
        check_sequence(caspar::timespan{1}, 25, 1, 1);
        check_sequence(caspar::timespan{5}, 25, 1, 5);
        check_sequence(caspar::timespan{"200ms"}, 25, 1, 5);
        check_sequence(caspar::timespan{"20ms"}, 25, 1, 1);
        check_sequence(caspar::timespan{25}, 25, 1, 25);
        check_sequence(caspar::timespan{-5}, 25, 1, 0);
        check_sequence(caspar::timespan{100}, 25, 1, 25);
        check_sequence(caspar::timespan{}, 25, 2, 0);
        check_sequence(caspar::timespan{1}, 25, 2, 1);
        check_sequence(caspar::timespan{"200ms"}, 25, 2, 5);
        check_sequence(caspar::timespan{"1s"}, 30000.0 / 1001, 2, 29);
        check_sequence(caspar::timespan{"200ms"}, 30000.0 / 1001, 2, 6);
        check_sequence(caspar::timespan{"200ms"}, 60000.0 / 1001, 1, 12);
        check_reset();
        check_ndi_capacity();
        check_ndi_field_pairs();
        check_ndi_delayed_overload();
        check_ndi_shutdown();
        check_ndi_preroll();
        std::cout << "Consumer frame delay regression checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
