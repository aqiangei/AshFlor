#include <set>
#include "test.h"
#include "server_harness.h"
#include "server/shop_lock.h"
using namespace flix;
using namespace flix::testsupport;

TEST(shop_purchases_and_legacy_locked_petals_can_be_crafted) {
    constexpr auto tier = Rarity::Mythic;
    Harness h("shop-protection", [](const std::string& path) {
        seedUser(path, "buyer", "password");
        seedStack(path, "buyer", "petal_rose", Rarity::Mythic, 4);
        seedStack(path, "buyer", "petal_rose", Rarity::Super, 20);
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        const auto* user = db.findUser("buyer");
        if (!user) return;
        auto& record = db.progress(user->id);
        record.stars = 1000000;
        record.extra["craftLocks"]["mythic"]["petal_rose"] = 4;
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient buyer;
    CHECK(loginAs(h, buyer, "buyer", "password"));
    const auto rose = content().petalIndex("rose");
    buyer.requestBuyPetal(rose, tier, net::kNoShopOffer, 6);
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.shopOutcome().pending; }));
    CHECK(buyer.shopOutcome().ok);
    CHECK_EQ(buyer.profile().stackCount(rose, tier), 10u);
    const auto request = buyer.prepareMythicUltraCraft({{rose, tier, 5}});
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.mythicUltraTicket(request).has_value(); }));
    CHECK(buyer.mythicUltraTicket(request).value_or(0) > 0u);

    buyer.requestCraft(rose, tier, 10);
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.craftOutcome().pending; }));
    CHECK(buyer.craftOutcome().attempts >= 2);
    const int remaining = buyer.craftOutcome().petalsReturned;
    CHECK_EQ(buyer.craftOutcome().consumed, static_cast<unsigned>(10 - remaining));
    CHECK(awaitProfile(h, buyer, [&](const Profile& profile) {
        return profile.stackCount(rose, tier) == static_cast<unsigned>(remaining);
    }));

    buyer.craftOutcome().pending = false;
    buyer.requestCraft(rose, Rarity::Super, 20);
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.craftOutcome().pending; }));
    CHECK_EQ(buyer.craftOutcome().attempts, 0u);
    CHECK_EQ(buyer.craftOutcome().consumed, 0u);
    h.server.persistAll();
    Database saved;
    std::string error;
    CHECK(saved.load(h.dbPath, error));
    const auto* user = saved.findUser("buyer");
    const auto* record = user ? saved.findProgress(user->id) : nullptr;
    CHECK(record != nullptr);
    if (record) {
        CHECK_EQ(record->itemCount(tier, "petal_rose"), remaining);
        CHECK_EQ(record->itemCount(Rarity::Super, "petal_rose"), 20);
        CHECK_EQ(record->itemCount(Rarity::Unique, "petal_rose"), 0);
        CHECK(!record->extra.contains("craftLocks"));
    }
}

TEST(shop_stock_locked_moon_rock_uses_the_new_discounted_price) {
    const auto rotation = shopRotation(shopClockNow()) - 1;
    Harness h("shop-moon-rock", [&](const std::string& path) {
        seedUser(path, "buyer", "password");
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        auto& record = db.progress(db.findUser("buyer")->id);
        record.stars = 600000;
        record.extra["lockedShopRotation"] = static_cast<double>(rotation);
        record.extra["lockedShopOffersRotation"] = static_cast<double>(rotation);
        Json row = Json::object();
        row["petalType"] = "moon_rock";
        row["rarity"] = rarityName(Rarity::Super);
        row["discountPercent"] = 30;
        row["price"] = 3982;  // An old cached seven-tenths price.
        record.extra["lockedShopOffers"] = Json::array();
        record.extra["lockedShopOffers"].push(row);
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient buyer;
    CHECK(loginAs(h, buyer, "buyer", "password"));
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.lockedShopOffers.size() == kShopOfferCount; }));
    if (buyer.lockedShopOffers.size() != kShopOfferCount) return;
    const auto moon = content().petalIndex("moon_rock");
    CHECK_EQ(buyer.lockedShopOffers.front().petalIndex, moon);
    CHECK_NEAR(buyer.lockedShopOffers.front().price, 31500.0, 1e-9);
    const double starsBefore = buyer.profile().stars;
    buyer.requestBuyPetal(moon, Rarity::Super, 0, 1);
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.shopOutcome().pending; }));
    CHECK(buyer.shopOutcome().ok);
    CHECK_EQ(buyer.profile().stackCount(moon, Rarity::Super), 1);
    CHECK_NEAR(buyer.profile().stars, starsBefore - 31500.0, 1e-9);
    buyer.shopOutcome().pending = false;
    buyer.requestBuyPetal(moon, Rarity::Super, 0, 1);
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.shopOutcome().pending; }));
    CHECK(buyer.shopOutcome().ok);
    CHECK_EQ(buyer.profile().stackCount(moon, Rarity::Super), 2);
    CHECK_NEAR(buyer.profile().stars, starsBefore - 2 * 31500.0, 1e-9);
}

