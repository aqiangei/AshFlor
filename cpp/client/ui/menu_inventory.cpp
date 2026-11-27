#include "shared/game/hyper_petal_techniques.h"
#include "shared/game/computer_set.h"
#include "shared/game/monstera.h"
#include "shared/game/computer_set.h"
// The petal inventory.
//
// The screen players spend the most time in, so the muscle memory is real and
// the layout is held to the reference shot: a five-column grid of 60px cells
// grouped under a centred tier label with a rule running out to either edge of
// the grid, a Stack toggle and a search box above them, and a scroll thumb in
// the strip the grid leaves free on the right.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "client/ease.h"
#include "client/render/art_cache.h"
#include "client/ui/content_i18n.h"
#include "client/ui/gallery_drop_preview.h"
#include "client/ui/gallery_map_mobs.h"
#include "client/ui/item_tile.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "client/ui/text_input.h"
#include "shared/core/text.h"
#include "shared/game/chromosome.h"
#include "shared/game/config.h"
#include "shared/game/dark_mark.h"
#include "shared/game/domino.h"
#include "shared/game/map_elements.h"
#include "shared/game/npc.h"
#include "shared/game/rarity.h"
#include "shared/game/summon_combat_stats.h"
#include "shared/game/zorr_mana.h"

namespace flix {

using namespace flix::ui;

namespace {

/// The stack toggle's knob: a quarter of the way per 60 fps frame, the
/// browser's rate.
const double kStackToggleEaseSeconds = easeTimeConstant(0.25);

/// The grid's margins. NOT equal: the right one also has to clear the scroll
/// thumb, so the five columns sit a touch left of the panel's own centre and
/// the thumb rides in the strip they leave. Centring the block on the panel
/// instead puts the last column under the thumb.
constexpr double kGridPadding = 26.0;
constexpr double kGridGutter = 42.0;
/// The air above the first heading and below the last row. Its own constant
/// rather than kGridPadding: the side margins are set by the scroll thumb and
/// this one is not.
constexpr double kGridTopPad = 12.0;
constexpr double kSectionGap = 14.0;
/// The band a tier heading occupies, and how far the tiles under it clear its
/// baseline. Both grew with the cells: a 60px tile needs more air above it
/// than a 56px one before the heading stops looking stuck to it.
constexpr double kLabelHeight = 26.0;
constexpr double kLabelToGrid = 12.0;
constexpr int kColumns = 5;

/// The controls row and everything above it. Fixed, because the grid below
/// scrolls and the header must not.
constexpr double kHeaderHeight = 173.0;
constexpr double kControlsY = 91.0;
constexpr double kControlsHeight = 33.0;
constexpr double kToggleBoxSize = 29.0;
constexpr double kToggleHitWidth = 95.0;

/// The controls row's own inset, wider than `kMenuPadding`: the toggle lines
/// up with the grid's left margin rather than with the title's, so the header
/// and the cells under it share one left edge.
constexpr double kControlsPad = kGridPadding;
constexpr double kControlsRightPad = 22.0;
/// The word beside the toggle, and the tier heading over each run of cells.
/// Both sized to the taller controls and larger cells around them.
constexpr double kToggleLabelSize = 16.0;
constexpr double kSectionLabelSize = 15.0;

/// The height the scroll range, the cull and the scrollbar are all measured
/// against. Deliberately the panel's own bottom padding rather than the
/// kMenuBorder the content is CLIPPED to: the wider strip is reserved, so the
/// last few units of the clip rect are never scrolled into.
///
/// Sized past a single cell so a trailing row that would otherwise sit flush
/// against the panel's floor -- and read as stacked on the row above -- is
/// pushed into the scroll range instead. Smaller than this and a short bag
/// still fits without a thumb; at one cell the thumb appears as soon as a
/// second row would crowd the bottom.
constexpr double kViewBottomPad = kCellSize + kCellGap;

/// One wheel notch is a raw ~100px deltaY in the browser; SDL reports ±1, so
/// the step is spelled out here.
constexpr double kWheelStep = 100.0;

/// The thumb, and how far in from the panel's right edge its RIGHT side sits.
/// There is no track: the panel paints the thumb alone, in the skin's accent
/// so it reads as part of the frame rather than as content.
constexpr double kThumbWidth = 8.0;
constexpr double kThumbInset = 14.0;
constexpr double kThumbMinHeight = 20.0;

/// Long enough that no player reaches it. The field scrolls its own head out
/// of view rather than refusing keys, exactly as the DOM input does.
constexpr std::size_t kSearchLimit = 128;

/// The tier heading's rules: their ink, and the air they leave either side of
/// the word. Round-capped, so the gap reads as a gap and not as a cut.
constexpr double kRuleWidth = 5.0;
constexpr double kRuleGap = 13.0;

/// How wide the hover tooltip's body text wraps. Its dwell before it appears
/// is TooltipDelay::kDelaySeconds.
constexpr double kTooltipWrapWidth = 230.0;

/// One laid-out cell. In stacked mode a cell stands for a petal TYPE at the
/// best tier the account holds, rather than for one stack.
struct Cell {
    Rect rect;
    std::uint16_t petalIndex = kNoPetal;
    Rarity rarity = Rarity::Common;
    std::uint32_t count = 0;
};

struct Section {
    Rarity rarity = Rarity::Common;
    double labelY = 0;
};

struct Grid {
    std::vector<Cell> cells;
    std::vector<Section> sections;
    double contentHeight = 0;
};

/// The tooltip's dwell timer.
///
/// File-local rather than a panel field because InventoryPanel's members are
/// declared in menus.h, which this file does not own -- and there is exactly
/// one inventory panel in the client, so one timer is the right number.
TooltipDelay tooltipDelay;
TouchInspectDelay touchInspect;

bool matchesSearch(std::uint16_t petalIndex, const std::string& needle) {
    if (needle.empty()) return true;
    const PetalConfig& config = content().petal(petalIndex);
    const std::string name = titleCase(config.name);
    return lowerCase(petalNameTr(config.id, name.c_str())).find(needle) != std::string::npos ||
           lowerCase(name).find(needle) != std::string::npos ||
           lowerCase(config.id).find(needle) != std::string::npos ||
           std::string_view(petalNameZh(config.id)).find(needle) != std::string_view::npos ||
           std::string_view(petalSearchAliases(config.id)).find(needle) != std::string_view::npos;
}

std::vector<bool> mapDropTypes(const MapData& map) {
    std::vector<bool> drops(content().petalCount(), false);
    for (const auto mob : mapMobIndices(map)) {
        for (int tier = 0; tier < kRarityCount; ++tier) {
            const auto rarity = clampRarity(tier);
            if (tier >= kLadderRarityCount && rarity != Rarity::Ascendant) continue;
            // This filters owned petals by possible source. Use the gallery's
            // Eternal orb preview alongside ordinary drops, regardless of equipment.
            for (const int orbTier : {kRarityCount, rarityIndex(Rarity::Unique)}) {
                for (const auto& drop : galleryDropPreview(mob, rarity, orbTier))
                    if (drop.petalIndex < drops.size() && drop.probability > 0.0)
                        drops[drop.petalIndex] = true;
            }
        }
    }
    return drops;
}

/// Centre each row within the scrolling content area, including partial rows.
void layoutRun(Grid& grid, const std::vector<Cell>& entries, double left, double innerWidth,
               double& y) {
    if (entries.empty()) return;
    const int rows = static_cast<int>((entries.size() + kColumns - 1) / kColumns);

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const int row = static_cast<int>(i) / kColumns;
        const int column = static_cast<int>(i) % kColumns;
        const int rowCount = std::min(kColumns, static_cast<int>(entries.size()) - row * kColumns);
        const double rowWidth = rowCount * kCellSize + (rowCount - 1) * kCellGap;
        const double rowStart = left + (innerWidth - rowWidth) * 0.5;
        Cell cell = entries[i];
        cell.rect = {rowStart + column * (kCellSize + kCellGap), y + row * (kCellSize + kCellGap),
                     kCellSize, kCellSize};
        grid.cells.push_back(cell);
    }
    y += rows * kCellSize + (rows - 1) * kCellGap;
}

/// Builds the grid in CONTENT space: y = 0 is the top of the scrolling area,
/// so the scroll offset is applied once, when drawing, and never leaks into
/// the layout or the hit test.
///
/// Petals only, and not because the panel is unfinished: Profile::Stack
/// carries a petal index alone. Mobs drop no consumables, and the TypeScript
/// build's speed boosts, health potions and shields became powder, roses and
/// shells where an account loads (server/db.cpp).
Grid buildGrid(const Profile& profile, double innerWidth, bool stacked, bool sortByCount,
               const std::string& needle, const std::vector<bool>* dropTypes) {
    Grid grid;
    double y = kGridTopPad;
    const auto visible = [&](std::uint16_t petal) {
        return (!dropTypes || (petal < dropTypes->size() && (*dropTypes)[petal])) &&
               matchesSearch(petal, needle);
    };

    const auto layoutEntries = [&](std::vector<Cell>& entries) {
        if (sortByCount) {
            std::stable_sort(entries.begin(), entries.end(), [](const Cell& a, const Cell& b) {
                return a.count > b.count;
            });
        }
        layoutRun(grid, entries, kGridPadding, innerWidth, y);
    };

    if (stacked) {
        // One cell per petal TYPE, shown at the best tier the account holds.
        // Deliberately no tier headings: the point of this mode is a compact
        // catalogue of what you own, not where it sits on the ladder. Petal
        // indices are assigned in sorted-id order, which is the canonical
        // ordering this mode is sorted by in the browser too.
        std::vector<std::array<std::uint32_t, kRarityCount>> counts(content().petalCount());
        // Reverse order preserves stackCount's first-match semantics.
        for (auto it = profile.inventory.rbegin(); it != profile.inventory.rend(); ++it) {
            const int tier = rarityIndex(it->rarity);
            if (it->petalIndex < counts.size() && tier >= 0 && tier < kRarityCount)
                counts[it->petalIndex][tier] = it->count;
        }
        std::vector<Cell> entries;
        for (std::size_t i = 0; i < content().petalCount(); ++i) {
            const auto petalIndex = static_cast<std::uint16_t>(i);
            if (!visible(petalIndex)) continue;
            for (int tier = kRarityCount - 1; tier >= 0; --tier) {
                const Rarity rarity = clampRarity(tier);
                const std::uint32_t count = counts[i][tier];
                if (count == 0) continue;
                entries.push_back({Rect{}, petalIndex, rarity, count});
                break;
            }
        }
        layoutEntries(entries);
        y += kSectionGap;
        grid.contentHeight = y + kGridTopPad;
        return grid;
    }

    for (int tier = kRarityCount - 1; tier >= 0; --tier) {
        const Rarity rarity = clampRarity(tier);
        std::vector<Cell> entries;
        // Left in the profile's own order, which is the order the server sent
        // and therefore acquisition order. Sorting it here would arrange the
        // same account's grid differently from the browser's.
        for (const Profile::Stack& stack : profile.inventory) {
            if (stack.rarity != rarity || stack.count == 0) continue;
            if (!visible(stack.petalIndex)) continue;
            entries.push_back({Rect{}, stack.petalIndex, stack.rarity, stack.count});
        }
        if (entries.empty()) continue;

        y += kLabelHeight;
        grid.sections.push_back({rarity, y - kLabelToGrid});
        layoutEntries(entries);
        y += kSectionGap;
    }

    grid.contentHeight = y + kGridTopPad;
    return grid;
}

/// Hit / paint box of the thumb strip: right-inset column over the grid view.
Rect thumbTrack(Rect panel, double contentTop, double visibleH) {
    return {panel.right() - kThumbInset - kThumbWidth, contentTop, kThumbWidth, visibleH};
}

/// The scroll thumb, and only the thumb: a square-cornered quarter-black bar
/// inset from the panel's right edge.
///
/// Local rather than ui::scrollbar(), whose rounded, tracked, opaque bar is
/// what the other panels are drawn with. This one has no track at all.
void scrollThumb(Canvas& canvas, Rect panel, double contentTop, double visibleH,
                 double contentHeight, double scroll) {
    if (contentHeight <= visibleH || visibleH <= 0) return;
    const double travel = contentHeight - visibleH;
    const double thumbHeight = std::max(kThumbMinHeight, visibleH * (visibleH / contentHeight));
    const double thumbY = contentTop + clamp(scroll / travel, 0.0, 1.0) * (visibleH - thumbHeight);
    setFill(canvas, kInventorySkin.accent);
    canvas.beginPath();
    canvas.roundRect(static_cast<float>(panel.right() - kThumbInset - kThumbWidth),
                     static_cast<float>(thumbY), static_cast<float>(kThumbWidth),
                     static_cast<float>(thumbHeight), static_cast<float>(kThumbWidth * 0.5));
    canvas.fill();
}

/// Scrollbar drag state.
///
/// Belongs on InventoryPanel, but menus.h is not this change's to edit.
/// One inventory panel in the process, so a file-local record is the same
/// state under another name (gallery / leaderboard do the same).
struct ThumbDrag {
    bool active = false;
    double startY = 0;
    double startOffset = 0;
};

ThumbDrag& thumbDrag() {
    static ThumbDrag drag;
    return drag;
}

/// A whole number with no separators, for the ALT variant of a stat row. The
/// browser prints the raw value there; commas would be a second abbreviation.
std::string exactNumber(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.0f", value);
    return buffer;
}

std::string tooltipStatNumber(double value) {
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "%.6f", value);
    std::string number = buffer;
    if (const auto decimal = number.find('.'); decimal != std::string::npos) {
        const auto last = number.find_last_not_of('0');
        number.resize(last == decimal ? decimal : last + 1);
    }
    return number == "-0" ? "0" : number;
}

/// A fraction as a percentage to a tenth, the tenth dropped when it is zero:
/// a golden leaf's 17.5% would round to a figure florr never prints.
std::string percentNumber(double fraction) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%g", std::round(fraction * 1000.0) / 10.0);
    return buffer;
}

/// One decimal, dropped when it is zero: a blueberry's 1.2 mana would read as
/// 1 through abbreviate().
std::string oneDecimal(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1f", value);
    std::string text = buffer;
    if (text.size() > 2 && text.compare(text.size() - 2, 2, ".0") == 0) text.erase(text.size() - 2);
    return text;
}

/// Seconds as florr's header prints them: always one decimal, "2.0s".
std::string reloadSeconds(double millis) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.1fs", millis / 1000.0);
    return buffer;
}

/// florr's colour markup around `text`, for a TooltipLine with `markup` set.
std::string colourMarkup(const std::string& text, std::uint32_t rgb) {
    char open[24];
    std::snprintf(open, sizeof open, "<c val=\"#%06X\">", rgb & 0xFFFFFFu);
    return open + text + "</c>";
}

} // namespace

