// The trader.
//
// Five-petal draws stay available during the daily coin-sale cooldown.
//
// The slot moves the way florr's trade window moves it (its draw,
// 0x100849600; ~/florr_images/menus_skia.json):
//
//   - the petal grows into the slot, turning once backwards;
//   - a trade spins the slot up toward 10 rad/s and swings it out from the
//     centre on a sine of the time since the click, 50 deep, as the shake
//     eases in -- the petal moves round the centre but never turns;
//   - the coin, 90 across, pops in where the slot was -- a forward turn as
//     it grows, then a 200 ms pulse -- with a burst of grains in its tier's
//     colour, while the slot's plate fades and the petal vanishes, turning;
//   - collecting the coin vanishes it the same way.
//
// florr spins for as long as its server takes to answer; this one answers
// at once, so the slot spins for kTradeSpinMs at the least.

#include <algorithm>
#include <cstdint>
#include <string>

#include "client/ui/item_tile.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "shared/game/config.h"
#include "shared/game/npc.h"
#include "shared/game/hel_plinko.h"

namespace flix {

using namespace flix::ui;

namespace {

/// The trade's spin: toward 10 rad/s while it is out, easing both ways at
/// the same rate.
constexpr double kSpinTarget = 10.0;
constexpr double kSpinRate = 0.00632163093946958;
/// The least a trade spins before its answer shows -- ours, not florr's.
constexpr double kTradeSpinMs = 1000.0;
/// How long a handed-over petal waits for the trader's answer before the
/// slot gives up on it. The trade resolved server-side either way, and the
/// profile will say how.
constexpr double kTradeTimeoutSeconds = 8.0;
constexpr double kRefusalSeconds = 3.0;
constexpr double kTradeConfirmDelay = 0.35;

bool tradable(std::uint16_t petalIndex) {
    if (!knownPetal(petalIndex)) return false;
    const auto& petal = content().petal(petalIndex);
    return petal.tradable && petal.id != kPlinkoChipPetal;
}

std::uint32_t tileKey(std::uint16_t petalIndex, Rarity rarity) {
    return (static_cast<std::uint32_t>(petalIndex) << 8) |
           static_cast<std::uint32_t>(rarityIndex(rarity));
}

} // namespace

Vec2 TradePanel::size(const Profile& profile) {
    return {slotCardWidth(SlotGridTiers::Trader),
            slotCardHeight(SlotGrid::rows(profile, SlotGridTiers::Trader))};
}

void TradePanel::reset() {
    ingredients_.clear();
    drawMode_ = true;
    grid_.reset();
    motion_.clear();
    stagedPetal_ = kNoPetal;
    phase_ = Phase::Idle;
    resultPending_ = false;
    tradeConfirmReady_.reset();
    refusal_.clear();
}

bool TradePanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Vec2 mouse = ctx.mouse();
    const double now = ctx.timeSeconds;
    const double dtMs = std::max(0.0, ctx.dt) * 1000.0;

    // The account's wait for its next trade, counted down on this client's
    // clock from what the last profile said.
    const double cooldown = ctx.net.traderCooldownRemainingMillis();
    const bool waiting = cooldown > 0.0;

    const auto coinIndex = content().petalIndex(kTraderCoinPetal);
    const auto eligible = [&](std::uint16_t index, Rarity rarity, unsigned owned) {
        if (!knownPetal(index)) return false;
        if (index == coinIndex) return false;
        return owned > 0 && tradable(index) && traderTakes(rarity) && !waiting;
    };

