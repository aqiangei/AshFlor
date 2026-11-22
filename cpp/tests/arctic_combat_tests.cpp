#include "test.h"
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
#include "server/workers.h"
#include "server/squads.h"
#include "server/replication.h"
#include "client/render/world_renderer.h"
#include "client/render/sprites.h"

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

    Vec2 armBomb() {
        const auto& ring = world.get<Loadout>(player).spawned;
        CHECK(!ring.empty());
        if (ring.empty()) return {};
        const auto petal = ring.front();
        const auto position = world.get<Transform>(petal).position;
        world.get<Health>(petal).current = 0;
        world.add<Dead>(petal);
        tickPetals(4050);
        return position;
    }
};
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
        const bool elemental = id == "void_elemental";
        const auto packSize = elemental ? 3u : 1u;
        if (first.size() < packSize || second.size() < packSize) { CHECK(false); continue; }
        for (std::size_t i = 0; i < packSize; ++i) {
            rig.world.get<Transform>(first[i]).position = at;
            rig.world.get<Transform>(second[i]).position = at;
        }
        rig.combatTick(25050);
        const auto* status = rig.world.tryGet<Afflictions>(dummy);
        CHECK(status);
        if (!status) continue;
        CHECK_EQ(status->poisonStacks.size(), poison ? 2u * packSize : 0u);
        CHECK_EQ(status->burns.size(), burn ? 2u * packSize : 0u);

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
        {"arctic_centipede_egg", "arctic_centipede", 1, {0, 1, 2, 2, 3, 4, 5, 6, 7, 8}, {5000, 10000, 15000, 2000, 3000, 4000, 5000, 6000, 15000, 25000}},
        {"arctic_spider_cave_egg", "arctic_spider", 10, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {17500, 20000, 23000, 26000, 30000, 5000, 7000, 11000, 16000, 75000}},
        {"arctic_spider_egg", "arctic_spider", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {8000, 10000, 12000, 14000, 17500, 3000, 5000, 7500, 12500, 40000}},
        {"arctic_frost_digger_egg", "arctic_frost_digger", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {6000, 7000, 8000, 9000, 10000, 3000, 6000, 9000, 12000, 46250}},
        {"arctic_ice_cube_egg", "arctic_ice_cube", 1, {0, 1, 2, 3, 4, 5, 5, 6, 7, 8}, {3000, 4500, 6000, 8000, 10000, 14000, 4000, 7500, 12500, 35000}},
        {"arctic_ice_dragon_egg", "arctic_ice_dragon", 1, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, {7000, 9000, 11000, 13000, 3000, 4500, 6000, 8500, 15000, 45000}},
        {"arctic_ice_hornet_egg", "arctic_ice_hornet", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {6000, 7000, 9000, 11000, 14000, 3000, 5500, 10000, 12500, 40000}},
        {"arctic_igloo_egg", "arctic_snowman", 10, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {10000, 15000, 20000, 25000, 30000, 10000, 12000, 14000, 30000, 95000}},
        {"arctic_nuke_egg", "arctic_nuke", 2, {0, 1, 2, 3, 4, 5, 5, 6, 7, 8}, {3800, 4500, 5300, 6000, 6800, 7500, 3400, 6000, 9000, 19450}},
        {"arctic_snowbush_egg", "arctic_snowbush", 1, {0, 1, 2, 3, 4, 5, 5, 6, 7, 8}, {5000, 6000, 7000, 8000, 9000, 10000, 2000, 5000, 11250, 25000}},
        {"arctic_snowman_egg", "arctic_snowman", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {2000, 3000, 5000, 8000, 13000, 2000, 4000, 8000, 16000, 40000}},
        {"arctic_spider_digger_egg", "arctic_spider_digger", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {4000, 4500, 5000, 5500, 6500, 3000, 5000, 7000, 13000, 37500}},
        {"arctic_spider_yoba_egg", "arctic_spider_yoba", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {3000, 4000, 5500, 7000, 8500, 3500, 5500, 7500, 15000, 40000}},
        {"arctic_stalagmite_egg", "arctic_stalagmite", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {1750, 2250, 3000, 4000, 6000, 3000, 3500, 4000, 7000, 27500}},
        {"arctic_tick_egg", "arctic_tick", 1, {0, 1, 2, 3, 4, 4, 5, 6, 7, 8}, {5000, 7000, 10000, 13000, 15000, 1500, 2500, 5000, 12500, 40000}},
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
            CHECK_EQ(countHatchlings(), egg.count);
            rig.tickPetals(hatchAt + egg.hatchMillis[tier]);
            CHECK_EQ(countHatchlings(), egg.count);
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
        CHECK(std::any_of(lines.begin(), lines.end(),
            [](const auto& line) { return line.text == "Hatch delay: 13.4s"; }));
        const auto livingPets = [&] { return Query<Pet>{rig.world}.without<Dead>().collect(); };
        const double firstHatch = shellReadyAt + incubation * scale;
        rig.tickPetals(firstHatch - 1.0);
        CHECK(livingPets().empty());
        rig.tickPetals(firstHatch);
        const auto pets = livingPets();
        CHECK_EQ(pets.size(), std::size_t(1));
        if (pets.empty()) continue;
        rig.world.add<Dead>(pets.front());
        const double nextHatch = firstHatch + incubation * scale;
        rig.tickPetals(nextHatch - 1.0);
        CHECK(livingPets().empty());
        rig.tickPetals(nextHatch);
        CHECK_EQ(livingPets().size(), std::size_t(1));
    }
}

