#include "test.h"
#include "test_data.h"
#include "server/systems/spawning.h"
#include "server/systems/loot.h"
#include <map>
#include <string>
#include <utility>
using namespace flix;
namespace {
const ContentRegistry& data(){static ContentRegistry c;static bool ready=false;if(!ready){std::string e;CHECK(c.loadFiles(testsupport::dataFile("mobs.json"),testsupport::dataFile("petals.json"),e));ready=true;}return c;}
int population(World&w,Realm realm,Rarity rarity){int n=0;Query<MobTag,MobType,Transform,AmbientMob> q{w};q.without<Dead>();q.each([&](Entity e,MobTag&,MobType& t,Transform& at,AmbientMob&){const auto* b=w.tryGet<BodySegment>(e);if(!w.has<HoleTether>(e)&&!w.has<SuperFarmMob>(e)&&at.realm==realm&&t.rarity==rarity&&(!b||b->chainHead==e))++n;});return n;}
// A natural Hyper may come out Ascendant and still fills the biome's Hyper slot.
int hyperSlot(World&w,Realm realm){int n=0;Query<MobTag,MobType,Transform,AmbientMob> q{w};q.without<Dead>();q.each([&](Entity e,MobTag&,MobType& t,Transform& at,AmbientMob& ambient){const auto* b=w.tryGet<BodySegment>(e);const bool hyper=t.rarity==Rarity::Apex||(t.rarity==Rarity::Ascendant&&ambient.zone!=kInvalidIndex);if(!w.has<HoleTether>(e)&&!w.has<SuperFarmMob>(e)&&at.realm==realm&&hyper&&(!b||b->chainHead==e))++n;});return n;}
}
TEST(hyper_mobs_keep_xp_speed_and_hyper_pollen_uses_two_x_eternal){
    for(std::uint16_t i=0;i<data().mobCount();++i){const auto e=data().mobStats(i,Rarity::Unique),h=data().mobStats(i,Rarity::Apex);
        CHECK_NEAR(h.health,e.health*18,std::max(1e-5,h.health*1e-12));CHECK_NEAR(h.damage,e.damage*12,1e-5);
        CHECK_NEAR(e.health,data().mob(i).health*14244000.0,std::max(1e-5,e.health*1e-12));
        CHECK_NEAR(data().mobStats(i,Rarity::Super).health,data().mob(i).health*1187000.0,1e-5);
        CHECK_NEAR(h.xp,e.xp*3,1e-5);
        CHECK_NEAR(h.speed,e.speed,1e-9);CHECK_NEAR(h.chaseSpeed,e.chaseSpeed,1e-9);CHECK_NEAR(h.armor,e.armor*4,1e-9);}
    const auto pollen=data().petalStats(data().petalIndex("pollen"),Rarity::Apex);
    const auto eternalPollen=data().petalStats(data().petalIndex("pollen"),Rarity::Unique);
    CHECK_NEAR(pollen.damage,eternalPollen.damage*3.0,1e-9);
    CHECK_NEAR(pollen.health,eternalPollen.health*2.0,1e-9);
    CHECK_EQ(pollen.count,3);CHECK_NEAR(pollen.reloadMillis,2000,1e-9);
}
TEST(hyper_petal_combat_caps_preserve_the_authored_hammer_exception){
    for(std::uint16_t i=0;i<data().petalCount();++i){
        const auto e=data().petalStats(i,Rarity::Unique),h=data().petalStats(i,Rarity::Apex);
        for(const auto field:{&PetalStats::damage,&PetalStats::health,&PetalStats::heal,
            &PetalStats::passiveHealPerSecond,&PetalStats::lightningDamagePerHit,
            &PetalStats::flowerHealthBonus,&PetalStats::armorReduction,&PetalStats::shield,
            &PetalStats::maxMana,&PetalStats::requiredMana,&PetalStats::petManaCost}){
            // Protocol 92 deliberately buffs the contested H hammer after
            // the general cap. Verify its authored values instead of treating
            // this existing exception as a regression in every other petal.
            if(data().petal(i).id=="mjolnir" && field==&PetalStats::health){
                CHECK_NEAR(h.health,e.health*2.25,1e-7);continue;
            }
            if(data().petal(i).id=="mjolnir" && field==&PetalStats::lightningDamagePerHit){
                CHECK_NEAR(h.lightningDamagePerHit,60.0*petalStrengthScale(Rarity::Apex),1e-7);continue;
            }
            if(e.*field>0) {
                if(h.*field>e.*field*1.5+std::max(1e-9,e.*field*1e-12))
                    std::fprintf(stderr,"Hyper cap: %s E=%g H=%g\n",data().petal(i).id.c_str(),e.*field,h.*field);
                CHECK(h.*field<=e.*field*1.5+std::max(1e-9,e.*field*1e-12));
            }
        }
    }
    for(const auto id:{"pollen","battery","soil","leaf","rose","lightning","bone"}){
        const auto index=data().petalIndex(id);CHECK(index!=kInvalidIndex);
        const auto e=data().petalStats(index,Rarity::Unique),h=data().petalStats(index,Rarity::Apex);
        if(e.damage>0)CHECK_NEAR(h.damage,e.damage*1.5,std::max(1e-8,e.damage*1e-12));
    }
    CHECK_NEAR(petalStatScale(Rarity::Unique),6561,1e-9);
    CHECK_NEAR(petalStrengthScale(Rarity::Apex),9841.5,1e-9);
}
TEST(hyper_one_per_regular_map_preserves_supers_eternals_and_waits_thirty_minutes){
    WorldMaps maps;Terrain terrain;std::string error;CHECK(maps.load(testsupport::dataDir(),&terrain,error));
    World w;CommandBuffer commands{w};SpawnSystem spawn;spawn.worldMaps=&maps;Rng rng{5832};
    auto step=[&](double now){spawn.run(w,terrain,data(),std::vector<RealmPoint>{},rng,now,net::kTickSeconds,commands);commands.flush();};
    step(1000);for(auto& clock:spawn.bossClocks()){clock.uniqueReadyMillis=0;clock.apexReadyMillis=0;}
    for(int i=0;i<30;++i)step(2000+i*500);
    Entity boss=NULL_ENTITY;bool found=false;const auto garden=maps.realmOfId("garden",found);CHECK(found);
    // Alternate layouts share their biome's slots; a rift fields no natural boss.
    std::map<std::string,std::pair<int,int>> bosses;
    for(const auto& map:maps.maps())if(!maps.isPrestige(map.realm())&&map.usesReferenceSpawnCurve()){
        CHECK_EQ(population(w,map.realm(),Rarity::Super),10);
        if(map.id().rfind("rift_",0)==0)continue;
        auto& biome=bosses[map.biome()];
        biome.first+=hyperSlot(w,map.realm());biome.second+=population(w,map.realm(),Rarity::Unique);
    }
    for(const auto& [biome,count]:bosses){CHECK_EQ(count.first,1);CHECK_EQ(count.second,3);}
    CHECK_EQ(bosses.size(),std::size_t(10));Query<MobTag,MobType,Transform,AmbientMob> q{w};q.without<Dead>();
    q.each([&](Entity e,MobTag&,MobType&t,Transform&at,AmbientMob& ambient){const auto* b=w.tryGet<BodySegment>(e);
        const bool hyper=t.rarity==Rarity::Apex||(t.rarity==Rarity::Ascendant&&ambient.zone!=kInvalidIndex);
        if(hyper&&at.realm==garden&&(!b||b->chainHead==e))boss=e;});
    CHECK(boss!=NULL_ENTITY);if(boss==NULL_ENTITY)return;
    w.get<Health>(boss).current*=.5;step(7200000);CHECK(w.isAlive(boss));CHECK(!w.has<Dead>(boss));
    w.add<Dead>(boss);step(7201000);CHECK_NEAR(spawn.bossCooldownLeft(Rarity::Apex,garden,7201000),1800000,1e-6);
    w.destroy(boss);step(9000999);CHECK_EQ(hyperSlot(w,garden),0);
    for(int i=0;i<20;++i)step(9001000+i*500);CHECK_EQ(hyperSlot(w,garden),1);
}

