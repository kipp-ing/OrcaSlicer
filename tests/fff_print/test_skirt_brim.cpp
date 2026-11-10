#include <catch2/catch_test_macros.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Model.hpp"

#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string_view>

#include "test_data.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

/// Helper method to find the tool used for the brim (always the first extrusion)
static int get_brim_tool(const std::string &gcode)
{
    int brim_tool	= -1;
    int tool		= -1;
	GCodeReader parser;
    parser.parse_buffer(gcode, [&tool, &brim_tool] (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line)
    {
        // if the command is a T command, set the the current tool
        if (boost::starts_with(line.cmd(), "T")) {
            tool = atoi(line.cmd().data() + 1);
        } else if (line.cmd() == "G1" && line.extruding(self) && line.dist_XY(self) > 0 && brim_tool < 0) {
            brim_tool = tool;
        }
    });
    return brim_tool;
}

TEST_CASE("Skirt height is honored", "[Skirt]") {
    DynamicPrintConfig config = Slic3r::Test::default_print_config();
    config.set_deserialize_strict({
        { "skirt_loops",            1 },
        { "skirt_height",           5 },
        { "wall_loops",             0 },
        { "support_speed",          99 },
		// avoid altering speeds unexpectedly
        { "slow_down_for_layer_cooling", false },
        { "initial_layer_speed",    100 },
        { "gcode_comments",         true }
    });

	std::string gcode;
    SECTION("printing a single object") {
        gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
    }
    SECTION("printing multiple objects") {
        gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, config);
    }

    std::map<double, bool> layers_with_skirt;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&layers_with_skirt] (Slic3r::GCodeReader &self, const Slic3r::GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && line.comment().find("skirt") != std::string_view::npos) {
            layers_with_skirt[self.z()] = 1;
        }
    });
    REQUIRE(layers_with_skirt.size() == (size_t)config.opt_int("skirt_height"));
}

SCENARIO("Original Slic3r Skirt/Brim tests", "[SkirtBrim]") {
    GIVEN("A default configuration") {
	    DynamicPrintConfig config = Slic3r::Test::default_print_config();
		config.set_num_extruders(4);
		config.set_deserialize_strict({
			{ "support_speed", 				99 },
			{ "initial_layer_print_height", 	0.3 },
        	{ "gcode_comments", 				true },
        	// avoid altering speeds unexpectedly
			{ "slow_down_for_layer_cooling", false },
			{ "initial_layer_speed", 		100 },
        	// remove noise from top/solid layers
			{ "top_shell_layers", 			0 },
			{ "bottom_shell_layers", 		1 },
			{ "machine_start_gcode",			"T[initial_tool]\n" }
        });

        WHEN("Brim width is set to 5") {
        	config.set_deserialize_strict({
				{ "wall_loops", 		0 },
				{ "skirt_loops", 		0 },
				{ "brim_type", 			"outer_only" },
				{ "brim_width", 		5 }
			});
			THEN("Brim is generated") {
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                bool brim_generated = false;
			    Slic3r::GCodeReader parser;
                parser.parse_buffer(gcode, [&brim_generated] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line) {
                    if (std::abs(self.z() - 0.3) < 1e-6 || std::abs(line.new_Z(self) - 0.3) < 1e-6) {
                        if (line.extruding(self) && line.comment().find("brim") != std::string_view::npos) {
                            brim_generated = true;
                        }
                    }
                });
                REQUIRE(brim_generated);
            }
        }

        WHEN("Skirt area is smaller than the brim") {
            config.set_deserialize_strict({
				{ "skirt_loops", 1 },
				{ "brim_type", 	"outer_only" },
            	{ "brim_width", 10}
            });
            THEN("Gcode generates") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }

        WHEN("Skirt height is 0 and skirt_loops > 0") {
            config.set_deserialize_strict({
				{ "skirt_loops",  2 },
            	{ "skirt_height", 0 }
            });
            THEN("Gcode generates") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }

#if 0
		// This is a real error! One shall print the brim with the external perimeter extruder!
        WHEN("Perimeter extruder = 2 and support extruders = 3") {
            THEN("Brim is printed with the extruder used for the perimeters of first object") {
				config.set_deserialize_strict({
					{ "skirts", 					0 },
					{ "brim_width", 				5 },
					{ "perimeter_extruder", 		2 },
					{ "support_material_extruder", 	3 },
					{ "infill_extruder", 			4 }
				});
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                int tool = get_brim_tool(gcode);
                REQUIRE(tool == config.opt_int("perimeter_extruder") - 1);
            }
        }
        WHEN("Perimeter extruder = 2, support extruders = 3, raft is enabled") {
            THEN("brim is printed with same extruder as skirt") {
				config.set_deserialize_strict({
					{ "skirts",						0 },
					{ "brim_width", 				5 },
					{ "perimeter_extruder", 		2 },
					{ "support_material_extruder", 	3 },
					{ "infill_extruder", 			4 },
					{ "raft_layers", 				1 }
				});
		        std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                int tool = get_brim_tool(gcode);
                REQUIRE(tool == config.opt_int("support_material_extruder") - 1);
            }
        }
#endif

        WHEN("brim width to 1 with layer_width of 0.5") {
        	config.set_deserialize_strict({
				{ "skirt_loops", 					0 },
				{ "brim_type", 						"outer_only" },
				{ "initial_layer_line_width", 	0.5 },
				{ "brim_width", 					1 }
        	});			
            THEN("2 brim lines") {
		        Slic3r::Print print;
		        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, config);
                size_t total_entities = 0;
                for (const auto& pair : print.get_brimMap()) {
                    total_entities += pair.second.entities.size();
                }
                REQUIRE(total_entities == 2);
            }
        }

