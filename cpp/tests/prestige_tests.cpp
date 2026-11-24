// Prestige: the talent, the prestige twin maps, what grows in them, and the
// join that reaches them. See shared/game/prestige.h.

#include "test.h"

#include "server_harness.h"
#include "client/render/sprites.h"
#include "client/render/world_renderer.h"
#include "client/ui/chat_i18n.h"
#include "client/ui/i18n.h"
#include "render_rig.h"
#include "server/db.h"
#include "server/practice_board.h"
#include "server/systems/spawning.h"
#include "server/systems/mob_ai.h"
#include "server/systems/combat.h"
#include "server/systems/dungeons.h"
#include "server/systems/loot.h"
#include "server/systems/petals.h"
#include "shared/game/prestige.h"
#include "shared/game/assembler.h"
#include "shared/game/difficulty.h"
#include "shared/game/map_elements.h"
#include "shared/game/skills.h"
#include "shared/game/shop.h"
#include "shared/game/terrain.h"
#include "test_data.h"

#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace flix;
using namespace flix::testsupport;

namespace {

/// Gives a seeded account the Prestige node, and optionally XP enough for
/// `level`, which is what a purchase is paid from.
void seedProgress(const std::string& path, const std::string& username, bool prestige,
                  int level = 1) {
    Database db;
    std::string error;
    db.load(path, error);
    const Account* account = db.findUser(username);
    if (account == nullptr) return;
    PlayerRecord& record = db.progress(account->id);
    if (prestige) record.skills.set(SkillId::Prestige, 0);
    double xp = 0.0;
    for (int l = 1; l < level; ++l) xp += xpForNextLevel(l);
    record.totalXp = xp;
    db.markDirty();
    db.save();
}

bool logIn(Harness& h, NetClient& client, const char* name) {
    if (!connectClient(h, client)) return false;
    client.requestLogin(name, "password7");
    return h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; });
}

bool join(Harness& h, NetClient& client, const char* name, bool prestige,
          const std::string& where = {}) {
    client.joinGame(1920, 1080, where, name, prestige);
    return h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; },
                       200);
}

Realm realmOf(World& world, Entity e) {
    return e == NULL_ENTITY ? Realm::Overworld : world.get<Transform>(e).realm;
}

} // namespace

static void checkBioMap(const char* mapId, int expectedWalls, std::uint32_t sourceMaskHash,
                        std::size_t expectedSites) {
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    bool found = false;
    const Realm realm = maps.realmOfId(mapId, found);
    CHECK(found);
    if (!found) return;
    const auto& registry = shippedContent();
    DropTables drops;
    CHECK(drops.load(registry, dataDir() + "/mob_drops.json", error));
    int walls = 0;
    std::uint32_t maskHash = 2166136261u;
    for (int y = 0; y < 100; ++y) {
        for (int x = 0; x < 100; ++x) {
            const bool blocked = terrain.blocked(Terrain::tileCenter(x, y), realm);
            walls += blocked;
            maskHash = (maskHash ^ static_cast<std::uint32_t>(blocked)) * 16777619u;
        }
    }
    CHECK_EQ(walls, expectedWalls);
    CHECK_EQ(maskHash, sourceMaskHash);
    Rng rng{42};
    const auto& choices = maps.spawnChoices();
    CHECK(std::any_of(choices.begin(), choices.end(), [realm](const auto& choice) {
        return choice.realm == realm;
    }));
    const auto at = maps.forRealm(realm)->defaultSpawn(rng, terrain);
    CHECK(!terrain.resolveWall(at, kPlayerBaseRadius, realm).collided);
    int exits = 0;
    std::set<std::string> centrePads;
    for (const auto& element : maps.forRealm(realm)->elements()) {
        if (element.kind == MapElementKind::Npc && element.npcId == "bio_assembler")
            CHECK(!terrain.resolveWall(element.centre(), kPlayerBaseRadius, realm).collided);
        if (element.kind != MapElementKind::Teleporter) continue;
        CHECK(!terrain.resolveWall(element.centre(), kPlayerBaseRadius, realm).collided);
        WorldMaps::Destination out;
        if (!maps.resolveTeleporter(element, rng, terrain, out)) { CHECK(false); continue; }
        if (out.realm == realm) {
            centrePads.insert(element.padId);
            CHECK(!terrain.resolveWall(out.position, kPlayerBaseRadius, realm).collided);
            CHECK(element.padId == "bio-centre-entry" || element.padId == "bio-centre-return");
            continue;
        }
        CHECK_EQ(maps.forRealm(out.realm)->id(), std::string("computer"));
        ++exits;
    }
    CHECK_EQ(exits, 1);
    CHECK_EQ(centrePads.size(), 2u);
    World world;
    CommandBuffer commands{world};
    SpawnSystem spawning;
    spawning.worldMaps = &maps;
    spawning.mobCap = 0;
    for (int step = 0; step < 15; ++step) {
        spawning.run(world, terrain, registry, {{at, realm}}, rng,
                     50000.0 + step * kPopulationIntervalMillis, net::kTickSeconds, commands);
        commands.flush();
    }
    std::vector<SpawnSystem::LatentSite> sites;
    spawning.latentSites(realm, sites);
    std::set<std::uint16_t> species;
    std::array<int, 8> regions{};
    for (const auto& site : sites) {
        CHECK(!terrain.blocked(site.position, realm));
        species.insert(site.mobIndex);
        const auto column = static_cast<std::size_t>(site.position.x / (25 * kTileSize));
        const auto row = static_cast<std::size_t>(site.position.y / (50 * kTileSize));
        if (column < 4 && row < 2) ++regions[row * 4 + column];
    }
    std::printf("[%s] %zu sites, %zu ambient species\n", mapId, sites.size(), species.size());
    CHECK_EQ(sites.size(), expectedSites);
    for (const int count : regions) CHECK(count > 0);
    CHECK_EQ(species.size(), std::size_t(10));
    const auto stem = registry.mobIndex("bio_stem_cell");
    const auto biologist = registry.mobIndex("bio_biologist");
    CHECK_EQ(registry.mob(stem).deathSpawn.mobIndex, biologist);
    species.insert(biologist);
    for (const auto id : species) {
        const auto& entries = drops.forMob(id);
        CHECK(std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
            const auto& petal = registry.petal(entry.petalIndex);
            return petal.petMobIndex == id && petalHasOwnSource(petal);
        }));
    }
}

TEST(zorr_bio_loads_shipped_grid_and_local_drops) {
    checkBioMap("bio", 5095, 622386924u, 7169u);
    checkBioMap("bio_micro", 5251, 2220919440u, 7152u);
    checkBioMap("bio_lab", 4616, 3851817087u, 7237u);
}

TEST(zorr_bio_and_jurassic_spawn_announcements_use_the_client_language) {
    std::string error;
    CHECK(loadContent(dataDir(), error));
    const auto saved = ui::language();
    const std::string virus = "<b style=\"color: #112233;\">A Super Virus has spawned somewhere!</b>";
    const std::string raptor = "<b style=\"color: #112233;\">A Super Velociraptor has spawned!</b>";
    ui::setLanguage(ui::Language::Zh);
    CHECK_EQ(ui::localizeChatText(virus), "<b style=\"color: #112233;\">超级 病毒 已在某处生成！</b>");
    CHECK_EQ(ui::localizeChatText(raptor), "<b style=\"color: #112233;\">超级 迅猛龙 已生成！</b>");
    ui::setLanguage(ui::Language::En);
    CHECK_EQ(ui::localizeChatText(virus), virus);
    CHECK_EQ(ui::localizeChatText(raptor), raptor);
    ui::setLanguage(saved);
}

TEST(coordinate_routes_a_connected_clients_directional_frame_to_a_hatched_pet) {
    Harness h("coordinate-wire", [](const std::string& path) {
        seedUser(path, "coordinateqa", "password7");
        seedStack(path, "coordinateqa", "petal_coordinate", Rarity::Common, 1);
        seedStack(path, "coordinateqa", "petal_bio_bacteria_egg", Rarity::Common, 1);
    }, twoMapDataDir("coordinate-wire"), 0);
    NetClient client;
    if (!h.ready || !logIn(h, client, "coordinateqa") || !join(h, client, "coordinateqa", false, "meadow")) {
        CHECK(false);
        return;
    }
    World& world = h.server.world();
    const Entity owner = bodyNamed(world, "coordinateqa");
    CHECK(owner != NULL_ENTITY);
    if (owner == NULL_ENTITY) return;

    const auto& registry = shippedContent();
    const auto coordinate = registry.petalIndex("coordinate");
    const auto egg = registry.petalIndex("bio_bacteria_egg");
    client.setLoadoutSlot(0, coordinate, Rarity::Common);
    client.setLoadoutSlot(1, egg, Rarity::Common);

    Entity pet = NULL_ENTITY;
    const bool hatched = h.stepUntil({&client}, [&] {
        Query<Pet, MobType>{world}.each([&](Entity entity, const Pet& summoned, const MobType& type) {
            if (summoned.owner == owner && type.configIndex == registry.mobIndex("bio_bacteria")) pet = entity;
        });
        return pet != NULL_ENTITY && world.get<Loadout>(owner).slots[0].configIndex == coordinate;
    });
    CHECK(world.get<Health>(owner).alive());
    CHECK(hatched);
    if (!hatched) return;

    const auto original = world.get<Transform>(owner);
    Vec2 center;
    bool found = false;
    for (int y = -600; y <= 600 && !found; y += 100) {
        for (int x = -600; x <= 600 && !found; x += 100) {
            center = original.position + Vec2{double(x), double(y)};
            found = !h.server.terrain().resolveWall(center, 250, original.realm).collided;
        }
    }
    CHECK(found);
    if (!found) return;
    world.get<Transform>(owner).position = center;
    world.get<Health>(owner).invulnerableUntilMillis = h.clock + 20000.0;
    const auto start = center + Vec2{100, 0};
    world.get<Transform>(pet).position = start;
    world.get<Motion>(pet).velocity = {};

    net::InputFrame direction;
    direction.moveAngle = kPi / 2;
    direction.moveStrength = 0.5;
    direction.aimDistance = 500;
    direction.flags = net::InputPetMoveDirection | net::InputHideOutsideSquad;
    for (int tick = 0; tick < 8; ++tick) {
        ++direction.sequence;
        CHECK(client.sendInput(direction));
        h.step(1, {&client});
    }
    const auto received = world.get<PlayerInput>(owner).current.flags;
    CHECK(received & net::InputPetMoveDirection);
    CHECK((received & net::InputHideOutsideSquad) == 0);
    CHECK_EQ(world.get<Transform>(pet).realm, original.realm);
    const Vec2 end = world.get<Transform>(pet).position;
    CHECK(end.y > start.y + 2.0);
    CHECK_NEAR(end.x, start.x, 2.0);
}

TEST(zorr_bio_rna_stacks_and_dna_checks_the_hatchling_tier) {
    const auto& content = shippedContent();
    World world;
    CommandBuffer commands{world};
    PetalSystem petals;
    const Entity flower = world.create();
    world.add<PlayerTag>(flower);
    world.add<Transform>(flower, Transform{{5000, 5000}, 0});
    world.add<Body>(flower, Body{25, 1});
    world.add<Health>(flower, Health{100, 100});
    world.add<Loadout>(flower);
    world.add<PlayerModifiers>(flower);
    world.add<PetalRing>(flower);
    auto& bar = world.get<Loadout>(flower).slots;
    bar[0] = {content.petalIndex("rna"), Rarity::Common};
    bar[1] = {content.petalIndex("rna"), Rarity::Rare};
    bar[2] = {content.petalIndex("dna"), Rarity::Rare};
    petals.run(world, content, 1000, net::kTickSeconds, commands);
    commands.flush();
    const auto& modifiers = world.get<PlayerModifiers>(flower);
    CHECK_NEAR(modifiers.petMoveSpeedScale, 1.1008, 1e-12);
    CHECK_EQ(modifiers.eggMutationSources[rarityIndex(Rarity::Common)], 1);
    CHECK_EQ(modifiers.eggMutationSources[rarityIndex(Rarity::Rare)], 1);
    CHECK_EQ(modifiers.eggMutationSources[rarityIndex(Rarity::Epic)], 0);
    world.get<Loadout>(flower).slots[1] = {};
    petals.run(world, content, 1100, net::kTickSeconds, commands);
    commands.flush();
    CHECK_NEAR(world.get<PlayerModifiers>(flower).petMoveSpeedScale, 1.04, 1e-12);
}

TEST(zorr_bio_biologist_retains_stem_cell_eligibility_without_damage_credit) {
    const auto& content = shippedContent();
    const auto stemType = content.mobIndex("bio_stem_cell");
    const auto scientistType = content.mobIndex("bio_biologist");
    World world;
    Terrain terrain;
    CommandBuffer commands{world};
    SpawnSystem spawning;
    const Entity qualified = world.create();
    world.add<PlayerTag>(qualified);
    const Entity ineligible = world.create();
    world.add<PlayerTag>(ineligible);
    const Entity stem = world.create();
    world.add<MobType>(stem, MobType{stemType, Rarity::Super});
    world.add<Transform>(stem, Transform{{5000, 5000}, 0});
    world.add<Health>(stem, Health{0, 1000});
    auto& ledger = world.add<Bounty>(stem);
    ledger.credit(qualified, 100);
    ledger.credit(ineligible, 2);
    Rng rng{42};
    Entity scientist = NULL_ENTITY;
    for (int attempt = 0; attempt < 100 && scientist == NULL_ENTITY; ++attempt) {
        spawning.answerDeaths(world, terrain, content, {stem}, rng, 1000.0,
                              net::kTickSeconds, commands);
        Query<MobTag, MobType>{world}.each([&](Entity e, MobTag&, MobType& type) {
            if (type.configIndex == scientistType) scientist = e;
        });
    }
    CHECK(scientist != NULL_ENTITY);
    if (scientist == NULL_ENTITY) return;
    CHECK_EQ(world.get<MobType>(scientist).rarity, Rarity::Super);
    CHECK(world.get<Bounty>(scientist).contributors.empty());
    const auto* claims = world.tryGet<InheritedLootClaims>(scientist);
    CHECK(claims && claims->recipients.size() == 1);
    if (claims && !claims->recipients.empty()) CHECK_EQ(claims->recipients.front().player, qualified);
}

TEST(zorr_bio_acid_spills_and_diamond_shatters_only_on_break) {
    const auto& content = shippedContent();
    World world;
    CommandBuffer commands{world};
    PetalSystem petals;
    const Entity flower = world.create();
    world.add<PlayerTag>(flower);
    world.add<Transform>(flower, Transform{{5000, 5000}, 0});
    world.add<Body>(flower, Body{25, 1});
    world.add<Health>(flower, Health{100, 100});
    world.add<PlayerInput>(flower);
    world.add<Loadout>(flower);
    world.add<PlayerModifiers>(flower);
    world.add<PetalRing>(flower);
    const auto acid = content.petalIndex("acid");
    const auto diamond = content.petalIndex("diamond");
    world.get<Loadout>(flower).slots[0] = {acid, Rarity::Common};
    world.get<Loadout>(flower).slots[1] = {diamond, Rarity::Common};
    world.get<PlayerInput>(flower).current.flags = net::InputAttack;
    const auto tick = [&](double now) {
        petals.run(world, content, now, net::kTickSeconds, commands);
        commands.flush();
    };
    tick(0.0);
    tick(3000.0);
    tick(4100.0);
    tick(4600.0);
    int patches = 0;
    Query<GroundEffect>{world}.each([&](Entity, GroundEffect& field) {
        if (field.kind == GroundEffectKind::AcidSpill) {
            ++patches;
            CHECK(field.damagePerSecond > 0.0);
        }
    });
    CHECK_EQ(patches, 1);
    Entity shardSource = NULL_ENTITY;
    Query<PetalInstance>{world}.each([&](Entity e, PetalInstance& active) {
        if (active.configIndex == diamond) shardSource = e;
    });
    CHECK(shardSource != NULL_ENTITY);
    if (shardSource == NULL_ENTITY) return;
    int shards = 0;
    const auto countShards = [&] {
        shards = 0;
        Query<Projectile>{world}.each([&](Entity, Projectile& shot) {
            if (shot.petalConfigIndex == diamond) ++shards;
        });
    };
    countShards();
    CHECK_EQ(shards, 0);
    world.get<Health>(shardSource).current = 0.0;
    world.add<Dead>(shardSource);
    tick(4650.0);
    countShards();
    CHECK_EQ(shards, 6);
    tick(4700.0);
    countShards();
    CHECK_EQ(shards, 6);
}

TEST(zorr_bio_cancer_uses_one_subdivision_without_additional_rewards) {
    const auto& content = shippedContent();
    World world;
    Terrain terrain;
    CommandBuffer commands{world};
    SpawnSystem spawning;
    Rng rng{42};
    const auto type = content.mobIndex("bio_bacteria");
    const auto parent = spawning.spawnMob(world, terrain, content, type, Rarity::Rare,
                                          {5000, 5000}, Realm::Overworld, 0.0, rng);
    CHECK(parent != NULL_ENTITY);
    if (parent == NULL_ENTITY) return;
    world.add<CancerSubdivision>(parent);
    world.add<Dead>(parent);
    spawning.answerDeaths(world, terrain, content, {parent}, rng, 100.0, net::kTickSeconds, commands);
    Entity child = NULL_ENTITY;
    int copies = 0;
    Query<MobType, CancerSubdivision>{world}.each(
        [&](Entity e, MobType& mob, CancerSubdivision& subdivision) {
            if (!subdivision.spawnedCopy) return;
            ++copies;
            child = e;
            CHECK_EQ(mob.configIndex, type);
            CHECK(mob.rarity >= Rarity::Uncommon && mob.rarity <= Rarity::Epic);
            CHECK_EQ(world.get<Bounty>(e).xp, 0.0);
        });
    CHECK_EQ(copies, 1);
    if (child == NULL_ENTITY) return;
    const Entity flower = world.create();
    world.add<PlayerTag>(flower);
    world.add<Transform>(flower, Transform{{5000, 5000}, 0});
    world.get<Bounty>(child).credit(flower, world.get<Health>(child).max);
    world.add<Dead>(child);
    spawning.answerDeaths(world, terrain, content, {child}, rng, 200.0, net::kTickSeconds, commands);
    LootSystem loot;
    std::string error;
    CHECK(loot.loadTables(content, dataDir() + "/mob_drops.json", error));
    SpatialGrid grid;
    EventQueue events;
    loot.run(world, grid, content, rng, 200.0, net::kTickSeconds, commands, events);
    commands.flush();
    CHECK(world.has<LootAwarded>(child));
    Query<DropTag>{world}.each([&](Entity, DropTag&) { CHECK(false); });
    copies = 0;
    Query<CancerSubdivision>{world}.each([&](Entity, CancerSubdivision& state) {
        if (state.spawnedCopy) ++copies;
    });
    CHECK_EQ(copies, 1);
}

TEST(zorr_bio_cancer_is_consumed_and_pill_treats_it_at_full_health) {
    const auto& content = shippedContent();
    World world;
    CommandBuffer commands{world};
    PetalSystem petals;
    const Entity flower = world.create();
    world.add<PlayerTag>(flower);
    world.add<Transform>(flower, Transform{{5000, 5000}, 0});
    world.add<Body>(flower, Body{25, 1});
    world.add<Health>(flower, Health{100, 100});
    world.add<PlayerInput>(flower);
    world.add<Loadout>(flower);
    world.add<PlayerModifiers>(flower);
    world.add<PetalRing>(flower);
    world.get<Loadout>(flower).slots[0] = {content.petalIndex("cancer"), Rarity::Common};
    const auto tick = [&](double now) {
        petals.run(world, content, now, net::kTickSeconds, commands);
        commands.flush();
    };
    tick(0.0);
    tick(4000.0);
    tick(14100.0);
    tick(14200.0);
    const auto* status = world.tryGet<Afflictions>(flower);
    CHECK(status && status->cancer == 3.0);
    const double reduced = world.get<Health>(flower).max;
    world.get<Loadout>(flower).slots[0] = {content.petalIndex("pill"), Rarity::Rare};
    for (int t = 14300; t < 22000; t += 100) tick(t);
    CHECK_EQ(world.get<Afflictions>(flower).cancer, 0.0);
    CHECK(world.get<Health>(flower).max > reduced);
}

TEST(zorr_bio_blood_patches_strip_wild_mob_armor_and_chloroplast_scales_shields) {
    const auto& content = shippedContent();
    World world;
    CommandBuffer commands{world};
    EventQueue events;
    PetalSystem petals;
    CombatSystem combat;
    const Entity flower = world.create();
    world.add<PlayerTag>(flower);
    world.add<Transform>(flower, Transform{{5000, 5000}, 0});
    world.add<Body>(flower, Body{25, 1});
    world.add<Health>(flower, Health{100, 100});
    world.add<PlayerInput>(flower);
    world.add<Loadout>(flower);
    world.add<PlayerModifiers>(flower);
    world.add<PetalRing>(flower);
    world.add<Faction>(flower, Faction{Team::Players, false});
    auto& loadout = world.get<Loadout>(flower);
    loadout.slots[0] = {content.petalIndex("blood"), Rarity::Common};
    loadout.slots[1] = {content.petalIndex("chloroplast"), Rarity::Rare};
    world.get<PlayerInput>(flower).current.flags = net::InputAttack;
    for (const double now : {0.0, 3000.0, 4100.0}) {
        petals.run(world, content, now, net::kTickSeconds, commands);
        commands.flush();
    }
    CHECK_NEAR(world.get<PlayerModifiers>(flower).chloroplastShieldScale, 1.35, 1e-9);
    Vec2 at;
    int patches = 0;
    Query<GroundEffect, Transform>{world}.each([&](Entity, GroundEffect& field, Transform& t) {
        if (field.kind == GroundEffectKind::Blood) { at = t.position; ++patches; }
    });
    CHECK_EQ(patches, 1);
    if (patches != 1) return;
    const Entity mob = world.create();
    world.add<MobTag>(mob);
    world.add<MobType>(mob, MobType{content.mobIndex("bio_bacteria"), Rarity::Common});
    world.add<Transform>(mob, Transform{at, 0.0});
    world.add<Body>(mob, Body{25, 1});
    world.add<Health>(mob, Health{100, 100});
    world.add<Faction>(mob, Faction{Team::Hostiles, false});
    world.add<Armor>(mob, Armor{10});
    SpatialGrid grid;
    grid.insert(mob, Realm::Overworld, at, 25);
    combat.beginTick(world, content, 4110.0, net::kTickSeconds, events);
    combat.runWorldPhase(world, grid, content, 4110.0, net::kTickSeconds);
    CHECK(CombatSystem::effectiveArmor(world, mob, 4110.0) < 10.0);
    CHECK_EQ(world.get<Health>(mob).current, 100.0);
}

TEST(zorr_bio_job_application_suppresses_the_chasing_head_on_first_tail_contact) {
    std::string error;
    CHECK(loadContent(dataDir(), error));
    if (!error.empty()) return;
    const auto& registry = shippedContent();
    for (auto [chain, headRarity] : {std::pair{false, Rarity::Common},
                                    std::pair{false, Rarity::Rare},
                                    std::pair{true, Rarity::Common},
                                    std::pair{true, Rarity::Rare}}) {
        World world;
        CombatSystem combat;
        EventQueue events;
        CommandBuffer commands{world};
        SpatialGrid grid;
        const Vec2 at{5100, 5000};
        const Entity player = world.create();
        world.add<PlayerTag>(player);
        world.add<Transform>(player, Transform{{4900, 5000}, 0});
        world.add<Faction>(player, Faction{Team::Players, false});
        world.add<Health>(player, Health{100, 100});
        world.add<Body>(player, Body{25, 1});
        grid.insert(player, Realm::Overworld, {4900, 5000}, 25);
        const auto mob = [&](Vec2 position, Rarity rarity) {
            const Entity e = world.create();
            world.add<MobTag>(e);
            world.add<MobType>(e, MobType{registry.mobIndex("centipede"), rarity});
            world.add<Transform>(e, Transform{position, 0});
            world.add<Health>(e, Health{100, 100});
            world.add<Body>(e, Body{24, 1});
            world.add<Faction>(e, Faction{Team::Hostiles, false});
            world.add<Bounty>(e);
            grid.insert(e, Realm::Overworld, position, 24);
            return e;
        };
        const Vec2 headPosition = chain ? Vec2{5200, 5000} : at;
        const Entity head = mob(headPosition, headRarity);
        const Entity tail = chain ? mob(at, Rarity::Common) : head;
        MobAi ai;
        ai.kind = AiKind::Hostile;
        ai.aggroRange = 800.0;
        ai.target = player;
        ai.unseenTarget = player;
        world.add<MobAi>(head, ai);
        BodySegment link;
        link.ahead = head;
        link.chainHead = head;
        if (chain) world.add<BodySegment>(tail, link);
        world.add<Motion>(head);

        const Entity petal = world.create();
        PetalInstance instance;
        instance.owner = player;
        instance.configIndex = registry.petalIndex("job_application");
        world.add<PetalInstance>(petal, instance);
        world.add<Transform>(petal, Transform{at + Vec2{10, 0}, 0});
        world.add<Body>(petal, Body{10, 1});
        world.add<Health>(petal, Health{100, 100});
        world.add<Faction>(petal, Faction{Team::Players, false});

        combat.run(world, grid, registry, 1000.0, net::kTickSeconds, commands, events);
        commands.flush();
        const bool eligible = headRarity == Rarity::Common;
        const auto& brain = world.get<MobAi>(head);
        CHECK_EQ(brain.aggroSuppressedUntilMillis, eligible ? 6000.0 : 0.0);
        CHECK_EQ(brain.target, eligible ? NULL_ENTITY : player);
        CHECK_EQ(brain.unseenTarget, eligible ? NULL_ENTITY : player);
        CHECK_EQ(world.get<Health>(tail).current, 100.0);
        if (!eligible) continue;

        combat.run(world, grid, registry, 2000.0, net::kTickSeconds, commands, events);
        commands.flush();
        CHECK_EQ(world.get<MobAi>(head).aggroSuppressedUntilMillis, 7000.0);
        world.destroy(petal);
        Terrain terrain;
        MobAiSystem aiSystem(world);
        const std::vector<RealmPoint> players{{{4900, 5000}, Realm::Overworld}};
        aiSystem.run(world, terrain, grid, players, 6999.0, net::kTickSeconds, commands);
        commands.flush();
        CHECK_EQ(world.get<MobAi>(head).target, NULL_ENTITY);
        aiSystem.run(world, terrain, grid, players, 7000.0, net::kTickSeconds, commands);
        commands.flush();
        CHECK_EQ(world.get<MobAi>(head).target, player);
    }
}

