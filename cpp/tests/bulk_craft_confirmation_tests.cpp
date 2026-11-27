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

TEST(craftable_filter_skips_understocked_rows_and_crafts_an_eligible_batch) {
    Harness server("craft-filter-ui", [](const std::string& path) {
        seedUser(path, "craftfilter", "Fixture42!");
        seedStack(path, "craftfilter", "petal_basic", Rarity::Common, 4);
        seedStack(path, "craftfilter", "petal_fragment", Rarity::PermanentHyper, 5);
    }, dataDir(), 0);
    CHECK(server.ready);
    if (!server.ready) return;
    NetClient client;
    const bool loggedIn = loginAs(server, client, "craftfilter", "Fixture42!");
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto fragment = content().petalIndex("fragment");
    CHECK_EQ(client.profile().stackCount(fragment, Rarity::PermanentHyper), 5u);

    Window window;
    std::string error;
    const bool opened = window.open(1000, 760, "Craft filter", error, false);
    CHECK(opened);
    if (!opened) return;
    CHECK(ui::Fonts::init(dataDir(), error));
    ui::setLanguage(ui::Language::En);
    SpriteCache sprites;
    WorldRenderer renderer;
    DragState drag;
    ClientSettings settings;
    CraftingPanel panel;
    const Rect bounds{28, 20, CraftingPanel::size(client.profile()).x, 680};
    MenuContext ctx{window.canvas(), window, client, sprites, renderer, settings, drag, 1.0, .016, bounds};
    const auto layout = drawSlotCard(ctx.canvas, bounds, ui::kCraftingSkin, "Crafting", "Craft", {});
    const auto render = [&] {
        CHECK(window.pump());
        ctx.timeSeconds += .016;
        CHECK(panel.render(ctx));
    };
    const auto click = [&](double x, double y) {
        SDL_Event move{};
        move.type = SDL_MOUSEMOTION;
        const double scale = window.uiScaleMultiplier();
        move.motion.x = static_cast<int>(std::lround(x * scale));
        move.motion.y = static_cast<int>(std::lround(y * scale));
        CHECK(SDL_PushEvent(&move) == 1);
        SDL_Event button{};
        button.button.button = SDL_BUTTON_LEFT;
        button.button.x = move.motion.x;
        button.button.y = move.motion.y;
        for (const auto type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
            button.type = type;
            CHECK(SDL_PushEvent(&button) == 1);
            render();
        }
    };
    const double gridY = layout.listView.y + 82 + 34 + 40;
    const double filterX = bounds.right() - 12 - 66;
    const double filterY = layout.listView.y + 82 + 14;
    const int column = rarityIndex(Rarity::Unique) + 1;
    const double gridWidth = (column + 2) * 70.0 + 16; // Apex follows the permanent-hyper cell.
    const double fragmentX = bounds.x + (bounds.w - gridWidth) * .5 + 35 + 70 * column;
    const double craftWidth = ui::measure("Craft", 16) + 26;
    const double pairWidth = craftWidth + 8 + ui::measure("Craft All", 16) + 26;
    const double craftX = layout.button.x + layout.button.w * .5 - pairWidth * .5 + craftWidth * .5;
    const double craftY = layout.button.y + layout.button.h * .5;
    render();
    click(fragmentX, gridY);
    click(craftX, craftY);
    server.step(10, {&client});
    CHECK(!client.craftOutcome().pending);
    CHECK_EQ(client.profile().stackCount(fragment, Rarity::PermanentHyper), 5u);

    click(filterX, filterY);
    // The only eligible stack occupies the first cell in the filtered grid.
    const double firstCellX = bounds.x + (bounds.w - gridWidth) * .5 + 35;
    click(firstCellX, gridY);
    click(craftX, craftY);
    CHECK(server.stepUntil({&client}, [&]{ return client.craftOutcome().pending; }, 250));
    CHECK(client.profile().stackCount(fragment, Rarity::PermanentHyper) < 5u);
}

