#pragma once

#include <unordered_map>
#include <unordered_set>

#include "client/net_client.h"
#include "shared/game/absorb.h"

namespace flix::ui {

inline std::uint32_t absorbStackKey(std::uint16_t petalIndex, Rarity rarity) {
    return (static_cast<std::uint32_t>(petalIndex) << 8) |
           static_cast<std::uint32_t>(rarityIndex(rarity));
}

// Skip entire stacks referenced by current or saved gear. Keep one copy of
// each other low-tier stack so batch selection only stages spare petals.
inline auto bulkAbsorbCounts(const Profile& profile, const ContentRegistry& registry) {
    std::unordered_set<std::uint32_t> protectedStacks;
    const auto protect = [&](const auto& slots) {
        for (const auto& slot : slots)
            if (!slot.empty()) protectedStacks.insert(absorbStackKey(slot.petalIndex, slot.rarity));
    };
    protect(profile.loadout);
    for (const auto& preset : profile.presets) protect(preset);

    std::unordered_map<std::uint32_t, std::uint32_t> counts;
    for (const auto& stack : profile.inventory) {
        if (stack.count <= 1 || rarityIndex(stack.rarity) > rarityIndex(Rarity::Rare) ||
            stack.petalIndex >= registry.petalCount() ||
            !absorbable(registry.petal(stack.petalIndex), stack.rarity)) continue;
        const auto key = absorbStackKey(stack.petalIndex, stack.rarity);
        if (protectedStacks.count(key) == 0) counts.emplace(key, stack.count - 1);
    }
    return counts;
}

} // namespace flix::ui