namespace ui {

void showTimedHammerExpiry(std::vector<TooltipLine>& lines, const Profile& profile, int slot) {
    const double now = renderClockMillis();
    double soonest = 0.0;
    int copies = 0;
    for (const auto& lease : profile.timedHammerLeases) {
        if (lease.slot != slot || lease.readyAtMillis <= now) continue;
        if (soonest == 0.0 || lease.readyAtMillis < soonest) soonest = lease.readyAtMillis;
        ++copies;
    }
    if (copies == 0) return;

    const auto seconds = static_cast<unsigned>(std::ceil((soonest - now) / 1000.0));
    char clock[32];
    std::snprintf(clock, sizeof clock, "%u:%02u", seconds / 60, seconds % 60);
    const char* label = copies > 1 ? trEnZh("Next hammer expires in ", "下一把到期还剩 ")
                                   : trEnZh("Time left ", "剩余时间 ");
    lines.insert(lines.begin() + std::min<std::size_t>(1, lines.size()),
                 TooltipLine(std::string(label) + clock));
}

/// Name / tier / description / HP / damage, in the shared petal tooltip's
/// order. HP and damage carry the account's skill tiers, since that is the
/// number the player will actually see in the field, and the reload carries
/// `reloadScale` -- the Reload talent and the bar's golden leaves -- for the
/// same reason.
std::vector<TooltipLine> equippedPetalTooltipLines(std::uint16_t originalIndex,
        const EquippedPetal& equipped, const SkillSet& skills, double reloadScale, int slot, double triangleBonus, double slotCounter) {
    const auto effectiveStats = equippedPetalStats(content(), equipped);
    const auto& config = content().petal(equipped.configIndex);
    const bool copiedSawblade = originalIndex != equipped.configIndex && config.id == "sawblade";
    const bool showHyperTechnique = samePetalTier(equipped.reloadRarity, equipped.rarity) && !copiedSawblade;
    auto lines = petalTooltipLines(equipped.configIndex, equipped.rarity, skills, reloadScale,
                                   slot, triangleBonus, showHyperTechnique, effectiveStats.reloadMillis);
    if (slotCounter >= 0 && config.id == "magic_bone") {
        TooltipLine bonus{trEnZh("Current bone multiplier (wild mobs / training dummies): ",
                                "当前骨头倍率（野怪／训练木桩）：") +
                          oneDecimal(1.0 + slotCounter / 100.0) + "x", 12.0};
        bonus.maxWidth = kTooltipWrapWidth;
        lines.push_back(std::move(bonus));
    }
    if (!lines.empty() && slotCounter >= 0 &&
        (isPermanentSandstormStick(config) || config.id == "arctic_snow_stick")) {
        const int wanted = configuredSummonCount(config, equipped.rarity, &skills, slot);
        TooltipLine summons{trEnZh("Summons here: ", "当前地图召唤物：") +
                            std::to_string(slotCounter) + "/" + std::to_string(wanted), 12.0};
        lines.insert(lines.begin() + 1, std::move(summons));
    }
    if (originalIndex != equipped.configIndex && !lines.empty()) {
        const auto& original = content().petal(originalIndex);
        const std::string enName = titleCase(original.name);
        lines.front().text = petalNameTr(original.id, enName.c_str());
        if (copiedSawblade) {
            lines.emplace_back(trEnZh("Effective copy: Sawblade", "实际复制效果：锯片"));
        }
    }
    return lines;
}

std::vector<TooltipLine> petalTooltipLines(std::uint16_t petalIndex, Rarity rarity,
                                           const SkillSet& skills, double reloadScale, int slot, double triangleBonus,
                                           bool showHyperTechnique, std::optional<double> baseReloadMillis) {
    const PetalConfig& config = content().petal(petalIndex);
    const PetalStats stats = content().petalStats(petalIndex, rarity);
    const bool fragment = config.id == "fragment";
    const auto* technique = showHyperTechnique && !fragment ? hyperTechniqueInfo(config.id, rarity) : nullptr;
    const double chargeScale = hyperAmount(technique, HyperTechnique::QuickCharge);
    const double health = std::round(stats.health > 1.0 ? stats.health * skills.healthScale(SkillId::PetalHealth) : stats.health);
    const double damageScale = skills.effectScale(SkillId::Damage);
    const double fireScale = skills.effectScale(SkillId::FirePower);
    const double baseDamage = petalContactDamage(config, stats, rarity);
    const double lightningScale = config.lightningDamage ? skills.effectScale(SkillId::LightningPower) : 1.0;
    const double damage = std::round((baseDamage + triangleBonus) * damageScale * lightningScale);

    std::vector<TooltipLine> lines;
    // florr's header: on the name's right, what bringing the petal out costs
    // in mana and how long its reload is, then the wait between spawning and
    // acting -- a rose's heal, an orb's mana -- when it has one ("3.5s +
    // 1.5s"). A worn petal has no reload and florr prints nothing for it.
    const std::string enName = titleCase(config.name);
    TooltipLine title{petalNameTr(config.id, enName.c_str()), 20.0};
    if (!config.noPhysics) {
        if (stats.requiredMana > 0.0) {
            // Paid on spawn, not on use -- see PetalConfig::requiredMana.
            title.badges.push_back({oneDecimal(stats.requiredMana), kStatMana, TooltipIcon::Mana});
            title.badges.push_back({" +"});
        }
        const double reloadMillis = ordinaryPetalReloadMillis(baseReloadMillis.value_or(stats.reloadMillis), reloadScale);
        std::string reload = reloadMillis < 100.0
            ? std::to_string(static_cast<int>(std::round(reloadMillis))) + "ms"
            : reloadSeconds(std::round(reloadMillis / 100.0) * 100.0);
        const double charge = stats.mana > 0.0                         ? stats.manaChargeMillis
                              : (stats.heal > 0.0 || stats.shield > 0.0) ? stats.healChargeMillis
                                                                         : 0.0;
        if (charge > 0.0) reload += " + " + reloadSeconds(charge * chargeScale);
        title.badges.push_back({reload, kPaper, TooltipIcon::Reload});
    }
    lines.push_back(title);
    lines.push_back({rarityTr(rarity), 14.0, rarityColor(rarity)});
    if (!config.description.empty()) {
        // florr's descriptions, from its English table, colour markup and
        // all. "{rarity}" is the card's own tier in its colour, as florr's
        // {0:rarity} is: a champion's crown hands out a Basic of whatever tier
        // the crown is.
        std::string description = petalDescTr(config.id, config.description.c_str());
        if (config.id == "mjolnir" && rarity == Rarity::Ascendant) {
            const std::string cost = std::to_string(kApexHammerForgeCost);
            const std::string refund = std::to_string(hyperForgeRefund(kApexHammerForgeCost));
            description = language() == Language::Zh
                ? "Apex 争夺成品。锻造消耗 " + cost + " 个 Apex 碎片，被夺返还 " + refund
                    + " 个。首保 15 小时，每次被夺后缩短 1 小时，最低 1 小时。接触时落下单目标雷击。"
                : "Contested Apex reward. Forging costs " + cost + " Apex fragments and returns "
                    + refund + " if taken. Protection starts at 15 hours, dropping one hour per takeover "
                      "to a minimum of one hour. Contact strikes one target with lightning.";
        } else if (config.id == "paranormal_goggles") {
            const double zoom = hyperAmount(technique, HyperTechnique::GhostSight, 1.25);
            const std::string loss = std::to_string(std::lround(100.0 * (1.0 - 1.0 / zoom)));
            description = language() == Language::Zh
                ? "装备在主栏可看见并攻击同品质或更低品质的幽灵。可见范围的宽和高各缩小约 " + loss + "%。"
                : "Equip on the active bar to see and hit ghosts at this rarity or below. "
                  "Visible width and height are about " + loss + "% smaller.";
        } else if (config.id == "laser") {
            const bool pulses = technique && technique->technique == HyperTechnique::LaserPulse;
            if (language() == Language::Zh) {
                description = pulses ? "造成接触伤害和连续闪电光束伤害，攻击时还会额外发射短促脉冲。"
                                     : "造成接触伤害，并持续向外发射闪电光束。";
            } else {
                description = pulses ? "Deals contact damage and fires a continuous lightning beam. Attacking also fires a brief pulse."
                                     : "Deals contact damage and fires a continuous lightning beam.";
            }
        }
        if (fragment) {
            const bool zh = language() == Language::Zh;
            if (rarity == Rarity::PermanentHyper) {
                description = zh
                    ? "普通合成：每次投入 5 个永久超凡碎片，2.5% 概率获得 1 个 Apex 碎片。\n海洋神使：120 个永久超凡碎片必换 1 个 Apex 碎片。"
                    : "Crafting: offer 5 Permanent Hyper Fragments per attempt, with a 2.5% chance for 1 Apex Fragment.\nOcean Oracle: trade 120 Permanent Hyper Fragments for 1 guaranteed Apex Fragment.";
            } else if (rarity == Rarity::Ascendant) {
                description = zh
                    ? "泰坦的 Apex 雷神锤页签：5 个未装备的 Apex 碎片锻造 1 把 Apex 雷神之锤。"
                    : "At the Titan's Apex Hammer tab, forge 1 Apex Mjolnir with 5 unequipped Apex Fragments.";
            } else if (rarity == Rarity::Unique) {
                description = zh
                    ? "泰坦：10 个永恒碎片参与超凡雷神之锤争夺，或用 30 个永恒碎片兑换 1 个永久超凡碎片。"
                    : "At the Titan, use 10 Eternal Fragments to contest Hyper Mjolnir or trade 30 for 1 Permanent Hyper Fragment.";
            } else if (rarity == Rarity::Super) {
                description = zh
                    ? "泰坦：15 个超级碎片兑换 1 小时限时永恒雷神之锤，或兑换 1 个永恒碎片。"
                    : "At the Titan, trade 15 Super Fragments for a 1-hour Eternal Mjolnir or 1 Eternal Fragment.";
            } else {
                description = zh ? "泰坦锻造材料。" : "Titan forging material.";
            }
        }
        if (config.id == "triangle") {
            description = language() == Language::Zh
                ? "主栏每多装备一片同稀有度的三角，该档每片增加自身 80% 的基础伤害。"
                : "Each extra same-tier Triangle on the active bar adds 80% of base damage to each Triangle of that tier.";
        }
        if (petalLendsBasic(config)) {
            const auto basicRarity = crownBasicRarity(rarity);
            const std::string tier = colourMarkup(rarityTr(basicRarity), rarityColor(basicRarity));
            const std::string count = std::to_string(crownBasicCount(rarity));
            description = language() == Language::Zh
                ? "为自己和附近花朵各附加 " + count + " 枚" + tier +
                      "品质的基本花瓣。\n这些花瓣造成的伤害计入你的贡献。"
                : "Adds " + count + " × " + tier +
                      " Basic to your ring and each nearby flower's ring.\nYou receive credit for their damage.";
        }
        static const std::string kRaritySlot = "{rarity}";
        for (std::size_t at = description.find(kRaritySlot); at != std::string::npos;
             at = description.find(kRaritySlot, at)) {
            const std::string tier = colourMarkup(rarityTr(rarity), rarityColor(rarity));
            description.replace(at, kRaritySlot.size(), tier);
            at += tier.size();
        }
        TooltipLine body{description, 12.0};
        body.gapBefore = 10.0;
        body.maxWidth = kTooltipWrapWidth;
        body.markup = true;
        lines.push_back(body);
    }

    if (computerWeapon(config.id)) {
        const bool zh = language() == Language::Zh;
        const auto addSetLine = [&](const char* text) {
            TooltipLine line{text, 12.0, kPaper};
            line.maxWidth = kTooltipWrapWidth;
            lines.push_back(line);
        };
        addSetLine(zh ? "电脑套装：主装备栏中，同阶或更高阶的组件按类别计数。"
                      : "Computer set: count distinct part types of this tier or higher on the active bar.");
        addSetLine(zh ? "六类组件：程序、内存、芯片、电容器、光标、几何体。"
                      : "Six part types: software, RAM, chip, capacitor, cursor, and geometry.");
        addSetLine(zh ? "3 类：基础伤害 +12%；4 类：额外增加 15% 耐久（原耐久须大于 1）；5 类：再缩短 10% 重装。"
                      : "3 types: +12% base damage. 4: also +15% durability if above 1. 5: also 10% shorter reload.");
        if (!computerPart(config.id)) {
            addSetLine(zh ? "这枚花瓣享受套装加成，自身不计作组件。"
                          : "This petal receives set bonuses and contributes no part type.");
        }
    }

    if (technique && technique->technique != HyperTechnique::GhostSight) {
        const bool zh = language() == Language::Zh;
        TooltipLine special{std::string(zh ? "超凡机制 · " : "Hyper technique · ") +
                            (zh ? technique->chinese : technique->english), 12.0, kPaper};
        special.gapBefore = 6.0;
        special.maxWidth = kTooltipWrapWidth;
        lines.push_back(special);
        TooltipLine hint{std::string(zh ? "神秘提示 · " : "Mysterious hint · ") +
                         hyperMysteryHint(technique->technique, zh), 12.0, kPaper};
        hint.alpha = 0.78;
        hint.maxWidth = kTooltipWrapWidth;
        lines.push_back(hint);
    }

    // Dimmed to gardn's 0xffffff90 rather than a pre-mixed grey, and spaced
    // off the description when there is one, off the tier row when there is not.
    TooltipLine hp{std::string(tipHp()) + abbreviate(health), 12.0, kPaper,
                   config.description.empty() ? 10.0 : 4.0};
    hp.alpha = 0.56;
    hp.labelColor = kStatHealth;
    hp.altText = std::string(tipHp()) + exactNumber(health);
    if (!stats.breakable) { hp.text = std::string(tipHp()) + (language() == Language::Zh ? "不可破坏" : "Unbreakable"); hp.altText = hp.text; }
    lines.push_back(hp);

    const std::string damageLabel = config.id == "flower"
        ? (language() == Language::Zh ? "花瓣接触伤害：" : "Petal contact damage: ")
        : config.id == "battery" || config.id == "lightning" ||
        config.id == "mecha_missile" || config.id == "magic_peas" || petalIsLaser(config)
        ? (language() == Language::Zh ? "接触伤害：" : "Contact damage: ") : tipDamage();
    TooltipLine hit{damageLabel + abbreviate(damage), 12.0};
    hit.alpha = 0.56;
    hit.labelColor = kStatDamage;
    hit.altText = damageLabel + exactNumber(damage);
    const bool magicTomato = config.id == "magic_tomato";
    if (petalGrowsOverTime(config) || magicTomato) {
        const double grown = std::round(damage * (magicTomato ? kMagicTomatoGrowth : kTomatoMaxGrowth));
        hit.text = damageLabel + abbreviate(damage) + " ~ " + abbreviate(grown);
        hit.altText = damageLabel + exactNumber(damage) + " ~ " + exactNumber(grown);
    }
    // Blueberries land every hit as lightning: florr's
    // Petal/Attribute/Damage/Lightning, "Damage: X (lightning)" with the
    // note in lightning cyan.
    if (config.lightningDamage) {
        const std::string label = colourMarkup(damageLabel, kStatDamage);
        const std::string note = " " + colourMarkup(tipLightning(), kStatLightning);
        hit.text = label + " " + abbreviate(damage) + note;
        hit.altText = label + " " + exactNumber(damage) + note;
        hit.markup = true;
    }
    lines.push_back(hit);

    // Wiki Laser: beam DPS is lightning, separate from emitter contact, 10×.
    if (petalIsLaser(config)) {
        const double beam = laserBeamDamagePerSecond(stats);
        const std::string beamLabel = language() == Language::Zh ? "光束伤害：" : "Beam damage: ";
        const std::string note = " " + colourMarkup(tipLightning(), kStatLightning);
        TooltipLine beamLine{beamLabel + abbreviate(beam) + "/s" + note, 12.0};
        beamLine.alpha = 0.56;
        beamLine.labelColor = kStatLightning;
        beamLine.altText = beamLabel + exactNumber(beam) + "/s" + note;
        beamLine.markup = true;
        lines.push_back(beamLine);
    }
    if (config.projectile.present) {
        const bool zh = language() == Language::Zh;
        lines.push_back({zh ? "发射物伤害加成：宠物伤害天赋。" :
            "Projectile damage scales with the Pet Damage talent.", 12.0});
        lines.push_back({zh ? "耐久天赋提高环上花瓣耐久；已发射物使用该阶基础耐久。" :
            "Health talents affect the ring petal. Shots use the tier's base health.", 12.0});
    } else if (config.petMobIndex == kInvalidIndex && stats.damage > 0.0) {
        lines.push_back({language() == Language::Zh ? "接触伤害加成：花瓣伤害天赋。" :
            "Contact damage scales with the Petal Damage talent.", 12.0});
    }

    // Burst / passive heal: field value includes kPlayerHealMultiplier (and the
    // Healing talent for the amount the flower actually receives).
    const double healTalent = skills.effectScale(SkillId::Medic);
    if (stats.heal > 0.0) {
        const double heal = std::round(stats.heal * healTalent * kPlayerHealMultiplier);
        TooltipLine line{std::string(tipHeal()) + abbreviate(heal), 12.0};
        line.alpha = 0.56;
        line.labelColor = kStatHeal;
        line.altText = std::string(tipHeal()) + exactNumber(heal);
        lines.push_back(line);
    }
    if (stats.passiveHealPerSecond > 0.0) {
        const double heal =
            std::round(stats.passiveHealPerSecond * healTalent * kPlayerHealMultiplier * 100.0) /
            100.0;
        TooltipLine line{std::string(tipHealPerSec()) + abbreviate(heal), 12.0};
        line.alpha = 0.56;
        line.labelColor = kStatHeal;
        line.altText = std::string(tipHealPerSec()) + oneDecimal(heal);
        lines.push_back(line);
    }
    if (healTalent * kPlayerHealMultiplier != 1.0 && (stats.heal > 0 || stats.passiveHealPerSecond > 0)) {
        const bool zh = language() == Language::Zh;
        const double base = stats.heal > 0 ? stats.heal : stats.passiveHealPerSecond;
        lines.push_back({std::string(zh ? "基础治疗：" : "Base healing: ") + exactNumber(base) +
            (stats.heal > 0 ? "" : (zh ? "/秒" : "/s")),12.0});
        lines.push_back({std::string(zh ? "本服治疗倍率：x" : "Server healing multiplier: x") + oneDecimal(kPlayerHealMultiplier),12.0});
        if (healTalent != 1) lines.push_back({std::string(zh ? "治疗天赋倍率：x" : "Healing talent: x") + oneDecimal(healTalent),12.0});
    }
    if (stats.shield > 0.0) {
        const double shield = std::round(stats.shield);
        TooltipLine line{std::string(tipShield()) + abbreviate(shield), 12.0};
        line.alpha = 0.56;
        line.labelColor = kStatGeneric;
        line.altText = std::string(tipShield()) + exactNumber(shield);
        lines.push_back(line);
    }
    if (stats.poisonPerSecond > 0.0) {
        const double pps = std::round(stats.poisonPerSecond);
        const double secs = stats.poisonDurationMillis / 1000.0;
        const char* sep = language() == Language::Zh ? "/秒 · " : "/s · ";
        TooltipLine line{std::string(tipPoison()) + abbreviate(pps) + sep + exactNumber(secs) + "s",
                         12.0};
        line.alpha = 0.56;
        line.labelColor = kStatPoison;
        line.altText =
            std::string(tipPoison()) + exactNumber(pps) + sep + exactNumber(secs) + "s";
        lines.push_back(line);
        const bool zh = language() == Language::Zh;
        const bool stacks = contactPoisonStacksByPetal(config.id);
        lines.push_back({stacks
            ? (zh ? "接触毒对怪：不同瓣体可叠加，同一瓣体续时。" : "Contact poison on mobs stacks across petals. The same petal refreshes.")
            : (zh ? "接触毒对怪：同一玩家共用一层，持续更久的替换。" : "Contact poison on mobs shares a layer per player. A later expiry replaces it."), 12.0});
        lines.push_back({zh ? "对玩家：新毒替换旧毒。" : "On players, new poison replaces existing poison.", 12.0});
    }

    // Report the same effective numbers the server uses, including passive
    // effects that do not belong to the petal's contact damage or HP.
    const bool chinese = language() == Language::Zh;
    const auto special = [&](const char* zhLabel, const char* enLabel, double value,
                             const std::string& suffix = "", const char* format = nullptr) {
        char number[32];
        if (format) std::snprintf(number, sizeof number, format, value);
        TooltipLine line{std::string(chinese ? zhLabel : enLabel) + (format ? std::string(number) : tooltipStatNumber(value)) + suffix, 12.0};
        line.alpha = 0.72;line.labelColor = kStatGeneric;lines.push_back(line);
    };
    if (const double upkeep = wornMagicManaPerSecond(config.id, rarity); upkeep > 0.0) {
        special("持续法力消耗：", "Mana upkeep: ", upkeep, "/s", "%.8g");
        lines.push_back({chinese ? "装备时持续生效，破碎期间也会耗蓝。蓝量不足时暂停效果。"
                                 : "Works while equipped, including during reload. Pauses when mana is too low.", 12.0});
    }
    if (config.id == "magic_marble") {
        special("每颗弹珠最多承受的命中次数：", "Maximum hits before shattering: ",
                kMagicMarbleImpactsBeforeShatter);
    }
    if (stats.flowerHealthBonus > 0) special("增加花朵生命：+", "Flower health: +", stats.flowerHealthBonus);
    if (magicTomato) {
        char cost[32];
        std::snprintf(cost, sizeof cost, "%g", magicTomatoRipeningMana(rarity));
        lines.push_back({std::string(chinese ? "催熟消耗法力：" : "Mana to ripen: ") + cost, 12.0});
    }
    if (config.id == "palm_leaf" || config.id == "magic_palm_leaf") {
        char rate[32];
        std::snprintf(rate, sizeof rate, "%g", palmLeafShieldPerSecond(rarity));
        TooltipLine line{std::string(chinese ? "回盾/秒：" : "Shield restored per second: ") + rate, 12.0};
        line.alpha = 0.72;
        line.labelColor = kStatGeneric;
        lines.push_back(line);
        special("回盾上限（最大生命）：", "Shield recovery cap (% of max health): ",
                100.0 * kPalmLeafShieldCapFraction, "%");
    }
    if (config.passivePetHeal > 0.0) {
        special("每只召唤物回血/秒：", "Healing per summon per second: ",
                passivePetHealAmount(config, rarity) * 1000.0 / kPassivePetHealIntervalMillis);
    }
    if (config.id == "magic_crystal_leaf") {
        special("每只召唤物护盾：", "Shield per summon: ", crystalLeafShieldAmount(rarity));
        special("护盾上限（召唤物最大生命）：", "Shield cap (% of summon max HP): ",
                kCrystalLeafShieldCap * 100.0, "%");
        special("每次补盾耗蓝：", "Mana per shield refresh: ", crystalLeafShieldMana(rarity));
        special("护盾持续：", "Shield duration: ", kSupportShieldDurationMillis / 1000.0, "s");
    }
    if (config.id == "magic_chromosome") {
        special("每片花瓣单份护盾：", "Shield per petal per charge: ", chromosomeShieldAmount(rarity));
        special("每份护盾耗蓝：", "Mana per shield charge: ", chromosomeShieldMana(rarity));
        special("每次最多发放份数：", "Maximum charges per pulse: ", 2);
        special("护盾上限（花瓣最大生命）：", "Shield cap (% of petal max HP): ",
                kChromosomeShieldCap * 100.0, "%");
        special("护盾持续：", "Shield duration: ", kSupportShieldDurationMillis / 1000.0, "s");
    }
    if (config.id == "chloroplast")
        special("护盾补充量：+", "Shield gain: +", chloroplastShieldBonus(rarity) * 100.0, "%");
    if (config.id == "stem_cell") {
        special("每次跨生命阈值的基础回血概率：", "Base heal chance per HP threshold: ",
                stemCellHealChance(rarity) * 100.0, "%");
        special("回血概率上限：", "Heal chance cap: ", kStemCellHealChanceCap * 100.0, "%");
    }
    if (config.id == "dna") {
        special("单片每次孵化基础突变概率：", "Base mutation chance per hatch with one DNA: ",
                kDnaChancePerSource * 100.0, "%");
    }
    if (config.id == "oxygen" || config.id == "void_cyanide") {
        if (config.id == "oxygen")
            special("每次延长已有灼烧：", "Existing burn extension per hit: ",
                    oxygenBurnExtensionMillis(rarity) / 1000.0, "s");
        else
            special("每次延长已有中毒：", "Existing poison extension per hit: ",
                    cyanidePoisonExtensionMillis(rarity) / 1000.0, "s");
        special("延长后的剩余时间上限：", "Extension cap on remaining time: ",
                kAfflictionExtensionCapMillis / 1000.0, "s");
    }
    if (config.id == "monstera") {
        special("范围治疗（每秒）：", "Area healing per second: ",
                monsteraHealPerSecond(rarity) * healTalent * kPlayerHealMultiplier);
        special("治疗半径：", "Healing radius: ", monsteraHealRadius(rarity));
    }
    if (config.id == "rna") {
        char bonus[16];
        std::snprintf(bonus, sizeof bonus, "%g%%", rnaPetSpeedBonus(rarity) * 100.0);
        TooltipLine line{std::string(chinese ? "宠物移速：+" : "Pet movement speed: +") + bonus, 12.0};
        line.alpha = 0.72;
        line.labelColor = kStatGeneric;
        lines.push_back(line);
    }
    if (config.id == "iodine") {
        special("每跳基础治疗：", "Base healing per pulse: ",
                iodineHealAmount(rarity) * kPlayerHealMultiplier);
        special("治疗间隔：", "Healing interval: ", kIodineHealIntervalMillis / 1000.0, "s");
        special("治疗半径：", "Healing radius: ", iodineHealRadius(rarity));
    }
    if(config.burrowDurationMillis>0){
        special("潜地准备：", "Burrow charge: ",config.burrowChargeMillis/1000.0,"s");
        special("潜地持续：", "Burrow duration: ",config.burrowDurationMillis/1000.0,"s");
        lines.push_back({chinese?"准备后按防御潜地；沙漠蜈蚣仍可命中你。墙体依然阻挡移动。":"Defend after charging to burrow. Desert Centipedes can still hit you. Walls remain solid.",12.0});
    }
    if(config.id=="arctic_nuke") {
        special("整套爆炸基础总预算：", "Total base blast budget: ",210.0 * referenceOffenseScale(config, rarity));
        lines.push_back({chinese?"主爆炸约占总伤害的64%，其余由外围子爆炸平分。子爆炸数量增加时，整套总伤害不变。":"The main blast deals about 64% of the total damage. The sub-blasts share the rest. More sub-blasts leave the total damage unchanged.",12.0});
    }
    const double explosionReach = skills.statScale(SkillId::ExplosionReach);
    if (config.blastRadiusPerTier > 0.0) {
        special("单次爆炸基础伤害：", "Base damage per blast: ",
                config.blastDamage * referenceOffenseScale(config, rarity));
        if (config.blastChance < 1.0)
            special("爆炸概率：", "Explosion chance: ", config.blastChance * 100.0, "%");
        if (!isFireBomb(config)) {
            const double radius = config.blastRadiusPerTier * (1 + ladderIndex(rarity));
            special("爆炸半径（含天赋）：", "Blast radius with talents: ", radius + explosionReach);
        }
    }
    if (config.burnPerSecond > 0.0 && config.burnMillis > 0.0) {
        special("基础灼烧伤害/秒：", "Base burn damage per second: ", petalBaseBurnRate(config, rarity));
        special("灼烧持续：", "Burn duration: ", config.burnMillis / 1000.0, "s");
    }
    if (isFireBomb(config)) {
        char burnRate[32];
        std::snprintf(burnRate, sizeof burnRate, "%.8g",
                      fireBombBaseBurnRate(config) * referenceOffenseScale(config, rarity));
        TooltipLine burn{std::string(chinese ? "每层基础燃烧伤害/秒：" : "Base burn damage per second per stack: ") +
                         burnRate, 12.0};
        burn.alpha = 0.72;
        burn.labelColor = kStatGeneric;
        lines.push_back(burn);
        special("燃烧持续时间：", "Burn duration: ", kFireBombBurnMillis / 1000.0, "s");
        special("燃烧半径：", "Burn radius: ", fireBombRadius(config, rarity) + explosionReach);
        if (config.id != "magic_void_fire_bomb")
            special("同一玩家对单个目标的叠火上限：", "Max stacks per player on one target: ",
                    kFireBombBurnLayers);
    }
    if (config.spill.kind != SpillKind::None) {
        special("基础最大半径：", "Base maximum pool radius: ", spillBaseRadius(config.spill, rarity), "", "%.8g");
        if (config.spill.growthPerSecond(rarity) > 0.0)
            special("基础扩散速度：", "Base radial growth: ", config.spill.growthPerSecond(rarity), "/s", "%.8g");
        const double radius = spillBaseRadius(config.spill, rarity);
        if (const double reach = skills.effectScale(SkillId::FluidReach); reach > 1.0)
            special("流体半径（仅计天赋）：", "Spill radius with talent only: ", radius * reach);
        special("液池持续：", "Pool lifetime: ", spillLifetimeMillis(config.spill) / 1000.0, "s");
        special("每朵花同时铺池上限：", "Active pools per flower: ", kFluidPoolLimit);
        if (!config.spill.onHit && config.spill.chargeMillis > 0.0) {
            const double rate = skills.effectScale(SkillId::SpillRate) * skills.effectScale(SkillId::SecondaryReload);
            special("铺池准备时间：", "Pool charge time: ",
                    std::max(config.spill.chargeMillis / rate, net::kTickMillis) / 1000.0, "s");
        }
        if (config.id == "acid" || config.id == "lava" ||
            config.id == "acid_bomb" || config.id == "lava_bomb") {
            TooltipLine trigger{config.spill.onHit
                ? (chinese ? "命中野怪或训练木桩时，在目标脚下铺池。"
                           : "Hitting a wild mob or practice dummy creates a pool at its feet.")
                : (chinese ? "花瓣出现并准备完成后，按攻击或防御在花瓣所在处铺池。提前破损时没有池。"
                           : "After the petal spawns and charges, attack or defend to place a pool at the petal. Breaking early leaves no pool."),
                12.0};
            trigger.maxWidth = kTooltipWrapWidth;
            lines.push_back(trigger);
            TooltipLine reach{chinese
                ? "虚空肥皂加快液池向外扩散。流体范围天赋扩大覆盖。"
                : "Void Soap speeds up spreading. Fluid Reach scales the pool coverage.",
                12.0};
            reach.maxWidth = kTooltipWrapWidth;
            lines.push_back(reach);
        }
        if (config.spill.kind == SpillKind::Elixir) {
            special("液池基础治疗/秒：", "Base pool healing per second: ",
                    config.spill.strength * petalHealScale(rarity));
        } else if (config.spill.kind != SpillKind::Blood) {
            const double scale = config.spill.onHit || config.spill.kind == SpillKind::Nitrogen
                ? petalStrengthScale(rarity) : referenceOffenseScale(config, rarity);
            const double basePoolDamage = config.spill.strength * scale * fluidDamageScale(config.spill.kind);
            special("液池基础伤害/秒：", "Base pool damage per second: ", basePoolDamage);
            if (technique && technique->technique == HyperTechnique::AcidPoolBoost) {
                special("专属酸池伤害/秒：", "Pool damage per second with technique: ",
                        basePoolDamage * technique->amount);
                TooltipLine duration{std::string(chinese ? "酸池持续时间：" : "Pool duration: ") +
                                     reloadSeconds(spillLifetimeMillis(config.spill)), 12.0};
                duration.alpha = 0.72;
                duration.labelColor = kStatGeneric;
                lines.push_back(duration);
            }
        }
    }
    if (config.id == "void_soap") {
        special("前两块各自增加液池半径：+", "Pool radius per soap for the first two: +", soapCoverageBonus(rarity) * 100.0, "%");
        special("液池扩散速度：+", "Liquid spread speed: +", soapSpillRateBonus(rarity) * 100.0, "%", "%.8g");
        TooltipLine stacking{chinese ? "按肥皂强度排序。第三块起，依次获得各自半径加成的 1/2、1/3、1/4……扩散速度直接相加。"
                                    : "Strongest soaps count first. From the third soap onward, radius bonuses contribute 1/2, 1/3, 1/4, and so on. Spread speed adds up.", 12.0};
        stacking.maxWidth = kTooltipWrapWidth;
        lines.push_back(std::move(stacking));
    }
    if (config.id == "spikeball" || config.id == "arctic_pinecone") {
        const double burst = stats.damage * damageScale;
        special("每次散射针数：", "Needles per burst: ", spikeballNeedleCount(config));
        special("单针伤害：", "Damage per needle: ", burst / spikeballNeedleCount(config));
        special("全针命中合计：", "All needles combined: ", burst);
    }
    if (config.id == "opal" || config.id == "void_opal" || config.id == "ruby") {
        lines.push_back({chinese ? "主栏被动：破碎装填时仍生效，移出主栏后结束。"
                                 : "Active-slot passive: stays active during reload and ends when removed.", 12.0});
    }
    if (stats.flowerRadiusBonus > 0) special("增加花朵半径：+", "Flower radius: +", stats.flowerRadiusBonus);
    if (stats.summonRangeBonus > 0) special("召唤物追击范围：+", "Summon pursuit range: +", stats.summonRangeBonus);
    if (stats.passiveManaDrain > 0) special("每秒法力消耗：", "Mana drain per second: ", stats.passiveManaDrain);
    if (!petalSharesFlowerDamage(config) && stats.modifiers.maxHealth != 1.0)
        special("最大生命：", "Max health: ", (stats.modifiers.maxHealth - 1.0) * 100.0, "%");
    if (config.id == "void_powder") {
        const double speed = stats.modifiers.speed * (1.0 + voidPowderFlatSpeed(rarity) / kPlayerMaxSpeed);
        special("单枚移速加成：+", "Speed bonus with one equipped: +", (speed - 1.0) * 100.0, "%", "%.1f");
    } else if (stats.modifiers.speed != 1.0) {
        special("移速变化：", "Movement speed: ", (stats.modifiers.speed - 1.0) * 100.0, "%");
    }
    if (stats.modifiers.playerRadius != 1.0)
        special("花朵半径变化：", "Flower radius: ", (stats.modifiers.playerRadius - 1.0) * 100.0, "%");
    if (stats.modifiers.rotationSpeed != 1.0) {
        const double spinBonusPercent = (stats.modifiers.rotationSpeed - 1.0) * 100.0;
        special("旋转速度变化：", "Rotation speed: ", spinBonusPercent, "%");
        if (technique && technique->technique == HyperTechnique::FastOrbit) {
            special("攻击时旋转速度变化：", "Rotation speed while attacking: ",
                    spinBonusPercent * hyperAmount(technique, HyperTechnique::FastOrbit), "%");
        }
    }
    if (config.id == "dead_leaf") {
        special("花瓣耐久：+", "Petal durability: +", deadLeafDurabilityBonus(rarity) * 100.0, "%");
        lines.push_back({chinese ? "重装时间 +5%，耐久与金仙人掌只取最高加成。"
                                 : "Reload time +5%. Only the strongest durability bonus from Dead Leaf or Golden Cactus applies.", 12.0});
    }
    if (stats.modifiers.attackRange > 0) special("攻击范围：+", "Attack range: +", stats.modifiers.attackRange);
    if (config.id == "ring_magnet") {
        special("牵引半径：", "Pull radius: ", stats.modifiers.magnetism);
        lines.push_back({chinese ? "普通磁铁扩大直接拾取范围；戒指的牵引半径单独计算。"
                                 : "Normal magnets extend direct pickup range. The ring has its own pull radius.", 12.0});
    } else if (stats.modifiers.magnetism > 0) {
        special("拾取范围：+", "Pickup range: +", stats.modifiers.magnetism);
    }
    if (config.id == "pharaoh_s_crown") {
        const auto summonRarity = pharaohCrownSummonLabelRarity(rarity);
        lines.push_back({std::string(chinese ? "召唤物稀有度：" : "Summon rarity: ") +
                         rarityTr(summonRarity), 12.0, rarityColor(summonRarity)});
        const auto mummy = content().mobIndex("mummy_beetle");
        if (mummy != kInvalidIndex) {
            const auto stats = pharaohCrownCombatStats(config, content().mob(mummy), rarity, &skills);
            special("召唤物生命：", "Summon health: ", stats.health);
            special("召唤物接触伤害：", "Summon contact damage: ", stats.contactDamage);
        }
    }
    if (config.petMobIndex != kInvalidIndex) {
        const auto& summon=content().mob(config.petMobIndex);
        const bool apex=ladderRarity(rarity)==Rarity::Apex;
        const auto summonRarity = configuredSummonRarity(config, rarity);
        const double summonScale = petalStatScale(summonRarity) * configuredSummonStrength(config, rarity, &skills, slot) *
            eggSummonCombatScale(config) * hyperSummonPromotionScale(config, summonRarity);
        TooltipLine pet{std::string(chinese ? "召唤：" : "Summons: ") +
            mobNameTr(content().mob(config.petMobIndex).id, content().mob(config.petMobIndex).name.c_str()) +
            " x" + std::to_string(configuredSummonCount(config, rarity, &skills, slot)),12.0};lines.push_back(pet);
        special("召唤物生命：", "Summon health: ",summon.health*summonScale*summon.petHealthScale*kSummonedMobHealthScale*skills.healthScale(SkillId::Summoner)*eggSummonHealthScale(config)*ascendantSummonHealthScale(rarity));
        lines.push_back({std::string(chinese ? "召唤物稀有度：" : "Summon rarity: ") + rarityTr(summonRarity), 12.0});
        special("召唤物接触伤害：", "Summon contact damage: ",
                summon.damage * summonScale * summon.petDamageScale * summonDamageScale(config, rarity) * configuredSummonDamageScale(config, rarity) *
                skills.effectScale(SkillId::PetDamage));
        if (isPermanentSandstormStick(config) || config.id == "arctic_snow_stick") {
            TooltipLine use{trEnZh("Equip in an active slot to summon automatically. Leave room under the pet cap.",
                                  "放入主装备栏后自动召唤。召唤已满时，需要先腾出名额。"), 12.0};
            use.maxWidth = kTooltipWrapWidth;
            lines.push_back(std::move(use));
        }
        if(apex && petalIsEgg(config))lines.push_back({chinese?"H护甲：己方召唤物受到不高于蛋稀有度的野怪体伤时，单次最多损失35%最大生命；不抵挡玩家、毒素或技能。":"H escort armour: wild body hits up to the egg tier deal at most 35% of summon max health per hit; no protection from players, poison or abilities.",12.0});
        if(config.id=="egg"||config.id=="beetle_egg")lines.push_back({chinese?"蛋壳不造成接触伤害；蛋壳破碎不会收回已孵化召唤物。":"The shell deals no contact damage; breaking it does not recall hatched pets.",12.0});
        if (config.petLifetimeMillis > 0) special("召唤持续：", "Summon lifetime: ", config.petLifetimeMillis / 1000.0, "s");
        if (config.petSummonIntervalMillis > 0.0)
            special("召唤间隔：", "Summon interval: ", config.petSummonIntervalMillis / 1000.0, "s");
        if (config.petSummonTiers || config.id == "egg" || config.id == "beetle_egg") {
            const double authoredDelay = config.petSummonTiers ? baseReloadMillis.value_or(stats.reloadMillis) : 1000.0;
            const double incubation = authoredDelay * (config.petSummonTiers ? skills.reloadScale() : 1.0);
            const double rate = petalIsEgg(config) ? skills.effectScale(SkillId::SecondaryReload) : 1.0;
            special("孵化等待：", "Hatch delay: ", std::max(incubation / rate, net::kTickMillis) / 1000.0, "s");
        }
        if (summon.ai != AiKind::Sandstorm) lines.push_back({chinese
            ? "自动索敌优先主人 600 范围内的高阶怪。主人侧上限 1250，灯泡可提高。空闲离主人 900、战斗中 1650 后回跟。光标指挥优先。"
            : "Prioritises high-tier mobs within 600 of the owner. The owner's range cap is 1250 before Bulb. Returns beyond 900 while idle or 1650 in combat. Cursor commands take priority.",12.0});
        if(config.id=="egg"||config.id=="beetle_egg")lines.push_back({chinese?"孵化准备的1秒内蛋壳受保护，不会被战斗打断。":"The shell is protected during its one-second incubation.",12.0});
    }
    if (config.projectile.summonMobIndex != kInvalidIndex) {
        const auto& summon = content().mob(config.projectile.summonMobIndex);
        const double scale = petalStatScale(rarity) * strengthBoost(rarity) * hyperSummonPromotionScale(config, rarity);
        lines.push_back({std::string(chinese ? "召唤物稀有度：" : "Summon rarity: ") +
                         rarityTr(petalGameplayRarity(rarity)), 12.0});
        special("每人召唤上限：", "Summon cap per player: ", config.projectile.summonMax);
        special("召唤物生命：", "Summon health: ", summon.health * scale * summon.petHealthScale * kSummonedMobHealthScale * skills.healthScale(SkillId::Summoner) * ascendantSummonHealthScale(rarity));
        special("召唤物伤害：", "Summon damage: ", summon.damage * scale * summon.petDamageScale * skills.effectScale(SkillId::PetDamage) * ascendantSummonDamageScale(rarity));
    }
    if(computerWeapon(config.id)) {
        lines.push_back({chinese?"电脑联动（只计算主槽、同阶或更高阶配件）：程序/内存/芯片/电容/光标/几何体为6种部件，重复不计。":"Computer synergy: active parts at this tier or higher; software/memory/chip/capacitor/cursor/geometry are six distinct roles.",12.0});
        lines.push_back({chinese?"3种：电脑花瓣伤害+12%；4种：耐久+15%；5种：装填时间−10%。低阶配件不会增幅高阶花瓣，拟态不补部件。":"3 roles: computer damage +12%; 4: health +15%; 5: reload time −10%. Lower-tier parts cannot boost higher tiers; Mimic does not count as hardware.",12.0});
    }
    if (config.id == "domino") {
        lines.push_back({chinese ? "伤害栏为平均值；每次重装独立掷两端0至6点，伤害随点数乘积变化，与幸运无关。" : "Damage shows the mean. Each reload rolls two independent ends from 0 to 6; their product determines damage, unaffected by Luck.",12.0});
        if (dominoHasReroll(rarity)) lines.push_back({chinese ? "H稳牌：每个零点端补掷一次，降低空牌概率；期望伤害仍为同种E的1.5倍。" : "H steady hand: reroll each zero end once, reducing blanks. Expected damage remains 1.5x the same Eternal petal.",12.0});
    }
    const bool darkMark = config.id == "dark_mark";
    const bool voidMark = config.id == "void_mark" || config.id == "void_void_mark";
    if (darkMark || voidMark) {
        special("印记蓄力：", "Mark delay: ", darkMarkDelayMillis(rarity) / 1000.0, "s");
        if (darkMark) {
            lines.push_back({chinese ? "靠近倒下的玩家并按攻击放置印记，蓄力后将其送往地狱。被救起或印记被打破则取消。" : "Attack near a fallen flower to place a mark. After charging, sends them to Hel. Revival or breaking the mark cancels it.",12.0});
        } else {
            lines.push_back({chinese ? "靠近倒下的玩家并按攻击放置印记。目标提前复活或印记被打破时取消。" : "Attack near a fallen flower to place a mark. Revival or breaking the mark during the charge cancels it.",12.0});
        }
        lines.push_back({chinese ? "双方需20级；不作用于分身、副本或已在地狱的玩家。成功后目标5分钟免疫印记。" : "Both flowers must be level 20+. Cannot target split flowers, dungeon runs or flowers in Hel. Target gains 5 minutes of Mark immunity.",12.0});
    }
    if (darkMark || voidMark || config.id == "arctic_frozen_mark") {
        if (markHoldsOnArrival(rarity)) {
            lines.push_back({chinese ? "Apex：传送后目标在目的地滞留 2 分钟，之后可传送或使用 /mark-return。"
                                     : "Apex: holds the target on the destination map for 2 minutes. Portals and /mark-return unlock afterward.", 12.0});
        } else {
            lines.push_back({chinese ? "传送后可立即使用 /mark-return 返回原处。" : "Use /mark-return immediately after transport to go back.",12.0});
        }
    }
    if (config.id == "magic_bubble") {
        lines.push_back({chinese ? "按住防御并移动时向该方向冲刺，攻击与防御可以同时按住。" : "Hold defend and move to dash in that direction. You can hold attack at the same time.",12.0});
    }
    if (config.id == "corruption") {
        if (ladderRarity(rarity)==Rarity::Apex) {
            special("腐化光环半径：", "Corruption aura radius: ", kHyperCorruptionRadius);
            lines.push_back({chinese ? "装备时影响同地图范围内20级及以上玩家；离开即解除，不连锁传播。" : "While worn, affects nearby level 20+ flowers in the same realm; ends on leaving, never chains.",12.0});
            special("卸下后的破绽：", "Exposure after unequipping: ",
                    kHyperCorruptionExposureMillis / 1000.0, "s");
            lines.push_back({chinese ? "卸下时光环消失，自己仍会短暂暴露，可被先前受到腐化影响的玩家反击。"
                                     : "Unequipping ends the aura and briefly leaves you vulnerable to retaliation by flowers affected by it.",12.0});
        }
        if (rarity == Rarity::Ascendant) {
            lines.push_back({chinese ? "装备 Apex 腐化时仍可使用绷带续命。" : "Bandage can still work while wearing Apex Corruption.",12.0});
        } else {
            lines.push_back({chinese ? "装备后无法触发或维持绷带续命。" : "Wearing this prevents and ends Bandage's Undead effect.",12.0});
        }
    }
    if (config.id == "bandage") {
        lines.push_back({chinese ? "随机腐化会阻止并终止续命。装备腐化时，只有 Apex 腐化可保持续命。单独受到光环影响仍可续命。" :
                                   "Random corruption prevents and ends Undead. Apex Corruption is the only Corruption petal compatible with Bandage. The aura alone allows Bandage.",12.0});
        if (isHyperPetal(rarity)) {
            lines.push_back({chinese ? "同档或更高档的虚空伤疤命中时，剩余续命时间缩短 25%，每次续命最多一次。"
                                     : "A Void Scar of the same tier or higher cuts the remaining Undead time by 25% once per Undead window.", 12.0});
        }
    }
    if ((config.id == "sawblade" && ladderRarity(rarity) >= Rarity::Unique) ||
        (config.id == "rock" && isHyperPetal(rarity))) {
        lines.push_back({chinese ? "对野怪接触命中且花瓣未碎时，追加半伤害并承受半反伤。" :
                                   "After landing a contact hit on a wild mob, a surviving petal strikes again at half damage and takes half recoil.", 12.0});
    }
    if (stats.count > 1) special("每组花瓣数量：", "Petals per cluster: ", stats.count);
    if (config.id == "magic_blood_stinger" && stats.count > 1)
        special("每根血刺的基础生成耗魔：", "Base mana per stinger formed: ", stats.requiredMana);
    if (stats.armorPerStack > 0) {
        special("每层护甲：", "Armor per stack: ", stats.armorPerStack);
        special("护甲叠层间隔：", "Armor stack interval: ", 2.0, "s");
        special("护甲层数上限：", "Armor stack limit: ", 10);
    }
    if (stats.noHealDurationMillis > 0) special("禁止治疗持续：", "Healing suppression: ", stats.noHealDurationMillis / 1000.0, "s");
    if (config.armorPiercing && !config.lightningDamage)
        lines.push_back({chinese ? "穿甲：直接命中无视护甲。" : "Armor piercing: direct hits ignore armor.", 12.0});
    if (config.armorDamageFraction > 0.0)
        special("额外伤害（野怪有效护甲）：", "Bonus damage (% of effective wild-mob armor): ",
                config.armorDamageFraction * 100.0, "%");
    if (config.contactSelfDamage > 0.0)
        special("每次有效接触的基础自伤：", "Base self-damage per landed contact: ",
                petalContactSelfDamage(config, rarity));
    if (stats.damageIntervalMillis != kPetalHitIntervalMillis)
        special("同一目标接触伤害间隔：", "Contact damage interval per target: ",
                stats.damageIntervalMillis / 1000.0, "s");
    if (stats.knockback != 0) special("击退强度：", "Knockback: ", stats.knockback);
    if (stats.attractionForce > 0) special("花瓣吸引强度：", "Petal attraction strength: ", stats.attractionForce);
    if (stats.modifiers.damage != 1) special("伤害变化：", "Damage bonus: ", (stats.modifiers.damage - 1) * 100, "%");
    if (stats.modifiers.aggroRadius != 0) special("额外警觉范围：", "Extra aggro range: ", stats.modifiers.aggroRadius);
    if (stats.modifiers.petalAttractionRadius > 0) special("花瓣吸引范围：", "Petal attraction radius: ", stats.modifiers.petalAttractionRadius);
    if (!stats.breakable) lines.push_back({chinese ? "耐久：不可被攻击击碎" : "Durability: unbreakable",12.0});
    if (stats.webRadius > 0) special("蛛网半径：", "Web radius: ", stats.webRadius);
    if (stats.slowFactor < 1.0) {
        special("减速比例：", "Slow: ", (1.0 - stats.slowFactor) * 100.0, "%");
        if (stats.slowDurationMillis > 0) special("减速持续：", "Slow duration: ", stats.slowDurationMillis / 1000.0, "s");
    }
    if (stats.spongeDamageDurationMillis > 0)
        special("伤害分摊时间：", "Damage spread duration: ", stats.spongeDamageDurationMillis / 1000.0, "s");
    if (config.id == "chromosome") {
        special("每槽单次修复上限：", "Repair per slot per pulse: ", chromosomeRepairPerPulse(rarity));
        special("每槽耐久比例上限：", "Slot durability cap: ",
                kChromosomeRepairHealthCap * 100.0, chinese ? "%（至少保留基础修复量）" : "% (at least base repair)");
    }
    if (config.id == "magic_bone") {
        TooltipLine stacking{chinese
            ? "每多一根就绪的魔法骨头，每根对野怪和训练木桩的接触伤害加算 50%。"
            : "Each additional ready Magic Bone adds 50% contact damage to every Magic Bone against wild mobs and training dummies.", 12.0};
        stacking.maxWidth = 290.0;
        lines.push_back(std::move(stacking));
    }
    const double cameraZoom = config.id == "antennae" && !showHyperTechnique && isHyperPetal(rarity)
        ? petalCameraZoom(content(), petalIndex, Rarity::Unique) : stats.cameraZoom;
    if (cameraZoom != 1.0) {
        lines.push_back({std::string(chinese ? "视野缩放：" : "Camera zoom: ") +
            percentNumber(cameraZoom) + "%", 12.0});
        lines.push_back({std::string(chinese ? "可见范围倍率：" : "Vision range: ") +
            oneDecimal(1.0 / cameraZoom) + "x", 12.0});
    }
    if (config.id == "observer") lines.push_back({chinese ?
        "机械导弹发射时自动瞄准同地图内的敌方目标。" :
        "Mecha Missiles automatically aim at hostile targets in the same map on launch.",12.0});
    if (stats.modifiers.luck != 0.0) {
        const bool positiveLuck = stats.modifiers.luck > 0;
        const char* label = chinese ? (positiveLuck ? "幸运加成：+" : "幸运变化：")
                                    : (positiveLuck ? "Luck: +" : "Luck: ");
        char value[32];
        std::snprintf(value, sizeof value, "%g", stats.modifiers.luck);
        TooltipLine line{std::string(label) + value, 12.0};
        line.alpha = 0.72;
        line.labelColor = kStatGeneric;
        lines.push_back(line);
    }
    if (stats.modifiers.aggroRange != 1.0)
        special("怪物警觉范围变化：", "Mob detection range: ", (stats.modifiers.aggroRange - 1.0) * 100.0, "%");
    if (config.id == "bulb") {
        special("单片召唤物索敌范围：", "Summon targeting range with one Bulb: ",
                (bulbSummonTargetRangeScale(stats.modifiers.aggroRange - 1.0) - 1.0) * 100.0, "%");
        special("额外灯泡的引怪增量生效比例：", "Extra bulb aggro bonus retained: ",
                kAdditionalBulbAggroScale * 100.0, "%");
        special("叠加上限（基础警觉范围）：", "Stack cap (base aggro range): ",
                kAdditionalBulbAggroCap * 100.0, "%");
    }
    if (stats.modifiers.poisonArmor > 0) special("毒抗：", "Poison armor: ", stats.modifiers.poisonArmor);
    if (config.projectile.present) {
        if (config.id == "marble" || config.id == "magic_marble") {
            const double wait = config.id == "marble" ? kMarblePreparationMillis : kMagicMarblePreparationMillis;
            special("装填完成后发射准备：", "Shot preparation after reload: ", wait / 1000.0, "s");
        }
        if (config.id == "mecha_missile") {
            special("单发伤害：", "Damage per missile: ", baseDamage * skills.effectScale(SkillId::PetDamage));
            lines.push_back({chinese ? "单发伤害随召唤物伤害天赋提升。" :
                                       "Missile damage scales with the summon damage talent.", 12.0});
        }
        if (config.id != "mecha_missile" && config.id != "magic_peas") {
            const double baseProjectileDamage = config.lightningDamage && stats.lightningDamagePerHit > 0.0
                ? stats.lightningDamagePerHit : stats.damage;
            if (baseProjectileDamage > 0.0)
                special("单发基础伤害（含召唤物伤害天赋）：", "Base shot damage with Pet Damage: ",
                        baseProjectileDamage * skills.effectScale(SkillId::PetDamage));
        }
        if (config.id == "spaceship") {
            lines.push_back({chinese ? "飞船单发基础伤害未计电脑套装加成。"
                                     : "Base Spaceship shot damage excludes computer set bonuses.", 12.0});
        }
        special("每颗花瓣每次发射弹数：", "Shots per petal per volley: ", config.projectile.count);
        special("弹速：", "Projectile speed: ", config.projectile.speed);
        special("基础飞行距离：", "Base projectile travel distance: ", config.projectile.distance);
        if (config.projectile.seekRange > 0)
            special("基础锁定半径：", "Base targeting radius: ", config.projectile.seekRange);
    }
    if (config.id == "magic_eye") {
        special("每次消耗当前魔力：", "Current mana spent per cast: ", 50.0, "%");
        special("每点耗蓝转化闪电伤害：", "Lightning damage per mana spent: ",
                magicEyeDamagePerMana(rarity));
        special("闪电索敌距离：", "Lightning targeting range: ", 240.0);
        special("最多连锁目标：", "Maximum lightning targets: ",
                std::clamp(std::min(rarityIndex(rarity), rarityIndex(Rarity::Unique)) - 3, 1, 5));
    }
    if (config.id == "blood") {
        special("血池基础最大半径：", "Base maximum blood pool radius: ", bloodPoolRadius(rarity), "", "%.8g");
        special("基础扩散速度：", "Base radial growth: ", bloodSpillRate(rarity), "/s", "%.8g");
        const double rate = skills.effectScale(SkillId::SpillRate) * skills.effectScale(SkillId::SecondaryReload);
        if (const double reach = skills.effectScale(SkillId::FluidReach); reach > 1.0)
            special("血池半径（仅计天赋）：", "Blood pool radius with talent only: ", bloodPoolRadius(rarity) * reach);
        special("血池准备时间：", "Blood pool charge time: ", std::max(1000.0 / rate, net::kTickMillis) / 1000.0, "s");
        special("血池持续：", "Blood pool lifetime: ", 2.0, "s");
    }
    if (fireScale > 1.0 && (config.burnPerSecond > 0.0 || config.radiation.burnDamage > 0.0))
        special("火伤天赋倍率：", "Fire talent multiplier: ", fireScale, "x");
    if (config.radiation.present) {
        const double radiationReach = skills.effectScale(SkillId::RadiationReach);
        const double radiationPower = skills.effectScale(SkillId::RadiationPower);
        if (config.id == "uranium")
            special("基础辐射伤害/秒：", "Base radiation damage per second: ", uraniumRadiationDps(content(), rarity) * kRadiationDamageScale);
        if(config.id=="uranium")
            special("辐射自伤上限（每秒最大生命）：", "Self-radiation cap (% max HP/s): ", 4.0, "%");
        const double poison = config.radiation.burnDamage > 0.0 ? config.radiation.poisonDamage :
            config.radiation.poisonPerSecond;
        if (poison > 0.0)
            special("基础辐射毒伤/秒：", "Base radiation poison per second: ", poison * referenceOffenseScale(config, rarity) * kRadiationDamageScale);
        if (poison > 0.0 || config.id == "uranium") {
            lines.push_back({chinese ? "辐射毒对怪：同一玩家共用一层，不同玩家可叠加。"
                                     : "Radiation poison on mobs shares a layer per player. Different players stack.", 12.0});
        }
        if (config.radiation.burnDamage > 0.0) {
            const double baseBurn = config.radiation.burnDamage * referenceOffenseScale(config, rarity) *
                kRadiationDamageScale;
            if (config.id == "pyrite") {
                const double rate = baseBurn * (2000.0 / kFireBombBurnMillis);
                special("基础灼烧伤害/秒：", "Base burn damage per second: ", rate);
                if (damageScale * radiationPower * fireScale != 1.0)
                    special("灼烧伤害/秒（含天赋）：", "Burn damage per second with talents: ",
                            rate * damageScale * radiationPower * fireScale);
                special("离开火圈后的灼烧持续：", "Burn after leaving the ring: ", kFireBombBurnMillis / 1000.0, "s");
            } else {
                special("基础辐射灼烧强度：", "Base radiation burn strength: ", baseBurn);
            }
            if (config.id == "arctic_plutonium") {
                special("辐射灼烧强度（含伤害天赋）：", "Radiation burn strength with talents: ",
                        baseBurn * damageScale * radiationPower * fireScale);
                lines.push_back({chinese ? "离开辐射区后灼烧最多持续 3 秒，伤害随灼烧时间上升。"
                                         : "Burn lasts up to 3 seconds after leaving the field. Damage ramps up over time.", 12.0});
            }
        }
        if (config.id == "void_stardust")
            special("中毒持续：", "Poison duration: ", config.radiation.poisonDurationMillis / 1000.0, "s");
        const double baseRadius = baseRadiationRadius(config, rarity);
        special("基础辐射半径：", "Base radiation radius: ", baseRadius);
        if (technique && technique->technique == HyperTechnique::WideRadiation) {
            special("专属辐射半径：", "Radiation radius with technique: ", baseRadius * technique->amount);
        }
        if (radiationReach > 1.0) {
            const double techniqueScale = technique && technique->technique == HyperTechnique::WideRadiation
                ? technique->amount : 1.0;
            special("实战辐射半径（含天赋）：", "Radiation radius with talent: ",
                    baseRadius * techniqueScale * radiationReach);
        }
        if (radiationPower > 1.0)
            special("辐射增幅倍率：", "Radiation Power multiplier: ", radiationPower, "x");
        special("辐射间隔：", "Radiation interval: ", config.radiation.intervalMillis / 1000.0, "s");
        if (config.radiation.selfPoisonScale > 0)
            special("自身中毒倍率：", "Self-poison multiplier: ", config.radiation.selfPoisonScale);
    }
    if(config.id=="void_balloon")
        special("向上漂移速度：","Upward drift speed: ",balloonDriftSpeed(rarity));
    if(config.id=="arctic_nitro") {
        special("喷口方向推进速度：","Nozzle-directed boost speed: ",nitroDriftSpeed(rarity));
        special("持续喷射时间：","Boost duration: ",nitroUseMillis(rarity)/1000.0,"s");
    }
    if (stats.conditionalHealPerSecond > 0) {
        special("低于75%生命时治疗：", "Healing below 75% health: ",
            stats.conditionalHealPerSecond * healTalent * kPlayerHealMultiplier, chinese ? "/秒" : "/s");
        special("基础条件治疗：", "Base conditional healing: ", stats.conditionalHealPerSecond, chinese ? "/秒" : "/s");
    }
    if (stats.totemHealPerSecond > 0) {
        special("每次击杀增加回血：", "Heal per kill: ", stats.totemHealPerSecond * healTalent * kPlayerHealMultiplier,
                chinese ? "/秒，持续10秒，可叠加" : "/s for 10s, stacking");
    }
    if (config.id == "fractal")
        special("单目标基础闪电伤害：", "Base lightning damage per target: ", fractalBaseLightningDamage(config, rarity));
    if (stats.lightningDamagePerHit > 0) {
        if (config.id == "magic_peas") {
            lines.push_back({chinese ? "四颗蓝莓分别造成闪电伤害，不会连锁跳跃。" :
                "Four blueberries deal lightning damage individually, with no chain jumps.", 12.0});
            const double berryDamage = stats.lightningDamagePerHit * skills.effectScale(SkillId::PetDamage) *
                skills.effectScale(SkillId::LightningPower);
            special("单颗发射伤害：", "Projectile damage per berry: ", berryDamage);
            lines.back().altText = std::string(chinese ? "单颗发射伤害：" : "Projectile damage per berry: ") + exactNumber(berryDamage);
        }
        lines.push_back({chinese ? "闪电无视护甲和闪避。" : "Lightning bypasses armor and dodge.",12.0});
        special("基础闪电伤害：", "Base lightning damage: ", stats.lightningDamagePerHit);
        if (showHyperTechnique && stats.wildLightningScale > 1.0) {
            special("对野怪单次雷击：", "Lightning per hit on wild mobs: ",
                    stats.lightningDamagePerHit * stats.wildLightningScale * damageScale);
            special("野怪接触反伤减免：", "Wild contact recoil reduction: ",
                    (1.0 - stats.wildRecoilScale) * 100.0, "%");
        }
        special("连锁跳跃次数：", "Chain bounces: ", stats.lightningBounces);
        special("每段索敌范围：", "Range per jump: ", stats.lightningRange);
        if (stats.batteryCharges > 0) {
            special("每轮放电次数：", "Discharges per reload: ", stats.batteryCharges);
            lines.push_back({chinese ? "装备槽数字显示当前剩余放电次数。"
                                     : "The loadout counter shows remaining discharges.", 12.0});
            TooltipLine mechanism{chinese ? "花朵实际掉血时触发。两次放电至少间隔 0.5 秒，期间受到的有效伤害最多保留一次，等间隔结束再放电。3 次用完才开始重装；重装天赋只缩短回满时间。仅接触、护盾全吸收、自伤和无敌不触发。" :
                "Triggers on actual HP loss. Discharges are at least 0.5s apart, and one qualifying hit can wait until the interval ends. Reload talent only shortens the refill after all 3 charges are spent. Touching alone, fully shielded hits, self-damage and invulnerability do not trigger it.",12.0};
            mechanism.maxWidth=kTooltipWrapWidth; lines.push_back(mechanism);
        }
    }
    if(stats.lightningRepeatChance>0){
        special("闪电重复概率：", "Lightning repeat chance: ",stats.lightningRepeatChance*100.0,"%");
        special("最多追加电击：", "Maximum extra strikes: ",stats.maxLightningRepeats);
        lines.push_back({chinese?"只取最强红宝石；每次追加重新判定，不额外开启新连锁。":"Only the strongest Ruby applies. Extra strikes roll independently without starting another chain.",12.0});
    }

    // Relic. The whole petal is this one number.
    if (petalSharesFlowerDamage(config)) {
        TooltipLine bonus{std::string(tipFlowerHealth()) + "+" +
                              exactNumber((stats.modifiers.maxHealth - 1.0) * 100.0) + "%",
                          12.0};
        bonus.alpha = 0.56;
        bonus.labelColor = kStatHealth;
        lines.push_back(bonus);
    }

    // Rubber. Its price is printed beside what it buys.
    if (stats.flowerKnockback > 0.0) {
        TooltipLine shove{std::string(chinese ? "花朵受击击退：+" : "Flower Knockback: +") + exactNumber(stats.flowerKnockback * 100.0) + "%",
                          12.0};
        shove.alpha = 0.56;
        shove.labelColor = kStatGeneric;
        lines.push_back(shove);
    }

    // Amulet. florr's own line, and the whole of what the petal does.
    if (stats.overhealShield > 0.0) {
        TooltipLine convert{std::string(chinese ? "溢出治疗转护盾：+" : "Overheal Conversion: +") + exactNumber(stats.overhealShield * 100.0) +
                                "%",
                            12.0};
        convert.alpha = 0.56;
        convert.labelColor = kStatGeneric;
        lines.push_back(convert);
    }

    // Basil. Percentage of each heal (after kPlayerHealMultiplier), not a flat.
    if (stats.healBonus > 0.0) {
        TooltipLine bonus{std::string(tipHealBonus()) + percentNumber(stats.healBonus) + "%", 12.0};
        bonus.alpha = 0.56;
        bonus.labelColor = kStatHeal;
        lines.push_back(bonus);
    }

    if (config.id == "magic_golden_leaf")
        special("耗魔降低：", "Mana costs reduced: ", stats.reloadCut * 100.0 / kMagicGoldenLeafReloadShare, "%");

    // Golden leaf, florr's line: what it takes off the rest of the bar.
    if (stats.reloadCut > 0.0) {
        TooltipLine cut{std::string(chinese ? "全局冷却缩减：-" : "Reload: -") + percentNumber(stats.reloadCut) + "%", 12.0};
        cut.alpha = 0.56;
        cut.labelColor = kStatGeneric;
        lines.push_back(cut);
    }

    // Disc, florr's line, and the whole of what the petal does.
    if (stats.collisionResist > 0.0) {
        TooltipLine resist{std::string(chinese ? "碰撞伤害减免：+" : "Collision Damage Resistance: +") +
                               percentNumber(stats.collisionResist) + "%",
                           12.0};
        resist.alpha = 0.56;
        resist.labelColor = kStatGeneric;
        lines.push_back(resist);
    }

    // Bandage, florr's three lines: how long Undead lasts, what the body hits
    // for meanwhile, and how long the flower then cannot be revived.
    if (stats.undeadMillis > 0.0) {
        const auto seconds = [](double millis) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%g", std::round(millis / 10.0) / 100.0);
            return std::string(buffer) + "s";
        };
        TooltipLine undead{std::string(chinese ? "不死持续：" : "Undead: ") + seconds(stats.undeadMillis), 12.0};
        undead.alpha = 0.56;
        undead.labelColor = kStatUndead;
        lines.push_back(undead);
        TooltipLine body{std::string(tipBodyDamage()) + "-" +
                             percentNumber(stats.undeadBodyDamageCut) + "%",
                         12.0};
        body.alpha = 0.56;
        body.labelColor = kStatDamage;
        lines.push_back(body);
    }