TEST(expansion_biomes_stock_natural_hypers_and_share_the_bio_slot) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    const bool loaded = maps.load(testsupport::dataDir(), &terrain, error);
    CHECK(loaded);
    if (!loaded) return;

    World world;
    CommandBuffer commands{world};
    SpawnSystem spawn;
    spawn.worldMaps = &maps;
    Rng rng{88113};
    const auto step = [&](double now) {
        spawn.run(world, terrain, data(), std::vector<RealmPoint>{}, rng, now, net::kTickSeconds, commands);
        commands.flush();
    };
    step(1000.0);
    for (auto& clock : spawn.bossClocks()) clock.apexReadyMillis = 0.0;
    step(2000.0);
    for (const std::string_view biome : {"bio", "jurassic", "zorr_void", "zorr_arctic",
                                          "zorr_glacier", "zorr_polar"}) {
        int found = 0;
        for (const auto& map : maps.maps()) {
            if (map.biome() == biome && !map.prestige())
                found += hyperSlot(world, map.realm());
        }
        if (found != 1) std::fprintf(stderr, "Expansion boss slots in %s: %d\n", biome.data(), found);
        CHECK_EQ(found, 1);
    }

    std::vector<Realm> seen;
    bool sawApex = false;
    double now = 2000.0;
    for (int round = 0; round < 36 && (seen.size() < 3 || !sawApex); ++round) {
        Entity boss = NULL_ENTITY;
        Query<MobTag, MobType, Transform, AmbientMob>{world}.without<Dead>().each(
            [&](Entity e, MobTag&, MobType& mob, Transform& at, AmbientMob& ambient) {
                const auto* map = maps.forRealm(at.realm);
                const auto* segment = world.tryGet<BodySegment>(e);
                if (!map || map->biome() != "bio" || map->prestige() || ambient.zone == kInvalidIndex ||
                    (segment && segment->chainHead != e) ||
                    (mob.rarity != Rarity::Apex && mob.rarity != Rarity::Ascendant)) return;
                CHECK(boss == NULL_ENTITY);
                boss = e;
                sawApex |= mob.rarity == Rarity::Ascendant;
                if (std::find(seen.begin(), seen.end(), at.realm) == seen.end()) seen.push_back(at.realm);
            });
        if (boss == NULL_ENTITY) break;
        world.destroy(boss);
        now += 1000.0;
        step(now);
        for (const auto& map : maps.maps())
            if (map.biome() == "bio" && !map.prestige()) CHECK_EQ(hyperSlot(world, map.realm()), 0);
        now += kApexSpawnCooldownMillis;
        step(now);
    }
    CHECK_EQ(seen.size(), std::size_t(3));
    CHECK(sawApex);
}
TEST(hyper_top_contributor_gets_thirty_nine_trials_of_materials_below_hyper){
    World w;CommandBuffer commands{w};EventQueue events;SpatialGrid grid;LootSystem loot;Rng rng{81927};
    const auto player=w.create();w.add<PlayerTag>(player);w.add<Transform>(player,Transform{{10000,10000},0});
    w.add<Body>(player,Body{25,1});w.add<Health>(player,Health{100,100});
    for(int n=0;n<100;++n){const auto corpse=w.create();w.add<MobTag>(corpse);
        w.add<MobType>(corpse,MobType{data().mobIndex("sandstorm"),Rarity::Apex,1});
        w.add<Transform>(corpse,Transform{{1000,1000},0});w.add<Health>(corpse,Health{0,100});
        Bounty bounty;bounty.credit(player,100);w.add<Bounty>(corpse,bounty);w.add<Dead>(corpse,Dead{player});
        loot.run(w,grid,data(),rng,n,0,commands,events);commands.flush();w.destroy(corpse);}
    int total=0,eternal=0;Query<DropTag,DropItem> q{w};q.each([&](Entity,DropTag&,DropItem& item){
        CHECK(rarityIndex(item.rarity)<rarityIndex(Rarity::Apex));total+=item.quantity;if(item.rarity==Rarity::Unique)eternal+=item.quantity;});
    CHECK_EQ(total-eternal,100*39*3);CHECK(eternal>20&&eternal<100);
}

