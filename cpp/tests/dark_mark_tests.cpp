#include "test.h"
#include "test_data.h"
#include "server_harness.h"
#include "server/systems/petals.h"
#include "shared/game/dark_mark.h"

using namespace flix;
using Peer = testsupport::GameServerPeer;
namespace {
struct MarkFixture {
    GameServer server;
    net::Connection caster{-1, 81, "127.0.0.1"};
    net::Connection victim{-1, 82, "127.0.0.2"};
    Entity owner = NULL_ENTITY, target = NULL_ENTITY;
    bool ready = false;
    MarkFixture() {
        ServerConfig config;
        config.port = 47889; config.botCount = 0;
        config.dataDir = testsupport::dataDir();
        config.databasePath = testsupport::tempDir("dark-mark") + "/inventory.json";
        std::string error;
        ready = server.start(config, error);
        CHECK(ready);
        if (!ready) return;
        join(caster); join(victim);
        Query<PlayerTag, PlayerAccount> players{server.world()};
        players.each([&](Entity e, PlayerTag&, PlayerAccount& a) {
            if (a.connection == 81) owner = e;
            if (a.connection == 82) target = e;
        });
        CHECK(owner != NULL_ENTITY); CHECK(target != NULL_ENTITY);
        ready = owner && target;
        if (!ready) return;
        for (const Entity e : {owner, target}) server.world().get<PlayerProgress>(e).level = 20;
        Peer::markClock(server, 1000000);
    }
    void send(net::Connection& connection, ByteWriter& w) {
        ByteReader reader(w.data(), w.size()); server.onMessage(connection, reader); CHECK(reader.ok());
    }
    void refreshTarget() {
        Peer::flushCommands(server);
        target = NULL_ENTITY;
        Query<PlayerTag, PlayerAccount> players{server.world()};
        players.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
            if (account.connection == victim.id()) target = e;
        });
        CHECK(target != NULL_ENTITY);
    }
    void command(net::ClientMessage action) {
        ByteWriter request;
        request.u8(static_cast<std::uint8_t>(action));
        send(victim, request);
    }
    void join(net::Connection& connection) {
        server.onConnect(connection);
        ByteWriter hello; hello.u8(static_cast<std::uint8_t>(net::ClientMessage::Hello));
        hello.u16(net::kProtocolVersion); hello.u32(content().contentHash()); send(connection, hello);
        ByteWriter guest; guest.u8(static_cast<std::uint8_t>(net::ClientMessage::RegisterGuest)); send(connection, guest);
        enterGarden(connection);
    }
    void enterGarden(net::Connection& connection) {
        ByteWriter join; join.u8(static_cast<std::uint8_t>(net::ClientMessage::JoinGame));
        join.u16(1280); join.u16(720); join.str("garden:spawn"); join.str("Mark QA"); join.boolean(false); send(connection, join);
    }
    void kill() {
        server.world().get<Health>(target).current = 0;
        server.world().ensure<Dead>(target);
        Peer::reportDeaths(server);
    }
    void returnCommand() {
        ByteWriter w; w.u8(static_cast<std::uint8_t>(net::ClientMessage::Chat)); w.str("/mark-return"); send(victim, w);
    }
};
}

