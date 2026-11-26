#include "test.h"
#include "shared/game/terrain.h"
#include "shared/game/zorr_mana.h"
#include <set>
#include "test_data.h"
#include "client/ui/chat_i18n.h"
#include "client/ui/content_i18n.h"
#include "client/ui/i18n.h"
#include "client/ui/menu_widgets.h"
#include "server/systems/combat.h"
#include "server/systems/petals.h"
#include "server/systems/loot.h"
#include "server/systems/movement.h"
#include "server/systems/spawning.h"
#include "server/workers.h"
#include "server/squads.h"
#include "server/replication.h"
#include "client/render/world_renderer.h"
#include "client/render/sprites.h"
#include "client/world_view.h"

using namespace flix;
namespace {
struct ArcticBlast {
    const ContentRegistry& registry = testsupport::shippedContent();
    World world;
    CommandBuffer commands{world};
    SpatialGrid grid;
    EventQueue events;
    PetalSystem petals;
    CombatSystem combat;
    Entity player = NULL_ENTITY;

    explicit ArcticBlast(const char* id, Rarity rarity = Rarity::Common, const char* manaSource = nullptr) {
        player = world.create();
        world.add<PlayerTag>(player);
        world.add<Transform>(player, Transform{{5000,5000},0});
        world.add<Health>(player, Health{1000,1000});
        world.add<Body>(player, Body{25,1});
        world.add<Faction>(player, Faction{Team::Players,false});
        world.add<PlayerModifiers>(player);
        world.add<PetalRing>(player);
        world.add<Loadout>(player);
        equip(0, id, rarity);
        if (manaSource) equip(1, manaSource);
        world.add<PlayerInput>(player);
        world.add<Motion>(player);
        tickPetals(0);
        if (auto* pool = manaSource ? world.tryGet<ManaPool>(player) : nullptr) pool->current = pool->max;
        tickPetals(4000);
    }

    void equip(int slot, const char* id, Rarity rarity = Rarity::Common) {
        world.get<Loadout>(player).slots[slot] = {registry.petalIndex(id), rarity};
    }

    std::vector<Entity> liveIn(int slot) {
        std::vector<Entity> result;
        for (const Entity e : world.get<Loadout>(player).spawned) {
            if (world.get<PetalInstance>(e).slot == slot) result.push_back(e);
        }
        return result;
    }

    void tickPetals(double now) {
        petals.run(world,registry,now,net::kTickSeconds,commands);
        commands.flush();
    }

    Entity mob(Vec2 at) {
        const auto e=world.create();
        world.add<MobTag>(e);
        world.add<Bounty>(e);
        world.add<Transform>(e,Transform{at,0});
        world.add<Body>(e,Body{5,1});
        world.add<Health>(e,Health{1000,1000});
        world.add<Faction>(e,Faction{Team::Hostiles,false});
        return e;
    }

    Entity dummy(Vec2 at) {
        const Entity entity = mob(at);
        world.remove<MobTag>(entity);
        world.add<NpcTag>(entity);
        world.add<Npc>(entity).configIndex = registry.mobIndex("target_dummy");
        return entity;
    }

    void combatTick(double now) {
        grid.clear();
        Query<Transform,Body>{world}.each([&](Entity e,Transform& t,Body& b){
            grid.insert(e,t.realm,t.position,b.radius);
        });
        combat.run(world,grid,registry,now,net::kTickSeconds,commands,events);
        commands.flush();
    }

    Vec2 armBomb(double now = 4050) {
        const auto& ring = world.get<Loadout>(player).spawned;
        CHECK(!ring.empty());
        if (ring.empty()) return {};
        const auto petal = ring.front();
        const auto position = world.get<Transform>(petal).position;
        world.get<Health>(petal).current = 0;
        world.add<Dead>(petal);
        tickPetals(now);
        return position;
    }
};
}

TEST(card_suit_changes_live_contact_durability_and_the_network_variant) {
    ArcticBlast rig("card", Rarity::Rare);
    const auto index = rig.registry.petalIndex("card");
    const auto stats = rig.registry.petalStats(index, Rarity::Rare);
    rig.world.add<PlayerLocation>(rig.player);
    rig.world.add<NetId>(rig.player, NetId{900000});
    rig.world.add<Replicated>(rig.player, Replicated{net::EntityKind::Player, 0, 0, Rarity::Common});
    std::set<std::uint8_t> seen;

    for (int draw = 0; draw < 24; ++draw) {
        const double now = 10000 + draw * 10000.0;
        rig.world.get<Loadout>(rig.player).slots[0] = {};
        rig.tickPetals(now);
        rig.equip(0, "card", Rarity::Rare);
        rig.tickPetals(now + 50);
        rig.tickPetals(now + 4000);
        const auto active = rig.liveIn(0);
        CHECK_EQ(active.size(), 1u);
        if (active.empty()) continue;
        const auto card = active.front();
        const auto suit = rig.world.get<PetalInstance>(card).cardSuit;
        CHECK(suit >= 1 && suit <= 4);
        seen.insert(suit);
        CHECK_NEAR(rig.world.get<Health>(card).max, stats.health * cardHealthScale(suit), 1e-8);
        CHECK_NEAR(rig.world.get<PetalSlotState>(rig.player).slots[0].healthFraction, 1.0, 1e-8);

        const auto target = rig.mob(rig.world.get<Transform>(card).position + Vec2{1, 0});
        rig.combatTick(now + 4050);
        CHECK_NEAR(1000 - rig.world.get<Health>(target).current, stats.damage * cardDamageScale(suit), 1e-8);
        rig.world.destroy(target);

        const std::uint32_t id = 900001 + draw;
        rig.world.add<NetId>(card, NetId{id});
        Replicator replication;
        ClientView serverView;
        Replicator::Frame frame;
        frame.tick = 1;
        frame.nowMillis = now + 4050;
        frame.events = &rig.events;
        ByteWriter bytes;
        replication.build(rig.world, rig.player, serverView, frame, bytes);
        ByteReader reader(bytes.data(), bytes.size());
        CHECK_EQ(reader.u8(), static_cast<std::uint8_t>(net::ServerMessage::Snapshot));
        WorldView client;
        CHECK(client.applySnapshot(reader));
        CHECK(reader.ok());
        const auto seenCard = client.entities().find(id);
        CHECK(seenCard != client.entities().end());
        if (seenCard != client.entities().end()) CHECK_EQ(seenCard->second.petalVariant, suit);
    }
    CHECK_EQ(seen.size(), 4u);
    const auto last = rig.liveIn(0);
    if (last.empty()) return;
    rig.world.get<Health>(last.front()).current = 0.0;
    rig.tickPetals(250000);
    CHECK(rig.liveIn(0).empty());
    rig.tickPetals(255000);
    const auto replacement = rig.liveIn(0);
    CHECK_EQ(replacement.size(), 1u);
    if (replacement.empty()) return;
    const auto suit = rig.world.get<PetalInstance>(replacement.front()).cardSuit;
    CHECK(suit >= 1 && suit <= 4);
    CHECK_NEAR(rig.world.get<Health>(replacement.front()).max,
               stats.health * cardHealthScale(suit), 1e-8);
}

TEST(regular_coral_shatters_once_on_contact_and_shards_deal_half_damage) {
    ArcticBlast rig("coral");
    const auto active = rig.liveIn(0);
    CHECK_EQ(active.size(), 1u);
    if (active.empty()) return;
    const Entity coral = active.front();
    const auto origin = rig.world.get<Transform>(coral).position;
    const auto target = rig.mob(origin + Vec2{1, 0});
    const auto stats = rig.registry.petalStats(rig.registry.petalIndex("coral"), Rarity::Common);
    rig.combatTick(4050);
    CHECK_NEAR(1000 - rig.world.get<Health>(target).current, stats.damage, 1e-8);
    CHECK_NEAR(rig.world.get<Health>(coral).current, 0.0, 1e-8);
    rig.world.destroy(target);
    rig.tickPetals(4100);
    const auto shards = Query<Projectile>{rig.world}.without<Dead>().collect();
    CHECK_EQ(shards.size(), 2u);
    if (shards.size() != 2) return;
    for (const Entity shard : shards) {
        CHECK_NEAR(rig.world.get<Projectile>(shard).damage, stats.damage * 0.5, 1e-8);
        CHECK_NEAR(rig.world.get<Health>(shard).max, stats.health * 0.5, 1e-8);
    }
    const auto a = rig.world.get<Transform>(shards.front());
    const auto next = rig.mob(a.position + Vec2::fromAngle(a.angle, 50));
    MovementSystem movement;
    Terrain terrain;
    terrain.fill(Tile::Open);
    for (int tick = 1; tick <= 6; ++tick) {
        movement.run(rig.world, terrain, 4100 + tick * 50, net::kTickSeconds);
        rig.combatTick(4100 + tick * 50);
    }
    CHECK_NEAR(1000 - rig.world.get<Health>(next).current, stats.damage * 0.5, 1e-8);
    CHECK(Query<Projectile>{rig.world}.without<Dead>().collect().size() <= 1);
}

TEST(clover_publishes_the_strongest_equipped_luck_bonus_to_the_world) {
    ArcticBlast rig("clover", Rarity::Rare);
    rig.equip(1, "clover");
    rig.tickPetals(4050);
    rig.tickPetals(10000);
    const auto luck = [&] { return rig.world.get<PlayerModifiers>(rig.player).luck; };
    CHECK_NEAR(luck(), 1.3, 1e-8);
    rig.world.get<Loadout>(rig.player).slots[0] = {};
    rig.tickPetals(10050);
    CHECK_NEAR(luck(), 1.1, 1e-8);
    rig.world.get<Loadout>(rig.player).slots[1] = {};
    rig.tickPetals(10100);
    CHECK_NEAR(luck(), 1.0, 1e-8);
}

TEST(bean_contact_bonus_depends_on_nearby_living_allies_in_its_map) {
    enum Mode { Alone, NearbyAlly, DistantAlly, OtherRealm, Enemy, DownedAlly };
    for (auto mode : {Alone, NearbyAlly, DistantAlly, OtherRealm, Enemy, DownedAlly}) {
        ArcticBlast rig("bean");
        const auto active=rig.liveIn(0);
        CHECK_EQ(active.size(),1u);
        if(active.empty())return;
        const auto blossom=rig.world.get<Transform>(rig.player).position;
        if (mode != Alone) {
            const Entity neighbor=rig.world.create();
            rig.world.add<PlayerTag>(neighbor);
            rig.world.add<Transform>(neighbor,Transform{blossom+Vec2{mode == DistantAlly?221.0:150.0,0},0,
                mode == OtherRealm?Realm::Arena:Realm::Overworld});
            rig.world.add<Body>(neighbor,Body{25,1});
            rig.world.add<Faction>(neighbor,Faction{Team::Players,mode == Enemy});
            rig.world.add<Health>(neighbor, Health{mode == DownedAlly ? 0.0 : 1000.0, 1000.0});
        }
        const auto at=rig.world.get<Transform>(active.front()).position;
        const auto target=rig.mob(at+Vec2{1,0});
        rig.combatTick(5000);
        const auto base=rig.registry.petalStats(rig.registry.petalIndex("bean"),Rarity::Common).damage;
        CHECK_NEAR(1000-rig.world.get<Health>(target).current,base*(mode == NearbyAlly?1.0:1.5),1e-8);
    }
}

TEST(broccoli_break_fires_one_bounded_radial_volley_at_actual_targets) {
    ArcticBlast rig("broccoli");
    const auto active=rig.liveIn(0);
    CHECK_EQ(active.size(),1u);
    if(active.empty())return;
    rig.world.get<PlayerInput>(rig.player).current.flags=net::InputAttack;
    rig.tickPetals(4950);
    CHECK(Query<Projectile>{rig.world}.collect().empty());
    const Vec2 at=rig.world.get<Transform>(active.front()).position;
    rig.world.add<Dead>(active.front());
    rig.tickPetals(5000);
    const auto shots=Query<Projectile>{rig.world}.collect();
    CHECK_EQ(shots.size(),3u);
    if(shots.size()!=3)return;
    double total=0;
    for(const Entity shot : shots) {
        const auto& p=rig.world.get<Projectile>(shot);
        total+=p.damage;
        CHECK_EQ(p.owner,rig.player);
        CHECK_NEAR(p.remainingDistance,240,1e-8);
        CHECK_NEAR(distance(at,rig.world.get<Transform>(shot).position),0,1e-8);
    }
    const double base=rig.registry.petalStats(rig.registry.petalIndex("broccoli"),Rarity::Common).damage;
    CHECK_NEAR(total,base,1e-8);
    const auto first=shots.front();
    const auto direction=Vec2::fromAngle(rig.world.get<Transform>(first).angle);
    const auto target=rig.mob(at+direction*100.0);
    MovementSystem movement;
    Terrain terrain;
    terrain.fill(Tile::Open);
    for (int tick=1;tick<=5;++tick) {
        movement.run(rig.world,terrain,5000+tick*50,net::kTickSeconds);
        rig.combatTick(5000+tick*50);
    }
    CHECK_NEAR(1000-rig.world.get<Health>(target).current,base/3,1e-8);
    rig.tickPetals(5300);
    CHECK(Query<Projectile>{rig.world}.without<Dead>().collect().size()<=2);
}

TEST(chromosome_repairs_each_slot_and_shares_it_across_damaged_grains) {
    ArcticBlast rig("chromosome");
    rig.equip(1, "grapes");
    rig.equip(2, "magic_peas");
    rig.equip(3, "scales");
    rig.equip(4, "cotton");
    rig.equip(5, "mana_orb", Rarity::Unique);
    rig.tickPetals(4050);
    rig.world.get<ManaPool>(rig.player).current = rig.world.get<ManaPool>(rig.player).max;
    rig.tickPetals(8000);

    const auto grapes = rig.liveIn(1);
    const auto blueberries = rig.liveIn(2);
    const auto scales = rig.liveIn(3);
    const auto cotton = rig.liveIn(4);
    CHECK(grapes.size() == 4 && blueberries.size() == 4 && scales.size() == 4 && cotton.size() == 1);
    if (grapes.size() != 4 || blueberries.size() != 4 || scales.size() != 4 || cotton.size() != 1) return;
    const auto damage = [&](Entity e, double amount) { rig.world.get<Health>(e).current -= amount; };
    const auto missing = [&](Entity e) {
        const auto& health = rig.world.get<Health>(e);
        return health.max - health.current;
    };

    for (std::size_t i = 0; i < grapes.size(); ++i) damage(grapes[i], i + 1.0);
    rig.world.get<Health>(blueberries[0]).current = 0.0;
    damage(blueberries[1], 5.0);
    damage(blueberries[2], 2.0);
    rig.world.add<Afflictions>(blueberries[2]).noHealUntilMillis = 10000;
    damage(scales.front(), 9.0);
    damage(cotton.front(), 1.75);

    rig.tickPetals(9000);
    for (std::size_t i = 0; i < grapes.size(); ++i) CHECK_NEAR(missing(grapes[i]), (i + 1.0) * 0.4, 1e-8);
    CHECK_EQ(rig.liveIn(2).size(), 3u);
    CHECK_NEAR(missing(blueberries[1]), 0.0, 1e-8);
    CHECK_NEAR(missing(blueberries[2]), 2.0, 1e-8);
    CHECK_NEAR(missing(blueberries[3]), 0.0, 1e-8);
    CHECK_NEAR(missing(cotton.front()), 0.75, 1e-8);
    for (Entity e : scales) CHECK_NEAR(missing(e), 3.0, 1e-8);
    rig.tickPetals(9050);
    for (Entity e : scales) CHECK_NEAR(missing(e), 3.0, 1e-8);
}

TEST(chromosome_repair_tooltip_identifies_the_budget_and_durability_cap) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    const auto index = content().petalIndex("chromosome");
    const auto language = ui::language();
    const auto hasLine = [&](ui::Language language, const char* expected) {
        ui::setLanguage(language);
        const auto lines = ui::petalTooltipLines(index, Rarity::Common, {}, 1.0);
        return std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
            return line.text.find(expected) != std::string::npos;
        });
    };
    CHECK(hasLine(ui::Language::En, "Repair per slot per pulse: 6"));
    CHECK(hasLine(ui::Language::En, "Slot durability cap: 30%"));
    CHECK(hasLine(ui::Language::Zh, "每槽单次修复上限：6"));
    CHECK(hasLine(ui::Language::Zh, "每槽耐久比例上限：30%"));
    ui::setLanguage(language);
}

TEST(zorr_feedback_mana_restorers_form_with_an_empty_innate_pool) {
    for (const char* id : {"magic_dahlia", "magic_arctic_banana", "magic_arctic_ice_rose",
                          "magic_fries", "magic_sunflower", "magic_starfish",
                          "magic_lily_pad", "magic_vampire_fang"}) {
        ArcticBlast rig(id);
        rig.world.get<ManaPool>(rig.player).current = 0.0;
        rig.tickPetals(6500);
        const auto live = rig.liveIn(0);
        CHECK(!live.empty());
        const auto stats = rig.registry.petalStats(rig.registry.petalIndex(id), Rarity::Common);
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).max, kFlowerBaseMana + stats.maxMana, 1e-8);
        for (Entity petal : live) {
            CHECK_NEAR(rig.world.get<PetalInstance>(petal).formationMana, 0.0, 1e-8);
            rig.world.add<Dead>(petal);
        }
        rig.tickPetals(6550);
        rig.world.get<ManaPool>(rig.player).current = 0.0;
        rig.tickPetals(16550);
        const auto reformed = rig.liveIn(0);
        CHECK(!reformed.empty());
        for (Entity petal : reformed) {
            CHECK(std::find(live.begin(), live.end(), petal) == live.end());
            CHECK_NEAR(rig.world.get<PetalInstance>(petal).formationMana, 0.0, 1e-8);
        }
    }
    const auto& registry = testsupport::shippedContent();
    CHECK(registry.petalStats(registry.petalIndex("magic_pumpkin_seed"), Rarity::Common).requiredMana > 0);
    CHECK(registry.petalStats(registry.petalIndex("magic_arctic_emerald"), Rarity::Common).requiredMana > 0);
}

TEST(zorr_feedback_worn_magic_is_paid_once_per_tick_even_while_broken) {
    for (const char* id : {"magic_void_opal", "magic_void_relativity", "magic_void_singularity",
                          "magic_topaz", "magic_citron", "magic_cutter", "magic_ruby",
                          "magic_arctic_sapphire", "magic_void_antimatter", "magic_void_mask",
                          "magic_rubber", "magic_quartz"}) {
        ArcticBlast rig(id);
        const std::string_view name(id);
        rig.world.ensure<ContactDamage>(rig.player);
        const auto index = rig.registry.petalIndex(id);
        const auto stats = rig.registry.petalStats(index, Rarity::Common);
        CHECK_NEAR(stats.requiredMana, 0.0, 1e-8);
        const double charge = wornMagicManaPerSecond(id, Rarity::Common) * net::kTickSeconds;
        CHECK(charge > 0.0);
        auto& slot = rig.world.get<Loadout>(rig.player).slots[0];
        slot.broken = true;
        slot.reloadReadyAtMillis = 9000.0;
        rig.world.get<ManaPool>(rig.player).current = charge * 1.5;
        if (name == "magic_arctic_sapphire") {
            rig.tickPetals(4050);
            CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current, charge * 1.5, 1e-8);
        }
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.petals.foldModifiers(rig.world, rig.registry, 4100, net::kTickSeconds);
        rig.tickPetals(4100);
        const auto mana = [&] { return rig.world.get<ManaPool>(rig.player); };
        CHECK_NEAR(mana().current, charge * 0.5, 1e-8);
        CHECK_EQ(mana().fundedWornSlots, 1u);
        const auto effectActive = [&](const PlayerModifiers& mods) {
            if (name == "magic_void_opal") return mods.magicOpalTier >= 0;
            if (name == "magic_void_relativity") return mods.secondaryMotionBonus > 0;
            if (name == "magic_void_singularity") return mods.sizeScale < 1.0;
            if (name == "magic_topaz") return mods.summonedArmor > 0;
            if (name == "magic_citron") return mods.petalSizeFactor < 1.0;
            if (name == "magic_ruby") return mods.concentratesBlasts;
            if (name == "magic_void_antimatter") return mods.retaliationBlast > 0;
            if (name == "magic_void_mask") return mods.blindRecovery > 0;
            if (name == "magic_rubber") return mods.knockbackTakenScale < 1.0;
            if (name == "magic_quartz") return mods.quartzTier >= 0;
            return mana().fundedWornSlots != 0;
        };
        CHECK(effectActive(rig.world.get<PlayerModifiers>(rig.player)));
        const double activeContact = rig.world.get<ContactDamage>(rig.player).amount;
        rig.tickPetals(4150);
        CHECK_NEAR(mana().current, charge * 0.5, 1e-8);
        CHECK_EQ(mana().fundedWornSlots, 0u);
        CHECK(!effectActive(rig.world.get<PlayerModifiers>(rig.player)));
        if (name == "magic_cutter")
            CHECK_NEAR(activeContact - rig.world.get<ContactDamage>(rig.player).amount, 8.0, 1e-8);
    }
}

TEST(zorr_feedback_mana_balance_uses_leaf_rate_and_strongest_hemoglobin) {
    const auto& registry = testsupport::shippedContent();
    for (Rarity tier : {Rarity::Common, Rarity::Mythic, Rarity::Unique, Rarity::Ascendant}) {
        const auto stats = [&](const char* id) { return registry.petalStats(registry.petalIndex(id), tier); };
        CHECK_NEAR(stats("magic_basil").passiveManaPerSecond, stats("magic_leaf").passiveManaPerSecond, 1e-8);
        CHECK_NEAR(stats("magic_palm_leaf").passiveManaPerSecond, 1.15 * stats("magic_leaf").passiveManaPerSecond, 1e-8);
        CHECK_NEAR(stats("void_elemental").maxMana, 0.0, 1e-8);
        const double apexGrowth = tier == Rarity::Ascendant ? 1.15 : 1.0;
        CHECK_NEAR(stats("magic_soil").maxMana, 66.0 * petalManaScale(tier) * apexGrowth, 1e-8);
    }
    ArcticBlast basil("magic_basil", Rarity::Mythic);
    const double perTick = registry.petalStats(registry.petalIndex("magic_leaf"), Rarity::Mythic)
        .passiveManaPerSecond * net::kTickSeconds;
    basil.world.get<ManaPool>(basil.player).current = kFlowerBaseMana / 4.0;
    basil.tickPetals(4050);
    CHECK_NEAR(basil.world.get<ManaPool>(basil.player).current, kFlowerBaseMana / 4.0 + perTick, 1e-8);
    basil.world.get<ManaPool>(basil.player).current = 0;
    basil.tickPetals(4100);
    CHECK_NEAR(basil.world.get<ManaPool>(basil.player).current, 2.0 * perTick, 1e-8);

    ArcticBlast blood("hemoglobin");
    blood.equip(1, "hemoglobin", Rarity::Uncommon);
    blood.equip(2, "hemoglobin", Rarity::Uncommon);
    blood.tickPetals(4100);
    CHECK_NEAR(blood.world.get<PlayerModifiers>(blood.player).hemoglobinHealBonus, 0.24, 1e-8);
}

TEST(zorr_feedback_magical_bomb_fields_stack_after_separate_shells_break) {
    for (const char* id : {"magic_void_fire_bomb", "magic_gas_bomb"}) {
        ArcticBlast rig(id, Rarity::Common, "mana_orb");
        for (int slot = 1; slot < 4; ++slot) rig.equip(slot, id);
        rig.equip(4, "mana_orb", Rarity::Rare);
        rig.tickPetals(4100);
        auto& mana = rig.world.get<ManaPool>(rig.player);
        mana.current = mana.max;
        rig.tickPetals(8000);
        const Vec2 at{5300, 5300};
        for (int slot = 0; slot < 4; ++slot) {
            const auto live = rig.liveIn(slot);
            CHECK_EQ(live.size(), 1u);
            if (live.empty()) continue;
            rig.world.get<Transform>(live.front()).position = at;
            rig.world.add<Dead>(live.front());
        }
        rig.tickPetals(8050);
        const Entity target = rig.mob(at);
        rig.combatTick(8050);
        const auto* status = rig.world.tryGet<Afflictions>(target);
        CHECK(status != nullptr);
        if (!status) continue;
        if (std::string_view(id) == "magic_gas_bomb") CHECK_EQ(status->poisonStacks.size(), 4u);
        else CHECK_EQ(status->burns.size(), 4u);
        const double afterBlast = rig.world.get<Health>(target).current;
        rig.combatTick(8100);
        CHECK(rig.world.get<Health>(target).current < afterBlast);
    }
}

TEST(zorr_feedback_separate_missiles_and_berry_arcs_keep_their_damage) {
    for (const char* id : {"magic_hel_missile", "magic_viper", "magic_arctic_blueberries", "magic_rocket"}) {
        const std::string_view name(id);
        ArcticBlast rig(id, Rarity::Common, "mana_orb");
        rig.equip(1, id);
        rig.equip(2, "mana_orb", Rarity::Rare);
        rig.tickPetals(4100);
        auto& mana = rig.world.get<ManaPool>(rig.player);
        mana.current = mana.max;
        rig.tickPetals(8000);
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.tickPetals(8200);
        rig.tickPetals(8600);
        const auto shots = Query<Projectile>{rig.world}.without<Dead>().collect();
        CHECK(shots.size() >= 2u);
        if (shots.size() < 2u) continue;

        const Vec2 at{5300, 5300};
        const Entity target = rig.mob(at);
        rig.world.get<Health>(target) = {10000, 10000};
        for (std::size_t i = 0; i < shots.size(); ++i) {
            if (i < 2) rig.world.get<Transform>(shots[i]).position = at;
            else rig.world.add<Dead>(shots[i]);
        }
        rig.combatTick(8600);
        const double beforeArcs = rig.world.get<Health>(target).current;
        CHECK(beforeArcs < 10000.0);
        rig.petals.landShots(rig.world, rig.registry, rig.combat.shotLandings(), nullptr, 8600, &rig.events);
        rig.combatTick(8650);
        const auto* status = rig.world.tryGet<Afflictions>(target);
        if (name == "magic_viper") CHECK(status && status->poisonStacks.size() == 2u);
        else if (name == "magic_rocket") CHECK(rig.world.get<Health>(target).current < beforeArcs);
        else CHECK(status && status->burns.size() == 2u);
    }
}

TEST(zorr_feedback_lightning_riders_stack_from_separate_petals) {
    for (const char* id : {"magic_lightning", "magic_hel_lightning", "magic_usain_bolt"}) {
        const std::string_view name(id);
        ArcticBlast rig(id, Rarity::Common, "mana_orb");
        rig.equip(1, id);
        rig.equip(2, "mana_orb", Rarity::Rare);
        rig.tickPetals(4100);
        auto& mana = rig.world.get<ManaPool>(rig.player);
        mana.current = mana.max;
        rig.tickPetals(8000);
        const Entity target = rig.mob(rig.world.get<Transform>(rig.player).position);
        rig.world.get<Health>(target) = {10000, 10000};
        rig.world.get<Body>(target).radius = 160.0;
        rig.tickPetals(8050);
        int casts = 0;
        Query<LightningTarget>{rig.world}.each([&](Entity, LightningTarget& arc) {
            if (arc.victim == target) ++casts;
        });
        CHECK_EQ(casts, 2);
        rig.combatTick(8050);
        CHECK(rig.world.get<Health>(target).current < 10000.0);
        const auto* status = rig.world.tryGet<Afflictions>(target);
        if (name == "magic_hel_lightning") CHECK(status && status->burns.size() == 2u);
        if (name == "magic_usain_bolt") CHECK(status && status->poisonStacks.size() == 2u);
    }
}

TEST(zorr_feedback_contact_and_radiation_sources_stack_without_refreshing_other_copies) {
    const Vec2 at{5300, 5300};
    for (const char* id : {"magic_gas", "magic_bubonic_plague"}) {
        ArcticBlast rig(id, Rarity::Common, "mana_orb");
        rig.equip(1, id);
        rig.equip(2, "mana_orb", Rarity::Rare);
        rig.tickPetals(4100);
        auto& mana = rig.world.get<ManaPool>(rig.player);
        mana.current = mana.max;
        rig.tickPetals(8000);
        const Entity target = rig.mob(at + Vec2{15, 0});
        rig.world.get<Health>(target) = {10000, 10000};
        rig.world.get<Body>(target).radius = 16.0;
        for (int slot = 0; slot < 2; ++slot) {
            const auto live = rig.liveIn(slot);
            CHECK_EQ(live.size(), 1u);
            if (live.empty()) continue;
            rig.world.get<Transform>(live.front()).position = at;
            rig.world.get<PetalInstance>(live.front()).facingAngle = 0.0;
        }
        rig.combatTick(8050);
        const auto* status = rig.world.tryGet<Afflictions>(target);
        if (std::string_view(id) == "magic_gas") CHECK(status && status->poisonStacks.size() == 2u);
        else CHECK(status && status->burns.size() == 2u);
    }

    ArcticBlast dust("magic_void_stardust", Rarity::Common, "mana_orb");
    dust.world.get<PlayerInput>(dust.player).current.flags = net::InputAttack;
    dust.tickPetals(4500);
    const auto projectiles = Query<Projectile>{dust.world}.without<Dead>().collect();
    CHECK(projectiles.size() >= 2u);
    if (projectiles.size() < 2u) return;
    for (std::size_t i = 0; i < projectiles.size(); ++i) {
        if (i < 2) dust.world.get<Transform>(projectiles[i]).position = at;
        else dust.world.add<Dead>(projectiles[i]);
    }
    const Entity target = dust.mob(at + Vec2{35, 0});
    dust.tickPetals(5000);
    dust.combatTick(5000);
    const auto* status = dust.world.tryGet<Afflictions>(target);
    CHECK(status && status->poisonStacks.size() == 2u);
    CHECK(status && status->burns.size() == 2u);

    dust.world.get<PlayerInput>(dust.player).current.flags = 0;
    dust.tickPetals(5500);
    dust.combatTick(5500);
    const auto& refreshed = dust.world.get<Afflictions>(target);
    CHECK_EQ(refreshed.poisonStacks.size(), 2u);
    CHECK_EQ(refreshed.burns.size(), 2u);
    for (const auto& burn : refreshed.burns) CHECK_NEAR(burn.untilMillis, 8500.0, 1e-8);
}

TEST(zorr_feedback_spills_keep_the_soap_bonus_after_swap_and_expire_at_the_new_limit) {
    ArcticBlast rig("void_soap");
    const auto& spill = rig.registry.petal(rig.registry.petalIndex("latrine")).spill;
    const Vec2 at{5300, 5300};
    CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, spill, 1.0, 0);
    const auto initial = Query<GroundEffect, Lifetime>{rig.world}.collect();
    CHECK_EQ(initial.size(), 1u);
    if (initial.empty()) return;
    const Entity first = initial.front();
    const double range = rig.world.get<GroundEffect>(first).spreadTargetRadius;
    CHECK_NEAR(rig.world.get<Lifetime>(first).remainingSeconds, 15.0, 1e-8);
    rig.world.get<Loadout>(rig.player).slots[0] = {};
    rig.tickPetals(4100);
    CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, spill, 1.0, 0);
    const auto pooled = Query<GroundEffect, Lifetime>{rig.world}.collect();
    CHECK_EQ(pooled.size(), 2u);
    for (Entity pool : pooled) {
        if (pool == first) CHECK_NEAR(rig.world.get<GroundEffect>(pool).spreadTargetRadius, range, 1e-8);
        else CHECK(rig.world.get<GroundEffect>(pool).spreadTargetRadius < range);
    }
    rig.world.get<GroundEffect>(first).spreadAgeSeconds = 1.0;
    for (int i = 0; i < kFluidPoolLimit - 1; ++i)
        CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, spill, 1.0, 0);
    CHECK(rig.world.has<Dead>(first));
    CHECK_EQ((Query<GroundEffect, Lifetime>{rig.world}.without<Dead>().count()), kFluidPoolLimit);
}

TEST(zorr_feedback_magic_sacrifice_uses_local_rosters_petal_tier_and_fission) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(testsupport::dataDir(), &terrain, error));
    if (!error.empty()) return;
    SpawnSystem spawning;
    spawning.worldMaps = &maps;
    for (const char* mapId : {"garden", "ocean", "zorr_void"}) {
        const bool split = std::string_view(mapId) == "ocean";
        const Rarity tier = std::string_view(mapId) == "zorr_void" ? Rarity::Apex : Rarity::Ascendant;
        ArcticBlast rig(split ? "fission" : "basic", tier);
        bool found = false;
        const Realm realm = maps.realmOfId(mapId, found);
        CHECK(found);
        if (!found) continue;
        const auto& regions = maps.forRealm(realm)->elements();
        const auto valid = std::find_if(regions.begin(), regions.end(), [](const auto& zone) {
            return zone.isSpawnBand() && !zone.singular && zone.contains(zone.centre()) &&
                rarityIndex(dominantTierForDifficulty(zone.difficulty)) >= rarityIndex(Rarity::Rare);
        });
        CHECK(valid != regions.end());
        if (valid == regions.end()) continue;
        Rng rng(23);
        std::set<std::uint16_t> local;
        for (int i = 0; i < 100; ++i) {
            const auto mob = spawning.rollSacrificePet(rig.registry, tier, valid->centre(), realm, rng);
            if (mob != kInvalidIndex) local.insert(mob);
        }
        CHECK(local.size() > 1u);
        rig.petals.worldMaps = &maps;
        rig.petals.spawning = &spawning;
        rig.world.get<Transform>(rig.player) = {valid->centre(), 0.0, realm};
        rig.equip(1, "magic_blood_sacrifice", tier);
        if (split) {
            auto& skills = rig.world.add<PlayerSkillTree>(rig.player).skills;
            skills.set(SkillId::Summoner, 3);
            skills.set(SkillId::PetDamage, 3);
        }
        rig.equip(2, "mana_orb", tier);
        rig.tickPetals(4050);
        auto& pool = rig.world.get<ManaPool>(rig.player);
        pool.current = pool.max;
        rig.tickPetals(16000);
        CHECK_EQ(rig.liveIn(1).size(), split ? 3u : 1u);
        const auto copies = rig.liveIn(1);
        if (copies.empty()) continue;
        const double combinationStrength = rig.world.get<PetalInstance>(copies.front()).combinationStrength;
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.tickPetals(20000);
        const auto pets = Query<Pet, MobType>{rig.world}.collect();
        std::size_t heads = 0;
        for (Entity pet : pets) {
            const auto* segment = rig.world.tryGet<BodySegment>(pet);
            if (segment && !segment->head) continue;
            ++heads;
            const auto& type = rig.world.get<MobType>(pet);
            CHECK_EQ(type.rarity, tier);
            CHECK(local.count(type.configIndex) == 1u);
            CHECK_NEAR(rig.world.get<Pet>(pet).expiresAtMillis, 35000.0, 1e-8);
            const auto& species = rig.registry.mob(type.configIndex);
            const auto& sacrifice = rig.registry.petal(rig.registry.petalIndex("magic_blood_sacrifice"));
            const auto& petState = rig.world.get<Pet>(pet);
            const auto* tree = rig.world.tryGet<PlayerSkillTree>(rig.player);
            const double healthTalent = tree ? tree->skills.healthScale(SkillId::Summoner) : 1.0;
            const double damageTalent = tree ? tree->skills.effectScale(SkillId::PetDamage) : 1.0;
            CHECK_NEAR(petState.damageTalentScale, damageTalent, 1e-8);
            const double scale = petalStatScale(tier) * configuredSummonStrength(sacrifice, tier, nullptr, 1) *
                hyperSummonPromotionScale(sacrifice, tier);
            const double hp = species.health * species.petHealthScale * scale * kSummonedMobHealthScale *
                6.0 * ascendantSummonHealthScale(tier) * healthTalent;
            const double damage = species.damage * species.petDamageScale * scale * 2.0 *
                ascendantSummonDamageScale(tier) * damageTalent;
            CHECK_NEAR(rig.world.get<Health>(pet).max, hp * combinationStrength,
                       std::max(1e-7, hp * 1e-10));
            CHECK_NEAR(rig.world.get<ContactDamage>(pet).amount, damage * combinationStrength,
                       std::max(1e-7, damage * 1e-10));
        }
        CHECK_EQ(heads, split ? 6u : 2u);
        if (std::string_view(mapId) != "garden") continue;

        rig.world.get<ManaPool>(rig.player).current = rig.world.get<ManaPool>(rig.player).max;
        for (double now = 20000.0 + net::kTickMillis; now < 35000.0; now += net::kTickMillis)
            rig.tickPetals(now);
        std::size_t firstBatch = 0;
        std::size_t secondBatch = 0;
        Query<Pet, MobType>{rig.world}.each([&](Entity pet, Pet& summoning, MobType&) {
            const auto* segment = rig.world.tryGet<BodySegment>(pet);
            if (segment && !segment->head) return;
            if (summoning.expiresAtMillis == 35000.0) ++firstBatch;
            if (summoning.expiresAtMillis > 35000.0) ++secondBatch;
        });
        CHECK_EQ(firstBatch, 2u);
        CHECK_EQ(secondBatch, 2u);
    }
}

TEST(damage_over_time_contact_copies_stack_and_count_on_the_dummy) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const char* name : {"stinger", "magic_stinger", "void_dvd", "void_elemental"}) {
        const std::string_view id(name);
        const bool poison = id != "void_dvd";
        const bool burn = id == "void_dvd" || id == "void_elemental";
        ArcticBlast rig(name, Rarity::Common, id == "magic_stinger" ? "mana_orb" : nullptr);
        rig.equip(2, name);
        if (auto* mana = rig.world.tryGet<ManaPool>(rig.player)) mana->current = mana->max;
        rig.tickPetals(15000);
        if (auto* mana = rig.world.tryGet<ManaPool>(rig.player)) mana->current = mana->max;
        rig.tickPetals(25000);
        const auto first = rig.liveIn(0);
        const auto second = rig.liveIn(2);
        CHECK(!first.empty() && !second.empty());
        if (first.empty() || second.empty()) continue;

        const Vec2 at{5300, 5300};
        const Entity dummy = rig.dummy(at);
        rig.world.get<Transform>(first.front()).position = at;
        rig.world.get<Transform>(second.front()).position = at;
        rig.combatTick(25050);
        const auto* status = rig.world.tryGet<Afflictions>(dummy);
        CHECK(status);
        if (!status) continue;
        CHECK_EQ(status->poisonStacks.size(), poison ? 2u : 0u);
        CHECK_EQ(status->burns.size(), burn ? 2u : 0u);

        const auto configIndex = rig.registry.petalIndex(name);
        const auto& stats = rig.registry.petalStats(configIndex, Rarity::Common);
        const auto& config = rig.registry.petal(configIndex);
        rig.world.get<Transform>(dummy).position = {5600, 5600};
        rig.combatTick(25100);
        const auto damage = rig.combat.practiceDamage(rig.player, 25100, dummy);
        const auto amount = [&](CombatSystem::PracticeSource source) {
            const auto row = std::find_if(damage.begin(), damage.end(),
                [source](const auto& hit) { return hit.source == source; });
            return row == damage.end() ? 0.0 : row->amount;
        };
        // The periodic amounts are counted even though the wood never loses HP.
        CHECK_NEAR(amount(CombatSystem::PracticeSource::Poison),
                   2 * stats.poisonPerSecond * net::kTickSeconds, 1e-8);
        CHECK_NEAR(amount(CombatSystem::PracticeSource::Burn),
                   2 * config.burnPerSecond * net::kTickSeconds, 1e-8);
        CHECK_EQ(rig.world.get<Health>(dummy).current, 1000.0);

        for (int now = 25150; now <= 28100; now += 50) rig.combatTick(now);
        CHECK(rig.world.get<Afflictions>(dummy).burns.empty());
        CHECK(rig.world.get<Afflictions>(dummy).poisonStacks.empty());
    }
}

