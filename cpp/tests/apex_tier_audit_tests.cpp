// Apex tier audit: for every petal and every PetalStats number, whether the
// Apex tier (Rarity::Ascendant) steps above Permanent Hyper wherever the
// ladder below it steps, and whether Permanent Hyper plays exactly as Hyper.

#include "test.h"

#include "server_harness.h"
#include "test_data.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace flix;
using namespace flix::testsupport;

namespace {

// Grows marks positive effects that receive an explicit Apex upgrade.
enum class Better { Grows, Higher, Lower, Either };

struct AuditField {
    const char* name;
    double (*read)(const PetalStats&);
    Better better;
};

#define AUDIT_FIELD(f, better) \
    AuditField{#f, [](const PetalStats& s) { return static_cast<double>(s.f); }, Better::better}

const AuditField kAuditFields[] = {
    AUDIT_FIELD(damage, Grows), AUDIT_FIELD(health, Grows), AUDIT_FIELD(bodyDamage, Grows),
    AUDIT_FIELD(armorReduction, Grows), AUDIT_FIELD(armorPerStack, Grows), AUDIT_FIELD(petalArmor, Grows),
    AUDIT_FIELD(petalEvasion, Higher), AUDIT_FIELD(critDamage, Grows), AUDIT_FIELD(lifesteal, Grows),
    AUDIT_FIELD(reloadMillis, Lower), AUDIT_FIELD(poisonPerSecond, Grows), AUDIT_FIELD(poisonDurationMillis, Either),
    AUDIT_FIELD(noHealDurationMillis, Higher), AUDIT_FIELD(heal, Grows), AUDIT_FIELD(healChargeMillis, Lower),
    AUDIT_FIELD(passiveHealPerSecond, Grows), AUDIT_FIELD(conditionalHealPerSecond, Grows),
    AUDIT_FIELD(totemHealPerSecond, Grows), AUDIT_FIELD(lightningDamagePerHit, Grows),
    AUDIT_FIELD(lightningBounces, Higher), AUDIT_FIELD(lightningRange, Higher), AUDIT_FIELD(batteryCharges, Higher),
    AUDIT_FIELD(flowerHealthBonus, Grows), AUDIT_FIELD(flowerRadiusBonus, Either), AUDIT_FIELD(maxMana, Grows),
    AUDIT_FIELD(mana, Grows), AUDIT_FIELD(manaChargeMillis, Lower), AUDIT_FIELD(passiveManaPerSecond, Grows),
    AUDIT_FIELD(requiredMana, Either), AUDIT_FIELD(petManaCost, Either), AUDIT_FIELD(knockback, Either),
    AUDIT_FIELD(flowerKnockback, Either), AUDIT_FIELD(overhealShield, Grows), AUDIT_FIELD(healBonus, Grows),
    AUDIT_FIELD(reloadCut, Grows), AUDIT_FIELD(lightningRepeatChance, Higher), AUDIT_FIELD(maxLightningRepeats, Higher),
    AUDIT_FIELD(collisionResist, Higher), AUDIT_FIELD(undeadMillis, Either), AUDIT_FIELD(unrevivableMillis, Either),
    AUDIT_FIELD(undeadBodyDamageCut, Either), AUDIT_FIELD(shield, Grows), AUDIT_FIELD(slowFactor, Either),
    AUDIT_FIELD(slowDurationMillis, Higher), AUDIT_FIELD(webRadius, Higher),
    AUDIT_FIELD(spongeDamageDurationMillis, Either), AUDIT_FIELD(attractionForce, Either), AUDIT_FIELD(radius, Either),
    AUDIT_FIELD(size, Either), AUDIT_FIELD(damageIntervalMillis, Lower), AUDIT_FIELD(count, Higher),
};

std::string number(double v) {
    char text[48];
    std::snprintf(text, sizeof text, "%.6g", v);
    return text;
}

void report(const char* kind, const std::map<std::string, std::vector<std::string>>& rows) {
    for (const auto& [field, ids] : rows) {
        std::string line;
        for (std::size_t i = 0; i < ids.size() && i < 60; ++i) line += " " + ids[i];
        if (ids.size() > 60) line += " ...";
        std::printf("AUDIT %s %s n=%zu:%s\n", kind, field.c_str(), ids.size(), line.c_str());
    }
}

} // namespace

TEST(apex_tier_audit_of_every_petal_stat) {
    const auto& c = shippedContent();
    std::map<std::string, std::vector<std::string>> flat, worse, hyperDiff;
    for (std::uint16_t i = 0; i < c.petalCount(); ++i) {
        const std::string& id = c.petal(i).id;
        const PetalStats u = c.petalStats(i, Rarity::Unique);
        const PetalStats h = c.petalStats(i, Rarity::Apex);
        const PetalStats p = c.petalStats(i, Rarity::PermanentHyper);
        const PetalStats a = c.petalStats(i, Rarity::Ascendant);
        for (const AuditField& f : kAuditFields) {
            const double vu = f.read(u), vh = f.read(h), vp = f.read(p), va = f.read(a);
            if (vh != vu && va == vp)
                flat[f.name].push_back(id + "(" + number(vu) + "/" + number(vh) + "/" + number(va) + ")");
            const bool missedUpgrade = f.better == Better::Grows && vp > 0 && va == vp;
            const bool dropped = (f.better == Better::Higher || f.better == Better::Grows) && va < vp;
            const bool slower = f.better == Better::Lower && va > vp;
            if (missedUpgrade || dropped || slower) worse[f.name].push_back(id + "(" + number(vp) + "->" + number(va) + ")");
            if (vp != vh) hyperDiff[f.name].push_back(id + "(" + number(vh) + "/" + number(vp) + ")");
        }
    }
    std::printf("AUDIT petals %zu\n", static_cast<std::size_t>(c.petalCount()));
    report("FLAT", flat);
    report("WORSE", worse);
    report("PH_NOT_HYPER", hyperDiff);
    CHECK(worse.empty());
    CHECK(c.petalCount() > 0);
}

// The Apex step raises what helps the holder. Glitch's damage is negative --
// it heals what it touches -- so it keeps its Permanent Hyper figure.
TEST(the_apex_step_does_not_deepen_a_downside) {
    const auto& registry = shippedContent();
    const auto glitch = registry.petalIndex("glitch");
    CHECK(glitch < registry.petalCount());
    if (glitch >= registry.petalCount()) return;
    const PetalStats hyper = registry.petalStats(glitch, Rarity::PermanentHyper);
    const PetalStats apex = registry.petalStats(glitch, Rarity::Ascendant);
    CHECK(hyper.damage < 0.0);
    CHECK_NEAR(apex.damage, hyper.damage, 1e-9);
    CHECK(apex.health > hyper.health);
}

TEST(apex_reload_bonuses_preserve_the_magic_ratio_and_stack_per_leaf) {
    const auto& registry = shippedContent();
    CHECK_NEAR(registry.petalStats(registry.petalIndex("golden_leaf"), Rarity::Ascendant).reloadCut,
               0.275, 1e-9);
    const auto leaf = registry.petalIndex("golden_leaf");
    const auto magic = registry.petalIndex("magic_golden_leaf");
    for (auto tier : {Rarity::Common, Rarity::Super, Rarity::Unique, Rarity::Apex,
                      Rarity::PermanentHyper, Rarity::Ascendant}) {
        const auto ordinary = registry.petalStats(leaf, tier).reloadCut;
        const auto equipped = [&](int slot) {
            EquippedPetal item;
            if (slot < 2) {
                item.configIndex = magic;
                item.rarity = item.reloadRarity = tier;
            }
            return item;
        };
        const double perLeaf = ordinary * kMagicGoldenLeafReloadShare;
        CHECK_NEAR(registry.petalStats(magic, tier).reloadCut, perLeaf, 1e-9);
        CHECK_NEAR(barReloadCut(registry, equipped), 1 - (1-perLeaf)*(1-perLeaf), 1e-9);
        CHECK_NEAR(barReloadCut(registry, equipped, nullptr, true),
                   1 - (1-ordinary)*(1-ordinary), 1e-9);
    }
}