TEST(shop_stock_unlimited_locked_offers_ignore_old_sold_out_records_and_keep_prices) {
    const auto rotation=shopRotation(shopClockNow())-1;
    Harness h("shop-unlimited-lock",[&](const std::string& path){
        seedUser(path,"buyer","password");
        Database db;std::string error;CHECK(db.load(path,error));
        auto& r=db.progress(db.findUser("buyer")->id);r.stars=1000000000;
        r.extra["lockedShopRotation"]=static_cast<double>(rotation);
        r.extra["lockedShopOffersRotation"]=static_cast<double>(rotation);
        Json offers=Json::array();
        for(const auto rarity:{Rarity::Super,Rarity::Ultra}){
            Json row=Json::object();row["petalType"]="basic";
            row["rarity"]=rarityName(rarity);row["discountPercent"]=10;offers.push(row);
            for(const auto at:{rotation,rotation+1})
                r.extra["shopPurchases"][std::to_string(at)][std::string("basic@")+rarityName(rarity)]=999;
        }
        r.extra["lockedShopOffers"]=offers;db.markDirty();CHECK(db.save());
    },dataDir(),0);
    CHECK(h.ready);if(!h.ready)return;
    NetClient buyer;CHECK(loginAs(h,buyer,"buyer","password"));
    CHECK(h.stepUntil({&buyer},[&]{return buyer.lockedShopOffers.size()==kShopOfferCount;}));
    if(buyer.lockedShopOffers.size()!=kShopOfferCount)return;
    const auto basic=content().petalIndex("basic");
    auto buy=[&](Rarity rarity,int slot,int quantity,bool expected){
        buyer.shopOutcome().pending=false;
        buyer.requestBuyPetal(basic,rarity,slot,quantity);
        CHECK(h.stepUntil({&buyer},[&]{return buyer.shopOutcome().pending;}));
        CHECK_EQ(buyer.shopOutcome().ok,expected);
    };
    const auto& savedSuper = buyer.lockedShopOffers.front();
    CHECK_EQ(savedSuper.petalIndex, basic);
    CHECK_EQ(savedSuper.remaining, -1);
    const double price = shopOfferPrice(basic, Rarity::Super, 10);
    CHECK_NEAR(savedSuper.price, price, 1e-9);
    const double startingStars = buyer.profile().stars;
    buy(Rarity::Super, 0, 6, true);
    buy(Rarity::Super, 0, 6, true);
    const double expectedStars = startingStars - 12 * price;
    CHECK_EQ(buyer.profile().stackCount(basic, Rarity::Super), 12);
    CHECK_EQ(buyer.lockedShopOffers.front().remaining, -1);
    buy(Rarity::Super, net::kNoShopOffer, 1, false);
    buy(Rarity::Super, kShopOfferCount, 1, false);
    buy(Rarity::Super, 0, 10001, false);
    buy(Rarity::Ultra, 1, 1, false); // Previously present at this slot.
    buy(Rarity::Ultra, net::kNoShopOffer, 1, false);
    CHECK_EQ(buyer.profile().stackCount(basic, Rarity::Ultra), 0);
    CHECK_NEAR(buyer.profile().stars, expectedStars, 1e-9);
    for (int i = 0; i < kShopTierOfferCount; ++i) {
        CHECK(buyer.lockedShopOffers[i].rarity == Rarity::Super);
    }
    buyer.disconnect();h.step(3,{});
    NetClient returning;CHECK(loginAs(h,returning,"buyer","password"));
    CHECK(h.stepUntil({&returning},[&]{return returning.lockedShopOffers.size()==kShopOfferCount;}));
    if(returning.lockedShopOffers.size()!=kShopOfferCount)return;
    CHECK_EQ(returning.lockedShopOffers[0].remaining,-1);
    CHECK_EQ(returning.profile().stackCount(basic,Rarity::Super),12);
    CHECK_NEAR(returning.profile().stars,expectedStars,1e-9);
    h.server.persistAll();
    Database saved;std::string error;CHECK(saved.load(h.dbPath,error));
    const auto* user=saved.findUser("buyer");CHECK(user!=nullptr);if(!user)return;
    const auto* record=saved.findProgress(user->id);CHECK(record!=nullptr);if(!record)return;
    CHECK_EQ(record->extra["shopPurchases"][std::to_string(rotation)][std::string("basic@")+rarityName(Rarity::Super)].asInt(),999);
}