    // Bur alone. Its description says the petal debuffs armour and cannot say
    // by how much, and how much is the entire question a player has about it:
    // whether this one out-strips the tier standing in front of them.
    if (stats.armorReduction > 0.0) {
        const char* debuff = language() == Language::Zh ? "护甲削弱: -" : "Armor Debuff: -";
        TooltipLine strip{std::string(debuff) + abbreviate(stats.armorReduction), 12.0};
        strip.alpha = 0.56;
        strip.labelColor = kStatArmor;
        strip.altText = std::string(debuff) + exactNumber(stats.armorReduction);
        lines.push_back(strip);
    }

    // Bone. Its armour is the whole of what the petal is for and it climbs the
    // mob armour ladder, so the figure a player is weighing belongs here.
    if (config.id == "bone") lines.push_back({chinese ? "骨头的护甲不会因受击而衰减。" : "Bone keeps its armor after taking a hit.",12.0});
    if (triangleBonus > 0.0) {
        const double bonus = triangleBonus * damageScale;
        const std::string label = chinese ? "三角叠加伤害：+" : "Triangle stacking damage: +";
        TooltipLine line{label + abbreviate(bonus), 12.0};
        line.altText = label + exactNumber(bonus);
        lines.push_back(std::move(line));
    }

    if (stats.petalArmor > 0.0) {
        TooltipLine armor{std::string(tipArmor()) + abbreviate(stats.petalArmor), 12.0};
        armor.alpha = 0.56;
        armor.labelColor = kStatArmor;
        armor.altText = std::string(tipArmor()) + exactNumber(stats.petalArmor);
        lines.push_back(armor);
    }