TEST(damage_over_time_hel_missile_impacts_stack_on_the_dummy) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("basic");
    const Vec2 at{5400, 5000};
    const Entity dummy = rig.dummy(at);
    const auto config = rig.registry.petalIndex("hel_missile");
    for (const double now : {4050.0, 4100.0}) {
        const Entity shot = rig.world.create();
        Projectile projectile;
        projectile.owner = rig.player;
        projectile.petalConfigIndex = config;
        projectile.damage = 30;
        projectile.remainingDistance = 500;
        projectile.lastPosition = at - Vec2{15, 0};
        rig.world.add<ProjectileTag>(shot);
        rig.world.add<Projectile>(shot, projectile);
        rig.world.add<Transform>(shot, Transform{at, 0.0});
        rig.world.add<Motion>(shot, Motion{{330, 0}});
        rig.world.add<Body>(shot, Body{4, 1});
        rig.world.add<Health>(shot, Health{1, 1});
        rig.world.add<Faction>(shot, Faction{Team::Players, false});
        rig.combatTick(now);
        if (rig.world.isAlive(shot)) rig.world.destroy(shot);
    }
    const auto* status = rig.world.tryGet<Afflictions>(dummy);
    CHECK(status);
    if (status) CHECK_EQ(status->burns.size(), 2u);
    rig.combatTick(4150);
    double burn = 0.0;
    for (const auto& row : rig.combat.practiceDamage(rig.player, 4150, dummy)) {
        if (row.source == CombatSystem::PracticeSource::Burn) burn += row.amount;
    }
    CHECK_NEAR(burn, 3.0 * 6.0 * net::kTickSeconds, 1e-8);
    CHECK_EQ(rig.world.get<Health>(dummy).current, 1000.0);
}

TEST(damage_over_time_magic_fire_pays_for_each_stack_on_one_target) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const bool practice : {false, true}) {
        ArcticBlast rig("magic_fire", Rarity::Common, "mana_orb");
        rig.equip(2, "magic_fire");
        for (int now = 4050; now < 9000; now += 50) rig.tickPetals(now);
        CHECK_EQ(rig.liveIn(0).size(), 1u);
        CHECK_EQ(rig.liveIn(2).size(), 1u);
        const Vec2 at{5000, 5000};
        const Entity target = practice ? rig.dummy(at) : rig.mob(at);
        rig.world.get<Body>(target).radius = 240;
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        double paid = 0.0;
        for (int now = 9000; now < 9300; now += 50) {
            auto& before = rig.world.get<ManaPool>(rig.player);
            before.current = before.max;
            rig.tickPetals(now);
            const auto& after = rig.world.get<ManaPool>(rig.player);
            paid += after.max - after.current;
        }
        const auto* status = rig.world.tryGet<Afflictions>(target);
        CHECK(status);
        if (!status) continue;
        CHECK_EQ(status->burns.size(), 2u);
        for (const auto& stack : status->burns) {
            CHECK_EQ(stack.owner, rig.player);
            CHECK_NEAR(stack.perSecond, 9.0, 1e-8);
        }
        CHECK_NEAR(paid, 1.2, 1e-8);
        const double before = rig.world.get<Health>(target).current;
        rig.combatTick(9300);
        if (practice) {
            const auto rows = rig.combat.practiceDamage(rig.player, 9300, target);
            CHECK_EQ(rows.size(), 1u);
            if (!rows.empty()) {
                CHECK(rows.front().source == CombatSystem::PracticeSource::Burn);
                CHECK_NEAR(rows.front().amount, 18.0 * net::kTickSeconds, 1e-8);
            }
            CHECK_NEAR(rig.world.get<Health>(target).current, before, 1e-8);
        } else {
            CHECK_NEAR(before - rig.world.get<Health>(target).current,
                       18.0 * net::kTickSeconds, 1e-8);
        }
    }
}

TEST(arctic_cryo_bomb_freezes_nearby_mobs_and_leaves_distant_mobs_alone) {
    ArcticBlast rig("arctic_cryo_bomb");
    const auto centre=rig.armBomb();
    const auto near=rig.mob(centre+Vec2{30,0});
    const auto far=rig.mob(centre+Vec2{200,0});
    rig.combatTick(4050);
    const auto* chill=rig.world.tryGet<Afflictions>(near);
    CHECK(chill && chill->slowFactor < 1.0);
    CHECK(!rig.world.has<Afflictions>(far));
    rig.combatTick(4150);
    chill=rig.world.tryGet<Afflictions>(near);
    CHECK(!chill || chill->slowFactor == 1.0);
}

TEST(arctic_nuke_fires_main_burst_and_three_delayed_sub_bursts) {
    ArcticBlast rig("arctic_nuke");
    const auto centre=rig.armBomb();
    const auto near=rig.mob(centre+Vec2{30,0});
    const auto far=rig.mob(centre+Vec2{200,0});
    const auto overlap=rig.mob(centre);
    rig.world.get<Body>(overlap).radius = 30.0;
    const auto firstSub=rig.mob(centre+Vec2{85,0});
    const auto secondSub=rig.mob(centre+Vec2::fromAngle(kTau/3,85));
    const auto thirdSub=rig.mob(centre+Vec2::fromAngle(2*kTau/3,85));
    rig.combatTick(4050);
    const double health=rig.world.get<Health>(near).current;
    CHECK_NEAR(health,865,1e-6);
    CHECK_NEAR(rig.world.get<Health>(far).current,1000,1e-6);
    CHECK_NEAR(rig.world.get<Health>(firstSub).current,1000,1e-6);
    rig.combatTick(4050+net::kTickMillis);
    CHECK_NEAR(rig.world.get<Health>(firstSub).current,975,1e-6);
    CHECK_NEAR(rig.world.get<Health>(secondSub).current,1000,1e-6);
    rig.combatTick(4050+2*net::kTickMillis);
    CHECK_NEAR(rig.world.get<Health>(secondSub).current,975,1e-6);
    CHECK_NEAR(rig.world.get<Health>(thirdSub).current,1000,1e-6);
    rig.combatTick(4050+3*net::kTickMillis);
    CHECK_NEAR(rig.world.get<Health>(thirdSub).current,975,1e-6);
    CHECK_NEAR(rig.world.get<Health>(overlap).current,790,1e-6);
    CHECK_NEAR(rig.world.get<Health>(far).current,1000,1e-6);
}

TEST(arctic_nuke_higher_tiers_keep_one_damage_budget_per_detonation) {
    for (const auto rarity : {Rarity::Rare, Rarity::Super, Rarity::Apex, Rarity::Ascendant}) {
        ArcticBlast rig("arctic_nuke", rarity);
        const Vec2 centre = rig.armBomb();
        const auto target = rig.mob(centre);
        rig.world.get<Body>(target).radius = 200.0;
        const double health = 1e8;
        rig.world.get<Health>(target) = Health{health, health};

        const double expected = 210.0 * (isHyperPetal(rarity)
            ? 3.0 * petalStrengthScale(Rarity::Unique) * (rarity == Rarity::Ascendant ? 2.0 : 1.0)
            : petalStrengthScale(rarity));
        const int count = 3 + ladderIndex(rarity) / 2;
        for (int step = 0; step <= count; ++step) rig.combatTick(4050 + step * net::kTickMillis);
        CHECK_NEAR(health - rig.world.get<Health>(target).current, expected, 1e-6);
    }
}

TEST(arctic_splitter_stacks_rightward_and_collapses_after_removal) {
    ArcticBlast rig("basic");
    rig.equip(1, "arctic_splitter");
    rig.equip(2, "basic");
    rig.equip(3, "arctic_splitter", Rarity::Rare);
    rig.equip(4, "basic");
    for (double now=4050; now<=9000; now+=net::kTickMillis) rig.tickPetals(now);
    const auto p0=rig.liveIn(0), p2=rig.liveIn(2), p4=rig.liveIn(4);
    CHECK(p0.size()==1 && p2.size()==1 && p4.size()==1);
    if (p0.empty() || p2.empty() || p4.empty()) return;
    auto reach=[&](Entity e) {
        return (rig.world.get<Transform>(e).position-rig.world.get<Transform>(rig.player).position).length();
    };
    // The ring spring has the same steady-state radial gain at each radius.
    const double gain=reach(p0[0])/rig.world.get<PetalRing>(rig.player).radius;
    CHECK_NEAR((reach(p2[0])-reach(p0[0]))/gain,30.00847,0.1);
    CHECK_NEAR((reach(p4[0])-reach(p2[0]))/gain,32.05761,0.1);
    rig.world.get<Loadout>(rig.player).slots[1] = {};
    for (double now=9050; now<=11000; now+=net::kTickMillis) rig.tickPetals(now);
    CHECK_NEAR(reach(p2[0]),reach(p0[0]),0.1);
    CHECK_NEAR((reach(p4[0])-reach(p2[0]))/gain,32.05761,0.1);
}

TEST(magic_fang_with_and_without_an_equipped_mana_source) {
    for (const auto tier : {Rarity::Common, Rarity::Rare}) {
        for (const bool withOrb : {false, true}) {
            ArcticBlast rig("magic_fang", tier, withOrb ? "mana_orb" : nullptr);
            const auto id = rig.registry.petalIndex("mana_orb");
            const double supplied = withOrb ? rig.registry.petalStats(id, Rarity::Common).maxMana : 0.0;
            CHECK_NEAR(rig.world.get<ManaPool>(rig.player).max, kFlowerBaseMana + supplied, 1e-8);
            rig.world.get<ManaPool>(rig.player).current = 0.0;
            const auto bites = rig.liveIn(0);
            CHECK_EQ(bites.size(), std::size_t{1});
            if (bites.empty()) continue;
            const Entity mob = rig.mob(rig.world.get<Transform>(bites.front()).position);
            rig.combatTick(5000);
            CHECK(rig.world.get<Health>(mob).current < 1000.0);
            CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current, magicFangManaPerBite(tier), 1e-9);
        }
    }
}

TEST(magic_fang_refunds_mana_on_real_bites_against_training_dummies) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const char* target : {"target_dummy", "test_dummy"}) {
        ArcticBlast rig("magic_fang", Rarity::Rare, "mana_orb");
        const auto fangs = rig.liveIn(0);
        CHECK(!fangs.empty());
        if (fangs.empty()) return;
        const auto dummy = rig.dummy(rig.world.get<Transform>(fangs.front()).position);
        rig.world.get<Npc>(dummy).configIndex = rig.registry.mobIndex(target);
        auto& pool = rig.world.get<ManaPool>(rig.player);
        pool.current = 0;
        rig.combatTick(5000);
        CHECK_NEAR(pool.current, magicFangManaPerBite(Rarity::Rare), 1e-9);
        CHECK_NEAR(rig.world.get<Health>(dummy).current, 1000, 1e-9);
    }
}

TEST(magic_claw_converts_from_claw_and_scales_contact_with_current_mana) {
    ArcticBlast dry("basic");
    const auto claw = dry.registry.petalIndex("magic_claw");
    CHECK_EQ(dry.registry.magicFormOf(dry.registry.petalIndex("claw")), claw);
    dry.equip(0, "magic_claw");
    dry.world.get<ManaPool>(dry.player).current = 0.0;
    dry.tickPetals(4050);
    dry.tickPetals(9000);
    CHECK(dry.liveIn(0).empty());
    for (double fraction : {0.0, 0.5, 1.0}) {
        ArcticBlast rig("magic_claw", Rarity::Common, "mana_orb");
        const auto petals = rig.liveIn(0);
        CHECK_EQ(petals.size(), std::size_t{1});
        if (petals.empty()) return;
        auto& pool = rig.world.get<ManaPool>(rig.player);
        pool.current = pool.max * fraction;
        const auto mob = rig.mob(rig.world.get<Transform>(petals.front()).position);
        rig.combatTick(5000);
        CHECK_NEAR(1000 - rig.world.get<Health>(mob).current, 125 * fraction, 1e-6);
    }
}

TEST(magic_blood_scythe_formation_inflicts_real_bounded_self_damage) {
    for (const Rarity tier : {Rarity::Common, Rarity::Ascendant}) {
        ArcticBlast rig("basic");
        rig.equip(1, "magic_soil", tier);
        rig.equip(0, "magic_void_blood_scythe", tier);
        rig.tickPetals(4100);
        auto& mana = rig.world.get<ManaPool>(rig.player);
        mana.current = mana.max;
        constexpr double formedAt = 14000;
        rig.world.ensure<Afflictions>(rig.player).noHealUntilMillis = formedAt + 2000;
        rig.tickPetals(formedAt);
        CHECK_EQ(rig.liveIn(0).size(), 1u);
        const auto* status = rig.world.tryGet<Afflictions>(rig.player);
        CHECK(status && status->poisoned(formedAt) && !status->burns.empty());
        const double before = rig.world.get<Health>(rig.player).current;
        const double maxHealth = rig.world.get<Health>(rig.player).max;
        for (int tick = 0; tick < 40; ++tick) rig.combatTick(formedAt + 50 * tick);
        const double after = rig.world.get<Health>(rig.player).current;
        CHECK(after < before);
        CHECK_NEAR(before - after, std::min(9.0 * referenceOffenseScale(
            rig.registry.petal(rig.registry.petalIndex("magic_void_blood_scythe")), tier),
            maxHealth * .025), 1e-6);
        rig.combatTick(formedAt + 2000);
        CHECK_NEAR(rig.world.get<Health>(rig.player).current, after, 1e-6);
    }
}

TEST(healing_cuts_independent_burns_without_overwriting_an_active_legacy_burn) {
    for (double healing : {2.0, 6.0}) {
        ArcticBlast rig("basic");
        const auto target = rig.mob({5500, 5000});
        auto& status = rig.world.add<Afflictions>(target);
        status.burns.push_back({rig.player, rig.player, 5.0, 7000.0});
        status.burnPerSecond = 4.0;
        status.burnUntilMillis = 7000.0;
        status.burnSource = rig.player;
        auto& health = rig.world.get<Health>(target);
        health.current = 900.0;
        CombatSystem::landHeal(rig.world, target, health, healing);
        CHECK_NEAR(status.burns.front().perSecond, std::max(0.0, 5.0 - healing), 1e-8);
        CHECK_NEAR(status.burnPerSecond, 4.0 - std::max(0.0, healing - 5.0), 1e-8);
        CHECK_NEAR(health.current, 900.0, 1e-8);
        rig.combatTick(5000);
        CHECK_NEAR(rig.world.get<Health>(target).current, 900.0 - (9.0 - healing) * net::kTickSeconds, 1e-8);
    }
}

TEST(magic_scythe_spends_only_the_mana_that_becomes_contact_damage) {
    for (bool expandedPool : {false, true}) {
        ArcticBlast rig("basic");
        if (expandedPool) rig.equip(1, "magic_soil", Rarity::Unique);
        rig.tickPetals(4050);
        rig.equip(0, "magic_void_scythe");
        rig.tickPetals(4100);
        auto& pool = rig.world.get<ManaPool>(rig.player);
        const double maxMana = pool.max;
        pool.current = maxMana;
        rig.tickPetals(16000);
        const auto petals = rig.liveIn(0);
        CHECK_EQ(petals.size(), std::size_t{1});
        if (petals.empty()) return;

        const double expectedCost = expandedPool ? 21.25 : maxMana * .25;
        const auto& mods = rig.world.get<PlayerModifiers>(rig.player);
        const double regeneration = mods.passiveManaPerSecond * mods.magicRegenScale * net::kTickSeconds;
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current,
                   maxMana - 1 - expectedCost + regeneration, 1e-8);

        const Vec2 strike = rig.world.get<Transform>(petals.front()).position;
        for (const Entity other : rig.liveIn(1))
            rig.world.get<Transform>(other).position += Vec2{500, 0};
        const Entity mob = rig.mob(strike);
        rig.combatTick(16000);
        CHECK_NEAR(1000 - rig.world.get<Health>(mob).current, 12 + 4 * expectedCost, 1e-6);
    }
}

TEST(magic_variant_ruby_concentrates_actual_bomb_damage_on_one_target) {
    const auto blast = [](bool focus) {
        ArcticBlast rig("magic_bomb", Rarity::Common, "mana_orb");
        if (focus) rig.equip(2,"magic_ruby");
        rig.tickPetals(4050);
        rig.tickPetals(14000);
        if (focus) CHECK(rig.world.get<PlayerModifiers>(rig.player).concentratesBlasts);
        const auto at = rig.armBomb(14050);
        std::array<Entity,3> targets{};
        for (int i=0;i<3;++i) targets[i]=rig.mob(at+Vec2{25.0+i*4,0});
        rig.combatTick(14100);
        double total=0; int touched=0;
        for(Entity e:targets) {
            const double damage=1000-rig.world.get<Health>(e).current;
            total+=damage;
            if(damage>0) ++touched;
        }
        return std::make_pair(total,touched);
    };
    const auto normal=blast(false), focused=blast(true);
    CHECK_EQ(normal.second,3);
    CHECK_EQ(focused.second,1);
    CHECK_NEAR(normal.first,focused.first,1e-6);
}
TEST(magic_variant_relic_splits_a_formation_cost_and_sponge_repays_after_unequip) {
    ArcticBlast rig("magic_mysterious_relic",Rarity::Uncommon,"magic_cactus");
    const auto peer=rig.world.create();
    rig.world.add<Transform>(peer,rig.world.get<Transform>(rig.player));
    rig.world.add<Health>(peer,Health{100,100});
    rig.world.add<ManaPool>(peer,ManaPool{30,30});
    rig.world.add<PlayerModifiers>(peer).sharesMana=true;
    SquadEntityIndex squad;
    squad.group[rig.player]=squad.group[peer]=0;
    squad.groups.push_back({{rig.player,1},{peer,2}});
    rig.petals.squads=&squad;
    rig.equip(2,"magic_sponge");
    rig.tickPetals(8000);
    rig.tickPetals(15000);
    CHECK(rig.world.get<PlayerModifiers>(rig.player).sharesMana);
    CHECK(rig.world.get<PlayerModifiers>(rig.player).magicSponge);
    auto& mana=rig.world.get<ManaPool>(rig.player);
    mana.current=mana.max;
    const double before=mana.current;
    const double peerBefore=rig.world.get<ManaPool>(peer).current;
    rig.equip(3,"magic_claw");
    rig.tickPetals(24000);
    rig.tickPetals(36000);
    CHECK(!rig.liveIn(3).empty());
    const double cost=rig.registry.petalStats(rig.registry.petalIndex("magic_claw"),Rarity::Common).requiredMana;
    CHECK_NEAR(peerBefore-rig.world.get<ManaPool>(peer).current,cost*.5,1e-8);
    CHECK(mana.deferred > 0 && mana.deferred <= cost*.25);
    CHECK_NEAR(before - mana.current + mana.deferred, cost*.5,1e-8);
    rig.world.get<Loadout>(rig.player).slots[2] = {};
    for(int tick=0;tick<180;++tick) rig.tickPetals(36100+tick*net::kTickSeconds*1000);
    CHECK_NEAR(mana.deferred,0,1e-8);
    CHECK_NEAR(before - mana.current,cost*.5,1e-8);
}

TEST(magic_variant_relic_shares_cotton_absorption_and_incense_shields_the_payer) {
    std::string error; CHECK(loadContent(testsupport::dataDir(),error));
    ArcticBlast rig("magic_cotton",Rarity::Common,"magic_cactus");
    rig.equip(2,"magic_mysterious_relic",Rarity::Uncommon);
    rig.equip(3,"magic_agarbatti",Rarity::Uncommon);
    rig.world.ensure<ShieldState>(rig.player);
    rig.tickPetals(16000);
    rig.tickPetals(24000);
    CHECK(!rig.liveIn(0).empty());
    rig.world.get<ShieldState>(rig.player).amount=0;
    auto& mana=rig.world.get<ManaPool>(rig.player);
    mana.current=mana.max;
    const double before=mana.current;
    const double shieldShare=rig.world.get<PlayerModifiers>(rig.player).manaShieldFraction;
    CHECK(shieldShare>0);
    const auto peer=rig.world.create();
    rig.world.add<Transform>(peer,rig.world.get<Transform>(rig.player));
    rig.world.add<Health>(peer,Health{100,100});
    rig.world.add<ManaPool>(peer,ManaPool{30,30});
    rig.world.add<PlayerModifiers>(peer).sharesMana=true;
    SquadEntityIndex group;
    group.group[peer]=group.group[rig.player]=0;
    group.groups.push_back({{peer,2},{rig.player,1}});
    rig.combat.squads=&group;
    const auto foe=rig.mob({5200,5000});
    const double hit=4.0*magicCottonDamagePerMana(Rarity::Common);
    CHECK_NEAR(rig.combat.applyDamage(rig.world,rig.player,foe,hit,25000).applied,0,1e-9);
    CHECK_NEAR(before-mana.current,2,1e-8);
    CHECK_NEAR(rig.world.get<ManaPool>(peer).current,28,1e-8);
    CHECK_NEAR(rig.world.get<ShieldState>(rig.player).amount,2*shieldShare,1e-8);
}

TEST(magic_variant_nazar_blocks_two_actual_pet_hits_then_reloads) {
    std::string error; CHECK(loadContent(testsupport::dataDir(),error));
    ArcticBlast rig("magic_nazar_amulet", Rarity::Common, "mana_orb");
    rig.tickPetals(15000);
    CHECK_EQ(rig.liveIn(0).size(),1u);
    const Entity pet=rig.mob({5200,5000}), foe=rig.mob({5250,5000});
    rig.world.get<Faction>(pet).team=Team::Players;
    rig.world.add<Pet>(pet,Pet{rig.player});
    for(int i=0;i<2;++i)
        CHECK(rig.combat.applyDamage(rig.world,pet,foe,10,15100+i*100).refused);
    CHECK_NEAR(rig.world.get<Health>(pet).current,1000,1e-6);
    CHECK(rig.combat.applyDamage(rig.world,pet,foe,10,15500).applied>0);
    rig.tickPetals(15550);
    CHECK(rig.world.get<Loadout>(rig.player).slots[0].broken);
}
TEST(magic_variant_coral_and_adjacent_fission_emit_bounded_shards) {
    const auto split = [](bool coral, Rarity other) {
        ArcticBlast rig(coral?"magic_coral":"magic_arctic_fission",Rarity::Common,"mana_orb");
        const int slot = coral?0:1;
        if (!coral) {
            rig.equip(2,"mana_orb");
            rig.equip(1,"basic",other);
            rig.tickPetals(5000);
            rig.tickPetals(12000);
        }
        const auto live=rig.liveIn(slot);
        CHECK(!live.empty());
        if (live.empty()) return 0;
        rig.world.get<Health>(live.front()).current=0;
        rig.world.add<Dead>(live.front());
        rig.tickPetals(12050);
        std::vector<Entity> first;
        Query<Projectile>{rig.world}.each([&](Entity e, Projectile& shot){
            if(shot.fragmented && shot.owner==rig.player && shot.damage>0) first.push_back(e);
        });
        for(Entity e:first) rig.world.add<Dead>(e);
        rig.combatTick(12100);
        int children=0;
        Query<Projectile>{rig.world}.each([&](Entity e, Projectile& shot){
            if(shot.fragmented && !rig.world.has<Dead>(e) && shot.owner==rig.player) ++children;
        });
        return static_cast<int>(first.size()) + children;
    };
    CHECK_EQ(split(true,Rarity::Common),10);
    CHECK_EQ(split(false,Rarity::Common),0);
    CHECK_EQ(split(false,Rarity::Rare),0);
}

TEST(magic_fission_rebuilds_the_actual_right_hand_petal_before_reload) {
    for (const char* id : {"basic", "rose"}) {
        ArcticBlast rig("magic_arctic_fission", Rarity::Common, "mana_orb");
        rig.equip(2, "mana_orb");
        rig.equip(1, id);
        rig.tickPetals(5000);
        rig.tickPetals(12000);
        const auto parents = rig.liveIn(1);
        CHECK_EQ(parents.size(), 1u);
        if (parents.empty()) continue;
        rig.world.get<Health>(parents.front()).current = 0;
        rig.tickPetals(12050);
        rig.tickPetals(12100);
        const auto children = rig.liveIn(1);
        CHECK_EQ(children.size(), 2u);
        for (auto e : children)
            CHECK_EQ(rig.world.get<PetalInstance>(e).configIndex, rig.registry.petalIndex(id));
        if (children.size() != 2) continue;
        rig.world.get<Health>(children.front()).current = 0;
        rig.tickPetals(12150);
        CHECK_EQ(rig.liveIn(1).size(), 1u);
        rig.world.get<Health>(children.back()).current = 0;
        rig.tickPetals(12200);
        rig.tickPetals(12250);
        const auto last = rig.liveIn(1);
        CHECK_EQ(last.size(), 2u);
        for (auto e : last) rig.world.get<Health>(e).current = 0;
        rig.tickPetals(12300);
        rig.tickPetals(12350);
        CHECK(rig.liveIn(1).empty());
        rig.tickPetals(20000);
        CHECK_EQ(rig.liveIn(1).size(), 1u);
    }
}

TEST(magic_variant_mimic_uses_left_neighbor_without_recursing_or_creating_items) {
    ArcticBlast rig("magic_mimic");
    const int left = rig.world.get<Loadout>(rig.player).activeSlots - 1;
    rig.equip(1, "fission");
    rig.equip(left,"rose", Rarity::Rare);
    rig.tickPetals(8000);
    const auto& loadout = rig.world.get<Loadout>(rig.player);
    const auto copied = equippedPetal(rig.registry, loadout, 0);
    CHECK_EQ(copied.configIndex, rig.registry.petalIndex("mana_orb"));
    CHECK_EQ(copied.rarity, Rarity::Common);
    CHECK_EQ(copied.reloadRarity, Rarity::Rare);
    CHECK_EQ(equippedPetalStats(rig.registry, copied).reloadMillis,
             rig.registry.petalStats(copied.configIndex, Rarity::Rare).reloadMillis);
    CHECK_EQ(loadout.slots[0].configIndex, rig.registry.petalIndex("magic_mimic"));
    CHECK(rig.world.get<ManaPool>(rig.player).has());
    rig.equip(left, "claw");
    CHECK_EQ(equippedPetal(rig.registry, loadout, 0).configIndex, rig.registry.petalIndex("magic_claw"));
    rig.equip(left, "magic_claw", Rarity::Rare);
    CHECK_EQ(equippedPetal(rig.registry, loadout, 0).configIndex, rig.registry.petalIndex("magic_claw"));
    CHECK_EQ(equippedPetal(rig.registry, loadout, 0).rarity, Rarity::Common);
    rig.equip(left, "magic_mimic");
    CHECK_EQ(equippedPetal(rig.registry, loadout, 0).configIndex, rig.registry.petalIndex("magic_mimic"));
    rig.world.get<Loadout>(rig.player).slots[left] = {};
    CHECK_EQ(equippedPetal(rig.registry, loadout, 0).configIndex, rig.registry.petalIndex("magic_mimic"));

    rig.tickPetals(8500);

    // Compare the server's actual timer to equipping the copied form at the source tier.
    ArcticBlast direct("basic");
    direct.equip(0, "mana_orb", Rarity::Rare);
    rig.equip(left, "rose", Rarity::Rare);
    rig.tickPetals(9000);
    direct.tickPetals(9000);
    const double readyAt = direct.world.get<Loadout>(direct.player).slots[0].reloadReadyAtMillis;
    CHECK(readyAt > 9000);
    CHECK_NEAR(rig.world.get<Loadout>(rig.player).slots[0].reloadReadyAtMillis, readyAt, 1e-8);
    rig.tickPetals(readyAt - 1.0);
    CHECK(rig.liveIn(0).empty());
    rig.tickPetals(readyAt);
    const auto live = rig.liveIn(0);
    CHECK_EQ(live.size(), 1u);
    if (!live.empty())
        CHECK_EQ(rig.world.get<PetalInstance>(live.front()).rarity, Rarity::Common);
}

TEST(magic_variant_battery_pays_once_after_maturing_and_respects_pool_cap) {
    ArcticBlast rig("magic_battery", Rarity::Common, "magic_cactus");
    auto& pool = rig.world.get<ManaPool>(rig.player);
    pool.current = 0;
    rig.tickPetals(6999);
    CHECK_NEAR(pool.current, 0, 1e-6);
    rig.tickPetals(7000);
    CHECK_NEAR(pool.current, 40.0, 1e-6);
    rig.tickPetals(7050);
    CHECK_NEAR(pool.current, 40.0, 1e-6);
}

TEST(magic_variant_bombs_burst_on_break_with_their_element_and_cost) {
    for (const char* id : {"magic_bomb","magic_void_cryo_bomb","magic_gas_bomb","magic_void_fire_bomb"}) {
        ArcticBlast dry(id);
        dry.world.get<ManaPool>(dry.player).current = 0.0;
        dry.world.get<Loadout>(dry.player).slots[0] = {};
        dry.tickPetals(4050);
        dry.equip(0, id);
        dry.tickPetals(4100);
        dry.tickPetals(9000);
        CHECK(dry.liveIn(0).empty());
        ArcticBlast rig(id, Rarity::Common, "mana_orb");
        CHECK_EQ(rig.liveIn(0).size(), 1u);
        const auto at = rig.armBomb();
        const Entity target = rig.mob(at + Vec2{30,0});
        rig.combatTick(4100);
        CHECK(rig.world.get<Health>(target).current < 1000.0);
        const auto* status = rig.world.tryGet<Afflictions>(target);
        if (std::string_view(id) == "magic_void_cryo_bomb") CHECK(status && status->slowFactor < 1.0 && status->slowUntilMillis >= 4250);
        if (std::string_view(id) == "magic_gas_bomb") CHECK(status && status->poisoned(4100));
        if (std::string_view(id) == "magic_void_fire_bomb") CHECK(status && !status->burns.empty());
    }
}

TEST(magic_fang_tooltip_shows_mana_per_successful_bite) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    const auto id = content().petalIndex("magic_fang");
    const auto hasHint = [&](ui::Language language, const char* expected) {
        ui::setLanguage(language);
        const auto lines = ui::petalTooltipLines(id, Rarity::Common, {}, 1.0);
        return std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
            return line.text == expected;
        });
    };
    const auto previous = ui::language();
    CHECK(hasHint(ui::Language::En, "Mana per damaging wild mob / dummy bite: 1.2"));
    CHECK(hasHint(ui::Language::Zh, "有效咬中野怪／训练木桩回魔：1.2"));
    ui::setLanguage(previous);
}

TEST(arctic_fission_splits_only_the_eligible_right_slot) {
    ArcticBlast rig("arctic_fission");
    rig.equip(1, "basic");
    rig.equip(2, "basic");
    rig.tickPetals(4050);
    rig.tickPetals(8000);
    const auto split=rig.liveIn(1), ordinary=rig.liveIn(2);
    CHECK(split.size()==3 && ordinary.size()==1);
    if (split.size()!=3 || ordinary.empty()) return;
    const auto& health=rig.world.get<Health>(ordinary[0]);
    const auto& damage=rig.world.get<ContactDamage>(ordinary[0]);
    for (const Entity e:split) {
        CHECK_NEAR(rig.world.get<Health>(e).max,health.max/3,1e-6);
        CHECK_NEAR(rig.world.get<ContactDamage>(e).amount,damage.amount/3,1e-6);
    }
    rig.equip(1,"basic",Rarity::Rare);
    rig.tickPetals(8100);
    rig.tickPetals(13000);
    CHECK(rig.liveIn(1).size()==1);
}

TEST(arctic_fusion_combines_three_and_restores_separate_slots_after_swap) {
    ArcticBlast rig("arctic_fusion");
    for (int i=1; i<=4; ++i) rig.equip(i,"basic");
    rig.tickPetals(4050);
    rig.tickPetals(8000);
    const auto fused=rig.liveIn(1), ordinary=rig.liveIn(4);
    CHECK(fused.size()==1 && ordinary.size()==1);
    CHECK(rig.liveIn(2).empty() && rig.liveIn(3).empty());
    if (fused.empty() || ordinary.empty()) return;
    CHECK_NEAR(rig.world.get<Health>(fused[0]).max,3*rig.world.get<Health>(ordinary[0]).max,1e-6);
    CHECK_NEAR(rig.world.get<ContactDamage>(fused[0]).amount,3*rig.world.get<ContactDamage>(ordinary[0]).amount,1e-6);
    rig.equip(0,"basic");
    rig.tickPetals(8100);
    rig.tickPetals(13000);
    CHECK(rig.liveIn(2).size()==1 && rig.liveIn(3).size()==1);
}

TEST(arctic_plutonium_burn_lingers_and_accelerates_after_leaving_cloud) {
    ArcticBlast rig("arctic_plutonium");
    rig.tickPetals(6000);
    const auto active=rig.liveIn(0);
    CHECK(active.size()==1);
    if (active.empty()) return;
    const auto centre=rig.world.get<Transform>(active[0]).position;
    const auto near=rig.mob(centre+Vec2{425,0});
    const auto outside=rig.mob(centre+Vec2{560,0});
    rig.combatTick(6000);
    CHECK(rig.world.has<Burn>(near));
    if (const auto* burn = rig.world.tryGet<Burn>(near)) CHECK_NEAR(burn->damage, 245.0, 1e-6);
    CHECK(!rig.world.has<Burn>(outside));
    CHECK(!rig.world.has<Afflictions>(near));
    rig.world.get<Transform>(near).position += Vec2{700,0};
    rig.combatTick(6500);
    const double health=rig.world.get<Health>(near).current;
    rig.combatTick(7000);
    CHECK(1000-health > 0);
    CHECK(health-rig.world.get<Health>(near).current > 1000-health);
    const double beforeExpiry=rig.world.get<Health>(near).current;
    rig.combatTick(9100);
    CHECK_NEAR(rig.world.get<Health>(near).current,beforeExpiry,1e-6);
}

TEST(arctic_nuke_kill_and_pickup_use_the_arctic_splitter_drop) {
    ArcticBlast rig("basic");
    LootSystem loot;
    std::string error;
    const bool loaded=loot.loadTables(rig.registry,testsupport::dataFile("mob_drops.json"),error);
    CHECK(loaded);
    if (!loaded) return;
    Rng rng(1447);
    rig.world.add<PlayerAccount>(rig.player, PlayerAccount{"arctic-test", "arctic-test", 77, false});
    rig.world.get<PlayerModifiers>(rig.player).magnetism = 100.0;
    const auto nuke=rig.registry.mobIndex("arctic_nuke");
    const auto splitter=rig.registry.petalIndex("arctic_splitter");
    const auto legacy=rig.registry.petalIndex("splitter");
    CHECK(nuke!=kInvalidIndex && splitter!=kInvalidIndex && legacy!=splitter);
    int pickedUp=0;
    int anyPickup=0;
    int oldItem=0;
    loot.onPickup=[&](const LootSystem::Pickup& item) {
        ++anyPickup;
        if (item.player==rig.player && item.petalIndex==splitter) pickedUp+=item.quantity;
        if (item.petalIndex==legacy) oldItem+=item.quantity;
    };
    const auto position=rig.world.get<Transform>(rig.player).position;
    for (int i=0; i<300 && pickedUp==0; ++i) {
        const auto corpse=rig.mob(position);
        rig.world.add<MobType>(corpse,MobType{nuke,Rarity::Super});
        const double now=5000+net::kTickMillis*i;
        const auto hit=rig.combat.applyDamage(rig.world,corpse,rig.player,10000,now);
        CHECK(hit.killed);
        CHECK(!rig.world.get<Bounty>(corpse).contributors.empty());
        loot.run(rig.world,rig.grid,rig.registry,rng,now,net::kTickSeconds,rig.commands,rig.events);
        rig.commands.flush();
        rig.world.destroy(corpse);
    }
    CHECK(anyPickup>0);
    CHECK(pickedUp>0);
    CHECK(oldItem==0);
}

TEST(arctic_names_descriptions_and_notice_follow_the_language_setting) {
    const auto& content = testsupport::shippedContent();
    const auto oldLanguage = ui::language();
    const std::string notice = "Frozen Mark: sent to Arctic. Use /mark-return to go back.";
    ui::setLanguage(ui::Language::Zh);
    int petals = 0;
    for (std::uint16_t i = 0; i < content.petalCount(); ++i) {
        const auto& petal = content.petal(i);
        if (petal.id.rfind("arctic_", 0) != 0) continue;
        CHECK(std::string(ui::petalNameTr(petal.id, petal.name.c_str())) != petal.name);
        CHECK(std::string(ui::petalDescTr(petal.id, petal.description.c_str())) != petal.description);
        ++petals;
    }
    CHECK_EQ(petals, 43);
    int mobs = 0;
    for (std::uint16_t i = 0; i < content.mobCount(); ++i) {
        const auto& mob = content.mob(i);
        if (mob.id.rfind("arctic_", 0) != 0) continue;
        CHECK(std::string(ui::mobNameTr(mob.id, mob.name.c_str())) != mob.name);
        CHECK(std::string(ui::mobDescTr(mob.id, mob.description.c_str())) != mob.description);
        ++mobs;
    }
    CHECK_EQ(mobs, 17);
    CHECK(ui::localizeChatText(notice) != notice);
    CHECK(std::string(ui::biomeTr("zorr_arctic", "Arctic")) != "Arctic");
    ui::setLanguage(ui::Language::En);
    CHECK_EQ(ui::localizeChatText(notice), notice);
    CHECK_EQ(std::string(ui::biomeTr("zorr_arctic", "Arctic")), "Arctic");
    const auto& nuke = content.petal(content.petalIndex("arctic_nuke"));
    CHECK_EQ(std::string(ui::petalNameTr(nuke.id, nuke.name.c_str())), "Nuke");
    CHECK_EQ(std::string(ui::petalDescTr(nuke.id, nuke.description.c_str())),
             "Detonates on contact or break, then triggers several blasts around the first.");
    ui::setLanguage(oldLanguage);
}