    // A result the server sent while this face of the menu was not showing
    // still has to land, so it is read here rather than in the click.
    TradeOutcome& outcome = ctx.net.tradeOutcome();
    if (outcome.pending) {
        outcome.pending = false;
        if (outcome.success && knownPetal(outcome.receivedIndex)) {
            ingredients_.clear();
            resultPetal_ = outcome.receivedIndex;
            resultRarity_ = outcome.rarity;
            resultPending_ = true;
        } else if (!outcome.success) {
            // Refused: nothing left the inventory, so what was offered goes
            // straight back into the slot, with the reason under it -- and
            // the spin runs down, as florr's does on a failure.
            if (phase_ == Phase::Trading) {
                stagedPetal_ = offeredPetal_;
                stagedRarity_ = offeredRarity_;
                phase_ = Phase::Idle;
            }
            refusal_ = outcome.reason.empty() ? "The trader refused." : outcome.reason;
            refusalUntil_ = now + kRefusalSeconds;
        }
    }
    if (resultPending_ && (phase_ != Phase::Trading || (now - phaseStarted_) * 1000.0 >= kTradeSpinMs)) {
        resultPending_ = false;
        phase_ = Phase::Result;
        phaseStarted_ = now;
    }
    if (phase_ == Phase::Trading && now - phaseStarted_ >= kTradeTimeoutSeconds) {
        phase_ = Phase::Idle;
    }

    // Nothing can be staged while the trader is not taking trades, and the
    // slot cannot hold a petal the account no longer has -- but never while a
    // trade is out: that petal is the server's, and a profile landing a frame
    // ahead of the result would empty the slot under it.
    if (phase_ == Phase::Idle &&
        knownPetal(stagedPetal_) && !eligible(stagedPetal_, stagedRarity_, profile.stackCount(stagedPetal_, stagedRarity_))) {
        stagedPetal_ = kNoPetal;
        tradeConfirmReady_.reset();
    }

    const bool trading = phase_ == Phase::Trading;
    const bool coin = phase_ == Phase::Result && knownPetal(resultPetal_);
    motion_.stepRoll(dtMs, trading, coin, kSpinTarget, kSpinRate, kSpinRate);

    const SlotCardLayout card =
        drawSlotCard(canvas, ctx.bounds, kTraderSkin, trEnZh("Trader", "商人"),
                     trEnZh("Trade", "兑换"), mouse);
    const Rect panel = card.panel;

    if (phase_ == Phase::Idle) {
        const Rect mode{panel.x + 14, card.close.y, 120, card.close.h};
        drawSlotButton(canvas, mode, drawMode_ ? trEnZh("Coin exchange", "切换到换币") :
            trEnZh("Random Apex", "切换到随机 Apex"), 0xa57836, mode.contains(mouse), false);
        if (ctx.pressed() && mode.contains(mouse)) {
            drawMode_ = !drawMode_;
            ingredients_.clear();
            stagedPetal_ = kNoPetal;
            tradeConfirmReady_.reset();
            return true;
        }
    }
    if (drawMode_ && phase_ == Phase::Idle) {
        constexpr auto tier = Rarity::Ascendant;
        const auto selected = [&](auto index) {
            return static_cast<unsigned>(std::count(ingredients_.begin(), ingredients_.end(), index));
        };
        const bool valid = ingredients_.size() == kTraderApexDrawCost &&
            std::all_of(ingredients_.begin(), ingredients_.end(), [&](auto index) {
                return selected(index) <= profile.stackCount(index, tier);
            });
        for (std::size_t i = 0; i < ingredients_.size(); ++i) {
            const Rect box{card.slot.x - 52 + (i % 3) * 52, card.slot.y - 52 + (i / 3) * 52, 50, 50};
            ItemTile tile;
            tile.petalIndex = ingredients_[i];
            tile.rarity = tier;
            drawItemTile(canvas, ctx.sprites, box, tile);
            if (ctx.pressed() && box.contains(mouse)) {
                ingredients_.erase(ingredients_.begin() + i);
                tradeConfirmReady_.reset();
                return true;
            }
        }
        drawSlotLine(canvas, card, trEnZh("Pick 5 Apex petals. Tap a selected petal to remove it.",
            "选 5 片 Apex，可混搭；点已选花瓣可撤回。") + std::string(" (") +
            std::to_string(ingredients_.size()) + "/5)");
        if (!refusal_.empty() && now < refusalUntil_) drawSlotRefusal(canvas, card, refusal_);
        drawSlotButton(canvas, card.button, tradeConfirmReady_ ? trEnZh("Confirm", "确认") :
            trEnZh("Draw", "兑换"), valid ? std::optional<std::uint32_t>(rarityColor(tier)) :
            std::nullopt, card.button.contains(mouse), false);
        const auto picked = grid_.render(ctx, card, kTraderSkin, SlotGridTiers::Trader,
            [&](std::uint16_t index, Rarity rarity, std::uint32_t owned) {
                SlotCell cell;
                cell.count = owned - (rarity == tier ? std::min(owned, selected(index)) : 0);
                cell.greyed = rarity != tier || !tradable(index) || index == coinIndex ||
                    content().petal(index).isAdminPetal || !petalCraftsToApex(content(), index) ||
                    cell.count == 0 || ingredients_.size() == kTraderApexDrawCost;
                if (cell.count > 1) cell.badge = "x" + stackCountText(cell.count);
                return cell;
            }, true);
        if (!ctx.pressed()) return true;
        if (card.close.contains(mouse)) return false;
        if (picked) {
            ingredients_.push_back(picked->petalIndex);
            tradeConfirmReady_.reset();
        } else if (valid && card.button.contains(mouse)) {
            if (!tradeConfirmReady_) tradeConfirmReady_ = now + kTradeConfirmDelay;
            else if (now >= *tradeConfirmReady_) {
                std::array<std::uint16_t, kTraderApexDrawCost> items;
                std::copy(ingredients_.begin(), ingredients_.end(), items.begin());
                ctx.net.requestApexDraw(items);
                offeredPetal_ = items.front();
                offeredRarity_ = tier;
                phase_ = Phase::Trading;
                phaseStarted_ = now;
                tradeConfirmReady_.reset();
                refusal_.clear();
            }
        }
        return true;
    }

