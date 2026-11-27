// The splitter petal, end to end on a real server.
//
// Four claims, and they are the whole feature:
//
//   * EQUIPPING it splits the flower, with no reload paid and no message sent
//     -- the split follows the loadout.
//   * The two halves are placed on either side of the petal's own cut line,
//     which its artwork draws vertically: one left, one right.
//   * CLICKING the loaded petal hands control to the other half and spends the
//     slot, so switching costs a reload and wearing it does not.
//   * The two halves are one person: they cannot hurt each other, they are
//     each other's squadmates, and no other squad will take them.
//
// Everything below the socket is the shipping path -- a real GameServer, a
// real NetClient, the real protocol.

#include "test.h"

#include <cmath>
#include <string>
#include <vector>

#include "client/net_client.h"
#include "server/game_server.h"
#include "server/loot_eligibility.h"
#include "server/systems/combat.h"
#include "server/systems/loot.h"
#include "server_harness.h"
#include "shared/game/config.h"
#include "shared/game/shop.h"

using namespace flix;

namespace {

using flix::testsupport::GameServerPeer;
using flix::testsupport::Harness;
using flix::testsupport::loginNew;

/// Every flower in the world belonging to `username`, however many bodies it
/// is steering. The test asks the WORLD rather than the session table because
/// "how many bodies does this account have" is exactly what is under test.
std::vector<Entity> bodiesNamed(World& world, const std::string& username) {
    std::vector<Entity> found;
    Query<PlayerTag, PlayerAccount> bodies{world};
    bodies.each([&](Entity e, PlayerTag&, PlayerAccount& account) {
        if (account.userId == username || account.username == username) found.push_back(e);
    });
    return found;
}

/// The one body a just-joined client owns.
Entity soleBody(World& world, const std::string& name) {
    const std::vector<Entity> found = bodiesNamed(world, name);
    return found.size() == 1 ? found.front() : NULL_ENTITY;
}

/// Equips a splitter the way a player does: a petal into the account's bag,
/// then a SetLoadout message.
///
/// Deliberately NOT a write into the live body's Loadout, which is the
/// shortcut the sponge and root tests take. A splitter's second body is built
/// from the ACCOUNT RECORD, so a petal that exists only on one body's
/// component is a splitter the clone does not wear -- and then switching to
/// the clone merges the flower straight back together. The real path is the
/// only one that tests the real thing.
bool equipSplitter(Harness& h, NetClient& client, const char* account, std::uint16_t splitter) {
    if (!h.server.grantAdmin(client.sessionToken())) return false;
    client.sendChat(std::string("/admin give ") + account + " " + kSplitterPetalId + " common 1");
    if (!h.stepUntil({&client}, [&] {
            return client.profile().stackCount(splitter, Rarity::Common) > 0;
        })) {
        return false;
    }
    client.setLoadoutSlot(0, splitter, Rarity::Common);
    return h.stepUntil({&client}, [&] {
        return !client.profile().loadout.empty() &&
               client.profile().loadout[0].petalIndex == splitter;
    });
}

/// Every live pet whose owner is `body`.
std::vector<Entity> petsOf(World& world, Entity body) {
    std::vector<Entity> found;
    Query<Pet> pets{world};
    pets.each([&](Entity e, Pet& pet) {
        if (pet.owner == body && !world.has<Dead>(e)) found.push_back(e);
    });
    return found;
}

/// Pets whose owner is no longer in the world. An ownerless pet wanders for
/// good, so any of these is a leaked summon.
std::size_t orphanedPets(World& world) {
    std::size_t orphans = 0;
    Query<Pet> pets{world};
    pets.each([&](Entity, Pet& pet) {
        if (!world.isAlive(pet.owner)) ++orphans;
    });
    return orphans;
}

/// Splits `account`'s flower, puts an egg in slot 1 -- which, through the one
/// shared account, both halves wear -- and waits for BOTH halves to hatch.
/// Returns the parked half, or NULL_ENTITY if any step failed.
Entity splitWithPetsOnBothHalves(Harness& h, NetClient& client, const char* account) {
    World& world = h.server.world();
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    const char* eggId=content().petalIndex("soldier_ant_egg")!=kInvalidIndex?"soldier_ant_egg":"egg";
    const std::uint16_t egg = content().petalIndex(eggId);
    if (splitter == kInvalidIndex || egg == kInvalidIndex) return NULL_ENTITY;

    if (!equipSplitter(h, client, account, splitter)) return NULL_ENTITY;
    if (!h.stepUntil({&client}, [&] { return bodiesNamed(world, account).size() == 2; })) {
        return NULL_ENTITY;
    }
    client.sendChat(std::string("/admin give ") + account + " "+eggId+" common 1");
    if (!h.stepUntil({&client}, [&] {
            return client.profile().stackCount(egg, Rarity::Common) > 0;
        })) {
        return NULL_ENTITY;
    }
    client.setLoadoutSlot(1, egg, Rarity::Common);
    // Past the egg's equip cooldown, on both halves. A pet that has hatched
    // is kept out of harm's way until the other half's has too: a wild mob at
    // the door can kill a common soldier ant first, and whether one is about
    // depends on everything else the world's dice were spent on. The roads
    // out under test destroy the pets; none of them hurts one.
    if (!h.stepUntil({&client}, [&] {
            const std::vector<Entity> halves = bodiesNamed(world, account);
            if (halves.size() != 2) return false;
            bool both = true;
            for (const Entity half : halves) {
                world.get<Health>(half).invulnerableUntilMillis=1e18;
                const std::vector<Entity> pets = petsOf(world, half);
                for (const Entity pet : pets) {
                    world.get<Health>(pet).invulnerableUntilMillis = 1e18;
                }
                if (pets.empty()) both = false;
            }
            return both;
        }, static_cast<int>(std::ceil(content().petalStats(egg,Rarity::Common).reloadMillis*kPetalReloadFeelScale/net::kTickMillis))+600)) {
        return NULL_ENTITY;
    }

    const std::uint32_t steered = client.view().self().netId;
    for (const Entity half : bodiesNamed(world, account)) {
        if (world.get<NetId>(half).value != steered) return half;
    }
    return NULL_ENTITY;
}

} // namespace

TEST(equipping_a_splitter_cuts_the_flower_along_the_petals_line) {
    Harness h("splitter-equip", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity original = soleBody(world, "alice");
    CHECK(original != NULL_ENTITY);
    if (original == NULL_ENTITY) return;

    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    CHECK(splitter != kInvalidIndex);
    if (splitter == kInvalidIndex) return;

    // No use, no chord, no reload: wearing it is what splits you. Budgeted at
    // a handful of ticks rather than the default four hundred, because "as
    // soon as it is equipped" is the claim -- a version that waited out the
    // petal's ten-second cooldown would pass an open-ended wait.
    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }, 5));
    const std::vector<Entity> halves = bodiesNamed(world, "alice");
    CHECK_EQ(halves.size(), std::size_t{2});
    if (halves.size() != 2) return;

    // The petal's artwork is a flower clipped by a vertical line, so the cut
    // is vertical and the halves are a LEFT one and a RIGHT one: same row,
    // one body's width apart. Measured on the tick the split happened, before
    // the ordinary body separation has had anything to say about it.
    const Vec2 a = world.get<Transform>(halves[0]).position;
    const Vec2 b = world.get<Transform>(halves[1]).position;
    const double radius = world.get<Body>(halves[0]).radius;
    CHECK(std::fabs(a.y - b.y) < 1.0);
    CHECK(std::fabs(std::fabs(a.x - b.x) - radius * 2.0) < 1.0);

    // And it did not wait for a reload -- it split THROUGH one. Every slot
    // serves a full cooldown the moment its contents change, equipping
    // included (PetalSystem::reconcileSlots), so the flower coming apart while
    // that one is still running is the proof that wearing the petal is the
    // whole trigger. Switching halves is the part that has to wait for it.
    CHECK(world.get<Loadout>(original).slots[0].broken);
}

