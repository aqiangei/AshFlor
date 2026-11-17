// Every magic petal with a body pays mana: to form on the ring
// (PetalConfig::requiredMana) or, for a timed summon, per summon
// (PetalConfig::petManaCost).

#include "test.h"

#include "server_harness.h"
#include "server/systems/petals.h"
#include "test_data.h"

#include <set>
#include <string>
#include <vector>

using namespace flix;
using namespace flix::testsupport;

TEST(magic_petals_with_a_body_all_cost_mana) {
    const auto& registry = shippedContent();
    int magic = 0;
    for (std::size_t i = 0; i < registry.petalCount(); ++i) {
        const auto index = static_cast<std::uint16_t>(i);
        const PetalConfig& config = registry.petal(index);
        if (config.id.rfind("magic_", 0) != 0) continue;
        ++magic;
        if (config.count <= 0 || config.id == "magic_soil" ||
            config.id == "magic_fang" ||
            config.id == "magic_tooth" ||
            config.id == "magic_leaf") continue;
        for (const Rarity tier : {Rarity::Common, Rarity::Apex}) {
            const PetalStats stats = registry.petalStats(index, tier);
            if (stats.requiredMana <= 0.0 && stats.petManaCost <= 0.0) {
                std::printf("  free magic petal: %s\n", config.id.c_str());
            }
            CHECK(stats.requiredMana > 0.0 || stats.petManaCost > 0.0);
        }
    }
    CHECK(magic >= 25);
    const auto rock = registry.petalIndex("magic_rock");
    CHECK_NEAR(registry.petalStats(rock, Rarity::Common).requiredMana, 1.4, 1e-9);
    CHECK_NEAR(registry.petalStats(rock, Rarity::Uncommon).requiredMana, 2.8, 1e-9);
}

TEST(zorr_magic_variants_have_distinct_stats_and_costs) {
    const auto& registry = shippedContent();
    const auto stats = [&](const char* id, Rarity tier = Rarity::Common) {
        return registry.petalStats(registry.petalIndex(id), tier);
    };
    const auto rock = stats("magic_rock");
    CHECK_NEAR(rock.health, 750.0, 1e-9);
    CHECK_NEAR(rock.damage, 25.0, 1e-9);
    CHECK_NEAR(rock.petalArmor, 5.0, 1e-9);
    CHECK_NEAR(rock.reloadMillis, 2000.0, 1e-9);
    const auto wing = stats("magic_wing");
    CHECK_NEAR(wing.damage, 23.0, 1e-9);
    CHECK_NEAR(wing.reloadMillis, 3000.0, 1e-9);
    CHECK_EQ(wing.count, 1);
    CHECK_EQ(stats("magic_wing", Rarity::Apex).count, 1);
    CHECK_NEAR(stats("magic_bubble_bomb").reloadMillis, 200.0, 1e-9);
    const auto soil = stats("magic_soil");
    CHECK_NEAR(soil.maxMana, 13.2, 1e-9);
    CHECK_NEAR(soil.passiveManaPerSecond, 1.0, 1e-9);
    CHECK_NEAR(soil.requiredMana, 0.0, 1e-9);
    CHECK_NEAR(soil.flowerHealthBonus, 0.0, 1e-9);
    CHECK_NEAR(soil.modifiers.maxHealth, 1.1, 1e-9);
    CHECK_NEAR(stats("magic_soil", Rarity::Ascendant).modifiers.maxHealth, 1.438, 0.001);
    CHECK_NEAR(stats("magic_poo", Rarity::Super).summonRangeBonus, 54.0, 1e-9);
    CHECK_NEAR(stats("magic_poo", Rarity::Super).passiveManaDrain, 102.4, 1e-9);
}

