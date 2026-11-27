#pragma once

#include "shared/game/rarity.h"

namespace flix {

inline constexpr double kChromosomeRepairHealthCap = 0.30;

inline double chromosomeRepairPerPulse(Rarity rarity) {
    return 10.0 * petalHealScale(rarity);
}

} // namespace flix
