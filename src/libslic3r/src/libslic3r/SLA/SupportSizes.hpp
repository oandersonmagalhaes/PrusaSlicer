///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Slic3r/Domain/SLA/SupportPoint.hpp"
#include "libslic3r/SLA/SupportTree.hpp"

namespace Slic3r::sla {

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

// The six <prefix>support_* options of a size with their values. The prefix
// selects the tree type ("" for default, "branching" for branching supports).
std::vector<std::pair<std::string, double>> support_size_values(SupportSize size, const std::string &prefix);

// The size whose values `get` returns for those options, or nullopt for
// custom values. `get` returns an option's current value.
std::optional<SupportSize> match_support_size(const std::function<double(const std::string &key)> &get,
                                              const std::string &prefix);

// The size a support point asks for, or nullopt when it follows the settings.
std::optional<SupportSize> to_support_size(Domain::SLA::SupportPointSize size);

// Nominal pinhead of a support point: from the settings, or from the size
// table when the point has its own size. The tip radius always comes from
// the point itself.
struct HeadDims
{
    double r_back;
    double width;
    double penetration;
};

HeadDims head_dims(const SupportTreeConfig &cfg, const Domain::SLA::SupportPoint &sp);

} // namespace Slic3r::sla