TEST(arctic_eggs_match_original_species_count_tier_and_hatch_delay) {
    // Zorr's public client egg definitions. Omega and Unique occupy AshFlor's
    // Eternal and Hyper rungs in the same order.
    struct Egg {
        const char* id;
        const char* mob;
        int count;
        std::array<int, kLadderRarityCount> tiers;
        std::array<int, kLadderRarityCount> hatchMillis;
    };
    constexpr Egg eggs[] = {
        {"arctic_centipede_egg", "arctic_centipede", 1, {0, 1, 2, 2, 3, 4, 5, 6, 7, 9}, {5000, 10000, 15000, 2000, 3000, 4000, 5000, 6000, 15000, 25000}},
        {"arctic_spider_cave_egg", "arctic_spider", 10, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {17500, 20000, 23000, 26000, 30000, 5000, 7000, 11000, 16000, 75000}},
        {"arctic_spider_egg", "arctic_spider", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {8000, 10000, 12000, 14000, 17500, 3000, 5000, 7500, 12500, 40000}},
        {"arctic_frost_digger_egg", "arctic_frost_digger", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {6000, 7000, 8000, 9000, 10000, 3000, 6000, 9000, 12000, 46250}},
        {"arctic_ice_cube_egg", "arctic_ice_cube", 1, {0, 1, 2, 3, 4, 5, 5, 6, 7, 9}, {3000, 4500, 6000, 8000, 10000, 14000, 4000, 7500, 12500, 35000}},
        {"arctic_ice_dragon_egg", "arctic_ice_dragon", 1, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {7000, 9000, 11000, 13000, 3000, 4500, 6000, 8500, 15000, 45000}},
        {"arctic_ice_hornet_egg", "arctic_ice_hornet", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {6000, 7000, 9000, 11000, 14000, 3000, 5500, 10000, 12500, 40000}},
        {"arctic_igloo_egg", "arctic_snowman", 10, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {10000, 15000, 20000, 25000, 30000, 10000, 12000, 14000, 30000, 95000}},
        {"arctic_nuke_egg", "arctic_nuke", 2, {0, 1, 2, 3, 4, 5, 5, 6, 7, 9}, {3800, 4500, 5300, 6000, 6800, 7500, 3400, 6000, 9000, 19450}},
        {"arctic_snowbush_egg", "arctic_snowbush", 1, {0, 1, 2, 3, 4, 5, 5, 6, 7, 9}, {5000, 6000, 7000, 8000, 9000, 10000, 2000, 5000, 11250, 25000}},
        {"arctic_snowman_egg", "arctic_snowman", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {2000, 3000, 5000, 8000, 13000, 2000, 4000, 8000, 16000, 40000}},
        {"arctic_spider_digger_egg", "arctic_spider_digger", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {4000, 4500, 5000, 5500, 6500, 3000, 5000, 7000, 13000, 37500}},
        {"arctic_spider_yoba_egg", "arctic_spider_yoba", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {3000, 4000, 5500, 7000, 8500, 3500, 5500, 7500, 15000, 40000}},
        {"arctic_stalagmite_egg", "arctic_stalagmite", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {1750, 2250, 3000, 4000, 6000, 3000, 3500, 4000, 7000, 27500}},
        {"arctic_tick_egg", "arctic_tick", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 9}, {5000, 7000, 10000, 13000, 15000, 1500, 2500, 5000, 12500, 40000}},
    };
    for (const auto& egg : eggs) {
        for (int tier = 0; tier < kLadderRarityCount; ++tier) {
            const auto rarity = static_cast<Rarity>(tier);
            ArcticBlast rig(egg.id, rarity);
            const auto species = rig.registry.mobIndex(egg.mob);
            const double hatchAt = 4000 + egg.hatchMillis[tier];
            const auto countHatchlings = [&] {
                int count = 0;
                Query<Pet, MobType>{rig.world}.without<Dead>().each(
                    [&](Entity, Pet& pet, MobType& mob) {
                        if (pet.owner != rig.player || mob.configIndex != species) return;
                        CHECK_EQ(ladderIndex(mob.rarity), egg.tiers[tier]);
                        ++count;
                    });
                return count;
            };
            rig.tickPetals(hatchAt - 1);
            CHECK_EQ(countHatchlings(), 0);
            rig.tickPetals(hatchAt);
            CHECK_EQ(countHatchlings(), tier == 9 && std::string(egg.id) != "arctic_ice_dragon_egg" ? std::max(3, egg.count) : egg.count);
            rig.tickPetals(hatchAt + egg.hatchMillis[tier]);
            CHECK_EQ(countHatchlings(), tier == 9 && std::string(egg.id) != "arctic_ice_dragon_egg" ? std::max(3, egg.count) : egg.count);
            rig.world.get<Loadout>(rig.player).slots[0] = {};
            rig.tickPetals(hatchAt + 2 * egg.hatchMillis[tier]);
            CHECK_EQ(countHatchlings(), 0);
        }
    }
}

TEST(arctic_permanent_hyper_egg_incubation_respects_reload_talent) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const char* egg : {"arctic_tick_egg", "arctic_spider_yoba_egg"}) {
        ArcticBlast rig("basic");
        auto& skills = rig.world.add<PlayerSkillTree>(rig.player).skills;
        skills.set(SkillId::Reload, 6);
        const double scale = skills.reloadScale();
        rig.equip(0, egg, Rarity::PermanentHyper);
        constexpr double equippedAt = 4100.0;
        constexpr double shellReadyAt = equippedAt + 1000.0;
        rig.tickPetals(equippedAt);
        rig.tickPetals(shellReadyAt);
        const auto index = rig.registry.petalIndex(egg);
        const double incubation =
            rig.registry.petalStats(index, Rarity::PermanentHyper).reloadMillis;
        CHECK_NEAR(incubation, 40000.0, 1e-6);
        const auto priorLanguage = ui::language();
        ui::setLanguage(ui::Language::En);
        const auto lines = ui::petalTooltipLines(index, Rarity::PermanentHyper, skills, scale);
        ui::setLanguage(priorLanguage);
        const std::string label = "Hatch delay: ";
        const auto delay = std::find_if(lines.begin(), lines.end(), [&](const auto& line) {
            return line.text.compare(0, label.size(), label) == 0;
        });
        CHECK(delay != lines.end());
        if (delay != lines.end()) {
            const std::string seconds = delay->text.substr(label.size());
            std::size_t end = 0;
            const double displayed = std::stod(seconds, &end);
            CHECK_EQ(seconds.substr(end), std::string("s"));
            CHECK_NEAR(displayed, incubation * scale / 1000.0, 0.000001);
        }
        const auto livingPets = [&] { return Query<Pet>{rig.world}.without<Dead>().collect(); };
        const double firstHatch = shellReadyAt + incubation * scale;
        rig.tickPetals(firstHatch - 1.0);
        CHECK(livingPets().empty());
        rig.tickPetals(firstHatch);
        const auto pets = livingPets();
        CHECK_EQ(pets.size(), std::size_t(3));
        for (Entity pet : pets) rig.world.add<Dead>(pet);
        const double nextHatch = firstHatch + incubation * scale;
        rig.tickPetals(nextHatch - 1.0);
        CHECK(livingPets().empty());
        rig.tickPetals(nextHatch);
        CHECK_EQ(livingPets().size(), std::size_t(3));
    }
}

TEST(zorr_soap_expands_liquid_fields_without_scaling_damage) {
    ArcticBlast rig("basic");
    const auto& acid = rig.registry.petal(rig.registry.petalIndex("acid")).spill;
    double now = 4050;
    const auto sample = [&](double growthScale, double coverage = 1.0, double reach = 1.0) {
        rig.petals.foldModifiers(rig.world, rig.registry);
        CombatSystem::createSpill(rig.world, rig.player, {5500, 5500}, Rarity::Common, acid, 1.0, 0);
        auto fields = Query<GroundEffect>{rig.world}.collect();
        CHECK_EQ(fields.size(), 1u);
        if (fields.empty()) return;
        const Entity pool = fields.front();
        const double landingRadius = 94.875 * reach;
        CHECK_NEAR(rig.world.get<GroundEffect>(pool).radius, landingRadius * 0.24, 1e-6);
        for (int second = 1; second <= 2; ++second) {
            for (int tick = 0; tick < net::kTicksPerSecond; ++tick) {
                now += net::kTickMillis;
                rig.combatTick(now);
            }
            const auto& field = rig.world.get<GroundEffect>(pool);
            CHECK_NEAR(field.spreadTargetRadius, landingRadius * coverage, 1e-6);
            CHECK_NEAR(field.radialGrowthPerSecond, 11.0 * growthScale * reach, 1e-6);
            CHECK_NEAR(field.radius, landingRadius * coverage - std::max(0.0, 5.5 - second * 11.0 * growthScale) * reach, 1e-6);
            CHECK_NEAR(field.damagePerSecond, 34.3, 1e-6);
        }
        rig.world.destroy(pool);
    };
    sample(1.0);
    rig.equip(1, "void_soap");
    sample(1.10, 1.30);
    rig.equip(2, "void_soap", Rarity::Rare);
    sample(1.248, 1.78);
    rig.equip(3, "void_soap", Rarity::Super);
    sample(1.936, 2.56);
    auto& slots = rig.world.get<Loadout>(rig.player).slots;
    slots[1] = slots[2] = slots[3] = {};
    sample(1.0);
    rig.equip(1, "void_soap", Rarity::Super);
    sample(1.688, 1.93);
    rig.equip(1, "void_soap", Rarity::Unique);
    sample(1.868, 2.02);
    for (auto rarity : {Rarity::Apex, Rarity::PermanentHyper, Rarity::Ascendant}) {
        rig.equip(1, "void_soap", rarity);
        sample(2.072, rarity == Rarity::Ascendant ? 2.20 : 2.11);
    }
    auto& skills = rig.world.add<PlayerSkillTree>(rig.player).skills;
    for (int level = 0; level < 3; ++level) {
        skills.set(SkillId::FluidReach, level);
        sample(2.072, 2.20, 1.0 + 0.1 * (level + 1));
    }
    skills.set(SkillId::FluidReach, -1);
    sample(2.072, 2.20);
}

TEST(arctic_spider_cave_egg_apex_hatches_ten_hyper_spiders) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    constexpr const char* id = "arctic_spider_cave_egg";
    for (const auto eggRarity : {Rarity::Apex, Rarity::Ascendant}) {
        ArcticBlast rig(id, eggRarity);
        const auto eggIndex = rig.registry.petalIndex(id);
        const auto& egg = rig.registry.petal(eggIndex);
        const bool apexEgg = eggRarity == Rarity::Ascendant;
        const auto expectedTier = Rarity::Apex;
        const double hatchAt = 4000.0 + rig.registry.petalStats(eggIndex, eggRarity).reloadMillis;
        const auto& spider = rig.registry.mob(egg.petMobIndex);
        const double baselineScale = petalStatScale(Rarity::Unique) * (apexEgg ? 1.0 : 1.1) * hyperSummonScale(egg, eggRarity)
                                   * eggSummonCombatScale(egg);

        rig.tickPetals(hatchAt - 1);
        CHECK(Query<Pet>(rig.world).collect().empty());
        rig.tickPetals(hatchAt);
        const auto pets = Query<Pet>(rig.world).collect();
        CHECK_EQ(pets.size(), std::size_t(10));
        for (const Entity pet : pets) {
            const auto& mob = rig.world.get<MobType>(pet);
            CHECK_EQ(mob.configIndex, egg.petMobIndex);
            CHECK_EQ(mob.rarity, expectedTier);
            CHECK_EQ(rig.world.get<Replicated>(pet).rarity, expectedTier);
            CHECK_NEAR(rig.world.get<Health>(pet).max,
                       spider.health * spider.petHealthScale * baselineScale * kSummonedMobHealthScale
                           * eggSummonHealthScale(egg) * ascendantSummonHealthScale(eggRarity), 1e-6);
            CHECK_NEAR(rig.world.get<ContactDamage>(pet).amount,
                       spider.damage * spider.petDamageScale * baselineScale
                           * ascendantSummonDamageScale(eggRarity), 1e-6);
        }

        const auto priorLanguage = ui::language();
        ui::setLanguage(ui::Language::En);
        const auto lines = ui::petalTooltipLines(eggIndex, eggRarity, SkillSet{}, 1.0);
        ui::setLanguage(priorLanguage);
        const char* expectedLabel = "Summon rarity: Hyper";
        CHECK(std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
            return line.text == expectedLabel;
        }));
    }
}

TEST(arctic_nuke_egg_apex_hatches_hyper_and_detonates_once_when_defeated) {
    ArcticBlast rig("arctic_nuke_egg", Rarity::Ascendant);
    rig.tickPetals(24000);
    std::vector<Entity> nukes;
    Query<Pet,MobType>{rig.world}.each([&](Entity e, Pet&, MobType& type) {
        CHECK(type.rarity == Rarity::Apex);
        nukes.push_back(e);
    });
    CHECK_EQ(nukes.size(), 3u);
    if (nukes.size() != 3) return;

    const Vec2 centre{5400,5000};
    rig.world.get<Transform>(nukes[0]).position = centre;
    rig.world.get<Transform>(nukes[1]).position = centre + Vec2{700,0};
    const double damage = rig.world.get<ContactDamage>(nukes[0]).amount;
    const Entity killer = rig.mob(centre);
    rig.world.get<Health>(killer) = Health{1e9,1e9};
    const Entity near = rig.mob(centre + Vec2{150,0});
    const Entity far = rig.mob(centre + Vec2{200,0});
    rig.world.get<Health>(near) = Health{1e9,1e9};
    rig.world.get<Health>(far) = Health{1e9,1e9};

    const auto blasts = [&]() {
        int count = 0;
        for (const auto& event : rig.events.events())
            if (event.kind == net::EventKind::Explosion) ++count;
        return count;
    };
    rig.grid.insert(near, rig.world.get<Transform>(near).realm, centre + Vec2{150,0}, 5);
    rig.grid.insert(far, rig.world.get<Transform>(far).realm, centre + Vec2{200,0}, 5);
    rig.combat.beginTick(rig.world, rig.registry, 24000, net::kTickSeconds, rig.events);
    const auto hit = rig.combat.applyDamage(rig.world, nukes[0], killer, 1e9, 24000);
    CHECK(hit.killed);
    rig.combat.runWorldPhase(rig.world, rig.grid, rig.registry, 24000, net::kTickSeconds);
    CHECK(rig.world.has<Dead>(nukes[0]));
    CHECK_NEAR(1e9 - rig.world.get<Health>(near).current, damage, 1e-5);
    CHECK_NEAR(rig.world.get<Health>(far).current, 1e9, 1e-5);
    CHECK(blasts() == 1);
    rig.world.get<Loadout>(rig.player).slots[0] = {};
    rig.tickPetals(24050);
    rig.combatTick(24050);
    CHECK(blasts() == 1);
}

TEST(arctic_wild_nuke_blast_targets_enemy_faction) {
    ArcticBlast rig("basic");
    const Vec2 centre{5100,5000};
    const Entity nuke = rig.mob(centre);
    rig.world.add<MobType>(nuke, MobType{rig.registry.mobIndex("arctic_nuke"), Rarity::Common});
    rig.world.add<ContactDamage>(nuke, ContactDamage{26.16});
    const Entity ally = rig.mob(centre + Vec2{100,0});

    rig.world.get<Health>(rig.player) = Health{110,110};
    const auto playerAt = rig.world.get<Transform>(rig.player);
    rig.grid.insert(rig.player, playerAt.realm, playerAt.position, 25);
    const auto allyAt = rig.world.get<Transform>(ally);
    rig.grid.insert(ally, allyAt.realm, allyAt.position, 5);
    rig.combat.beginTick(rig.world, rig.registry, 24000, net::kTickSeconds, rig.events);
    CHECK(rig.combat.applyDamage(rig.world, nuke, rig.player, 1e9, 24000).killed);
    rig.combat.runWorldPhase(rig.world, rig.grid, rig.registry, 24000, net::kTickSeconds);
    CHECK_NEAR(rig.world.get<Health>(rig.player).current, 83.84, 1e-6);
    CHECK_NEAR(rig.world.get<Health>(ally).current, 1000, 1e-6);
}

TEST(zorr_mechanic_cyanide_extends_live_poison_and_keeps_credit) {
    ArcticBlast rig("void_cyanide");
    const auto live = rig.liveIn(0);
    CHECK(!live.empty());
    if (live.empty()) return;
    const Vec2 at = rig.world.get<Transform>(live[0]).position;
    const auto poisoned = rig.mob(at);
    const auto clean = rig.mob(at + Vec2{500,0});
    rig.world.add<MobType>(poisoned, MobType{rig.registry.mobIndex("ladybug"), Rarity::Common});
    rig.world.add<MobType>(clean, MobType{rig.registry.mobIndex("ladybug"), Rarity::Common});
    rig.world.add<Afflictions>(poisoned).poisonStacks.push_back({rig.player, 4, 5000});
    rig.combatTick(4050);
    const auto& stacks = rig.world.get<Afflictions>(poisoned).poisonStacks;
    CHECK(stacks.size() == 1);
    if (stacks.size() != 1) return;
    CHECK(stacks[0].source == rig.player);
    CHECK_NEAR(stacks[0].perSecond, 4, 1e-6);
    CHECK_NEAR(stacks[0].untilMillis, 5150, 1e-6);
    CHECK(!rig.world.has<Afflictions>(clean));
}

TEST(zorr_mechanic_dizzy_has_a_single_direct_contact_bonus) {
    ArcticBlast rig("void_dizzy", Rarity::Super);
    rig.equip(1, "void_dizzy", Rarity::Common);
    rig.equip(2, "wound", Rarity::Super);
    rig.petals.foldModifiers(rig.world, rig.registry);
    CHECK_NEAR(rig.world.get<PlayerModifiers>(rig.player).jurassicDirectDamageScale, 1.18, 1e-6);
    const auto& content = rig.registry.petal(rig.registry.petalIndex("void_dizzy"));
    CHECK(content.noPhysics);
}

TEST(zorr_mechanic_void_fire_lights_without_contact_damage) {
    ArcticBlast rig("void_fire");
    const auto live = rig.liveIn(0);
    CHECK(!live.empty());
    if (live.empty()) return;
    const auto mob = rig.mob(rig.world.get<Transform>(live[0]).position);
    rig.combatTick(4050);
    const auto* status = rig.world.tryGet<Afflictions>(mob);
    CHECK(status && status->burns.size() == 1);
    if (!status || status->burns.size() != 1) return;
    CHECK_NEAR(status->burns.front().untilMillis, 7050, 1e-6);
    CHECK_NEAR(status->burns.front().perSecond, 10, 1e-6);
    CHECK_NEAR(rig.world.get<Health>(mob).current, 1000, 1e-6);
    rig.combatTick(4100);
    CHECK(rig.world.get<Health>(mob).current < 1000);
}

TEST(zorr_mechanic_cryo_freezes_near_and_not_far) {
    ArcticBlast rig("void_cryo_bomb");
    const Vec2 at = rig.armBomb();
    const auto near = rig.mob(at + Vec2{30,0});
    const auto far = rig.mob(at + Vec2{200,0});
    rig.combatTick(4050);
    const auto* status = rig.world.tryGet<Afflictions>(near);
    CHECK(status);
    if (!status) return;
    CHECK(status->slowFactor < 1.0);
    CHECK(!rig.world.has<Afflictions>(far));
}

TEST(zorr_mechanic_fire_bomb_keeps_blast_and_burns_per_bomb) {
    ArcticBlast rig("void_fire_bomb");
    const Vec2 at = rig.armBomb();
    const auto near = rig.mob(at + Vec2{30,0});
    const auto far = rig.mob(at + Vec2{200,0});
    rig.combatTick(4050);
    CHECK_NEAR(rig.world.get<Health>(near).current, 988, 1e-6);
    const auto* status = rig.world.tryGet<Afflictions>(near);
    CHECK(status);
    if (!status) return;
    CHECK_EQ(status->burns.size(), 1u);
    if (!status->burns.empty()) CHECK_NEAR(status->burns.front().perSecond, 7.5, 1e-6);
    CHECK_NEAR(rig.world.get<Health>(far).current, 1000, 1e-6);
    CHECK(!rig.world.has<Afflictions>(far));
}

TEST(fire_bombs_stack_independently_up_to_three_and_expire) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("void_fire_bomb");
    for (int slot = 1; slot < 4; ++slot) rig.equip(slot, "void_fire_bomb");
    rig.tickPetals(4100);
    rig.tickPetals(8000);
    const Vec2 at{5300, 5300};
    std::vector<Entity> shells;
    const auto config = rig.registry.petalIndex("void_fire_bomb");
    Query<PetalInstance>{rig.world}.each([&](Entity petal, PetalInstance& instance) {
        if (instance.configIndex == config) shells.push_back(petal);
    });
    CHECK_EQ(shells.size(), 4u);
    for (const Entity shell : shells) {
        rig.world.get<Transform>(shell).position = at;
        rig.world.get<Health>(shell).current = 0.0;
        rig.world.add<Dead>(shell);
    }
    rig.tickPetals(8050);
    const Entity target = rig.mob(at + Vec2{30.0, 0.0});
    const Entity dummy = rig.dummy(at + Vec2{-30.0, 0.0});
    rig.combatTick(8050);
    CHECK_NEAR(rig.world.get<Health>(target).current, 952.0, 1e-8);
    const auto* status = rig.world.tryGet<Afflictions>(target);
    CHECK(status && status->burns.size() == 3);
    if (!status) return;
    for (const auto& burn : status->burns) {
        CHECK_NEAR(burn.perSecond, 7.5, 1e-8);
        CHECK_NEAR(burn.untilMillis, 11050.0, 1e-8);
    }
    std::vector<Entity> fields;
    Query<GroundEffect>{rig.world}.collect(fields);
    for (const Entity field : fields) rig.world.destroy(field);
    rig.combatTick(8100);
    CHECK_NEAR(rig.world.get<Health>(target).current, 952.0 - 22.5 * net::kTickSeconds, 1e-8);
    const auto* dummyStatus = rig.world.tryGet<Afflictions>(dummy);
    CHECK(dummyStatus && dummyStatus->burns.size() == 3u);
    const auto recorded = rig.combat.practiceDamage(rig.player, 8100, dummy);
    const auto burnRecorded = std::find_if(recorded.begin(), recorded.end(), [](const auto& row) {
        return row.source == CombatSystem::PracticeSource::Burn;
    });
    CHECK(burnRecorded != recorded.end());
    if (burnRecorded != recorded.end())
        CHECK_NEAR(burnRecorded->amount, 22.5 * net::kTickSeconds, 1e-8);
    CHECK_NEAR(rig.world.get<Health>(dummy).current, 1000.0, 1e-8);
    rig.combatTick(11050);
    CHECK(rig.world.get<Afflictions>(target).burns.empty());
}

TEST(offensive_fluid_buffs_reach_the_new_edge_and_raise_actual_damage) {
    ArcticBlast rig("basic");
    const auto& acid = rig.registry.petal(rig.registry.petalIndex("acid")).spill;
    const Vec2 centre{5300, 5300};
    CombatSystem::createSpill(rig.world, rig.player, centre, Rarity::Common, acid, 1.0, 0);
    for (int tick = 1; tick <= 21; ++tick) rig.combatTick(4000 + tick * 50);
    const Entity tested = rig.mob(centre + Vec2{78, 0});
    const Entity outside = rig.mob(centre + Vec2{0, 120});
    rig.combatTick(5100);
    CHECK_NEAR(rig.world.get<Health>(tested).current, 1000.0 - 34.3 * net::kTickSeconds, 1e-8);
    CHECK_NEAR(rig.world.get<Health>(outside).current, 1000.0, 1e-8);
}

TEST(fire_bomb_tooltip_matches_the_common_burn_and_cap_in_both_languages) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("basic");
    const auto previous = ui::language();
    for (const char* id : {"fire_bomb", "void_fire_bomb"}) {
        const auto index = rig.registry.petalIndex(id);
        for (const auto language : {ui::Language::En, ui::Language::Zh}) {
            ui::setLanguage(language);
            const auto lines = ui::petalTooltipLines(index, Rarity::Common, SkillSet{}, 1.0);
            const auto hasText = [&](const std::string& text) {
                return std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
                    return line.text.find(text) != std::string::npos;
                });
            };
            const bool zh = language == ui::Language::Zh;
            const std::string burnRate = std::string(id) == "void_fire_bomb" ? "7.5" : "8.75";
            CHECK(hasText((zh ? "每层基础燃烧伤害/秒：" : "Base burn damage per second per stack: ") + burnRate));
            CHECK(hasText(zh ? "燃烧持续时间：3s" : "Burn duration: 3s"));
            CHECK(hasText(zh ? "燃烧半径：" : "Burn radius: "));
            CHECK(hasText(zh ? "叠火上限：3" : "Max stacks per player on one target: 3"));
        }
    }
    ui::setLanguage(previous);
}

TEST(spill_build_modifiers_apply_to_real_pools_and_their_damage) {
    for (const char* id : {"acid", "lava", "oil", "latrine"}) {
        const auto measure = [&](std::initializer_list<const char*> support) {
            ArcticBlast rig(id);
            int slot = 1;
            for (const char* buff : support) rig.equip(slot++, buff, Rarity::Super);
            rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
            for (double t = 4050; t < 21000; t += 50) {
                rig.tickPetals(t);
                const auto fields = Query<GroundEffect>{rig.world}.without<Dead>().collect();
                for (Entity e : fields) {
                    const auto& field = rig.world.get<GroundEffect>(e);
                    if (field.kind != GroundEffectKind::AcidSpill &&
                        field.kind != GroundEffectKind::LavaSpill &&
                        field.kind != GroundEffectKind::OilSpill &&
                        field.kind != GroundEffectKind::LatrineSpill) continue;
                    const double rate = field.damagePerSecond;
                    const auto kind = field.kind;
                    for(int spread=1;spread<=7;++spread)rig.combatTick(t - 400 + spread*50); // Settle the new authoritative spread before measuring one damage tick.
                    const auto centre = rig.world.get<Transform>(e).position;
                    const Entity target = rig.mob(centre + Vec2{0, 20});
                    const double before = rig.world.get<Health>(target).current;
                    rig.combatTick(t);
                    if (kind == GroundEffectKind::LavaSpill || kind == GroundEffectKind::LatrineSpill) {
                        rig.world.destroy(e);
                        rig.combatTick(t + 50);
                    }
                    CHECK_NEAR(before - rig.world.get<Health>(target).current,
                               rate * net::kTickSeconds, 1e-8);
                    return rate;
                }
            }
            CHECK(false);
            return 0.0;
        };
        const double base = measure({});
        CHECK(base > 0.0);
        CHECK_NEAR(measure({"void_elemental"}), base * 1.07, 1e-8);
        CHECK_NEAR(measure({"void_dizzy"}), base * 1.18, 1e-8);
        CHECK_NEAR(measure({"void_horn", "void_horn"}), base * 1.124, 1e-8);
        CHECK_NEAR(measure({"void_elemental", "void_dizzy", "void_horn"}),
                   base * 1.07 * 1.18 * 1.124, 1e-8);
    }
}

TEST(spill_build_collision_bombs_inherit_damage_modifiers) {
    for (const char* id : {"acid_bomb", "lava_bomb"}) {
        ArcticBlast rig(id);
        rig.equip(1, "void_dizzy");
        rig.petals.foldModifiers(rig.world, rig.registry);
        const auto bombs = rig.liveIn(0);
        CHECK(!bombs.empty());
        if (bombs.empty()) continue;
        const auto at = rig.world.get<Transform>(bombs.front()).position;
        const auto& spill = rig.registry.petal(rig.registry.petalIndex(id)).spill;
        CHECK(spill.onHit);
        rig.mob(at);
        rig.combatTick(4050);
        int pools = 0;
        Query<GroundEffect>{rig.world}.each([&](Entity, GroundEffect& field) {
            if (field.kind != GroundEffectKind::AcidSpill &&
                field.kind != GroundEffectKind::LavaSpill) return;
            ++pools;
            CHECK_NEAR(field.damagePerSecond, spill.strength * 2.1875 * 1.04, 1e-8);
        });
        CHECK_EQ(pools, 1);
    }
}

TEST(spill_build_opal_rolls_once_per_pool_at_an_eligible_tier) {
    ArcticBlast rig("basic");
    const auto& acid = rig.registry.petal(rig.registry.petalIndex("acid")).spill;
    const auto countCritical = [&](Rarity opalRarity, Rarity poolRarity) {
        rig.equip(1, "void_opal", opalRarity);
        rig.petals.foldModifiers(rig.world, rig.registry);
        Rng rng{16};
        const double base = acid.strength * fluidDamageScale(acid.kind);
        const int tier = petalTier(poolRarity);
        const double critical = base * (1.5 + tier * (1.5 / 8.0));
        int count = 0;
        for (int i = 0; i < 160; ++i) {
            CombatSystem::createSpill(rig.world, rig.player, {5300, 5300},
                                      poolRarity, acid, 1.0, 0, 1.0, &rng);
            const auto pool = Query<GroundEffect>{rig.world}.collect().front();
            const double actual = rig.world.get<GroundEffect>(pool).damagePerSecond;
            CHECK(std::abs(actual - base) < 1e-8 || std::abs(actual - critical) < 1e-8);
            if (actual > base) ++count;
            rig.world.destroy(pool);
        }
        return count;
    };
    CHECK_EQ(countCritical(Rarity::Common, Rarity::Rare), 0);
    const int critical = countCritical(Rarity::Common, Rarity::Common);
    CHECK(critical > 0 && critical < 160);
}

TEST(spill_build_opal_marks_real_acid_and_bomb_hits_and_lingering_damage) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const char* id : {"acid", "acid_bomb", "lava", "lava_bomb", "hot_water", "oil", "latrine"}) {
        ArcticBlast rig(id);
        rig.equip(1, "void_opal");
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        const auto& spill = rig.registry.petal(rig.registry.petalIndex(id)).spill;
        bool sawCritical = false;
        int pools = 0;
        for (int now = 4050; now <= 230000 && !sawCritical && pools < 100; now += 50) {
            rig.tickPetals(now);
            if (spill.onHit) {
                const auto live = rig.liveIn(0);
                if (live.empty()) continue;
                const Entity contact = rig.mob(rig.world.get<Transform>(live.front()).position);
                rig.combatTick(now);
                rig.world.destroy(contact);
            }
            const auto fields = Query<GroundEffect>{rig.world}.without<Dead>().collect();
            if (fields.empty()) continue;
            ++pools;
            const auto pool = fields.front();
            const GroundEffect field = rig.world.get<GroundEffect>(pool);
            if (!field.criticalSpill) {
                rig.world.destroy(pool);
                continue;
            }
            sawCritical = true;
            const Vec2 centre = rig.world.get<Transform>(pool).position;
            rig.world.get<Transform>(rig.player).position = centre + Vec2{1000, 0};
            for (Entity petal : rig.world.get<Loadout>(rig.player).spawned)
                rig.world.get<Transform>(petal).position = centre + Vec2{1000, 0};
            const Entity mob = rig.mob(centre + Vec2{0, 9});
            const Entity dummy = rig.dummy(centre + Vec2{0, -9});
            rig.world.add<NetId>(mob, NetId{80});
            rig.world.add<NetId>(dummy, NetId{81});
            rig.events.clear();
            rig.combatTick(now + 50);
            if (spill.kind == SpillKind::Lava || spill.kind == SpillKind::Latrine) {
                // The emitter is gone before the first status tick. Both its
                // rate and critical flag must survive in that target's stack.
                rig.world.destroy(pool);
                rig.combatTick(now + 100);
            }
            for (std::uint32_t netId : {80u, 81u}) {
                double indicated = 0;
                for (const auto& event : rig.events.events())
                    if (event.kind == net::EventKind::Damage && event.netId == netId &&
                        (event.flag & net::DamageCritical)) indicated += event.amount;
                CHECK_NEAR(indicated, field.damagePerSecond * net::kTickSeconds, 1e-5);
            }
            CHECK_NEAR(1000 - rig.world.get<Health>(mob).current,
                       field.damagePerSecond * net::kTickSeconds, 1e-5);
            CHECK(!rig.combat.practiceDamage(rig.player, now + 100, dummy).empty());

            rig.world.add<NetId>(rig.player, NetId{79});
            Replicator replicator;
            Replicator::Frame frame;
            frame.nowMillis = now + 100;
            frame.tick = 1;
            frame.viewport = Vec2{3000, 2000};
            frame.events = &rig.events;
            ClientView peer;
            ByteWriter packet;
            replicator.build(rig.world, rig.player, peer, frame, packet);
            ByteReader reader(packet.data(), packet.size());
            CHECK_EQ(reader.u8(), static_cast<std::uint8_t>(net::ServerMessage::Snapshot));
            WorldView client;
            CHECK(client.applySnapshot(reader, now + 100));
            for (std::uint32_t netId : {80u, 81u})
                CHECK(std::any_of(client.events().begin(), client.events().end(),
                    [&](const ViewEvent& event) {
                        return event.kind == net::EventKind::Damage && event.netId == netId &&
                               (event.flag & net::DamageCritical) && (event.flag & net::DamageByViewer);
                    }));
        }
        if (!sawCritical) std::printf("No critical pool observed for %s after %d casts\n", id, pools);
        CHECK(sawCritical);
    }
}

TEST(spill_build_extra_golden_leaves_shorten_pool_cadence) {
    const auto interval = [](int leaves) {
        ArcticBlast rig("acid");
        for (int i = 1; i <= leaves; ++i) rig.equip(i, "golden_leaf", Rarity::Apex);
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        double first = -1;
        for (double now = 4050; now < 28000; now += 50) {
            rig.tickPetals(now);
            const auto fields = Query<GroundEffect>{rig.world}.without<Dead>().collect();
            if (fields.empty()) continue;
            for (Entity e : fields) rig.world.destroy(e);
            if (first >= 0) return now - first;
            first = now;
        }
        CHECK(false);
        return 0.0;
    };
    const double zero = interval(0);
    const double one = interval(1);
    const double two = interval(2);
    std::printf("Acid intervals for zero, one and two Apex Golden Leaves: %.0f, %.0f, %.0f ms\n",
                zero, one, two);
    CHECK(zero > one && one > two && two > 0);
}

TEST(spill_build_multiple_soaps_continue_to_extend_radius) {
    double previous = 0.0;
    for (int soaps = 0; soaps <= 5; ++soaps) {
        ArcticBlast rig("basic");
        const auto& acid = rig.registry.petal(rig.registry.petalIndex("acid")).spill;
        for (int slot = 1; slot <= soaps; ++slot) rig.equip(slot, "void_soap", Rarity::Ascendant);
        rig.petals.foldModifiers(rig.world, rig.registry);
        const Vec2 centre{5300, 5300};
        CombatSystem::createSpill(rig.world, rig.player, centre, Rarity::Common, acid, 1.0, 0);
        const Entity pool = Query<GroundEffect>{rig.world}.collect().front();
        for (int tick = 1; tick <= 36; ++tick) rig.combatTick(4000 + tick * 50);
        const double radius = rig.world.get<GroundEffect>(pool).radius;
        CHECK(radius > previous);
        previous = radius;
        const auto inside = rig.mob(centre + Vec2{radius - 6, 0});
        const auto outside = rig.mob(centre + Vec2{radius + 6, 0});
        rig.combatTick(5850);
        CHECK_NEAR(rig.world.get<Health>(inside).current, 1000.0 - 34.3 * net::kTickSeconds, 1e-8);
        CHECK_NEAR(rig.world.get<Health>(outside).current, 1000.0, 1e-8);
    }
    CHECK_NEAR(previous / 94.875, 4.7, 1e-8);
}

TEST(soap_accelerates_edge_contact_while_center_damage_is_constant) {
    int previousFirstTick = 41;
    for (int soaps = 0; soaps <= 2; ++soaps) {
        ArcticBlast rig("basic");
        for (int slot = 1; slot <= soaps; ++slot) rig.equip(slot, "void_soap");
        rig.petals.foldModifiers(rig.world, rig.registry);
        const auto& acid = rig.registry.petal(rig.registry.petalIndex("acid")).spill;
        const Vec2 at{5500, 5500};
        CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, acid, 1.0, 0);
        const Entity center = rig.mob(at);
        const Entity edge = rig.mob(at + Vec2{98, 0});
        int first = 0;
        for (int tick = 1; tick <= 40; ++tick) {
            rig.combatTick(4000 + tick * net::kTickMillis);
            if (!first && rig.world.get<Health>(edge).current < 1000.0) first = tick;
        }
        CHECK(first > 0 && first < previousFirstTick);
        previousFirstTick = first;
        const double damage = 1000.0 - rig.world.get<Health>(center).current;
        CHECK_NEAR(damage, 68.6, 1e-8);
        std::printf("common acid with %d Soap: edge hit %.2fs, center %.2f damage in 2s\n",
                    soaps, first * net::kTickSeconds, damage);
    }
}

TEST(soap_and_acid_tooltips_display_original_rate_units_in_both_languages) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    const auto previous = ui::language();
    for (auto language : {ui::Language::En, ui::Language::Zh}) {
        ui::setLanguage(language);
        const auto hasLine = [&](const char* id, const char* expected) {
            const auto lines = ui::petalTooltipLines(content().petalIndex(id), Rarity::Super, SkillSet{}, 1.0);
            return std::any_of(lines.begin(), lines.end(), [&](const auto& line) { return line.text == expected; });
        };
        const bool zh = language == ui::Language::Zh;
        CHECK(hasLine("void_soap", zh ? "液池扩散速度：+68.8%" : "Liquid spread speed: +68.8%"));
        CHECK(hasLine("hot_water", zh ? "基础扩散速度：61.84/s" : "Base radial growth: 61.84/s"));
        CHECK(hasLine("acid", zh ? "基础扩散速度：46/s" : "Base radial growth: 46/s"));
    }
    ui::setLanguage(previous);
}

TEST(zorr_mechanic_soap_widens_a_single_lava_pool) {
    for(bool soap : {false,true}) {
        ArcticBlast rig("basic");
        if(soap) rig.equip(1,"void_soap",Rarity::Super);
        rig.petals.foldModifiers(rig.world,rig.registry);
        const auto& lava=rig.registry.petal(rig.registry.petalIndex("lava")).spill;
        CombatSystem::createSpill(rig.world,rig.player,{5400,5400},Rarity::Common,lava,1,0);
        for (int tick = 1; tick <= 29; ++tick) rig.combatTick(4000 + tick * 50);
        const Entity edge=rig.mob({5503,5400});
        const Entity outside=rig.mob({5620,5400});
        rig.combatTick(5500);
        const auto burned=[&](Entity e) {
            const auto* status=rig.world.tryGet<Afflictions>(e);
            return status && !status->burns.empty();
        };
        CHECK(burned(edge)==soap);
        CHECK(!burned(outside));
    }
}

TEST(zorr_mechanic_soap_expands_blood_without_changing_shred) {
    for (bool soap : {false, true}) for (bool talent : {false, true}) {
        ArcticBlast rig("blood");
        if (soap) rig.equip(1, "void_soap");
        if (talent) rig.world.add<PlayerSkillTree>(rig.player).skills.set(SkillId::FluidReach, 2);
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.tickPetals(5000);
        rig.tickPetals(6000);
        for (int tick = 1; tick <= 7; ++tick) rig.combatTick(6000 + tick * 50);
        int patches = 0;
        Query<GroundEffect>{rig.world}.each([&](Entity, GroundEffect& field) {
            if (field.kind != GroundEffectKind::Blood) return;
            ++patches;
            CHECK_NEAR(field.radius, (36.8 * 1.20 * (soap ? 1.30 : 1.0) - 5.0 + 0.35 * 10.0 * (soap ? 1.10 : 1.0)) * (talent ? 1.30 : 1.0), 1e-6);
            CHECK_NEAR(field.armorReduction,
                       rig.registry.petalStats(rig.registry.petalIndex("blood"), Rarity::Common).armorReduction,
                       1e-6);
        });
        CHECK(patches == 1);
    }
}

TEST(acid_spill_hits_the_new_radius_with_the_tuned_damage) {
    ArcticBlast rig("basic");
    const auto& acid = rig.registry.petal(rig.registry.petalIndex("acid")).spill;
    CHECK_NEAR(acid.radius, 57.5, 1e-6);
    CHECK_NEAR(acid.strength, 15.68, 1e-6);
    const Vec2 center{5500, 5500};
    CombatSystem::createSpill(rig.world, rig.player, center, Rarity::Common, acid, 1.0, 0);
    // Range assertions below describe the fully spread pool, not its birth frame.
    for(int spread=1;spread<=7;++spread)rig.combatTick(4000+spread*50);
    const auto near = rig.mob(center + Vec2{58, 0});
    const auto far = rig.mob(center + Vec2{110, 0});
    rig.combatTick(4400);
    CHECK(rig.world.get<Health>(near).current < 1000.0);
    CHECK_EQ(rig.world.get<Health>(far).current, 1000.0);
}