    // --- the slot ----------------------------------------------------------
    // The petal in the slot -- staged, or out with the trader -- until the
    // coin replaces it. One petal, so no badge.
    const std::uint16_t shown = trading ? offeredPetal_ : stagedPetal_;
    const Rarity shownRarity = trading ? offeredRarity_ : stagedRarity_;
    ItemTile shownTile;
    shownTile.petalIndex = shown;
    shownTile.rarity = shownRarity;
    shownTile.timeSeconds = now;
    // Lit under the cursor only while the slot is at rest.
    shownTile.hovered =
        motion_.spin() < 0.05 && phase_ == Phase::Idle && card.slotRect.contains(mouse);
    ItemTile coinTile;
    coinTile.petalIndex = resultPetal_;
    coinTile.rarity = resultRarity_;
    coinTile.timeSeconds = now;
    if (!drawMode_ && coin && knownPetal(offeredPetal_) && content().petal(offeredPetal_).id == "magic_aztec_coin")
        coinTile.badge = "4";
    motion_.drawSlotScene(canvas, ctx.sprites, card, kTraderSkin, (now - phaseStarted_) * 1000.0,
                          dtMs, !coin && knownPetal(shown) ? &shownTile : nullptr,
                          tileKey(shown, shownRarity), coin ? &coinTile : nullptr,
                          tileKey(resultPetal_, resultRarity_) | 0x80000000u, {});

    if (!refusal_.empty() && now < refusalUntil_) {
        drawSlotRefusal(canvas, card, refusal_);
    } else {
        refusal_.clear();
    }

    // --- trade button ------------------------------------------------------
    // The colour of the tier the coin will come back at, once something can
    // be traded.
    const bool canTrade = phase_ == Phase::Idle && knownPetal(stagedPetal_) && eligible(stagedPetal_, stagedRarity_, profile.stackCount(stagedPetal_, stagedRarity_));
    const bool overButton = card.button.contains(mouse);
    const bool confirming = canTrade && tradeConfirmReady_.has_value();
    const bool canConfirm = !confirming || now >= *tradeConfirmReady_;
    const char* buttonLabel = confirming ? trEnZh("Confirm trade", "确认兑换")
                                         : trEnZh("Trade", "兑换");
    drawSlotButton(canvas, card.button, buttonLabel,
                   canTrade && canConfirm ? std::optional<std::uint32_t>(rarityColor(stagedRarity_))
                            : std::nullopt,
                   overButton, overButton && ctx.window.mouseDown(MouseButton::Left));

