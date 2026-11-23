#include "test.h"
#include "test_data.h"
#include "server/systems/combat.h"
#include "server/systems/petals.h"
#include "server/systems/loot.h"
#include "client/ui/content_i18n.h"
#include "client/ui/i18n.h"
using namespace flix;
namespace {
struct RingWorld {
 const ContentRegistry& c = testsupport::shippedContent();
 World w; CommandBuffer commands{w}; SpatialGrid grid; EventQueue events;
 PetalSystem petals; CombatSystem combat; LootSystem loot; Rng rng{42};
 Entity player;
 RingWorld() { player=flower({5000,5000},17); }
 Entity flower(Vec2 at, int connection) {
  Entity e=w.create(); w.add<PlayerTag>(e); w.add<Transform>(e,Transform{at,0});
  w.add<Body>(e,Body{25,1}); w.add<Health>(e,Health{1000,1000});
  w.add<Faction>(e,Faction{Team::Players,false});w.add<PlayerModifiers>(e);
  w.add<Loadout>(e);w.add<PetalRing>(e);w.add<PlayerInput>(e);
  PlayerAccount a;a.connection=connection;w.add<PlayerAccount>(e,a);return e;
 }
 void equip(int slot,const char* id,Rarity rarity=Rarity::Common) {
  w.get<Loadout>(player).slots[slot]={c.petalIndex(id),rarity};
 }
 void tickPetals(double now) {petals.run(w,c,now,net::kTickSeconds,commands);commands.flush();}
 void index() {grid.clear();Query<Transform,Body>{w}.each([&](Entity e,Transform& t,Body& b){grid.insert(e,t.realm,t.position,b.radius);});
  Query<DropTag,Transform>{w}.each([&](Entity e,DropTag&,Transform& t){grid.insert(e,t.realm,t.position,0);});}
 void tickCombat(double now) {index();combat.run(w,grid,c,now,net::kTickSeconds,commands,events);commands.flush();}
 Entity drop(Vec2 at,Entity eligible=0,int connection=17,Realm realm=Realm::Overworld) {
  Entity e=w.create();w.add<DropTag>(e);w.add<Transform>(e,Transform{at,0,realm});
  w.add<Lifetime>(e,Lifetime{30});DropItem d;d.configIndex=c.petalIndex("basic");d.quantity=1;
  d.eligible.push_back({eligible?eligible:player,static_cast<net::ConnectionId>(connection)});w.add<DropItem>(e,d);return e;
 }
 void tickLoot(double now=1000) {index();loot.run(w,grid,c,rng,now,net::kTickSeconds,commands,events);commands.flush();}
};
}
TEST(zorr_completion_ring_burn_is_real_finite_and_survives_petal_break) {
 RingWorld r;r.equip(0,"ring");r.tickPetals(0);r.tickPetals(7000);
 auto& spawned=r.w.get<Loadout>(r.player).spawned;CHECK(!spawned.empty());if(spawned.empty())return;
 Entity petal=spawned.front();Vec2 at=r.w.get<Transform>(petal).position;
 r.w.get<Transform>(r.player).position={8000,8000};
 Entity mob=r.w.create();r.w.add<MobTag>(mob);r.w.add<Transform>(mob,Transform{at,0});
 r.w.add<Body>(mob,Body{5,1});r.w.add<Health>(mob,Health{10000,10000});
 r.w.add<Faction>(mob,Faction{Team::Hostiles,false});r.w.add<Bounty>(mob);
 r.tickCombat(8000);CHECK(r.w.has<Afflictions>(mob));if(!r.w.has<Afflictions>(mob))return;
 auto burns=r.w.get<Afflictions>(mob).burns;CHECK_EQ(burns.size(),std::size_t{1});
 if(burns.empty())return;CHECK_EQ(burns.front().perSecond,40.0);CHECK_EQ(burns.front().untilMillis,13000.0);
 r.w.destroy(petal);const double before=r.w.get<Health>(mob).current;
 for(int i=1;i<=105;++i)r.tickCombat(8000+50*i);
 const double lost=before-r.w.get<Health>(mob).current;CHECK(lost>190);CHECK(lost<=200.00001);
 const double after=r.w.get<Health>(mob).current;r.tickCombat(15000);CHECK_EQ(r.w.get<Health>(mob).current,after);
}
TEST(zorr_completion_ring_magnet_uses_strongest_active_copy_without_instant_pickup) {
 RingWorld r;r.equip(0,"ring_magnet");r.equip(1,"ring_magnet");r.tickPetals(0);r.tickPetals(7000);r.tickPetals(7050);
 const auto& mods=r.w.get<PlayerModifiers>(r.player);CHECK_EQ(mods.ringMagnetism,300.0);CHECK_EQ(mods.magnetism,0.0);
 auto drop=r.drop({5200,5000});r.tickLoot();CHECK_NEAR(r.w.get<Transform>(drop).position.x,5182.0,1e-6);CHECK(r.loot.pickups().empty());
 r.w.get<Loadout>(r.player).slots[0].configIndex=kInvalidIndex;r.w.get<Loadout>(r.player).slots[1].configIndex=kInvalidIndex;r.tickPetals(7100);
 CHECK_EQ(r.w.get<PlayerModifiers>(r.player).ringMagnetism,0.0);r.tickLoot();CHECK_NEAR(r.w.get<Transform>(drop).position.x,5182.0,1e-6);
}
TEST(zorr_completion_ring_magnet_cannot_pull_others_claims_other_maps_or_twice_per_tick) {
 RingWorld r;r.w.get<PlayerModifiers>(r.player).ringMagnetism=300;
 auto stranger=r.flower({5000,5000},18);r.w.get<PlayerModifiers>(stranger).ringMagnetism=300;
 auto shared=r.drop({5200,5000});r.w.get<DropItem>(shared).eligible.push_back({stranger,18});
 auto other=r.drop({5200,5010},Entity{9999},99);
 auto elsewhere=r.drop({5200,5020},r.player,17,Realm::Arena);
 auto claimed=r.drop({5200,5030});r.w.get<DropItem>(claimed).pickedUpBy.push_back({r.player,17});
 r.tickLoot();CHECK_NEAR(r.w.get<Transform>(shared).position.x,5182.0,1e-6);
 CHECK_EQ(r.w.get<Transform>(other).position.x,5200);CHECK_EQ(r.w.get<Transform>(elsewhere).position.x,5200);
 CHECK_EQ(r.w.get<Transform>(claimed).position.x,5200);
}
TEST(zorr_completion_ring_recipe_growth_and_localization) {
 RingWorld r;const auto ring=r.c.petalIndex("ring"),magnet=r.c.petalIndex("ring_magnet");
 CHECK(ring!=kInvalidIndex);CHECK(magnet!=kInvalidIndex);
 const auto& recipes=r.c.mob(r.c.mobIndex("bio_assembler")).npc.recipes;
 const auto& recipe=recipes[14];CHECK(!recipe.previewOnly);CHECK_EQ(recipe.inputs.front().petal,ring);
 CHECK_EQ(recipe.inputs.front().count,1);CHECK_EQ(recipe.outputs.size(),std::size_t{2});
 const auto& eternal=r.c.petalStats(magnet,Rarity::Unique);const auto& hyper=r.c.petalStats(magnet,Rarity::Apex);
 CHECK(hyper.modifiers.magnetism<=eternal.modifiers.magnetism*1.5);
 CHECK(hyper.health<=eternal.health*2.1+1e-6);CHECK(hyper.damage<=eternal.damage*3.15+1e-6);
 for(int i=0;i<kRarityCount;++i){auto tier=static_cast<Rarity>(i);const auto& s=r.c.petalStats(ring,tier);CHECK(s.reloadMillis>=3000.0);CHECK(std::isfinite(s.damage));}
 ui::setLanguage(ui::Language::Zh);CHECK_EQ(std::string(ui::petalNameTr("ring","Ring")),std::string("燃烧戒指"));
 ui::setLanguage(ui::Language::En);CHECK_EQ(std::string(ui::petalNameTr("ring","Ring")),std::string("Ring"));
}
TEST(zorr_completion_all_content_is_finite_and_imported_hyper_caps_hold) {
 const auto& c=testsupport::shippedContent();CHECK(c.petalCount()>=339);int imported=0;
 for(std::uint16_t i=0;i<c.petalCount();++i) {
  const auto& config=c.petal(i);
  const bool added=config.biomeDropOnly || config.id.rfind("void_",0)==0 ||
   config.id.rfind("arctic_",0)==0 || config.id.rfind("bio_",0)==0 || config.id.rfind("jurassic_",0)==0;
  if(added) {
   ++imported;const auto& e=c.petalStats(i,Rarity::Unique);const auto& h=c.petalStats(i,Rarity::Apex);
   if(e.damage>0)CHECK(h.damage<=e.damage*3.15+1e-5);
   if(e.health>0)CHECK(h.health<=e.health*2.1+1e-5);
  }
  for(int n=0;n<kRarityCount;++n) {
   const auto& s=c.petalStats(i,static_cast<Rarity>(n));
   for(double value:{s.damage,s.health,s.poisonPerSecond,s.radius,s.reloadMillis,s.modifiers.magnetism}) {
    CHECK(std::isfinite(value));if(added)CHECK(value>=0);
   }
  }
 }
 CHECK_EQ(imported,219);
 std::printf("  %zu petals x %d rarities finite; %d imported items meet Eternal/Hyper caps\n",c.petalCount(),kRarityCount,imported);
}
TEST(zorr_completion_ring_is_linked_to_angels_drop_table) {
 const auto& c=testsupport::shippedContent();DropTables tables;std::string error;
 CHECK(tables.load(c,testsupport::dataFile("mob_drops.json"),error));
 const auto& drops=tables.forMob(c.mobIndex("void_angel"));
 const auto ring=c.petalIndex("ring"),mirror=c.petalIndex("void_mirror");
 auto a=std::find_if(drops.begin(),drops.end(),[&](const auto& x){return x.petalIndex==ring;});
 auto b=std::find_if(drops.begin(),drops.end(),[&](const auto& x){return x.petalIndex==mirror;});
 CHECK(a!=drops.end());CHECK(b!=drops.end());
 if(a!=drops.end()&&b!=drops.end()){CHECK_EQ(a->probability,b->probability);CHECK_EQ(a->minCount,1);CHECK_EQ(a->maxCount,1);}
}
#include "canvas.h"
#include "client/render/sprites.h"
TEST(zorr_completion_ring_art_uses_original_gold_gem_and_red_blue_magnet) {
 const auto& c=testsupport::shippedContent();SpriteCache sprites;sprites.build(c,testsupport::dataDir());
 Canvas image=Canvas::createVirtual(256,128);image.clear(Color{7,7,7});
 sprites.drawPetal(image,c.petalIndex("ring"),Rarity::Common,64,64,96,0,0);
 sprites.drawPetal(image,c.petalIndex("ring_magnet"),Rarity::Common,192,64,96,0,0);
 const auto bytes=image.getImageData(0,0,256,128);int gold=0,gem=0,red=0,blue=0;
 for(std::size_t i=0;i+3<bytes.size();i+=4){const int r=bytes[i],g=bytes[i+1],b=bytes[i+2];
  gold+=(r>150&&g>80&&g<210&&b<70);gem+=(b>140&&g>100&&r<120);
  red+=(r>90&&r>g*1.25&&r>b*1.25);blue+=(b>90&&b>r*1.25&&b>g*1.25);
 }
 CHECK(gold>100);CHECK(gem>50);CHECK(red>100);CHECK(blue>100);
 if(const char* path=std::getenv("FLIX_RING_ART_RGBA")){std::FILE* f=std::fopen(path,"wb");CHECK(f!=nullptr);if(f){std::fwrite(bytes.data(),1,bytes.size(),f);std::fclose(f);}}
}
#include "shared/game/terrain.h"
TEST(zorr_completion_ring_magnet_stops_at_walls_without_teleporting_loot) {
 RingWorld r;Terrain terrain;terrain.fill(Tile::Open);terrain.setTile(10,10,Tile::Solid);
 r.loot.terrain=&terrain;r.w.get<Transform>(r.player).position=Terrain::tileCenter(8,10);
 r.w.get<PlayerModifiers>(r.player).ringMagnetism=8*kTileSize;
 const auto start=Terrain::tileCenter(12,10);const auto drop=r.drop(start);
 for(int n=0;n<100;++n)r.tickLoot(1000+n*50);
 CHECK(r.w.isAlive(drop));if(!r.w.isAlive(drop))return;
 const auto at=r.w.get<Transform>(drop).position;CHECK(at.x<start.x);
 CHECK(at.x>Terrain::tileCenter(10,10).x+kTileSize*.5);
 CHECK(!terrain.blocked(at,Realm::Overworld));CHECK(r.loot.pickups().empty());
}

