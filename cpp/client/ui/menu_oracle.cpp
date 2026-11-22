// The oracle.
//
// Fixed-price guaranteed upgrades, Common through Ultra -> Super.
// Quantity is chosen with the batch controls; no account cooldown applies.
// The server checks reach, inventory and output capacity atomically.
//
// The slot moves the way florr's oracle window moves it (its draw,
// 0x1008125f0): the staged petal grows in turning once backwards; a craft
// spins the slot up toward 10 rad/s and swings it out from the centre on a
// sine of the time since the click as the shake eases in; the upgrade, 90
// across, pops in at the centre with a 200 ms pulse and a burst of rising
// grains in its tier's colour, while the plate fades and the offer vanishes,
// turning. florr spins for as long as its server takes to answer; this one
// answers at once, so the slot spins for kCraftSpinMs at the least.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <string>

#include "client/ui/content_i18n.h"
#include "client/ui/item_tile.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "shared/game/config.h"
#include "shared/game/npc.h"

namespace flix {

using namespace flix::ui;

namespace {


/// The craft's spin: toward 10 rad/s while it is out, easing both ways at
/// the same rate. The least it spins before its answer shows is ours.
constexpr double kSpinTarget = 10.0;
constexpr double kSpinRate = 0.00632163093946958;
constexpr double kCraftSpinMs = 1000.0;
/// The upgrade's grains mostly rise: y velocity from -500 to 200, and any
/// starting angle.
constexpr SlotBurstShape kBurst{200.0, 0.0, kTau};
/// A craft that never hears back drops to idle after this: it resolved
/// server-side either way, and the profile will say how.
constexpr double kCraftTimeoutSeconds = 8.0;
/// How long a refusal stays under the slot.
constexpr double kRefusalSeconds = 3.0;

std::string forecastTime(std::int64_t at) {
    if (at < 0) return trEnZh("No listing in the next 7 days", "未来 7 天未上架");
    constexpr std::int64_t beijingOffset = 8 * 3600;
    const auto local = static_cast<std::time_t>(at + beijingOffset);
    char date[32]{};
    if (const auto* calendar = std::gmtime(&local))
        std::strftime(date, sizeof(date), "%m-%d %H:%M", calendar);
    return std::string(date) + trEnZh(" (UTC+8)", "（北京时间）");
}

} // namespace

Vec2 OraclePanel::size(const Profile& profile) {
    constexpr auto tiers = SlotGridTiers::PermanentHyper;
    return {slotCardWidth(tiers), slotCardHeight(SlotGrid::rows(profile, tiers))};
}

void OraclePanel::reset() {
    grid_.reset();
    motion_.clear();
    stagedPetal_ = kNoPetal;
    crafts_ = 0;
    phase_ = Phase::Idle;
    resultPending_ = false;
    refusal_.clear();
    shopForecastOpen_ = false;
    shopSearch_.clear();
    shopSearchField_.blur();
    shopSearchMatches_.clear();
    shopPetal_ = kNoPetal;
}

void OraclePanel::stage(const Profile& profile, std::uint16_t petalIndex, Rarity rarity) {
    const int cost = oracleCraftCost(rarity);
    if (cost <= 0 ||
        (rarity == Rarity::PermanentHyper && !petalCraftsToApex(content(), petalIndex))) return;
    if (profile.stackCount(petalIndex, rarity) < static_cast<std::uint32_t>(cost)) return;
    // One upgrade, so a click REPLACES what was staged rather than adding to
    // it: there is only ever one price in the slot.
    stagedPetal_ = petalIndex;
    stagedRarity_ = rarity;
    crafts_ = 1;
    // Whatever was refused, the player has moved on from it.
    refusal_.clear();
}

void OraclePanel::renderShopForecast(MenuContext& ctx, const SlotCardLayout& card) {
    const Rect panel = card.panel;
    const double columnWidth = (panel.w - 56.0) * 0.5;
    const Rect searchRect{panel.x + 20.0, panel.y + 105.0, columnWidth, 28.0};
    if (editSlotSearch(ctx, searchRect, shopSearch_, shopSearchField_)) {
        shopSearchMatches_.clear();
        shopPetal_ = kNoPetal;
        if (!shopSearch_.empty()) {
            for (const auto index : content().petalDisplayOrder())
                if (shopSellsPetal(index)) shopSearchMatches_.push_back(index);
            filterSlotTypes(shopSearchMatches_, shopSearch_);
        }
    }

    const double firstRow = searchRect.bottom() + 9.0;
    const auto resultRows = static_cast<std::size_t>(
        std::clamp((panel.bottom() - firstRow - 12.0) / 34.0, 0.0, 5.0));
    const auto mouse = ctx.mouse();
    const auto rotation = shopRotation(shopClockNow());
    const auto lookup = [&](std::uint16_t index) {
        shopPetal_ = index;
        shopAskedAt_ = ctx.timeSeconds;
        shopAskedRotation_ = rotation;
        ctx.net.requestOracleShopForecast(index);
    };
    for (std::size_t i = 0; i < std::min(resultRows, shopSearchMatches_.size()); ++i) {
        const auto index = shopSearchMatches_[i];
        const auto& petal = content().petal(index);
        const Rect row{searchRect.x, firstRow + i * 34.0, columnWidth, 28.0};
        const bool over = row.contains(mouse);
        drawSlotButton(ctx.canvas, row, petalNameTr(petal.id, petal.name.c_str()),
                       shopPetal_ == index ? std::optional<std::uint32_t>(0x5A9FDBu) : std::nullopt,
                       over, over && ctx.window.mouseDown(MouseButton::Left));
        if (over && ctx.pressed()) lookup(index);
    }
    if (shopPetal_ != kNoPetal && shopAskedRotation_ != rotation) lookup(shopPetal_);
    SlotCardLayout line = card;
    line.centreX = searchRect.x + columnWidth * 0.5;
    line.lineY = firstRow + (resultRows == 0 ? 0.0 : 14.0);
    if (shopSearchMatches_.empty())
        drawSlotLine(ctx.canvas, line, shopSearch_.empty()
            ? trEnZh("Search by petal name", "输入花瓣名称")
            : trEnZh("No rotating petal matches", "轮换货架没有匹配的花瓣"));
    line.centreX = panel.right() - 20.0 - columnWidth * 0.5;
    const auto& result = ctx.net.oracleShopForecast();
    if (shopPetal_ == kNoPetal) {
        drawSlotLine(ctx.canvas, line, trEnZh("Choose a petal to see its next listing", "选择花瓣，查询下次上架时间"));
        return;
    }
    const auto& petal = content().petal(shopPetal_);
    drawSlotLine(ctx.canvas, line, petalNameTr(petal.id, petal.name.c_str()));
    line.lineY += 32.0;
    if (!result || result->petalIndex != shopPetal_) {
        drawSlotLine(ctx.canvas, line, ctx.timeSeconds - shopAskedAt_ < kCraftTimeoutSeconds
            ? trEnZh("Looking up the shop rotation…", "正在查询轮换…")
            : trEnZh("No response. Select the petal to retry.", "查询超时，重新选择花瓣可重试。"));
        return;
    }
    if (!result->available) {
        drawSlotLine(ctx.canvas, line, trEnZh("Ask while standing near an oracle.", "请靠近神使后重试。"));
        return;
    }
    if (petalComesAt(petal, Rarity::Super)) {
        drawSlotLine(ctx.canvas, line, std::string(trEnZh("Super  ", "超级  ")) + forecastTime(result->superAt),
                     rarityColor(Rarity::Super));
        line.lineY += 32.0;
    }
    if (petalComesAt(petal, Rarity::Unique))
        drawSlotLine(ctx.canvas, line, std::string(trEnZh("Eternal  ", "永恒  ")) + forecastTime(result->eternalAt),
                     rarityColor(Rarity::Unique));
}

bool OraclePanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Vec2 mouse = ctx.mouse();
    const double now = ctx.timeSeconds;
    const double dtMs = std::max(0.0, ctx.dt) * 1000.0;

