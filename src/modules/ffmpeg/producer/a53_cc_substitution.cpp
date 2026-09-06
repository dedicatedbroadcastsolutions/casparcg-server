#include "a53_cc_substitution.h"

#include "../util/av_assert.h"
#include "core/frame/frame_side_data.h"

#include <boost/log/utility/manipulators/dump.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace caspar { namespace ffmpeg {

class SubstitutedA53CCQueue::Impl final
{
  public:
    void insert_and_replace_with_key(AVFrame* frame)
    {
        if (auto* side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC)) {
            auto buf = std::vector(side_data->data, side_data->data + side_data->size);
            auto pos = queue_.add_frame(
                std::vector{core::const_frame_side_data(core::frame_side_data_type::a53_cc, std::move(buf))});
            key key_value(pos);
            CASPAR_LOG(trace) << "ffmpeg producer: SubstitutedA53CCQueue::insert_and_replace_with_key: key: "
                              << boost::log::dump(key_value.data, sizeof(key_value.data))
                              << "  input: " << boost::log::dump(side_data->data, side_data->size, 128);
            av_frame_remove_side_data(frame, AV_FRAME_DATA_A53_CC);
            side_data = FFMEM(av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC, sizeof(key_value.data)));
            std::memcpy(side_data->data, key_value.data, sizeof(key_value.data));
        }
    }

    void extract_by_key(AVFrame* frame)
    {
        if (auto* side_data = av_frame_get_side_data(frame, AV_FRAME_DATA_A53_CC)) {
            CASPAR_LOG(trace) << "ffmpeg producer: SubstitutedA53CCQueue::extract_by_key: input: "
                              << boost::log::dump(side_data->data, side_data->size, 128);
            auto valid_position_range = queue_.valid_position_range();
            std::vector<std::uint8_t> side_data_from_queue;
            auto*                     data = side_data->data;
            std::size_t               size = side_data->size;
            while (size > 0) {
                if (size >= a53_cc_chunk_size) {
                    auto cc_valid = (data[0] & 0x04) >> 2;
                    auto cc_type  = data[0] & 0x03;
                    if ((cc_type == 0x00 || cc_type == 0x01) && data[1] == 0x80 && data[2] == 0x80) {
                        data += a53_cc_chunk_size;
                        size -= a53_cc_chunk_size;
                        continue;
                    } else if ((cc_type != 0x00 && cc_type != 0x01) && !cc_valid) {
                        data += a53_cc_chunk_size;
                        size -= a53_cc_chunk_size;
                        continue;
                    }
                }
                key key_value;
                if (size >= sizeof(key_value.data)) {
                    std::memcpy(key_value.data, data, sizeof(key_value.data));
                    if (auto pos = key_value.pos(valid_position_range)) {
                        if (auto current_side_data = queue_.get(*pos)) {
                            auto& current_data = (*current_side_data)[0].data();
                            side_data_from_queue.insert(side_data_from_queue.end(), current_data.begin(), current_data.end());
                        }
                        data += sizeof(key_value.data);
                        size -= sizeof(key_value.data);
                        continue;
                    }
                }
                if (!corrupted_.exchange(true, std::memory_order_relaxed)) {
                    CASPAR_LOG(error) << "ffmpeg producer: SubstitutedA53CCQueue: closed captions corrupted by ffmpeg, removing them.";
                    break;
                }
            }
            av_frame_remove_side_data(frame, AV_FRAME_DATA_A53_CC);
            if (!side_data_from_queue.empty() && !corrupted_.load(std::memory_order_relaxed)) {
                side_data = FFMEM(av_frame_new_side_data(frame, AV_FRAME_DATA_A53_CC, side_data_from_queue.size()));
                std::memcpy(side_data->data, side_data_from_queue.data(), side_data_from_queue.size());
                CASPAR_LOG(trace) << "ffmpeg producer: SubstitutedA53CCQueue::extract_by_key: output: "
                                  << boost::log::dump(side_data->data, side_data->size, 128);
            }
        }
    }

  private:
    using position = core::frame_side_data_queue::position;
    static inline constexpr std::size_t a53_cc_chunk_size = 3;

    static constexpr std::uint8_t to_hex_digit(std::uint8_t value)
    {
        value &= 0xF;
        return value < 0xA ? value + '0' : value - 0xA + 'A';
    }
    static constexpr std::optional<std::uint8_t> from_hex_digit(std::uint8_t value)
    {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 0xA;
        return std::nullopt;
    }
    struct key final
    {
        static constexpr inline std::uint8_t data_template[][a53_cc_chunk_size] = {
            {0xFF, 'C', 'C'}, {0xFE, 'Q', ':'}, {0xFE, 0x0C, 0x08}, {0xFE, 0x04, 0x00}};
        std::uint8_t data[sizeof(data_template)];

        constexpr explicit key(position pos) noexcept
            : data{}
        {
            const std::uint8_t* template_data = &data_template[0][0];
            for (std::size_t index = 0; index < sizeof(data_template); index++)
                data[index] = template_data[index] < 0x20 ? to_hex_digit(pos >> template_data[index]) : template_data[index];
        }
        key() noexcept = default;
        constexpr std::optional<position> pos(std::pair<position, position> valid_position_range) const noexcept
        {
            position result = valid_position_range.second;
            const std::uint8_t* template_data = &data_template[0][0];
            for (std::size_t index = 0; index < sizeof(data_template); index++) {
                if (template_data[index] < 0x20) {
                    result &= ~(static_cast<position>(0xF) << template_data[index]);
                    if (auto digit = from_hex_digit(data[index]))
                        result |= static_cast<position>(*digit) << template_data[index];
                    else
                        return std::nullopt;
                } else if (data[index] != template_data[index]) {
                    return std::nullopt;
                }
            }
            for (position offset : {-1 << 16, 0, 1 << 16}) {
                position candidate = result + offset;
                if (candidate >= valid_position_range.first && candidate < valid_position_range.second)
                    return candidate;
            }
            return std::nullopt;
        }
    };

    core::frame_side_data_queue queue_;
    std::atomic_bool            corrupted_ = false;
};

SubstitutedA53CCQueue::SubstitutedA53CCQueue()
    : impl_(std::make_unique<Impl>())
{
}

SubstitutedA53CCQueue::~SubstitutedA53CCQueue() = default;

void SubstitutedA53CCQueue::insert_and_replace_with_key(AVFrame* frame)
{
    impl_->insert_and_replace_with_key(frame);
}

void SubstitutedA53CCQueue::extract_by_key(AVFrame* frame) { impl_->extract_by_key(frame); }

}} // namespace caspar::ffmpeg