TEST(splitting_underground_keeps_both_halves_underground_until_the_same_deadline) {
    Harness h("splitter-burrow", {}, testsupport::dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    constexpr auto name = "burrower";
    if (!loginNew(h, client, name, "hunter2!")) { CHECK(false); return; }
    client.joinGame(1280, 720, {}, name);
    if (!h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; })) {
        CHECK(false);
        return;
    }

    World& world = h.server.world();
    const Entity original = soleBody(world, name);
    if (original == NULL_ENTITY) { CHECK(false); return; }

    const auto predator = content().mobIndex("desert_centipede");
    const auto surface = content().mobIndex("ladybug");
    const double deadline = GameServerPeer::practiceClock(h.server) + 8000.0;
    auto& state = world.add<BurrowState>(original, BurrowState{deadline, true});
    state.predatorIndices.front() = predator;

    if (!equipSplitter(h, client, name, content().petalIndex(kSplitterPetalId))) {
        CHECK(false);
        return;
    }
    if (!h.stepUntil({&client}, [&] { return bodiesNamed(world, name).size() == 2; }, 5)) {
        CHECK(false);
        return;
    }
    const auto halves = bodiesNamed(world, name);
    const Entity surfaceMob = world.create();
    world.add<MobType>(surfaceMob, MobType{surface, Rarity::Common});
    const Entity undergroundMob = world.create();
    world.add<MobType>(undergroundMob, MobType{predator, Rarity::Common});
    for (Entity half : halves) {
        const auto* burrow = world.tryGet<BurrowState>(half);
        CHECK(burrow && burrow->active);
        if (burrow) CHECK_NEAR(burrow->untilMillis, deadline, 1e-8);
        CHECK(!canNoticeBurrowed(world, surfaceMob, half));
        CHECK(canNoticeBurrowed(world, undergroundMob, half));
    }
    CHECK(h.stepUntil({&client}, [&] {
        return GameServerPeer::practiceClock(h.server) >= deadline;
    }, 200));
    for (Entity half : halves) CHECK(canNoticeBurrowed(world, surfaceMob, half));
}

TEST(a_split_flowers_two_halves_are_one_persons) {
    Harness h("splitter-one-person", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity original = soleBody(world, "alice");
    CHECK(original != NULL_ENTITY);
    if (original == NULL_ENTITY) return;
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    if (splitter == kInvalidIndex) { CHECK(false); return; }

    // Wounded going in: a split is a flower cut in two, not a second one
    // issued free, so the halves come out at the fraction it was standing at
    // rather than one of them at full.
    Health& pool = world.get<Health>(original);
    pool.current = pool.max * 0.5;
    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));

    const std::vector<Entity> halves = bodiesNamed(world, "alice");
    if (halves.size() != 2) { CHECK(false); return; }
    const double hurt = world.get<Health>(halves[0]).fraction();
    // Stated as "the same, and not full" rather than as an exact 0.5: the
    // equip takes a handful of ticks to come back and a flower's own passive
    // heal runs through them, so pinning the number would be pinning the
    // regeneration rate as well.
    CHECK(hurt < 0.9);
    CHECK(std::fabs(world.get<Health>(halves[1]).fraction() - hurt) < 0.02);

    // The same connection on both, which is what every "one person" rule below
    // keys off -- combat's friendly-fire exemption and the loot ranking's
    // one-share-per-person.
    const net::ConnectionId owner = world.get<PlayerAccount>(halves[0]).connection;
    CHECK(owner != 0);
    CHECK_EQ(world.get<PlayerAccount>(halves[1]).connection, owner);

    // BOTH ARE STREAMED to their owner's client. The parked half is one click
    // away from being the one you are steering, so it is never allowed to fall
    // out of the snapshot -- and its party bar is drawn from the body, so a
    // half that is not streamed is a bar with nothing in it.
    CHECK(h.stepUntil({&alice}, [&] {
        std::size_t flowers = 0;
        for (const auto& e : alice.view().entities()) {
            if (e.second.kind == net::EntityKind::Player) ++flowers;
        }
        return flowers == 2;
    }));

    // One squad, holding both bodies: the party HUD carries a bar for the half
    // the player is not looking through.
    CHECK(h.stepUntil({&alice}, [&] { return alice.squad().members.size() == 2; }));
    CHECK(!alice.squad().inSquad);
    CHECK(alice.squad().joinCode.empty());
    CHECK(alice.squad().members[0].netId != 0);
    CHECK(alice.squad().members[1].netId != 0);
    CHECK(alice.squad().members[0].netId != alice.squad().members[1].netId);
}

TEST(one_persons_two_bodies_rank_and_are_paid_as_one) {
    // The loot ranking is a pure function over a damage tally, so the claim is
    // made here rather than through a socket: two bodies on one connection are
    // ONE contender scored on one person's damage, and they take one of the
    // corpse's slots between them.
    SquadEntityIndex index;
    const Entity left = static_cast<Entity>(101);
    const Entity right = static_cast<Entity>(102);
    index.groups.push_back({SquadBody{left, 7}, SquadBody{right, 7}});
    index.group[left] = 0;
    index.group[right] = 0;

    CHECK_EQ(contenderSize(index.groups[0]), 1);

    std::vector<Bounty::Share> shares;
    shares.push_back({left, 7, 50.0});
    std::vector<Entity> paid;
    selectLootRecipients(shares, 4, &index, paid);
    // One slot spent, on the half that did the work.
    CHECK_EQ(paid.size(), std::size_t{1});
    if (!paid.empty()) CHECK_EQ(paid.front(), left);

    // And two real people in a squad still take two slots, so the rule above
    // did not just turn every squad into one payout.
    SquadEntityIndex pair;
    pair.groups.push_back({SquadBody{left, 7}, SquadBody{right, 8}});
    pair.group[left] = 0;
    pair.group[right] = 0;
    CHECK_EQ(contenderSize(pair.groups[0]), 2);
    selectLootRecipients(shares, 4, &pair, paid);
    CHECK_EQ(paid.size(), std::size_t{2});
}

TEST(clicking_the_loaded_splitter_swaps_halves_and_pays_a_reload) {
    Harness h("splitter-click", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity original = soleBody(world, "alice");
    if (original == NULL_ENTITY) { CHECK(false); return; }
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    if (splitter == kInvalidIndex) { CHECK(false); return; }

    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }, 5));

    // Which flower the client is looking through, before and after. The client
    // learns this from the self flag on a spawn record, which is what the
    // switch's view reset restates.
    // This test waits through a full reload in a populated map. Keep both
    // flowers alive so wild mobs cannot turn a switch check into a death test.
    for(const Entity half:bodiesNamed(world,"alice"))
        world.get<Health>(half).invulnerableUntilMillis=1e12;
    const std::uint32_t steered = alice.view().self().netId;
    CHECK(steered != 0);

    // A click before the petal has loaded does nothing at all -- the equip
    // cooldown is a real one, and the tile is drawn with its wedge over it.
    alice.usePetal(0);
    h.step(10, {&alice});
    CHECK_EQ(alice.view().self().netId, steered);

    // Loaded, and now it swaps. Budgeted past the petal's own ten seconds, and
    // waiting on BOTH halves: they were born a tick apart, so their equip
    // cooldowns lapse a tick apart, and "loaded" has to mean the one the click
    // will land on as well as the one it is sent from.
    CHECK(h.stepUntil({&alice}, [&] {
        for (const Entity half : bodiesNamed(world, "alice")) {
            if (world.get<Loadout>(half).slots[0].broken) return false;
        }
        return true;
    }, 600));
    // Account equipment updates may replace the original body. Resolve the
    // current half by the authoritative net ID before testing held input.
    Entity activeBeforeClick=NULL_ENTITY;
    for(const Entity half:bodiesNamed(world,"alice"))
        if(world.get<NetId>(half).value==steered)activeBeforeClick=half;
    CHECK(activeBeforeClick!=NULL_ENTITY);
    if(activeBeforeClick==NULL_ENTITY)return;
    // Both bodies keep the splitter on the bar with no physical petal to hit.
    for (const Entity half : bodiesNamed(world, "alice")) {
        CHECK_EQ(world.get<Loadout>(half).slots[0].configIndex, splitter);
    }
    int orbitingSplitters = 0;
    Query<PetalInstance>{world}.without<Dead>().each([&](Entity, PetalInstance& state) {
        if (state.configIndex == splitter) ++orbitingSplitters;
    });
    CHECK_EQ(orbitingSplitters, 0);
    const auto beforeSwitchDeadline = world.get<Loadout>(activeBeforeClick).slots[0].reloadReadyAtMillis;
    auto& held=world.get<PlayerInput>(activeBeforeClick).current;
    held.moveStrength=1;held.moveAngle=.75;held.flags=net::InputDefend;
    alice.usePetal(0);
    CHECK(h.stepUntil({&alice}, [&] {
        return alice.view().self().netId != 0 && alice.view().self().netId != steered;
    }));
    for(const Entity half:bodiesNamed(world,"alice")){
        const bool active=world.get<NetId>(half).value==alice.view().self().netId;
        CHECK_NEAR(world.get<PlayerInput>(half).current.moveStrength,active?1:0,1e-8);
    }
    // Still two flowers: a switch does not merge them.
    CHECK_EQ(bodiesNamed(world, "alice").size(), std::size_t{2});
    // And the slot is reloading, on BOTH halves, so the bar reads the same
    // whichever body is being steered.
    for (const Entity half : bodiesNamed(world, "alice")) {
        const auto& wear = world.get<Loadout>(half).slots[0];
        CHECK(wear.broken);
        CHECK(wear.reloadReadyAtMillis > beforeSwitchDeadline);
    }
    CHECK(h.stepUntil({&alice}, [&] {
        return alice.view().self().slotReloadRemainingMillis[0] > 0.0;
    }));

    // A second click while it reloads does nothing: the switch is rationed by
    // the petal's own cooldown.
    const std::uint32_t nowSteering = alice.view().self().netId;
    alice.usePetal(0);
    h.step(10, {&alice});
    CHECK_EQ(alice.view().self().netId, nowSteering);
}

