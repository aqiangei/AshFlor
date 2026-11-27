// The loadout keys that act on the whole bar at once: R swaps the two rows,
// and K/L + a number save and load a whole loadout.
//
// End to end on a real server. The accounts are seeded through the Database
// itself, so a preset written by the seed is also a preset that made it
// through the JSON file and back -- which is the path a saved loadout takes
// across a restart.

#include "test.h"

#include <functional>
#include <string>

#include "client/net_client.h"
#include "server/db.h"
#include "server/game_server.h"
#include "server_harness.h"
#include "shared/game/config.h"

using namespace flix;
using namespace flix::testsupport;

namespace {

constexpr const char* kName = "keeper";
constexpr const char* kPassword = "password7";

StoredItem petal(const char* id, Rarity rarity) {
    StoredItem item;
    item.petalType = id;
    item.rarity = rarity;
    return item;
}

std::string bagKey(const char* id) { return std::string("petal_") + id; }

/// One account, whose record `fill` writes before the server ever opens it.
void seedKeeper(const std::string& path, const std::function<void(PlayerRecord&)>& fill) {
    Database db;
    std::string error;
    db.load(path, error);
    db.setPasswordCost(4);
    CreateResult created = db.createUser(kName, kPassword);
    if (!created.ok()) return;
    PlayerRecord& record = db.progress(created.account->id);
    record.loadout.assign(kLoadoutSlots, std::nullopt);
    fill(record);
    db.markDirty();
    db.save();
}

bool login(Harness& h, NetClient& client) {
    if (!connectClient(h, client)) return false;
    client.requestLogin(kName, kPassword);
    return h.stepUntil({&client}, [&] {
        return client.status() == NetClient::Status::LoggedIn &&
               client.profile().loadout.size() == static_cast<std::size_t>(kLoadoutSlots);
    });
}

bool slotIs(const NetClient& client, int slot, const char* id, Rarity rarity) {
    const auto& loadout = client.profile().loadout;
    if (slot < 0 || static_cast<std::size_t>(slot) >= loadout.size()) return false;
    return loadout[static_cast<std::size_t>(slot)].petalIndex == content().petalIndex(id) &&
           loadout[static_cast<std::size_t>(slot)].rarity == rarity;
}

bool slotEmpty(const NetClient& client, int slot) {
    const auto& loadout = client.profile().loadout;
    return static_cast<std::size_t>(slot) < loadout.size() &&
           loadout[static_cast<std::size_t>(slot)].empty();
}

const PlayerRecord* keeperRecord(Harness& h) {
    const Account* account = h.server.database().findUser(kName);
    return account == nullptr ? nullptr : h.server.database().findProgress(account->id);
}

} // namespace

TEST(loadout_preset_names_follow_the_keys_that_load_them) {
    CHECK_EQ(loadoutPresetName(0), std::string("K1"));
    CHECK_EQ(loadoutPresetName(8), std::string("K9"));
    CHECK_EQ(loadoutPresetName(9), std::string("K0"));
    CHECK_EQ(loadoutPresetName(10), std::string("L1"));
    CHECK_EQ(loadoutPresetName(19), std::string("L0"));
    CHECK(loadoutPresetName(20).empty());
    CHECK(loadoutPresetName(-1).empty());
}

TEST(r_swaps_unlocked_nonempty_reserve_slots_and_keeps_the_other_slots) {
    Harness h("preset-row-swap", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            record.loadout[0] = petal("rose", Rarity::Rare);
            record.loadout[3] = petal("basic", Rarity::Common);
            record.loadout[13] = petal("light", Rarity::Epic);
            record.loadout[19] = petal("stinger", Rarity::Common);
        });
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(login(h, client));
    client.swapLoadoutRows();
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 3, "light", Rarity::Epic); }));

    CHECK(slotIs(client, 0, "rose", Rarity::Rare));
    CHECK(slotEmpty(client, 10));
    CHECK(slotIs(client, 13, "basic", Rarity::Common));
    CHECK(slotEmpty(client, 9));
    CHECK(slotIs(client, 19, "stinger", Rarity::Common));
}