TEST(zorr_bio_oxygen_extends_burning_without_creating_a_second_damage_source) {
    const auto& content = shippedContent();
    World world;
    CombatSystem combat;
    EventQueue events;
    CommandBuffer commands{world};
    SpatialGrid grid;
    const Vec2 at{5100, 5000};

    const Entity flower = world.create();
    world.add<PlayerTag>(flower);
    world.add<Transform>(flower, Transform{{5000, 5000}, 0});
    world.add<Faction>(flower, Faction{Team::Players, false});
    const Entity mob = world.create();
    world.add<MobTag>(mob);
    world.add<MobType>(mob, MobType{content.mobIndex("bio_bacteria"), Rarity::Common});
    world.add<Transform>(mob, Transform{at, 0});
    world.add<Health>(mob, Health{100, 100});
    world.add<Body>(mob, Body{24, 1});
    world.add<Faction>(mob, Faction{Team::Hostiles, false});
    world.add<Bounty>(mob);
    grid.insert(mob, Realm::Overworld, at, 24);

    const Entity oxygen = world.create();
    PetalInstance instance;
    instance.owner = flower;
    instance.configIndex = content.petalIndex("oxygen");
    world.add<PetalInstance>(oxygen, instance);
    world.add<Transform>(oxygen, Transform{at + Vec2{15, 0}, 0});
    world.add<Body>(oxygen, Body{10, 1});
    world.add<Health>(oxygen, Health{30, 30});
    world.add<Faction>(oxygen, Faction{Team::Players, false});
    const auto tick = [&](double now) {
        combat.run(world, grid, content, now, net::kTickSeconds, commands, events);
        commands.flush();
    };
    tick(1000.0);
    CHECK_EQ(world.get<Health>(mob).current, 100.0);
    const auto* before = world.tryGet<Afflictions>(mob);
    CHECK(!before || before->burnPerSecond == 0.0);

    const Entity fire = world.create();
    world.add<GroundEffectTag>(fire);
    world.add<Transform>(fire, Transform{at, 0});
    GroundEffect field;
    field.kind = GroundEffectKind::Fire;
    field.owner = flower;
    field.radius = 50.0;
    field.burnPerSecond = 4.0;
    world.add<GroundEffect>(fire, field);
    world.add<Lifetime>(fire, Lifetime{.20});
    tick(2000.0);
    CHECK(world.has<Afflictions>(mob));
    if (!world.has<Afflictions>(mob)) return;
    const auto first = world.get<Afflictions>(mob);
    CHECK_EQ(first.burnSource, flower);
    CHECK_EQ(first.burnUntilMillis, 5000.0);
    world.destroy(fire);
    tick(3050.0);
    CHECK_EQ(world.get<Afflictions>(mob).burnUntilMillis, 5100.0);
    CHECK_EQ(world.get<Afflictions>(mob).burnPerSecond, 4.0);
    CHECK_NEAR(world.get<Health>(mob).current, 99.8, 1e-8);
    CHECK(!world.get<Bounty>(mob).contributors.empty());
    world.destroy(oxygen);
    tick(5200.0);
    CHECK_EQ(world.get<Afflictions>(mob).burnPerSecond, 0.0);
    CHECK_NEAR(world.get<Health>(mob).current, 99.8, 1e-8);
}

TEST(zorr_bio_fire_and_fire_bomb_use_the_shared_burn_field) {
    const auto& content = shippedContent();
    const auto fire = content.petalIndex("fire");
    const auto bomb = content.petalIndex("fire_bomb");
    const auto missile = content.petalIndex("electron_missile");
    const auto magic = content.petalIndex("magic_electron_missile");
    CHECK_EQ(content.magicFormOf(missile), magic);
    CHECK(content.petalStats(magic, Rarity::Common).requiredMana > 0.0);
    World world;
    CommandBuffer commands{world};
    PetalSystem petals;
    const Entity flower = world.create();
    world.add<PlayerTag>(flower);
    world.add<Transform>(flower, Transform{{5000, 5000}, 0});
    world.add<Body>(flower, Body{25, 1});
    world.add<Health>(flower, Health{100, 100});
    world.add<PlayerInput>(flower);
    world.add<Loadout>(flower);
    world.add<PlayerModifiers>(flower);
    world.add<PetalRing>(flower);
    world.get<Loadout>(flower).slots[0] = {fire, Rarity::Common};
    world.get<Loadout>(flower).slots[1] = {bomb, Rarity::Common};
    world.get<PlayerInput>(flower).current.flags = net::InputAttack;
    const auto tick = [&](double now) {
        petals.run(world, content, now, net::kTickSeconds, commands);
        commands.flush();
    };
    tick(0.0);
    tick(3000.0);
    tick(5100.0);
    int fields = 0;
    const auto countFields = [&] {
        fields = 0;
        Query<GroundEffect>{world}.each([&](Entity, GroundEffect& field) {
            if (field.kind == GroundEffectKind::Fire) {
                CHECK_EQ(field.owner, flower);
                CHECK(field.burnPerSecond > 0.0);
                ++fields;
            }
        });
    };
    countFields();
    CHECK_EQ(fields, 1);
    Entity shell = NULL_ENTITY;
    Query<PetalInstance>{world}.each([&](Entity e, PetalInstance& petal) {
        if (petal.configIndex == bomb) shell = e;
    });
    CHECK(shell != NULL_ENTITY);
    if (shell == NULL_ENTITY) return;
    world.get<Health>(shell).current = 0.0;
    world.add<Dead>(shell);
    tick(5150.0);
    countFields();
    CHECK_EQ(fields, 2);
}

#ifndef __EMSCRIPTEN__
TEST(zorr_bio_micro_and_lab_centre_pads_round_trip_without_bounce) {
    Harness h("bio-centre", [](const std::string& path) {
        seedUser(path, "biocentre", "password7");
    }, dataDir(), 0);
    NetClient client;
    CHECK(h.ready && logIn(h, client, "biocentre"));
    if (client.status() != NetClient::Status::LoggedIn) return;
    for (const char* id : {"bio_micro", "bio_lab"}) {
        const bool joined = join(h, client, "biocentre", false, id);
        CHECK(joined);
        if (!joined) return;
        World& world = h.server.world();
        const Entity player = bodyNamed(world, "biocentre");
        CHECK(player != NULL_ENTITY);
        if (player == NULL_ENTITY) return;
        world.get<Health>(player).invulnerableUntilMillis = h.clock + 30000;
        const Realm realm = realmOf(world, player);
        const auto& maps = h.server.worldMaps();
        const auto& terrain = h.server.terrain();
        const auto* map = maps.forRealm(realm);
        CHECK(map && map->id() == id);
        if (!map || map->id() != id) return;
        const MapElement* entry = nullptr;
        const MapElement* centre = nullptr;
        for (const auto& element : map->elements()) {
            if (element.padId == "bio-centre-entry") entry = &element;
            if (element.padId == "bio-centre-return") centre = &element;
        }
        CHECK(entry && centre);
        if (!entry || !centre) return;
        for (const auto* pad : {entry, centre})
            CHECK(!terrain.resolveWall(pad->centre(), kPlayerBaseRadius, realm).collided);
        const auto nearby = [&](const MapElement& pad) {
            return distanceSq(world.get<Transform>(player).position, pad.centre()) < 250.0 * 250.0;
        };
        const auto take = [&](const MapElement& from, const MapElement& to) {
            for (int tick = 0; tick < 100; ++tick) {
                world.get<Transform>(player).position = from.centre();
                h.step(1, {&client});
                if (nearby(to)) return true;
            }
            return false;
        };
        CHECK(take(*entry, *centre));
        for (int tick = 0; tick < 140 && nearby(*centre); ++tick) {
            world.get<Transform>(player).position = centre->centre();
            h.step(1, {&client});
        }
        CHECK(nearby(*centre));
        const Vec2 rest = centre->centre() + Vec2{90.0, 0.0};
        CHECK(!terrain.resolveWall(rest, kPlayerBaseRadius, realm).collided);
        world.get<Transform>(player).position = rest;
        h.step(3, {&client});
        CHECK(take(*centre, *entry));
        CHECK(realmOf(world, player) == realm && client.view().realm() == realm);
        std::printf("[bio centre] %s entry=%.0f,%.0f centre=%.0f,%.0f\n", id,
            entry->centre().x, entry->centre().y, centre->centre().x, centre->centre().y);
        client.leaveGame();
        h.step(10, {&client});
    }
}

TEST(zorr_bio_fire_burns_a_mob_through_a_connected_client_loadout) {
    Harness h("bio-fire", [](const std::string& path) {
        seedUser(path, "biofire", "password7");
        seedStack(path, "biofire", "petal_fire", Rarity::Common, 1);
    }, dataDir(), 0);
    NetClient client;
    if (!h.ready || !logIn(h, client, "biofire") || !join(h, client, "biofire", false, "bio")) {
        CHECK(false);
        return;
    }
    World& world = h.server.world();
    const Entity flower = bodyNamed(world, "biofire");
    CHECK(flower != NULL_ENTITY);
    if (flower == NULL_ENTITY) return;
    const auto& registry = shippedContent();
    const auto fire = registry.petalIndex("fire");
    client.setLoadoutSlot(0, fire, Rarity::Common);
    const bool equipped = h.stepUntil({&client}, [&] {
        return world.get<Loadout>(flower).slots[0].configIndex == fire;
    });
    CHECK(equipped);
    if (!equipped) return;

    const auto spawn = world.get<Transform>(flower);
    auto origin = spawn;
    bool clear = false;
    for (int y = -800; y <= 800 && !clear; y += 50) {
        for (int x = -800; x <= 800 && !clear; x += 50) {
            const Vec2 point = spawn.position + Vec2{double(x), double(y)};
            const auto* bio = h.server.worldMaps().forRealm(spawn.realm);
            if (!bio || h.server.terrain().blocked(point,spawn.realm) ||
                std::any_of(bio->elements().begin(),bio->elements().end(),[&](const MapElement& e) {
                    return e.kind == MapElementKind::Teleporter &&
                           distanceSq(point,e.centre()) < std::pow(kTeleporterSuctionRadius * e.padScale() + 180,2);
                }) || h.server.terrain().resolveWall(point,150,spawn.realm).collided) continue;
            origin.position = point;
            clear = true;
        }
    }
    CHECK(clear);
    if (!clear) return;
    world.get<Transform>(flower) = origin;
    world.get<Health>(flower).invulnerableUntilMillis = h.clock + 20000.0;
    const Entity target = world.create();
    world.add<MobTag>(target);
    world.add<Bounty>(target);
    world.add<Transform>(target, Transform{origin.position + Vec2{0, 100}, 0, origin.realm});
    world.add<Body>(target, Body{32, 1000});
    world.add<Health>(target, Health{10000, 10000});
    world.add<Faction>(target, Faction{Team::Hostiles, false});

    net::InputFrame attack;
    attack.flags = net::InputDefend;
    bool sawFire = false;
    for (int i = 0; i < 220; ++i) {
        // The test checks a single burn target. Keep ambient Bio mobs from
        // pushing the invulnerable QA flower and breaking the firing petal.
        std::vector<Entity> ambient;
        Query<MobTag>{world}.collect(ambient);
        for (const Entity mob : ambient) {
            if (mob != target) world.destroy(mob);
        }
        Query<PetalInstance, Transform>{world}.each(
            [&](Entity e, const PetalInstance& petal, const Transform& at) {
                if (petal.owner != flower || petal.configIndex != fire || world.has<Dead>(e)) return;
                sawFire = true;
                world.get<Transform>(target) = at;
            });
        attack.sequence++;
        CHECK(client.sendInput(attack));
        h.step(1, {&client});
        const auto* status = world.tryGet<Afflictions>(target);
        if (status && !status->burns.empty()) break;
    }
    CHECK(sawFire);
    const auto* burning = world.tryGet<Afflictions>(target);
    CHECK(burning && std::any_of(burning->burns.begin(),burning->burns.end(),
        [&](const auto& burn){return burn.owner == flower && burn.perSecond > 0;}));
    world.get<Transform>(target).position += Vec2{250,0};
    const double before = world.get<Health>(target).current;
    h.step(10, {&client});
    CHECK(world.get<Health>(target).current < before);
}

TEST(assembler_output_capacity_handles_full_partial_and_reused_stacks) {
    CHECK_EQ(assemblerOutputCapacity(kMaxStackCount, 2, 0), 0);
    CHECK_EQ(assemblerOutputCapacity(kMaxStackCount - 5, 2, 0), 2);
    CHECK_EQ(assemblerOutputCapacity(kMaxStackCount - 5, 3, 2), 5);
    CHECK_EQ(assemblerOutputCapacity(kMaxStackCount, 2, 2), kMaxStackCount / 2);
    CHECK_EQ(assemblerOutputCapacity(0, kMaxStackCount + 1LL, 0), 0);
}

TEST(zorr_bio_assembler_rotates_at_the_hour_and_enforces_atomic_account_cooldown) {
    static double epoch = 24.0 * kBioAssemblerRotationMillis + 10.0;
    epoch = 24.0 * kBioAssemblerRotationMillis + 10.0;
    Harness h("bio-assembler", [](const std::string& path) {
        seedUser(path, "biobuilder", "password7");
        seedStack(path, "biobuilder", "petal_rna", Rarity::Common, 6);
        seedStack(path, "biobuilder", "petal_moon_rock", Rarity::Ultra, 2);
    }, dataDir(), 0, [](ServerConfig& config) {
        config.assemblerClock = [] { return epoch; };
    });
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient client;
    if (!logIn(h, client, "biobuilder")) { CHECK(false); return; }
    if (!join(h, client, "biobuilder", false, "bio")) { CHECK(false); return; }

    World& world = h.server.world();
    const Entity flower = bodyNamed(world, "biobuilder");
    CHECK(flower != NULL_ENTITY);
    if (flower == NULL_ENTITY) return;
    const auto realm = world.get<Transform>(flower).realm;
    const auto findMachine = [&] {
        Entity found = NULL_ENTITY;
        Query<Npc, Transform>{world}.each([&](Entity e, Npc& npc, Transform& at) {
            if (at.realm == realm && npc.service == NpcService::Assembler) found = e;
        });
        return found;
    };
    const Entity station = findMachine();
    CHECK(station != NULL_ENTITY);
    if (station == NULL_ENTITY) return;
    const auto& registry = shippedContent();
    const auto rna = registry.petalIndex("rna");
    const auto dna = registry.petalIndex("dna");
    const auto& spec = registry.mob(world.get<Npc>(station).configIndex).npc;
    CHECK_EQ(spec.hourlyRecipeSlots, 49);
    CHECK_EQ(spec.recipes.size(), std::size_t{49});
    for (int slot = 0; slot < 49; ++slot)
        CHECK_EQ(assemblyRecipeAtSlot(spec, slot), static_cast<std::uint8_t>(slot));
    CHECK(!spec.recipes[10].previewOnly);
    CHECK_EQ(spec.recipes[48].inputs.front().petal, registry.petalIndex("moon_rock"));
    CHECK_EQ(spec.recipes[48].outputs.front().petal, registry.petalIndex("rock"));
    CHECK_EQ(spec.recipes[48].outputs.front().count, 15);
    const std::uint8_t recipe = world.get<Npc>(station).recipe;
    CHECK(recipe < spec.recipes.size());
    if (recipe >= spec.recipes.size()) return;
    CHECK_EQ(spec.recipes[recipe].rotationSlot, 24);
    CHECK_EQ(spec.recipes[world.get<Npc>(station).nextRecipe].rotationSlot, 25);
    CHECK_EQ(hourlyAssemblyRecipe(spec, 10.0 * kBioAssemblerRotationMillis), 10);
    CHECK_EQ(hourlyAssemblyRecipe(spec, (24.0 + 49.0) * kBioAssemblerRotationMillis), recipe);

    const Vec2 centre = world.get<Transform>(station).position;
    world.get<Transform>(flower).position = centre + Vec2{0, 70};
    const auto assemble = [&](std::uint16_t count) {
        client.assembleOutcome().pending = false;
        client.requestAssemble(Rarity::Common, recipe, count);
        CHECK(h.stepUntil({&client}, [&] { return client.assembleOutcome().pending; }, 200));
        return client.assembleOutcome();
    };
    CHECK(!assemble(4).success);
    CHECK_EQ(client.profile().stackCount(rna, Rarity::Common), 6u);
    CHECK(assemble(1).success);
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stackCount(rna, Rarity::Common) == 4u && p.stackCount(dna, Rarity::Common) == 1u;
    }));
    const auto refused = assemble(1);
    CHECK(!refused.success);
    CHECK_EQ(refused.reason, std::string("The Bio assembler is cooling down."));
    CHECK_EQ(client.profile().stackCount(rna, Rarity::Common), 4u);
    epoch += kBioAssemblerCooldownMillis;
    CHECK(assemble(1).success);
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stackCount(rna, Rarity::Common) == 2u && p.stackCount(dna, Rarity::Common) == 2u;
    }));
    epoch = 25.0 * kBioAssemblerRotationMillis;
    CHECK(!assemble(1).success);
    CHECK_EQ(client.profile().stackCount(rna, Rarity::Common), 2u);
    const Entity previousStation = findMachine();
    CHECK(previousStation != NULL_ENTITY);
    if (previousStation == NULL_ENTITY) return;
    world.get<Npc>(previousStation).recipeRefreshAtMillis = 1.0;
    epoch = 48.0 * kBioAssemblerRotationMillis;
    CHECK(h.stepUntil({&client}, [&] {
        const auto current = findMachine();
        return current != NULL_ENTITY && world.get<Npc>(current).recipe == 48;
    }, 200));
    const auto makeMoon = [&](Rarity tier) {
        client.assembleOutcome().pending = false;
        client.requestAssemble(tier, 48, 1);
        CHECK(h.stepUntil({&client}, [&] { return client.assembleOutcome().pending; }, 200));
        return client.assembleOutcome().success;
    };
    CHECK(!makeMoon(Rarity::Common));
    CHECK(makeMoon(Rarity::Ultra));
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stackCount(registry.petalIndex("moon_rock"), Rarity::Ultra) == 1u &&
               p.stackCount(registry.petalIndex("rock"), Rarity::Ultra) == 15u;
    }));
}

TEST(zorr_bio_new_assembler_slots_trade_real_inventory) {
    static double epoch;
    epoch = 4 * kBioAssemblerRotationMillis + 50.0;
    Harness h("bio-new-slots", [](const std::string& path) {
        seedUser(path, "slotbuilder", "password7");
        seedStack(path, "slotbuilder", "petal_bomb", Rarity::Super, 3);
        seedStack(path, "slotbuilder", "petal_arctic_icicle", Rarity::Super, 6);
        seedStack(path, "slotbuilder", "petal_mysterious_stick", Rarity::Super, 6);
        seedStack(path, "slotbuilder", "petal_void_knife", Rarity::Super, 4);
        seedStack(path, "slotbuilder", "petal_ring", Rarity::Super, 2);
    }, dataDir(), 0, [&](ServerConfig& config) {
        config.assemblerClock = [] { return epoch; };
    });
    NetClient client;
    CHECK(h.ready);
    if (!h.ready || !logIn(h, client, "slotbuilder") ||
        !join(h, client, "slotbuilder", false, "bio")) { CHECK(false); return; }
    auto& world = h.server.world();
    const Entity flower = bodyNamed(world, "slotbuilder");
    CHECK(flower != NULL_ENTITY);
    if (flower == NULL_ENTITY) return;
    const auto findMachine = [&] {
        Entity station = NULL_ENTITY;
        Query<Npc, Transform>{world}.each([&](Entity e, Npc& npc, Transform& at) {
            if (at.realm == world.get<Transform>(flower).realm &&
                npc.service == NpcService::Assembler) station = e;
        });
        return station;
    };
    auto count = [&](const char* id, unsigned n) {
        return client.profile().stackCount(shippedContent().petalIndex(id), Rarity::Super) == n;
    };
    for (const auto slot : {4, 14, 17, 34}) {
        epoch = slot * kBioAssemblerRotationMillis + 50.0;
        const Entity before = findMachine();
        CHECK(before != NULL_ENTITY);
        if (before == NULL_ENTITY) return;
        if (world.get<Npc>(before).recipe != slot) world.get<Npc>(before).recipeRefreshAtMillis = 1;
        if (!h.stepUntil({&client}, [&] {
            const Entity current = findMachine();
            return current != NULL_ENTITY && world.get<Npc>(current).recipe == slot;
        }, 200)) { CHECK(false); return; }
        const auto station = findMachine();
        world.get<Transform>(flower).position = world.get<Transform>(station).position + Vec2{0,70};
        client.assembleOutcome().pending = false;
        client.requestAssemble(Rarity::Super, static_cast<std::uint8_t>(slot), 2);
        CHECK(h.stepUntil({&client}, [&] { return client.assembleOutcome().pending; }, 200));
        CHECK(client.assembleOutcome().success);
        CHECK(awaitProfile(h, client, [&](const Profile&) {
            if (slot == 4) return count("bomb", 1) && count("arctic_icicle", 2) && count("arctic_cryo_bomb", 2);
            if (slot == 14) return count("ring", 0) && count("diamond", 2) && count("ring_magnet", 2);
            if (slot == 17) return count("mysterious_stick", 2) && count("plank", 2);
            return count("mysterious_stick", 0) && count("void_knife", 0) && count("void_scythe", 2);
        }));
    }
}


TEST(zorr_all_imported_petals_equip_and_run_through_a_connected_client) {
    const auto& registry = shippedContent();
    static const std::vector<std::string> variants{
        "magic_agarbatti", "magic_air", "magic_antennae", "magic_arctic_banana",
        "magic_arctic_blueberries", "magic_arctic_emerald", "magic_arctic_fission", "magic_arctic_ice_rose",
        "magic_arctic_nitro", "magic_arctic_nitrogen", "magic_arctic_sapphire", "magic_arctic_splitter",
        "magic_aztec_coin", "magic_basil", "magic_battery", "magic_bitcoin",
        "magic_blood_sacrifice", "magic_blood_stinger", "magic_bloom", "magic_bomb",
        "magic_bone", "magic_bubble", "magic_bubble_bomb", "magic_bubonic_plague",
        "magic_bur", "magic_cactus", "magic_cancer", "magic_candlestick",
        "magic_cherry", "magic_chromosome", "magic_citron", "magic_claw",
        "magic_compass", "magic_coral", "magic_corruption", "magic_cotton",
        "magic_crystal_leaf", "magic_cutter", "magic_dahlia", "magic_dice",
        "magic_dna", "magic_dollar", "magic_electron_missile", "magic_fang",
        "magic_fire", "magic_fries", "magic_gas", "magic_gas_bomb",
        "magic_golden_leaf", "magic_grapes", "magic_hel_lightning", "magic_hel_missile",
        "magic_hemoglobin", "magic_honey", "magic_iodine", "magic_iris",
        "magic_job_application", "magic_latrine", "magic_leaf", "magic_lightning",
        "magic_lily_pad", "magic_marble", "magic_microplastics", "magic_mimic",
        "magic_missile", "magic_mysterious_relic", "magic_nazar_amulet", "magic_neurotoxin",
        "magic_orange", "magic_palm_leaf", "magic_pearl", "magic_peas",
        "magic_pill", "magic_pollen", "magic_poo", "magic_pumpkin_seed",
        "magic_quartz", "magic_rna", "magic_rock", "magic_rocket",
        "magic_rubber", "magic_ruby", "magic_salt", "magic_shovel",
        "magic_soil", "magic_sponge", "magic_starfish", "magic_stinger",
        "magic_sunflower", "magic_tomato", "magic_tooth", "magic_topaz",
        "magic_torch", "magic_usain_bolt", "magic_vampire_fang", "magic_viper",
        "magic_void_antimatter", "magic_void_balloon", "magic_void_blood_scythe", "magic_void_copper_rod",
        "magic_void_cryo_bomb", "magic_void_fire_bomb", "magic_void_gunpowder", "magic_void_halo",
        "magic_void_mask", "magic_void_nut", "magic_void_opal", "magic_void_pipe_bomb",
        "magic_void_relativity", "magic_void_sad", "magic_void_scar", "magic_void_scythe",
        "magic_void_singularity", "magic_void_slime", "magic_void_stardust", "magic_void_trident",
        "magic_void_zodiac", "magic_web", "magic_wing", "magic_yggdrasil",
        "magic_yuan", "mana_orb",
    };
    for (const auto& id : variants) {
        const auto index = registry.petalIndex(id);
        CHECK(index != kInvalidIndex);
        if (index == kInvalidIndex) return;
        for (int tier = 0; tier < kRarityCount; ++tier) {
            const auto stats = registry.petalStats(index,clampRarity(tier));
            CHECK(std::isfinite(stats.damage) && stats.damage >= 0);
            CHECK(std::isfinite(stats.requiredMana) && stats.requiredMana >= 0);
        }
    }
    const auto imported = [](const PetalConfig& item) {
        return std::binary_search(variants.begin(), variants.end(), item.id) || item.biomeDropOnly || item.id.rfind("void_", 0) == 0 ||
            item.id.rfind("arctic_", 0) == 0 || item.id.rfind("bio_", 0) == 0 ||
            item.id.rfind("jurassic_", 0) == 0;
    };
    Harness h("zorr-all-equip", [&](const std::string& path) {
        seedUser(path, "allpetals", "password7");
        for (std::uint16_t i = 0; i < registry.petalCount(); ++i)
            if (imported(registry.petal(i)))
                seedStack(path, "allpetals", ("petal_" + registry.petal(i).id).c_str(), Rarity::Super, 100);
    }, dataDir(), 0);
    NetClient client;
    if (!h.ready || !logIn(h, client, "allpetals") ||
        !join(h, client, "allpetals", false, "bio")) { CHECK(false); return; }
    auto& world = h.server.world();
    const Entity flower = bodyNamed(world, "allpetals");
    CHECK(flower != NULL_ENTITY);
    if (flower == NULL_ENTITY) return;
    const auto orb = registry.petalIndex("mana_orb");
    client.setLoadoutSlot(1, orb, Rarity::Super);
    CHECK(h.stepUntil({&client}, [&] { return world.get<Loadout>(flower).slots[1].configIndex == orb; }, 100));
    const Transform start = world.get<Transform>(flower);
    int magicChecked = 0;
    net::InputFrame input;
    int checked = 0;
    for (std::uint16_t i = 0; i < registry.petalCount(); ++i) {
        if (!imported(registry.petal(i))) continue;
        world.get<Health>(flower).invulnerableUntilMillis = 1e12;
        world.get<Transform>(flower) = start;
        client.setLoadoutSlot(0, i, Rarity::Super);
        const bool accepted = h.stepUntil({&client}, [&] {
            return !client.profile().loadout.empty() &&
                client.profile().loadout[0].petalIndex == i &&
                world.get<Loadout>(flower).slots[0].configIndex == i;
        }, 100);
        if (!accepted) std::printf("  equip refused: %s\n", registry.petal(i).id.c_str());
        CHECK(accepted);
        if (!accepted) return;
        // Real elapsed simulation time lets a newly equipped slot finish its
        // normal reload. No private petal system or mocked packet handler.
        h.clock += 20000;
        for (int step = 0; step < 12; ++step) {
            input.sequence++;
            input.flags = step < 6 ? net::InputAttack : net::InputDefend;
            CHECK(client.sendInput(input));
            h.step(1, {&client});
            CHECK(client.status() == NetClient::Status::Playing);
        }
        Query<Transform>{world}.each([](Entity, const Transform& at) {
            CHECK(std::isfinite(at.position.x)); CHECK(std::isfinite(at.position.y));
        });
        if (std::binary_search(variants.begin(), variants.end(), registry.petal(i).id)) ++magicChecked;
        ++checked;
    }
    CHECK_EQ(magicChecked, 122);
    CHECK(checked >= 219);
    std::printf("  %d imported petals: real client equip acknowledgement and attack/defend ticks\n", checked);
}

