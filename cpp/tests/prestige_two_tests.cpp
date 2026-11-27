// Prestige II (#801, #803): the talent's second tier takes the flower to the
// prestige_2_ twin, the first tier to the prestige_ one, and the second tier
// is bought on top of the first.

#include "test.h"

#include "server_harness.h"
#include "server/db.h"
#include "shared/game/map_elements.h"
#include "shared/game/skills.h"
#include "test_data.h"

#include <cstdio>
#include <string>

using namespace flix;
using namespace flix::testsupport;

namespace {

/// `tier` 0 leaves the talent unbought; `level` is the XP the purchase is
/// paid from.
void seedPrestigeTier(const std::string& path, const std::string& username, int tier, int level) {
    Database db;
    std::string error;
    db.load(path, error);
    const Account* account = db.findUser(username);
    if (account == nullptr) return;
    PlayerRecord& record = db.progress(account->id);
    if (tier > 0) record.skills.set(SkillId::Prestige, tier - 1);
    double xp = 0.0;
    for (int l = 1; l < level; ++l) xp += xpForNextLevel(l);
    record.totalXp = xp;
    db.markDirty();
    db.save();
}

bool signIn(Harness& h, NetClient& client, const char* name) {
    if (!connectClient(h, client)) return false;
    client.requestLogin(name, "password7");
    return h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; });
}

bool enterPrestige(Harness& h, NetClient& client, const char* name) {
    client.joinGame(1920, 1080, {}, name, true);
    return h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }, 200);
}

int bodyRealm(World& world, const char* name) {
    const Entity e = bodyNamed(world, name);
    return e == NULL_ENTITY ? -1 : static_cast<int>(world.get<Transform>(e).realm);
}

} // namespace