    // Claw. Petal damage like the line above, so it carries the same talent.
    if (stats.critDamage > 0.0) {
        const double crit = std::round(stats.critDamage * damageScale);
        TooltipLine bonus{std::string(tipCritDamage()) + "+" + abbreviate(crit), 12.0};
        bonus.alpha = 0.56;
        bonus.labelColor = kStatDamage;
        bonus.altText = std::string(tipCritDamage()) + "+" + exactNumber(crit);
        lines.push_back(bonus);
    }

    // Fang. landHeal multiplies the stolen amount by kPlayerHealMultiplier, so
    // the card shows the effective share of damage returned as HP.
    if (stats.lifesteal > 0.0) {
        special("基础吸血比例：", "Base lifesteal: ", stats.lifesteal * 100.0, "%");
        const double pct = stats.lifesteal * 100.0 * kPlayerHealMultiplier;
        TooltipLine steal{std::string(tipLifesteal()) + exactNumber(pct) + "%", 12.0};
        steal.alpha = 0.56;
        steal.labelColor = kStatHeal;
        lines.push_back(steal);
    }

    if (config.id == "magic_fang" || config.id == "magic_tooth") {
        const bool fang = config.id == "magic_fang";
        const char* label = fang
            ? (chinese ? "有效咬中野怪／训练木桩回魔：" : "Mana per damaging wild mob / dummy bite: ")
            : (chinese ? "每次重装至多回复法力：" : "Mana restored at most once per reload: ");
        const double amount = fang ? magicFangManaPerBite(rarity) : magicToothManaPerReload(rarity);
        TooltipLine refund{label + oneDecimal(amount), 12.0};
        refund.alpha = 0.72;
        refund.labelColor = kStatGeneric;
        lines.push_back(refund);
    }

