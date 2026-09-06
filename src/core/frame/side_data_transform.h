#pragma once

#include <common/tweener.h>

#include <optional>

namespace caspar { namespace core {

class closed_captions_priority final
{
  public:
    constexpr closed_captions_priority() noexcept
        : value_(0)
    {
    }
    /// larger values mean higher priority; zero, negative, or NaN means don't use closed captions
    explicit constexpr closed_captions_priority(float value) noexcept
        : closed_captions_priority()
    {
        // > returns false for NaN, which is correct here
        if (value > 0) {
            // if non-empty then set value, otherwise leave it at zero to normalize the value
            value_ = value;
        }
    }
    explicit constexpr closed_captions_priority(std::optional<float> value) noexcept
        : closed_captions_priority()
    {
        if (value) {
            *this = closed_captions_priority(*value);
        }
    }
    explicit constexpr operator bool() const noexcept
    {
        // > returns false for NaN, which is correct here
        return value_ > 0;
    }
    explicit constexpr operator float() const noexcept
    {
        if (*this)
            return value_;
        return 0;
    }
    explicit constexpr operator std::optional<float>() const noexcept
    {
        if (*this)
            return value_;
        return std::nullopt;
    }
    friend constexpr bool operator==(closed_captions_priority l, closed_captions_priority r) noexcept
    {
        // empty == empty
        return static_cast<std::optional<float>>(l) == static_cast<std::optional<float>>(r);
    }
    friend constexpr bool operator!=(closed_captions_priority l, closed_captions_priority r) noexcept
    {
        return !(l == r);
    }
    friend constexpr bool operator<(closed_captions_priority l, closed_captions_priority r) noexcept
    {
        // empty is considered less, so optional correctly handles the comparison
        return static_cast<std::optional<float>>(l) < static_cast<std::optional<float>>(r);
    }
    friend constexpr bool operator>(closed_captions_priority l, closed_captions_priority r) noexcept { return r < l; }
    friend constexpr bool operator<=(closed_captions_priority l, closed_captions_priority r) noexcept
    {
        return !(l > r);
    }
    friend constexpr bool operator>=(closed_captions_priority l, closed_captions_priority r) noexcept
    {
        return !(l < r);
    }

  private:
    float value_;
};

struct side_data_transform final
{
    closed_captions_priority closed_captions_priority_{1};

    static side_data_transform tween(double                     time,
                                     const side_data_transform& source,
                                     const side_data_transform& dest,
                                     double                     duration,
                                     const tweener&             tween);
};

bool operator==(const side_data_transform& lhs, const side_data_transform& rhs);
bool operator!=(const side_data_transform& lhs, const side_data_transform& rhs);

}} // namespace caspar::core