TEST(zorr_soap_expands_liquid_fields_without_scaling_damage) {
    ArcticBlast rig("basic");
    const auto sample = [&](double now, double expectedRadius) {
        rig.tickPetals(now);
        SpillSpec acid;
        acid.kind = SpillKind::Acid;
        acid.radius = 50.0;
        acid.strength = 14.0;
        acid.durationMillis = 2100.0;
        CombatSystem::createSpill(rig.world, rig.player, {5000, 5000}, Rarity::Common, acid, 1.0, 0);
        int acidFields = 0;
        Query<GroundEffect>{rig.world}.each([&](Entity, GroundEffect& field) {
            if (field.kind != GroundEffectKind::AcidSpill) return;
            ++acidFields;
            CHECK_NEAR(field.radius, expectedRadius * kFluidRadiusScale, 1e-6);
            CHECK_NEAR(field.damagePerSecond, 14.0 * kFluidDamageScale, 1e-6);
        });
        CHECK(acidFields == 1);
        std::vector<Entity> fields;
        Query<GroundEffect>{rig.world}.collect(fields);
        for (const Entity field : fields) rig.world.destroy(field);
    };
    sample(4050, 50.0);
    rig.equip(1, "void_soap");
    sample(4100, 60.0);
    rig.equip(2, "void_soap", Rarity::Rare);
    sample(4150, 76.0);
    rig.equip(3, "void_soap", Rarity::Super);
    sample(4200, 100.0);
    auto& slots = rig.world.get<Loadout>(rig.player).slots;
    slots[1] = slots[2] = slots[3] = {};
    sample(4250, 50.0);
    rig.equip(1, "void_soap", Rarity::Super);
    sample(4300, 81.0);
    rig.equip(1, "void_soap", Rarity::Unique);
    sample(4350, 84.0);
    rig.equip(1, "void_soap", Rarity::Apex);
    sample(4400, 87.0);
    rig.equip(1, "void_soap", Rarity::PermanentHyper);
    sample(4450, 87.0);
    rig.equip(1, "void_soap", Rarity::Ascendant);
    sample(4500, 90.0);
    auto& skills = rig.world.add<PlayerSkillTree>(rig.player).skills;
    skills.set(SkillId::FluidReach, 0);
    sample(4550, 99.0);
    skills.set(SkillId::FluidReach, 1);
    sample(4600, 108.0);
    skills.set(SkillId::FluidReach, 2);
    sample(4650, 117.0);
    skills.set(SkillId::FluidReach, -1);
    sample(4700, 90.0);
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
        const auto expectedTier = apexEgg ? Rarity::Apex : Rarity::Unique;
        const double hatchAt = 4000.0 + rig.registry.petalStats(eggIndex, eggRarity).reloadMillis;
        const auto& spider = rig.registry.mob(egg.petMobIndex);
        const double baselineScale = petalStatScale(Rarity::Unique) * hyperSummonScale(egg, eggRarity)
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
        const char* expectedLabel = apexEgg ? "Summon rarity: Hyper" : "Summon rarity: Eternal";
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
    CHECK(nukes.size() == 2);
    if (nukes.size() != 2) return;

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

TEST(zorr_mechanic_fire_bomb_keeps_blast_and_burn_budget) {
    ArcticBlast rig("void_fire_bomb");
    const Vec2 at = rig.armBomb();
    const auto near = rig.mob(at + Vec2{30,0});
    const auto far = rig.mob(at + Vec2{200,0});
    rig.combatTick(4050);
    CHECK_NEAR(rig.world.get<Health>(near).current, 988, 1e-6);
    const auto* status = rig.world.tryGet<Afflictions>(near);
    CHECK(status);
    if (!status) return;
    CHECK_NEAR(status->burnPerSecond, 6, 1e-6);
    CHECK_NEAR(rig.world.get<Health>(far).current, 1000, 1e-6);
    CHECK(!rig.world.has<Afflictions>(far));
}

TEST(zorr_mechanic_soap_widens_a_single_lava_pool) {
    for(bool soap : {false,true}) {
        ArcticBlast rig("basic");
        if(soap) rig.equip(1,"void_soap",Rarity::Super);
        rig.petals.foldModifiers(rig.world,rig.registry);
        const auto& lava=rig.registry.petal(rig.registry.petalIndex("lava")).spill;
        CombatSystem::createSpill(rig.world,rig.player,{5400,5400},Rarity::Common,lava,1,0);
        const Entity edge=rig.mob({5480,5400});
        const Entity outside=rig.mob({5520,5400});
        rig.combatTick(5000);
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
        int patches = 0;
        Query<GroundEffect>{rig.world}.each([&](Entity, GroundEffect& field) {
            if (field.kind != GroundEffectKind::Blood) return;
            ++patches;
            CHECK_NEAR(field.radius, (soap ? 44.16 : 36.8) * 1.20 * (talent ? 1.30 : 1.0), 1e-6);
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
    const auto near = rig.mob(center + Vec2{58, 0});
    const auto far = rig.mob(center + Vec2{80, 0});
    rig.combatTick(4050);
    CHECK(rig.world.get<Health>(near).current < 1000.0);
    CHECK_EQ(rig.world.get<Health>(far).current, 1000.0);
}

TEST(zorr_mechanic_soap_applies_to_all_configured_liquid_fields) {
    ArcticBlast rig("basic");
    for (const char* id : {"acid", "lava", "oil", "hot_water", "elixir", "acid_bomb", "lava_bomb"}) {
        const auto& spill = rig.registry.petal(rig.registry.petalIndex(id)).spill;
        CHECK(spill.kind != SpillKind::None);
        CHECK(spill.radius > 0.0);
        for (bool soap : {false, true}) {
            if (soap) rig.equip(1, "void_soap");
            else rig.world.get<Loadout>(rig.player).slots[1] = {};
            rig.petals.foldModifiers(rig.world, rig.registry);
            CombatSystem::createSpill(rig.world, rig.player, {5000,5000}, Rarity::Common, spill, 1.0, 0);
            std::vector<Entity> fields;
            Query<GroundEffect>{rig.world}.collect(fields);
            CHECK(fields.size() == 1);
            for (const auto field : fields) {
                const auto& effect = rig.world.get<GroundEffect>(field);
                CHECK_NEAR(effect.radius, spill.radius * 1.20 * (soap ? 1.2 : 1.0), 1e-6);
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
        CHECK_NEAR(stats.lightningDamagePerHit,r==Rarity::Super?262440:r==Rarity::Unique?372009:744018,1e-6);
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

TEST(zorr_reference_gunpowder_fivefold_contact_damage_requires_live_burn) {
    double plain=0;
    for (int burn : {0,1,2}) {
        ArcticBlast rig("void_gunpowder");
        const auto petals = rig.liveIn(0);
        CHECK(!petals.empty());
        if (petals.empty()) continue;
        const auto victim=rig.mob(rig.world.get<Transform>(petals.front()).position);
        if (burn) rig.world.ensure<Afflictions>(victim).burns.push_back(
            {rig.player,rig.player,0.001,burn==1 ? 9000.0 : 3999.0});
        rig.combatTick(4050);
        const double hit=1000.0-rig.world.get<Health>(victim).current;
        if (!burn) plain=hit;
        CHECK(plain>0);
        CHECK_NEAR(hit,plain*(burn==1 ? 5 : 1),0.002);
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
    const auto radius = rig.world.get<GroundEffect>(field).radius;
    CHECK_NEAR(radius, 57.5 * 1.62 * 1.30 * 1.20, 1e-6);
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
    CHECK_NEAR(rig.world.get<GroundEffect>(zone).radius, 92.5 * 2.0 * 1.30 * 1.20, 1e-6);
    const Vec2 at = rig.world.get<Transform>(zone).position;
    const auto near = rig.mob(at + Vec2{0, 200});
    const auto far = rig.mob(at + Vec2{0, 350});
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
        CHECK_NEAR(1000.0 - rig.world.get<Health>(cold).current, oil.strength * 1.25 * net::kTickSeconds, 1e-6);
        CHECK_NEAR(1000.0 - rig.world.get<Health>(burning).current,
                   (2.0 * oil.strength * 1.25 + 5.0) * net::kTickSeconds, 1e-6);
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

TEST(passive_mana_petals_supply_and_refill_a_pool_on_their_own) {
    for (const auto* id : {"void_elemental", "magic_basil", "magic_palm_leaf", "magic_soil"}) {
        ArcticBlast rig(id);
        const auto* pool = rig.world.tryGet<ManaPool>(rig.player);
        CHECK(pool && pool->max > 0);
        if (!pool) continue;
        const auto index = rig.registry.petalIndex(id);
        const auto stats = rig.registry.petalStats(index, Rarity::Common);
        rig.world.get<ManaPool>(rig.player).current = 0;
        rig.tickPetals(4050);
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current,
                   stats.passiveManaPerSecond * net::kTickSeconds, 1e-6);
        rig.world.get<Loadout>(rig.player).slots[0] = {};
        rig.tickPetals(4100);
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).max, 0, 1e-6);
        CHECK_NEAR(rig.world.get<ManaPool>(rig.player).current, 0, 1e-6);
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
    for (const std::size_t hits : {1u, 3u}) {
        ArcticBlast rig("void_elemental");
        const auto petals = rig.liveIn(0);
        CHECK_EQ(petals.size(), 3u);
        if (petals.size() != 3u) return;
        const Vec2 at{5300, 5300};
        const Entity victim = rig.mob(at);
        for (std::size_t i = 0; i < hits; ++i) rig.world.get<Transform>(petals[i]).position = at;
        rig.combatTick(4050);
        const auto* effects = rig.world.tryGet<Afflictions>(victim);
        CHECK(effects);
        if (!effects) return;
        CHECK_EQ(effects->poisonStacks.size(), hits);
        CHECK_EQ(effects->burns.size(), hits);
        CHECK_NEAR(effects->poisonPerSecond, 10.0 * hits / 3.0, 1e-6);
        double burnRate = 0.0;
        for (const auto& burn : effects->burns) burnRate += burn.perSecond;
        CHECK_NEAR(burnRate, 7.0 * hits / 3.0, 1e-6);
        CHECK_NEAR(1000.0 - rig.world.get<Health>(victim).current, 47.0 * hits / 3.0, 1e-6);
    }
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
            CHECK_NEAR(effect.burnDamage,45,1e-6);
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
    int fields=0;
    Query<GroundEffect>{rig.world}.each([&](Entity,GroundEffect& field){
        ++fields;
        CHECK_NEAR(field.radius,60,1e-6);
        bool offensive=field.kind==GroundEffectKind::AcidSpill ||
            field.kind==GroundEffectKind::LavaSpill || field.kind==GroundEffectKind::OilSpill;
        CHECK_NEAR(field.damagePerSecond,offensive?12.5:10,1e-6);
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
            const double expectedDamage=1.25*(uranium?uraniumRadiationDps(stats,rarity)*scale:previousPulse)*offenseRatio;
            const double step=config.id=="pyrite"?140:config.id=="arctic_plutonium"?200:(config.id=="arctic_corium" || config.id=="void_corium")?150:0;
            const double oldRadius=spec.radius+(config.id=="void_stardust"?12.5*ladderIndex(rarity):0)+step*std::max(0,ladderIndex(rarity)-7);
            int fields=0;
            Query<GroundEffect>{rig.world}.each([&](Entity,GroundEffect& effect){
                ++fields;
                CHECK_NEAR(effect.radius,oldRadius*1.20*(uranium && rarity==Rarity::Apex?1.10:1.0),1e-5);
                CHECK_NEAR(effect.damagePerSecond,expectedDamage,std::max(1e-6,expectedDamage*1e-12));
                CHECK_NEAR(effect.damagePerHit,spec.pulseDamage*strength*scale*1.25*offenseRatio,std::max(1e-6,effect.damagePerHit*1e-12));
                CHECK_NEAR(effect.burnDamage,spec.burnDamage*strength*scale*1.25*offenseRatio,std::max(1e-6,effect.burnDamage*1e-12));
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
TEST(reference_vampire_fang_heals_landed_body_damage_and_respects_no_heal) {
 ArcticBlast r("vampire_fang");r.world.get<Health>(r.player).current=50;
 auto enemy=r.mob({5500,5000});r.combat.applyDamage(r.world,r.player,enemy,10,5000);
 CHECK_NEAR(r.world.get<Health>(r.player).current,45.4,1e-6);
 r.combat.applyNoHeal(r.world,r.player,1000,6000);
 r.combat.applyDamage(r.world,r.player,enemy,10,6000);
 CHECK_NEAR(r.world.get<Health>(r.player).current,35.4,1e-6);
}
TEST(toilet_paper_requires_an_active_loadout_slot_with_sponge) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const bool storedSponge : {false, true}) {
        ArcticBlast rig("toilet_paper");
        if (storedSponge) rig.equip(kLoadoutActiveSlots, "sponge");
        rig.tickPetals(4500);
        const auto paper = rig.liveIn(0);
        CHECK_EQ(paper.size(), 1u);
        if (paper.empty()) return;
        const auto enemy = rig.mob({5500, 5000});
        const double maxHealth = rig.world.get<Health>(rig.player).max;
        rig.combat.applyDamage(rig.world, rig.player, enemy, maxHealth * .95, 5000);
        const auto& status = rig.world.get<Afflictions>(rig.player);
        CHECK(!status.toiletArmed);
        CHECK(!status.toiletBoostActive);
        CHECK_EQ(status.toiletBoostUntilMillis, 0.0);
        CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, 5050).applied, 6.0, 1e-6);
    }
}

TEST(toilet_paper_boosts_after_sponge_repayment_and_expires) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("toilet_paper", Rarity::Common, "sponge");
    const auto paper = rig.liveIn(0);
    CHECK_EQ(paper.size(), 1u);
    if (paper.empty()) return;
    const auto enemy = rig.mob({5500, 5000});
    const double maxHealth = rig.world.get<Health>(rig.player).max;
    CHECK(rig.world.get<Afflictions>(rig.player).toiletArmed);
    rig.combat.applyDamage(rig.world, rig.player, enemy, maxHealth * .95, 5000);
    CHECK(rig.world.has<SpongeDamageState>(rig.player));
    CHECK_NEAR(rig.world.get<Health>(rig.player).current, maxHealth, 1e-6);
    for (int time = 5050; time <= 7000; time += 50) rig.combatTick(time);
    const auto& status = rig.world.get<Afflictions>(rig.player);
    const double deadline = status.toiletBoostUntilMillis;
    CHECK(status.toiletBoostActive);
    CHECK_NEAR(rig.world.get<Health>(rig.player).current, maxHealth * .05, 1e-6);
    CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, deadline - 1).applied, 18.0, 1e-6);
    CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, deadline).applied, 6.0, 1e-6);
}

TEST(toilet_paper_cancels_on_sponge_removal_and_keeps_the_original_cooldown) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ArcticBlast rig("toilet_paper", Rarity::Common, "sponge");
    const auto paper = rig.liveIn(0);
    CHECK_EQ(paper.size(), 1u);
    if (paper.empty()) return;
    const auto enemy = rig.mob({5500, 5000});
    const double maxHealth = rig.world.get<Health>(rig.player).max;
    rig.combat.applyDamage(rig.world, rig.player, enemy, maxHealth * .95, 5000, DamageKind::Sponge);
    CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, 5050).applied, 18.0, 1e-6);

    rig.world.get<Loadout>(rig.player).slots[1] = {};
    // An already spawned paper must stop getting the boost before the next fold.
    CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, 5100).applied, 6.0, 1e-6);
    rig.tickPetals(5150);
    CHECK(!rig.world.get<Afflictions>(rig.player).toiletBoostActive);
    CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 14000.0);

    rig.equip(1, "sponge");
    rig.world.get<Health>(rig.player).current = maxHealth;
    rig.tickPetals(5200);
    rig.combat.applyDamage(rig.world, rig.player, enemy, maxHealth * .95, 5300, DamageKind::Sponge);
    CHECK(!rig.world.get<Afflictions>(rig.player).toiletBoostActive);
    CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, 5350).applied, 6.0, 1e-6);

    rig.world.get<Health>(rig.player).current = maxHealth;
    rig.tickPetals(14000);
    rig.combat.applyDamage(rig.world, rig.player, enemy, maxHealth * .95, 14100, DamageKind::Sponge);
    CHECK(rig.world.get<Afflictions>(rig.player).toiletBoostActive);
    CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, 14150).applied, 18.0, 1e-6);
}

