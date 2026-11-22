#include "test.h"
#include "test_data.h"
#include "client/ui/menu_widgets.h"
#include "client/ui/menus.h"
#include "client/ui/i18n.h"
#include "client/ui/content_i18n.h"
#include "shared/game/config.h"
using namespace flix;
TEST(laser_and_void_tooltips_explain_the_available_effects) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    ui::setLanguage(ui::Language::En);
    const auto laser = content().petalIndex("laser");
    const auto hasLine = [&](Rarity rarity, const char* text) {
        const auto lines = ui::petalTooltipLines(laser, rarity, {}, 1.0);
        for (const auto& line : lines) if (line.text.find(text) != std::string::npos) return true;
        return false;
    };
    CHECK(hasLine(Rarity::Unique, "fires a continuous lightning beam."));
    CHECK(!hasLine(Rarity::Unique, "390 units"));
    CHECK(hasLine(Rarity::PermanentHyper, "390 units"));
    CHECK(std::string(ui::petalDescTr("void_singularity", "")).find("33%") != std::string::npos);
    CHECK(std::string(ui::petalDescTr("void_void_mark", "")).find("the Void") != std::string::npos);
    ui::setLanguage(ui::Language::Zh);
    CHECK(std::string(ui::petalDescTr("void_gamma_ray", "")).find("攻击") != std::string::npos);
}

TEST(fang_and_tooth_tooltips_report_their_mana_refunds) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    ui::setLanguage(ui::Language::En);
    const auto hasLine = [&](const char* petal, Rarity rarity, const char* expected) {
        const auto lines = ui::petalTooltipLines(content().petalIndex(petal), rarity, {}, 1.0);
        for (const auto& line : lines) if (line.text == expected) return true;
        return false;
    };
    CHECK(hasLine("magic_fang", Rarity::Common, "Mana per damaging wild mob / dummy bite: 1.2"));
    CHECK(hasLine("magic_fang", Rarity::Rare, "Mana per damaging wild mob / dummy bite: 4.8"));
    CHECK(hasLine("magic_tooth", Rarity::Common, "Mana restored at most once per reload: 0.3"));
    CHECK(hasLine("jurassic_fossil_egg", Rarity::Common, "Summon contact damage: 0"));
}

TEST(magic_blood_stinger_tooltip_matches_mana_and_contact_damage) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    const auto index = content().petalIndex("magic_blood_stinger");
    CHECK(index != kInvalidIndex);
    const auto& config = content().petal(index);
    CHECK_NEAR(config.contactSelfDamage, 0.0, 1e-9);
    CHECK(config.requiredMana > 0.0);
    CHECK(content().petalStats(index, Rarity::Common).damage > 0.0);
    ui::setLanguage(ui::Language::En);
    CHECK_EQ(std::string(ui::petalDescTr(config.id, "")), "Costs mana to form and deals contact damage.");
    ui::setLanguage(ui::Language::Zh);
    CHECK_EQ(std::string(ui::petalDescTr(config.id, "")), "生成时消耗法力，碰撞时造成伤害。");
}

