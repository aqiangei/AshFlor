#include "test.h"

#include "client/camera.h"
#include "client/ui/menus.h"
#include "shared/game/config.h"

#include <string>
#include "fixture_content.h"

using namespace flix;

// What the camera shows: the player's zoom setting times what the worn
// loadout asks for. Antennae and observer are the petals that ask, and the
// figure is the client's alone -- the server neither computes it nor sends
// it, exactly as the TypeScript client's getEquippedZoomMultiplier was -- so
// this is the one place it can be right or wrong.

namespace {

/// Content of its own, so the assertions ride the RULES and not the shipped
/// balance numbers: one petal that asks nothing, two that ask the figures
/// petals.json gives antennae and observer, and one authored so low that a
/// high tier would take it through zero.
const char* const kPetalsJson = R"JSON({
  "basic":    {"name":"Basic","damage":10,"health":10,"size":1,"cooldown":2500,"count":1,"color":"#FFFFFF"},
  "antennae": {"name":"Antennae","damage":0,"health":null,"size":1,"cooldown":1,"count":0,"equipFlags":"Antennae","noPhysics":true,"cameraZoom":0.92,"color":"#000000"},
  "observer": {"name":"Observer","damage":0,"health":null,"size":1,"cooldown":1,"count":0,"equipFlags":"Observer","noPhysics":true,"cameraZoom":0.85,"color":"#000000"},
  "scope":    {"name":"Scope","damage":0,"health":null,"size":1,"cooldown":1,"count":0,"noPhysics":true,"cameraZoom":0.1,"color":"#000000"}
})JSON";

const char* const kMobsJson = R"JSON({
  "critter": {"name":"Critter","health":10,"damage":1,"size":1,"speed":0.2,"range":300,"cooldown":500,"color":"#FF0000","section":[0],"ai_type":"hostile"}
})JSON";

struct Fixture {
    ContentRegistry registry;
    bool ok = false;
    std::string error;
};

const Fixture& fixture() {
    static const Fixture state = [] {
        Fixture f;
        f.ok = test::loadFixtureContent(f.registry, testsupport::tempDir("flix_camera_tests"),
                                        kMobsJson, kPetalsJson, f.error);
        return f;
    }();
    return state;
}

bool contentLoaded() {
    if (!fixture().ok) {
        ::testing::reportFailure(__FILE__, __LINE__,
                                 "fixture content failed to load: " + fixture().error);
    }
    return fixture().ok;
}

Profile::Slot slot(const char* id, Rarity rarity = Rarity::Common) {
    Profile::Slot s;
    s.petalIndex = fixture().registry.petalIndex(id);
    s.rarity = rarity;
    return s;
}

/// A profile with `count` slots, every one of them empty.
Profile emptyBar(std::size_t count = static_cast<std::size_t>(kLoadoutActiveSlots) * 2) {
    Profile p;
    p.loadout.assign(count, Profile::Slot{});
    return p;
}

double zoomOf(const Profile& p) { return loadoutCameraZoom(p, fixture().registry); }

/// The multiplier a tier applies to the authored figure's distance from 1:
/// 4/3 per tier, geometric, so each upgrade widens the view by as much as the
/// last one did. See petalZoomScale.
double scaled(double authored, Rarity rarity) {
    return 1.0 + (authored - 1.0) * petalZoomScale(rarity);
}

/// The floor loadoutCameraZoom applies after the scaling.
constexpr double kZoomFloor = 0.3;

} // namespace

TEST(a_bar_with_no_antennae_or_observer_asks_the_camera_for_nothing) {
    if (!contentLoaded()) return;
    CHECK_NEAR(zoomOf(Profile{}), 1.0, 1e-12);
    Profile p = emptyBar();
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
    p.loadout[0] = slot("basic", Rarity::Unique);
    p.loadout[3] = slot("basic");
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
}

TEST(antennae_and_observer_zoom_the_camera_out_at_their_supported_tiers) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[2] = slot("antennae");
    CHECK_NEAR(zoomOf(p), 0.92, 1e-12);
    p.loadout[2] = slot("observer");
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
    p.loadout[2] = slot("observer", Rarity::Rare);
    CHECK_NEAR(zoomOf(p), 1.0 / 1.12, 1e-12);
}

TEST(two_of_them_show_the_better_one_and_never_stack) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[0] = slot("antennae");
    p.loadout[1] = slot("observer", Rarity::Rare);
    CHECK_NEAR(zoomOf(p), 1.0 / 1.12, 1e-12);
    p.loadout[0] = slot("observer", Rarity::Rare);
    CHECK_NEAR(zoomOf(p), 1.0 / 1.12, 1e-12);
    p.loadout[p.skills.loadoutSlots() - 1] = slot("antennae", Rarity::Unique);
    CHECK_NEAR(zoomOf(p), 0.1, 1e-12);
}

