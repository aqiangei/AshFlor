#pragma once

#include <algorithm>
#include <array>

#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "shared/game/config.h"

namespace flix::ui {

inline std::vector<Profile::Stack> mythicUltraCraftMaterials(
    const Profile& profile, const std::vector<std::string>& excluded) {
    std::vector<Profile::Stack> materials;
    for (const auto& stack : profile.inventory) {
        if ((stack.rarity != Rarity::Mythic && stack.rarity != Rarity::Ultra) || stack.craftable() < 5 ||
            stack.petalIndex >= content().petalCount()) continue;
        const auto& petal = content().petal(stack.petalIndex);
        if (petal.id == "timed_mjolnir" || !petalComesAt(petal, stack.rarity) ||
            std::find(excluded.begin(), excluded.end(), petal.id) != excluded.end()) continue;
        materials.push_back({stack.petalIndex, stack.rarity, stack.craftable()});
    }
    // A refreshed Profile may present identical stacks in a different order.
    std::sort(materials.begin(), materials.end(), [](const auto& a, const auto& b) {
        if (a.rarity != b.rarity) return rarityIndex(a.rarity) < rarityIndex(b.rarity);
        return a.petalIndex < b.petalIndex;
    });
    return materials;
}

inline std::array<std::string, 2> mythicUltraCraftLabels(const std::vector<Profile::Stack>& materials) {
    std::uint64_t mythic = 0;
    std::uint64_t ultra = 0;
    for (const auto& stack : materials)
        (stack.rarity == Rarity::Mythic ? mythic : ultra) += stack.count;

    return {std::string(trEnZh("Mythic materials  ×", "神话材料  ×")) + std::to_string(mythic),
            std::string(trEnZh("Ultra materials  ×", "究极材料  ×")) + std::to_string(ultra)};
}

inline Rect mythicUltraCraftEntry(Rect panel, Rect materialView) {
    return {panel.right() - 12.0 - 184.0, materialView.y + 40.0, 184.0, 30.0};
}

struct MythicUltraCraftButtons {
    Rect cancel;
    Rect confirm;
};

enum class MythicUltraCraftState { Preparing, Ready, Refresh };

inline MythicUltraCraftButtons drawMythicUltraConfirmation(
    MenuContext& ctx, const SlotCardLayout& card, const std::vector<Profile::Stack>& materials,
    bool refreshed, MythicUltraCraftState state = MythicUltraCraftState::Ready) {
    auto text = card;
    text.lineY = card.panel.y + 82.0;
    const auto line = [&](const std::string& label) {
        drawSlotLine(ctx.canvas, text, label);
        text.lineY += 30.0;
    };
    line(trEnZh("Craft only Mythic & Ultra", "仅合成神话、究极"));
    for (const auto& label : mythicUltraCraftLabels(materials)) line(label);
    line(trEnZh("New Ultra petals stay in your bag.", "本次由神话合出的究极会保留。"));
    line(trEnZh("Failed attempts consume 1–4 petals.", "每次失败会消耗 1–4 件。"));
    if (materials.empty()) {
        line(trEnZh("No eligible materials.", "没有符合条件的材料。"));
    } else if (state == MythicUltraCraftState::Refresh) {
        line(trEnZh("Could not prepare these materials. Refresh the counts.",
                    "当前材料无法准备合成，请刷新数量。"));
    } else if (state == MythicUltraCraftState::Preparing) {
        line(trEnZh("Checking materials…", "正在核对材料…"));
    } else if (refreshed) {
        line(trEnZh("Review the updated counts.", "请核对刷新后的数量。"));
    } else {
        line(trEnZh("Only these counts will be used.", "仅使用此处列出的数量。"));
    }

    const double width = std::min(126.0, (card.panel.w - 34.0) * 0.5);
    MythicUltraCraftButtons buttons{
        {card.centreX - width - 5.0, card.panel.y + 271.0, width, 36.0},
        {card.centreX + 5.0, card.panel.y + 271.0, width, 36.0},
    };
    drawSlotButton(ctx.canvas, buttons.cancel, trEnZh("Cancel", "取消"),
                   0x5A9FDBu, ctx.over(buttons.cancel), false);
    const auto tint = state != MythicUltraCraftState::Preparing
        ? std::optional<std::uint32_t>{0xCC6644u} : std::nullopt;
    const auto action = state == MythicUltraCraftState::Refresh
        ? trEnZh("Refresh counts", "刷新数量") : trEnZh("Confirm craft", "确认合成");
    drawSlotButton(ctx.canvas, buttons.confirm, action,
                   tint, ctx.over(buttons.confirm), false);
    return buttons;
}

} // namespace flix::ui
