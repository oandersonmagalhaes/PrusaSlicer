#include "Slic3r/Domain/SLA/SupportPoint.hpp"

namespace Slic3r::Domain::SLA {

bool SupportPoint::operator==(const SupportPoint& sp) const
{
    float rdiff = std::abs(head_front_radius - sp.head_front_radius);
    return (pos == sp.pos) && rdiff < float(EPSILON) && type == sp.type && size == sp.size;
}

bool SupportPoint::operator!=(const SupportPoint& sp) const { return !(sp == (*this)); }
} // namespace Slic3r::Domain::SLA
