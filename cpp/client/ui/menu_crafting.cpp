// The forge.
//
// Five slots in a ring, a Craft button beside them, and a grid of everything
// the account owns laid out as petal-per-row and tier-per-column -- which is
// the shape that makes "what am I five away from upgrading" readable at a
// glance, and is why the crafting grid is not the inventory grid.
//
// It is florr's craft window (menus.h), and the ring moves the way florr's
// does. Those motions were read out of florr's client, where the body's draw
// integrates them every frame (sub_1008378b0 and 0x1008387a0; the recorded
// frames are in ~/florr_images/menus_skia.json). Every one eases the same way,
// x += (target - x) * (1 - e^(-rate * dt)), with dt in milliseconds:
//
//   - a petal landing in a slot grows in, from nothing to the slot's size,
//     fading in and turning one full turn backwards as it does;
//   - while a craft rolls the ring's spin climbs toward 30 rad/s, and its
//     radius pulses in from 90 toward 40 on a 0.01 rad/ms sine whose depth
//     eases in; when the roll stops the spin runs down and the pulse out;
//   - when the outcome shows, the petals it spent vanish where they stand,
//     turning: all five on a success, which then draws the empty ring in
//     onto its centre and fades it while the upgrade -- half as large again
//     as a slot -- pops in with a pulse and a burst of grains; on a failure
//     a random set of slots, each with a few grains thrown along the turn.
//
// How long a roll lasts is the client's too (its craft-result handler,
// 0x1007e5530): a base time for the tier being crafted, cut the more of the
// next tier the account already holds, nothing at all when it holds the
// petal two tiers up or more, and on a failure stopped short by how many
// petals it cost. The Craft button wears the colour of the tier it crafts
// into whenever it can be pressed, grey otherwise. The Switch button beside
// the close button turns the window over to Absorb (menu_absorb.cpp), as
// florr's old client's did.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "client/ui/craft_bulk_confirmation.h"
#include "client/ui/item_tile.h"
#include "client/ui/menu_theme.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "shared/game/config.h"

namespace flix {

using namespace flix::ui;

namespace {

/// One craft consumes five of a kind. The server enforces the same number; it
/// is here so the panel can refuse a stack that cannot fill the ring.
constexpr int kBatch = 5;

/// The ring: five slots 60 across, 90 out from the slot's centre, the first
/// straight up.
constexpr double kRingRadius = 90.0;
constexpr double kRingSlot = 60.0;
constexpr double kRingStart = -kPi * 0.5;
/// A slot's grow toward 1 once a petal is in it.
constexpr double kGrowRate = 0.021400496636323946;
constexpr double kGrowDone = 0.99999;
/// The upgrade a success leaves in the ring's middle, and its burst: five
/// times florr's grains for its tier, mostly rising, at any angle. A failure
/// throws its tier's count once from each slot it empties.
constexpr double kResultSlot = 90.0;
constexpr int kBurstMultiplier = 5;
constexpr SlotBurstShape kBurst{200.0, 0.0, kTau};
/// The roll. The spin eases toward kRollSpin while rolling and back to rest
/// after, at different rates; the pulse's depth and the outcome's show ease
/// at the same rate as each other.
constexpr double kRollSpin = 30.0;
constexpr double kSpinUpRate = 0.000603020151210087;
constexpr double kSpinDownRate = 0.00632163093946958;
constexpr double kShakeRate = 0.0133886130788526;
constexpr double kShowRate = 0.0133886130788526;
/// The pulse: the radius dips by up to this much, on this sine of the clock.
constexpr double kPulseDepth = 50.0;
constexpr double kPulseRate = 0.01;
/// The roll's length by the tier crafted out of (float table 0x100E16A30):
/// common through legendary, and 5 s from there up -- florr stops at super,
/// and unique, a tier our ladder crafts out of and florr's does not, takes
/// the top figure too.
constexpr std::array<double, 5> kRollBaseMillis = {250.0, 750.0, 1500.0, 3000.0, 5000.0};
/// Holding this many of the next tier already cuts the roll to nothing; each
/// one held takes a tenth off.
constexpr double kRollHeldCut = 10.0;
/// A failure that cost n petals (1 to 4) stops at a random point between
/// (n - 1) / 4 and n / 4 of the way through.
constexpr double kRollLossStep = 0.25;
/// How long the ring waits on an outcome that has not come before it gives
/// up. The craft resolved server-side either way.
constexpr double kCraftTimeoutMillis = 8000.0;
/// Grid hold-to-stage: same cadence as Absorb (menu_absorb.cpp).
constexpr double kHoldDelayMs = 1000.0;
constexpr double kHoldRepeatMs = 300.0;
constexpr double kHoldGrowth = 1.3;
constexpr std::uint32_t kHoldMaxStep = 1000000;
/// Gap between Craft and the Craft All toggle.
constexpr double kCraftButtonGap = 8.0;
constexpr double kCraftButtonPadX = 13.0;
constexpr double kCraftButtonLabelSize = 16.0;
constexpr double kSearchBandHeight = 34.0;
constexpr double kBulkControlsHeight = 82.0 + kSearchBandHeight;
/// Face of the Craft All toggle while it is on (panel accent).
constexpr std::uint32_t kFillAllOnTint = kCraftingSkin.accent;

/// florr's roll length for crafting `petal` out of `from`, before any loss.
double rollBaseMillis(const Profile& profile, std::uint16_t petal, Rarity from) {
    const int r = rarityIndex(from);
    const int top = rarityIndex(kTopLadderRarity);
    // Already holding it two tiers up or more: no suspense left.
    for (int up = r + 2; up <= top; ++up) {
        if (profile.stackCount(petal, static_cast<Rarity>(up)) > 0) return 0.0;
    }
    const double base =
        kRollBaseMillis[static_cast<std::size_t>(std::min<int>(r, kRollBaseMillis.size() - 1))];
    if (r + 2 > top) return base;
    const double held = static_cast<double>(profile.stackCount(petal, static_cast<Rarity>(r + 1)));
    return base * clamp((kRollHeldCut - held) / kRollHeldCut, 0.0, 1.0);
}

/// What a slot shows: petal and tier, or nothing.
constexpr std::uint32_t kNothing = 0xFFFFFFFFu;
std::uint32_t tileKey(std::uint16_t petal, Rarity rarity) {
    return (static_cast<std::uint32_t>(petal) << 8) | static_cast<std::uint32_t>(rarityIndex(rarity));
}

/// Uniform in [0, 1). Not reproducible and not meant to be: florr draws its
/// own from a thread-local generator.
double unitRandom() {
    static std::uint32_t state = 0x2545F491u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<double>(state % 1000000u) / 1000000.0;
}

/// Each clover equipped in the ten PRIMARY loadout slots at the tier being
/// crafted adds this many percentage points. Storage slots do not count.
constexpr double kCloverBonus = 0.05;
constexpr std::size_t kPrimarySlots = 10;

/// The shortest string that round-trips a percentage rounded to two decimals:
/// "64%", "8%", "0.5%", "0.25%". The top tiers are fractions of a percent and
/// rounding those to "0%" would say the craft is impossible, which it is not.
std::string percentText(double percent) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.2f", percent);
    std::string out = buffer;
    if (out.find('.') != std::string::npos) {
        while (!out.empty() && out.back() == '0') out.pop_back();
        if (!out.empty() && out.back() == '.') out.pop_back();
    }
    return out + "%";
}

/// A stack as the staging math counts it. The wire carries a u32 but no stack
/// is deeper than kMaxStackCount, and a count past it cast straight to int
/// would stage a negative number of batches.
int ownedCount(const Profile& profile, std::uint16_t petalIndex, Rarity rarity) {
    return static_cast<int>(std::min<std::uint32_t>(profile.stackCount(petalIndex, rarity),
                                                    static_cast<std::uint32_t>(kMaxStackCount)));
}

/// Clovers raise the roll, so they have to raise the number the panel prints
/// or the two disagree in front of the player.
double cloverBonus(const Profile& profile, Rarity rarity) {
    const std::uint16_t clover = content().petalIndex("clover");
    if (clover == kInvalidIndex) return 0.0;
    double bonus = 0.0;
    const std::size_t slots = std::min(profile.loadout.size(), kPrimarySlots);
    for (std::size_t i = 0; i < slots; ++i) {
        const Profile::Slot& slot = profile.loadout[i];
        if (!slot.empty() && slot.petalIndex == clover && slot.rarity == rarity) {
            bonus += kCloverBonus;
        }
    }
    return bonus;
}


} // namespace