TEST(taking_the_splitter_off_puts_the_flower_back_together) {
    Harness h("splitter-merge", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity original = soleBody(world, "alice");
    if (original == NULL_ENTITY) { CHECK(false); return; }
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    if (splitter == kInvalidIndex) { CHECK(false); return; }

    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));

    // Off the bar. One account, one loadout: the unequip reaches both bodies
    // through applyAccountToSession, which is what the merge then reads.
    alice.setLoadoutSlot(0, kNoPetal, Rarity::Common);
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 1; }));
    // The flower left standing is the one the player was steering, and its
    // client is still looking through it.
    const std::vector<Entity> left = bodiesNamed(world, "alice");
    CHECK_EQ(left.size(), std::size_t{1});
    if (left.empty()) return;
    CHECK(world.has<NetId>(left.front()));
    CHECK_EQ(alice.view().self().netId, world.get<NetId>(left.front()).value);
}

TEST(a_split_flower_can_join_switch_and_merge_without_losing_squad) {
    const double cell = kTileSize;
    const auto map = flix::testsupport::fixtureMap(24, 24,
        flix::testsupport::fixtureDoor("meadow", "Meadow", cell * 2, cell * 2,
                                      cell * 4, cell * 4, true, 0.0), "");
    const std::string dir = flix::testsupport::stageDataDir("splitter-squad", {{"meadow", map}});
    struct Cleanup {
        const std::string& dir;
        ~Cleanup() { flix::testsupport::removeDataDir(dir); }
    } cleanup{dir};
    if (dir.empty()) { CHECK(false); return; }
    Harness h("splitter-squad", {}, dir, 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    NetClient bob;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    CHECK(loginNew(h, bob, "bob", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    bob.joinGame(1280, 720, {}, "bob");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.status() == NetClient::Status::Playing &&
               bob.status() == NetClient::Status::Playing;
    }));

    World& world = h.server.world();
    const Entity original = soleBody(world, "alice");
    if (original == NULL_ENTITY) { CHECK(false); return; }
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    if (splitter == kInvalidIndex) { CHECK(false); return; }

    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));

    bob.sendChat("/squad create public");
    CHECK(h.stepUntil({&alice, &bob}, [&] { return bob.squad().inSquad; }));
    bob.sendChat("/squad-invite alice");
    CHECK(h.stepUntil({&alice, &bob}, [&] { return alice.squadInvite().waiting; }));
    alice.sendChat("/squad-accept");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.squad().inSquad && alice.squad().id == bob.squad().id &&
               alice.squad().members.size() == 3 && bob.squad().members.size() == 3;
    }));

    alice.sendChat("/squad-leave");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return !alice.squad().inSquad && alice.squad().members.size() == 2;
    }));
    alice.sendChat("/squad-code " + bob.squad().joinCode);
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.squad().inSquad && alice.squad().id == bob.squad().id;
    }));

    alice.sendChat("/squad-leave");
    bob.sendChat("/squad-leave");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return !alice.squad().inSquad && !bob.squad().inSquad;
    }));
    alice.sendChat("/squad-auto on");
    bob.sendChat("/squad-auto on");
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.squad().inSquad && alice.squad().id == bob.squad().id &&
               bob.squad().members.size() == 3;
    }, 240));

    const auto oldBody = alice.view().self().netId;
    // The splitter must finish its initial equip reload before it can switch.
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        const auto halves = bodiesNamed(world, "alice");
        if (halves.size() != 2) return false;
        for (const Entity half : halves) {
            const auto* loadout = world.tryGet<Loadout>(half);
            if (!loadout || loadout->slots[0].broken) return false;
        }
        return true;
    }, 650));
    alice.usePetal(0);
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return alice.view().self().netId != 0 && alice.view().self().netId != oldBody;
    }));
    CHECK_EQ(alice.squad().id, bob.squad().id);
    alice.setLoadoutSlot(0, kNoPetal, Rarity::Common);
    CHECK(h.stepUntil({&alice, &bob}, [&] {
        return bodiesNamed(world, "alice").size() == 1 && bob.squad().members.size() == 2;
    }));
    CHECK(alice.squad().inSquad);
    CHECK_EQ(alice.squad().id, bob.squad().id);
}

TEST(losing_a_half_ends_the_split_and_the_splitter_reloads) {
    Harness h("splitter-death", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity original = soleBody(world, "alice");
    if (original == NULL_ENTITY) { CHECK(false); return; }
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    if (splitter == kInvalidIndex) { CHECK(false); return; }

    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));

    // Kill the half the player is NOT steering. That is a lost flower, not a
    // death: no death card, and the player carries on in the one still up.
    const std::uint32_t steered = alice.view().self().netId;
    Entity parked = NULL_ENTITY;
    for (const Entity half : bodiesNamed(world, "alice")) {
        if (world.get<NetId>(half).value != steered) parked = half;
    }
    CHECK(parked != NULL_ENTITY);
    if (parked == NULL_ENTITY) return;
    world.get<Health>(parked).current = 0.0;

    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 1; }));
    CHECK(!alice.dead());
    CHECK_EQ(alice.view().self().netId, steered);

    // And the splitter is reloading, which is what keeps the spare from being
    // replaced on the very next tick. When it lapses, the flower splits again
    // -- the petal is still on the bar and the bar is what splits you.
    const Entity survivor = bodiesNamed(world, "alice").front();
    CHECK(world.get<Loadout>(survivor).slots[0].broken);
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }, 600));
}

TEST(coordinate_sends_one_clients_commands_to_both_split_halfs_pets) {
    Harness h("splitter-coordinate", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "alice", "hunter2!"));
    client.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    World& world = h.server.world();
    const Entity parked = splitWithPetsOnBothHalves(h, client, "alice");
    CHECK(parked != NULL_ENTITY);
    if (parked == NULL_ENTITY) return;
    const auto bodies = bodiesNamed(world, "alice");
    CHECK_EQ(bodies.size(), 2u);
    if (bodies.size() != 2) return;
    const Entity active = bodies[0] == parked ? bodies[1] : bodies[0];
    const Entity activePet = petsOf(world, active).front();
    const Entity parkedPet = petsOf(world, parked).front();
    const auto coordinate = content().petalIndex("coordinate");
    client.sendChat("/admin give alice coordinate common 1");
    CHECK(h.stepUntil({&client}, [&] {
        return client.profile().stackCount(coordinate, Rarity::Common) == 1;
    }));
    client.setLoadoutSlot(2, coordinate, Rarity::Common);
    CHECK(h.stepUntil({&client}, [&] {
        return world.get<PlayerModifiers>(active).coordinateControlTier >= 0 &&
               world.get<PlayerModifiers>(parked).coordinateControlTier >= 0;
    }));
    const auto position = [&](Entity body) { return world.get<Transform>(body).position; };
    const auto parkedInput = [&]() -> const PlayerInput& { return world.get<PlayerInput>(parked); };
    net::InputFrame input;
    input.sequence = 1;
    input.moveAngle = kPi * 0.5;
    input.moveStrength = 1;
    input.flags = net::InputPetMoveDirection | net::InputNoPetPointer;
    client.sendInput(input);
    CHECK(h.stepUntil({&client}, [&] {
        return world.get<Motion>(activePet).velocity.y > 1.0 &&
               world.get<Motion>(parkedPet).velocity.y > 1.0;
    }, 60));
    CHECK_EQ(parkedInput().coordinateInputSource, active);
    CHECK_EQ(parkedInput().current.moveStrength, 0.0);

    ++input.sequence;
    input.moveStrength = 0.0;
    input.flags = 0;
    input.aimDistance = 500;
    client.sendInput(input);
    CHECK(h.stepUntil({&client}, [&] {
        const Vec2 pointer = position(active) + Vec2{500, 0};
        const auto movesToward = [&](Entity pet) {
            const Vec2 delta = pointer - position(pet);
            const Vec2 velocity = world.get<Motion>(pet).velocity;
            return velocity.x * delta.x + velocity.y * delta.y > 0.0;
        };
        return movesToward(activePet) && movesToward(parkedPet);
    }, 60));
    CHECK_EQ(parkedInput().current.moveStrength, 0.0);
}