TEST(zorr_completion_golden_cactus_buffs_real_pools_without_stacking_or_refilling) {
 RingWorld r;r.equip(0,"basic");r.equip(1,"golden_cactus",Rarity::Super);r.tickPetals(0);r.tickPetals(7000);
 auto find=[&](){for(auto e:r.w.get<Loadout>(r.player).spawned)if(r.w.get<PetalInstance>(e).slot==0)return e;return NULL_ENTITY;};
 auto e=find();CHECK(e!=NULL_ENTITY);if(e==NULL_ENTITY)return;
 CHECK_EQ(r.w.get<Health>(e).max,13.0);
 r.equip(2,"golden_cactus",Rarity::Super);r.tickPetals(7050);e=find();CHECK_EQ(r.w.get<Health>(e).max,13.0);
 r.w.get<Health>(e).current=5;
 r.w.get<Loadout>(r.player).slots[1].configIndex=kInvalidIndex;r.w.get<Loadout>(r.player).slots[2].configIndex=kInvalidIndex;
 r.tickPetals(7100);e=find();CHECK_EQ(r.w.get<Health>(e).max,10.0);CHECK(r.w.get<Health>(e).current<=5.0);
 const double before=r.w.get<Health>(e).current;r.equip(1,"golden_cactus",Rarity::Super);r.tickPetals(7150);e=find();
 CHECK_EQ(r.w.get<Health>(e).max,13.0);CHECK(r.w.get<Health>(e).current<=before*1.34+1e-6);
}