TEST(digger_uses_the_reduced_health_and_damage_across_rarities) {
    const auto id = data().mobIndex("digger");
    CHECK(id != kInvalidIndex);
    if (id == kInvalidIndex) return;
    CHECK_NEAR(data().mobStats(id, Rarity::Common).health, 200.0, 1e-6);
    CHECK_NEAR(data().mobStats(id, Rarity::Common).damage, 24.0, 1e-6);
    const auto super = data().mobStats(id, Rarity::Super);
    CHECK_NEAR(super.health, 200.0 * 1187000.0, 1e-3);
    CHECK_NEAR(super.damage, 24.0 * kMobDamageScale[rarityIndex(Rarity::Super)], 1e-6);
    CHECK_NEAR(data().mob(id).petHealthScale, 0.5, 1e-6);
    CHECK_NEAR(data().mob(id).petDamageScale, 0.5, 1e-6);
}

TEST(super_farm_respects_map_caps_and_refills_the_deepest_sewer_room){
    ContentRegistry c;WorldMaps maps;Terrain terrain;std::string error;
    CHECK(c.loadFiles(testsupport::dataFile("mobs.json"),testsupport::dataFile("petals.json"),error));
    CHECK(maps.load(testsupport::dataDir(),&terrain,error));
    World w;CommandBuffer commands{w};SpawnSystem spawn;spawn.worldMaps=&maps;Rng rng{2853};
    auto step=[&](double at){spawn.run(w,terrain,c,std::vector<RealmPoint>{},rng,at,net::kTickSeconds,commands);commands.flush();};
    step(1000);step(1500);Entity victim=NULL_ENTITY;Realm garden=Realm::Overworld;int mapsChecked=0;
    std::vector<Entity> deepSewers;
    const MapData* sewerMap=nullptr;double sewerHighest=0;
    for(const auto& map:maps.maps()){
        if(maps.isPrestige(map.realm())||!map.usesReferenceSpawnCurve()||!supportsSuperFarm(map.id()))continue;
        int farm=0;double highest=-1;
        for(const auto& band:map.elements())if(band.isSpawnBand()&&!band.singular&&band.forceRarity<0)highest=std::max(highest,band.difficulty);
        if(map.id()=="sewers"){sewerMap=&map;sewerHighest=highest;}
        Query<SuperFarmMob,MobType,Transform> q{w};q.without<Dead>();q.each([&](Entity e,SuperFarmMob&,MobType& type,Transform& at){
            if(at.realm!=map.realm())return;++farm;CHECK(type.rarity==Rarity::Super);
            bool inside=false;for(const auto& band:map.elements())if(band.isSpawnBand()&&!band.singular&&band.forceRarity<0&&band.difficulty>=highest-kSuperFarmDifficultyWindow)
                inside=inside||zoneContains(band.bounds,band.polygon,at.position);CHECK(inside);
            if(map.id()=="garden"){victim=e;garden=map.realm();}
            if(map.id()=="sewers")for(const auto& band:map.elements())
                if(band.isSpawnBand()&&!band.singular&&band.forceRarity<0&&band.difficulty==highest&&zoneContains(band.bounds,band.polygon,at.position)){deepSewers.push_back(e);break;}
        });
        const int mapCap = map.templateId() == "hel" ? 36 :
            map.templateId() == "computer" ? kSuperFarmPopulation + 6 : kSuperFarmPopulation;
        CHECK(farm > 0);
        CHECK(farm <= mapCap);
        CHECK_EQ(population(w, map.realm(), Rarity::Super), 10);
        ++mapsChecked;
    }
    CHECK(mapsChecked>=10);CHECK(victim!=NULL_ENTITY);
    CHECK(deepSewers.size() >= static_cast<std::size_t>(kSuperFarmPopulation / 3));
    for (const auto& event : spawn.bossSpawns) CHECK(event.rarity != Rarity::Super);
    auto count=[&](){int n=0;Query<SuperFarmMob,Transform> q{w};q.without<Dead>();q.each([&](Entity,SuperFarmMob&,Transform& at){if(at.realm==garden)++n;});return n;};
    const auto deepCount=[&](){int n=0;if(!sewerMap)return n;Query<SuperFarmMob,Transform> q{w};q.without<Dead>();
        q.each([&](Entity,SuperFarmMob&,Transform& at){if(at.realm!=sewerMap->realm())return;
            for(const auto& band:sewerMap->elements())if(band.isSpawnBand()&&!band.singular&&band.forceRarity<0&&band.difficulty==sewerHighest&&zoneContains(band.bounds,band.polygon,at.position)){++n;break;}});return n;};
    const int before=count();w.add<Dead>(victim);for(auto e:deepSewers)w.add<Dead>(e);
    step(2000);CHECK_EQ(count(),before-1);CHECK_EQ(deepCount(),0);
    step(31999);CHECK_EQ(count(),before-1);CHECK_EQ(deepCount(),0);
    step(32500);CHECK_EQ(count(),before);
    CHECK(deepCount() >= kSuperFarmPopulation / 3);
}
TEST(super_farm_health_and_loot_budget_change_without_touching_eternal_or_hyper_rewards){
    for(std::uint16_t i=0;i<data().mobCount();++i){const auto& mob=data().mob(i);
        CHECK_NEAR(data().mobStats(i,Rarity::Super).health,mob.health*1187000.0,std::max(1e-6,mob.health*.001));
        CHECK_NEAR(data().mobStats(i,Rarity::Unique).health,mob.health*14244000.0,std::max(1e-6,mob.health*.001));
    }
    CHECK_EQ(baseLootTrialsForRarity(Rarity::Super),20);CHECK_EQ(baseLootTrialsForRarity(Rarity::Unique),30);CHECK_EQ(baseLootTrialsForRarity(Rarity::Apex),30);
    CHECK(kAnnouncedRarity==Rarity::Unique);
}