TEST(zorr_all_bio_slots_and_native_base_routes_settle_real_inventory) {
 static double epoch=0;
 const auto& registry=shippedContent();
 Harness h("all-zorr-recipes",[&](const std::string& path){
  seedUser(path,"allrecipebuilder","password7");
  for(std::size_t i=0;i<registry.petalCount();++i)
   seedStack(path,"allrecipebuilder",("petal_"+registry.petal(static_cast<std::uint16_t>(i)).id).c_str(),Rarity::Super,10000);
 },dataDir(),0,[&](ServerConfig& config){config.assemblerClock=[] {return epoch;};});
 NetClient client;CHECK(h.ready);
 if(!h.ready || !logIn(h,client,"allrecipebuilder") || !join(h,client,"allrecipebuilder",false,"bio")){CHECK(false);return;}
 auto& world=h.server.world();const Entity flower=bodyNamed(world,"allrecipebuilder");CHECK(flower!=NULL_ENTITY);if(flower==NULL_ENTITY)return;
 world.get<Health>(flower).invulnerableUntilMillis=1e12;
 auto findStation=[&](){Entity station=NULL_ENTITY;Query<Npc,Transform>{world}.each([&](Entity e,Npc& npc,Transform& at){if(at.realm==world.get<Transform>(flower).realm&&npc.service==NpcService::Assembler)station=e;});return station;};
 const auto& bio=registry.mob(registry.mobIndex("bio_assembler")).npc;CHECK_EQ(bio.recipes.size(),49);
 const auto exchange=[&](const AssemblyRecipe& recipe,std::uint8_t index,Entity station){
  std::map<std::uint16_t,std::int64_t> expected;
  for(const auto& item:recipe.inputs)expected[item.petal]=client.profile().stackCount(item.petal,Rarity::Super);
  for(const auto& item:recipe.outputs)expected[item.petal]=client.profile().stackCount(item.petal,Rarity::Super);
  for(const auto& item:recipe.inputs)expected[item.petal]-=assemblerInputCount(recipe,registry,Rarity::Super,item.petal,item.count)*2;
  for(const auto& item:recipe.outputs)expected[item.petal]+=item.count*2;
  world.get<Transform>(flower).position=world.get<Transform>(station).position+Vec2{0,70};
  client.assembleOutcome().pending=false;client.requestAssemble(Rarity::Super,index,2);
  CHECK(h.stepUntil({&client},[&]{return client.assembleOutcome().pending;},200));
  if(!client.assembleOutcome().success)std::printf("recipe %u refused: %s\n",unsigned(index),client.assembleOutcome().reason.c_str());
  CHECK(client.assembleOutcome().success);
  CHECK(awaitProfile(h,client,[&](const Profile&){for(const auto& item:expected)if(client.profile().stackCount(item.first,Rarity::Super)!=item.second)return false;return true;}));
 };
 for(int slot=0;slot<49;++slot){
  epoch=slot*kBioAssemblerRotationMillis+50;
  Entity station=findStation();CHECK(station!=NULL_ENTITY);if(station==NULL_ENTITY)return;
  world.get<Npc>(station).recipeRefreshAtMillis=1;
  CHECK(h.stepUntil({&client},[&]{Entity e=findStation();return e!=NULL_ENTITY&&world.get<Npc>(e).recipe==slot;},200));
  station=findStation();CHECK(!bio.recipes[slot].previewOnly);exchange(bio.recipes[slot],static_cast<std::uint8_t>(slot),station);
 }
 Entity station=findStation();CHECK(station!=NULL_ENTITY);if(station==NULL_ENTITY)return;
 const auto factoryIndex=registry.mobIndex("assembler");auto& npc=world.get<Npc>(station);npc.configIndex=factoryIndex;npc.rarity=Rarity::Super;
 const auto& factory=registry.mob(factoryIndex).npc;CHECK_EQ(factory.recipes.size(),17);
 for(int i=13;i<17;++i){epoch+=kBioAssemblerRotationMillis;world.get<Npc>(station).recipe=static_cast<std::uint8_t>(i);world.get<Npc>(station).recipeRefreshAtMillis=1e18;exchange(factory.recipes[i],static_cast<std::uint8_t>(i),station);}
}


#endif


TEST(jurassic_links_both_jungles_and_stocks_all_fifteen_species) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    const bool loaded = maps.load(dataDir(), &terrain, error);
    CHECK(loaded);
    if (!loaded) return;
    Rng rng{42};
    const auto checkPad = [&](const char* from, const char* to, const char* id) {
        bool found = false;
        const Realm realm = maps.realmOfId(from, found);
        CHECK(found);
        if (!found) return;
        const Realm expected = maps.realmOfId(to, found);
        CHECK(found);
        bool sawPad = false;
        for (const auto& pad : maps.forRealm(realm)->elements()) {
            if (pad.padId != id) continue;
            sawPad = true;
            CHECK(!terrain.resolveWall(pad.centre(), kPlayerBaseRadius, realm).collided);
            WorldMaps::Destination dest;
            CHECK(maps.resolveTeleporter(pad, rng, terrain, dest));
            CHECK(dest.realm == expected);
            CHECK(!terrain.resolveWall(dest.position, kPlayerBaseRadius, dest.realm).collided);
        }
        CHECK(sawPad);
    };
    checkPad("jungle", "jurassic", "to-jurassic");
    checkPad("jurassic", "jungle", "from-jungle");
    checkPad("prestige_jungle", "prestige_jurassic", "to-jurassic");
    checkPad("prestige_jurassic", "prestige_jungle", "from-jungle");

    bool found = false;
    constexpr int side = 100;
    for (const auto& [id, expectedWalls] : {
             std::pair{"jurassic", 5083}, {"jurassic_pangea", 6175}, {"jurassic_tectonics", 5661}}) {
        const Realm realm = maps.realmOfId(id, found);
        CHECK(found);
        if (!found) continue;
        const auto* layout = maps.forRealm(realm);
        CHECK_EQ(layout->width(), side);
        CHECK_EQ(layout->height(), side);
        // The original client paints the set bits as paths. Compare every
        // effective collision cell, including both prestige twins.
        const std::string idText = id;
        const std::uint32_t sourceMaskHash = idText == "jurassic" ? 1962760074u
            : idText == "jurassic_pangea" ? 3646136026u : 2681293992u;
        for (int level : {0, 1, 2}) {
            const Realm current = level == 0 ? realm : maps.prestigeRealm(realm, found, level);
            CHECK(found);
            int walls = 0;
            std::uint32_t maskHash = 2166136261u;
            for (int y = 0; y < side; ++y) {
                for (int x = 0; x < side; ++x) {
                    const bool blocked = terrain.blocked(Terrain::tileCenter(x, y), current);
                    walls += blocked;
                    maskHash = (maskHash ^ static_cast<std::uint32_t>(blocked)) * 16777619u;
                }
            }
            CHECK_EQ(walls, expectedWalls);
            CHECK_EQ(maskHash, sourceMaskHash);
        }
        const Vec2 spawn = layout->defaultSpawn(rng, terrain);
        CHECK(!terrain.resolveWall(spawn, kPlayerBaseRadius, realm).collided);
        checkPad(id, "jungle", "from-jungle");
        const std::string prestige = std::string("prestige_") + id;
        checkPad(prestige.c_str(), "prestige_jungle", "from-jungle");
    }

    const auto& content = shippedContent();
    DropTables drops;
    CHECK(drops.load(content, dataDir() + "/mob_drops.json", error));
    for (const char* map : {"jurassic", "prestige_jurassic", "jurassic_pangea", "prestige_jurassic_pangea",
                            "jurassic_tectonics", "prestige_jurassic_tectonics"}) {
        const Realm instance = maps.realmOfId(map, found);
        CHECK(found);
        World world;
        CommandBuffer commands{world};
        SpawnSystem spawning;
        spawning.worldMaps = &maps;
        spawning.mobCap = 0;
        for (int step = 0; step < 15; ++step) {
            spawning.run(world, terrain, content, {{terrain.spawnPoint(instance), instance}},
                         rng, 50000.0 + step * kPopulationIntervalMillis, net::kTickSeconds, commands);
            commands.flush();
        }
        std::vector<SpawnSystem::LatentSite> sites;
        spawning.latentSites(instance, sites);
        std::set<std::uint16_t> species;
        for (const auto& site : sites) {
            CHECK(!terrain.blocked(site.position, instance));
            species.insert(site.mobIndex);
        }
        CHECK_EQ(species.size(), std::size_t(15));
        for (const auto id : species) {
            CHECK(content.mob(id).id.find("jurassic_") == 0);
            const auto& pool = drops.forMob(id);
            CHECK(!pool.empty());
            double superTail = 0.0;
            for (const auto& entry : pool) {
                CHECK(entry.petalIndex != kInvalidIndex);
                CHECK_EQ(entry.rollGroup, 1);
                CHECK_EQ(entry.rates.size(), std::size_t(9));
                superTail += hyperSuperChance(entry.rates);
                const auto& petal = content.petal(entry.petalIndex);
                if (petal.petMobIndex != kInvalidIndex) CHECK_EQ(petal.petMobIndex, id);
            }
            CHECK(superTail > 0 && superTail <= .0150001);
        }
        std::printf("[jurassic] %s: %zu stocked monsters, %zu species\n",
                    map, sites.size(), species.size());
    }
    for (const auto& [mobId, baseId, magicId] : {
             std::array<const char*, 3>{"jurassic_raptor", "bone", "magic_bone"},
             std::array<const char*, 3>{"jurassic_fossil", "crystal_leaf", "magic_crystal_leaf"},
             std::array<const char*, 3>{"jurassic_meteor", "fire", "magic_fire"}}) {
        const auto base = content.petalIndex(baseId);
        const auto magic = content.petalIndex(magicId);
        CHECK(magic != kInvalidIndex);
        CHECK_EQ(content.magicFormOf(base), magic);
        const auto& pool = drops.forMob(content.mobIndex(mobId));
        CHECK(std::any_of(pool.begin(), pool.end(), [&](const auto& row) {
            return row.petalIndex == base && row.magicRates.empty() && row.rollGroup == 1;
        }));
        CHECK(std::none_of(pool.begin(), pool.end(), [&](const auto& row) {
            return row.petalIndex == magic;
        }));
    }
    const auto mob = [&](const char* id) -> const MobConfig& {
        return content.mob(content.mobIndex(id));
    };
    CHECK(mob("jurassic_raptor").crabStrafe);
    CHECK(mob("jurassic_acid_bubble").poisonPerSecond > 0);
    CHECK(mob("jurassic_fossil").petalRing.present);
    CHECK_EQ(mob("jurassic_volcano").petalRing.petalIndex, content.petalIndex("rock"));
    CHECK(mob("jurassic_diplodocus").massMultiplier > 1.0);

}

TEST(feedback_3620_void_astral_and_null_pads_keep_the_prestige_tier_both_ways) {
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    Rng rng{42};
    for (int level : {0, 1, 2}) {
        bool found = false;
        const Realm plain = maps.realmOfId("zorr_void", found);
        CHECK(found);
        Realm originRealm = level == 0 ? plain : maps.prestigeRealm(plain, found, level);
        CHECK(found);
        const auto* origin = maps.forRealm(originRealm);
        if (!origin) continue;
        for (const char* id : {"zorr_astral", "zorr_null"}) {
            const Realm target = maps.realmOfId(id, found);
            CHECK(found);
            const Realm expected = level == 0 ? target : maps.prestigeRealm(target, found, level);
            CHECK(found);
            const auto* targetMap = maps.forRealm(expected);
            if (!targetMap) continue;
            WorldMaps::Destination arrival, back;
            bool outbound = false, inbound = false;
            for (const auto& pad : origin->elements()) {
                if (pad.kind != MapElementKind::Teleporter || pad.targetMap != targetMap->id()) continue;
                outbound = maps.resolveTeleporter(pad, rng, terrain, arrival);
                if (outbound) break;
            }
            for (const auto& pad : targetMap->elements()) {
                if (pad.kind != MapElementKind::Teleporter || pad.targetMap != origin->id()) continue;
                inbound = maps.resolveTeleporter(pad, rng, terrain, back);
                if (inbound) break;
            }
            CHECK(outbound);
            CHECK(inbound);
            if (outbound) {
                CHECK(arrival.realm == expected);
                CHECK(!terrain.blocked(arrival.position, expected));
            }
            if (inbound) {
                CHECK(back.realm == originRealm);
                CHECK(!terrain.blocked(back.position, originRealm));
            }
            originRealm = expected;
            origin = targetMap;
        }
    }
}

TEST(jurassic_rotation_keeps_prestige_twins_and_retires_wild_mobs) {
    Harness h("jurassic-rotation", {}, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    const auto& maps = h.server.worldMaps();
    const auto& terrain = h.server.terrain();
    auto& world = h.server.world();
    bool found = false;
    const Realm entrance = maps.realmOfId("jurassic", found);
    CHECK(found);
    const Realm prestigeEntrance = maps.prestigeRealm(entrance, found);
    CHECK(found);
    if (!found) return;
    const Realm prestigeTwoEntrance = maps.prestigeRealm(entrance, found, 2);
    CHECK(found);
    if (!found) return;
    const auto flower = [&](Realm destination) {
        const Entity e = world.create();
        world.add<PlayerTag>(e);
        world.add<Transform>(e, Transform{terrain.spawnPoint(Realm::Overworld), 0.0, Realm::Overworld});
        world.add<Body>(e, Body{kPlayerBaseRadius, 1.0});
        GameServerPeer::markTeleport(h.server, e, destination, terrain.spawnPoint(destination));
        return e;
    };
    const Entity ordinary = flower(entrance);
    const Entity prestige = flower(prestigeEntrance);
    const Entity prestigeTwo = flower(prestigeTwoEntrance);
    std::set<Realm> visited;
    for (int i = 0; i < 3; ++i) {
        const Realm old = realmOf(world, ordinary);
        const Entity mob = world.create();
        world.add<MobTag>(mob);
        world.add<Transform>(mob, Transform{terrain.spawnPoint(old), 0.0, old});
        GameServerPeer::advanceJurassicCycle(h.server);
        CHECK(!world.isAlive(mob));
        const Realm current = realmOf(world, ordinary);
        CHECK(current != old);
        CHECK(maps.prestigeRealm(current, found) == realmOf(world, prestige));
        CHECK(found);
        CHECK(maps.prestigeRealm(current, found, 2) == realmOf(world, prestigeTwo));
        CHECK(found);
        for (const Entity e : {ordinary, prestige, prestigeTwo}) {
            const auto& at = world.get<Transform>(e);
            CHECK(!terrain.resolveWall(at.position, kPlayerBaseRadius, at.realm).collided);
        }
        visited.insert(current);
    }
    CHECK_EQ(visited.size(), std::size_t(3));
}

TEST(jurassic_original_ground_and_rock_wall_render_in_game) {
    const auto& registry = shippedContent();
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    bool found = false;
    const Realm realm = maps.realmOfId("jurassic", found);
    CHECK(found);
    if (!found) return;
    SpriteCache sprites;
    CHECK(sprites.build(registry, dataDir()));
    for (const char* material : {"ground", "wall"}) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const std::string tile = std::string("jurassic_zorr_") + material + "_" +
                    std::to_string(x) + "_" + std::to_string(y) + ".svg";
                CHECK(sprites.tileArt(tile) != nullptr);
            }
        }
    }
    CHECK(sprites.petalDrawable(registry.petalIndex("jurassic_raptor_egg")));

    // Node runs the WebAssembly suite without a browser canvas.
#if !defined(__EMSCRIPTEN__)
    WorldRenderer renderer;
    renderer.setSprites(&sprites);
    renderer.setContent(&registry);
    renderer.setWorldMaps(&maps);
    WorldView view;
    view.setRealm(realm);
    constexpr int size = 120;
    const auto pixel = [&](Vec2 at) {
        Canvas canvas = Canvas::createVirtual(size, size);
        renderer.draw(canvas, view, frameCamera(size, at), at, 0.0);
        return canvas.getImageData(size / 2, size / 2, 1, 1);
    };
    Rng rng{42};
    const auto soil = pixel(maps.forRealm(realm)->defaultSpawn(rng, terrain));
    Vec2 wallCell;
    bool foundWall = false;
    for (int y = 0; y < 100 && !foundWall; ++y) {
        for (int x = 0; x < 100 && !foundWall; ++x) {
            wallCell = Terrain::tileCenter(x, y);
            foundWall = terrain.blocked(wallCell, realm);
        }
    }
    CHECK(foundWall);
    const auto wall = pixel(wallCell);
    CHECK(soil.size() == 4 && soil[0] > soil[1] && soil[1] > soil[2] && soil[2] > 0);
    CHECK(wall.size() == 4 && wall[0] == wall[1] && wall[1] == wall[2] && wall[0] >= 90);
#endif
}

TEST(jurassic_lava_stacks_and_oil_only_doubles_while_burning) {
    World world;
    SpatialGrid grid;
    CombatSystem combat;
    CommandBuffer commands{world};
    EventQueue events;
    const auto& content = shippedContent();
    const Entity owner = world.create();
    world.add<PlayerTag>(owner);
    world.add<Faction>(owner, Faction{Team::Players, true});
    world.add<Transform>(owner, Transform{{900.0, 900.0}, 0.0});
    world.add<Health>(owner, Health{1000.0, 1000.0});
    const Entity target = world.create();
    world.add<MobTag>(target);
    world.add<Faction>(target, Faction{Team::Hostiles, false});
    world.add<Transform>(target, Transform{{1000.0, 900.0}, 0.0});
    world.add<Body>(target, Body{20.0, 1.0});
    world.add<Health>(target, Health{1000.0, 1000.0});
    grid.insert(target, Realm::Overworld, {1000.0, 900.0}, 20.0);
    const auto& oil = content.petal(content.petalIndex("oil")).spill;
    const auto& lava = content.petal(content.petalIndex("lava_bomb")).spill;
    CHECK(oil.kind == SpillKind::Oil);
    CHECK(lava.kind == SpillKind::Lava && lava.onHit);
    const Vec2 impact{1000.0, 900.0};
    const double dt = net::kTickSeconds;
    CombatSystem::createSpill(world, owner, impact, Rarity::Common, oil, 1.0, 0);
    combat.run(world, grid, content, 1000.0, dt, commands, events);
    const double unlit = world.get<Health>(target).current;
    CHECK_NEAR(unlit, 1000.0 - oil.strength * dt, 1e-8);
    CombatSystem::createSpill(world, owner, impact, Rarity::Common, lava, 1.0, 0);
    CombatSystem::createSpill(world, owner, impact, Rarity::Common, lava, 1.0, 0);
    combat.run(world, grid, content, 1050.0, dt, commands, events);
    CHECK_EQ(world.get<Afflictions>(target).burns.size(), std::size_t(2));
    const double before = world.get<Health>(target).current;
    combat.run(world, grid, content, 1100.0, dt, commands, events);
    CHECK_NEAR(before - world.get<Health>(target).current,
               (2.0 * oil.strength + 2.0 * lava.strength) * dt, 1e-8);
    CHECK_NEAR(world.get<Health>(owner).current, 1000.0, 1e-8);

    const auto& hotWater = content.petal(content.petalIndex("hot_water")).spill;
    CombatSystem::createSpill(world, owner, impact, Rarity::Common, hotWater, 1.0, 0);
    combat.run(world, grid, content, 1150.0, dt, commands, events);
    const auto& status = world.get<Afflictions>(target);
    CHECK_NEAR(status.frostSpeed(1150.0), .5, 1e-8);
    CHECK_NEAR(status.frostSpeed(1650.0), 1.0, 1e-8);
    CHECK_EQ(status.burns.size(), std::size_t(3));
}

TEST(jurassic_amber_scales_and_chromosome_apply_to_the_right_health_pool) {
    std::string error;
    CHECK(loadContent(dataDir(), error));
    if (!error.empty()) return;
    World world;
    CombatSystem combat;
    const Entity owner = world.create();
    world.add<PlayerTag>(owner);
    world.add<Faction>(owner, Faction{Team::Players, true});
    world.add<Transform>(owner, Transform{{900.0, 900.0}, 0.0});
    world.add<Health>(owner, Health{100.0, 100.0});
    auto& mods = world.add<PlayerModifiers>(owner);
    mods.scalesBodyReduction = .04;
    mods.amberPetalReduction = .025;
    world.add<Loadout>(owner);
    const Entity petal = world.create();
    world.add<PetalTag>(petal);
    world.add<Faction>(petal, Faction{Team::Players, true});
    world.add<Transform>(petal, Transform{{905.0, 900.0}, 0.0});
    world.add<Health>(petal, Health{100.0, 100.0});
    auto& instance = world.add<PetalInstance>(petal);
    instance.owner = owner;
    instance.slot = 1;
    instance.chromosomeShield = 10.0;
    instance.chromosomeShieldUntilMillis = 1500.0;
    const auto gear = [&](const char* id, Vec2 position, int slot) {
        const Entity e = world.create();
        world.add<Transform>(e, Transform{position, 0.0});
        world.add<Body>(e, Body{10.0, 1.0});
        auto& gear = world.add<PetalInstance>(e);
        gear.owner = owner;
        gear.slot = slot;
        gear.configIndex = shippedContent().petalIndex(id);
        world.get<Loadout>(owner).spawned.push_back(e);
        return e;
    };
    const Entity amber = gear("amber", {895.0, 900.0}, 0);
    const Entity scales = gear("scales", {920.0, 900.0}, 2);
    const Entity enemy = world.create();
    world.add<MobTag>(enemy);
    world.add<Body>(enemy, Body{10.0, 1.0});
    world.add<Faction>(enemy, Faction{Team::Hostiles, false});
    world.add<Transform>(enemy, Transform{{920.0, 900.0}, 0.0});
    world.add<Health>(enemy, Health{100.0, 100.0});

    CHECK_NEAR(combat.applyDamage(world, petal, enemy, 20.0, 1000.0,
                                  DamageKind::Recoil).applied, 9.5, 1e-8);
    CHECK_NEAR(world.get<PetalInstance>(petal).chromosomeShield, 0.0, 1e-8);
    CHECK_NEAR(combat.applyDamage(world, owner, enemy, 20.0, 1000.0).applied, 19.2, 1e-8);
    world.get<Transform>(scales).position.x += 100.0;
    CHECK_NEAR(combat.applyDamage(world, owner, enemy, 20.0, 1750.0).applied, 20.0, 1e-8);
    world.get<PetalInstance>(petal).rarity = Rarity::Uncommon;
    CHECK_NEAR(combat.applyDamage(world, petal, enemy, 20.0, 1750.0,
                                  DamageKind::Recoil).applied, 20.0, 1e-8);
    world.get<PetalInstance>(petal).rarity = Rarity::Common;
    world.get<PetalInstance>(amber).slot = 2;
    CHECK_NEAR(combat.applyDamage(world, petal, enemy, 20.0, 1900.0,
                                  DamageKind::Recoil).applied, 20.0, 1e-8);
    CHECK_NEAR(combat.applyDamage(world, owner, enemy, 20.0, 2000.0,
                                  DamageKind::Periodic).applied, 20.0, 1e-8);
}

TEST(jurassic_feather_cost_and_amulet_squad_damage_follow_equipment_and_map) {
    World world;
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    bool found = false;
    const Realm realm = maps.realmOfId("jurassic", found);
    CHECK(found);
    if (!found) return;
    PetalSystem petals;
    petals.worldMaps = &maps;
    CommandBuffer commands{world};
    const auto& registry = shippedContent();
    const Vec2 centre = terrain.spawnPoint(realm);
    const auto flower = [&](Vec2 at) {
        const auto e = world.create();
        world.add<PlayerTag>(e);
        world.add<Transform>(e, Transform{at, 0.0, realm});
        world.add<Motion>(e);
        world.add<Body>(e, Body{kPlayerBaseRadius, 1.0});
        world.add<Health>(e, Health{maxHealthForLevel(1), maxHealthForLevel(1)});
        world.add<Faction>(e, Faction{Team::Players, false});
        world.add<PlayerProgress>(e);
        world.add<PlayerInput>(e);
        world.add<PlayerModifiers>(e);
        world.add<PlayerLocation>(e);
        world.add<Loadout>(e);
        world.add<PetalRing>(e);
        return e;
    };
    const Entity wearer = flower(centre);
    const Entity ally = flower(centre + Vec2{40.0, 0.0});
    auto& slots = world.get<Loadout>(wearer).slots;
    slots[0].configIndex = registry.petalIndex("feather");
    slots[1].configIndex = registry.petalIndex("amulet");
    world.get<PlayerInput>(wearer).current.moveStrength = 1.0;
    petals.run(world, registry, 1000.0, net::kTickSeconds, commands, &terrain);
    // Newly equipped petals have to complete their first reload.
    petals.run(world, registry, 10000.0, net::kTickSeconds, commands, &terrain);
    commands.flush();
    CHECK(world.get<PlayerModifiers>(wearer).speedScale > 1.0);
    CHECK(world.get<Health>(wearer).current < world.get<Health>(wearer).max);
    CHECK(world.get<PlayerModifiers>(wearer).jurassicAmuletBonus > 0.0);

    const Entity striker = world.create();
    world.add<PetalInstance>(striker).owner = ally;
    world.add<Transform>(striker, Transform{centre, 0.0, realm});
    world.add<Faction>(striker, Faction{Team::Players, false});
    const Entity enemy = world.create();
    world.add<MobTag>(enemy);
    world.add<Faction>(enemy, Faction{Team::Hostiles, false});
    world.add<Transform>(enemy, Transform{centre, 0.0, realm});
    world.add<Health>(enemy, Health{100.0, 100.0});
    CombatSystem combat;
    SquadEntityIndex squads;
    squads.groups = {{{wearer, 1}, {ally, 2}}};
    squads.group = {{wearer, 0}, {ally, 0}};
    combat.squads = &squads;
    CHECK(combat.applyDamage(world, enemy, striker, 10.0, 10050.0).applied > 10.0);
    CHECK_NEAR(combat.applyDamage(world, enemy, striker, 10.0, 10200.0,
                                  DamageKind::Periodic).applied, 10.0, 1e-8);
    world.get<Transform>(wearer).realm = Realm::Overworld;
    world.get<PlayerInput>(wearer).current.moveStrength = 0.0;
    petals.run(world, registry, 10500.0, net::kTickSeconds, commands, &terrain);
    commands.flush();
    CHECK_NEAR(world.get<PlayerModifiers>(wearer).speedScale, 1.0, 1e-8);
    CHECK_NEAR(world.get<PlayerModifiers>(wearer).jurassicAmuletBonus, 0.0, 1e-8);
    CHECK_NEAR(combat.applyDamage(world, enemy, striker, 10.0, 10600.0).applied, 10.0, 1e-8);
}

