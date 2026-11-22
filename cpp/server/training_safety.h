#pragma once
#include "shared/game/components.h"
#include "shared/game/config.h"
#include "shared/game/realm.h"
#include <vector>
namespace flix {
inline constexpr double kTrainingSafetyMargin = 450.0;
struct TrainingZone { Entity dummy; Realm realm; Vec2 position; double radius; };
inline bool trainingSegmentBlocked(const std::vector<TrainingZone>& zones, Realm realm, Vec2 a, Vec2 b, double extraRadius=0) {
    const Vec2 ab=b-a;const double length=ab.lengthSq();
    for(const auto& z:zones) {
        if(z.realm!=realm)continue;
        const Vec2 az=z.position-a;
        const double t=length>1e-9?clamp((az.x*ab.x+az.y*ab.y)/length,0.0,1.0):0.0;
        const double radius=z.radius+extraRadius;
        if(distanceSq(z.position,a+ab*t)<=radius*radius)return true;
    }
    return false;
}
inline bool isTrainingDummy(const World& world, Entity entity) {
    const auto* npc=world.tryGet<Npc>(entity);const auto* mob=world.tryGet<MobType>(entity);
    const auto index=npc?npc->configIndex:mob?mob->configIndex:kInvalidIndex;
    if(index>=content().mobCount())return false;
    const auto& id=content().mob(index).id;return id=="target_dummy" || id=="test_dummy";
}
inline bool isTrainingProtected(const World& world, Entity player) {
    if (!world.has<PlayerTag>(player) || !world.isAlive(player) || world.has<Dead>(player)) return false;
    const auto* hp=world.tryGet<Health>(player);
    if (hp && !hp->alive()) return false;
    const auto* link=world.tryGet<TrainingProtection>(player);
    const auto* at=world.tryGet<Transform>(player);
    if (!link || !at || !isWorldRealm(at->realm) || !world.isAlive(link->dummy) || world.has<Dead>(link->dummy)) return false;
    const auto* post=world.tryGet<Transform>(link->dummy);
    const auto* body=world.tryGet<Body>(link->dummy);
    const auto* postHp=world.tryGet<Health>(link->dummy);
    if (!post || !body || (postHp && !postHp->alive()) || post->realm!=at->realm) return false;
    const double radius=body->radius+kTrainingSafetyMargin;
    return distanceSq(post->position,at->position)<=radius*radius;
}
inline std::vector<TrainingZone> refreshTrainingProtection(World& world, const ContentRegistry& registry) {
    std::vector<TrainingZone> zones;
    const auto note=[&](Entity e,std::uint16_t index,const Transform& at,const Body& body) {
        if (!isWorldRealm(at.realm) || index>=registry.mobCount() || world.has<Dead>(e)) return;
        const auto* hp=world.tryGet<Health>(e);if(hp && !hp->alive()) return;
        const auto& id=registry.mob(index).id;
        if(id=="target_dummy" || id=="test_dummy")zones.push_back({e,at.realm,at.position,body.radius+kTrainingSafetyMargin});
    };
    Query<Npc,Transform,Body>{world}.each([&](Entity e,Npc& n,Transform& at,Body& b){note(e,n.configIndex,at,b);});
    Query<MobType,Transform,Body> mobs{world};mobs.without<Dead,Pet>();
    mobs.each([&](Entity e,MobType& n,Transform& at,Body& b){if(!world.has<Npc>(e))note(e,n.configIndex,at,b);});
    std::vector<std::pair<Entity,Entity>> links;
    Query<PlayerTag,Transform>{world}.each([&](Entity e,PlayerTag&,Transform& at){
        Entity post=NULL_ENTITY;
        if (!world.has<Dead>(e))for(const auto& z:zones)if(z.realm==at.realm && distanceSq(z.position,at.position)<=z.radius*z.radius){post=z.dummy;break;}
        links.emplace_back(e,post);
    });
    // Adding a component can relocate rows: never mutate archetypes during the query.
    for(const auto& link:links)world.ensure<TrainingProtection>(link.first).dummy=link.second;
    return zones;
}
} // namespace flix