TEST(taking_the_splitter_off_takes_the_parked_halfs_pets_with_it) {
    // The dupe: let the parked half hatch its own squad, take the splitter
    // off, and the half went but its pets stayed -- ownerless, wandering, and
    // one more squad on every re-equip.
    Harness h("splitter-merge-pets", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity parked = splitWithPetsOnBothHalves(h, alice, "alice");
    CHECK(parked != NULL_ENTITY);
    if (parked == NULL_ENTITY) return;
    const std::vector<Entity> parkedPets = petsOf(world, parked);
    CHECK(!parkedPets.empty());

    alice.setLoadoutSlot(0, kNoPetal, Rarity::Common);
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 1; }));
    h.step(5, {&alice});

    for (const Entity pet : parkedPets) CHECK(!world.isAlive(pet));
    CHECK_EQ(orphanedPets(world), std::size_t{0});
    // The half still standing keeps its own: the merge takes the OTHER body's
    // kit, not the player's.
    const std::vector<Entity> survivor = bodiesNamed(world, "alice");
    CHECK_EQ(survivor.size(), std::size_t{1});
    if (!survivor.empty()) CHECK(!petsOf(world, survivor.front()).empty());
}

TEST(losing_the_parked_half_takes_its_pets_with_it) {
    // The other road out of a split: the parked half is KILLED, and the
    // reaper meets a corpse nobody owns on the same tick. Killed by a mob,
    // not by zeroing its health between ticks: combat runs after the ring
    // pass, so a combat death is never seen down by the pass that recalls a
    // downed flower's summons. A health bar emptied from outside the tick is,
    // and would pass this test with the leak still in.
    Harness h("splitter-death-pets", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const Entity parked = splitWithPetsOnBothHalves(h, alice, "alice");
    CHECK(parked != NULL_ENTITY);
    if (parked == NULL_ENTITY) return;
    const std::vector<Entity> parkedPets = petsOf(world, parked);
    CHECK(!parkedPets.empty());

    // The mob is left nothing else to kill. A pet it killed would be reaped
    // as a death, and the steered half going down too would recall its own
    // squad through the ordinary death path -- either one empties the count
    // below without ever touching the leak.
    for (const Entity half : bodiesNamed(world, "alice")) {
        if (half == parked) continue;
        world.get<Health>(half).invulnerableUntilMillis = 1e18;
        for (const Entity pet : petsOf(world, half)) {
            world.get<Health>(pet).invulnerableUntilMillis = 1e18;
        }
    }
    for (const Entity pet : parkedPets) world.get<Health>(pet).invulnerableUntilMillis = 1e18;
    Health& health = world.get<Health>(parked);
    health.current = health.max * 1e-4;
    health.invulnerableUntilMillis = 0.0;
    health.hitCooldownUntilMillis = 0.0;
    const Vec2 at = world.get<Transform>(parked).position;
    alice.sendChat("/admin spawn soldier_ant mythic " + std::to_string(static_cast<int>(at.x)) +
                   " " + std::to_string(static_cast<int>(at.y)) + " 1");
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 1; }));
    h.step(5, {&alice});

    for (const Entity pet : parkedPets) CHECK(!world.isAlive(pet));
    CHECK_EQ(orphanedPets(world), std::size_t{0});
}

TEST(a_join_over_a_split_that_lost_its_active_half_takes_the_other_half_out) {
    // spawnPlayer will not put a body down over one the session still holds
    // (reauth_tests.cpp, "A body the stage forgot", has the rest of that
    // guard). This is its second clause: a split held with no active half to
    // end it through -- `entity` gone, `splitOther` still standing.
    // despawnPlayer starts from the active half, so it has nothing to do
    // there, and without the clause the new body would become the active
    // half of the old split, with last life's parked flower still on the
    // session. No road leads to that state. This builds it by hand from a
    // real split: the stage dropped under it, so nothing that services a
    // playing split ends this one, and the active half let go of and killed,
    // so the reaper takes it as a body nobody owns -- with bob in the world,
    // because a server with nobody playing freezes its tick, the reaper
    // included.
    Harness h("splitter-lost-active", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice, bob;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));
    World& world = h.server.world();
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    if (splitter == kInvalidIndex) { CHECK(false); return; }
    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));
    // Bob joins only now: equipSplitter steps alice alone, and a client in the
    // world that nobody polls just piles up a snapshot backlog.
    const std::vector<NetClient*> both{&alice, &bob};
    CHECK(loginNew(h, bob, "bob", "hunter2!"));
    bob.joinGame(1280, 720, {}, "bob");
    CHECK(h.stepUntil(both, [&] { return bob.status() == NetClient::Status::Playing; }));

    Session* session = GameServerPeer::sessionOf(h.server, "alice");
    if (session == nullptr || !session->split()) { CHECK(false); return; }
    const Entity active = session->entity;
    const Entity parked = session->splitOther;
    session->stage = SessionStage::Authenticated;
    session->entity = NULL_ENTITY;
    world.get<Health>(active).current = 0.0;
    world.add<Dead>(active, Dead{NULL_ENTITY});
    CHECK(h.stepUntil(both, [&] { return !world.isAlive(active); }));
    session = GameServerPeer::sessionOf(h.server, "alice");
    if (session == nullptr) { CHECK(false); return; }
    CHECK(session->split());
    CHECK(world.isAlive(parked));

    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil(both, [&] {
        const Session* now = GameServerPeer::sessionOf(h.server, "alice");
        return now != nullptr && now->playing();
    }));
    h.step(3, both);
    // The old half is gone, and whatever alice stands in now -- the splitter is
    // still on the bar, so it may already be two bodies again -- is all new.
    CHECK(!world.isAlive(parked));
    const std::vector<Entity> now = bodiesNamed(world, "alice");
    CHECK(!now.empty());
    for (const Entity body : now) CHECK(body != parked);
}

TEST(the_parked_half_taking_a_pad_leaves_the_client_where_it_is) {
    // The parked half is a body of its own, so a shove can put it on a pad and
    // the pad takes it. sessionForEntity matches EITHER half, and the crossing
    // used to be reported to the client as if the steered half had made it:
    // the session's realm flipped and a RealmChange loaded the far map under a
    // flower that was still standing on this one. Only switching to the half
    // over there may move the client.
    const std::string dir = flix::testsupport::twoMapDataDir("splitterpad");
    CHECK(!dir.empty());
    if (dir.empty()) return;
    Harness h("splitter-pad", {}, dir, 0);
    if (!h.ready) { CHECK(false); flix::testsupport::removeDataDir(dir); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));
    World& world = h.server.world();
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    const MapData* overworld = h.server.worldMaps().forRealm(Realm::Overworld);
    const MapElement* pad = nullptr;
    if (overworld != nullptr) {
        for (const MapElement& element : overworld->elements()) {
            if (element.kind == MapElementKind::Teleporter && element.targetMap == "warren") {
                pad = &element;
            }
        }
    }
    bool found = false;
    const Realm warren = h.server.worldMaps().realmOfId("warren", found);
    if (splitter == kInvalidIndex || pad == nullptr || !found) {
        CHECK(false);
        flix::testsupport::removeDataDir(dir);
        return;
    }

    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));
    Session* session = GameServerPeer::sessionOf(h.server, "alice");
    if (session == nullptr || !session->split()) {
        CHECK(false);
        flix::testsupport::removeDataDir(dir);
        return;
    }
    const Entity active = session->entity;
    const Entity parked = session->splitOther;
    const std::uint32_t steered = alice.view().self().netId;
    CHECK_EQ(steered, world.get<NetId>(active).value);
    Vec2 arrival;
    while (alice.takeRealmChange(arrival)) {}

    // Held on the pad until it fires. A parked half has an empty input frame,
    // so it stays where it is put.
    world.get<Transform>(parked).position = pad->centre();
    CHECK(h.stepUntil({&alice}, [&] { return world.get<Transform>(parked).realm == warren; }, 150));
    h.step(10, {&alice});

    // The steered half never moved, and nothing told the client it had.
    CHECK(world.get<Transform>(active).realm == Realm::Overworld);
    session = GameServerPeer::sessionOf(h.server, "alice");
    CHECK(session != nullptr && session->realm == Realm::Overworld);
    CHECK(!alice.takeRealmChange(arrival));
    CHECK(alice.view().realm() == Realm::Overworld);
    CHECK_EQ(alice.view().self().netId, steered);

    // Switching to the half in the warren is what sends the client there.
    CHECK(h.stepUntil({&alice}, [&] {
        for (const Entity half : bodiesNamed(world, "alice")) {
            if (world.get<Loadout>(half).slots[0].broken) return false;
        }
        return true;
    }, 600));
    alice.usePetal(0);
    CHECK(h.stepUntil({&alice}, [&] { return alice.view().realm() == warren; }));
    CHECK(h.stepUntil({&alice}, [&] {
        return alice.view().self().netId == world.get<NetId>(parked).value;
    }));
    session = GameServerPeer::sessionOf(h.server, "alice");
    CHECK(session != nullptr && session->realm == warren);
    flix::testsupport::removeDataDir(dir);
}