TEST(jurassic_magic_petals_pay_for_live_pve_effects) {
    World world;
    PetalSystem petals;
    CommandBuffer commands{world};
    const auto& registry = shippedContent();
    const Entity player = world.create();
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
    slots[0].configIndex = registry.petalIndex("magic_leaf");
    slots[1].configIndex = registry.petalIndex("magic_bone");
    slots[1].rarity = Rarity::Uncommon;
    slots[2].configIndex = registry.petalIndex("magic_bone");
    slots[3].configIndex = registry.petalIndex("magic_crystal_leaf");
    slots[4].configIndex = registry.petalIndex("magic_fire");
    slots[5].configIndex = registry.petalIndex("torch");
    slots[6].configIndex = registry.petalIndex("magic_torch");

    const auto body = [&](Vec2 at, Realm realm, Team team) {
        const Entity entity = world.create();
        world.add<MobTag>(entity);
        world.add<Transform>(entity, Transform{at, 0.0, realm});
        world.add<Body>(entity, Body{40.0, 1.0});
        world.add<Health>(entity, Health{80.0, 100.0});
        world.add<Faction>(entity, Faction{team, false});
        return entity;
    };
    const Entity summon = body({910.0, 900.0}, Realm::Overworld, Team::Players);
    world.add<Pet>(summon).owner = player;
    const Entity away = body({910.0, 900.0}, Realm::Arena, Team::Players);
    world.add<Pet>(away).owner = player;
    const Entity enemy = body({920.0, 900.0}, Realm::Overworld, Team::Hostiles);
    world.get<Body>(enemy).radius = 180.0;
    const auto step = [&](double now) {
        petals.run(world, registry, now, net::kTickSeconds, commands);
        commands.flush();
    };
    step(1000.0);
    auto& mana = world.get<ManaPool>(player);
    const double initial = mana.current;
    step(4000.0);
    CHECK(world.get<ManaPool>(player).current < initial);
    const auto& bonus = world.get<PlayerModifiers>(player).magicBoneDamageBonus;
    CHECK_NEAR(bonus[ladderIndex(Rarity::Common)], .5, 1e-8);
    CHECK_NEAR(bonus[ladderIndex(Rarity::Uncommon)], .5, 1e-8);

    world.get<PlayerInput>(player).current.flags = net::InputAttack;
    world.get<ManaPool>(player).current = 0.0;
    step(5000.0);
    CHECK(!world.has<Afflictions>(enemy));
    CHECK_NEAR(world.get<Pet>(summon).crystalLeafShield, 0.0, 1e-8);
    CHECK(world.get<Health>(summon).current > 80.0);
    CHECK_NEAR(world.get<Health>(away).current, 80.0, 1e-8);

    world.add<Afflictions>(player).frostUntilMillis = 6500.0;
    world.add<Afflictions>(summon).frostUntilMillis = 6500.0;
    world.add<Afflictions>(away).frostUntilMillis = 6500.0;
    world.get<ManaPool>(player).current = 20.0;
    step(6000.0);
    const auto* status = world.tryGet<Afflictions>(enemy);
    CHECK(status && status->burns.size() == 1);
    if (status && !status->burns.empty()) CHECK_NEAR(status->burns[0].untilMillis, 9000.0, 1e-8);
    CHECK_NEAR(world.get<Pet>(summon).crystalLeafShield, 4.0, 1e-8);
    CHECK_NEAR(world.get<Pet>(away).crystalLeafShield, 0.0, 1e-8);
    CHECK(world.get<ManaPool>(player).current < 20.0);
    CHECK_NEAR(world.get<Afflictions>(player).frostUntilMillis, 6445.0, 1e-8);
    CHECK_NEAR(world.get<Afflictions>(summon).frostUntilMillis, 6445.0, 1e-8);
    CHECK_NEAR(world.get<Afflictions>(away).frostUntilMillis, 6500.0, 1e-8);

    CombatSystem combat;
    CHECK_NEAR(combat.applyDamage(world, summon, enemy, 2.0, 6050.0).applied, 0.0, 1e-8);
    CHECK_NEAR(world.get<Pet>(summon).crystalLeafShield, 2.0, 1e-8);
    CHECK_NEAR(combat.applyDamage(world, summon, enemy, 6.0, 7550.0).applied, 6.0, 1e-8);
}

TEST(jurassic_feedback_bone_stats_and_moon_rock_price) {
    const auto& registry = shippedContent();
    const auto bone = registry.petalIndex("bone");
    CHECK(bone != kInvalidIndex);
    const auto common = registry.petalStats(bone, Rarity::Common);
    CHECK_NEAR(common.health, 12.0, 1e-9);
    CHECK_NEAR(common.petalArmor, 12.0, 1e-9);
    CHECK_NEAR(common.damage, 14.0, 1e-9);
    CHECK_NEAR(common.reloadMillis, 2500.0, 1e-9);
    const auto super = registry.petalStats(bone, Rarity::Super);
    CHECK_NEAR(super.health, 26244.0, 1e-9);
    CHECK_NEAR(super.petalArmor, 26244.0, 1e-9);
    const auto orb = registry.petalIndex("mana_orb");
    CHECK(orb != kInvalidIndex);
    CHECK(!registry.petalStats(orb, Rarity::Common).breakable);
    CHECK(!registry.petalStats(orb, Rarity::Apex).breakable);
    std::string error;
    CHECK(loadContent(dataDir(), error));
    CHECK_NEAR(shopPrice("basic", Rarity::Super), 30000.0, 1e-9);
    const auto moon = registry.petalIndex("moon_rock");
    CHECK_NEAR(shopPrice(moon, Rarity::Ultra), 9000.0, 1e-9);
    CHECK_NEAR(shopPrice(moon, Rarity::Super), 772000.0, 1e-9);
    CHECK_NEAR(shopOfferPrice(moon, Rarity::Super, 30), 31500.0, 1e-9);
    CHECK(shopSellsRarity(Rarity::Unique));
}

TEST(guild_deputies_can_invite_but_cannot_manage_or_transfer) {
    Harness h("guild-deputy", [](const std::string& path) {
        for (const char* name : {"leader", "deputy", "guest", "outsider"})
            seedUser(path, name, "password7");
    });
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient boss, deputy, guest, outsider;
    CHECK(logIn(h, boss, "leader"));
    CHECK(logIn(h, deputy, "deputy"));
    CHECK(logIn(h, guest, "guest"));
    CHECK(logIn(h, outsider, "outsider"));
    const std::vector<NetClient*> clients{&boss, &deputy, &guest, &outsider};
    boss.requestGuildCreate("ALPHA");
    CHECK(h.stepUntil(clients, [&] { return boss.guild().joined; }));
    boss.requestGuildInvite("deputy");
    CHECK(h.stepUntil(clients, [&] { return deputy.guildInvite().waiting; }));
    deputy.requestGuildAccept();
    CHECK(h.stepUntil(clients, [&] { return deputy.guild().joined; }));
    deputy.requestGuildInvite("guest");
    h.step(10, clients);
    CHECK(!guest.guildInvite().waiting);
    boss.sendChat("/guild-deputy deputy");
    const std::vector<std::string> firstDeputy{"deputy"};
    CHECK(h.stepUntil(clients, [&] { return deputy.guild().deputies == firstDeputy; }));
    // An existing singleton record still grants its invitation rights.
    auto& guild = GameServerPeer::authorityDb(h.server).rawTable("guilds")["ALPHA"];
    guild.erase("deputyUsernames");
    guild["deputyUsername"] = "deputy";
    deputy.requestGuildInvite("guest");
    CHECK(h.stepUntil(clients, [&] { return guest.guildInvite().waiting; }));
    guest.requestGuildAccept();
    CHECK(h.stepUntil(clients, [&] { return guest.guild().joined; }));

    deputy.sendChat("/guild-deputy guest");
    deputy.sendChat("/guild-transfer guest");
    deputy.requestGuildKick("guest");
    deputy.requestGuildEdit("Changed", "Changed");
    h.step(20, clients);
    CHECK(guest.guild().joined);
    CHECK_EQ(boss.guild().leader, std::string("leader"));
    CHECK(boss.guild().deputies == firstDeputy);
    CHECK(boss.guild().name != "Changed");

    boss.sendChat("/guild-deputy guest");
    const std::vector<std::string> bothDeputies{"deputy", "guest"};
    CHECK(h.stepUntil(clients, [&] { return deputy.guild().deputies == bothDeputies; }));
    guest.requestGuildInvite("outsider");
    CHECK(h.stepUntil(clients, [&] { return outsider.guildInvite().waiting; }));
    outsider.requestGuildAccept();
    CHECK(h.stepUntil(clients, [&] { return outsider.guild().joined; }));
    boss.sendChat("/guild-deputy outsider");
    h.step(10, clients);
    CHECK(boss.guild().deputies == bothDeputies);
    boss.sendChat("/guild-transfer guest");
    CHECK(h.stepUntil(clients, [&] { return boss.guild().leader == "guest"; }));
    CHECK(boss.guild().deputies == firstDeputy);
    guest.sendChat("/guild-deputy outsider");
    CHECK(h.stepUntil(clients, [&] { return boss.guild().deputies.size() == 2; }));
    guest.sendChat("/guild-undeputy outsider");
    CHECK(h.stepUntil(clients, [&] { return boss.guild().deputies == firstDeputy; }));
    deputy.requestGuildLeave();
    CHECK(h.stepUntil(clients, [&] { return boss.guild().deputies.empty(); }));
}

TEST(hyper_corruption_exposure_allows_retaliation_and_continue_returns_to_spawn) {
    Harness h("corruption-continue", [](const std::string& path) {
        seedUser(path, "attacker", "password7");
        seedUser(path, "survivor", "password7");
        seedProgress(path, "attacker", false, 25);
        seedProgress(path, "survivor", false, 25);
    });
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient attacker, survivor;
    CHECK(logIn(h, attacker, "attacker") && join(h, attacker, "attacker", false, "garden"));
    CHECK(logIn(h, survivor, "survivor") && join(h, survivor, "survivor", false, "garden"));
    auto& world = h.server.world();
    const Entity aggressor = bodyNamed(world, "attacker");
    const Entity victim = bodyNamed(world, "survivor");
    CHECK(aggressor != NULL_ENTITY && victim != NULL_ENTITY);
    if (aggressor == NULL_ENTITY || victim == NULL_ENTITY) return;
    const auto spawn = world.get<Transform>(victim).position;
    world.get<Loadout>(aggressor).slots[0] = {content().petalIndex("corruption"), Rarity::Apex};
    h.step(1, {&attacker, &survivor});
    CHECK(world.get<PlayerVisuals>(aggressor).hyperCorruption);
    world.get<Loadout>(aggressor).slots[0] = {};
    h.step(1, {&attacker, &survivor});
    CHECK(world.get<PlayerVisuals>(aggressor).hyperCorruptionExposed);
    CHECK(CombatSystem::canDamage(world, victim, aggressor));
    CHECK(!CombatSystem::canDamage(world, aggressor, victim));
    h.clock += kHyperCorruptionExposureMillis + 50.0;
    h.step(1, {&attacker, &survivor});
    CHECK(!world.get<PlayerVisuals>(aggressor).hyperCorruptionExposed);
    CHECK(!CombatSystem::canDamage(world, victim, aggressor));

    world.get<Loadout>(aggressor).slots[0] = {content().petalIndex("corruption"), Rarity::Apex};
    h.step(1, {&attacker, &survivor});
    const Vec2 deathAt = spawn + Vec2{5000.0, 5000.0};
    world.get<Transform>(victim).position = deathAt;
    world.add<Dead>(victim, Dead{aggressor});
    CHECK(h.stepUntil({&attacker, &survivor}, [&] { return survivor.dead(); }));
    survivor.requestRespawn();
    CHECK(h.stepUntil({&attacker, &survivor}, [&] {
        return !survivor.dead() && bodyNamed(world, "survivor") != victim;
    }));
    const Entity reborn = bodyNamed(world, "survivor");
    CHECK(reborn != NULL_ENTITY);
    if (reborn != NULL_ENTITY) {
        CHECK(distance(world.get<Transform>(reborn).position, spawn) < 1800.0);
        CHECK(distance(world.get<Transform>(reborn).position, deathAt) > 4000.0);
    }
}

TEST(wave_loot_pays_from_normal_tables_and_throttles_boss_tiers_independently) {
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    LootSystem loot;
    EventQueue events;
    Rng rng{5150};
    const auto& registry = shippedContent();
    std::string error;
    CHECK(loot.loadTables(registry, dataDir() + "/mob_drops.json", error));
    const Entity player = world.create();
    world.add<PlayerTag>(player);
    world.add<Health>(player, Health{100.0, 100.0});
    world.add<Transform>(player, Transform{{2000.0, 2000.0}, 0.0});
    const auto roll = [&](Rarity rarity, double now, std::optional<bool> waveCanDrop) {
        const Entity corpse = world.create();
        world.add<MobTag>(corpse);
        world.add<Transform>(corpse, Transform{{4000.0, 4000.0}, 0.0});
        world.add<MobType>(corpse, MobType{registry.mobIndex("beetle"), rarity, 1.0});
        world.add<Health>(corpse, Health{0.0, 1000.0});
        world.add<Bounty>(corpse).credit(player, 1000.0);
        world.add<Dead>(corpse, Dead{player});
        if (waveCanDrop) world.add<WaveEnemy>(corpse, WaveEnemy{*waveCanDrop});
        loot.run(world, grid, registry, rng, now, net::kTickSeconds, commands, events);
        commands.flush();
        const auto drops = Query<DropTag>{world}.collect();
        for (const Entity drop : drops) world.destroy(drop);
        world.destroy(corpse);
        return drops.size();
    };
    CHECK_EQ(roll(Rarity::Rare, 1000.0, false), std::size_t(0));
    CHECK(roll(Rarity::Rare, 1100.0, true) > 0);
    CHECK_EQ(roll(Rarity::Apex, 1200.0, false), std::size_t(0));
    CHECK(roll(Rarity::Apex, 1300.0, true) > 0);
    CHECK(roll(Rarity::Unique, 1400.0, true) > 0);
    CHECK_EQ(roll(Rarity::Apex, 1500.0, true), std::size_t(0));
    CHECK_EQ(roll(Rarity::Unique, 1600.0, true), std::size_t(0));
    CHECK(roll(Rarity::Apex, 1700.0, std::nullopt) > 0);
    CHECK(roll(Rarity::Unique, 361400.0, true) > 0);
    CHECK_EQ(roll(Rarity::Apex, 361500.0, true), std::size_t(0));
    CHECK(roll(Rarity::Apex, 721300.0, true) > 0);
}

// ---------------------------------------------------------------------------
// The rules
// ---------------------------------------------------------------------------

TEST(an_prestige_band_rolls_one_tier_up_and_stops_at_super) {
    CHECK(prestigeRarity(Rarity::Common) == Rarity::Uncommon);
    CHECK(prestigeRarity(Rarity::Mythic) == Rarity::Ultra);
    CHECK(prestigeRarity(Rarity::Ultra) == Rarity::Super);
    // Super has nowhere to go: unique and apex never stand in a twin.
    CHECK(prestigeRarity(Rarity::Super) == Rarity::Super);
    CHECK(!barredFromPrestige(Rarity::Super));
    CHECK(barredFromPrestige(Rarity::Unique));
    CHECK(barredFromPrestige(Rarity::Apex));
}

TEST(deep_prestige_sewers_grows_mostly_super_with_some_ultra) {
    constexpr int samples = 50000;
    Rng band{711}, promotion{117}, previousPromotion{117};
    int ultra = 0;
    int fullPromotionSupers = 0;
    int previousSupers = 0;
    int newSupers = 0;
    for (int i = 0; i < samples; ++i) {
        const auto ordinary = rollReferenceSpawnRarity(75, 1.0, band);
        const auto adjusted = rollPrestigeAmbientRarity(ordinary, promotion);
        ultra += adjusted == Rarity::Ultra;
        fullPromotionSupers += prestigeRarity(ordinary) == Rarity::Super;
        previousSupers += ordinary == Rarity::Super ||
                          (ordinary == Rarity::Ultra && previousPromotion.chance(0.25));
        newSupers += adjusted == Rarity::Super;
        if (ordinary != Rarity::Ultra) CHECK(adjusted == prestigeRarity(ordinary));
    }
    CHECK(fullPromotionSupers > samples * .75);
    CHECK(previousSupers > samples * .18 && previousSupers < samples * .22);
    CHECK(newSupers > samples * .58 && newSupers < samples * .62);
    CHECK(ultra > samples * .34 && ultra < samples * .38);
    std::printf("[prestige sewers difficulty 75] full promotion Super %.2f%%, previous %.2f%%, "
                "retuned Super %.2f%%, Ultra %.2f%%\n",
                100.0 * fullPromotionSupers / samples, 100.0 * previousSupers / samples,
                100.0 * newSupers / samples,
                100.0 * ultra / samples);
}

TEST(prestige_is_one_root_node_costing_175) {
    CHECK_EQ(skillTierCount(SkillId::Prestige), 1);
    CHECK_EQ(skillTierCost(SkillId::Prestige, 0), 175);
    CHECK(skillGate(SkillId::Prestige) == nullptr);
    CHECK(skillFromKey("prestige") == SkillId::Prestige);

    SkillSet skills;
    CHECK(!skills.prestigeUnlocked());
    CHECK(skills.prerequisiteMet(SkillId::Prestige));
    skills.set(SkillId::Prestige, 0);
    CHECK(skills.prestigeUnlocked());
    CHECK_EQ(skills.spent(), 175);
    CHECK_EQ(availableTalentPoints(88, skills), 1);
    CHECK_EQ(availableTalentPoints(113, skills), 51);
    // It multiplies nothing.
    CHECK_NEAR(skills.statScale(SkillId::Damage), 1.0, 1e-12);
    CHECK_NEAR(skills.reloadScale(), 1.0, 1e-12);
}

// ---------------------------------------------------------------------------
// The twin maps
// ---------------------------------------------------------------------------

TEST(every_authored_prestige_twin_is_loaded_after_the_plain_realms) {
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), nullptr, error));

    Json manifest;
    CHECK(Json::parseFile(dataDir() + "/maps.json", manifest, error));
    std::set<std::string> authoredTwins;
    for (const Json& entry : manifest["maps"].items()) {
        if (entry["prestige"].asBool()) {
            authoredTwins.insert(std::filesystem::path(entry["file"].asString()).stem().string());
        }
    }
    CHECK(authoredTwins.size() >= 8u);
    int twins = 0;
    int lastPlainSlot = -1;
    int firstTwinSlot = maps.count();
    for (int i = 0; i < maps.count(); ++i) {
        const MapData& map = maps.maps()[static_cast<std::size_t>(i)];
        if (map.prestige()) {
            ++twins;
            firstTwinSlot = std::min(firstTwinSlot, i);
            CHECK(maps.isPrestige(map.realm()));
            CHECK_EQ(map.id(), std::string(kPrestigeMapPrefix) + map.templateId());
            CHECK(map.displayName().rfind("Prestige ", 0) == 0);
            continue;
        }
        lastPlainSlot = i;
        CHECK(!maps.isPrestige(map.realm()));
        bool found = false;
        const Realm twin = maps.prestigeRealm(map.realm(), found);
        CHECK(found == (authoredTwins.count(map.id()) > 0));
        if (!found) continue;
        const MapData* other = maps.forRealm(twin);
        CHECK(other != nullptr);
        if (other == nullptr) continue;
        CHECK(other->prestige());
        CHECK_EQ(other->id(), std::string(kPrestigeMapPrefix) + map.id());
        // Same place, same mobs.
        CHECK_EQ(other->biome(), map.biome());
        CHECK_EQ(other->defaultMobGroup(), map.defaultMobGroup());
        CHECK_EQ(other->elements().size(), map.elements().size());
        // A twin of a twin is not a thing.
        bool again = false;
        maps.prestigeRealm(twin, again);
        CHECK(!again);
    }
    CHECK_EQ(static_cast<std::size_t>(twins), authoredTwins.size());
    // Appended: turning prestige on renumbered no existing realm.
    CHECK(lastPlainSlot < firstTwinSlot);
    // There is no prestige arena or maze.
    bool found = true;
    maps.prestigeRealm(Realm::Arena, found);
    CHECK(!found);
    maps.prestigeRealm(Realm::Maze, found);
    CHECK(!found);
    // Nor are a twin's doors offered on their own.
    for (const SpawnChoice& door : maps.doors()) CHECK(!maps.isPrestige(door.realm));
}

TEST(late_biomes_have_both_prestige_twins_after_existing_realms) {
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), nullptr, error));
    const std::array<const char*, 5> biomes{
        "zorr_void", "bio", "bio_micro", "bio_lab", "zorr_arctic"
    };
    for (std::size_t i = 0; i < biomes.size(); ++i) {
        bool found = false;
        const Realm ordinary = maps.realmOfId(biomes[i], found);
        CHECK(found);
        if (!found) continue;
        for (int level : {1, 2}) {
            const Realm twin = maps.prestigeRealm(ordinary, found, level);
            CHECK(found);
            if (!found) continue;
            CHECK_EQ(maps.forRealm(twin)->prestigeLevel(), level);
            const std::string prefix = level == 1 ? "prestige_" : "prestige_2_";
            CHECK_EQ(maps.forRealm(twin)->id(), prefix + biomes[i]);
            CHECK(worldMapSlot(twin) > worldMapSlot(ordinary));
        }
    }
}

TEST(a_pad_on_an_prestige_twin_leads_to_another_twin) {
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), nullptr, error));
    int pads = 0;
    for (const MapData& map : maps.maps()) {
        if (!map.prestige()) continue;
        for (const MapElement& pad : map.elements()) {
            if (pad.kind != MapElementKind::Teleporter || pad.targetMap.empty()) continue;
            bool found = false;
            const Realm target = maps.realmOfId(pad.targetMap, found);
            CHECK(found);
            if (!found) continue;
            const MapData* destination = maps.forRealm(target);
            if (destination == nullptr) continue;
            // Into a twin, unless the place it leads has none (a dungeon).
            bool hasTwin = false;
            maps.prestigeRealm(maps.realmOfId(stripPrestigePrefix(pad.targetMap), found), hasTwin);
            CHECK(destination->prestige() || !hasTwin);
            ++pads;
        }
    }
    CHECK(pads > 0);
    // And the plain maps' pads were left alone.
    for (const MapData& map : maps.maps()) {
        if (map.prestige()) continue;
        for (const MapElement& pad : map.elements()) {
            if (pad.kind != MapElementKind::Teleporter) continue;
            CHECK(pad.targetMap.rfind(kPrestigeMapPrefix, 0) != 0);
        }
    }
}

TEST(a_copies_entry_cannot_also_be_prestige) {
    const std::string dir = tempDir("flix-prestige-manifest-" + std::to_string(::getpid()));
    CHECK(writeText(dir + "/maps.json",
                    R"({"maps": [{"file": "a.tmj", "copies": 2, "prestige": true}]})"));
    WorldMaps maps;
    std::string error;
    CHECK(!maps.load(dir, nullptr, error));
    CHECK(error.find("cannot also have a prestige twin") != std::string::npos);
}

// ---------------------------------------------------------------------------
// What grows there
// ---------------------------------------------------------------------------

TEST(an_empty_prestige_twin_stocks_regular_mobs_and_leaves_its_eternal_on_cooldown) {
    World world;
    CommandBuffer commands{world};
    Terrain terrain;
    SpawnSystem spawner;
    Rng rng{0xA5CE11u};
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    spawner.worldMaps = &maps;
    // One pass with nobody about stocks every band on every map as records.
    spawner.run(world, terrain, shippedContent(), {}, rng, 50000.0, net::kTickSeconds, commands);
    commands.flush();

    bool found = false;
    const Realm garden = maps.realmOfId("garden", found);
    CHECK(found);
    const Realm prestigeGarden = maps.prestigeRealm(garden, found);
    CHECK(found);
    CHECK(spawner.prestigeRealm(prestigeGarden));
    CHECK(!spawner.prestigeRealm(garden));

    const auto tiers = [&](Realm realm) {
        std::vector<SpawnSystem::LatentSite> sites;
        spawner.latentSites(realm, sites);
        return sites;
    };
    const std::vector<SpawnSystem::LatentSite> plainSites = tiers(garden);
    const std::vector<SpawnSystem::LatentSite> twinSites = tiers(prestigeGarden);
    CHECK(!plainSites.empty());
    CHECK(!twinSites.empty());

    // Difficulty-0 ground is all common in the garden and all uncommon in its
    // twin, mob floors aside -- and nothing in the twin is common.
    int plainCommons = 0;
    double plainSum = 0.0;
    for (const auto& site : plainSites) {
        plainCommons += site.rarity == Rarity::Common ? 1 : 0;
        plainSum += rarityIndex(site.rarity);
    }
    double twinSum = 0.0;
    for (const auto& site : twinSites) {
        CHECK(site.rarity != Rarity::Common);
        twinSum += rarityIndex(site.rarity);
    }
    CHECK(plainCommons > 0);
    CHECK(twinSum / twinSites.size() > plainSum / plainSites.size() + 0.5);

    // On every twin: nothing above super, as a record or as a body.
    for (const MapData& map : maps.maps()) {
        if (!map.prestige()) continue;
        for (const auto& site : tiers(map.realm())) CHECK(!barredFromPrestige(site.rarity));
    }
    Query<MobTag, Transform, MobType> mobs{world};
    mobs.each([&](Entity, MobTag&, Transform& transform, MobType& type) {
        if (maps.isPrestige(transform.realm)) CHECK(!barredFromPrestige(type.rarity));
    });
    CHECK(spawner.bossCooldownLeft(Rarity::Unique, prestigeGarden, 50000.0) > 0);
    // Spawn a real announcing tier in the ordinary biome. Supers in the
    // cold stock are below the global announcement threshold.
    const auto& content = shippedContent();
    spawner.restoreBossClock("garden", 0.0, 0.0);
    const auto boss = spawner.spawnMob(world, terrain, content, content.mobIndex("bee"),
                                       Rarity::Unique, terrain.spawnPoint(garden), garden, 50000.0, rng);
    CHECK(boss != NULL_ENTITY);
    bool announced = false;
    for (const auto& entry : spawner.bossSpawns) {
        CHECK(!maps.isPrestige(entry.realm));
        announced |= entry.entity == boss;
    }
    CHECK(announced);
}