TEST(toilet_paper_with_sponge_still_excludes_second_chance_and_bandage) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    for (const bool secondChance : {false, true}) {
        ArcticBlast rig("toilet_paper", Rarity::PermanentHyper, "sponge");
        if (secondChance) {
            rig.world.ensure<PlayerSkillTree>(rig.player).skills.set(SkillId::SecondChance, 0);
        } else {
            rig.equip(2, "bandage", Rarity::Ultra);
        }
        rig.tickPetals(4500);
        rig.tickPetals(7500);
        const auto paper = rig.liveIn(0);
        CHECK_EQ(paper.size(), 1u);
        if (paper.empty()) return;
        const auto enemy = rig.mob({5500, 5000});
        auto& health = rig.world.get<Health>(rig.player);
        health.max = health.current = 100.0;
        rig.combat.applyDamage(rig.world, rig.player, enemy, 95.0, 8000, DamageKind::Sponge);
        CHECK(rig.world.get<Afflictions>(rig.player).toiletBoostActive);
        CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 17000.0);
        rig.combat.applyDamage(rig.world, rig.player, enemy, 200.0, 8050, DamageKind::Sponge);
        CHECK_EQ(rig.world.get<Health>(rig.player).current, 1.0);
        CHECK_EQ(rig.world.has<SecondChance>(rig.player), secondChance);
        CHECK_EQ(rig.world.has<Undead>(rig.player), !secondChance);
        CHECK(!rig.world.get<Afflictions>(rig.player).toiletBoostActive);
        CHECK_EQ(rig.world.get<Afflictions>(rig.player).toiletBoostUntilMillis, 17000.0);
        CHECK_NEAR(rig.combat.applyDamage(rig.world, enemy, paper.front(), 6.0, 8100).applied, 6.0, 1e-6);
    }
}

