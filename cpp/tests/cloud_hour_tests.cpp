// Player reports from the 1114 cloud-hour list, each pinned at the system
// level it was reported against.
#include "test.h"
#include "server_harness.h"
#include "server/systems/movement.h"
#include "server/systems/petals.h"
#include "client/ui/menu_widgets.h"
#include "client/ui/menus.h"
#include "client/camera.h"
#include "test_data.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>
using namespace flix;
using namespace flix::testsupport;

namespace {

struct Flower {
    World world;
    PetalSystem petals;
    CommandBuffer commands{world};
    Entity player = NULL_ENTITY;
    double now = 1000.0;

    explicit Flower(std::initializer_list<const char*> ids, Rarity rarity = Rarity::Common) {
        const auto& registry = shippedContent();
        player = world.create();
        world.add<PlayerTag>(player);
        world.add<Transform>(player, Transform{{900.0, 900.0}, 0.0});
        world.add<Motion>(player);
        world.add<Body>(player, Body{kPlayerBaseRadius, 1.0});
        world.add<Health>(player, Health{100.0, 100.0});
        world.add<Faction>(player, Faction{Team::Players, true});
        world.add<PlayerInput>(player);
        world.add<PlayerModifiers>(player);
        world.add<PetalRing>(player);
        auto& slots = world.add<Loadout>(player).slots;
        std::size_t slot = 0;
        for (const char* id : ids) {
            slots[slot].configIndex = registry.petalIndex(id);
            slots[slot].rarity = rarity;
            ++slot;
        }
    }

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            now += net::kTickMillis;
            petals.run(world, shippedContent(), now, net::kTickSeconds, commands);
            commands.flush();
        }
    }

    int ticksFor(double millis) const { return static_cast<int>(millis / net::kTickMillis) + 1; }
};

} // namespace

// #3: a rose has to reach a flower that is standing still.
TEST(rose_heals_a_flower_that_stands_still) {
    for (const Rarity rarity : {Rarity::Common, Rarity::Legendary, Rarity::Super}) {
        Flower f({"rose"}, rarity);
        f.step();
        auto& health = f.world.get<Health>(f.player);
        health.current = 40.0;
        const Vec2 at = f.world.get<Transform>(f.player).position;
        f.step(f.ticksFor(6000.0));
        CHECK(f.world.get<Transform>(f.player).position == at);
        if (!(f.world.get<Health>(f.player).current > 40.0))
            std::printf("  rose %d did not heal a still flower\n", static_cast<int>(rarity));
        CHECK(f.world.get<Health>(f.player).current > 40.0);
    }
}

