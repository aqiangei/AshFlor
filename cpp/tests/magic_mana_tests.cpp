// Every magic petal with a body pays mana: to form on the ring
// (PetalConfig::requiredMana) or, for a timed summon, per summon
// (PetalConfig::petManaCost).

#include "test.h"

#include "server_harness.h"
#include "server/systems/petals.h"
#include "test_data.h"

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
        if (config.id.rfind("magic_", 0) != 0 || config.count <= 0) continue;
        ++magic;
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
        if (const auto* pool = world.tryGet<ManaPool>(player); pool && pool->max - pool->current > cost * 0.5) ++paid;
    }
    std::printf("  magic_bubble cost %.3f formed %d paid %d pops %d\n", cost, formed, paid, pops);
    CHECK(formed >= 4);
    CHECK(pops >= 3);
    CHECK_EQ(paid, formed);
}

// A storm lasts 20 s and the stick casts every 2 s, so the squads pile up
// against the 30-summon cap: a cast with no room summons nothing and costs
// nothing, which reads to a player as a stick that ignores its full bar.
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
        for (double now = 1000.0; now <= 41000.0; now += net::kTickMillis) {
            if (auto* pool = world.tryGet<ManaPool>(player)) pool->current = pool->max;
            const int before = storms();
            petals.run(world, registry, now, net::kTickSeconds, commands);
            commands.flush();
            const int after = storms();
            most = std::max(most, after);
            const auto* pool = world.tryGet<ManaPool>(player);
            if (pool && pool->max - pool->current > cost * 0.5) {
                ++casts;
                CHECK(after > before);
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