TEST(zorr_mechanic_soap_applies_to_all_configured_liquid_fields) {
    ArcticBlast rig("basic");
    struct Rates { const char* id; double common; double super; double hyper; };
    for (const Rates row : {Rates{"acid", 11, 46, 56}, {"lava", 11, 46, 56},
                            {"oil", 11, 46, 71}, {"hot_water", 5, 61.84, 98.96},
                            {"elixir", 15, 15, 15}, {"acid_bomb", 10, 41.5, 50.5},
                            {"lava_bomb", 10, 41.5, 50.5}, {"latrine", 9, 40.5, 49.5},
                            {"arctic_nitrogen", 10, 66, 82}}) {
        const auto& spill = rig.registry.petal(rig.registry.petalIndex(row.id)).spill;
        CHECK_NEAR(spill.growthPerSecond(Rarity::Super), row.super, 1e-6);
        CHECK_NEAR(spill.growthPerSecond(Rarity::Apex), row.hyper, 1e-6);
        CHECK(spill.kind != SpillKind::None);
        CHECK(spill.radius > 0.0);
        for (bool soap : {false, true}) {
            if (soap) rig.equip(1, "void_soap");
            else rig.world.get<Loadout>(rig.player).slots[1] = {};
            rig.petals.foldModifiers(rig.world, rig.registry);
            CombatSystem::createSpill(rig.world, rig.player, {5000,5000}, Rarity::Common, spill, 1.0, 0);
            for (int tick = 1; tick <= 7; ++tick) rig.combatTick(4000 + tick * 50);
            std::vector<Entity> fields;
            Query<GroundEffect>{rig.world}.collect(fields);
            CHECK(fields.size() == 1);
            for (const auto field : fields) {
                const auto& effect = rig.world.get<GroundEffect>(field);
                const double maximum = spillBaseRadius(spill, Rarity::Common) * (soap ? 1.30 : 1.0);
                const double reserve = std::min(0.24 * maximum, row.common * 0.5);
                CHECK_NEAR(effect.spreadTargetRadius, maximum, 1e-6);
                CHECK_NEAR(effect.radius, maximum - std::max(0.0, reserve - 0.35 * row.common * (soap ? 1.10 : 1.0)), 1e-6);
                CHECK_NEAR(effect.damagePerSecond, spill.strength * fluidDamageScale(spill.kind), 1e-6);
                rig.world.destroy(field);
            }
        }
    }
}

TEST(zorr_mechanic_void_opal_enables_each_eligible_tier) {
    ArcticBlast rig("void_opal", Rarity::Rare);
    rig.petals.foldModifiers(rig.world, rig.registry);
    auto tiers = rig.world.get<PlayerModifiers>(rig.player).opalTiers;
    CHECK(tiers[0] && tiers[1] && tiers[2] && !tiers[3]);
    rig.equip(0, "basic");
    rig.petals.foldModifiers(rig.world, rig.registry);
    tiers = rig.world.get<PlayerModifiers>(rig.player).opalTiers;
    CHECK(!tiers[0] && !tiers[1] && !tiers[2]);
}

TEST(zorr_mechanic_comb_applies_poison_and_burn_on_contact) {
    ArcticBlast rig("void_comb");
    rig.tickPetals(4600);
    const auto live = rig.liveIn(0);
    CHECK(!live.empty());
    if (live.empty()) return;
    const auto victim = rig.mob(rig.world.get<Transform>(live.front()).position);
    rig.combatTick(4650);
    const auto* status = rig.world.tryGet<Afflictions>(victim);
    CHECK(status && status->poisonStacks.size() == 1 && status->burns.size() == 1);
    if (!status || status->poisonStacks.empty() || status->burns.empty()) return;
    CHECK_NEAR(status->poisonStacks.front().perSecond, 12, 1e-6);
    CHECK_NEAR(status->burns.front().perSecond, 5, 1e-6);
    CHECK(status->poisonStacks.front().source == rig.player);
    CHECK(status->burns.front().owner == rig.player);
}

TEST(zorr_mechanic_mirror_and_dvd_spawn_armored) {
    for (const char* id : {"void_mirror", "void_dvd"}) {
        ArcticBlast rig(id);
        const auto live = rig.liveIn(0);
        CHECK(!live.empty());
        if (live.empty()) return;
        const auto* armor = rig.world.tryGet<Armor>(live.front());
        CHECK(armor);
        if (armor) CHECK_NEAR(armor->amount, id == std::string("void_mirror") ? 2 : 3, 1e-6);
    }
    ArcticBlast rig("void_dvd");
    const auto live = rig.liveIn(0);
    CHECK(!live.empty());
    if (live.empty()) return;
    const auto victim = rig.mob(rig.world.get<Transform>(live.front()).position);
    rig.combatTick(4050);
    const auto* status = rig.world.tryGet<Afflictions>(victim);
    CHECK(status && status->burns.size() == 1);
    if (status && !status->burns.empty()) CHECK_NEAR(status->burns.front().perSecond, 3, 1e-6);
}

TEST(zorr_mechanic_horn_and_fan_use_the_strongest_worn_bonus) {
    ArcticBlast rig("void_horn", Rarity::Super);
    rig.equip(1, "void_horn");
    rig.equip(2, "void_fan", Rarity::Rare);
    rig.equip(3, "void_fan");
    rig.petals.foldModifiers(rig.world, rig.registry);
    auto mods = rig.world.get<PlayerModifiers>(rig.player);
    CHECK_NEAR(mods.summonDamageScale, 1.124, 1e-6);
    CHECK_NEAR(mods.fireResistance, .034, 1e-6);
    const double before = rig.world.get<Health>(rig.player).current;
    rig.world.add<Afflictions>(rig.player).burns.push_back({NULL_ENTITY, NULL_ENTITY, 100, 6000});
    rig.combatTick(4050);
    CHECK_NEAR(rig.world.get<Health>(rig.player).current, before - 96.6 * net::kTickSeconds, 1e-6);
    for (int i = 0; i < 4; ++i) rig.world.get<Loadout>(rig.player).slots[i] = {};
    rig.petals.foldModifiers(rig.world, rig.registry);
    mods = rig.world.get<PlayerModifiers>(rig.player);
    CHECK_NEAR(mods.summonDamageScale, 1, 1e-6);
    CHECK_NEAR(mods.fireResistance, 0, 1e-6);
}
TEST(zorr_mechanic_horn_updates_an_existing_summon_and_restores_damage) {
    ArcticBlast rig("egg");
    for (double now : {10000.0, 20000.0, 30000.0}) rig.tickPetals(now);
    std::vector<Entity> pets;
    Query<Pet,ContactDamage>{rig.world}.collect(pets);
    CHECK(!pets.empty());
    if (pets.empty()) return;
    const auto pet = pets.front();
    const double before = rig.world.get<ContactDamage>(pet).amount;
    rig.equip(1, "void_horn", Rarity::Super);
    rig.petals.foldModifiers(rig.world, rig.registry);
    rig.tickPetals(30050);
    CHECK_NEAR(rig.world.get<ContactDamage>(pet).amount, before * 1.124, 1e-6);
    CHECK_NEAR(rig.world.get<Pet>(pet).damageTalentScale, 1.124, 1e-6);
    rig.world.get<Loadout>(rig.player).slots[1] = {};
    rig.petals.foldModifiers(rig.world, rig.registry);
    rig.tickPetals(30100);
    CHECK_NEAR(rig.world.get<ContactDamage>(pet).amount, before, 1e-6);
}

TEST(magic_bubble_bomb_contact_pushes_nearby_wild_mobs_once) {
    // A magic petal forms only by paying mana, so the bar carries an orb.
    ArcticBlast rig("magic_bubble_bomb", Rarity::Common, "mana_orb");
    const auto live = rig.liveIn(0);
    CHECK(!live.empty());
    if (live.empty()) return;
    auto at = rig.world.get<Transform>(live.front()).position;
    const auto allied = rig.mob(at + Vec2{1,0});
    rig.world.add<Pet>(allied, Pet{rig.player});
    rig.world.get<Faction>(allied).team = Team::Players;
    rig.tickPetals(4000);
    CHECK(!rig.world.has<Dead>(live.front()));
    at = rig.world.get<Transform>(live.front()).position;
    const auto trigger = rig.mob(at + Vec2{1,0});
    const auto near = rig.mob(at + Vec2{35,0});
    const auto far = rig.mob(at + Vec2{200,0});
    for (const auto mob : {trigger,near,far}) rig.world.add<Motion>(mob);
    rig.tickPetals(4000);
    CHECK(!rig.world.has<Knockback>(allied));
    CHECK(rig.world.has<Dead>(live.front()));
    const auto* shove = rig.world.tryGet<Knockback>(near);
    CHECK(shove && shove->impulse.x > 0);
    CHECK(!rig.world.has<Knockback>(far));
    rig.combatTick(4050);
    CHECK_NEAR(rig.world.get<Health>(near).current, 999.65, 1e-6);
    CHECK_NEAR(rig.world.get<Health>(far).current, 1000, 1e-6);
    rig.tickPetals(4100);
    rig.combatTick(4100);
    CHECK_NEAR(rig.world.get<Health>(near).current, 999.65, 1e-6);
}
TEST(reference_recipe_bubble_bomb_checks_range_damage_and_single_impulse) {
    ArcticBlast rig("bubble_bomb", Rarity::Common);
    const auto live = rig.liveIn(0);
    CHECK(!live.empty());
    if (live.empty()) return;
    rig.tickPetals(4000);
    const auto at = rig.world.get<Transform>(live.front()).position;
    const Entity trigger = rig.mob(at + Vec2{1, 0});
    const Entity near = rig.mob(at + Vec2{20, 0});
    const Entity outside = rig.mob(at + Vec2{200, 0});
    for (const Entity mob : {trigger, near, outside}) rig.world.add<Motion>(mob);
    rig.tickPetals(4000);
    CHECK(rig.world.has<Dead>(live.front()));
    const auto* shove = rig.world.tryGet<Knockback>(near);
    CHECK(shove && shove->impulse.x > 0);
    CHECK(!rig.world.has<Knockback>(outside));
    rig.combatTick(4050);
    const double health = rig.world.get<Health>(near).current;
    CHECK(health < 1000.0);
    CHECK_NEAR(rig.world.get<Health>(outside).current, 1000.0, 1e-6);
    rig.tickPetals(4100);
    rig.combatTick(4100);
    CHECK_NEAR(rig.world.get<Health>(near).current, health, 1e-6);
}

TEST(magic_bubble_bomb_pushes_a_mob_that_covers_its_centre) {
    ArcticBlast collision("magic_bubble_bomb", Rarity::Common, "mana_orb");
    const auto live = collision.liveIn(0);
    CHECK(!live.empty());
    if (live.empty()) return;
    const Vec2 centre = collision.world.get<Transform>(live.front()).position;
    const Vec2 flower = collision.world.get<Transform>(collision.player).position;
    const Entity overlap = collision.mob(centre);
    collision.world.add<Motion>(overlap);
    // Keep the ring still while the contact pass runs at exact overlap.
    collision.petals.run(collision.world, collision.registry, 4050, 0.0, collision.commands);
    collision.commands.flush();
    const auto* knockback = collision.world.tryGet<Knockback>(overlap);
    CHECK(knockback && knockback->impulse.length() > 1.0);
    if (knockback) CHECK_NEAR(knockback->impulse.angle(), (centre - flower).angle(), 1e-9);
}

TEST(magic_bubble_bomb_high_blueberries_keep_volley_rate) {
    const auto& c=testsupport::shippedContent();
    const auto berry=c.petalIndex("magic_peas");
    for(const auto r:{Rarity::Super,Rarity::Unique,Rarity::Apex,Rarity::PermanentHyper}) {
        const auto stats=c.petalStats(berry,r);
        const double eternalHit = 279006.75; // Authored Eternal projectile damage, including its rounding.
        const double expectedHit = r == Rarity::Super ? 90.0 * petalStrengthScale(r) :
                                   r == Rarity::Unique ? eternalHit : 3.0 * eternalHit;
        CHECK_NEAR(stats.lightningDamagePerHit, expectedHit, 1e-6);
        CHECK_NEAR(stats.reloadMillis,250,1e-6);
        CHECK(stats.count==4);
    }
    LootSystem loot;
    std::string error;
    CHECK(loot.loadTables(c,testsupport::dataFile("mob_drops.json"),error));
}

TEST(feedback_728_802_eggs_hatch_real_duplicates_and_replicate_ice_dragon_rarity) {
    for (const auto* egg : {"jurassic_ankylosaurus_egg", "arctic_ice_dragon_egg"}) {
        const bool dragon = std::string_view(egg) == "arctic_ice_dragon_egg";
        for (const auto rarity : {Rarity::Super, Rarity::Unique, Rarity::Apex,
                                  Rarity::PermanentHyper, Rarity::Ascendant}) {
            for (int slot : {0, 1}) {
                ArcticBlast rig("basic");
                auto& skills = rig.world.add<PlayerSkillTree>(rig.player).skills;
                skills.set(SkillId::Loadout, 4);
                skills.set(SkillId::Duplicator, 3);
                rig.world.get<Loadout>(rig.player).slots[0] = {};
                rig.equip(slot, egg, rarity);
                for (const double now : {5000.0, 80000.0, 140000.0, 220000.0}) rig.tickPetals(now);
                int actual = 0;
                Query<Pet, MobType, Replicated>{rig.world}.without<Dead>().each(
                    [&](Entity, Pet& pet, MobType& mob, Replicated& net) {
                        if (pet.owner != rig.player || pet.slot != slot) return;
                        ++actual;
                        if (dragon) {
                            CHECK(mob.rarity == petalGameplayRarity(rarity));
                            CHECK(net.rarity == mob.rarity);
                        }
                    });
                const int copies = slot == 0 ? 2 : 1;
                CHECK_EQ(actual, copies * (!dragon && ladderRarity(rarity) == Rarity::Apex ? 3 : 1));
            }
        }
    }
}

TEST(zorr_reference_eyeball_homes_same_or_lower_tiers_and_gunpowder_speeds_missiles) {
    for (auto rarity : {Rarity::Common, Rarity::Rare, Rarity::Epic}) {
        for (bool supports : {false, true}) {
            ArcticBlast rig("missile", rarity);
            if (supports) {
                rig.equip(1,"void_eyeball",Rarity::Rare);
                rig.equip(2,"void_gunpowder",Rarity::Rare);
            }
            rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
            for (int now=4050;now<=12000;now+=50) rig.tickPetals(now);
            const auto& spec=rig.registry.petal(rig.registry.petalIndex("missile")).projectile;
            int shots=0;
            Query<Projectile,Motion>{rig.world}.each([&](Entity,Projectile& p,Motion& motion){
                if(p.petalConfigIndex!=rig.registry.petalIndex("missile")) return;
                ++shots;
                const bool guided=supports && rarity!=Rarity::Epic;
                CHECK_NEAR(p.seekRange,guided ? std::max(spec.seekRange,std::min(1200.0,spec.distance)) : spec.seekRange,1e-6);
                CHECK_NEAR(p.seekCone,guided ? kPi : spec.seekCone,1e-6);
                CHECK_NEAR(motion.velocity.length(),spec.speed*(supports ? 1.15 : 1.0),1e-6);
            });
            CHECK(shots>0);
        }
    }
}

TEST(eyeball_aims_charged_magic_missiles_through_their_effective_range) {
    const auto checkLaunch = [](const char* petal, Rarity tier, Rarity eye,
                                double distance, bool shouldAim) {
        ArcticBlast rig(petal, tier);
        rig.equip(1, "void_eyeball", eye);
        rig.equip(2, "mana_orb", tier);
        rig.tickPetals(16000);
        auto& mana = rig.world.get<ManaPool>(rig.player);
        mana.current = mana.max;
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        const auto type = rig.registry.petalIndex(petal);
        Entity shot = NULL_ENTITY;
        int now = 16000;
        while (shot == NULL_ENTITY && now < 23000) {
            now += 50;
            rig.tickPetals(now);
            Query<Projectile>{rig.world}.without<Dead>().each([&](Entity e, Projectile& p) {
                if (p.petalConfigIndex == type) shot = e;
            });
        }
        CHECK(shot != NULL_ENTITY);
        if (shot == NULL_ENTITY) return;
        const auto origin = rig.world.get<Transform>(shot).position;
        const auto initial = rig.world.get<Motion>(shot).velocity;
        const Vec2 side = Vec2::fromAngle(initial.angle() + kPi / 2);
        const auto target = rig.mob(origin + side * distance);
        Terrain terrain;
        MovementSystem movement;
        movement.runWorldPhase(rig.world, terrain, now, net::kTickSeconds);
        const auto launch = rig.world.get<Motion>(shot).velocity;
        CHECK_NEAR(angleDelta(initial.angle(), launch.angle()), shouldAim ? kPi / 2 : 0.0, 1e-8);
        CHECK_NEAR(launch.length(), initial.length(), 1e-8);
        rig.world.get<Transform>(target).position = origin - side * distance;
        movement.runWorldPhase(rig.world, terrain, now + 50, net::kTickSeconds);
        CHECK_NEAR(angleDelta(launch.angle(), rig.world.get<Motion>(shot).velocity.angle()), 0.0, 1e-8);
    };
    checkLaunch("magic_missile", Rarity::Apex, Rarity::Apex, 574, true);
    checkLaunch("magic_missile", Rarity::Apex, Rarity::Apex, 576, false);
    checkLaunch("magic_missile", Rarity::Apex, Rarity::Unique, 400, false);
    checkLaunch("magic_missile", Rarity::Unique, Rarity::Apex, 501, false);
    checkLaunch("magic_electron_missile", Rarity::Unique, Rarity::Unique, 379, true);
    checkLaunch("magic_electron_missile", Rarity::Unique, Rarity::Unique, 381, false);
    checkLaunch("void_gamma_ray", Rarity::Unique, Rarity::Unique, 200, false);
}

TEST(zorr_reference_gunpowder_fivefold_contact_damage_requires_live_burn) {
    double plain=0;
    for (int burn : {0,1,2,3,4}) {
        ArcticBlast rig("void_gunpowder");
        const auto petals = rig.liveIn(0);
        CHECK(!petals.empty());
        if (petals.empty()) continue;
        const auto victim=rig.mob(rig.world.get<Transform>(petals.front()).position);
        if (burn == 1 || burn == 2)
            rig.world.ensure<Afflictions>(victim).burns.push_back(
                {rig.player, rig.player, 0.001, burn == 1 ? 9000.0 : 3999.0});
        if (burn == 3 || burn == 4)
            rig.world.add<Burn>(victim, Burn{rig.player, 0.001, 3900.0,
                                              burn == 3 ? 6900.0 : 3999.0});
        rig.combatTick(4050);
        const double hit=1000.0-rig.world.get<Health>(victim).current;
        if (!burn) plain=hit;
        CHECK(plain>0);
        CHECK_NEAR(hit,plain * (burn == 1 || burn == 3 ? 5 : 1),0.002);
    }
}

TEST(zorr_reference_mask_powder_and_slime_use_the_standard_combat_stats) {
    ArcticBlast rig("void_slime", Rarity::Rare);
    rig.equip(1,"void_mask");
    rig.equip(2,"void_powder");
    rig.tickPetals(4500);
    const auto& mods=rig.world.get<PlayerModifiers>(rig.player);
    CHECK_NEAR(mods.poisonArmor,1.0,1e-6);
    CHECK_NEAR(mods.speedScale,1.04,1e-6);
    const auto slime=rig.registry.petalIndex("void_slime");
    CHECK_EQ(rig.liveIn(0).size(),std::size_t(2));
    CHECK_NEAR(rig.registry.petalStats(slime,Rarity::Rare).knockback,20.0,1e-6);
    CHECK_NEAR(rig.registry.petalStats(slime,Rarity::Apex).knockback,148.0,1e-6);
}

TEST(feedback_scar_shortens_each_eligible_undead_window_only_once) {
    ArcticBlast rig("void_scar", Rarity::Rare);
    rig.tickPetals(65000.0);
    const auto petals = rig.liveIn(0);
    CHECK(!petals.empty());
    if (petals.empty()) return;
    const auto at = rig.world.get<Transform>(petals.front()).position;

    const auto beetle = rig.mob(at + Vec2{2,0});
    rig.world.add<MobType>(beetle, MobType{rig.registry.mobIndex("mummy_beetle"), Rarity::Rare});
    rig.world.add<Undead>(beetle, Undead{70000.0});
    rig.combatTick(66000.0);
    CHECK_NEAR(rig.world.get<Undead>(beetle).untilMillis, 69000.0, 1e-6);
    CHECK(rig.world.get<Undead>(beetle).shortenedByScar);

    rig.world.get<Health>(petals.front()).current = 1e6;
    rig.world.get<Transform>(beetle).position = at + Vec2{2,0};
    rig.combatTick(67000.0);
    CHECK_NEAR(rig.world.get<Undead>(beetle).untilMillis, 69000.0, 1e-6);

    rig.world.destroy(beetle);
    rig.world.get<Faction>(rig.player).friendlyFireEnabled = true;
    const auto enemy = rig.world.create();
    rig.world.add<PlayerTag>(enemy);
    rig.world.add<Faction>(enemy, Faction{Team::Players,true});
    rig.world.add<Health>(enemy, Health{10000.0,10000.0});
    rig.world.add<Transform>(enemy, Transform{at+Vec2{2,0},0});
    rig.world.add<Body>(enemy, Body{5,1});
    rig.world.add<Undead>(enemy, Undead{72000.0,NULL_ENTITY,0.0,1.0,rarityIndex(Rarity::Epic)});
    rig.combatTick(68000.0);
    CHECK_NEAR(rig.world.get<Undead>(enemy).untilMillis,72000.0,1e-6);
    rig.world.get<Undead>(enemy).bandageTier = rarityIndex(Rarity::Rare);
    rig.world.get<Transform>(enemy).position = at + Vec2{2,0};
    rig.combatTick(69000.0);
    CHECK_NEAR(rig.world.get<Undead>(enemy).untilMillis,71250.0,1e-6);
}

TEST(feedback_wild_stingers_push_flowers_without_changing_hit_damage) {
    for (const char* ammo : {"hornet_missile","wasp_missile","stinger"}) {
        for (const bool wild : {false,true}) {
            ArcticBlast rig("basic");
            const auto shooter = rig.world.create();
            if (wild) rig.world.add<MobTag>(shooter);
            rig.world.add<Faction>(shooter, Faction{Team::Hostiles,false});
            rig.world.add<Transform>(shooter, Transform{{4900,5000},0});
            const auto shot = rig.world.create();
            Projectile projectile;
            projectile.owner = shooter;
            projectile.damage = 10.0;
            projectile.remainingDistance = 1000.0;
            projectile.petalConfigIndex = rig.registry.petalIndex(ammo);
            projectile.lastPosition = {4965,5000};
            rig.world.add<ProjectileTag>(shot);
            rig.world.add<Projectile>(shot, projectile);
            rig.world.add<Transform>(shot, Transform{{4980,5000},0});
            rig.world.add<Motion>(shot, Motion{{400,0}});
            rig.world.add<Body>(shot, Body{4,1});
            rig.world.add<Health>(shot, Health{1000,1000});
            rig.world.add<Faction>(shot, Faction{Team::Hostiles,false});
            rig.combatTick(5000.0);
            CHECK_NEAR(rig.world.get<Health>(rig.player).current, 100.0, 1e-6);
            const auto* kb = rig.world.tryGet<Knockback>(rig.player);
            const double push = kb ? kb->impulse.x : 0.0;
            CHECK_NEAR(push, wild && std::string_view(ammo) != "stinger" ? 48.0 : 5.0, 1e-6);
        }
    }
}

TEST(fluid_spills_reach_buffed_range_and_record_training_damage) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("acid");
    rig.equip(1, "void_soap", Rarity::Super);
    rig.world.add<PlayerSkillTree>(rig.player).skills.set(SkillId::FluidReach, 2);
    rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
    for (int now = 4050; now <= 6100; now += 50) rig.tickPetals(now);
    Entity field = NULL_ENTITY;
    Query<GroundEffect>{rig.world}.each([&](Entity e, GroundEffect& spill) {
        if (spill.kind == GroundEffectKind::AcidSpill && field == NULL_ENTITY) field = e;
    });
    CHECK(field != NULL_ENTITY);
    if (field == NULL_ENTITY) return;
    for(int spread=1;spread<=7;++spread)rig.combatTick(5700+spread*50);
    const auto radius = rig.world.get<GroundEffect>(field).radius;
    CHECK_NEAR(radius, (57.5 * 1.20 * 1.375 * 1.93 - 5.5 + std::min(5.5, 0.35 * 11 * 1.688)) * 1.30, 1e-6);
    const auto centre = rig.world.get<Transform>(field).position;
    const auto wild = rig.mob(centre + Vec2{100, 0});
    const auto outside = rig.mob(centre + Vec2{0, 500});
    auto dummy = rig.mob(centre + Vec2{0, 100});
    rig.world.remove<MobTag>(dummy);
    rig.world.add<NpcTag>(dummy);
    const auto dummyIndex = content().mobIndex("target_dummy");
    CHECK(dummyIndex != kInvalidIndex);
    rig.world.add<Npc>(dummy).configIndex = dummyIndex;
    rig.combatTick(6150);
    CHECK(rig.world.get<Health>(wild).current < 1000.0);
    CHECK_EQ(rig.world.get<Health>(outside).current, 1000.0);
    CHECK_EQ(rig.world.get<Health>(dummy).current, 1000.0);
    const auto damage = rig.combat.practiceDamage(rig.player, 6150, dummy);
    CHECK(!damage.empty());
    if (!damage.empty()) CHECK(damage.front().amount > 0.0);
    std::printf("Acid radius %.3f, wild damage %.3f, training damage %.3f\n", radius,
                1000.0 - rig.world.get<Health>(wild).current,
                damage.empty() ? 0.0 : damage.front().amount);
}

TEST(fluid_spills_quality_grows_and_soap_stacks_with_talent) {
    ArcticBlast rig("acid", Rarity::Super);
    rig.equip(1, "void_soap", Rarity::Super);
    rig.equip(2, "void_soap", Rarity::Super);
    rig.world.add<PlayerSkillTree>(rig.player).skills.set(SkillId::FluidReach, 2);
    rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
    for (int now = 4050; now <= 6100; now += 50) rig.tickPetals(now);
    Entity zone = NULL_ENTITY;
    Query<GroundEffect>{rig.world}.each([&](Entity e, GroundEffect& effect) {
        if (effect.kind == GroundEffectKind::AcidSpill && zone == NULL_ENTITY) zone = e;
    });
    CHECK(zone != NULL_ENTITY);
    if (zone == NULL_ENTITY) return;
    for(int spread=1;spread<=7;++spread)rig.combatTick(5700+spread*50);
    CHECK_NEAR(rig.world.get<GroundEffect>(zone).radius, (92.5 * 1.20 * 1.375 * 2.86 - 23.0 + std::min(23.0, 0.35 * 46 * 2.376)) * 1.30, 1e-6);
    const Vec2 at = rig.world.get<Transform>(zone).position;
    const auto near = rig.mob(at + Vec2{0, 200});
    const auto far = rig.mob(at + Vec2{0, 650});
    rig.world.get<Health>(near) = {1e9, 1e9};
    rig.combatTick(6150);
    CHECK(rig.world.get<Health>(near).current < 1e9);
    CHECK_EQ(rig.world.get<Health>(far).current, 1000.0);
}

TEST(fluid_spills_oil_doubles_for_both_burn_implementations) {
    for (bool stacked : {false, true}) {
        ArcticBlast rig("basic");
        const auto& oil = rig.registry.petal(rig.registry.petalIndex("oil")).spill;
        const Vec2 at{5500, 5500};
        const auto cold = rig.mob(at + Vec2{15, 0});
        const auto burning = rig.mob(at + Vec2{-15, 0});
        if (stacked) CombatSystem::ignite(rig.world, burning, rig.player, rig.player, 5.0, 1000.0, 4000.0);
        else {
            auto& status = rig.world.ensure<Afflictions>(burning);
            status.burnPerSecond = 5.0;
            status.burnSource = rig.player;
            status.burnUntilMillis = 5000.0;
        }
        CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, oil, 1.0, 0);
        rig.combatTick(4050);
        CHECK_NEAR(1000.0 - rig.world.get<Health>(cold).current, oil.strength * 2.1875 * net::kTickSeconds, 1e-6);
        CHECK_NEAR(1000.0 - rig.world.get<Health>(burning).current,
                   (2.0 * oil.strength * 2.1875 + 5.0) * net::kTickSeconds, 1e-6);
    }
}

TEST(reference_cross_defense_teleports_and_spends_one_cast) {
    ArcticBlast rig("void_cross");
    auto petals=rig.liveIn(0);
    CHECK_EQ(petals.size(),std::size_t(1));
    if(petals.empty()) return;
    rig.world.get<Transform>(petals.front()).position={5200,5000};
    rig.world.get<PlayerInput>(rig.player).current.flags=net::InputDefend;
    rig.tickPetals(5100);
    CHECK(distanceSq(rig.world.get<Transform>(rig.player).position,Vec2{5000,5000}) > 1.0);
    CHECK(rig.world.has<Dead>(petals.front()) ||
          rig.world.get<Health>(petals.front()).current<=0);
}

TEST(reference_cross_attack_and_neutral_do_not_teleport) {
    for(int flags : {0,int(net::InputAttack)}) {
        ArcticBlast rig("void_cross");
        auto petals=rig.liveIn(0);
        CHECK(!petals.empty());
        if(petals.empty()) continue;
        rig.world.get<Transform>(petals.front()).position={5200,5000};
        rig.world.get<PlayerInput>(rig.player).current.flags=flags;
        rig.tickPetals(5100);
        CHECK_NEAR(rig.world.get<Transform>(rig.player).position.x,5000,1e-6);
    }
}

TEST(passive_mana_petals_refill_the_innate_pool_without_capacity_except_soil) {
    for (const auto* id : {"void_elemental", "magic_basil", "magic_palm_leaf", "magic_soil"}) {
        ArcticBlast rig(id);
        const auto* pool = rig.world.tryGet<ManaPool>(rig.player);
        CHECK(pool && pool->has());
        if (!pool) continue;
        const auto index = rig.registry.petalIndex(id);
        const auto stats = rig.registry.petalStats(index, Rarity::Common);
        CHECK_NEAR(pool->max, kFlowerBaseMana + stats.maxMana, 1e-6);
        const bool basil = std::string_view(id) == "magic_basil";
        rig.world.get<ManaPool>(rig.player).current = 0;
        rig.tickPetals(4050);
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current,
                   stats.passiveManaPerSecond * net::kTickSeconds * (basil ? 2.0 : 1.0), 1e-6);
        rig.world.get<Loadout>(rig.player).slots[0] = {};
        rig.tickPetals(4100);
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).max, kFlowerBaseMana, 1e-6);
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current,
                   stats.passiveManaPerSecond * net::kTickSeconds * (basil ? 2.0 : 1.0), 1e-6);
    }
}

TEST(reference_elemental_equipment_restores_resources_and_removes_bonuses_on_unequip) {
    ArcticBlast rig("void_elemental",Rarity::Super,"mana_orb");
    const double initialHealth=rig.world.get<Health>(rig.player).max*0.5;
    rig.world.get<Health>(rig.player).current=initialHealth;
    rig.world.ensure<ShieldState>(rig.player).amount=0;
    auto& mana=rig.world.get<ManaPool>(rig.player);
    mana.current=0;
    rig.tickPetals(4050);
    CHECK(rig.world.get<Health>(rig.player).current>initialHealth);
    CHECK(rig.world.get<ShieldState>(rig.player).amount>0);
    CHECK(rig.world.get<ManaPool>(rig.player).current>0);
    CHECK(rig.world.get<PlayerModifiers>(rig.player).petalDamageScale>1);
    CHECK(rig.world.get<PlayerModifiers>(rig.player).speedScale>1);
    const auto index=rig.registry.petalIndex("void_elemental");
    const auto& config=rig.registry.petal(index);
    CHECK(config.burnPerSecond>0);
    CHECK(rig.registry.petalStats(index,Rarity::Super).poisonPerSecond>0);
    rig.world.get<Loadout>(rig.player).slots[0]={};
    rig.tickPetals(4100);
    CHECK_NEAR(rig.world.get<PlayerModifiers>(rig.player).petalDamageScale,1,1e-6);
    CHECK_NEAR(rig.world.get<PlayerModifiers>(rig.player).speedScale,1,1e-6);
}

TEST(reference_elemental_contact_applies_both_poison_and_burn) {
    ArcticBlast rig("void_elemental");
    const auto petals = rig.liveIn(0);
    CHECK_EQ(petals.size(), 1u);
    if (petals.size() != 1u) return;
    const Vec2 at{5300, 5300};
    const Entity victim = rig.mob(at);
    rig.world.get<Transform>(petals.front()).position = at;
    rig.combatTick(4050);
    const auto* effects = rig.world.tryGet<Afflictions>(victim);
    CHECK(effects);
    if (!effects) return;
    CHECK_EQ(effects->poisonStacks.size(), 1u);
    CHECK_EQ(effects->burns.size(), 1u);
    CHECK_NEAR(effects->poisonPerSecond, 10.0, 1e-6);
    if (!effects->burns.empty()) CHECK_NEAR(effects->burns.front().perSecond, 7.0, 1e-6);
    CHECK(effects->frostSpeed(4050) < 1.0);
    CHECK_EQ(effects->frostSpeed(5050), 1.0);
    CHECK_NEAR(1000.0 - rig.world.get<Health>(victim).current, 47.0, 1e-6);
}

TEST(reference_scar_charges_before_teleporting_in_the_same_realm) {
    ArcticBlast rig("void_scar");
    rig.tickPetals(300000);
    CHECK(!rig.liveIn(0).empty());
    rig.tickPetals(301000);
    CHECK_NEAR(rig.world.get<Transform>(rig.player).position.x,5000,1e-6);
    const auto realm=rig.world.get<Transform>(rig.player).realm;
    rig.tickPetals(307000);
    const auto& moved=rig.world.get<Transform>(rig.player);
    CHECK(moved.realm==realm);
    CHECK(distanceSq(moved.position,Vec2{5000,5000})>1.0);
}

TEST(reference_landmine_places_a_stationary_trap_and_explodes_once) {
    ArcticBlast rig("void_landmine");
    for(int now=4050;now<=10000;now+=50) rig.tickPetals(now);
    std::vector<Entity> traps;
    Query<PlantedMine>{rig.world}.without<Dead>().collect(traps);
    CHECK(!traps.empty());
    if(traps.empty()) return;
    const Entity trap=traps.front();
    const Vec2 at=rig.world.get<Transform>(trap).position;
    const auto target=rig.mob(at);
    rig.world.add<Motion>(target);
    rig.tickPetals(10050);
    CHECK(rig.world.has<Dead>(trap));
    rig.combatTick(10050);
    CHECK(rig.world.get<Health>(target).current<1000);
    CHECK(rig.world.has<Knockback>(target));
    rig.tickPetals(10100);
}

TEST(apex_magic_air_extends_a_regular_petal_to_real_contact) {
    double threshold = 0.0;
    for (const bool inflated : {false, true}) {
        ArcticBlast rig("basic");
        if (inflated) rig.equip(1, "magic_air", Rarity::Ascendant);
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        for (int time = 4050; time <= 12000; time += 50) rig.tickPetals(time);
        const auto ring = rig.liveIn(0);
        CHECK_EQ(ring.size(), 1u);
        if (ring.empty()) return;
        const auto flower = rig.world.get<Transform>(rig.player).position;
        const auto at = rig.world.get<Transform>(ring.front()).position;
        const double contactReach = distance(at, flower) + rig.world.get<Body>(ring.front()).radius + 5.0;
        if (!inflated) threshold = contactReach + 1.5;
        const Entity mob = rig.mob(flower + (at - flower).normalized() * threshold);
        const auto hp = rig.world.get<Health>(mob).current;
        rig.combatTick(12050);
        const auto damage = hp - rig.world.get<Health>(mob).current;
        const double mana = rig.world.get<ManaPool>(rig.player).max;
        std::printf("Apex Magic Air worn=%d radius=%.3f reach=%.3f target=%.3f damage=%.3f mana=%.1f\n",
                    inflated, rig.world.get<Body>(rig.player).radius, contactReach, threshold, damage, mana);
        CHECK_NEAR(mana, kFlowerBaseMana, 1e-8);
        CHECK_EQ(damage > 0.0, inflated);
    }
}

TEST(pyrite_deals_full_burn_on_entry_and_each_equipped_petal_contributes) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    if (!error.empty()) return;
    double singleLoss = 0.0;
    for (const int count : {1, 2}) {
        ArcticBlast rig("pyrite", Rarity::Ascendant);
        if (count == 2) rig.equip(1, "pyrite", Rarity::Ascendant);
        for (int now = 4050; now <= 12000; now += 50) {
            rig.tickPetals(now);
            rig.combatTick(now);
        }
        const Vec2 origin = rig.world.get<Transform>(rig.player).position;
        const auto target = rig.mob(origin + Vec2{0, 360});
        const auto outside = rig.mob(origin + Vec2{1000, 0});
        const auto dummy = rig.dummy(origin + Vec2{0, -360});
        constexpr double health = 1e9;
        const auto addPlayer = [&](Vec2 at, bool pvp) {
            const Entity e = rig.world.create();
            rig.world.add<PlayerTag>(e);
            rig.world.add<Transform>(e, Transform{at, 0});
            rig.world.add<Body>(e, Body{25, 1});
            rig.world.add<Health>(e, Health{health, health});
            rig.world.add<Faction>(e, Faction{Team::Players, pvp});
            return e;
        };
        const Entity hostile = addPlayer(origin + Vec2{360, 0}, true);
        const Entity ally = addPlayer(origin + Vec2{-360, 0}, false);
        for (const auto e : {target, outside}) rig.world.get<Health>(e) = {health, health};
        for (int now = 12050; now <= 12400; now += 50) {
            rig.tickPetals(now);
            rig.combatTick(now);
        }
        const auto index = rig.registry.petalIndex("pyrite");
        const auto& config = rig.registry.petal(index);
        const double rate = config.radiation.burnDamage *
            referenceOffenseScale(config, Rarity::Ascendant) * kRadiationDamageScale *
            (2000.0 / kFireBombBurnMillis);
        const double loss = health - rig.world.get<Health>(target).current;
        CHECK_NEAR(loss, count * rate * 0.35, 1e-5);
        if (count == 1) singleLoss = loss;
        else CHECK_NEAR(loss, singleLoss * 2, 1e-5);
        CHECK_NEAR(rig.world.get<Health>(outside).current, health, 1e-6);
        CHECK_NEAR(rig.world.get<Health>(ally).current, health, 1e-6);
        CHECK_NEAR(health - rig.world.get<Health>(hostile).current, loss, 1e-5);
        double meter = 0.0;
        for (const auto& row : rig.combat.practiceDamage(rig.player, 12400, dummy)) meter += row.amount;
        CHECK_NEAR(meter, loss, 1e-5);
        const auto* afflictions = rig.world.tryGet<Afflictions>(target);
        CHECK(afflictions && afflictions->burns.size() == static_cast<std::size_t>(count));

        rig.world.get<Transform>(target).position = origin + Vec2{1100, 0};
        for (int now = 12450; now <= 15550; now += 50) {
            rig.tickPetals(now);
            rig.combatTick(now);
        }
        const double afterBurn = rig.world.get<Health>(target).current;
        CHECK(afterBurn < health - loss);
        rig.tickPetals(15600);
        rig.combatTick(15600);
        CHECK_NEAR(rig.world.get<Health>(target).current, afterBurn, 1e-6);
    }
}