TEST(todo1105_dedicated_super_realm_is_pickable_and_stocks_only_supers) {
    WorldMaps maps;
    Terrain terrain;
    std::string error;
    CHECK(maps.load(testsupport::dataDir(), &terrain, error));
    bool found = false;
    const Realm realm = maps.realmOfId("super", found);
    CHECK(found);
    const auto& choices = maps.spawnChoices();
    CHECK(std::any_of(choices.begin(), choices.end(), [&](const SpawnChoice& door) {
        return door.realm == realm && door.biome == "super" && door.pickable;
    }));

    World world;
    CommandBuffer commands{world};
    SpawnSystem spawn;
    spawn.worldMaps = &maps;
    Rng rng{2853};
    for (int i = 0; i < 20; ++i) {
        spawn.run(world, terrain, data(), std::vector<RealmPoint>{}, rng,
                  1000 + i * 500, net::kTickSeconds, commands);
        commands.flush();
    }
    const auto farmMobs = [&] {
        std::vector<Entity> heads;
        Query<MobTag, MobType, Transform> mobs{world};
        mobs.without<Dead>();
        mobs.each([&](Entity entity, MobTag&, MobType& type, Transform& at) {
            if (at.realm != realm) return;
            CHECK(type.rarity == Rarity::Super);
            const auto* segment = world.tryGet<BodySegment>(entity);
            if (!segment || segment->chainHead == entity) heads.push_back(entity);
        });
        return heads;
    };
    const auto initial = farmMobs();
    CHECK_EQ(initial.size(), static_cast<std::size_t>(kSuperFarmPopulation));
    for (const Entity mob : initial) world.add<Dead>(mob);

    const auto* map = maps.forRealm(realm);
    CHECK(map != nullptr);
    if (!map) return;
    const auto viewer = map->defaultSpawn(rng, terrain);
    const auto step = [&](double now) {
        spawn.run(world, terrain, data(), std::vector<RealmPoint>{{viewer, realm}}, rng,
                  now, net::kTickSeconds, commands);
        commands.flush();
    };
    step(11000.0);
    CHECK(farmMobs().empty());
    step(40999.0);
    CHECK(farmMobs().empty());
    for (double now = 41000.0; now <= 45000.0; now += 500.0) step(now);
    CHECK_EQ(farmMobs().size(), static_cast<std::size_t>(kSuperFarmPopulation));
}

