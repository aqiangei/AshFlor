#include "test.h"
#include "test_data.h"
#include "client/render/sprites.h"
#include "client/render/world_renderer.h"
#include "shared/game/config.h"
using namespace flix;
TEST(dice_art_is_visible_at_every_rarity){
    ContentRegistry c;std::string error;CHECK(c.loadFiles(testsupport::dataFile("mobs.json"),testsupport::dataFile("petals.json"),error));
    SpriteCache sprites;CHECK(sprites.build(c));
    for(const auto& w:sprites.warnings())if(w.find("dice")!=std::string::npos)std::printf("%s\n",w.c_str());
    const auto dice=c.petalIndex("dice");CHECK(sprites.petalDrawable(dice));
    for(int tier=0;tier<kLadderRarityCount;++tier){Canvas canvas=Canvas::createVirtual(100,100);
        sprites.drawPetal(canvas,dice,static_cast<Rarity>(tier),50,50,60,0,0);
        const auto px=canvas.getImageData(0,0,100,100);int visible=0;
        for(std::size_t i=3;i<px.size();i+=4)visible+=px[i]>64;CHECK(visible>500);
    }
}

#include "client/ui/item_tile.h"
#include "render_rig.h"
TEST(wide_view_keeps_nearby_drop_icons_in_a_dense_pile) {
    const auto& registry = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(registry));
    Camera camera = testsupport::frameCamera(512, {2000, 2000});
    camera.loadoutZoom = 0.15;
    camera.snapTo({2000, 2000});
    const auto render = [&](const char* nearArt, const char* farArt) {
        WorldRenderer renderer;
        renderer.setContent(&registry);
        renderer.setSprites(&sprites);
        WorldView view;
        RemoteEntity drop;
        drop.kind = net::EntityKind::Drop;
        drop.radius = 20;
        drop.rarity = Rarity::Epic;
        for (int i = 0; i < 50; ++i) {
            drop.netId = i + 1;
            drop.position = {2010.0 + 29 * (i % 10), 2040.0 + 29 * (i / 10)};
            drop.typeIndex = registry.petalIndex("basic");
            if (i == 0) {
                drop.position = {2000, 2000};
                drop.typeIndex = registry.petalIndex(nearArt);
            }
            if (i == 49) {
                drop.position = {2700, 2000};
                drop.typeIndex = registry.petalIndex(farArt);
            }
            view.seedForTest(drop);
        }
        auto canvas = Canvas::createVirtual(512, 512);
        renderer.draw(canvas, view, camera, {2000, 2000}, 0.0);
        CHECK_EQ(renderer.sectionTiming().itemCount, 50);
        return canvas.getImageData(0, 0, 512, 512);
    };
    const auto base = render("basic", "basic");
    CHECK(testsupport::differingPixels(base, render("leaf", "basic")) > 4);
    CHECK_EQ(testsupport::differingPixels(base, render("basic", "leaf")), 0);
}

TEST(equipped_dice_is_a_projected_cube_that_turns_over_time){
 std::string error;CHECK(loadContent(testsupport::dataDir(),error));SpriteCache sprites;CHECK(sprites.build(content()));
 auto frame=[&](double time,int face){Canvas canvas=Canvas::createVirtual(120,120);ui::ItemTile tile;tile.petalIndex=content().petalIndex("dice");tile.rarity=Rarity::Apex;tile.diceFace=face;tile.timeSeconds=time;tile.showName=false;ui::drawItemTile(canvas,sprites,{10,10,100,100},tile);return canvas.getImageData(0,0,120,120);};
 CHECK_EQ(testsupport::differingPixels(frame(1.2,6),frame(1.5,6)),0);
 const auto first=frame(0,6);CHECK(testsupport::differingPixels(first,frame(.5,6))>200);CHECK(testsupport::differingPixels(first,frame(0,1))>40);
}

TEST(orbiting_dice_renders_its_replicated_face_and_rolls) {
    const auto& registry = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(registry));
    WorldRenderer renderer;
    renderer.setContent(&registry);
    renderer.setSprites(&sprites);
    const auto frame = [&](double age, std::uint8_t face) {
        RemoteEntity petal;
        petal.netId = 2;
        petal.kind = net::EntityKind::Petal;
        petal.typeIndex = registry.petalIndex("dice");
        petal.ownerNetId = 1;
        petal.petalVariant = face;
        petal.petalAgeSeconds = age;
        WorldView view;
        view.seedForTest(petal);
        Canvas canvas = Canvas::createVirtual(160, 160);
        renderer.draw(canvas, view, testsupport::frameCamera(160, {}), {}, 1.0);
        return canvas.getImageData(0, 0, 160, 160);
    };
    CHECK(testsupport::differingPixels(frame(0.0, 6), frame(0.5, 6)) > 12);
    CHECK(testsupport::differingPixels(frame(1.2, 6), frame(1.2, 1)) > 3);
}

TEST(orbiting_card_draws_each_replicated_suit) {
    const auto& registry = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(registry));
    WorldRenderer renderer;
    renderer.setContent(&registry);
    renderer.setSprites(&sprites);
    const auto frame = [&](std::uint8_t suit) {
        RemoteEntity card;
        card.netId = 2;
        card.kind = net::EntityKind::Petal;
        card.typeIndex = registry.petalIndex("card");
        card.ownerNetId = 1;
        card.petalVariant = suit;
        card.radius = 18;
        WorldView view;
        view.seedForTest(card);
        Canvas canvas = Canvas::createVirtual(160, 160);
        renderer.draw(canvas, view, testsupport::frameCamera(160, {}), {}, 1.0);
        return canvas.getImageData(0, 0, 160, 160);
    };
    const std::array frames{frame(1), frame(2), frame(3), frame(4)};
    for (std::size_t i = 0; i < frames.size(); ++i)
        for (std::size_t j = i + 1; j < frames.size(); ++j)
            CHECK(testsupport::differingPixels(frames[i], frames[j]) > 10);
}

