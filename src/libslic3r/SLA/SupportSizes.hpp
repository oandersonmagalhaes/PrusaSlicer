///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef SLA_SUPPORTSIZES_HPP
#define SLA_SUPPORTSIZES_HPP

#include <optional>
#include <string>

#include "libslic3r/SLA/SupportPoint.hpp"
#include "libslic3r/SLA/SupportTree.hpp"

namespace Slic3r {

class DynamicPrintConfig;

namespace sla {

// One-click support sizes, modelled on CHITUBOX's Light / Middle / Heavy.
enum class SupportSize { light, medium, heavy };

struct SupportSizeSpec
{
    double head_front_diameter;
    double head_penetration;
    double head_width;
    double pillar_diameter;
    double base_diameter;
    double base_height;
};

const SupportSizeSpec &support_size_spec(SupportSize size);

// Writes the six <prefix>support_* keys of the given size. The prefix selects
// the tree type ("" for default, "branching" for branching supports).
void apply_support_size(DynamicPrintConfig &cfg, SupportSize size, const std::string &prefix);

// The size whose values the config holds, or nullopt for custom values.
std::optional<SupportSize> match_support_size(const DynamicPrintConfig &cfg, const std::string &prefix);

// The size a support point asks for, or nullopt when it follows the settings.
std::optional<SupportSize> to_support_size(SupportPointSize size);

// Nominal pinhead of a support point: from the settings, or from the size
// table when the point has its own size. The tip radius always comes from
// the point itself.
struct HeadDims
{
    double r_back;
    double width;
    double penetration;
};

HeadDims head_dims(const SupportTreeConfig &cfg, const SupportPoint &sp);

}} // namespace Slic3r::sla

#endif // SLA_SUPPORTSIZES_HPP