Vec2 CraftingPanel::size(const Profile& profile) {
    constexpr auto tiers = SlotGridTiers::PermanentHyper;
    return {slotCardWidth(tiers), slotCardHeight(SlotGrid::rows(profile, tiers), kBulkControlsHeight)};
}

double CraftingPanel::minimumHeight() {
    return slotCardHeight(1, kBulkControlsHeight);
}

void CraftingPanel::reopen() {
    // Keep the staged stack, scroll position and any server result on the ring.
    bulkArmed_ = false;
    editingBlacklist_ = false;
    mythicUltraMaterials_.reset();
    grid_.endDrag();
    searchField_.blur();
    tooltip_.reset();
    touchInspect_.reset();
    heldPetal_ = kNoPetal;
    heldStep_ = 1;
}

void CraftingPanel::stage(const Profile& profile, std::uint16_t petalIndex, Rarity rarity,
                          std::uint32_t addBatches) {
    const int owned = ownedCount(profile, petalIndex, rarity);
    if (owned < kBatch || !craftsOutOf(rarity) ||
        (rarity == Rarity::PermanentHyper && !petalCraftsToApex(content(), petalIndex))) return;

    if (stagedPetal_ != petalIndex || stagedRarity_ != rarity) {
        stagedPetal_ = petalIndex;
        stagedRarity_ = rarity;
        stagedCount_ = 0;
    }
    if (addBatches == UINT32_MAX) {
        stagedCount_ = owned;
        return;
    }
    const auto availableBatches = static_cast<std::uint32_t>(
        std::max(0, owned - stagedCount_) / kBatch);
    stagedCount_ += static_cast<int>(std::min(addBatches, availableBatches)) * kBatch;
}

void CraftingPanel::animate(double dtMs) {
    const auto ease = [dtMs](double& value, double target, double rate) {
        value += (target - value) * (1.0 - std::exp(-rate * dtMs));
    };
    const bool rolling = phase_ == Phase::Spinning;
    ease(spin_, rolling ? kRollSpin : 0.0, rolling ? kSpinUpRate : kSpinDownRate);
    phaseAngle_ = std::fmod(phaseAngle_ + spin_ * dtMs / 1000.0, kTau);
    ease(shake_, rolling ? 1.0 : 0.0, kShakeRate);
    ease(show_, phase_ == Phase::Result && lastSuccess_ ? 1.0 : 0.0, kShowRate);
}

void CraftingPanel::startRoll(NetClient& net, double timeSeconds) {
    const Profile& profile = net.profile();
    heldPetal_ = kNoPetal;
    phase_ = Phase::Spinning;
    phaseStarted_ = timeSeconds;
    spinPetal_ = stagedPetal_;
    spinRarity_ = stagedRarity_;
    attemptsBeforeRoll_ = net.craftAttemptCount();
    visibleRevision_.reset();
    preCraftStacks_.clear();
    const Rarity upgrade = upgradeRarity(spinRarity_);
    for (std::size_t i = 0; i < profile.inventory.size(); ++i) {
        const auto& stack = profile.inventory[i];
        if (stack.petalIndex == spinPetal_ &&
            (stack.rarity == spinRarity_ || stack.rarity == upgrade)) {
            preCraftStacks_.emplace_back(i, stack);
        }
    }
    rollBaseMillis_ = rollBaseMillis(profile, stagedPetal_, stagedRarity_);
    rollMillis_ = rollBaseMillis_;
    net.requestCraft(stagedPetal_, stagedRarity_, stagedCount_);
    stagedPetal_ = kNoPetal;
    stagedCount_ = 0;
}

