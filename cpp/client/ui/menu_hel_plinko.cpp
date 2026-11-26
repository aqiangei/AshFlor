#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "client/ui/draw.h"
#include "client/ui/menu_style.h"
#include "client/ui/menus.h"
#include "client/ui/text.h"
#include "shared/game/config.h"
#include "shared/game/hel_plinko.h"

namespace flix {
using namespace ui;

namespace {
constexpr PanelSkin kPlinkoSkin{0x713930, 0x4B2526, 0xAD6C56};
constexpr double kAutoInterval = 0.35;
constexpr std::size_t kMaxOnBoard = 20;


const char* refusal(HelPlinkoStatus status) {
    switch (status) {
        case HelPlinkoStatus::Unavailable: return trEnZh("Unavailable here", "当前模式暂不可用");
        case HelPlinkoStatus::NoStars: return trEnZh("Not enough stars", "星星不足");
        case HelPlinkoStatus::InvalidTier: return trEnZh("This rarity cannot be used", "该品质不可投放");
        case HelPlinkoStatus::NoChips: return trEnZh("No chips of this rarity", "该品质筹码不足");
        case HelPlinkoStatus::StackFull: return trEnZh("Make room for the payout", "请先留出筹码库存空间");
        default: return trEnZh("Please try again", "请稍后再试");
    }
}

void drawChip(Canvas& canvas, Vec2 at, double radius, double angle, double squash, Rarity rarity) {
    canvas.save();
    canvas.translate(static_cast<float>(at.x), static_cast<float>(at.y));
    canvas.rotate(static_cast<float>(angle));
    canvas.scale(static_cast<float>(1.0 + squash * 0.55), static_cast<float>(1.0 - squash));
    canvas.scale(static_cast<float>(radius / 32.0), static_cast<float>(radius / 32.0));
    const auto hue = rarityColor(rarity);
    disc(canvas, {0, 0}, 29.0, shade(hue, 0.68), 0x34232B, 4.0);
    disc(canvas, {0, -0.6}, 26.8, hue, shade(hue, 0.88), 0.9);
    for (int segment = 0; segment < 6; ++segment) {
        canvas.save();
        canvas.rotate(static_cast<float>(kTau * segment / 6.0));
        fillQuadRound(canvas, {-3.6, -27.1, 7.2, 7.2}, 2.0, 0xD5C8B3);
        fillQuadRound(canvas, {-2.9, -26.9, 5.8, 5.2}, 1.5, 0xFFF6E4);
        canvas.restore();
    }
    disc(canvas, {0, 0}, 17.4, shade(hue, 0.82), 0xFFF3D5, 1.5);
    disc(canvas, {0, 0}, 13.2, shade(hue, 0.82), shade(hue, 0.64), 1.0);
    canvas.beginPath();
    canvas.arc(0.0f, 0.0f, 11.8f, static_cast<float>(kPi * 1.10), static_cast<float>(kPi * 1.66));
    setStroke(canvas, 0xFFF6E4, 0.52);
    canvas.setLineWidth(1.15f);
    canvas.stroke();
    canvas.restore();
}
}

double HelPlinkoPanel::pointScale(const Window& window) {
    return window.uiScale() / (window.devicePixelRatio() * window.renderScale());
}

Rect HelPlinkoPanel::bounds(const Window& window) {
    const double scale = pointScale(window);
    const double width = window.width() * scale;
    const double height = window.height() * scale;
    const double availableWidth = std::max(0.0, width - 16.0);
    const double availableHeight = std::max(0.0, height - 48.0);
    const double w = std::min(availableWidth, 550.0);
    const double h = std::min(availableHeight, 610.0);
    return {(width - w) / (2.0 * scale), std::max(38.0, (height - h) / 2.0) / scale,
            w / scale, h / scale};
}

bool HelPlinkoPanel::render(MenuContext& ctx, bool interactive) {
    Canvas& canvas = ctx.canvas;
    const double scale = pointScale(ctx.window);
    const Rect r{0.0, 0.0, ctx.bounds.w * scale, ctx.bounds.h * scale};
    canvas.save();
    struct RestoreCanvas { Canvas& canvas; ~RestoreCanvas() { canvas.restore(); } } restore{canvas};
    canvas.translate(static_cast<float>(ctx.bounds.x), static_cast<float>(ctx.bounds.y));
    canvas.scale(static_cast<float>(1.0 / scale), static_cast<float>(1.0 / scale));
    const auto mouse = ctx.mouse();
    const Vec2 point{(mouse.x - ctx.bounds.x) * scale, (mouse.y - ctx.bounds.y) * scale};
    const auto over = [&](Rect rect) { return rect.contains(point); };
    const auto clicked = [&](Rect rect) { return ctx.released() && over(rect); };
    const double now = ctx.timeSeconds;
    const auto chip = content().petalIndex(kPlinkoChipPetal);
    const auto stopForRequest = [&](const Request& request) {
        if (request.action != HelPlinkoAction::Drop) return;
        const auto tier = std::find(kPlinkoTiers.begin(), kPlinkoTiers.end(), request.rarity);
        if (tier != kPlinkoTiers.end()) nextAutoDrop_[tier - kPlinkoTiers.begin()] = -1.0;
    };
    for (const auto reply : ctx.net.takeHelPlinkoReplies()) {
        const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const Request& request) {
            return request.id == reply.request;
        });
        if (found == pending_.end()) continue;
        const Request request = *found;
        pending_.erase(found);
        const auto status = static_cast<HelPlinkoStatus>(reply.status);
        if (status != HelPlinkoStatus::Success) {
            message_ = refusal(status);
            stopForRequest(request);
        } else if (request.action == HelPlinkoAction::Drop) {
            falling_.push_back({reply.path, request.rarity, now});
        } else if (plinkoBuyCount(request.action) > 0) {
            message_ = std::string(trEnZh("Purchased ×", "筹码已买入 ×")) + std::to_string(plinkoBuyCount(request.action));
        } else if (request.action == HelPlinkoAction::Exchange) {
            message_ = trEnZh("Received 1 Apex coin", "已换得 1 枚 Apex 硬币");
        }
    }
    const auto expired = std::remove_if(pending_.begin(), pending_.end(), [&](const Request& request) {
        if (now - request.sentAt <= 6.0) return false;
        stopForRequest(request);
        return true;
    });
    if (expired != pending_.end()) {
        pending_.erase(expired, pending_.end());
        message_ = trEnZh("Response delayed; check your inventory", "回应超时，请检查背包");
    }
    for (const auto& chipInBoard : falling_) {
        const double landing = chipInBoard.startsAt + kHelPlinkoDropSeconds;
        if (landing > now || landing <= latestLanding_) continue;
        latestLanding_ = landing;
        latestSlot_ = helPlinkoSlot(chipInBoard.path);
        message_ = std::string(trEnZh("Chips returned: ", "返还筹码：")) +
                   std::to_string(kHelPlinkoPayout[latestSlot_]);
    }
    falling_.erase(std::remove_if(falling_.begin(), falling_.end(), [&](const FallingChip& chipInBoard) {
        return now - chipInBoard.startsAt > kHelPlinkoDropSeconds + 0.22;
    }), falling_.end());

    const auto canDrop = [&](Rarity tier) {
        const auto held = ctx.net.profile().stackCount(chip, tier);
        const auto inTransit = std::count_if(pending_.begin(), pending_.end(), [&](const Request& request) {
            return request.rarity == tier && request.action == HelPlinkoAction::Drop;
        });
        return interactive && chip != kInvalidIndex && held > static_cast<unsigned>(inTransit) &&
               pending_.size() + falling_.size() < kMaxOnBoard;
    };
    const auto send = [&](HelPlinkoAction action, Rarity tier) {
        pending_.push_back({ctx.net.requestHelPlinko(action, tier), action, tier, now});
    };

    panelCard(canvas, r, kPlinkoSkin);
    const Rect close{r.right() - 50.0, r.y + 6.0, 44.0, 42.0};
    panelClose(canvas, close, over(close));
    if (interactive && clicked(close)) {
        stop();
        shopExpanded_ = false;
        return false;
    }
    const double pad = 10.0;
    const double shopButtonHeight = r.h < 400.0 ? 40.0 : 48.0;
    const double gap = 6.0;
    const double railWidth = std::clamp(r.w * 0.23, 80.0, 108.0);
    const double boardRight = r.right() - pad;
    const double controlsX = r.x + pad;
    const double controlsWidth = boardRight - controlsX;
    const double cardGap = r.w < 420.0 ? 2.0 : 4.0;
    const double width = (controlsWidth - cardGap * (kPlinkoTiers.size() - 1)) / kPlinkoTiers.size();
    const double cardHeight = std::clamp(width, 34.0, 51.0);
    const double autoFaceHeight = cardHeight * 0.46;
    const double autoHitHeight = std::max(34.0, autoFaceHeight);
    constexpr double autoGap = 3.0;
    const double controlsY = r.bottom() - pad - cardHeight - autoHitHeight - autoGap;
    const Rect board{controlsX, r.y + 49.0, controlsWidth, controlsY - 21.0 - (r.y + 49.0)};
    const auto caption = [&](const std::string& value, double x, double y, double size,
                             std::uint32_t fill = 0xFFF0D4) {
        TextStyle style;
        style.size = size;
        style.fill = fill;
        style.align = Align::Centre;
        style.baseline = Baseline::Middle;
        text(canvas, value, x, y, style);
    };
    caption(trEnZh("Plinko", "弹珠"), r.x + r.w * 0.5, r.y + 24.0, 22.0);
    const Rect back{r.x + pad, r.y + 7.0, 76.0, 37.0};
    ButtonStyle backStyle;
    backStyle.fill = 0x824338;
    backStyle.textSize = 14.0;
    backStyle.outlineWidth = 2.0;
    button(canvas, back, trEnZh("Craft", "合成"), over(back), false, backStyle);
    if (interactive && clicked(back)) {
        stop();
        shopExpanded_ = false;
        craftTab_ = true;
        return true;
    }

    fillQuadRound(canvas, board, 8.0, 0x512B2B);
    const double middle = board.x + board.w * 0.5;
    const double slotMargin = 4.0;
    const double slotWidth = (board.w - 2.0 * slotMargin) / static_cast<double>(kHelPlinkoPayout.size());
    const double stepX = slotWidth * 0.5;
    const double radius = std::min(8.5, slotWidth * 0.28);
    const double pegRadius = std::clamp(slotWidth * 0.11, 2.5, 4.5);
    const double top = board.y + radius + 3.0;
    const double slots = board.bottom() - 26.0;
    const double stepY = std::max(0.0, (slots - top - 6.0) / (kHelPlinkoRows + 1));
    for (int row = 1; row <= kHelPlinkoRows; ++row) {
        for (int i = -1; i <= row; ++i) {
            const double x = middle + (2 * i - row + 1) * stepX;
            disc(canvas, {x, top + row * stepY}, pegRadius, 0xBC7C65, 0x824737, 1);
        }
    }
    for (int i = 0; i < static_cast<int>(kHelPlinkoPayout.size()); ++i) {
        const Rect slot{board.x + slotMargin + i * slotWidth + 1.0, slots, slotWidth - 2.0, 25.0};
        const int multiplier = kHelPlinkoPayout[i];
        const std::uint32_t color = latestSlot_ == i && now - latestLanding_ < 0.65 ? 0xB86638
            : multiplier == 0 ? 0x5E3431 : multiplier > 1 ? 0x925842 : 0x72453A;
        fillQuadRound(canvas, slot, 4, color);
        const auto label = std::to_string(multiplier) + "x";
        const double fontSize = std::min({14.0, slot.w * 0.67,
                                          14.0 * (slotWidth - 2.0) / std::max(1.0, measure(label, 14.0))});
        caption(label, slot.x + slot.w * 0.5, slot.y + slot.h * 0.5, fontSize);
    }

    // The first contact is the centre pin. Each result bit turns the chip
    // toward the next pin or slot. The spin reverses with lateral travel.
    canvas.save();
    canvas.beginPath();
    canvas.rect(static_cast<float>(board.x), static_cast<float>(board.y),
                static_cast<float>(board.w), static_cast<float>(board.h));
    canvas.clip();
    for (const auto& chipInBoard : falling_) {
        const double elapsed = std::max(0.0, now - chipInBoard.startsAt);
        const double progress = clamp(elapsed / kHelPlinkoDropSeconds, 0.0, 1.0);
        const double travel = progress * (kHelPlinkoRows + 1);
        const int leg = std::min(kHelPlinkoRows, static_cast<int>(travel));
        const double t = travel - leg;
        double offset = 0.0;
        for (int bit = 0; bit < leg - 1; ++bit)
            offset += (chipInBoard.path & (1u << bit)) ? stepX : -stepX;
        const double direction = leg == 0 ? 0.0 : (chipInBoard.path & (1u << (leg - 1))) ? 1.0 : -1.0;
        const auto heightAt = [&](int row) {
            if (row == 0) return top;
            if (row == kHelPlinkoRows + 1) return slots + 11.0;
            return top + row * stepY - radius - pegRadius;
        };
        const double from = heightAt(leg);
        const double to = heightAt(leg + 1);
        double across = 0.0;
        double y = from + (to - from) * t * t;
        if (leg > 0) {
            constexpr double contactPhase = 0.48;
            const double contactRadius = radius + pegRadius;
            const double angle = std::asin(std::min(0.75, stepX * 0.68 / contactRadius));
            if (t <= contactPhase) {
                // First a short tap, then roll along the upper edge of the pin.
                const double turn = angle * t / contactPhase;
                across = contactRadius * std::sin(turn);
                y = from + contactRadius * (1.0 - std::cos(turn));
                const double tap = std::sin(kPi * std::min(1.0, t / 0.22));
                y -= std::min(1.8, stepY * 0.10) * tap * tap;
            } else {
                // Leave the pin with some downward speed and accelerate to the next one.
                const double travel = (t - contactPhase) / (1.0 - contactPhase);
                const double contactX = contactRadius * std::sin(angle);
                const double contactY = from + contactRadius * (1.0 - std::cos(angle));
                across = contactX + (stepX - contactX) * (travel * (2.0 - travel));
                y = contactY + (to - contactY) * (travel * (0.35 + 0.65 * travel));
            }
        }
        const double drift = direction * across;
        const double impact = leg > 0 ? std::max(0.0, 1.0 - t * 7.0) : 0.0;
        if (impact > 0.0) {
            disc(canvas, {middle + offset, top + leg * stepY}, pegRadius + (1.0 - impact) * 4.0,
                 0xFFE1A4, 0xFFE1A4, 1.0, impact * 0.62);
        }
        const double settle = std::max(0.0, elapsed - kHelPlinkoDropSeconds);
        drawChip(canvas, {middle + offset + drift, y - 1.2 * std::sin(kPi * settle / 0.22)},
                 radius, (offset + drift) / radius * 0.95, impact * 0.09, chipInBoard.rarity);
    }
    canvas.restore();

    std::string status = message_.empty() ? trEnZh("Tap a chip to drop it", "点筹码即可投入") : message_;
    if (!falling_.empty() && now - latestLanding_ > 0.6)
        status = std::to_string(falling_.size()) + trEnZh(" in play", " 枚筹码下落中");
    caption(status, middle, controlsY - 11.0, 12.0);

    const std::array<const char*, kPlinkoTiers.size()> labelsZh{
        "普", "罕", "稀", "史", "传", "神", "究", "超", "永", "永久超凡", "Apex"};
    const std::array<const char*, kPlinkoTiers.size()> labelsEn{
        "C", "U", "R", "E", "L", "M", "U", "S", "E", "PH", "Apex"};
    for (std::size_t i = 0; i < kPlinkoTiers.size(); ++i) {
        const auto tier = kPlinkoTiers[i];
        const Rect card{controlsX + i * (width + cardGap), controlsY, width, cardHeight};
        const auto stack = ctx.net.profile().stackCount(chip, tier);
        const auto hue = rarityColor(tier);
        const bool canPlay = canDrop(tier);
        const double exposure = stack > 0 ? 0.84 : 0.56;
        const auto face = hsvScale(hue, exposure * (canPlay && over(card) ? 1.1 : 1.0));
        fillQuadRound(canvas, card, 4.0, shade(hue, 0.39));
        fillQuadRound(canvas, {card.x + 1, card.y + 1, card.w - 2, card.h - 3}, 3.0, shade(hue, 0.94));
        fillQuadRound(canvas, {card.x + 2, card.y + 3, card.w - 4, card.h - 6}, 2.0, face);
        const double centerX = card.x + card.w * 0.5;
        drawChip(canvas, {centerX, card.y + card.h * 0.45}, std::clamp(card.w * 0.215, 5.5, 11.0), 0, 0, tier);
        const auto label = trEnZh(labelsEn[i], labelsZh[i]);
        const auto fit = [&](const std::string& value, double size) {
            return std::min(size, size * (card.w - 4.0) / std::max(1.0, measure(value, size)));
        };
        caption(label, centerX, card.y + card.h * 0.80, fit(label, 10.0));
        const auto quantity = "×" + std::to_string(stack);
        TextStyle badge;
        badge.size = fit(quantity, 12.0);
        badge.align = Align::Right;
        badge.baseline = Baseline::Middle;
        canvas.save();
        canvas.translate(static_cast<float>(card.right() - 3.0), static_cast<float>(card.y + 9.0));
        canvas.rotate(static_cast<float>(kPi / 12.0));
        text(canvas, quantity, 0, 0, badge);
        canvas.restore();
        if (canPlay && clicked(card)) send(HelPlinkoAction::Drop, tier);

        const Rect autoHit{card.x, card.bottom() + autoGap, card.w, autoHitHeight};
        const Rect autoFace{autoHit.x, autoHit.y, autoHit.w, autoFaceHeight};
        const bool active = nextAutoDrop_[i] >= 0.0;
        ButtonStyle style;
        style.outlineWidth = 1.5;
        style.fill = active ? 0xA35642 : 0x5E312D;
        style.outline = active ? 0xFFD695 : 0xAB6B59;
        style.textSize = fit("Auto", 12.0);
        style.enabled = interactive && (active || stack > 0);
        button(canvas, autoFace, "Auto", over(autoHit), false, style);
        if (style.enabled && clicked(autoHit)) nextAutoDrop_[i] = active ? -1.0 : now;
    }

    const Rect shopToggle{close.x - 50.0, close.y + 1.0, 44.0, 40.0};
    ButtonStyle toggleStyle;
    toggleStyle.fill = shopExpanded_ ? 0xA35642 : 0x794336;
    toggleStyle.textSize = 12.0;
    toggleStyle.outlineWidth = 2.0;
    button(canvas, shopToggle, trEnZh("Shop", "商店"), over(shopToggle), false, toggleStyle);
    const bool toggled = interactive && clicked(shopToggle);
    if (toggled) shopExpanded_ = !shopExpanded_;
    if (shopExpanded_) {
        const bool anyAuto = std::any_of(nextAutoDrop_.begin(), nextAutoDrop_.end(),
                                         [](double at) { return at >= 0.0; });
        const Rect quantityButton{boardRight - railWidth * 2 - gap, board.y, railWidth, shopButtonHeight};
        if (interactive && !toggled && clicked(quantityButton))
            buySizeIndex_ = (buySizeIndex_ + 1) % kPlinkoBuyActions.size();
        const auto buyAction = kPlinkoBuyActions[buySizeIndex_];
        const int quantity = plinkoBuyCount(buyAction);
        const auto countLabel = std::string(trEnZh("Qty ×", "数量 ×")) + std::to_string(quantity);
        button(canvas, quantityButton, countLabel, over(quantityButton), false, toggleStyle);
        const std::array<std::string,3> shopLabels{
            std::string(trEnZh("Super · ", "超级 · ")) + std::to_string(quantity * kPlinkoSuperPrice) + "★",
            std::string(trEnZh("Eternal · ", "永恒 · ")) + std::to_string(quantity * kPlinkoEternalPrice) + "★",
            trEnZh("Apex → Coin", "Apex 换硬币")};
        for (int i = 0; i < 3; ++i) {
            const Rect shop{boardRight - railWidth, board.y + i * (shopButtonHeight + gap),
                            railWidth, shopButtonHeight};
            const auto tier = i == 0 ? Rarity::Super : i == 1 ? Rarity::Unique : Rarity::Ascendant;
            ButtonStyle style;
            style.fill = 0x794336;
            style.textSize = 12.0;
            style.outlineWidth = 2.0;
            style.shrinkToFit = true;
            style.enabled = interactive && !anyAuto && pending_.empty();
            button(canvas, shop, shopLabels[i], over(shop), false, style);
            if (!toggled && style.enabled && clicked(shop))
                send(i == 2 ? HelPlinkoAction::Exchange : buyAction, tier);
        }
    }
    const std::size_t first = autoStart_;
    for (std::size_t index = 0; index < kPlinkoTiers.size(); ++index) {
        const std::size_t i = (first + index) % kPlinkoTiers.size();
        auto& nextDrop = nextAutoDrop_[i];
        if (nextDrop < 0.0 || now < nextDrop) continue;
        const auto tier = kPlinkoTiers[i];
        if (canDrop(tier)) {
            send(HelPlinkoAction::Drop, tier);
            nextDrop = now + kAutoInterval;
            autoStart_ = (i + 1) % kPlinkoTiers.size();
        } else if (ctx.net.profile().stackCount(chip, tier) == 0) {
            const auto awaitingResult = [&](const Request& request) {
                return request.action == HelPlinkoAction::Drop && request.rarity == tier;
            };
            if (std::none_of(pending_.begin(), pending_.end(), awaitingResult)) nextDrop = -1.0;
        }
    }
    return true;
}

} // namespace flix