TEST(pyrite_tooltip_shows_burn_damage_per_second_and_its_linger) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    if (!error.empty()) return;
    const auto index = content().petalIndex("pyrite");
    const auto previousLanguage = ui::language();
    for (const auto language : {ui::Language::En, ui::Language::Zh}) {
        ui::setLanguage(language);
        const auto lines = ui::petalTooltipLines(index, Rarity::Ascendant, SkillSet{}, 1.0);
        const bool chinese = language == ui::Language::Zh;
        const auto has = [&](std::string_view text) {
            return std::any_of(lines.begin(), lines.end(), [&](const auto& line) {
                return line.text.find(text) != std::string::npos;
            });
        };
        CHECK(has(chinese ? "基础灼烧伤害/秒" : "Base burn damage per second"));
        CHECK(has(chinese ? "离开火圈后的灼烧持续" : "Burn after leaving the ring"));
        CHECK(!has(chinese ? "基础辐射灼烧强度" : "Base radiation burn strength"));
    }
    ui::setLanguage(previousLanguage);
}

TEST(reference_pyrite_radiates_without_spending_the_petal) {
    ArcticBlast rig("pyrite");
    rig.tickPetals(12000);
    CHECK(!rig.liveIn(0).empty());
    const auto& config=rig.registry.petal(rig.registry.petalIndex("pyrite"));
    CHECK(config.radiation.present);
    CHECK(config.spill.kind==SpillKind::None);
    CHECK_NEAR(baseRadiationRadius(config,Rarity::Super),270,1e-6);
    CHECK_NEAR(baseRadiationRadius(config,Rarity::Unique),438,1e-6);
    bool found=false;
    Query<GroundEffect>{rig.world}.each([&](Entity,GroundEffect& effect){
        if(effect.kind==GroundEffectKind::Radiation) {
            found=true;
            CHECK_NEAR(effect.burnPerSecond,30,1e-6);
            CHECK_NEAR(effect.radius,270,1e-6);
        }
    });
    CHECK(found);
}

TEST(reference_radiation_and_fluids_receive_the_authorized_buff) {
    ArcticBlast rig("basic");
    for(auto kind : {SpillKind::Acid,SpillKind::Lava,SpillKind::Oil,SpillKind::Blood,SpillKind::Elixir}) {
        SpillSpec spec;
        spec.kind=kind;spec.radius=50;spec.strength=10;spec.durationMillis=2000;
        CombatSystem::createSpill(rig.world,rig.player,{5400,5400},Rarity::Common,spec,1,0);
    }
    for (int tick = 1; tick <= 7; ++tick) rig.combatTick(4000 + tick * 50);
    int fields=0;
    Query<GroundEffect>{rig.world}.each([&](Entity,GroundEffect& field){
        ++fields;
        const bool offensive = field.kind == GroundEffectKind::AcidSpill ||
            field.kind == GroundEffectKind::LavaSpill || field.kind == GroundEffectKind::OilSpill;
        const bool blood = field.kind == GroundEffectKind::Blood;
        CHECK_NEAR(field.radius, blood ? 44.16 : (offensive ? 82.5 : 60.0), 1e-6);
        CHECK_NEAR(field.damagePerSecond, blood ? 0.0 : (offensive ? 21.875 : 10.0), 1e-6);
        if (blood) CHECK_NEAR(field.armorReduction, 10.0, 1e-6);
    });
    CHECK_EQ(fields,5);
    const auto& pyro=rig.registry.petal(rig.registry.petalIndex("pyrite"));
    CHECK_NEAR(baseRadiationRadius(pyro,Rarity::Common),270,1e-6);
}

TEST(reference_radiation_offense_scales_at_common_and_hyper_without_extra_self_harm) {
    for (const char* id : {"uranium", "void_corium", "void_stardust", "pyrite", "arctic_plutonium", "arctic_corium"}) {
        for (Rarity rarity : {Rarity::Common, Rarity::Apex}) {
            ArcticBlast rig(id, rarity);
            rig.tickPetals(300000);
            rig.tickPetals(301000);
            const auto& config=rig.registry.petal(rig.registry.petalIndex(id));
            const auto stats=rig.registry.petalStats(rig.registry.petalIndex(id),rarity);
            const auto& spec=config.radiation;
            const double strength=petalStrengthScale(rarity);
            const double offenseRatio=referenceOffenseScale(config,rarity)/strength;
            const double scale=rig.world.get<PlayerModifiers>(rig.player).petalDamageScale;
            const double previousPulse=spec.burnDamage>0 ? spec.poisonDamage*strength*scale :
                (spec.poisonPerSecond>0 ? spec.poisonPerSecond*strength : stats.damage)*scale;
            const bool uranium=config.id=="uranium";
            const double expectedDamage = 1.25 * (uranium
                ? uraniumRadiationDps(rig.registry, rarity) * scale : previousPulse * offenseRatio);
            const double step=config.id=="pyrite"?140:config.id=="arctic_plutonium"?200:(config.id=="arctic_corium" || config.id=="void_corium")?150:0;
            const double oldRadius=spec.radius+(config.id=="void_stardust"?12.5*ladderIndex(rarity):0)+step*std::max(0,ladderIndex(rarity)-7);
            int fields=0;
            Query<GroundEffect>{rig.world}.each([&](Entity,GroundEffect& effect){
                ++fields;
                CHECK_NEAR(effect.radius,oldRadius*1.20*(uranium && rarity==Rarity::Apex?1.25:1.0),1e-5);
                CHECK_NEAR(effect.damagePerSecond,expectedDamage,std::max(1e-6,expectedDamage*1e-12));
                CHECK_NEAR(effect.damagePerHit,spec.pulseDamage*strength*scale*1.25*offenseRatio,std::max(1e-6,effect.damagePerHit*1e-12));
                const double burn = spec.burnDamage * strength * scale * 1.25 * offenseRatio;
                if (config.id == "pyrite")
                    CHECK_NEAR(effect.burnPerSecond, burn * (2000.0 / kFireBombBurnMillis), std::max(1e-6, burn * 1e-12));
                else
                    CHECK_NEAR(effect.burnDamage, burn, std::max(1e-6, burn * 1e-12));
                const double oldSelf=previousPulse*spec.selfPoisonScale;
                CHECK_NEAR(effect.ownerPoisonPerSecond,uranium?std::min(oldSelf,rig.world.get<Health>(rig.player).max*.04):oldSelf,1e-6);
            });
            CHECK(fields>0);
            if(uranium) {
                rig.combatTick(301000);
                const auto* poison=rig.world.tryGet<Afflictions>(rig.player);
                CHECK(poison!=nullptr);
                if(poison) CHECK_NEAR(poison->poisonPerSecond,std::min(previousPulse*spec.selfPoisonScale,rig.world.get<Health>(rig.player).max*.04),1e-6);
            }
        }
    }
}

TEST(reference_lithium_reaches_new_outer_band_and_deals_buffed_lightning_damage) {
    ArcticBlast rig("void_lithium");
    rig.tickPetals(300000);
    auto active=rig.liveIn(0);CHECK(!active.empty());if(active.empty())return;
    Vec2 at=rig.world.get<Transform>(active.front()).position;
    auto target=rig.mob(at+Vec2{500,0});
    auto distant=rig.mob(at+Vec2{2500,0});
    rig.tickPetals(301000);
    rig.combatTick(301000);
    CHECK_NEAR(rig.world.get<Health>(target).current,960,1e-6);
    CHECK_NEAR(rig.world.get<Health>(distant).current,1000,1e-6);
}

TEST(reference_nut_requires_three_equipped_slots_of_one_rarity) {
 ArcticBlast r("void_nut");CHECK(r.liveIn(0).empty());
 r.equip(1,"void_nut");r.equip(2,"void_nut",Rarity::Rare);r.tickPetals(10000);r.tickPetals(20000);
 CHECK(r.liveIn(0).empty());CHECK(r.liveIn(1).empty());CHECK(r.liveIn(2).empty());
 r.equip(2,"void_nut");r.tickPetals(20100);r.tickPetals(30000);
 CHECK_EQ(r.liveIn(0).size(),1);CHECK_EQ(r.liveIn(1).size(),1);CHECK_EQ(r.liveIn(2).size(),1);
 r.world.get<Loadout>(r.player).slots[2]={};r.tickPetals(30100);
 CHECK(r.liveIn(0).empty());CHECK(r.liveIn(1).empty());
}
TEST(reference_sapphire_drops_same_tier_only_and_does_not_follow_owner) {
 ArcticBlast r("arctic_sapphire");r.equip(1,"basic");r.equip(2,"basic",Rarity::Rare);
 r.tickPetals(10000);r.tickPetals(20000);
 auto same=r.liveIn(1),other=r.liveIn(2);CHECK(!same.empty());CHECK(!other.empty());
 if(same.empty()||other.empty())return;
 CHECK(r.world.get<PetalInstance>(same.front()).sapphireGrounded);
 CHECK(!r.world.get<PetalInstance>(other.front()).sapphireGrounded);
 Vec2 position=r.world.get<Transform>(same.front()).position;
 r.world.get<Transform>(r.player).position+=Vec2{200,100};r.tickPetals(20050);
 CHECK_NEAR(distanceSq(position,r.world.get<Transform>(same.front()).position),0,1e-6);
}
TEST(reference_relativity_accelerates_existing_reload_in_motion) {
 ArcticBlast r("basic");r.equip(1,"void_relativity");r.tickPetals(5000);
 auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 r.world.get<Health>(live.front()).current=0;r.world.add<Dead>(live.front());r.tickPetals(5050);
 double before=r.world.get<Loadout>(r.player).slots[0].reloadReadyAtMillis;
 r.tickPetals(5100);CHECK_NEAR(r.world.get<Loadout>(r.player).slots[0].reloadReadyAtMillis,before,1e-6);
 r.world.get<Motion>(r.player).velocity={5*kPlayerMaxSpeed,0};r.tickPetals(5150);
 CHECK_NEAR(r.world.get<Loadout>(r.player).slots[0].reloadReadyAtMillis,before-4,1e-6);
}
TEST(reference_mine_is_breakable_and_breaking_triggers_exactly_one_blast) {
 ArcticBlast r("void_landmine");r.tickPetals(10000);r.tickPetals(11000);
 std::vector<Entity> mines;Query<PlantedMine>{r.world}.without<Dead>().collect(mines);CHECK(!mines.empty());if(mines.empty())return;
 Entity mine=mines.front();Vec2 at=r.world.get<Transform>(mine).position;
 CHECK_NEAR(r.world.get<Health>(mine).current,1,1e-6);
 Entity target=r.mob(at+Vec2{30,0}),attacker=r.mob(at+Vec2{500,0});
 r.grid.clear();Query<Transform,Body>{r.world}.each([&](Entity e,Transform& t,Body& b){r.grid.insert(e,t.realm,t.position,b.radius);});
 r.combat.beginTick(r.world,r.registry,11100,net::kTickSeconds,r.events);
 auto hit=r.combat.applyDamage(r.world,mine,attacker,2,11100);CHECK(hit.killed);
 r.combat.runWorldPhase(r.world,r.grid,r.registry,11100,net::kTickSeconds);
 CHECK_NEAR(r.world.get<Health>(target).current,700,1e-6);
 r.combatTick(11150);CHECK_NEAR(r.world.get<Health>(target).current,700,1e-6);
}
TEST(reference_gas_bomb_leaves_real_poison) {
 ArcticBlast r("gas_bomb");auto at=r.armBomb();Entity mob=r.mob(at);r.combatTick(4100);
 CHECK(r.world.has<Afflictions>(mob));if(!r.world.has<Afflictions>(mob))return;
 CHECK(r.world.get<Afflictions>(mob).poisoned(4100));
 double before=r.world.get<Health>(mob).current;r.combatTick(4150);CHECK(r.world.get<Health>(mob).current<before);
}

TEST(reference_antimatter_explodes_on_actual_flower_death_once) {
 ArcticBlast r("void_antimatter");Vec2 origin=r.world.get<Transform>(r.player).position;
 Entity target=r.mob(origin+Vec2{100,0}),outside=r.mob(origin+Vec2{400,0}),killer=r.mob(origin+Vec2{600,0});
 r.world.get<Health>(target)={5000,5000};
 r.grid.clear();Query<Transform,Body>{r.world}.each([&](Entity e,Transform& t,Body& b){r.grid.insert(e,t.realm,t.position,b.radius);});
 r.combat.beginTick(r.world,r.registry,5000,net::kTickSeconds,r.events);
 CHECK(r.combat.applyDamage(r.world,r.player,killer,100000,5000).killed);
 r.combat.runWorldPhase(r.world,r.grid,r.registry,5000,net::kTickSeconds);
 CHECK_NEAR(r.world.get<Health>(target).current,3800,1e-6);
 CHECK_NEAR(r.world.get<Health>(outside).current,1000,1e-6);
 r.combatTick(5050);CHECK_NEAR(r.world.get<Health>(target).current,3800,1e-6);
}
TEST(reference_kitty_requires_full_body_outside_every_observers_view_and_matching_tier) {
 std::string error;
 if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
 ArcticBlast r("basic");r.equip(1,"void_kitty");r.tickPetals(5000);
 auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;Entity source=live.front();
 r.world.get<PlayerModifiers>(r.player).luck=8;
 auto target=r.mob({7000,5000});r.world.get<Health>(target)={10000,10000};
 r.world.get<Transform>(source).position={7000,5000};
 CHECK_NEAR(r.combat.applyDamage(r.world,target,source,10,5000).applied,40,1e-6);
 r.world.get<Transform>(source).position={5900,5000};
 CHECK_NEAR(r.combat.applyDamage(r.world,target,source,10,5050).applied,10,1e-6);
 r.world.get<Transform>(source).position={5960,5000};
 CHECK_NEAR(r.combat.applyDamage(r.world,target,source,10,5100).applied,10,1e-6);
 r.world.get<Transform>(source).position={7000,5000};
 Entity observer=r.world.create();r.world.add<PlayerTag>(observer);r.world.add<Transform>(observer,Transform{{7000,5000},0});r.world.add<Health>(observer,Health{100,100});
 CHECK_NEAR(r.combat.applyDamage(r.world,target,source,10,5150).applied,10,1e-6);
 r.world.get<Transform>(observer).realm=Realm::Arena;
 r.world.get<PetalInstance>(source).rarity=Rarity::Rare;
 CHECK_NEAR(r.combat.applyDamage(r.world,target,source,10,5200).applied,10,1e-6);
 r.equip(2, "mimic", Rarity::Rare);
 CHECK_NEAR(r.combat.applyDamage(r.world,target,source,10,5250).applied,40,1e-6);
}

TEST(reference_blood_rose_delivers_both_health_and_shield_and_timed_blindness) {
 ArcticBlast r("blood_rose");r.world.get<Health>(r.player).current=50;
 auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 r.world.get<Transform>(live.front()).position=r.world.get<Transform>(r.player).position;
 for(int now=4500;now<=5500;now+=50)r.tickPetals(now);
 CHECK(r.world.get<Health>(r.player).current>90);
 CHECK(r.world.get<Health>(r.player).current<100);
 CHECK_NEAR(r.world.get<ShieldState>(r.player).amount,6,1e-6);
 CHECK(computePlayerVisuals(r.world,r.player,5500).renderFlags&PlayerRenderBlinded);
 CHECK(!(computePlayerVisuals(r.world,r.player,r.world.get<Afflictions>(r.player).blindUntilMillis+1).renderFlags&PlayerRenderBlinded));
}
TEST(reference_bugatti_preserves_top_speed_bonus_and_slippery_response) {
 ArcticBlast r("bugatti");const auto mods=r.world.get<PlayerModifiers>(r.player);
 CHECK_NEAR(mods.speedScale,1.0+30.0/kPlayerMaxSpeed,1e-6);
 CHECK_NEAR(mods.accelerationScale,.65,1e-6);
 MoveState plain{{0,0},{0,0}},slippery=plain;Vec2 target{100,0};
 integrateVelocity(plain,target,.05);integrateVelocity(slippery,target,.05*mods.accelerationScale);
 CHECK(slippery.velocity.x<plain.velocity.x);
 plain.velocity=slippery.velocity={100,0};integrateVelocity(plain,{},.05);integrateVelocity(slippery,{},.05*mods.accelerationScale);
 CHECK(slippery.velocity.x>plain.velocity.x);
}
TEST(reference_vampire_fang_heals_after_incoming_body_damage_and_respects_no_heal) {
 ArcticBlast r("vampire_fang");r.world.get<Health>(r.player).current=50;
 auto enemy=r.mob({5500,5000});r.combat.applyDamage(r.world,r.player,enemy,10,5000);
 CHECK_NEAR(r.world.get<Health>(r.player).current,45.4,1e-6);
 r.combat.applyNoHeal(r.world,r.player,1000,6000);
 r.combat.applyDamage(r.world,r.player,enemy,10,6000);
 CHECK_NEAR(r.world.get<Health>(r.player).current,35.4,1e-6);
}
TEST(toilet_paper_single_item_uses_inclusive_thresholds_and_a_fixed_window) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const double remaining : {100.0, 90.0, 4.0}) {
        ArcticBlast rig("toilet_paper");
        const auto paper = rig.liveIn(0);
        CHECK_EQ(paper.size(), 1u);
        if (paper.empty()) return;
        const auto enemy = rig.mob({5500, 5000});
        const auto target = rig.dummy({5700, 5000});
        const auto& stats = rig.registry.petalStats(rig.registry.petalIndex("toilet_paper"), Rarity::Common);
        CHECK_EQ(stats.damage, 75.0);
        CHECK_EQ(stats.damageIntervalMillis, kPetalHitIntervalMillis);
        const auto damage = [&](double now) {
            return rig.combat.applyDamage(rig.world, target, paper.front(), stats.damage, now).applied;
        };
        CHECK_EQ(damage(4500), 75.0);
        rig.world.get<Health>(rig.player) = Health{900.0, 1000.0};
        rig.combat.applyDamage(rig.world, rig.player, enemy, 900.0 - remaining, 5000);
        CHECK_EQ(rig.world.get<Health>(rig.player).current, remaining);
        CHECK(!rig.world.get<Afflictions>(rig.player).toiletArmed);
        CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 13000.0);
        CHECK_EQ(damage(5050), 750.0);
        CHECK_EQ(rig.combat.applyDamage(rig.world, target, rig.player, 90.0, 5100).applied, 90.0);
        rig.world.get<Health>(rig.player) = Health{500.0, 1000.0};
        CHECK_EQ(damage(5150), 750.0);
        rig.world.get<Health>(rig.player) = Health{950.0, 1000.0};
        rig.tickPetals(5200);
        CHECK(rig.world.get<Afflictions>(rig.player).toiletArmed);
        CHECK_EQ(damage(12999), 750.0);
        CHECK_EQ(damage(13000), 75.0);
    }
}

TEST(toilet_paper_keeps_the_window_through_unequip_but_clears_arming) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("toilet_paper");
    const auto enemy = rig.mob({5500, 5000});
    const auto target = rig.dummy({5700, 5000});
    rig.world.get<Health>(rig.player) = Health{1000.0, 1000.0};
    rig.combat.applyDamage(rig.world, rig.player, enemy, 910.0, 5000);
    rig.world.get<Health>(rig.player) = Health{1000.0, 1000.0};
    rig.tickPetals(5050);
    CHECK(rig.world.get<Afflictions>(rig.player).toiletArmed);
    rig.world.get<Loadout>(rig.player).slots[0] = {};
    rig.tickPetals(5100);
    CHECK(!rig.world.get<Afflictions>(rig.player).toiletArmed);
    rig.world.get<Health>(rig.player) = Health{120.0, 1000.0};
    rig.equip(0, "toilet_paper");
    rig.tickPetals(5150);
    rig.tickPetals(10000);
    const auto paper = rig.liveIn(0);
    CHECK_EQ(paper.size(), 1u);
    if (paper.empty()) return;
    CHECK(!rig.world.get<Afflictions>(rig.player).toiletArmed);
    CHECK_EQ(rig.combat.applyDamage(rig.world, target, paper.front(), 90.0, 10050).applied, 900.0);
    rig.world.get<Health>(rig.player) = Health{120.0, 1000.0};
    rig.combat.applyDamage(rig.world, rig.player, enemy, 30.0, 13050);
    CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 13000.0);
    CHECK_EQ(rig.combat.applyDamage(rig.world, target, paper.front(), 90.0, 13100).applied, 90.0);
}

TEST(toilet_paper_needs_real_hp_loss_and_must_rearm_after_each_trigger) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("basic");
    const auto enemy = rig.mob({5500, 5000});
    rig.world.get<Health>(rig.player) = Health{600.0, 1000.0};
    rig.equip(0, "toilet_paper");
    rig.tickPetals(4500);
    rig.world.get<Health>(rig.player) = Health{600.0, 1000.0};
    rig.combat.applyDamage(rig.world, rig.player, enemy, 510.0, 5000);
    CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 0.0);
    rig.world.get<Health>(rig.player) = Health{900.0, 1000.0};
    rig.tickPetals(5500);
    rig.world.get<Health>(rig.player) = Health{90.0, 1000.0};
    rig.combat.applyDamage(rig.world, rig.player, enemy, 0.0, 5800);
    CHECK(rig.world.get<Afflictions>(rig.player).toiletArmed);
    CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 0.0);
    rig.combat.applyDamage(rig.world, rig.player, enemy, 10.0, 6000);
    CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 14000.0);
    rig.tickPetals(14000);
    rig.world.get<Health>(rig.player) = Health{80.0, 1000.0};
    rig.combat.applyDamage(rig.world, rig.player, enemy, 10.0, 14100);
    CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 14000.0);
    rig.world.get<Health>(rig.player) = Health{900.0, 1000.0};
    rig.combat.applyDamage(rig.world, rig.player, enemy, 810.0, 14700);
    CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 22700.0);
}

TEST(toilet_paper_lethal_hits_do_not_start_or_cancel_the_window) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (int rescue = 0; rescue <= 2; ++rescue) {
        for (const bool activeWindow : {false, true}) {
            ArcticBlast rig("toilet_paper");
            if (rescue == 1) {
                rig.world.ensure<PlayerSkillTree>(rig.player).skills.set(SkillId::SecondChance, 0);
            } else if (rescue == 2) {
                rig.equip(2, "bandage", Rarity::Ultra);
            }
            rig.tickPetals(4500);
            rig.tickPetals(7500);
            const auto enemy = rig.mob({5500, 5000});
            rig.world.get<Health>(rig.player) = Health{1000.0, 1000.0};
            if (activeWindow) rig.combat.applyDamage(rig.world, rig.player, enemy, 910.0, 8000);
            rig.world.get<Health>(rig.player) = Health{120.0, 1000.0};
            rig.combat.applyDamage(rig.world, rig.player, enemy, 200.0, 9000);
            CHECK_EQ(rig.world.get<Health>(rig.player).current, rescue == 0 ? 0.0 : 1.0);
            CHECK_EQ(rig.world.has<SecondChance>(rig.player), rescue == 1);
            CHECK_EQ(rig.world.has<Undead>(rig.player), rescue == 2);
            CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis,
                     activeWindow ? 16000.0 : 0.0);
            const auto paper = rig.liveIn(0);
            if (rescue != 0 && activeWindow && !paper.empty()) {
                const auto target = rig.dummy({5700, 5000});
                CHECK_EQ(rig.combat.applyDamage(rig.world, target, paper.front(), 90.0, 9050).applied, 900.0);
            }
        }
    }
}

TEST(toilet_paper_consecutive_contacts_use_the_normal_game_hit_interval) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("toilet_paper");
    const auto paper = rig.liveIn(0);
    CHECK_EQ(paper.size(), 1u);
    if (paper.empty()) return;
    const Vec2 at{5300, 5300};
    const auto target = rig.dummy(at);
    const auto enemy = rig.mob({5500, 5000});
    rig.world.get<Health>(rig.player) = Health{1000.0, 1000.0};
    rig.combat.applyDamage(rig.world, rig.player, enemy, 910.0, 5000);
    rig.world.get<Transform>(paper.front()).position = at;
    rig.combatTick(5050);
    rig.world.get<Transform>(paper.front()).position = at;
    rig.combatTick(5100);
    const auto totals = rig.combat.practiceDamage(rig.player, 5100, target);
    CHECK_EQ(totals.size(), 1u);
    if (!totals.empty()) CHECK_EQ(totals.front().amount, 1500.0);
}

TEST(reference_fractal_strikes_seven_unique_targets_without_duplicate_damage) {
 ArcticBlast r("fractal");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 auto at=r.world.get<Transform>(live.front()).position;std::vector<Entity> targets;
 for(int i=0;i<7;++i)targets.push_back(r.mob(at+Vec2{double(i*30),0}));
 r.tickPetals(4050);r.combatTick(4050);
 for(Entity mob:targets)CHECK_NEAR(r.world.get<Health>(mob).current,930,1e-6);
}
TEST(reference_fractal_apex_hits_practice_dummy_and_large_mob) {
 std::string error;
 if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
 for (bool large : {false, true}) {
    ArcticBlast r("fractal", Rarity::Ascendant);
    const auto petals = r.liveIn(0);
    CHECK(!petals.empty());
    if (petals.empty()) return;
    const auto at = r.world.get<Transform>(petals.front()).position;
    Entity target = large ? r.mob(at + Vec2{310,0}) : r.dummy(at + Vec2{1,0});
    r.world.get<Body>(target).radius = large ? 320 : 80;
    r.tickPetals(4050);
    r.combatTick(4050);
    if (large) CHECK(r.world.get<Health>(target).current < 1000);
    else {
        const auto hits = r.combat.practiceDamage(r.player, 4050, target);
        CHECK(!hits.empty() && hits.front().amount > 0);
    }
 }
}
TEST(reference_scripted_contact_hits_at_a_coincident_centre) {
    for (const char* id : {"fractal", "bomb"}) {
        ArcticBlast rig(id);
        const auto ring = rig.liveIn(0);
        CHECK_EQ(ring.size(), 1u);
        if (ring.empty()) continue;
        const Vec2 at = rig.world.get<Transform>(ring.front()).position;
        const Entity target = rig.mob(at);
        // Keep the ring fixed for this tick so both centres still coincide.
        rig.petals.run(rig.world, rig.registry, 4050.0, 0.0, rig.commands);
        rig.commands.flush();
        rig.combatTick(4050.0);
        CHECK(rig.world.get<Health>(target).current < 1000.0);
    }
}
TEST(reference_fission_splits_effect_damage_with_zero_contact_damage) {
    for (const char* id : {"fractal", "bomb"}) {
        for (const Rarity rarity : {Rarity::Common, Rarity::Unique, Rarity::Apex}) {
            ArcticBlast rig("fission", Rarity::Unique);
            rig.equip(1, id, rarity);
            rig.tickPetals(4050.0);
            rig.tickPetals(30000.0);
            const auto bodies = rig.liveIn(1);
            CHECK_EQ(bodies.size(), rarity == Rarity::Apex ? 1u : 3u);
            if (bodies.empty()) continue;
            CHECK_NEAR(rig.registry.petalStats(rig.registry.petalIndex(id), rarity).damage, 0.0, 1e-8);
            const Vec2 at = rig.world.get<Transform>(bodies.front()).position;
            const Entity target = rig.mob(at + Vec2{6, 0});
            rig.world.get<Body>(target).radius = 30;
            rig.tickPetals(30050.0);
            rig.combatTick(30050.0);
            CHECK(rig.world.get<Health>(target).current < 1000.0);
        }
    }
}
TEST(reference_fission_copies_burn_lightning_and_healing_or_damaging_spills) {
    for (const char* id : {"void_fire", "mjolnir", "latrine", "elixir", "arctic_nitrogen"}) {
        const std::string_view name(id);
        const bool lightning = name == "mjolnir";
        const bool burn = name == "void_fire";
        const bool healing = name == "elixir";
        const bool frostOnly = name == "arctic_nitrogen";
        const auto rarity = lightning ? Rarity::Unique : Rarity::Common;
        ArcticBlast rig("fission", Rarity::Unique);
        rig.equip(1, id, rarity);
        rig.tickPetals(4050.0);
        rig.tickPetals(50000.0);
        const auto copies = rig.liveIn(1);
        CHECK_EQ(copies.size(), frostOnly ? 1u : 3u);
        if (copies.empty()) continue;

        if (lightning || burn) {
            const Vec2 at{5300.0, 5300.0};
            for (const Entity copy : copies) rig.world.get<Transform>(copy).position = at;
            const Entity target = rig.mob(at);
            rig.world.get<Health>(target) = {1e8, 1e8};
            rig.combatTick(50050.0);
            rig.combatTick(50100.0);
            CHECK(rig.world.get<Health>(target).current < 1e8);
            continue;
        }

        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.tickPetals(52500.0);
        const auto fields = Query<GroundEffect>{rig.world}.collect();
        CHECK_EQ(fields.size(), frostOnly ? 1u : 3u);
        if (fields.empty() || frostOnly) continue;
        const Vec2 at = rig.world.get<Transform>(fields.front()).position;
        if (healing) {
            rig.world.get<Transform>(rig.player).position = at;
            auto& health = rig.world.get<Health>(rig.player);
            const double wounded = health.max * .5;
            health.current = wounded;
            rig.combatTick(52550.0);
            CHECK(rig.world.get<Health>(rig.player).current > wounded);
        } else {
            const Entity target = rig.mob(at);
            rig.combatTick(52550.0);
            rig.combatTick(52600.0);
            CHECK(rig.world.get<Health>(target).current < 1000.0);
        }
    }
}
TEST(reference_bloom_revives_all_eligible_squadmates_without_touching_strangers) {
 ArcticBlast r("bloom");SquadEntityIndex squad;r.petals.squads=&squad;
 auto corpse=[&](Realm realm,bool locked){
  Entity e=r.world.create();r.world.add<PlayerTag>(e);r.world.add<Transform>(e,Transform{{5100,5000},0,realm});
  r.world.add<Health>(e,Health{0,100});r.world.add<Loadout>(e);r.world.add<PetalRing>(e);r.world.add<PlayerModifiers>(e);r.world.add<Dead>(e);
  if(locked)r.world.add<Unrevivable>(e,Unrevivable{1000000});return e;
 };
 Entity first=corpse(Realm::Overworld,false),second=corpse(Realm::Overworld,false),locked=corpse(Realm::Overworld,true),stranger=corpse(Realm::Overworld,false),away=corpse(Realm::Arena,false);
 squad.groups={{{r.player,0},{first,0},{second,0},{locked,0},{away,0}}};squad.group[r.player]=0;
 r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(604001);
 CHECK(!r.world.has<Dead>(first));CHECK(!r.world.has<Dead>(second));CHECK(r.world.has<Dead>(locked));CHECK(r.world.has<Dead>(stranger));CHECK(r.world.has<Dead>(away));
 CHECK_NEAR(r.world.get<Health>(first).current,r.world.get<Health>(first).max,1e-6);
}

TEST(reference_gas_uses_narrow_jet_collision_and_real_poison) {
 ArcticBlast r("gas");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 Entity gas=live.front();auto at=r.world.get<Transform>(gas).position;r.world.get<PetalInstance>(gas).facingAngle=0;
 Entity along=r.mob(at+Vec2{15,0}),beside=r.mob(at+Vec2{0,15});r.combatTick(5000);
 CHECK(r.world.has<Afflictions>(along));if(r.world.has<Afflictions>(along))CHECK_NEAR(r.world.get<Afflictions>(along).poisonPerSecond,16,1e-6);
 CHECK(!r.world.has<Afflictions>(beside));
 CHECK_NEAR(r.registry.petalStats(r.registry.petalIndex("gas"),Rarity::Super).radius,160,1e-6);
}
TEST(reference_latrine_creates_persistent_stacking_poison_and_soap_applies) {
 ArcticBlast r("basic");r.equip(1,"void_soap");r.petals.foldModifiers(r.world,r.registry);
 const auto& spec=r.registry.petal(r.registry.petalIndex("latrine")).spill;
 CombatSystem::createSpill(r.world,r.player,{5400,5400},Rarity::Common,spec,1,0);
    // Range assertions below describe the fully spread pool, not its birth frame.
    for(int spread=1;spread<=7;++spread)r.combatTick(4000+spread*50);
 CombatSystem::createSpill(r.world,r.player,{5400,5400},Rarity::Common,spec,1,0);
    // Range assertions below describe the fully spread pool, not its birth frame.
    for(int spread=1;spread<=7;++spread)r.combatTick(4000+spread*50);
 Entity mob=r.mob({5400,5400});r.combatTick(5000);
 CHECK(r.world.has<Afflictions>(mob));if(r.world.has<Afflictions>(mob)) {
  CHECK_EQ(r.world.get<Afflictions>(mob).poisonStacks.size(),2);
  CHECK_NEAR(r.world.get<Afflictions>(mob).poisonPerSecond,21.875,1e-6);
 }
 int fields=0;Query<GroundEffect>{r.world}.each([&](Entity e,GroundEffect& f){
  if(f.kind!=GroundEffectKind::LatrineSpill)return;++fields;CHECK(!r.world.has<Health>(e));CHECK(!r.world.has<Body>(e));CHECK_NEAR(f.radius,42.9 - std::max(0.0, 4.5 - 9.9 * f.spreadAgeSeconds),1e-6);
 });CHECK_EQ(fields,2);
}
TEST(reference_silk_detaches_from_ring_tracks_one_target_and_frees_reload) {
 ArcticBlast r("silk");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 Entity silk=live.front();auto at=r.world.get<Transform>(silk).position;
 Entity target=r.mob(at);r.world.get<Health>(target)={2000,2000};r.combatTick(5000);
 CHECK(r.world.has<AttachedSilk>(silk));CHECK(r.liveIn(0).empty());
 r.world.get<Transform>(target).position+=Vec2{500,200};r.tickPetals(5050);
 CHECK_NEAR(distanceSq(r.world.get<Transform>(silk).position,r.world.get<Transform>(target).position),0,1e-6);
 CHECK(r.world.get<Loadout>(r.player).slots[0].broken);
 r.combatTick(6000);CHECK_NEAR(r.world.get<Health>(target).current,1976,1e-6);
 r.world.add<Dead>(target);r.tickPetals(6050);CHECK(!r.world.isAlive(silk));
}
TEST(reference_marble_launches_bounces_and_uses_real_armour_for_penetration) {
 ArcticBlast r("marble");
 r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;
 r.tickPetals(4349);
 std::vector<Entity> shots;
 Query<Projectile>{r.world}.without<Dead>().collect(shots);
 CHECK(shots.empty());
 r.tickPetals(4350);
 Query<Projectile>{r.world}.without<Dead>().collect(shots);CHECK_EQ(shots.size(),1);if(shots.empty())return;
 Entity shot=shots.front();CHECK(r.world.get<Projectile>(shot).bouncesOffWalls);CHECK_NEAR(r.world.get<Lifetime>(shot).remainingSeconds,15,1e-6);
 Entity target=r.mob(r.world.get<Transform>(shot).position);r.world.add<ContactDamage>(target,ContactDamage{10,0});r.combatTick(4350);
 CHECK(!r.world.has<Dead>(shot));CHECK_NEAR(r.world.get<Health>(shot).current,35,1e-6);CHECK_NEAR(r.world.get<Health>(target).current,900,1e-6);
}
TEST(marble_hits_along_its_last_clipped_step_and_then_expires) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("marble");
    rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
    rig.tickPetals(4500);
    std::vector<Entity> shots;
    Query<Projectile>{rig.world}.without<Dead>().collect(shots);
    CHECK_EQ(shots.size(), 1u);
    if (shots.empty()) return;

    const Entity shot = shots.front();
    const Vec2 from{6500, 5000};
    rig.world.get<Transform>(shot).position = from;
    rig.world.get<Motion>(shot).velocity = {1600, 0};
    rig.world.get<Projectile>(shot).remainingDistance = 70.0;
    const Entity inRange = rig.mob(from + Vec2{35, 0});
    rig.world.add<ContactDamage>(inRange, ContactDamage{10, 0});
    const Entity outOfRange = rig.mob(from + Vec2{110, 0});

    Terrain terrain;
    MovementSystem movement;
    movement.runWorldPhase(rig.world, terrain, 4550.0, net::kTickSeconds);
    rig.combatTick(4550.0);
    CHECK_NEAR(rig.world.get<Health>(inRange).current, 900.0, 1e-6);
    CHECK_NEAR(rig.world.get<Health>(outOfRange).current, 1000.0, 1e-6);
    CHECK(rig.world.has<Dead>(shot));
    rig.combatTick(4600.0);
    CHECK_NEAR(rig.world.get<Health>(inRange).current, 900.0, 1e-6);
}

TEST(reference_magic_marble_breaks_into_three_glass_shots_once) {
 ArcticBlast r("magic_marble",Rarity::Common,"mana_orb");r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(4300);
 std::vector<Entity> shots;Query<Projectile>{r.world}.without<Dead>().collect(shots);CHECK_EQ(shots.size(),1);if(shots.empty())return;
 Entity marble=shots.front();r.world.add<Dead>(marble);r.combatTick(4350);
 auto fragments=[&](){int count=0;Query<Projectile>{r.world}.without<Dead>().each([&](Entity,Projectile& p){if(p.petalConfigIndex==r.registry.petalIndex("glass")){++count;CHECK_NEAR(p.damage,60,1e-6);}});return count;};
 CHECK_EQ(fragments(),3);r.combatTick(4400);CHECK_EQ(fragments(),3);
}

TEST(zorr_catalog_magic_marble_shatters_from_live_impacts_even_against_low_damage) {
    for (const Rarity rarity : {Rarity::Common, Rarity::Unique, Rarity::Ascendant}) {
        ArcticBlast rig("magic_marble", rarity, "mana_orb");
        rig.equip(1, "mana_orb", rarity);
        rig.tickPetals(4050);
        auto& mana = rig.world.get<ManaPool>(rig.player);
        mana.current = mana.max;
        rig.tickPetals(6900);
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.tickPetals(7150);
        const auto shots = Query<Projectile>{rig.world}.without<Dead>().collect();
        CHECK_EQ(shots.size(), 1u);
        if (shots.empty()) continue;
        const Entity shot = shots.front();
        const Entity target = rig.mob(rig.world.get<Transform>(shot).position);
        rig.world.get<Health>(target) = {1e10, 1e10};
        rig.world.add<ContactDamage>(target).amount = 1.0;
        for (int hit = 0; hit < kMagicMarbleImpactsBeforeShatter; ++hit) {
            rig.combatTick(7150 + hit * 1250.0);
            CHECK_EQ(rig.world.has<Dead>(shot), hit + 1 == kMagicMarbleImpactsBeforeShatter);
        }
        const auto fragments = Query<Projectile>{rig.world}.without<Dead>().collect();
        CHECK_EQ(fragments.size(), 3u);
        for (Entity e : fragments)
            CHECK_EQ(rig.world.get<Projectile>(e).petalConfigIndex, rig.registry.petalIndex("glass"));
    }
}