    if (stats.petalEvasion > 0.0) {
        lines.push_back({std::string(chinese ? "花瓣自身闪避：" : "Petal evasion: ") + exactNumber(stats.petalEvasion * 100.0) + "%", 12.0});
        lines.push_back({chinese ? "仅保护花瓣自身；闪电不可闪避。" : "Protects this petal only; lightning bypasses evasion.", 12.0});
    }
    if (stats.petalPoisonImmune) lines.push_back({chinese ? "花瓣自身免疫中毒。" : "This petal is immune to poison.", 12.0});

    // Talisman. The chance grows a step every tier and nothing in play shows
    // it -- a dodge is a hit that simply did not happen -- so it is printed.
    if (stats.modifiers.evasion > 0.0) {
        TooltipLine dodge{std::string(chinese ? "闪避概率：" : "Evasion: ") + exactNumber(stats.modifiers.evasion * 100.0) + "%", 12.0};
        dodge.alpha = 0.56;
        dodge.labelColor = kStatGeneric;
        lines.push_back(dodge);
    }

    // Salt. What it repays depends on who bit, so the card states the one
    // fixed point -- an attacker of this card's own tier -- and the
    // description says how the rest of the ladder runs from there.
    if (stats.modifiers.damageReflection > 0.0) {
        TooltipLine reflect{std::string(chinese ? "反伤比例：" : "Reflection: ") +
                                exactNumber(stats.modifiers.damageReflection * 100.0) +
                                "% vs " + rarityTr(rarity),
                            12.0};
        reflect.alpha = 0.56;
        reflect.labelColor = kStatReflection;
        lines.push_back(reflect);
    }

