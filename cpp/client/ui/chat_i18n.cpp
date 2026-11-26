#include "client/ui/chat_i18n.h"

#include "client/ui/content_i18n.h"
#include "client/ui/i18n.h"
#include "shared/game/config.h"
#include "shared/core/text.h"

#include <algorithm>

#include <cstddef>
#include <string>
#include <string_view>

namespace flix {
namespace ui {
namespace {

bool zh() { return language() == Language::Zh; }

struct Pair {
    const char* en;
    const char* zh;
};

constexpr Pair kNameTr[] = {
    {"Stick", "神秘木棍"},
    {"Mana Orb", "法力宝珠"},
    {"Magic Cotton", "魔法棉花"},
    {"Magic Eye", "魔眼"},
    {"Fragment", "碎片"},
    {"Mecha Missile", "机甲导弹"},
    {"Mark", "暗黑印记"},
    {"Frozen Mark", "冰封印记"},
    {"Arctic Centipede", "极地蜈蚣"},
    {"Arctic Spider Cave", "极地蜘蛛洞"},
    {"Arctic Spider", "极地蜘蛛"},
    {"Frost Digger", "霜冻掘地者"},
    {"Alien", "外星人"},
    {"Alien Digger", "外星掘地者"},
    {"Angel", "天使"},
    {"Black Hole", "黑洞"},
    {"Cicada 3301", "3301蝉"},
    {"Eye", "眼球"},
    {"Ghost", "幽灵"},
    {"Grave Digger", "掘墓者"},
    {"Gravestone", "墓碑"},
    {"Illuminati", "光明会"},
    {"Momy", "木乃伊"},
    {"Rain Frog", "雨蛙"},
    {"Tesla Coil", "特斯拉线圈"},
    {"Troll", "巨魔"},
    {"UFO", "飞碟"},
    {"Unabomber", "炸弹客"},
    {"White Hole", "白洞"},
    {"Zatan", "撒旦"},
    {"Ice Cube", "冰块"},
    {"Ice Dragon", "冰龙"},
    {"Ice Hornet", "冰胡蜂"},
    {"Igloo", "冰屋"},
    {"Nuke", "核弹"},
    {"Snowbush", "雪灌木"},
    {"Snowman", "雪人"},
    {"Snowstorm", "暴风雪"},
    {"Spider Digger", "掘地蜘蛛"},
    {"Spider Yoba", "尤巴蜘蛛"},
    {"Stalagmite", "石笋"},
    {"Tick", "蜱虫"},
    {"Blood Sacrifice", "血祭"},
    {"Heavy", "重物"},
    {"Bean", "豆子"},
    {"Dice", "骰子"},
    {"Laser", "激光"},
    {"Talisman of Evasion", "闪避护符"},
    {"Mjolnir", "雷神之锤"},
    {"Broccoli", "西兰花"},
    {"Clay", "黏土"},
    {"Electric Web", "电网"},
    {"Mysterious Powder", "神秘粉末"},
    {"Poker Chip", "筹码"},
    {"Blood Stinger", "血刺"},
    {"Ankh", "生命之符"},
    {"Dead Leaf", "枯叶"},
    {"Privet Berry", "女贞果"},
    {"Moon Rock", "月岩"},
    {"Totem", "图腾"},
    {"Monstera", "龟背竹"},
    {"Cogwheel", "齿轮"},
    {"Shovel", "铲子"},
    {"Domino", "多米诺骨牌"},
    {"Card", "扑克牌"},
    {"Mysterious Relic", "神秘遗物"},
    {"Paranormal Goggles", "灵异护目镜"},
    {"Pharaoh's Crown", "法老王冠"},
    {"Corruption", "腐化"},
    {"Nazar Amulet", "纳扎尔护符"},
    {"Action Test", "动作测试花瓣"},
    {"Action Test Petal", "动作测试花瓣"},
    {"Air", "空气"},
    {"Amulet", "护符"},
    {"Ant Egg", "蚂蚁蛋"},
    {"Ant Egg Egg", "蚂蚁蛋蛋"},
    {"Ant Hole", "蚁穴"},
    {"Ant Hole Egg", "蚁穴蛋"},
    {"Antennae", "触角"},
    {"Assembler", "装配工"},
    {"Assembler Egg", "装配工蛋"},
    {"Azalea", "杜鹃"},
    {"Baby Ant", "幼蚁"},
    {"Baby Ant Egg", "幼蚁蛋"},
    {"Baby Fire Ant", "幼火蚁"},
    {"Baby Fire Ant Egg", "幼火蚁蛋"},
    {"Baby Termite", "幼白蚁"},
    {"Baby Termite Egg", "幼白蚁蛋"},
    {"Bandage", "绷带"},
    {"Basic", "基础"},
    {"Basil", "罗勒"},
    {"Battery", "电池"},
    {"Bee", "蜜蜂"},
    {"Bee Egg", "蜜蜂蛋"},
    {"Beetle", "甲虫"},
    {"Beetle Egg", "甲虫蛋"},
    {"Blood Leaf", "血叶"},
    {"Blue Iris", "蓝鸢尾"},
    {"Blueberries", "蓝莓"},
    {"Bomb", "炸弹"},
    {"Bone", "骨头"},
    {"Bubble", "泡泡"},
    {"Bubble Egg", "泡泡蛋"},
    {"Bulb", "灯泡"},
    {"Bumble Bee", "熊蜂"},
    {"Bumble Bee Egg", "熊蜂蛋"},
    {"Bumble Bee Pollen", "熊蜂花粉"},
    {"Bur", "刺果"},
    {"Bush", "灌木"},
    {"Bush Egg", "灌木蛋"},
    {"C++", "C++"},
    {"C++ Egg", "C++蛋"},
    {"Cactus", "仙人掌"},
    {"Cactus Egg", "仙人掌蛋"},
    {"Capacitor", "电容器"},
    {"Carrot", "胡萝卜"},
    {"Centipede", "蜈蚣身体"},
    {"Centipede Body", "蜈蚣身体"},
    {"Centipede Body Egg", "蜈蚣身体蛋"},
    {"Centipede Egg", "蜈蚣蛋"},
    {"Champion Crown", "冠军王冠"},
    {"Champion's Crown", "冠军王冠"},
    {"Claw", "爪子"},
    {"Clover", "三叶草"},
    {"Coin", "硬币"},
    {"Compass", "指南针"},
    {"Computer Chip", "电脑芯片"},
    {"Coral", "珊瑚"},
    {"Corn", "玉米"},
    {"Cotton", "棉花"},
    {"Crab", "螃蟹"},
    {"Crab Egg", "螃蟹蛋"},
    {"Cube", "立方体"},
    {"Cube Egg", "立方体蛋"},
    {"Cursor", "光标"},
    {"Cutter", "切割器"},
    {"Dahlia", "大丽花"},
    {"Dandelion", "蒲公英"},
    {"Dandelion Egg", "蒲公英蛋"},
    {"Dark Ladybug", "暗瓢虫"},
    {"Dark Ladybug Egg", "暗瓢虫蛋"},
    {"Desert Centipede", "沙漠蜈蚣身体"},
    {"Desert Centipede Body", "沙漠蜈蚣身体"},
    {"Desert Centipede Body Egg", "沙漠蜈蚣身体蛋"},
    {"Desert Centipede Egg", "沙漠蜈蚣蛋"},
    {"Digger", "挖掘者"},
    {"Digger Egg", "挖掘者蛋"},
    {"Disc", "圆盘"},
    {"Dust", "尘埃"},
    {"Dust Egg", "尘埃蛋"},
    {"Egg", "蛋"},
    {"Evil Centipede", "邪恶蜈蚣身体"},
    {"Evil Centipede Body", "邪恶蜈蚣身体"},
    {"Evil Centipede Body Egg", "邪恶蜈蚣身体蛋"},
    {"Evil Centipede Egg", "邪恶蜈蚣蛋"},
    {"Explosive", "爆炸花瓣"},
    {"Explosive Petal", "爆炸花瓣"},
    {"Fang", "尖牙"},
    {"Faster", "加速"},
    {"Fire Ant Egg", "火蚁蛋"},
    {"Fire Ant Egg Egg", "火蚁蛋蛋"},
    {"Fire Ant Hole", "火蚁穴"},
    {"Fire Ant Hole Egg", "火蚁穴蛋"},
    {"Firefly", "萤火虫"},
    {"Firefly Egg", "萤火虫蛋"},
    {"Flower", "花朵"},
    {"Fly", "苍蝇"},
    {"Fly Egg", "苍蝇蛋"},
    {"Garbage", "垃圾"},
    {"Garbage Egg", "垃圾蛋"},
    {"Gas", "毒气"},
    {"Glass", "玻璃"},
    {"Glasss", "玻璃"},
    {"Glitch", "故障"},
    {"Glitch Egg", "故障蛋"},
    {"Glitch Flower", "故障花"},
    {"Glitch Flower Egg", "故障花蛋"},
    {"Golden Leaf", "金叶"},
    {"Magic Golden Leaf", "魔法金叶"},
    {"Golden Leafbug", "金色叶虫"},
    {"Golden Leafbug Egg", "金色叶虫蛋"},
    {"Grapes", "葡萄"},
    {"Guided Missile", "制导导弹"},
    {"Healing", "治疗花瓣"},
    {"Healing Petal", "治疗花瓣"},
    {"Heaviest", "最重"},
    {"Hel Beetle", "地狱甲虫"},
    {"Hel Beetle Egg", "地狱甲虫蛋"},
    {"Honey", "蜂蜜"},
    {"Hornet", "大黄蜂"},
    {"Hornet Egg", "大黄蜂蛋"},
    {"Hornet Missile", "大黄蜂导弹"},
    {"Icosahedron", "二十面体"},
    {"Infinity", "无限"},
    {"Iris", "鸢尾"},
    {"Item Spawner", "物品生成器"},
    {"Item Spawner Egg", "物品生成器蛋"},
    {"JavaScript", "JavaScript"},
    {"Javascript", "JavaScript"},
    {"Javascript Egg", "JavaScript蛋"},
    {"JavaScript Egg", "JavaScript蛋"},
    {"Jelly", "果冻"},
    {"Jellyfish", "水母"},
    {"Jellyfish Egg", "水母蛋"},
    {"Ladybug", "瓢虫"},
    {"Ladybug Egg", "瓢虫蛋"},
    {"Leaf", "叶子"},
    {"Leafbug", "叶虫"},
    {"Leafbug Egg", "叶虫蛋"},
    {"Leech", "水蛭身体"},
    {"Leech Body", "水蛭身体"},
    {"Leech Body Egg", "水蛭身体蛋"},
    {"Leech Egg", "水蛭蛋"},
    {"Lentil", "扁豆"},
    {"Light", "轻"},
    {"Lightning", "闪电"},
    {"Lightning Cutter", "闪电切割器"},
    {"Lotus", "莲花"},
    {"Magic Bubble", "魔法泡泡"},
    {"Magic Cactus", "魔法仙人掌"},
    {"Magic Firefly", "魔法萤火虫"},
    {"Magic Firefly Egg", "魔法萤火虫蛋"},
    {"Magic Leaf", "魔法叶"},
    {"Magic Missile", "魔法导弹"},
    {"Magic Orb", "法球"},
    {"Magic Peas", "蓝莓"},
    {"Magic Stick", "魔法木棍"},
    {"Magic Stinger", "魔法刺针"},
    {"Magnet", "磁铁"},
    {"Mantis", "螳螂"},
    {"Mantis Egg", "螳螂蛋"},
    {"Mecha Antennae", "机械触角"},
    {"Mimic", "拟态"},
    {"Missile", "导弹"},
    {"Moon", "月亮"},
    {"Moth", "蛾"},
    {"Moth Egg", "蛾蛋"},
    {"Mummy Beetle", "木乃伊甲虫"},
    {"Mummy Beetle Egg", "木乃伊甲虫蛋"},
    {"Observer", "机械触角"},
    {"Oracle", "神谕"},
    {"Oracle Egg", "神谕蛋"},
    {"Orange", "橙子"},
    {"Oranges", "橙子"},
    {"Orb", "法球"},
    {"Pearl", "珍珠"},
    {"Peas", "豌豆"},
    {"Pentagon", "五边形"},
    {"Pharaoh Beetle", "法老甲虫"},
    {"Pharaoh Beetle Egg", "法老甲虫蛋"},
    {"Pincer", "螯钳"},
    {"Plank", "木板"},
    {"Poison Cactus", "毒仙人掌"},
    {"Pollen", "花粉"},
    {"Poo", "便便"},
    {"Powder", "粉末"},
    {"Queen Ant", "蚁后"},
    {"Queen Ant Egg", "蚁后蛋"},
    {"Rainbow", "彩虹"},
    {"Raindrop", "雨滴"},
    {"RAM", "内存"},
    {"Ram", "内存"},
    {"Relic", "遗物"},
    {"Rice", "米饭"},
    {"Roach", "蟑螂"},
    {"Roach Egg", "蟑螂蛋"},
    {"Rock", "石头"},
    {"Rock Egg", "石头蛋"},
    {"Root", "根"},
    {"Rose", "玫瑰"},
    {"Rubber", "橡胶"},
    {"Sacrifice", "献祭"},
    {"Salt", "盐"},
    {"Sand", "沙子"},
    {"Sandstorm", "沙暴"},
    {"Sandstorm Egg", "沙暴蛋"},
    {"Sawblade", "锯片"},
    {"Scorpion", "蝎子"},
    {"Scorpion Egg", "蝎子蛋"},
    {"Server", "服务器"},
    {"Server Egg", "服务器蛋"},
    {"Shell", "贝壳"},
    {"Shell Egg", "贝壳蛋"},
    {"Shield", "护盾花瓣"},
    {"Shield Petal", "护盾花瓣"},
    {"Shiny Ladybug", "闪光瓢虫"},
    {"Shiny Ladybug Egg", "闪光瓢虫蛋"},
    {"Magic Soil", "魔法土"},
    {"Soil", "土"},
    {"Soldier Ant", "兵蚁"},
    {"Soldier Ant Egg", "兵蚁蛋"},
    {"Soldier Fire Ant", "兵火蚁"},
    {"Soldier Fire Ant Egg", "兵火蚁蛋"},
    {"Soldier Termite", "兵白蚁"},
    {"Soldier Termite Egg", "兵白蚁蛋"},
    {"Spaceship", "飞船"},
    {"Sparkle", "闪光花瓣"},
    {"Sparkle Petal", "闪光花瓣"},
    {"Spider", "蜘蛛"},
    {"Spider Egg", "蜘蛛蛋"},
    {"Splitter", "分裂器"},
    {"Sponge", "海绵"},
    {"Sponge Egg", "海绵蛋"},
    {"Square", "方块"},
    {"Square Egg", "方块蛋"},
    {"Starfish", "海星"},
    {"Starfish Egg", "海星蛋"},
    {"Stick", "木棍"},
    {"Stinger", "刺针"},
    {"Sun", "太阳"},
    {"Sun Egg", "太阳蛋"},
    {"Sun Pet", "太阳宠物"},
    {"Sun Pet Egg", "太阳宠物蛋"},
    {"Talisman", "护身符"},
    {"Target Dummy", "靶子"},
    {"Target Dummy Egg", "靶子蛋"},
    {"Termite Egg", "白蚁蛋"},
    {"Termite Egg Egg", "白蚁蛋蛋"},
    {"Termite Mound", "白蚁丘"},
    {"Termite Mound Egg", "白蚁丘蛋"},
    {"Termite Overmind", "白蚁主宰"},
    {"Termite Overmind Egg", "白蚁主宰蛋"},
    {"Test Explosive", "测试爆炸花瓣"},
    {"Test Explosive Petal", "测试爆炸花瓣"},
    {"Third Eye", "第三只眼"},
    {"Titan", "泰坦"},
    {"Titan Egg", "泰坦蛋"},
    {"Tomato", "番茄"},
    {"Tomb", "古墓"},
    {"Tomb Egg", "古墓蛋"},
    {"Trader", "商人"},
    {"Trader Egg", "商人蛋"},
    {"Triangle", "三角形"},
    {"Uranium", "铀"},
    {"Wasp", "胡蜂"},
    {"Wasp Egg", "胡蜂蛋"},
    {"Wasp Missile", "胡蜂导弹"},
    {"Wax", "蜂蜡"},
    {"Web", "蛛网"},
    {"Wing", "翅膀"},
    {"Worker Ant", "工蚁"},
    {"Worker Ant Egg", "工蚁蛋"},
    {"Worker Fire Ant", "工火蚁"},
    {"Worker Fire Ant Egg", "工火蚁蛋"},
    {"Worker Termite", "工白蚁"},
    {"Worker Termite Egg", "工白蚁蛋"},
    {"Yggdrasil", "世界树"},
    {"Yin Yang", "阴阳"},
    {"Yucca", "丝兰"},
};


const char* nameTr(const std::string& en) {
    for (std::size_t i = 0; i < sizeof(kNameTr) / sizeof(kNameTr[0]); ++i) {
        if (en == kNameTr[i].en) return kNameTr[i].zh;
    }
    return en.c_str();
}

const char* rarityTrEn(const std::string& en) {
    static constexpr Pair kR[] = {
        {"Common", "普通"},     {"Uncommon", "罕见"}, {"Unusual", "罕见"},
        {"Rare", "稀有"},       {"Epic", "史诗"},     {"Legendary", "传奇"},
        {"Mythic", "神话"},     {"Ultra", "究极"},    {"Super", "超级"},
        {"Permanent Hyper", "永久超凡"}, {"Eternal", "永恒"}, {"Unique", "唯一"},
        {"Hyper", "超凡"},
        {"Universal", "宇宙"},  {"Devine", "宇宙"}, {"Admin", "管理"},
    };
    for (const Pair& p : kR) {
        if (en == p.en) return p.zh;
    }
    return en.c_str();
}

bool startsWith(const std::string& s, const char* p) {
    const std::size_t n = std::char_traits<char>::length(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

bool endsWith(const std::string& s, const char* p) {
    const std::size_t n = std::char_traits<char>::length(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

/// Strip a leading "A "/"An "/"The " article.
std::string dropArticle(std::string s) {
    if (startsWith(s, "A ")) return s.substr(2);
    if (startsWith(s, "An ")) return s.substr(3);
    if (startsWith(s, "The ")) return s.substr(4);
    return s;
}

/// Split "Rarity Name" at the first space after a known rarity label.
bool splitRarityName(const std::string& body, std::string& rarity, std::string& name) {
    static const char* kLabels[] = {
        "Permanent Hyper", "Universal", "Devine", "Uncommon", "Unusual", "Legendary", "Eternal", "Unique",
        "Common", "Rare", "Epic", "Mythic", "Ultra", "Super", "Hyper", "Apex", "Admin",
    };
    for (const char* lab : kLabels) {
        const std::size_t n = std::char_traits<char>::length(lab);
        if (body.size() > n + 1 && body.compare(0, n, lab) == 0 && body[n] == ' ') {
            rarity = lab;
            name = body.substr(n + 1);
            return true;
        }
    }
    return false;
}

std::string localizePlain(std::string text) {
    struct Exact {
        const char* en;
        const char* zh;
    };
    static constexpr Exact kExact[] = {
        {"You cannot leave a dungeon in your current state.", "当前状态无法退出白蚁穴。"},
        {"You are not inside an active termite mound.", "你当前不在白蚁穴内。"},
        {"You left the termite mound.", "已退出白蚁穴。"},
        {"Type /dungeon-leave to leave the termite mound early.", "输入 /dungeon-leave 可提前退出白蚁穴。"},
        {"Not enough mana to summon.", "法力不足，无法召唤。"},
        {"That player is already in a squad.", "该玩家已在小队中。"},
        {"The two squads would exceed 4 players.", "两队合计超过 4 人，无法合并。"},
        {"The two squads have merged.", "两个小队已合并。"},
        {"That player already has a pending invite.", "该玩家已有待处理邀请。"},
        {"You are already in a squad.", "你已在小队中。"},
        {"No pending invite.", "没有待处理的邀请。"},
        {"Leave your current squad before accepting an invite.", "请先退出当前小队，再接受邀请。"},
        {"Squad invite declined.", "已拒绝小队邀请。"},
        {"You have left the squad.", "你已离开小队。"},
        {"You are not in a squad.", "你不在小队中。"},
        {"You cannot invite yourself.", "不能邀请自己。"},
        {"You are not in a guild.", "你不在公会中。"},
        {"You were revived.", "你已复活。"},
        {"Rift is entered through the portals in Garden, Desert or Jungle. /rift shows the schedule.",
         "请从花园、沙漠或丛林的裂隙传送门进入。输入 /rift 查看开放时间。"},
        {"Void Mark: sent to Void. Use /mark-return to go back.",
         "虚空印记已将你送到虚空。输入 /mark-return 返回原处。"},
        {"Frozen Mark: sent to Arctic. Use /mark-return to go back.",
         "冰封印记已将你送到极地。输入 /mark-return 返回原处。"},
        {"Blood Sacrifice kept. The monsters in this area are too low in rarity.",
         "血祭已保留。死亡位置的怪物稀有度太低，无法召唤。"},
        {"Blood Sacrifice kept. This Prestige map does not allow a ritual of this tier.",
         "血祭已保留。这张声望地图不允许这个档位的血祭。"},
        {"Blood Sacrifice kept. A boss or another ritual already occupies this rarity slot.",
         "血祭已保留。此处同阶 Boss 名额已被怪物或另一场血祭占用。"},
        {"Blood Sacrifice kept. There is no spawn area at the spot where you died.",
         "血祭已保留。死亡位置没有刷怪区域。"},
        {"Blood Sacrifice kept. This area has no eligible monster for the ritual.",
         "血祭已保留。此处刷怪区域没有可供血祭召唤的怪物。"},
        {"Press Enter to chat...", "按 Enter 聊天…"},
    };
    for (const Exact& e : kExact) {
        if (text == e.en) return e.zh;
    }

    const std::string_view savedLoadout = "Saved loadout ";
    if (text.rfind(savedLoadout, 0) == 0 && endsWith(text, ".")) {
        return "已保存配装 " + text.substr(savedLoadout.size(), text.size() - savedLoadout.size() - 1) + "。";
    }

    const std::string_view loaded = "Loaded loadout ";
    const std::string_view kept = ". Current talents kept: the saved talents cost too many points.";
    if (text.rfind(loaded, 0) == 0 && endsWith(text, kept.data())) {
        std::string detail = text.substr(loaded.size(), text.size() - loaded.size() - kept.size());
        const auto swap = [&](std::string_view en, std::string_view zh) {
            const auto at = detail.find(en);
            if (at != std::string::npos) detail.replace(at, en.size(), zh);
        };
        swap(" petals at a lower rarity", " 枚花瓣使用较低品质");
        swap(" petal at a lower rarity", " 枚花瓣使用较低品质");
        swap(" petals not found", " 枚花瓣缺失");
        swap(" petal not found", " 枚花瓣缺失");
        return "已切换配装 " + detail + "。保存的天赋点数超出当前预算，已保留当前天赋。";
    }
    if (text.rfind(loaded, 0) == 0 && endsWith(text, ".")) {
        std::string detail = text.substr(loaded.size(), text.size() - loaded.size() - 1);
        const auto swap = [&](std::string_view en, std::string_view zh) {
            const auto at = detail.find(en);
            if (at != std::string::npos) detail.replace(at, en.size(), zh);
        };
        swap(" petals at a lower rarity", " 枚花瓣使用较低品质");
        swap(" petal at a lower rarity", " 枚花瓣使用较低品质");
        swap(" petals not found", " 枚花瓣缺失");
        swap(" petal not found", " 枚花瓣缺失");
        swap(" (", "（");
        swap(")", "）");
        return "已切换配装 " + detail + "。";
    }

    // "{name} has joined the squad."
    const char* joinSuf = " has joined the squad.";
    if (endsWith(text, joinSuf)) {
        return text.substr(0, text.size() - std::char_traits<char>::length(joinSuf)) +
               " 加入了小队。";
    }
    const char* leftSuf = " has left the squad.";
    if (endsWith(text, leftSuf)) {
        return text.substr(0, text.size() - std::char_traits<char>::length(leftSuf)) +
               " 离开了小队。";
    }
    const char* leadSuf = " is now the squad leader.";
    if (endsWith(text, leadSuf)) {
        return text.substr(0, text.size() - std::char_traits<char>::length(leadSuf)) +
               " 成为了小队长。";
    }
    if (startsWith(text, "Squad is now ")) {
        const std::string mode = text.substr(13); // ends with "."
        if (mode == "public.") return "小队已设为公开。";
        if (mode == "private.") return "小队已设为私密。";
        return "小队已设为 " + mode;
    }
    if (startsWith(text, "Squad invite sent to ")) {
        return "已向 " + text.substr(21) + " 发送小队邀请";
    }
    if (startsWith(text, "Invite sent to ")) {
        // may include trailing "." or markup
        return "已向 " + text.substr(15) + " 发送邀请";
    }
    if (startsWith(text, "You were revived by ")) {
        return "你被 " + text.substr(20) + " 复活。";
    }
    if (startsWith(text, "No player named ") && endsWith(text, " is online.")) {
        return "没有名为 " +
               text.substr(16, text.size() - 16 - std::char_traits<char>::length(" is online.")) +
               " 的在线玩家。";
    }
    return text;
}

/// A map's name as the spawn picker gives it: by its id, or by the biome its
/// id begins with (termite_mound_t3 is a termite mound); the English name the
/// server sent when the table has neither.
std::string mapNameTr(std::string id, const std::string& english) {
    const bool prestigeTwo = startsWith(id, "prestige_2_");
    const bool prestigeOne = !prestigeTwo && startsWith(id, "prestige_");
    const auto base = std::string_view(id).substr(prestigeTwo ? sizeof("prestige_2_") - 1 : prestigeOne ? sizeof("prestige_") - 1 : 0);
    if (base == "jurassic" || base == "jurassic_pangea" || base == "jurassic_tectonics")
        return prestigeTwo ? "声望 II 侏罗纪" : prestigeOne ? "声望 I 侏罗纪" : "侏罗纪";
    if (id.empty() && (english == "Jurassic" || english == "Pangea" || english == "Tectonics"))
        return "侏罗纪";
    while (!id.empty()) {
        if (const char* name = biomeTr(id, nullptr)) return name;
        const std::size_t cut = id.rfind('_');
        if (cut == std::string::npos) break;
        id.erase(cut);
    }
    return english;
}

std::string announcedPetalName(const std::string& id, const std::string& name) {
    if (!id.empty()) {
        const auto index = content().petalIndex(id);
        if (index < content().petalCount()) {
            const auto& petal = content().petal(index);
            if (petal.name == name) return petalNameTr(petal.id, petal.name.c_str());
        }
        return nameTr(name);
    }
    // Old chat history has only the English name. Treat a shared name as
    // ambiguous rather than choosing a different petal's translation.
    const PetalConfig* match = nullptr;
    for (std::size_t i = 0; i < content().petalCount(); ++i) {
        const auto& petal = content().petal(static_cast<std::uint16_t>(i));
        if (petal.name != name) continue;
        if (match) return name;
        match = &petal;
    }
    return match ? petalNameTr(match->id, match->name.c_str()) : nameTr(name);
}

/// Translate entity phrase "Rarity Name" using the relevant content table.
std::string trEntity(const std::string& rarityAndName, const std::string& mobId, bool mobAnnouncement,
                     const std::string& petalId, bool petalAnnouncement) {
    std::string rarity, name;
    if (!splitRarityName(rarityAndName, rarity, name)) {
        return std::string(nameTr(rarityAndName));
    }
    if (petalAnnouncement)
        return std::string(rarityTrEn(rarity)) + " " + announcedPetalName(petalId, name);
    if (!mobId.empty()) {
        const auto index = content().mobIndex(mobId);
        if (index < content().mobCount()) {
            const auto& mob = content().mob(index);
            if (mob.name == name)
                return std::string(rarityTrEn(rarity)) + " " + mobNameTr(mob.id, mob.name.c_str());
        }
    }
    // Older history rows carry the English mob name without a data-mob ID.
    // Translate only when the announced name identifies a single mob.
    if (mobAnnouncement && mobId.empty()) {
        const MobConfig* match = nullptr;
        for (std::size_t i = 0; i < content().mobCount(); ++i) {
            const auto& mob = content().mob(static_cast<std::uint16_t>(i));
            if (mob.name != name) continue;
            if (match != nullptr) {
                match = nullptr;
                break;
            }
            match = &mob;
        }
        if (match != nullptr)
            return std::string(rarityTrEn(rarity)) + " " + mobNameTr(match->id, match->name.c_str());
    }
    return std::string(rarityTrEn(rarity)) + " " + nameTr(name);
}

std::string localizeMarkupAware(std::string text) {
    // Invite: ...>@name (guild) has invited you to their squad. Use /squad-accept or /squad-decline.</span>
    const char* inviteGuild = " (guild) has invited you to their squad. Use /squad-accept or /squad-decline.";
    const char* invitePlain = " has invited you to their squad. Use /squad-accept or /squad-decline.";
    auto rewriteInvite = [&](const char* mid, const char* zhMid) -> bool {
        const std::size_t at = text.find(mid);
        if (at == std::string::npos) return false;
        // Keep everything before mid (includes @name and optional colour open tag).
        text.replace(at, std::char_traits<char>::length(mid), zhMid);
        return true;
    };
    if (rewriteInvite(inviteGuild,
                      "（公会）邀请你加入小队。输入 /sqa 接受，/sqd 拒绝。")) {
        return text;
    }
    if (rewriteInvite(invitePlain, " 邀请你加入小队。输入 /sqa 接受，/sqd 拒绝。")) {
        return text;
    }

    // Boss / sacrifice: A {Rarity} {Name} has spawned[ somewhere]!
    // Also defeated / summoned / crafted / forged / collected.
    // Pattern inside coloured <b>: ARTICLE RARITY NAME VERB...
    const std::size_t open = text.find("\">");
    if (open != std::string::npos) {
        const std::size_t bodyStart = open + 2;
        // Names in announcements have their own coloured bold tags.
        std::size_t close = text.find("</b>", bodyStart);
        for (auto nested = text.find("<b ", bodyStart); close != std::string::npos && nested < close;
             nested = text.find("<b ", nested + 3)) {
            close = text.find("</b>", close + 4);
        }
        if (close != std::string::npos) {
            std::string body = text.substr(bodyStart, close - bodyStart);
            std::string prefix = text.substr(0, bodyStart);
            std::string suffix = text.substr(close); // includes </b>...

            std::string mobId;
            constexpr std::string_view mobTag = "<b data-mob='";
            if (prefix.compare(0, mobTag.size(), mobTag) == 0) {
                const auto end = prefix.find('\'', mobTag.size());
                if (end != std::string::npos) mobId = prefix.substr(mobTag.size(), end - mobTag.size());
            }
            std::string mapId;
            constexpr std::string_view mapTag = " data-map='";
            if (const auto at = prefix.find(mapTag); at != std::string::npos) {
                const auto from = at + mapTag.size();
                const auto end = prefix.find('\'', from);
                if (end != std::string::npos) mapId = prefix.substr(from, end - from);
            }
            std::string petalId;
            constexpr std::string_view pickupTag = "<b data-pickup='";
            const std::string_view petalTag = prefix.compare(0, pickupTag.size(), pickupTag) == 0
                ? pickupTag : "<b data-petal='";
            if (prefix.compare(0, petalTag.size(), petalTag) == 0) {
                const auto end = prefix.find('\'', petalTag.size());
                if (end != std::string::npos) petalId = prefix.substr(petalTag.size(), end - petalTag.size());
            }
            auto tryVerb = [&](const char* enVerb, const char* zhVerbTpl) -> bool {
                // zhVerbTpl uses %e for entity and optional trailing bits handled outside
                const std::size_t at = body.find(enVerb);
                if (at == std::string::npos) return false;
                std::string head = dropArticle(body.substr(0, at));
                // trim trailing space already handled by verb starting with space? verbs include leading space
                while (!head.empty() && head.back() == ' ') head.pop_back();
                const std::string_view verb = enVerb;
                const bool mobAnnouncement = verb == " has spawned" ||
                    verb == " has been summoned" || verb == " has been defeated";
                const bool petalAnnouncement = verb == " has been crafted by " ||
                    verb == " has been forged by " || verb == " has been collected by ";
                const std::string entity = trEntity(head, mobId, mobAnnouncement, petalId, petalAnnouncement);
                const std::string rest = body.substr(at + std::char_traits<char>::length(enVerb));
                // rest may be "!", " somewhere!", " in MAP!", " by MARKUP", " by MARKUP!</b>" already cut
                std::string zhBody;
                if (std::string(enVerb) == " has spawned") {
                    if (startsWith(rest, " somewhere in Prestige II!")) {
                        zhBody = entity + " 已在声望 II 的某处生成！";
                    } else if (startsWith(rest, " somewhere in Prestige I!")) {
                        zhBody = entity + " 已在声望 I 的某处生成！";
                    } else if (startsWith(rest, " somewhere")) {
                        zhBody = entity + (startsWith(rest, " somewhere in Prestige")
                            ? " 已在声望层的某处生成！" : " 已在某处生成！");
                    } else if (startsWith(rest, " in Prestige II!")) {
                        zhBody = entity + " 已在声望 II 生成！";
                    } else if (startsWith(rest, " in Prestige I!")) {
                        zhBody = entity + " 已在声望 I 生成！";
                    } else if (startsWith(rest, " in Prestige!")) {
                        zhBody = entity + " 已在声望层生成！";
                    } else if (startsWith(rest, " in ")) {
                        const std::size_t bang = rest.find('!', 4);
                        const std::string english =
                            rest.substr(4, bang == std::string::npos ? std::string::npos : bang - 4);
                        zhBody = entity + " 已在" + mapNameTr(mapId, english) + "生成！";
                    } else {
                        zhBody = entity + " 已生成！";
                    }
                } else if (std::string(enVerb) == " has been summoned") {
                    if (startsWith(rest, " somewhere")) {
                        zhBody = entity + " 已在某处被召唤！";
                    } else {
                        zhBody = entity + " 已被召唤！";
                    }
                } else if (std::string(enVerb) == " has been defeated") {
                    if (startsWith(rest, " by ")) {
                        zhBody = entity + " 已被 " + rest.substr(4) + " 击败";
                    } else {
                        zhBody = entity + " 已被击败！";
                    }
                } else if (std::string(enVerb) == " has been crafted by ") {
                    zhBody = entity + " 已被 " + rest + " 合成";
                } else if (std::string(enVerb) == " has been forged by ") {
                    zhBody = entity + " 已被 " + rest + " 锻造";
                } else if (std::string(enVerb) == " has been collected by ") {
                    zhBody = entity + " 已被 " + rest + " 拾取";
                } else {
                    return false;
                }
                text = prefix + zhBody + suffix;
                return true;
            };

            if (tryVerb(" has spawned", nullptr)) return text;
            if (tryVerb(" has been summoned", nullptr)) return text;
            if (tryVerb(" has been defeated", nullptr)) return text;
            if (tryVerb(" has been crafted by ", nullptr)) return text;
            if (tryVerb(" has been forged by ", nullptr)) return text;
            if (tryVerb(" has been collected by ", nullptr)) return text;
        }
    }

    // Restart warnings
    {
        const char* a = "Server will restart in ";
        const char* b = "Server restarting in ";
        const char* c = "Server restarting now (";
        if (text.find(a) != std::string::npos) {
            // Server will restart in N minute(s) (reason).
            std::string t = text;
            auto replace = [&](const char* en, const char* zh) {
                std::size_t p = t.find(en);
                if (p != std::string::npos) t.replace(p, std::char_traits<char>::length(en), zh);
            };
            replace(a, "服务器将在 ");
            replace(" minutes (", " 分钟后重启（");
            replace(" minute (", " 分钟后重启（");
            replace("daily maintenance", "每日维护");
            return t;
        }
        if (text.find(b) != std::string::npos) {
            std::string t = text;
            auto replace = [&](const char* en, const char* zh) {
                std::size_t p = t.find(en);
                if (p != std::string::npos) t.replace(p, std::char_traits<char>::length(en), zh);
            };
            replace(b, "服务器将在 ");
            replace(" seconds!", " 秒后重启！");
            replace(" second!", " 秒后重启！");
            return t;
        }
        if (text.find(c) != std::string::npos) {
            std::string t = text;
            auto replace = [&](const char* en, const char* zh) {
                std::size_t p = t.find(en);
                if (p != std::string::npos) t.replace(p, std::char_traits<char>::length(en), zh);
            };
            replace(c, "服务器正在重启（");
            replace("). Reconnecting shortly...", "）。即将重新连接…");
            replace("daily maintenance", "每日维护");
            return t;
        }
    }

    // Span-wrapped "Invite sent to X."
    {
        const char* mid = "Invite sent to ";
        const std::size_t at = text.find(mid);
        if (at != std::string::npos) {
            text.replace(at, std::char_traits<char>::length(mid), "已向 ");
            const std::size_t dot = text.find(".</span>", at);
            if (dot != std::string::npos) {
                text.replace(dot, 1, " 发送邀请");
            }
            return text;
        }
    }

    return localizePlain(text);
}

} // namespace

std::string localizeChatText(std::string text) {
    if (!zh() || text.empty()) return text;
    if (text == "This entrance has no Prestige map. Switch to Ordinary or choose another entrance.")
        return "这个入口没有声望地图。请切换到普通，或选择其他入口。";
    for (const auto& pair : {Pair{"crafted x", "合成 ×"}, Pair{"forged x", "锻造 ×"}, Pair{"consumed x", "消耗 ×"}}) {
        std::size_t at = 0;
        while ((at = text.find(pair.en, at)) != std::string::npos) {
            text.replace(at, std::char_traits<char>::length(pair.en), pair.zh);
            at += std::char_traits<char>::length(pair.zh);
        }
    }
    text = localizeMarkupAware(std::move(text));
    // Trader rewards and other inline item labels carry a standalone
    // "Rarity Name" span inside a sentence instead of tagging the sentence.
    constexpr std::string_view tag = "<b data-petal='";
    for (std::size_t at = 0; (at = text.find(tag, at)) != std::string::npos;) {
        const auto idStart = at + tag.size();
        const auto idEnd = text.find('\'', idStart);
        const auto openEnd = text.find('>', idStart);
        if (idEnd == std::string::npos || openEnd == std::string::npos || idEnd > openEnd) break;
        at = openEnd + 1;
        const auto close = text.find('<', at);
        if (close == std::string::npos || text.compare(close, 4, "</b>") != 0) continue;
        std::string rarity, name;
        if (!splitRarityName(text.substr(at, close - at), rarity, name)) continue;
        const auto localized = std::string(rarityTrEn(rarity)) + " " +
            announcedPetalName(text.substr(idStart, idEnd - idStart), name);
        text.replace(at, close - at, localized);
        at += localized.size();
    }
    return text;
}

std::string localizeNotificationText(const std::string& text, const std::string& petalId) {
    if (!zh()) return text;
    for (const auto& verb : {Pair{" has been crafted by ", " 合成了 "},
                            Pair{" has been forged by ", " 锻造了 "}}) {
        const auto at = text.find(verb.en);
        if (at == std::string::npos) continue;
        const auto subject = dropArticle(text.substr(0, at));
        std::string rarity, petalName;
        if (!splitRarityName(subject, rarity, petalName)) continue;
        const auto from = at + std::char_traits<char>::length(verb.en);
        const auto details = text.rfind(" (");
        const auto detailFrom = details != std::string::npos && details >= from ? details : text.size();
        const std::string translated = std::string(rarityTrEn(rarity)) + " " +
            announcedPetalName(petalId, petalName);
        return text.substr(from, detailFrom - from) + verb.zh + translated +
            localizeChatText(text.substr(detailFrom));
    }
    if (startsWith(text, "Star code \"") && endsWith(text, " Stars")) {
        constexpr std::string_view codePrefix = "Star code ";
        constexpr std::string_view redeemedBy = "\" redeemed by ";
        constexpr std::string_view rewardSuffix = " Stars";
        const auto redeemed = text.find(redeemedBy);
        const auto reward = text.rfind("! +");
        if (redeemed != std::string::npos && reward != std::string::npos) {
            const auto playerFrom = redeemed + redeemedBy.size();
            if (reward > playerFrom) {
                return text.substr(playerFrom, reward - playerFrom) + " 兑换了星币码 " +
                    text.substr(codePrefix.size(), redeemed + 1 - codePrefix.size()) + "，获得 " +
                    text.substr(reward + 2, text.size() - reward - 2 - rewardSuffix.size()) + " 星币";
            }
        }
    }
    return text;
}

} // namespace ui
} // namespace flix