TEST(a_split_flower_pays_for_what_it_buys) {
    Harness h("splitter-shop", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    const std::uint16_t splitter = content().petalIndex(kSplitterPetalId);
    const std::uint16_t rose = content().petalIndex("rose");
    if (splitter == kInvalidIndex || rose == kInvalidIndex) { CHECK(false); return; }
    CHECK(equipSplitter(h, alice, "alice", splitter));
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));
    const std::vector<Entity> halves = bodiesNamed(world, "alice");
    if (halves.size() != 2) { CHECK(false); return; }

    // Both halves carry the balance, and the split levels them to the higher
    // of the two every tick. A purchase charged to the steered half alone was
    // refunded by the parked one on the next tick: the petal was free, and
    // every click bought another.
    constexpr double kBalance = 1000.0;
    for (const Entity half : halves) world.get<PlayerProgress>(half).stars = kBalance;
    const double price = shopPrice(rose, Rarity::Common);
    CHECK(price > 0 && price * 2 <= kBalance);

    alice.requestBuyPetal(rose, Rarity::Common);
    CHECK(h.stepUntil({&alice}, [&] { return alice.profile().stackCount(rose, Rarity::Common) == 1; }));
    alice.requestBuyPetal(rose, Rarity::Common);
    CHECK(h.stepUntil({&alice}, [&] { return alice.profile().stackCount(rose, Rarity::Common) == 2; }));
    h.step(10, {&alice});

    const double left = kBalance - price * 2;
    for (const Entity half : halves) CHECK_EQ(world.get<PlayerProgress>(half).stars, left);
    CHECK_EQ(alice.view().self().stars, left);
}

TEST(splitter_spare_uses_live_rotation_and_viewport_without_taking_control) {
    Harness h("splitter-rotation-viewport", {}, testsupport::dataDir(), 0);
    CHECK(h.ready); if (!h.ready) return;
    NetClient client;
    CHECK(loginNew(h, client, "alice", "hunter2!"));
    client.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    auto& world = h.server.world();

    net::InputFrame frame;
    frame.sequence = 100;
    frame.rotationStep = 2;
    frame.viewportWidth = 1800;
    frame.viewportHeight = 1000;
    client.sendInput(frame);
    CHECK(h.stepUntil({&client}, [&] {
        const auto body = soleBody(world, "alice");
        return body != NULL_ENTITY && world.get<PlayerInput>(body).current.rotationStep == 2;
    }));
    CHECK(equipSplitter(h, client, "alice", content().petalIndex(kSplitterPetalId)));
    CHECK(h.stepUntil({&client}, [&] { return bodiesNamed(world, "alice").size() == 2; }));
    const auto halves = bodiesNamed(world, "alice");
    if (halves.size() != 2) return;
    for (auto body : halves) {
        CHECK_EQ(world.get<PlayerInput>(body).current.rotationStep, 2);
        CHECK_EQ(world.get<PlayerLocation>(body).viewport.x, 1800.0);
        CHECK_EQ(world.get<PlayerLocation>(body).viewport.y, 1000.0);
        world.get<Health>(body).invulnerableUntilMillis = 1e12;
    }

    frame.sequence++;
    frame.rotationStep = 1;
    frame.viewportWidth = 2400;
    frame.viewportHeight = 1400;
    client.sendInput(frame);
    h.step(5, {&client});
    for (auto body : halves) {
        CHECK_EQ(world.get<PlayerInput>(body).current.rotationStep, 1);
        CHECK_EQ(world.get<PlayerLocation>(body).viewport.x, 2400.0);
        CHECK_EQ(world.get<PlayerLocation>(body).viewport.y, 1400.0);
    }
    const auto first = world.get<PetalRing>(halves[0]).spin;
    const auto second = world.get<PetalRing>(halves[1]).spin;
    h.step(4, {&client});
    const auto firstChange = angleDelta(first, world.get<PetalRing>(halves[0]).spin);
    const auto secondChange = angleDelta(second, world.get<PetalRing>(halves[1]).spin);
    CHECK(std::abs(firstChange) > 1e-6);
    CHECK_NEAR(firstChange, secondChange, 1e-6);
}

TEST(splitter_spare_fires_blueberries_with_the_active_flower) {
 Harness h("splitter-blueberry",{},testsupport::dataDir(),0);CHECK(h.ready);if(!h.ready)return;
 NetClient alice;CHECK(loginNew(h,alice,"alice","hunter2!"));alice.joinGame(1280,720,{},"alice");
 CHECK(h.stepUntil({&alice},[&]{return alice.status()==NetClient::Status::Playing;}));
 CHECK(equipSplitter(h,alice,"alice",content().petalIndex(kSplitterPetalId)));
 CHECK(h.stepUntil({&alice},[&]{return bodiesNamed(h.server.world(),"alice").size()==2;}));
 const auto berry=content().petalIndex("magic_peas");alice.sendChat("/admin give alice magic_peas common 1");
 CHECK(h.stepUntil({&alice},[&]{return alice.profile().stackCount(berry,Rarity::Common)>0;}));
 alice.setLoadoutSlot(1,berry,Rarity::Common);
 const auto mana=content().petalIndex("mana_orb");alice.sendChat("/admin give alice mana_orb common 1");
 CHECK(h.stepUntil({&alice},[&]{return alice.profile().stackCount(mana,Rarity::Common)>0;}));
 alice.setLoadoutSlot(2,mana,Rarity::Common);h.step(100,{&alice});
 net::InputFrame frame;frame.sequence=100;frame.flags=net::InputDefend;frame.aimDistance=300;alice.sendInput(frame);
 std::vector<Entity> fired;auto& world=h.server.world();
 CHECK(h.stepUntil({&alice},[&]{Query<Projectile> q{world};q.each([&](Entity,Projectile& p){
  if(p.petalConfigIndex==berry&&std::find(fired.begin(),fired.end(),p.owner)==fired.end())fired.push_back(p.owner);
 });return fired.size()==2;},200));
}

TEST(splitter_parked_pickup_is_banked_once_and_bulk_blacklist_preserves_materials) {
    Harness h("splitter-pickup-bank", {}, flix::testsupport::dataDir(), 0);
    CHECK(h.ready); if (!h.ready) return;
    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));
    CHECK(equipSplitter(h, alice, "alice", content().petalIndex(kSplitterPetalId)));
    auto& world = h.server.world();
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));
    const auto halves = bodiesNamed(world, "alice");
    if (halves.size() != 2) return;
    const auto petal = content().petalIndex("rock");
    const auto initial = alice.profile().stackCount(petal, Rarity::Rare);
    Entity parked = NULL_ENTITY, active = NULL_ENTITY;
    for (auto body : halves) {
        world.get<Health>(body).invulnerableUntilMillis = 1e12;
        if (world.get<NetId>(body).value == alice.view().self().netId) active = body;
        else parked = body;
    }
    CHECK(parked != NULL_ENTITY); CHECK(active != NULL_ENTITY);
    if (parked == NULL_ENTITY || active == NULL_ENTITY) return;
    world.get<Transform>(parked).position += Vec2{10000,0};
    const auto at = world.get<Transform>(parked);
    const auto drop = world.create();
    world.add<DropTag>(drop);
    world.add<NetId>(drop, NetId{4000000000u});
    world.add<Transform>(drop, at);
    world.add<Body>(drop, Body{14.0, 1.0});
    world.add<Lifetime>(drop, Lifetime{60.0});
    DropItem item; item.configIndex = petal; item.rarity = Rarity::Rare; item.quantity = 7;
    item.eligible.push_back(LootClaim{active, world.get<PlayerAccount>(active).connection});
    world.add<DropItem>(drop, item);
    CHECK(h.stepUntil({&alice}, [&] { return alice.profile().stackCount(petal, Rarity::Rare) == initial + 7; }));
    CHECK(!world.isAlive(drop));
    bool pickupReported=false;
    for(const auto& event:alice.view().events()) {
        if(event.kind!=net::EventKind::PickedUp || event.netId!=4000000000u)continue;
        pickupReported=true; CHECK_EQ(event.pickupQuantity,7u); CHECK(event.pickupByAccount);
    }
    CHECK(pickupReported);
    h.step(10, {&alice});
    CHECK_EQ(alice.profile().stackCount(petal, Rarity::Rare), initial + 7);
    const auto leaf = content().petalIndex("leaf");
    alice.sendChat("/admin give alice leaf rare 30");
    CHECK(h.stepUntil({&alice}, [&] { return alice.profile().stackCount(leaf, Rarity::Rare) >= 30; }));
    const auto leavesBefore = alice.profile().stackCount(leaf, Rarity::Rare);
    alice.requestBulkCraft(Rarity::Epic, {petal});
    h.step(20, {&alice});
    CHECK(alice.profile().stackCount(leaf, Rarity::Rare) < leavesBefore);
    CHECK_EQ(alice.profile().stackCount(petal, Rarity::Rare), initial + 7);
}