    const auto removeCraft = [this]() {
        crafts_ = 0;
        stagedPetal_ = kNoPetal;
    };
    // The account's wait for its next craft, counted down on this client's
    // clock from what the last profile said.
    const double cooldown = 0.0;
    const bool waiting = cooldown > 0.0;
    const auto land = [this]() {
        phase_ = Phase::Result;
        resultPending_ = false;
    };

    // A result the server sent while this face of the menu was not showing
    // still has to land, so it is read here rather than in the click.
    OracleOutcome& outcome = ctx.net.oracleOutcome();
    if (outcome.pending) {
        outcome.pending = false;
        if (outcome.success) {
            resultPetal_ = outcome.petalIndex;
            resultRarity_ = outcome.rarity;
            resultCount_ = outcome.crafted;
            // Mid-spin it is only recorded -- the spin owns when it lands.
            if (phase_ == Phase::Pulsing && (now - phaseStarted_) * 1000.0 < kCraftSpinMs) {
                resultPending_ = true;
            } else {
                land();
            }
        } else {
            // Refused: nothing left the inventory, so what was offered goes
            // straight back into the slot, with the reason under it.
            if (phase_ == Phase::Pulsing) {
                stagedPetal_ = offeredPetal_;
                stagedRarity_ = offeredRarity_;
                crafts_ = offeredCrafts_;
                phase_ = Phase::Idle;
            }
            refusal_ = outcome.reason.empty() ? "The oracle refused." : outcome.reason;
            refusalUntil_ = now + kRefusalSeconds;
        }
    }

