#pragma once

#include "shared/game/rarity.h"

#include <string_view>

namespace flix {

inline constexpr double kFlowerBaseMana = 20.0;
inline constexpr double kBasilLowManaFraction = 0.25;

inline double wornMagicManaPerSecond(std::string_view id, Rarity rarity) {
    double base = 0.0;
    if (id == "magic_void_singularity" || id == "magic_citron") base = 0.25;
    else if (id == "magic_void_opal" || id == "magic_topaz" || id == "magic_rubber" ||
             id == "magic_void_antimatter") base = 2.0;
    else if (id == "magic_cutter") base = 2.5;
    else if (id == "magic_void_relativity") base = 3.0;
    else if (id == "magic_arctic_sapphire") base = 4.0;
    else if (id == "magic_void_mask") base = 5.0;
    else if (id == "magic_quartz") base = 7.5;
    else if (id == "magic_ruby") base = 8.0;
    return base > 0.0 ? base * petalManaScale(rarity) : 0.0;
}

} // namespace flix
