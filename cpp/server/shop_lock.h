#pragma once
#include "server/db.h"
#include "shared/game/shop.h"

namespace flix {
// Store stable content IDs, never registry indices or prices. Locked stock stays
// the same through content updates; prices still follow the current economy.
inline Json shopLockRow(const ShopOffer& offer) {
    Json row=Json::object();row["petalType"]=content().petal(offer.petalIndex).id;
    row["rarity"]=rarityName(offer.rarity);row["discountPercent"]=offer.discountPercent;
    return row;
}
// A lock holds the whole shelf of its hour: the Super row and the Eternal row.
inline void captureShopLock(PlayerRecord& record, std::int64_t rotation) {
    Json rows=Json::array();
    for(const auto& offer:shopOffers(rotation)) rows.push(shopLockRow(offer));
    record.extra["lockedShopOffers"]=rows;
    record.extra["lockedShopOffersRotation"]=static_cast<double>(rotation);
}
inline std::vector<ShopOffer> lockedShopOffers(const PlayerRecord& record) {
    const auto rotation=static_cast<std::int64_t>(record.extra["lockedShopRotation"].asDouble(-1));
    if(rotation<0)return {};
    const auto sameHour = shopOffers(rotation);
    const auto& rows=record.extra["lockedShopOffers"];
    const bool captured = rows.isArray() && record.extra["lockedShopOffersRotation"].asDouble(-1) == rotation;
    const std::size_t tierSize = kShopTierOfferCount;
    std::vector<ShopOffer> result;
    // Saved cards come first. A slot the save cannot fill takes a card of the
    // same hour, so the shelf is always whole: a lock from when Ultra was sold,
    // one from when only the Super row was saved, a petal since withdrawn.
    const auto addTier = [&](Rarity rarity) {
        const std::size_t first = result.size();
        const auto listed = [&](std::uint16_t petalIndex) {
            return std::any_of(result.begin() + static_cast<std::ptrdiff_t>(first), result.end(),
                               [&](const ShopOffer& offer) { return offer.petalIndex == petalIndex; });
        };
        if (captured) for (const auto& row : rows.items()) {
            if (result.size() - first == tierSize) break;
            if (row["rarity"].asString() != rarityName(rarity)) continue;
            const auto petalIndex = content().petalIndex(row["petalType"].asString());
            if (!shopSellsPetal(petalIndex) || listed(petalIndex)) continue;
            if (!petalComesAt(content().petal(petalIndex), rarity)) continue;
            // Only Super cards are ever discounted.
            const int discount = rarity == Rarity::Super ? clamp(row["discountPercent"].asInt(), 0, 30) : 0;
            result.push_back({petalIndex, rarity, shopOfferPrice(petalIndex, rarity, discount), discount});
        }
        for (const auto& offer : sameHour) {
            if (result.size() - first == tierSize) break;
            if (offer.rarity == rarity && !listed(offer.petalIndex)) result.push_back(offer);
        }
    };
    addTier(Rarity::Super);
    addTier(Rarity::Unique);
    return result;
}
// A lock taken while only the Super row was saved has no Eternal cards on
// record. Write down the ones it shows now, so that a later change to the petal
// pool cannot swap them under the player. Returns whether the record changed.
inline bool saveMissingLockedEternals(PlayerRecord& record) {
    const PlayerRecord& saved = record;
    const auto rotation=static_cast<std::int64_t>(saved.extra["lockedShopRotation"].asDouble(-1));
    if(rotation<0 || !saved.extra["lockedShopOffers"].isArray() ||
       saved.extra["lockedShopOffersRotation"].asDouble(-1)!=rotation) return false;
    for(const auto& row:saved.extra["lockedShopOffers"].items())
        if(row["rarity"].asString()==rarityName(Rarity::Unique)) return false;
    bool changed=false;
    for(const auto& offer:lockedShopOffers(record)) {
        if(offer.rarity!=Rarity::Unique) continue;
        record.extra["lockedShopOffers"].push(shopLockRow(offer));changed=true;
    }
    return changed;
}
inline int shopTierLockMask(const PlayerRecord& record) {
    const auto& tiers = record.extra["shopTierLocks"];
    if (!tiers.isObject())
        return record.extra["lockedShopRotation"].asDouble(-1) >= 0 ? 3 : 0;
    return (tiers["super"].isArray() ? 1 : 0) | (tiers["unique"].isArray() ? 2 : 0);
}

inline Json shopTierRows(const std::vector<ShopOffer>& offers, Rarity rarity) {
    Json rows = Json::array();
    for (const auto& offer : offers) {
        if (offer.rarity != rarity) continue;
        auto row = shopLockRow(offer);
        rows.push(row);
    }
    return rows;
}

inline void initializeShopTierLocks(PlayerRecord& record) {
    if (record.extra["shopTierLocks"].isObject()) return;
    const auto legacy = lockedShopOffers(record);
    Json tiers = Json::object();
    if (shopTierLockMask(record)) {
        tiers["super"] = shopTierRows(legacy, Rarity::Super);
        tiers["unique"] = shopTierRows(legacy, Rarity::Unique);
    }
    record.extra["shopTierLocks"] = std::move(tiers);
}

inline std::vector<ShopOffer> tierLockedShopOffers(const PlayerRecord& record, std::int64_t current) {
    if (!record.extra["shopTierLocks"].isObject()) {
        return shopTierLockMask(record) ? lockedShopOffers(record) : shopOffers(current);
    }
    const auto fresh = shopOffers(current);
    std::vector<ShopOffer> result;
    result.reserve(fresh.size());
    for (const auto rarity : {Rarity::Super, Rarity::Unique}) {
        const auto& rows = record.extra["shopTierLocks"][rarityName(rarity)];
        if (!rows.isArray()) {
            for (const auto& offer : fresh) {
                if (offer.rarity == rarity) result.push_back(offer);
            }
            continue;
        }
        const std::size_t first = result.size();
        const auto listed = [&](std::uint16_t index) {
            return std::any_of(result.begin() + static_cast<std::ptrdiff_t>(first), result.end(),
                               [index](const ShopOffer& offer) { return offer.petalIndex == index; });
        };
        for (const auto& row : rows.items()) {
            const auto index = content().petalIndex(row["petalType"].asString());
            if (!shopSellsPetal(index) || !petalComesAt(content().petal(index), rarity)) continue;
            const int discount = rarity == Rarity::Super ? clamp(row["discountPercent"].asInt(), 0, 30) : 0;
            const double price = shopOfferPrice(index, rarity, discount);
            if (listed(index)) continue;
            result.push_back({index, rarity, price, discount});
            if (result.size() - first == kShopTierOfferCount) break;
        }
        for (const auto& offer : fresh) {
            if (result.size() - first == kShopTierOfferCount) break;
            if (offer.rarity == rarity && !listed(offer.petalIndex)) result.push_back(offer);
        }
    }
    return result;
}

inline void toggleShopTierLock(PlayerRecord& record, int mask, std::int64_t current) {
    initializeShopTierLocks(record);
    const int previous = shopTierLockMask(record);
    const bool unlock = (previous & mask) == mask;
    const auto offers = tierLockedShopOffers(record, current);
    for (const auto rarity : {Rarity::Super, Rarity::Unique}) {
        const int bit = rarity == Rarity::Super ? 1 : 2;
        if (!(mask & bit)) continue;
        auto& rows = record.extra["shopTierLocks"][rarityName(rarity)];
        if (unlock) rows = Json();
        else if (!(previous & bit)) rows = shopTierRows(offers, rarity);
    }
    // Keep the old field as an any-lock indicator for existing account views.
    record.extra["lockedShopRotation"] = shopTierLockMask(record) ? static_cast<double>(current) : -1.0;
}
} // namespace flix