TEST(observer_camera_zoom_uses_the_current_rarity_ladder) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    constexpr double vision[] = {1.0, 1.0, 1.12, 1.2, 1.3, 1.6, 2.2, 4.0, 8.0, 9.2};
    static_assert(std::size(vision) == kLadderRarityCount);
    for (int i = 0; i < kLadderRarityCount; ++i) {
        p.loadout[0] = slot("observer", static_cast<Rarity>(i));
        CHECK_NEAR(zoomOf(p), 1.0 / vision[i], 1e-12);
    }
}

TEST(the_view_is_floored_for_generic_camera_petals) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[0] = slot("scope", Rarity::Apex);
    CHECK(scaled(0.1, Rarity::Apex) < 0.0);
    CHECK_NEAR(zoomOf(p), kZoomFloor, 1e-12);
}

TEST(storage_grants_no_zoom) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    const auto observer = slot("observer", Rarity::Rare);
    p.loadout[kLoadoutActiveSlots] = observer;
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
    p.loadout[p.skills.loadoutSlots()] = observer;
    CHECK_NEAR(zoomOf(p), 1.0, 1e-12);
    p.loadout[p.skills.loadoutSlots() - 1] = observer;
    CHECK_NEAR(zoomOf(p), 1.0 / 1.12, 1e-12);
}

TEST(a_slot_the_content_does_not_know_is_skipped) {
    if (!contentLoaded()) return;
    Profile p = emptyBar();
    p.loadout[0].petalIndex = static_cast<std::uint16_t>(fixture().registry.petalCount() + 5);
    p.loadout[1] = slot("antennae");
    CHECK_NEAR(zoomOf(p), 0.92, 1e-12);
}

TEST(map_edge_camera_stops_at_each_edge_and_keeps_pointer_coordinates_aligned) {
    Camera camera;
    camera.setViewport(1200, 600);
    camera.setWorldExtent({3000, 2000});
    for (const auto flower : {Vec2{25, 25}, Vec2{2975, 25}, Vec2{25, 1975}, Vec2{2975, 1975}}) {
        camera.snapTo(flower);
        const Rect view = camera.visibleWorld();
        CHECK(view.left() >= 0);
        CHECK(view.top() >= 0);
        CHECK(view.right() <= 3000);
        CHECK(view.bottom() <= 2000);
        const Vec2 onScreen = camera.worldToScreen(flower);
        CHECK_NEAR(onScreen.x, flower.x < 1500 ? 25.0 : 1175.0, 1e-9);
        CHECK_NEAR(onScreen.y, flower.y < 1000 ? 25.0 : 575.0, 1e-9);
        CHECK(camera.screenToWorld(onScreen) == flower);
    }
    camera.snapTo({1500, 1000});
    CHECK(camera.worldToScreen({1500, 1000}) == Vec2(600, 300));
    camera.setWorldExtent({});
    camera.snapTo({25, 25});
    CHECK(camera.worldToScreen({25, 25}) == Vec2(600, 300));
}

TEST(map_edge_camera_fills_the_view_when_the_map_is_smaller_than_the_requested_view) {
    Camera camera;
    camera.setViewport(950, 380, 1.25);
    camera.setWorldExtent({600, 520});
    camera.loadoutZoom = 0.10;
    camera.snapTo({580, 510});
    CHECK_NEAR(camera.visibleWorld().left(), 0.0, 1e-9);
    CHECK_NEAR(camera.visibleWorld().right(), 600.0, 1e-9);
    CHECK_NEAR(camera.visibleWorld().bottom(), 520.0, 1e-9);
    CHECK_NEAR(camera.zoom(), 950.0 / 600.0, 1e-9);

    camera.setViewport(380, 950, 1.25);
    camera.snapTo({20, 10});
    CHECK_NEAR(camera.visibleWorld().left(), 0.0, 1e-9);
    CHECK_NEAR(camera.visibleWorld().top(), 0.0, 1e-9);
    CHECK_NEAR(camera.visibleWorld().bottom(), 520.0, 1e-9);
    CHECK_NEAR(camera.zoom(), 950.0 / 520.0, 1e-9);
}