TEST(prestige_hel_stock_is_denser_and_upper_bands_have_more_supers) {
    World world;
    CommandBuffer commands{world};
    Terrain terrain;
    SpawnSystem spawner;
    Rng rng{0xE114u};
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    spawner.worldMaps = &maps;
    spawner.run(world, terrain, shippedContent(), {}, rng, 50000.0, net::kTickSeconds, commands);
    commands.flush();
    bool found = false;
    const Realm hel = maps.realmOfId("hel", found);
    CHECK(found);
    const Realm prestigeHel = maps.prestigeRealm(hel, found);
    CHECK(found);
    if (!found) return;

    std::size_t ordinaryStock = 0;
    for (const auto realm : {hel, prestigeHel}) {
        std::vector<SpawnSystem::LatentSite> sites;
        spawner.latentSites(realm, sites);
        int deep = 0;
        int supers = 0;
        int entrance = 0;
        for (const auto& site : sites) {
            if (realm == prestigeHel) CHECK(!barredFromPrestige(site.rarity));
            if (site.difficulty == 0.0) ++entrance;
            if (site.difficulty < 65.0) continue;
            ++deep;
            supers += site.rarity == Rarity::Super;
        }
        CHECK(entrance > 0);
        CHECK(deep >= 12);
        if (realm == prestigeHel) {
            CHECK(sites.size() > ordinaryStock);
            CHECK(supers > deep * 0.4 && supers < deep * 0.9);
        } else {
            ordinaryStock = sites.size();
            CHECK(supers < deep * 0.1);
        }
        std::printf("Hel stock (%s): %zu total, %d/%d deep Super\n",
                    realm == prestigeHel ? "prestige" : "ordinary", sites.size(), supers, deep);
    }
}

