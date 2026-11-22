#include "test.h"
#include "server_harness.h"
using namespace flix;
using namespace flix::testsupport;
TEST(feedback3748_saving_a_preset_also_persists_its_rotation) {
    Harness h("feedback3748-save", {}, dataDir(), 0);
    CHECK(h.ready); if (!h.ready) return;
    NetClient client;
    CHECK(connectClient(h, client));
    CHECK(loginNew(h, client, "speedkeeper", "password7"));
    client.saveLoadoutPreset(0);
    CHECK(h.stepUntil({&client}, [&] { return !client.profile().presets[0].empty(); }));
    const auto* account = h.server.database().findUser("speedkeeper");
    CHECK(account != nullptr); if (!account) return;
    const auto* record = h.server.database().findProgress(account->id);
    CHECK(record != nullptr); if (!record) return;
    CHECK_EQ(record->extra["loadoutPresetRotation"][loadoutPresetName(0)].asInt(0), 4);
}

TEST(feedback3748_rotation_follows_server_presets_and_autosave_policy) {
    Harness h("feedback3748-rotation", {}, dataDir(), 0);
    CHECK(h.ready); if (!h.ready) return;
    NetClient client;
    CHECK(loginNew(h, client, "speedowner", "password7"));
    const auto* account = h.server.database().findUser("speedowner");
    CHECK(account != nullptr); if (!account) return;
    const auto userId = account->id;
    const auto command = [&](auto send) {
        const auto revision = client.profile().revision;
        send();
        CHECK(h.stepUntil({&client}, [&] { return client.profile().revision > revision; }));
    };
    const auto speed = [&](int preset) {
        const auto* record = h.server.database().findProgress(userId);
        return record ? record->extra["loadoutPresetRotation"][loadoutPresetName(preset)].asInt(0) : -1;
    };
    command([&] { client.saveLoadoutPreset(0, 4); });
    command([&] { client.saveLoadoutPreset(1, 2); });
    CHECK_EQ(speed(0), 4); CHECK_EQ(speed(1), 2);
    // Saving marks the slot active. Disable autosave for the first switch so
    // the outgoing dial does not overwrite the other preset's saved speed.
    client.setPresetAutosave(false);
    CHECK(h.stepUntil({&client}, [&] { return !client.profile().presetAutosave; }));
    command([&] { client.loadLoadoutPreset(0, 4); });
    CHECK_EQ(client.takePresetRotationStep(), 4);
    CHECK_EQ(client.takePresetRotationStep(), 0);
    CHECK_EQ(speed(1), 2);
    client.setPresetAutosave(true);
    CHECK(h.stepUntil({&client}, [&] { return client.profile().presetAutosave; }));
    command([&] { client.loadLoadoutPreset(1, 1); });
    CHECK_EQ(client.takePresetRotationStep(), 2);
    CHECK_EQ(speed(0), 1);
    command([&] { client.loadLoadoutPreset(0, 3); });
    CHECK_EQ(client.takePresetRotationStep(), 1);
    CHECK_EQ(speed(1), 3);
    client.setPresetAutosave(false);
    CHECK(h.stepUntil({&client}, [&] { return !client.profile().presetAutosave; }));
    command([&] { client.loadLoadoutPreset(1, 4); });
    CHECK_EQ(client.takePresetRotationStep(), 3);
    command([&] { client.loadLoadoutPreset(0, 2); });
    CHECK_EQ(client.takePresetRotationStep(), 1);
    CHECK_EQ(speed(0), 1); CHECK_EQ(speed(1), 3);
    // Legacy/never-saved presets have no speed and must retain the UI value.
    command([&] { client.loadLoadoutPreset(2, 2); });
    CHECK_EQ(client.takePresetRotationStep(), 0);
    CHECK_EQ(speed(2), 0);
    h.server.persistAll();
    Database reloaded; std::string error;
    CHECK(reloaded.load(h.files.dbPath, error));
    const auto* saved = reloaded.findProgress(userId);
    CHECK(saved != nullptr);
    if (saved) CHECK_EQ(saved->extra["loadoutPresetRotation"][loadoutPresetName(0)].asInt(), 1);
    client.logout(); h.step(3, {&client});
    NetClient secondDevice;
    CHECK(loginAs(h, secondDevice, "speedowner", "password7"));
    CHECK_EQ(secondDevice.takePresetRotationStep(), 0);
    secondDevice.loadLoadoutPreset(0, 4);
    CHECK(h.stepUntil({&secondDevice}, [&] { return secondDevice.takePresetRotationStep() == 1; }));
    NetClient unrelated;
    CHECK(loginNew(h, unrelated, "otherspeed", "password7"));
    unrelated.loadLoadoutPreset(0, 4);
    h.step(10, {&secondDevice, &unrelated});
    CHECK_EQ(unrelated.takePresetRotationStep(), 0);
}
