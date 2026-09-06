#include "side_data_transform.h"

namespace caspar { namespace core {

side_data_transform side_data_transform::tween(double                     time,
                                               const side_data_transform& source,
                                               const side_data_transform& dest,
                                               double                     duration,
                                               const tweener&             tween)
{
    side_data_transform result;
    // shouldn't tween since that can have weird effects like temporarily picking some third source while transitioning
    // between two sources
    result.closed_captions_priority_ = dest.closed_captions_priority_;
    static_cast<void>(time);
    static_cast<void>(source);
    static_cast<void>(duration);
    static_cast<void>(tween);
    return result;
}

bool operator==(const side_data_transform& lhs, const side_data_transform& rhs)
{
    return lhs.closed_captions_priority_ == rhs.closed_captions_priority_;
}

bool operator!=(const side_data_transform& lhs, const side_data_transform& rhs) { return !(lhs == rhs); }

}} // namespace caspar::core