TEST(magic_poo_spends_mana_for_summon_reach_and_stops_when_unequipped) {
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
    auto& loadout = world.add<Loadout>(player);
    loadout.slots[0] = {registry.petalIndex("magic_soil"), Rarity::Common};
    loadout.slots[1] = {registry.petalIndex("magic_poo"), Rarity::Common};
    const auto step = [&](double now) {
        petals.run(world, registry, now, net::kTickSeconds, commands);
        commands.flush();
    };
    step(1000.0);
    CHECK_NEAR(world.get<ManaPool>(player).max, 13.2, 1e-9);
    CHECK_NEAR(world.get<ManaPool>(player).current, 13.2 * .4 + .2 * net::kTickSeconds, 1e-9);
    CHECK_NEAR(world.get<PlayerModifiers>(player).summonRangeBonus, 5.0, 1e-9);
    world.get<Loadout>(player).slots[1].rarity = Rarity::Super;
    world.get<ManaPool>(player).current = 0;
    step(1050.0);
    CHECK_NEAR(world.get<PlayerModifiers>(player).summonRangeBonus, 0.0, 1e-9);
    world.get<Loadout>(player).slots[1] = {};
    step(1100.0);
    CHECK_NEAR(world.get<PlayerModifiers>(player).summonRangeBonus, 0.0, 1e-9);
    CHECK_NEAR(world.get<ManaPool>(player).current, 2.0 * net::kTickSeconds, 1e-9);
}

TEST(magic_rock_waits_for_mana_then_pays_when_it_forms) {
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
    slots[0].configIndex = registry.petalIndex("mana_orb");
    slots[1].configIndex = registry.petalIndex("magic_rock");
    const auto step = [&](double now) {
        petals.run(world, registry, now, net::kTickSeconds, commands);
        commands.flush();
    };
    const auto rocks = [&] {
        int n = 0;
        Query<PetalInstance>{world}.each([&](Entity, PetalInstance& petal) {
            if (petal.owner == player && petal.slot == 1) ++n;
        });
        return n;
    };
    const double cost = registry.petalStats(registry.petalIndex("magic_rock"), Rarity::Common).requiredMana;

    step(1000.0);
    CHECK(world.has<ManaPool>(player));
    world.get<ManaPool>(player).current = 0.0;
    step(6000.0);
    CHECK(rocks() == 0);

    world.get<ManaPool>(player).current = 10.0;
    step(6050.0);
    CHECK(rocks() == 1);
    CHECK_NEAR(world.get<ManaPool>(player).current, 10.0 - cost, 1e-6);
}

// The pop is free, so the price has to be paid again by every bubble that
// comes back after one.
TEST(magic_bubble_pays_again_after_every_pop) {
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
    slots[0].configIndex = registry.petalIndex("mana_orb");
    slots[1].configIndex = registry.petalIndex("magic_bubble");
    const double cost = registry.petalStats(registry.petalIndex("magic_bubble"), Rarity::Common).requiredMana;
    CHECK(cost > 0.0);

    const auto bubbles = [&] {
        int n = 0;
        Query<PetalInstance>{world}.each([&](Entity, PetalInstance& petal) {
            if (petal.owner == player && petal.slot == 1) ++n;
        });
        return n;
    };
    int formed = 0;
    int paid = 0;
    int pops = 0;
    for (double now = 1000.0; now < 60000.0 && formed < 4; now += net::kTickMillis) {
        // Topped up every tick, so a payment is the only way below the top.
        // The pool is made part-filled on the first tick, which is no payment.
        const bool hadPool = world.tryGet<ManaPool>(player) != nullptr;
        if (auto* pool = world.tryGet<ManaPool>(player)) pool->current = pool->max;
        const int before = bubbles();
        auto& input = world.get<PlayerInput>(player).current;
        input.flags = before > 0 ? net::InputDefend : 0;
        input.moveStrength = before > 0 ? 1.0 : 0.0;
        if (before > 0) ++pops;

        petals.run(world, registry, now, net::kTickSeconds, commands);
        commands.flush();
        std::vector<Entity> doomed;
        Query<Dead>{world}.collect(doomed);
        for (const Entity e : doomed) world.destroy(e);

        if (before == 0 && bubbles() > 0) ++formed;
        if (const auto* pool = world.tryGet<ManaPool>(player); hadPool && pool && pool->max - pool->current > cost * 0.5) ++paid;
    }
    std::printf("  magic_bubble cost %.3f formed %d paid %d pops %d\n", cost, formed, paid, pops);
    CHECK(formed >= 4);
    CHECK(pops >= 3);
    CHECK_EQ(paid, formed);
}

