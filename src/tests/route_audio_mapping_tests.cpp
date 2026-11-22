#include <core/frame/draw_frame.h>
#include <core/frame/frame.h>
#include <core/frame/frame_transform.h>
#include <core/frame/frame_visitor.h>
#include <core/mixer/image/image_mixer.h>
#include <core/producer/cg_proxy.h>
#include <core/producer/frame_producer.h>
#include <core/producer/frame_producer_registry.h>
#include <core/producer/route/route_audio_mapping.h>
#include <core/producer/route/route_producer.h>
#include <core/video_channel.h>

#include <array>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition)
{
    if (!condition) {
        throw std::runtime_error("route audio mapping sample test failed");
    }
}

class test_image_mixer final : public caspar::core::image_mixer
{
  public:
    void                      push(const caspar::core::frame_transform&) override {}
    void                      visit(const caspar::core::const_frame&) override {}
    void                      pop() override {}
    void                      update_aspect_ratio(double) override {}
    caspar::common::bit_depth depth() const override { return caspar::common::bit_depth::bit8; }
    std::future<std::tuple<caspar::array<const uint8_t>, std::shared_ptr<caspar::core::texture>>>
    render(const caspar::core::video_format_desc&, bool) override
    {
        throw std::runtime_error("route PCM tests must not render video");
    }
    caspar::core::mutable_frame create_frame(const void* tag, const caspar::core::pixel_format_desc& desc) override
    {
        std::vector<caspar::array<uint8_t>> image;
        for (const auto& plane : desc.planes)
            image.emplace_back(std::vector<uint8_t>(plane.size));
        return caspar::core::mutable_frame(tag, std::move(image), {}, desc);
    }
    caspar::core::mutable_frame
    create_frame(const void* tag, const caspar::core::pixel_format_desc& desc, caspar::common::bit_depth) override
    {
        return create_frame(tag, desc);
    }
#ifdef WIN32
    caspar::core::const_frame import_d3d_texture(const void*,
                                                 const std::shared_ptr<caspar::accelerator::d3d::d3d_texture2d>&,
                                                 caspar::core::pixel_format,
                                                 caspar::common::bit_depth) override
    {
        throw std::runtime_error("route PCM tests must not import GPU textures");
    }
#endif
};

class capture_frame final : public caspar::core::frame_visitor
{
    std::vector<double> volumes_{1.0};

  public:
    std::optional<caspar::core::const_frame> frame;
    std::vector<caspar::core::const_frame>   frames;
    std::vector<double>                      frame_volumes;
    double                                   volume = 1.0;

    void push(const caspar::core::frame_transform& transform) override
    {
        volumes_.push_back(volumes_.back() * transform.audio_transform.volume);
    }
    void visit(const caspar::core::const_frame& value) override
    {
        frame = value;
        frames.push_back(value);
        volume = volumes_.back();
        frame_volumes.push_back(volume);
    }
    void pop() override { volumes_.pop_back(); }
};

struct captured_result
{
    std::optional<caspar::core::const_frame> frame;
    double                                   volume;
};