void CraftingPanel::update(NetClient& net, double timeSeconds) {
    const Profile& profile = net.profile();
    if (!net.haveSession()) {
        if (!account_.empty()) *this = CraftingPanel{};
        return;
    }
    if (account_ != profile.username) {
        *this = CraftingPanel{};
        account_ = profile.username;
    }
    CraftOutcome& outcome = net.craftOutcome();
    if (outcome.pending) {
        outcome.pending = false;
        lastSuccess_ = outcome.success;
        resultPetal_ = outcome.petalIndex;
        resultRarity_ = outcome.rarity;
        resultCount_ = outcome.crafted;
        // What the server could not spend: the pool is crafted five at a time
        // until fewer than five are left, and that tail is what the remaining
        // slots show. It is a count off the wire, not a constant -- a failure
        // hands back one to four, not always three.
        survivors_ = outcome.attempts == 0 ? kBatch : outcome.petalsReturned;
        // A failure stops the roll short, by a random share of its length
        // that grows with what it cost.
        rollMillis_ = rollBaseMillis_;
        if (outcome.attempts == 0) {
            rollMillis_ = 0.0;
        } else if (!lastSuccess_) {
            const int lost = std::clamp(kBatch - survivors_, 1, 4);
            const double from = kRollLossStep * (lost - 1);
            rollMillis_ *= from + unitRandom() * kRollLossStep;
        }
        // Mid-roll the outcome is only recorded; the ring owns when it is
        // shown. Anywhere else -- a result that arrived while the panel was
        // shut, or after the roll already ran out -- it lands now.
        if (phase_ == Phase::Spinning) {
            resultPending_ = true;
        } else {
            spinPetal_ = kNoPetal;
            phase_ = Phase::Result;
            phaseStarted_ = timeSeconds;
        }
    }
    if (phase_ == Phase::Spinning) {
        const double elapsed = (timeSeconds - phaseStarted_) * 1000.0;
        if (elapsed >= rollMillis_ && resultPending_) {
            resultPending_ = false;
            spinPetal_ = kNoPetal;
            phase_ = Phase::Result;
            phaseStarted_ = timeSeconds;
        } else if (elapsed >= kCraftTimeoutMillis) {
            // Release the display hold, but wait for the outstanding reply
            // before sending another roll on this session.
            phase_ = Phase::Idle;
        }
    }
}

const Profile& CraftingPanel::visibleInventory(const Profile& profile) {
    if (phase_ != Phase::Spinning) return profile;
    if (visibleRevision_ != profile.revision) {
        auto& stacks = displayBag_.inventory;
        stacks.clear();
        const Rarity upgrade = upgradeRarity(spinRarity_);
        for (const auto& stack : profile.inventory) {
            if (stack.petalIndex == spinPetal_ &&
                (stack.rarity == spinRarity_ || stack.rarity == upgrade)) continue;
            stacks.push_back(stack);
        }
        for (const auto& [index, stack] : preCraftStacks_) {
            stacks.insert(stacks.begin() + std::min(index, stacks.size()), stack);
        }
        visibleRevision_ = profile.revision;
    }
    return displayBag_;
}