TEST(loaded_stinger_mobs_show_ammo_behind_the_body_until_firing) {
    const auto& registry = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(registry));
    WorldRenderer renderer;
    renderer.setContent(&registry);
    renderer.setSprites(&sprites);
    for (const char* id : {"hornet", "wasp", "hel_wasp", "mecha_wasp"}) {
        const auto frame = [&](std::uint8_t state) {
            RemoteEntity mob;
            mob.netId = 1;
            mob.kind = net::EntityKind::Mob;
            mob.typeIndex = registry.mobIndex(id);
            mob.radius = 30.0;
            mob.state = state;
            WorldView view;
            view.seedForTest(mob);
            Canvas canvas = Canvas::createVirtual(200, 200);
            renderer.draw(canvas, view, testsupport::frameCamera(200, {}), {}, 1.0);
            return canvas.getImageData(0, 75, 74, 50);
        };
        CHECK(testsupport::differingPixels(frame(0), frame(net::StateUnloaded)) > 20);
    }
}

#include "shared/game/skills.h"
TEST(duplicator_shows_extra_equipped_particles_using_each_petals_layout) {
 std::string error;CHECK(loadContent("/data",error));SpriteCache sprites;CHECK(sprites.build(content()));
 for(const char* id:{"orange","peas","dust","stinger","light","dahlia","dice","rock"}){
  auto frame=[&](const SkillSet* skills){ui::setViewerSkills(skills);Canvas c=Canvas::createVirtual(120,120);
   ui::ItemTile tile;tile.petalIndex=content().petalIndex(id);tile.rarity=Rarity::Super;tile.loadoutSlot=0;tile.showName=false;
   if(std::string(id)=="dice")tile.diceFace=6;
   ui::drawItemTile(c,sprites,{10,10,100,100},tile);return c.getImageData(0,0,120,120);};
  SkillSet skills;skills.set(SkillId::Loadout,4);skills.set(SkillId::Duplicator,3);
  CHECK(testsupport::differingPixels(frame(nullptr),frame(&skills))>10);
 }
 ui::setViewerSkills(nullptr);
}
TEST(dust_source_art_contains_one_particle_for_one_simulated_body) {
 std::string error;CHECK(loadContent("/data",error));SpriteCache sprites;CHECK(sprites.build(content()));
 Canvas c=Canvas::createVirtual(100,100);sprites.drawPetal(c,content().petalIndex("dust"),Rarity::Common,50,50,60,0,0);
 auto px=c.getImageData(0,0,100,100);int groups=0;std::vector<int> pending;
 for(int i=0;i<10000;++i){if(px[i*4+3]<128)continue;++groups;px[i*4+3]=0;pending.push_back(i);
 while(!pending.empty()){int n=pending.back();pending.pop_back();for(int next:{n-1,n+1,n-100,n+100})if(next>=0&&next<10000&&std::abs(next%100-n%100)<=1&&px[next*4+3]>=128){px[next*4+3]=0;pending.push_back(next);}}}
 CHECK_EQ(groups,1);CHECK_EQ(content().petalStats(content().petalIndex("dust"),Rarity::Common).count,3);
}
TEST(world_dust_is_small_and_does_not_change_inventory_art) {
 std::string error;CHECK(loadContent("/data",error));SpriteCache sprites;CHECK(sprites.build(content()));
 Canvas world=Canvas::createVirtual(100,100),icon=Canvas::createVirtual(100,100);
 world.save();world.translate(50,50);SpriteCache::drawDustGrain(world,60);world.restore();
 sprites.drawPetal(icon,content().petalIndex("dust"),Rarity::Common,50,50,60,0,0);
 const auto wp=world.getImageData(0,0,100,100),ip=icon.getImageData(0,0,100,100);
 int w=0,i=0;for(std::size_t n=3;n<wp.size();n+=4){w+=wp[n]>128;i+=ip[n]>128;}
 CHECK(w>100);CHECK(w<i/4);
}

TEST(attached_mark_art_pulses_over_fallen_flower) {
    const auto& registry = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(registry));
    auto frame = [&](const char* mark, double time, bool dead) {
        WorldRenderer renderer;
        renderer.setContent(&registry);
        renderer.setSprites(&sprites);
        WorldView view;
        RemoteEntity body;
        body.netId = 1;
        body.kind = net::EntityKind::Player;
        body.radius = 25;
        body.state = dead ? net::StateDead : 0;
        view.seedForTest(body);
        if (mark) {
            RemoteEntity petal;
            petal.kind = net::EntityKind::Petal;
            petal.netId = 2;
            petal.ownerNetId = 1;
            petal.typeIndex = registry.petalIndex(mark);
            petal.petalVariant = net::kAttachedMarkVariant;
            petal.radius = 9;
            view.seedForTest(petal);
        }
        Canvas canvas = Canvas::createVirtual(160, 160);
        renderer.draw(canvas, view, testsupport::frameCamera(160, {}), {}, time);
        return canvas.getImageData(48, 40, 64, 48);
    };
    const auto bare = frame(nullptr, 0, true);
    for (auto mark : {"dark_mark", "void_mark", "void_void_mark", "arctic_frozen_mark"}) {
        const auto attached = frame(mark, 0, true);
        CHECK(testsupport::differingPixels(bare, attached) > 8);
        CHECK(testsupport::differingPixels(attached, frame(mark, 0.3, true)) > 8);
        CHECK_EQ(testsupport::differingPixels(frame(nullptr, 0, false), frame(mark, 0, false)), 0);
    }
}
