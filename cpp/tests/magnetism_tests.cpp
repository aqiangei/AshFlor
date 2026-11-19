#include "test.h"
#include "server_harness.h"

using namespace flix;
using flix::testsupport::awaitProfile;
using flix::testsupport::connectClient;
using flix::testsupport::Harness;

TEST(magnetism_tier_two_replaces_tier_one_and_requires_full_loadout) {
    SkillSet skills;
    skills.set(SkillId::Loadout, 3);
    CHECK(!skills.prerequisiteMet(SkillId::Magnetism));
    CHECK_NEAR(skills.magnetismBonus(), 0.0, 1e-12);
    skills.set(SkillId::Loadout, 4);
    CHECK(skills.prerequisiteMet(SkillId::Magnetism));
    CHECK_EQ(skillTierCount(SkillId::Magnetism), 2);

    const int loadout = skills.spent();
    CHECK_EQ(skillPurchaseCost(skills, SkillId::Magnetism, 1), 40);
    skills.set(SkillId::Magnetism, 0);
    CHECK_EQ(skills.spent() - loadout, 15);
    CHECK_NEAR(skills.magnetismBonus(), 1000.0, 1e-12);
    skills.set(SkillId::Magnetism, 1);
    CHECK_EQ(skills.spent() - loadout, 40);
    CHECK_NEAR(skills.magnetismBonus(), 1500.0, 1e-12);
    skillRefundTo(skills, SkillId::Magnetism, 0);
    CHECK_NEAR(skills.magnetismBonus(), 1000.0, 1e-12);
}

TEST(magnetism_tier_two_is_bought_and_reported_over_the_wire) {
    Harness h("talent-magnetism", [](const std::string& path) {
        Database db;
        std::string error;
        db.load(path, error);
        db.setPasswordCost(4);
        const auto user = db.createUser("magneto", "password7");
        CHECK(user.ok());
        if (!user.ok()) return;
        db.progress(user.account->id).totalXp = 1e15;
        db.markDirty();
        db.save();
    }, flix::testsupport::twoMapDataDir("talent-magnetism"), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(connectClient(h, client));
    client.requestLogin("magneto", "password7");
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    CHECK(awaitProfile(h, client, [](const Profile& p) { return p.level > 100; }));

    client.requestUpgradeSkill(SkillId::Magnetism, 0);
    h.step(20, {&client});
    CHECK_EQ(client.profile().skills.level(SkillId::Magnetism), -1);

    for (int tier = 0; tier < 5; ++tier) {
        client.requestUpgradeSkill(SkillId::Loadout, tier);
        CHECK(awaitProfile(h, client, [tier](const Profile& p) {
            return p.skills.level(SkillId::Loadout) == tier;
        }));
    }

    const int before = client.profile().talentPoints();
    client.requestUpgradeSkill(SkillId::Magnetism, 1);
    h.step(20, {&client});
    CHECK_EQ(client.profile().skills.level(SkillId::Magnetism), -1);
    for (int tier = 0; tier < 2; ++tier) {
        client.requestUpgradeSkill(SkillId::Magnetism, tier);
        CHECK(awaitProfile(h, client, [tier](const Profile& p) {
            return p.skills.level(SkillId::Magnetism) == tier;
        }));
    }
    CHECK_EQ(client.profile().talentPoints(), before - 15 - 25);
    client.requestDowngradeSkill(SkillId::Magnetism, 1);
    CHECK(awaitProfile(h, client, [](const Profile& p) {
        return p.skills.level(SkillId::Magnetism) == 0;
    }));
    CHECK_EQ(client.profile().talentPoints(), before - 15);
}