// #5/#31: every pad a player needs on a map can be walked to from where
// players arrive. Pockets the flood fill cannot reach are listed with their
// size, centre and how much mob band they cover.
TEST(map_reachability_audit) {
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    std::map<std::string, std::vector<Vec2>> arrivals;
    for (const MapData& map : maps.maps())
        for (const MapElement& e : map.elements())
            if (e.kind == MapElementKind::Teleporter && e.hasTeleportTo && !e.targetMap.empty())
                arrivals[e.targetMap].push_back(e.teleportTo);
    int strandedPads = 0;
    for (const MapData& map : maps.maps()) {
        if (map.prestige() || map.id() != map.templateId()) continue;
        const Realm realm = map.realm();
        std::vector<Vec2> seeds = arrivals[map.id()];
        for (const MapElement* door : map.playerSpawns()) seeds.push_back(door->centre());
        if (seeds.empty()) {
            std::printf("[reach] %s: no arrival point\n", map.id().c_str());
            continue;
        }
        constexpr int step = 64;
        const Vec2 extent = terrain.realmExtent(realm);
        const int width = std::max(1, static_cast<int>(extent.x) / step);
        const int height = std::max(1, static_cast<int>(extent.y) / step);
        const int cells = width * height;
        const auto sample = [width](int cell) {
            return Vec2{static_cast<double>(cell % width * step + step / 2),
                        static_cast<double>(cell / width * step + step / 2)};
        };
        const auto cellOf = [&](Vec2 at) {
            const int x = std::clamp(static_cast<int>(at.x) / step, 0, width - 1);
            const int y = std::clamp(static_cast<int>(at.y) / step, 0, height - 1);
            return y * width + x;
        };
        std::vector<unsigned char> open(cells);
        int walkable = 0;
        for (int cell = 0; cell < cells; ++cell) {
            if (terrain.resolveWall(sample(cell), kPlayerBaseRadius, realm).collided) continue;
            open[cell] = 1;
            ++walkable;
        }
        const auto neighbours = [&](int cell) {
            const int x = cell % width;
            return std::array<int, 4>{x > 0 ? cell - 1 : -1, x + 1 < width ? cell + 1 : -1,
                                      cell >= width ? cell - width : -1,
                                      cell + width < cells ? cell + width : -1};
        };
        const auto passable = [&](int from, int to) {
            return !terrain.resolveWall((sample(from) + sample(to)) * 0.5, kPlayerBaseRadius, realm).collided;
        };
        const auto nearestOpen = [&](Vec2 at) {
            const int centre = cellOf(at);
            int best = -1;
            double bestGap = 1e18;
            const int cx = centre % width, cy = centre / width;
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    const int x = cx + dx, y = cy + dy;
                    if (x < 0 || y < 0 || x >= width || y >= height || !open[y * width + x]) continue;
                    const double gap = distanceSq(sample(y * width + x), at);
                    if (gap < bestGap) { bestGap = gap; best = y * width + x; }
                }
            return best;
        };
        std::vector<int> reached;
        for (const Vec2 seed : seeds) {
            const int start = nearestOpen(seed);
            if (start < 0 || open[start] == 2) continue;
            open[start] = 2;
            reached.push_back(start);
            for (std::size_t i = reached.size() - 1; i < reached.size(); ++i) {
                for (const int next : neighbours(reached[i])) {
                    if (next < 0 || open[next] != 1 || !passable(reached[i], next)) continue;
                    open[next] = 2;
                    reached.push_back(next);
                }
            }
        }
        std::vector<int> component(cells, -1);
        struct Pocket { int size = 0; double x = 0, y = 0; int band = 0; };
        std::vector<Pocket> pockets;
        for (int cell = 0; cell < cells; ++cell) {
            if (open[cell] != 1 || component[cell] >= 0) continue;
            const int id = static_cast<int>(pockets.size());
            pockets.push_back({});
            std::vector<int> queue{cell};
            component[cell] = id;
            for (std::size_t i = 0; i < queue.size(); ++i) {
                const int at = queue[i];
                Pocket& pocket = pockets[id];
                ++pocket.size;
                pocket.x += sample(at).x;
                pocket.y += sample(at).y;
                for (const MapElement& e : map.elements())
                    if (e.isSpawnBand() && e.contains(sample(at))) { ++pocket.band; break; }
                for (const int next : neighbours(at)) {
                    if (next < 0 || open[next] != 1 || component[next] >= 0 || !passable(at, next)) continue;
                    component[next] = id;
                    queue.push_back(next);
                }
            }
        }
        std::sort(pockets.begin(), pockets.end(), [](const Pocket& a, const Pocket& b) { return a.size > b.size; });
        int unreachedBand = 0;
        for (const Pocket& pocket : pockets) unreachedBand += pocket.band;
        std::printf("[reach] %s: reached %zu/%d cells, %zu pockets, %d unreachable band cells",
                    map.id().c_str(), reached.size(), walkable, pockets.size(), unreachedBand);
        for (std::size_t i = 0; i < pockets.size() && i < 4 && pockets[i].size >= 4; ++i)
            std::printf(" | %d@(%.0f,%.0f) band %d", pockets[i].size, pockets[i].x / pockets[i].size,
                        pockets[i].y / pockets[i].size, pockets[i].band);
        std::printf("\n");
        for (const MapElement& e : map.elements()) {
            if (e.kind != MapElementKind::Teleporter || e.targetMap.empty()) continue;
            const int at = nearestOpen(e.centre());
            if (at >= 0 && open[at] == 2) continue;
            ++strandedPads;
            std::printf("[reach] %s: pad to %s at (%.0f,%.0f) cannot be walked to\n", map.id().c_str(),
                        e.targetMap.c_str(), e.centre().x, e.centre().y);
        }
    }
    CHECK_EQ(strandedPads, 0);
}