TEST(termite_mound_backlog_prunes_idle_duplicates_but_keeps_occupied_instances){
    const auto& c=data();World w;Terrain terrain;CommandBuffer commands{w};SpawnSystem spawn;Rng rng{25};
    const auto index=c.mobIndex("termite_mound");std::vector<Entity> nests;
    for(int n=0;n<4;++n)nests.push_back(spawn.spawnMob(w,terrain,c,index,Rarity::Rare,{1000.0+n*300,1000},Realm::Overworld,0,rng));
    for(const auto e:nests)CHECK(e!=NULL_ENTITY);
    auto& occupied=w.get<DungeonEntrance>(nests[3]);occupied.claimed=true;occupied.realm=Realm::Arena;
    const auto p=w.create();w.add<PlayerTag>(p);w.add<Transform>(p,Transform{{1000,1000},0,Realm::Arena});w.add<Dead>(p);
    spawn.run(w,terrain,c,{},rng,1000,net::kTickSeconds,commands);commands.flush();
    CHECK(w.isAlive(nests[3]));int living=0;for(const auto e:nests)living+=w.isAlive(e);CHECK_EQ(living,1);
    CHECK(!w.has<Dead>(nests[3]));
}

TEST(jungle_centipede_spawns_a_complete_networked_body_chain){
    const auto& c=data();const auto index=c.mobIndex("jungle_centipede");const auto& config=c.mob(index);
    CHECK_EQ(config.segmentCount,kCentipedeSegmentCount);CHECK(config.segmentBodyIndex!=kInvalidIndex);
    CHECK(c.mob(config.segmentBodyIndex).chainBody);CHECK(c.mob(config.segmentBodyIndex).groups.empty());
    World w;Terrain terrain;SpawnSystem spawn;Rng rng{66};NetIdAllocator ids;spawn.netIds=&ids;
    const auto head=spawn.spawnMob(w,terrain,c,index,Rarity::Super,{3000,3000},Realm::Overworld,0,rng);
    CHECK(head!=NULL_ENTITY);int bodies=0;Query<BodySegment,MobType> q{w};q.each([&](Entity e,BodySegment& part,MobType& type){
        if(part.chainHead!=head||e==head)return;++bodies;CHECK_EQ(type.configIndex,config.segmentBodyIndex);CHECK(type.rarity==Rarity::Super);CHECK(w.has<Replicated>(e));
    });CHECK_EQ(bodies,kCentipedeSegmentCount);
}

