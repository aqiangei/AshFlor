#include <set>
#include "test.h"
#include "server_harness.h"
#include "server/shop_lock.h"
using namespace flix;
using namespace flix::testsupport;

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
    for (std::uint64_t rotation = 100; rotation < 200; ++rotation) {
        const auto offers = shopOffers(rotation);
        CHECK_EQ(offers.size(), std::size_t(kShopOfferCount));
        std::set<std::uint16_t> petals;
        for (std::size_t i = 0; i < offers.size(); ++i) {
            const auto& offer = offers[i];
            CHECK(offer.rarity == (i < kShopTierOfferCount ? Rarity::Super : Rarity::Unique));
            CHECK_EQ(offer.price, shopOfferPrice(offer.petalIndex, offer.rarity, offer.discountPercent));
            CHECK(shopSellsPetal(offer.petalIndex));
            CHECK(petals.insert(offer.petalIndex).second);
        }
    }
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
    CHECK_EQ(shopPrice(nuke, Rarity::Super), shopPrice(mimic, Rarity::Super));
    CHECK_EQ(shopPrice(egg, Rarity::Super), shopPrice(mimic, Rarity::Super));
    CHECK_EQ(shopOfferPrice(nuke, Rarity::Super, 30), 31'500);
    CHECK_EQ(shopPrice(nuke, Rarity::Unique), 810'000);
    for (const auto petal : content().petalDisplayOrder()) {
        if (!shopSellsPetal(petal)) continue;
        const auto super = shopPrice(petal, Rarity::Super);
        CHECK_EQ(shopPrice(petal, Rarity::Unique), 18 * super);
        CHECK_EQ(shopOfferPrice(petal, Rarity::Unique, 30), 18 * super);
        if (content().petal(petal).id.rfind("arctic_", 0) != 0 &&
            content().petal(petal).id != "magic_golden_leaf")
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
