#include "test.h"
#include "server_harness.h"
#include "server/systems/petals.h"
#include "server/systems/combat.h"
#include "shared/game/absorb.h"

using namespace flix;
using namespace flix::testsupport;

TEST(reference_new_talents_purchase_persist_and_refund) {
    Harness h("new-talents-1106", [](const std::string& path) {
        seedUser(path, "newtalentqa", "password7");
        Database db; std::string error; CHECK(db.load(path,error));
        db.progress(db.findUser("newtalentqa")->id).totalXp = 1e15;
        db.markDirty(); CHECK(db.save());
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient client;
    CHECK(loginAs(h,client,"newtalentqa","password7"));
    const int points=client.profile().talentPoints();
    for(auto id : {SkillId::Prestige,SkillId::PetDamage}) {
        client.requestUpgradeSkill(id,0);
        CHECK(awaitProfile(h,client,[=](const Profile& p){return p.skills.level(id)==0;}));
    }
    CHECK_EQ(client.profile().talentPoints(),points-227);
    client.disconnect(); h.step(10,{});
    NetClient again;
    CHECK(loginAs(h,again,"newtalentqa","password7"));
    CHECK(again.profile().skills.prestigeUnlocked());
    CHECK_EQ(again.profile().skills.level(SkillId::PetDamage),0);
    for(auto id : {SkillId::Prestige,SkillId::PetDamage}) {
        again.requestDowngradeSkill(id,0);
        CHECK(awaitProfile(h,again,[=](const Profile& p){return p.skills.level(id)==-1;}));
    }
    CHECK_EQ(again.profile().talentPoints(),points);
}

TEST(reference_talents_preserve_old_indices_and_price_every_tier) {
    CHECK_EQ(static_cast<int>(SkillId::Summoner), 16);
    CHECK_EQ(static_cast<int>(SkillId::Damage), 17);
    CHECK_EQ(static_cast<int>(SkillId::Absorbing), 18);
    SkillSet skills;
    CHECK_NEAR(skills.effectScale(SkillId::Damage), 1.0, 1e-12);
    CHECK_NEAR(absorbXpScale(skills), 1.0, 1e-12);
    for (const auto id : {SkillId::Damage, SkillId::Absorbing}) {
        CHECK_EQ(skillTierCount(id), 10);
        CHECK_EQ(skillPurchaseCost(skills, id, 9), 130);
        CHECK_EQ(skillFromKey(kSkillKeys[static_cast<int>(id)]), id);
        skills.set(id, 9);
    }
    CHECK_EQ(skills.spent(), 260);
    CHECK_EQ(availableTalentPoints(200, skills), 140);
    CHECK_NEAR(skills.statScale(SkillId::Damage), 1.9, 1e-12);
    CHECK_NEAR(skills.effectScale(SkillId::Damage), 2.4, 1e-12);
    CHECK_EQ(absorbXpTotal(50 * absorbUnits(Rarity::Rare), skills), 400.0);
    skillRefundTo(skills, SkillId::Damage, -1);
    CHECK_EQ(skills.spent(), 130);
    CHECK_NEAR(skills.effectScale(SkillId::Damage), 1.0, 1e-12);
    CHECK_NEAR(absorbXpScale(skills), 8.0, 1e-12);
}

TEST(reference_talents_purchase_refund_and_reconnect_through_real_server) {
    Harness h("reference-talents", [](const std::string& path) {
        seedUser(path, "talentqa", "password7");
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        auto& record = db.progress(db.findUser("talentqa")->id);
        record.totalXp = 1e15;
        record.skills.set(SkillId::Health, 2);
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient first;
    CHECK(loginAs(h, first, "talentqa", "password7"));
    const int before = first.profile().talentPoints();
    // A jump is refused even when the account has plenty of TP.
    first.requestUpgradeSkill(SkillId::Damage, 9);
    h.step(10, {&first});
    CHECK_EQ(first.profile().skills.level(SkillId::Damage), -1);
    for (const auto id : {SkillId::Damage, SkillId::Absorbing}) {
        for (int tier = 0; tier < 10; ++tier) {
            first.requestUpgradeSkill(id, tier);
            CHECK(awaitProfile(h, first, [=](const Profile& p) {
                return p.skills.level(id) == tier;
            }));
        }
    }
    CHECK_EQ(first.profile().talentPoints(), before - 260);
    first.disconnect();
    h.step(10, {});
    NetClient second;
    CHECK(loginAs(h, second, "talentqa", "password7"));
    CHECK_EQ(second.profile().skills.level(SkillId::Damage), 9);
    CHECK_EQ(second.profile().skills.level(SkillId::Absorbing), 9);
    CHECK_EQ(second.profile().skills.level(SkillId::Health), 2);
    second.requestDowngradeSkill(SkillId::Damage, 0);
    CHECK(awaitProfile(h, second, [](const Profile& p) {
        return p.skills.level(SkillId::Damage) == -1;
    }));
    CHECK_EQ(second.profile().talentPoints(), before - 130);
    second.requestDowngradeSkill(SkillId::Absorbing, 0);
    CHECK(awaitProfile(h, second, [](const Profile& p) {
        return p.skills.level(SkillId::Absorbing) == -1;
    }));
    CHECK_EQ(second.profile().talentPoints(), before);
    CHECK_EQ(second.profile().skills.level(SkillId::Health), 2);
}

TEST(reference_talents_survive_disk_reload_without_changing_old_branches) {
    HarnessFiles files("reference-talent-disk");
    seedUser(files.dbPath, "saveqa", "password7");
    std::string id, error;
    {
        Database db;
        CHECK(db.load(files.dbPath, error));
        id = db.findUser("saveqa")->id;
        auto& record = db.progress(id);
        CHECK_EQ(record.skills.level(SkillId::Damage), -1);
        CHECK_EQ(record.skills.level(SkillId::Absorbing), -1);
        record.skills.set(SkillId::Health, 3);
        record.skills.set(SkillId::SharpEdges, 5);
        record.skills.set(SkillId::Damage, 9);
        record.skills.set(SkillId::Absorbing, 8);
        record.mazeSkills.set(SkillId::Absorbing, 4);
        db.markDirty();
        CHECK(db.save());
    }
    Database reloaded;
    CHECK(reloaded.load(files.dbPath, error));
    const auto& record = reloaded.progress(id);
    CHECK_EQ(record.skills.level(SkillId::Health), 3);
    CHECK_EQ(record.skills.level(SkillId::SharpEdges), 5);
    CHECK_EQ(record.skills.level(SkillId::Damage), 9);
    CHECK_EQ(record.skills.level(SkillId::Absorbing), 8);
    CHECK_EQ(record.mazeSkills.level(SkillId::Absorbing), 4);
}

TEST(reference_damage_talent_updates_live_petals_and_body_then_refunds_cleanly) {
    std::string error;
    CHECK(loadContent(dataDir(), error));
    World world;
    CommandBuffer commands{world};
    PetalSystem petals;
    const auto player = world.create();
    world.add<PlayerTag>(player);
    world.add<Transform>(player, Transform{{10000, 10000}, 0});
    world.add<Motion>(player);
    world.add<Body>(player, Body{25, 1});
    world.add<Health>(player, Health{1000, 1000});
    world.add<Faction>(player, Faction{Team::Players, false});
    world.add<PlayerProgress>(player);
    world.add<PlayerInput>(player);
    world.add<PlayerModifiers>(player);
    world.add<PetalRing>(player);
    world.add<Afflictions>(player);
    world.add<HitCooldowns>(player);
    world.add<PlayerSkillTree>(player);
    auto& slot = world.add<Loadout>(player).slots[0];
    slot.configIndex = content().petalIndex("basic");
    slot.rarity = Rarity::Common;
    double now = 1000;
    auto tick = [&](int count) {
        for (int n = 0; n < count; ++n) {
            now += net::kTickMillis;
            petals.run(world, content(), now, net::kTickSeconds, commands);
            commands.flush();
        }
    };
    auto petalDamage = [&]() {
        Vec2 at;
        bool found = false;
        Query<PetalInstance, Transform> query{world};
        query.each([&](Entity, PetalInstance& p, Transform& transform) {
            if (p.owner == player && p.slot == 0) { at = transform.position; found = true; }
        });
        CHECK(found);
        const auto victim = world.create();
        world.add<MobTag>(victim);
        world.add<Transform>(victim, Transform{at, 0});
        world.add<Body>(victim, Body{2, 1});
        world.add<Health>(victim, Health{100000, 100000});
        world.add<Faction>(victim, Faction{Team::Hostiles, false});
        SpatialGrid grid;
        Query<Transform, Body> bodies{world};
        bodies.each([&](Entity e, Transform& t, Body& b) {
            grid.insert(e, t.realm, t.position, b.radius);
        });
        CombatSystem combat;
        EventQueue events;
        combat.run(world, grid, content(), now, net::kTickSeconds, commands, events);
        const double damage = 100000 - world.get<Health>(victim).current;
        world.destroy(victim);
        return damage;
    };
    tick(400);
    const double base = petalDamage();
    CHECK(base > 0);
    world.get<PlayerSkillTree>(player).skills.set(SkillId::SharpEdges, 0);
    world.get<PlayerSkillTree>(player).skills.set(SkillId::Damage, 9);
    tick(60); // clear the previous petal swing's source cooldown
    CHECK_NEAR(petalDamage(), base * 2.4, 1e-8);
    CHECK_NEAR(world.get<PlayerModifiers>(player).damageScale, 1.38 * 1.9, 1e-12);
    skillRefundTo(world.get<PlayerSkillTree>(player).skills, SkillId::Damage, -1);
    tick(60);
    CHECK_NEAR(petalDamage(), base, 1e-8);
    CHECK_NEAR(world.get<PlayerModifiers>(player).damageScale, 1.38, 1e-12);
}

TEST(reference_absorption_talent_preview_matches_server_and_protects_hyper) {
    Harness h("reference-absorption", [](const std::string& path) {
        seedUser(path, "absorbqa", "password7");
        seedStack(path, "absorbqa", "petal_rose", Rarity::Rare, 50);
        seedStack(path, "absorbqa", "petal_rose", Rarity::Apex, 1);
        seedStack(path, "absorbqa", "petal_rose", Rarity::PermanentHyper, 2);
        seedStack(path, "absorbqa", "petal_rose", Rarity::Ascendant, 1);
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        auto& record = db.progress(db.findUser("absorbqa")->id);
        record.skills.set(SkillId::Absorbing, 9);
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient client;
    CHECK(loginAs(h, client, "absorbqa", "password7"));
    const auto rose = content().petalIndex("rose");
    const double before = client.profile().totalXp;
    const double preview = absorbXpTotal(50 * absorbUnits(Rarity::Rare), client.profile().skills);
    CHECK_EQ(preview, 6400.0);
    client.requestAbsorb({{rose, Rarity::Rare, 50}}, "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.absorbOutcome().pending; }, 200));
    CHECK(client.absorbOutcome().success);
    CHECK_EQ(client.absorbOutcome().reward, preview);
    CHECK(awaitProfile(h, client, [&](const Profile& p) { return p.totalXp == before + preview; }));
    client.absorbOutcome().pending = false;
    client.requestAbsorb({{rose, Rarity::Apex, 1}}, "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.absorbOutcome().pending; }, 200));
    CHECK(!client.absorbOutcome().success);
    CHECK_EQ(client.profile().stackCount(rose, Rarity::Apex), 1u);
    CHECK_EQ(client.profile().totalXp, before + preview);

    client.absorbOutcome().pending = false;
    const double permanentXp = absorbXpTotal(2 * 16400000.0 + 840000000.0, client.profile().skills);
    client.requestAbsorb({{rose, Rarity::PermanentHyper, 2}, {rose, Rarity::Ascendant, 1}}, "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.absorbOutcome().pending; }, 200));
    CHECK(client.absorbOutcome().success);
    CHECK_EQ(client.absorbOutcome().reward, permanentXp);
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.totalXp == before + preview + permanentXp &&
               p.stackCount(rose, Rarity::PermanentHyper) == 0 &&
               p.stackCount(rose, Rarity::Ascendant) == 0;
    }));
    CHECK(!absorbable(content().petal(content().petalIndex("mjolnir")), Rarity::Ascendant));
}

TEST(radiation_talents_serialize_purchase_and_refund_every_rank) {
    Harness h("radiation-talents", [](const std::string& path) {
        seedUser(path, "radiationqa", "password7");
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        const auto* user = db.findUser("radiationqa");
        CHECK(user != nullptr);
        if (!user) return;
        auto& record = db.progress(user->id);
        record.totalXp = 1e15;
        record.skills.set(SkillId::Health, 2);
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient client;
    CHECK(loginAs(h, client, "radiationqa", "password7"));
    const int startPoints = client.profile().talentPoints();
    CHECK_EQ(client.profile().skills.level(SkillId::RadiationReach), -1);
    CHECK_EQ(client.profile().skills.level(SkillId::RadiationPower), -1);
    client.requestUpgradeSkill(SkillId::RadiationReach, 4);
    h.step(10, {&client});
    CHECK_EQ(client.profile().skills.level(SkillId::RadiationReach), -1);
    constexpr std::array<int, 5> reachSpent{6, 18, 36, 60, 90};
    constexpr std::array<int, 3> powerSpent{4, 12, 26};
    int alreadySpent = 0;
    for (auto id : {SkillId::RadiationReach, SkillId::RadiationPower}) {
        const auto expected = id == SkillId::RadiationReach ? reachSpent.data() : powerSpent.data();
        for (int tier = 0; tier < skillTierCount(id); ++tier) {
            client.requestUpgradeSkill(id, tier);
            CHECK(awaitProfile(h, client, [=](const Profile& p) {
                return p.skills.level(id) == tier;
            }));
            CHECK_EQ(client.profile().talentPoints(), startPoints - alreadySpent - expected[tier]);
            constexpr std::array<double, 5> reachScales{1.20, 1.40, 1.60, 1.80, 2.00};
            constexpr std::array<double, 3> powerScales{1.15, 1.30, 1.50};
            CHECK_NEAR(client.profile().skills.effectScale(id),
                       id == SkillId::RadiationReach ? reachScales[tier] : powerScales[tier], 1e-12);
        }
        alreadySpent += expected[skillTierCount(id) - 1];
    }
    client.disconnect();
    h.step(10, {});
    NetClient reconnect;
    CHECK(loginAs(h, reconnect, "radiationqa", "password7"));
    CHECK_EQ(reconnect.profile().skills.level(SkillId::RadiationReach), 4);
    CHECK_EQ(reconnect.profile().skills.level(SkillId::RadiationPower), 2);
    CHECK_EQ(reconnect.profile().skills.level(SkillId::Health), 2);
    for (auto id : {SkillId::RadiationReach, SkillId::RadiationPower}) {
        reconnect.requestDowngradeSkill(id, 0);
        CHECK(awaitProfile(h, reconnect, [=](const Profile& p) {
            return p.skills.level(id) == -1;
        }));
        CHECK_NEAR(reconnect.profile().skills.effectScale(id), 1.0, 1e-12);
    }
    CHECK_EQ(reconnect.profile().talentPoints(), startPoints);
}

TEST(imported_talents_new_nodes_buy_sync_and_refund_prerequisites) {
    Harness h("imported-talents", [](const std::string& path) {
        seedUser(path, "importedtalentqa", "password7");
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        const auto* user = db.findUser("importedtalentqa");
        CHECK(user != nullptr);
        if (!user) return;
        auto& record = db.progress(user->id);
        record.totalXp = 1e15;
        record.skills.set(SkillId::Damage, 4);
        record.skills.set(SkillId::Reload, 4);
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient client;
    CHECK(loginAs(h, client, "importedtalentqa", "password7"));
    const int before = client.profile().talentPoints();
    const std::array<SkillId, 8> ids{{
        SkillId::SpillRate, SkillId::SecondaryReload, SkillId::FirePower, SkillId::LightningPower,
        SkillId::ExplosionReach, SkillId::CorruptDamage, SkillId::CorruptReload, SkillId::CorruptReach,
    }};
    int spent = 0;
    for (const SkillId id : ids) {
        client.requestUpgradeSkill(id, 0);
        CHECK(awaitProfile(h, client, [=](const Profile& p) { return p.skills.level(id) == 0; }));
        spent += skillTierCost(id, 0);
        CHECK_EQ(client.profile().talentPoints(), before - spent);
    }
    client.requestDowngradeSkill(SkillId::Damage, 4);
    CHECK(awaitProfile(h, client, [](const Profile& p) {
        return p.skills.level(SkillId::Damage) == 3 &&
               p.skills.level(SkillId::FirePower) == -1 &&
               p.skills.level(SkillId::LightningPower) == -1;
    }));
    client.requestDowngradeSkill(SkillId::Reload, 4);
    CHECK(awaitProfile(h, client, [](const Profile& p) {
        return p.skills.level(SkillId::Reload) == 3 &&
               p.skills.level(SkillId::SecondaryReload) == -1;
    }));
    CHECK_EQ(client.profile().skills.level(SkillId::SpillRate), 0);
    CHECK_EQ(client.profile().skills.level(SkillId::ExplosionReach), 0);
}