TEST(zorr_catalog_magic_blood_stinger_charges_health_only_when_it_forms) {
    ArcticBlast rig("basic");
    const auto step = [&](double now) {
        rig.petals.run(rig.world, rig.registry, now, 0.0, rig.commands);
        rig.commands.flush();
    };
    const auto health = [&] { return rig.world.get<Health>(rig.player).current; };
    rig.equip(0, "magic_blood_stinger");
    rig.world.get<ManaPool>(rig.player).current = 0.0;
    step(4050.0);
    const double before = health();
    step(12000.0);
    CHECK(rig.liveIn(0).empty());
    CHECK_NEAR(health(), before, 1e-8);
    rig.world.get<ManaPool>(rig.player).current = 10.0;
    step(12050.0);
    CHECK_EQ(rig.liveIn(0).size(), 1u);
    CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current, 8.0, 1e-8);
    const auto index = rig.registry.petalIndex("magic_blood_stinger");
    const double damage = rig.registry.petalStats(index, Rarity::Common).damage;
    const double cap = rig.world.get<Health>(rig.player).max * 0.025;
    const double expected = before - std::min(damage * 0.05, cap);
    CHECK_NEAR(health(), expected, 1e-8);
    step(12100.0);
    CHECK_NEAR(health(), expected, 1e-8);
}

TEST(arctic_emerald_rewards_only_the_player_whose_emerald_landed_a_hit) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const bool blocked : {false, true}) {
        ArcticBlast rig("arctic_emerald");
        const auto active = rig.liveIn(0);
        CHECK_EQ(active.size(), 1u);
        if (active.empty()) return;
        const auto emerald = active.front();
        rig.world.add<PlayerProgress>(rig.player);
        const auto teammate = rig.world.create();
        rig.world.add<PlayerTag>(teammate);
        rig.world.add<Transform>(teammate, Transform{{5200, 5000}, 0});
        rig.world.add<PlayerProgress>(teammate);
        rig.world.add<Faction>(teammate, Faction{Team::Players, false});
        const auto mob = rig.mob({5300,5000});
        rig.world.get<Health>(mob) = {100,100,0,blocked ? 5100.0 : 0.0};
        rig.world.get<Bounty>(mob).xp = 100;
        const auto first = rig.combat.applyDamage(rig.world, mob, emerald, 10.0, 5000.0);
        CHECK_NEAR(first.applied, blocked ? 0.0 : 10.0, 1e-8);
        CHECK_NEAR(rig.combat.applyDamage(rig.world, mob, rig.player, 10.0, 5200.0).applied, 10.0, 1e-8);
        rig.world.get<Loadout>(rig.player).slots[0] = {};
        rig.tickPetals(5250);
        CHECK(rig.combat.applyDamage(rig.world, mob, teammate, 100.0, 5300.0).killed);
        CHECK_NEAR(rig.world.get<PlayerProgress>(rig.player).totalXp, blocked ? 100.0 : 250.0, 1e-8);
        CHECK_NEAR(rig.world.get<PlayerProgress>(teammate).totalXp, 100.0, 1e-8);
        const auto& shares = rig.world.get<Bounty>(mob).contributors;
        CHECK_EQ(shares.size(), 2u);
        if (shares.size() == 2u)
            CHECK_NEAR(shares.front().damage, blocked ? 10.0 : 20.0, 1e-8);
    }
}

TEST(zorr_catalog_magic_emerald_requires_a_paid_intact_copy) {
    ArcticBlast rig("basic");
    rig.equip(0, "magic_arctic_emerald", Rarity::Rare);
    rig.world.get<ManaPool>(rig.player).current = 0.0;
    rig.tickPetals(4050);
    rig.tickPetals(5500);
    CHECK(rig.liveIn(0).empty());
    CHECK_EQ(activeMagicEmeraldTier(rig.world, rig.registry, rig.player), -1);
    CHECK_EQ(rig.world.get<PlayerModifiers>(rig.player).autoAbsorbTier, -1);

    rig.world.get<ManaPool>(rig.player).current = 10.0;
    const auto index = rig.registry.petalIndex("magic_arctic_emerald");
    const double cost = rig.registry.petalStats(index, Rarity::Rare).requiredMana;
    rig.tickPetals(9000);
    CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current, 10.0 - cost, 1e-8);
    const auto deployed = rig.liveIn(0);
    CHECK_EQ(deployed.size(), 1u);
    if (deployed.empty()) return;
    CHECK_EQ(activeMagicEmeraldTier(rig.world, rig.registry, rig.player), petalTier(Rarity::Rare));
    rig.world.add<Dead>(deployed.front());
    CHECK_EQ(activeMagicEmeraldTier(rig.world, rig.registry, rig.player), -1);
    rig.tickPetals(9050);
    CHECK_EQ(rig.world.get<PlayerModifiers>(rig.player).autoAbsorbTier, -1);
}

TEST(zorr_catalog_conditional_petals_activate_with_their_required_setup) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    if (!error.empty()) return;
    const Vec2 at{6400, 6400};
    ArcticBlast iris("magic_iris", Rarity::Common, "mana_orb");
    const auto ring = iris.liveIn(0);
    CHECK_EQ(ring.size(), 1u);
    if (ring.empty()) return;
    iris.world.get<Transform>(ring.front()).position = at;
    auto target = iris.mob(at);
    iris.combatTick(5000.0);
    CHECK_NEAR(iris.world.get<Health>(target).current, 1000.0, 1e-8);
    CombatSystem::applyPoison(iris.world, target, iris.player, 20.0, 5000.0, 5050.0);
    iris.world.get<Transform>(target).position = iris.world.get<Transform>(ring.front()).position;
    iris.combatTick(6000.0);
    CHECK_NEAR(iris.world.get<Health>(target).current, 1000.0 - 20.0 - 20.0 * net::kTickSeconds, 1e-8);

    ArcticBlast nuts("magic_void_nut", Rarity::Common, "mana_orb");
    CHECK(nuts.liveIn(0).empty());
    nuts.equip(2, "magic_void_nut");
    nuts.equip(3, "magic_void_nut");
    nuts.tickPetals(4050.0);
    nuts.world.get<ManaPool>(nuts.player).current = nuts.world.get<ManaPool>(nuts.player).max;
    nuts.tickPetals(8050.0);
    for (int slot : {0, 2, 3}) CHECK_EQ(nuts.liveIn(slot).size(), 1u);

    ArcticBlast rescue("magic_yggdrasil", Rarity::Common, "mana_orb");
    CHECK(rescue.liveIn(0).empty());
    rescue.world.get<ManaPool>(rescue.player).current = rescue.world.get<ManaPool>(rescue.player).max;
    rescue.tickPetals(50000.0);
    const auto wards = rescue.liveIn(0);
    CHECK_EQ(wards.size(), 1u);
    if (wards.empty()) return;
    CHECK(!rescue.combat.applyDamage(rescue.world, rescue.player, rescue.mob(at), 10000.0, 50100.0).killed);
    CHECK(rescue.world.has<Dead>(wards.front()));
    const auto& saved = rescue.world.get<Health>(rescue.player);
    CHECK_NEAR(saved.current, saved.max * 0.10, 1e-8);
}

TEST(reference_spikeball_scatters_nine_needles_with_one_finite_budget) {
 ArcticBlast r("spikeball");r.armBomb();int count=0;double damage=0;
 Query<Projectile,Motion,Body>{r.world}.without<Dead>().each([&](Entity,Projectile& p,Motion& m,Body& b){
  if(p.petalConfigIndex!=r.registry.petalIndex("spikeball"))return;++count;damage+=p.damage;
  CHECK_NEAR(m.velocity.length(),1000,1e-6);CHECK_NEAR(b.radius,2,1e-6);
 });CHECK_EQ(count,9);CHECK_NEAR(damage,160,1e-6);
}

namespace {
Entity referenceFlower(ArcticBlast& r,Vec2 position) {
 Entity e=r.world.create();r.world.add<PlayerTag>(e);r.world.add<Transform>(e,Transform{position,0});
 r.world.add<Health>(e,Health{100,100});r.world.add<Body>(e,Body{25,1});r.world.add<Loadout>(e);r.world.add<PetalRing>(e);
 r.world.add<PlayerModifiers>(e);r.world.add<PlayerInput>(e);r.world.add<Motion>(e);r.world.add<Faction>(e,Faction{Team::Players,false});return e;
}
}
TEST(reference_bomb_blast_limit_is_per_flower) {
    ArcticBlast rig("bomb");
    rig.equip(1, "bomb");
    const Entity peer = referenceFlower(rig, {5000, 5000});
    rig.world.get<Transform>(peer).realm = Realm::Arena;
    rig.world.get<Loadout>(peer).slots[0] = {rig.registry.petalIndex("bomb"), Rarity::Common};
    rig.tickPetals(4050.0);
    rig.tickPetals(10000.0);

    const auto bombs = Query<PetalInstance>{rig.world}.collect();
    CHECK_EQ(bombs.size(), 3u);
    if (bombs.size() != 3u) return;
    std::vector<Entity> targets;
    for (const Entity bomb : bombs) {
        const auto at = rig.world.get<Transform>(bomb);
        const Entity target = rig.mob(at.position + Vec2{1, 0});
        rig.world.get<Transform>(target).realm = at.realm;
        targets.push_back(target);
    }

    rig.petals.run(rig.world, rig.registry, 10050.0, 0.0, rig.commands);
    rig.commands.flush();
    rig.combatTick(10050.0);
    for (const Entity bomb : bombs) CHECK(rig.world.has<Dead>(bomb));
    for (const Entity target : targets) CHECK_NEAR(rig.world.get<Health>(target).current, 970.0, 1e-8);
}
TEST(reference_mysterious_relic_grants_health_and_shares_damage_when_equipped) {
    ArcticBlast rig("mysterious_relic", Rarity::Uncommon);
    const Entity peer = referenceFlower(rig, {5300, 5000});
    const Entity outsider = referenceFlower(rig, {5500, 5000});
    rig.world.get<Loadout>(peer).slots[0] = {
        rig.registry.petalIndex("mysterious_relic"), Rarity::Uncommon};
    SquadEntityIndex squad;
    squad.groups.push_back({{rig.player, 1}, {peer, 2}, {outsider, 3}});
    for (const Entity player : {rig.player, peer, outsider}) squad.group.emplace(player, 0);
    rig.combat.squads = &squad;
    rig.tickPetals(4050.0);

    const auto& stats = rig.registry.petalStats(rig.registry.petalIndex("mysterious_relic"), Rarity::Uncommon);
    CHECK_NEAR(stats.modifiers.maxHealth, 1.2, 1e-8);
    const double ordinaryMax = rig.world.get<Health>(outsider).max;
    CHECK_NEAR(rig.world.get<Health>(rig.player).max, std::round(ordinaryMax * 1.2), 1e-8);
    CHECK_NEAR(rig.world.get<Health>(peer).max, std::round(ordinaryMax * 1.2), 1e-8);
    const auto hp = [&](Entity player) { return rig.world.get<Health>(player).current; };
    const auto attacker = rig.mob({5200, 5000});
    const double initial = hp(rig.player);
    const double peerInitial = hp(peer);
    const double outsiderInitial = hp(outsider);
    rig.combat.applyDamage(rig.world, rig.player, attacker, 30.0, 4100.0);
    CHECK_NEAR(hp(rig.player), initial - 15.0, 1e-8);
    CHECK_NEAR(hp(peer), peerInitial - 15.0, 1e-8);
    CHECK_NEAR(hp(outsider), outsiderInitial, 1e-8);

    rig.world.get<Transform>(peer).realm = Realm::Arena;
    rig.combat.applyDamage(rig.world, rig.player, attacker, 30.0, 4200.0);
    CHECK_NEAR(hp(rig.player), initial - 45.0, 1e-8);
    CHECK_NEAR(hp(peer), peerInitial - 15.0, 1e-8);
}

TEST(player_feedback_upright_blades_use_the_same_axis_in_both_combat_modes) {
    for (const char* id : {"void_scythe", "void_blood_scythe", "void_trident",
                           "magic_void_scythe", "magic_void_blood_scythe", "magic_void_trident"}) {
        for (bool pvp : {false, true}) {
            ArcticBlast rig(id, Rarity::Common, "mana_orb");
            rig.tickPetals(20000);
            const auto petals = rig.liveIn(0);
            CHECK(!petals.empty());
            if (petals.empty()) continue;
            const Entity blade = petals.front();
            const Vec2 centre{5500,5500};
            rig.world.get<Transform>(blade).position = centre;
            const double radius = rig.world.get<Body>(blade).radius;
            CHECK(radius >= 80);
            const auto target = [&](Vec2 offset) {
                Entity e = pvp ? referenceFlower(rig, centre + offset) : rig.mob(centre + offset);
                rig.world.get<Health>(e) = Health{1000,1000};
                rig.world.get<Faction>(e).friendlyFireEnabled = true;
                return e;
            };
            rig.world.get<Faction>(rig.player).friendlyFireEnabled = true;
            const Entity side = target({radius * .75, 0});
            const Entity tip = target({0, radius * .75});
            rig.combatTick(20050);
            CHECK_EQ(rig.world.get<Health>(side).current, 1000);
            CHECK(rig.world.get<Health>(tip).current < 1000);
        }
    }
}

TEST(player_feedback_flower_poisons_for_iris_and_never_hatches_a_pet) {
    ArcticBlast rig("flower");
    const auto flower = rig.liveIn(0).at(0);
    const auto at = rig.world.get<Transform>(flower).position;
    const Entity target = rig.mob(at);
    rig.combatTick(4050);
    const auto* status = rig.world.tryGet<Afflictions>(target);
    CHECK(status && status->poisoned(4050) && status->poisonPerSecond > 0);
    rig.world.get<Health>(flower).current = 0;
    rig.tickPetals(4100);
    CHECK_EQ(Query<Pet>{rig.world}.count(), 0u);
}

TEST(player_feedback_both_sewage_pools_survive_contact) {
    for (const char* id : {"latrine", "magic_latrine"}) {
        ArcticBlast rig("basic");
        Vec2 at{5400, 5400};
        CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common,
            rig.registry.petal(rig.registry.petalIndex(id)).spill, 1, 0);
        const Entity field = Query<GroundEffect>{rig.world}.collect().at(0);
        const Entity target = rig.mob(at);
        for (int t = 4050; t <= 5050; t += 50) rig.combatTick(t);
        CHECK(!rig.world.has<Health>(field) && !rig.world.has<Dead>(field));
        CHECK(rig.world.has<GroundEffect>(field));
        CHECK(rig.world.has<Afflictions>(target));
    }
}

TEST(reference_dark_matter_creates_three_empty_common_orbit_seats) {
 ArcticBlast r("basic");r.equip(1,"void_dark_matter");r.equip(2,"basic");r.tickPetals(5000);r.tickPetals(10000);
 auto first=r.liveIn(0),last=r.liveIn(2);CHECK(!first.empty()&&!last.empty());CHECK(r.liveIn(1).empty());if(first.empty()||last.empty())return;
 double delta=r.world.get<PetalInstance>(last.front()).facingAngle-r.world.get<PetalInstance>(first.front()).facingAngle;
 CHECK_NEAR(std::cos(delta),std::cos(kTau*4/5),1e-6);
}
TEST(reference_ears_expands_resting_orbit_as_well_as_attack) {
 ArcticBlast plain("basic"),wide("basic");wide.equip(1,"arctic_ears");
 for(int now=4050;now<=14000;now+=50){plain.tickPetals(now);wide.tickPetals(now);}
 auto radius=[](ArcticBlast& r){auto live=r.liveIn(0);return distance(r.world.get<Transform>(live.front()).position,r.world.get<Transform>(r.player).position);};
 CHECK(radius(wide)-radius(plain)>50);CHECK(radius(wide)-radius(plain)<160);
}
TEST(reference_zodiac_penalizes_real_reload_and_carries_fire) {
 ArcticBlast plain("basic"),zodiac("basic");zodiac.equip(1,"void_zodiac");
 for(ArcticBlast* r:{&plain,&zodiac}){r->tickPetals(5000);auto live=r->liveIn(0);CHECK(!live.empty());if(live.empty())return;r->world.get<Health>(live.front()).current=0;r->tickPetals(5050);}
 double base=plain.world.get<Loadout>(plain.player).slots[0].reloadReadyAtMillis-5050;
 double penalized=zodiac.world.get<Loadout>(zodiac.player).slots[0].reloadReadyAtMillis-5050;
 CHECK_NEAR(penalized/base,1/.95,1e-6);
 CHECK_NEAR(zodiac.registry.petal(zodiac.registry.petalIndex("void_zodiac")).burnPerSecond,30,1e-6);
}
TEST(reference_copper_rod_reduces_lightning_without_reducing_body_damage) {
 ArcticBlast r("void_copper_rod");Entity mob=r.mob({5500,5000});
 CHECK_NEAR(r.combat.applyDamage(r.world,r.player,mob,50,5000,DamageKind::Lightning).applied,35,1e-6);
 CHECK_NEAR(r.combat.applyDamage(r.world,r.player,mob,50,5100).applied,50,1e-6);
 CHECK(!r.registry.petal(r.registry.petalIndex("void_copper_rod")).projectile.present);
}
TEST(reference_halo_heals_visible_allies_and_leaves_offscreen_flowers_alone) {
 ArcticBlast r("void_halo");Entity ally=referenceFlower(r,{5100,5000}),far=referenceFlower(r,{8000,5000});
 r.petals.foldModifiers(r.world,r.registry);r.world.get<Health>(ally).current=50;r.world.get<Health>(far).current=50;
 r.tickPetals(5000);
 CHECK_NEAR(r.world.get<Health>(ally).current,51.035,1e-6);
 CHECK_NEAR(r.world.get<Health>(far).current,50.225,1e-6);
}
TEST(reference_sad_reduces_its_damage_when_another_player_watches_owner) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("void_sad");
    const auto live = rig.liveIn(0);
    CHECK_EQ(live.size(), 1u);
    if (live.empty()) return;
    const Entity target = rig.mob({5500, 5000});
    const auto hit = [&](double now) {
        return rig.combat.applyDamage(rig.world, target, live.front(), 80, now).applied;
    };
    CHECK_NEAR(hit(5000), 80, 1e-6);
    const Entity observer = referenceFlower(rig, {5100, 5000});
    CHECK_NEAR(hit(5050), 40, 1e-6);
    rig.world.get<Transform>(observer).position = {20000, 5000};
    CHECK_NEAR(hit(5100), 80, 1e-6);
    rig.world.get<Transform>(observer).position = {5100, 5000};
    rig.world.get<Transform>(observer).realm = Realm::Arena;
    CHECK_NEAR(hit(5150), 80, 1e-6);
}
TEST(magic_iodine_pulses_only_on_attack_or_defend_and_reaches_local_friends) {
    for (auto flags : {0, int(net::InputAttack), int(net::InputDefend)}) {
        ArcticBlast rig("magic_iodine", Rarity::Common, "mana_orb");
        const auto ally = referenceFlower(rig, {5030, 5000});
        const auto outOfRange = referenceFlower(rig, {6000, 5000});
        const auto otherMap = referenceFlower(rig, {5030, 5000});
        const auto hostile = referenceFlower(rig, {5030, 5000});
        rig.world.get<Transform>(otherMap).realm = Realm::Arena;
        rig.world.get<Faction>(hostile) = {Team::Players, true};
        rig.world.get<PlayerInput>(rig.player).current.flags = flags;
        rig.tickPetals(4050);
        for (const auto recipient : {rig.player, ally, outOfRange, otherMap, hostile})
            rig.world.get<ManaPool>(recipient).current = 0;
        rig.tickPetals(5000);
        CHECK_NEAR(rig.world.get<ManaPool>(ally).current, flags ? 2.0 : 0.0, 1e-8);
        for (const auto recipient : {outOfRange, otherMap, hostile})
            CHECK_NEAR(rig.world.get<ManaPool>(recipient).current, 0.0, 1e-8);
    }
}

TEST(magic_neurotoxin_turns_the_shared_leech_head_when_its_tail_is_hit) {
    for (const Rarity tier : {Rarity::Common, Rarity::Rare}) {
        ArcticBlast rig("magic_neurotoxin", Rarity::Common, "mana_orb");
        const auto live = rig.liveIn(0);
        CHECK_EQ(live.size(), 1u);
        if (live.empty()) return;
        const Vec2 strike = rig.world.get<Transform>(live.front()).position;
        const auto head = rig.mob(strike + Vec2{300,0});
        const auto tail = rig.mob(strike + Vec2{1,0});
        rig.world.add<MobType>(head, MobType{rig.registry.mobIndex("leech"), tier, 1.0});
        rig.world.add<MobType>(tail, MobType{rig.registry.mobIndex("leech"), Rarity::Common, 1.0});
        rig.world.add<BodySegment>(head).head = true;
        auto& segment = rig.world.add<BodySegment>(tail);
        segment.ahead = head;
        segment.sharedHealth = true;
        segment.chainHead = head;
        rig.world.add<Motion>(head).velocity = {100,0};
        rig.combatTick(5000);
        CHECK(rig.world.get<Health>(head).current < 1000);

        Terrain terrain;
        terrain.fill(Tile::Open);
        MovementSystem movement;
        const Vec2 start = rig.world.get<Transform>(head).position;
        movement.runWorldPhase(rig.world, terrain, 5000, net::kTickSeconds);
        const Vec2 changed = rig.world.get<Transform>(head).position;
        CHECK(changed.x > start.x);
        if (tier == Rarity::Common) CHECK(std::abs(changed.y - start.y) > 1.0);
        else CHECK_NEAR(changed.y, start.y, 1e-8);
        rig.world.get<Motion>(head).velocity = {100,0};
        movement.runWorldPhase(rig.world, terrain, 10000, net::kTickSeconds);
        CHECK_NEAR(rig.world.get<Transform>(head).position.y, changed.y, 1e-8);
    }
}

TEST(reference_trident_only_highest_other_wearer_can_keep_it_active) {
 ArcticBlast r("void_trident");Entity rival=referenceFlower(r,{5500,5000});
 r.world.get<Loadout>(rival).slots[0]={r.registry.petalIndex("void_trident"),Rarity::Rare};
 r.tickPetals(5000);r.tickPetals(100000);CHECK(r.liveIn(0).empty());
 bool higher=false;for(Entity e:r.world.get<Loadout>(rival).spawned)if(r.world.get<PetalInstance>(e).slot==0)higher=true;CHECK(higher);
 r.world.get<Loadout>(rival).slots[0]={};r.tickPetals(100050);r.tickPetals(110000);CHECK(!r.liveIn(0).empty());
 CHECK(!r.registry.petal(r.registry.petalIndex("void_trident")).projectile.present);
}
TEST(reference_frost_pierces_armour_without_becoming_poison) {
 ArcticBlast r("arctic_frost");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 Entity mob=r.mob(r.world.get<Transform>(live.front()).position);r.world.add<Armor>(mob,Armor{10000,false});r.world.add<PoisonImmune>(mob);
 r.combatTick(5000);double damage=r.registry.petalStats(r.registry.petalIndex("arctic_frost"),Rarity::Common).damage;
 CHECK_NEAR(r.world.get<Health>(mob).current,1000-damage,1e-6);
 const auto* frost=r.world.tryGet<Afflictions>(mob);
 CHECK(frost && frost->frostSpeed(5000)<1.0 && frost->poisonPerSecond==0.0);
}
TEST(reference_polar_electron_missile_produces_real_visible_chain_lightning) {
 ArcticBlast r("arctic_electron_missile");r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(4500);
 std::vector<Entity> shots;Query<Projectile>{r.world}.without<Dead>().collect(shots);CHECK_EQ(shots.size(),1);if(shots.empty())return;
 Vec2 at=r.world.get<Transform>(shots.front()).position;Entity first=r.mob(at),second=r.mob(at+Vec2{80,0});
 r.combatTick(4500);CHECK(!r.combat.shotLandings().empty());auto count=r.events.events().size();
 r.petals.landShots(r.world,r.registry,r.combat.shotLandings(),nullptr,4500,&r.events);CHECK(r.events.events().size()>count);
 r.combatTick(4550);CHECK_NEAR(r.world.get<Health>(second).current,997,1e-6);CHECK(r.world.get<Health>(first).current<990);
}
TEST(projectile_lightning_riders_work_on_the_training_dummy) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const char* id : {"electron_missile", "arctic_electron_missile", "magic_electron_missile", "void_plasma"}) {
        ArcticBlast rig(id, Rarity::Common, "mana_orb");
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        std::vector<Entity> shots;
        double now = 4000.0;
        while (shots.empty() && now < 12000.0) {
            now += 50.0;
            rig.tickPetals(now);
            Query<Projectile>{rig.world}.without<Dead>().collect(shots);
        }
        CHECK_EQ(shots.size(), 1u);
        if (shots.empty()) continue;
        const Vec2 impact = rig.world.get<Transform>(shots.front()).position;
        const Entity target = rig.dummy(impact);
        const Entity nearby = rig.mob(impact + Vec2{80, 0});
        const auto measuredDamage = [&](double at) {
            double total = 0;
            for (const auto& hit : rig.combat.practiceDamage(rig.player, at))
                total += hit.amount;
            return total;
        };
        rig.combatTick(now);
        const double afterImpact = measuredDamage(now);
        CHECK(afterImpact > 0);
        CHECK_EQ(rig.combat.shotLandings().size(), 1u);

        const auto eventsBefore = rig.events.events().size();
        rig.petals.landShots(rig.world, rig.registry, rig.combat.shotLandings(), nullptr, now, &rig.events);
        CHECK(rig.events.events().size() > eventsBefore);
        rig.combatTick(now + 50);
        CHECK(measuredDamage(now + 50) > afterImpact);
        if (std::string_view(id) != "void_plasma")
            CHECK(rig.world.get<Health>(nearby).current < 1000);
        if (std::string_view(id) == "magic_electron_missile") {
            const auto* affliction = rig.world.tryGet<Afflictions>(nearby);
            CHECK(affliction && !affliction->burns.empty());
        }
    }
}

TEST(reference_plasma_chain_can_hit_the_same_target_multiple_times_and_applies_both_dots) {
 ArcticBlast r("void_plasma");r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(4500);
 std::vector<Entity> shots;Query<Projectile>{r.world}.without<Dead>().collect(shots);CHECK_EQ(shots.size(),1);if(shots.empty())return;
 Entity target=r.mob(r.world.get<Transform>(shots.front()).position);r.world.get<Health>(target)={5000,5000};r.combatTick(4500);
 const auto* status=r.world.tryGet<Afflictions>(target);CHECK(status!=nullptr);if(status){CHECK_NEAR(status->poisonPerSecond,20,1e-6);CHECK(!status->burns.empty());}
 r.petals.landShots(r.world,r.registry,r.combat.shotLandings(),nullptr,4500,&r.events);
 int hits=0;Query<LightningTarget,GroundEffect>{r.world}.each([&](Entity,LightningTarget& lightning,GroundEffect& field){if(lightning.victim==target){++hits;CHECK_NEAR(field.damagePerHit,8,1e-6);}});CHECK_EQ(hits,4);
 // Rider statuses were checked above; isolate the four authoritative arcs.
 r.world.remove<Afflictions>(target);
 for(Entity shot:shots)if(!r.world.has<Dead>(shot))r.world.add<Dead>(shot);
 double before=r.world.get<Health>(target).current;r.combatTick(4550);double lost=before-r.world.get<Health>(target).current;CHECK_NEAR(lost,32,1e-6);
}

TEST(reference_nitrogen_places_breakable_non_damaging_fluid_and_freezes_only_hostiles) {
 ArcticBlast r("arctic_nitrogen");r.tickPetals(5000);
 Entity pool=NULL_ENTITY;Query<GroundEffect,Health>{r.world}.each([&](Entity e,GroundEffect& f,Health& h){
  if(f.kind==GroundEffectKind::NitrogenSpill){pool=e;CHECK_NEAR(h.max,5,1e-6);CHECK_NEAR(f.damagePerSecond,0,1e-6);CHECK_NEAR(f.spreadTargetRadius,24.0,1e-6);CHECK(f.radius<f.spreadTargetRadius);}
 });CHECK(pool!=NULL_ENTITY);if(pool==NULL_ENTITY)return;
 Entity mob=r.mob(r.world.get<Transform>(pool).position);r.combatTick(5000);
 auto* status=r.world.tryGet<Afflictions>(mob);CHECK(status!=nullptr);if(status)CHECK_NEAR(status->frostSpeed(5000),.6,1e-6);
 CHECK_NEAR(r.world.get<Health>(mob).current,1000,1e-6);CHECK(!r.world.has<Afflictions>(r.player) || r.world.get<Afflictions>(r.player).frostSpeed(5000)==1);
 for(int tick=1;tick<=7;++tick)r.combatTick(5000+tick*50);
 CHECK_NEAR(r.world.get<GroundEffect>(pool).radius,24.0 - 5.0 + 0.40 * 10.0,1e-6);
 CHECK_NEAR(r.world.get<Body>(pool).radius,24.0 - 5.0 + 0.40 * 10.0,1e-6);
}
TEST(reference_banana_sticks_to_one_target_without_releasing_its_ring_slot) {
 ArcticBlast r("arctic_banana");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;Entity petal=live.front();
 Entity target=r.mob(r.world.get<Transform>(petal).position);r.combatTick(4100);
 CHECK(r.world.has<AttachedSilk>(petal));CHECK(!r.liveIn(0).empty());
 r.world.get<Transform>(target).position={5200,5100};r.tickPetals(4150);
 CHECK_NEAR(distance(r.world.get<Transform>(petal).position,r.world.get<Transform>(target).position),0,1e-6);
 r.world.add<Dead>(target);r.tickPetals(4200);CHECK(!r.world.isAlive(petal));
}
TEST(reference_radiation_radius_growth_uses_original_tier_formulas_before_range_buff) {
 const auto& c=testsupport::shippedContent();
 const auto& lithium=c.petal(c.petalIndex("void_lithium"));CHECK_NEAR(baseRadiationRadius(lithium,Rarity::Apex),1140,1e-6);
 const auto& stardust=c.petal(c.petalIndex("void_stardust"));CHECK_NEAR(baseRadiationRadius(stardust,Rarity::Unique),240,1e-6);
 const auto& corium=c.petal(c.petalIndex("void_corium"));CHECK_NEAR(baseRadiationRadius(corium,Rarity::Common),360,1e-6);CHECK_NEAR(corium.radiation.pulseDamage,0,1e-6);
}

TEST(thorns_cross_a_target_between_ticks_and_damage_it_before_breaking) {
    for (const char* id : {"spikeball", "arctic_pinecone"}) {
        ArcticBlast rig(id);
        const Entity petal = rig.liveIn(0).at(0);
        const Vec2 target{5500, 5500};
        const Entity mob = rig.mob(target);
        rig.world.ensure<ThornFlightStep>(petal) = {target - Vec2{160, 0}, rig.world.get<Transform>(petal).realm};
        rig.world.get<Transform>(petal).position = target + Vec2{160, 0};
        rig.combatTick(4100);
        CHECK(rig.world.get<Health>(mob).current < 1000);
        CHECK_EQ(rig.world.get<Health>(petal).current, 0);
        rig.tickPetals(4150);
        CHECK_EQ(Query<Projectile>{rig.world}.count(), std::string(id) == "spikeball" ? 9u : 8u);
    }
}

TEST(reference_pinecone_stays_on_ground_then_breaks_into_eight_thorns_once) {
 ArcticBlast r("arctic_pinecone");r.tickPetals(4500);auto petals=r.liveIn(0);CHECK(!petals.empty());if(petals.empty())return;
 Entity cone=petals.front();CHECK(r.world.get<PetalInstance>(cone).sapphireGrounded);
 Vec2 at=r.world.get<Transform>(cone).position;r.world.get<Transform>(r.player).position+=Vec2{300,100};r.tickPetals(4550);
 CHECK_NEAR(distance(at,r.world.get<Transform>(cone).position),0,1e-6);
 r.world.add<Dead>(cone);r.tickPetals(4600);int count=0;double total=0;
 Query<Projectile,Motion,Lifetime>{r.world}.without<Dead>().each([&](Entity,Projectile& p,Motion& m,Lifetime& l){
  if(p.petalConfigIndex!=r.registry.petalIndex("spikeball"))return;
  ++count;total+=p.damage;CHECK_NEAR(m.velocity.length(),1000,1e-6);CHECK_NEAR(l.remainingSeconds,1.2,1e-6);CHECK_NEAR(p.remainingDistance,1200,1e-6);
 });CHECK_EQ(count,8);CHECK_NEAR(total,r.registry.petalStats(r.registry.petalIndex("arctic_pinecone"),Rarity::Common).damage,1e-6);
 r.tickPetals(4650);int after=0;Query<Projectile>{r.world}.without<Dead>().each([&](Entity,Projectile& p){if(p.petalConfigIndex==r.registry.petalIndex("spikeball"))++after;});CHECK_EQ(after,8);
}

TEST(reference_balloon_adds_real_upward_drift_and_stops_when_broken) {
 ArcticBlast r("void_balloon");r.tickPetals(4500);
 const auto& mods=r.world.get<PlayerModifiers>(r.player);CHECK_NEAR(mods.petalDriftVelocity.x,0,1e-9);CHECK_NEAR(mods.petalDriftVelocity.y,-kPlayerMaxSpeed*.04,1e-9);
 MovementSystem movement;Terrain terrain;Vec2 before=r.world.get<Transform>(r.player).position;
 movement.run(r.world,terrain,4500,net::kTickSeconds);
 CHECK(r.world.get<Transform>(r.player).position.y<before.y);CHECK_NEAR(r.world.get<Transform>(r.player).position.x,before.x,1e-6);
 auto live=r.liveIn(0);for(Entity e:live)r.world.get<Health>(e).current=0;
 r.tickPetals(4550);CHECK_NEAR(r.world.get<PlayerModifiers>(r.player).petalDriftVelocity.length(),0,1e-9);
 CHECK(balloonDriftSpeed(Rarity::Apex)<=balloonDriftSpeed(Rarity::Unique)*1.5);
}
TEST(reference_nitro_follows_its_nozzle_then_serves_its_actual_reload) {
 ArcticBlast r("arctic_nitro");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 Entity nozzle=live.front();r.world.get<PetalInstance>(nozzle).facingAngle=kPi*.5;
 r.petals.foldModifiers(r.world,r.registry);
 const auto& mods=r.world.get<PlayerModifiers>(r.player);CHECK_NEAR(mods.speedScale,1,1e-9);CHECK_NEAR(mods.petalDriftVelocity.x,0,1e-8);CHECK_NEAR(mods.petalDriftVelocity.y,kPlayerMaxSpeed*.1,1e-8);
 const double start=r.world.get<PetalInstance>(nozzle).spawnedAtMillis;
 r.tickPetals(start+5500);r.tickPetals(start+5550);
 CHECK_NEAR(r.world.get<PlayerModifiers>(r.player).petalDriftVelocity.length(),0,1e-8);
 r.tickPetals(start+6000);CHECK(r.liveIn(0).empty());r.tickPetals(start+7600);CHECK(!r.liveIn(0).empty());
 CHECK(nitroDriftSpeed(Rarity::Apex)<=nitroDriftSpeed(Rarity::Unique)*1.5);
}
TEST(reference_saber_and_frost_have_long_narrow_contact_shapes) {
 for(const char* id:{"arctic_lightsaber","arctic_visaber","arctic_frost","void_fire"}) {
  ArcticBlast r(id);auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())continue;
  Entity blade=live.front();Vec2 at=r.world.get<Transform>(blade).position;r.world.get<PetalInstance>(blade).facingAngle=0;
  Entity along=r.mob(at+Vec2{15,0}),beside=r.mob(at+Vec2{0,15});r.combatTick(4500);
  CHECK_NEAR(r.world.get<Health>(beside).current,1000,1e-8);CHECK(!r.world.has<Afflictions>(beside));CHECK(!r.world.has<Burn>(beside));
  CHECK(r.world.get<Health>(along).current<1000 || r.world.has<Afflictions>(along) || r.world.has<Burn>(along));
 }
}

TEST(reference_stardust_carries_scaled_radiation_after_leaving_the_ring) {
 ArcticBlast r("void_stardust");r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(4500);
 std::vector<Entity> shots;Query<Projectile>{r.world}.without<Dead>().collect(shots);CHECK_EQ(shots.size(),1);if(shots.empty())return;
 Entity shot=shots.front();r.world.get<Projectile>(shot).effectStrength=.5;
 r.world.get<Transform>(shot).position={7000,5000};r.tickPetals(5000);
 Entity near=r.mob({7110,5000}),far=r.mob({7140,5000});r.combatTick(5000);
 const auto* status=r.world.tryGet<Afflictions>(near);CHECK(status!=nullptr);if(status)CHECK_NEAR(status->poisonPerSecond,14,1e-6);
 CHECK(!r.world.has<Afflictions>(far));
 r.world.get<Transform>(shot).position={7300,5000};r.world.get<Transform>(r.player).realm=Realm::Arena;r.tickPetals(5500);
 Query<GroundEffect,Transform>{r.world}.each([&](Entity,GroundEffect&,Transform& at){CHECK(distance(at.position,Vec2{7300,5000})>1);});
}
TEST(reference_fission_nuke_copies_share_one_break_budget) {
 ArcticBlast r("arctic_fission");r.equip(1,"arctic_nuke");r.tickPetals(5000);r.tickPetals(10000);
 auto copies=r.liveIn(1);CHECK_EQ(copies.size(),3);if(copies.size()!=3)return;
 for(Entity e:copies)r.world.add<Dead>(e);r.tickPetals(10050);
 double total=0;Query<GroundEffect>{r.world}.each([&](Entity,GroundEffect& f){total+=f.damagePerHit;});
 CHECK_NEAR(total,210,1e-6);
 r.tickPetals(10100);double again=0;Query<GroundEffect>{r.world}.each([&](Entity,GroundEffect& f){again+=f.damagePerHit;});CHECK_NEAR(again,total,1e-6);
}

TEST(reference_headwear_flags_are_replicable_and_follow_equipped_items) {
 ArcticBlast r("arctic_ears");r.equip(1,"void_halo");r.world.add<PlayerVisuals>(r.player);r.tickPetals(5000);
 CHECK(r.world.get<PlayerVisuals>(r.player).equipFlags&EquipCatEars);CHECK(r.world.get<PlayerVisuals>(r.player).equipFlags&EquipHalo);
 CHECK(r.liveIn(0).empty());CHECK(r.liveIn(1).empty());
 r.world.get<Loadout>(r.player).slots[0]={};r.tickPetals(5050);
 CHECK(!(r.world.get<PlayerVisuals>(r.player).equipFlags&EquipCatEars));CHECK(r.world.get<PlayerVisuals>(r.player).equipFlags&EquipHalo);
}
TEST(reference_oxygen_extends_real_stacked_and_radiation_burns_without_changing_their_rates) {
 ArcticBlast r("oxygen");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 Entity target=r.mob(r.world.get<Transform>(live.front()).position);
 r.world.add<MobType>(target,MobType{r.registry.mobIndex("ladybug"),Rarity::Common});
 Afflictions status;status.burns.push_back({live.front(),r.player,7,5100});status.burnPerSecond=3;status.burnSource=r.player;status.burnUntilMillis=5100;
 r.world.add<Afflictions>(target,status);r.world.add<Burn>(target,Burn{r.player,5,4000,5100});r.combatTick(5000);
 const auto& after=r.world.get<Afflictions>(target);CHECK_EQ(after.burns.size(),1);CHECK_NEAR(after.burns.front().untilMillis,5200,1e-6);
 CHECK_NEAR(after.burns.front().perSecond,7,1e-6);CHECK_NEAR(after.burnUntilMillis,5200,1e-6);CHECK_NEAR(r.world.get<Burn>(target).untilMillis,5200,1e-6);
}
TEST(reference_hyper_nuke_and_radiation_effects_obey_threefold_offense_growth) {
 for(const char* id:{"arctic_nuke","void_corium","arctic_plutonium"}) {
  double values[2]={};
  for(int h=0;h<2;++h) {
   ArcticBlast r(id,h?Rarity::Apex:Rarity::Unique);
   if(std::string(id)=="arctic_nuke")r.armBomb();else{r.tickPetals(100000);r.tickPetals(101000);}
   Query<GroundEffect>{r.world}.each([&](Entity,GroundEffect& f){values[h]+=f.damagePerHit+f.damagePerSecond+f.burnDamage;});
  }
  CHECK(values[0]>0);CHECK_NEAR(values[1],values[0]*3,std::max(1e-5,values[0]*1e-9));
 }
}