    // Nothing can be staged while the oracle is not taking crafts.
    if (waiting && phase_ == Phase::Idle) removeCraft();

    // The staging area cannot outlive the petals behind it -- but never while
    // the pulse runs: those are already the server's, and a profile landing a
    // frame ahead of the result would empty the slot under its own animation.
    if (phase_ != Phase::Pulsing && knownPetal(stagedPetal_)) {
        const int cost = oracleCraftCost(stagedRarity_);
        const int affordable =
            cost > 0 ? static_cast<int>(profile.stackCount(stagedPetal_, stagedRarity_) /
                                        static_cast<std::uint32_t>(cost))
                     : 0;
        if (affordable < crafts_) {
            crafts_ = affordable;
            if (crafts_ <= 0) stagedPetal_ = kNoPetal;
        }
    }

    const char* title = shopForecastOpen_ ? trEnZh("Shop rotation forecast", "商店轮换查询")
                                          : trEnZh("Oracle", "神使");
    SlotCardLayout card = drawSlotCard(canvas, ctx.bounds, kOracleSkin, title, "Craft", mouse);
    const Rect panel = card.panel;
    const Rect mode{panel.right() - 172.0, panel.y + 61.0, 152.0, 30.0};
    if (phase_ != Phase::Pulsing) {
        const bool over = mode.contains(mouse);
        drawSlotButton(canvas, mode, shopForecastOpen_ ? trEnZh("Back to crafting", "返回兑换")
                                                       : trEnZh("Shop forecast", "查询商店轮换"),
                       std::nullopt, over, over && ctx.window.mouseDown(MouseButton::Left));
        if (over && ctx.pressed()) {
            shopForecastOpen_ = !shopForecastOpen_;
            shopSearchField_.blur();
            return true;
        }
    }
    if (shopForecastOpen_) {
        if (card.close.contains(mouse) && ctx.pressed()) return false;
        renderShopForecast(ctx, card);
        return true;
    }
    const double centre = card.button.x + card.button.w * 0.5;
    card.button.x = centre - 79.0;
    card.button.w = 158.0;
    const double controlsY = card.button.bottom() + 9.0;
    const Rect minus{card.button.x, controlsY, 36, 28};
    const Rect plus{minus.right() + 8, controlsY, 36, 28};
    const Rect maximum{plus.right() + 8, controlsY, 70, 28};
    if (phase_ == Phase::Idle && knownPetal(stagedPetal_)) {
        drawSlotButton(canvas, minus, "−", std::nullopt, minus.contains(mouse), false);
        drawSlotButton(canvas, plus, "+", std::nullopt, plus.contains(mouse), false);
        drawSlotButton(canvas, maximum, trEnZh("Max", "最大"), std::nullopt, maximum.contains(mouse), false);
        const int limit = std::min(65535, static_cast<int>(profile.stackCount(stagedPetal_, stagedRarity_) /
            static_cast<std::uint32_t>(oracleCraftCost(stagedRarity_))));
        if (ctx.pressed()) {
            if (minus.contains(mouse)) crafts_ = std::max(1, crafts_ - 1);
            if (plus.contains(mouse)) crafts_ = std::min(limit, crafts_ + 1);
            if (maximum.contains(mouse)) crafts_ = limit;
        }
    }

