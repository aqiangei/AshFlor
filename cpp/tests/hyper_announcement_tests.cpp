#include "test.h"
#include "test_data.h"
#include "client/ui/chat_i18n.h"
#include "client/ui/i18n.h"
#include "shared/game/config.h"
using namespace flix;
TEST(hyper_spawn_announcements_translate_rarity_and_mob_in_both_locations) {
    const auto before=ui::language();
    for(const auto tail:{"!"," somewhere!"}) {
        const std::string line=std::string("<b style=\"color: #ff0000;\">A Hyper Spider has spawned")+tail+"</b>";
        ui::setLanguage(ui::Language::Zh);const auto result=ui::localizeChatText(line);
        CHECK(result.find("超凡")!=std::string::npos);CHECK(result.find("蜘蛛")!=std::string::npos);
        CHECK(result.find("Hyper")==std::string::npos);CHECK(result.find("</b>")!=std::string::npos);
        CHECK(result.find(std::string(tail)=="!"?"已生成":"已在某处生成")!=std::string::npos);
        ui::setLanguage(ui::Language::En);CHECK_EQ(ui::localizeChatText(line),line);
    }
    ui::setLanguage(before);
}
TEST(a_hyper_spawn_on_another_map_names_the_map_in_chinese) {
    const auto before=ui::language();
    const std::string line="<b data-mob='bee' data-map='termite_mound_t3' style=\"color: #ff0000;\">A Hyper Bee has spawned in Termite Mound!</b>";
    ui::setLanguage(ui::Language::Zh);const auto result=ui::localizeChatText(line);
    CHECK(result.find("超凡")!=std::string::npos);CHECK(result.find("已在白蚁丘生成！")!=std::string::npos);
    CHECK(result.find("Termite")==std::string::npos);CHECK(result.find("</b>")!=std::string::npos);
    // A map the table has no name for keeps the one the server sent.
    const std::string astral="<b data-mob='bee' data-map='zorr_astral' style=\"color: #ff0000;\">A Hyper Bee has spawned in Astral!</b>";
    CHECK(ui::localizeChatText(astral).find("已在Astral生成！")!=std::string::npos);
    ui::setLanguage(ui::Language::En);CHECK_EQ(ui::localizeChatText(line),line);
    ui::setLanguage(before);
}

TEST(craft_announcements_use_inventory_petal_names) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    const auto before = ui::language();
    const std::string arctic = "<b data-petal='arctic_stick' style=\"color: #eeeeee;\">"
        "An Apex Stick has been crafted by <b style=\"color: #00ff00;\">@smith</b> [smith]</b>";
    const std::string desert = "<b data-petal='mysterious_stick' style=\"color: #eeeeee;\">"
        "An Apex Stick has been crafted by <b style=\"color: #00ff00;\">@smith</b> [smith]</b>";
    const std::string legacy = "<b style=\"color: #eeeeee;\">"
        "The Permanent Hyper Eyeball has been forged by @smith</b>";
    const std::string ambiguous = "<b style=\"color: #eeeeee;\">"
        "An Apex Stick has been crafted by @smith</b>";

    ui::setLanguage(ui::Language::Zh);
    const auto translated = ui::localizeChatText(arctic);
    CHECK(translated.find("极地树枝") != std::string::npos);
    CHECK(translated.find("神秘木棍") == std::string::npos);
    CHECK(translated.find("<b style=\"color: #00ff00;\">@smith</b>") != std::string::npos);
    CHECK(ui::localizeChatText(desert).find("神秘木棍") != std::string::npos);
    CHECK(ui::localizeChatText(legacy).find("虚空眼球") != std::string::npos);
    CHECK(ui::localizeChatText(ambiguous).find(" Stick ") != std::string::npos);
    ui::setLanguage(ui::Language::En);
    CHECK_EQ(ui::localizeChatText(arctic), arctic);
    ui::setLanguage(before);
}

TEST(light_craft_announcement_uses_the_same_name_as_the_inventory) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    const auto before = ui::language();
    ui::setLanguage(ui::Language::Zh);
    for (const std::string prefix : {"<b data-petal='light'", "<b", "<b data-petal='unknown'"}) {
        const std::string line =
            prefix + " style=\"color: #eeeeee;\">A Unique Light has been crafted by @smith</b>";
        CHECK(ui::localizeChatText(line).find("轻 已被 @smith 合成") != std::string::npos);
    }
    ui::setLanguage(before);
}

TEST(notification_language_uses_petal_id_and_preserves_player_text) {
    std::string error;
    CHECK(loadContent(testsupport::dataDir(), error));
    const auto before = ui::language();
    const std::string craft = "An Apex Stick has been crafted by @smith [Smith] (crafted x2, consumed x10)";
    const std::string stars = "Star code \"gift\" redeemed by @smith [Smith]! +1,000 ⭐ Stars";
    ui::setLanguage(ui::Language::Zh);
    CHECK_EQ(ui::localizeNotificationText(craft, "arctic_stick"),
        std::string("@smith [Smith] 合成了 Apex 极地树枝 (合成 ×2, 消耗 ×10)"));
    CHECK_EQ(ui::localizeNotificationText(stars, {}),
        std::string("@smith [Smith] 兑换了星币码 \"gift\"，获得 +1,000 ⭐ 星币"));
    CHECK_EQ(ui::localizeNotificationText("Event starting now", {}), std::string("Event starting now"));
    ui::setLanguage(ui::Language::En);
    CHECK_EQ(ui::localizeNotificationText(craft, "arctic_stick"), craft);
    CHECK_EQ(ui::localizeNotificationText(stars, {}), stars);
    ui::setLanguage(before);
}

TEST(announced_jurassic_layouts_preserve_prestige_tier) {
    const auto previous = ui::language();
    ui::setLanguage(ui::Language::Zh);
    const auto translate = [](const std::string& mapId,const std::string& suffix) {
        const std::string tag=mapId.empty() ? "" : " data-map='" + mapId + "'";
        return ui::localizeChatText("<b data-mob='bee'" + tag +
            " style=\"color: #ff0000;\">A Hyper Bee has spawned" + suffix + "!</b>");
    };
    CHECK(translate("jurassic_pangea"," in Pangea").find("已在侏罗纪生成！")!=std::string::npos);
    CHECK(translate("prestige_jurassic_pangea"," in Prestige Pangea").find("已在声望 I 侏罗纪生成！")!=std::string::npos);
    CHECK(translate("prestige_2_jurassic_tectonics"," in Prestige II Tectonics").find("已在声望 II 侏罗纪生成！")!=std::string::npos);
    CHECK(translate("prestige_2_jurassic"," in Prestige II Jurassic").find("已在声望 II 侏罗纪生成！")!=std::string::npos);
    CHECK(translate(""," in Prestige II").find("已在声望 II 生成！")!=std::string::npos);
    CHECK(translate(""," somewhere in Prestige I").find("已在声望 I 的某处生成！")!=std::string::npos);
    ui::setLanguage(previous);
}
