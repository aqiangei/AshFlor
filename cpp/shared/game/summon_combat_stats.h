#pragma once

#include "shared/game/config.h"
#include "shared/game/hyper_petal_techniques.h"
#include "shared/game/skills.h"

namespace flix {

struct PharaohCrownCombatStats {
    double health;
    double contactDamage;
};

inline PharaohCrownCombatStats pharaohCrownCombatStats(const PetalConfig& crown,
                                                       const MobConfig& mummy, Rarity rarity,
                                                       const SkillSet* skills) {
    // Permanent Hyper keeps its original body tier and matches Hyper for these two stats.
    const Rarity combatRarity = rarity == Rarity::PermanentHyper
        ? Rarity::Apex : pharaohCrownSummonStatsRarity(rarity);
    const double scale = petalStatScale(combatRarity) * hyperSummonPromotionScale(crown, combatRarity);
    const double healthTalent = skills ? skills->healthScale(SkillId::Summoner) : 1.0;
    const double damageTalent = skills ? skills->effectScale(SkillId::PetDamage) : 1.0;

    return {
        mummy.health * scale * mummy.petHealthScale * kSummonedMobHealthScale *
            healthTalent * ascendantSummonHealthScale(rarity),
        mummy.damage * scale * mummy.petDamageScale * summonDamageScale(crown, combatRarity) *
            damageTalent * configuredSummonDamageScale(crown, rarity),
    };
}

}  // namespace flix