TEST(shop_stock_current_offers_unlimited_but_still_require_stars) {
    Harness h("shop-unlimited-current",[](const std::string& path){
        seedUser(path,"buyer","password");
        seedUser(path,"poor","password");
        Database db;std::string error;CHECK(db.load(path,error));
        db.progress(db.findUser("buyer")->id).stars=1000000000;
        db.markDirty();CHECK(db.save());
    },dataDir(),0);
    CHECK(h.ready);if(!h.ready)return;
    NetClient buyer;CHECK(loginAs(h,buyer,"buyer","password"));
    CHECK(h.stepUntil({&buyer},[&]{return !buyer.currentShopOffers.empty();}));
    if(buyer.currentShopOffers.empty())return;
    for(int i = 0; i < kShopTierOfferCount; ++i) CHECK_EQ(buyer.currentShopOffers[i].remaining,-1);
    const auto slot=kShopTierOfferCount-1;
    const auto offer=buyer.currentShopOffers[slot];
    const auto before=buyer.profile().stackCount(offer.petalIndex,offer.rarity);
    buyer.requestBuyPetal(offer.petalIndex,offer.rarity,slot,6);
    CHECK(h.stepUntil({&buyer},[&]{return buyer.shopOutcome().pending;}));
    CHECK(buyer.shopOutcome().ok);
    CHECK_EQ(buyer.profile().stackCount(offer.petalIndex,offer.rarity),before+6);
    NetClient poor;CHECK(loginAs(h,poor,"poor","password"));
    const auto poorStars=poor.profile().stars;
    const auto unaffordable = static_cast<std::uint32_t>(std::floor(poorStars / offer.price)) + 1;
    const auto poorBefore=poor.profile().stackCount(offer.petalIndex,offer.rarity);
    poor.requestBuyPetal(offer.petalIndex,offer.rarity,slot,unaffordable);
    CHECK(h.stepUntil({&poor},[&]{return poor.shopOutcome().pending;}));
    CHECK(!poor.shopOutcome().ok);
    CHECK_EQ(poor.profile().stars,poorStars);
    CHECK_EQ(poor.profile().stackCount(offer.petalIndex,offer.rarity),poorBefore);
    CHECK_EQ(buyer.profile().stackCount(offer.petalIndex,offer.rarity),before+6);
}

TEST(shop_stock_regular_rotation_has_five_distinct_offers_of_each_tier) {
    Harness h("shop-stock-tiers");
    CHECK(h.ready);
    CHECK(!shopSellsRarity(Rarity::Ultra));
    const auto fission = content().petalIndex("fission");
    const auto fusion = content().petalIndex("arctic_fusion");
    CHECK(shopSellsPetal(fission));
    CHECK(shopSellsPetal(fusion));
    bool sawFission = false;
    bool sawFusion = false;
    for (std::uint64_t rotation = 100; rotation < 1000; ++rotation) {
        const auto offers = shopOffers(rotation);
        CHECK_EQ(offers.size(), std::size_t(kShopOfferCount));
        std::set<std::uint16_t> petals;
        for (std::size_t i = 0; i < offers.size(); ++i) {
            const auto& offer = offers[i];
            CHECK(offer.rarity == (i < kShopTierOfferCount ? Rarity::Super : Rarity::Unique));
            CHECK_EQ(offer.price, shopOfferPrice(offer.petalIndex, offer.rarity, offer.discountPercent));
            CHECK(shopSellsPetal(offer.petalIndex));
            CHECK(petals.insert(offer.petalIndex).second);
            if (offer.petalIndex == fission) {
                CHECK(offer.rarity == Rarity::Unique);
                CHECK_EQ(offer.price, shopPrice(content().petalIndex("mimic"), Rarity::Unique));
                sawFission = true;
            }
            if (offer.petalIndex == fusion) sawFusion = true;
        }
    }
    CHECK(sawFission);
    CHECK(sawFusion);
}

