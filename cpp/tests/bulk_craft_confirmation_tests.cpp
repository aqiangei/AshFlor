#include <cmath>
#include <SDL.h>

#include "test.h"
#include "server_harness.h"
#include "fixture_content.h"
#include "client/render/world_renderer.h"
#include "client/ui/craft_bulk_confirmation.h"
#include "client/ui/text.h"

using namespace flix;
using namespace flix::testsupport;

namespace {

std::uint32_t confirmedCraftBytes(NetClient& net) {
    std::uint32_t received = 0, sent = 0;
    std::vector<NetClient::WireEvent> messages;
    net.takeWireStats(received, sent, messages, 512);
    for (const auto& message : messages) {
        if (!message.incoming && std::string(message.name) == "bulkCraftMythicUltra") return message.bytes;
    }
    return 0;
}

} // namespace

TEST(mythic_ultra_confirm_panel_handles_cancel_refresh_and_stale_requests_over_loopback) {
    Harness h("4108-ui", [](const std::string& path) {
        seedUser(path, "craftui", "Fixture42!");
        seedStack(path, "craftui", "petal_basic", Rarity::Common, 100);
        seedStack(path, "craftui", "petal_rose", Rarity::Mythic, 20000);
        seedStack(path, "craftui", "petal_rose", Rarity::Legendary, 50000);
        seedStack(path, "craftui", "petal_rose", Rarity::Ultra, 6000);
        seedStack(path, "craftui", "petal_rock", Rarity::Mythic, 12000);
        seedStack(path, "craftui", "petal_rock", Rarity::Ultra, 4);
        seedStack(path, "craftui", "petal_leaf", Rarity::Mythic, 20);
        seedStack(path, "craftui", "petal_leaf", Rarity::Ultra, 30);
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    NetClient client;
    const bool loggedIn = loginAs(h, client, "craftui", "Fixture42!");
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    const auto rock = content().petalIndex("rock");
    const auto leaf = content().petalIndex("leaf");
    const auto basic = content().petalIndex("basic");
    CHECK(awaitProfile(h, client, [&](const Profile& p) { return p.stackCount(rose, Rarity::Mythic) == 20000; }));

    Window window;
    std::string error;
    const bool opened = window.open(1000, 760, "Bulk Craft confirmation", error, false);
    CHECK(opened);
    if (!opened) return;
    CHECK(ui::Fonts::init(dataDir(), error));
    ui::setLanguage(ui::Language::En);
    SpriteCache sprites;
    WorldRenderer renderer;
    DragState drag;
    ClientSettings settings;
    settings.craftBlacklist = {"leaf"};
    CraftingPanel panel;
    const Rect bounds{28, 20, CraftingPanel::size(client.profile()).x, 560};
    double seconds = 1.0;
    MenuContext ctx{window.canvas(), window, client, sprites, renderer, settings, drag,
                    seconds, 0.016, bounds};
    const auto layout = drawSlotCard(ctx.canvas, bounds, ui::kCraftingSkin, "Craft", "Craft", {});
    const auto entry = ui::mythicUltraCraftEntry(bounds, layout.listView);
    const auto initial = ui::mythicUltraCraftMaterials(client.profile(), settings.craftBlacklist);
    const auto buttons = ui::drawMythicUltraConfirmation(ctx, layout, initial, false);
    CHECK_EQ(initial.size(), 3u);
    const auto startLabels = ui::mythicUltraCraftLabels(initial);
    CHECK(startLabels[0] == "Mythic materials  ×32000");
    CHECK(startLabels[1] == "Ultra materials  ×6000");
    ui::setLanguage(ui::Language::Zh);
    const auto zh = ui::mythicUltraCraftLabels(initial);
    CHECK(zh[0] == "神话材料  ×32000");
    CHECK(zh[1] == "究极材料  ×6000");
    ui::setLanguage(ui::Language::En);

    const auto click = [&](Rect rect) {
        const double scale = window.uiScaleMultiplier();
        const int x = static_cast<int>(std::lround((rect.x + rect.w * 0.5) * scale));
        const int y = static_cast<int>(std::lround((rect.y + rect.h * 0.5) * scale));
        SDL_Event move{};
        move.type = SDL_MOUSEMOTION;
        move.motion.x = x;
        move.motion.y = y;
        CHECK_EQ(SDL_PushEvent(&move), 1);

        SDL_Event button{};
        button.button.button = SDL_BUTTON_LEFT;
        for (const auto type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
            button.type = type;
            CHECK_EQ(SDL_PushEvent(&button), 1);
            CHECK(window.pump());
            ctx.timeSeconds += 0.016;
            CHECK(panel.render(ctx));
        }
    };

    // Opening and canceling never submit a packet or change stock.
    panel.update(client, seconds);
    confirmedCraftBytes(client);
    click(entry);
    CHECK_EQ(confirmedCraftBytes(client), 33u); // non-mutating ticket preparation
    h.step(4, {&client});
    click(buttons.cancel);
    CHECK_EQ(confirmedCraftBytes(client), 0u);
    CHECK_EQ(client.profile().stackCount(rose, Rarity::Mythic), 20000u);

    // A normal craft's update lands after preview opens and before Confirm.
    click(entry);
    h.step(4, {&client});
    confirmedCraftBytes(client);
    for (int i = 0; i < 4; ++i) {
        const auto before = client.profile().stackCount(rose, Rarity::Mythic);
        client.requestCraft(rose, Rarity::Mythic, 5);
        CHECK(awaitProfile(h, client, [&](const Profile& p) {
            return p.stackCount(rose, Rarity::Mythic) < before;
        }));
        CHECK(panel.render(ctx));
        CHECK_EQ(confirmedCraftBytes(client), 0u); // idle renders never requote
    }
    const auto afterLocalChange = client.profile().stackCount(rose, Rarity::Mythic);
    const auto fresh = ui::mythicUltraCraftMaterials(client.profile(), settings.craftBlacklist);
    CHECK(ui::mythicUltraCraftLabels(fresh)[0] ==
          "Mythic materials  ×" + std::to_string(afterLocalChange + 12000));
    click(buttons.confirm);
    CHECK_EQ(confirmedCraftBytes(client), 33u); // refreshed ticket only
    h.step(4, {&client});

    // The next Confirm sends the displayed list. Before the server reads it,
    // an earlier normal craft invalidates a stack without the UI knowing.
    client.requestCraft(rose, Rarity::Mythic, 5);
    click(buttons.confirm);
    CHECK_EQ(confirmedCraftBytes(client), 14u); // frame + opcode + action + ticket
    CHECK(h.stepUntil({&client}, [&] {
        const auto& lines = client.chat();
        return std::any_of(lines.begin(), lines.end(), [](const auto& line) {
            return line.text.find("重新确认") != std::string::npos;
        });
    }));
    const auto afterRace = client.profile().stackCount(rose, Rarity::Mythic);
    CHECK(afterRace < afterLocalChange && afterRace >= afterLocalChange - 5);
    CHECK_EQ(client.profile().stackCount(rock, Rarity::Mythic), 12000u);

    click(entry);
    h.step(4, {&client});
    CHECK_EQ(confirmedCraftBytes(client), 33u);
    const auto approved = client.profile().stackCount(rose, Rarity::Mythic);
    client.requestCraft(rose, Rarity::Legendary, 50000);
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stackCount(rose, Rarity::Mythic) > approved;
    }));
    const auto gained = client.profile().stackCount(rose, Rarity::Mythic) - approved;
    CHECK(panel.render(ctx));
    CHECK_EQ(confirmedCraftBytes(client), 0u);
    click(buttons.confirm);
    CHECK_EQ(confirmedCraftBytes(client), 14u);
    CHECK(awaitProfile(h, client, [&](const Profile& p) {
        return p.stackCount(rose, Rarity::Mythic) < gained + 5 && p.stackCount(rock, Rarity::Mythic) < 5;
    }));
    CHECK(client.profile().stackCount(rose, Rarity::Mythic) >= gained);
    CHECK_EQ(client.profile().stackCount(basic, Rarity::Common), 100u);
    CHECK_EQ(client.profile().stackCount(leaf, Rarity::Mythic), 20u);
    CHECK_EQ(client.profile().stackCount(leaf, Rarity::Ultra), 30u);
    CHECK(client.profile().stackCount(rock, Rarity::Ultra) > 4u);
    CHECK_EQ(client.profile().stackCount(rock, Rarity::Super), 0u);
    std::printf("SDL preview showed %s / %s; cancel sent zero, stale commit sent 14 bytes and refreshed, "
                "new confirmation applied exactly three bag stacks\n", startLabels[0].c_str(), startLabels[1].c_str());
}
