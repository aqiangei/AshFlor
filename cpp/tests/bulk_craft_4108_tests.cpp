#include "test.h"
#include "server_harness.h"
#include "fixture_content.h"

using namespace flix;
using namespace flix::testsupport;

namespace {

struct ConfirmedStack {
    std::uint16_t petal;
    Rarity rarity;
    std::uint32_t count;
};

struct CraftClient : net::TransportHandler {
    net::Dialer dialer;
    bool welcome = false;
    bool accepted = false;
    bool authenticated = false;
    unsigned profiles = 0;
    std::uint32_t request = 0;
    std::uint32_t ticketReply = 0;
    std::uint64_t ticket = 0;
    std::vector<std::string> notices;

    void onMessage(net::Connection&, ByteReader& reader) override {
        switch (static_cast<net::ServerMessage>(reader.u8())) {
        case net::ServerMessage::Welcome:
            CHECK_EQ(reader.u16(), net::kProtocolVersion);
            accepted = reader.boolean();
            welcome = true;
            break;
        case net::ServerMessage::AuthResult:
            authenticated = reader.u8() == static_cast<std::uint8_t>(net::AuthStatus::Ok);
            break;
        case net::ServerMessage::BulkCraftTicket:
            ticketReply = reader.u32();
            ticket = reader.u64();
            break;
        case net::ServerMessage::Notice:
            reader.u8();
            notices.push_back(reader.str());
            break;
        case net::ServerMessage::Profile:
        case net::ServerMessage::ProfileDelta:
            ++profiles;
            break;
        default:
            break;
        }
    }

    void step(Harness& h, int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            dialer.poll(*this, 1);
            h.step(1, {});
            dialer.poll(*this, 1);
        }
    }

    template <typename Predicate>
    bool until(Harness& h, Predicate done) {
        for (int i = 0; i < 200 && !done(); ++i) step(h);
        return done();
    }

    bool handshake(Harness& h, std::uint16_t version) {
        std::string error;
        if (!dialer.connect("127.0.0.1", h.port, error)) return false;
        ByteWriter hello;
        hello.u8(static_cast<std::uint8_t>(net::ClientMessage::Hello));
        hello.u16(version);
        hello.u32(content().contentHash());
        dialer.send(hello);
        return until(h, [&] { return welcome; });
    }

    bool login(Harness& h) {
        if (!handshake(h, net::kProtocolVersion) || !accepted) return false;
        ByteWriter message;
        message.u8(static_cast<std::uint8_t>(net::ClientMessage::Login));
        message.str("craftaudit");
        message.str("Fixture42!");
        dialer.send(message);
        return until(h, [&] { return authenticated && profiles != 0; });
    }

    void prepare(std::initializer_list<ConfirmedStack> list, bool trailing = false) {
        ByteWriter message;
        message.u8(static_cast<std::uint8_t>(net::ClientMessage::BulkCraftMythicUltra));
        message.u8(0);
        message.u32(++request);
        message.u16(static_cast<std::uint16_t>(list.size()));
        for (const auto& item : list) {
            message.u16(item.petal);
            message.u8(static_cast<std::uint8_t>(item.rarity));
            message.u32(item.count);
        }
        if (trailing) message.u8(0);
        dialer.send(message);
    }

    void commit(std::uint64_t id) {
        ByteWriter message;
        message.u8(static_cast<std::uint8_t>(net::ClientMessage::BulkCraftMythicUltra));
        message.u8(1);
        message.u64(id);
        dialer.send(message);
    }

    std::uint64_t quote(Harness& h, std::initializer_list<ConfirmedStack> list) {
        prepare(list);
        CHECK(until(h, [&] { return ticketReply == request; }));
        return ticket;
    }

    void craft(Harness& h, std::initializer_list<ConfirmedStack> list) {
        const auto id = quote(h, list);
        if (id != 0) commit(id);
    }
};

void seedCraftBag(const std::string& path) {
    seedUser(path, "craftaudit", "Fixture42!");
    Database db;
    std::string error;
    CHECK(db.load(path, error));
    const auto* account = db.findUser("craftaudit");
    if (!account) return;
    auto& record = db.progress(account->id);
    record.addItem(Rarity::Common, "petal_basic", 100);
    record.addItem(Rarity::Legendary, "petal_rose", 80);
    record.addItem(Rarity::Mythic, "petal_rose", 1000000);
    record.addItem(Rarity::Ultra, "petal_rose", 6000);
    record.addItem(Rarity::Ultra, "petal_rock", 4);
    record.addItem(Rarity::Mythic, "petal_rock", 25000);
    record.addItem(Rarity::Mythic, "petal_leaf", 20);
    record.addItem(Rarity::Ultra, "petal_leaf", 30);
    record.addItem(Rarity::Super, "petal_rose", 1);
    record.loadout.resize(kLoadoutSlots);
    record.loadout[0] = StoredItem{"petal", Rarity::Ultra, "rose"};
    record.loadout[kLoadoutActiveSlots] = StoredItem{"petal", Rarity::Mythic, "rose"};
    db.markDirty();
    db.save();
}