#if 0
        WHEN("brim ears on a square") {
			config.set_deserialize_strict({
				{ "skirts",							0 },
				{ "initial_layer_line_width",	0.5 },
				{ "brim_width",						1 },
				{ "brim_ears",						1 },
				{ "brim_ears_max_angle",			91 }
			});
	        Slic3r::Print print;
	        Slic3r::Test::init_and_process_print({TestMesh::cube_20x20x20}, print, config);
            THEN("Four brim ears") {
                REQUIRE(print.brim().entities.size() == 4);
            }
        }

        WHEN("brim ears on a square but with a too small max angle") {
			config.set_deserialize_strict({
				{ "skirts",							0 },
				{ "initial_layer_line_width",	0.5 },
				{ "brim_width",						1 },
				{ "brim_ears",						1 },
				{ "brim_ears_max_angle",			89 }
				});
            THEN("no brim") {
		        Slic3r::Print print;
                Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, config);
                REQUIRE(print.brim().entities.size() == 0);
            }
        }
#endif

        WHEN("Object is plated with overhang support and a brim") {
        	config.set_deserialize_strict({
	            { "layer_height", 				0.4 },
	            { "initial_layer_print_height", 0.4 },
	            { "skirt_loops", 				1 },
	            { "skirt_distance", 			0 },
	            { "support_speed", 			99 },
	            { "perimeter_extruder", 		1 },
	            { "support_material_extruder", 	2 },
	            { "infill_extruder", 			3 },			// ensure that a tool command gets emitted.
	            { "slow_down_for_layer_cooling", false },		// to prevent speeds to be altered
	            { "initial_layer_speed", 		100 },			// to prevent speeds to be altered
				{ "machine_start_gcode",		"T[initial_tool]\n" }
        	});

            THEN("overhang generates?") {
            	//FIXME does it make sense?
                REQUIRE(! Slic3r::Test::slice({TestMesh::overhang}, config).empty());
            }

            // config.set("support_material", true);      // to prevent speeds to be altered

#if 0
			// This test is not finished.
            THEN("skirt length is large enough to contain object with support") {
                CHECK(config.opt_bool("support_material")); // test is not valid if support material is off
				std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                double support_speed = config.opt<ConfigOptionFloat>("support_material_speed")->value * MM_PER_MIN;
				double skirt_length = 0.0;
				Points extrusion_points;
				int tool = -1;
				GCodeReader parser;
                parser.parse_buffer(gcode, [config, &extrusion_points, &tool, &skirt_length, support_speed] (Slic3r::GCodeReader& self, const Slic3r::GCodeReader::GCodeLine& line) {
                    // std::cerr << line.cmd() << "\n";
					if (boost::starts_with(line.cmd(), "T")) {
						tool = atoi(line.cmd().data() + 1);
					} else if (std::abs(self.z() - config.opt<ConfigOptionFloat>("first_layer_height")->value) < 1e-6) {
                        // on first layer
						if (line.extruding(self) && line.dist_XY(self) > 0) {
                            float speed = ( self.f() > 0 ?  self.f() : line.new_F(self));
                            // std::cerr << "Tool " << tool << "\n";
                            if (std::abs(speed - support_speed) < 1e-6 && tool == config.opt_int("perimeter_extruder") - 1) {
                                // Skirt uses first material extruder, support material speed.
                                skirt_length += line.dist_XY(self);
                            } else
                                extrusion_points.push_back(Slic3r::Point::new_scale(line.new_X(self), line.new_Y(self)));
                        }
                    }
                    if (std::abs(self.z() - 0.3) < 1e-6 || std::abs(line.new_Z(self) - 0.3) < 1e-6) {
                        if (line.extruding(self) && std::abs(self.f() - support_speed) < 1e-6) {
                        }
                    }
                });
                Slic3r::Polygon convex_hull = Slic3r::Geometry::convex_hull(extrusion_points);
                double hull_perimeter = unscale<double>(convex_hull.split_at_first_point().length());
                REQUIRE(skirt_length > hull_perimeter);
            }
#endif

        }
        WHEN("Large minimum skirt length is used.") {
            config.set("min_skirt_length", 20);
            THEN("Gcode generation doesn't crash") {
                REQUIRE(! Slic3r::Test::slice({TestMesh::cube_20x20x20}, config).empty());
            }
        }
    }
}

