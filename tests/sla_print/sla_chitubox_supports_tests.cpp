// CHITUBOX-style supports: sizes, tip/pillar/foot shapes, cross grid pad and
// a size per support point.

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sla_test_utils.hpp"

#include <libslic3r/TriangleMeshSlicer.hpp>
#include <libslic3r/SLA/SupportTreeMesher.hpp>
#include <libslic3r/SLA/SupportTreeStrategies.hpp>
#include <libslic3r/SLA/SupportSizes.hpp>
#include <libslic3r/SLA/DefaultSupportTree.hpp>
#include <libslic3r/ConfigViews.hpp>
#include "Slic3r/Biz/Algorithms/BoundingBox.hpp"
#include "Slic3r/Biz/Algorithms/ClipperUtils.hpp"
#include "Slic3r/Biz/Algorithms/ModelObject.hpp"
#include "Slic3r/Domain/TemplateUtils.hpp"

namespace BB = Slic3r::Biz::Algorithms::BoundingBox;
namespace TM = Slic3r::Biz::Algorithms::TriangleMesh;
using Slic3r::Biz::Algorithms::ClipperUtils::diff_ex;
using Slic3r::Domain::bounding_box;
using Slic3r::Domain::its_volume;

namespace {

double slice_area(const indexed_triangle_set &its, float z)
{
    const std::vector<ExPolygons> slices = slice_mesh_ex(its, {z});
    double area = 0.;
    for (const ExPolygon &p : slices.front())
        area += p.area();
    return area * SCALING_FACTOR * SCALING_FACTOR;
}

// Support trees are built in parallel, so the order of their parts (and of
// the mesh vertices) can differ between runs. Compare sorted vertices.
std::vector<Vec3f> sorted_vertices(const indexed_triangle_set &its)
{
    std::vector<Vec3f> v = its.vertices;
    std::sort(v.begin(), v.end(), [](const Vec3f &a, const Vec3f &b) {
        return std::lexicographical_compare(a.data(), a.data() + 3, b.data(), b.data() + 3);
    });
    return v;
}

double max_radius_at_top(const indexed_triangle_set &its)
{
    float top = its.vertices.front().z();
    for (const auto &v : its.vertices) top = std::max(top, v.z());
    double r = 0.;
    for (const auto &v : its.vertices)
        if (std::abs(v.z() - top) < 1e-5f)
            r = std::max(r, double(v.head<2>().norm()));
    return r;
}

// An SLA object config view over the defaults with some values changed.
SLAPrintObjectConfigView config_with(const std::map<std::string, std::function<void(Domain::ConfigValue &)>> &changes)
{
    Domain::FullConfigSLA full = Domain::FullConfigSLA::defaults();
    for (const auto &[key, change] : changes) {
        Domain::ConfigValue value = full.get_value(key);
        change(value);
        full.set(key, value);
    }
    const auto full_ptr = std::make_shared<const Domain::FullConfigSLA>(full);
    const auto object   = std::make_shared<const Domain::PartialObjectConfigSLA>(Domain::SLAObjectSettings{}, full_ptr->hw_config());
    return SLAPrintObjectConfigView{full_ptr, object};
}

template<class T> std::function<void(Domain::ConfigValue &)> to(const T &v)
{
    return [v](Domain::ConfigValue &value) { value.set(v); };
}

} // namespace

// ---------------------------------------------------------------------------
// Light / Medium / Heavy
// ---------------------------------------------------------------------------

TEST_CASE("Support size table matches CHITUBOX Light/Middle/Heavy", "[chitubox]") {
    const sla::SupportSizeSpec &l = sla::support_size_spec(sla::SupportSize::light);
    const sla::SupportSizeSpec &m = sla::support_size_spec(sla::SupportSize::medium);
    const sla::SupportSizeSpec &h = sla::support_size_spec(sla::SupportSize::heavy);

    CHECK(l.head_front_diameter == Approx(0.3));
    CHECK(m.head_front_diameter == Approx(0.4));
    CHECK(h.head_front_diameter == Approx(0.6));
    CHECK(l.head_penetration == Approx(0.3));
    CHECK(m.head_penetration == Approx(0.4));
    CHECK(h.head_penetration == Approx(0.6));
    CHECK(l.head_width == Approx(2.0));
    CHECK(m.head_width == Approx(2.0));
    CHECK(h.head_width == Approx(3.0));
    CHECK(l.pillar_diameter == Approx(0.8));
    CHECK(m.pillar_diameter == Approx(1.2));
    CHECK(h.pillar_diameter == Approx(1.5));
    CHECK(l.base_diameter == Approx(3.0));
    CHECK(m.base_diameter == Approx(4.0));
    CHECK(h.base_diameter == Approx(5.0));
    CHECK(l.base_height == Approx(0.8));
    CHECK(m.base_height == Approx(1.0));
    CHECK(h.base_height == Approx(1.0));
}

