#include "test.h"
#include "server/hyper_board.h"
#include "server_harness.h"

using namespace flix;
using namespace flix::testsupport;

TEST(apex_holdings_count_physical_petals_by_account) {
    Database db;
    db.setPasswordCost(4);
    const auto holder = db.createUser("holder", "password7");
    CHECK(holder.ok());
    if (!holder.ok()) return;
    const auto id = holder.account->id;
    auto& record = db.progress(id);
    record.inventory = Json::object();
    record.loadout.clear();
    record.addItem(Rarity::Ascendant, "petal_rock", 2);
    record.addItem(Rarity::Ascendant, "petal_leaf", 1);
    record.addItem(Rarity::PermanentHyper, "petal_rock", 15);
    record.addItem(Rarity::Ascendant, "other_item", 6);
    StoredItem worn;
    worn.type = "petal";
    worn.petalType = "rock";
    worn.rarity = Rarity::Ascendant;
    record.loadout.push_back(worn);
    record.loadoutPresets["saved"].push_back(worn);
    db.rawTable("hyperForges")["apex-hammer:some-titan"]["owner"] = id;
    const auto lower = db.createUser("lower", "password7");
    CHECK(lower.ok());
    if (!lower.ok()) return;
    db.progress(lower.account->id).addItem(Rarity::Apex, "petal_rock", 10);

    const auto rows = apexHoldingBoard(db);
    CHECK_EQ(rows.size(), std::size_t(1));
    if (rows.size() != 1) return;
    CHECK_EQ(rows[0].name, std::string("holder"));
    CHECK(rows[0].petal.empty());
    CHECK_EQ(rows[0].count, 4u);
    CHECK_EQ(rows[0].ownerTotal, 4u);
    CHECK_EQ(db.findProgress(id)->itemCount(Rarity::Ascendant, "petal_rock"), 2);
}

TEST(apex_holdings_roundtrip_pages_ranked_accounts) {
    Harness h("apex-holdings", [](const std::string& path) {
        Database db;
        std::string error;
        db.load(path, error);
        db.setPasswordCost(4);
        for (int i = 0; i < 105; ++i) {
            const auto name = "holder" + std::to_string(100 + i);
            const auto account = db.createUser(name, "password7");
            CHECK(account.ok());
            if (!account.ok()) return;
            auto& record = db.progress(account.account->id);
            record.addItem(Rarity::Ascendant, "petal_rock", i == 104 ? 5 : 1);
            record.addItem(Rarity::Ascendant, "petal_leaf", 1);
        }
        db.markDirty();
        CHECK(db.save());
    });
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient c;
    CHECK(connectClient(h, c));
    c.requestLogin("holder100", "password7");
    CHECK(h.stepUntil({&c}, [&] { return c.status() == NetClient::Status::LoggedIn; }));
    c.requestLeaderboard(false);
    CHECK(h.stepUntil({&c}, [&] { return !c.leaderboardPending(); }));
    CHECK_EQ(c.hyperHoldingsTotal(), 105u);
    CHECK_EQ(c.hyperBoard().size(), std::size_t(100));
    if (c.hyperBoard().size() != 100) return;
    CHECK_EQ(c.hyperBoard()[0].name, std::string("holder204"));
    CHECK_EQ(c.hyperBoard()[0].ownerTotal, 6u);
    CHECK_EQ(c.hyperBoard()[1].name, std::string("holder100"));
    CHECK(c.hyperBoard()[0].petal.empty());
    c.requestLeaderboard(false, UINT32_MAX);
    CHECK(h.stepUntil({&c}, [&] { return !c.leaderboardPending(); }));
    CHECK_EQ(c.hyperHoldingsOffset(), 100u);
    CHECK_EQ(c.hyperBoard().size(), std::size_t(5));
    if (c.hyperBoard().size() != 5) return;
    CHECK_EQ(c.hyperBoard()[0].name, std::string("holder199"));
    CHECK_EQ(c.hyperBoard().back().name, std::string("holder203"));
}