// #23: the reload a bar of golden leaves really pays is the one the petal card
// prints, for one leaf, for leaves that stack and for none.
TEST(golden_leaf_reload_on_the_card_is_the_reload_the_server_pays) {
    const auto& registry = shippedContent();
    const auto basic = registry.petalIndex("basic");
    const auto leaf = registry.petalIndex("golden_leaf");
    const std::vector<std::vector<Rarity>> bars{
        {}, {Rarity::Ultra}, {Rarity::Ultra, Rarity::Common}, {Rarity::Super, Rarity::Super}};
    for (const auto& leaves : bars) {
        Flower f({"basic"});
        auto& slots = f.world.get<Loadout>(f.player).slots;
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            slots[i + 1].configIndex = leaf;
            slots[i + 1].rarity = leaves[i];
        }
        f.step(f.ticksFor(8000.0));
        Entity petal = NULL_ENTITY;
        Query<PetalInstance, Health> live{f.world};
        live.each([&](Entity e, PetalInstance& instance, Health&) {
            if (instance.owner == f.player && instance.slot == 0 && !f.world.has<Dead>(e)) petal = e;
        });
        CHECK(petal != NULL_ENTITY);
        if (petal == NULL_ENTITY) continue;
        f.world.get<Health>(petal).current = 0.0;
        const double brokenAt = f.now + net::kTickMillis;
        f.step();
        const double paid = f.world.get<Loadout>(f.player).slots[0].reloadReadyAtMillis - brokenAt;
        const double cut = barReloadCut(registry, [&](int i) {
            EquippedPetal worn;
            if (i == 0) worn.configIndex = static_cast<std::uint16_t>(basic);
            if (i >= 1 && i <= static_cast<int>(leaves.size())) {
                worn.configIndex = static_cast<std::uint16_t>(leaf);
                worn.rarity = worn.reloadRarity = leaves[i - 1];
            }
            return worn;
        });
        const double card = ui::ordinaryPetalReloadMillis(
            registry.petalStats(basic, Rarity::Common).reloadMillis, 1.0 - cut);
        std::printf("  %zu leaves: cut %.4f, card %.1f ms, server %.1f ms\n", leaves.size(), cut, card, paid);
        CHECK_NEAR(paid, card, 1e-6);
    }
}

// #25: taking the antennae off brings the view back to the flower's own --
// the Vision talent's, or the default -- and never narrower than that.
TEST(taking_the_antennae_off_returns_the_view_to_the_flowers_own) {
    const auto& registry = shippedContent();
    const auto antennae = registry.petalIndex("antennae");
    for (const int vision : {-1, 0, 2}) {
        for (const Rarity rarity : {Rarity::Common, Rarity::Super, Rarity::Unique}) {
            Profile profile;
            profile.skills.set(SkillId::Loadout, 0);
            profile.skills.set(SkillId::Antennae, vision);
            profile.loadout.resize(kLoadoutSlots);
            const double own = loadoutCameraZoom(profile, registry);
            profile.loadout[0].petalIndex = antennae;
            profile.loadout[0].rarity = rarity;
            const double worn = loadoutCameraZoom(profile, registry);
            CHECK(worn <= own);
            Camera camera;
            camera.loadoutZoom = worn;
            profile.loadout[0] = Profile::Slot{};
            const double off = loadoutCameraZoom(profile, registry);
            CHECK_NEAR(off, own, 1e-12);
            for (int frame = 0; frame < 60; ++frame) camera.approachLoadoutZoom(off, 1.0 / 60.0);
            CHECK_NEAR(camera.loadoutZoom, own, 1e-9);
            CHECK(camera.zoom() >= camera.userZoom * own - 1e-9);
        }
    }
}