// Snapmaker: brim around the draft shield (draft_shield_brim_width).
namespace {

DynamicPrintConfig draft_shield_brim_config(double brim_width)
{
    DynamicPrintConfig config = Slic3r::Test::default_print_config();
    config.set_deserialize_strict({
        { "skirt_loops",             2 },
        { "skirt_distance",          3 },
        { "draft_shield",            "enabled" },
        { "draft_shield_brim_width", brim_width },
        { "brim_type",               "no_brim" },
        { "layer_height",            0.2 },
        { "initial_layer_print_height", 0.2 },
        { "gcode_comments",          true },
    });
    return config;
}

// Returns the skirt loop roles of the only skirt/brim group, outermost first.
std::vector<ExtrusionRole> shield_loop_roles(double brim_width)
{
    Print print;
    Model model;
    Slic3r::Test::init_print({ TestMesh::cube_20x20x20 }, print, model, draft_shield_brim_config(brim_width));
    print.process();
    REQUIRE(print.skirt_brim_groups().size() == 1);
    std::vector<ExtrusionRole> roles;
    for (const ExtrusionEntity *ee : print.skirt_brim_groups().front().skirt.entities)
        roles.push_back(ee->role());
    return roles;
}

} // namespace

TEST_CASE("Draft shield brim adds outer first-layer loops", "[SkirtBrim][DraftShieldBrim]")
{
    SECTION("disabled by default") {
        const std::vector<ExtrusionRole> roles = shield_loop_roles(0.);
        CHECK(roles == std::vector<ExtrusionRole>{ erSkirt, erSkirt });
    }
    SECTION("brim loops sit outside the shield walls") {
        const std::vector<ExtrusionRole> roles = shield_loop_roles(4.);
        const size_t n_brim  = std::count(roles.begin(), roles.end(), erBrim);
        const size_t n_skirt = std::count(roles.begin(), roles.end(), erSkirt);
        CHECK(n_skirt == 2);
        CHECK(n_brim >= 4); // 4 mm at well below 1 mm loop spacing
        REQUIRE(roles.size() == n_brim + n_skirt);
        // Outermost first: all brim loops precede the shield walls.
        CHECK(std::is_partitioned(roles.begin(), roles.end(), [](ExtrusionRole r) { return r == erBrim; }));
    }
}

TEST_CASE("Draft shield brim is printed on the first layer only", "[SkirtBrim][DraftShieldBrim]")
{
    const std::string gcode = Slic3r::Test::slice({ TestMesh::cube_20x20x20 }, draft_shield_brim_config(4.));
    const std::string role_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);

    std::string        role;
    std::set<double>   brim_z, skirt_z, all_z;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (boost::starts_with(line.raw(), role_tag)) {
            role = std::string(line.raw().substr(role_tag.size()));
            boost::trim(role);
            return;
        }
        if (!line.extruding(self) || line.dist_XY(self) <= 0)
            return;
        all_z.insert(self.z());
        if (role == ExtrusionEntity::role_to_string(erBrim))
            brim_z.insert(self.z());
        else if (role == ExtrusionEntity::role_to_string(erSkirt))
            skirt_z.insert(self.z());
    });

    REQUIRE_FALSE(all_z.empty());
    REQUIRE(brim_z.size() == 1);
    CHECK(std::abs(*brim_z.begin() - *all_z.begin()) < 1e-6);
    // The shield itself still rises with the object.
    CHECK(skirt_z.size() == all_z.size());
}