    // The magic petals. Every one of these figures moves with rarity and none
    // of them can be read off the ring in play -- a player comparing two orbs
    // has nothing else to go on -- so they belong on the card. What a reload
    // costs is in the header, beside the reload; see the title above.
    //
    // The magic stick: paid per storm rather than per reload of the petal,
    // and the storm's span is what the payment buys. florr's
    // Petal/Attribute/SpawnManaCost, "Spawn Cost: X mana".
    if (stats.petManaCost > 0.0) {
        const std::string label = colourMarkup(chinese ? "每次召唤消耗：" : "Summon cost:", kStatGeneric) + " ";
        const std::string unit = " " + colourMarkup(chinese ? "法力" : "mana", kStatMana);
        TooltipLine summon{label + abbreviate(stats.petManaCost) + unit, 12.0};
        summon.alpha = 0.56;
        summon.markup = true;
        summon.altText = label + exactNumber(stats.petManaCost) + unit;
        lines.push_back(summon);
    }
    if (config.petLifetimeMillis > 0.0) {
        TooltipLine span{std::string(chinese ? "召唤持续：" : "Summon Lifetime: ") + exactNumber(config.petLifetimeMillis / 1000.0) + "s",
                         12.0};
        span.alpha = 0.56;
        span.labelColor = kStatDuration;
        lines.push_back(span);
    }
    if (stats.maxMana > 0.0) {
        TooltipLine pool{std::string(chinese ? "魔法值上限：+" : "Max Mana: +") + abbreviate(stats.maxMana), 12.0};
        pool.alpha = 0.56;
        pool.labelColor = kStatMana;
        pool.altText = std::string(chinese ? "魔法值上限：+" : "Max Mana: +") + exactNumber(stats.maxMana);
        lines.push_back(pool);
    }
    if (config.id == "honey") {
        lines.push_back({chinese ? "投出的蜂蜜持续 12 秒，期间不会被怪物咬碎。" :
                                   "Thrown honey lasts 12 seconds and cannot be broken by mobs.", 12.0});
    }
    if (config.id == "magic_cotton") {
        TooltipLine absorption{
            std::string(chinese ? "每点法力吸收伤害：" : "Damage absorbed per mana: ") +
                oneDecimal(magicCottonDamagePerMana(rarity)), 12.0
        };
        absorption.alpha = 0.56;
        absorption.labelColor = kStatMana;
        absorption.altText = std::string(chinese ? "每点法力吸收伤害：" : "Damage absorbed per mana: ") +
            exactNumber(magicCottonDamagePerMana(rarity));
        lines.push_back(absorption);
    }
    const double manaTalent = skills.effectScale(SkillId::ManaRegen);
    if (stats.mana > 0.0) {
        TooltipLine burst{std::string(chinese ? "单次恢复魔法值：" : "Mana per Burst: ") + abbreviate(stats.mana * manaTalent), 12.0};
        burst.alpha = 0.56;
        burst.labelColor = kStatMana;
        burst.altText = std::string(chinese ? "单次恢复魔法值：" : "Mana per Burst: ") + exactNumber(stats.mana * manaTalent);
        lines.push_back(burst);
        const double cycle = stats.reloadMillis + stats.manaChargeMillis * chargeScale;
        if (cycle > 0.0) {
            TooltipLine average{std::string(chinese ? "完整周期平均回蓝/秒：" : "Full-cycle Mana/s: ") +
                                oneDecimal(stats.mana * manaTalent * 1000.0 / cycle), 12.0};
            average.alpha = 0.56;
            average.labelColor = kStatMana;
            lines.push_back(average);
        }
    }
    if (stats.passiveManaPerSecond > 0.0) {
        TooltipLine regen{std::string(chinese ? "魔法恢复/秒：" : "Mana/s: ") + oneDecimal(stats.passiveManaPerSecond * manaTalent), 12.0};
        regen.alpha = 0.56;
        regen.labelColor = kStatMana;
        lines.push_back(regen);
    }

    // Only the cutters have one, and theirs moves with rarity, so the number
    // has to be on the card -- the description alone cannot say how much.
    if (stats.bodyDamage > 0.0) {
        const double body = std::round(stats.bodyDamage);
        TooltipLine slam{std::string(tipBodyDamage()) + "+" + abbreviate(body), 12.0};
        slam.alpha = 0.56;
        slam.labelColor = kStatDamage;
        slam.altText = std::string(tipBodyDamage()) + "+" + exactNumber(body);
        lines.push_back(slam);
    }
    return lines;
}

} // namespace ui

struct InventoryPanel::LayoutCache {
    double lastPaintScroll = -1;
    Grid grid;
    const WorldMaps* dropMapSource = nullptr;
    std::vector<const MapData*> dropMaps;
    std::vector<Profile::Stack> stacks;
    std::array<std::uint64_t, kRarityCount> inventoryTotals{};
    std::string search;
    double width = -1;
    bool stacked = false;
    bool sortByCount = false;
    Language lang = Language::Zh;
    const std::vector<bool>* drops = nullptr;

    const Grid& get(const Profile& profile, double nextWidth, bool nextStacked,
                    const std::string& nextSearch, const std::vector<bool>* nextDrops) {
        const auto sameStack = [](const Profile::Stack& a, const Profile::Stack& b) {
            return a.petalIndex == b.petalIndex && a.rarity == b.rarity && a.count == b.count;
        };
        if (width == nextWidth && stacked == nextStacked && search == nextSearch &&
            lang == language() && drops == nextDrops &&
            std::equal(stacks.begin(), stacks.end(), profile.inventory.begin(),
                       profile.inventory.end(), sameStack)) return grid;

        grid = buildGrid(profile, nextWidth, nextStacked, sortByCount, trimmed(lowerCase(nextSearch)), nextDrops);
        stacks = profile.inventory;
        inventoryTotals.fill(0);
        for (const auto& stack : stacks) {
            const auto tier = rarityIndex(stack.rarity);
            if (tier < kRarityCount) inventoryTotals[tier] += stack.count;
        }
        width = nextWidth;
        stacked = nextStacked;
        search = nextSearch;
        lang = language();
        drops = nextDrops;
        return grid;
    }
};

double InventoryPanel::preferredWidth() {
    // Five cells, their gaps, and the two unequal margins. The panel is sized
    // to the grid rather than the grid fitted to the panel, so a change to the
    // cell size moves the card's edge instead of crowding its contents.
    return kColumns * kCellSize + (kColumns - 1) * kCellGap + kGridPadding + kGridGutter;
}

void InventoryPanel::reset() {
    // Only the hover and the field's focus. Scroll position, search text and
    // Stack mode all survive a close and reopen in the browser -- the panel is
    // a window onto the account, not a wizard that starts over.
    searchField_.blur();
    tooltipDelay.reset();
    touchInspect.reset();
    thumbDrag() = {};
    showRunLoot_ = false;
    showRunKills_ = false;
    runLootScroll_ = {};
}

