#include "test.h"
#include "test_data.h"
#include "server/systems/npcs.h"
#include <set>
using namespace flix;
namespace {
const ContentRegistry& data(){static ContentRegistry c;static bool ready=false;if(!ready){std::string e;CHECK(c.loadFiles(testsupport::dataFile("mobs.json"),testsupport::dataFile("petals.json"),e));ready=true;}return c;}
Entity machine(World& w){Entity result=NULL_ENTITY;Query<NpcTag,Npc> q{w};q.each([&](Entity e,NpcTag&,Npc& n){if(n.service==NpcService::Assembler){CHECK(result==NULL_ENTITY);result=e;}});return result;}
}
TEST(assembler_recipe_pool_contains_all_reference_recipes_and_fragment_sawblade){
    const auto& c=data();const auto& config=c.mob(c.mobIndex("assembler"));
    CHECK_EQ(config.npc.recipes.size(),17u);int moonRecipes=0;bool fragment=false,pentagon=false;
    for(const auto& recipe:config.npc.recipes){
        bool moon=false;
        for(const auto& item:recipe.inputs){CHECK(item.petal!=kInvalidIndex);moon=moon||c.petal(item.petal).id=="moon_rock";}
        for(const auto& item:recipe.outputs){CHECK(item.petal!=kInvalidIndex);moon=moon||c.petal(item.petal).id=="moon_rock";}
        moonRecipes+=moon;
        for(const auto& item:recipe.outputs){
            pentagon=pentagon||c.petal(item.petal).id=="pentagon";
            if(c.petal(item.petal).id=="sawblade")for(const auto& input:recipe.inputs)
                fragment=fragment||c.petal(input.petal).id=="fragment";
        }
    }
    CHECK_EQ(moonRecipes,2);
    CHECK(fragment);CHECK(pentagon);
    const auto& saw=config.npc.recipes[12];
    CHECK_EQ(saw.takes(c.petalIndex("cogwheel")),1u);
    CHECK_EQ(saw.takes(c.petalIndex("fragment")),20u);
    for (const auto* machineType : {"assembler", "cyber_assembler"}) {
        for (const auto& recipe : c.mob(c.mobIndex(machineType)).npc.recipes)
            CHECK(!recipe.outputRarity);
    }
}
TEST(assembler_refreshes_after_ten_wall_minutes_even_when_simulation_lags){
    World w;Terrain terrain;NpcSystem npcs;NetIdAllocator ids;npcs.netIds=&ids;
    auto e=npcs.spawnNpc(w,terrain,data(),data().mobIndex("assembler"),Rarity::Super,{1000,1000},Realm::Overworld,1000);
    const auto first=w.get<Npc>(e).recipe;const auto netId=w.get<NetId>(e).value;
    constexpr double slowSimulation = 1000 + kAssemblerRecipeRefreshMillis * 0.5;
    npcs.run(w,terrain,data(),{},slowSimulation-1,1000+kAssemblerRecipeRefreshMillis-1);
    CHECK(w.isAlive(e));
    npcs.run(w,terrain,data(),{},slowSimulation,1000+kAssemblerRecipeRefreshMillis);
    CHECK(!w.isAlive(e));
    const auto current=machine(w);CHECK(current!=NULL_ENTITY);
    CHECK(w.get<Npc>(current).recipe!=first);CHECK(w.get<NetId>(current).value!=netId);
    CHECK(w.get<Npc>(current).rarity==Rarity::Super);
    CHECK_NEAR(w.get<Npc>(current).recipeRefreshAtMillis,1000+2*kAssemblerRecipeRefreshMillis,1e-8);
}
TEST(assembler_refreshes_while_players_wait_and_follows_its_announced_next_recipe){
    World w;Terrain terrain;NpcSystem npcs;
    const auto e=npcs.spawnNpc(w,terrain,data(),data().mobIndex("assembler"),Rarity::Super,{1000,1000},Realm::Overworld,0);
    const auto at=w.get<Transform>(e);const auto next=w.get<Npc>(e).nextRecipe;
    CHECK_NEAR(w.get<Npc>(e).recipeRefreshAtMillis,kAssemblerRecipeRefreshMillis,1e-8);
    npcs.run(w,terrain,data(),{{at.position,at.realm}},kAssemblerRecipeRefreshMillis);
    CHECK(!w.isAlive(e));
    const auto current=machine(w);
    CHECK_EQ(w.get<Npc>(current).recipe,next);
    CHECK_NEAR(w.get<Npc>(current).recipeRefreshAtMillis,2*kAssemblerRecipeRefreshMillis,1e-8);
}
TEST(assembler_rotation_visits_every_available_recipe_without_random_starvation){
    World w;Terrain terrain;NpcSystem npcs;
    auto e=npcs.spawnNpc(w,terrain,data(),data().mobIndex("assembler"),Rarity::Super,{1000,1000},Realm::Overworld,0);
    std::set<unsigned> seen;
    for(int i=0;i<17;++i){seen.insert(w.get<Npc>(e).recipe);npcs.run(w,terrain,data(),{},(i+1)*kAssemblerRecipeRefreshMillis);e=machine(w);}
    CHECK_EQ(seen.size(),17u);
}