// Snapmaker: stepped fillet between the draft shield and its brim (draft_shield_fillet_width/_layers).
TEST_CASE("Draft shield fillet narrows over the layers above the brim", "[SkirtBrim][DraftShieldBrim]")
{
    DynamicPrintConfig config = draft_shield_brim_config(4.);
    config.set_deserialize_strict({
        { "draft_shield_fillet_width",  1.5 },
        { "draft_shield_fillet_layers", 3 },
    });
    const std::string gcode    = Slic3r::Test::slice({ TestMesh::cube_20x20x20 }, config);
    const std::string role_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);

    std::string                role;
    std::map<double, double>   brim_length; // print_z -> brim extrusion length
    std::set<double>           skirt_z, all_z;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (boost::starts_with(line.raw(), role_tag)) {
            role = std::string(line.raw().substr(role_tag.size()));
            boost::trim(role);
            return;
        }
        if (!line.extruding(self) || line.dist_XY(self) <= 0)
            return;
        all_z.insert(self.z());
        if (role == ExtrusionEntity::role_to_string(erBrim))
            brim_length[self.z()] += line.dist_XY(self);
        else if (role == ExtrusionEntity::role_to_string(erSkirt))
            skirt_z.insert(self.z());
    });

    REQUIRE(all_z.size() > 5);
    // Brim on the first layer plus three fillet layers, nothing above.
    REQUIRE(brim_length.size() == 4);
    auto layer = all_z.begin();
    for (const auto &[z, length] : brim_length) {
        CHECK(std::abs(z - *layer) < 1e-6);
        ++layer;
    }
    // Each fillet layer is narrower than the one below it.
    std::vector<double> lengths;
    for (const auto &[z, length] : brim_length)
        lengths.push_back(length);
    CHECK(lengths[0] > lengths[1]);
    CHECK(lengths[1] > lengths[2]);
    CHECK(lengths[2] > lengths[3]);
    CHECK(lengths[3] > 0.);
    // The shield itself still rises with the object.
    CHECK(skirt_z.size() == all_z.size());
}

// Snapmaker: brim around the first layer of the supports (support_brim_width).
namespace {

// A thin pillar carrying a wide cap: the supports below the cap stand mostly outside the object's footprint.
TriangleMesh mushroom_mesh()
{
    TriangleMesh pillar(its_make_cube(4., 4., 10.));
    pillar.translate(13.f, 13.f, 0.f);
    TriangleMesh cap(its_make_cube(30., 30., 2.));
    cap.translate(0.f, 0.f, 10.f);
    pillar.merge(cap);
    return pillar;
}

// Size of the bounding box of all brim extrusions in mm, or 0 if there is no brim.
// Optionally returns the size of the first support layer's extrusions and the x positions (mm, sorted)
// of the brim lines crossing a horizontal band through the middle of the right edge of the support.
Vec2d brim_size(const std::string &support_type, const std::string &brim_type, double support_brim_width,
                Vec2d *support_size = nullptr, std::vector<double> *right_edge_brim_x = nullptr, double *brim_spacing = nullptr)
{
    DynamicPrintConfig config = Slic3r::Test::default_print_config();
    config.set_deserialize_strict({
        { "skirt_loops",          0 },
        { "enable_support",       true },
        { "support_type",         support_type },
        { "raft_layers",          0 },
        { "brim_type",            brim_type },
        { "brim_width",           3 },
        { "brim_object_gap",      0 },
        { "support_brim_width",   support_brim_width },
        { "layer_height",         0.2 },
        { "initial_layer_print_height", 0.2 },
    });
    Print print;
    Slic3r::Test::init_and_process_print({ mushroom_mesh() }, print, config);
    BoundingBox bbox;
    for (const auto &[_, brim] : print.get_brimMap())
        for (const ExtrusionEntity *ee : brim.entities)
            for (const Polyline &pl : ee->as_polylines())
                bbox.merge(get_extents(pl));
    if (brim_spacing != nullptr)
        *brim_spacing = print.brim_flow().spacing();
    if (support_size != nullptr) {
        BoundingBox sbox;
        const PrintObject *object = print.objects().front();
        if (!object->support_layers().empty())
            for (const Polyline &pl : object->support_layers().front()->support_fills.as_polylines())
                sbox.merge(get_extents(pl));
        *support_size = sbox.defined ? unscale(sbox.size()) : Vec2d::Zero();
        if (right_edge_brim_x != nullptr && sbox.defined) {
            const coord_t cy = sbox.center().y();
            for (const auto &[_, brim] : print.get_brimMap())
                for (const ExtrusionEntity *ee : brim.entities)
                    for (const Polyline &pl : ee->as_polylines())
                        for (const Point &pt : pl.points)
                            if (pt.x() > sbox.max.x() && std::abs(pt.y() - cy) < scaled(5.))
                                right_edge_brim_x->push_back(unscale<double>(pt.x()));
            std::sort(right_edge_brim_x->begin(), right_edge_brim_x->end());
        }
    }
    return bbox.defined ? unscale(bbox.size()) : Vec2d::Zero();
}

} // namespace

