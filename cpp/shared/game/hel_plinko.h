#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <cstdint>

#include "shared/game/rarity.h"

namespace flix {

inline constexpr int kHelPlinkoRows = 16;
inline constexpr unsigned kHelPlinkoPaths = 1u << kHelPlinkoRows;
inline constexpr std::array<int, kHelPlinkoRows + 1> kHelPlinkoPayout{500, 20, 5, 3, 2, 0, 1, 1, 1, 1, 1, 0, 2, 3, 5, 20, 500};
inline constexpr int kHelPlinkoMaxPayout = *std::max_element(kHelPlinkoPayout.begin(), kHelPlinkoPayout.end());
static_assert(kHelPlinkoPaths - 1 <= std::numeric_limits<std::uint16_t>::max());
inline constexpr double kHelPlinkoDropSeconds = 6.60;

constexpr int helPlinkoSlot(unsigned path) {
    int slot = 0;
    for (int step = 0; step < kHelPlinkoRows; ++step)
        slot += static_cast<int>((path >> step) & 1u);
    return slot;
}

inline constexpr char kPlinkoChipPetal[] = "poker_chip";

constexpr bool plinkoAnnouncesWin(Rarity rarity, int payout) {
    return rarity == Rarity::Ascendant && (payout == 5 || payout == 20 || payout == 500);
}

inline constexpr int kPlinkoSuperPrice = 8800;
inline constexpr int kPlinkoEternalPrice = 88000;

constexpr int plinkoStarPrice(Rarity rarity) {
    if (rarity == Rarity::Super) return kPlinkoSuperPrice;
    if (rarity == Rarity::Unique) return kPlinkoEternalPrice;
    return 0;
}

inline constexpr std::array kPlinkoTiers{
    Rarity::Common, Rarity::Uncommon, Rarity::Rare, Rarity::Epic, Rarity::Legendary,
    Rarity::Mythic, Rarity::Ultra, Rarity::Super, Rarity::Unique,
    Rarity::PermanentHyper, Rarity::Ascendant};

constexpr bool plinkoAccepts(Rarity rarity) {
    return std::find(kPlinkoTiers.begin(), kPlinkoTiers.end(), rarity) != kPlinkoTiers.end();
}

enum class HelPlinkoAction : std::uint8_t { Drop, Buy, Exchange, BuyTen, BuyHundred };
inline constexpr std::array kPlinkoBuyActions{HelPlinkoAction::Buy, HelPlinkoAction::BuyTen, HelPlinkoAction::BuyHundred};
constexpr int plinkoBuyCount(HelPlinkoAction action) {
    switch (action) {
        case HelPlinkoAction::Buy: return 1;
        case HelPlinkoAction::BuyTen: return 10;
        case HelPlinkoAction::BuyHundred: return 100;
        default: return 0;
    }
}
enum class HelPlinkoStatus : std::uint8_t { Success, Unavailable, InvalidTier, NoChips, StackFull, Busy, NoStars };

} // namespace flix
