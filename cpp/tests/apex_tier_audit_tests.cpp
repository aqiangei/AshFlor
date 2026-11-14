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

enum class Better { Higher, Lower, Either };

struct AuditField {
    const char* name;
    double (*read)(const PetalStats&);
    Better better;
};

#define AUDIT_FIELD(f, better) \
    AuditField{#f, [](const PetalStats& s) { return static_cast<double>(s.f); }, Better::better}

const AuditField kAuditFields[] = {
    AUDIT_FIELD(damage, Higher), AUDIT_FIELD(health, Higher), AUDIT_FIELD(bodyDamage, Higher),
    AUDIT_FIELD(armorReduction, Higher), AUDIT_FIELD(armorPerStack, Higher), AUDIT_FIELD(petalArmor, Higher),
    AUDIT_FIELD(petalEvasion, Higher), AUDIT_FIELD(critDamage, Higher), AUDIT_FIELD(lifesteal, Higher),
    AUDIT_FIELD(reloadMillis, Lower), AUDIT_FIELD(poisonPerSecond, Higher), AUDIT_FIELD(poisonDurationMillis, Either),
    AUDIT_FIELD(noHealDurationMillis, Higher), AUDIT_FIELD(heal, Higher), AUDIT_FIELD(healChargeMillis, Lower),
    AUDIT_FIELD(passiveHealPerSecond, Higher), AUDIT_FIELD(conditionalHealPerSecond, Higher),
    AUDIT_FIELD(totemHealPerSecond, Higher), AUDIT_FIELD(lightningDamagePerHit, Higher),
    AUDIT_FIELD(lightningBounces, Higher), AUDIT_FIELD(lightningRange, Higher), AUDIT_FIELD(batteryCharges, Higher),
    AUDIT_FIELD(flowerHealthBonus, Higher), AUDIT_FIELD(flowerRadiusBonus, Either), AUDIT_FIELD(maxMana, Higher),
    AUDIT_FIELD(mana, Higher), AUDIT_FIELD(manaChargeMillis, Lower), AUDIT_FIELD(passiveManaPerSecond, Higher),
    AUDIT_FIELD(requiredMana, Either), AUDIT_FIELD(petManaCost, Either), AUDIT_FIELD(knockback, Either),
    AUDIT_FIELD(flowerKnockback, Either), AUDIT_FIELD(overhealShield, Higher), AUDIT_FIELD(healBonus, Higher),
    AUDIT_FIELD(reloadCut, Higher), AUDIT_FIELD(lightningRepeatChance, Higher), AUDIT_FIELD(maxLightningRepeats, Higher),
    AUDIT_FIELD(collisionResist, Higher), AUDIT_FIELD(undeadMillis, Either), AUDIT_FIELD(unrevivableMillis, Either),
    AUDIT_FIELD(undeadBodyDamageCut, Either), AUDIT_FIELD(shield, Higher), AUDIT_FIELD(slowFactor, Either),
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
            const bool down = f.better == Better::Higher ? va < vp : f.better == Better::Lower ? va > vp : false;
            if (down) worse[f.name].push_back(id + "(" + number(vp) + "->" + number(va) + ")");
            if (vp != vh) hyperDiff[f.name].push_back(id + "(" + number(vh) + "/" + number(vp) + ")");
        }
    }
    std::printf("AUDIT petals %zu\n", static_cast<std::size_t>(c.petalCount()));
    report("FLAT", flat);
    report("WORSE", worse);
    report("PH_NOT_HYPER", hyperDiff);
    CHECK(c.petalCount() > 0);
}