TEST(death_summary_uses_credited_splitter_loot_and_ignores_visual_cues) {
    Harness h("splitter-death-loot", {}, flix::testsupport::dataDir(), 0);
    NetClient alice;
    if (!h.ready || !loginNew(h, alice, "alice", "hunter2!")) { CHECK(false); return; }
    alice.joinGame(1280, 720, {}, "alice");
    if (!h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }) ||
        !equipSplitter(h, alice, "alice", content().petalIndex(kSplitterPetalId))) {
        CHECK(false); return;
    }
    auto& world = h.server.world();
    auto* session = GameServerPeer::sessionOf(h.server, "alice");
    if (!session || !h.stepUntil({&alice}, [&] { return session->split(); })) { CHECK(false); return; }

    const auto active = session->entity;
    const auto parked = session->splitOther;
    const auto rock = content().petalIndex("rock");
    const auto at = world.get<Transform>(parked);
    auto& record = GameServerPeer::authorityDb(h.server).progress(session->userId);
    record.setItemCount(Rarity::Rare, "petal_rock", kMaxStackCount - 1);
    LootSystem fixtureLoot;
    const auto drop = fixtureLoot.spawnDrop(world, rock, Rarity::Rare, at.position, at.realm, {parked});
    world.add<NetId>(drop, NetId{4000000000u});
    world.get<DropItem>(drop).quantity = 7;
    if (!h.stepUntil({&alice}, [&] { return !world.isAlive(drop); })) { CHECK(false); return; }
    CHECK_EQ(record.itemCount(Rarity::Rare, "petal_rock"), kMaxStackCount);

    alice.requestRunLoot();
    CHECK(h.stepUntil({&alice}, [&] { return alice.runLootOverview().has_value(); }));
    CHECK(!alice.dead());
    if (const auto& overview = alice.runLootOverview(); overview && overview->size() == 1) {
        CHECK_EQ(overview->front().petalIndex, rock);
        CHECK_EQ(overview->front().rarity, Rarity::Rare);
        CHECK_EQ(overview->front().count, 1u);
    } else {
        CHECK(false);
    }

    // The legacy event tally included the entire ground stack even though only
    // one fit. Two plausible-width visual cues can also fabricate a 4.1B row
    // without any inventory credit. Deliver both through the real death frame.
    const auto activeId = world.get<NetId>(active).value;
    auto& events = GameServerPeer::snapshotEvents(h.server);
    for (std::uint32_t id : {4000000001u, 4000000002u}) {
        events.pickedUp(id, activeId, at.position, at.realm, rock, Rarity::PermanentHyper,
                        2050000000u, session->connection);
    }
    for (const auto body : {active, parked}) {
        world.get<Health>(body).current = 0.0;
        world.add<Dead>(body);
    }
    CHECK(h.stepUntil({&alice}, [&] { return alice.dead(); }));
    h.step(4, {&alice});

    std::uint64_t visualRock = 0, visualGold = 0;
    for (const auto& cue : alice.view().events()) {
        if (cue.kind != net::EventKind::PickedUp || !cue.pickupByAccount || cue.amount != rock) continue;
        if (cue.flag == rarityIndex(Rarity::Rare)) visualRock += cue.pickupQuantity;
        if (cue.flag == rarityIndex(Rarity::PermanentHyper)) visualGold += cue.pickupQuantity;
    }
    CHECK_EQ(visualRock, 7u);
    CHECK_EQ(visualGold, 4100000000ull);
    CHECK_EQ(record.itemCount(Rarity::PermanentHyper, "petal_rock"), 0);
    CHECK_EQ(alice.runLoot().size(), 1u);
    if (alice.runLoot().size() != 1u) return;
    CHECK_EQ(alice.runLoot().front().petalIndex, rock);
    CHECK_EQ(alice.runLoot().front().rarity, Rarity::Rare);
    CHECK_EQ(alice.runLoot().front().count, 1u);
}

TEST(stars_from_distinct_simultaneous_split_kills_all_reach_the_account) {
    Harness h("split-star-bounties",[](const std::string& path){
        Database db;
        std::string error;
        db.load(path,error);
        db.setPasswordCost(4);
        const auto user=db.createUser("splitstars","password7");
        if (!user.ok()) return;
        auto& record=db.progress(user.account->id);
        record.loadout.resize(kLoadoutSlots);
        record.loadout[0]=StoredItem{};
        record.loadout[0]->petalType=kSplitterPetalId;
        record.loadout[0]->rarity=Rarity::Unique;
        db.markDirty();
        db.save();
    }, flix::testsupport::dataDir(), 0);
    CHECK(h.ready);
    NetClient client;
    if (!h.ready || !connectClient(h,client)) {CHECK(false);return;}
    client.requestLogin("splitstars","password7");
    if (!h.stepUntil({&client},[&]{return client.status()==NetClient::Status::LoggedIn;})) {CHECK(false);return;}
    client.joinGame(1280,720,{},"splitstars");
    auto& world=h.server.world();
    const auto bodies=[&]{return Query<PlayerTag,PlayerAccount,PlayerProgress>{world}.collect();};
    if (!h.stepUntil({&client},[&]{return bodies().size()==2;})) {CHECK(false);return;}
    const auto halves=bodies();
    const double before=client.profile().stars;
    const auto species=content().mobIndex("ladybug");
    const auto make=[&](Entity player,Rarity tier,Vec2 pos,double hp=1.0){
        auto& at=world.get<Transform>(player);
        at.position=pos;
        world.get<ContactDamage>(player).amount=10000;
        const auto enemy=world.create();
        world.add<MobTag>(enemy);
        world.add<Bounty>(enemy);
        world.add<MobType>(enemy,MobType{species,tier});
        world.add<Transform>(enemy,Transform{pos,0,at.realm});
        world.add<Body>(enemy,Body{5,1});
        world.add<Health>(enemy,Health{hp,hp});
        world.add<Faction>(enemy,Faction{Team::Hostiles,false});
        return enemy;
    };
    const auto realm=world.get<Transform>(halves[0]).realm;
    const auto clearFrom=[&](double startX){
        for (double x=startX;x<startX+3500;x+=100) {
            for (double y=1500;y<6000;y+=100) {
                bool clear=true;
                for (const double dx : {-40.0,0.0,40.0})
                    for (const double dy : {-40.0,0.0,40.0})
                        clear &= !h.server.terrain().blocked({x+dx,y+dy},realm);
                if (clear) return Vec2{x,y};
            }
        }
        return Vec2{};
    };
    const auto pos1=clearFrom(1500);
    const auto pos2=clearFrom(5200);
    if (pos1.x==0 || pos2.x==0) {CHECK(false);return;}
    const auto e1=make(halves[0],Rarity::Mythic,pos1);
    const auto e2=make(halves[1],Rarity::Ultra,pos2);
    h.step(1,{&client});
    CHECK(!world.isAlive(e1) || world.has<Dead>(e1));
    CHECK(!world.isAlive(e2) || world.has<Dead>(e2));
    h.step(20,{&client});
    const auto& db=h.server.database();
    const auto* account=db.findUser("splitstars");
    const auto* stored=account?db.findProgress(account->id):nullptr;
    if (!stored) {CHECK(false);return;}
    const auto& kills=stored->mobKills["ladybug"];
    CHECK_EQ(kills["mythic"].asInt(),1);
    CHECK_EQ(kills["ultra"].asInt(),1);
    CHECK_NEAR(stored->stars,before+6,1e-9);
    CHECK_NEAR(client.profile().stars,before+6,1e-9);
    for (auto half:halves) CHECK_NEAR(world.get<PlayerProgress>(half).stars,before+6,1e-9);

    // Both halves contribute and the ledger combines their damage under one
    // connection. The server's finishing hit should still pay only once.
    const auto shared=make(halves[0],Rarity::Mythic,pos1,3);
    CombatSystem prehit;
    prehit.applyDamage(world,shared,halves[0],1,100000);
    prehit.applyDamage(world,shared,halves[1],1,100000);
    const auto& contributed=world.get<Bounty>(shared).contributors;
    CHECK_EQ(contributed.size(),std::size_t(1));
    if (!contributed.empty()) CHECK_NEAR(contributed.front().damage,2.0,1e-9);
    CHECK(!world.has<Dead>(shared));
    CHECK(h.stepUntil({&client},[&]{return !world.isAlive(shared) || world.has<Dead>(shared);},30));
    h.step(20,{&client});
    const auto* finished=db.findProgress(account->id);
    if (!finished) {CHECK(false);return;}
    CHECK_EQ(finished->mobKills["ladybug"]["mythic"].asInt(),2);
    CHECK_EQ(finished->mobKills["ladybug"]["ultra"].asInt(),1);
    CHECK_NEAR(finished->stars,before+7,1e-9);
    CHECK_NEAR(client.profile().stars,before+7,1e-9);
    for (auto half:halves) CHECK_NEAR(world.get<PlayerProgress>(half).stars,before+7,1e-9);
}