TEST_CASE("Support brim surrounds the first layer of the supports", "[SkirtBrim][SupportBrim]")
{
    for (const std::string support_type : { "normal(auto)", "tree(auto)" }) {
        SECTION(support_type) {
            SECTION("disabled by default") {
                CHECK(brim_size(support_type, "no_brim", 0.) == Vec2d::Zero());
            }
            SECTION("object brim alone stays around the pillar") {
                // The support footprint blocks the object brim, so at most the pillar's brim remains
                // (here usually nothing, as the supports enclose the pillar).
                const Vec2d size = brim_size(support_type, "outer_only", 0.);
                CHECK(size.x() < 4. + 2. * 3. + 1.);
                CHECK(size.y() < 4. + 2. * 3. + 1.);
            }
            SECTION("support brim without object brim") {
                const Vec2d size = brim_size(support_type, "no_brim", 3.);
                UNSCOPED_INFO("support brim size " << size.x() << " x " << size.y());
                // Supports spread far beyond the 4 mm pillar below the 30 mm cap.
                CHECK(size.x() > 15.);
                CHECK(size.y() > 15.);
            }
            SECTION("support brim and object brim together") {
                const Vec2d support_only = brim_size(support_type, "no_brim", 3.);
                const Vec2d combined     = brim_size(support_type, "outer_only", 3.);
                CHECK(combined.x() >= support_only.x() - EPSILON);
                CHECK(combined.y() >= support_only.y() - EPSILON);
            }
        }
    }
}

TEST_CASE("Support brim covers the support footprint of normal support", "[SkirtBrim][SupportBrim]")
{
    // Normal support fills the whole area below the cap, so the brim reaches past the cap's 30 mm.
    Vec2d support_size;
    const Vec2d size = brim_size("normal(auto)", "no_brim", 3., &support_size);
    UNSCOPED_INFO("support " << support_size.x() << " x " << support_size.y() << ", brim " << size.x() << " x " << size.y());
    CHECK(size.x() > 30.);
    CHECK(size.y() > 30.);
    // The brim lines follow each other without a gap (an odd line count used to drop the middle line).
    Vec2d unused;
    std::vector<double> xs;
    double spacing = 0.;
    brim_size("normal(auto)", "no_brim", 3., &unused, &xs, &spacing);
    REQUIRE(xs.size() >= 2);
    double max_step = 0.;
    for (size_t i = 1; i < xs.size(); ++i)
        max_step = std::max(max_step, xs[i] - xs[i - 1]);
    UNSCOPED_INFO("brim x positions " << xs.front() << " .. " << xs.back() << ", max step " << max_step << ", spacing " << spacing);
    CHECK(max_step < 1.5 * spacing);
    // The brim adds about its width on each side of the support (centerlines, so minus one line).
    CHECK(std::abs((size.x() - support_size.x()) / 2. - 3.) < 1.);
    CHECK(std::abs((size.y() - support_size.y()) / 2. - 3.) < 1.);
}
