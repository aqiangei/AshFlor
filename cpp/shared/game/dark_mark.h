#pragma once

#include "shared/game/rarity.h"

namespace flix {
// Corpse transport. Apex (H) marks also hold the target for a short lockout.
inline constexpr double kDarkMarkRange = 45.0;
inline constexpr double kDarkMarkReloadMillis = 20000.0;
inline constexpr double kDarkMarkProtectionMillis = 300000.0;
// No imprisonment: Mark only transports. Hold kept at 0 so H behaves like
// ordinary Mark for /mark-return and door teleports (board: 传送不行).
inline constexpr double kHyperMarkHoldMillis = 0.0;
inline double darkMarkDelayMillis(Rarity rarity) {
    const int tier = rarityIndex(ladderRarity(rarity));
    return tier >= 8 ? 2000.0 : tier == 7 ? 4000.0 : tier == 6 ? 6000.0 : 8000.0;
}
} // namespace flix