TEST(a_clump_swapped_onto_the_bar_reloads_instead_of_arriving_whole) {
    // Sand is a clump: each grain runs its own reload, and the swap used to
    // hand all four back on the very tick it landed in the top row.
    Harness h("preset-row-swap-reload", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            record.loadout[0] = petal("rose", Rarity::Common);
            record.loadout[10] = petal("sand", Rarity::Common);
        });
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(login(h, client));
    client.joinGame(1280, 720, {}, kName);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));

    World& world = h.server.world();
    Entity body = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount> bodies{world};
    bodies.each([&](Entity e, PlayerTag&, PlayerAccount&) { body = e; });
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;
    const auto onRing = [&](const char* id) {
        int n = 0;
        for (const Entity spawned : world.get<Loadout>(body).spawned) {
            const PetalInstance* instance = world.tryGet<PetalInstance>(spawned);
            if (instance && instance->slot == 0 && instance->configIndex == content().petalIndex(id)) ++n;
        }
        return n;
    };
    CHECK(h.stepUntil({&client}, [&] { return onRing("rose") == 1; }));

    client.swapLoadoutRows();
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 0, "sand", Rarity::Common); }));
    CHECK_EQ(onRing("rose"), 0);
    CHECK_EQ(onRing("sand"), 0);
    // The bar is told, so the slot sweeps its reload rather than sitting full.
    CHECK(h.stepUntil({&client}, [&] {
        return client.view().self().slotReloadRemainingMillis[0] > 0.0;
    }, 5));
    CHECK_EQ(onRing("sand"), 0);

    CHECK(h.stepUntil({&client}, [&] { return onRing("sand") == 4; }));
}

TEST(preset_moves_a_shovel_between_active_slots_without_resetting_its_timers) {
    Harness h("preset-shovel-timers", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            record.loadout[0] = petal("shovel", Rarity::Super);
            record.loadout[2] = petal("rose", Rarity::Common);
            record.loadoutPresets["K1"] = record.loadout;
            auto moved = record.loadout;
            std::swap(moved[0], moved[2]);
            record.loadoutPresets["K2"] = moved;
        });
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    if (!login(h, client)) { CHECK(false); return; }
    client.joinGame(1280, 720, {}, kName);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    auto& world = h.server.world();
    Entity body = NULL_ENTITY;
    Query<PlayerTag, PlayerAccount>{world}.each(
        [&](Entity e, PlayerTag&, PlayerAccount&) { body = e; });
    CHECK(body != NULL_ENTITY);
    if (body == NULL_ENTITY) return;

    const auto loadout = [&]() -> const Loadout& { return world.get<Loadout>(body); };
    const auto findShovel = [&] {
        for (const Entity e : loadout().spawned) {
            const auto* instance = world.tryGet<PetalInstance>(e);
            if (instance && instance->configIndex == content().petalIndex("shovel")) return e;
        }
        return NULL_ENTITY;
    };

    CHECK(h.stepUntil({&client}, [&] { return loadout().slots[0].broken; }));
    const double initialReload = loadout().slots[0].reloadReadyAtMillis;
    client.loadLoadoutPreset(1);
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 2, "shovel", Rarity::Super); }));
    CHECK(loadout().slots[2].broken);
    CHECK_NEAR(loadout().slots[2].reloadReadyAtMillis, initialReload, 1e-6);

    CHECK(h.stepUntil({&client}, [&] { return findShovel() != NULL_ENTITY; }));
    const Entity live = findShovel();
    if (live == NULL_ENTITY) return;
    const double charge = world.get<PetalInstance>(live).nextActionMillis;
    client.loadLoadoutPreset(0);
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 0, "shovel", Rarity::Super); }));
    CHECK(findShovel() == live);
    if (!world.isAlive(live)) return;
    CHECK_EQ(world.get<PetalInstance>(live).slot, 0);
    CHECK_NEAR(world.get<PetalInstance>(live).nextActionMillis, charge, 1e-6);

    net::InputFrame input{};
    input.flags = net::InputDefend;
    bool burrowed = false;
    for (int i = 0; i < 100 && !burrowed; ++i) {
        ++input.sequence;
        CHECK(client.sendInput(input));
        h.step(1, {&client});
        const auto* burrow = world.tryGet<BurrowState>(body);
        burrowed = burrow && burrow->active;
    }
    CHECK(burrowed);
    ++input.sequence;
    input.flags = 0;
    CHECK(client.sendInput(input));
    CHECK(loadout().slots[0].broken);
    const double spentReload = loadout().slots[0].reloadReadyAtMillis;
    client.loadLoadoutPreset(1);
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 2, "shovel", Rarity::Super); }));
    CHECK(loadout().slots[2].broken);
    CHECK_NEAR(loadout().slots[2].reloadReadyAtMillis, spentReload, 1e-6);
    CHECK(findShovel() == NULL_ENTITY);
}

