// Fission (U5): whether the petal to its right actually comes in threes, and
// how strong each of the three is, read off the petals the server spawns
// rather than off the description.

#include "test.h"

#include "server_harness.h"
#include "server/systems/petals.h"
#include "test_data.h"

#include <cstdio>
#include <string>

using namespace flix;
using namespace flix::testsupport;

namespace {

constexpr std::uint16_t kEmpty = kNoPetal;

struct Ring {
    int slotCount = -1;  // what the server built slot 1 for
    int bodies = 0;      // petals of slot 1's kind actually orbiting
    double strength = 0.0;
    double health = 0.0;
};

Ring build(std::uint16_t left, Rarity leftTier, std::uint16_t target, Rarity targetTier) {
    World world;
    PetalSystem petals;
    CommandBuffer commands{world};
    const ContentRegistry& registry = shippedContent();
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
    if (left != kEmpty) {
        slots[0].configIndex = left;
        slots[0].rarity = leftTier;
    }
    slots[1].configIndex = target;
    slots[1].rarity = targetTier;
    for (double now = 0.0; now <= 4000.0; now += 250.0) {
        petals.run(world, registry, now, net::kTickSeconds, commands);
        commands.flush();
    }
    Ring out;
    if (const auto* state = world.tryGet<PetalSlotState>(player); state && state->slots.size() > 1)
        out.slotCount = state->slots[1].count;
    for (const Entity petal : world.get<Loadout>(player).spawned) {
        const auto& instance = world.get<PetalInstance>(petal);
        if (instance.slot != 1) continue;
        ++out.bodies;
        out.strength = instance.combinationStrength;
        if (const auto* health = world.tryGet<Health>(petal)) out.health = health->max;
    }
    return out;
}

std::uint16_t petal(const char* id) {
    return static_cast<std::uint16_t>(shippedContent().petalIndex(id));
}

} // namespace

TEST(fission_triples_the_petal_to_its_right) {
    const ContentRegistry& registry = shippedContent();
    const auto fission = petal("fission");
    struct Pair {
        Rarity fission, target;
        const char* label;
    };
    const Pair pairs[] = {
        {Rarity::Unique, Rarity::Unique, "unique>unique"},
        {Rarity::Unique, Rarity::Apex, "unique>hyper"},
        {Rarity::Apex, Rarity::Apex, "hyper>hyper"},
        {Rarity::PermanentHyper, Rarity::Ascendant, "ph>apex"},
    };
    for (const auto& pair : pairs) {
        int tripled = 0, unchanged = 0, other = 0, skipped = 0;
        std::string unchangedIds;
        for (std::size_t i = 0; i < registry.petalCount(); ++i) {
            const auto& config = registry.petal(i);
            if (i == fission || !petalComesAt(config, pair.target)) {
                ++skipped;
                continue;
            }
            const auto target = static_cast<std::uint16_t>(i);
            const int plain = build(kEmpty, pair.fission, target, pair.target).slotCount;
            const int split = build(fission, pair.fission, target, pair.target).slotCount;
            if (plain > 0 && split == plain * 3) {
                ++tripled;
            } else if (split == plain) {
                ++unchanged;
                unchangedIds += " " + config.id;
            } else {
                ++other;
                std::printf("FISSION %s odd %s plain=%d split=%d\n", pair.label, config.id.c_str(), plain, split);
            }
        }
        std::printf("FISSION %s fissionExists=%d tripled=%d unchanged=%d odd=%d skipped=%d\n", pair.label,
                    petalComesAt(registry.petal(fission), pair.fission) ? 1 : 0, tripled, unchanged, other, skipped);
        std::printf("FISSION %s unchanged:%s\n", pair.label, unchangedIds.c_str());
    }

    const char* modifiers[] = {"fission", "arctic_fission"};
    const Rarity tiers[] = {Rarity::Unique, Rarity::Apex};
    const char* targets[] = {"stinger", "rose", "light", "missile", "rice", "leaf", "wing", "magic_leaf", "sand", "pollen"};
    for (const char* modifier : modifiers) {
        for (const Rarity tier : tiers) {
            if (!petalComesAt(registry.petal(petal(modifier)), tier)) {
                std::printf("SPLIT %s %s does not come at this tier\n", modifier, rarityName(tier));
                continue;
            }
            for (const char* id : targets) {
                if (registry.petalIndex(id) >= registry.petalCount() || !petalComesAt(registry.petal(petal(id)), tier))
                    continue;
                const Ring plain = build(kEmpty, tier, petal(id), tier);
                const Ring split = build(petal(modifier), tier, petal(id), tier);
                std::printf("SPLIT %s %s %s plain=%d bodies x%.3f hp %.1f | split=%d bodies x%.3f hp %.1f | "
                            "total %.2f vs two plain slots %.2f\n",
                            modifier, rarityName(tier), id, plain.bodies, plain.strength, plain.health,
                            split.bodies, split.strength, split.health, split.bodies * split.strength,
                            2.0 * plain.bodies * plain.strength);
            }
        }
    }
    std::fflush(stdout);

    // Rose orbits within the first second; slow reloaders such as stinger never show up in this window.
    const Ring plain = build(kEmpty, Rarity::Unique, petal("rose"), Rarity::Unique);
    const Ring split = build(fission, Rarity::Unique, petal("rose"), Rarity::Unique);
    CHECK(plain.bodies > 0);
    CHECK_EQ(split.bodies, 3 * plain.bodies);
    CHECK_NEAR(split.strength, 1.0, 1e-9);

    // Arctic fission shares the petal out: more bodies, same total.
    const Ring weak = build(petal("arctic_fission"), Rarity::Unique, petal("rose"), Rarity::Unique);
    CHECK_EQ(weak.bodies, 4 * plain.bodies);
    CHECK_NEAR(weak.bodies * weak.strength, plain.bodies * plain.strength, 1e-9);
}

TEST(fission_respects_mimic_and_duplicator) {
    const ContentRegistry& registry = shippedContent();
    const auto fission = petal("fission");
    const auto mimic = petal("mimic");
    const auto rose = petal("rose");
    CHECK(fission != static_cast<std::uint16_t>(kInvalidIndex));
    CHECK(mimic != static_cast<std::uint16_t>(kInvalidIndex));
    CHECK(rose != static_cast<std::uint16_t>(kInvalidIndex));

    auto run = [&](std::uint16_t a, std::uint16_t b, std::uint16_t c, int expectSlot, int expectBodies) {
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
        auto& skills = world.add<PlayerSkillTree>(player).skills;
        skills.set(SkillId::Loadout, 3);      // Duplicator gate: Loadout florrLevel >= 4
        skills.set(SkillId::Duplicator, 3);   // Unique singles need tier 3
        auto& slots = world.add<Loadout>(player).slots;
        slots[0] = {a, Rarity::Unique};
        slots[1] = {b, Rarity::Unique};
        if (c != kEmpty) slots[2] = {c, Rarity::Unique};
        for (double now = 0.0; now <= 4000.0; now += 250.0) {
            petals.run(world, registry, now, net::kTickSeconds, commands);
            commands.flush();
        }
        int bodies = 0;
        for (const Entity petal : world.get<Loadout>(player).spawned) {
            if (world.get<PetalInstance>(petal).slot == expectSlot) ++bodies;
        }
        CHECK_EQ(bodies, expectBodies);
    };

    // Fission left of rose + duplicator on rose slot -> 2 * 3 = 6.
    run(fission, rose, kEmpty, 1, 6);
    // Mimic between fission and rose copies fission; rose still triples with duplicator.
    run(fission, mimic, rose, 2, 6);
}