TEST(mimic_tooltip_shows_eternal_cpp_stats_and_copied_reload){
    std::string error;CHECK(loadContent(testsupport::dataDir(),error));ui::setLanguage(ui::Language::Zh);
    const auto mimic=content().petalIndex("mimic"),cpp=content().petalIndex("c++");
    CHECK(mimic!=kInvalidIndex);CHECK(cpp!=kInvalidIndex);
    for(const auto sourceRarity:{Rarity::Common,Rarity::Unique}){
        auto slotAt=[&](int i){return i==0?EquippedPetal{cpp,sourceRarity,sourceRarity}:EquippedPetal{mimic,Rarity::Unique,Rarity::Unique};};
        const auto effective=resolveEquippedPetal(content(),1,slotAt);
        CHECK_EQ(effective.configIndex,cpp);CHECK(effective.rarity==Rarity::Unique);
        const auto lines=ui::equippedPetalTooltipLines(mimic,effective,{},1.0);
        const auto stats=equippedPetalStats(content(),effective);
        CHECK(stats.damage>0);CHECK(stats.health>0);
        const auto base=content().petalStats(cpp,Rarity::Unique);
        const auto expected=ui::petalTooltipLines(cpp,Rarity::Unique,{},stats.reloadMillis/base.reloadMillis);
        CHECK_EQ(lines.size(),expected.size());CHECK_EQ(lines.front().text,"拟态");
        for(std::size_t i=1;i<lines.size()&&i<expected.size();++i){CHECK_EQ(lines[i].text,expected[i].text);CHECK_EQ(lines[i].altText,expected[i].altText);}
        CHECK_EQ(lines.front().badges.size(),expected.front().badges.size());
        for(std::size_t i=0;i<lines.front().badges.size();++i)CHECK_EQ(lines.front().badges[i].text,expected.front().badges[i].text);
        ui::setLanguage(ui::Language::En);
        CHECK_EQ(ui::equippedPetalTooltipLines(mimic,effective,{},1.0).front().text,"Mimic");
        ui::setLanguage(ui::Language::Zh);
    }
}
TEST(ant_and_beetle_tooltips_match_the_buffed_summon_stats){
    std::string error;CHECK(loadContent(testsupport::dataDir(),error));ui::setLanguage(ui::Language::En);
    for(const auto* id:{"egg","beetle_egg"})for(int tier=0;tier<kRarityCount;++tier){
        const auto rarity=clampRarity(tier);const auto index=content().petalIndex(id);const auto& config=content().petal(index);
        const auto lines=ui::petalTooltipLines(index,rarity,{},1.0);bool health=false,damage=false;
        for(const auto& line:lines){health|=line.text.find("Summon health: ")==0;damage|=line.text.find("Summon contact damage: ")==0;}
        CHECK(health);CHECK(damage);CHECK_NEAR(eggSummonCombatScale(config),2.0,1e-9);
    }
}

TEST(hammer_tooltip_matches_wild_lightning_recoil_and_reload) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    ui::setLanguage(ui::Language::En);
    for (const auto rarity : {Rarity::Unique, Rarity::Apex}) {
        const bool hyper = rarity == Rarity::Apex;
        const auto index = content().petalIndex(hyper ? "mjolnir" : "timed_mjolnir");
        const auto stats = content().petalStats(index, rarity);
        CHECK_NEAR(stats.lightningDamagePerHit * stats.wildLightningScale,
                   (hyper ? 80.0 : 70.0) * petalStrengthScale(rarity), 1e-6);
        CHECK_NEAR(stats.wildRecoilScale, hyper ? 0.35 : 0.5, 1e-9);
        CHECK_NEAR(stats.reloadMillis, hyper ? 4000.0 : 5000.0, 1e-6);
        CHECK_EQ(stats.lightningBounces, 0);
        const auto lines = ui::petalTooltipLines(index, rarity, {}, 1.0);
        bool lightning = false, recoil = false;
        for (const auto& line : lines) {
            lightning |= line.text.find("Lightning per hit on wild mobs: ") == 0;
            recoil |= line.text.find(hyper ? "65%" : "50%") != std::string::npos;
        }
        CHECK(lightning);
        CHECK(recoil);
    }
}

TEST(cpp_uses_42_damage_28_health_and_2500ms_reload_on_every_ladder_tier){
    std::string error;CHECK(loadContent(testsupport::dataDir(),error));ui::setLanguage(ui::Language::En);
    const auto cpp=content().petalIndex("c++"),saw=content().petalIndex("sawblade");
    for(int tier=0;tier<kLadderRarityCount;++tier){const auto rarity=static_cast<Rarity>(tier);
        const auto stats=content().petalStats(cpp,rarity),blade=content().petalStats(saw,rarity);
        const double damageScale = rarity == Rarity::Apex
            ? 2.0 * petalStrengthScale(Rarity::Unique) : petalStrengthScale(rarity);
        CHECK_NEAR(stats.damage,42 * damageScale,1e-6);
        CHECK_NEAR(stats.health,28*petalStrengthScale(rarity),1e-6);CHECK_NEAR(stats.reloadMillis,2500,1e-8);
        CHECK_NEAR(stats.health/blade.health,28.0/25,1e-9);
        const auto lines=ui::petalTooltipLines(cpp,rarity,{},1);bool reload=false;
        for(const auto& badge:lines.front().badges)reload|=badge.icon==ui::TooltipIcon::Reload && badge.text=="1.3s";CHECK(reload);
        const auto mimic=content().petalIndex("mimic");
        const auto copied=ui::equippedPetalTooltipLines(mimic,EquippedPetal{cpp,rarity,rarity},{},1);
        CHECK_EQ(copied.size(),lines.size());for(std::size_t i=1;i<copied.size();++i)CHECK_EQ(copied[i].altText,lines[i].altText);
    }
}