    if (phase_ == Phase::Pulsing) {
        const double elapsed = now - phaseStarted_;
        if (elapsed * 1000.0 >= kCraftSpinMs && resultPending_) {
            land();
        } else if (elapsed >= kCraftTimeoutSeconds) {
            phase_ = Phase::Idle;
        }
    }
    const bool upgraded = phase_ == Phase::Result && knownPetal(resultPetal_);
    motion_.stepRoll(dtMs, phase_ == Phase::Pulsing, upgraded, kSpinTarget, kSpinRate, kSpinRate);

    // --- the slot ----------------------------------------------------------
    const std::uint16_t shown = phase_ == Phase::Pulsing ? offeredPetal_ : stagedPetal_;
    const Rarity shownRarity = phase_ == Phase::Pulsing ? offeredRarity_ : stagedRarity_;
    const int shownCrafts = phase_ == Phase::Pulsing ? offeredCrafts_ : crafts_;
    ItemTile shownTile;
    shownTile.petalIndex = shown;
    shownTile.rarity = shownRarity;
    // The badge counts PETALS, the way a stack is counted everywhere else:
    // this is how many of them the oracle is being handed.
    if (knownPetal(shown)) {
        shownTile.badge = "x" + std::to_string(shownCrafts * oracleCraftCost(shownRarity));
    }
    shownTile.timeSeconds = now;
    // Lit under the cursor only while the slot is at rest.
    shownTile.hovered =
        motion_.spin() < 0.05 && phase_ == Phase::Idle && card.slotRect.contains(mouse);
    ItemTile upgradeTile;
    upgradeTile.petalIndex = resultPetal_;
    upgradeTile.rarity = resultRarity_;
    if (resultCount_ > 1) upgradeTile.badge = "x" + std::to_string(resultCount_);
    upgradeTile.timeSeconds = now;
    const auto key = [](std::uint16_t petal, Rarity rarity) {
        return (static_cast<std::uint32_t>(petal) << 8) |
               static_cast<std::uint32_t>(rarityIndex(rarity));
    };
    motion_.drawSlotScene(canvas, ctx.sprites, card, kOracleSkin, (now - phaseStarted_) * 1000.0,
                          dtMs, !upgraded && knownPetal(shown) ? &shownTile : nullptr,
                          key(shown, shownRarity), upgraded ? &upgradeTile : nullptr,
                          key(resultPetal_, resultRarity_) | 0x80000000u, kBurst);

    // A refusal, under the slot, for as long as it lasts. The reference has no
    // line here; the only thing that ever puts one there is the oracle saying
    // no, which is worth saying where the player is looking.
    if (!refusal_.empty() && now < refusalUntil_) {
        drawSlotRefusal(canvas, card, refusal_);
    } else {
        refusal_.clear();
    }

    // --- craft button ------------------------------------------------------
    // florr's grey button at rest, and the colour of the tier being bought
    // once there is something to buy -- the forge's rule, so the faces of the
    // menu answer "is anything staged" the same way.
    const bool canCraft =
        !waiting && phase_ == Phase::Idle && knownPetal(stagedPetal_) && crafts_ > 0;
    const bool overButton = card.button.contains(mouse);
    const std::string craftLabel = std::string(trEnZh("Craft ×", "兑换 ×")) + std::to_string(crafts_);
    drawSlotButton(canvas, card.button, craftLabel.c_str(),
                   canCraft ? std::optional<std::uint32_t>(rarityColor(upgradeRarity(stagedRarity_)))
                            : std::nullopt,
                   overButton, overButton && ctx.window.mouseDown(MouseButton::Left));