TEST(a_saved_loadout_loads_back_out_of_the_bag_and_the_bar_alike) {
    Harness h("preset-round-trip", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            record.loadout[0] = petal("rose", Rarity::Rare);
            record.loadout[1] = petal("basic", Rarity::Common);
            record.loadout[12] = petal("light", Rarity::Epic);
        });
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(login(h, client));
    client.saveLoadoutPreset(0);
    CHECK(h.stepUntil({&client}, [&] {
        return sawText(client, "Saved loadout K1.") || sawText(client, "已保存配装 K1。");
    }));
    // The profile carries it back, which is what the K row is drawn from.
    const std::vector<Profile::Slot>& k1 = client.profile().presets[0];
    CHECK_EQ(k1.size(), static_cast<std::size_t>(kLoadoutSlots));
    if (k1.size() == static_cast<std::size_t>(kLoadoutSlots)) {
        CHECK_EQ(k1[0].petalIndex, content().petalIndex("rose"));
        CHECK(k1[0].rarity == Rarity::Rare);
        CHECK_EQ(k1[12].petalIndex, content().petalIndex("light"));
        CHECK(k1[5].empty());
    }
    CHECK(client.profile().presets[1].empty());

    // Manual save marks K1 active; pause autosave so the scramble below does
    // not overwrite the snapshot we are about to restore.
    client.setPresetAutosave(false);
    CHECK(h.stepUntil({&client}, [&] { return !client.profile().presetAutosave; }));

    // Scramble within the base five unlocked slots -- slot 5+ needs the
    // loadout-slot talent and is rejected on a fresh account.
    client.swapLoadoutSlots(0, 3);
    client.setLoadoutSlot(1, kNoPetal, Rarity::Common);
    client.swapLoadoutSlots(12, 2);
    CHECK(h.stepUntil({&client}, [&] {
        return slotIs(client, 3, "rose", Rarity::Rare) && slotEmpty(client, 1) &&
               slotIs(client, 2, "light", Rarity::Epic);
    }));
    CHECK_EQ(client.profile().stackCount(content().petalIndex("basic"), Rarity::Common), 1u);

    client.loadLoadoutPreset(0);
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 0, "rose", Rarity::Rare); }));
    CHECK(slotIs(client, 1, "basic", Rarity::Common));
    CHECK(slotIs(client, 12, "light", Rarity::Epic));
    CHECK(slotEmpty(client, 3));
    CHECK(slotEmpty(client, 2));
    // Every petal is accounted for: the basic came out of the bag, and
    // nothing the load moved was left behind in it.
    CHECK_EQ(client.profile().stackCount(content().petalIndex("basic"), Rarity::Common), 0u);
    CHECK_EQ(client.profile().stackCount(content().petalIndex("rose"), Rarity::Rare), 0u);
    CHECK(sawText(client, "Loaded loadout K1.") || sawText(client, "已切换配装 K1。"));

    const PlayerRecord* record = keeperRecord(h);
    CHECK(record != nullptr);
    if (record != nullptr) CHECK(record->loadoutPresets.count("K1") == 1);
}