TEST(a_new_centipede_chain_stays_on_one_side_of_a_wall) {
    const auto& content = data();
    const auto index = content.mobIndex("jungle_centipede");
    const auto& config = content.mob(index);
    Terrain terrain;
    constexpr int wallX = 20;
    for (int y = 0; y < kTilesPerAxis; ++y) terrain.setTile(wallX, y, Tile::Solid);
    const double east = (wallX + 1) * kTileSize;
    const Vec2 origin{east + 15.0, 20 * kTileSize};
    int shortened = 0;
    for (int seed = 1; seed <= 12; ++seed) {
        World world;
        SpawnSystem spawn;
        Rng rng{static_cast<std::uint64_t>(seed)};
        const Entity head = spawn.spawnMob(world, terrain, content, index, Rarity::Apex,
                                          origin, Realm::Overworld, 0, rng);
        CHECK(head != NULL_ENTITY);
        int links = 0;
        Query<BodySegment, Transform>{world}.each([&](Entity part, BodySegment& link, Transform& at) {
            if (part == head || link.chainHead != head) return;
            ++links;
            const Vec2 leader = world.get<Transform>(link.ahead).position;
            CHECK(at.position.x >= east);
            CHECK(!terrain.blocked(at.position, Realm::Overworld));
            CHECK(!terrain.segmentBlocked(leader, at.position, Realm::Overworld));
            if ((at.position - leader).length() < link.spacing - 1.0) ++shortened;
        });
        CHECK_EQ(links, config.segmentCount);
    }
    CHECK(shortened > 0);
}

