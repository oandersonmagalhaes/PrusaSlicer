///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SupportSizes.hpp"

#include <cmath>

namespace Slic3r::sla {

namespace {

// Values from CHITUBOX's bundled printer profiles (Light / Middle / Heavy),
// mapped to PrusaSlicer's parameters. CHITUBOX's feet are 10-12 mm skate
// discs, so the base diameters are scaled to PrusaSlicer's cone feet instead.
const SupportSizeSpec Specs[] = {
    // tip   depth  length pillar base_d base_h
    {  0.3,  0.3,   2.0,   0.8,   3.0,   0.8 }, // light
    {  0.4,  0.4,   2.0,   1.2,   4.0,   1.0 }, // medium
    {  0.6,  0.6,   3.0,   1.5,   5.0,   1.0 }, // heavy
};

struct Field { const char *key; double SupportSizeSpec::*value; };

const Field Fields[] = {
    {"support_head_front_diameter", &SupportSizeSpec::head_front_diameter},
    {"support_head_penetration",    &SupportSizeSpec::head_penetration},
    {"support_head_width",          &SupportSizeSpec::head_width},
    {"support_pillar_diameter",     &SupportSizeSpec::pillar_diameter},
    {"support_base_diameter",       &SupportSizeSpec::base_diameter},
    {"support_base_height",         &SupportSizeSpec::base_height},
};

} // namespace

const SupportSizeSpec &support_size_spec(SupportSize size)
{
    return Specs[static_cast<int>(size)];
}

std::vector<std::pair<std::string, double>> support_size_values(SupportSize size, const std::string &prefix)
{
    const SupportSizeSpec &spec = support_size_spec(size);
    std::vector<std::pair<std::string, double>> values;
    for (const Field &f : Fields)
        values.emplace_back(prefix + f.key, spec.*f.value);
    return values;
}

std::optional<SupportSize> match_support_size(const std::function<double(const std::string &)> &get,
                                              const std::string &prefix)
{
    for (SupportSize size : {SupportSize::light, SupportSize::medium, SupportSize::heavy}) {
        bool same = true;
        for (const auto &[key, value] : support_size_values(size, prefix))
            if (std::abs(get(key) - value) > 1e-6) {
                same = false;
                break;
            }
        if (same)
            return size;
    }
    return std::nullopt;
}

std::optional<SupportSize> to_support_size(Domain::SLA::SupportPointSize size)
{
    using Size = Domain::SLA::SupportPointSize;
    switch (size) {
    case Size::light:  return SupportSize::light;
    case Size::medium: return SupportSize::medium;
    case Size::heavy:  return SupportSize::heavy;
    case Size::from_settings: break;
    }
    return std::nullopt;
}

HeadDims head_dims(const SupportTreeConfig &cfg, const Domain::SLA::SupportPoint &sp)
{
    if (auto size = to_support_size(sp.size)) {
        const SupportSizeSpec &spec = support_size_spec(*size);
        return {spec.pillar_diameter / 2., spec.head_width, spec.head_penetration};
    }
    return {cfg.head_back_radius_mm, cfg.head_width_mm, cfg.head_penetration_mm};
}

} // namespace Slic3r::sla
