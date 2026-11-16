#include "test.h"
#include "test_data.h"
#include "client/ui/gallery_drop_preview.h"
#include "client/ui/text.h"
#include "client/net_client.h"
#include "shared/core/json.h"
#include "shared/game/config.h"
#include "shared/game/drop_rates.h"
using namespace flix;

TEST(gallery_magic_drop_preview_follows_all_authored_conversions_and_orb_cap) {
    std::string error; CHECK(loadContent(testsupport::dataDir(),error)); CHECK(ui::Fonts::init(testsupport::dataDir(),error));
    Json data; CHECK(Json::parseFile(testsupport::dataFile("mob_drops.json"),data,error)); int checked=0;
    for(const auto& mob:data.keys())for(const auto& entry:data[mob]["drops"].items()){
        const auto source=content().petalIndex(entry["itemType"].asString());
        const auto magic=entry.contains("magicItemType")?content().petalIndex(entry["magicItemType"].asString()):content().magicFormOf(source);
        if(magic==kInvalidIndex)continue;
        const auto plain=galleryDropPreview(content().mobIndex(mob),Rarity::Apex,kRarityCount);
        const auto converted=galleryDropPreview(content().mobIndex(mob),Rarity::Apex,rarityIndex(Rarity::Unique));
        bool seen=false;
        for(const auto& d:plain)CHECK(d.petalIndex!=magic);
        for(const auto& d:converted)if(d.petalIndex==magic){seen=true;CHECK(d.rarity==Rarity::PermanentHyper || rarityIndex(d.rarity)<=8);CHECK(d.probability>0);}
        CHECK(seen);++checked;
    }
    CHECK(checked>=20);
    DropRates conditional;
    for(const auto& entry:data["golden_leafbug"]["drops"].items())
        if(entry.contains("magicRarityChances"))CHECK(parseDropRates(entry["magicRarityChances"],conditional));
    CHECK(!conditional.empty());
    double expected=0,shown=0;
    for(const auto& r:ratesFor(conditional,Rarity::Unique))expected+=r.probability*100;
    for(const auto& d:galleryDropPreview(content().mobIndex("golden_leafbug"),Rarity::Unique,5))
        if(d.petalIndex==content().petalIndex("magic_leaf")){shown+=d.probability;CHECK(rarityIndex(d.rarity)<=5);}
    CHECK_NEAR(shown,expected,1e-7);
    for(const char* mob:{"ladybug","jungle_ladybug","desert_ladybug"}){
        bool found=false;
        for(const auto& d:galleryDropPreview(content().mobIndex(mob),Rarity::Apex,5))
            if(d.petalIndex==content().petalIndex("mana_orb")){found=true;CHECK(d.rarity==Rarity::PermanentHyper || rarityIndex(d.rarity)<=5);}
        CHECK(found);
    }
}
TEST(gallery_magic_orb_checks_active_slots_and_resolves_mimic) {
    std::string error; CHECK(loadContent(testsupport::dataDir(),error));
    Profile p;p.loadout.resize(kLoadoutSlots);
    const auto orb=content().petalIndex("mana_orb");
    p.loadout[kLoadoutActiveSlots]={orb,Rarity::Apex};CHECK_EQ(galleryOrbTier(p),kRarityCount);
    p.loadout[0]={orb,Rarity::Mythic};CHECK_EQ(galleryOrbTier(p),5);
    p.loadout[1]={content().petalIndex("mimic"),Rarity::Unique};CHECK_EQ(galleryOrbTier(p),8);
}

TEST(gallery_hyper_pharaoh_shows_the_budgeted_eternal_crown_chance) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    CHECK(ui::Fonts::init(testsupport::dataDir(), error));
    double total = 0.0;
    double crown = 0.0;
    const auto index = content().petalIndex("pharaoh_s_crown");
    for (const auto& drop : galleryDropPreview(content().mobIndex("pharaoh_beetle"), Rarity::Apex, kRarityCount)) {
        if (drop.rarity != Rarity::Unique) continue;
        total += drop.probability;
        if (drop.petalIndex == index) crown += drop.probability;
    }
    CHECK_NEAR(crown, 0.2, 1e-8);
    CHECK_NEAR(total, 100.0 * kHyperEternalDropBudget / kLootFeelCopies, 1e-8);
}

TEST(gallery_hyper_eternal_cells_share_the_reduced_budget) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    CHECK(ui::Fonts::init(testsupport::dataDir(), error));
    double total=0;
    for(const auto& d:galleryDropPreview(content().mobIndex("ladybug"),Rarity::Apex,kRarityCount))
        if(d.rarity==Rarity::Unique)total+=d.probability;
    // The two 0.9645% Super tails each cap their Eternal share at half.
    CHECK_NEAR(total, 0.9645, 1e-8);
}

TEST(gallery_shows_personal_boss_rewards_at_two_percent_total) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    CHECK(ui::Fonts::init(testsupport::dataDir(), error));
    const auto ladybug = content().mobIndex("ladybug");
    for (const auto mobTier : {Rarity::Apex, Rarity::Ascendant}) {
        const auto petalTier = personalBossRewardRarity(mobTier);
        double total = 0.0;
        int rows = 0;
        for (const auto& drop : galleryDropPreview(ladybug, mobTier, kRarityCount)) {
            if (drop.rarity != petalTier) continue;
            CHECK(drop.petalIndex == content().petalIndex("rose") ||
                  drop.petalIndex == content().petalIndex("light"));
            total += drop.probability;
            ++rows;
        }
        CHECK_EQ(rows, 2);
        CHECK_NEAR(total, 100.0 * kApexPetalDropChance, 1e-8);
    }
}
