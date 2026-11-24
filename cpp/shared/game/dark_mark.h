#pragma once

#include "shared/game/rarity.h"

namespace flix {
// Corpse transport and the post-arrival hold for Apex items.
inline constexpr double kDarkMarkRange = 45.0;
inline constexpr double kDarkMarkReloadMillis = 20000.0;
inline constexpr double kDarkMarkProtectionMillis = 300000.0;
inline constexpr double kApexMarkHoldMillis = 120000.0;
inline constexpr bool markHoldsOnArrival(Rarity rarity) { return rarity == Rarity::Ascendant; }
inline double darkMarkDelayMillis(Rarity rarity) {
    const int tier = rarityIndex(ladderRarity(rarity));
    return tier >= 8 ? 2000.0 : tier == 7 ? 4000.0 : tier == 6 ? 6000.0 : 8000.0;
}
} // namespace flix