TEST(eternal_shop_charges_exact_quote_for_multiple_copies_across_cards_and_relogin) {
    Harness h("eternal-shop", [](const std::string& path) {
        seedUser(path, "buyer", "password");
        seedUser(path, "poor", "password");
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        db.progress(db.findUser("buyer")->id).stars = 100'000'000;
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }
    NetClient buyer;
    CHECK(loginAs(h, buyer, "buyer", "password"));
    if (!h.stepUntil({&buyer}, [&] { return buyer.currentShopOffers.size() == kShopOfferCount; })) {
        CHECK(false);
        return;
    }
    const auto initial = buyer.profile().stars;
    const int slot = kShopTierOfferCount;
    const auto first = buyer.currentShopOffers[slot];
    const auto second = buyer.currentShopOffers[slot + 1];
    auto buy = [&](NetClient& client, const ShopOffer& offer, int index, int quantity, bool ok) {
        client.shopOutcome().pending = false;
        client.requestBuyPetal(offer.petalIndex, offer.rarity, index, quantity);
        CHECK(h.stepUntil({&client}, [&] { return client.shopOutcome().pending; }));
        CHECK_EQ(client.shopOutcome().ok, ok);
    };
    NetClient poor;
    CHECK(loginAs(h, poor, "poor", "password"));
    buy(poor, first, slot, 1, false);
    CHECK_EQ(poor.profile().stackCount(first.petalIndex, Rarity::Unique), 0);
    buy(buyer, first, net::kNoShopOffer, 1, false);
    buy(buyer, first, slot, 2, true);
    CHECK_EQ(buyer.profile().stackCount(first.petalIndex, Rarity::Unique), 2);
    CHECK_NEAR(buyer.profile().stars, initial - 2 * first.price, 1e-9);
    buy(buyer, second, slot + 1, 1, true);
    buyer.toggleShopLock();
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.lockedShopOffers.size() == kShopOfferCount; }));
    buy(buyer, second, slot + 1, 1, true);
    buyer.disconnect();
    h.step(3, {});
    NetClient returning;
    CHECK(loginAs(h, returning, "buyer", "password"));
    CHECK(h.stepUntil({&returning}, [&] { return returning.lockedShopOffers.size() == kShopOfferCount; }));
    buy(returning, first, slot, 1, true);
    CHECK_EQ(returning.profile().stackCount(first.petalIndex, Rarity::Unique), 3);
    CHECK_EQ(returning.profile().stackCount(second.petalIndex, Rarity::Unique), 2);
    CHECK_NEAR(returning.profile().stars, initial - 3 * first.price - 2 * second.price, 1e-9);
    h.server.persistAll();
    Database saved;
    std::string error;
    CHECK(saved.load(h.dbPath, error));
    const auto* account = saved.findUser("buyer");
    if (!account) { CHECK(false); return; }
    const auto* record = saved.findProgress(account->id);
    if (!record) { CHECK(false); return; }
    CHECK_EQ(record->itemCount(Rarity::Unique, "petal_" + content().petal(first.petalIndex).id), 3);
}