// #32: one card at a time, whichever group the second button belongs to.
TEST(opening_a_top_row_card_closes_the_open_bottom_left_card) {
    const std::array<MenuId, 3> column = {MenuId::Inventory, MenuId::Talents, MenuId::Crafting};
    const std::array<MenuId, 6> row = {MenuId::Settings, MenuId::Changelog, MenuId::Leaderboard,
                                       MenuId::Shop,     MenuId::Skins,     MenuId::Gallery};
    for (MenuId below : column) {
        for (MenuId above : row) {
            MenuSystem menus;
            menus.toggle(below);
            CHECK(menus.open() == below);
            menus.toggle(above);
            CHECK(menus.open() == above);
            menus.toggle(below);
            CHECK(menus.open() == below);
            menus.toggle(below);
            CHECK(!menus.anyOpen());
        }
    }
}

// #36: a flower split while pinned between a wall and a mob keeps both halves
// out of the wall, and the half being steered can still walk away.
TEST(splitting_between_a_wall_and_a_mob_leaves_both_halves_free) {
    Harness h("cloud-hour-split-wall", {}, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Terrain& terrain = h.server.terrain();
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    if (splitter == kInvalidIndex) { CHECK(false); return; }
    const auto bodies = [&] {
        std::vector<Entity> found;
        Query<PlayerTag, PlayerAccount> all{world};
        all.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
            if (account.username == "alice" && !world.has<Dead>(e)) found.push_back(e);
        });
        return found;
    };
    if (bodies().size() != 1) { CHECK(false); return; }
    const Entity original = bodies().front();
    const Realm realm = world.get<Transform>(original).realm;
    const double radius = world.get<Body>(original).radius;
    const Vec2 from = world.get<Transform>(original).position;

    // Clear of the nearest wall due west or east, but close enough that the
    // half cut towards it is proposed overlapping it.
    const auto besideWall = [&](double side, Vec2& at) {
        for (double reach = radius; reach < 4000.0; reach += 4.0) {
            const Vec2 probe{from.x + side * reach, from.y};
            if (!terrain.blocked(probe, realm)) continue;
            for (double gap = radius + 2.0; gap < radius * 2.0 - 4.0; gap += 2.0) {
                at = Vec2{probe.x - side * gap, probe.y};
                if (!terrain.resolveWall(at, radius, realm).collided) return true;
            }
            return false;
        }
        return false;
    };
    double side = -1.0;
    Vec2 spot{};
    if (!besideWall(side, spot)) {
        side = 1.0;
        if (!besideWall(side, spot)) { CHECK(false); return; }
    }
    std::printf("  wall %s of (%.0f,%.0f), standing at (%.0f,%.0f)\n", side < 0 ? "west" : "east",
                from.x, from.y, spot.x, spot.y);
    world.get<Transform>(original).position = spot;
    world.get<Motion>(original).velocity = {0, 0};
    world.get<Health>(original).invulnerableUntilMillis = 1e18;

    CHECK(h.server.grantAdmin(alice.sessionToken()));
    alice.sendChat(std::string("/admin give alice ") + kSplitterPetalId + " common 1");
    CHECK(h.stepUntil({&alice}, [&] { return alice.profile().stackCount(splitter, Rarity::Common) > 0; }));
    const Vec2 mobAt{spot.x - side * (radius * 3.0 + 20.0), spot.y};
    alice.sendChat("/admin spawn soldier_ant mythic " + std::to_string(static_cast<int>(mobAt.x)) + " " +
                   std::to_string(static_cast<int>(mobAt.y)) + " 1");
    h.step(10, {&alice});

    double worst = 0.0;
    bool centreInWall = false;
    const auto measure = [&] {
        for (const Entity half : bodies()) {
            const Vec2 p = world.get<Transform>(half).position;
            const double r = world.get<Body>(half).radius;
            worst = std::max(worst, distance(terrain.resolveWall(p, r, realm).position, p));
            if (terrain.blocked(p, realm)) centreInWall = true;
        }
    };
    alice.setLoadoutSlot(0, splitter, Rarity::Common);
    CHECK(h.stepUntil({&alice}, [&] {
        measure();
        return bodies().size() == 2;
    }, 60));
    if (bodies().size() != 2) return;
    for (const Entity half : bodies()) world.get<Health>(half).invulnerableUntilMillis = 1e18;
    for (int tick = 0; tick < 150; ++tick) {
        h.step(1, {&alice});
        measure();
    }
    std::printf("  deepest wall overlap of either half: %.3f\n", worst);
    CHECK(!centreInWall);
    CHECK(worst <= 1.0);

    Query<MobTag, Transform, Health> mobs{world};
    mobs.each([&](Entity, MobTag&, Transform& at, Health& health) {
        if (at.realm == realm && distance(at.position, spot) < 800.0) health.current = 0.0;
    });
    h.step(5, {&alice});
    Entity steered = NULL_ENTITY;
    for (const Entity half : bodies())
        if (world.get<NetId>(half).value == alice.view().self().netId) steered = half;
    CHECK(steered != NULL_ENTITY);
    if (steered == NULL_ENTITY) return;
    const Vec2 before = world.get<Transform>(steered).position;
    for (int tick = 0; tick < 30; ++tick) {
        auto& input = world.get<PlayerInput>(steered).current;
        input.moveAngle = side < 0 ? 0.0 : kPi;
        input.moveStrength = 1.0;
        h.step(1, {&alice});
    }
    const double walked = (world.get<Transform>(steered).position.x - before.x) * -side;
    std::printf("  steered half walked %.1f away from the wall\n", walked);
    CHECK(walked > 20.0);
}