TEST(goggles_reduce_visible_width_by_twenty_percent_without_stacking){
    std::string error;CHECK(loadContent(testsupport::dataDir(),error));Profile p;p.loadout.resize(kLoadoutSlots);
    const double normal=loadoutCameraZoom(p,content());const auto goggles=content().petalIndex("paranormal_goggles");
    p.loadout[0]={goggles,Rarity::Common};CHECK_NEAR(loadoutCameraZoom(p,content()),normal*1.25,1e-9);
    p.loadout[1]={goggles,Rarity::Apex};CHECK_NEAR(loadoutCameraZoom(p,content()),normal*1.25,1e-9);
    p.loadout[0]={};p.loadout[1]={};CHECK_NEAR(loadoutCameraZoom(p,content()),normal,1e-9);
}

TEST(goggles_tooltip_explains_ghost_tiers_and_actual_view_penalty) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    const auto goggles = content().petalIndex("paranormal_goggles");
    const auto oldLanguage = ui::language();
    Profile profile;
    profile.loadout.resize(kLoadoutSlots);
    for (const auto language : {ui::Language::En, ui::Language::Zh}) {
        ui::setLanguage(language);
        for (const auto rarity : {Rarity::Common, Rarity::PermanentHyper, Rarity::Ascendant}) {
            profile.loadout[0] = {goggles, rarity};
            const double zoom = loadoutCameraZoom(profile, content());
            CHECK_NEAR(zoom, rarity == Rarity::Common ? 1.25 : 1.3, 1e-9);
            const auto text = ui::petalTooltipLines(goggles, rarity, {}, 1);
            const std::string loss = rarity == Rarity::Common ? "20%" : "23%";
            bool explainsGhosts = false;
            for (const auto& line : text) {
                const auto tierExplanation = language == ui::Language::Zh ? "同品质或更低品质" : "rarity or below";
                explainsGhosts |= line.text.find(tierExplanation) != std::string::npos &&
                                  line.text.find(loss) != std::string::npos;
                CHECK(line.text.find("Mysterious hint") == std::string::npos);
                CHECK(line.text.find("神秘提示") == std::string::npos);
            }
            CHECK(explainsGhosts);
        }
    }
    ui::setLanguage(oldLanguage);
}

TEST(mimic_wraps_within_its_row_and_all_mimic_rows_terminate) {
    std::string error; CHECK(loadContent(testsupport::dataDir(),error));
    Loadout row; const auto mimic=content().petalIndex("mimic");
    const auto egg=content().petalIndex("egg");
    row.slots[0]={mimic,Rarity::Unique};
    row.slots[kLoadoutActiveSlots-1]={egg,Rarity::Super};
    auto copy=equippedPetal(content(),row,0);
    CHECK_EQ(copy.configIndex,egg); CHECK(copy.rarity==Rarity::Unique);
    CHECK(copy.reloadRarity==Rarity::Super);
    for(int i=0;i<kLoadoutActiveSlots;++i)row.slots[i]={mimic,Rarity::Unique};
    CHECK_EQ(equippedPetal(content(),row,0).configIndex,mimic);
    row.slots[kLoadoutActiveSlots-1]={};
    CHECK_EQ(equippedPetal(content(),row,0).configIndex,mimic);
}