const PlayerRecord& bag(Harness& h) {
    return *h.server.database().findProgress(h.server.database().findUser("craftaudit")->id);
}

} // namespace

TEST(confirmed_mythic_ultra_bulk_craft_preserves_gear_exclusions_and_new_ultra) {
    Harness h("4108-fixed-bulk", seedCraftBag, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    CraftClient client;
    const bool loggedIn = client.login(h);
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    const auto rock = content().petalIndex("rock");
    const auto previousProfiles = client.profiles;
    const auto spent = client.quote(h, {{rose, Rarity::Mythic, 1000000}, {rock, Rarity::Mythic, 25000},
                                       {rose, Rarity::Ultra, 6000}});
    CHECK(spent != 0);
    client.commit(spent);
    CHECK(client.until(h, [&] {
        return !client.notices.empty() && client.profiles > previousProfiles;
    }));
    const auto& after = bag(h);
    CHECK_EQ(after.itemCount(Rarity::Common, "petal_basic"), 100);
    CHECK_EQ(after.itemCount(Rarity::Legendary, "petal_rose"), 80);
    CHECK(after.itemCount(Rarity::Mythic, "petal_rose") < 5);
    CHECK(after.itemCount(Rarity::Mythic, "petal_rock") < 5);
    CHECK(after.itemCount(Rarity::Ultra, "petal_rose") > 6000);
    CHECK(after.itemCount(Rarity::Ultra, "petal_rock") > 4);
    CHECK_EQ(after.itemCount(Rarity::Super, "petal_rock"), 0);
    CHECK(after.itemCount(Rarity::Super, "petal_rose") > 1);
    CHECK_EQ(after.itemCount(Rarity::Mythic, "petal_leaf"), 20);
    CHECK_EQ(after.itemCount(Rarity::Ultra, "petal_leaf"), 30);
    CHECK(after.loadout[0] && after.loadout[0]->petalType == "rose" &&
          after.loadout[0]->rarity == Rarity::Ultra);
    CHECK(after.loadout[kLoadoutActiveSlots] && after.loadout[kLoadoutActiveSlots]->petalType == "rose" &&
          after.loadout[kLoadoutActiveSlots]->rarity == Rarity::Mythic);
    std::printf("Rose Mythic=%d Ultra=%d Super=%d; Rock Mythic=%d Ultra=%d Super=%d\n",
                after.itemCount(Rarity::Mythic, "petal_rose"), after.itemCount(Rarity::Ultra, "petal_rose"),
                after.itemCount(Rarity::Super, "petal_rose"), after.itemCount(Rarity::Mythic, "petal_rock"),
                after.itemCount(Rarity::Ultra, "petal_rock"), after.itemCount(Rarity::Super, "petal_rock"));

    const auto firstUltra = after.itemCount(Rarity::Ultra, "petal_rose");
    const auto firstSuper = after.itemCount(Rarity::Super, "petal_rose");
    const auto firstNotices = client.notices.size();
    client.craft(h, {{rose, Rarity::Ultra, static_cast<std::uint32_t>(firstUltra)}});
    CHECK(client.until(h, [&] { return client.notices.size() > firstNotices; }));
    CHECK(client.notices.back().find("冷却") != std::string::npos);
    CHECK_EQ(bag(h).itemCount(Rarity::Ultra, "petal_rose"), firstUltra);

    client.step(h, 40);
    client.commit(spent);
    CHECK(client.until(h, [&] { return client.notices.size() > firstNotices + 1; }));
    CHECK(client.notices.back().find("重新确认") != std::string::npos);
    CHECK_EQ(bag(h).itemCount(Rarity::Ultra, "petal_rose"), firstUltra);
    CHECK_EQ(bag(h).itemCount(Rarity::Super, "petal_rose"), firstSuper);
}

TEST(confirmed_mythic_ultra_bulk_craft_rejects_invalid_or_stale_lists_atomically) {
    Harness h("4108-invalid-bulk", seedCraftBag, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    CraftClient client;
    const bool loggedIn = client.login(h);
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    const auto basic = content().petalIndex("basic");
    const auto rock = content().petalIndex("rock");
    const ConfirmedStack roseMythic{rose, Rarity::Mythic, 1000000};
    client.prepare({roseMythic, {basic, Rarity::Common, 100}});
    client.prepare({roseMythic, {rose, Rarity::Ultra, 6000}, {rose, Rarity::Ultra, 6000}});
    client.prepare({roseMythic, {rock, Rarity::Ultra, 4}});
    client.prepare({roseMythic}, true);
    client.step(h, 60);
    CHECK_EQ(bag(h).itemCount(Rarity::Common, "petal_basic"), 100);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rose"), 1000000);
    CHECK_EQ(bag(h).itemCount(Rarity::Ultra, "petal_rose"), 6000);

    const auto firstNotices = client.notices.size();
    CHECK_EQ(client.quote(h, {roseMythic, {rose, Rarity::Ultra, 6001}}), 0u);
    client.step(h, 4);
    CHECK_EQ(client.notices.size(), firstNotices);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rose"), 1000000);
    CHECK_EQ(bag(h).itemCount(Rarity::Ultra, "petal_rose"), 6000);
    CHECK_EQ(bag(h).itemCount(Rarity::Super, "petal_rose"), 1);

    client.craft(h, {roseMythic, {rose, Rarity::Ultra, 6000}});
    CHECK(client.until(h, [&] { return client.notices.size() > firstNotices; }));
    CHECK(bag(h).itemCount(Rarity::Mythic, "petal_rose") < 5);
}

TEST(confirmed_mythic_ultra_bulk_craft_rejects_previous_protocol_version) {
    Harness h("4108-bulk-protocol", {}, dataDir(), 0);
    CHECK(h.ready);
    if (!h.ready) return;
    CraftClient old;
    CHECK(old.handshake(h, net::kProtocolVersion - 1));
    CHECK(!old.accepted);
}

TEST(confirmed_mythic_ultra_tickets_are_single_use_across_refill_reconnect_and_expiration) {
    Harness h("4108-ticket-retention", seedCraftBag, dataDir(), 0);
    CraftClient first;
    const bool loggedIn = h.ready && first.login(h);
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    const ConfirmedStack material{rose, Rarity::Mythic, 1000000};
    const auto spent = first.quote(h, {material});
    CHECK(spent != 0);
    first.commit(spent);
    CHECK(first.until(h, [&] { return bag(h).itemCount(Rarity::Mythic, "petal_rose") < 5; }));
    auto& db = GameServerPeer::authorityDb(h.server);
    db.progress(db.findUser("craftaudit")->id).addItem(Rarity::Mythic, "petal_rose",
        1000000 - bag(h).itemCount(Rarity::Mythic, "petal_rose"));
    first.step(h, 40);
    auto notices = first.notices.size();
    first.commit(spent);
    CHECK(first.until(h, [&] { return first.notices.size() > notices; }));
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rose"), 1000000);

    first.dialer.disconnect();
    h.step(3, {});
    CraftClient rejoined;
    const bool reconnected = rejoined.login(h);
    CHECK(reconnected);
    if (!reconnected) return;
    const auto idle = rejoined.quote(h, {material});
    CHECK(idle != 0 && idle != spent);
    notices = rejoined.notices.size();
    rejoined.commit(spent);
    CHECK(rejoined.until(h, [&] { return rejoined.notices.size() > notices; }));
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rose"), 1000000);
    rejoined.step(h, 310);
    notices = rejoined.notices.size();
    rejoined.commit(idle);
    CHECK(rejoined.until(h, [&] { return rejoined.notices.size() > notices; }));
    CHECK(rejoined.notices.back().find("失效") != std::string::npos);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rose"), 1000000);
}

TEST(confirmed_mythic_ultra_capacity_depends_on_a_confirmed_ultra_pool) {
    Harness h("4108-ticket-capacity", [](const std::string& path) {
        seedUser(path, "craftaudit", "Fixture42!");
        seedStack(path, "craftaudit", "petal_rose", Rarity::Mythic, 50);
        seedStack(path, "craftaudit", "petal_rose", Rarity::Ultra, kMaxStackCount - 1);
    }, dataDir(), 0);
    CraftClient client;
    const bool loggedIn = h.ready && client.login(h);
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    const ConfirmedStack mythic{rose, Rarity::Mythic, 50};
    CHECK_EQ(client.quote(h, {mythic}), 0u);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rose"), 50);
    const auto both = client.quote(h, {mythic, {rose, Rarity::Ultra, kMaxStackCount - 1}});
    CHECK(both != 0);
    client.commit(both);
    CHECK(client.until(h, [&] { return bag(h).itemCount(Rarity::Mythic, "petal_rose") < 5; }));
    CHECK(bag(h).itemCount(Rarity::Ultra, "petal_rose") <= 14);
    CHECK(bag(h).itemCount(Rarity::Super, "petal_rose") > 0);
}

TEST(confirmed_mythic_ultra_4144_retains_pickups_and_refuses_short_previews_silently) {
    Harness h("4144-ticket-churn", seedCraftBag, dataDir(), 0);
    CraftClient client;
    const bool loggedIn = h.ready && client.login(h);
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    const auto rock = content().petalIndex("rock");
    auto& db = GameServerPeer::authorityDb(h.server);
    auto& record = db.progress(db.findUser("craftaudit")->id);

    // Several stale previews arrive after another inventory action has removed petals.
    for (int i = 0; i < 4; ++i) {
        record.addItem(Rarity::Mythic, "petal_rock", -1);
        CHECK_EQ(client.quote(h, {{rock, Rarity::Mythic, 25000}}), 0u);
    }
    client.step(h, 4);
    std::printf("Four refused previews produced %zu chat notices\n", client.notices.size());
    CHECK_EQ(client.notices.size(), 0u);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rock"), 24996);

    // Stock increases both before the server sees Prepare and after it issues a ticket.
    record.addItem(Rarity::Ultra, "petal_rose", 10);
    const auto id = client.quote(h, {{rose, Rarity::Ultra, 6000}, {rock, Rarity::Mythic, 24996}});
    CHECK(id != 0);
    if (id == 0) return;
    record.addItem(Rarity::Ultra, "petal_rose", 30);
    record.addItem(Rarity::Mythic, "petal_rock", 15);
    record.addItem(Rarity::Mythic, "petal_leaf", 1);  // outside the approved list
    const auto notices = client.notices.size();
    client.commit(id);
    CHECK(client.until(h, [&] { return client.notices.size() > notices; }));
    const auto ultra = bag(h).itemCount(Rarity::Ultra, "petal_rose");
    const auto mythic = bag(h).itemCount(Rarity::Mythic, "petal_rock");
    std::printf("Confirmed quantities left Ultra Rose=%d and Mythic Rock=%d\n", ultra, mythic);
    CHECK(ultra >= 40 && ultra < 45);
    CHECK(mythic >= 15 && mythic < 20);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_leaf"), 21);

    client.step(h, 40);
    client.commit(id);
    CHECK(client.until(h, [&] { return client.notices.size() > notices + 1; }));
    CHECK_EQ(bag(h).itemCount(Rarity::Ultra, "petal_rose"), ultra);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rock"), mythic);
}

TEST(confirmed_mythic_ultra_4144_checks_capacity_with_unapproved_ultra_stock) {
    Harness h("4144-ticket-capacity", [](const std::string& path) {
        seedUser(path, "craftaudit", "Fixture42!");
        seedStack(path, "craftaudit", "petal_rose", Rarity::Mythic, 50);
        seedStack(path, "craftaudit", "petal_rose", Rarity::Ultra, kMaxStackCount - 4);
    }, dataDir(), 0);
    CraftClient client;
    const bool loggedIn = h.ready && client.login(h);
    CHECK(loggedIn);
    if (!loggedIn) return;
    const auto rose = content().petalIndex("rose");
    // Processing only five Ultra cannot make room for up to ten new Ultra.
    CHECK_EQ(client.quote(h, {{rose, Rarity::Ultra, 5}, {rose, Rarity::Mythic, 50}}), 0u);
    CHECK_EQ(bag(h).itemCount(Rarity::Mythic, "petal_rose"), 50);
    CHECK_EQ(bag(h).itemCount(Rarity::Ultra, "petal_rose"), kMaxStackCount - 4);

    const auto id = client.quote(h, {{rose, Rarity::Ultra, 14}, {rose, Rarity::Mythic, 50}});
    CHECK(id != 0);
    if (id == 0) return;
    auto& db = GameServerPeer::authorityDb(h.server);
    auto& record = db.progress(db.findUser("craftaudit")->id);
    record.addItem(Rarity::Ultra, "petal_rose", 4);
    client.commit(id);
    CHECK(client.until(h, [&] { return bag(h).itemCount(Rarity::Mythic, "petal_rose") < 5; }));
    CHECK(bag(h).itemCount(Rarity::Ultra, "petal_rose") >= kMaxStackCount - 14);
}