// #54: a flower that dies pressed against, or squeezed a little way into, any
// wall of the sewers comes back with its whole body clear, on its own side of
// the wall, and on ground that can be walked to from where players arrive.
TEST(a_death_against_any_sewers_wall_respawns_on_walkable_ground) {
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    std::map<std::string, std::vector<Vec2>> arrivals;
    for (const MapData& map : maps.maps())
        for (const MapElement& e : map.elements())
            if (e.kind == MapElementKind::Teleporter && e.hasTeleportTo && !e.targetMap.empty())
                arrivals[e.targetMap].push_back(e.teleportTo);
    int audited = 0;
    for (const MapData& map : maps.maps()) {
        if (map.id() != "sewers" && map.id() != "rift_sewers") continue;
        ++audited;
        const Realm realm = map.realm();
        std::vector<Vec2> seeds = arrivals[map.id()];
        for (const MapElement* door : map.playerSpawns()) seeds.push_back(door->centre());
        CHECK(!seeds.empty());
        if (seeds.empty()) continue;

        constexpr int step = 32;
        const Vec2 extent = terrain.realmExtent(realm);
        const int width = std::max(1, static_cast<int>(extent.x) / step);
        const int height = std::max(1, static_cast<int>(extent.y) / step);
        const int cells = width * height;
        const auto sample = [width](int cell) {
            return Vec2{static_cast<double>(cell % width * step + step / 2),
                        static_cast<double>(cell / width * step + step / 2)};
        };
        // 0 wall, 1 open, 2 open and reached from an arrival point.
        std::vector<unsigned char> ground(cells);
        for (int cell = 0; cell < cells; ++cell)
            if (!terrain.resolveWall(sample(cell), kPlayerBaseRadius, realm).collided) ground[cell] = 1;
        const auto cellsAround = [&](Vec2 at, int span, auto&& visit) {
            const int cx = std::clamp(static_cast<int>(at.x) / step, 0, width - 1);
            const int cy = std::clamp(static_cast<int>(at.y) / step, 0, height - 1);
            for (int dy = -span; dy <= span; ++dy)
                for (int dx = -span; dx <= span; ++dx) {
                    const int x = cx + dx, y = cy + dy;
                    if (x >= 0 && y >= 0 && x < width && y < height) visit(y * width + x);
                }
        };
        const auto nearestVisibleOpen = [&](Vec2 at) {
            int best = -1;
            double bestGap = 1e18;
            cellsAround(at, 3, [&](int cell) {
                if (ground[cell] == 0 || terrain.segmentBlocked(at, sample(cell), realm)) return;
                const double gap = distanceSq(sample(cell), at);
                if (gap < bestGap) { bestGap = gap; best = cell; }
            });
            return best;
        };
        std::vector<int> queue;
        for (const Vec2 seed : seeds) {
            const int start = nearestVisibleOpen(seed);
            if (start < 0 || ground[start] == 2) continue;
            ground[start] = 2;
            queue.push_back(start);
        }
        for (std::size_t i = 0; i < queue.size(); ++i) {
            const int at = queue[i];
            const int x = at % width;
            for (const int next : {x > 0 ? at - 1 : -1, x + 1 < width ? at + 1 : -1,
                                   at >= width ? at - width : -1, at + width < cells ? at + width : -1}) {
                if (next < 0 || ground[next] != 1) continue;
                if (terrain.resolveWall((sample(at) + sample(next)) * 0.5, kPlayerBaseRadius, realm).collided)
                    continue;
                ground[next] = 2;
                queue.push_back(next);
            }
        }

        int deaths = 0, embedded = 0, crossed = 0, stranded = 0;
        const auto report = [&](const char* what, int count, Vec2 corpse, Vec2 at, double radius) {
            if (count <= 3)
                std::printf("[respawn] %s: %s, radius %.0f, died at (%.0f,%.0f), back at (%.0f,%.0f)\n",
                            map.id().c_str(), what, radius, corpse.x, corpse.y, at.x, at.y);
        };
        for (double y = 8.0; y < extent.y; y += 16.0) {
            for (double x = 8.0; x < extent.x; x += 16.0) {
                const Vec2 corpse{x, y};
                if (!terrain.resolveWall(corpse, kPlayerBaseRadius, realm).collided) continue;
                const bool inside = terrain.blocked(corpse, realm);
                if (inside && distance(terrain.resolveWall(corpse, 1.0, realm).position, corpse) > kPlayerBaseRadius)
                    continue;
                bool besideWalkable = false;
                cellsAround(corpse, 2, [&](int cell) { besideWalkable = besideWalkable || ground[cell] == 2; });
                if (!besideWalkable) continue;
                for (const double radius : {kPlayerBaseRadius, 45.0}) {
                    ++deaths;
                    const Vec2 at = playerRespawnPosition(terrain, realm, corpse, radius);
                    const auto hull = terrain.resolveWall(at, radius, realm);
                    if (hull.collided || hull.unresolved) report("body in a wall", ++embedded, corpse, at, radius);
                    // A corpse exactly on a wall's face touches it whichever
                    // way the body leaves; its side is the one it moved to.
                    const double moved = distance(at, corpse);
                    const Vec2 from = moved > 1.0 && terrain.segmentTouchesBlockingTile(corpse, corpse, 0.0, realm)
                                          ? corpse + (at - corpse) * (0.5 / moved)
                                          : corpse;
                    if (!inside && terrain.segmentTouchesBlockingTile(from, at, 0.0, realm))
                        report("put across a wall", ++crossed, corpse, at, radius);
                    const int cell = nearestVisibleOpen(at);
                    if (cell < 0 || ground[cell] != 2) report("shut off from the map", ++stranded, corpse, at, radius);
                }
            }
        }
        std::printf("[respawn] %s: %d deaths against a wall, %d in a wall, %d across one, %d shut off\n",
                    map.id().c_str(), deaths, embedded, crossed, stranded);
        CHECK(deaths > 0);
        CHECK_EQ(embedded, 0);
        CHECK_EQ(crossed, 0);
        CHECK_EQ(stranded, 0);
    }
    CHECK_EQ(audited, 2);
}