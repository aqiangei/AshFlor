// Magic Bone (#773, #804): its armour climbs Bone's ladder at every tier, and
// it forms only once the flower has a mana pool to pay for it.

#include "test.h"

#include "server_harness.h"
#include "server/systems/petals.h"
#include "test_data.h"

#include <cstdio>

using namespace flix;
using namespace flix::testsupport;

TEST(magic_bone_armour_follows_bone_at_every_tier) {
    const auto& c = shippedContent();
    const auto bone = c.petalIndex("bone");
    const auto magic = c.petalIndex("magic_bone");
    CHECK(bone != kInvalidIndex);
    CHECK(magic != kInvalidIndex);
    double previous = 0.0;
    for (const Rarity r : {Rarity::Common, Rarity::Uncommon, Rarity::Rare, Rarity::Epic,
                           Rarity::Legendary, Rarity::Mythic, Rarity::Ultra, Rarity::Super,
                           Rarity::Unique, Rarity::Apex, Rarity::PermanentHyper}) {
        const double magicArmour = c.petalStats(magic, r).petalArmor;
        const double boneArmour = c.petalStats(bone, r).petalArmor;
        std::printf("  %-15s magic_bone armour %.1f  bone armour %.1f\n", rarityName(r), magicArmour, boneArmour);
        CHECK_NEAR(magicArmour, boneArmour, 1e-6);
        CHECK(magicArmour >= previous);
        previous = magicArmour;
    }
}

namespace {

struct MagicBoneRing {
    World world;
    PetalSystem petals;
    CommandBuffer commands{world};
    const ContentRegistry& registry = shippedContent();
    Entity player = world.create();

    explicit MagicBoneRing(Rarity tier, bool withOrb) {
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
        slots[1].configIndex = registry.petalIndex("magic_bone");
        slots[1].rarity = tier;
        if (withOrb) {
            slots[0].configIndex = registry.petalIndex("mana_orb");
            slots[0].rarity = tier;
        }
    }

    void step(double now) {
        petals.run(world, registry, now, net::kTickSeconds, commands);
        commands.flush();
    }

    int bones() {
        int n = 0;
        Query<PetalInstance>{world}.each([&](Entity, PetalInstance& petal) {
            if (petal.owner == player && petal.slot == 1) ++n;
        });
        return n;
    }
};

} // namespace

TEST(magic_bone_without_a_mana_source_never_forms) {
    for (const Rarity tier : {Rarity::Common, Rarity::Super, Rarity::Apex}) {
        MagicBoneRing ring(tier, false);
        for (double now = 1000.0; now <= 20000.0; now += 500.0) ring.step(now);
        std::printf("  %-6s no mana source: pool=%d bones=%d\n", rarityName(tier),
                    ring.world.has<ManaPool>(ring.player) ? 1 : 0, ring.bones());
        CHECK(!ring.world.has<ManaPool>(ring.player));
        CHECK(ring.bones() == 0);
    }
}

TEST(magic_bone_forms_and_pays_mana_beside_an_orb) {
    for (const Rarity tier : {Rarity::Common, Rarity::Super, Rarity::Apex}) {
        MagicBoneRing ring(tier, true);
        const double cost = ring.registry.petalStats(ring.registry.petalIndex("magic_bone"), tier).requiredMana;
        ring.step(1000.0);
        CHECK(ring.world.has<ManaPool>(ring.player));
        auto& pool = ring.world.get<ManaPool>(ring.player);
        pool.current = pool.max;
        const double before = pool.current;
        int formedAt = -1;
        for (int i = 1; i <= 40 && formedAt < 0; ++i) {
            ring.step(1000.0 + 500.0 * i);
            if (ring.bones() > 0) formedAt = i;
        }
        std::printf("  %-6s with orb: max=%.1f cost=%.2f formed_step=%d bones=%d\n", rarityName(tier),
                    ring.world.get<ManaPool>(ring.player).max, cost, formedAt, ring.bones());
        CHECK(formedAt > 0);
        CHECK(ring.bones() == 1);
        CHECK(cost > 0.0);
        CHECK(ring.world.get<ManaPool>(ring.player).current < before);
    }
}
