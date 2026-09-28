#include <catch2/catch_test_macros.hpp>

#include "Slic3r/App/Config/CategoryUtils.hpp"
#include "Slic3r/Domain/ConfigDefsSLA.hpp"

#include <string>
#include <vector>

using Slic3r::App::CategoryUtils::category_render_icon;
using Slic3r::Domain::ConfigItemDef;
using Slic3r::Domain::PrinterTechnology;

TEST_CASE("Every category of the SLA options has a settings icon mapping", "[CategoryUtils]")
{
    // The settings dialogs ask for the icon of every category they list, and an
    // unknown category aborts the app.
    for (const ConfigItemDef& def : Slic3r::Domain::get_defs_sla().defs()) {
        if (def.category == ConfigItemDef::Category::Unknown || def.category == ConfigItemDef::Category::Hidden)
            continue;
        INFO(def.name);
        category_render_icon(def.category, PrinterTechnology::SLA);
    }
    SUCCEED();
}

TEST_CASE("Every SLA option an object can override has an option group", "[CategoryUtils]")
{
    // The object override dialog groups its rows by option group and aborts on
    // an unknown one.
    std::vector<std::string> without_group;
    for (const ConfigItemDef& def : Slic3r::Domain::get_defs_sla().defs()) {
        if (def.overrides_in.contains(Slic3r::Domain::SLAConfigLocation::Object)
            && def.option_group == ConfigItemDef::OptionGroup::Unknown)
            without_group.push_back(def.name);
    }
    CHECK(without_group == std::vector<std::string>{});
}
