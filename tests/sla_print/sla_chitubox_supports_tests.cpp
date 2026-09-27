#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <set>
#include <string>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLA/SupportSizes.hpp"

using namespace Slic3r;
using Catch::Approx;

namespace {

DynamicPrintConfig sla_print_defaults()
{
    DynamicPrintConfig cfg;
    cfg.apply(SLAFullPrintConfig{});
    return cfg;
}

} // namespace

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

TEST_CASE("apply_support_size writes only the chosen tree's keys", "[chitubox]") {
    for (const std::string prefix : {"", "branching"}) {
        const std::string other = prefix.empty() ? "branching" : "";
        DynamicPrintConfig cfg = sla_print_defaults();
        const DynamicPrintConfig before = cfg;

        sla::apply_support_size(cfg, sla::SupportSize::heavy, prefix);

        CHECK(cfg.opt_float(prefix + "support_head_front_diameter") == Approx(0.6));
        CHECK(cfg.opt_float(prefix + "support_head_penetration") == Approx(0.6));
        CHECK(cfg.opt_float(prefix + "support_head_width") == Approx(3.0));
        CHECK(cfg.opt_float(prefix + "support_pillar_diameter") == Approx(1.5));
        CHECK(cfg.opt_float(prefix + "support_base_diameter") == Approx(5.0));
        CHECK(cfg.opt_float(prefix + "support_base_height") == Approx(1.0));

        for (const char *key : {"support_head_front_diameter", "support_head_penetration",
                                "support_head_width", "support_pillar_diameter",
                                "support_base_diameter", "support_base_height"})
            CHECK(cfg.opt_float(other + key) == before.opt_float(other + key));
    }
}

TEST_CASE("match_support_size recognises each size and reports custom values", "[chitubox]") {
    for (auto size : {sla::SupportSize::light, sla::SupportSize::medium, sla::SupportSize::heavy}) {
        DynamicPrintConfig cfg = sla_print_defaults();
        sla::apply_support_size(cfg, size, "");
        REQUIRE(sla::match_support_size(cfg, "") == size);

        cfg.set_key_value("support_pillar_diameter", new ConfigOptionFloat(1.35));
        CHECK(!sla::match_support_size(cfg, ""));
    }

    // PrusaSlicer's own defaults are not one of the CHITUBOX sizes.
    CHECK(!sla::match_support_size(sla_print_defaults(), ""));
}

// ---------------------------------------------------------------------------
// Tip, pillar and foot shapes
// ---------------------------------------------------------------------------

#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/FileReader.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/SLA/SupportTreeMesher.hpp"
#include "libslic3r/SLA/SupportTreeStrategies.hpp"
#include "sla_test_utils.hpp"

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

} // namespace

TEST_CASE("Cone tip is as long as the ball tip and ends in a flat point", "[chitubox]") {
    const double r_pin = 0.2, r_back = 0.6, length = 2.0;
    indexed_triangle_set ball = sla::pinhead(r_pin, r_back, length);
    indexed_triangle_set cone = sla::pinhead_cone(r_pin, r_back, length);

    BoundingBoxf3 bb_ball = bounding_box(ball), bb_cone = bounding_box(cone);
    CHECK(bb_cone.max.z() == Approx(bb_ball.max.z()).margin(1e-5));
    CHECK(bb_cone.min.z() == Approx(bb_ball.min.z()).margin(1e-5));
    CHECK(max_radius_at_top(cone) == Approx(r_pin).margin(1e-5));

    its_merge_vertices(cone);
    CHECK(its_num_open_edges(cone) == 0);
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

    BoundingBoxf3 bb = bounding_box(square_mesh);
    CHECK(bb.size().x() == Approx(2 * r).margin(1e-5));
    CHECK(bb.size().y() == Approx(2 * r).margin(1e-5));

    its_merge_vertices(square_mesh);
    CHECK(its_num_open_edges(square_mesh) == 0);
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
    SLAPrintObjectConfig c;
    c.support_head_shape.value = sla::HeadShape::cone;
    c.support_pillar_shape.value = sla::PillarShape::square;
    c.support_base_shape.value = sla::BaseShape::cylinder;

    sla::SupportTreeConfig scfg = make_support_cfg(c);
    CHECK(scfg.shapes.head == sla::HeadShape::cone);
    CHECK(scfg.shapes.pillar == sla::PillarShape::square);
    CHECK(scfg.shapes.base == sla::BaseShape::cylinder);

    // The branching tree reads its own twins, which are still the defaults.
    c.support_tree_type.value = sla::SupportTreeType::Branching;
    scfg = make_support_cfg(c);
    CHECK(scfg.shapes.head == sla::HeadShape::ball);
    CHECK(scfg.shapes.pillar == sla::PillarShape::round);
    CHECK(scfg.shapes.base == sla::BaseShape::cone);

    c.branchingsupport_head_shape.value = sla::HeadShape::cone;
    CHECK(make_support_cfg(c).shapes.head == sla::HeadShape::cone);
}