void test_native_route(const std::wstring& format_name)
{
    using namespace caspar;
    using namespace caspar::core;

    const auto format = video_format_repository().find(format_name);
    check(format.audio_channels == 16);
    auto source = spl::make_shared<video_channel>(
        1, format, color_space::bt709, std::make_unique<test_image_mixer>(), [](monitor::state) {});
    const std::vector<spl::shared_ptr<video_channel>> channels{source};
    frame_producer_dependencies                       dependencies(source->frame_factory(),
                                             channels,
                                             video_format_repository(),
                                             format,
                                             spl::make_shared<frame_producer_registry>(),
                                             spl::make_shared<cg_producer_registry>());
    auto      producer = create_route_producer(dependencies, {L"route://1-10", L"AUDIO_MAP", L"0:14,1:15"});
    auto      route    = source->route(10);
    const int samples  = format.audio_cadence.front();
    const int ramp     = format.audio_sample_rate / 200;
    check(samples > 2 * ramp);
    int               tag = 0;
    pixel_format_desc desc(pixel_format::bgra);
    desc.planes.emplace_back(1, 1, 4);
    std::vector<array<uint8_t>> image;
    image.emplace_back(std::vector<uint8_t>{1, 2, 3, 4});
    std::vector<int32_t> pcm(static_cast<std::size_t>(samples) * 16);
    for (int sample = 0; sample < samples; ++sample)
        for (int channel = 0; channel < 16; ++channel)
            pcm[sample * 16 + channel] = 10000 + channel * 1000;
    mutable_frame input(
        &tag, std::move(image), array<int32_t>(std::move(pcm)), desc, [](std::vector<array<const uint8_t>>) {
            return std::any(std::string("route-metadata"));
        });
    const_frame original(std::move(input));
    const auto  publish = [&](int count = 0) {
        auto published = count == 0 ? original
                                     : original.with_tag(&tag,
                                                        array<const int32_t>(original.audio_data().data(),
                                                                             static_cast<std::size_t>(count) * 16,
                                                                             original.audio_data()));
        route->signal(draw_frame(published), format.field_count == 2 ? draw_frame(published) : draw_frame{});
    };
    const auto receive = [&](video_field field) {
        capture_frame captured;
        producer->receive(field, samples).accept(captured);
        check(captured.frame.has_value());
        check(captured.frame->image_data(0).data() == original.image_data(0).data());
        check(std::any_cast<std::string>(captured.frame->opaque()) == "route-metadata");
        return captured_result{captured.frame, captured.volume};
    };
    const auto first_field = format.field_count == 2 ? video_field::a : video_field::progressive;
    {
        auto default_producer = create_route_producer(dependencies, {L"route://1-10"});
        publish();
        capture_frame default_frame;
        default_producer->receive(first_field, samples).accept(default_frame);
        check(default_frame.frame.has_value());
        check(default_frame.frame->audio_data().data() == original.audio_data().data());
        check(parse_route_audio_mapping(default_producer->call({L"AUDIO_MAP"}).get(), 16, 16) ==
              identity_route_audio_mapping(16));
        if (format.field_count == 2)
            default_producer->receive(video_field::b, samples);
        capture_frame repeated;
        default_producer->receive(first_field, samples).accept(repeated);
        check(repeated.volume == 1.0);
    }
    auto initial = receive(first_field);
    check(initial.frame->audio_data().data()[0] == 24000 && initial.frame->audio_data().data()[1] == 25000);
    check(initial.frame->audio_data().data()[2] == 12000 && original.audio_data().data()[0] == 10000);
    check(initial.frame->audio_data().data() != original.audio_data().data());
    if (format.field_count == 2)
        check(receive(video_field::b).frame->audio_data().data()[0] == 24000);
    const auto tag_before = initial.frame->stream_tag();

    producer->call({L"AUDIO_MAP", L"0:6"}).get();
    publish();
    auto        faded = receive(first_field);
    const auto* audio = faded.frame->audio_data().data();
    check(audio[0] == 24000 && audio[(ramp / 2) * 16] == 12000);
    check(audio[ramp * 16] == 0 && audio[(2 * ramp) * 16] == 16000);
    for (int sample = 0; sample < samples; ++sample) {
        check(audio[sample * 16 + 1] == 25000);
        for (int channel = 2; channel < 16; ++channel)
            check(audio[sample * 16 + channel] == original.audio_data().data()[sample * 16 + channel]);
    }
    check(faded.frame->stream_tag() == tag_before);
    if (format.field_count == 2)
        check(receive(video_field::b).frame->audio_data().data()[0] == 16000);
    bool rejected = false;
    try {
        producer->call({L"AUDIO_MAP", L"1:2,0:16"}).get();
    } catch (const user_error&) {
        rejected = true;
    }
    auto expected = identity_route_audio_mapping(16);
    expected[0]   = 6;
    expected[1]   = 15;
    check(rejected && parse_route_audio_mapping(producer->call({L"AUDIO_MAP"}).get(), 16, 16) == expected);
    producer->call({L"AUDIO_MAP", L"0:6"}).get();
    publish();
    auto unchanged = receive(first_field);
    check(unchanged.frame->audio_data().data()[0] == 16000);
    if (format.field_count == 2)
        receive(video_field::b);

    producer->call({L"AUDIO_MAP", L"1:SILENCE"}).get();
    publish();
    auto silenced = receive(first_field);
    for (int sample = 0; sample < samples; ++sample) {
        check(silenced.frame->audio_data().data()[sample * 16] == 16000);
        check(silenced.frame->audio_data().data()[sample * 16 + 2] == 12000);
    }
    check(silenced.frame->audio_data().data()[2 * ramp * 16 + 1] == 0);
    expected[1] = -1;
    check(parse_route_audio_mapping(producer->call({L"AUDIO_MAP"}).get(), 16, 16) == expected);
    if (format.field_count == 2)
        receive(video_field::b);
    check(receive(first_field).volume == 0.0);
    producer->call({L"AUDIO_MAP", L"DEFAULT"}).get();
    check(receive(first_field).volume == 0.0);
    publish();
    auto restored = receive(first_field);
    check(restored.frame->audio_data().data()[2 * ramp * 16] == 10000);
    check(restored.frame->audio_data().data()[2 * ramp * 16 + 1] == 11000);
    for (int sample = 0; sample < samples; ++sample)
        for (int channel = 2; channel < 16; ++channel)
            check(restored.frame->audio_data().data()[sample * 16 + channel] ==
                  original.audio_data().data()[sample * 16 + channel]);
    check(parse_route_audio_mapping(producer->call({L"AUDIO_MAP"}).get(), 16, 16) == identity_route_audio_mapping(16));
    if (format.field_count == 2)
        receive(video_field::b);
    publish();
    auto passthrough = receive(first_field);
    check(passthrough.frame->audio_data().data() == original.audio_data().data());
    check(passthrough.frame->audio_data().data()[0] == 10000 && passthrough.volume == 1.0);

    if (format.field_count == 1) {
        auto mixed_producer = create_route_producer(dependencies, {L"route://1-11"});
        auto mixed_route    = source->route(11);
        mixed_route->signal(draw_frame(original), draw_frame{});
        mixed_producer->receive(first_field, samples);
        mixed_producer->call({L"AUDIO_MAP", L"0:3"}).get();
        auto short_leaf                                = original.with_tag(&tag,
                                            array<const int32_t>(original.audio_data().data(),
                                                                 static_cast<std::size_t>(ramp / 2) * 16,
                                                                 original.audio_data()));
        auto short_layer                               = draw_frame(short_leaf);
        auto long_layer                                = draw_frame(original);
        short_layer.transform().audio_transform.volume = 0.5;
        long_layer.transform().audio_transform.volume  = 0.25;
        mixed_route->signal(draw_frame(std::vector<draw_frame>{short_layer, long_layer}), draw_frame{});
        capture_frame unequal_leaves;
        mixed_producer->receive(first_field, samples).accept(unequal_leaves);
        check(unequal_leaves.frames.size() == 2);
        check(unequal_leaves.frame_volumes[0] == 0.5 && unequal_leaves.frame_volumes[1] == 0.25);
        check(unequal_leaves.frames[0].audio_data().data()[0] == 10000);
        check(unequal_leaves.frames[1].audio_data().data()[0] == 10000);
        check(unequal_leaves.frames[1].audio_data().data()[ramp * 16] == 0);
        check(unequal_leaves.frames[1].audio_data().data()[2 * ramp * 16] == 13000);
        for (const auto& leaf : unequal_leaves.frames)
            for (std::size_t sample = 0; sample < leaf.audio_data().size() / 16; ++sample)
                for (int channel = 1; channel < 16; ++channel)
                    check(leaf.audio_data().data()[sample * 16 + channel] ==
                          original.audio_data().data()[sample * 16 + channel]);

        mixed_producer->call({L"AUDIO_MAP", L"DEFAULT"}).get();
        mixed_route->signal(draw_frame(std::vector<draw_frame>{draw_frame(original), draw_frame(short_leaf)}),
                            draw_frame{});
        capture_frame reverse_order;
        mixed_producer->receive(first_field, samples).accept(reverse_order);
        check(reverse_order.frames.size() == 2);
        check(reverse_order.frames[0].audio_data().data()[2 * ramp * 16] == 10000);
        check(reverse_order.frames[1].audio_data().data()[0] == 13000);
        check(reverse_order.frame_volumes[0] == 1.0 && reverse_order.frame_volumes[1] == 1.0);

        producer->call({L"AUDIO_MAP", L"0:3"}).get();
        publish(ramp / 2);
        check(receive(first_field).frame->audio_data().data()[0] == 10000);
        producer->call({L"AUDIO_MAP", L"0:4,1:5"}).get();
        publish(ramp / 2);
        auto pending = receive(first_field);
        check(pending.frame->audio_data().data()[0] == 5000);
        check(pending.frame->audio_data().data()[1] == 11000);
        publish(ramp / 2);
        auto switched = receive(first_field);
        check(switched.frame->audio_data().data()[0] == 0);
        check(switched.frame->audio_data().data()[1] == 5500);
        publish(ramp / 2);
        auto rising = receive(first_field);
        check(rising.frame->audio_data().data()[0] == 7000);
        check(rising.frame->audio_data().data()[1] == 0);
        publish();
        auto finished = receive(first_field);
        check(finished.frame->audio_data().data()[0] == 14000);
        check(finished.frame->audio_data().data()[1] == 7500);
        check(finished.frame->audio_data().data()[ramp * 16 + 1] == 15000);
        for (int sample = 0; sample < samples; ++sample)
            for (int channel = 2; channel < 16; ++channel)
                check(finished.frame->audio_data().data()[sample * 16 + channel] ==
                      original.audio_data().data()[sample * 16 + channel]);
    }

    auto patch2 = std::async(std::launch::async, [&] { producer->call({L"AUDIO_MAP", L"2:8"}).get(); });
    auto patch3 = std::async(std::launch::async, [&] { producer->call({L"AUDIO_MAP", L"3:9"}).get(); });
    patch2.get();
    patch3.get();
    const auto concurrent = parse_route_audio_mapping(producer->call({L"AUDIO_MAP"}).get(), 16, 16);
    check(concurrent[2] == 8 && concurrent[3] == 9 && concurrent[4] == 4);
}

} // namespace