    if (waiting && !canTrade) {
        drawSlotLine(canvas, card, traderCooldownText(cooldown), kSlotCardWaitInk);
    } else if (confirming) {
        if (knownPetal(stagedPetal_) && content().petal(stagedPetal_).id == "magic_aztec_coin")
            drawSlotLine(canvas, card, trEnZh("Trade this petal for 4 coins? Tap the slot to cancel.", "兑换四枚同品质硬币？点花瓣槽取消。"));
        else drawSlotLine(canvas, card, trEnZh("Trade 1 petal for 1 coin? Tap the slot to cancel.",
                                        "用 1 个花瓣兑换 1 枚硬币？点花瓣槽取消。"));
    } else {
        drawSlotLine(canvas, card, trEnZh("All tiers share one coin exchange every 24 hours.",
                                         "所有品质每账号每 24 小时合计换币一次"));
    }

    // --- the grid ----------------------------------------------------------
    // On grey, and not clickable, while the trader is not taking trades and
    // for a petal it never takes. The petal and its count still show.
    const std::optional<SlotGrid::Pick> hovered = grid_.render(
        ctx, card, kTraderSkin, SlotGridTiers::Trader,
        [&](std::uint16_t petalIndex, Rarity rarity, std::uint32_t owned) {
            SlotCell cell;
            cell.count = owned;
            // The staged petal is in the slot, not in its stack.
            if (petalIndex == stagedPetal_ && rarity == stagedRarity_ && owned > 0)
                --cell.count;
            cell.greyed = !eligible(petalIndex, rarity, owned);
            // A lone petal carries no badge, as in the inventory.
            if (cell.count > 1) cell.badge = "x" + stackCountText(cell.count);
            return cell;
        }, true);

    // Tiles on their way out go over everything but the words.
    motion_.drawVanishing(canvas, ctx.sprites, dtMs);

    // --- input -------------------------------------------------------------
    // On press, as the forge and the oracle answer.
    const bool rightPressed = panel.contains(mouse) && ctx.window.mousePressed(MouseButton::Right);
    if (phase_ == Phase::Idle && rightPressed) {
        tradeConfirmReady_.reset();
        stagedPetal_ = kNoPetal;
        return true;
    }
    if (!ctx.pressed() && !rightPressed) return true;
    if (card.close.contains(mouse) && ctx.pressed()) return false;

    // The coin sits there until it is collected, and the press that collects
    // it does nothing else.
    if (phase_ == Phase::Result && panel.contains(mouse)) {
        phase_ = Phase::Idle;
        resultPetal_ = kNoPetal;
        return true;
    }

    if (overButton) {
        if (canTrade) {
            if (stagedRarity_ >= Rarity::Super && !confirming) {
                tradeConfirmReady_ = now + kTradeConfirmDelay;
                return true;
            }
            if (!canConfirm) return true;
            tradeConfirmReady_.reset();
            phase_ = Phase::Trading;
            phaseStarted_ = now;
            offeredPetal_ = stagedPetal_;
            offeredRarity_ = stagedRarity_;
            refusal_.clear();
            ctx.net.requestTrade(stagedPetal_, stagedRarity_);
            // Handed over: the slot draws the offer from here, and a refusal
            // is what puts it back.
            stagedPetal_ = kNoPetal;
        }
        return true;
    }

    tradeConfirmReady_.reset();
    if (phase_ == Phase::Idle && card.slotRect.contains(mouse)) {
        stagedPetal_ = kNoPetal;
        return true;
    }

    if (hovered && phase_ == Phase::Idle) {
        // One petal a trade, so a click REPLACES what was staged.
        stagedPetal_ = hovered->petalIndex;
        stagedRarity_ = hovered->rarity;
        refusal_.clear();
    }
    return true;
}

} // namespace flix