    // The line says how long to wait while there is a wait, in red.
    if (waiting) {
        drawSlotLine(canvas, card, oracleCooldownText(cooldown), kSlotCardWaitInk);
    } else {
        drawSlotLine(canvas, card, trEnZh("Apex: 120 matching Permanent Hyper petals",
                                           "兑换 Apex：需要 120 个相同的永久超凡"),
                     rarityColor(Rarity::Ascendant));
    }

    // --- the grid ----------------------------------------------------------
    // Every cell says "owned/price". Held but short of the column's price: on
    // a grey plate, and not clickable -- the petal keeps its colours, and the
    // label still says how far short, "4/19" being the reason to go farm.
    // While the oracle is not taking crafts, EVERY stack is on grey and says
    // only how many there are: there is no price to be short of until the
    // wait is over.
    const std::uint32_t held =
        knownPetal(stagedPetal_)
            ? static_cast<std::uint32_t>(std::max(0, crafts_)) *
                  static_cast<std::uint32_t>(oracleCraftCost(stagedRarity_))
            : 0u;
    const std::optional<SlotGrid::Pick> hovered = grid_.render(
        ctx, card, kOracleSkin, SlotGridTiers::PermanentHyper,
        [&](std::uint16_t petalIndex, Rarity rarity, std::uint32_t owned) {
            SlotCell cell;
            cell.count = owned;
            if (petalIndex == stagedPetal_ && rarity == stagedRarity_) {
                cell.count = owned > held ? owned - held : 0;
            }
            const std::uint32_t price =
                static_cast<std::uint32_t>(std::max(1, oracleCraftCost(rarity)));
            cell.greyed = oracleCraftCost(rarity) <= 0 || waiting || cell.count < price ||
                (rarity == Rarity::PermanentHyper && !petalCraftsToApex(content(), petalIndex));
            cell.badge = waiting ? "x" + stackCountText(cell.count)
                                 : stackCountText(cell.count) + "/" + std::to_string(price);
            cell.badgeCentred = true;
            return cell;
        });

    // Tiles on their way out go over everything but the words.
    motion_.drawVanishing(canvas, ctx.sprites, dtMs);

    // --- input -------------------------------------------------------------
    // On press, as the forge answers: a press that starts on a cell and
    // drifts off must not still fire.
    const bool rightPressed = panel.contains(mouse) && ctx.window.mousePressed(MouseButton::Right);
    if (phase_ == Phase::Idle && rightPressed) {
        removeCraft();
        return true;
    }
    if (!ctx.pressed() && !rightPressed) return true;
    if (card.close.contains(mouse) && ctx.pressed()) return false;

    // The upgrade sits there until it is dismissed, and the press that
    // dismisses it does nothing else.
    if (phase_ == Phase::Result && panel.contains(mouse)) {
        phase_ = Phase::Idle;
        resultPetal_ = kNoPetal;
        return true;
    }

    if (card.button.contains(mouse)) {
        if (canCraft) {
            phase_ = Phase::Pulsing;
            phaseStarted_ = now;
            resultPending_ = false;
            offeredPetal_ = stagedPetal_;
            offeredRarity_ = stagedRarity_;
            offeredCrafts_ = crafts_;
            refusal_.clear();
            ctx.net.requestOracleCraft(stagedPetal_, stagedRarity_, static_cast<std::uint16_t>(crafts_));
            // Handed over: the slot draws the offer's copy from here, and a
            // refusal is what puts it back.
            stagedPetal_ = kNoPetal;
            crafts_ = 0;
        }
        return true;
    }

    if (phase_ == Phase::Idle && card.slotRect.contains(mouse)) {
        removeCraft();
        return true;
    }

    if (hovered && phase_ == Phase::Idle) {
        if (hovered->rarity == Rarity::PermanentHyper && !petalCraftsToApex(content(), hovered->petalIndex)) {
            refusal_ = tr(Msg::ApexOwnSource);
            refusalUntil_ = now + kRefusalSeconds;
        } else {
            stage(profile, hovered->petalIndex, hovered->rarity);
        }
    }
    return true;
}

} // namespace flix
