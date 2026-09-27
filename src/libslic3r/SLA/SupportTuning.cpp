///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#include "SupportTuning.hpp"

#include <algorithm>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r { namespace sla {

std::vector<std::string> support_tuning_keys(const std::string &prefix)
{
    std::vector<std::string> keys;
    for (const char *k : {"support_head_shape", "support_head_front_diameter", "support_head_penetration",
                          "support_head_width", "support_pillar_diameter", "support_pillar_shape",
                          "support_pillar_connection_mode", "support_max_pillar_link_distance",
                          "support_base_shape", "support_base_diameter", "support_base_height",
                          "support_object_elevation"})
        keys.emplace_back(prefix + k);

    for (const char *k : {"pad_enable", "pad_shape", "pad_wall_thickness", "pad_brim_size",
                          "pad_grid_opening", "pad_grid_bar_width"})
        keys.emplace_back(k);

    return keys;
}

bool clamp_support_tuning(DynamicPrintConfig &cfg, const std::string &prefix)
{
    bool changed = false;
    auto cap = [&cfg, &prefix, &changed](const char *key, const char *limit_key) {
        const double limit = cfg.opt_float(prefix + limit_key);
        if (cfg.opt_float(prefix + key) > limit) {
            cfg.set_key_value(prefix + key, new ConfigOptionFloat(limit));
            changed = true;
        }
    };

    cap("support_head_front_diameter", "support_pillar_diameter");
    cap("support_head_penetration", "support_head_width");

    return changed;
}

RaftChoice raft_choice(const DynamicPrintConfig &cfg)
{
    if (!cfg.opt_bool("pad_enable"))
        return RaftChoice::none;

    return cfg.opt_enum<PadShape>("pad_shape") == PadShape::grid ? RaftChoice::grid : RaftChoice::skate;
}

void set_raft_choice(DynamicPrintConfig &cfg, RaftChoice choice)
{
    cfg.set_key_value("pad_enable", new ConfigOptionBool(choice != RaftChoice::none));
    if (choice != RaftChoice::none)
        cfg.set_key_value("pad_shape", new ConfigOptionEnum<PadShape>(
                                           choice == RaftChoice::grid ? PadShape::grid : PadShape::solid));
}

std::vector<std::string> copy_tuning_to_profile(const DynamicPrintConfig &object_cfg,
                                                DynamicPrintConfig &profile,
                                                const std::string &prefix)
{
    std::vector<std::string> copied;
    for (const std::string &key : support_tuning_keys(prefix))
        if (const ConfigOption *opt = object_cfg.option(key)) {
            profile.set_key_value(key, opt->clone());
            copied.push_back(key);
        }
    return copied;
}

}} // namespace Slic3r::sla