TEST(zorr_completion_dead_leaf_slows_reload_and_buffs_petal_health) {
 RingWorld plain;plain.equip(0,"basic");plain.tickPetals(0);plain.tickPetals(7000);
 RingWorld leaf;leaf.equip(0,"basic");leaf.equip(1,"dead_leaf",Rarity::Unique);leaf.tickPetals(0);leaf.tickPetals(7000);
 auto find=[&](RingWorld& r){for(Entity e:r.w.get<Loadout>(r.player).spawned){if(r.w.get<PetalInstance>(e).slot==0)return e;}return NULL_ENTITY;};
 Entity a=find(plain), b=find(leaf);
 CHECK(a!=NULL_ENTITY && b!=NULL_ENTITY);
 // Durability buff: leaf side basic pool max > plain
 CHECK(leaf.w.get<Health>(b).max > plain.w.get<Health>(a).max);
 // Break and compare reload: leaf reload ready later by ~5%
 leaf.w.get<Health>(b).current=0;leaf.w.add<Dead>(b);
 plain.w.get<Health>(a).current=0;plain.w.add<Dead>(a);
 leaf.tickPetals(7050);plain.tickPetals(7050);
 const double leafReady=leaf.w.get<Loadout>(leaf.player).slots[0].reloadReadyAtMillis;
 const double plainReady=plain.w.get<Loadout>(plain.player).slots[0].reloadReadyAtMillis;
 const double leafWait=leafReady-7050, plainWait=plainReady-7050;
 CHECK(plainWait>0 && leafWait>0);
 CHECK_NEAR(leafWait/plainWait, 1.05, 0.02);
}
TEST(zorr_completion_honey_addons_affect_real_bait_and_do_not_stack) {
 RingWorld r;r.equip(0,"hel_honey");r.equip(1,"hel_honey");r.equip(2,"irey");r.tickPetals(0);r.tickPetals(7000);r.tickPetals(7050);
 auto bait=r.w.create();r.w.add<Transform>(bait,Transform{{5100,5000},0});r.w.add<Body>(bait,Body{20,1});
 r.w.add<HoneyBait>(bait,HoneyBait{r.player,{},0});r.w.add<Lifetime>(bait,Lifetime{12});
 GroundEffect field;field.owner=r.player;r.w.add<GroundEffect>(bait,field);r.w.add<GroundEffectTag>(bait);
 r.w.add<Faction>(bait,Faction{Team::Players,false});r.tickPetals(7100);
 CHECK_EQ(r.w.get<GroundEffect>(bait).burnPerSecond,35.0);CHECK_EQ(r.w.get<GroundEffect>(bait).damagePerSecond,40.0);
 Entity mob=r.w.create();r.w.add<MobTag>(mob);r.w.add<Transform>(mob,Transform{{5101,5000},0});r.w.add<Body>(mob,Body{10,1});
 r.w.add<Health>(mob,Health{1000,1000});r.w.add<Faction>(mob,Faction{Team::Hostiles,false});r.w.add<Bounty>(mob);
 r.tickCombat(7150);CHECK(r.w.has<Afflictions>(mob));
 if(r.w.has<Afflictions>(mob)){CHECK(r.w.get<Afflictions>(mob).poisoned(7150));CHECK_EQ(r.w.get<Afflictions>(mob).burnPerSecond,35.0);}
 for(int i=0;i<3;++i)r.w.get<Loadout>(r.player).slots[i].configIndex=kInvalidIndex;
 r.tickPetals(7200);CHECK_EQ(r.w.get<GroundEffect>(bait).radius,0.0);
}

