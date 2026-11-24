#pragma once
#include "server/systems/combat.h"
#include "shared/game/skills.h"
namespace flix {
inline double lightningTalentScale(const World& world, Entity source) {
    const Entity player = CombatSystem::creditedPlayer(world, source);
    const auto* tree = world.tryGet<PlayerSkillTree>(player);
    return tree ? tree->skills.effectScale(SkillId::LightningPower) : 1.0;
}
inline double explosionTalentRadius(const World& world, Entity source, double radius) {
    if (!(radius > 0.0)) return radius;
    const Entity player = CombatSystem::creditedPlayer(world, source);
    const auto* tree = world.tryGet<PlayerSkillTree>(player);
    const auto* modifiers = world.tryGet<PlayerModifiers>(player);
    return radius * (modifiers ? modifiers->magicExplosionRadiusScale : 1.0) + (tree ? tree->skills.statScale(SkillId::ExplosionReach) : 0.0);
}
} // namespace flix