TEST(a_petal_the_account_no_longer_has_comes_back_a_rarity_down) {
    // L3, saved when the account was richer than it is now. The bag holds one
    // epic and one rare rose; the bar wears a common basic.
    Harness h("preset-step-down", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            std::vector<std::optional<StoredItem>> preset(kLoadoutSlots);
            preset[0] = petal("rose", Rarity::Legendary);   // gone: steps down
            preset[1] = petal("rose", Rarity::Epic);        // still owned exactly
            preset[2] = petal("stinger", Rarity::Common);   // never owned at all
            preset[3] = petal("basic", Rarity::Mythic);     // only the bar's common
            record.loadoutPresets["L3"] = preset;
            record.addItem(Rarity::Epic, bagKey("rose"), 1);
            record.addItem(Rarity::Rare, bagKey("rose"), 1);
            record.addItem(Rarity::Common, bagKey("light"), 1);
            record.loadout[7] = petal("basic", Rarity::Common);
        });
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(login(h, client));
    client.joinGame(1280, 720);
    CHECK(h.stepUntil({&client}, [&] { return client.view().self().netId != 0; }));

    client.loadLoadoutPreset(kLoadoutPresetsPerBank + 2);   // L3
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 1, "rose", Rarity::Epic); }));
    // The exact epic went to the slot saved with an epic, even though the
    // legendary's slot comes first on the bar and had to step down past it.
    CHECK(slotIs(client, 0, "rose", Rarity::Rare));
    CHECK(slotEmpty(client, 2));
    // A petal equipped elsewhere on the bar is part of what a slot draws from.
    CHECK(slotIs(client, 3, "basic", Rarity::Common));
    CHECK(slotEmpty(client, 7));
    CHECK_EQ(client.profile().stackCount(content().petalIndex("rose"), Rarity::Epic), 0u);
    CHECK_EQ(client.profile().stackCount(content().petalIndex("rose"), Rarity::Rare), 0u);
    CHECK_EQ(client.profile().stackCount(content().petalIndex("basic"), Rarity::Common), 0u);
    CHECK(h.stepUntil({&client}, [&] {
        return sawText(client, "Loaded loadout L3 (2 petals at a lower rarity, 1 petal not found).") ||
               sawText(client, "已切换配装 L3");
    }));
    // An edit in another slot must not overwrite either a downgraded
    // petal or an unavailable one, on switch-out or after reconnecting.
    client.setLoadoutSlot(4, content().petalIndex("light"), Rarity::Common);
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 4, "light", Rarity::Common); }));
    client.loadLoadoutPreset(0);
    CHECK(h.stepUntil({&client}, [&] { return slotEmpty(client, 4); }));
    client.disconnect();
    h.step(6, {});
    NetClient reconnected;
    CHECK(login(h, reconnected));
    const auto& saved = reconnected.profile().presets[kLoadoutPresetsPerBank + 2];
    CHECK_EQ(saved.size(), static_cast<std::size_t>(kLoadoutSlots));
    if (saved.size() != static_cast<std::size_t>(kLoadoutSlots)) return;
    CHECK(saved[0].rarity == Rarity::Legendary);
    CHECK_EQ(saved[2].petalIndex, content().petalIndex("stinger"));
    CHECK(saved[3].rarity == Rarity::Mythic);
    CHECK_EQ(saved[4].petalIndex, content().petalIndex("light"));
    reconnected.loadLoadoutPreset(kLoadoutPresetsPerBank + 2);
    CHECK(h.stepUntil({&reconnected}, [&] { return slotIs(reconnected, 4, "light", Rarity::Common); }));
    CHECK(slotIs(reconnected, 0, "rose", Rarity::Rare));
    CHECK(slotEmpty(reconnected, 2));
}