TEST_CASE("Changing a support shape regenerates only the support tree", "[chitubox]") {
    SLAPrint print;
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    Model model = FileReader::load_model(TEST_DATA_DIR PATH_SEPARATOR + std::string("20mm_cube.obj"));

    SLAFullPrintConfig fullcfg;
    fullcfg.printer_technology.setInt(ptSLA);
    fullcfg.pad_enable.value = false;
    DynamicPrintConfig cfg;
    cfg.apply(fullcfg);

    print.apply(model, cfg);
    print.process();
    REQUIRE(print.is_step_done(slaposSupportTree));

    for (const char *key : {"support_head_shape", "support_pillar_shape", "support_base_shape",
                            "branchingsupport_head_shape", "branchingsupport_pillar_shape",
                            "branchingsupport_base_shape"}) {
        DynamicPrintConfig changed = cfg;
        changed.set_deserialize_strict(key, std::string(key).find("base") != std::string::npos ?
                                                "cylinder" :
                                            std::string(key).find("head") != std::string::npos ?
                                                "cone" : "square");
        print.apply(model, changed);
        CHECK(print.is_step_done(slaposSupportPoints));
        CHECK(!print.is_step_done(slaposSupportTree));
        print.apply(model, cfg);
        print.process();
    }
}

// ---------------------------------------------------------------------------
// Raft styles
// ---------------------------------------------------------------------------

#include "libslic3r/SLA/Pad.hpp"
#include "libslic3r/ClipperUtils.hpp"

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

// 1 mm feet on a 10 mm grid, like pillar bases standing on the pad.
ExPolygons support_feet()
{
    ExPolygons feet;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            Polygon sq{{0, 0}, {scaled(1.), 0}, {scaled(1.), scaled(1.)}, {0, scaled(1.)}};
            sq.translate(scaled(10. * i), scaled(10. * j));
            feet.emplace_back(sq);
        }
    return feet;
}

} // namespace

TEST_CASE("Cross grid pad geometry is valid", "[chitubox]") {
    for (auto fname : {"20mm_cube.obj", "V.obj"})
        test_pad(fname, grid_pad_cfg());
}

TEST_CASE("Cross grid pad has openings, uses less resin and holds every foot", "[chitubox]") {
    sla::PadConfig solid = grid_pad_cfg();
    solid.shape = sla::PadShape::solid;

    const ExPolygons feet = support_feet();
    indexed_triangle_set solid_its, grid_its;
    sla::create_pad(feet, {}, solid_its, solid);
    sla::create_pad(feet, {}, grid_its, grid_pad_cfg());
    REQUIRE(!solid_its.empty());
    REQUIRE(!grid_its.empty());

    CHECK(its_volume(grid_its) < its_volume(solid_its));
    CHECK(bounding_box(grid_its).size().z() == Approx(bounding_box(solid_its).size().z()));

    const ExPolygons top = top_of_pad(grid_its);
    size_t holes = 0;
    for (const ExPolygon &p : top) holes += p.holes.size();
    CHECK(holes > 0);

    double uncovered = 0.;
    for (const ExPolygon &p : diff_ex(feet, top)) uncovered += p.area();
    CHECK(uncovered * SCALING_FACTOR * SCALING_FACTOR < 1e-3);
}