TEST_CASE("A size gives the six support options of the chosen tree", "[chitubox]") {
    for (const std::string prefix : {"", "branching"}) {
        const auto values = sla::support_size_values(sla::SupportSize::heavy, prefix);
        std::map<std::string, double> by_key(values.begin(), values.end());
        REQUIRE(by_key.size() == 6);
        CHECK(by_key.at(prefix + "support_head_front_diameter") == Approx(0.6));
        CHECK(by_key.at(prefix + "support_head_penetration") == Approx(0.6));
        CHECK(by_key.at(prefix + "support_head_width") == Approx(3.0));
        CHECK(by_key.at(prefix + "support_pillar_diameter") == Approx(1.5));
        CHECK(by_key.at(prefix + "support_base_diameter") == Approx(5.0));
        CHECK(by_key.at(prefix + "support_base_height") == Approx(1.0));
        // Every key is a real SLA option.
        for (const auto &[key, value] : values)
            CHECK(std::any_of(Domain::get_defs_sla().defs().begin(), Domain::get_defs_sla().defs().end(),
                              [&key](const Domain::ConfigItemDef &d) { return d.name == key; }));
    }
}

TEST_CASE("match_support_size recognises each size and reports custom values", "[chitubox]") {
    for (auto size : {sla::SupportSize::light, sla::SupportSize::medium, sla::SupportSize::heavy}) {
        std::map<std::string, double> cfg;
        for (const auto &[key, value] : sla::support_size_values(size, ""))
            cfg[key] = value;
        auto get = [&cfg](const std::string &key) { return cfg.at(key); };
        REQUIRE(sla::match_support_size(get, "") == size);

        cfg["support_pillar_diameter"] = 1.35;
        CHECK(!sla::match_support_size(get, ""));
    }

    // PrusaSlicer's own defaults are not one of the CHITUBOX sizes.
    const Domain::FullConfigSLA defaults = Domain::FullConfigSLA::defaults();
    CHECK(!sla::match_support_size([&defaults](const std::string &key) { return defaults.get<double>(key); }, ""));
}

// ---------------------------------------------------------------------------
// Tip, pillar and foot shapes
// ---------------------------------------------------------------------------

TEST_CASE("Cone tip is as long as the ball tip and ends in a flat point", "[chitubox]") {
    const double r_pin = 0.2, r_back = 0.6, length = 2.0;
    indexed_triangle_set ball = sla::pinhead(r_pin, r_back, length);
    indexed_triangle_set cone = sla::pinhead_cone(r_pin, r_back, length);

    const auto bb_ball = bounding_box(ball), bb_cone = bounding_box(cone);
    CHECK(bb_cone.max.z() == Approx(bb_ball.max.z()).margin(1e-5));
    CHECK(bb_cone.min.z() == Approx(bb_ball.min.z()).margin(1e-5));
    CHECK(max_radius_at_top(cone) == Approx(r_pin).margin(1e-5));

    TM::its_merge_vertices(cone);
    CHECK(TM::its_num_open_edges(cone) == 0);
}

TEST_CASE("Square pillars are never thinner than round ones", "[chitubox]") {
    const double r = 0.6, h = 10.;
    sla::Pillar pillar{Vec3d::Zero(), h, r};

    sla::SupportShapes square;
    square.pillar = sla::PillarShape::square;

    indexed_triangle_set round_mesh  = sla::get_mesh(pillar, 45, sla::SupportShapes{});
    indexed_triangle_set square_mesh = sla::get_mesh(pillar, 45, square);

    CHECK(slice_area(square_mesh, 5.f) == Approx(4 * r * r).epsilon(0.01));
    CHECK(slice_area(round_mesh, 5.f) == Approx(PI * r * r).epsilon(0.01));

    const auto bb = bounding_box(square_mesh);
    CHECK(BB::sizes(bb).x() == Approx(2 * r).margin(1e-5));
    CHECK(BB::sizes(bb).y() == Approx(2 * r).margin(1e-5));

    TM::its_merge_vertices(square_mesh);
    CHECK(TM::its_num_open_edges(square_mesh) == 0);
}