TEST(loading_a_preset_that_was_never_saved_empties_the_loadout) {
    Harness h("preset-empty", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            record.loadout[0] = petal("rose", Rarity::Rare);
            record.loadout[14] = petal("basic", Rarity::Common);
        });
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(login(h, client));
    client.loadLoadoutPreset(kLoadoutPresetsPerBank + 9);   // L0, never saved
    CHECK(h.stepUntil({&client}, [&] { return slotEmpty(client, 0); }));
    for (int i = 0; i < kLoadoutSlots; ++i) CHECK(slotEmpty(client, i));
    // Both petals went back into the bag rather than vanishing.
    CHECK_EQ(client.profile().stackCount(content().petalIndex("rose"), Rarity::Rare), 1u);
    CHECK_EQ(client.profile().stackCount(content().petalIndex("basic"), Rarity::Common), 1u);
    CHECK(h.stepUntil({&client}, [&] { return sawText(client, "Loaded loadout L0.") || sawText(client, "已切换配装 L0"); }));
}

TEST(loadout_preset_full_inventory_never_discards_equipped_gear) {
    Harness h("preset-full-stack", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            record.loadout[0] = petal("rose", Rarity::Rare);
            record.setItemCount(Rarity::Rare, bagKey("rose"), kMaxStackCount);
            record.loadoutPresets["K1"] = record.loadout;
        });
    }, dataDir(), 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(login(h, client));
    client.loadLoadoutPreset(0);
    CHECK(h.stepUntil({&client}, [&] { return sawText(client, "Loaded loadout K1.") || sawText(client, "已切换配装 K1"); }));
    CHECK_EQ(keeperRecord(h)->itemCount(Rarity::Rare, bagKey("rose")), kMaxStackCount);
    client.setLoadoutSlot(0, kNoPetal, Rarity::Common);
    CHECK(h.stepUntil({&client}, [&] { return sawText(client, "库存叠放已满，无法卸下该花瓣。"); }));
    client.loadLoadoutPreset(1);
    CHECK(h.stepUntil({&client}, [&] { return sawText(client, "库存叠放已满，无法切换配装。"); }));
    CHECK(slotIs(client, 0, "rose", Rarity::Rare));
    CHECK_EQ(keeperRecord(h)->itemCount(Rarity::Rare, bagKey("rose")), kMaxStackCount);
    CHECK_EQ(keeperRecord(h)->extra["activeLoadoutPreset"].asInt(-1), 0);
}

