#pragma once
#include "shared/game/config.h"
#include "shared/core/world.h"
#include <algorithm>
namespace flix {
// Functional parts, not copies: software, memory, bus, power, controller,
// geometry. Raw slots are counted so Mimic cannot create fictitious hardware.
inline unsigned computerPart(const std::string& id) {
 if(id=="javascript"||id=="c++")return 1;
 if(id=="ram")return 2;
 if(id=="computer_chip")return 4;
 if(id=="capacitor")return 8;
 if(id=="cursor")return 16;
 if(id=="cube"||id=="icosahedron"||id=="pentagon")return 32;
 return 0;
}
inline bool computerWeapon(const std::string& id) {
 return computerPart(id)!=0||id=="spaceship"||id=="glitch"||id=="bomb";
}
inline int computerPartCount(unsigned mask){int n=0;for(;mask;mask>>=1)n+=mask&1;return n;}
inline void applyComputerSet(const PetalConfig& config, unsigned mask, PetalStats& stats){
 if(!computerWeapon(config.id))return;
 const int n=computerPartCount(mask);
 if(n>=3){if(stats.damage>0)stats.damage*=1.12;stats.lightningDamagePerHit*=1.12;}
 if(n>=4&&std::isfinite(stats.health)&&stats.health>1)stats.health*=1.15;
 if(n>=5)stats.reloadMillis*=.9;
}
inline double deadLeafDurabilityBonus(Rarity rarity) {
    constexpr double byTier[] = {.04, .05, .06, .08, .10, .13, .17, .22, .28, .35};
    return byTier[ladderIndex(rarity)];
}

inline PetalStats computerSetStats(const ContentRegistry& registry, World& world, Entity owner,
 std::uint16_t index, Rarity rarity, PetalStats stats){
 if(const auto* mods=world.tryGet<PlayerModifiers>(owner))
  applyComputerSet(registry.petal(index),mods->computerParts[static_cast<std::size_t>(rarityIndex(rarity))],stats);
 // Golden cactus / dead leaf raise real petal pools and projectile durability.
 // Strongest copy only; cactus and leaf share one max so they do not stack.
 double bonus=0.0;
 if(const auto* loadout=world.tryGet<Loadout>(owner)) {
  static constexpr double cactus[]={.01,.02,.03,.05,.08,.13,.21,.34,.55,.825};
  for(int slot=0;slot<std::min(loadout->activeSlots,kLoadoutActiveSlots);++slot) {
   const auto item=equippedPetal(registry,*loadout,slot);
   if(item.empty()) continue;
   const auto& id=registry.petal(item.configIndex).id;
   const int tier=ladderIndex(item.rarity);
   if(id=="golden_cactus") bonus=std::max(bonus,cactus[tier]);
   else if(id=="dead_leaf") bonus=std::max(bonus,deadLeafDurabilityBonus(item.rarity));
  }
 }
 if(stats.health>0.0&&stats.breakable)stats.health*=1.0+bonus;
 if(registry.petal(index).id=="void_scythe") {
  int equipped=0;
  if(const auto* loadout=world.tryGet<Loadout>(owner))
   for(int slot=0;slot<std::min(loadout->activeSlots,kLoadoutActiveSlots);++slot)
    equipped+=!loadout->slots[slot].empty();
  stats.damage/=std::max(1,equipped);stats.health/=std::max(1,equipped);
 }
 return stats;
}
}
