#include "test.h"
#include "client/ui/chat_i18n.h"
#include "client/ui/i18n.h"
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