void InventoryPanel::openRunLoot(DragState& drag) {
    searchField_.blur();
    drag.clear();
    runLootScroll_ = {};
    showRunLoot_ = true;
    showRunKills_ = false;
    requestedRunStatsRevision_.reset();
}

bool InventoryPanel::renderRunLoot(MenuContext& ctx) {
    auto& canvas = ctx.canvas;
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    const bool zh = language() == Language::Zh;
    panelCard(canvas, panel, kInventorySkin);
    panelTitle(canvas, panel, zh ? "本局" : "This Run", "");
    const Rect close = closeButtonRect(panel);
    panelClose(canvas, close, close.contains(mouse));
    if (drawPanelExpand(ctx, close.y + close.h * 0.5, kInventorySkin.fill, expanded_)) {
        expanded_ = !expanded_;
        runLootScroll_.thumbGrab.reset();
        return true;
    }
    const Rect back{panel.x + kControlsPad, panel.y + kControlsY, 100.0, kControlsHeight};
    drawSlotButton(canvas, back, zh ? "返回背包" : "Back", kInventorySkin.fill,
                   back.contains(mouse), false);
    if (ctx.pressed() && (close.contains(mouse) || back.contains(mouse))) {
        showRunLoot_ = false;
        return true;
    }

    constexpr double gap = 6.0;
    const double tabWidth = (panel.right() - kControlsPad - back.right() - 2.0 * gap) * 0.5;
    for (int i = 0; i < 2; ++i) {
        const bool kills = i == 1;
        const Rect tab{back.right() + gap + i * (tabWidth + gap), back.y, tabWidth, back.h};
        if (ctx.pressed() && tab.contains(mouse) && showRunKills_ != kills) {
            showRunKills_ = kills;
            runLootScroll_ = {};
        }
        const char* label = kills ? (zh ? "本局击杀" : "Kills") : (zh ? "本局收集" : "Collected");
        drawSlotButton(canvas, tab, label, kInventorySkin.fill, tab.contains(mouse),
                       showRunKills_ == kills);
    }

    // Profile updates invalidate the server's run totals. Coalesce bursts
    // without polling while idle or replacing the current grid with a spinner.
    constexpr double kRunStatsRefreshSeconds = 0.75;
    const auto revision = ctx.net.runStatsRevision();
    const bool opening = !requestedRunStatsRevision_;
    if (opening || (*requestedRunStatsRevision_ != revision && ctx.timeSeconds >= nextRunStatsRefreshAt_)) {
        ctx.net.requestRunLoot(opening);
        requestedRunStatsRevision_ = revision;
        nextRunStatsRefreshAt_ = ctx.timeSeconds + kRunStatsRefreshSeconds;
    }

    const auto& loot = ctx.net.runLootOverview();
    const auto& kills = ctx.net.runKillsOverview();
    const bool ready = showRunKills_ ? kills.has_value() : loot.has_value();
    const std::size_t size = ready ? (showRunKills_ ? kills->size() : loot->size()) : 0;
    if (size == 0) {
        TextStyle style;
        style.size = 16.0;
        style.align = Align::Centre;
        const char* status = !ready ? (zh ? "正在读取本局记录…" : "Loading this run…")
            : showRunKills_ ? (zh ? "本局还没有击杀" : "No kills yet")
                            : (zh ? "本局还没有收集到花瓣" : "No petals collected yet");
        text(canvas, status, panel.x + panel.w * 0.5, back.bottom() + 40.0, style);
        return true;
    }

    constexpr double side = 50.0;
    constexpr double pitch = 60.0;
    const double top = back.bottom() + 10.0;
    const Rect view{panel.x + kGridPadding, top, panel.w - 2.0 * kGridPadding,
                    std::max(0.0, panel.bottom() - top - 12.0)};
    const auto columnsFor = [&](double width) {
        return std::max<std::size_t>(1, static_cast<std::size_t>(std::max(0.0, width + pitch - side) / pitch));
    };
    const auto rowsFor = [size](std::size_t columns) { return (size + columns - 1) / columns; };
    auto columns = columnsFor(view.w);
    if (rowsFor(columns) * pitch - (pitch - side) > view.h)
        columns = columnsFor(view.w - kListScrollbarGutter);
    runLootScroll_.viewHeight = view.h;
    runLootScroll_.contentHeight = rowsFor(columns) * pitch - (pitch - side);
    if (view.contains(mouse)) runLootScroll_.offset -= ctx.window.wheelDelta() * pitch;
    ctx.scrollbarOwnsPointer |= updateListScroll(ctx.window, view, runLootScroll_);
    const Rect body = listScrollContent(view, runLootScroll_);

    canvas.save();
    canvas.beginPath();
    canvas.rect(body.x, body.y, body.w, body.h);
    canvas.clip();
    const auto first = static_cast<std::size_t>(runLootScroll_.offset / pitch) * columns;
    for (std::size_t i = first; i < size; ++i) {
        const double y = body.y + (i / columns) * pitch - runLootScroll_.offset;
        if (y >= body.bottom()) break;
        const auto rowCount = std::min(columns, size - (i / columns) * columns);
        const double rowWidth = rowCount * pitch - (pitch - side);
        const Rect cell{body.x + (body.w - rowWidth) * 0.5 + (i % columns) * pitch, y, side, side};
        ItemTile tile;
        tile.timeSeconds = ctx.timeSeconds;
        if (showRunKills_) {
            const auto& kill = (*kills)[i];
            tile.rarity = kill.rarity;
            tile.showName = false;
            drawItemTile(canvas, ctx.sprites, cell, tile);
            const double radius = content().mobStats(kill.mobIndex, Rarity::Common).radius;
            ctx.sprites.drawMobPicture(canvas, kill.mobIndex, kill.rarity,
                cell.x + side * 0.5, cell.y + side * 0.64, side * 0.72, side * 0.52,
                ctx.timeSeconds, radius);

            // Reserve the top strip for the count and paint it after the mob.
            const std::string count = "x" + stackCountText(kill.count);
            TextStyle label;
            label.size = 13.0;
            const double countWidth = measure(count, label.size);
            if (countWidth > side - 8.0) label.size *= (side - 8.0) / countWidth;
            label.fill = kPaper;
            label.stroke = kInk;
            label.strokeWidth = 2.5;
            label.roundJoin = true;
            label.align = Align::Centre;
            label.baseline = Baseline::Top;
            text(canvas, count, cell.x + side * 0.5, cell.y + 4.0, label);
        } else {
            const auto& item = (*loot)[i];
            tile.petalIndex = item.petalIndex;
            tile.rarity = item.rarity;
            if (item.count > 1) tile.badge = "x" + stackCountText(item.count);
            drawItemTile(canvas, ctx.sprites, cell, tile);
        }
    }
    canvas.restore();
    drawListScrollbar(canvas, view, runLootScroll_, kPaper);
    return true;
}

bool InventoryPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Rect panel = ctx.bounds;
    const Vec2 mouse = ctx.mouse();
    const bool canViewRunLoot = ctx.net.status() == NetClient::Status::Playing && !ctx.net.dead();
    if (!canViewRunLoot) showRunLoot_ = false;
    if (showRunLoot_) return renderRunLoot(ctx);
    // Leave the drag live while the bag is hidden so the loadout can still
    // receive it, including when a desktop slot overlaps the bag.
    if (ctx.drag.source == DragState::Source::Inventory &&
        ((!ctx.window.pointerIsTouch() && ctx.drag.inventoryDragMoved(mouse)) || !panel.contains(mouse))) {
        return true;
    }

    // The grid's own span, and its centre. Not the panel's: the scroll thumb
    // takes a strip off the right, so the cells -- and the tier headings and
    // rules that introduce them -- sit slightly left of the card's middle.
    const double gridLeft = panel.x + kGridPadding;
    const double gridRight = panel.right() - kGridGutter;
    const double centre = (gridLeft + gridRight) * 0.5;

    panelCard(canvas, panel, kInventorySkin);
    const bool riftBag = ctx.net.inRiftMap();
    const bool chinese = language() == Language::Zh;
    panelTitle(canvas, panel,
               riftBag ? (chinese ? "裂隙临时背包" : "Temporary Rift Bag") : menuTitle(MenuId::Inventory),
               riftBag ? (chinese ? "对局材料不会带回主背包" : "Match items stay in the Rift")
                       : (chinese ? "拖动花瓣以装备" : "Drag a petal to equip it"));

    // --- header controls ---------------------------------------------------
    const Rect closeRect = closeButtonRect(panel);
    if (drawPanelExpand(ctx, closeRect.y + closeRect.h * 0.5, kInventorySkin.fill, expanded_)) {
        expanded_ = !expanded_;
        searchField_.blur();
        scroll_.thumbGrab.reset();
        return true;
    }
    const Rect toggleBoxRect{panel.x + kControlsPad,
                             panel.y + kControlsY + (kControlsHeight - kToggleBoxSize) * 0.5,
                             kToggleBoxSize, kToggleBoxSize};
    const Rect toggleHit{panel.x + kControlsPad, panel.y + kControlsY, kToggleHitWidth,
                         kControlsHeight};
    const double searchX = panel.x + kControlsPad + kToggleHitWidth;
    const double controlsRight = panel.right() - kControlsRightPad;
    const Rect runLootButton{controlsRight - 132.0, panel.y + kControlsY, 132.0, kControlsHeight};
    const double searchRight = canViewRunLoot ? runLootButton.x - 6.0 : controlsRight;
    const Rect searchRect{searchX, panel.y + kControlsY,
                          std::max(60.0, searchRight - searchX), kControlsHeight};
    if (canViewRunLoot) {
        const bool hovered = runLootButton.contains(mouse);
        drawSlotButton(canvas, runLootButton, trEnZh("This Run", "本局收集"), kInventorySkin.fill,
                       hovered, hovered && ctx.pressed());
        if (hovered && ctx.pressed()) {
            openRunLoot(ctx.drag);
            return true;
        }
    }

    // Eased rather than snapped: the toggle is the only animated control on
    // the panel and a hard flip reads as a redraw glitch. The rate matches the
    // browser's per-frame quarter-of-the-gap at 60fps.
    const double target = stacked_ ? 1.0 : 0.0;
    easeToward(stackLerp_, target, kStackToggleEaseSeconds);
    if (std::fabs(stackLerp_ - target) < 0.01) stackLerp_ = target;

    toggleBox(canvas, toggleBoxRect, stackLerp_);
    TextStyle toggleLabel;
    toggleLabel.size = kToggleLabelSize;
    toggleLabel.roundJoin = true;
    text(canvas, tr(Msg::Stack), toggleBoxRect.right() + 5.0,
         toggleBoxRect.y + kToggleBoxSize * 0.5 + 1.0, toggleLabel);

    // No placeholder: the browser's field is a bare white box until it is typed
    // into, and a grey "Search" there reads as a value the filter is applying.
    inputField(canvas, searchRect, search_, "", searchField_.focused, ctx.timeSeconds,
               &searchField_);
    panelClose(canvas, closeRect, closeRect.contains(mouse));

    if (!layout_) layout_ = std::make_shared<LayoutCache>();
    const WorldMaps* maps = riftBag ? nullptr : ctx.renderer.worldMaps();
    if (layout_->dropMapSource != maps) {
        layout_->dropMapSource = maps;
        layout_->dropMaps.clear();
        mapDrops_.clear();
        layout_->width = -1;
        if (maps) for (const auto& map : maps->maps()) {
            if (map.id() != map.templateId() || map.id().rfind("rift_", 0) == 0) continue;
            if (std::none_of(map.elements().begin(), map.elements().end(),
                             [](const MapElement& e) { return e.isSpawnBand(); })) continue;
            layout_->dropMaps.push_back(&map);
        }
    }
    const auto& dropMaps = layout_->dropMaps;
    const std::vector<bool>* dropTypes = nullptr;
    if (!dropMaps.empty()) {
        if (dropMap_ > dropMaps.size()) dropMap_ = 0;
        const Rect left{gridLeft, 132.0 + panel.y, 32.0, 30.0};
        const Rect right{controlsRight - 32.0, left.y, 32.0, left.h};
        if (ctx.pressed() && (left.contains(mouse) || right.contains(mouse))) {
            const std::size_t options = dropMaps.size() + 1;
            dropMap_ = (dropMap_ + (right.contains(mouse) ? 1 : options - 1)) % options;
            scroll_.offset = 0.0;
            tooltipDelay.reset();
            touchInspect.reset();
        }
        drawSlotButton(canvas, left, "‹", kInventorySkin.fill, left.contains(mouse),
                       left.contains(mouse) && ctx.pressed());
        drawSlotButton(canvas, right, "›", kInventorySkin.fill, right.contains(mouse),
                       right.contains(mouse) && ctx.pressed());
        std::string label = chinese ? "地图掉落：全部" : "Map drops: All";
        if (dropMap_ != 0) {
            const auto& map = *dropMaps[dropMap_ - 1];
            label = (chinese ? "地图掉落：" : "Map drops: ") +
                    std::string(biomeTr(map.id(), map.displayName().c_str()));
            auto found = mapDrops_.find(map.id());
            if (found == mapDrops_.end())
                found = mapDrops_.emplace(map.id(), mapDropTypes(map)).first;
            dropTypes = &found->second;
        }
        TextStyle style;
        style.size = 15.0;
        style.align = Align::Centre;
        style.baseline = Baseline::Middle;
        text(canvas, label, (left.right() + right.x) * 0.5, left.y + left.h * 0.5, style);
    }

    constexpr double inventoryHeaderHeight = kHeaderHeight + 26.0;
    const Rect sortButton{gridLeft, panel.y + inventoryHeaderHeight - 25.0, 152.0, 24.0};
    const bool sortHovered = sortButton.contains(mouse);
    if (sortHovered && ctx.pressed()) {
        layout_->sortByCount = !layout_->sortByCount;
        layout_->width = -1;
        scroll_.offset = 0.0;
        tooltipDelay.reset();
        touchInspect.reset();
    }
    const char* order = layout_->sortByCount ? trEnZh("Sort: Quantity", "排序：数量")
                                            : trEnZh("Sort: Default", "排序：默认");
    drawSlotButton(canvas, sortButton, order, kInventorySkin.fill,
                   sortHovered, sortHovered && ctx.pressed());

    // --- content -----------------------------------------------------------
    const Rect view{panel.x + kMenuBorder, panel.y + inventoryHeaderHeight, panel.w - kMenuBorder * 2,
                    std::max(0.0, panel.bottom() - (panel.y + inventoryHeaderHeight) - kMenuBorder)};
    const double visibleH =
        std::max(0.0, panel.bottom() - (panel.y + inventoryHeaderHeight) - kViewBottomPad);
    const double innerWidth = panel.w - kGridPadding - kGridGutter;
    // Leading and trailing whitespace is dropped before matching: the browser
    // trims the field's value, so a stray space must not filter the grid
    // empty. The shared trim takes spaces, tabs, CRs and LFs, which is every
    // blank this field can hold -- editText lets no other control byte into a
    // single-line field.
    const Grid& grid = layout_->get(ctx.craftInventory ? *ctx.craftInventory : profile,
                                    innerWidth, stacked_, search_, dropTypes);
    if (dropTypes && grid.cells.empty()) {
        TextStyle hint;
        hint.size = 14.0;
        hint.align = Align::Centre;
        text(canvas, trEnZh("No matching petals", "没有符合条件的花瓣"), centre, view.y + 26.0, hint);
    }

    constexpr int summaryColumns = 2;
    constexpr double summaryRowHeight = 20.0;
    constexpr double summaryTopPad = 16.0;
    constexpr double summaryHeadingHeight = 24.0;
    const bool showSummary = !ctx.net.isSkinAdmin();
    const double summaryHeight = summaryTopPad + summaryHeadingHeight +
        ((kRarityCount + summaryColumns - 1) / summaryColumns) * summaryRowHeight;
    scroll_.contentHeight = grid.contentHeight + (showSummary ? summaryHeight : 0.0);
    scroll_.viewHeight = visibleH;
    const double maxScroll = scroll_.maxOffset();
    const Rect track = thumbTrack(panel, view.y, visibleH);

    // Thumb drag: grab anywhere in the right strip (gallery's rule). Applied
    // before wheel / touch so a press on the bar is not also a grid fling.
    ThumbDrag& drag = thumbDrag();
    if (!ctx.window.mouseDown(MouseButton::Left)) drag.active = false;
    if (ctx.pressed() && maxScroll > 0 && track.contains(mouse) &&
        !closeRect.contains(mouse) && !toggleHit.contains(mouse) && !searchRect.contains(mouse)) {
        drag.active = true;
        drag.startY = mouse.y;
        drag.startOffset = scroll_.offset;
    }
    if (drag.active) {
        scroll_.offset =
            drag.startOffset + (mouse.y - drag.startY) * (maxScroll / std::max(1.0, visibleH));
    } else if (panel.contains(mouse)) {
        // The browser's wheel listener is on the whole canvas, so the title and
        // the controls row scroll the grid too.
        scroll_.offset -= ctx.wheel() * kWheelStep;
    }
    // Scroll/hold gestures may suppress the synthetic mouse press that normally
    // blurs search. Release the caret on the real grid touch before the app's
    // per-frame keyboard ensure can reopen the IME while inspecting a petal.
    for (const auto& touch : ctx.window.touchEvents()) {
        if (touch.phase == TouchPhase::Began &&
            view.contains({touch.point.x, touch.point.y})) {
            searchField_.blur();
            break;
        }
    }
    // A finger drags the grid itself, not the header: the search field and
    // the toggle keep answering the moment they are touched. Skip while the
    // thumb is held -- that contact already owns the scroll.
    if (!drag.active) {
        scroll_.offset -= touchScroll(ctx.window, view, maxScroll > 0);
    }
    scroll_.offset = clamp(scroll_.offset, 0.0, maxScroll);

    const double originX = panel.x;
    const double originY = view.y - scroll_.offset;
    const double viewBottom = view.y + visibleH;

    // The browser rejects the header strip and then tests every laid-out cell,
    // including one the cull below will not paint, so the hit test is its own
    // pass rather than a side effect of drawing.
    //
    // Not while a finger is dragging the grid (gallery's rule): the cell under
    // it rides with the scroll, and a tip that followed would be noise. A TAP
    // still hits -- on a phone that is how petal stats are read at all.
    // Also quiet while the thumb is dragged: the strip is not a petal cell.
    int hovered = -1;
    Rect hoveredRect{};
    const bool touchPointer = ctx.window.pointerIsTouch();
    const bool touchScrolling = ctx.window.touchScrolling();
    if (panel.contains(mouse) && mouse.y >= view.y && !drag.active &&
        !(touchPointer && touchScrolling) && !track.contains(mouse)) {
        const double localY = mouse.y - originY;
        const auto row = std::lower_bound(grid.cells.begin(), grid.cells.end(), localY,
            [](const Cell& cell, double y) { return cell.rect.bottom() < y; });
        for (auto it = row; it != grid.cells.end() && it->rect.y <= localY; ++it) {
            const Cell& cell = *it;
            const Rect rect{originX + cell.rect.x, originY + cell.rect.y, cell.rect.w,
                            cell.rect.h};
            if (!rect.contains(mouse)) continue;
            hovered = static_cast<int>(it - grid.cells.begin());
            hoveredRect = rect;
            break;
        }
    }

    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(view.x), static_cast<float>(view.y), static_cast<float>(view.w),
                static_cast<float>(view.h));
    canvas.clip();

    for (const Section& section : grid.sections) {
        const double labelY = originY + section.labelY;
        if (labelY < view.y - kLabelHeight || labelY >= viewBottom) continue;

        TextStyle label;
        label.size = kSectionLabelSize;
        label.align = Align::Centre;
        label.baseline = Baseline::Bottom;
        label.fill = rarityColor(section.rarity);
        const std::string caption = rarityTr(section.rarity);

        // The heading's outline is 60% black, not solid -- solid reads a whole
        // weight heavier. TextStyle carries no stroke alpha, so the outlined
        // text goes down once under a global alpha and the fill is then laid
        // over it opaque, which leaves exactly the browser's two layers.
        canvas.setGlobalAlpha(0.6f);
        text(canvas, caption, centre, labelY, label);
        canvas.setGlobalAlpha(1.0f);
        label.strokeWidth = 0.0;
        text(canvas, caption, centre, labelY, label);

        // Rules out to the grid's own edges, stopping clear of the text. They
        // are what makes a tier read as a section rather than a stray heading,
        // and running them to the GRID rather than to the panel is what keeps
        // the heading centred over the cells it introduces.
        const double half = measure(caption, label.size) * 0.5;
        const double ruleY = labelY - 6.0;
        setStroke(canvas, kInventorySkin.accent);
        canvas.setLineWidth(static_cast<float>(kRuleWidth));
        canvas.setLineCap("round");
        if (centre - half - kRuleGap > gridLeft) {
            canvas.beginPath();
            canvas.moveTo(static_cast<float>(gridLeft), static_cast<float>(ruleY));
            canvas.lineTo(static_cast<float>(centre - half - kRuleGap), static_cast<float>(ruleY));
            canvas.moveTo(static_cast<float>(centre + half + kRuleGap), static_cast<float>(ruleY));
            canvas.lineTo(static_cast<float>(gridRight), static_cast<float>(ruleY));
            canvas.stroke();
        }
        canvas.setLineCap("butt");
    }