TEST(centipede_chains_stay_complete_at_capacity_and_touch_at_actual_body_size) {
    const auto& content = data();
    const auto species = {"centipede", "jungle_centipede", "desert_centipede",
                          "evil_centipede", "hel_centipede"};
    Terrain terrain;
    for (const auto* id : species) {
        for (const auto rarity : {Rarity::Common, Rarity::Super, Rarity::Unique, Rarity::Apex}) {
            World world;
            SpawnSystem spawn;
            Rng rng{66};
            const auto index = content.mobIndex(id);
            const auto& config = content.mob(index);
            CHECK_EQ(config.segmentCount, kCentipedeSegmentCount);
            const auto spawnChain = [&] {
                return spawn.spawnMob(world, terrain, content, index, rarity,
                                      {3000, 3000}, Realm::Overworld, 0, rng);
            };

            spawn.mobCap = kCentipedeSegmentCount;
            CHECK(spawnChain() == NULL_ENTITY);
            CHECK_EQ(Query<MobTag>{world}.count(), 0u);

            spawn.mobCap = 1 + kCentipedeSegmentCount;
            const Entity head = spawnChain();
            CHECK(head != NULL_ENTITY);
            int count = 0;
            Query<BodySegment, Body, Transform>{world}.each([&](Entity part, BodySegment& link, Body& body, Transform& at) {
                if (part == head || link.chainHead != head) return;
                ++count;
                const auto& leader = world.get<Body>(link.ahead);
                const auto& position = world.get<Transform>(link.ahead);
                CHECK_NEAR(link.spacing, leader.radius + body.radius + kGardnSegmentGap, 1e-6);
                // The largest chains can start compressed against the map bounds.
                if (rarity == Rarity::Common && config.minRarity == Rarity::Common) {
                    CHECK_NEAR((at.position - position.position).length(), link.spacing, 1e-6);
                }
            });
            CHECK_EQ(count, kCentipedeSegmentCount);
            CHECK(spawnChain() == NULL_ENTITY);
        }
    }
}

#include "shared/game/tiled_map.h"
TEST(entrance_tile_objects_survive_map_loading_with_their_authored_size) {
 for(const auto* name:{"garden.tmj","desert.tmj","computer.tmj"}) {
  TiledMap map;std::string error;CHECK(map.load(testsupport::dataFile(name),error));
  const std::string expected=std::string(name)=="garden.tmj"?"factory_entrance.svg":std::string(name)=="desert.tmj"?"pyramid.svg":"factory_exit.svg";
  bool found=false;for(const auto& d:map.decorations())if(map.artFiles()[d.art]==expected){found=true;CHECK(d.width>300);CHECK(d.height>300);}
  CHECK(found);
 }
}