TEST_CASE("Cylinder foot keeps its full diameter up to the pillar", "[chitubox]") {
    sla::Pedestal foot{Vec3d::Zero(), 1.0, 2.0, 0.5};

    sla::SupportShapes cylinder;
    cylinder.base = sla::BaseShape::cylinder;

    indexed_triangle_set cone_mesh = sla::get_mesh(foot, 45, sla::SupportShapes{});
    indexed_triangle_set cyl_mesh  = sla::get_mesh(foot, 45, cylinder);

    CHECK(slice_area(cyl_mesh, 0.95f) == Approx(PI * 2.0 * 2.0).epsilon(0.01));
    CHECK(slice_area(cone_mesh, 0.95f) < PI * 1.0 * 1.0);
}

TEST_CASE("Default shapes give the same meshes as before", "[chitubox]") {
    sla::Head head{0.6, 0.2, 1.0, 0.2, Vec3d{0.3, 0.1, -1.}.normalized(), Vec3d{1., 2., 3.}};
    sla::Pillar pillar{Vec3d::Zero(), 10., 0.6};
    sla::Pedestal foot{Vec3d::Zero(), 1.0, 2.0, 0.6};
    const sla::SupportShapes defaults;

    CHECK(sla::get_mesh(head, 45, defaults).vertices == sla::get_mesh(head, 45).vertices);
    CHECK(sla::get_mesh(pillar, 45, defaults).vertices == sla::get_mesh(pillar, 45).vertices);
    CHECK(sla::get_mesh(foot, 45, defaults).vertices == sla::get_mesh(foot, 45).vertices);
}

TEST_CASE("Every shape combination builds a support tree", "[chitubox]") {
    auto build = [](const sla::SupportShapes &shapes) {
        sla::SupportTreeConfig cfg;
        cfg.object_elevation_mm = 5.;
        cfg.shapes = shapes;
        SupportByproducts bp;
        test_supports("A_upsidedown.obj", cfg, bp);
        return bp.suptree_builder.retrieve_mesh(sla::MeshType::Support);
    };

    const indexed_triangle_set defaults = build({});
    REQUIRE(!defaults.empty());

    for (auto head : {sla::HeadShape::ball, sla::HeadShape::cone})
        for (auto pillar : {sla::PillarShape::round, sla::PillarShape::square})
            for (auto base : {sla::BaseShape::cone, sla::BaseShape::cylinder}) {
                const indexed_triangle_set mesh = build({head, pillar, base});
                REQUIRE(!mesh.empty());

                // The shapes change the mesh, the default shapes don't.
                const bool is_default = head == sla::HeadShape::ball &&
                                        pillar == sla::PillarShape::round &&
                                        base == sla::BaseShape::cone;
                CHECK((sorted_vertices(mesh) == sorted_vertices(defaults)) == is_default);
            }
}

TEST_CASE("Shape options reach the support tree config for both tree types", "[chitubox]") {
    sla::SupportTreeConfig scfg = make_support_cfg(config_with({
        {"support_head_shape", to(Domain::sla::HeadShape::cone)},
        {"support_pillar_shape", to(Domain::sla::PillarShape::square)},
        {"support_base_shape", to(Domain::sla::BaseShape::cylinder)},
    }));
    CHECK(scfg.shapes.head == sla::HeadShape::cone);
    CHECK(scfg.shapes.pillar == sla::PillarShape::square);
    CHECK(scfg.shapes.base == sla::BaseShape::cylinder);

    // The branching tree reads its own twins.
    scfg = make_support_cfg(config_with({
        {"support_tree_type", to(Domain::sla::SupportTreeType::Branching)},
        {"support_head_shape", to(Domain::sla::HeadShape::cone)},
    }));
    CHECK(scfg.shapes.head == sla::HeadShape::ball);

    scfg = make_support_cfg(config_with({
        {"support_tree_type", to(Domain::sla::SupportTreeType::Branching)},
        {"branchingsupport_head_shape", to(Domain::sla::HeadShape::cone)},
    }));
    CHECK(scfg.shapes.head == sla::HeadShape::cone);
}

// ---------------------------------------------------------------------------
// Raft styles
// ---------------------------------------------------------------------------