TEST(wave_camera_responds_at_both_ends_of_the_zoom_range_without_showing_past_its_edges) {
    constexpr double roomSize = 4096.0;
    Camera camera;
    camera.setWorldExtent({roomSize, roomSize}, true);
    for (const Vec2 viewport : {Vec2{1920, 1080}, Vec2{850, 1080}}) {
        camera.setViewport(static_cast<int>(viewport.x), static_cast<int>(viewport.y));
        for (const double loadout : {1.0, 0.09}) {
            camera.loadoutZoom = loadout;
            double previous = 0;
            for (const double setting : {kMinimumUserCameraZoom, kMinimumUserCameraZoom + 0.05,
                                         0.95, 1.0, 1.05, 3.95, 4.0}) {
                camera.userZoom = setting;
                camera.snapTo({roomSize - 20.0, 20.0});
                CHECK(camera.zoom() > previous);
                previous = camera.zoom();
                const Rect view = camera.visibleWorld();
                CHECK_NEAR(view.top(), 0.0, 1e-9);
                CHECK_NEAR(view.right(), roomSize, 1e-9);
                CHECK(view.left() >= -1e-9 && view.bottom() <= roomSize + 1e-9);
                if (setting == kMinimumUserCameraZoom) {
                    CHECK_NEAR(std::max(view.w, view.h), roomSize, 1e-9);
                }
                if (setting == 1.0 && loadout == 1.0) CHECK_NEAR(camera.zoom(), 1.0, 1e-9);
            }
        }
    }

    camera.setWorldExtent({38400, 38400});
    camera.userZoom = 1.0;
    CHECK_NEAR(camera.zoom(), camera.loadoutZoom, 1e-9);
}

TEST(the_camera_multiplies_the_setting_by_what_the_loadout_asks) {
    Camera camera;
    camera.setViewport(1920, 1080);
    camera.snapTo({1000, 1000});
    camera.userZoom = 1.0;
    camera.loadoutZoom = 1.0;
    CHECK_NEAR(camera.zoom(), 1.0, 1e-12);
    const Rect bare = camera.visibleWorld();

    // An observer alone: the world is 1/0.85 wider through the same window.
    camera.loadoutZoom = 0.85;
    CHECK_NEAR(camera.zoom(), 0.85, 1e-12);
    const Rect worn = camera.visibleWorld();
    CHECK_NEAR(worn.w, bare.w / 0.85, 1e-9);
    CHECK_NEAR(worn.h, bare.h / 0.85, 1e-9);
    // Still centred on the flower.
    CHECK_NEAR(worn.x + worn.w * 0.5, 1000.0, 1e-9);
    CHECK_NEAR(worn.y + worn.h * 0.5, 1000.0, 1e-9);

    // The player's own zoom-in rides on top of it, not instead of it.
    camera.userZoom = 1.6;
    CHECK_NEAR(camera.zoom(), 1.6 * 0.85, 1e-12);
    // And the screen centre maps back to the flower whatever the product.
    const Vec2 centre = camera.screenToWorld({960, 540});
    CHECK_NEAR(centre.x, 1000.0, 1e-9);
    CHECK_NEAR(centre.y, 1000.0, 1e-9);
}

TEST(interface_scale_preserves_the_world_view_and_cursor_aim) {
    Camera camera;
    camera.setViewport(1920, 1080);
    camera.snapTo({1000, 1000});
    camera.userZoom = 1.6;
    camera.loadoutZoom = 0.85;
    const Rect original = camera.visibleWorld();
    const Vec2 aim = camera.screenToWorld({1200, 600});
    for (const double scale : {0.6, 1.2}) {
        camera.setViewport(static_cast<int>(std::round(1920 / scale)),
                           static_cast<int>(std::round(1080 / scale)), scale);
        const Rect world = camera.visibleWorld();
        CHECK_NEAR(world.w, original.w, 1e-9);
        CHECK_NEAR(world.h, original.h, 1e-9);
        const Vec2 cursor = camera.screenToWorld({1200 / scale, 600 / scale});
        CHECK_NEAR(cursor.x, aim.x, 1e-9);
        CHECK_NEAR(cursor.y, aim.y, 1e-9);
    }
}

TEST(camera_loadout_zoom_eases_and_keeps_the_flower_centred) {
    Camera camera;
    camera.setViewport(1200, 800);
    camera.snapTo({5000.0, 7000.0});
    for (int i = 0; i < 12; ++i) {
        const auto before = camera.loadoutZoom;
        camera.approachLoadoutZoom(0.1, 1.0 / 60.0);
        CHECK(camera.loadoutZoom < before);
        CHECK(camera.loadoutZoom > 0.1);
        const Vec2 centre = camera.worldToScreen({5000.0, 7000.0});
        CHECK_NEAR(centre.x, 600.0, 1e-8);
        CHECK_NEAR(centre.y, 400.0, 1e-8);
    }
    for (int i = 0; i < 24; ++i) camera.approachLoadoutZoom(0.1, 1.0 / 60.0);
    CHECK_NEAR(camera.loadoutZoom, 0.1, 1e-8);
}