TEST(ring_speed_pickup_time_from_outer_edge) {
 for (const auto rarity : {Rarity::Common, Rarity::Unique, Rarity::Apex}) {
  RingWorld r;
  r.equip(0, "ring_magnet", rarity);
  r.tickPetals(0); r.tickPetals(7000); r.tickPetals(7050);
  const double reach = r.w.get<PlayerModifiers>(r.player).ringMagnetism;
  CHECK(reach > 0);
  const auto drop = r.drop({5000 + reach - 1, 5000});
  int ticks = 0;
  for (; ticks < 400 && r.w.isAlive(drop); ++ticks) r.tickLoot(1000 + ticks * net::kTickMillis);
  CHECK(!r.w.isAlive(drop));
  std::printf("PICKUP %s range=%.0f ticks=%d sec=%.2f\n", rarityName(rarity), reach,
              ticks, ticks * net::kTickSeconds);
  CHECK(ticks * net::kTickSeconds < 2.6);
  CHECK(ticks > 1);
 }
}
TEST(ring_speed_still_cannot_pull_a_drop_through_a_wall) {
 RingWorld r;
 r.equip(0, "ring_magnet", Rarity::Apex);
 r.tickPetals(0); r.tickPetals(7000); r.tickPetals(7050);
 Terrain terrain;
 terrain.fill(Tile::Open);
 const int column = static_cast<int>(5000 / kTileSize) + 1;
 const int row = static_cast<int>(5000 / kTileSize);
 terrain.setTile(column, row, Tile::Solid);
 r.loot.terrain = &terrain;
 const double wallEnd = (column + 1) * kTileSize;
 const auto drop = r.drop({wallEnd + 125, 5000});
 for (int tick = 0; tick < 80; ++tick) r.tickLoot(1000 + tick * net::kTickMillis);
 CHECK(r.w.isAlive(drop));
 if (r.w.isAlive(drop)) {
  CHECK(r.w.get<Transform>(drop).position.x >= wallEnd);
  CHECK(r.loot.pickups().empty());
 }
}