TEST(reference_quantum_superformula_uses_authored_tuples_and_stays_finite) {
 CHECK_NEAR(quantumOrbitRadiusScale(Rarity::Common,0),1,1e-8);
 CHECK_NEAR(quantumOrbitRadiusScale(Rarity::Common,kPi/10),std::pow(2.0,1.0/6.0),1e-8);
 for(int tier=0;tier<kRarityCount;++tier)for(int sample=0;sample<512;++sample) {
  double scale=quantumOrbitRadiusScale(static_cast<Rarity>(tier),kTau*sample/512);
  CHECK(std::isfinite(scale));CHECK(scale>=0);CHECK(scale<8);
 }
}

TEST(reference_scythe_shares_real_damage_and_durability_across_equipped_slots) {
 ArcticBlast alone("void_scythe"),shared("void_scythe");shared.equip(1,"void_halo");
 alone.tickPetals(20000);shared.tickPetals(20000);auto a=alone.liveIn(0),b=shared.liveIn(0);CHECK(!a.empty()&&!b.empty());if(a.empty()||b.empty())return;
 CHECK_NEAR(shared.world.get<Health>(b.front()).max,alone.world.get<Health>(a.front()).max*.5,1e-6);
 Entity one=alone.mob(alone.world.get<Transform>(a.front()).position),two=shared.mob(shared.world.get<Transform>(b.front()).position);
 alone.combatTick(4500);shared.combatTick(4500);
 CHECK_NEAR(1000-shared.world.get<Health>(two).current,(1000-alone.world.get<Health>(one).current)*.5,1e-6);
}
TEST(reference_all_imported_petal_descriptions_reach_the_actual_bilingual_lookup) {
 const auto& c=testsupport::shippedContent();auto previous=ui::language();int checked=0;
 for(std::uint16_t i=0;i<c.petalCount();++i) {
  const auto& v=c.petal(i);
  if(!v.biomeDropOnly && v.id.rfind("void_",0)!=0 && v.id.rfind("arctic_",0)!=0 && v.id.rfind("bio_",0)!=0 && v.id.rfind("jurassic_",0)!=0)continue;
  ++checked;
  ui::setLanguage(ui::Language::En);
  const std::string english = ui::petalDescTr(v.id, v.description.c_str());
  ui::setLanguage(ui::Language::Zh);
  const std::string chinese = ui::petalDescTr(v.id, v.description.c_str());
  // Language overrides take precedence over the optional JSON description.
  if (english.empty() || chinese.empty() || english == chinese)
      std::fprintf(stderr, "Missing bilingual description for %s\n", v.id.c_str());
  CHECK(!english.empty());CHECK(!chinese.empty());CHECK(chinese!=english);
 }
 CHECK_EQ(checked,219);ui::setLanguage(previous);
}

TEST(reference_original_headwear_produces_visible_pixels_on_default_and_builtin_skins) {
 const auto& c=testsupport::shippedContent();SpriteCache sprites;sprites.build(c,testsupport::dataDir());WorldRenderer renderer;renderer.setContent(&c);renderer.setSprites(&sprites);
 for(auto skin:{PlayerRenderNone,PlayerRenderPumpkin,PlayerRenderRobot}) {
  auto frame=[&](std::uint16_t flags){Canvas canvas=Canvas::createVirtual(128,128);canvas.clear(Color{12,12,12});canvas.translate(64,80);RemoteEntity flower;flower.kind=net::EntityKind::Player;flower.equipFlags=flags;flower.renderFlags=skin;renderer.drawFlowerBody(canvas,flower,0);return canvas.getImageData(0,0,128,128);};
  auto plain=frame(0);
  for(std::uint16_t flag:{static_cast<std::uint16_t>(EquipCatEars),static_cast<std::uint16_t>(EquipHalo)}) {
   auto worn=frame(flag);int different=0;
   for(int y=0;y<53;++y)for(int x=0;x<128;++x){auto p=(y*128+x)*4;if(plain[p]!=worn[p] || plain[p+1]!=worn[p+1] || plain[p+2]!=worn[p+2])++different;}
   CHECK(different>100);
  }
 }
}

TEST(reference_gamma_ray_sweeps_a_long_narrow_beam_instead_of_a_circle) {
 ArcticBlast r("void_gamma_ray");r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(4500);
 std::vector<Entity> shots;Query<Projectile>{r.world}.without<Dead>().collect(shots);CHECK_EQ(shots.size(),1);if(shots.empty())return;
 Entity beam=shots.front();Vec2 at=r.world.get<Transform>(beam).position;r.world.get<Transform>(beam).angle=0;
 CHECK_NEAR(r.world.get<Body>(beam).radius,40,1e-6);CHECK_NEAR(r.world.get<Projectile>(beam).collisionHalfWidth,2.5,1e-6);
 r.world.get<Projectile>(beam).lastPosition=at-Vec2{150,0};
 Entity along=r.mob(at-Vec2{100,0}),side=r.mob(at+Vec2{-100,20});r.combatTick(4500);
 CHECK(r.world.get<Health>(along).current<1000);CHECK_NEAR(r.world.get<Health>(side).current,1000,1e-6);
}
TEST(gamma_ray_keeps_its_super_ratio_to_sawblade_at_higher_tiers) {
    const auto& registry = testsupport::shippedContent();
    const auto gamma = registry.petalIndex("void_gamma_ray");
    const auto saw = registry.petalIndex("sawblade");
    const auto damage = [&](std::uint16_t index, Rarity tier) {
        return registry.petalStats(index, tier).damage;
    };
    const double ratio = damage(gamma, Rarity::Super) / damage(saw, Rarity::Super);
    CHECK_NEAR(ratio, .533, 1e-8);
    for (const auto tier : {Rarity::Unique, Rarity::Apex, Rarity::PermanentHyper, Rarity::Ascendant}) {
        // Only Apex sawblade receives the 10% contact damage bonus.
        const double expectedRatio = tier == Rarity::Ascendant ? ratio / 1.1 : ratio;
        CHECK_NEAR(damage(gamma, tier) / damage(saw, tier), expectedRatio, 1e-5);
        ArcticBlast rig("void_gamma_ray", tier);
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.tickPetals(4500);
        const auto shots = Query<Projectile>{rig.world}.without<Dead>().collect();
        CHECK_EQ(shots.size(), 1u);
        if (shots.size() != 1) continue;
        CHECK_NEAR(rig.world.get<Projectile>(shots.front()).damage, damage(gamma, tier), 1e-6);
    }
    CHECK(damage(gamma, Rarity::Ascendant) > damage(saw, Rarity::Apex));
}

TEST(reference_enemy_shots_hit_beam_tips_and_miss_its_sides_in_live_and_worker_paths) {
 for(bool parallel:{false,true})for(bool side:{false,true}) {
  ArcticBlast r("void_gamma_ray");r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(4500);
  std::vector<Entity> shots;Query<Projectile>{r.world}.without<Dead>().collect(shots);if(shots.empty()){CHECK(false);continue;}
  Entity beam=shots.front();r.world.get<Transform>(beam).angle=0;const Vec2 origin=r.world.get<Transform>(beam).position;
  Entity enemy=r.mob({8000,5000});
  auto shot=[&](Vec2 at,Entity owner,Team team){Entity e=r.world.create();r.world.add<ProjectileTag>(e);r.world.add<Transform>(e,Transform{at,0});r.world.add<Motion>(e,Motion{Vec2{1,0}});r.world.add<Body>(e,Body{1,1});r.world.add<Health>(e,Health{100,100});r.world.add<Faction>(e,Faction{team,false});r.world.add<HitCooldowns>(e);Projectile p;p.owner=p.creditTo=owner;p.damage=10;p.remainingDistance=100;p.lastPosition=at;r.world.add<Projectile>(e,p);};
  shot(origin+(side?Vec2{0,20}:Vec2{30,0}),enemy,Team::Hostiles);
  ServerWorkers workers(parallel?2:0);if(parallel){for(int i=0;i<128;++i)shot({12000.0+i*10,12000},r.player,Team::Players);r.combat.workers=&workers;r.combat.workerLimit=2;}
  const double before=r.world.get<Health>(beam).current;r.combatTick(4500);
  if(side)CHECK_NEAR(r.world.get<Health>(beam).current,before,1e-6);else CHECK(r.world.get<Health>(beam).current<before);
 }
}
TEST(reference_all_imported_petals_survive_equip_action_combat_and_realm_lifecycles) {
 const auto& c=testsupport::shippedContent();int configurations=0,runs=0;
 for(std::uint16_t i=0;i<c.petalCount();++i) {
  const auto& item=c.petal(i);
  if(!item.biomeDropOnly && item.id.rfind("void_",0)!=0 && item.id.rfind("arctic_",0)!=0 && item.id.rfind("bio_",0)!=0 && item.id.rfind("jurassic_",0)!=0)continue;
  ++configurations;
  for(auto rarity:{Rarity::Common,Rarity::Super,Rarity::Unique,Rarity::Apex}) {
   if(!petalComesAt(item,rarity))continue;++runs;
   ArcticBlast r(item.id.c_str(),rarity,"mana_orb");r.equip(1,"mana_orb",rarity);
   r.tickPetals(5000);if(auto* mana=r.world.tryGet<ManaPool>(r.player)){mana->current=1e12;mana->max=1e12;}
   r.tickPetals(1000000);r.world.get<Health>(r.player)={1e12,1e12};
   Entity target=r.mob({5100,5000});r.world.add<MobType>(target,MobType{r.registry.mobIndex("ladybug"),rarity});r.world.get<Health>(target)={1e12,1e12};
   for(int step=1;step<=20;++step) {
    if(auto* mana=r.world.tryGet<ManaPool>(r.player))mana->current=1e12;
    r.world.get<PlayerInput>(r.player).current.flags=step<10?net::InputAttack:net::InputDefend;
    r.tickPetals(1000000+step*50);r.combatTick(1000000+step*50);
   }
   r.world.get<Transform>(r.player).realm=Realm::Arena;r.tickPetals(1001100);r.combatTick(1001100);
   Query<Transform>{r.world}.each([&](Entity,Transform& at){CHECK(std::isfinite(at.position.x));CHECK(std::isfinite(at.position.y));CHECK(std::isfinite(at.angle));});
   Query<Health>{r.world}.each([&](Entity,Health& hp){CHECK(std::isfinite(hp.current));CHECK(std::isfinite(hp.max));CHECK(hp.current<=hp.max+1e-5);});
   r.world.get<Loadout>(r.player).slots[0]={};r.tickPetals(1001200);
  }
 }
 CHECK_EQ(configurations,219);CHECK(runs>=800);std::printf("  %d imported items exercised in %d tiered lifecycle runs\n",configurations,runs);
}
TEST(reference_quantum_equipment_changes_actual_sibling_orbit) {
 ArcticBlast circle("basic"),shape("basic");circle.equip(1,"basic");shape.equip(1,"void_quantum");
 double greatestDifference=0;
 for(int now=4050;now<20000;now+=50) {
  circle.tickPetals(now);shape.tickPetals(now);auto a=circle.liveIn(0),b=shape.liveIn(0);if(a.empty() || b.empty())continue;
  const Vec2 centre{5000,5000};double r1=distance(circle.world.get<Transform>(a.front()).position,centre),r2=distance(shape.world.get<Transform>(b.front()).position,centre);
  greatestDifference=std::max(greatestDifference,std::abs(r1-r2));
 }CHECK(greatestDifference>1);
}

TEST(arctic_frost_slows_the_orbit_and_the_ice_rose_applies_it_only_when_consumed) {
    ArcticBlast control("basic");
    ArcticBlast chilled("basic");
    chilled.world.ensure<Afflictions>(chilled.player).applyFrost(.6, 400, 4010);
    const double start = control.world.get<PetalRing>(control.player).spin;
    control.tickPetals(4050);
    chilled.tickPetals(4050);
    const double normalTurn = wrapAngle(control.world.get<PetalRing>(control.player).spin - start);
    const double frostTurn = wrapAngle(chilled.world.get<PetalRing>(chilled.player).spin - start);
    CHECK_NEAR(frostTurn, normalTurn * .6, 1e-9);
    const double afterFrost = chilled.world.get<PetalRing>(chilled.player).spin;
    chilled.tickPetals(4450);
    CHECK_NEAR(wrapAngle(chilled.world.get<PetalRing>(chilled.player).spin - afterFrost),
               normalTurn, 1e-9);

    ArcticBlast rose("arctic_ice_rose");
    CHECK(!rose.world.has<Afflictions>(rose.player) ||
          rose.world.get<Afflictions>(rose.player).frostSpeed(5000) == 1.0);
    const double injured = rose.world.get<Health>(rose.player).max * .5;
    rose.world.get<Health>(rose.player).current = injured;
    double consumedAt = 0.0;
    for (double now = 5000; now <= 8000; now += 50) {
        rose.tickPetals(now);
        const auto* frost = rose.world.tryGet<Afflictions>(rose.player);
        if (!frost || frost->frostSpeed(now) == 1.0) continue;
        consumedAt = now;
        CHECK_NEAR(frost->frostSpeed(now), .75, 1e-9);
        CHECK_NEAR(frost->frostSpeed(now + 500), 1.0, 1e-9);
        break;
    }
    CHECK(consumedAt > 0.0);
    CHECK(rose.world.get<Health>(rose.player).current > injured);
}

TEST(reference_recipe_petals_render_distinct_icons_and_localize_names) {
    const auto& registry = testsupport::shippedContent();
    SpriteCache sprites;
    sprites.build(registry, testsupport::dataDir());
    const auto previous = ui::language();
    std::set<std::vector<std::uint8_t>> renders;
    for (const char* id : {"gas", "blood_rose", "bloom", "bugatti", "latrine", "spikeball",
                           "vampire_fang", "ring", "ring_magnet", "gas_bomb", "bubble_bomb",
                           "hel_missile", "viper", "golden_cactus", "hel_honey", "irey",
                           "toilet_paper", "fractal", "marble", "magic_marble", "silk"}) {
        const auto index = registry.petalIndex(id);
        CHECK(index != kInvalidIndex);
        const auto& config = registry.petal(index);
        CHECK(config.image.find("<svg") == 0);
        auto canvas = Canvas::createVirtual(64, 64);
        canvas.clear(Color{7, 7, 7});
        const auto empty = canvas.getImageData(0, 0, 64, 64);
        sprites.drawPetal(canvas, index, Rarity::Common, 32, 32, 56, 0, 0);
        auto drawn = canvas.getImageData(0, 0, 64, 64);
        CHECK(drawn != empty);
        CHECK(renders.insert(std::move(drawn)).second);
        ui::setLanguage(ui::Language::Zh);
        CHECK(std::string(ui::petalNameTr(id, config.name.c_str())) != config.name);
        CHECK(std::string(ui::petalDescTr(id, config.description.c_str())) != config.description);
        ui::setLanguage(ui::Language::En);
        CHECK_EQ(std::string(ui::petalDescTr(id, config.description.c_str())), config.description);
    }
    ui::setLanguage(previous);
}

namespace {

// Match end-of-tick removal in the isolated combat fixture.
void cleanLongFightTick(ArcticBlast& rig) {
    std::vector<Entity> expired;
    Query<GroundEffect, Dead>{rig.world}.collect(expired);
    for (Entity e : expired) rig.world.destroy(e);
    rig.events.clear();
}

constexpr int kLongFightSeconds = 300;
constexpr int kLongFightTicks = kLongFightSeconds * net::kTicksPerSecond;

}

TEST(reference_long_radiation_hits_and_clears_over_five_minutes) {
    for (const char* id : {"uranium", "pyrite", "void_corium", "arctic_corium",
                           "arctic_plutonium", "void_stardust"}) {
        ArcticBlast rig(id);
        constexpr Vec2 centre{5000, 5000};
        const Entity target = rig.mob(centre);
        const Entity distant = rig.mob(centre + Vec2{2000, 0});
        constexpr double maxTargetHp = 1e8;
        rig.world.get<Health>(target) = {maxTargetHp, maxTargetHp};
        double previousTargetHp = maxTargetHp;
        int damagedWindows = 0;
        int selfDamageWindows = 0;
        bool emitted = false;
        for (int tick = 1; tick <= kLongFightTicks; ++tick) {
            const double now = 4000.0 + tick * net::kTickMillis;
            rig.tickPetals(now);
            Query<GroundEffect>{rig.world}.without<Dead>().each([&](Entity, GroundEffect& field) {
                emitted |= field.damagePerSecond > 0 || field.burnDamage > 0 || field.burnPerSecond > 0 || field.damagePerHit > 0;
            });
            rig.combatTick(now);
            cleanLongFightTick(rig);
            if (tick % (5 * net::kTicksPerSecond) != 0) continue;
            const double targetHp = rig.world.get<Health>(target).current;
            damagedWindows += targetHp < previousTargetHp;
            previousTargetHp = targetHp;
            auto& wearer = rig.world.get<Health>(rig.player);
            selfDamageWindows += wearer.current < wearer.max;
            CHECK(wearer.current > 0.0);
            // Replenish test health between windows. Leave all statuses intact.
            wearer.current = wearer.max;
        }
        CHECK(emitted);
        CHECK(damagedWindows > 0);
        CHECK_EQ(rig.world.get<Health>(distant).current, 1000.0);
        if (std::string_view(id) == "uranium") CHECK(selfDamageWindows > 0);
        rig.world.get<Loadout>(rig.player).slots[0] = {};
        for (int tick = 1; tick <= 15 * net::kTicksPerSecond; ++tick) {
            const double now = 4000.0 + (kLongFightTicks + tick) * net::kTickMillis;
            rig.tickPetals(now);
            rig.combatTick(now);
            cleanLongFightTick(rig);
        }
        std::vector<Entity> remaining;
        Query<GroundEffect>{rig.world}.without<Dead>().collect(remaining);
        CHECK(remaining.empty());
        std::printf("RADIATION %s sim_s=%d damaged_windows=%d self_damage_windows=%d active_after_unequip=%zu\n",
                    id, kLongFightSeconds, damagedWindows, selfDamageWindows, remaining.size());
    }
}

TEST(reference_long_charged_spills_damage_and_expire_over_five_minutes) {
    for (const char* id : {"acid", "hot_water", "lava", "oil", "latrine"}) {
        ArcticBlast rig(id);
        constexpr Vec2 outside{9000, 9000};
        const Entity target = rig.mob(outside);
        const Entity distant = rig.mob(outside);
        constexpr double maxTargetHp = 1e8;
        rig.world.get<Health>(target) = {maxTargetHp, maxTargetHp};
        int fieldWindows = 0;
        int maxActive = 0;
        double previousTargetHp = maxTargetHp;
        int damagedWindows = 0;
        for (int tick = 1; tick <= kLongFightTicks; ++tick) {
            const double now = 4000.0 + tick * net::kTickMillis;
            rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
            rig.tickPetals(now);
            int active = 0;
            Query<GroundEffect, Transform>{rig.world}.without<Dead>().each(
                [&](Entity, GroundEffect& field, Transform& at) {
                    if (field.owner != rig.player) return;
                    ++active;
                    rig.world.get<Transform>(target).position = at.position;
                });
            maxActive = std::max(maxActive, active);
            fieldWindows += active > 0;
            rig.combatTick(now);
            cleanLongFightTick(rig);
            if (tick % (5 * net::kTicksPerSecond) != 0) continue;
            const double targetHp = rig.world.get<Health>(target).current;
            damagedWindows += targetHp < previousTargetHp;
            previousTargetHp = targetHp;
        }
        CHECK(fieldWindows > 0);
        CHECK(damagedWindows > 0);
        CHECK(maxActive <= 12);
        CHECK_EQ(rig.world.get<Health>(distant).current, 1000.0);
        rig.world.get<Loadout>(rig.player).slots[0] = {};
        for (int tick = 1; tick <= 15 * net::kTicksPerSecond; ++tick) {
            const double now = 4000.0 + (kLongFightTicks + tick) * net::kTickMillis;
            rig.tickPetals(now);
            rig.combatTick(now);
            cleanLongFightTick(rig);
        }
        std::vector<Entity> remaining;
        Query<GroundEffect>{rig.world}.without<Dead>().collect(remaining);
        CHECK(remaining.empty());
        std::printf("SPILL %s sim_s=%d damaged_windows=%d concurrent=%d active_after_unequip=%zu\n",
                    id, kLongFightSeconds, damagedWindows, maxActive, remaining.size());
    }
}

TEST(reference_long_recipe_missiles_repeat_distinct_statuses) {
    for (const char* id : {"hel_missile", "viper"}) {
        ArcticBlast rig(id);
        Terrain terrain;
        MovementSystem movement;
        constexpr double maxTargetHp = 1e8;
        constexpr Vec2 waitingPoint{6200, 6200};
        const Entity target = rig.mob(waitingPoint);
        rig.world.get<Health>(target) = {maxTargetHp, maxTargetHp};
        const auto configIndex = rig.registry.petalIndex(id);
        const bool viper = std::string_view(id) == "viper";
        constexpr int windowTicks = 30 * net::kTicksPerSecond;
        int damageWindows = 0;
        for (int tick = 1; tick <= kLongFightTicks; ++tick) {
            const double now = 4000.0 + tick * net::kTickMillis;
            rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
            rig.tickPetals(now);
            rig.world.get<Transform>(target).position = waitingPoint;
            Query<Projectile, Transform>{rig.world}.without<Dead>().each(
                [&](Entity, Projectile& shot, Transform& at) {
                    if (shot.owner != rig.player || shot.petalConfigIndex != configIndex) return;
                    rig.world.get<Transform>(target).position = at.position;
                });
            movement.run(rig.world, terrain, now, net::kTickSeconds);
            rig.combatTick(now);
            std::vector<Entity> expired;
            Query<Projectile, Dead>{rig.world}.collect(expired);
            for (Entity e : expired) rig.world.destroy(e);
            rig.events.clear();
            if (tick % windowTicks != 0) continue;
            const auto* effects = rig.world.tryGet<Afflictions>(target);
            const bool correctEffect = effects && (viper ? effects->poisonUntilMillis > now - 30000.0 :
                effects->burnPerSecond > 0.0 || !effects->burns.empty());
            auto& hp = rig.world.get<Health>(target);
            damageWindows += correctEffect && hp.current < hp.max;
            hp.current = hp.max;
            rig.world.remove<Afflictions>(target);
        }
        CHECK_EQ(damageWindows, 10);
        std::printf("MISSILE %s sim_s=300 damage_status_windows=%d/10\n", id, damageWindows);
    }
}

TEST(reference_long_imported_projectile_types_reload_and_deal_damage) {
    Terrain terrain;
    for (const char* id : {"antibody", "arctic_blueberries", "arctic_electron_missile",
                           "arctic_icicle", "arctic_snowball", "arctic_snowflake",
                           "diamond", "electron_missile", "magic_electron_missile",
                           "magic_marble", "marble", "void_gamma_ray", "void_plasma",
                           "void_stardust", "void_thorn"}) {
        const std::string_view name(id);
        const bool breakTriggered = name == "diamond" || name == "arctic_snowflake";
        const bool magic = name.rfind("magic_", 0) == 0;
        ArcticBlast rig(id, Rarity::Common, magic ? "mana_orb" : nullptr);
        MovementSystem movement;
        constexpr double maxHp = 1e8;
        constexpr Vec2 waitingPoint{6200, 6200};
        const Entity target = rig.mob(waitingPoint);
        rig.world.get<Health>(target) = {maxHp, maxHp};
        const auto config = rig.registry.petalIndex(id);
        constexpr int windowTicks = 30 * net::kTicksPerSecond;
        int shotWindows = 0;
        int hitWindows = 0;
        bool armed = false;
        for (int tick = 1; tick <= kLongFightTicks; ++tick) {
            const double now = 4000.0 + tick * net::kTickMillis;
            rig.world.get<PlayerInput>(rig.player).current.flags =
                !breakTriggered && tick % 180 < 120 ? net::InputAttack : net::InputDefend;
            if (magic) {
                if (auto* mana = rig.world.tryGet<ManaPool>(rig.player))
                    mana->current = mana->max;
            }
            if (breakTriggered && tick % 100 == 1) {
                for (Entity petal : rig.liveIn(0)) rig.world.add<Dead>(petal);
            }
            rig.tickPetals(now);
            rig.world.get<Transform>(target).position = waitingPoint;
            Query<Projectile, Transform>{rig.world}.without<Dead>().each(
                [&](Entity, Projectile& shot, Transform& at) {
                    if (shot.owner != rig.player || shot.petalConfigIndex != config) return;
                    armed = true;
                    rig.world.get<Transform>(target).position = at.position;
                });
            movement.run(rig.world, terrain, now, net::kTickSeconds);
            rig.combatTick(now);
            std::vector<Entity> expired;
            Query<Projectile, Dead>{rig.world}.collect(expired);
            for (Entity e : expired) rig.world.destroy(e);
            rig.events.clear();
            if (tick % windowTicks != 0) continue;
            auto& hp = rig.world.get<Health>(target);
            shotWindows += armed;
            hitWindows += armed && hp.current < hp.max;
            hp.current = hp.max;
            armed = false;
        }
        CHECK_EQ(shotWindows, 10);
        CHECK_EQ(hitWindows, 10);
        std::printf("SHOT %s sim_s=300 shot_windows=%d/10 damage_windows=%d/10\n",
                    id, shotWindows, hitWindows);
    }
}

TEST(reference_long_remaining_fluid_types_trigger_their_own_effects) {
    constexpr Vec2 ownerPoint{5000, 5000};
    constexpr Vec2 waitingPoint{9000, 9000};
    constexpr double maxHp = 1e8;
    constexpr int windowTicks = 30 * net::kTicksPerSecond;
    for (const char* id : {"acid_bomb", "lava_bomb", "arctic_nitrogen", "elixir"}) {
        const std::string_view name(id);
        const bool nitrogen = name == "arctic_nitrogen";
        const bool elixir = name == "elixir";
        ArcticBlast rig(id);
        const auto& config = rig.registry.petal(rig.registry.petalIndex(id));
        const Entity target = rig.mob(waitingPoint);
        rig.world.get<Health>(target) = {maxHp, maxHp};
        int effectWindows = 0;
        bool appliedInWindow = false;
        for (int tick = 1; tick <= kLongFightTicks; ++tick) {
            const double now = 4000.0 + tick * net::kTickMillis;
            rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
            rig.tickPetals(now);
            Vec2 targetPoint = waitingPoint;
            if (config.spill.onHit) {
                for (const Entity petal : rig.liveIn(0)) {
                    if (!rig.world.has<Dead>(petal))
                        targetPoint = rig.world.get<Transform>(petal).position;
                }
            }
            bool pool = false;
            Query<GroundEffect, Transform>{rig.world}.without<Dead>().each(
                [&](Entity, GroundEffect& field, Transform& at) {
                    if (field.owner != rig.player) return;
                    pool = true;
                    if (elixir) rig.world.get<Transform>(rig.player).position = at.position;
                    else targetPoint = at.position;
                });
            rig.world.get<Transform>(target).position = targetPoint;
            const double targetBefore = rig.world.get<Health>(target).current;
            auto& owner = rig.world.get<Health>(rig.player);
            if (elixir) owner.current = owner.max / 2;
            const double ownerBefore = owner.current;
            rig.combatTick(now);
            if (pool) {
                const auto* status = rig.world.tryGet<Afflictions>(target);
                if (nitrogen) {
                    appliedInWindow |= status && status->frostSpeed(now) < 1.0;
                    CHECK_EQ(rig.world.get<Health>(target).current, maxHp);
                } else if (elixir) {
                    appliedInWindow |= rig.world.get<Health>(rig.player).current > ownerBefore;
                    CHECK_EQ(rig.world.get<Health>(target).current, maxHp);
                } else {
                    appliedInWindow |= rig.world.get<Health>(target).current < targetBefore;
                }
            }
            rig.world.get<Transform>(rig.player).position = ownerPoint;
            cleanLongFightTick(rig);
            if (tick % windowTicks != 0) continue;
            effectWindows += appliedInWindow;
            appliedInWindow = false;
        }
        CHECK_EQ(effectWindows, 10);
        std::printf("FLUID %s sim_s=300 correct_effect_windows=%d/10\n", id, effectWindows);
    }
}

TEST(reference_long_imported_summons_hatch_replace_and_recall) {
    const auto& content = testsupport::shippedContent();
    constexpr int expectedPetCap = 30;
    int checked = 0;
    for (std::uint16_t index = 0; index < content.petalCount(); ++index) {
        const auto& config = content.petal(index);
        const bool imported = config.biomeDropOnly || config.id.rfind("void_", 0) == 0 ||
            config.id.rfind("arctic_", 0) == 0 || config.id.rfind("bio_", 0) == 0 ||
            config.id.rfind("jurassic_", 0) == 0;
        if (!imported || config.petMobIndex == kInvalidIndex) continue;
        ++checked;
        constexpr auto rarity = Rarity::Common;
        ArcticBlast rig(config.id.c_str(), rarity);
        const int nominal = configuredSummonCount(config, rarity, nullptr, 0);
        const auto wanted = std::min(nominal, expectedPetCap);
        const auto pets = [&] {
            std::vector<Entity> live;
            auto* state = rig.world.tryGet<PetalSlotState>(rig.player);
            if (!state) return live;
            for (Entity pet : state->slots[0].pets) {
                if (!rig.world.isAlive(pet) || rig.world.has<Dead>(pet)) continue;
                const auto& mob = rig.world.get<MobType>(pet);
                // Chain segments and carrier offspring have their own mob IDs.
                if (mob.configIndex != config.petMobIndex) continue;
                CHECK_EQ(static_cast<int>(mob.rarity), static_cast<int>(configuredSummonRarity(config, rarity)));
                CHECK_EQ(rig.world.get<Pet>(pet).owner, rig.player);
                CHECK(std::isfinite(rig.world.get<Health>(pet).max));
                live.push_back(pet);
            }
            return live;
        };
        int peak = 0;
        bool respawned = false;
        constexpr int casualtyTick = 120 * net::kTicksPerSecond;
        for (int tick = 1; tick <= kLongFightTicks; ++tick) {
            const double now = 4000.0 + tick * net::kTickMillis;
            rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
            rig.tickPetals(now);
            if (tick % net::kTicksPerSecond != 0) continue;
            const auto live = pets();
            const auto count = static_cast<int>(live.size());
            CHECK(count <= expectedPetCap);
            if (config.petLifetimeMillis <= 0.0) CHECK(count <= wanted);
            peak = std::max(peak, count);
            if (tick == casualtyTick && !live.empty()) rig.world.destroy(live.front());
            if (tick > casualtyTick && count == wanted) respawned = true;
        }
        CHECK(peak >= wanted);
        CHECK(respawned);
        rig.world.get<Loadout>(rig.player).slots[0] = {};
        rig.tickPetals(4000.0 + kLongFightTicks * net::kTickMillis + 50.0);
        const auto after = pets().size();
        if (config.petLifetimeMillis <= 0.0) CHECK_EQ(after, 0u);
        std::printf("SUMMON %s sim_s=300 peak=%d expected=%d replenished=%d after_unequip=%zu\n",
                    config.id.c_str(), peak, wanted, respawned, after);
    }
    CHECK_EQ(checked, 61);
}

TEST(reference_long_all_imported_petals_run_with_combat_and_repeated_input) {
    const auto& content = testsupport::shippedContent();
    Terrain terrain;
    constexpr Vec2 centre{5000, 5000};
    constexpr double maxTargetHp = 1e9;
    int checked = 0;
    for (std::uint16_t index = 0; index < content.petalCount(); ++index) {
        const auto& config = content.petal(index);
        const bool imported = config.biomeDropOnly || config.id.rfind("void_", 0) == 0 ||
            config.id.rfind("arctic_", 0) == 0 || config.id.rfind("bio_", 0) == 0 ||
            config.id.rfind("jurassic_", 0) == 0;
        if (!imported) continue;
        ++checked;
        ArcticBlast rig(config.id.c_str(), Rarity::Super, "mana_orb");
        MovementSystem movement;
        const Entity target = rig.mob(centre + Vec2{110, 0});
        rig.world.get<Health>(target) = {maxTargetHp, maxTargetHp};
        int completed = 0;
        int liveSeconds = 0;
        int attackSeconds = 0;
        for (int tick = 1; tick <= kLongFightTicks; ++tick) {
            const double now = 4000.0 + tick * net::kTickMillis;
            auto& input = rig.world.get<PlayerInput>(rig.player).current;
            input.flags = ((tick / 50) % 3 == 2) ? net::InputDefend : net::InputAttack;
            input.aimAngle = (tick % 400) * 2 * kPi / 400;
            if (auto* mana = rig.world.tryGet<ManaPool>(rig.player)) mana->current = mana->max;
            rig.tickPetals(now);
            movement.run(rig.world, terrain, now, net::kTickSeconds);
            rig.combatTick(now);
            if (!rig.world.isAlive(rig.player) || rig.world.has<Dead>(rig.player)) break;
            std::vector<Entity> expired;
            Query<Dead>{rig.world}.collect(expired);
            for (Entity entity : expired) {
                if (entity != target) rig.world.destroy(entity);
            }
            rig.events.clear();
            if (tick % net::kTicksPerSecond != 0) continue;
            ++completed;
            const auto& spawned = rig.world.get<Loadout>(rig.player).spawned;
            liveSeconds += std::any_of(spawned.begin(), spawned.end(), [&](Entity e) {
                const auto* instance = rig.world.tryGet<PetalInstance>(e);
                return instance && instance->slot == 0 && !rig.world.has<Dead>(e);
            });
            attackSeconds += rig.world.get<Health>(target).current < maxTargetHp;
            auto& hp = rig.world.get<Health>(rig.player);
            CHECK(std::isfinite(hp.current));
            hp.current = hp.max;
            rig.world.get<Health>(target).current = maxTargetHp;
        }
        CHECK_EQ(completed, kLongFightSeconds);
        std::printf("ENDURANCE %s complete_s=%d live_slot_samples=%d target_damage_samples=%d\n",
                    config.id.c_str(), completed, liveSeconds, attackSeconds);
    }
    CHECK_EQ(checked, 219);
}



TEST(fluid_motion_grows_authoritative_radius_and_delays_outer_hits) {
 ArcticBlast r("basic");
 SpillSpec spill;spill.kind=SpillKind::Acid;spill.radius=100;spill.strength=10;spill.durationMillis=2000;
 r.world.get<Motion>(r.player).velocity={100,0};
 CombatSystem::createSpill(r.world,r.player,{5400,5000},Rarity::Common,spill,1,0);
 Entity pool=NULL_ENTITY;Query<GroundEffect>{r.world}.each([&](Entity e,GroundEffect&){pool=e;});CHECK(pool!=NULL_ENTITY);if(pool==NULL_ENTITY)return;
 const double finalRadius=r.world.get<GroundEffect>(pool).spreadTargetRadius;
 Entity outer=r.mob({5400+finalRadius*.95,5000});r.world.get<Health>(outer)={1e9,1e9};
 double previous=0;
 for(int tick=1;tick<=8;++tick){r.combatTick(5000+tick*50);const auto& f=r.world.get<GroundEffect>(pool);const auto& at=r.world.get<Transform>(pool);
  std::printf("fluid_tick,%d,%.4f,%.4f,%.4f,%.4f\n",tick,at.position.x,at.position.y,f.radius,r.world.get<Health>(outer).current);
  CHECK(f.radius>=previous);CHECK(f.radius<=finalRadius+1e-8);previous=f.radius;
  if(tick==1)CHECK_NEAR(r.world.get<Health>(outer).current,1e9,1e-6);
 }
 CHECK_NEAR(previous,finalRadius,1e-6);CHECK(r.world.get<Health>(outer).current<1e9);
 CHECK_NEAR(r.world.get<Transform>(pool).position.x,5400+35*.34,1e-6);
}
TEST(fluid_motion_bomb_collision_pools_expand_without_repeated_birth) {
 for(const char* id:{"acid_bomb","lava_bomb"}) {
  ArcticBlast r(id);r.world.get<Motion>(r.player).velocity={0,200};
  auto bombs=r.liveIn(0);CHECK(!bombs.empty());if(bombs.empty())continue;
  const Vec2 origin=r.world.get<Transform>(bombs.front()).position;r.mob(origin);
  r.combatTick(4050);Entity pool=NULL_ENTITY;Query<GroundEffect>{r.world}.each([&](Entity e,GroundEffect& f){if(isFluidField(f.kind))pool=e;});
  CHECK(pool!=NULL_ENTITY);if(pool==NULL_ENTITY)continue;
  const double target=r.world.get<GroundEffect>(pool).spreadTargetRadius;
  CHECK(r.world.get<GroundEffect>(pool).radius<target);
  for(int tick=1;tick<=8;++tick)r.combatTick(4050+tick*50);
  CHECK_NEAR(r.world.get<GroundEffect>(pool).radius,target - 5.0 + 10.0 * r.world.get<GroundEffect>(pool).spreadAgeSeconds,1e-6);
  CHECK(r.world.get<Transform>(pool).position.y>origin.y);
  int count=0;Query<GroundEffect>{r.world}.without<Dead>().each([&](Entity,GroundEffect& f){if(isFluidField(f.kind))++count;});CHECK_EQ(count,1);
  r.world.get<Transform>(r.player).realm=Realm::Arena;r.combatTick(4500);CHECK(r.world.has<Dead>(pool)||!r.world.isAlive(pool));
 }
}
TEST(fluid_bombs_spill_on_practice_dummies_and_record_the_pool_damage) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const char* id : {"acid_bomb", "lava_bomb"}) {
        ArcticBlast rig(id);
        const auto petals = rig.liveIn(0);
        CHECK_EQ(petals.size(), 1u);
        if (petals.size() != 1) continue;
        const auto at = rig.world.get<Transform>(petals.front()).position;
        const auto dummy = rig.dummy(at);
        rig.combatTick(4050);
        const auto pools = Query<GroundEffect>{rig.world}.without<Dead>().collect();
        CHECK_EQ(pools.size(), 1u);
        if (pools.size() != 1) continue;
        const auto pool = pools.front();
        const auto& field = rig.world.get<GroundEffect>(pool);
        CHECK_EQ(field.owner, rig.player);
        CHECK_EQ(field.kind, std::string_view(id) == "acid_bomb" ? GroundEffectKind::AcidSpill : GroundEffectKind::LavaSpill);
        CHECK_NEAR(distance(at, rig.world.get<Transform>(pool).position), 0.0, 1e-6);
        CHECK(rig.world.get<Health>(petals.front()).current <= 0.0);
        rig.tickPetals(4100);
        rig.combat.resetPracticeDamage(rig.player);
        for (int i = 0; i < 20; ++i) rig.combatTick(4100 + i * 50);
        double total = 0.0;
        for (const auto& row : rig.combat.practiceDamage(rig.player, 5100, dummy)) total += row.amount;
        CHECK(total > 0.0);
        CHECK_EQ(rig.world.get<Health>(dummy).current, rig.world.get<Health>(dummy).max);
        CHECK_EQ(Query<GroundEffect>{rig.world}.without<Dead>().count(), 1u);
    }
}