namespace {

sla::PadConfig grid_pad_cfg()
{
    sla::PadConfig cfg;
    cfg.wall_height_mm  = 0.;
    cfg.shape           = sla::PadShape::grid;
    cfg.grid_opening_mm = 2.;
    cfg.grid_bar_mm     = 2.;
    return cfg;
}

ExPolygons top_of_pad(const indexed_triangle_set &pad)
{
    const std::vector<ExPolygons> slices = slice_mesh_ex(pad, {float(bounding_box(pad).max.z() - 0.05)});
    return slices.front();
}

// Square feet of `size` mm on a grid with `pitch` mm, like pillar bases.
ExPolygons support_feet(double size, double pitch)
{
    ExPolygons feet;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            Polygon sq{{0, 0}, {scaled(size), 0}, {scaled(size), scaled(size)}, {0, scaled(size)}};
            sq.translate(scaled(pitch * i), scaled(pitch * j));
            feet.emplace_back(sq);
        }
    return feet;
}

double uncovered_area(const ExPolygons &feet, const ExPolygons &top)
{
    double uncovered = 0.;
    for (const ExPolygon &p : diff_ex(feet, top)) uncovered += p.area();
    return uncovered * SCALING_FACTOR * SCALING_FACTOR;
}

} // namespace

TEST_CASE("Cross grid pad geometry is valid", "[chitubox]") {
    for (auto fname : {"20mm_cube.obj", "V.obj"})
        test_pad(fname, grid_pad_cfg());
}

TEST_CASE("Cross grid pad has openings, uses less resin and holds every foot", "[chitubox]") {
    sla::PadConfig solid = grid_pad_cfg();
    solid.shape = sla::PadShape::solid;

    const ExPolygons feet = support_feet(1., 10.);
    indexed_triangle_set solid_its, grid_its;
    sla::create_pad(feet, {}, solid_its, solid);
    sla::create_pad(feet, {}, grid_its, grid_pad_cfg());
    REQUIRE(!solid_its.empty());
    REQUIRE(!grid_its.empty());

    CHECK(its_volume(grid_its) < its_volume(solid_its));
    CHECK(BB::sizes(bounding_box(grid_its)).z() == Approx(BB::sizes(bounding_box(solid_its)).z()));

    const ExPolygons top = top_of_pad(grid_its);
    size_t holes = 0;
    for (const ExPolygon &p : top) holes += p.holes.size();
    CHECK(holes > 0);
    CHECK(uncovered_area(feet, top) < 1e-3);
}

TEST_CASE("Wide grid openings never isolate or drop a support foot", "[chitubox]") {
    // Feet smaller than an opening: a clipped cell would become a ring
    // around the foot and cut it off from the raft.
    const ExPolygons feet = support_feet(1., 7.3);
    for (double slope_deg : {90., 45.}) {
        sla::PadConfig cfg = grid_pad_cfg();
        cfg.grid_opening_mm = 5.;
        cfg.grid_bar_mm     = 1.;
        cfg.wall_slope      = slope_deg * PI / 180.;

        indexed_triangle_set pad;
        sla::create_pad(feet, {}, pad, cfg);
        const ExPolygons top = top_of_pad(pad);

        CHECK(top.size() == 1); // one connected raft
        CHECK(uncovered_area(feet, top) < 1e-3);
    }
}

TEST_CASE("Solid pad is unchanged by the grid settings", "[chitubox]") {
    sla::PadConfig a = grid_pad_cfg(), b = grid_pad_cfg();
    a.shape = b.shape = sla::PadShape::solid;
    b.grid_opening_mm = 5.;
    b.grid_bar_mm = 0.5;

    indexed_triangle_set pa, pb;
    sla::create_pad(support_feet(1., 10.), {}, pa, a);
    sla::create_pad(support_feet(1., 10.), {}, pb, b);
    CHECK(pa.vertices == pb.vertices);
}

TEST_CASE("Raft style options reach the pad config", "[chitubox]") {
    CHECK(make_pad_cfg(config_with({})).shape == sla::PadShape::solid);

    const sla::PadConfig p = make_pad_cfg(config_with({
        {"pad_shape", to(Domain::sla::PadShape::grid)},
        {"pad_grid_opening", to(3.)},
        {"pad_grid_bar_width", to(1.5)},
        {"pad_wall_height", to(2.)},
    }));
    CHECK(p.shape == sla::PadShape::grid);
    CHECK(p.grid_opening_mm == Approx(3.));
    CHECK(p.grid_bar_mm == Approx(1.5));
    CHECK(p.wall_height_mm == 0.); // a cavity makes no sense in a grid
}

