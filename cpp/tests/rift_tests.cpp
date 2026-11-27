#include "test.h"
#include "server_harness.h"
#include "client/ui/i18n.h"
#include "server/systems/combat.h"
#include "server/systems/loot.h"

using namespace flix;
using namespace flix::testsupport;

TEST(rift_cooldown_uses_thirty_to_sixty_minutes_and_clamps_old_saves) {
    constexpr std::int64_t now = 1'800'000'000'000;
    constexpr std::int64_t minute = 60'000;
    for (bool resume : {false, true}) {
        Harness h("rift-cooldown", {}, dataDir(), 0);
        CHECK(h.ready);
        if (!h.ready) return;
        auto& db = GameServerPeer::authorityDb(h.server);
        db.setClock([] { return now; });
        if (resume) db.rawTable("riftSchedule")["nextOpenAtMillis"] = double(now + 100 * minute);
        h.step(1, {});
        const auto delay = db.storedTable("riftSchedule")["nextOpenAtMillis"].asDouble() - now;
        CHECK(delay >= 30 * minute);
        CHECK(delay <= 60 * minute);
        if (resume) CHECK_EQ(delay, 60 * minute);
    }
}

TEST(rift_elimination_ends_the_round_and_champion_claims_all_rewards_once) {
    Harness h("rift-round", [](const std::string& path) {
        seedUser(path, "riftwinner", "password7");
        seedUser(path, "riftsecond", "password7");
        seedStack(path, "riftwinner", "petal_basic", Rarity::Super, 5);
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient winner, second;
    winner.setWorldMaps(&h.server.worldMaps());
    const std::vector<NetClient*> clients{&winner, &second};
    if (!connectClient(h, winner) || !connectClient(h, second)) {
        CHECK(false);
        return;
    }
    winner.requestLogin("riftwinner", "password7");
    second.requestLogin("riftsecond", "password7");
    CHECK(h.stepUntil(clients, [&] {
        return winner.status() == NetClient::Status::LoggedIn &&
               second.status() == NetClient::Status::LoggedIn;
    }));
    winner.joinGame(1280, 720, "pvp", "Winner", false);
    second.joinGame(1280, 720, "", "Second", false);
    CHECK(h.stepUntil(clients, [&] {
        return winner.status() == NetClient::Status::Playing &&
               second.status() == NetClient::Status::Playing;
    }));
    CHECK(!winner.riftOpen());
    const auto& maps = h.server.worldMaps();
    bool found = false;
    const Realm main = maps.realmOfId("rift_main", found);
    CHECK(found);
    if (!found) return;
    const auto* map = maps.forRealm(Realm::Overworld);
    const MapElement* pad = nullptr;
    for (const auto& entry : map->elements()) {
        if (entry.padId == "rift") pad = &entry;
    }
    CHECK(pad != nullptr);
    if (!pad) return;
    const auto bodies = Query<PlayerTag, Transform>{h.server.world()}.collect();
    CHECK_EQ(bodies.size(), std::size_t{2});
    if (bodies.size() != 2) return;
    for (const auto body : bodies) {
        CHECK(h.server.world().get<Transform>(body).realm == Realm::Overworld);
        h.server.world().get<Transform>(body).position = pad->centre();
        h.server.world().get<Health>(body).invulnerableUntilMillis =
            std::numeric_limits<double>::infinity();
    }
    h.step(25, clients);
    CHECK(!winner.riftOpen());
    for (const auto body : bodies) CHECK(h.server.world().isAlive(body));

    h.clock += 4.0 * 60 * 60 * 1000;
    CHECK(h.stepUntil(clients, [&] { return winner.riftOpen() && second.riftOpen(); }));
    // Keep each flower within the charging radius despite player shoves and
    // wandering ambient mobs during this accelerated-clock test.
    bool bothEntered = false;
    for (int attempt = 0; attempt < 100 && !bothEntered; ++attempt) {
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            if (auto* at = h.server.world().tryGet<Transform>(bodies[i])) {
                at->position = pad->centre() + Vec2{i == 0 ? -32.0 : 32.0, 0};
            }
        }
        h.step(1, clients);
        bothEntered = winner.view().realm() == main && second.view().realm() == main;
    }
    CHECK(bothEntered);
    if (winner.view().realm() != main || second.view().realm() != main) return;
    CHECK_EQ(winner.profile().inventory.size(), std::size_t{0});
    CHECK(winner.inRiftMap());
    winner.requestBulkCraft(Rarity::Super);
    CHECK(h.stepUntil(clients, [&] {
        for (const auto& line : winner.chat()) {
            if (line.text.find("裂隙对局中无法一键合成") != std::string::npos) return true;
        }
        return false;
    }, 100));
    CHECK_EQ(winner.profile().inventory.size(), std::size_t{0});
    for (int slot = 0; slot < 5; ++slot) {
        const auto expected = content().petalIndex(slot == 4 ? "rose" : "basic");
        CHECK_EQ(winner.profile().loadout[slot].petalIndex, expected);
        CHECK(winner.profile().loadout[slot].rarity == Rarity::Common);
    }
    CHECK(sawText(winner, "离开时清空"));
    const auto* entrant = GameServerPeer::sessionOf(h.server, "riftwinner");
    CHECK(entrant && entrant->arena);
    if (!entrant || !entrant->arena) return;
    LootSystem fixtureLoot;
    const auto rock = content().petalIndex("rock");
    const Vec2 at = h.server.world().get<Transform>(entrant->entity).position;
    CHECK(fixtureLoot.spawnDrop(h.server.world(), rock, Rarity::Rare, at, main, {entrant->entity}) != NULL_ENTITY);
    CHECK(h.stepUntil(clients, [&] { return winner.profile().stackCount(rock, Rarity::Rare) == 1; }, 150));
    CHECK_EQ(GameServerPeer::authorityDb(h.server).progress(entrant->userId)
                 .itemCount(Rarity::Rare, "petal_rock"), 0);
    // The rift freezes XP, so absorbing must also leave the run bag intact.
    const double xpBefore = h.server.world().get<PlayerProgress>(entrant->entity).totalXp;
    winner.requestAbsorb({{rock, Rarity::Rare, 1}}, "password7");
    CHECK(h.stepUntil(clients, [&] { return winner.absorbOutcome().pending; }));
    const auto& absorb = winner.absorbOutcome();
    CHECK(!absorb.success);
    CHECK_EQ(absorb.reward, 0.0);
    CHECK_EQ(absorb.absorbed, 0u);
    CHECK(!absorb.reason.empty());
    CHECK_EQ(entrant->arena->itemCount(Rarity::Rare, "petal_rock"), 1);
    CHECK_EQ(h.server.world().get<PlayerProgress>(entrant->entity).totalXp, xpBefore);
    h.clock += 3.0 * 60 * 1000;
    CHECK(h.stepUntil(clients, [&] { return !winner.riftOpen(); }));
    auto& world = h.server.world();
    Entity secondBody = NULL_ENTITY;
    Query<PlayerTag, NetId>{world}.each([&](Entity body, PlayerTag&, NetId& id) {
        if (id.value == second.view().self().netId) secondBody = body;
    });
    CHECK(secondBody != NULL_ENTITY);
    if (secondBody == NULL_ENTITY) return;
    world.get<Health>(secondBody).current = 0;
    world.add<Dead>(secondBody, Dead{NULL_ENTITY});
    CHECK(h.stepUntil(clients, [&] { return sawText(second, "你已被淘汰"); }));
    h.clock += 16000;
    second.requestRespawn();
    second.sendChat("/rift revive");
    h.step(10, clients);
    CHECK(second.view().realm() != main);
    const Realm victory = maps.realmOfId("rift_victory", found);
    CHECK(found);
    CHECK(h.stepUntil(clients, [&] { return winner.view().realm() == victory; }));
    const auto prizeDrops = [&] {
        std::vector<Entity> drops;
        Query<DropItem, Transform>{world}.each([&](Entity e, DropItem& item, Transform& at) {
            if (at.realm == victory && item.riftPrize) drops.push_back(e);
        });
        return drops;
    };
    const auto championBody = [&] {
        Entity result = NULL_ENTITY;
        Query<PlayerTag, Transform>{world}.each([&](Entity e, PlayerTag&, Transform& at) {
            if (at.realm == victory) result = e;
        });
        return result;
    };
    CHECK_EQ(prizeDrops().size(), std::size_t{12});
    const auto* loser = GameServerPeer::sessionOf(h.server, "riftsecond");
    if (loser && !prizeDrops().empty()) {
        const auto original = world.get<Transform>(loser->entity);
        world.get<Transform>(loser->entity) = world.get<Transform>(prizeDrops().front());
        h.step(1, clients);
        CHECK_EQ(prizeDrops().size(), std::size_t{12});
        world.get<Transform>(loser->entity) = original;
    }
    winner.sendChat("/rift claim");
    h.step(8, clients);
    CHECK_EQ(prizeDrops().size(), std::size_t{12});
    CHECK_EQ(winner.profile().inventory.size(), std::size_t{0});
    if (prizeDrops().empty() || championBody() == NULL_ENTITY) return;
    world.get<Transform>(championBody()).position = world.get<Transform>(prizeDrops().front()).position;
    CHECK(h.stepUntil(clients, [&] { return prizeDrops().size() == 11; }));
    CHECK(winner.view().realm() == victory);

    // A fresh connection must restore only the uncollected petals, even
    // after the results phase has expired.
    winner.disconnect();
    const std::vector<NetClient*> remaining{&second};
    CHECK(h.stepUntil(remaining, [&] { return prizeDrops().empty(); }));
    h.clock += 4.0 * 60 * 1000;
    h.step(2, remaining);
    NetClient returning;
    CHECK(connectClient(h, returning));
    returning.requestLogin("riftwinner", "password7");
    CHECK(h.stepUntil({&returning}, [&] { return returning.status() == NetClient::Status::LoggedIn; }));
    returning.joinGame(1280, 720, "", "Winner", false);
    CHECK(h.stepUntil({&returning}, [&] { return returning.view().realm() == victory; }));
    CHECK_EQ(prizeDrops().size(), std::size_t{11});
    CHECK_EQ(returning.profile().inventory.size(), std::size_t{0});
    for (int i = 0; i < 11 && !prizeDrops().empty(); ++i) {
        if (championBody() == NULL_ENTITY) break;
        world.get<Transform>(championBody()).position = world.get<Transform>(prizeDrops().front()).position;
        h.step(3, {&returning});
    }
    CHECK(h.stepUntil({&returning}, [&] { return returning.view().realm() == Realm::Overworld; }));
    CHECK(prizeDrops().empty());
    CHECK_EQ(returning.profile().stackCount(rock, Rarity::Rare), 0u);
    unsigned total = 0;
    unsigned permanentHyper = 0;
    unsigned fission = 0;
    for (const auto& stack : returning.profile().inventory) {
        CHECK(stack.petalIndex != content().petalIndex("mjolnir"));
        CHECK(stack.petalIndex != content().petalIndex("timed_mjolnir"));
        if (stack.rarity == Rarity::Unique) total += stack.count;
        if (stack.rarity == Rarity::PermanentHyper) permanentHyper += stack.count;
        if (stack.rarity == Rarity::Unique && stack.petalIndex == content().petalIndex("fission")) fission += stack.count;
    }
    CHECK_EQ(total, 11u);
    CHECK_EQ(fission, 1u);
    CHECK_EQ(permanentHyper, 1u);
    const auto* account = GameServerPeer::authorityDb(h.server).findProgress(
        GameServerPeer::sessionOf(h.server, "riftwinner")->userId);
    CHECK(account && account->extra["riftPrizes"].items().empty());
}

TEST(ghost_announcements_start_at_hyper) {
    Harness h("ghost-announcements", [](const std::string& path) {
        seedUser(path, "boss", "password7", true);
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("boss", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.joinGame(1280, 720, "", "Boss", false);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    const auto language = ui::language();
    ui::setLanguage(ui::Language::En);
    for (const auto rarity : {Rarity::Super, Rarity::Unique, Rarity::Apex, Rarity::Ascendant}) {
        const auto mark = client.chatSequence();
        const std::string label = std::string(rarityLabel(rarity)) + " Ghost";
        GameServerPeer::announceGhost(h.server, rarity);
        h.step(3, {&client});
        const bool shouldAnnounce = rarity == Rarity::Apex || rarity == Rarity::Ascendant;
        CHECK_EQ(sawTextSince(client, mark, label + " has spawned"), shouldAnnounce);
        CHECK_EQ(sawTextSince(client, mark, label + " has been summoned"), shouldAnnounce);
        CHECK_EQ(sawTextSince(client, mark, label + " has been defeated"), shouldAnnounce);
    }
    ui::setLanguage(language);
}

TEST(rift_entrant_keeps_account_level) {
    // Regression: enterRift used to call startArenaRun(PlayerRecord{}), which
    // zeroed XP and spawned a level-1 flower (110 HP) under Mythic ambient
    // spiders. Keep the account's level; the common starter kit is unchanged.
    constexpr int kSeedLevel = 40;
    Harness h("rift-level", [](const std::string& path) {
        seedUser(path, "veteran", "password7");
        Database db;
        std::string error;
        db.load(path, error);
        const Account* account = db.findUser("veteran");
        if (!account) return;
        PlayerRecord& record = db.progress(account->id);
        double xp = 0.0;
        for (int l = 1; l < kSeedLevel; ++l) xp += xpForNextLevel(l);
        record.totalXp = xp;
        db.markDirty();
        db.save();
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("veteran", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.joinGame(1280, 720, "", "Veteran", false);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    CHECK_EQ(client.view().self().level, kSeedLevel);

    const auto& maps = h.server.worldMaps();
    bool found = false;
    const Realm main = maps.realmOfId("rift_main", found);
    CHECK(found);
    if (!found) return;
    const auto* map = maps.forRealm(Realm::Overworld);
    const MapElement* pad = nullptr;
    for (const auto& entry : map->elements()) {
        if (entry.padId == "rift") pad = &entry;
    }
    CHECK(pad != nullptr);
    if (!pad) return;

    Entity body = NULL_ENTITY;
    Query<PlayerTag, Transform>{h.server.world()}.each([&](Entity e, PlayerTag&, Transform&) {
        body = e;
    });
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    h.server.world().get<Transform>(body).position = pad->centre();
    h.server.world().get<Health>(body).invulnerableUntilMillis =
        std::numeric_limits<double>::infinity();

    h.clock += 4.0 * 60 * 60 * 1000;
    CHECK(h.stepUntil({&client}, [&] { return client.riftOpen(); }));
    bool entered = false;
    for (int attempt = 0; attempt < 100 && !entered; ++attempt) {
        if (auto* at = h.server.world().tryGet<Transform>(body)) at->position = pad->centre();
        h.step(1, {&client});
        entered = client.view().realm() == main;
    }
    CHECK(entered);
    if (!entered) return;

    CHECK_EQ(client.view().self().level, kSeedLevel);
    Entity riftBody = NULL_ENTITY;
    Query<PlayerTag, Transform>{h.server.world()}.each([&](Entity e, PlayerTag&, Transform& at) {
        if (at.realm == main) riftBody = e;
    });
    CHECK(riftBody != NULL_ENTITY);
    if (riftBody != NULL_ENTITY) {
        CHECK_EQ(h.server.world().get<PlayerProgress>(riftBody).level, kSeedLevel);
        // Flat PVP pool still applies in rift; the balance fix is level retention
        // (body/petal damage) plus the mob-contact PvE brake.
        CHECK_NEAR(h.server.world().get<Health>(riftBody).max, kArenaMaxHealth, 1e-9);
        CHECK(h.server.world().has<RiftPlayer>(riftBody));
    }
    for (int slot = 0; slot < 5; ++slot) {
        const auto expected = content().petalIndex(slot == 4 ? "rose" : "basic");
        CHECK_EQ(client.profile().loadout[slot].petalIndex, expected);
        CHECK(client.profile().loadout[slot].rarity == Rarity::Common);
    }
}

TEST(rift_portal_arrival_blocks_damage_and_repeat_travel_for_three_seconds) {
    Harness h("rift-portal-protection", [](const std::string& path) {
        seedUser(path, "portalrunner", "password7");
    }, dataDir(), 0);
    if (!h.ready) { CHECK(h.ready); return; }
    NetClient player;
    if (!connectClient(h, player)) { CHECK(false); return; }
    player.requestLogin("portalrunner", "password7");
    CHECK(h.stepUntil({&player}, [&] { return player.status() == NetClient::Status::LoggedIn; }));
    player.joinGame(1280, 720, "", "Runner", false);
    CHECK(h.stepUntil({&player}, [&] { return player.status() == NetClient::Status::Playing; }));
    const auto* session = GameServerPeer::sessionOf(h.server, "portalrunner");
    if (!session || !session->playing()) { CHECK(false); return; }

    const auto& maps = h.server.worldMaps();
    bool found = false;
    const Realm main = maps.realmOfId("rift_main", found);
    if (!found) { CHECK(found); return; }
    const Realm sewers = maps.realmOfId("rift_sewers", found);
    if (!found) { CHECK(found); return; }
    const auto findPad = [&](Realm realm, const std::string& target) -> const MapElement* {
        for (const auto& pad : maps.forRealm(realm)->elements()) {
            if (pad.kind == MapElementKind::Teleporter && pad.targetMap == target) return &pad;
        }
        return nullptr;
    };
    const auto* entry = findPad(Realm::Overworld, "rift_main");
    const auto* onward = findPad(main, "rift_sewers");
    const auto* back = findPad(sewers, "rift_main");
    CHECK(entry && onward && back);
    if (!entry || !onward || !back) return;

    auto& world = h.server.world();
    const auto takePad = [&](const MapElement& pad, Realm target, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            if (auto* at = world.tryGet<Transform>(session->entity)) at->position = pad.centre();
            h.step(1, {&player});
            if (player.view().realm() == target) return true;
        }
        return false;
    };
    h.clock += 4.0 * 60 * 60 * 1000;
    CHECK(h.stepUntil({&player}, [&] { return player.riftOpen(); }));
    const bool entered = takePad(*entry, main, 100);
    CHECK(entered);
    if (!entered) return;
    const Entity flower = session->entity;
    const double entryUntil = world.get<RiftPlayer>(flower).portalProtectedUntilMillis;
    CHECK_NEAR(entryUntil, GameServerPeer::practiceClock(h.server) + 3000.0, 1e-6);
    CHECK(!takePad(*onward, sewers, 40));
    CHECK_NEAR(world.get<RiftPlayer>(flower).portalProtectedUntilMillis, entryUntil, 1e-6);
    const bool arrived = takePad(*onward, sewers, 100);
    CHECK(arrived);
    if (!arrived) return;

    const double now = GameServerPeer::practiceClock(h.server);
    const double until = world.get<RiftPlayer>(flower).portalProtectedUntilMillis;
    CHECK_NEAR(until, now + 3000.0, 1e-6);
    CHECK_NEAR(world.get<Health>(flower).invulnerableUntilMillis, until, 1e-6);
    CHECK_NEAR(world.get<TeleporterState>(flower).cooldownUntilMillis,
               now + kTeleporterCooldownMillis, 1e-6);

    const Entity opponent = world.create();
    world.add<Health>(opponent, Health{100.0, 100.0});
    const Entity pet = world.create();
    world.add<Pet>(pet).owner = flower;
    const Entity shot = world.create();
    world.add<Projectile>(shot).owner = pet;
    world.get<Projectile>(shot).creditTo = flower;
    const Entity field = world.create();
    world.add<GroundEffect>(field).owner = flower;
    auto& combat = GameServerPeer::practiceCombat(h.server);
    for (const Entity source : {flower, pet, shot, field}) {
        CHECK(combat.applyDamage(world, opponent, source, 1.0, until - 1.0).refused);
    }
    CHECK(combat.applyDamage(world, opponent, flower, 1.0, until - 1.0, DamageKind::Poison).refused);
    combat.applyPoison(world, opponent, shot, 1.0, 5000.0, until - 1.0);
    CombatSystem::ignite(world, opponent, shot, flower, 1.0, 5000.0, until - 1.0);
    CHECK(!world.has<Afflictions>(opponent));
    CHECK_NEAR(world.get<Health>(opponent).current, 100.0, 1e-6);
    CHECK(combat.applyDamage(world, flower, opponent, 1.0, until - 1.0).refused);
    CHECK(combat.applyDamage(world, flower, NULL_ENTITY, 1.0, until - 1.0, DamageKind::Poison).refused);

    CHECK(!takePad(*back, main, 40));
    CHECK_NEAR(world.get<RiftPlayer>(flower).portalProtectedUntilMillis, until, 1e-6);
    CHECK(combat.applyDamage(world, opponent, shot, 1.0, until).applied > 0.0);
    CHECK(combat.applyDamage(world, flower, opponent, 1.0, until).applied > 0.0);
    for (const Entity fixture : {opponent, pet, shot, field}) world.destroy(fixture);
    // The fixture jumped directly onto the arrival pad before the first
    // ordinary movement tick. A player must leave that pad once before it
    // can charge again, independent of the three-second damage protection.
    world.get<Transform>(flower).position = back->centre() + Vec2{400.0, 0.0};
    h.step(1, {&player});
    CHECK(!world.get<TeleporterState>(flower).arriving);
    CHECK(takePad(*back, main, 160));
    CHECK_NEAR(world.get<RiftPlayer>(flower).portalProtectedUntilMillis,
               GameServerPeer::practiceClock(h.server) + 3000.0, 1e-6);
}