#include "server/shop_lock.h"
TEST(shop_locked_quotes_apply_current_price_without_unlocking) {
    std::string error; CHECK(loadContent(testsupport::dataDir(),error));
    PlayerRecord record; const auto rotation=shopRotation(1234567890);
    record.extra["lockedShopRotation"]=static_cast<double>(rotation);
    captureShopLock(record,rotation);
    const auto rows=record.extra.dump();
    for(const auto& offer:lockedShopOffers(record)) {
        CHECK_NEAR(offer.price, shopOfferPrice(offer.petalIndex, offer.rarity, offer.discountPercent), 1e-9);
    }
    CHECK_EQ(record.extra.dump(),rows);
}

TEST(peas_full_volley_is_weaker_and_reloads_slower_than_sawblade) {
 std::string error;CHECK(loadContent(testsupport::dataDir(),error));
 for(int tier=0;tier<kLadderRarityCount;++tier){auto r=static_cast<Rarity>(tier);
  auto peas=content().petalStats(content().petalIndex("peas"),r),saw=content().petalStats(content().petalIndex("sawblade"),r);
  CHECK(peas.damage*peas.count < saw.damage);
  CHECK(peas.health*peas.count < saw.health);
  CHECK(peas.reloadMillis > saw.reloadMillis);
 }
}

#include "client/ui/content_i18n.h"
TEST(chinese_inventory_names_are_available_in_both_ui_languages) {
 auto previous=ui::language();
 for(auto language:{ui::Language::Zh,ui::Language::En}) {
  ui::setLanguage(language);
  CHECK_EQ(std::string(ui::petalNameZh("magic_peas")),std::string("蓝莓"));
  CHECK(std::string(ui::petalNameZh("peas")).find("豌豆")!=std::string::npos);
 }
 ui::setLanguage(previous);
}

TEST(mimic_ram_shows_mimic_identity_and_effective_sawblade) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    const auto ram = content().petalIndex("ram");
    const auto mimic = content().petalIndex("mimic");
    Loadout loadout;
    loadout.slots[0] = {ram, Rarity::Common};
    loadout.slots[1] = {mimic, Rarity::Rare};
    const auto copy = equippedPetal(content(), loadout, 1);
    CHECK_EQ(copy.configIndex, content().petalIndex("sawblade"));
    CHECK(copy.rarity == Rarity::Rare);
    CHECK(copy.reloadRarity == Rarity::Common);
    const auto oldLanguage = ui::language();
    ui::setLanguage(ui::Language::En);
    const auto en = ui::equippedPetalTooltipLines(mimic, copy, {}, 1.0);
    CHECK_EQ(en.front().text, "Mimic");
    CHECK_EQ(en.back().text, "Effective copy: Sawblade");
    ui::setLanguage(ui::Language::Zh);
    const auto zh = ui::equippedPetalTooltipLines(mimic, copy, {}, 1.0);
    CHECK_EQ(zh.front().text, "拟态");
    CHECK_EQ(zh.back().text, "实际复制效果：锯片");
    ui::setLanguage(oldLanguage);
}


TEST(a_long_unspaced_tooltip_description_stays_within_its_wrap_width) {
    std::string text;
    for (int i = 0; i < 16; ++i) text += u8"这行花瓣说明需要正确换行";
    flix::ui::TooltipLine line{text, 14.0};
    line.maxWidth = 180.0;
    const auto size = flix::ui::measureTooltip({line});
    CHECK(size.x < 205.0);
    CHECK(size.y > 30.0);
}

TEST(fission_tooltips_split_the_real_chinese_description_into_bounded_rows) {
    std::string error;
    if (!loadContent(testsupport::dataDir(), error)) { CHECK(false); return; }
    ui::setLanguage(ui::Language::Zh);
    for (const char* id : {"fission", "arctic_fission"}) {
        const auto index = content().petalIndex(id);
        const auto lines = ui::petalTooltipLines(index, Rarity::Unique, {}, 1.0);
        CHECK(lines.size() >= 3u);
        if (lines.size() < 3) continue;
        CHECK(lines[2].text.find('\n') != std::string::npos);
        const auto size = ui::measureTooltip({lines[2]});
        CHECK(size.x < 255.0);
        CHECK(size.y > 60.0);
    }
    ui::setLanguage(ui::Language::En);
}