TEST(loadout_preserves_current_prestige_and_restores_saved_talents_when_affordable) {
    const double cell = kTileSize;
    const auto map = fixtureMap(24, 24,
        fixtureDoor("meadow", "Meadow", cell * 2, cell * 2, cell * 4, cell * 4, true, 0.0), "");
    const std::string dir = stageDataDir("preset-prestige-budget", {{"meadow", map}});
    if (dir.empty()) { CHECK(false); return; }
    struct Cleanup {
        const std::string& dir;
        ~Cleanup() { removeDataDir(dir); }
    } cleanup{dir};
    if (!writeText(dir + "/maps.json", R"({"maps":[{"file":"meadow.tmj","prestige":true}]})")) {
        CHECK(false);
        return;
    }
    Harness h("preset-prestige-budget", [](const std::string& path) {
        seedKeeper(path, [](PlayerRecord& record) {
            for (int level = 1; level < 290; ++level) record.totalXp += xpForNextLevel(level);
            // Leave exactly two ordinary points after buying both prestige tiers.
            record.skills.set(SkillId::Reload, skillTierCount(SkillId::Reload) - 1);
            record.skills.set(SkillId::Medic, skillTierCount(SkillId::Medic) - 1);
            record.skills.set(SkillId::Luck, 0);
            CHECK_EQ(record.talentPoints(), 302);
            record.setItemCount(Rarity::Common, bagKey("rose"), 1);
            record.setItemCount(Rarity::Common, bagKey("basic"), 1);
        });
    }, dir, 0);
    if (!h.ready) { CHECK(false); return; }

    NetClient client;
    CHECK(login(h, client));
    const auto level = [&](SkillId skill) { return client.profile().skills.florrLevel(skill); };
    const auto equip = [&](const char* petalId) {
        client.setLoadoutSlot(0, content().petalIndex(petalId), Rarity::Common);
        return h.stepUntil({&client}, [&] { return slotIs(client, 0, petalId, Rarity::Common); });
    };
    constexpr int l1 = kLoadoutPresetsPerBank;
    constexpr int l2 = l1 + 1;

    // Save L2 with Prestige I and seven points spent on Health.
    client.requestUpgradeSkill(SkillId::Prestige, 0);
    CHECK(h.stepUntil({&client}, [&] { return level(SkillId::Prestige) == 1; }));
    for (int tier = 0; tier < 2; ++tier) {
        client.requestUpgradeSkill(SkillId::Health, tier);
        CHECK(h.stepUntil({&client}, [&] { return level(SkillId::Health) == tier + 1; }));
    }
    CHECK(equip("rose"));
    client.saveLoadoutPreset(l2);
    CHECK(h.stepUntil({&client}, [&] { return !client.profile().presets[l2].empty(); }));

    // Leave L2 before editing so autosave keeps its allocation intact.
    client.loadLoadoutPreset(l1);
    CHECK(h.stepUntil({&client}, [&] { return slotEmpty(client, 0); }));

    // Prestige II leaves only two ordinary points. Save L1 and enter its map.
    client.requestDowngradeSkill(SkillId::Health, 0);
    CHECK(h.stepUntil({&client}, [&] { return level(SkillId::Health) == 0; }));
    client.requestUpgradeSkill(SkillId::Prestige, 1);
    CHECK(h.stepUntil({&client}, [&] { return level(SkillId::Prestige) == 2; }));
    client.requestUpgradeSkill(SkillId::Damage, 0);
    CHECK(h.stepUntil({&client}, [&] { return level(SkillId::Damage) == 1; }));
    CHECK(equip("basic"));
    client.saveLoadoutPreset(l1);
    CHECK(h.stepUntil({&client}, [&] { return !client.profile().presets[l1].empty(); }));
    client.loadLoadoutPreset(l1);
    client.joinGame(1280, 720, "meadow", kName, true);
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::Playing; }));
    const Realm prestigeRealm = client.view().realm();
    const auto* mapData = h.server.worldMaps().forRealm(prestigeRealm);
    CHECK(mapData && mapData->prestigeLevel() == 2);

    client.loadLoadoutPreset(l2);
    CHECK(h.stepUntil({&client}, [&] { return slotIs(client, 0, "rose", Rarity::Common); }));
    CHECK(h.stepUntil({&client}, [&] {
        return sawText(client, "已保留当前天赋") ||
               sawText(client, "Current talents kept: the saved talents cost too many points.");
    }));
    CHECK(client.status() == NetClient::Status::Playing);
    CHECK(client.view().realm() == prestigeRealm);
    CHECK_EQ(level(SkillId::Prestige), 2);
    CHECK_EQ(level(SkillId::Damage), 1);
    CHECK_EQ(level(SkillId::Health), 0);

    client.leaveGame();
    CHECK(h.stepUntil({&client}, [&] { return client.status() == NetClient::Status::LoggedIn; }));
    client.requestDowngradeSkill(SkillId::Prestige, 1);
    CHECK(h.stepUntil({&client}, [&] { return level(SkillId::Prestige) == 1; }));
    client.loadLoadoutPreset(l2);
    CHECK(h.stepUntil({&client}, [&] { return level(SkillId::Health) == 2; }));
    CHECK_EQ(level(SkillId::Damage), 0);
    CHECK_EQ(level(SkillId::Prestige), 1);
}