TEST(splitter_parked_kills_keep_loot_entitlement_for_the_active_body) {
    Harness h("splitter-kill-loot", {}, flix::testsupport::dataDir(), 0);
    CHECK(h.ready); if (!h.ready) return;
    NetClient alice; CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));
    CHECK(equipSplitter(h, alice, "alice", content().petalIndex(kSplitterPetalId)));
    auto& world = h.server.world();
    CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));
    Entity parked = NULL_ENTITY, active = NULL_ENTITY;
    for (auto body : bodiesNamed(world, "alice")) {
        world.get<Health>(body).invulnerableUntilMillis = 1e12;
        if (world.get<NetId>(body).value == alice.view().self().netId) active = body; else parked = body;
    }
    CHECK(parked != NULL_ENTITY); CHECK(active != NULL_ENTITY);
    if (parked == NULL_ENTITY || active == NULL_ENTITY) return;
    const auto at = world.get<Transform>(parked);
    CombatSystem combat;
    for (int i = 0; i < 40; ++i) {
        const Entity mob = world.create();
        world.add<MobTag>(mob);
        world.add<MobType>(mob, MobType{content().mobIndex(i % 2 ? "server" : "sandstorm"), Rarity::Apex});
        world.add<Transform>(mob, Transform{at.position + Vec2{300, 0}, 0, at.realm});
        world.add<Health>(mob, Health{100, 100});
        world.add<Faction>(mob, Faction{Team::Hostiles});
        world.add<Bounty>(mob);
        combat.applyDamage(world, mob, parked, 1000, 100000);
        CHECK(world.has<Dead>(mob));
        CHECK_EQ(world.get<Bounty>(mob).contributors.size(), std::size_t{1});
    }
    h.step(1, {&alice});
    int eligible = 0;
    Query<DropTag, DropItem> drops{world};
    drops.each([&](Entity, DropTag&, DropItem& d) {
        if (LootSystem::mayPickUp(d, active, world.get<PlayerAccount>(active).connection)) eligible += d.quantity;
    });
    CHECK(eligible > 0);
    // A shot and cloud already released must keep their account credit when
    // the spare dies before impact; otherwise their kills have no claimant.
    const auto shot = world.create();
    Projectile projectile; projectile.owner = parked; projectile.creditTo = parked;
    world.add<Projectile>(shot, projectile);
    const auto cloud = world.create();
    GroundEffect effect; effect.owner = parked;
    world.add<GroundEffect>(cloud, effect);
    world.get<Health>(parked).current = 0;
    world.add<Dead>(parked);
    h.step(2, {&alice});
    CHECK_EQ(CombatSystem::creditedPlayer(world, shot), active);
    CHECK_EQ(CombatSystem::creditedPlayer(world, cloud), active);
    for (int i = 0; i < 40; ++i) {
        const auto mob = world.create();
        world.add<MobTag>(mob);
        world.add<MobType>(mob, MobType{content().mobIndex(i % 2 ? "server" : "sandstorm"), Rarity::Apex});
        world.add<Transform>(mob, Transform{at.position + Vec2{300, 0}, 0, at.realm});
        world.add<Health>(mob, Health{100, 100});
        world.add<Faction>(mob, Faction{Team::Hostiles});
        world.add<Bounty>(mob);
        combat.applyDamage(world, mob, i % 2 ? shot : cloud, 1000, 100000);
        CHECK_EQ(world.get<Bounty>(mob).contributors.size(), std::size_t{1});
    }
    h.step(1, {&alice});
    int after = 0;
    drops.each([&](Entity, DropTag&, DropItem& d) {
        if (LootSystem::mayPickUp(d, active, world.get<PlayerAccount>(active).connection)) after += d.quantity;
    });
    CHECK(after > eligible);
}

TEST(ankh_break_relocates_only_its_living_owner_without_healing) {
    Harness h("ankh-break", {}, flix::testsupport::dataDir(), 0);
    CHECK(h.ready); if (!h.ready) return;
    NetClient alice; CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));
    auto& world = h.server.world();
    const auto body = soleBody(world, "alice");
    CHECK(body != NULL_ENTITY); if (body == NULL_ENTITY) return;
    const auto at = world.get<Transform>(body);
    const auto healthBefore = world.get<Health>(body).current;
    const auto makeAnkh = [&]() {
        auto petal = world.create();
        PetalInstance instance; instance.owner = body; instance.configIndex = content().petalIndex("ankh");
        world.add<PetalInstance>(petal, instance);
        world.add<Transform>(petal, at);
        world.add<Health>(petal, Health{0, 10});
        world.add<Dead>(petal);
        return petal;
    };
    const auto petal = makeAnkh();
    world.get<Transform>(body).position = at.position + Vec2{100, 0};
    h.step(1, {&alice});
    CHECK(!world.isAlive(petal));
    CHECK(distanceSq(world.get<Transform>(body).position, at.position) < 1.0);
    CHECK(world.get<Health>(body).current <= healthBefore + 1.0);
    makeAnkh();
    world.get<Health>(body).current = 0;
    world.add<Dead>(body);
    h.step(1, {&alice});
    CHECK(world.has<Dead>(body));
}

TEST(dice_real_equipment_server_ticks_deliver_critical_damage) {
    Harness h("dice-real",{},testsupport::dataDir(),0);CHECK(h.ready);if(!h.ready)return;
    NetClient alice;CHECK(loginNew(h,alice,"alice","hunter2!"));
    alice.joinGame(1280,720,{},"alice");
    CHECK(h.stepUntil({&alice},[&]{return alice.status()==NetClient::Status::Playing;}));
    CHECK(h.server.grantAdmin(alice.sessionToken()));
    const auto dice=content().petalIndex("dice");
    alice.sendChat("/admin give alice dice unique 1");
    CHECK(h.stepUntil({&alice},[&]{return alice.profile().stackCount(dice,Rarity::Unique)>0;}));
    alice.setLoadoutSlot(0,dice,Rarity::Unique);
    CHECK(h.stepUntil({&alice},[&]{return alice.profile().loadout[0].petalIndex==dice;}));
    auto& world=h.server.world();const auto owner=soleBody(world,"alice");CHECK(owner!=NULL_ENTITY);if(owner==NULL_ENTITY)return;
    world.get<Health>(owner).invulnerableUntilMillis=1e12;
    const auto victim=world.create();const auto at=world.get<Transform>(owner);
    world.add<NetId>(victim,NetId{4000000001u});world.add<MobTag>(victim);world.add<NpcTag>(victim);world.add<Transform>(victim,at);
    world.add<Body>(victim,Body{200,1});world.add<Health>(victim,Health{1e15,1e15});
    world.add<Faction>(victim,Faction{Team::Hostiles});world.add<Bounty>(victim);
    double maximum=0,maximumOnClient=0;int hits=0;bool criticalFlag=false;const double base=content().petalStats(dice,Rarity::Unique).damage;
    auto total=[&]{double sum=0;for(const auto& v:world.get<Bounty>(victim).contributors)sum+=v.damage;return sum;};
    // Keep the target in contact range even when ambient mobs push the flower.
    for(int i=0;i<2400;++i){
        world.get<Transform>(victim) = world.get<Transform>(owner);
        const auto before=total();h.step(1,{&alice});for(const auto& event:alice.view().events())if(event.kind==net::EventKind::Damage && event.netId==4000000001u){maximumOnClient=std::max(maximumOnClient,event.amount);if(event.amount>base*30)criticalFlag=criticalFlag||((event.flag&net::DamageCritical)!=0);}alice.view().events().clear();const double delta=total()-before;if(delta>0){++hits;maximum=std::max(maximum,delta);}}
    CHECK(hits>=100);CHECK(maximum>base*30);CHECK(maximumOnClient>base*30);CHECK(criticalFlag);
}