// The stick casts a squad every couple of seconds and each storm runs out on
// its own clock. The storms never pass the 30-summon cap, and a cast pays only
// when it summons: one with no room costs nothing, which reads to a player as
// a stick that ignores its full bar.
TEST(magic_stick_storms_stop_at_the_summon_cap_and_cost_only_when_cast) {
    const auto& registry = shippedContent();
    for (const Rarity tier : {Rarity::Common, Rarity::Apex}) {
        World world;
        PetalSystem petals;
        CommandBuffer commands{world};
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
        slots[0].configIndex = registry.petalIndex("mana_orb");
        slots[0].rarity = tier;
        slots[1].configIndex = registry.petalIndex("magic_stick");
        slots[1].rarity = tier;
        const double cost = registry.petalStats(registry.petalIndex("magic_stick"), tier).petManaCost;

        const auto storms = [&] {
            int n = 0;
            Query<Pet>{world}.each([&](Entity pet, Pet& tag) {
                if (tag.owner == player && !world.has<Dead>(pet)) ++n;
            });
            return n;
        };
        int casts = 0, most = 0, castsAtCap = 0;
        std::string curve;
        // Counted by birth, not by head count: once the first storms run out,
        // a cast lands on the same tick an older squad expires.
        std::set<Entity> seen;
        for (double now = 1000.0; now <= 41000.0; now += net::kTickMillis) {
            const bool hadPool = world.tryGet<ManaPool>(player) != nullptr;
            if (auto* pool = world.tryGet<ManaPool>(player)) pool->current = pool->max;
            petals.run(world, registry, now, net::kTickSeconds, commands);
            commands.flush();
            int born = 0;
            Query<Pet>{world}.each([&](Entity pet, Pet& tag) {
                if (tag.owner == player && !world.has<Dead>(pet) && seen.insert(pet).second) ++born;
            });
            const int after = storms();
            most = std::max(most, after);
            const auto* pool = world.tryGet<ManaPool>(player);
            if (hadPool && pool && pool->max - pool->current > cost * 0.5) {
                ++casts;
                CHECK(born > 0);
                if (after >= 30) ++castsAtCap;
            }
            if (static_cast<long>(now) % 4000 == 0) curve += " " + std::to_string(after);
        }
        std::printf("  magic_stick %s cost %.1f casts %d (reaching the cap %d) most %d storms every 4s:%s\n",
                    rarityName(tier), cost, casts, castsAtCap, most, curve.c_str());
        CHECK(casts > 0);
        CHECK(most <= 30);
    }
}

TEST(magic_stick_reports_when_an_eligible_summon_lacks_mana) {
    const auto& registry = shippedContent();
    World world;
    PetalSystem petals;
    CommandBuffer commands{world};
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
    auto& slot = world.add<Loadout>(player).slots[0];
    slot.configIndex = registry.petalIndex("magic_stick");
    slot.rarity = Rarity::Common;

    int warnings = 0;
    petals.onTimedSummonManaShortage = [&](Entity owner) {
        CHECK_EQ(owner, player);
        ++warnings;
    };
    for (int tick = 1; tick <= 300; ++tick) {
        petals.run(world, registry, tick * net::kTickMillis, net::kTickSeconds, commands);
        commands.flush();
    }
    CHECK(warnings > 0);
    int storms = 0;
    Query<Pet>{world}.each([&](Entity, Pet&) { ++storms; });
    CHECK_EQ(storms, 0);
}