TEST_CASE("Wide grid openings never isolate or drop a support foot", "[chitubox]") {
    // Feet smaller than an opening: a clipped cell would become a ring
    // around the foot and cut it off from the raft.
    ExPolygons feet;
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 5; ++j) {
            Polygon sq{{0, 0}, {scaled(1.), 0}, {scaled(1.), scaled(1.)}, {0, scaled(1.)}};
            sq.translate(scaled(7.3 * i), scaled(7.3 * j));
            feet.emplace_back(sq);
        }

    for (double slope_deg : {90., 45.}) {
        sla::PadConfig cfg = grid_pad_cfg();
        cfg.grid_opening_mm = 5.;
        cfg.grid_bar_mm     = 1.;
        cfg.wall_slope      = slope_deg * PI / 180.;

        indexed_triangle_set pad;
        sla::create_pad(feet, {}, pad, cfg);
        const ExPolygons top = top_of_pad(pad);

        CHECK(top.size() == 1); // one connected raft

        double uncovered = 0.;
        for (const ExPolygon &p : diff_ex(feet, top)) uncovered += p.area();
        CHECK(uncovered * SCALING_FACTOR * SCALING_FACTOR < 1e-3);
    }
}

TEST_CASE("Solid pad is unchanged by the grid settings", "[chitubox]") {
    sla::PadConfig a = grid_pad_cfg(), b = grid_pad_cfg();
    a.shape = b.shape = sla::PadShape::solid;
    b.grid_opening_mm = 5.;
    b.grid_bar_mm = 0.5;

    indexed_triangle_set pa, pb;
    sla::create_pad(support_feet(), {}, pa, a);
    sla::create_pad(support_feet(), {}, pb, b);
    CHECK(pa.vertices == pb.vertices);
}

TEST_CASE("Raft style options reach the pad config", "[chitubox]") {
    SLAPrintObjectConfig c;
    CHECK(make_pad_cfg(c).shape == sla::PadShape::solid);

    c.pad_shape.value          = sla::PadShape::grid;
    c.pad_grid_opening.value   = 3.;
    c.pad_grid_bar_width.value = 1.5;
    c.pad_wall_height.value    = 2.;

    sla::PadConfig p = make_pad_cfg(c);
    CHECK(p.shape == sla::PadShape::grid);
    CHECK(p.grid_opening_mm == Approx(3.));
    CHECK(p.grid_bar_mm == Approx(1.5));
    CHECK(p.wall_height_mm == 0.); // a cavity makes no sense in a grid

    CHECK(print_config_def.get("pad_brim_size")->mode == comSimple);
    CHECK(print_config_def.get("pad_shape")->mode == comSimple);
}

TEST_CASE("Changing the raft style regenerates only the pad", "[chitubox]") {
    SLAPrint print;
    print.set_status_callback([](const PrintBase::SlicingStatus &) {});
    Model model = FileReader::load_model(TEST_DATA_DIR PATH_SEPARATOR + std::string("20mm_cube.obj"));

    SLAFullPrintConfig fullcfg;
    fullcfg.printer_technology.setInt(ptSLA);
    DynamicPrintConfig cfg;
    cfg.apply(fullcfg);

    print.apply(model, cfg);
    print.process();
    REQUIRE(print.is_step_done(slaposPad));

    for (auto [key, value] : {std::pair{"pad_shape", "grid"}, std::pair{"pad_grid_opening", "3"},
                              std::pair{"pad_grid_bar_width", "1"}}) {
        DynamicPrintConfig changed = cfg;
        changed.set_deserialize_strict(key, value);
        print.apply(model, changed);
        CHECK(print.is_step_done(slaposSupportTree));
        CHECK(!print.is_step_done(slaposPad));
        print.apply(model, cfg);
        print.process();
    }
}

// ---------------------------------------------------------------------------
// Size per support point
// ---------------------------------------------------------------------------

#include <boost/filesystem.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/SLA/DefaultSupportTree.hpp"