// ---------------------------------------------------------------------------
// Size per support point
// ---------------------------------------------------------------------------

namespace {

using Domain::SLA::SupportPoints;
using Domain::SLA::SupportPointSize;

// A support tree config holding the values of one of the sizes.
sla::SupportTreeConfig tree_cfg_for(sla::SupportSize size)
{
    const sla::SupportSizeSpec &s = sla::support_size_spec(size);
    sla::SupportTreeConfig cfg;
    cfg.object_elevation_mm      = 5.;
    cfg.head_front_radius_mm     = s.head_front_diameter / 2.;
    cfg.head_penetration_mm      = s.head_penetration;
    cfg.head_width_mm            = s.head_width;
    cfg.head_back_radius_mm      = s.pillar_diameter / 2.;
    cfg.head_fallback_radius_mm  = 0.5 * cfg.head_back_radius_mm;
    cfg.base_radius_mm           = s.base_diameter / 2.;
    cfg.base_height_mm           = s.base_height;
    return cfg;
}

SupportPoints with_size(SupportPoints pts, SupportPointSize size, float head_front_radius)
{
    for (auto &p : pts) {
        p.size = size;
        p.head_front_radius = head_front_radius;
    }
    return pts;
}

sla::SupportableMesh supportable(const Domain::TriangleMesh &mesh, const SupportPoints &pts, const sla::SupportTreeConfig &cfg)
{
    return sla::SupportableMesh{.emesh = AABBMesh(mesh.its),
                                .pts = std::make_shared<const SupportPoints>(pts),
                                .cfg = cfg};
}

const indexed_triangle_set &build_tree(const Domain::TriangleMesh &mesh, const SupportPoints &pts,
                                       const sla::SupportTreeConfig &cfg, sla::SupportTreeBuilder &builder)
{
    sla::DefaultSupportTree::execute(builder, supportable(mesh, pts, cfg));
    return builder.retrieve_mesh(sla::MeshType::Support);
}

double max_pillar_radius(const sla::SupportTreeBuilder &b)
{
    double r = 0.;
    for (const sla::Pillar &p : b.pillars()) r = std::max(r, p.r_start);
    return r;
}

std::set<double> head_back_radii(const sla::SupportTreeBuilder &b)
{
    std::set<double> radii;
    for (const sla::Head &h : b.heads())
        if (h.is_valid()) radii.insert(h.r_back_mm);
    return radii;
}

} // namespace

TEST_CASE("Points sized like the preset give the same supports as unsized points", "[chitubox]") {
    Domain::TriangleMesh mesh = load_model("A_upsidedown.obj");
    const float tip_r = float(sla::support_size_spec(sla::SupportSize::medium).head_front_diameter / 2.);
    const SupportPoints pts = calc_support_pts(mesh);
    REQUIRE(!pts.empty());

    const sla::SupportTreeConfig cfg = tree_cfg_for(sla::SupportSize::medium);
    sla::SupportTreeBuilder unsized, sized;
    const indexed_triangle_set &a = build_tree(mesh, with_size(pts, SupportPointSize::from_settings, tip_r), cfg, unsized);
    const indexed_triangle_set &b = build_tree(mesh, with_size(pts, SupportPointSize::medium, tip_r), cfg, sized);

    REQUIRE(!a.empty());
    CHECK(sorted_vertices(a) == sorted_vertices(b));
}

TEST_CASE("Heavy points get thicker supports than light points", "[chitubox]") {
    Domain::TriangleMesh mesh = load_model("A_upsidedown.obj");
    const SupportPoints pts = calc_support_pts(mesh);
    sla::SupportTreeConfig cfg;
    cfg.object_elevation_mm = 5.;

    const sla::SupportSizeSpec &l = sla::support_size_spec(sla::SupportSize::light);
    const sla::SupportSizeSpec &h = sla::support_size_spec(sla::SupportSize::heavy);

    sla::SupportTreeBuilder light, heavy;
    build_tree(mesh, with_size(pts, SupportPointSize::light, float(l.head_front_diameter / 2.)), cfg, light);
    build_tree(mesh, with_size(pts, SupportPointSize::heavy, float(h.head_front_diameter / 2.)), cfg, heavy);

    // Each head is either full size or the preset's small fallback pillar.
    const double fallback = cfg.head_fallback_radius_mm;
    for (double r : head_back_radii(light)) CHECK((r == Approx(l.pillar_diameter / 2.) || r == Approx(fallback)));
    for (double r : head_back_radii(heavy)) CHECK((r == Approx(h.pillar_diameter / 2.) || r == Approx(fallback)));
    CHECK(head_back_radii(heavy).count(h.pillar_diameter / 2.) == 1);

    CHECK(max_pillar_radius(heavy) == Approx(h.pillar_diameter / 2.));
    CHECK(max_pillar_radius(heavy) > max_pillar_radius(light));
}