TEST(wave_splitter_switches_and_leaves_as_one_participant) {
    Harness h("wave-splitter", {}, flix::testsupport::dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient alice;
    CHECK(loginNew(h, alice, "alice", "hunter2!"));
    alice.joinGame(1280, 720, {}, "alice");
    if (!h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; })) {
        CHECK(false);
        return;
    }
    World& world = h.server.world();
    const Entity original = soleBody(world, "alice");
    if (original == NULL_ENTITY) { CHECK(false); return; }
    const auto origin = world.get<Transform>(original).realm;
    alice.sendChat("/wave start");
    if (!h.stepUntil({&alice}, [&] { return world.has<WavePlayer>(original); })) {
        CHECK(false);
        return;
    }
    CHECK(equipSplitter(h, alice, "alice", content().petalIndex(kSplitterPetalId)));
    if (!h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; })) {
        CHECK(false);
        return;
    }
    for (const Entity body : bodiesNamed(world, "alice")) {
        CHECK(world.has<WavePlayer>(body));
        world.get<Health>(body).invulnerableUntilMillis = 1e12;
    }
    auto* session = GameServerPeer::sessionOf(h.server, "alice");
    if (!session) { CHECK(false); return; }
    const auto spare = session->splitOther;
    CHECK(h.stepUntil({&alice}, [&] {
        return !world.get<Loadout>(original).slots[0].broken;
    }, 600));
    alice.usePetal(0);
    CHECK(h.stepUntil({&alice}, [&] { return session->entity == spare; }));
    CHECK(h.stepUntil({&alice}, [&] { return alice.view().self().wave.durationMillis > 0; }));
    alice.requestWaveRoomStatus();
    CHECK(h.stepUntil({&alice}, [&] { return alice.waveRoomStatus.occupied == 1; }));
    const double initialStars = world.get<PlayerProgress>(spare).stars;
    const auto connection = world.get<PlayerAccount>(original).connection;
    h.clock += 30000.0;
    CHECK(h.stepUntil({&alice}, [&] {
        if (world.get<PlayerProgress>(spare).stars > initialStars) return true;
        for (const Entity enemy : Query<WaveEnemy>{world}.collect()) {
            if (!isStanding(world, enemy)) continue;
            world.get<Bounty>(enemy).credit(original, world.get<Health>(enemy).max, connection);
            world.add<Dead>(enemy);
        }
        return false;
    }, 850));
    CHECK_NEAR(world.get<PlayerProgress>(spare).stars, initialStars + 7500.0, 1e-9);
    alice.sendChat("/wave leave");
    CHECK(h.stepUntil({&alice}, [&] { return alice.view().realm() == origin; }));
    CHECK(!world.has<WavePlayer>(spare));
    CHECK(h.stepUntil({&alice}, [&] { return alice.waveResultNotice().sequence > 0; }));
}


// A single network input triggers both equipped copies. The parked half
// must carry the burst into the next movement tick without receiving walk input.
TEST(splitter_bubbles_pop_and_move_both_halves) {
    for (const std::string bubble : {"bubble", "magic_bubble"}) {
        const std::string testName = "splitter-" + bubble;
        Harness h(testName.c_str(), {}, testsupport::dataDir(), 0);
        CHECK(h.ready);
        if (!h.ready) return;
        NetClient alice;
        CHECK(loginNew(h, alice, "alice", "hunter2!"));
        alice.joinGame(1280, 720, {}, "alice");
        CHECK(h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }));
        CHECK(equipSplitter(h, alice, "alice", content().petalIndex(kSplitterPetalId)));
        auto& world = h.server.world();
        CHECK(h.stepUntil({&alice}, [&] { return bodiesNamed(world, "alice").size() == 2; }));
        const auto halves = bodiesNamed(world, "alice");
        if (halves.size() != 2) return;
        const Entity parked = world.get<NetId>(halves[0]).value == alice.view().self().netId
            ? halves[1] : halves[0];
        for (const auto half : halves) world.get<Health>(half).invulnerableUntilMillis = 1e18;

        const auto wear = [&](int slot, const std::string& id) {
            const auto index = content().petalIndex(id);
            alice.sendChat("/admin give alice " + id + " common 1");
            if (!h.stepUntil({&alice}, [&] { return alice.profile().stackCount(index, Rarity::Common) > 0; }))
                return false;
            alice.setLoadoutSlot(slot, index, Rarity::Common);
            return h.stepUntil({&alice}, [&] {
                const auto& loadout = alice.profile().loadout;
                return loadout.size() > static_cast<std::size_t>(slot) && loadout[slot].petalIndex == index;
            });
        };
        if (bubble == "magic_bubble") CHECK(wear(2, "mana_orb"));
        CHECK(wear(1, bubble));
        const auto ready = [&](Entity body) {
            bool found = false;
            Query<PetalInstance>{world}.without<Dead>().each([&](Entity, PetalInstance& petal) {
                if (petal.owner == body && petal.slot == 1) found = true;
            });
            return found;
        };
        CHECK(h.stepUntil({&alice}, [&] { return ready(halves[0]) && ready(halves[1]); }, 250));

        net::InputFrame input;
        input.sequence = 100;
        input.flags = net::InputDefend;
        if (bubble == "magic_bubble") {
            alice.sendInput(input);
            h.step(4, {&alice});
            CHECK(ready(halves[0]) && ready(halves[1]));
            input.sequence++;
            input.moveStrength = 1.0;
        }
        alice.sendInput(input);
        CHECK(h.stepUntil({&alice}, [&] { return !ready(halves[0]) && !ready(halves[1]); }, 8));
        const auto origin = world.get<Transform>(parked).position;
        CHECK(world.get<Motion>(parked).velocity.length() > kPlayerMaxSpeed);
        CHECK_EQ(world.get<PlayerInput>(parked).current.moveStrength, 0.0);
        if (bubble == "magic_bubble") CHECK(world.get<Motion>(parked).velocity.x > kPlayerMaxSpeed);
        input.sequence++;
        input.flags = 0;
        input.moveStrength = 0.0;
        alice.sendInput(input);
        h.step(2, {&alice});
        CHECK(distance(world.get<Transform>(parked).position, origin) > 40.0);
    }
}

TEST(feedback3719_run_loot_orders_rarity_then_quantity) {
    Harness h("splitter-death-loot", {}, flix::testsupport::dataDir(), 0);
    NetClient alice;
    if (!h.ready || !loginNew(h, alice, "alice", "hunter2!")) { CHECK(false); return; }
    alice.joinGame(1280, 720, {}, "alice");
    if (!h.stepUntil({&alice}, [&] { return alice.status() == NetClient::Status::Playing; }) ||
        !equipSplitter(h, alice, "alice", content().petalIndex(kSplitterPetalId))) {
        CHECK(false); return;
    }
    auto& world = h.server.world();
    auto* session = GameServerPeer::sessionOf(h.server, "alice");
    if (!session || !h.stepUntil({&alice}, [&] { return session->split(); })) { CHECK(false); return; }

    const auto rock = content().petalIndex("rock");
    const auto rose = content().petalIndex("rose");
    session->runLoot[{rock, Rarity::Rare}] = 200;
    session->runLoot[{rose, Rarity::Unique}] = 1;
    session->runLoot[{rock, Rarity::Unique}] = 8;
    const auto bee = content().mobIndex("bee");
    const auto crab = content().mobIndex("crab");
    session->runKills[{bee, Rarity::Rare}] = 30;
    session->runKills[{crab, Rarity::Ascendant}] = 1;
    const Entity target = world.create();
    world.add<MobTag>(target);
    world.add<MobType>(target, MobType{crab, Rarity::Ascendant, 1.0});
    auto location = world.get<Transform>(session->entity);
    location.position.x += 50.0;
    world.add<Transform>(target, location);
    world.add<Health>(target, Health{100, 100, 0, 0});
    world.add<Body>(target, Body{20, 1});
    world.add<Faction>(target, Faction{Team::Hostiles, false});
    world.add<Bounty>(target);
    for (const Entity body : bodiesNamed(world, "alice"))
        world.get<Health>(body).invulnerableUntilMillis = 1e12;
    CHECK(GameServerPeer::practiceCombat(h.server).applyDamage(
        world, target, session->entity, 1000.0, h.clock).killed);
    const double beforeStars = world.get<PlayerProgress>(session->entity).stars;
    GameServerPeer::bankKills(h.server);
    CHECK_EQ(world.get<PlayerProgress>(session->entity).stars - beforeStars, 1000);
    CHECK_EQ((session->runKills[{crab, Rarity::Ascendant}]), 2u);
    world.destroy(target);
    alice.requestRunLoot();
    CHECK(h.stepUntil({&alice}, [&] {
        return alice.runLootOverview().has_value() && alice.runKillsOverview().has_value();
    }));
    const auto& kills = alice.runKillsOverview();
    CHECK(kills && kills->size() == 2);
    if (kills && kills->size() == 2) {
        CHECK_EQ((*kills)[0].mobIndex, crab);
        CHECK_EQ((*kills)[0].rarity, Rarity::Ascendant);
        CHECK_EQ((*kills)[0].count, 2u);
        CHECK_EQ((*kills)[1].mobIndex, bee);
        CHECK_EQ((*kills)[1].rarity, Rarity::Rare);
        CHECK_EQ((*kills)[1].count, 30u);
    }
    const auto& loot = alice.runLootOverview();
    CHECK(loot && loot->size() == 3);
    if (!loot || loot->size() != 3) return;
    CHECK_EQ((*loot)[0].rarity, Rarity::Unique);
    CHECK_EQ((*loot)[0].count, 8u);
    CHECK_EQ((*loot)[1].rarity, Rarity::Unique);
    CHECK_EQ((*loot)[1].count, 1u);
    CHECK_EQ((*loot)[2].rarity, Rarity::Rare);
    CHECK_EQ((*loot)[2].count, 200u);
}