namespace {

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

sla::SupportPoints with_size(sla::SupportPoints pts, sla::SupportPointSize size, float head_front_radius)
{
    for (sla::SupportPoint &p : pts) {
        p.size = size;
        p.head_front_radius = head_front_radius;
    }
    return pts;
}

const indexed_triangle_set &build_tree(const TriangleMesh &mesh, const sla::SupportPoints &pts,
                                       const sla::SupportTreeConfig &cfg, sla::SupportTreeBuilder &builder)
{
    sla::SupportableMesh sm{mesh.its, pts, cfg};
    sla::DefaultSupportTree::execute(builder, sm);
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
    TriangleMesh mesh = load_model("A_upsidedown.obj");
    const float tip_r = float(sla::support_size_spec(sla::SupportSize::medium).head_front_diameter / 2.);
    const sla::SupportPoints pts = calc_support_pts(mesh);
    REQUIRE(!pts.empty());

    const sla::SupportTreeConfig cfg = tree_cfg_for(sla::SupportSize::medium);
    sla::SupportTreeBuilder unsized, sized;
    const indexed_triangle_set &a = build_tree(mesh, with_size(pts, sla::SupportPointSize::from_settings, tip_r), cfg, unsized);
    const indexed_triangle_set &b = build_tree(mesh, with_size(pts, sla::SupportPointSize::medium, tip_r), cfg, sized);

    REQUIRE(!a.empty());
    CHECK(sorted_vertices(a) == sorted_vertices(b));
}

TEST_CASE("Heavy points get thicker supports than light points", "[chitubox]") {
    TriangleMesh mesh = load_model("A_upsidedown.obj");
    const sla::SupportPoints pts = calc_support_pts(mesh);
    const sla::SupportTreeConfig cfg = [] { sla::SupportTreeConfig c; c.object_elevation_mm = 5.; return c; }();

    const sla::SupportSizeSpec &l = sla::support_size_spec(sla::SupportSize::light);
    const sla::SupportSizeSpec &h = sla::support_size_spec(sla::SupportSize::heavy);

    sla::SupportTreeBuilder light, heavy;
    build_tree(mesh, with_size(pts, sla::SupportPointSize::light, float(l.head_front_diameter / 2.)), cfg, light);
    build_tree(mesh, with_size(pts, sla::SupportPointSize::heavy, float(h.head_front_diameter / 2.)), cfg, heavy);

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
        TriangleMesh mesh = load_model(fname);
        const sla::SupportPoints pts = calc_support_pts(mesh);

        // Default settings have a thicker pillar (r 0.5) than Light (r 0.4).
        sla::SupportTreeConfig cfg;
        cfg.object_elevation_mm = 5.;

        sla::SupportTreeBuilder builder;
        build_tree(mesh, with_size(pts, sla::SupportPointSize::light, float(l.head_front_diameter / 2.)), cfg, builder);
        const double gnd = sla::ground_level(sla::SupportableMesh{mesh.its, {}, cfg});

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

TEST_CASE("Support point sizes survive a 3mf round trip", "[chitubox]") {
    namespace fs = boost::filesystem;

    auto round_trip = [](const sla::SupportPoints &pts) {
        Model model;
        ModelObject *obj = model.add_object();
        obj->add_volume(load_model("20mm_cube.obj"));
        obj->add_instance();
        obj->sla_support_points = pts;
        obj->sla_points_status  = sla::PointsStatus::UserModified;

        const fs::path file = fs::temp_directory_path() / fs::unique_path("sizes-%%%%-%%%%.3mf");
        REQUIRE(store_3mf(file.string().c_str(), &model, nullptr, false));

        Model loaded;
        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::Disable};
        boost::optional<Semver> version;
        REQUIRE(load_3mf(file.string().c_str(), config, ctxt, &loaded, false, version));
        fs::remove(file);

        REQUIRE(loaded.objects.size() == 1);
        return loaded.objects.front()->sla_support_points;
    };

    const sla::SupportPoints pts = {
        {Vec3f{1.f, 2.f, 3.f}, 0.15f, sla::SupportPointType::manual_add, sla::SupportPointSize::light},
        {Vec3f{4.f, 5.f, 6.f}, 0.30f, sla::SupportPointType::manual_add, sla::SupportPointSize::heavy},
        {Vec3f{7.f, 8.f, 9.f}, 0.20f, sla::SupportPointType::island, sla::SupportPointSize::from_settings},
    };

    const sla::SupportPoints loaded = round_trip(pts);
    REQUIRE(loaded.size() == 3);
    CHECK(loaded[0].size == sla::SupportPointSize::light);
    CHECK(loaded[1].size == sla::SupportPointSize::heavy);
    CHECK(loaded[2].size == sla::SupportPointSize::from_settings);

    // Files without sizes, like the ones upstream PrusaSlicer writes, load
    // as sized from the settings.
    for (const sla::SupportPoint &p : round_trip(with_size(pts, sla::SupportPointSize::from_settings, 0.2f)))
        CHECK(p.size == sla::SupportPointSize::from_settings);
}

// ---------------------------------------------------------------------------
// Support settings in the support points panel
// ---------------------------------------------------------------------------

#include "libslic3r/SLA/SupportTuning.hpp"

TEST_CASE("The panel edits the active tree's keys and the pad keys", "[chitubox]") {
    for (const std::string prefix : {"", "branching"}) {
        const std::vector<std::string> keys = sla::support_tuning_keys(prefix);
        const auto has = [&keys](const std::string &k) { return std::find(keys.begin(), keys.end(), k) != keys.end(); };

        for (const char *k : {"support_head_shape", "support_head_front_diameter", "support_head_penetration",
                              "support_head_width", "support_pillar_diameter", "support_pillar_shape",
                              "support_pillar_connection_mode", "support_max_pillar_link_distance",
                              "support_base_shape", "support_base_diameter", "support_base_height",
                              "support_object_elevation"})
            CHECK(has(prefix + k));

        for (const char *k : {"pad_enable", "pad_shape", "pad_wall_thickness", "pad_brim_size",
                              "pad_grid_opening", "pad_grid_bar_width"}) {
            CHECK(has(k));
            CHECK(!has(std::string("branching") + k));
        }

        // Every key is a real print option.
        for (const std::string &k : keys)
            CHECK(print_config_def.get(k) != nullptr);
    }
}

TEST_CASE("Clamping keeps the tip and penetration valid", "[chitubox]") {
    for (const std::string prefix : {"", "branching"}) {
        DynamicPrintConfig cfg = sla_print_defaults();
        CHECK(!sla::clamp_support_tuning(cfg, prefix)); // defaults are valid

        cfg.set_key_value(prefix + "support_pillar_diameter", new ConfigOptionFloat(0.8));
        cfg.set_key_value(prefix + "support_head_front_diameter", new ConfigOptionFloat(1.2));
        cfg.set_key_value(prefix + "support_head_width", new ConfigOptionFloat(0.5));
        cfg.set_key_value(prefix + "support_head_penetration", new ConfigOptionFloat(0.9));

        CHECK(sla::clamp_support_tuning(cfg, prefix));
        CHECK(cfg.opt_float(prefix + "support_head_front_diameter") == Approx(0.8));
        CHECK(cfg.opt_float(prefix + "support_head_penetration") == Approx(0.5));
        CHECK(cfg.opt_float(prefix + "support_pillar_diameter") == Approx(0.8));
        CHECK(cfg.opt_float(prefix + "support_head_width") == Approx(0.5));
    }
}

TEST_CASE("Raft choice maps to pad enable and pad style", "[chitubox]") {
    DynamicPrintConfig cfg = sla_print_defaults();
    CHECK(sla::raft_choice(cfg) == sla::RaftChoice::skate); // pad on, solid by default

    for (auto choice : {sla::RaftChoice::none, sla::RaftChoice::grid, sla::RaftChoice::skate}) {
        sla::set_raft_choice(cfg, choice);
        CHECK(sla::raft_choice(cfg) == choice);
    }

    sla::set_raft_choice(cfg, sla::RaftChoice::none);
    CHECK(!cfg.opt_bool("pad_enable"));
    sla::set_raft_choice(cfg, sla::RaftChoice::grid);
    CHECK(cfg.opt_bool("pad_enable"));
    CHECK(cfg.opt_enum<sla::PadShape>("pad_shape") == sla::PadShape::grid);
}

TEST_CASE("Saving to the profile copies only the panel keys the object overrides", "[chitubox]") {
    DynamicPrintConfig object_cfg;
    object_cfg.set_key_value("support_pillar_diameter", new ConfigOptionFloat(1.4));
    object_cfg.set_key_value("branchingsupport_pillar_diameter", new ConfigOptionFloat(2.2));
    object_cfg.set_key_value("pad_brim_size", new ConfigOptionFloat(3.));
    object_cfg.set_key_value("support_points_density_relative", new ConfigOptionInt(150)); // not a panel key

    DynamicPrintConfig profile = sla_print_defaults();
    const DynamicPrintConfig before = profile;

    std::vector<std::string> copied = sla::copy_tuning_to_profile(object_cfg, profile, "");
    std::sort(copied.begin(), copied.end());

    CHECK(copied == std::vector<std::string>{"pad_brim_size", "support_pillar_diameter"});
    CHECK(profile.opt_float("support_pillar_diameter") == Approx(1.4));
    CHECK(profile.opt_float("pad_brim_size") == Approx(3.));
    CHECK(profile.opt_float("branchingsupport_pillar_diameter") == before.opt_float("branchingsupport_pillar_diameter"));
    CHECK(profile.opt_int("support_points_density_relative") == before.opt_int("support_points_density_relative"));
}