TEST(shop_stock_prices_preserve_mimic_and_anchor_new_items_by_rarity) {
    Harness h("shop-stock-economy", {}, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    const auto mimic = content().petalIndex("mimic");
    const auto nuke = content().petalIndex("arctic_nuke");
    const auto egg = content().petalIndex("arctic_nuke_egg");
    CHECK_EQ(shopPrice(mimic, Rarity::Super), legacyShopPrice(mimic, Rarity::Super));
    CHECK_EQ(shopPrice(mimic, Rarity::Super), 45'000);
    CHECK_EQ(shopPrice(nuke, Rarity::Super), 36'000);
    CHECK_EQ(shopPrice(egg, Rarity::Super), shopPrice(mimic, Rarity::Super));
    CHECK_EQ(shopOfferPrice(nuke, Rarity::Super, 30), 25'200);
    CHECK_EQ(shopPrice(nuke, Rarity::Unique), 540'000);
    for (const auto petal : content().petalDisplayOrder()) {
        if (!shopSellsPetal(petal)) continue;
        const auto super = shopPrice(petal, Rarity::Super);
        CHECK_EQ(shopPrice(petal, Rarity::Unique), 15 * super);
        CHECK_EQ(shopOfferPrice(petal, Rarity::Unique, 30), 15 * super);
        if (content().petal(petal).id.rfind("arctic_", 0) != 0 &&
            content().petal(petal).id != "magic_golden_leaf" && content().petal(petal).id != "fission")
            CHECK_EQ(super, legacyShopPrice(petal, Rarity::Super));
    }
}

namespace {
std::vector<std::uint16_t> eternalRow(std::int64_t rotation) {
    std::vector<std::uint16_t> petals;
    for (const auto& offer : shopOffers(rotation))
        if (offer.rarity == Rarity::Unique) petals.push_back(offer.petalIndex);
    return petals;
}
} // namespace

TEST(shop_lock_holds_the_eternal_row_of_its_hour) {
    std::string contentError;
    CHECK(loadContent(dataDir(), contentError));
    const auto now = shopRotation(shopClockNow());
    // An earlier hour whose Eternal row is not this hour's: a lock that still
    // followed the clock would show the wrong cards.
    std::int64_t lockedAt = now - 1;
    while (lockedAt > now - 50 && eternalRow(lockedAt) == eternalRow(now)) --lockedAt;
    CHECK(eternalRow(lockedAt) != eternalRow(now));
    PlayerRecord record;
    record.extra["lockedShopRotation"] = static_cast<double>(lockedAt);
    captureShopLock(record, lockedAt);
    const auto expected = shopOffers(lockedAt);
    const auto locked = lockedShopOffers(record);
    CHECK_EQ(expected.size(), std::size_t(kShopOfferCount));
    CHECK_EQ(locked.size(), expected.size());
    for (std::size_t i = 0; i < locked.size() && i < expected.size(); ++i) {
        CHECK_EQ(locked[i].petalIndex, expected[i].petalIndex);
        CHECK(locked[i].rarity == expected[i].rarity);
        CHECK_NEAR(locked[i].price, expected[i].price, 1e-9);
    }
}

TEST(shop_lock_saved_without_eternals_gets_the_eternal_row_of_its_hour_written_down) {
    std::string contentError;
    CHECK(loadContent(dataDir(), contentError));
    const auto lockedAt = shopRotation(shopClockNow()) - 9;
    PlayerRecord record;
    CHECK(!saveMissingLockedEternals(record));  // not locked: nothing to write
    record.extra["lockedShopRotation"] = static_cast<double>(lockedAt);
    captureShopLock(record, lockedAt);
    // What a lock taken before this change looks like on disk: the Super row only.
    Json superOnly = Json::array();
    for (const auto& row : record.extra["lockedShopOffers"].items())
        if (row["rarity"].asString() == rarityName(Rarity::Super)) superOnly.push(row);
    CHECK_EQ(superOnly.items().size(), std::size_t(kShopTierOfferCount));
    record.extra["lockedShopOffers"] = superOnly;
    const auto before = lockedShopOffers(record);
    CHECK_EQ(before.size(), std::size_t(kShopOfferCount));
    CHECK(eternalRow(lockedAt).size() == std::size_t(kShopTierOfferCount));
    for (std::size_t i = kShopTierOfferCount; i < before.size(); ++i)
        CHECK_EQ(before[i].petalIndex, eternalRow(lockedAt)[i - kShopTierOfferCount]);

    CHECK(saveMissingLockedEternals(record));
    CHECK(!saveMissingLockedEternals(record));  // once is enough
    CHECK_EQ(record.extra["lockedShopOffers"].items().size(), std::size_t(kShopOfferCount));
    const auto after = lockedShopOffers(record);
    CHECK_EQ(after.size(), before.size());
    for (std::size_t i = 0; i < after.size() && i < before.size(); ++i) {
        CHECK_EQ(after[i].petalIndex, before[i].petalIndex);
        CHECK(after[i].rarity == before[i].rarity);
    }

    // From here on the saved row decides, not a fresh roll of that hour.
    std::uint16_t other = kInvalidIndex;
    for (const std::uint16_t petal : content().petalDisplayOrder()) {
        const bool onShelf = std::any_of(after.begin(), after.end(),
                                         [&](const ShopOffer& offer) { return offer.petalIndex == petal; });
        if (!onShelf && shopSellsPetal(petal) && petalComesAt(content().petal(petal), Rarity::Unique)) {
            other = petal;
            break;
        }
    }
    CHECK(other != kInvalidIndex);
    if (other == kInvalidIndex) return;
    record.extra["lockedShopOffers"].items()[kShopTierOfferCount]["petalType"] = content().petal(other).id;
    const auto swapped = lockedShopOffers(record);
    CHECK_EQ(swapped.size(), std::size_t(kShopOfferCount));
    if (swapped.size() != std::size_t(kShopOfferCount)) return;
    CHECK_EQ(swapped[kShopTierOfferCount].petalIndex, other);
    CHECK(swapped[kShopTierOfferCount].rarity == Rarity::Unique);
    CHECK_EQ(swapped[kShopTierOfferCount].discountPercent, 0);
    CHECK_NEAR(swapped[kShopTierOfferCount].price, shopPrice(other, Rarity::Unique), 1e-9);
}

TEST(shop_lock_from_an_earlier_hour_sells_its_own_eternal_cards_until_unlocked) {
    std::string contentError;
    CHECK(loadContent(dataDir(), contentError));
    const auto now = shopRotation(shopClockNow());
    const auto onShelf = [](std::int64_t rotation, std::uint16_t petal) {
        const auto offers = shopOffers(rotation);
        return std::any_of(offers.begin(), offers.end(),
                           [&](const ShopOffer& offer) { return offer.petalIndex == petal; });
    };
    // An earlier hour whose first Eternal card is on neither this hour's shelf
    // nor the next one's, in case the hour turns while the test runs.
    std::int64_t lockedAt = now - 1;
    const auto firstEternal = [](std::int64_t rotation) { return eternalRow(rotation).front(); };
    CHECK(!eternalRow(lockedAt).empty());
    if (eternalRow(lockedAt).empty()) return;
    while (lockedAt > now - 50 &&
           (onShelf(now, firstEternal(lockedAt)) || onShelf(now + 1, firstEternal(lockedAt)))) --lockedAt;
    const auto lockedOffers = shopOffers(lockedAt);
    CHECK_EQ(lockedOffers.size(), std::size_t(kShopOfferCount));
    if (lockedOffers.size() != std::size_t(kShopOfferCount)) return;
    const int slot = kShopTierOfferCount;
    const ShopOffer eternal = lockedOffers[slot];
    CHECK(eternal.rarity == Rarity::Unique);
    CHECK(!onShelf(now, eternal.petalIndex));

    Harness h("shop-lock-eternal-buy", [&](const std::string& path) {
        seedUser(path, "buyer", "password");
        Database db;
        std::string error;
        CHECK(db.load(path, error));
        auto& record = db.progress(db.findUser("buyer")->id);
        record.stars = 100'000'000;
        record.extra["lockedShopRotation"] = static_cast<double>(lockedAt);
        captureShopLock(record, lockedAt);
        db.markDirty();
        CHECK(db.save());
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient buyer;
    CHECK(loginAs(h, buyer, "buyer", "password"));
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.lockedShopOffers.size() == kShopOfferCount; }));
    if (buyer.lockedShopOffers.size() != kShopOfferCount) return;
    CHECK_EQ(buyer.lockedShopRotation, lockedAt);
    for (std::size_t i = 0; i < lockedOffers.size(); ++i) {
        CHECK_EQ(buyer.lockedShopOffers[i].petalIndex, lockedOffers[i].petalIndex);
        CHECK(buyer.lockedShopOffers[i].rarity == lockedOffers[i].rarity);
    }
    auto buy = [&](const ShopOffer& offer, bool ok) {
        buyer.shopOutcome().pending = false;
        buyer.requestBuyPetal(offer.petalIndex, offer.rarity, slot, 1);
        CHECK(h.stepUntil({&buyer}, [&] { return buyer.shopOutcome().pending; }));
        CHECK_EQ(buyer.shopOutcome().ok, ok);
    };
    const double stars = buyer.profile().stars;
    const double price = shopPrice(eternal.petalIndex, Rarity::Unique);
    buy(eternal, true);
    CHECK_EQ(buyer.profile().stackCount(eternal.petalIndex, Rarity::Unique), 1);
    CHECK_NEAR(buyer.profile().stars, stars - price, 1e-9);

    // This hour's Eternal card in the same slot is not for sale to a locked shelf.
    const ShopOffer current = shopOffers(shopRotation(shopClockNow()))[slot];
    CHECK(current.petalIndex != eternal.petalIndex);
    buy(current, false);
    CHECK_EQ(buyer.profile().stackCount(current.petalIndex, Rarity::Unique), 0);
    CHECK_NEAR(buyer.profile().stars, stars - price, 1e-9);

    // Unlocking hands the shelf back to the clock: the old card is gone.
    buyer.toggleShopLock();
    CHECK(h.stepUntil({&buyer}, [&] { return buyer.lockedShopRotation == -1; }));
    CHECK(buyer.lockedShopOffers.empty());
    buy(eternal, false);
    CHECK_EQ(buyer.profile().stackCount(eternal.petalIndex, Rarity::Unique), 1);
    CHECK_NEAR(buyer.profile().stars, stars - price, 1e-9);
}

TEST(apex_kill_pays_the_normal_top_tier_star_bounty) {
    Harness h("apex-star-credit");
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(loginNew(h, client, "slayer", "password7"));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    // Reaching into the world is the only way to make a kill deterministic:
    // waiting for a real fight to resolve is a test that fails on a slow
    // machine. Everything from the damage onward is the shipping path. The
    // usual bots are running, so the flower is found by its account rather
    // than as whichever one a query visits last.
    World& world = h.server.world();
    const Entity player = onlyPlayer(world);
    const double starsBefore = client.profile().stars;
    CHECK(player != NULL_ENTITY);
    if (player == NULL_ENTITY) return;

    // Mobs arrive on the spawner's own schedule, and the starter petals need a
    // tick or two to reach the ring, so wait for both -- and take a mob IN THE
    // PLAYER'S REALM. Every map with spawn bands stocks its own, and one from
    // another map cannot be parked on this flower: the park below moves a
    // position, and the same position in another coordinate space is not the
    // same place, so the two never touch and the kill never happens.
    const Realm realm = world.get<Transform>(player).realm;
    Entity mob = NULL_ENTITY;
    Query<MobTag, MobType, Transform> mobs{world};
    Query<PetalInstance, Transform> petals{world};
    const bool armed = h.stepUntil({&client}, [&] {
        mob = NULL_ENTITY;
        mobs.each([&](Entity e, MobTag&, MobType&, Transform& at) {
            if (mob == NULL_ENTITY && at.realm == realm) mob = e;
        });
        bool anyPetal = false;
        petals.each([&](Entity, PetalInstance&, Transform&) { anyPetal = true; });
        return mob != NULL_ENTITY && anyPetal;
    }, 600);
    CHECK(armed);
    if (!armed) return;

    const std::uint16_t mobIndex = world.get<MobType>(mob).configIndex;
    // Apex needs the same loot eligibility as its normal star bounty.
    world.get<MobType>(mob).rarity = Rarity::Ascendant;
    // And unarmoured, because whatever this map happened to stock may be one
    // whose armour a starting ring cannot get through -- an ultra bee wears
    // 729, and the stock basic petal swings 10. This test is about the kill
    // ledger; armour has its own tests.
    if (world.has<Armor>(mob)) world.get<Armor>(mob).amount = 0.0;

    const bool credited = h.stepUntil({&client}, [&] {
        if (client.profile().killCount(mobIndex, Rarity::Ascendant) > 0) return true;
        if (!world.isAlive(mob) || world.has<Dead>(mob)) return false;
        // Park the mob on the flower, one hit from death. The TypeScript
        // player pipeline resolves this body contact before moveEnemies(), so
        // this is deterministic without predicting next tick's petal angle.
        // The MAX comes down with it: a kill is only credited to a flower
        // that did 1% of the mob's health, and one body hit on whatever this
        // map stocked need not be 1% of its spawned pool.
        world.get<Transform>(mob).position = world.get<Transform>(player).position;
        world.get<Health>(mob).max = 1.0;
        world.get<Health>(mob).current = 1.0;
        world.get<Health>(mob).invulnerableUntilMillis = 0;
        world.get<Health>(player).current = world.get<Health>(player).max;
        return false;
    }, 200);
    CHECK(credited);
    if (!credited) return;
    CHECK_EQ(client.profile().stars, starsBefore + 8000.0);
}

TEST(tier_shop_locks_keep_the_other_tier_refreshing_and_preserve_quotes) {
    std::string error;CHECK(loadContent(dataDir(),error));
    for(int bit:{1,2}) {
        PlayerRecord r;
        toggleShopTierLock(r,bit,100);
        CHECK_EQ(shopTierLockMask(r),bit);
        const auto before=tierLockedShopOffers(r,100);
        const auto after=tierLockedShopOffers(r,103);
        const auto fresh=shopOffers(103);
        CHECK_EQ(before.size(),after.size());
        for(std::size_t i=0;i<after.size();++i){
            const bool locked=after[i].rarity==(bit==1?Rarity::Super:Rarity::Unique);
            CHECK_EQ(after[i].petalIndex,locked?before[i].petalIndex:fresh[i].petalIndex);
            CHECK_EQ(after[i].price,locked?before[i].price:fresh[i].price);
        }
        toggleShopTierLock(r,3,103);
        CHECK_EQ(shopTierLockMask(r),3);
        const auto both=tierLockedShopOffers(r,104);
        for(std::size_t i=0;i<both.size();++i)CHECK_EQ(both[i].petalIndex,after[i].petalIndex);
        toggleShopTierLock(r,3,104);
        CHECK_EQ(shopTierLockMask(r),0);
    }
}
TEST(tier_shop_locks_migrate_saved_whole_shelf_and_roundtrip_prices) {
    const auto dir=tempDir("tier-shop-save");
    const auto path=dir+"/inventory.json";
    std::remove(path.c_str());
    Database db;std::string error;CHECK(db.load(path,error));db.setPasswordCost(4);
    auto a=db.createUser("locks","password123");CHECK(a.ok());if(!a.ok())return;
    const auto id=a.account->id;
    auto& r=db.progress(id);
    r.extra["lockedShopRotation"]=100;
    captureShopLock(r,100);
    const auto original=lockedShopOffers(r);
    initializeShopTierLocks(r);
    CHECK_EQ(shopTierLockMask(r),3);
    const auto migrated=tierLockedShopOffers(r,200);
    CHECK_EQ(original.size(),migrated.size());
    for(std::size_t i=0;i<original.size();++i)CHECK_EQ(original[i].price,migrated[i].price);
    toggleShopTierLock(r,1,200);
    CHECK_EQ(shopTierLockMask(r),2);
    db.markDirty();CHECK(db.save());
    Database reloaded;CHECK(reloaded.load(path,error));
    CHECK_EQ(shopTierLockMask(*reloaded.findProgress(id)),2);
    const auto saved=tierLockedShopOffers(*reloaded.findProgress(id),201);
    for(std::size_t i=5;i<saved.size();++i){
        CHECK_EQ(saved[i].petalIndex,original[i].petalIndex);
        CHECK_EQ(saved[i].price,original[i].price);
    }
}
TEST(tier_shop_locks_network_purchase_and_relogin_use_the_mixed_shelf) {
    const auto dir=twoMapDataDir("tier-shop-network");
    const auto hour=shopRotation(shopClockNow())-2;
    Harness h("tier-shop-network",[&](const std::string& path){
        seedUser(path,"buyer","password123");
        Database db;std::string error;CHECK(db.load(path,error));
        auto& r=db.progress(db.findUser("buyer")->id);r.stars=1e15;
        r.extra["lockedShopRotation"]=static_cast<double>(hour);captureShopLock(r,hour);
        db.markDirty();CHECK(db.save());
    },dir,0);
    CHECK(h.ready);if(!h.ready)return;
    NetClient c;CHECK(loginAs(h,c,"buyer","password123"));
    CHECK(h.stepUntil({&c},[&]{return c.shopLockMask==3;}));
    c.toggleShopLock(1);
    CHECK(h.stepUntil({&c},[&]{return c.shopLockMask==2;}));
    const auto shelf=c.lockedShopOffers;
    CHECK_EQ(shelf.size(),10u);if(shelf.size()!=10)return;
    for(int slot:{0,5}){
        const auto offer=shelf[slot];
        const double stars=c.profile().stars;
        c.shopOutcome().pending=false;
        c.requestBuyPetal(offer.petalIndex,offer.rarity,slot,1);
        CHECK(h.stepUntil({&c},[&]{return c.shopOutcome().pending;}));
        CHECK(c.shopOutcome().ok);
        CHECK(h.stepUntil({&c},[&]{return c.profile().stars==stars-offer.price;}));
    }
    c.disconnect();h.step(5,{});
    NetClient again;CHECK(loginAs(h,again,"buyer","password123"));
    CHECK(h.stepUntil({&again},[&]{return again.shopLockMask==2;}));
    for(int i=5;i<10;++i)CHECK_EQ(again.lockedShopOffers[i].price,shelf[i].price);
    again.toggleShopLock(3);
    CHECK(h.stepUntil({&again},[&]{return again.shopLockMask==3;}));
    again.toggleShopLock(3);
    CHECK(h.stepUntil({&again},[&]{return again.shopLockMask==0;}));
}


TEST(tier_shop_locks_repair_partial_and_old_ultra_shelves_before_migration) {
    std::string error;CHECK(loadContent(dataDir(),error));
    for(int shape:{0,1,2}) {
        PlayerRecord r;
        r.extra["lockedShopRotation"]=100;
        captureShopLock(r,100);
        Json partial=Json::array();
        const auto offers=shopOffers(100);
        for(std::size_t i=0;i<offers.size();++i) {
            if(shape==0 && offers[i].rarity!=Rarity::Super)continue;
            if(shape==1 && i>=2 && i<5)continue;
            auto row=shopLockRow(offers[i]);
            if(shape==1 && i>=5)row["rarity"]="ultra";
            if(shape==2 && i%2==0)continue;
            partial.push(row);
        }
        r.extra["lockedShopOffers"]=partial;
        const auto repaired=lockedShopOffers(r);
        initializeShopTierLocks(r);
        const auto migrated=tierLockedShopOffers(r,200);
        CHECK_EQ(migrated.size(),10u);
        CHECK_EQ(shopTierLockMask(r),3);
        for(std::size_t i=0;i<migrated.size();++i) {
            CHECK_EQ(migrated[i].petalIndex,repaired[i].petalIndex);
            CHECK_EQ(migrated[i].rarity,repaired[i].rarity);
            CHECK_EQ(migrated[i].price,repaired[i].price);
        }
        for(const auto* tier:{"super","unique"})
            for(const auto& row:r.extra["shopTierLocks"][tier].items())CHECK(row["price"].isNull());
    }
}

TEST(tier_shop_locks_fill_a_retired_slot_without_duplicate_cards) {
    std::string error;
    CHECK(loadContent(dataDir(), error));
    constexpr std::int64_t hour = 103;
    const auto fresh = shopOffers(hour);
    CHECK_EQ(fresh.size(), 2 * kShopTierOfferCount);
    if (fresh.size() != 2 * kShopTierOfferCount) return;
    const auto& retained = fresh[kShopTierOfferCount - 1];
    PlayerRecord record;
    auto retired = shopLockRow(retained);
    retired["petalType"] = "withdrawn-shop-test-petal";
    Json super = Json::array();
    super.push(retired);
    super.push(shopLockRow(retained));
    record.extra["shopTierLocks"]["super"] = std::move(super);

    const auto offers = tierLockedShopOffers(record, hour);
    CHECK_EQ(offers.size(), fresh.size());
    if (offers.size() != fresh.size()) return;
    CHECK_EQ(offers[0].petalIndex, retained.petalIndex);
    CHECK_EQ(offers[0].discountPercent, retained.discountPercent);
    for (std::size_t i = 0; i < kShopTierOfferCount; ++i) {
        CHECK(offers[i].rarity == Rarity::Super);
        for (std::size_t j = 0; j < i; ++j) CHECK(offers[i].petalIndex != offers[j].petalIndex);
    }
    for (std::size_t i = kShopTierOfferCount; i < offers.size(); ++i) {
        CHECK_EQ(offers[i].petalIndex, fresh[i].petalIndex);
        CHECK_EQ(offers[i].price, fresh[i].price);
    }
}


TEST(shop_rotation_salt_changes_stock_without_changing_hour) {
    Harness h("shop-salt");
    CHECK(h.ready);
    setShopRotationSalt(0);
    const auto plain = shopOffers(42);
    setShopRotationSalt(0xC0FFEEu);
    const auto salted = shopOffers(42);
    setShopRotationSalt(0); // restore for later tests in this binary
    CHECK(!plain.empty());
    CHECK_EQ(plain.size(), salted.size());
    bool differed = false;
    for (std::size_t i = 0; i < plain.size(); ++i) {
        if (plain[i].petalIndex != salted[i].petalIndex ||
            plain[i].rarity != salted[i].rarity ||
            plain[i].discountPercent != salted[i].discountPercent) {
            differed = true;
            break;
        }
    }
    CHECK(differed);
}