TEST(prestige_jungle_sewers_and_hel_wake_local_mobs_at_the_shared_ambient_limit) {
    World world;
    CommandBuffer commands{world};
    Terrain terrain;
    SpawnSystem spawner;
    Rng rng{42u};
    WorldMaps maps;
    std::string error;
    if (!maps.load(dataDir(), &terrain, error)) {
        CHECK(false);
        return;
    }
    spawner.worldMaps = &maps;
    bool found = false;
    const Realm jungle = maps.realmOfId("jungle", found);
    CHECK(found);
    const Realm twin = maps.prestigeRealm(jungle, found);
    CHECK(found);
    if (!found) return;

    const Realm pyramid = maps.realmOfId("pyramid", found);
    CHECK(found);
    if (!found) return;
    const Realm prestigePyramid = maps.prestigeRealm(pyramid, found);
    CHECK(found);
    if (!found) return;
    spawner.setActivePyramidRealms(pyramid, prestigePyramid);

    // Many ordinary bands in view exhaust their share before the first
    // player enters the prestige twin. Use the shipped map geometry.
    std::vector<RealmPoint> viewers;
    for (const MapData& map : maps.maps()) {
        if (map.prestige() || map.id() != map.templateId()) continue;
        if (map.biome() == "pyramid" && map.realm() != pyramid) continue;
        for (const MapElement& band : map.elements()) {
            if (!band.isSpawnBand() || band.singular || band.forceRarity >= 0) continue;
            const Vec2 at{band.bounds.x + band.bounds.w * 0.5, band.bounds.y + band.bounds.h * 0.5};
            if (!terrain.blocked(at, map.realm())) viewers.push_back({at, map.realm()});
            if (viewers.size() == 60) break;
        }
        if (viewers.size() == 60) break;
    }
    const auto tick = [&](double time) {
        spawner.run(world, terrain, shippedContent(), viewers, rng, time, net::kTickSeconds, commands);
        commands.flush();
    };
    for (int pass = 0; pass < 12; ++pass) tick(50'000.0 + pass * kPopulationIntervalMillis);

    const int before = spawner.census().mobs;
    CHECK(before >= kMaxLiveMobs - 170);
    const Vec2 entrance{25217.0, 3577.0};
    viewers.push_back({entrance, twin});
    tick(55'550.0); // Arrive between regular population passes.

    int nearby = 0;
    Query<MobTag, Transform, MobType, AmbientMob> mobs{world};
    mobs.without<Dead>();
    mobs.each([&](Entity, MobTag&, Transform& at, MobType& type, AmbientMob&) {
        if (at.realm != twin) return;
        CHECK(!barredFromPrestige(type.rarity));
        if (std::abs(at.position.x - entrance.x) <= 1200.0 &&
            std::abs(at.position.y - entrance.y) <= 820.0) ++nearby;
    });
    CHECK(nearby > 0);
    CHECK(spawner.census().mobs <= kMaxLiveMobs);

    const Realm boxRealm = maps.realmOfId("prestige_sewers", found);
    CHECK(found);
    if (!found) return;
    const Vec2 box = terrain.resolveCircle({21400.0, 4050.0}, kPlayerBaseRadius, boxRealm);
    viewers.push_back({box, boxRealm});
    tick(55'600.0);
    int boxNearby = 0;
    mobs.each([&](Entity, MobTag&, Transform& at, MobType&, AmbientMob&) {
        if (at.realm == boxRealm && std::abs(at.position.x - box.x) <= 1400.0 &&
            std::abs(at.position.y - box.y) <= 1000.0) ++boxNearby;
    });
    CHECK(boxNearby > 0);
    CHECK(spawner.census().mobs <= kMaxLiveMobs);
    std::printf("[sewers] %d mobs near the box at the ambient budget\n", boxNearby);

    const Realm helRealm = maps.realmOfId("prestige_hel", found);
    CHECK(found);
    if (!found) return;
    const Vec2 hel = terrain.resolveCircle({4252.0, 12721.0}, kPlayerBaseRadius, helRealm);
    viewers.push_back({hel, helRealm});
    tick(55'650.0);
    int helNearby = 0;
    mobs.each([&](Entity, MobTag&, Transform& at, MobType& type, AmbientMob&) {
        if (at.realm != helRealm) return;
        CHECK(!barredFromPrestige(type.rarity));
        if (std::abs(at.position.x - hel.x) <= 1400.0 &&
            std::abs(at.position.y - hel.y) <= 1000.0) ++helNearby;
    });
    CHECK(helNearby > 0);
    CHECK(spawner.census().mobs <= kMaxLiveMobs);
    std::printf("[prestige hel] %d mobs near the upper band at the ambient budget\n", helNearby);
}

TEST(deep_prestige_factory_refills_at_the_shared_ambient_limit) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    bool found = false;
    const Realm realm = maps.realmOfId("prestige_computer", found);
    CHECK(found);
    if (!found) return;

    for (const auto seed : {42, 99, 2026}) {
        World world;
        CommandBuffer commands{world};
        SpawnSystem spawner;
        spawner.worldMaps = &maps;
        spawner.sizePlacementGrids(terrain);
        Rng rng{static_cast<std::uint64_t>(seed)};
        std::vector<RealmPoint> viewers;
        const auto tick = [&](double now) {
            spawner.run(world, terrain, shippedContent(), viewers, rng, now, net::kTickSeconds, commands);
            commands.flush();
        };
        for (int pass = 0; pass < 12; ++pass) tick(50'000 + pass * kPopulationIntervalMillis);
        CHECK(spawner.census().mobs > 0);
        spawner.mobCap = spawner.census().mobs + 128; // Existing live mobs use the entire normal budget.

        // End of the winding difficulty-70 section on the shipped Factory map.
        const Vec2 terminal{3456.0, 14464.0};
        CHECK(!terrain.blocked(terminal, realm));
        viewers.push_back({terminal, realm});
        const auto reachable = [&] {
            std::vector<Entity> local;
            Query<AmbientMob, MobTag, Transform, MobType> mobs{world};
            mobs.without<Dead>();
            mobs.each([&](Entity e, AmbientMob& ambient, MobTag&, Transform& where, MobType& type) {
                if (ambient.zone != kInvalidIndex && where.realm == realm &&
                    rarityIndex(type.rarity) < rarityIndex(Rarity::Unique) &&
                    std::abs(where.position.x - terminal.x) <= kSpawnViewportHalfWidth &&
                    std::abs(where.position.y - terminal.y) <= kSpawnViewportHalfHeight &&
                    !terrain.segmentBlocked(terminal, where.position, realm)) local.push_back(e);
            });
            return local;
        };
        tick(55'550); // A new viewer must wake monsters between population passes.
        const auto cleared = reachable();
        CHECK(!cleared.empty());
        for (const Entity entity : cleared) world.add<Dead>(entity);
        tick(56'000);
        for (const Entity entity : cleared) world.destroy(entity);
        for (int pass = 1; pass <= 16; ++pass) tick(56'000 + pass * kPopulationIntervalMillis);
        CHECK(!reachable().empty());
        CHECK(spawner.census().mobs <= spawner.mobCap);
    }
}

TEST(jungle_refills_near_a_stationary_viewer_after_kills) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    for (const char* mapId : {"jungle", "prestige_jungle"}) {
        bool found = false;
        const Realm realm = maps.realmOfId(mapId, found);
        CHECK(found);
        if (!found) return;
        const Vec2 at = terrain.resolveCircle({25217, 3577}, kPlayerBaseRadius, realm);
        World world;
        SpawnSystem spawner;
        spawner.worldMaps = &maps;
        CommandBuffer commands{world};
        Rng rng{42};
        const auto tick = [&](double now) {
            spawner.run(world, terrain, shippedContent(), {{at, realm}}, rng, now,
                        net::kTickSeconds, commands);
            commands.flush();
        };
        const auto nearby = [&] {
            std::vector<Entity> entities;
            Query<AmbientMob, Transform, MobType> mobs{world};
            mobs.without<Dead>();
            mobs.each([&](Entity e, AmbientMob& ambient, Transform& where, MobType& type) {
                if (ambient.zone != kInvalidIndex && where.realm == realm &&
                    rarityIndex(type.rarity) < rarityIndex(Rarity::Unique) &&
                    std::abs(where.position.x - at.x) <= 1200 &&
                    std::abs(where.position.y - at.y) <= 820) entities.push_back(e);
            });
            return entities;
        };
        for (int i = 0; i < 12; ++i) tick(50'000 + i * 500);
        const auto cleared = nearby();
        CHECK(!cleared.empty());
        for (const Entity e : cleared) world.add<Dead>(e);
        tick(56'000);
        for (const Entity e : cleared) world.destroy(e);
        for (int i = 1; i <= 13; ++i) tick(56'000 + i * 500);
        const auto refilled = nearby();
        CHECK(!refilled.empty());
        CHECK(spawner.census().mobs <= kMaxLiveMobs);
        std::printf("[stationary %s] cleared %zu, refilled %zu after 6.5 seconds\n",
                    mapId, cleared.size(), refilled.size());
    }
}

TEST(prestige_ocean_refills_repeated_clears_for_a_stationary_viewer) {
    WorldMaps full;
    Terrain terrain;
    std::string error;
    CHECK(full.load(dataDir(), &terrain, error));
    bool found = false;
    const Realm realm = full.realmOfId("prestige_2_ocean", found);
    CHECK(found);
    if (!found) return;
    const MapData& map = *full.forRealm(realm);
    std::vector<const MapElement*> bands;
    for (const auto& element : map.elements())
        if (element.isSpawnBand() && !element.singular && element.forceRarity < 0)
            bands.push_back(&element);
    CHECK(!bands.empty());
    if (bands.empty()) return;
    std::sort(bands.begin(), bands.end(), [](const auto* a, const auto* b) {
        return a->difficulty > b->difficulty;
    });
    Rng rng{400};
    Vec2 at;
    CHECK(map.spawnInElement(*bands.front(), rng, terrain, at));
    at = terrain.resolveCircle(at, kPlayerBaseRadius, realm);

    // Keep this scenario independent of stock in unrelated maps.
    std::vector<MapData> subset(static_cast<std::size_t>(worldMapSlot(realm)) + 1);
    for (std::size_t i = 0; i < subset.size(); ++i)
        subset[i].setId("unused_" + std::to_string(i));
    subset[worldMapSlot(realm)] = map;
    WorldMaps maps;
    maps.adoptMaps(std::move(subset));

    World world;
    SpawnSystem spawn;
    spawn.worldMaps = &maps;
    spawn.mobCap = 4500;
    spawn.sizePlacementGrids(terrain);
    CommandBuffer commands{world};
    const auto player = world.create();
    world.add<PlayerTag>(player);
    world.add<Transform>(player, Transform{at, 0.0, realm});
    world.add<Body>(player, Body{25.0, 1.0});
    world.add<Health>(player, Health{1e12, 1e12});
    const Vec2 viewport{1920, 1080};
    world.add<PlayerLocation>(player, PlayerLocation{viewport});
    const Vec2 centre = boundedViewportCentre(at, viewport, terrain.cameraExtent(realm));
    const std::vector<RealmPoint> viewers{{at, realm}};
    const auto& content = shippedContent();

    std::vector<Entity> cleared;
    int emptyTicks = 0;
    int longestEmpty = 0;
    int clears = 0;
    for (int tick = 0; tick < 360; ++tick) {
        const double now = (tick + 1) * 100.0;
        spawn.run(world, terrain, content, viewers, rng, now, 0.1, commands);
        commands.flush();
        if (!cleared.empty()) {
            spawn.answerDeaths(world, terrain, content, cleared, rng, now, 0.1, commands);
            commands.flush();
            for (Entity entity : cleared)
                if (world.isAlive(entity)) world.destroy(entity);
            cleared.clear();
        }
        if (tick < 60) continue;

        std::vector<Entity> visible;
        Query<MobTag, MobType, Transform, Body> mobs{world};
        mobs.without<Dead, Pet>();
        mobs.each([&](Entity e, MobTag&, MobType& type, Transform& where, Body& body) {
            if (where.realm != realm || content.mob(type.configIndex).neverAmbient) return;
            const double dx = std::max(0.0, std::abs(where.position.x - centre.x) - viewport.x * 0.5);
            const double dy = std::max(0.0, std::abs(where.position.y - centre.y) - viewport.y * 0.5);
            if (dx * dx + dy * dy <= body.radius * body.radius &&
                !terrain.segmentBlocked(at, where.position, realm)) visible.push_back(e);
        });
        emptyTicks = visible.empty() ? emptyTicks + 1 : 0;
        longestEmpty = std::max(longestEmpty, emptyTicks);
        if (tick % 60 == 0 && !visible.empty()) {
            cleared = std::move(visible);
            for (Entity entity : cleared) world.add<Dead>(entity);
            ++clears;
        }
        CHECK(spawn.census().mobs <= spawn.mobCap);
    }
    CHECK(clears >= 4);
    CHECK(longestEmpty <= 60);
}

TEST(deep_jungle_refills_a_reachable_empty_room_for_a_stationary_flower) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    bool found = false;
    const auto realm = maps.realmOfId("jungle", found);
    CHECK(found);
    if (!found) return;
    const auto& registry = shippedContent();
    const auto* map = maps.forRealm(realm);
    World world;
    SpawnSystem spawn;
    spawn.worldMaps = &maps;
    spawn.sizePlacementGrids(terrain);
    CommandBuffer commands{world};
    Rng rng{42};
    double now = 50'000.0;
    const auto tick = [&](const std::vector<RealmPoint>& viewers) {
        spawn.run(world, terrain, registry, viewers, rng, now, net::kTickSeconds, commands);
        commands.flush();
        now += kPopulationIntervalMillis;
    };
    for (int pass = 0; pass < 4; ++pass) tick({});
    std::vector<SpawnSystem::LatentSite> sites;
    spawn.latentSites(realm, sites);
    constexpr double half = kViewportBuffer + 120.0;
    const auto nearby = [&](Vec2 a, Vec2 b) {
        return std::abs(a.x - b.x) <= half && std::abs(a.y - b.y) <= half &&
            !terrain.segmentBlocked(a, b, realm);
    };
    std::optional<Vec2> stand;
    for (const auto& band : map->elements()) {
        if (!band.isSpawnBand() || band.difficulty < 70.0 || band.singular) continue;
        for (double y = band.bounds.y; !stand && y < band.bounds.bottom(); y += 160.0) {
            for (double x = band.bounds.x; !stand && x < band.bounds.right(); x += 160.0) {
                const Vec2 point{x, y};
                if (!band.contains(point) || terrain.blocked(point, realm) ||
                    std::any_of(sites.begin(), sites.end(), [&](const auto& site) {
                        return nearby(point, site.position);
                    })) continue;
                int usable = 0;
                for (int i = 0; i < 8; ++i) {
                    const auto at = point + Vec2::fromAngle(i * kPi / 4.0, 540.0);
                    usable += band.contains(at) && nearby(point, at);
                }
                if (usable >= 4) stand = point;
            }
        }
    }
    CHECK(stand.has_value());
    if (!stand) return;
    const auto player = world.create();
    world.add<PlayerTag>(player);
    world.add<Transform>(player, Transform{*stand, 0.0, realm});
    world.add<PlayerLocation>(player).viewport = {240.0, 240.0};
    for (int pass = 0; pass < 18; ++pass) tick({{*stand, realm}});
    int inRoom = 0;
    Query<AmbientMob, Transform, MobType>{world}.each(
        [&](Entity, AmbientMob&, Transform& at, MobType&) {
            if (at.realm == realm && nearby(*stand, at.position)) ++inRoom;
        });
    std::printf("Deep Jungle room: %d nearby mobs after stationary refill\n", inRoom);
    CHECK(inRoom > 0);
}

TEST(authored_maps_wake_blocked_ambient_records_without_viewer_movement) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    World world;
    SpawnSystem spawner;
    spawner.worldMaps = &maps;
    spawner.sizePlacementGrids(terrain);
    CommandBuffer commands{world};
    Rng rng{42};
    double now = 50'000.0;
    const auto tick = [&](const std::vector<RealmPoint>& viewers) {
        spawner.run(world, terrain, shippedContent(), viewers, rng, now,
                    net::kTickSeconds, commands);
        commands.flush();
        now += kPopulationIntervalMillis;
    };
    for (int pass = 0; pass < 5; ++pass) tick({});
    for (const char* id : {"garden", "desert", "computer", "ocean", "jungle", "prestige_jungle",
                           "hel", "ant_hell", "sewers", "prestige_sewers", "pyramid", "jurassic"}) {
        bool found = false;
        const Realm realm = maps.realmOfId(id, found);
        const auto* map = maps.forRealm(realm);
        CHECK(found && map != nullptr);
        if (!found || map == nullptr) continue;
        std::vector<SpawnSystem::LatentSite> sites;
        spawner.latentSites(realm, sites);
        // Probe a point with open ground in the same band around it.
        const bool narrow = map->biome() == "pyramid";
        const auto probe = std::find_if(sites.begin(), sites.end(), [&](const auto& site) {
            if (shippedContent().mob(site.mobIndex).segmentCount > 0 || terrain.blocked(site.position, realm))
                return false;
            return std::any_of(map->elements().begin(), map->elements().end(), [&](const auto& band) {
                if (!band.isSpawnBand() || band.singular || band.forceRarity >= 0 ||
                    band.difficulty != site.difficulty || !band.contains(site.position)) return false;
                int clear = 0;
                for (int i = 0; i < 8; ++i) {
                    const Vec2 at = site.position + Vec2::fromAngle(i * kPi / 4.0, narrow ? 400.0 : 600.0);
                    if (band.contains(at) && !terrain.segmentBlocked(site.position, at, realm)) ++clear;
                }
                return clear >= (narrow ? 1 : 6);
            });
        });
        CHECK(probe != sites.end());
        if (probe == sites.end()) continue;
        const Vec2 standing = probe->position;
        tick({{standing, realm}});
        std::vector<Entity> cleared;
        Query<AmbientMob, Transform, MobType> nearby{world};
        nearby.without<Dead>();
        nearby.each([&](Entity mob, AmbientMob& ambient, Transform& at, MobType& type) {
            if (ambient.zone != kInvalidIndex && at.realm == realm &&
                rarityIndex(type.rarity) < rarityIndex(Rarity::Super) &&
                distanceSq(at.position, standing) <= kRespawnScatter * kRespawnScatter)
                cleared.push_back(mob);
        });
        for (const Entity mob : cleared) world.add<Dead>(mob);
        tick({{standing, realm}});
        for (const Entity mob : cleared) world.destroy(mob);
        // Relocation checks a rotating, bounded set of blocked records. Allow
        // more than one pass through a congested band while the viewer stays put.
        double clearedAfter = 0.0;
        for (int pass = 1; pass <= 12 || (pass <= 24 && clearedAfter == 0.0); ++pass) {
            tick({{standing, realm}});
            sites.clear();
            spawner.latentSites(realm, sites);
            const bool held = std::any_of(sites.begin(), sites.end(), [&](const auto& site) {
                return distanceSq(site.position, standing) == 0.0;
            });
            if (!held && clearedAfter == 0.0) clearedAfter = pass * kPopulationIntervalMillis / 1000.0;
        }
        std::printf("[ambient wake] %s: cleared after %.1fs stationary (0 means held; %zu nearby mobs cleared)\n",
                    id, clearedAfter, cleared.size());
        CHECK(clearedAfter > 0.0);
    }
}

TEST(ghosts_spawn_independently_of_ambient_slots_outside_hel) {
    const double tile = kTileSize;
    auto fixture = fixtureMap(100, 100, fixtureDoor("spawn", "Spawn", 2 * tile, 2 * tile,
                                    tile, tile, true, 0.0), "",
                                    fixtureBand(tile, tile, 98 * tile, 98 * tile, "bee 100%", 35.0));
    fixture.insert(fixture.rfind('}'), R"(,"properties":[{"name":"spawnCurve","value":"florrclone"}])");
    const auto dir = stageDataDir("ghost-natural", {{"garden", fixture}, {"hel", fixture}});
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dir, &terrain, error));
    bool found = false;
    const Realm garden = maps.realmOfId("garden", found);
    CHECK(found);
    const Realm hel = maps.realmOfId("hel", found);
    CHECK(found);
    const auto ghost = shippedContent().mobIndex("ghost");
    World world;
    SpawnSystem spawner;
    spawner.worldMaps = &maps;
    spawner.restoreBossClock("garden", 1e9, 1e9);
    spawner.restoreBossClock("hel", 1e9, 1e9);
    CommandBuffer commands{world};
    Rng rng{23};
    const std::vector<RealmPoint> viewers{{{50*tile,50*tile}, garden}, {{50*tile,50*tile}, hel}};
    int peak = 0;
    for (int tick = 0; tick <= 6; ++tick) {
        spawner.run(world, terrain, shippedContent(), viewers, rng, 10'000.0 + tick * 90'000.0,
                    net::kTickSeconds, commands);
        commands.flush();
        int count = 0;
        int ordinary = 0;
        Query<MobTag, Transform, AmbientMob>{world}.each(
            [&](Entity e, MobTag&, Transform& at, AmbientMob& ambient) {
                if (ambient.independentGhost) {
                    CHECK(at.realm == garden);
                    CHECK(world.has<GhostTag>(e));
                    CHECK_EQ(ambient.zone, kInvalidIndex);
                    ++count;
                } else {
                    ++ordinary;
                }
            });
        CHECK(count <= 3);
        CHECK_EQ(spawner.census().mobs, ordinary);
        peak = std::max(peak, count);
    }
    CHECK_EQ(peak, 3);
    std::vector<SpawnSystem::LatentSite> sites;
    spawner.latentSites(garden, sites);
    CHECK(sites.size() >= 100);
    CHECK(std::none_of(sites.begin(), sites.end(), [&](const auto& site) {
        return site.mobIndex == ghost;
    }));
    removeDataDir(dir);
}

TEST(ghosts_spawn_at_most_three_in_shipped_prestige_sewers_during_restock) {
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    bool found = false;
    const auto realm = maps.realmOfId("prestige_sewers", found);
    CHECK(found);
    if (!found) return;
    World world;
    CommandBuffer commands{world};
    SpawnSystem spawner;
    spawner.worldMaps = &maps;
    Rng rng{672};
    const Vec2 box = terrain.resolveCircle({21400.0, 4050.0}, kPlayerBaseRadius, realm);
    const std::vector<RealmPoint> viewers{{box, realm}};
    int peak = 0;
    int totalRemoved = 0;
    for (int i = 0; i < 60; ++i) {
        spawner.run(world, terrain, shippedContent(), viewers, rng,
                    10'000.0 + i * 15'000.0, net::kTickSeconds, commands);
        commands.flush();
        int ghosts = 0;
        int ordinary = 0;
        std::vector<Entity> ordinaryNearby;
        Query<MobTag, Transform, AmbientMob>{world}.each(
            [&](Entity e, MobTag&, Transform& at, AmbientMob& ambient) {
                if (at.realm != realm || !isStanding(world, e)) return;
                if (world.has<GhostTag>(e)) {
                    CHECK(ambient.independentGhost);
                    CHECK_EQ(ambient.zone, kInvalidIndex);
                    ++ghosts;
                } else if (ambient.zone != kInvalidIndex) {
                    ++ordinary;
                    if (distanceSq(at.position, box) < 1300.0 * 1300.0 && ordinaryNearby.size() < 15)
                        ordinaryNearby.push_back(e);
                }
            });
        CHECK(ghosts <= 3);
        CHECK(ordinary > 0);
        peak = std::max(peak, ghosts);
        for (const auto e : ordinaryNearby) world.destroy(e);
        totalRemoved += static_cast<int>(ordinaryNearby.size());
    }
    CHECK_EQ(peak, 3);
    CHECK(totalRemoved > 10);
    std::printf("[prestige sewers] ghost peak %d, ordinary mobs removed during restock %d\n",
                peak, totalRemoved);
}

TEST(apex_crab_uses_the_existing_ocean_hyper_slot) {
    const double tile = kTileSize;
    auto map = fixtureMap(40, 40, fixtureDoor("spawn", "Spawn", 2 * tile, 2 * tile,
                          tile, tile, true, 0.0), "",
                          fixtureBand(tile, tile, 38 * tile, 38 * tile, "crab 100%", 70.0));
    map.insert(1, R"("properties":[{"name":"spawnCurve","type":"string","value":"florrclone"}],)");
    const auto dir = stageDataDir("ocean-apex-refill", {{"ocean", map}});
    CHECK(writeText(dir + "/maps.json", R"({"maps":[{"file":"ocean.tmj","prestige":true}]})"));
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dir, &terrain, error));
    bool found = false;
    const Realm ocean = maps.realmOfId("ocean", found);
    CHECK(found);

    bool sawApex = false;
    bool sawHyper = false;
    for (int seed = 1; seed <= 96 && !(sawApex && sawHyper); ++seed) {
        World world;
        SpawnSystem spawner;
        spawner.worldMaps = &maps;
        spawner.restoreBossClock("ocean", 100000000.0, 0.0);
        CommandBuffer commands{world};
        Rng rng{static_cast<std::uint32_t>(seed)};
        int slots = 0;
        Entity wildApex = NULL_ENTITY;
        for (int pass = 0; pass < 3; ++pass) {
            spawner.run(world, terrain, shippedContent(), {}, rng,
                        20000.0 + pass * kPopulationIntervalMillis, net::kTickSeconds, commands);
            commands.flush();
            slots = 0;
            Query<MobType, Transform, AmbientMob>{world}.each(
                [&](Entity entity, MobType& type, Transform& at, AmbientMob& ambient) {
                    if (type.rarity != Rarity::Apex && type.rarity != Rarity::Ascendant) return;
                    CHECK_EQ(at.realm, ocean);
                    CHECK(ambient.managedPopulation);
                    CHECK_EQ(type.configIndex, shippedContent().mobIndex("crab"));
                    ++slots;
                    if (type.rarity == Rarity::Ascendant) wildApex = entity;
                    sawHyper |= type.rarity == Rarity::Apex;
                });
            CHECK(slots <= 1);
        }
        if (wildApex != NULL_ENTITY) {
            const auto& registry = shippedContent();
            const auto crab = registry.mobIndex("crab");
            const double expected = registry.mobStats(crab, Rarity::Apex).health * 12.5;
            const auto& health = world.get<Health>(wildApex);
            CHECK_NEAR(health.current / expected, 1.0, 1e-12);
            CHECK_NEAR(health.max / expected, 1.0, 1e-12);
            sawApex = true;
            const double now = 20000.0 + 3 * kPopulationIntervalMillis;
            world.add<Dead>(wildApex);
            spawner.run(world, terrain, shippedContent(), {}, rng, now,
                        net::kTickSeconds, commands);
            commands.flush();
            CHECK_NEAR(spawner.bossCooldownLeft(Rarity::Apex, ocean, now),
                       kApexSpawnCooldownMillis, 1e-9);
        }
    }
    CHECK(sawHyper && sawApex);
    removeDataDir(dir);
}

TEST(prestige_twin_promotes_a_natural_super_and_starts_its_own_eternal_clock) {
    const double tile = kTileSize;
    const auto dir = stageDataDir("prestige-eternal", {{"garden", fixtureMap(40, 40,
        fixtureDoor("spawn", "Spawn", 2 * tile, 2 * tile, tile, tile, true, 0.0), "",
        fixtureBand(tile, tile, 38 * tile, 38 * tile, "garden 100%", 200.0))}});
    CHECK(writeText(dir + "/maps.json", R"({"maps":[{"file":"garden.tmj","prestige":true}]})"));
    World world;
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dir, &terrain, error));
    bool found = false;
    const Realm twin = maps.realmOfId("prestige_garden", found);
    CHECK(found);
    if (!found) { removeDataDir(dir); return; }

    SpawnSystem spawner;
    spawner.worldMaps = &maps;
    spawner.restoreBossClock("prestige_garden", 0.0, 0.0);
    spawner.restoreBossClock("garden", 800000.0, 800000.0);
    CommandBuffer commands{world};
    Rng rng{42};
    double now = 10000.0;
    for (int i = 0; i < 60; ++i) {
        now = 10000.0 + i * kPopulationIntervalMillis;
        spawner.run(world, terrain, shippedContent(), {{terrain.spawnPoint(twin), twin}},
                    rng, now, net::kTickSeconds, commands);
        commands.flush();
        if (spawner.bossCooldownLeft(Rarity::Unique, twin, now) > 0.0) break;
    }

    int eternals = 0;
    Query<MobTag, Transform, MobType> mobs{world};
    mobs.each([&](Entity, MobTag&, Transform& at, MobType& type) {
        if (at.realm != twin) return;
        if (type.rarity == Rarity::Unique) ++eternals;
        CHECK(type.rarity != Rarity::Apex);
    });
    CHECK_EQ(eternals, 1);
    CHECK_NEAR(spawner.bossCooldownLeft(Rarity::Unique, twin, now), kUniqueSpawnCooldownMillis, 1e-9);
    CHECK_NEAR(spawner.bossCooldownLeft(Rarity::Unique, Realm::Overworld, now), 800000.0 - now, 1e-9);
    CHECK(std::any_of(spawner.bossSpawns.begin(), spawner.bossSpawns.end(), [&](const auto& spawn) {
        return spawn.realm == twin && spawn.rarity == Rarity::Unique;
    }));
    removeDataDir(dir);
}

TEST(manual_spawn_cannot_put_a_unique_or_apex_into_a_prestige_twin) {
    World world;
    Terrain terrain;
    SpawnSystem spawner;
    Rng rng{7u};
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    spawner.worldMaps = &maps;
    bool found = false;
    const Realm garden = maps.realmOfId("garden", found);
    const Realm twin = maps.prestigeRealm(garden, found);
    CHECK(found);
    const std::uint16_t bee = shippedContent().mobIndex("bee");
    CHECK(bee != kInvalidIndex);
    const Vec2 at = terrain.spawnPoint(twin);

    CHECK(spawner.spawnMob(world, terrain, shippedContent(), bee, Rarity::Unique, at, twin, 0.0,
                           rng) == NULL_ENTITY);
    CHECK(spawner.spawnMob(world, terrain, shippedContent(), bee, Rarity::Apex, at, twin, 0.0,
                           rng) == NULL_ENTITY);
    // A manual Super still works.
    CHECK(spawner.spawnMob(world, terrain, shippedContent(), bee, Rarity::Super, at, twin, 0.0,
                           rng) != NULL_ENTITY);
}

TEST(prestige_xp_bonus_caps_the_jump_from_the_tier_below) {
    World world;
    Terrain terrain;
    SpawnSystem spawner;
    Rng rng{11u};
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    spawner.worldMaps = &maps;
    bool found = false;
    const Realm garden = maps.realmOfId("garden", found);
    const Realm twin = maps.prestigeRealm(garden, found);
    CHECK(found);
    const ContentRegistry& content = shippedContent();
    const std::uint16_t bee = content.mobIndex("bee");
    CHECK(bee != kInvalidIndex);
    const auto xpOf = [&](Rarity rarity, Realm realm) {
        const Entity e = spawner.spawnMob(world, terrain, content, bee, rarity,
                                          terrain.spawnPoint(realm), realm, 0.0, rng);
        CHECK(e != NULL_ENTITY);
        return e == NULL_ENTITY ? -1.0 : world.get<Bounty>(e).xp;
    };

    // The plain garden pays a tier its own XP.
    CHECK(xpOf(Rarity::Super, garden) == content.mobStats(bee, Rarity::Super).xp);
    // A prestige tier earns at most 50% more than the former payout and
    // never more than the same monster in the ordinary biome.
    CHECK(xpOf(Rarity::Super, twin) == std::min(content.mobStats(bee, Rarity::Ultra).xp * 1.5,
        content.mobStats(bee, Rarity::Super).xp));
    CHECK(xpOf(Rarity::Ultra, twin) == std::min(content.mobStats(bee, Rarity::Mythic).xp * 1.5,
        content.mobStats(bee, Rarity::Ultra).xp));
    CHECK(xpOf(Rarity::Uncommon, twin) == std::min(content.mobStats(bee, Rarity::Common).xp * 1.5,
        content.mobStats(bee, Rarity::Uncommon).xp));
    CHECK(xpOf(Rarity::Common, twin) == content.mobStats(bee, Rarity::Common).xp);
    CHECK(content.mobStats(bee, Rarity::Ultra).xp < content.mobStats(bee, Rarity::Super).xp);
}

// ---------------------------------------------------------------------------
// The join
// ---------------------------------------------------------------------------

TEST(only_a_tree_with_prestige_joins_the_prestige_biome) {
    Harness h("prestige-join",
              [](const std::string& path) {
                  seedUser(path, "climber", "password7");
                  seedUser(path, "walker", "password7");
                  seedProgress(path, "climber", true);
                  seedProgress(path, "walker", false);
              },
              dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    World& world = h.server.world();
    const WorldMaps& maps = h.server.worldMaps();
    bool found = false;
    const Realm twin = maps.prestigeRealm(Realm::Overworld, found);
    CHECK(found);

    // Without the talent the flag is refused, and said so.
    NetClient walker;
    CHECK(logIn(h, walker, "walker"));
    CHECK(join(h, walker, "walker", true));
    CHECK(realmOf(world, bodyNamed(world, "walker")) == Realm::Overworld);
    CHECK(sawText(walker, "Prestige is a talent"));

    // With it, the default door's twin.
    NetClient climber;
    CHECK(logIn(h, climber, "climber"));
    CHECK(join(h, climber, "climber", true));
    CHECK(realmOf(world, bodyNamed(world, "climber")) == twin);
    CHECK(climber.view().realm() == twin);

    // A named door goes to that door's twin.
    climber.leaveGame();
    h.step(5, {&climber, &walker});
    const SpawnChoice* desert = nullptr;
    for (const auto& door : maps.doors()) {
        const auto* map = maps.forRealm(door.realm);
        if (map && map->id() == "desert") { desert = &door; break; }
    }
    CHECK(desert != nullptr);
    if (desert != nullptr) {
        CHECK(join(h, climber, "climber", true, desert->id));
        const Realm desertTwin = maps.prestigeRealm(desert->realm, found);
        CHECK(found);
        CHECK(realmOf(world, bodyNamed(world, "climber")) == desertTwin);
    }

    // The arena has no twin: the flag means nothing there.
    climber.leaveGame();
    h.step(5, {&climber, &walker});
    CHECK(join(h, climber, "climber", true, kArenaSpawnChoice));
    CHECK(realmOf(world, bodyNamed(world, "climber")) == Realm::Arena);
}

TEST(prestige_is_bought_with_175_points_and_not_on_the_maze_tree) {
    Harness h("prestige-buy",
              [](const std::string& path) {
                  seedUser(path, "rich", "password7");
                  seedUser(path, "poor", "password7");
                  seedProgress(path, "rich", false, 88);
                  seedProgress(path, "poor", false, 87);
              },
              dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient poor;
    CHECK(logIn(h, poor, "poor"));
    CHECK(awaitProfile(h, poor, [](const Profile& p) { return p.level == 87; }));
    poor.requestUpgradeSkill(SkillId::Prestige, 0);
    CHECK(h.stepUntil({&poor}, [&] { return sawText(poor, "need 175"); }, 60));
    CHECK(!poor.profile().skills.prestigeUnlocked());

    NetClient rich;
    CHECK(logIn(h, rich, "rich"));
    CHECK(awaitProfile(h, rich, [](const Profile& p) { return p.level == 88; }));
    rich.requestUpgradeSkill(SkillId::Prestige, 0);
    CHECK(awaitProfile(h, rich, [](const Profile& p) {
        return p.skills.prestigeUnlocked() && p.talentPoints() == 1;
    }));

    // A reset hands the points back and the next join is an ordinary one.
    rich.requestResetSkills();
    CHECK(awaitProfile(h, rich, [](const Profile& p) { return !p.skills.prestigeUnlocked(); }));
    CHECK(join(h, rich, "rich", true));
    World& world = h.server.world();
    CHECK(realmOf(world, bodyNamed(world, "rich")) == Realm::Overworld);
}

TEST(a_reset_in_a_prestige_biome_sends_the_flower_home) {
    Harness h("prestige-reset-kick",
              [](const std::string& path) {
                  seedUser(path, "climber", "password7");
                  seedUser(path, "walker", "password7");
                  seedProgress(path, "climber", true);
                  seedProgress(path, "walker", true);
              },
              dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    World& world = h.server.world();

    NetClient climber;
    CHECK(logIn(h, climber, "climber"));
    CHECK(awaitProfile(h, climber, [](const Profile& p) { return p.skills.prestigeUnlocked(); }));
    CHECK(join(h, climber, "climber", true));
    CHECK(h.server.worldMaps().isPrestige(realmOf(world, bodyNamed(world, "climber"))));

    // Without the talent the flower may not stay: the body goes, and the
    // client is told so rather than left drawing a world it is not in.
    climber.requestResetSkills();
    CHECK(h.stepUntil({&climber}, [&] { return climber.leftWorld; }, 60));
    CHECK(climber.status() == NetClient::Status::LoggedIn);
    CHECK(bodyNamed(world, "climber") == NULL_ENTITY);
    CHECK(awaitProfile(h, climber, [](const Profile& p) { return !p.skills.prestigeUnlocked(); }));

    // And the next join, asking for prestige all the same, is an ordinary one.
    CHECK(join(h, climber, "climber", true));
    CHECK(realmOf(world, bodyNamed(world, "climber")) == Realm::Overworld);

    // A reset in an ordinary biome leaves the flower where it is.
    NetClient walker;
    CHECK(logIn(h, walker, "walker"));
    CHECK(awaitProfile(h, walker, [](const Profile& p) { return p.skills.prestigeUnlocked(); }));
    CHECK(join(h, walker, "walker", false));
    walker.requestResetSkills();
    CHECK(awaitProfile(h, walker, [](const Profile& p) { return !p.skills.prestigeUnlocked(); }));
    h.step(10, {&walker});
    CHECK(!walker.leftWorld);
    CHECK(walker.status() == NetClient::Status::Playing);
    CHECK(bodyNamed(world, "walker") != NULL_ENTITY);
}

TEST(a_prestige_super_is_neither_announced_nor_mourned) {
    // A super is a prestige biome's everyday stock, not a boss: no spawn line,
    // no "has been defeated" line, whether it dies or an operator clears it.
    Harness h("prestige-quiet-super",
              [](const std::string& path) {
                  seedUser(path, "warden", "password7", true);
                  seedProgress(path, "warden", true);
              },
              dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    World& world = h.server.world();

    NetClient client;
    CHECK(logIn(h, client, "warden"));
    CHECK(join(h, client, "warden", true));
    const Realm realm = realmOf(world, bodyNamed(world, "warden"));
    CHECK(h.server.worldMaps().isPrestige(realm));

    CHECK(say(h, client, "/admin spawn bee super"));
    CHECK(sawText(client, "Spawned super bee"));
    const std::uint16_t bee = shippedContent().mobIndex("bee");
    std::vector<Entity> supers;
    Query<MobTag, MobType, Transform> mobs{world};
    mobs.each([&](Entity e, MobTag&, MobType& type, Transform& at) {
        if (type.configIndex == bee && type.rarity == Rarity::Super && at.realm == realm) {
            supers.push_back(e);
        }
    });
    CHECK_EQ(supers.size(), std::size_t(1));
    for (const Entity e : supers) {
        Afflictions* afflictions = world.tryGet<Afflictions>(e);
        if (afflictions == nullptr) {
            world.add<Afflictions>(e, Afflictions{});
            afflictions = world.tryGet<Afflictions>(e);
        }
        afflictions->poisonStacks.push_back({NULL_ENTITY, 1e12, 1e18});
    }
    CHECK(h.stepUntil({&client}, [&] {
        for (const Entity e : supers) {
            if (world.isAlive(e)) return false;
        }
        return true;
    }, 120));
    h.step(10, {&client});

    // And one cleared by killall, which announces the bosses it removes.
    CHECK(say(h, client, "/admin spawn bee super"));
    CHECK(say(h, client, "/admin killall"));
    h.step(10, {&client});

    // The plain biomes' supers are still announced server-wide, "somewhere";
    // what must not appear is a line about THIS bee, which would say it
    // spawned here.
    CHECK(!sawText(client, "Super Bee has spawned!"));
    CHECK(!sawText(client, "Super Bee has been defeated"));
}

TEST(pyramid_layouts_have_connected_routes_and_share_the_active_spawn_rules) {
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    if (!maps.load(dataDir(), &terrain, error)) { CHECK(false); return; }

    const auto center = [](const MapElement& object) {
        return Vec2{object.bounds.x + object.bounds.w * 0.5, object.bounds.y + object.bounds.h * 0.5};
    };
    const auto exitPad = [](const MapData& map) {
        return std::find_if(map.elements().begin(), map.elements().end(), [](const MapElement& object) {
            return object.kind == MapElementKind::Teleporter;
        });
    };
    int layouts = 0;
    for (const MapData& map : maps.maps()) {
        if (map.biome() != "pyramid" || map.prestige()) continue;
        ++layouts;
        const Realm realm = map.realm();
        bool found = false;
        const Realm twin = maps.prestigeRealm(realm, found);
        if (!found) { CHECK(false); continue; }
        CHECK(realmFromByte(static_cast<std::uint8_t>(twin)) == twin);
        const auto entrances = map.playerSpawns();
        const auto pad = exitPad(map);
        if (entrances.size() != 1 || pad == map.elements().end()) { CHECK(false); continue; }
        const Vec2 entrance = center(*entrances.front());
        const Vec2 exit = center(*pad);
        CHECK(!terrain.resolveWall(entrance, kPlayerBaseRadius, realm).collided);
        CHECK(!terrain.resolveWall(exit, kPlayerBaseRadius, realm).collided);

        // Test the collision polygons used by the server, including the
        // transformed tiles. Every walkable sample must be reachable from
        // the entrance by a flower with room to clear each step.
        constexpr int step = 64;
        const Vec2 extent = terrain.realmExtent(realm);
        const int width = static_cast<int>(extent.x) / step;
        const int height = static_cast<int>(extent.y) / step;
        const auto grid = [width](Vec2 at) {
            return static_cast<int>(at.y) / step * width + static_cast<int>(at.x) / step;
        };
        const auto sample = [width](int cell) {
            return Vec2{static_cast<double>(cell % width * step + step / 2),
                        static_cast<double>(cell / width * step + step / 2)};
        };
        std::vector<unsigned char> open(width * height);
        int walkable = 0;
        for (int cell = 0; cell < width * height; ++cell) {
            if (terrain.resolveWall(sample(cell), kPlayerBaseRadius, realm).collided) continue;
            open[cell] = 1;
            ++walkable;
        }
        const int start = grid(entrance);
        if (!open[start]) { CHECK(false); continue; }
        std::vector<int> reached{start};
        open[start] = 2;
        for (std::size_t i = 0; i < reached.size(); ++i) {
            const int cell = reached[i];
            const int x = cell % width;
            for (const int next : {x > 0 ? cell - 1 : -1, x + 1 < width ? cell + 1 : -1,
                                   cell - width, cell + width}) {
                if (next < 0 || next >= width * height || open[next] != 1) continue;
                if (terrain.resolveWall((sample(cell) + sample(next)) * 0.5, kPlayerBaseRadius, realm).collided)
                    continue;
                open[next] = 2;
                reached.push_back(next);
            }
        }
        CHECK(open[grid(exit)] == 2);
        CHECK_EQ(reached.size(), static_cast<std::size_t>(walkable));

        Rng rng{20261108};
        for (const Realm current : {realm, twin}) {
            const MapData& currentMap = *maps.forRealm(current);
            const auto currentPad = exitPad(currentMap);
            WorldMaps::Destination destination;
            if (currentPad == currentMap.elements().end() ||
                !maps.resolveTeleporter(*currentPad, rng, terrain, destination)) {
                CHECK(false);
                continue;
            }
            CHECK(maps.forRealm(destination.realm)->biome() == "desert");
            CHECK(maps.isPrestige(destination.realm) == (current == twin));
            CHECK(!terrain.blocked(destination.position, destination.realm));
        }

        World world;
        CommandBuffer commands{world};
        SpawnSystem spawner;
        spawner.worldMaps = &maps;
        spawner.sizePlacementGrids(terrain);
        spawner.setActivePyramidRealms(realm, twin);
        spawner.restoreBossClock("pyramid", 0.0, 0.0);
        for (int pass = 0; pass < 12; ++pass) {
            spawner.run(world, terrain, shippedContent(), {{entrance, realm}, {entrance, twin}},
                        rng, 50'000.0 + pass * kPopulationIntervalMillis, net::kTickSeconds, commands);
            commands.flush();
        }

        int ordinary = 0, prestige = 0, farm = 0, eternal = 0, hyper = 0;
        Query<MobTag, Transform, MobType, AmbientMob> mobs{world};
        mobs.without<Dead>();
        mobs.each([&](Entity entity, MobTag&, Transform& at, MobType& mob, AmbientMob& ambient) {
            if (maps.forRealm(at.realm) == nullptr || maps.forRealm(at.realm)->biome() != "pyramid")
                return;
            CHECK(at.realm == realm || at.realm == twin);
            if (at.realm == realm) {
                ordinary += ambient.zone != kInvalidIndex;
                farm += world.has<SuperFarmMob>(entity);
                eternal += mob.rarity == Rarity::Unique;
                hyper += mob.rarity == Rarity::Apex;
            } else if (at.realm == twin) {
                prestige += ambient.zone != kInvalidIndex;
                CHECK(!barredFromPrestige(mob.rarity));
            }
        });
        CHECK(ordinary > 0);
        CHECK(prestige > 0);
        CHECK_EQ(farm, kSuperFarmPopulation);
        CHECK(eternal > 0);
        CHECK(hyper > 0);
        for (const MapData& inactive : maps.maps()) {
            if (inactive.biome() != "pyramid" || inactive.realm() == realm || inactive.realm() == twin)
                continue;
            std::vector<SpawnSystem::LatentSite> sites;
            spawner.latentSites(inactive.realm(), sites);
            CHECK(sites.empty());
        }
        std::printf("[pyramid %s] reachable %zu/%d, ordinary %d, prestige %d, farm %d, Eternal %d, Hyper %d\n",
                    map.id().c_str(), reached.size(), walkable, ordinary, prestige, farm, eternal, hyper);

        std::vector<SpawnSystem::LatentSite> previousStock;
        spawner.latentSites(twin, previousStock);
        CHECK(!previousStock.empty());
        const Realm next = maps.realmOfId(map.id() == "pyramid" ? "pyramid_alt" : "pyramid", found);
        CHECK(found);
        const Realm nextTwin = maps.prestigeRealm(next, found);
        CHECK(found);
        spawner.setActivePyramidRealms(next, nextTwin);
        std::vector<SpawnSystem::LatentSite> retiredStock;
        spawner.latentSites(twin, retiredStock);
        CHECK(retiredStock.empty());
    }
    CHECK_EQ(layouts, 4);
}

TEST(prestige_pyramid_scrolls_follow_each_active_layout_and_reveal_one_visit) {
    const std::array<const char*, 3> names{"pyramidwalker", "pyramidclimber", "pyramidveteran"};
    Harness h("prestige-pyramid-scrolls", [&](const std::string& path) {
        for (const char* name : names) seedUser(path, name, "password7");
        seedProgress(path, names[1], true);
        Database db;
        std::string error;
        db.load(path, error);
        if (const Account* account = db.findUser(names[2])) {
            db.progress(account->id).skills.set(SkillId::Prestige, 1);
            db.markDirty();
            db.save();
        }
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;

    const auto& maps = h.server.worldMaps();
    bool found = false;
    const auto baseRealm = maps.realmOfId("pyramid", found);
    CHECK(found);
    const auto& choices = maps.spawnChoices();
    const auto entry = std::find_if(choices.begin(), choices.end(), [&](const SpawnChoice& choice) {
        return choice.realm == baseRealm;
    });
    CHECK(entry != choices.end());
    if (!found || entry == choices.end()) return;

    std::array<NetClient, 3> clients;
    auto& world = h.server.world();
    std::array<Entity, 3> bodies;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        CHECK(logIn(h, clients[i], names[i]));
        CHECK(join(h, clients[i], names[i], i != 0, entry->id));
        bodies[i] = bodyNamed(world, names[i]);
        CHECK(bodies[i] != NULL_ENTITY);
        if (bodies[i] == NULL_ENTITY) return;
    }

    auto poll = [&] {
        for (int frame = 0; frame < 12; ++frame) {
            h.server.serviceNetwork(1);
            for (auto& client : clients) client.poll(1);
        }
    };
    auto scrollsByRealm = [&] {
        std::map<Realm, std::vector<Entity>> result;
        Query<Replicated, Transform>{world}.each([&](Entity e, const Replicated& entry,
                                                      const Transform& at) {
            if (entry.kind == net::EntityKind::Effect &&
                entry.typeIndex == static_cast<std::uint16_t>(GroundEffectKind::PyramidMap)) {
                result[at.realm].push_back(e);
            }
        });
        return result;
    };

    std::set<Realm> layouts;
    for (int cycle = 0; cycle < 4; ++cycle) {
        poll();
        const auto ordinary = realmOf(world, bodies[0]);
        layouts.insert(ordinary);
        const double clock = GameServerPeer::practiceClock(h.server) + cycle * 100000.0;
        GameServerPeer::stagePyramidMaps(h.server, clock);
        auto stock = scrollsByRealm();
        CHECK_EQ(stock.size(), 3u);
        for (std::size_t tier = 0; tier < clients.size(); ++tier) {
            const Realm realm = realmOf(world, bodies[tier]);
            const MapData* map = maps.forRealm(realm);
            CHECK(map != nullptr);
            if (!map) return;
            CHECK_EQ(map->biome(), std::string("pyramid"));
            CHECK_EQ(map->prestigeLevel(), static_cast<int>(tier));
            CHECK(!map->showsMinimap());
            CHECK(clients[tier].view().realm() == realm);
            CHECK(!clients[tier].pyramidMapRevealed);
            CHECK_EQ(stock[realm].size(), 40u);
        }
        if (stock.size() != 3 || stock[realmOf(world, bodies[1])].empty() ||
            stock[realmOf(world, bodies[2])].empty()) return;

        for (std::size_t tier = 0; tier < clients.size(); ++tier) {
            const Realm realm = realmOf(world, bodies[tier]);
            const Entity picked = stock[realm].front();
            const auto inventory = clients[tier].profile().inventory.size();
            world.get<Transform>(bodies[tier]).position = world.get<Transform>(picked).position;
            GameServerPeer::stagePyramidMaps(h.server, clock);
            poll();
            CHECK(clients[tier].pyramidMapRevealed);
            CHECK(!world.isAlive(picked));
            CHECK_EQ(scrollsByRealm()[realm].size(), 39u);
            CHECK_EQ(clients[tier].profile().inventory.size(), inventory);
            for (std::size_t other = tier + 1; other < clients.size(); ++other) {
                CHECK(!clients[other].pyramidMapRevealed);
            }
        }
        GameServerPeer::stagePyramidMaps(h.server, clock + 29999.0);
        for (const auto body : bodies) CHECK_EQ(scrollsByRealm()[realmOf(world, body)].size(), 39u);
        GameServerPeer::stagePyramidMaps(h.server, clock + 30000.0);
        const auto refilled = scrollsByRealm();
        for (const auto body : bodies) CHECK_EQ(refilled.at(realmOf(world, body)).size(), 40u);
        if (cycle < 3) {
            GameServerPeer::advancePyramidCycle(h.server);
            CHECK(scrollsByRealm().empty());
        }
    }
    CHECK_EQ(layouts.size(), 4u);

    // Leaving after a real pickup resets the visit even at the same prestige tier.
    CHECK(clients[1].pyramidMapRevealed);
    clients[1].leaveGame();
    for (int frame = 0; frame < 12; ++frame) {
        h.server.serviceNetwork(1);
        clients[1].poll(1);
    }
    CHECK(!clients[1].pyramidMapRevealed);
    CHECK(join(h, clients[1], names[1], true, entry->id));
    CHECK(!clients[1].pyramidMapRevealed);
}

TEST(pyramid_rotation_returns_ordinary_and_prestige_players_to_the_new_entrance) {
    Harness h("pyramid-rotation",
              [](const std::string& path) {
                  seedUser(path, "pyramidwalker", "password7");
                  seedUser(path, "pyramidclimber", "password7");
                  seedProgress(path, "pyramidclimber", true);
              }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    const auto& maps = h.server.worldMaps();
    bool found = false;
    const Realm pyramid = maps.realmOfId("pyramid", found);
    CHECK(found);
    const auto& choices = maps.spawnChoices();
    const auto entry = std::find_if(choices.begin(), choices.end(), [&](const SpawnChoice& choice) {
        return choice.realm == pyramid;
    });
    CHECK(entry != choices.end());
    if (!found || entry == choices.end()) return;
    NetClient ordinary, prestige;
    CHECK(logIn(h, ordinary, "pyramidwalker"));
    CHECK(join(h, ordinary, "pyramidwalker", false, entry->id));
    CHECK(logIn(h, prestige, "pyramidclimber"));
    CHECK(join(h, prestige, "pyramidclimber", true, entry->id));
    World& world = h.server.world();
    const Entity walker = bodyNamed(world, "pyramidwalker");
    const Entity climber = bodyNamed(world, "pyramidclimber");
    if (walker == NULL_ENTITY || climber == NULL_ENTITY) { CHECK(false); return; }

    const auto oldOrdinary = realmOf(world, walker);
    const auto oldPrestige = realmOf(world, climber);
    CHECK_EQ(maps.forRealm(oldOrdinary)->biome(), std::string("pyramid"));
    CHECK_EQ(maps.forRealm(oldPrestige)->biome(), std::string("pyramid"));
    CHECK(maps.isPrestige(oldPrestige));
    CHECK(ordinary.view().realm() == oldOrdinary);
    CHECK(prestige.view().realm() == oldPrestige);

    const auto padFor = [](const MapData& map, const std::string& target) {
        return std::find_if(map.elements().begin(), map.elements().end(), [&](const MapElement& object) {
            return object.kind == MapElementKind::Teleporter && object.targetMap == target;
        });
    };
    Rng padRng{123u};
    std::set<Realm> visited;
    for (int i = 0; i < 4; ++i) {
        const Realm before = realmOf(world, walker);
        const Entity retired = world.create();
        world.add<MobTag>(retired);
        world.add<Transform>(retired, Transform{{2000.0, 2000.0}, 0.0, before});
        world.get<Transform>(walker).position = {2000.0, 2000.0};
        world.get<Transform>(climber).position = {2200.0, 2200.0};
        GameServerPeer::advancePyramidCycle(h.server);
        CHECK(!world.isAlive(retired));
        const Realm newOrdinary = realmOf(world, walker);
        const Realm newPrestige = realmOf(world, climber);
        CHECK(newOrdinary != before);
        bool twinFound = false;
        CHECK(maps.prestigeRealm(newOrdinary, twinFound) == newPrestige);
        CHECK(twinFound);
        visited.insert(newOrdinary);

        for (const auto player : {walker, climber}) {
            const auto at = world.get<Transform>(player);
            const MapData& map = *maps.forRealm(at.realm);
            const auto doors = map.playerSpawns();
            CHECK(!doors.empty());
            if (!doors.empty()) CHECK(doors.front()->bounds.contains(at.position));
            CHECK(!h.server.terrain().blocked(at.position, at.realm));

            const auto outgoing = padFor(map, maps.isPrestige(at.realm) ? "prestige_desert" : "desert");
            WorldMaps::Destination exit;
            if (outgoing == map.elements().end() ||
                !maps.resolveTeleporter(*outgoing, padRng, h.server.terrain(), exit)) {
                CHECK(false);
                continue;
            }
            GameServerPeer::markTeleport(h.server, player, exit.realm, exit.position);
            CHECK(realmOf(world, player) == exit.realm);
            const MapData& desert = *maps.forRealm(exit.realm);
            const auto incoming = padFor(desert, maps.isPrestige(exit.realm) ? "prestige_pyramid" : "pyramid");
            WorldMaps::Destination arrival;
            if (incoming == desert.elements().end() ||
                !maps.resolveTeleporter(*incoming, padRng, h.server.terrain(), arrival)) {
                CHECK(false);
                continue;
            }
            GameServerPeer::markTeleport(h.server, player, arrival.realm, arrival.position);
            CHECK(realmOf(world, player) == at.realm);
            CHECK(!h.server.terrain().blocked(world.get<Transform>(player).position, at.realm));
        }
        for (int frame = 0; frame < 20; ++frame) {
            h.server.serviceNetwork(1);
            ordinary.poll(1);
            prestige.poll(1);
        }
        CHECK(ordinary.view().realm() == newOrdinary);
        CHECK(prestige.view().realm() == newPrestige);
    }
    CHECK_EQ(visited.size(), std::size_t(4));
    CHECK(realmOf(world, walker) == oldOrdinary);
    CHECK(realmOf(world, climber) == oldPrestige);
}

TEST(wave_chat_creates_a_private_code_and_round_trips_to_the_origin) {
    Harness h("wave-chat",
              [](const std::string& path) {
                  seedUser(path, "wavehost", "password7");
                  seedUser(path, "waveguest", "password7");
              },
              dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient host, guest;
    CHECK(logIn(h, host, "wavehost"));
    CHECK(join(h, host, "wavehost", false));
    CHECK(logIn(h, guest, "waveguest"));
    CHECK(join(h, guest, "waveguest", false));

    World& world = h.server.world();
    const auto hostBody = bodyNamed(world, "wavehost");
    const auto guestBody = bodyNamed(world, "waveguest");
    if (hostBody == NULL_ENTITY || guestBody == NULL_ENTITY) {
        CHECK(false);
        return;
    }
    const auto origin = world.get<Transform>(guestBody).realm;
    guest.requestWaveRoomStatus();
    CHECK(h.stepUntil({&host, &guest}, [&] { return guest.waveRoomStatus.known; }));
    CHECK_EQ(guest.waveRoomStatus.occupied, 0);
    CHECK(!guest.waveRoomStatus.dailyCapReached);
    std::string code;
    host.sendChat("/wave start");
    CHECK(h.stepUntil({&host, &guest}, [&] {
        for (const auto& line : host.chat()) {
            const auto marker = line.text.find("已进入 试炼房 ");
            if (marker == std::string::npos) continue;
            const auto firstDigit = line.text.find_first_of("0123456789", marker);
            if (firstDigit != std::string::npos) code = line.text.substr(firstDigit, 6);
        }
        return code.size() == 6 && world.has<WavePlayer>(hostBody);
    }));
    if (code.size() != 6) return;
    CHECK(!sawText(guest, code));
    const auto arena = world.get<Transform>(hostBody).realm;
    CHECK(arena != origin);

    guest.sendChat("/wave join " + code);
    CHECK(h.stepUntil({&host, &guest}, [&] {
        return world.has<WavePlayer>(guestBody) && guest.view().realm() == arena;
    }));
    guest.requestWaveRoomStatus();
    CHECK(h.stepUntil({&host, &guest}, [&] { return guest.waveRoomStatus.occupied == 1; }));
    CHECK(host.view().self().wave.durationMillis == 30000);
    const Vec2 rescuePoint = world.get<Transform>(hostBody).position;
    world.get<Transform>(guestBody).position = rescuePoint;
    world.get<Health>(guestBody).current = 0.0;
    world.add<Dead>(guestBody);
    CHECK(h.stepUntil({&host, &guest}, [&] { return isStanding(world, guestBody); }, 140));
    guest.sendChat("/wave leave");
    CHECK(h.stepUntil({&host, &guest}, [&] {
        return !world.has<WavePlayer>(guestBody) && guest.view().realm() == origin;
    }));
    CHECK(world.has<WavePlayer>(hostBody));
    guest.requestWaveRoomStatus();
    CHECK(h.stepUntil({&host, &guest}, [&] { return guest.waveRoomStatus.occupied == 1; }));
    CHECK(h.stepUntil({&guest}, [&] { return guest.waveResultNotice().sequence > 0; }));
    CHECK(guest.waveResultNotice().lines[1].find("本局 +0 星星") != std::string::npos);

    const auto awardedStars = [&] { return world.get<PlayerProgress>(hostBody).stars; };
    const double initialStars = awardedStars();
    double total = initialStars;
    constexpr int wavesPerRun = 5;
    for (int run = 0; run < 4; ++run) {
        if (run > 0) {
            host.sendChat("/wave start");
            CHECK(h.stepUntil({&host, &guest}, [&] { return world.has<WavePlayer>(hostBody); }));
        }
        world.get<Health>(hostBody).invulnerableUntilMillis = std::numeric_limits<double>::infinity();
        h.clock += 30000.0;
        bool sawEternal = false;
        for (int wave = 1; wave <= wavesPerRun; ++wave) {
            total += 7500.0;
            CHECK(h.stepUntil({&host, &guest}, [&] {
                if (awardedStars() >= total) return true;
                for (const Entity enemy : Query<WaveEnemy>{world}.collect()) {
                    if (!isStanding(world, enemy)) continue;
                    if (world.get<MobType>(enemy).rarity == Rarity::Unique) sawEternal = true;
                    world.get<Bounty>(enemy).credit(hostBody, world.get<Health>(enemy).max);
                    world.add<Dead>(enemy);
                }
                return false;
            }, 850));
            CHECK_NEAR(awardedStars(), total, 1e-9);
        }
        CHECK(sawEternal);
        const auto noticeBefore = host.waveResultNotice().sequence;
        host.sendChat("/wave leave");
        CHECK(h.stepUntil({&host, &guest}, [&] {
            return host.waveResultNotice().sequence > noticeBefore && host.view().realm() == origin;
        }));
        CHECK(host.waveResultNotice().lines[1] == "通关 5 波 · 本局 +37500 星星");
    }
    CHECK(h.stepUntil({&host, &guest}, [&] { return host.profile().stars >= total; }));
    CHECK_NEAR(host.profile().stars, initialStars + 150000.0, 1e-9);
}

TEST(wave_kills_feed_totem_without_paying_world_kill_rewards) {
    Harness h("wave-totem", [](const std::string& path) {
        seedUser(path, "totemhost", "password7");
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        const auto* account = db.findUser("totemhost");
        if (!account) { CHECK(false); return; }
        db.progress(account->id).addItem(Rarity::Uncommon, "petal_totem", 1);
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    NetClient host;
    if (!h.ready || !logIn(h, host, "totemhost") || !join(h, host, "totemhost", false)) {
        CHECK(false);
        return;
    }
    World& world = h.server.world();
    const Entity player = bodyNamed(world, "totemhost");
    const auto totem = content().petalIndex("totem");
    host.setLoadoutSlot(0, totem, Rarity::Uncommon);
    CHECK(h.stepUntil({&host}, [&] {
        const auto& slot = world.get<Loadout>(player).slots[0];
        return slot.configIndex == totem && !slot.broken;
    }));
    host.sendChat("/wave start");
    CHECK(h.stepUntil({&host}, [&] { return world.has<WavePlayer>(player); }));
    h.clock += 30000.0;
    CHECK(h.stepUntil({&host}, [&] { return Query<WaveEnemy>{world}.count() >= 2; }, 60));
    const auto enemies = Query<WaveEnemy>{world}.collect();
    if (enemies.size() < 2) return;
    const Vec2 defender = world.get<Transform>(player).position;
    const Vec2 firstSpawn = world.get<Transform>(enemies[1]).position;
    const double initialGap = distance(defender, firstSpawn);
    world.get<Health>(player).invulnerableUntilMillis = std::numeric_limits<double>::infinity();
    CHECK(h.stepUntil({&host}, [&] {
        const auto* ai = world.tryGet<MobAi>(enemies[1]);
        const auto* at = world.tryGet<Transform>(enemies[1]);
        return ai && at && ai->target == player &&
               distance(at->position, defender) <= initialGap - 80.0;
    }, 40));
    world.get<MobType>(enemies[1]).rarity = Rarity::Mythic;
    auto& db = GameServerPeer::authorityDb(h.server);
    const auto account = db.findUser("totemhost")->id;
    const auto oldKills = db.findProgress(account)->mobKills;
    const auto before = world.get<PlayerProgress>(player);
    auto& health = world.get<Health>(player);
    health.invulnerableUntilMillis = std::numeric_limits<double>::infinity();
    health.current = health.max * 0.5;
    const double lowHealth = health.current;
    for (const Entity enemy : {enemies[0], enemies[1]}) {
        const auto position = world.get<Transform>(enemy);
        const auto damage = world.get<Health>(enemy).max * 2;
        const Entity hit = world.create();
        world.add<GroundEffectTag>(hit);
        world.add<Transform>(hit, position);
        world.add<GroundEffect>(hit, GroundEffect{GroundEffectKind::Poison, player, 1.0,
                                                0.0, 1.0, Rarity::Common, damage});
        world.add<LightningBurst>(hit);
        world.add<LightningTarget>(hit, LightningTarget{enemy});
    }
    CHECK(h.stepUntil({&host}, [&] {
        const auto* regeneration = world.tryGet<TotemRegeneration>(player);
        return regeneration && regeneration->stacks.size() == 2 &&
            world.get<Health>(player).current > lowHealth;
    }, 20));
    CHECK_NEAR(world.get<PlayerProgress>(player).stars, before.stars, 1e-9);
    CHECK_NEAR(world.get<PlayerProgress>(player).totalXp, before.totalXp, 1e-9);
    CHECK(db.findProgress(account)->mobKills.dump() == oldKills.dump());
    host.sendChat("/wave leave");
    CHECK(h.stepUntil({&host}, [&] { return !world.has<WavePlayer>(player); }, 20));
    CHECK(!world.has<TotemRegeneration>(player));
}

TEST(wave_batches_find_another_bearing_when_a_meteor_blocks_the_spawn) {
    const auto firstBatch = [](const std::vector<Vec2>& obstacles, bool dead, bool otherRealm, bool blockedHost = false) {
        World world;
        Terrain terrain;
        WorldMaps maps;
        std::string error;
        CHECK(maps.load(dataDir(), &terrain, error));
        if (!error.empty()) return std::vector<Vec2>{};
        const auto& content = shippedContent();
        SpawnSystem spawner;
        spawner.worldMaps = &maps;
        DungeonSystem dungeon;
        dungeon.worldMaps = &maps;
        dungeon.moveToRealm = [&](Entity e, Realm realm, Vec2 position) {
            world.get<Transform>(e) = Transform{position, 0.0, realm};
        };
        CombatSystem combat;
        Rng rng{42u};
        CommandBuffer commands{world};
        const Entity player = world.create();
        world.add<PlayerTag>(player);
        world.add<Body>(player, Body{25.0, 1.0});
        world.add<Health>(player, Health{100.0, 100.0});
        world.add<Transform>(player, Transform{terrain.spawnPoint(Realm::Overworld), 0.0});
        dungeon.run(world, terrain, content, spawner, combat, rng, 0.0, commands);
        const auto reply = dungeon.waveCommand(world, terrain, player, "start", rng, 1000.0);
        const auto realm = world.get<Transform>(player).realm;
        if (blockedHost) {
            const auto codeStart = reply.find_first_of("0123456789");
            CHECK(codeStart != std::string::npos);
            if (codeStart == std::string::npos) return std::vector<Vec2>{};
            const Entity guest = world.create();
            world.add<PlayerTag>(guest);
            world.add<Body>(guest, Body{25.0, 1.0});
            world.add<Health>(guest, Health{100.0, 100.0});
            world.add<Transform>(guest, Transform{terrain.spawnPoint(Realm::Overworld), 0.0});
            dungeon.waveCommand(world, terrain, guest, "join " + reply.substr(codeStart, 6), rng, 2000.0);
            CHECK(world.get<Transform>(guest).realm == realm);
            world.get<Transform>(player).position = {1024, 1024};
            world.get<Transform>(guest).position = {3072, 3072};
            const Entity meteor = world.create();
            world.add<PlantedMeteor>(meteor, PlantedMeteor{player});
            world.add<Transform>(meteor, Transform{{1024, 1024}, 0.0, realm});
            world.add<Body>(meteor, Body{1050.0, 1.0});
        }
        CHECK(dungeon.inWave(world, player));
        for (const Vec2 at : obstacles) {
            const Entity meteor = world.create();
            world.add<PlantedMeteor>(meteor, PlantedMeteor{player});
            world.add<Transform>(meteor, Transform{at, 0.0, otherRealm ? Realm::Overworld : realm});
            world.add<Body>(meteor, Body{100.0, 1.0});
            if (dead) world.add<Dead>(meteor);
        }
        dungeon.run(world, terrain, content, spawner, combat, rng, 31100.0, commands);
        commands.flush();

        std::vector<Vec2> positions;
        Query<WaveEnemy, Transform, Body>{world}.each(
            [&](Entity, WaveEnemy&, Transform& at, Body& body) {
                positions.push_back(at.position);
                if (!dead && !otherRealm) {
                    for (const Vec2 meteor : obstacles)
                        CHECK(distance(at.position, meteor) >= 100.0 + body.radius);
                }
            });
        return positions;
    };
    const auto fallback = firstBatch({}, false, false, true);
    CHECK_EQ(fallback.size(), 2u);
    for (const Vec2 point : fallback)
        CHECK(distance(point, {3072, 3072}) <= 961.0);
    const auto clear = firstBatch({}, false, false);
    CHECK_EQ(clear.size(), 2u);
    if (clear.empty()) return;
    const std::vector<Vec2> blocker{clear.front()};
    CHECK_EQ(firstBatch(blocker, false, false).size(), clear.size());
    for (const auto& ignored : {firstBatch(blocker, true, false), firstBatch(blocker, false, true)}) {
        CHECK_EQ(ignored.size(), clear.size());
        if (ignored.size() == clear.size()) {
            for (std::size_t i = 0; i < clear.size(); ++i)
                CHECK_NEAR(distance(ignored[i], clear[i]), 0.0, 1e-9);
        }
    }
}

TEST(wave_four_rooms_isolate_full_parties_rewards_and_cleanup) {
    World world;
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    CHECK_EQ(maps.copiesOf("wave").size(), 4u);
    DungeonSystem dungeon;
    dungeon.worldMaps = &maps;
    SpawnSystem spawner;
    spawner.worldMaps = &maps;
    dungeon.moveToRealm = [&](Entity e, Realm realm, Vec2 position) {
        world.get<Transform>(e) = Transform{position, 0.0, realm};
    };
    CombatSystem combat;
    CommandBuffer commands{world};
    Rng rng{42u};
    const auto& registry = shippedContent();
    const auto tick = [&](double now) {
        dungeon.run(world, terrain, registry, spawner, combat, rng, now, commands);
        commands.flush();
    };
    const Vec2 exterior = terrain.spawnPoint(Realm::Overworld);
    std::array<Entity, 17> players;
    for (auto& e : players) {
        e = world.create();
        world.add<PlayerTag>(e);
        world.add<Body>(e, Body{25.0, 1.0});
        world.add<Health>(e, Health{100.0, 100.0});
        world.add<Faction>(e, Faction{Team::Players, true});
        world.add<Transform>(e, Transform{exterior, 0.0});
    }
    std::set<Realm> realms;
    std::set<std::string> codes;
    for (int room = 0; room < 4; ++room) {
        const auto host = players[room * 4];
        const auto reply = dungeon.waveCommand(world, terrain, host, "start", rng, 1000.0);
        const auto firstDigit = reply.find_first_of("0123456789");
        CHECK(firstDigit != std::string::npos);
        if (firstDigit == std::string::npos) return;
        const auto code = reply.substr(firstDigit, 6);
        CHECK(codes.insert(code).second);
        const auto realm = world.get<Transform>(host).realm;
        CHECK(realms.insert(realm).second);
        for (int seat = 1; seat < 4; ++seat) {
            const auto guest = players[room * 4 + seat];
            dungeon.waveCommand(world, terrain, guest, "join " + code, rng, 2000.0);
            CHECK(world.get<Transform>(guest).realm == realm);
        }
        CHECK(dungeon.waveCommand(world, terrain, players.back(), "join " + code, rng, 2000.0)
                  .find("已满") != std::string::npos);
    }
    CHECK_EQ(dungeon.waveOccupancy(), 4u);
    CHECK(dungeon.waveCommand(world, terrain, players.back(), "start", rng, 2000.0)
              .find("均已占用") != std::string::npos);
    CHECK(!dungeon.inWave(world, players.back()));
    const auto firstRealm = world.get<Transform>(players[0]).realm;
    const auto secondRealm = world.get<Transform>(players[4]).realm;
    std::vector<Entity> awarded;
    dungeon.waveAward = [&](Entity player, std::uint64_t, bool) -> DungeonSystem::WaveAward {
        awarded.push_back(player);
        return {7500, false};
    };
    for (double now = 31000.0; now < 76000.0 && awarded.empty(); now += 600.0) {
        tick(now);
        for (const Entity enemy : Query<WaveEnemy>{world}.collect()) {
            if (world.get<Transform>(enemy).realm != firstRealm || !isStanding(world, enemy)) continue;
            world.get<Bounty>(enemy).credit(players[0], world.get<Health>(enemy).max);
            world.add<Dead>(enemy);
        }
    }
    CHECK_EQ(awarded.size(), 1u);
    if (!awarded.empty()) CHECK(awarded.front() == players[0]);
    CHECK_EQ(dungeon.waveHudFor(world, players[0], 0.0).round, 2u);
    CHECK_EQ(dungeon.waveHudFor(world, players[4], 0.0).round, 1u);

    for (int i = 0; i < 4; ++i) {
        dungeon.waveCommand(world, terrain, players[i], "leave", rng, 77000.0);
        CHECK(world.get<Transform>(players[i]).realm == Realm::Overworld);
    }
    tick(77000.0);
    CHECK_EQ(dungeon.waveOccupancy(), 3u);
    const auto enemies = Query<WaveEnemy>{world}.collect();
    CHECK(std::none_of(enemies.begin(), enemies.end(), [&](Entity e) {
        return world.get<Transform>(e).realm == firstRealm;
    }));
    CHECK(std::any_of(enemies.begin(), enemies.end(), [&](Entity e) {
        return world.get<Transform>(e).realm == secondRealm;
    }));
    dungeon.waveCommand(world, terrain, players.back(), "start", rng, 77100.0);
    CHECK_EQ(dungeon.waveOccupancy(), 4u);
    CHECK(world.get<Transform>(players.back()).realm == firstRealm);
}

TEST(wave_is_endless_bounded_spaced_and_rewards_participation) {
    World world;
    Terrain terrain;
    WorldMaps maps;
    std::string error;
    CHECK(maps.load(dataDir(), &terrain, error));
    if (!error.empty()) return;
    const auto& content = shippedContent();
    SpawnSystem spawner;
    spawner.worldMaps = &maps;
    DungeonSystem dungeon;
    dungeon.worldMaps = &maps;
    dungeon.moveToRealm = [&](Entity e, Realm realm, Vec2 position) {
        world.get<Transform>(e) = Transform{position, 0.0, realm};
    };
    dungeon.waveRevived = [&](Entity e) {
        auto& hp = world.get<Health>(e);
        hp.current = hp.max;
        world.remove<Dead>(e);
    };
    CombatSystem combat;
    Rng rng{42u};
    CommandBuffer commands{world};
    const auto tick = [&](double now) {
        dungeon.run(world, terrain, content, spawner, combat, rng, now, commands);
        commands.flush();
    };
    const Vec2 entry = terrain.spawnPoint(Realm::Overworld);
    std::vector<Entity> members;
    for (int i = 0; i < 5; ++i) {
        const auto e = world.create();
        world.add<PlayerTag>(e);
        world.add<Body>(e, Body{25.0, 1.0});
        world.add<Health>(e, Health{100.0, 100.0});
        world.add<Faction>(e, Faction{Team::Players, true});
        world.add<Transform>(e, Transform{entry, 0.0});
        members.push_back(e);
    }
    tick(0.0);
    const auto reply = dungeon.waveCommand(world, terrain, members[0], "start", rng, 1000.0);
    const auto firstDigit = reply.find_first_of("0123456789");
    CHECK(firstDigit != std::string::npos);
    if (firstDigit == std::string::npos) return;
    const auto join = "join " + reply.substr(firstDigit, 6);
    for (int i = 1; i < 5; ++i)
        dungeon.waveCommand(world, terrain, members[i], join, rng, 2000.0);
    for (int i = 0; i < 4; ++i) CHECK(dungeon.inWave(world, members[i]));
    CHECK(!dungeon.inWave(world, members[4]));
    CHECK(!CombatSystem::canDamage(world, members[0], members[1]));

    int hostAwards = 0;
    int guestAwards = 0;
    std::map<Entity, std::string> summaries;
    dungeon.waveAward = [&](Entity player, std::uint64_t round, bool bonus) -> DungeonSystem::WaveAward {
        CHECK_EQ(bonus, round == 7 || round == 13 || round == 17);
        if (player == members[3]) {
            CHECK_EQ(round, std::uint64_t(1));
            ++guestAwards;
            return {7500, false};
        }
        CHECK(player == members[0]);
        ++hostAwards;
        CHECK_EQ(round, static_cast<std::uint64_t>(hostAwards));
        return {round >= 20 ? 9000 : round >= 11 ? 8250 : 7500, false, round == 7};
    };
    dungeon.waveSummary = [&](Entity player, const std::string& text) { summaries[player] = text; };
    const Realm realm = world.get<Transform>(members[0]).realm;
    spawner.mobCap = 0;
    spawner.restoreBossClock("wave", 1000000.0, 1000000.0);
    spawner.run(world, terrain, content, {{world.get<Transform>(members[0]).position, realm}},
                rng, 0.0, 0.0, commands);
    commands.flush();
    CHECK(spawner.bossCooldownLeft(Rarity::Apex, realm, 16000.0) > 0.0);
    CHECK_EQ(spawner.census().mobs, 0);
    tick(30999.0);
    CHECK(Query<WaveEnemy>{world}.collect().empty());
    CHECK_EQ(dungeon.waveHudFor(world, members[0], 30999.0).remainingMillis, 1u);
    tick(31000.0);
    CHECK(Query<WaveEnemy>{world}.collect().empty());
    CHECK(dungeon.inWave(world, members[0]));
    spawner.mobCap = kMaxLiveMobs;

    // Start the squad near a corner, far away from the old centre rings.
    for (int i = 0; i < 4; ++i)
        world.get<Transform>(members[i]).position = {240.0 + i * 55.0, 240.0};

    double now = 31100.0;
    int total = 0;
    std::set<std::uint16_t> gardenMobs, desertMobs, oceanMobs;
    const auto species = [&](std::uint16_t id, std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(), [&](const char* name) {
            return id == content.mobIndex(name);
        });
    };
    for (int wave = 1; wave <= 21; ++wave) {
        const bool boss = wave % 5 == 0;
        const bool swarm = wave == 7 || wave == 13 || wave == 17;
        const int ordinary = 28 + 2 * std::min(wave - 1, 8);
        const int expected = boss ? 1 : swarm ? ordinary + ordinary / 2 : ordinary;
        const auto theme = wave % 10 == 0 ? net::WaveTheme::HyperBoss :
                           boss ? net::WaveTheme::EternalBoss :
                           wave == 7 ? net::WaveTheme::Bubble :
                           wave == 13 ? net::WaveTheme::Sandstorm :
                           wave == 17 ? net::WaveTheme::Golden : net::WaveTheme::Normal;
        int seen = 0;
        const double start = now;
        for (; now - start <= 42000.0; now += 2800.0) {
            tick(now);
            const auto hud = dungeon.waveHudFor(world, members[0], now);
            CHECK_EQ(hud.round, static_cast<unsigned>(wave));
            CHECK_EQ(static_cast<int>(hud.theme), static_cast<int>(theme));
            const auto mobs = Query<WaveEnemy>{world}.collect();
            if (seen == 0) CHECK_EQ(mobs.size(), static_cast<std::size_t>(boss || swarm ? 1 : 2));
            CHECK(mobs.size() <= (swarm ? 26u : 10u));
            if (swarm && now - start >= 16800.0 && expected - seen >= 26)
                CHECK_EQ(mobs.size(), std::size_t(26));
            if (wave == 1 && now - start >= 16000.0 && expected - seen >= 10)
                CHECK_EQ(mobs.size(), std::size_t(10));
            if (wave == 2) CHECK(isStanding(world, members[1]));
            for (const Entity mob : mobs) {
                const auto& at = world.get<Transform>(mob);
                const auto& hp = world.get<Health>(mob);
                const auto& body = world.get<Body>(mob);
                const auto& type = world.get<MobType>(mob);
                CHECK_EQ(world.get<WaveEnemy>(mob).round, static_cast<std::uint64_t>(wave));
                CHECK(at.realm == realm);
                CHECK(world.get<MobAi>(mob).kind ==
                      (type.configIndex == content.mobIndex("sandstorm") ? AiKind::Sandstorm : AiKind::Hostile));
                CHECK(world.get<MobAi>(mob).aggroRange >= terrain.realmExtent(realm).length());
                const auto nearParty = std::any_of(members.begin(), members.begin() + 4, [&](Entity player) {
                    return isStanding(world, player) &&
                        distance(at.position, world.get<Transform>(player).position) <= 961.0;
                });
                CHECK(nearParty);
                CHECK(!terrain.resolveWall(at.position, body.radius, realm).collided);
                CHECK_EQ(world.get<Bounty>(mob).xp, 0.0);
                CHECK(CombatSystem::canDamage(world, members[0], mob));
                for (const auto other : mobs) {
                    if (other == mob) continue;
                    CHECK(distance(at.position, world.get<Transform>(other).position) >=
                          body.radius + world.get<Body>(other).radius);
                }
                if (boss) {
                    CHECK_EQ(type.configIndex, content.mobIndex("beetle"));
                    CHECK(type.rarity == (wave % 10 == 0 ? Rarity::Apex : Rarity::Unique));
                    CHECK(body.radius > 92.0);
                } else if (wave == 7 || wave == 13 || wave == 17) {
                    CHECK_EQ(type.configIndex, content.mobIndex(
                        wave == 7 ? "bubble" : wave == 13 ? "sandstorm" : "golden_leafbug"));
                } else if (wave == 1) {
                    CHECK(species(type.configIndex, {"bee", "baby_ant", "ladybug"}));
                    gardenMobs.insert(type.configIndex);
                } else if (wave == 4) {
                    CHECK(species(type.configIndex, {"sandstorm", "cactus", "beetle", "scorpion", "shiny_ladybug", "desert_ladybug",
                        "soldier_fire_ant", "digger"}));
                    desertMobs.insert(type.configIndex);
                } else if (wave == 8) {
                    CHECK(species(type.configIndex, {"bubble", "sponge", "shell", "crab",
                        "starfish", "jellyfish"}));
                    oceanMobs.insert(type.configIndex);
                }
                // Hold the final batch to near its deadline; 21 clears exceed ten minutes.
                if (seen + static_cast<int>(mobs.size()) == expected && now - start < 42000.0) continue;
                world.get<Bounty>(mob).credit(members[0], hp.max);
                world.get<Bounty>(mob).credit(members[2], hp.max * 0.005);
                if (wave == 1) world.get<Bounty>(mob).credit(members[3], hp.max * 0.01);
                world.add<Dead>(mob);
            }
            if (!mobs.empty() && world.has<Dead>(mobs.front())) {
                seen += static_cast<int>(mobs.size());
                if (wave == 1 && seen == expected) {
                    world.get<Health>(members[1]).current = 0.0;
                    world.add<Dead>(members[1]);
                }
                if (seen == expected) {
                    now += 50.0;
                    tick(now);
                    for (const auto mob : mobs) world.destroy(mob);
                    break;
                }
                tick(now + 50.0);
                for (const auto mob : mobs) world.destroy(mob);
            }
        }
        CHECK_EQ(seen, expected);
        total += seen;
        CHECK(dungeon.inWave(world, members[0]));
    }
    CHECK_EQ(hostAwards, 21);
    CHECK_EQ(guestAwards, 1);
    CHECK(spawner.bossSpawns.empty());
    CHECK(summaries.empty());
    CHECK(gardenMobs.size() >= 3);
    CHECK(desertMobs.size() >= 3);
    CHECK(oceanMobs.size() >= 3);
    std::printf("[wave] %d enemies in 21 waves, paid all 21 clears\n", total);

    dungeon.waveCommand(world, terrain, members[0], "leave", rng, now);
    CHECK(summaries[members[0]].find("通关 21 波 · 本局 +167250 星星") != std::string::npos);
    CHECK(summaries[members[0]].find("已领满每日 200,000 星星") == std::string::npos);
    CHECK(summaries[members[0]].find("+1 限时锤") != std::string::npos);
    const auto survivors = Query<WaveEnemy>{world}.collect();
    CHECK(!survivors.empty());
    // Leave a full pack alive at the deadline so cleanup holds the next wave.
    tick(now + 16000.0);
    const auto beforeCleanup = Query<WaveEnemy>{world}.collect();
    CHECK(beforeCleanup.size() > 4);
    tick(now + 45000.0);
    const auto cleanupHud = dungeon.waveHudFor(world, members[1], now + 45000.0);
    CHECK_EQ(cleanupHud.round, 22u);
    CHECK_EQ(cleanupHud.remainingMillis, 0u);
    CHECK_EQ(Query<WaveEnemy>{world}.count(), beforeCleanup.size());
    for (const auto enemy : beforeCleanup) CHECK(world.get<WaveEnemy>(enemy).rushing);
    tick(now + 60000.0);
    CHECK_EQ(dungeon.waveHudFor(world, members[1], now + 60000.0).round, 23u);
    bool overlapped = false;
    for (const auto enemy : Query<WaveEnemy>{world}.collect()) {
        if (world.get<WaveEnemy>(enemy).round == 23) overlapped = true;
    }
    CHECK(overlapped);
    for (const auto enemy : survivors) CHECK(isStanding(world, enemy));
    tick(now + 181000.0);
    CHECK(summaries[members[1]].find("连续 3 分钟未推进") != std::string::npos);
    CHECK(summaries[members[1]].find("至少一只怪造成 1% 伤害") != std::string::npos);
    for (const Entity e : members) {
        CHECK(world.get<Transform>(e).realm == Realm::Overworld);
        CHECK(!world.has<WavePlayer>(e));
    }
    CHECK(Query<WaveEnemy>{world}.collect().empty());
    CHECK(CombatSystem::canDamage(world, members[0], members[1]));
    CHECK_EQ(dungeon.waveOccupancy(), 0u);
    const auto nextRoom = dungeon.waveCommand(world, terrain, members[0], "start", rng, now + 181100.0);
    CHECK(nextRoom.find("已进入") != std::string::npos);
    CHECK_EQ(dungeon.waveOccupancy(), 1u);
    CHECK(dungeon.inWave(world, members[0]));
}

namespace {
std::int64_t waveStarClock = 0;
std::int64_t waveStarNow() { return waveStarClock; }
}

TEST(wave_stars_accumulate_per_wave_to_the_daily_cap_across_reloads) {
    const std::string dir = tempDir("flix-wave-stars-" + std::to_string(::getpid()));
    const std::string path = dir + "/inventory.json";
    std::string error;
    Database db;
    CHECK(db.load(path, error));
    waveStarClock = 100 * 86400000ll;
    db.setClock(waveStarNow);
    auto& legacy = db.progress("wave-account");
    legacy.stars = 150000.0;
    legacy.extra["waveStars"]["day"] = 100;
    legacy.extra["waveStars"]["earned"] = 150000;
    for (int wave = 0; wave < 6; ++wave)
        CHECK_EQ(db.awardWaveStars("wave-account"), 7500);
    CHECK_NEAR(db.progress("wave-account").stars, 195000.0, 1e-9);
    CHECK(db.save());

    Database restarted;
    CHECK(restarted.load(path, error));
    restarted.setClock(waveStarNow);
    bool reachedDailyCap = false;
    CHECK(!restarted.waveDailyStarCapReached("wave-account"));
    CHECK_EQ(restarted.awardWaveStars("wave-account", &reachedDailyCap), 5000);
    CHECK(restarted.waveDailyStarCapReached("wave-account"));
    CHECK(!restarted.waveDailyStarCapReached("other-account"));
    CHECK(reachedDailyCap);
    CHECK_EQ(restarted.awardWaveStars("wave-account", &reachedDailyCap), 0);
    CHECK(reachedDailyCap);
    CHECK_NEAR(restarted.progress("wave-account").stars, 200000.0, 1e-9);
    CHECK_EQ(restarted.awardWaveStars("other-account"), 7500);
    CHECK_EQ(restarted.awardWaveStars("other-account", nullptr, 11), 8250);
    CHECK_EQ(restarted.awardWaveStars("other-account", nullptr, 19), 8250);
    CHECK_EQ(restarted.awardWaveStars("other-account", nullptr, 20), 9000);
    waveStarClock += 86400000ll;
    CHECK(!restarted.waveDailyStarCapReached("wave-account"));
    CHECK_EQ(restarted.awardWaveStars("wave-account"), 7500);
    CHECK(restarted.awardWaveBonusHammer("wave-account"));
    CHECK(!restarted.awardWaveBonusHammer("wave-account"));
    CHECK_EQ(restarted.progress("wave-account").itemCount(Rarity::Unique, "petal_timed_mjolnir"), 1);
    CHECK(restarted.save());
    Database hammerReload;
    hammerReload.setClock(waveStarNow);
    CHECK(hammerReload.load(path, error));
    CHECK(!hammerReload.awardWaveBonusHammer("wave-account"));
    waveStarClock += 86400000ll;
    CHECK(hammerReload.awardWaveBonusHammer("wave-account"));
    CHECK_NEAR(restarted.progress("wave-account").stars, 207500.0, 1e-9);
}


TEST(practice_history_is_private_persistent_and_shared_per_score) {
    const std::string dir = tempDir("flix-practice-history-" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    const std::string path = dir + "/inventory.json";
    seedUser(path, "tester_one", "password7");
    seedUser(path, "tester_two", "password7");
    std::string error;
    Database db;
    if (!db.load(path, error)) { CHECK(false); return; }
    const auto* first = db.findUser("tester_one");
    const auto* second = db.findUser("tester_two");
    if (!first || !second) { CHECK(false); return; }
    const std::string owner = first->id, viewer = second->id;
    const std::string basic = R"([{"slot":1,"petal":"basic","tier":0}])";
    const std::string leaf = R"([{"slot":2,"petal":"leaf","tier":0}])";
    savePracticeScore(db, owner, basic, 1000.0);
    savePracticeScore(db, viewer, leaf, 500.0);
    const auto before = practiceBoardView(db, viewer, false);
    CHECK_EQ(before["rank"].asInt(), 2);
    CHECK(before["public"].items().empty());
    CHECK_EQ(before["own"].size(), 1u);
    const auto ownerView = practiceBoardView(db, owner, false);
    CHECK_EQ(ownerView["own"].size(), 1u);
    if (ownerView["own"].items().empty()) return;
    const auto id = static_cast<std::uint32_t>(ownerView["own"][std::size_t(0)]["id"].asDouble());
    sharePracticeScore(db, owner, id, true);
    savePracticeScore(db, owner, basic, 1300.0);
    savePracticeScore(db, owner, leaf, 1500.0);
    CHECK(db.save());
    Database reloaded;
    CHECK(reloaded.load(path, error));
    const auto fromOther = practiceBoardView(reloaded, viewer, false);
    CHECK_EQ(fromOther["rank"].asInt(), 2);
    CHECK_EQ(fromOther["public"].size(), 1u);
    if (fromOther["public"].items().empty()) return;
    CHECK_EQ(fromOther["public"][std::size_t(0)]["damage"].asDouble(), 1000.0);
    CHECK_EQ(fromOther["own"][std::size_t(0)]["damage"].asDouble(), 500.0);
    sharePracticeScore(reloaded, owner, id, false);
    CHECK(practiceBoardView(reloaded, viewer, false)["public"].items().empty());
    CHECK_EQ(practiceBoardView(reloaded, owner, false)["own"].size(), 3u);
}


TEST(practice_history_round_trips_real_timed_trial_and_publication) {
    Harness h("practice-trial", [](const std::string& path) {
        seedUser(path, "trial_owner", "password7");
        seedUser(path, "trial_viewer", "password7");
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient owner, viewer;
    if (!logIn(h, owner, "trial_owner") || !join(h, owner, "trial_owner", false) ||
        !logIn(h, viewer, "trial_viewer") || !join(h, viewer, "trial_viewer", false)) {
        CHECK(false);
        return;
    }
    auto& world = h.server.world();
    const auto player = bodyNamed(world, "trial_owner");
    bool found = false;
    const auto jungle = h.server.worldMaps().realmOfId("jungle", found);
    if (!found || player == NULL_ENTITY) { CHECK(false); return; }
    const auto dummyType = shippedContent().mobIndex("target_dummy");
    Entity dummy = NULL_ENTITY;
    const auto testPoint = Vec2{12148.17, 8225.17};
    GameServerPeer::markTeleport(h.server, player, jungle, testPoint + Vec2{0, 230});
    CHECK(h.stepUntil({&owner, &viewer}, [&] {
        Query<Npc, Transform>{world}.each([&](Entity e, Npc& npc, Transform& at) {
            if (npc.configIndex == dummyType && npc.rarity == Rarity::Super && at.realm == jungle) {
                dummy = e;
            }
        });
        return dummy != NULL_ENTITY;
    }));
    if (dummy == NULL_ENTITY) return;
    const auto* ownerSession = GameServerPeer::sessionOf(h.server, "trial_owner");
    if (!ownerSession) { CHECK(false); return; }
    owner.requestPracticeBoard(false, net::PracticeBoardAction::Start);
    CHECK(h.stepUntil({&owner, &viewer}, [&] { return ownerSession->practiceTrial.player == player; }));
    if (ownerSession->practiceTrial.player != player) return;
    auto& combat = GameServerPeer::practiceCombat(h.server);
    combat.applyDamage(world, dummy, player, 1000.0,
                       GameServerPeer::practiceClock(h.server), DamageKind::Reflect);
    const Entity shot = world.create();
    auto& projectile = world.add<Projectile>(shot);
    projectile.owner = player;
    projectile.petalConfigIndex = content().petalIndex("basic");
    projectile.rarity = Rarity::Super;
    world.add<Transform>(shot, world.get<Transform>(player));
    const auto result = combat.applyDamage(world, dummy, shot, 100000000.0,
        ownerSession->practiceTrial.endsAt - 7900.0, DamageKind::Direct);
    CHECK(result.applied > 0.0);
    h.clock += 10000;
    h.step(4, {&owner, &viewer});
    owner.requestPracticeBoard(false);
    viewer.requestPracticeBoard(false);
    CHECK(h.stepUntil({&owner, &viewer}, [&] {
        return !owner.practiceBoard().pending && !viewer.practiceBoard().pending;
    }));
    CHECK_EQ(owner.practiceBoard().own.size(), 1u);
    CHECK(viewer.practiceBoard().own.empty());
    CHECK(viewer.practiceBoard().published.empty());
    if (owner.practiceBoard().own.empty()) return;
    const auto& row = owner.practiceBoard().own.front();
    const auto id = row.id;
    CHECK(!row.published);
    CHECK(row.damage > 0.0);
    CHECK_EQ(row.breakdown.size(), 2u);
    if (row.breakdown.size() == 2) {
        const auto& first = row.breakdown[1];
        const auto& second = row.breakdown[0];
        CHECK(first.petal.empty());
        CHECK_EQ(second.petal, "basic");
        CHECK(second.rarity == Rarity::Super);
        CHECK_NEAR(first.total + second.total, row.damage, 1e-9);
        CHECK_NEAR(first.seconds[0], first.total, 1e-9);
        CHECK_NEAR(second.seconds[2], second.total, 1e-9);
        CHECK_NEAR(second.seconds[9], 0.0, 1e-9);
    }
    owner.requestPracticeBoard(false, net::PracticeBoardAction::Publish, id);
    CHECK(h.stepUntil({&owner, &viewer}, [&] {
        return !owner.practiceBoard().pending && !owner.practiceBoard().own.empty() &&
            owner.practiceBoard().own.front().published;
    }));
    viewer.requestPracticeBoard(false);
    CHECK(h.stepUntil({&owner, &viewer}, [&] {
        return !viewer.practiceBoard().pending && viewer.practiceBoard().published.size() == 1;
    }));
    CHECK(viewer.practiceBoard().own.empty());
    if (!viewer.practiceBoard().published.empty())
        CHECK_EQ(viewer.practiceBoard().published.front().breakdown.size(), 2u);
    owner.requestPracticeBoard(false, net::PracticeBoardAction::Hide, id);
    CHECK(h.stepUntil({&owner, &viewer}, [&] {
        return !owner.practiceBoard().pending && !owner.practiceBoard().own.empty() &&
            !owner.practiceBoard().own.front().published;
    }));
    viewer.requestPracticeBoard(false);
    CHECK(h.stepUntil({&owner, &viewer}, [&] {
        return !viewer.practiceBoard().pending && viewer.practiceBoard().published.empty();
    }));
    owner.requestPracticeBoard(false, net::PracticeBoardAction::Publish, id);
    CHECK(h.stepUntil({&owner, &viewer}, [&] { return !owner.practiceBoard().pending; }));
    viewer.requestPracticeBoard(false, net::PracticeBoardAction::Delete, id);
    CHECK(h.stepUntil({&owner, &viewer}, [&] { return !viewer.practiceBoard().pending; }));
    CHECK_EQ(viewer.practiceBoard().published.size(), 1u);

    owner.requestPracticeBoard(false, net::PracticeBoardAction::Delete, id);
    CHECK(h.stepUntil({&owner, &viewer}, [&] { return !owner.practiceBoard().pending; }));
    CHECK(owner.practiceBoard().own.empty());
    CHECK(owner.practiceBoard().published.empty());
    CHECK_EQ(owner.practiceBoard().ownRank, 0u);
    viewer.requestPracticeBoard(false);
    CHECK(h.stepUntil({&owner, &viewer}, [&] { return !viewer.practiceBoard().pending; }));
    CHECK(viewer.practiceBoard().published.empty());
}

TEST(zorr_dna_hatches_bio_and_other_eggs_at_the_actual_tier) {
    const auto& registry = shippedContent();
    const auto trial = [&](const char* id, Rarity eggTier, Rarity dnaTier,
                           Rarity normal, bool hasDna, bool canPromote) {
        World world;
        CommandBuffer commands{world};
        PetalSystem petals;
        const Entity flower = world.create();
        world.add<PlayerTag>(flower);
        world.add<Transform>(flower, Transform{{15000, 15000}, 0});
        world.add<Body>(flower, Body{25, 1});
        world.add<Health>(flower, Health{100, 100});
        world.add<Loadout>(flower);
        world.add<PlayerModifiers>(flower);
        world.add<PetalRing>(flower);
        auto& bar = world.get<Loadout>(flower).slots;
        const auto eggIndex = registry.petalIndex(id);
        CHECK(eggIndex < registry.petalCount());
        if (eggIndex >= registry.petalCount()) return;
        bar[0] = {eggIndex, eggTier};
        if (hasDna) bar[1] = {registry.petalIndex("dna"), dnaTier};

        int checked = 0, upgraded = 0;
        for (int tick = 1; tick < 10000 && checked < 4000; ++tick) {
            petals.run(world, registry, tick * 60000.0, net::kTickSeconds, commands);
            commands.flush();
            for (const Entity entity : Query<Pet, MobType>{world}.collect()) {
                const auto& type = world.get<MobType>(entity);
                if (type.configIndex == registry.petal(eggIndex).petMobIndex) {
                    const bool higher = rarityIndex(type.rarity) == rarityIndex(normal) + 1;
                    CHECK(type.rarity == normal || (higher && canPromote));
                    upgraded += higher;
                    ++checked;
                }
                world.destroy(entity);
            }
        }
        std::printf("DNA %s egg=%d dna=%d hatch=%d total=%d upgraded=%d\n",
                    id, rarityIndex(eggTier), hasDna ? rarityIndex(dnaTier) : -1,
                    rarityIndex(normal), checked, upgraded);
        CHECK(checked >= 4000);
        if (canPromote) CHECK(upgraded >= 8 && upgraded <= 40);
        else CHECK_EQ(upgraded, 0);
    };
    trial("bio_bacteria_egg", Rarity::Rare, Rarity::Rare, Rarity::Rare, true, true);
    trial("arctic_spider_egg", Rarity::Mythic, Rarity::Mythic, Rarity::Legendary, true, true);
    trial("arctic_spider_egg", Rarity::Mythic, Rarity::Legendary, Rarity::Legendary, true, false);
    trial("arctic_spider_egg", Rarity::Mythic, Rarity::Mythic, Rarity::Legendary, false, false);
    trial("bio_bacteria_egg", Rarity::Unique, Rarity::Unique, Rarity::Unique, true, true);
    trial("bio_bacteria_egg", Rarity::Unique, Rarity::PermanentHyper, Rarity::Unique, true, true);
    trial("bio_bacteria_egg", Rarity::Apex, Rarity::Apex, Rarity::Apex, true, false);
}

TEST(zorr_bio_blood_cells_fire_their_own_ammo) {
    std::string error;
    CHECK(loadContent(dataDir(), error));
    if (!error.empty()) return;
    const auto& registry = shippedContent();
    for (bool white : {false, true}) {
        World world;
        Terrain terrain;
        SpatialGrid grid;
        Rng rng{42};
        CommandBuffer commands{world};
        SpawnSystem spawner;
        MobAiSystem ai{world};
        const Vec2 at{5000, 5000};
        const Vec2 target = at + Vec2{160, 0};
        const Entity flower = world.create();
        world.add<PlayerTag>(flower);
        world.add<Transform>(flower, Transform{target, 0});
        world.add<Body>(flower, Body{25, 1});
        world.add<Health>(flower, Health{1000, 1000});
        world.add<Faction>(flower, Faction{Team::Players, false});
        world.add<Motion>(flower);
        const auto index = registry.mobIndex(white ? "bio_white_blood_cell" : "bio_red_blood_cell");
        const Entity mob = spawner.spawnMob(world, terrain, registry, index, Rarity::Common,
                                           at, Realm::Overworld, 0, rng);
        CHECK(mob != NULL_ENTITY);
        if (mob == NULL_ENTITY) continue;
        int shots = 0;
        for (int i = 1; i <= 150 && shots == 0; ++i) {
            grid.clear();
            grid.insert(flower, Realm::Overworld, target, 25);
            grid.insert(mob, Realm::Overworld, at, world.get<Body>(mob).radius);
            ai.run(world, terrain, grid, {{target, Realm::Overworld}}, i * 50.0, .05, commands);
            commands.flush();
            Query<Projectile>{world}.each([&](Entity, Projectile& shot) {
                if (shot.owner != mob) return;
                ++shots;
                CHECK_EQ(shot.petalConfigIndex, registry.petalIndex(white ? "antibody" : "bubble"));
                CHECK_NEAR(shot.damage, white ? 3.0 : 4.0, 1e-9);
            });
        }
        CHECK_EQ(shots, white ? 4 : 3);
    }
}