TEST_CASE("Points smaller than the preset still stand on a foot", "[chitubox]") {
    const sla::SupportSizeSpec &l = sla::support_size_spec(sla::SupportSize::light);
    const double light_r = l.pillar_diameter / 2.;

    for (auto fname : {"20mm_cube.obj", "A_upsidedown.obj"}) {
        Domain::TriangleMesh mesh = load_model(fname);
        const SupportPoints pts = with_size(calc_support_pts(mesh), SupportPointSize::light, float(l.head_front_diameter / 2.));

        // Default settings have a thicker pillar (r 0.5) than Light (r 0.4).
        sla::SupportTreeConfig cfg;
        cfg.object_elevation_mm = 5.;

        sla::SupportTreeBuilder builder;
        build_tree(mesh, pts, cfg, builder);
        const double gnd = sla::ground_level(supportable(mesh, pts, cfg));

        // Full-size Light pillars reach the ground level and get a foot.
        // Only the preset's small fallback pillars may sink into the pad.
        size_t full_on_ground = 0, full_sunk = 0;
        for (const sla::Pillar &p : builder.pillars()) {
            if (p.r_start < light_r - EPSILON || p.endpt.z() > gnd + EPSILON)
                continue;
            ++full_on_ground;
            if (p.endpt.z() < gnd - EPSILON)
                ++full_sunk;
        }
        CHECK(full_on_ground > 0);
        CHECK(full_sunk == 0);
        CHECK(builder.pedestals().size() >= full_on_ground);
    }
}

// ---------------------------------------------------------------------------
// Re-slicing
// ---------------------------------------------------------------------------

TEST_CASE("Changing a new support option re-runs its step", "[chitubox]") {
    Domain::Model model;
    Domain::ModelObject *object = model.add_object();
    Biz::Algorithms::ModelObject::add_volume(object, Biz::Algorithms::TriangleMesh::make_cube(10., 10., 10.));
    object->add_instance();

    SLAPrint print{[](auto &&) {}, [](const auto &) {}};
    const Domain::Preset::SelectedPresetMetadata metadata{.hw_config = Domain::FullConfigSLA::defaults().hw_config()};
    const Biz::Slicing::IPrint::MetadataSerializeFn serializer = [](const auto &) { return Biz::Slicing::SerializedConfig{}; };
    Domain::ConfigPackSLA pack;
    print.apply(model, pack, metadata, serializer);

    const std::map<std::string, SLAPrintObjectStep> expected{
        {"support_head_shape", slaposSupportTree},
        {"support_pillar_shape", slaposSupportTree},
        {"support_base_shape", slaposSupportTree},
        {"branchingsupport_head_shape", slaposSupportTree},
        {"branchingsupport_pillar_shape", slaposSupportTree},
        {"branchingsupport_base_shape", slaposSupportTree},
        {"pad_shape", slaposPad},
        {"pad_grid_opening", slaposPad},
        {"pad_grid_bar_width", slaposPad},
    };
    for (const auto &[key, step] : expected) {
        INFO(key);
        const Domain::FindResult found = pack.contains(key);
        REQUIRE(found.item != nullptr);
        found.item->visit(Domain::overloaded{
            [](Domain::EnumWrapper &v) { v.set_index(v.index_of_value(v.value()) == 0 ? 1 : 0); },
            [](double &v) { v += 0.5; },
            [](auto &) { FAIL("unexpected option type"); },
        });

        const SLASlicingSync::InvalidatedSteps steps = print.apply(model, pack, metadata, serializer);
        REQUIRE(steps.object.size() == 1);
        const auto &object_steps = std::get<SLASlicingSync::PrintObjectSteps>(steps.object.begin()->second);
        CHECK(object_steps.count(step) == 1);
    }
}