TEST(prestige_two_joins_the_second_twin) {
    Harness h("prestige-two-join",
              [](const std::string& path) {
                  seedUser(path, "tierone", "password7");
                  seedUser(path, "tiertwo", "password7");
                  seedPrestigeTier(path, "tierone", 1, 1);
                  seedPrestigeTier(path, "tiertwo", 2, 1);
              },
              dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    World& world = h.server.world();
    const WorldMaps& maps = h.server.worldMaps();
    bool foundOne = false;
    bool foundTwo = false;
    const int one = static_cast<int>(maps.prestigeRealm(Realm::Overworld, foundOne, 1));
    const int two = static_cast<int>(maps.prestigeRealm(Realm::Overworld, foundTwo, 2));
    CHECK(foundOne);
    CHECK(foundTwo);
    CHECK(one != two);

    NetClient first;
    CHECK(signIn(h, first, "tierone"));
    CHECK(enterPrestige(h, first, "tierone"));
    NetClient second;
    CHECK(signIn(h, second, "tiertwo"));
    CHECK(enterPrestige(h, second, "tiertwo"));
    const int a = bodyRealm(world, "tierone");
    const int b = bodyRealm(world, "tiertwo");
    std::printf("  twins: tier one %d, tier two %d; joined: tier one %d, tier two %d\n", one, two, a, b);
    CHECK_EQ(a, one);
    CHECK_EQ(b, two);
    CHECK_EQ(static_cast<int>(second.view().realm()), two);
}

TEST(guild_roster_distinguishes_the_two_prestige_realms) {
    Harness h("guild-prestige", [](const std::string& path) {
        seedUser(path, "tierone", "password7");
        seedUser(path, "tiertwo", "password7");
        seedPrestigeTier(path, "tierone", 1, 1);
        seedPrestigeTier(path, "tiertwo", 2, 1);
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient first, second;
    CHECK(signIn(h, first, "tierone"));
    CHECK(signIn(h, second, "tiertwo"));
    first.requestGuildCreate("TEST");
    CHECK(h.stepUntil({&first, &second}, [&] { return first.guild().joined; }, 200));
    first.requestGuildInvite("tiertwo");
    CHECK(h.stepUntil({&first, &second}, [&] { return second.guildInvite().waiting; }, 200));
    second.requestGuildAccept();
    CHECK(h.stepUntil({&first, &second}, [&] { return second.guild().joined; }, 200));
    CHECK(enterPrestige(h, first, "tierone"));
    CHECK(enterPrestige(h, second, "tiertwo"));

    const auto rosterHas = [&](const char* name, const char* label) {
        for (const auto& member : first.guild().members) {
            if (member.online && member.name == name && member.location.rfind(label, 0) == 0) return true;
        }
        return false;
    };
    CHECK(h.stepUntil({&first, &second}, [&] {
        return rosterHas("tierone", "Prestige I ") && rosterHas("tiertwo", "Prestige II ");
    }, 200));
}

TEST(prestige_two_is_bought_on_top_of_the_first_tier) {
    Harness h("prestige-two-buy",
              [](const std::string& path) {
                  seedUser(path, "veteran", "password7");
                  seedPrestigeTier(path, "veteran", 1, 290);
                  seedUser(path, "almost", "password7");
                  seedPrestigeTier(path, "almost", 1, 289);
              },
              dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient almost;
    CHECK(signIn(h, almost, "almost"));
    almost.requestUpgradeSkill(SkillId::Prestige, 1);
    CHECK(h.stepUntil({&almost}, [&] { return sawText(almost, "Requires player level 290"); }));
    CHECK_EQ(almost.profile().skills.level(SkillId::Prestige), 0);

    NetClient veteran;
    CHECK(signIn(h, veteran, "veteran"));
    CHECK(awaitProfile(h, veteran, [](const Profile& p) { return p.skills.prestigeUnlocked(); }));
    std::printf("  before: level %d, points %d, prestige level %d\n", veteran.profile().level,
                veteran.profile().talentPoints(), veteran.profile().skills.level(SkillId::Prestige));
    const int pointsBefore = veteran.profile().talentPoints();
    veteran.requestUpgradeSkill(SkillId::Prestige, 1);
    const bool bought = awaitProfile(h, veteran, [](const Profile& p) {
        return p.skills.level(SkillId::Prestige) == 1;
    });
    std::printf("  after:  bought %d, points %d, prestige level %d\n", bought ? 1 : 0,
                veteran.profile().talentPoints(), veteran.profile().skills.level(SkillId::Prestige));
    CHECK(bought);
    CHECK_EQ(pointsBefore - veteran.profile().talentPoints(), 200);
    CHECK(enterPrestige(h, veteran, "veteran"));
    bool found = false;
    const int two = static_cast<int>(h.server.worldMaps().prestigeRealm(Realm::Overworld, found, 2));
    CHECK(found);
    CHECK_EQ(bodyRealm(h.server.world(), "veteran"), two);
}

TEST(wave_return_checks_prestige_entitlement_after_refunding_talents) {
    Harness h("prestige-two-wave-return", [](const std::string& path) {
        seedUser(path, "veteran", "password7");
        seedPrestigeTier(path, "veteran", 2, 240);
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient veteran;
    CHECK(signIn(h, veteran, "veteran"));
    CHECK(enterPrestige(h, veteran, "veteran"));
    auto& world = h.server.world();
    const auto entity = bodyNamed(world, "veteran");
    CHECK(entity != NULL_ENTITY);
    if (entity == NULL_ENTITY) return;
    const auto& maps = h.server.worldMaps();
    CHECK(maps.isPrestige(world.get<Transform>(entity).realm));
    veteran.sendChat("/wave start");
    CHECK(h.stepUntil({&veteran}, [&] { return world.has<WavePlayer>(entity); }, 150));
    veteran.requestResetSkills();
    CHECK(awaitProfile(h, veteran, [](const Profile& p) { return !p.skills.prestigeUnlocked(); }));
    veteran.sendChat("/wave leave");
    CHECK(h.stepUntil({&veteran}, [&] { return !world.has<WavePlayer>(entity); }, 150));
    CHECK_EQ(bodyRealm(world, "veteran"), static_cast<int>(Realm::Overworld));
}
