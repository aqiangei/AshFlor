// Beetle and ant eggs (U4): what each top tier of egg actually hatches, read
// off the summons the server spawns rather than the tooltip.

#include "test.h"

#include "server_harness.h"
#include "server/systems/petals.h"
#include "test_data.h"

#include <cstdio>

using namespace flix;
using namespace flix::testsupport;

namespace {

struct Hatchlings {
    int count = 0;
    double health = 0.0;
    double damage = 0.0;
    double radius = 0.0;
    double armour = 0.0;
    Rarity rarity = Rarity::Common;
};

Hatchlings hatch(const char* egg, Rarity tier) {
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
    slots[0].configIndex = registry.petalIndex(egg);
    slots[0].rarity = tier;
    Hatchlings out;
    for (double now = 1000.0; now <= 120000.0 && out.count == 0; now += 250.0) {
        petals.run(world, registry, now, net::kTickSeconds, commands);
        commands.flush();
        Query<Pet, Health, ContactDamage, Body, MobType>{world}.each(
            [&](Entity pet, Pet& tag, Health& hp, ContactDamage& hit, Body& body, MobType& type) {
                if (tag.owner != player) return;
                ++out.count;
                out.health = hp.max;
                out.damage = hit.amount;
                out.radius = body.radius;
                out.rarity = type.rarity;
                if (const auto* armour = world.tryGet<Armor>(pet)) out.armour = armour->amount;
            });
    }
    return out;
}

} // namespace

TEST(egg_hatchlings_at_the_top_tiers) {
    for (const char* egg : {"beetle_egg", "egg"}) {
        Hatchlings at[4];
        int i = 0;
        for (const Rarity tier : {Rarity::Unique, Rarity::Apex, Rarity::PermanentHyper, Rarity::Ascendant}) {
            const Hatchlings h = hatch(egg, tier);
            at[i++] = h;
            std::printf("  %-10s %-15s x%d health %.0f damage %.1f radius %.2f armour %.1f rarity %s\n", egg,
                        rarityName(tier), h.count, h.health, h.damage, h.radius, h.armour, rarityName(h.rarity));
            CHECK(h.count > 0);
        }
        // Permanent Hyper plays as Hyper; Apex is one step above it, as its petals are.
        CHECK_NEAR(at[2].health, at[1].health, at[1].health * 1e-9);
        CHECK_NEAR(at[2].damage, at[1].damage, at[1].damage * 1e-9);
        CHECK_NEAR(at[3].health, at[2].health * 1.30, at[2].health * 1e-9);
        CHECK_NEAR(at[3].damage, at[2].damage * 1.25, at[2].damage * 1e-9);
        CHECK_EQ(at[3].count, at[2].count);
    }
}