bool CraftingPanel::render(MenuContext& ctx) {
    Canvas& canvas = ctx.canvas;
    const Profile& profile = ctx.net.profile();
    const Vec2 mouse = ctx.mouse();
    const double nowMs = ctx.timeSeconds * 1000.0;
    const bool inRift = ctx.net.inRiftMap();
    const CraftOutcome& outcome = ctx.net.craftOutcome();
    const bool waiting = spinPetal_ != kNoPetal;
    if (inRift) {
        bulkArmed_ = false;
        editingBlacklist_ = false;
        mythicUltraMaterials_.reset();
        fillAllMode_ = false;
        heldPetal_ = kNoPetal;
    }

    const auto removeBatch = [this]() {
        stagedCount_ -= kBatch;
        if (stagedCount_ < kBatch) {
            stagedCount_ = 0;
            stagedPetal_ = kNoPetal;
        }
    };

    // The staged batch cannot outlive the petals behind it: another window, a
    // pickup, or the craft that just consumed them all change the stack. Never
    // during a roll, though -- those five are already at the server, and a
    // profile that lands a frame ahead of the result would empty the ring
    // half-way through its own animation.
    if (phase_ != Phase::Spinning && stagedPetal_ != kNoPetal) {
        const int owned = ownedCount(profile, stagedPetal_, stagedRarity_);
        if (owned < stagedCount_) {
            stagedCount_ = stagedCount_ % kBatch == 0 ? owned / kBatch * kBatch : owned;
            if (stagedCount_ < kBatch) {
                stagedCount_ = 0;
                stagedPetal_ = kNoPetal;
            }
        }
    }

    animate(ctx.dt * 1000.0);

    const SlotCardLayout card =
        drawSlotCard(canvas, ctx.bounds, kCraftingSkin, menuTitle(MenuId::Crafting),
                     tr(Msg::Craft), mouse);
    const Rect panel = card.panel;
    if (mythicUltraMaterials_) {
        const auto ticket = ctx.net.mythicUltraTicket(mythicUltraRequest_);
        const auto state = mythicUltraRequest_ == 0 || (ticket && *ticket == 0)
            ? MythicUltraCraftState::Refresh
            : (ticket ? MythicUltraCraftState::Ready : MythicUltraCraftState::Preparing);
        const auto buttons = drawMythicUltraConfirmation(ctx, card, *mythicUltraMaterials_,
                                                         mythicUltraRefreshed_, state);
        if (ctx.pressed()) {
            if (card.close.contains(mouse) || buttons.cancel.contains(mouse)) {
                mythicUltraMaterials_.reset();
            } else if (state != MythicUltraCraftState::Preparing && buttons.confirm.contains(mouse)) {
                // Pickups leave the approved counts intact. A shortage needs a fresh
                // preview and another click before the server can spend anything.
                const bool shortOnMaterials = std::any_of(
                    mythicUltraMaterials_->begin(), mythicUltraMaterials_->end(), [&](const auto& stack) {
                        return profile.stackCount(stack.petalIndex, stack.rarity) < stack.count;
                    });
                if (state == MythicUltraCraftState::Refresh || shortOnMaterials) {
                    *mythicUltraMaterials_ = mythicUltraCraftMaterials(profile, ctx.settings.craftBlacklist);
                    mythicUltraRefreshed_ = true;
                    mythicUltraRequest_ = ctx.net.prepareMythicUltraCraft(*mythicUltraMaterials_);
                } else {
                    ctx.net.confirmMythicUltraCraft(*ticket);
                    mythicUltraMaterials_.reset();
                }
            }
        }
        return true;
    }
    if (drawPanelExpand(ctx, card.close.y + card.close.h * 0.5, kCraftingSkin.fill, expanded_)) {
        expanded_ = !expanded_;
        searchField_.blur();
        grid_.endDrag();
        heldPetal_ = kNoPetal;
        return true;
    }
    // florr's old client turns this window over to Absorb with a button in
    // Absorb's purple beside the close button.
    const Rect switchRect = drawSlotSwitch(canvas, card, kAbsorbSkin.fill, mouse,
                                           ctx.window.mouseDown(MouseButton::Left));

    // --- the ring ----------------------------------------------------------
    // Through the roll AND through a FAILED result the ring keeps showing the
    // petal as it was before the craft. A success replaces the ring with the
    // upgrade: the five went in and one came out.
    std::uint16_t ringPetal = stagedPetal_;
    Rarity ringRarity = stagedRarity_;
    if (phase_ == Phase::Spinning) {
        // The staging area went to the server whole on the click, so the roll
        // draws from its own copy of what was sent.
        ringPetal = spinPetal_;
        ringRarity = spinRarity_;
    } else if (phase_ == Phase::Result) {
        // The server answers a success with the upgraded tier, so the
        // original is one step back down.
        ringPetal = resultPetal_;
        ringRarity = lastSuccess_ ? downgradeRarity(resultRarity_) : resultRarity_;
    }
    const bool ringFilled = knownPetal(ringPetal);

    // The reveal frame: a success empties every slot -- the five went in --
    // and a failure a random set of them, as many as it cost. Each emptied
    // slot's petal vanishes where it stood, a failure's with a few grains
    // in its colour thrown along the ring's turn, and a success throws its
    // burst from the middle as the upgrade pops in.
    const bool inResult = phase_ == Phase::Result;
    const bool revealFrame = inResult && !revealed_;
    if (revealFrame) {
        revealed_ = true;
        survives_.fill(false);
        if (!lastSuccess_) {
            std::array<int, kBatch> order{0, 1, 2, 3, 4};
            for (int i = kBatch - 1; i > 0; --i) {
                std::swap(order[static_cast<std::size_t>(i)],
                          order[static_cast<std::size_t>(unitRandom() * (i + 1)) % (i + 1)]);
            }
            for (int k = 0; k < std::clamp(survivors_, 0, kBatch); ++k) {
                survives_[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])] = true;
            }
        }
    }
    if (!inResult) revealed_ = false;

    // The ring draws in onto its centre and fades away as the outcome shows;
    // while rolling its radius pulses in on the clock by however deep the
    // shake has eased in.
    const double keep = 1.0 - show_;
    const double pulse = (std::sin(nowMs * kPulseRate) + 1.0) * 0.5;
    const double radius = (kRingRadius - kPulseDepth * shake_ * pulse) * keep;
    const double side = kRingSlot;

    std::array<Rect, kBatch> slots{};
    std::array<Vec2, kBatch> centres{};
    std::array<double, kBatch> angles{};
    canvas.setGlobalAlpha(static_cast<float>(clamp(keep, 0.0, 1.0)));
    for (int i = 0; i < kBatch; ++i) {
        const std::size_t at = static_cast<std::size_t>(i);
        angles[at] = kRingStart + kTau * i / kBatch + phaseAngle_;
        centres[at] = card.slot + Vec2::fromAngle(angles[at], radius);
        slots[at] = {centres[at].x - side * 0.5, centres[at].y - side * 0.5, side, side};
        if (keep > 0.001) drawSlotPlate(canvas, ctx.sprites, slots[at], kCraftingSkin);
    }
    canvas.setGlobalAlpha(1.0f);
    // The grains go under every petal.
    fx_.drawGrains(canvas, ctx.dt * 1000.0);

    for (int i = 0; i < kBatch; ++i) {
        const std::size_t at = static_cast<std::size_t>(i);
        ItemTile tile;
        tile.petalIndex = ringPetal;
        tile.rarity = ringRarity;
        tile.timeSeconds = ctx.timeSeconds;
        const bool occupied = ringFilled && (!inResult || (!lastSuccess_ && survives_[at]));
        double& grow = grow_[at];
        const std::uint32_t key = occupied ? tileKey(ringPetal, ringRarity) : kNothing;
        if (key != slotKey_[at]) {
            // What stood here vanishes where it stood, and grows in again only
            // as something new; a failure's lost petals also throw a few
            // grains in their colour along the ring's turn.
            if (slotKey_[at] != kNothing) {
                fx_.vanish(centres[at], side, slotTile_[at]);
                if (revealFrame && !lastSuccess_) {
                    // Along the ring's turn: radius times spin, tangent to it.
                    const Vec2 carry = Vec2::fromAngle(angles[at] + kPi * 0.5, radius * spin_);
                    fx_.burst(centres[at], slotTile_[at].rarity,
                              slotBurstGrains(slotTile_[at].rarity), kBurst, carry);
                }
            }
            slotKey_[at] = key;
            grow = 0.0;
        }
        if (!occupied) continue;
        grow += (1.0 - grow) * (1.0 - std::exp(-kGrowRate * ctx.dt * 1000.0));
        const bool growing = grow < kGrowDone;
        if (phase_ == Phase::Idle && stagedCount_ >= 2 * kBatch) {
            tile.badge = "x" + stackCountText(static_cast<std::uint64_t>(stagedCount_ / kBatch));
        }
        slotTile_[at] = tile;
        // drawItemTile SETS the canvas's alpha rather than multiplying it, so
        // the ring's fade is folded in here.
        tile.alpha = clamp(keep, 0.0, 1.0) * (growing ? clamp(grow, 0.0, 1.0) : 1.0);
        drawSlotTile(canvas, ctx.sprites, centres[at], side * (growing ? grow : 1.0),
                     growing ? grow * -kTau : 0.0, tile);
    }

    // The upgrade, popped into the ring's middle once a success shows.
    const bool upgraded = inResult && lastSuccess_ && knownPetal(resultPetal_);
    ItemTile upgradeTile;
    upgradeTile.petalIndex = resultPetal_;
    upgradeTile.rarity = resultRarity_;
    // How many upgrades the pool actually produced -- a staged x3 that landed
    // twice reads "x2", which is the only place the player is told.
    if (resultCount_ > 1) {
        upgradeTile.badge = "x" + stackCountText(static_cast<std::uint64_t>(resultCount_));
    }
    upgradeTile.timeSeconds = ctx.timeSeconds;
    const std::uint32_t upgradeKey = upgraded ? tileKey(resultPetal_, resultRarity_) : kNothing;
    if (upgradeKey != resultKey_) {
        if (resultKey_ != kNothing) fx_.vanish(card.slot, kResultSlot, resultTile_);
        if (upgraded) {
            fx_.startPop();
            fx_.burst(card.slot, resultRarity_, kBurstMultiplier * slotBurstGrains(resultRarity_),
                      kBurst);
        }
        resultKey_ = upgradeKey;
    }
    if (upgraded) {
        resultTile_ = upgradeTile;
        fx_.drawPopped(canvas, ctx.sprites, card.slot, kResultSlot, ctx.dt * 1000.0, upgradeTile);
    }

    // --- odds and the lines (buttons drawn after the grid, for hover tint) ---
    // A valid craft always has a chance above zero, even if it is a quarter of
    // a percent; a zero means nothing is staged, which reads as "?%".
    std::string odds = language() == Language::Zh ? "成功率：?%" : "?% success chance";
    if (stagedPetal_ != kNoPetal) {
        const double percent = std::min(100.0, craftSuccessChance(stagedRarity_) * 100.0 +
                                                   (stagedRarity_ == Rarity::PermanentHyper ? 0.0 : cloverBonus(profile, stagedRarity_)));
        odds = language() == Language::Zh ? "成功率：" + percentText(percent) : percentText(percent) + " success chance";
    }
    drawSlotChance(canvas, card, odds);
    auto attemptCard = card;
    attemptCard.chanceY += 20.0;
    drawSlotChance(canvas, attemptCard, std::string(language() == Language::Zh ?
        "合成尝试次数：" : "Craft attempts: ") +
        std::to_string(waiting ? attemptsBeforeRoll_ : ctx.net.craftAttemptCount()));

    if (waiting) {
        drawSlotLine(canvas, card, phase_ == Phase::Spinning
            ? trEnZh("Crafting…", "合成中…")
            : trEnZh("Waiting for the craft result…", "等待服务器返回合成结果…"));
    } else if (stagedPetal_ != kNoPetal) {
        drawSlotLine(canvas, card, std::string(trEnZh("Staged x", "已投入 ×")) +
            std::to_string(stagedCount_));
    } else if (outcome.attempts > 0) {
        const bool chinese = language() == Language::Zh;
        drawSlotLine(canvas, card, std::string(chinese ? "本次合成 ×" : "Crafted x") +
            std::to_string(outcome.crafted) + (chinese ? "，消耗 ×" : ", consumed x") +
            std::to_string(outcome.consumed));
    } else if (phase_ == Phase::Result && !outcome.reason.empty()) {
        drawSlotLine(canvas, card, outcome.reason);
    } else {
        drawSlotLine(canvas, card, trEnZh("Select 5 matching petals to craft",
                                           "选择 5 个相同花瓣进行合成"));
    }
    // The warning only once there is something to lose.
    if (stagedPetal_ != kNoPetal) {
        drawSlotLine(canvas, card, language() == Language::Zh ? "失败会消耗1至4个花瓣" : "Failure will destroy 1-4 petals", std::nullopt, true);
    } else {
        drawSlotLine(canvas, card, trEnZh("Apex: select Permanent Hyper in the last column",
                                           "合成 Apex：选择最右列的永久超凡"),
                     rarityColor(Rarity::Ascendant), true);
    }

    // --- the grid ----------------------------------------------------------
    const auto held = static_cast<std::uint32_t>(stagedCount_);
    SlotCardLayout bulkCard = card;
    bulkCard.listView.y += kBulkControlsHeight;
    bulkCard.listView.h = std::max(0.0, bulkCard.listView.h - kBulkControlsHeight);
    const Rect bulkPrev{panel.x+12,card.listView.y+4,32,30};
    const Rect bulkTarget{bulkPrev.right()+5,bulkPrev.y,170,30};
    const Rect bulkNext{bulkTarget.right()+5,bulkPrev.y,32,30};
    const Rect bulkRun{bulkNext.right()+10,bulkPrev.y,std::max(100.0,panel.right()-12-bulkNext.right()-10),30};
    static const char* bulkZh[]={"普通","罕见","稀有","史诗","传说","神话","究极","超级","永恒","Hyper"};
    const bool bulkChinese=language()==Language::Zh;
    drawSlotButton(canvas,bulkPrev,"<",std::nullopt,!inRift && bulkPrev.contains(mouse),false);
    const std::string bulkTargetLabel=std::string(bulkChinese?"合成至：":"Craft to: ")+(bulkChinese?bulkZh[bulkTarget_]:rarityLabel(static_cast<Rarity>(bulkTarget_)));
    drawSlotButton(canvas,bulkTarget,bulkTargetLabel.c_str(),rarityColor(static_cast<Rarity>(bulkTarget_)),false,false);
    drawSlotButton(canvas,bulkNext,">",std::nullopt,!inRift && bulkNext.contains(mouse),false);
    const char* bulkRunLabel = inRift ? (bulkChinese ? "裂隙内无法合成" : "Crafting unavailable in Rift")
        : bulkArmed_ ? (bulkChinese ? "确认消耗低阶材料" : "Confirm consuming lower tiers")
                     : (bulkChinese ? "一键合成全部低阶花瓣" : "Craft all lower tiers");
    const auto bulkRunTint = inRift ? std::nullopt
        : std::optional<std::uint32_t>{bulkArmed_ ? 0xCC6644u : 0x5A9FDBu};
    drawSlotButton(canvas, bulkRun, bulkRunLabel, bulkRunTint, !inRift && bulkRun.contains(mouse), false);
    const Rect mythicUltraEntry = mythicUltraCraftEntry(panel, card.listView);
    const Rect blacklistButton{bulkPrev.x, bulkPrev.y + 36,
                               panel.w - 24 - (editingBlacklist_ ? 100.0 : mythicUltraEntry.w + 6.0), 30};
    const Rect clearBlacklist{blacklistButton.right() + 5, blacklistButton.y, 95, 30};
    const std::string blacklistLabel = (bulkChinese ? "合成黑名单（" : "Excluded types (") +
        std::to_string(ctx.settings.craftBlacklist.size()) + (bulkChinese ? "）：" : "): ") +
        (editingBlacklist_ ? (bulkChinese ? "点击花瓣切换，点此完成 · 自动保存" : "Click petals to toggle · Done")
                           : (bulkChinese ? "点击设置" : "Edit"));
    const auto blacklistTint = inRift ? std::nullopt
        : std::optional<std::uint32_t>{editingBlacklist_ ? 0xCC6644u : 0x5A9FDBu};
    drawSlotButton(canvas, blacklistButton, blacklistLabel.c_str(), blacklistTint,
                   !inRift && blacklistButton.contains(mouse), false);
    if (editingBlacklist_) drawSlotButton(canvas, clearBlacklist, bulkChinese ? "清空名单" : "Clear list",
                                          0xCC6644u, clearBlacklist.contains(mouse), false);
    const bool canOpenMythicUltra = !inRift && !waiting && !editingBlacklist_ && ctx.net.haveSession();
    if (!editingBlacklist_) {
        const auto tint = canOpenMythicUltra ? std::optional<std::uint32_t>{0x5A9FDBu} : std::nullopt;
        drawSlotButton(canvas, mythicUltraEntry, trEnZh("Craft Mythic & Ultra", "仅合成神话、究极"),
                       tint, canOpenMythicUltra && mythicUltraEntry.contains(mouse), false);
    }
    const auto isExcluded = [&](std::uint16_t index) {
        const auto& ids = ctx.settings.craftBlacklist;
        return std::find(ids.begin(), ids.end(), content().petal(index).id) != ids.end();
    };
    const Rect searchRect{panel.x + 12.0, bulkCard.listView.y - kSearchBandHeight,
                          panel.w - 162.0, 28.0};
    const Rect craftableRect{searchRect.right() + 6.0, searchRect.y, 132.0, searchRect.h};
    const bool canFilter = !inRift && !editingBlacklist_;
    if (editSlotSearch(ctx, searchRect, search_, searchField_,
                       trEnZh("Petal name or rarity", "查找花瓣名称或品质"))) grid_.reset();
    const bool craftableHover = canFilter && craftableRect.contains(mouse);
    const bool craftablePressed = craftableHover && (ctx.window.pointerIsTouch()
        ? ctx.released() && !ctx.window.touchScrolling() && !touchInspect_.downBefore
        : ctx.pressed());
    if (craftablePressed) {
        onlyCraftable_ = !onlyCraftable_;
        grid_.reset();
        searchField_.blur();
    }
    CraftSearchQuery query = parseCraftSearch(search_);
    if (canFilter && onlyCraftable_) query.minimumCraftMaterials = kBatch;
    const auto craftableTint = query.minimumCraftMaterials > 0
        ? std::optional<std::uint32_t>{kFillAllOnTint} : std::nullopt;
    drawSlotButton(canvas, craftableRect, trEnZh("Craftable only", "仅显示可合成"),
                   craftableTint, craftableHover, craftablePressed);
    SlotGrid::Pick inspected;
    const std::optional<SlotGrid::Pick> hovered = grid_.render(
        ctx, bulkCard, kCraftingSkin, SlotGridTiers::PermanentHyper,
        [&](std::uint16_t petalIndex, Rarity rarity, std::uint32_t owned) {
            SlotCell cell;
            cell.count = owned;
            cell.greyed = !craftsOutOf(rarity) ||
                (rarity == Rarity::PermanentHyper && !petalCraftsToApex(content(), petalIndex));
            // Staged petals are gone from the player's point of view the
            // moment they land in the ring, so the badge counts down with each
            // click.
            if (petalIndex == stagedPetal_ && rarity == stagedRarity_) {
                cell.count = owned > held ? owned - held : 0;
            }
            if (rarity == Rarity::PermanentHyper && cell.count < kBatch) cell.greyed = true;
            // A lone petal carries no badge; "x1" is noise on every cell of a
            // fresh account.
            if (cell.count > 1) cell.badge = "x" + stackCountText(cell.count);
            if (isExcluded(petalIndex)) cell.badge = bulkChinese ? "排除" : "Skip";
            return cell;
        }, true, &inspected, {}, &query, expanded_);

    // Craft + Craft All toggle side by side, centred on the card's button
    // anchor. Craft All is a mode, not a one-shot: while on, a click on a
    // craftable petal stages the entire stack (Shift-click's behaviour).
    const bool canSkipRoll = !inRift && phase_ == Phase::Spinning && resultPending_;
    const char* craftLabel = canSkipRoll ? trEnZh("Skip animation", "跳过动画") : tr(Msg::Craft);
    const char* craftAllLabel = tr(Msg::CraftAll);
    const double craftW = measure(craftLabel, kCraftButtonLabelSize) + kCraftButtonPadX * 2.0;
    const double craftAllW = measure(craftAllLabel, kCraftButtonLabelSize) + kCraftButtonPadX * 2.0;
    const double craftPairW = craftW + kCraftButtonGap + craftAllW;
    const double craftPairX = card.button.x + card.button.w * 0.5 - craftPairW * 0.5;
    const Rect craftRect{craftPairX, card.button.y, craftW, card.button.h};
    const Rect craftAllRect{craftPairX + craftW + kCraftButtonGap, card.button.y, craftAllW,
                            card.button.h};
    const bool mouseDown = ctx.window.mouseDown(MouseButton::Left);
    const bool canCraft = !inRift && !waiting && phase_ == Phase::Idle &&
                          stagedPetal_ != kNoPetal && stagedCount_ >= kBatch;
    const bool craftActionEnabled = canCraft || canSkipRoll;
    const auto craftTint =
        canCraft ? std::optional<std::uint32_t>(rarityColor(upgradeRarity(stagedRarity_)))
        : canSkipRoll ? std::optional<std::uint32_t>(kFillAllOnTint) : std::nullopt;
    const auto craftAllTint =
        fillAllMode_ ? std::optional<std::uint32_t>(kFillAllOnTint) : std::nullopt;
    drawSlotButton(canvas, craftRect, craftLabel, craftTint,
                   craftActionEnabled && craftRect.contains(mouse),
                   craftActionEnabled && craftRect.contains(mouse) && mouseDown);
    // Toggle stays lit while on; hover/press still flash over the face.
    drawSlotButton(canvas, craftAllRect, craftAllLabel, craftAllTint,
                   !inRift && (craftAllRect.contains(mouse) || fillAllMode_),
                   !inRift && ((craftAllRect.contains(mouse) && mouseDown) || fillAllMode_));

    // Tiles on their way out go over everything.
    fx_.drawVanishing(canvas, ctx.sprites, ctx.dt * 1000.0);

    const int inspectedId = inspected.petalIndex == kNoPetal ? -1
        : inspected.petalIndex * kRarityCount + rarityIndex(inspected.rarity);
    const bool movedFromMaterial = touchInspect_.downBefore && touchInspect_.blocked;
    const bool touchTooltip = touchInspect_.touch(ctx.window, ctx.timeSeconds, [&](Vec2 point) {
        return inspected.bounds.contains(point) ? inspectedId : -1;
    }, ctx.window.touchPan().active);
    const bool mouseTooltip = tooltip_.update(ctx.window.pointerIsTouch() ? -1 : inspectedId,
                                               ctx.timeSeconds, mouseDown);
    if (touchTooltip || mouseTooltip) {
        const double reloadScale = profile.skills.reloadScale() *
            (1.0 - barReloadCut(content(), [&](int i) { return profile.equipped(content(), i); }, &profile.skills));
        auto lines = petalTooltipLines(inspected.petalIndex, inspected.rarity, profile.skills, reloadScale);
        if (inspected.rarity == Rarity::PermanentHyper && !petalCraftsToApex(content(), inspected.petalIndex)) {
            TooltipLine note{tr(Msg::ApexOwnSource), 12.0, kPaper, 6.0};
            note.alpha = 0.78;
            note.maxWidth = 230.0;
            lines.push_back(note);
        }
        const Vec2 size = measureTooltip(lines, 0.0, 0.0, ctx.altMode);
        const Vec2 anchor = tooltipAnchor(inspected.bounds, size, canvas.width(), canvas.height());
        paintTooltip(canvas, anchor.x, anchor.y, lines, 0.0, 0.0, ctx.altMode);
    }

    // --- input -------------------------------------------------------------
    if (craftablePressed) {
        heldPetal_ = kNoPetal;
        return true;
    }
    // Hold-repeat staging: same cadence as Absorb, in batches of five.
    if (heldPetal_ != kNoPetal) {
        const bool still = mouseDown && phase_ == Phase::Idle && hovered &&
                           hovered->petalIndex == heldPetal_ && hovered->rarity == heldRarity_;
        if (!still) {
            heldPetal_ = kNoPetal;
        } else if (nowMs >= heldNextMs_) {
            stage(profile, heldPetal_, heldRarity_, heldStep_);
            const double next = std::floor(static_cast<double>(heldStep_) * kHoldGrowth + 1.0);
            heldStep_ = static_cast<std::uint32_t>(
                std::clamp(next, 1.0, static_cast<double>(kHoldMaxStep)));
            heldNextMs_ = nowMs + kHoldRepeatMs;
        }
    }

    // On press, not release: the browser hit-tests in mousedown, so a press
    // that starts on a cell and drifts off must not still fire.
    const bool rightPressed = panel.contains(mouse) && ctx.window.mousePressed(MouseButton::Right);
    if (!inRift && phase_ == Phase::Idle && rightPressed) {
        removeBatch();
        heldPetal_ = kNoPetal;
        return true;
    }
    // A tap still stages one batch. Wait for release on touch so inspection
    // and a scroll starting on a material cell cannot stage a batch.
    if (ctx.window.pointerIsTouch() && (touchInspect_.downBefore || touchInspect_.consumedRelease || movedFromMaterial)) return true;
    const bool materialTap = ctx.window.pointerIsTouch() && hovered && ctx.released() &&
                             !ctx.window.touchScrolling();
    if (!ctx.pressed() && !rightPressed && !materialTap) return true;
    if (card.close.contains(mouse) && ctx.pressed()) return false;
    if (switchRect.contains(mouse) && ctx.pressed()) {
        switchPressed_ = true;
        return true;
    }
    // Rift materials remain inspectable, but no crafting input can change the panel.
    if (inRift) return true;
    if (ctx.pressed() && canOpenMythicUltra && mythicUltraEntry.contains(mouse)) {
        mythicUltraMaterials_ = mythicUltraCraftMaterials(profile, ctx.settings.craftBlacklist);
        mythicUltraRequest_ = ctx.net.prepareMythicUltraCraft(*mythicUltraMaterials_);
        mythicUltraRefreshed_ = false;
        bulkArmed_ = false;
        heldPetal_ = kNoPetal;
        grid_.endDrag();
        searchField_.blur();
        return true;
    }

    const auto stageAmount = [&]() -> std::uint32_t {
        return (fillAllMode_ || ctx.window.shiftHeld()) ? UINT32_MAX : 1u;
    };
    const auto stageFromGrid = [&](std::uint16_t petal, Rarity rarity) {
        const std::uint32_t add = stageAmount();
        stage(profile, petal, rarity, add);
        // Fill-all lands everything on the first press; hold-repeat is only
        // for the one-batch mode.
        if (add == UINT32_MAX) {
            heldPetal_ = kNoPetal;
            return;
        }
        heldPetal_ = petal;
        heldRarity_ = rarity;
        heldNextMs_ = nowMs + kHoldDelayMs;
        heldStep_ = 1;
    };

    // An outcome stays until the player does something else, florr's way:
    // the upgrade until it is clicked, the survivors of a failure until a
    // slot is. Picking a petal from the grid goes straight on -- the same
    // petal refills only the slots the failure emptied.
    if (phase_ == Phase::Result) {
        const Rect upgradeRect{card.slot.x - kResultSlot * 0.5, card.slot.y - kResultSlot * 0.5,
                               kResultSlot, kResultSlot};
        bool onSlot = upgraded && upgradeRect.contains(mouse);
        for (const Rect& slot : slots) onSlot = onSlot || slot.contains(mouse);
        if (onSlot) {
            phase_ = Phase::Idle;
            return true;
        }
        if (hovered) {
            phase_ = Phase::Idle;
            stageFromGrid(hovered->petalIndex, hovered->rarity);
        }
        return true;
    }

    if (phase_ == Phase::Idle && blacklistButton.contains(mouse)) {
        editingBlacklist_ = !editingBlacklist_; bulkArmed_ = false; heldPetal_ = kNoPetal;
        return true;
    }
    if (editingBlacklist_) {
        if (phase_ == Phase::Idle && clearBlacklist.contains(mouse)) {
            ctx.settings.craftBlacklist.clear();
            ctx.settings.craftBlacklistDirty = true;
            return true;
        }
        if (phase_ == Phase::Idle && hovered) {
            auto& ids = ctx.settings.craftBlacklist;
            const auto& id = content().petal(hovered->petalIndex).id;
            const auto found = std::find(ids.begin(), ids.end(), id);
            if (found == ids.end()) ids.push_back(id); else ids.erase(found);
            ctx.settings.craftBlacklistDirty = true;
            bulkArmed_ = false; heldPetal_ = kNoPetal;
        }
        return true;
    }
    if(phase_==Phase::Idle && (bulkPrev.contains(mouse)||bulkNext.contains(mouse))) {
        const int last = rarityIndex(Rarity::Super);
        bulkTarget_ = 1 + (bulkTarget_ - 1 + (bulkNext.contains(mouse) ? 1 : last - 1)) % last;
        bulkArmed_ = false;
        return true;
    }
    if (phase_ == Phase::Idle && bulkRun.contains(mouse)) {
        if (waiting) return true;
        if(!bulkArmed_){bulkArmed_=true;return true;}
        std::vector<std::uint16_t> excluded;
        for (std::uint16_t i = 0; i < content().petalCount(); ++i) if (isExcluded(i)) excluded.push_back(i);
        ctx.net.requestBulkCraft(static_cast<Rarity>(bulkTarget_), excluded);bulkArmed_=false;stagedPetal_=kNoPetal;stagedCount_=0;return true;
    }
    if (craftRect.contains(mouse)) {
        if (canSkipRoll) {
            // Reuse the regular reveal so inventory, attempts and result land together.
            rollMillis_ = 0.0;
            update(ctx.net, ctx.timeSeconds);
        } else if (canCraft) {
            startRoll(ctx.net, ctx.timeSeconds);
        }
        return true;
    }
    if (craftAllRect.contains(mouse)) {
        fillAllMode_ = !fillAllMode_;
        return true;
    }

    if (phase_ == Phase::Idle) {
        for (const Rect& slot : slots) {
            if (!slot.contains(mouse)) continue;
            removeBatch();
            heldPetal_ = kNoPetal;
            return true;
        }
    }

    if (hovered && phase_ == Phase::Idle) {
        stageFromGrid(hovered->petalIndex, hovered->rarity);
    }
    return true;
}

} // namespace flix