TEST(dark_mark_normal_transports_revives_returns_and_preserves_progress) {
    MarkFixture f; if (!f.ready) return;
    auto& w = f.server.world();
    const Transform before = w.get<Transform>(f.target);
    const auto stars = w.get<PlayerProgress>(f.target).stars;
    CHECK(!Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
    f.kill();
    CHECK(Peer::applyMark(f.server, f.target, f.owner, Rarity::Unique));
    CHECK(isStanding(w, f.target));
    CHECK(f.server.worldMaps().forRealm(w.get<Transform>(f.target).realm)->biome() == "hel");
    CHECK_NEAR(w.get<TeleporterState>(f.target).markHoldUntilMillis, 0, 0);
    CHECK_EQ(w.get<PlayerProgress>(f.target).stars, stars);
    CHECK_NEAR(w.get<Health>(f.target).current, w.get<Health>(f.target).max, 0);
    f.returnCommand();
    CHECK(w.get<Transform>(f.target).realm == before.realm);
    CHECK(distanceSq(w.get<Transform>(f.target).position, before.position) < 1);
    f.kill();
    CHECK(!Peer::applyMark(f.server, f.target, f.owner, Rarity::Apex));
    Peer::markClock(f.server, 1300000);
    CHECK(Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
}

TEST(dark_mark_hyper_transports_without_imprisonment) {
    // The legacy H tier still teleports without an Apex hold.
    MarkFixture f; if (!f.ready) return;
    auto& w = f.server.world(); const Transform before = w.get<Transform>(f.target);
    f.kill(); CHECK(Peer::applyMark(f.server, f.target, f.owner, Rarity::Apex));
    CHECK_NEAR(w.get<TeleporterState>(f.target).markHoldUntilMillis, 0, 0);
    CHECK(w.get<Transform>(f.target).realm != before.realm);
    f.returnCommand();
    CHECK(w.get<Transform>(f.target).realm == before.realm);
    Peer::markClock(f.server, 1300000); f.kill();
    CHECK(Peer::applyMark(f.server, f.target, f.owner, Rarity::Apex));
    const Realm hel = w.get<Transform>(f.target).realm;
    Peer::markTeleport(f.server, f.target, before.realm, before.position);
    CHECK(w.get<Transform>(f.target).realm == before.realm);
    CHECK(hel != before.realm);
}

TEST(apex_marks_hold_all_destinations_for_two_wall_clock_minutes) {
    struct Variant {
        const char* petal;
        const char* map;
        double chargeMillis;
    };
    const Variant variants[] = {
        {"dark_mark", "hel", 2000.0},
        {"void_mark", "hel", 2000.0},
        {"void_void_mark", "zorr_void", 2000.0},
        {"arctic_frozen_mark", "zorr_arctic", 500.0},
    };
    for (const auto& variant : variants) {
        MarkFixture f;
        if (!f.ready) return;
        auto& w = f.server.world();
        auto& petals = Peer::markPetals(f.server);
        CommandBuffer commands{w};
        const auto index = content().petalIndex(variant.petal);
        auto& slot = w.get<Loadout>(f.owner).slots[0];
        slot.configIndex = index;
        slot.rarity = Rarity::Ascendant;
        petals.run(w, content(), 1000000, 0, commands);
        petals.run(w, content(), 1021000, 0, commands);
        Entity mark = NULL_ENTITY;
        for (const Entity e : w.get<Loadout>(f.owner).spawned) {
            if (w.get<PetalInstance>(e).configIndex == index) mark = e;
        }
        CHECK(mark != NULL_ENTITY);
        if (!mark) continue;

        f.kill();
        const Transform dismissedFrom = w.get<Transform>(f.owner);
        w.get<Transform>(f.target).position = dismissedFrom.position;
        w.get<Transform>(mark).position = dismissedFrom.position;
        w.get<PlayerInput>(f.owner).current.flags = net::InputAttack;
        Peer::markClock(f.server, 1021000);
        petals.run(w, content(), 1021000, 0, commands);
        CHECK_EQ(w.get<PetalInstance>(mark).markTarget, f.target);
        const double landingTime = 1021000 + variant.chargeMillis;
        CHECK_NEAR(w.get<PetalInstance>(mark).markReadyAtMillis, landingTime, 0);
        Peer::markClock(f.server, landingTime);
        Peer::markWallClock(f.server, 2000000);
        petals.run(w, content(), landingTime, 0, commands);
        CHECK(isStanding(w, f.target));
        const Realm destination = w.get<Transform>(f.target).realm;
        const auto* map = f.server.worldMaps().forRealm(destination);
        CHECK(map && map->id() == variant.map);
        const auto* pads = w.tryGet<TeleporterState>(f.target);
        CHECK(pads != nullptr);
        if (!pads) continue;
        CHECK_NEAR(pads->markHoldUntilMillis, 2120000, 0);

        Peer::markWallClock(f.server, 2119999);
        f.returnCommand();
        Peer::markTeleport(f.server, f.target, dismissedFrom.realm, dismissedFrom.position);
        CHECK(w.get<Transform>(f.target).realm == destination);

        f.kill();
        f.command(net::ClientMessage::Respawn);
        f.refreshTarget();
        CHECK(isStanding(w, f.target));
        CHECK(w.get<Transform>(f.target).realm == destination);

        f.command(net::ClientMessage::LeaveGame);
        f.enterGarden(f.victim);
        f.refreshTarget();
        CHECK(w.get<Transform>(f.target).realm == destination);
        CHECK_NEAR(w.get<TeleporterState>(f.target).markHoldUntilMillis, 2120000, 0);
        f.returnCommand();
        CHECK(w.get<Transform>(f.target).realm == destination);

        Peer::markWallClock(f.server, 2120000);
        f.returnCommand();
        CHECK(w.get<Transform>(f.target).realm == dismissedFrom.realm);
        CHECK(distanceSq(w.get<Transform>(f.target).position, dismissedFrom.position) < 1);
    }
}

TEST(void_mark_can_target_a_corpse_near_the_flower_and_return_immediately) {
    MarkFixture f;
    if (!f.ready) return;
    auto& w = f.server.world();
    auto& petals = Peer::markPetals(f.server);
    CommandBuffer commands{w};
    const auto index = content().petalIndex("void_mark");
    auto& slot = w.get<Loadout>(f.owner).slots[0];
    slot.configIndex = index;
    slot.rarity = Rarity::Unique;
    petals.run(w, content(), 1000000, 0, commands);
    petals.run(w, content(), 1021000, 0, commands);
    Entity mark = NULL_ENTITY;
    for (Entity entity : w.get<Loadout>(f.owner).spawned) {
        if (w.get<PetalInstance>(entity).configIndex == index) mark = entity;
    }
    CHECK(mark != NULL_ENTITY);
    if (!mark) return;
    f.kill();
    const Transform before = w.get<Transform>(f.owner);
    w.get<Transform>(f.target).position = before.position;
    w.get<Transform>(mark).position = before.position + Vec2{100, 0};
    w.get<PlayerInput>(f.owner).current.flags = net::InputAttack | net::InputDefend;
    Peer::markClock(f.server, 1021000);
    petals.run(w, content(), 1021000, 0, commands);
    CHECK_EQ(w.get<PetalInstance>(mark).markTarget, f.target);
    const double ready = 1021000 + darkMarkDelayMillis(Rarity::Unique);
    Peer::markClock(f.server, ready);
    petals.run(w, content(), ready, 0, commands);
    CHECK(isStanding(w, f.target));
    const auto* destination = f.server.worldMaps().forRealm(w.get<Transform>(f.target).realm);
    CHECK(destination && destination->id() == "hel");
    f.returnCommand();
    CHECK_EQ(w.get<Transform>(f.target).realm, before.realm);
}

TEST(dark_mark_rejects_higher_tier_low_level_other_realms_and_dungeons) {
    MarkFixture f; if (!f.ready) return;
    auto& w = f.server.world(); f.kill();
    w.get<Loadout>(f.target).slots[0] = {};
    auto& slot = w.get<Loadout>(f.target).slots[0];
    slot.configIndex = content().petalIndex("rose"); slot.rarity = Rarity::Apex;
    CHECK(!Peer::canMark(f.server, f.target, f.owner, Rarity::Unique));
    CHECK(Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
    slot.rarity = Rarity::Admin;
    CHECK(!Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
    slot.rarity = Rarity::Common;
    w.get<PlayerProgress>(f.target).level = 19;
    CHECK(!Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
    w.get<PlayerProgress>(f.target).level = 20;
    const Realm original = w.get<Transform>(f.target).realm;
    w.get<Transform>(f.target).realm = Realm::Arena;
    CHECK(!Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
    bool found = false; const Realm dungeon = f.server.worldMaps().realmOfId("swarm", found); CHECK(found);
    w.get<Transform>(f.target).realm = dungeon; w.get<Transform>(f.owner).realm = dungeon;
    CHECK(!Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
    w.get<Transform>(f.target).realm = original; w.get<Transform>(f.owner).realm = original;
    CHECK(Peer::canMark(f.server, f.target, f.owner, Rarity::Apex));
}

TEST(dark_mark_real_petal_arms_once_and_cancels_on_revival) {
    MarkFixture f; if (!f.ready) return;
    auto& w = f.server.world(); CommandBuffer commands{w};
    auto& petals = Peer::markPetals(f.server);
    auto& slot = w.get<Loadout>(f.owner).slots[0];
    slot.configIndex = content().petalIndex("dark_mark"); slot.rarity = Rarity::Apex;
    petals.run(w, content(), 1000000, 0, commands);
    petals.run(w, content(), 1021000, 0, commands);
    Entity mark = NULL_ENTITY;
    for (const auto e : w.get<Loadout>(f.owner).spawned) {
        if (w.get<PetalInstance>(e).configIndex == content().petalIndex("dark_mark")) mark = e;
    }
    CHECK(mark != NULL_ENTITY); if (!mark) return;
    f.kill(); w.get<Transform>(f.target).position = w.get<Transform>(mark).position;
    w.get<PlayerInput>(f.owner).current.flags = 1;
    Peer::markClock(f.server, 1021000); petals.run(w, content(), 1021000, 0, commands);
    CHECK(w.get<PetalInstance>(mark).markTarget == f.target);
    const Vec2 parked = w.get<Transform>(mark).position;
    w.get<Transform>(f.owner).position.x += 500;
    petals.run(w, content(), 1022000, 0.016, commands);
    CHECK(distanceSq(w.get<Transform>(mark).position, parked) < 1e-8);
    w.get<Health>(f.target).current = w.get<Health>(f.target).max; w.remove<Dead>(f.target);
    petals.run(w, content(), 1023000, 0, commands);
    CHECK_NEAR(w.get<Health>(mark).current, 0, 0);
    CHECK(w.get<Transform>(f.target).realm == Realm::Overworld);
}
TEST(arctic_frozen_mark_charges_and_transports_through_the_live_petal) {
    MarkFixture f;
    if (!f.ready) return;
    auto& w = f.server.world();
    CommandBuffer commands{w};
    auto& petals = Peer::markPetals(f.server);
    auto& slot = w.get<Loadout>(f.owner).slots[0];
    const auto index = content().petalIndex("arctic_frozen_mark");
    CHECK(index != kInvalidIndex);
    slot.configIndex = index;
    slot.rarity = Rarity::Common;
    petals.run(w, content(), 1000000, 0, commands);
    petals.run(w, content(), 1021000, 0, commands);
    Entity mark = NULL_ENTITY;
    for (const auto e : w.get<Loadout>(f.owner).spawned) {
        if (w.get<PetalInstance>(e).configIndex == index) mark = e;
    }
    CHECK(mark != NULL_ENTITY);
    if (!mark) return;
    const auto originalRealm = w.get<Transform>(f.target).realm;
    f.kill();
    w.get<Transform>(f.target).position = w.get<Transform>(mark).position;
    w.get<PlayerInput>(f.owner).current.flags = net::InputAttack;
    Peer::markClock(f.server, 1021000);
    petals.run(w, content(), 1021000, 0, commands);
    CHECK(w.get<PetalInstance>(mark).markTarget == f.target);
    CHECK_NEAR(w.get<PetalInstance>(mark).markReadyAtMillis, 1141000.0, 0);
    Peer::markClock(f.server, 1140999);
    petals.run(w, content(), 1140999, 0, commands);
    CHECK(w.has<Dead>(f.target));
    Peer::markClock(f.server, 1141000);
    petals.run(w, content(), 1141000, 0, commands);
    CHECK(isStanding(w, f.target));
    const auto* destination = f.server.worldMaps().forRealm(w.get<Transform>(f.target).realm);
    CHECK(destination && destination->id() == "zorr_arctic");
    f.returnCommand();
    CHECK(w.get<Transform>(f.target).realm == originalRealm);
}