TEST(fluid_motion_walls_block_drift_and_damage_across_the_wall) {
 ArcticBlast r("basic");Terrain terrain;
 CHECK(terrain.setTiles(std::vector<std::uint8_t>(32*32,0),32,32));
 for(int y=0;y<32;++y)terrain.setTile(20,y,Tile::Solid);
 r.combat.terrain=&terrain;r.world.get<Motion>(r.player).velocity={1000,0};
 SpillSpec spill;spill.kind=SpillKind::Acid;spill.radius=160;spill.strength=10;spill.durationMillis=2000;
 CombatSystem::createSpill(r.world,r.player,{5100,5000},Rarity::Common,spill,1,0);
 Entity beyond=r.mob({5160,5000});r.world.get<Health>(beyond)={10000,10000};
 for(int i=0;i<12;++i)r.combatTick(5000+i*50);
 CHECK_NEAR(r.world.get<Health>(beyond).current,10000,1e-6);
 Query<GroundEffect,Transform>{r.world}.each([&](Entity,GroundEffect& f,Transform& at){CHECK(at.position.x<5120);CHECK_NEAR(f.spillDrift.length(),0,1e-6);});
}
TEST(fluid_motion_repeated_release_budget_and_expiration_stay_bounded) {
 ArcticBlast r("basic");Entity target=r.mob({5400,5000});r.world.get<Health>(target)={1e9,1e9};
 SpillSpec spill;spill.kind=SpillKind::Acid;spill.radius=100;spill.strength=10;spill.durationMillis=1000;
 double expected=0;
 for(int tick=0;tick<2400;++tick){
  if(tick%4==0)CombatSystem::createSpill(r.world,r.player,{5400,5000},Rarity::Common,spill,1,0);
  Query<GroundEffect,Lifetime>{r.world}.without<Dead>().each([&](Entity,GroundEffect& f,Lifetime& life){if(life.remainingSeconds>net::kTickSeconds)expected+=f.damagePerSecond*net::kTickSeconds;});
  r.combatTick(5000+tick*50);
  int live=0;Query<GroundEffect>{r.world}.without<Dead>().each([&](Entity,GroundEffect&){++live;});CHECK(live<=12);
 }
 CHECK_NEAR(1e9-r.world.get<Health>(target).current,expected,.01);
 for(int tick=0;tick<30;++tick)r.combatTick(125000+tick*50);
 int live=0;Query<GroundEffect>{r.world}.without<Dead>().each([&](Entity,GroundEffect&){++live;});CHECK_EQ(live,0);
}
TEST(fluid_motion_direct_attack_release_captures_moving_flow) {
 for(const char* id:{"acid","lava"}) {
  ArcticBlast r(id);r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;
  r.world.get<Motion>(r.player).velocity={120,60};
  Entity pool=NULL_ENTITY;
  for(int tick=0;tick<100&&pool==NULL_ENTITY;++tick){r.tickPetals(4500+tick*50);Query<GroundEffect>{r.world}.without<Dead>().each([&](Entity e,GroundEffect& f){if(isFluidField(f.kind))pool=e;});}
  CHECK(pool!=NULL_ENTITY);if(pool==NULL_ENTITY)continue;
  const auto start=r.world.get<Transform>(pool).position;const auto radius=r.world.get<GroundEffect>(pool).radius;
  for(int tick=0;tick<8;++tick)r.combatTick(10000+tick*50);
  const auto end=r.world.get<Transform>(pool).position;CHECK(end.x>start.x);CHECK(end.y>start.y);
  CHECK(r.world.get<GroundEffect>(pool).radius>radius);
 }
}
TEST(void_fire_burn_uses_the_owners_live_petal_damage_bonus) {
    ArcticBlast rig("void_fire");
    const auto petals = rig.liveIn(0);
    CHECK_EQ(petals.size(), std::size_t(1));
    if (petals.size() != 1) return;
    rig.world.get<PlayerModifiers>(rig.player).petalDamageScale = 1.5;
    const Entity mob = rig.mob(rig.world.get<Transform>(petals.front()).position);
    rig.combatTick(4050);
    const auto* status = rig.world.tryGet<Afflictions>(mob);
    CHECK(status && status->burns.size() == 1);
    if (!status || status->burns.size() != 1) return;
    CHECK_NEAR(status->burns.front().perSecond, 15.0, 1e-6);
    CHECK_NEAR(status->burns.front().untilMillis, 7050.0, 1e-6);
    CHECK_NEAR(rig.world.get<Health>(mob).current, 1000.0, 1e-6);
}

TEST(bio_147_opal_crits_at_matching_or_lower_tiers) {
    const auto sample = [](Rarity tier) {
        ArcticBlast rig("opal", Rarity::Rare);
        rig.equip(1, "basic", tier);
        rig.tickPetals(4100);
        rig.tickPetals(10000);
        const auto equipped = rig.liveIn(1);
        CHECK_EQ(equipped.size(), 1u);
        if (equipped.empty()) return 0;
        const Entity petal = equipped[0];
        const Vec2 at{5400, 5400};
        rig.world.get<Transform>(petal).position = at;
        const double direct = rig.registry.petalStats(rig.registry.petalIndex("basic"), tier).damage;
        const Entity target = rig.mob(at);
        int crits = 0;
        for (int i = 0; i < 600; ++i) {
            rig.world.get<Transform>(petal).position = at;
            rig.world.get<Health>(petal).current = rig.world.get<Health>(petal).max;
            rig.world.get<Health>(target) = Health{1000, 1000};
            rig.combatTick(10100.0 + i * 1000.0);
            const double dealt = 1000.0 - rig.world.get<Health>(target).current;
            if (std::abs(dealt - direct) > 1e-7) {
                ++crits;
                CHECK_NEAR(dealt, direct * (1.5 + ladderIndex(tier) * (1.5 / 8.0)), 1e-7);
            }
        }
        std::printf("Bio Opal Rare, attack=%s: %d/600 crits, base=%.2f\n",
                    rarityName(tier), crits, direct);
        return crits;
    };
    const int lower = sample(Rarity::Common);
    CHECK(lower >= 25 && lower <= 80);
    const int matched = sample(Rarity::Rare);
    CHECK(matched >= 35 && matched <= 90);
    CHECK_EQ(sample(Rarity::Epic), 0);

    const auto saved = ui::language();
    ui::setLanguage(ui::Language::En);
    CHECK(std::string_view(ui::petalDescTr("opal", "")).find("its rarity or lower") != std::string_view::npos);
    CHECK(std::string_view(ui::petalDescTr("electron_missile", "")).find("chain lightning") != std::string_view::npos);
    ui::setLanguage(ui::Language::Zh);
    CHECK(std::string_view(ui::petalDescTr("opal", "")).find("及以下") != std::string_view::npos);
    ui::setLanguage(saved);
}

TEST(bio_147_electron_missile_hits_a_second_target_with_lightning) {
    ArcticBlast rig("electron_missile");
    rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
    rig.tickPetals(4500);
    // The ready attack starts the 500 ms preparation.
    rig.tickPetals(4950);
    CHECK(Query<Projectile>{rig.world}.without<Dead>().collect().empty());
    rig.tickPetals(5000);
    const auto shots = Query<Projectile>{rig.world}.without<Dead>().collect();
    CHECK_EQ(shots.size(), 1u);
    if (shots.empty()) return;
    const Vec2 at = rig.world.get<Transform>(shots[0]).position;
    const Entity first = rig.mob(at);
    const Entity second = rig.mob(at + Vec2{80, 0});
    rig.combatTick(5000);
    CHECK(!rig.combat.shotLandings().empty());
    rig.petals.landShots(rig.world, rig.registry, rig.combat.shotLandings(), nullptr, 5000, &rig.events);
    rig.combatTick(5050);
    const double firstHit = 1000.0 - rig.world.get<Health>(first).current;
    const double secondHit = 1000.0 - rig.world.get<Health>(second).current;
    std::printf("Bio Electron Missile Common impact=%.2f chain=%.2f\n", firstHit, secondHit);
    CHECK(firstHit > 0);
    CHECK_NEAR(secondHit, 3.0, 1e-8);
}

TEST(fluid_radiation_pyrite_burn_doubles_oil_only_until_expiry) {
    ArcticBlast rig("pyrite");
    rig.tickPetals(12000);
    Vec2 centre;
    int found = 0;
    Query<GroundEffect, Transform>{rig.world}.without<Dead>().each(
        [&](Entity, GroundEffect& effect, Transform& at) {
            if (effect.burnPerSecond > 0.0) {
                centre = at.position;
                ++found;
            }
        });
    CHECK(found > 0);
    if (!found) return;
    const Entity doubled = rig.mob(centre);
    const Entity burnOnly = rig.mob(centre);
    rig.world.get<Health>(doubled) = {10000.0, 10000.0};
    rig.world.get<Health>(burnOnly) = {10000.0, 10000.0};
    rig.combatTick(12000);
    const auto* status = rig.world.tryGet<Afflictions>(doubled);
    CHECK(status && !status->burns.empty());
    if (!status || status->burns.empty()) return;
    CHECK_NEAR(status->burns.front().untilMillis, 15000, 1e-8);

    const Vec2 poolAt = centre + Vec2{0, 520};
    rig.world.get<Transform>(doubled).position = poolAt + Vec2{10, 0};
    rig.world.get<Transform>(burnOnly).position = poolAt + Vec2{175, 0};
    const Entity cold = rig.mob(poolAt + Vec2{-10, 0});
    rig.world.get<Health>(cold) = {10000.0, 10000.0};
    const auto& oil = rig.registry.petal(rig.registry.petalIndex("oil")).spill;
    const auto hp = [&](Entity target) { return rig.world.get<Health>(target).current; };
    CombatSystem::createSpill(rig.world, rig.player, poolAt, Rarity::Common, oil, 1.0, 0);
    const double oldA = hp(doubled), oldB = hp(burnOnly), oldCold = hp(cold);
    rig.combatTick(12050);
    const double coldOil = oldCold - hp(cold);
    const double hotOil = (oldA - hp(doubled)) - (oldB - hp(burnOnly));
    std::printf("Pyrite ignition oil damage per tick cold=%.6f hot=%.6f\n", coldOil, hotOil);
    CHECK(coldOil > 0.0);
    CHECK_NEAR(hotOil, 2.0 * coldOil, 1e-6);

    for (int now = 12100; now <= 15000; now += 50) rig.combatTick(now);
    CombatSystem::createSpill(rig.world, rig.player, poolAt, Rarity::Common, oil, 1.0, 0);
    const double noLongerBurning = hp(doubled);
    const double coldBefore = hp(cold);
    rig.combatTick(15050);
    // The first pool still overlaps the new one, and both now do the cold rate.
    CHECK_NEAR(coldBefore - hp(cold), 2.0 * coldOil, 1e-6);
    CHECK_NEAR(noLongerBurning - hp(doubled), coldBefore - hp(cold), 1e-6);
}

TEST(fluid_motion_blood_spreads_before_shred_and_prunes_after_crossing) {
    ArcticBlast rig("blood");
    rig.world.get<Motion>(rig.player).velocity = {100.0, 0.0};
    rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
    rig.tickPetals(5000);
    rig.tickPetals(6000);
    Entity pool = NULL_ENTITY;
    Query<GroundEffect>{rig.world}.without<Dead>().each([&](Entity e, GroundEffect& effect) {
        if (effect.kind == GroundEffectKind::Blood) pool = e;
    });
    CHECK(pool != NULL_ENTITY);
    if (pool == NULL_ENTITY) return;
    const Vec2 at = rig.world.get<Transform>(pool).position;
    const double radius = bloodPoolRadius(Rarity::Common);
    const Entity mob = rig.mob(at + Vec2{0, radius - 2.0});
    rig.world.add<Armor>(mob, Armor{100.0});
    rig.combatTick(6050);
    CHECK_NEAR(CombatSystem::effectiveArmor(rig.world, mob, 6050), 100.0, 1e-8);
    for (int now = 6100; now <= 6350; now += 50) rig.combatTick(now);
    CHECK_NEAR(rig.world.get<GroundEffect>(pool).radius, radius - 5.0 + 3.5, 1e-8);
    CHECK(rig.world.get<Transform>(pool).position.x > at.x);
    const double shred = rig.registry.petalStats(rig.registry.petalIndex("blood"),
                                                 Rarity::Common).armorReduction;
    CHECK_NEAR(CombatSystem::effectiveArmor(rig.world, mob, 6350), 100.0 - shred, 1e-8);
    CHECK_NEAR(rig.world.get<Health>(mob).current, 1000.0, 1e-8);
    rig.world.get<Transform>(rig.player).realm = Realm::Arena;
    rig.combatTick(6400);
    CHECK(rig.world.has<Dead>(pool));
    CHECK_NEAR(rig.world.get<Afflictions>(mob).armorShredUntilMillis,
               6350 + kArmorShredMillis, 1e-8);
    rig.combatTick(6350 + kArmorShredMillis);
    CHECK_NEAR(CombatSystem::effectiveArmor(rig.world, mob, 6350 + kArmorShredMillis),
               100.0, 1e-8);
}

TEST(fluid_radiation_hot_water_stacks_burns_and_thaws_after_leaving) {
    ArcticBlast rig("basic");
    const auto& hot = rig.registry.petal(rig.registry.petalIndex("hot_water")).spill;
    const Vec2 at{5500, 5500};
    const Entity target = rig.mob(at);
    CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, hot, 1.0, 0);
    CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, hot, 1.0, 0);
    rig.combatTick(4100);
    const auto* status = rig.world.tryGet<Afflictions>(target);
    CHECK(status && status->burns.size() == 2);
    if (!status || status->burns.size() != 2) return;
    CHECK_NEAR(status->frostSpeed(4100), 0.5, 1e-8);
    CHECK_NEAR(status->burns[0].perSecond, 8.96 * 2.1875, 1e-8);
    CHECK_NEAR(status->burns[1].perSecond, 8.96 * 2.1875, 1e-8);
    CHECK_NEAR(status->burns[0].untilMillis, 5500.0, 1e-8);
    rig.world.get<Transform>(target).position = at + Vec2{0, 500};
    rig.combatTick(4150);
    CHECK_NEAR(rig.world.get<Health>(target).current, 1000.0 - 2.0 * 19.6 * net::kTickSeconds, 1e-8);
    rig.combatTick(4650);
    CHECK_NEAR(rig.world.get<Afflictions>(target).frostSpeed(4650), 1.0, 1e-8);
    const double beforeExpiry = rig.world.get<Health>(target).current;
    rig.combatTick(5550);
    CHECK_NEAR(rig.world.get<Health>(target).current, beforeExpiry, 1e-8);
}

TEST(fluid_radiation_elixir_heals_only_available_same_realm_squadmates) {
    ArcticBlast rig("basic");
    SquadEntityIndex squad;
    rig.combat.squads = &squad;
    const Vec2 at{5500, 5500};
    const auto makeFlower = [&](Vec2 position, Realm realm = Realm::Overworld) {
        const Entity e = rig.world.create();
        rig.world.add<PlayerTag>(e);
        rig.world.add<Transform>(e, Transform{position, 0.0, realm});
        rig.world.add<Body>(e, Body{5.0, 1.0});
        rig.world.add<Health>(e, Health{100, 1000});
        rig.world.add<PlayerModifiers>(e);
        return e;
    };
    const Entity teammate = makeFlower(at + Vec2{10, 0});
    const Entity stranger = makeFlower(at + Vec2{-10, 0});
    const Entity elsewhere = makeFlower(at + Vec2{0, 10}, Realm::Arena);
    const Entity blocked = makeFlower(at + Vec2{0, -10});
    rig.world.add<Afflictions>(blocked).noHealUntilMillis = 5000;
    squad.groups.push_back({{rig.player, 1}, {teammate, 2}, {elsewhere, 3}, {blocked, 4}});
    for (Entity e : {rig.player, teammate, elsewhere, blocked}) squad.group.emplace(e, 0);
    const auto& elixir = rig.registry.petal(rig.registry.petalIndex("elixir")).spill;
    CombatSystem::createSpill(rig.world, rig.player, at, Rarity::Common, elixir, 1.0, 0);
    rig.combatTick(4050);
    const auto hp = [&](Entity e) { return rig.world.get<Health>(e).current; };
    CHECK_NEAR(hp(teammate), 100.0 + 2.25 * kPlayerHealMultiplier * net::kTickSeconds, 1e-8);
    CHECK_NEAR(hp(stranger), 100.0, 1e-8);
    CHECK_NEAR(hp(elsewhere), 100.0, 1e-8);
    CHECK_NEAR(hp(blocked), 100.0, 1e-8);
    rig.world.get<Transform>(teammate).position = at + Vec2{0, 500};
    rig.combatTick(4100);
    CHECK_NEAR(hp(teammate), 100.0 + 2.25 * kPlayerHealMultiplier * net::kTickSeconds, 1e-8);
}

TEST(magic_tomato_ripens_once_per_body_when_mana_is_available) {
    ArcticBlast rig("basic", Rarity::Common, "magic_soil");
    rig.equip(0, "magic_tomato");
    const auto mana = [&]() -> ManaPool& { return rig.world.get<ManaPool>(rig.player); };
    const auto allocate = [&](int from, int to) {
        for (int now = from; now <= to; now += 50) {
            mana().current = 0.5;
            rig.tickPetals(now);
        }
    };
    allocate(4050, 7000);
    const auto petals = rig.liveIn(0);
    CHECK_EQ(petals.size(), 1u);
    if (petals.empty()) return;
    const Entity petal = petals.front();
    const auto hit = [&](Entity piece, double tick) {
        const auto at = rig.world.get<Transform>(piece).position;
        const Entity victim = rig.mob(at);
        rig.combatTick(tick);
        const double dealt = 1000.0 - rig.world.get<Health>(victim).current;
        rig.world.destroy(victim);
        return dealt;
    };
    const double greenHit = hit(petal, 7000);
    allocate(7050, 12000);
    CHECK_NEAR(hit(petal, 12000), greenHit, 1e-7);
    mana().current = 9.0;
    rig.tickPetals(12050);
    const double paidRemainder = mana().current;
    CHECK(paidRemainder < 1.0);
    const double ripeHit = hit(petal, 12050);
    std::printf("magic tomato green=%.6f ripe=%.6f remaining_mana=%.6f\n",
                greenHit, ripeHit, paidRemainder);
    CHECK(greenHit > 0.0);
    CHECK_NEAR(ripeHit, greenHit * 10.0, 1e-7);
    mana().current = 9.0;
    rig.tickPetals(12100);
    CHECK(mana().current >= 9.0);
    rig.world.get<Health>(petal).current = 0.0;
    rig.world.add<Dead>(petal);
    allocate(12150, 17000);
    const auto respawned = rig.liveIn(0);
    CHECK_EQ(respawned.size(), 1u);
    if (respawned.empty()) return;
    CHECK(respawned.front() != petal);
    CHECK_NEAR(hit(respawned.front(), 17000), greenHit, 1e-7);
}

TEST(content_icicle_freezes_a_real_projectile_target_and_recovers) {
 for (auto rarity : {Rarity::Common,Rarity::Super,Rarity::Unique,Rarity::Apex}) {
 for (bool slowFirst : {true,false}) for (auto targetTier : {Rarity::Common,rarity,Rarity::Apex}) {
  ArcticBlast r("arctic_icicle",rarity);
  r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;
  r.tickPetals(4500);
  std::vector<Entity> shots;Query<Projectile>{r.world}.without<Dead>().collect(shots);
  CHECK(!shots.empty());if(shots.empty())continue;
  Entity shot=shots.front();const auto at=r.world.get<Transform>(shot).position;
  Entity target=r.mob(at);r.world.add<MobType>(target,MobType{r.registry.mobIndex("ladybug"),targetTier});
  r.world.get<Health>(target)={1e12,1e12};
  if(slowFirst) r.combat.applySlow(r.world,target,.6,4500,targetTier,4500);
  r.combatTick(4500);
  auto* status=r.world.tryGet<Afflictions>(target);CHECK(status!=nullptr);if(!status)continue;
  CHECK_NEAR(status->freezeFactor,1.0-stallPower(rarity,targetTier),1e-9);
  if(!slowFirst) r.combat.applySlow(r.world,target,.6,4500,targetTier,4500);
  CHECK_NEAR(r.world.get<Afflictions>(target).slowFactor,.6,1e-9);
  CHECK_NEAR(r.world.get<Afflictions>(target).slowUntilMillis,9000,1e-9);
  const double duration=200+100*ladderIndex(rarity);
  CHECK_NEAR(status->freezeUntilMillis,4500+duration,1e-6);
  CHECK(status->freezeSpeed(4500+duration-1)<1);CHECK_NEAR(status->freezeSpeed(4500+duration),1,1e-9);
 }
}
}

TEST(content_snowflake_waits_for_break_then_fires_one_six_fragment_volley) {
 for(auto tier:{Rarity::Common,Rarity::Super,Rarity::Unique,Rarity::Apex}) {
 ArcticBlast r("arctic_snowflake",tier);r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;
 r.tickPetals(4500);CHECK_EQ(Query<Projectile>{r.world}.without<Dead>().collect().size(),0);
 auto petals=r.liveIn(0);CHECK_EQ(petals.size(),1);if(petals.empty())return;
 auto target=r.mob(r.world.get<Transform>(petals.front()).position);r.world.get<Health>(target)={1e12,1e12};r.world.add<ContactDamage>(target,ContactDamage{1e9,0});
 const double before=r.world.get<Health>(target).current;r.combatTick(4500);CHECK(r.world.get<Health>(target).current<before);
 r.tickPetals(4550);auto shots=Query<Projectile>{r.world}.without<Dead>().collect();CHECK_EQ(shots.size(),6);
 const double expected=6*r.registry.petalStats(r.registry.petalIndex("arctic_snowflake"),tier).damage;
 double budget=0;for(Entity e:shots)budget+=r.world.get<Projectile>(e).damage;CHECK_NEAR(budget,expected,1e-6);
 r.tickPetals(4600);CHECK_EQ(Query<Projectile>{r.world}.without<Dead>().collect().size(),6);
}
}
TEST(content_snowball_tier_count_and_total_shot_damage_and_health_budgets) {
 for(auto tier:{Rarity::Common,Rarity::Mythic,Rarity::Ultra,Rarity::Super,Rarity::Unique,Rarity::Apex,Rarity::PermanentHyper,Rarity::Ascendant}) {
  ArcticBlast r("arctic_snowball",tier);const auto index=r.registry.petalIndex("arctic_snowball");const auto stats=r.registry.petalStats(index,tier);
  const int count=ladderIndex(tier)>=8?5:ladderIndex(tier)>=7?3:ladderIndex(tier)>=5?2:1;
  CHECK_EQ(stats.count,count);CHECK_EQ(r.liveIn(0).size(),count);
  r.world.get<PlayerInput>(r.player).current.flags=net::InputAttack;r.tickPetals(4500);
  const auto shots=Query<Projectile>{r.world}.without<Dead>().collect();CHECK_EQ(shots.size(),count);
  double damage=0,health=0;for(Entity e:shots){damage+=r.world.get<Projectile>(e).damage;health+=r.world.get<Health>(e).max;}
  CHECK_NEAR(damage,stats.damage*count,1e-5);CHECK_NEAR(health,stats.health*count,1e-5);
  std::printf("snowball,%s,%d,%.5f,%.5f\n",kRarityNames[rarityIndex(tier)],count,damage,health);
 }
}

TEST(radiation_talents_change_real_hits_and_keep_wearer_poison) {
    struct Result {
        double innerDamage = 0.0;
        double outerDamage = 0.0;
        double ownerPoison = 0.0;
        double radius = 0.0;
    };
    auto run = [&](int reach, int power, bool refund) {
        ArcticBlast rig("uranium", Rarity::Apex);
        auto& tree = rig.world.add<PlayerSkillTree>(rig.player).skills;
        tree.set(SkillId::RadiationReach, reach);
        tree.set(SkillId::RadiationPower, power);
        if (refund) {
            rig.tickPetals(15000);
            skillRefundTo(tree, SkillId::RadiationReach, -1);
            skillRefundTo(tree, SkillId::RadiationPower, -1);
        }
        for (Entity previous : Query<GroundEffect>{rig.world}.collect()) rig.world.destroy(previous);
        rig.tickPetals(60000);
        Result result;
        const auto clouds = Query<GroundEffect, Transform>{rig.world}.collect();
        CHECK_EQ(clouds.size(), std::size_t(1));
        if (clouds.size() != 1) return result;
        const auto& field = rig.world.get<GroundEffect>(clouds.front());
        const Vec2 at = rig.world.get<Transform>(clouds.front()).position;
        result.radius = field.radius;
        const auto kind = rig.registry.petalIndex("uranium");
        const double base = baseRadiationRadius(rig.registry.petal(kind), Rarity::Apex) * 1.25;
        const auto inner = rig.mob(at + Vec2{base * .5, 0});
        const auto outer = rig.mob(at + Vec2{base * 1.125, 0});
        for (Entity target : {inner, outer}) rig.world.get<Health>(target) = {1e9, 1e9};
        rig.combatTick(60000);
        rig.combatTick(60050);
        result.innerDamage = 1e9 - rig.world.get<Health>(inner).current;
        result.outerDamage = 1e9 - rig.world.get<Health>(outer).current;
        if (const auto* aff = rig.world.tryGet<Afflictions>(rig.player))
            result.ownerPoison = aff->poisonPerSecond;
        return result;
    };
    const auto plain = run(-1, -1, false);
    const auto reach = run(4, -1, false);
    const auto power = run(-1, 2, false);
    const auto combined = run(4, 2, false);
    const auto refunded = run(4, 2, true);
    CHECK(plain.innerDamage > 0.0);
    CHECK_EQ(plain.outerDamage, 0.0);
    CHECK_EQ(power.outerDamage, 0.0);
    CHECK_NEAR(reach.radius, plain.radius * 1.25, 1e-6);
    CHECK_NEAR(reach.innerDamage, plain.innerDamage, 1e-6);
    CHECK_NEAR(reach.outerDamage, plain.innerDamage, 1e-6);
    CHECK_NEAR(power.innerDamage, plain.innerDamage * 1.15, 1e-5);
    CHECK_NEAR(combined.outerDamage, power.innerDamage, 1e-5);
    CHECK(plain.ownerPoison > 0.0);
    CHECK_NEAR(combined.ownerPoison, plain.ownerPoison, 1e-6);
    CHECK_NEAR(refunded.radius, plain.radius, 1e-6);
    CHECK_NEAR(refunded.innerDamage, plain.innerDamage, 1e-6);
    CHECK_EQ(refunded.outerDamage, 0.0);
    std::printf("RADIATION actual inner %.6f -> %.6f, outer %.6f -> %.6f, radius %.6f -> %.6f, self %.6f -> %.6f, refund %.6f\n",
        plain.innerDamage, power.innerDamage, plain.outerDamage, combined.outerDamage, plain.radius,
        combined.radius, plain.ownerPoison, combined.ownerPoison, refunded.innerDamage);
}

TEST(imported_talents_fire_applies_to_real_burn_without_changing_self_or_direct_hits) {
    const auto fire = [](bool bought, bool gated, bool refund) {
        ArcticBlast rig("void_dvd");
        auto& skills = rig.world.add<PlayerSkillTree>(rig.player).skills;
        skills.set(SkillId::Damage, gated ? 4 : 3);
        if (bought) skills.set(SkillId::FirePower, 1);
        if (refund) skillRefundTo(skills, SkillId::FirePower, -1);
        rig.tickPetals(8000);
        const auto pets = rig.liveIn(0);
        CHECK(!pets.empty());
        if (pets.empty()) return std::array<double, 3>{};
        const Entity petal = pets.front();
        const Entity target = rig.mob(rig.world.get<Transform>(petal).position);
        rig.combatTick(8000);
        const auto* status = rig.world.tryGet<Afflictions>(target);
        CHECK(status && !status->burns.empty());
        const double hp = rig.world.get<Health>(target).current;
        rig.world.get<Transform>(target).position = {5700, 5700};
        rig.combatTick(8050);
        const double burn = hp - rig.world.get<Health>(target).current;
        const Entity selfEmitter = rig.world.create();
        CombatSystem::ignite(rig.world, rig.player, selfEmitter, rig.player, 10.0, 1000.0, 8100);
        const auto& selfBurn = rig.world.get<Afflictions>(rig.player).burns.front();
        return std::array<double, 3>{1000.0 - hp, burn, selfBurn.perSecond};
    };
    const auto base = fire(false, true, false);
    const auto upgraded = fire(true, true, false);
    const auto refunded = fire(true, true, true);
    const auto locked = fire(true, false, false);
    const auto lockedBase = fire(false, false, false);
    CHECK(base[1] > 0);
    CHECK_NEAR(upgraded[0], base[0], 1e-6);
    CHECK_NEAR(upgraded[1], base[1] * 1.15, 1e-6);
    CHECK_NEAR(upgraded[2], base[2], 1e-6);
    CHECK_NEAR(refunded[1], base[1], 1e-6);
    CHECK_NEAR(locked[1], lockedBase[1], 1e-6);
    std::printf("FIRE actual tick %.6f -> %.6f self %.6f -> %.6f\n",
                base[1], upgraded[1], base[2], upgraded[2]);
}

TEST(imported_talents_corrupt_only_while_actually_corrupted) {
    const auto sample = [](bool corrupted, bool aura, bool buys) {
        ArcticBlast rig("cactus");
        rig.world.add<PlayerVisuals>(rig.player).corrupted = corrupted;
        auto& skills = rig.world.add<PlayerSkillTree>(rig.player).skills;
        if (aura) rig.equip(1, "corruption", Rarity::Ultra);
        if (buys) {
            skills.set(SkillId::CorruptDamage, 6);
            skills.set(SkillId::CorruptReload, 2);
            skills.set(SkillId::CorruptReach, 2);
        }
        rig.world.get<PlayerInput>(rig.player).current.flags = net::InputAttack;
        rig.tickPetals(6000);
        CHECK_EQ(rig.world.get<PlayerVisuals>(rig.player).equippedCorruption, aura);
        const auto petals = rig.liveIn(0);
        CHECK(!petals.empty());
        if (petals.empty()) return std::array<double, 3>{};
        const auto petal = petals.front();
        const double reach = rig.world.get<PetalRing>(rig.player).radius;
        const auto target = rig.mob(rig.world.get<Transform>(petal).position);
        rig.combatTick(6000);
        const double damage = 1000.0 - rig.world.get<Health>(target).current;
        if (rig.world.isAlive(petal)) rig.world.ensure<Dead>(petal);
        rig.tickPetals(6050);
        const auto& slot = rig.world.get<Loadout>(rig.player).slots[0];
        return std::array<double, 3>{damage, reach, slot.reloadReadyAtMillis - 6050.0};
    };
    const auto baseline = sample(false, false, false);
    const auto plain = sample(false, false, true);
    const auto aura = sample(false, true, true);
    const auto corrupt = sample(true, false, true);
    CHECK(baseline[0] > 0.0);
    CHECK(baseline[2] > 0.0);
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        CHECK_NEAR(plain[i], baseline[i], 1e-6);
        CHECK_NEAR(aura[i], baseline[i], 1e-6);
    }
    CHECK_NEAR(corrupt[0], baseline[0] * 1.175, 1e-6);
    CHECK_NEAR(corrupt[1], baseline[1] + 60.0, 1e-6);
    CHECK_NEAR(corrupt[2], baseline[2] / 1.12, 1e-6);
    std::printf("CORRUPT hit %.6f -> %.6f, radius %.6f -> %.6f, reload %.6f -> %.6f\n",
        baseline[0], corrupt[0], baseline[1], corrupt[1], baseline[2], corrupt[2]);
}

TEST(imported_talents_poison_boosts_direct_acid_and_oil_ticks) {
    const auto hit = [](SpillKind kind, bool learned) {
        ArcticBlast rig("basic");
        if (learned) rig.world.add<PlayerSkillTree>(rig.player).skills.set(SkillId::Poison, 8);
        const Entity target = rig.mob({5400,5400});
        SpillSpec spec;
        spec.kind = kind;
        spec.radius = 70.0;
        spec.strength = 16.0;
        spec.durationMillis = 2100.0;
        CombatSystem::createSpill(rig.world, rig.player, {5400,5400}, Rarity::Common, spec, 1.0, 4500);
        rig.combatTick(4550);
        return 1000.0 - rig.world.get<Health>(target).current;
    };
    for (auto kind : {SpillKind::Acid, SpillKind::Oil}) {
        const double base = hit(kind, false);
        const double upgraded = hit(kind, true);
        CHECK(base > 0.0);
        CHECK_NEAR(upgraded, base * 1.5625, 1e-7);
        std::printf("FLUID_POISON %d %.6f -> %.6f\n", static_cast<int>(kind), base, upgraded);
    }
}

TEST(apex_magic_blood_stinger_forms_only_the_spikes_its_mana_can_pay_for) {
    ArcticBlast rig("basic");
    rig.equip(0, "magic_blood_stinger", Rarity::Ascendant);
    rig.equip(1, "mana_orb", Rarity::Ascendant);
    const auto step = [&](double now) {
        rig.petals.run(rig.world, rig.registry, now, 0.0, rig.commands);
        rig.commands.flush();
    };
    step(4050.0);
    const auto mana = [&]() -> ManaPool& { return rig.world.get<ManaPool>(rig.player); };
    const auto hp = [&] { return rig.world.get<Health>(rig.player).current; };
    mana().current = 2047.0;
    step(15000.0);
    CHECK_EQ(rig.liveIn(0).size(), 1u);
    CHECK_NEAR(mana().current, 1023.0, 1e-8);
    const double before = hp();
    mana().current = 6144.0;
    step(15050.0);
    CHECK_EQ(rig.liveIn(0).size(), 7u);
    CHECK_NEAR(mana().current, 0.0, 1e-8);
    CHECK_NEAR(before - hp(), rig.world.get<Health>(rig.player).max * .025, 1e-8);
}

TEST(void_powder_speed_curve_and_multiple_copies_are_additive) {
    for (int tier = 0; tier < kLadderRarityCount; ++tier) {
        ArcticBlast rig("void_powder", static_cast<Rarity>(tier));
        rig.tickPetals(5000);
        const double expected = 1.04 + .015 * tier;
        CHECK_NEAR(rig.world.get<PlayerModifiers>(rig.player).speedScale, expected, 1e-8);
    }

    ArcticBlast rig("void_powder", Rarity::Ascendant);
    rig.equip(1, "void_powder", Rarity::Ascendant);
    rig.tickPetals(5000);
    CHECK_NEAR(rig.world.get<PlayerModifiers>(rig.player).speedScale, 1.35, 1e-8);
    Terrain terrain;
    MovementSystem movement;
    auto& input = rig.world.get<PlayerInput>(rig.player).current;
    input.moveAngle = 0;
    input.moveStrength = 1.0;
    for (int tick = 0; tick < 100; ++tick)
        movement.runPlayerPhase(rig.world, terrain, 5100 + tick * 50, .05);
    CHECK_NEAR(rig.world.get<Motion>(rig.player).velocity.length(), kPlayerMaxSpeed * 1.35, 1.0);
}

TEST(radiation_specialists_match_sustained_output_and_corium_splits_its_budget) {
    for (const auto tier : {Rarity::Common, Rarity::Unique, Rarity::Apex, Rarity::Ascendant}) {
        double purePoison = 0.0, pureFire = 0.0;
        for (const char* id : {"uranium", "arctic_plutonium", "void_corium", "arctic_corium"}) {
            ArcticBlast rig(id, tier);
            const auto target = rig.mob({5000, 5000});
            rig.world.get<Health>(target) = {1e13, 1e13};
            double before = 0.0;
            for (int tick = 0; tick <= 400; ++tick) {
                const double now = 300000 + tick * 50;
                rig.tickPetals(now);
                rig.combatTick(now);
                if (tick == 200) before = rig.world.get<Health>(target).current;
            }
            const double dps = (before - rig.world.get<Health>(target).current) / 10.0;
            const std::string_view name(id);
            if (name == "uranium") purePoison = dps;
            else if (name == "arctic_plutonium") {
                pureFire = dps;
                CHECK(pureFire > 0.0);
                CHECK_NEAR(purePoison, pureFire, std::max(0.5, pureFire * 0.01));
            } else {
                CHECK(dps > pureFire);
                CHECK(dps < pureFire * 1.25);
                const auto* poison = rig.world.tryGet<Afflictions>(target);
                const auto* burn = rig.world.tryGet<Burn>(target);
                CHECK(poison && poison->poisonPerSecond > 0.0 && poison->poisonPerSecond < purePoison);
                CHECK(burn && burn->damage * (2.0 / 3.0) < pureFire);
            }
            std::printf("%s %s sustained DPS %.3f\n", rarityName(tier), id, dps);
        }
    }
}

TEST(imported_mana_recovery_supplies_capacity_and_apex_outpaces_permanent_hyper) {
    for (const char* id : {"magic_dahlia", "magic_arctic_banana", "magic_arctic_ice_rose",
                           "magic_fries", "magic_sunflower", "magic_starfish",
                           "magic_lily_pad", "magic_vampire_fang"}) {
        ArcticBlast permanent(id, Rarity::PermanentHyper);
        ArcticBlast apex(id, Rarity::Ascendant);
        const auto getPool = [](ArcticBlast& rig) -> ManaPool& { return rig.world.get<ManaPool>(rig.player); };
        const double permaCap = getPool(permanent).max - kFlowerBaseMana;
        const double apexCap = getPool(apex).max - kFlowerBaseMana;
        CHECK(permaCap > 0.0);
        CHECK_NEAR(apexCap, permaCap * 1.40, 1e-6);
        const auto index = apex.registry.petalIndex(id);
        const auto basic = permanent.registry.petalStats(index, Rarity::PermanentHyper);
        const auto higher = apex.registry.petalStats(index, Rarity::Ascendant);
        if (basic.mana > 0) CHECK_NEAR(higher.mana, basic.mana * 1.40, 1e-7);
        if (std::string_view(id) == "magic_dahlia") {
            auto firstBurst = [&](ArcticBlast& rig) {
                getPool(rig).current = 0.0;
                for (int tick = 1; tick <= 160 && getPool(rig).current == 0.0; ++tick)
                    rig.tickPetals(4000 + tick * 50);
                return getPool(rig).current;
            };
            const double earlier = firstBurst(permanent);
            const double later = firstBurst(apex);
            CHECK(earlier > 0.0);
            CHECK_NEAR(later, earlier * 1.40, 1e-6);
            std::printf("Dahlia mana capacity %.2f -> %.2f, first delivery %.2f -> %.2f\n",
                        permaCap, apexCap, earlier, later);
        }
        getPool(apex).current = getPool(apex).max;
        apex.equip(0, "basic");
        apex.tickPetals(14000);
        CHECK_NEAR(getPool(apex).max, kFlowerBaseMana, 1e-8);
        CHECK(getPool(apex).current <= getPool(apex).max);
    }
}