TEST(zorr_base_material_routes_rotate_at_each_native_factory_tier_without_starvation){
    for(const char* type:{"assembler","cyber_assembler"})for(int tier=0;tier<=ladderIndex(Rarity::Unique);++tier){
        World w;Terrain terrain;NpcSystem npcs;
        Entity e=npcs.spawnNpc(w,terrain,data(),data().mobIndex(type),clampRarity(tier),{1000,1000},Realm::Overworld,0);
        std::set<std::string> outputs;
        std::set<unsigned> visited;
        const auto first=w.get<Npc>(e).recipe;
        bool closed=false;
        for(int step=0;step<17;++step){
            const auto state=w.get<Npc>(e);
            CHECK(visited.insert(state.recipe).second);
            const auto& recipe=data().mob(state.configIndex).npc.recipes[state.recipe];
            for(const auto& item:recipe.outputs)outputs.insert(data().petal(item.petal).id);
            npcs.run(w,terrain,data(),{},(step+1)*kAssemblerRecipeRefreshMillis);e=machine(w);
            CHECK(e!=NULL_ENTITY);if(e==NULL_ENTITY)break;
            CHECK_EQ(w.get<Npc>(e).recipe,state.nextRecipe);
            if(w.get<Npc>(e).recipe==first){closed=true;break;}
        }
        for(const char* id:{"gas","blood_rose","silk","marble"})CHECK(outputs.count(id)==1);
        CHECK(closed);
        std::printf("%s tier %d: %zu recipes, %.0f min maximum cycle\n",type,tier,visited.size(),visited.size()*kAssemblerRecipeRefreshMillis/60000);
    }
}
TEST(assembler_rotation_does_not_offer_petals_missing_at_the_machine_rarity){
    for(int tier=0;tier<kLadderRarityCount;++tier){
        World w;Terrain terrain;NpcSystem npcs;
        auto e=npcs.spawnNpc(w,terrain,data(),data().mobIndex("assembler"),clampRarity(tier),{1000,1000},Realm::Overworld,0);
        for(int cycle=0;cycle<20;++cycle){
            const auto& npc=w.get<Npc>(e);const auto& recipe=data().mob(npc.configIndex).npc.recipes[npc.recipe];
            for(const auto& item:recipe.inputs)CHECK(petalComesAt(data().petal(item.petal),npc.rarity));
            for(const auto& item:recipe.outputs)CHECK(petalComesAt(data().petal(item.petal),npc.rarity));
            const auto old=npc.recipe;
            npcs.run(w,terrain,data(),{},(cycle+1)*kAssemblerRecipeRefreshMillis);
            e=machine(w);CHECK(w.get<Npc>(e).recipe!=old);
        }
    }
}