TEST(reference_fractal_strikes_seven_unique_targets_without_duplicate_damage) {
 ArcticBlast r("fractal");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 auto at=r.world.get<Transform>(live.front()).position;std::vector<Entity> targets;
 for(int i=0;i<7;++i)targets.push_back(r.mob(at+Vec2{double(i*30),0}));
 r.tickPetals(4050);r.combatTick(4050);
 for(Entity mob:targets)CHECK_NEAR(r.world.get<Health>(mob).current,930,1e-6);
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
TEST(reference_latrine_creates_breakable_stacking_poison_and_soap_applies) {
 ArcticBlast r("basic");r.equip(1,"void_soap");r.petals.foldModifiers(r.world,r.registry);
 const auto& spec=r.registry.petal(r.registry.petalIndex("latrine")).spill;
 CombatSystem::createSpill(r.world,r.player,{5400,5400},Rarity::Common,spec,1,0);
 CombatSystem::createSpill(r.world,r.player,{5400,5400},Rarity::Common,spec,1,0);
 Entity mob=r.mob({5400,5400});r.combatTick(5000);
 CHECK(r.world.has<Afflictions>(mob));if(r.world.has<Afflictions>(mob)) {
  CHECK_EQ(r.world.get<Afflictions>(mob).poisonStacks.size(),2);
  CHECK_NEAR(r.world.get<Afflictions>(mob).poisonPerSecond,12.5,1e-6);
 }
 int fields=0;Query<GroundEffect,Health>{r.world}.each([&](Entity,GroundEffect& f,Health& hp){
  if(f.kind!=GroundEffectKind::LatrineSpill)return;++fields;CHECK_NEAR(hp.max,5,1e-6);CHECK_NEAR(f.radius,28.8,1e-6);
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
 ArcticBlast r("void_sad");auto live=r.liveIn(0);CHECK(!live.empty());if(live.empty())return;
 Entity target=r.mob({5500,5000});CHECK_NEAR(r.combat.applyDamage(r.world,target,live.front(),80,5000).applied,80,1e-6);
 Entity observer=referenceFlower(r,{5100,5000});CHECK_NEAR(r.combat.applyDamage(r.world,target,live.front(),80,5050).applied,40,1e-6);
 r.world.get<Transform>(observer).realm=Realm::Arena;CHECK_NEAR(r.combat.applyDamage(r.world,target,live.front(),80,5100).applied,80,1e-6);
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
  if(f.kind==GroundEffectKind::NitrogenSpill){pool=e;CHECK_NEAR(h.max,5,1e-6);CHECK_NEAR(f.damagePerSecond,0,1e-6);CHECK(f.radius>=24);}
 });CHECK(pool!=NULL_ENTITY);if(pool==NULL_ENTITY)return;
 Entity mob=r.mob(r.world.get<Transform>(pool).position);r.combatTick(5000);
 auto* status=r.world.tryGet<Afflictions>(mob);CHECK(status!=nullptr);if(status)CHECK_NEAR(status->frostSpeed(5000),.6,1e-6);
 CHECK_NEAR(r.world.get<Health>(mob).current,1000,1e-6);CHECK(!r.world.has<Afflictions>(r.player) || r.world.get<Afflictions>(r.player).frostSpeed(5000)==1);
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
 const auto& corium=c.petal(c.petalIndex("void_corium"));CHECK_NEAR(corium.radiation.burnDamage,112.5,1e-6);CHECK_NEAR(corium.radiation.pulseDamage,0,1e-6);
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
  const auto& v=c.petal(i);if(v.id=="arctic_stick")continue;
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
 CHECK_EQ(checked,218);ui::setLanguage(previous);
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
                emitted |= field.damagePerSecond > 0 || field.burnDamage > 0 || field.damagePerHit > 0;
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
        const bool diamond = name == "diamond";
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
                !diamond && tick % 180 < 120 ? net::InputAttack : net::InputDefend;
            if (magic) {
                if (auto* mana = rig.world.tryGet<ManaPool>(rig.player))
                    mana->current = mana->max;
            }
            if (diamond && tick % 100 == 1) {
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
    CHECK_EQ(checked, 60);
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