#ifdef __EMSCRIPTEN__
    const bool settled = layout_->lastPaintScroll == scroll_.offset;
    layout_->lastPaintScroll = scroll_.offset;
#endif
    const auto firstVisible = std::lower_bound(grid.cells.begin(), grid.cells.end(), view.y - originY,
        [](const Cell& cell, double top) { return cell.rect.bottom() < top; });
    for (auto it = firstVisible; it != grid.cells.end(); ++it) {
        const Cell& cell = *it;
        const Rect rect{originX + cell.rect.x, originY + cell.rect.y, cell.rect.w, cell.rect.h};
        if (rect.y >= viewBottom) break;
        if (rect.bottom() <= view.y) continue;

        const std::uint32_t badgeCount = !ctx.net.isSkinAdmin() && cell.count > 1 ? cell.count : 0;
        const auto paintTile = [&](Canvas& target, Rect bounds) {
            ItemTile tile;
            tile.petalIndex = cell.petalIndex;
            tile.rarity = cell.rarity;
            tile.timeSeconds = ctx.timeSeconds;
            if (badgeCount) tile.badge = "x" + stackCountText(badgeCount);
            drawItemTile(target, ctx.sprites, bounds, tile);
        };
#ifdef __EMSCRIPTEN__
        if (settled && Fonts::ready() && !ctx.sprites.petalAnimated(cell.petalIndex)) {
            // Only inventory cells have this fixed appearance. The padding
            // includes the tilted count badge outside the upper corner.
            static const char inventoryArt = 0;
            const std::uint64_t variant = (std::uint64_t{badgeCount} << 32) |
                (static_cast<std::uint64_t>(language()) << 24) |
                (static_cast<std::uint64_t>(cell.rarity) << 16) | cell.petalIndex;
            const double pad = rect.w * 0.25;
            if (drawCachedPicture(canvas, &inventoryArt, variant, rect.x - pad, rect.y - pad,
                                  rect.w + 2 * pad, rect.h + 2 * pad, [&](Canvas& bitmap) {
                    paintTile(bitmap, {pad, pad, rect.w, rect.h});
                })) continue;
        }
#endif
        paintTile(canvas, rect);
    }
    const double summaryY = originY + grid.contentHeight + summaryTopPad;
    if (showSummary && summaryY < viewBottom) {
        TextStyle heading;
        heading.size = 14.0;
        heading.align = Align::Centre;
        heading.baseline = Baseline::Middle;
        text(canvas, trEnZh("Bag totals by rarity", "背包各品质总数"), centre, summaryY + 9.0, heading);
        TextStyle count;
        count.size = 12.0;
        count.align = Align::Centre;
        count.baseline = Baseline::Middle;
        const double columnWidth = (gridRight - gridLeft) / summaryColumns;
        for (int tier = 0; tier < kRarityCount; ++tier) {
            const auto rarity = static_cast<Rarity>(tier);
            const std::string label = std::string(rarityTr(rarity)) + " × " +
                std::to_string(layout_->inventoryTotals[tier]);
            const double x = gridLeft + (tier % summaryColumns + 0.5) * columnWidth;
            const double y = summaryY + summaryHeadingHeight + (tier / summaryColumns + 0.5) * summaryRowHeight;
            text(canvas, label, x, y, count);
        }
    }
    canvas.restore();

    scrollThumb(canvas, panel, view.y, visibleH, scroll_.contentHeight, scroll_.offset);

    // --- tooltip -----------------------------------------------------------
    // Anchored to the CELL rather than the cursor. Mouse: after the dwell, and
    // a press cancels it (drag starts next). Touch: gallery-style -- show
    // while the finger rests on a cell and is not pressing, dragging, or
    // scrolling. Press / drag / click-to-equip are unchanged; the tip is only
    // painted when those are idle.
    const bool pointerDown = ctx.window.mouseDown(MouseButton::Left) ||
                             ctx.window.mousePressed(MouseButton::Left) || ctx.drag.active();
    const auto& pan=ctx.window.touchPan();
    const bool holdTip = touchInspect.touch(ctx.window,ctx.timeSeconds,[&](Vec2 point){
        if(!view.contains(point)||track.contains(point))return -1;
        for(std::size_t i=0;i<grid.cells.size();++i){const auto& cell=grid.cells[i];
            if(Rect{originX+cell.rect.x,originY+cell.rect.y,cell.rect.w,cell.rect.h}.contains(point))return static_cast<int>(i);}
        return -1;
    },pan.active && view.contains({pan.originX,pan.originY}));
    if(holdTip && ctx.drag.source==DragState::Source::Inventory)ctx.drag.clear();
    const int tipIndex=holdTip?touchInspect.target:hovered;
    const bool showTooltip = holdTip || (hovered >= 0 &&
        (!touchPointer && tooltipDelay.update(hovered,ctx.timeSeconds,pointerDown)));
    if (showTooltip) {
        const Cell& cell = grid.cells[static_cast<std::size_t>(tipIndex)];
        // The reload the bar would actually run, as the slot wedge times it.
        const double reloadScale =
            profile.skills.reloadScale() *
            (1.0 - barReloadCut(content(), [&](int i) { return profile.equipped(content(), i); }, &profile.skills));
        auto lines =
            petalTooltipLines(cell.petalIndex, cell.rarity, profile.skills, reloadScale);
        if (!ctx.net.isSkinAdmin()) {
            const std::string quantity = trEnZh("In inventory: ", "背包持有：") +
                                         std::to_string(cell.count);
            lines.insert(lines.begin() + 2, TooltipLine{quantity, 14.0});
        }
        if (cell.petalIndex == content().petalIndex("timed_mjolnir")) {
            showTimedHammerExpiry(lines, profile, -1);
        }
        const bool alt = ctx.altMode;
        const Vec2 size = measureTooltip(lines, 0.0, 0.0, alt);
        const Rect tipRect{originX+cell.rect.x,originY+cell.rect.y,cell.rect.w,cell.rect.h};
        const Vec2 at = tooltipAnchor(tipRect, size, canvas.width(), canvas.height());
        paintTooltip(canvas, at.x, at.y, lines, 0.0, 0.0, alt);
    }

    // --- input -------------------------------------------------------------
    // The field's pointer handling runs unless Close or Stack is taking this
    // press: neither of those disturbs the caret in the browser, and blurring
    // on them would lose a half-typed filter to a stray click on the toggle.
    if (!(ctx.pressed() && (closeRect.contains(mouse) || toggleHit.contains(mouse)))) {
        trackTextMouse(ctx.window, searchField_, searchRect,
                       inputFieldRun(searchRect, search_, searchField_), search_,
                       ctx.timeSeconds);
    }
    if (searchField_.focused) {
        ctx.wantsText = true;
        TextEditOptions typing;
        typing.maxBytes = kSearchLimit;
        editText(ctx.window, search_, searchField_, ctx.timeSeconds, typing);
        if (ctx.window.keyPressed(Key::Enter) || ctx.window.keyPressed(Key::Escape)) {
            searchField_.blur();
        }
    }
    if (searchRect.contains(mouse)) ctx.window.setCursorShape(CursorShape::Text);
    // Held through a press as well: a drag starts on the press, and the hand
    // flipping back to an arrow the moment a petal is picked up reads as lost.
    if (hovered >= 0) ctx.window.setCursorShape(CursorShape::Hand);
    if (maxScroll > 0 && (track.contains(mouse) || drag.active)) {
        ctx.window.setCursorShape(CursorShape::Hand);
    }

    if (ctx.pressed()) {
        // Close and Stack act on PRESS, as the browser's mousedown handler
        // does. Both also swallow the press rather than letting it reach the
        // field or a cell, and neither disturbs the search focus -- the
        // browser's preventDefault leaves the input focused where it was.
        if (closeRect.contains(mouse)) return false;
        if (toggleHit.contains(mouse)) {
            // Scroll survives the flip: it is re-clamped against the new
            // content height on the next frame, which is all the browser does.
            stacked_ = !stacked_;
            return true;
        }
        // The field answered for its own press above.
        if (searchRect.contains(mouse)) return true;
        // Thumb strip owns the press: do not also pick up a petal under it.
        if (drag.active || track.contains(mouse)) return true;
        if (hovered >= 0 && !ctx.drag.active()) {
            const Cell& cell = grid.cells[static_cast<std::size_t>(hovered)];
            if (ctx.craftInventory != nullptr && ctx.craftInventory != &profile &&
                profile.stackCount(cell.petalIndex, cell.rarity) == 0) return true;
            ctx.drag.inventoryPress = mouse;
            ctx.drag.source = DragState::Source::Inventory;
            ctx.drag.petalIndex = cell.petalIndex;
            ctx.drag.rarity = cell.rarity;
            ctx.drag.slot = -1;
        }
    }

    if (ctx.released() && touchInspect.consumedRelease) {
        if(ctx.drag.source==DragState::Source::Inventory)ctx.drag.clear();
        return true;
    }
    if (ctx.released() && hovered >= 0 && ctx.drag.source == DragState::Source::Inventory &&
        ctx.drag.petalIndex == grid.cells[static_cast<std::size_t>(hovered)].petalIndex &&
        ctx.drag.rarity == grid.cells[static_cast<std::size_t>(hovered)].rarity) {
        // Released on the cell it was picked up from: a click, not a drag.
        // Equip it into the first free slot, which is what a player wants nine
        // times out of ten and saves the trip to the bar.
        for (std::size_t slot = 0; slot < kLoadoutSlots; ++slot) {
            if (slot % kLoadoutActiveSlots >= static_cast<std::size_t>(profile.skills.loadoutSlots())) continue;
            if (slot < profile.loadout.size() && !profile.loadout[slot].empty()) continue;
            ctx.net.setLoadoutSlot(static_cast<int>(slot), ctx.drag.petalIndex, ctx.drag.rarity);
            break;
        }
        ctx.drag.clear();
    }
    return true;
}

} // namespace flix