TEST(zorr_completion_apex_magnet_picks_up_further_and_keeps_its_defence_bonus) {
    for (const auto rarity : {Rarity::PermanentHyper, Rarity::Ascendant}) {
        RingWorld r;
        r.equip(0, "magnet", rarity);
        r.tickPetals(0);
        r.tickPetals(7000);
        const bool apex = rarity == Rarity::Ascendant;
        const double expected = apex ? 1650.0 : 1500.0;
        const auto id = r.c.petalIndex("magnet");
        CHECK_NEAR(r.c.petalStats(id, rarity).modifiers.magnetism, expected, 1e-8);
        CHECK_NEAR(r.w.get<PlayerModifiers>(r.player).magnetism, expected, 1e-8);
        const double base = 2.0 * r.w.get<Body>(r.player).radius;
        r.drop({5000.0 + base + 1575.0, 5000.0});
        r.tickLoot(7050);
        CHECK_EQ(r.loot.pickups().size(), apex ? std::size_t{1} : std::size_t{0});
        r.w.get<PlayerInput>(r.player).current.flags = net::InputDefend;
        r.tickPetals(7100);
        CHECK_NEAR(r.w.get<PlayerModifiers>(r.player).magnetism, expected * 1.12, 1e-8);
    }
}

TEST(zorr_completion_apex_ring_magnet_pulls_loot_from_beyond_ph_range) {
    for (const auto rarity : {Rarity::PermanentHyper, Rarity::Ascendant}) {
        RingWorld r;
        r.equip(0, "ring_magnet", rarity);
        r.tickPetals(0);
        r.tickPetals(7000);
        const bool apex = rarity == Rarity::Ascendant;
        const double expected = apex ? 3190.0 : 2900.0;
        const auto id = r.c.petalIndex("ring_magnet");
        CHECK_NEAR(r.c.petalStats(id, rarity).modifiers.magnetism, expected, 1e-8);
        CHECK_NEAR(r.w.get<PlayerModifiers>(r.player).ringMagnetism, expected, 1e-8);
        CHECK_NEAR(r.w.get<PlayerModifiers>(r.player).magnetism, 0.0, 1e-8);
        const double at = 8000.0;
        const auto drop = r.drop({at, 5000.0});
        r.tickLoot(7050);
        const double moved = apex ? expected / 2.5 * net::kTickSeconds : 0.0;
        CHECK_NEAR(r.w.get<Transform>(drop).position.x, at - moved, 1e-6);
        CHECK(r.loot.pickups().empty());
    }
}