int main()
{
    using caspar::core::identity_route_audio_mapping;
    using caspar::core::parse_route_audio_mapping;
    using caspar::core::remap_route_audio;

    const auto                         mapping = parse_route_audio_mapping(L"2:0, 3:1, 4:0", 8, 8);
    const std::array<std::int32_t, 16> input   = {10, 11, 12, 13, 14, 15, 16, 17, 20, 21, 22, 23, 24, 25, 26, 27};
    const auto                         output  = remap_route_audio(input.data(), input.size(), 8, 8, mapping);

    check(output.size() == input.size());
    check(output[0] == 10 && output[1] == 11);
    check(output[2] == 10 && output[3] == 11);
    check(output[4] == 10 && output[5] == 15);
    check(output[10] == 20 && output[11] == 21);
    check(input[0] == 10 && input[1] == 11);
    const auto patched = parse_route_audio_mapping(L"0:7,5:SiLeNcE", 8, 8, mapping);
    check(patched[0] == 7 && patched[2] == 0 && patched[3] == 1 && patched[5] == -1 && patched[6] == 6);
    const auto silence = remap_route_audio(input.data(), input.size(), 8, 8, patched);
    check(silence[5] == 0 && silence[13] == 0 && silence[6] == 16);

    const auto invalid_update = [&] { parse_route_audio_mapping(L"2:0,2:1", 8, 8); };
    bool       rejected       = false;
    try {
        invalid_update();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected);

    for (const auto& value : {L"",
                              L"0",
                              L"0::1",
                              L"0=1",
                              L"0:1:2",
                              L":1",
                              L"0:",
                              L"0:1,1=2",
                              L"-1:0",
                              L"0:-1",
                              L"0:8",
                              L"0:0,",
                              L",0:0",
                              L"0:0,,1:1",
                              L"999999999999999999999:0",
                              L"0:SILENCE,0:1",
                              L"0:SILENT",
                              L"0:0,1:SILENT"}) {
        rejected = false;
        try {
            parse_route_audio_mapping(value, 8, 8);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        check(rejected);
    }

    rejected = false;
    try {
        parse_route_audio_mapping(L"8:0", 8, 8);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected);

    rejected = false;
    try {
        parse_route_audio_mapping(L"2:x", 8, 8);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected);

    const auto recovery_mapping = parse_route_audio_mapping(L"2:4,3:5", 8, 8);
    const auto recovered        = remap_route_audio(input.data(), input.size(), 8, 8, recovery_mapping);
    check(recovered[2] == 14 && recovered[3] == 15);
    check(recovered[10] == 24 && recovered[11] == 25);

    const std::array<std::int32_t, 3> partial_input   = {30, 31, 32};
    const auto                        partial_mapping = parse_route_audio_mapping(L"0:0,1:1", 2, 4);
    const auto partial = remap_route_audio(partial_input.data(), partial_input.size(), 2, 4, partial_mapping);
    check(partial.size() == 8);
    check(partial[0] == 30 && partial[1] == 31 && partial[2] == 0 && partial[3] == 0);
    check(partial[4] == 32 && partial[5] == 0);

    for (const auto* format : {L"1080p2500", L"1080p5000", L"1080p5994", L"1080i5994"})
        test_native_route(format);
}