TEST(apex_bulk_craft_confirmation_only_spends_eligible_permanent_hyper_inventory) {
    constexpr std::uint32_t batchSize = 5;
    Harness h("apex-bulk-ui", [](const std::string& path) {
        seedUser(path, "apexbulk", "Fixture42!");
        Database db;
        std::string error;
        if (!db.load(path, error)) return;
        const auto* account = db.findUser("apexbulk");
        if (!account) return;
        auto& bag = db.progress(account->id);
        bag.loadout.resize(kLoadoutSlots);
        bag.loadout[0] = StoredItem{"petal", Rarity::PermanentHyper, "rose"};
        bag.addItem(Rarity::Common, "petal_rose", 20);
        bag.addItem(Rarity::Unique, "petal_rose", 100);
        bag.addItem(Rarity::PermanentHyper, "petal_rose", 5000);
        bag.addItem(Rarity::PermanentHyper, "petal_leaf", 200);
        bag.addItem(Rarity::PermanentHyper, "petal_mjolnir", 200);
        bag.addItem(Rarity::PermanentHyper, "petal_rock", 21);
        bag.addItem(Rarity::Ascendant, "petal_rock", kMaxStackCount - 1);
        bag.addItem(Rarity::PermanentHyper, "petal_basic", 50);
        bag.addItem(Rarity::Ascendant, "petal_basic", kMaxStackCount);
        db.markDirty();
        db.save();
    }, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;

    NetClient client;
    const bool loggedIn = loginAs(h, client, "apexbulk", "Fixture42!");
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    const auto leaf = content().petalIndex("leaf");
    const auto hammer = content().petalIndex("mjolnir");
    const auto rock = content().petalIndex("rock");
    const auto basic = content().petalIndex("basic");
    CHECK(awaitProfile(h, client, [&](const Profile& profile) {
        return profile.stackCount(rose, Rarity::PermanentHyper) == 5000;
    }));

    Window window;
    std::string error;
    const bool opened = window.open(1000, 760, "Bulk Apex craft", error, false);
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
    MenuContext ctx{window.canvas(), window, client, sprites, renderer, settings, drag,
                    1.0, 0.016, bounds};
    const auto layout = drawSlotCard(ctx.canvas, bounds, ui::kCraftingSkin, "Craft", "Craft", {});
    const auto click = [&](double x, double y) {
        const double scale = window.uiScaleMultiplier();
        SDL_Event move{};
        move.type = SDL_MOUSEMOTION;
        move.motion.x = static_cast<int>(std::lround(x * scale));
        move.motion.y = static_cast<int>(std::lround(y * scale));
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

    const double buttonY = layout.listView.y + 19;
    click(bounds.x + 28, buttonY); // Rare to Uncommon.
    click(bounds.x + 28, buttonY); // Uncommon wraps to Apex.
    click(bounds.right() - 55, buttonY); // Arm only.
    h.step(4, {&client});
    CHECK_EQ(client.profile().stackCount(rose, Rarity::PermanentHyper), 5000u);
    click(bounds.right() - 55, buttonY); // Confirm the actual request.
    CHECK(awaitProfile(h, client, [&](const Profile& profile) {
        return profile.stackCount(rose, Rarity::PermanentHyper) < batchSize;
    }));
    const auto& profile = client.profile();
    CHECK(profile.stackCount(rose, Rarity::Ascendant) > 0);
    // One public announcement per successful pool, including the actual totals.
    const auto announcements = [&](const std::string& petal, unsigned made, unsigned spent) {
        const auto details = "(crafted x" + std::to_string(made) + ", consumed x" +
                             std::to_string(spent) + ")";
        unsigned count = 0;
        for (const auto& line : client.chat()) {
            if (line.channel != net::ChatChannel::System ||
                line.text.find("data-petal='" + petal + "'") == std::string::npos) continue;
            CHECK(line.text.find(details) != std::string::npos);
            ++count;
        }
        return count;
    };
    CHECK_EQ(announcements("rose", profile.stackCount(rose, Rarity::Ascendant),
                           5000 - profile.stackCount(rose, Rarity::PermanentHyper)), 1u);
    const unsigned rocksMade = profile.stackCount(rock, Rarity::Ascendant) - (kMaxStackCount - 1);
    CHECK_EQ(announcements("rock", rocksMade, 21 - profile.stackCount(rock, Rarity::PermanentHyper)),
             rocksMade > 0 ? 1u : 0u);
    CHECK_EQ(announcements("leaf", 0, 0), 0u);

    client.requestNotifications(20, 0);
    CHECK(h.stepUntil({&client}, [&] { return !client.notifications().empty(); }, 200));
    CHECK_EQ(client.notifications().size(), rocksMade > 0 ? 2u : 1u);

    CHECK_EQ(profile.stackCount(rose, Rarity::Common), 20u);
    CHECK_EQ(profile.stackCount(rose, Rarity::Unique), 100u);
    CHECK_EQ(profile.loadout[0].petalIndex, rose);
    CHECK(profile.loadout[0].rarity == Rarity::PermanentHyper);
    CHECK_EQ(profile.stackCount(leaf, Rarity::PermanentHyper), 200u);
    CHECK_EQ(profile.stackCount(hammer, Rarity::PermanentHyper), 200u);
    CHECK_EQ(profile.stackCount(basic, Rarity::PermanentHyper), 50u);
    CHECK_EQ(profile.stackCount(basic, Rarity::Ascendant), static_cast<unsigned>(kMaxStackCount));
    CHECK(profile.stackCount(rock, Rarity::PermanentHyper) >= 16u);
    CHECK(profile.stackCount(rock, Rarity::PermanentHyper) < 21u);
    CHECK(profile.stackCount(rock, Rarity::Ascendant) >= static_cast<unsigned>(kMaxStackCount - 1));

    h.step(36, {&client});
    client.requestBulkCraft(Rarity::Uncommon, {});
    CHECK(awaitProfile(h, client, [&](const Profile& updated) {
        return updated.stackCount(rose, Rarity::Common) < batchSize;
    }));
}

TEST(plinko_holds_the_board_and_badge_until_the_chip_lands) {
    Harness server("plinko-badge", [](const std::string& path) {
        seedUser(path, "plinko", "Fixture42!");
        seedStack(path, "plinko", "petal_poker_chip", Rarity::Common, 1);
        seedStack(path, "plinko", "petal_poker_chip", Rarity::Uncommon, 3);
    }, dataDir(), 0);
    CHECK(server.ready);
    if (!server.ready) return;
    NetClient client;
    if (!loginAs(server, client, "plinko", "Fixture42!")) { CHECK(false); return; }
    Window window;
    std::string error;
    if (!window.open(1000, 760, "Plinko", error, false)) { CHECK(false); return; }
    CHECK(ui::Fonts::init(dataDir(), error));
    SpriteCache sprites;
    WorldRenderer renderer;
    DragState drag;
    ClientSettings settings;
    HelPlinkoPanel panel;
    const auto bounds = panel.bounds(window);
    MenuContext ctx{window.canvas(), window, client, sprites, renderer, settings, drag,
                    1.0, .016, bounds};
    auto frame = [&] {
        CHECK(window.pump());
        ctx.timeSeconds += .016;
        CHECK(panel.render(ctx, true));
    };
    const auto scale = panel.pointScale(window);
    const auto click = [&](int column, bool automatic) {
        const double w = bounds.w * scale, h = bounds.h * scale;
        const double gap = w < 420 ? 2 : 4;
        const double card = (w - 20 - 10 * gap) / 11;
        const double height = std::clamp(card, 34.0, 51.0);
        const double x = bounds.x + (10 + card / 2 + column * (card + gap)) / scale;
        const double y = bounds.y + (h - 10 - (automatic ? 17 : 37 + height / 2)) / scale;
        SDL_Event motion{};
        motion.type = SDL_MOUSEMOTION;
        motion.motion.x = std::lround(x * window.uiScaleMultiplier());
        motion.motion.y = std::lround(y * window.uiScaleMultiplier());
        CHECK(SDL_PushEvent(&motion) == 1);
        for (auto type : {SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP}) {
            SDL_Event button{};
            button.type = type;
            button.button.button = SDL_BUTTON_LEFT;
            button.button.x = motion.motion.x;
            button.button.y = motion.motion.y;
            CHECK(SDL_PushEvent(&button) == 1);
            frame();
        }
    };
    const auto dropBytes = [&] {
        std::uint32_t received = 0, sent = 0;
        std::vector<NetClient::WireEvent> messages;
        client.takeWireStats(received, sent, messages, 512);
        for (const auto& event : messages) {
            if (!event.incoming && std::string(event.name) == "helPlinko") return event.bytes;
        }
        return std::uint32_t{0};
    };
    constexpr unsigned oneDropBytes = 4 + 1 + 4 + 1 + 1; // frame, opcode, request ID, action, tier
    frame();
    CHECK_EQ(panel.displayedChips(client.profile(), Rarity::Common, ctx.timeSeconds), 1u);
    click(0, false);
    CHECK_EQ(panel.displayedChips(client.profile(), Rarity::Common, ctx.timeSeconds), 0u);
    click(0, false);
    click(1, true);
    frame();
    CHECK_EQ(dropBytes(), oneDropBytes);
    server.step(10, {&client});
    frame();
    click(1, false);
    ctx.timeSeconds += 3;
    frame();
    CHECK_EQ(panel.displayedChips(client.profile(), Rarity::Common, ctx.timeSeconds), 0u);
    CHECK_EQ(dropBytes(), 0u);
    // An early inventory snapshot must not leak a winning (or losing) result.
    const auto chip = content().petalIndex(kPlinkoChipPetal);
    for (const auto returned : {0u, 1u, 20u}) {
        Profile settled;
        settled.inventory.push_back({chip, Rarity::Common, returned});
        CHECK_EQ(panel.displayedChips(settled, Rarity::Common, ctx.timeSeconds), 0u);
        CHECK_EQ(panel.displayedChips(settled, Rarity::Common, ctx.timeSeconds + 4), returned);
    }
    ctx.timeSeconds += 4;
    frame();
    CHECK_EQ(panel.displayedChips(client.profile(), Rarity::Common, ctx.timeSeconds),
             client.profile().stackCount(chip, Rarity::Common));
    CHECK_EQ(dropBytes(), 0u);
    ctx.timeSeconds += .3;
    frame();
    CHECK_EQ(dropBytes(), oneDropBytes);
    CHECK_EQ(panel.displayedChips(client.profile(), Rarity::Uncommon, ctx.timeSeconds), 2u);
    click(1, false);
    CHECK_EQ(dropBytes(), 0u);
}
