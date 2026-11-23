#include "server/systems/spawning.h"

#include "server/loot_eligibility.h"
#include "server/systems/movement.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <tuple>

#include "shared/game/content_report.h"
#include "shared/game/prestige.h"

namespace flix {
namespace {

constexpr double kSpawnedMobBodyScale = 0.75;

// Set one deadline on every body. The deadline belongs to the body even if
// the chain is repaired or cut while the protection is running.
void protectNewLeech(World& world, Entity head, double nowMillis) {
    const double until = nowMillis + kNewLeechContactGraceMillis;
    for (Entity body = head; body != NULL_ENTITY;) {
        const auto* link = world.tryGet<BodySegment>(body);
        const Entity next = link ? link->behind : NULL_ENTITY;
        world.add<NewLeechContactGrace>(body, NewLeechContactGrace{until});
        body = next;
    }
}


/// Mobs that are never put to sleep and never recycled: they stay live
/// entities wherever they stand, however long nobody looks at them.
///
// Prestige Jungle lacks the ordinary Jungle's separately stocked boss populations.
// More of its own ambient slots can stay latent outside the players' views.
constexpr double kPrestigeJungleDensityScale = 1.25;
constexpr double kPrestigeEternalChance = 0.05;

bool hasDeepArcticEternalSlot(const MapData& map) {
    return map.prestigeLevel() == 2 && map.templateId() == "zorr_arctic";
}
constexpr double kDeepHelDifficulty = 50.0;
constexpr double kDeepHelDensityScale = 1.20;
constexpr double kPrestigeHelDensityScale = 1.20;
constexpr double kPrestigeHelUpperBand = 45.0;
constexpr double kPrestigeHelSuperBand = 65.0;
constexpr double kPrestigeHelMidBandUpperRoll = 0.40;
constexpr int kPrestigeHelMinAwake = 12;
constexpr double kPrestigeFactoryDeepDifficulty = 70.0;
constexpr int kPrestigeFactoryMinAwake = 12;
constexpr double kPrestigeFactoryRespawnMinMillis = 3000.0;
constexpr double kPrestigeFactoryRespawnMaxMillis = 7000.0;
constexpr double kDeepJungleDensityScale = 1.20;
constexpr double kDeepJungleDifficulty = 65.0;
constexpr double kPrestigeSewersDensityScale = 0.90;
constexpr double kOrdinaryInViewRespawnMaxMillis = 8000.0;
constexpr double kJungleRespawnMinMillis = 2000.0;
constexpr double kJungleRespawnMaxMillis = 6000.0;
constexpr int kJungleMinAwake = 12;
constexpr int kPrestigeSewersMinAwake = 16;
constexpr double kPrestigeSewersBoxDifficulty = 70.0;

bool isPrestigeJungle(const MapData& map) {
    return map.prestige() && map.templateId() == "jungle";
}

/// A BOSS is an event -- the whole server was told it appeared and the bots
/// were sent at it, so it has to be there when somebody arrives, doing what it
/// was doing. A target dummy is furniture someone walked away from, and the
/// DPS row has to be standing when they come back to it.
///
/// Nothing else qualifies, ULTRAS INCLUDED. An ultra out of everyone's sight
/// goes back to being a record like any other mob, which is not the same as
/// being destroyed: it is still on the map, at the same spot, and it comes
/// back to life when somebody returns. It costs nothing while nobody is there,
/// which is the whole point -- a difficulty-100 band is nearly all ultras, and
/// keeping that band's population simulated forever is exactly the bill this
/// system exists not to pay.
/// `prestige`: the mob stands in a prestige twin, where a super is not a
/// boss but the band's ordinary stock -- a difficulty-100 band there is nearly
/// all super (shared/game/prestige.h) -- so it sleeps when nobody is near
/// like any other band mob. Nothing above super ever stands in a twin.
bool alwaysAwake(const ContentRegistry& content, std::uint16_t mobIndex, Rarity rarity,
                 bool prestige) {
    if (isBossRarity(rarity) && (!prestige || rarity == Rarity::Unique)) return true;
    return mobIndex < content.mobCount() && content.mob(mobIndex).neverAmbient;
}

bool alwaysAwake(const ContentRegistry& content, const MobType& type, bool prestige) {
    return alwaysAwake(content, type.configIndex, type.rarity, prestige);
}

/// What a band's roll of `type` at `rarity` comes out as once the stand-ins --
/// the mobs with a `replace_chance`, which is to say the square -- have had
/// their chance to take its place. One draw however many stand-ins there are,
/// and none at all when the content has none, so a roster without a square
/// rolls exactly the stream it always did.
///
/// A permanent fixture is never replaced: the DPS row is furniture, and a
/// square standing in a dummy's plot would be one for good.
std::uint16_t rollStandIn(const ContentRegistry& content, std::uint16_t type, Rarity rarity,
                          Rng& rng) {
    const std::vector<std::uint16_t>& standIns = content.standInMobs();
    if (standIns.empty() || content.mob(type).neverAmbient) return type;
    const std::size_t tier = static_cast<std::size_t>(ladderIndex(rarity));
    double roll = rng.unit();
    for (const std::uint16_t standIn : standIns) {
        // Never below a stand-in's own floor, the rule every other spawn obeys.
        if (rarityIndex(rarity) < rarityIndex(content.mob(standIn).minRarity)) continue;
        roll -= content.mob(standIn).replaceChance[tier];
        if (roll < 0.0) return standIn;
    }
    return type;
}

/// The per-spawn size multiplier on the mob's nominal size: the `random_size`
/// roll, times the fixture ramp (MobConfig::fixtureSizeScale). Both factors
/// ride in the one number because the reference resolves them in the one
/// function (getEnemySizeScale), and everything that turns a mob's stat size
/// into world units multiplies by exactly that.
// Match the movement system's physical separation shape, including square's
// corner enclosure. Decorative wings and legs do not reserve extra floor.
double spawnSpacingScale(const MobConfig& mob) {
    if(mob.id=="square")return std::max(1.0,mob.visualScale*.90*std::sqrt(2.0));
    return mobBodyContactScale(mob.id);
}

double rollSizeJitter(const MobConfig& config, Rarity rarity, Rng& rng) {
    return config.rollRandomSize(rng) * config.fixtureSizeScale(rarity);
}

/// A mob whose body cannot hurt a player.
///
/// Matched by id because the reference states the rule that way -- there is no
/// JSON field for it, only `enemy.type !== 'item_spawner'` guarding the
/// contact-damage branch (src/server/playerState.ts:1695). An intangible mob is
/// the other kind: nothing touches it, so it has no touch to hurt with.
bool harmlessOnContact(const MobConfig& config) {
    return config.id == "item_spawner" || config.intangible;
}

/// Where an escort appears: the centre of whatever spawned it, a hair off on a
/// bearing of its own. A hole's ants climb out of its mouth, a server's
/// glitches out of its middle and a queen's soldiers out of her body, rather
/// than materialising in a ring around the parent.
Vec2 escortSpawnPoint(Vec2 anchor, Rng& rng) {
    return anchor + Vec2::fromAngle(rng.angle(), kEscortSpawnScatter);
}

/// A NEUTRAL child comes out already on `attacker`. Hostile ones find their
/// own target and passive ones never take one, so only neutral is handed it.
void provokeIfNeutral(World& world, Entity child, Entity attacker) {
    if (attacker == NULL_ENTITY) return;
    MobAi* brain = world.tryGet<MobAi>(child);
    if (brain != nullptr && brain->kind == AiKind::Neutral) brain->target = attacker;
}

/// Whether `e` is still somebody a mob can be sent after, in `realm`.
bool attackerUsable(World& world, Entity e, Realm realm) {
    if (!isStanding(world, e)) return false;
    const Transform* at = world.tryGet<Transform>(e);
    return at != nullptr && at->realm == realm;
}

/// The player who hit the nest since `seen` was taken: whoever's share of its
/// ledger grew the most, in the nest's own realm and still standing. `seen`
/// is brought up to date either way.
///
/// The ledger rather than the health bar, because the ledger is the only place
/// combat says WHO a hit came from -- petal, pet and projectile all credit the
/// flower behind them there.
Entity freshestAttacker(World& world, const Bounty* bounty, Realm realm,
                        std::vector<Bounty::Share>& seen) {
    if (bounty == nullptr) {
        seen.clear();
        return NULL_ENTITY;
    }
    Entity best = NULL_ENTITY;
    double bestGrowth = 0.0;
    for (const Bounty::Share& share : bounty->contributors) {
        double before = 0.0;
        for (const Bounty::Share& old : seen) {
            // Match by connection when known -- body changes on respawn.
            const bool same = share.owner != 0 ? old.owner == share.owner
                                               : old.player == share.player;
            if (same) {
                before = old.damage;
                break;
            }
        }
        const double growth = share.damage - before;
        if (!(growth > bestGrowth)) continue;
        const Entity body = liveContributorBody(world, share);
        if (body == NULL_ENTITY || !attackerUsable(world, body, realm)) continue;
        best = body;
        bestGrowth = growth;
    }
    seen.assign(bounty->contributors.begin(), bounty->contributors.end());
    return best;
}

/// How far a coordinate may sit from a flower and still BE that flower.
///
/// The runtime builds its position list out of these very transforms earlier
/// in the same tick, so the pairing is normally exact; the slack is for a
/// caller that snapshotted a moment before. Tighter than a flower's own body,
/// so the only pair it can confuse is two players standing inside each other,
/// who are owed the same neighbourhood anyway.
constexpr double kViewerMatchRadius = 24.0;

/// The flower closest to `at` in `realm` -- the first of equals -- or null when
/// nobody is in it. Its squared distance goes to `distSqOut` when one is given.
const SpawnSystem::Viewer* nearestViewer(const std::vector<SpawnSystem::Viewer>& viewers,
                                         Realm realm, Vec2 at, double* distSqOut = nullptr) {
    const SpawnSystem::Viewer* nearest = nullptr;
    double nearestSq = 0.0;
    for (const SpawnSystem::Viewer& viewer : viewers) {
        if (viewer.realm != realm) continue;
        const double distSq = distanceSq(viewer.position, at);
        if (nearest != nullptr && distSq >= nearestSq) continue;
        nearest = &viewer;
        nearestSq = distSq;
    }
    if (distSqOut != nullptr) *distSqOut = nearestSq;
    return nearest;
}

/// The luck a spawn placed at `at` is charged to: the closest flower to it in
/// the SAME realm. What the reference does for a zone fill, which belongs to
/// nobody's viewport (src/server/enemySpawner.ts:855-869). Distances are only
/// meaningful inside one coordinate space, so a flower standing at the same
/// numbers on another map is not "near" anything here.
double nearestViewerLuck(const std::vector<SpawnSystem::Viewer>& viewers, Realm realm, Vec2 at) {
    const SpawnSystem::Viewer* nearest = nearestViewer(viewers, realm, at);
    return nearest == nullptr ? kNeutralSpawnLuck : nearest->luck;
}

/// The border band, which is the thickness of the boundary wall. A mob
/// standing in it is half inside the edge of the world, so the reference
/// refuses the point outright rather than moving it
/// (isInOutOfBoundsZone, src/server/shared/positions.ts:25-30).
///
/// `extent` is the rectangle of the realm the point is in: every map is its
/// own coordinate space with its own size -- the shipped maps run from 32 to
/// 128 tiles square -- so a single world constant is the wrong number for all
/// but one of them.
bool inBorderBand(Vec2 position, Vec2 extent) {
    return position.x < kWorldBoundaryThreshold ||
           position.x > extent.x - kWorldBoundaryThreshold ||
           position.y < kWorldBoundaryThreshold ||
           position.y > extent.y - kWorldBoundaryThreshold;
}

/// No spawn lands in anyone's lap, whoever asked for it.
///
/// `gap` is the clear ground between the two BODIES: the flower's hitbox and
/// the one the spawn will have are both added to it, so the rule means the
/// same thing for a soldier ant as for the ultra beetle that is five times its
/// size. A caller that does not know yet what it is placing passes the nominal
/// radius a placement is spaced by (kPreliminarySpawnRadius) and re-asks with
/// the real one once the type is rolled.
bool nearAnyPlayer(const std::vector<SpawnSystem::Viewer>& viewers, Realm realm, Vec2 position,
                   double gap, double bodyRadius) {
    for (const SpawnSystem::Viewer& viewer : viewers) {
        if (viewer.realm != realm) continue;
        const double reach = gap + bodyRadius + viewer.radius;
        if (distanceSq(viewer.position, position) < reach * reach) return true;
    }
    return false;
}

/// What a mob takes up the moment it appears, for the lap test.
///
/// The NOMINAL radius is not what appears. The size roll comes after the test
/// and makes some bodies twice the nominal size, so the test allows for the
/// largest body the roll can give. A chain head also brings its whole body,
/// laid out in a straight line behind it.

class DensityRoots {
public:
    explicit DensityRoots(World& world):world_(world){}
    Entity get(Entity entity) {
        if(!world_.isAlive(entity)||world_.has<Dead>(entity))return NULL_ENTITY;
        if(!world_.has<BodySegment>(entity))return entity;
        auto found=roots_.find(entity);if(found!=roots_.end())return found->second;
        std::vector<Entity> path;Entity at=entity;
        for(int links=0;links<256;++links) {
            auto known=roots_.find(at);if(known!=roots_.end()){at=known->second;break;}
            path.push_back(at);const auto* segment=world_.tryGet<BodySegment>(at);
            if(!segment||segment->head||segment->ahead==NULL_ENTITY||
                !world_.isAlive(segment->ahead)||world_.has<Dead>(segment->ahead))break;
            if(std::find(path.begin(),path.end(),segment->ahead)!=path.end())break;
            at=segment->ahead;
        }
        for(Entity body:path)roots_[body]=at;
        return at;
    }
private:World& world_;std::unordered_map<Entity,Entity> roots_;
};
struct SpawnFootprint {
    double radius = kPreliminarySpawnRadius;   ///< the head, at its largest
    double tail = 0.0;                         ///< how far the body reaches behind it
    double tailRadius = 0.0;                   ///< one body segment, at its largest
};

SpawnFootprint spawnFootprint(const ContentRegistry& content, std::uint16_t mobIndex,
                              Rarity rarity) {
    SpawnFootprint footprint;
    if (mobIndex >= content.mobCount()) return footprint;
    const MobConfig& config = content.mob(mobIndex);

    footprint.radius = content.mobStats(mobIndex, rarity).radius * config.maxRandomSize() * kSpawnedMobBodyScale;
    if (config.segmentCount > 0 && config.segmentBodyIndex < content.mobCount()) {
        // spawnBodyChain's own layout: segments stepped off the NOMINAL body.
        const double bodyRadius = content.mobStats(config.segmentBodyIndex, rarity).radius * kSpawnedMobBodyScale;
        footprint.tail = config.segmentSpacing(footprint.radius, bodyRadius) +
            std::max(0, config.segmentCount - 1) * config.segmentSpacing(bodyRadius, bodyRadius);
        footprint.tailRadius = bodyRadius * content.mob(config.segmentBodyIndex).maxRandomSize();
    }
    return footprint;
}

double distanceToSegmentSq(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double lengthSq = ab.lengthSq();
    if (!(lengthSq > 0.0)) return distanceSq(p, a);
    const Vec2 ap = p - a;
    const double t = clamp((ap.x * ab.x + ap.y * ab.y) / lengthSq, 0.0, 1.0);
    return distanceSq(p, a + ab * t);
}

/// The bearing from `at` TOWARDS the nearest flower, which lays a chain head's
/// body directly away from it. Unset when nobody is in the realm.
std::optional<double> facingNearestFlower(const std::vector<SpawnSystem::Viewer>& viewers,
                                          Realm realm, Vec2 at) {
    double nearestSq = 0.0;
    const SpawnSystem::Viewer* nearest = nearestViewer(viewers, realm, at, &nearestSq);
    if (nearest == nullptr || !(nearestSq > 0.0)) return std::nullopt;
    return (nearest->position - at).angle();
}

/// No mob appears in anyone's lap: `gap` of clear ground between every
/// flower's hitbox and every part of the animal, its largest head and, for a
/// chain, the body trailing behind `facing`. With no facing to lay a body
/// along, the body is allowed for in every direction.
bool clearOfEveryLap(const std::vector<SpawnSystem::Viewer>& viewers, Realm realm, Vec2 at,
                     const SpawnFootprint& footprint, std::optional<double> facing, double gap) {
    if (nearAnyPlayer(viewers, realm, at, gap, footprint.radius)) return false;
    if (!(footprint.tail > 0.0)) return true;
    if (!facing) return !nearAnyPlayer(viewers, realm, at, gap, footprint.tail + footprint.tailRadius);
    const Vec2 tailEnd = at + Vec2::fromAngle(*facing + kPi, footprint.tail);
    for (const SpawnSystem::Viewer& viewer : viewers) {
        if (viewer.realm != realm) continue;
        const double reach = gap + footprint.tailRadius + viewer.radius;
        if (distanceToSegmentSq(viewer.position, at, tailEnd) < reach * reach) return false;
    }
    return true;
}

/// The bearing a mob waking at `at` faces, for the lap test and for the spawn.
/// Unset for anything but a chain head, whose bearing then comes from the
/// spawn's own roll. A chain keeps a bearing of its own whenever its body
/// clears every lap that way. Otherwise it is turned to face the nearest
/// flower, so its body trails straight away from it. Then only the head has to
/// keep that flower's distance, rather than the record staying asleep for as
/// long as anyone stands within a body's length of it.
std::optional<double> wakingFacing(const std::vector<SpawnSystem::Viewer>& viewers, Realm realm,
                                   Vec2 at, const SpawnFootprint& footprint, double gap, Rng& rng) {
    if (!(footprint.tail > 0.0)) return std::nullopt;
    const double rolled = rng.angle();
    if (clearOfEveryLap(viewers, realm, at, footprint, rolled, gap)) return rolled;
    const std::optional<double> towards = facingNearestFlower(viewers, realm, at);
    return towards ? towards : std::optional<double>(rolled);
}

/// True when a band's box overlaps any player's buffered viewport, grown by
/// `margin`.
///
/// The cheap half of waking the world: a band nobody is anywhere near is one
/// rectangle test per viewer, and its records are never walked at all.
// Local refill should use the room a player can actually fight in. The
// network's spawn buffer is for pre-waking, not for measuring room density.
Rect viewerRoom(const SpawnSystem::Viewer& viewer) {
    const double halfWidth = std::max(100.0, viewer.half.x - kViewportBuffer);
    const double halfHeight = std::max(100.0, viewer.half.y - kViewportBuffer);
    return {viewer.viewportCentre.x - halfWidth, viewer.viewportCentre.y - halfHeight,
            2.0 * halfWidth, 2.0 * halfHeight};
}

bool bodyOverlapsRoom(const Rect& room, Vec2 position, double radius) {
    const double dx = std::max({room.left() - position.x, 0.0, position.x - room.right()});
    const double dy = std::max({room.top() - position.y, 0.0, position.y - room.bottom()});
    return dx * dx + dy * dy <= radius * radius;
}

bool zoneInView(const Rect& bounds, Realm realm,
                const std::vector<SpawnSystem::Viewer>& viewers, double margin) {
    for (const SpawnSystem::Viewer& viewer : viewers) {
        // Same map first: two maps' coordinates overlap numerically, so a
        // viewport test alone would wake a second map because somebody was
        // standing at the same numbers in the first.
        if (viewer.realm != realm) continue;
        if (bounds.left() < viewer.viewportCentre.x + viewer.half.x + margin &&
            bounds.right() > viewer.viewportCentre.x - viewer.half.x - margin &&
            bounds.top() < viewer.viewportCentre.y + viewer.half.y + margin &&
            bounds.bottom() > viewer.viewportCentre.y - viewer.half.y - margin) {
            return true;
        }
    }
    return false;
}

/// True when `e` is a whole animal rather than a piece of one: anything
/// without a body chain, or the head a chain was built from. A centipede's
/// segments carry its tier, and counting them as more of it would make one
/// unique centipede a crowd of them.
bool isWholeAnimal(World& world, Entity e) {
    const BodySegment* link = world.tryGet<BodySegment>(e);
    return link == nullptr || link->chainHead == e;
}

/// Announcements held for a runtime that has not drained them. Two bosses a
/// minute at the very most, so this is a leak guard rather than a queue depth.
constexpr std::size_t kMaxPendingBossSpawns = 16;

/// A uniform point in `bounds`, clamped into the realm's own rectangle
/// (`extent`). The map's rectangles are allowed to hang over the edge and
/// several do.
Vec2 samplePointInRect(const Rect& bounds, Vec2 extent, Rng& rng) {
    return {clamp(bounds.x + rng.unit() * bounds.w, 0.0, extent.x),
            clamp(bounds.y + rng.unit() * bounds.h, 0.0, extent.y)};
}

/// A uniform point within `radius` of `around`, in the same rectangle.
Vec2 samplePointNear(Vec2 around, double radius, Vec2 extent, Rng& rng) {
    const Vec2 at = around + rng.insideCircle(radius);
    return {clamp(at.x, 0.0, extent.x), clamp(at.y, 0.0, extent.y)};
}

} // namespace

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

void SpawnSystem::bind(World& world) {
    if (boundWorld_ == &world) return;
    boundWorld_ = &world;
    // Queries cache their matched archetypes and are meant to outlive a tick,
    // but they need a world to be constructed against and this system is built
    // before the server has one. Rebinding also covers a test that runs the
    // same system over a second world.
    ambient_.emplace(world);
    ambient_->without<Dead>();
    ambientChains_.emplace(world);
    ambientChains_->without<Dead>();
    casualties_.emplace(world);
    escorts_.emplace(world);
    escorts_->without<Dead>();
    dependents_.emplace(world);
    dependents_->without<Dead>();
    spawners_.emplace(world);
    spawners_->without<Dead>();
    waveNests_.emplace(world);
    allMobs_.emplace(world);
    allMobs_->without<Dead>();
    playerBodies_.emplace(world);
    ambushNests_.emplace(world);
    ambushNests_->without<Dead>();
    // A corpse lying at a hole's rim is nobody for the brood to come out at.
    livingFlowers_.emplace(world);
    livingFlowers_->without<Dead>();
    rituals_.emplace(world);
}

// ---------------------------------------------------------------------------
// Type and tier selection
// ---------------------------------------------------------------------------

std::uint16_t SpawnSystem::chooseGroupMob(const ContentRegistry& content, std::uint16_t group,
                                         Rarity rarity, Rng& rng) const {
    if (group == kInvalidIndex || group >= content.mobGroupCount()) return kInvalidIndex;
    const MobGroup& members = content.mobGroup(group);
    if (members.members.empty()) return kInvalidIndex;

    // Two walks rather than a cached cumulative table, because eligibility is
    // per TIER: a mob with a min_rarity is in the group at legendary and out
    // of it at common, so a table built once would have to be built per tier
    // per group. A group holds a handful of mobs and this runs a few times a
    // second, so two passes over it is not worth caching away.
    //
    // A zero weight is how the data says "in this group, but never rolled":
    // centipede body segments belong to the garden and are only ever placed by
    // the head that owns them.
    double total = 0.0;
    for (const MobGroupMember& member : members.members) {
        if (!(member.weight > 0.0)) continue;
        if (!content.mobStats(member.mob, rarity).ambient) continue;
        total += member.weight;
    }
    if (!(total > 0.0)) return kInvalidIndex;

    double roll = rng.unit() * total;
    std::uint16_t last = kInvalidIndex;
    for (const MobGroupMember& member : members.members) {
        if (!(member.weight > 0.0)) continue;
        if (!content.mobStats(member.mob, rarity).ambient) continue;
        last = member.mob;
        roll -= member.weight;
        if (roll < 0.0) return member.mob;
    }
    return last;
}

std::uint16_t SpawnSystem::chooseRegionMobAt(const ContentRegistry& content, Realm realm, Vec2 at,
                                             Rarity rarity, Rng& rng) {
    // The region standing on this ground. First match in map order, as every
    // other shape lookup here is.
    for (const SpawnZone& region : regions_) {
        if (region.realm != realm) continue;
        if (!zoneContains(region.bounds, region.polygon, at)) continue;
        return rollResolvedRows(content, region.resolved, rarity, rng);
    }
    // Outside every region: the map's own default group, which is the only
    // thing left that can say what belongs here.
    //
    // There is no arm below this one. A map is always in hand by the time we
    // get here: this is reached only from chooseZoneMobType, which is reached
    // only from stockZone, which needs a band -- and a band exists only
    // because rebuildZones read it off a map in `worldMaps`, whose realm
    // therefore resolves. A harness with no map has no band, so it spawns
    // nothing at all and never arrives here; that is what spawning.h's
    // `worldMaps` says, and it used to be untrue while the density fill could
    // reach this function with a bare Terrain.
    const MapData* map = worldMaps != nullptr ? worldMaps->forRealm(realm) : nullptr;
    if (map == nullptr) return kInvalidIndex;
    return chooseGroupMob(content, content.mobGroupIndex(map->defaultMobGroup()), rarity, rng);
}

bool SpawnSystem::permanentFixtureExists(World& world, std::uint16_t mobIndex, Rarity rarity,
                                         Realm realm, int section) {
    bind(world);
    bool found = false;
    allMobs_->each([&](Entity, MobTag&, Transform& transform, MobType& type) {
        if (found) return;
        if (type.configIndex != mobIndex || type.rarity != rarity) return;
        // Same map first: a dummy on the overworld says nothing about whether
        // a biome map has one, and the numeric section is the OVERWORLD's
        // grid. On any other map the fixture is one per tier per map.
        if (transform.realm != realm) return;
        if (realm == Realm::Overworld && sectionAt(transform.position) != section) return;
        found = true;
    });
    return found;
}

Rarity SpawnSystem::rollRarity(const MobConfig& config, double difficulty, double luck,
                               Rng& rng) {
    const Rarity rolled = rollSpawnRarity(difficulty, luck, rng);
    return clampLadderRarity(std::max(rarityIndex(rolled), rarityIndex(config.minRarity)));
}

double SpawnSystem::difficultyAt(Realm realm, Vec2 at) const {
    // A band, or nothing. The author drawing danger onto a shape is the only
    // way a square of the world is dangerous; a square no band covers grows no
    // mob at all, and zero is what that reads as. First match in map order, as
    // every other shape lookup here is.
    for (const SpawnZone& zone : zones_) {
        if (zone.realm != realm) continue;
        if (!zoneContains(zone.bounds, zone.polygon, at)) continue;
        return zone.difficulty;
    }
    return 0.0;
}

// ---------------------------------------------------------------------------
// Placing a mob
// ---------------------------------------------------------------------------

void SpawnSystem::spawnRingPetals(World& world, const ContentRegistry& content, Entity mob,
                                  const PetalRingSpec& spec, const MobStats& stats,
                                  Rarity rarity) {
    spawnMobRingSeeds(world, content, mob, spec, stats.damage, stats.mass, rarity,
                      [this]() -> std::uint32_t { return netIds != nullptr ? netIds->next() : 0; });
}

void spawnMobRingSeeds(World& world, const ContentRegistry& content, Entity mob,
                       const PetalRingSpec& spec, double contactDamage, double mass,
                       Rarity rarity, const std::function<std::uint32_t()>& nextNetId) {
    if (spec.petalIndex == kInvalidIndex) return;
    // Graded at the MOB's tier, not at the tier the ring's config names: an
    // apex dandelion wears apex seeds, the same rule its shots are fired at.
    const PetalStats seed = content.petalStats(spec.petalIndex, rarity);

    // Read once, up front: every create() below relocates this row.
    const MobPetalRing* ring = world.tryGet<MobPetalRing>(mob);
    if (ring == nullptr || ring->seats.empty()) return;
    const std::size_t count = ring->seats.size();
    const double orbit = ring->orbit;
    const double seedRadius = ring->seedRadius;
    const Transform* at = world.tryGet<Transform>(mob);
    if (at == nullptr) return;
    const Vec2 centre = at->position;
    const Realm realm = at->realm;
    const Faction* faction = world.tryGet<Faction>(mob);
    const Faction side = faction != nullptr ? *faction : Faction{Team::Hostiles, false};

    for (std::size_t i = 0; i < count; ++i) {
        const double bearing = i * (kTau / static_cast<double>(count));
        const Entity e = world.create();
        world.add<MobRingPetal>(e, MobRingPetal{mob, i, seed.noHealDurationMillis});
        // The seed faces OUTWARD, which is gardn's kFollowRot and what puts a
        // dandelion's stem back into the body it grew from.
        world.add<Transform>(e, Transform{centre + Vec2::fromAngle(bearing, orbit), bearing, realm});
        world.add<Body>(e, Body{seedRadius, mass});
        world.add<Faction>(e, side);
        // Its own pool, so it breaks alone -- which is the whole point of the
        // seeds being entities. Graded at the mob's tier like everything else
        // about it.
        world.add<Health>(e, Health{seed.health, seed.health, 0.0, 0.0});
        // It hits for what the MOB hits for, on the mob's own cadence: the
        // ring is how this animal touches you, so touching a seed and touching
        // its hull cost the same. (The reference is explicit about this --
        // applyPetalRingDamage deals `mobDamage(enemy.entity)`.)
        world.add<ContactDamage>(e, ContactDamage{contactDamage, kMobHitIntervalMillis});
        world.add<HitCooldowns>(e);
        world.add<Knockback>(e);
        Replicated replicated;
        replicated.kind = net::EntityKind::Petal;
        replicated.typeIndex = spec.petalIndex;
        replicated.rarity = rarity;
        replicated.spawnFlags = net::SpawnRingPetal;
        world.add<Replicated>(e, replicated);
        if (nextNetId) {
            if (const std::uint32_t id = nextNetId()) world.add<NetId>(e, NetId{id});
        }
        // Re-fetched every time, never held: the create above moved it.
        if (MobPetalRing* live = world.tryGet<MobPetalRing>(mob)) live->seats[i] = e;
    }
}

Entity SpawnSystem::spawnMob(World& world, const Terrain& terrain, const ContentRegistry& content,
                             std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                             double nowMillis, Rng& rng, MobSpawnScope scope) {
    // No band owns a mob somebody else asked for -- the arena, the maze, a
    // script, an operator's console -- so it is counted against no band's
    // target and is recycled the old way rather than going back to a record
    // there is no band to hold.
    const bool worldBoss = scope == MobSpawnScope::World;
    return spawnMobAt(world, terrain, content, mobIndex, rarity, position, realm, nowMillis, rng, 0,
                      kInvalidIndex, std::nullopt, worldBoss, worldBoss);
}

SpawnSystem::Sacrifice SpawnSystem::planSacrifice(World& world, const ContentRegistry& content,
                                                  Rarity rarity, Vec2 at, Realm realm, Rng& rng) {
    bind(world);
    // The band the flower fell in, first match in map order as difficultyAt
    // reads it: its difficulty is the "mob rarity" of the area, and its rows
    // are what lives there.
    const SpawnZone* zone = nullptr;
    for (const SpawnZone& candidate : zones_) {
        if (candidate.realm != realm) continue;
        if (!zoneContains(candidate.bounds, candidate.polygon, at)) continue;
        zone = &candidate;
        break;
    }
    if (zone == nullptr) return Sacrifice{SacrificeOutcome::NoBand};
    if (!groundTakesSacrifice(zone->difficulty)) return Sacrifice{SacrificeOutcome::LowGround};

    if (rarity != Rarity::Ascendant) rarity = ladderRarity(rarity);
    // A prestige biome raises the summons a tier as it raises every mob, and
    // refuses a unique or an apex outright.
    if (prestigeRealm(realm)) {
        if (rarity == Rarity::Ascendant || barredFromPrestige(rarity)) return Sacrifice{SacrificeOutcome::Prestige};
        rarity = prestigeRarity(rarity);
    }
    if ((rarity == Rarity::Unique || rarity == Rarity::Apex || rarity == Rarity::Ascendant) &&
        topBossTaken(world, rarity, realm)) {
        return Sacrifice{SacrificeOutcome::Taken};
    }

    // A few rolls, because a band may name a permanent fixture outright -- the
    // target dummy -- and a DPS post is not a mob to fight.
    std::uint16_t type = kInvalidIndex;
    for (int attempt = 0; attempt < 8 && type == kInvalidIndex; ++attempt) {
        const std::uint16_t rolled = chooseZoneMobType(content, *zone, at, ladderRarity(rarity), rng);
        if (rolled == kInvalidIndex || content.mob(rolled).neverAmbient) continue;
        type = rolled;
    }
    if (type == kInvalidIndex) return Sacrifice{SacrificeOutcome::NoMob};
    Sacrifice plan{SacrificeOutcome::Summoned};
    plan.mobIndex = type;
    // The tier it will actually stand at, the mob's own floor included.
    plan.rarity = rarity == Rarity::Ascendant ? rarity :
        clampLadderRarity(std::max(rarityIndex(rarity), rarityIndex(content.mob(type).minRarity)));
    if (barredFromPrestige(plan.rarity) && prestigeRealm(realm)) {
        return Sacrifice{SacrificeOutcome::Prestige};
    }
    if (plan.rarity != rarity && (plan.rarity == Rarity::Unique || plan.rarity == Rarity::Apex) &&
        topBossTaken(world, plan.rarity, realm)) return Sacrifice{SacrificeOutcome::Taken};
    return plan;
}

Entity SpawnSystem::spawnSacrifice(World& world, const Terrain& terrain,
                                   const ContentRegistry& content, const Sacrifice& plan, Vec2 at,
                                   Realm realm, double nowMillis, Rng& rng) {
    if (plan.outcome != SacrificeOutcome::Summoned || plan.mobIndex >= content.mobCount()) {
        return NULL_ENTITY;
    }
    bind(world);
    if ((plan.rarity == Rarity::Unique || plan.rarity == Rarity::Apex || plan.rarity == Rarity::Ascendant) &&
        topBossTaken(world, plan.rarity, realm, false)) return NULL_ENTITY;
    // Nobody's band: the summon is not part of any band's population, and like
    // every boss it stays live wherever it stands.
    const Entity mob = spawnMobAt(world, terrain, content, plan.mobIndex, plan.rarity, at, realm,
                                  nowMillis, rng, 0, kInvalidIndex, std::nullopt, false);
    if (mob != NULL_ENTITY) chargeBossClock(plan.rarity, realm, nowMillis);
    return mob;
}

SpawnSystem::Sacrifice SpawnSystem::summonSacrifice(World& world, const Terrain& terrain,
                                                    const ContentRegistry& content, Rarity rarity,
                                                    Vec2 at, Realm realm, double nowMillis,
                                                    Rng& rng) {
    Sacrifice plan = planSacrifice(world, content, rarity, at, realm, rng);
    if (plan.outcome != SacrificeOutcome::Summoned) return plan;
    plan.mob = spawnSacrifice(world, terrain, content, plan, at, realm, nowMillis, rng);
    if (plan.mob == NULL_ENTITY) plan.outcome = SacrificeOutcome::NoMob;
    return plan;
}

bool SpawnSystem::randomBandPoint(const Terrain& terrain, Realm realm, Rng& rng,
                                  Vec2& out) const {
    // Weighted by population, which is area: a point uniform over the realm's
    // bands rather than over the band list.
    double total = 0.0;
    for (const SpawnZone& zone : zones_) {
        if (zone.realm == realm) total += static_cast<double>(zone.targetMobs);
    }
    if (!(total > 0.0)) return false;
    const Vec2 extent = terrain.realmExtent(realm);
    for (int attempt = 0; attempt < 200; ++attempt) {
        double roll = rng.unit() * total;
        const SpawnZone* zone = nullptr;
        for (const SpawnZone& candidate : zones_) {
            if (candidate.realm != realm) continue;
            zone = &candidate;
            roll -= static_cast<double>(candidate.targetMobs);
            if (roll < 0.0) break;
        }
        if (zone == nullptr) return false;
        const Vec2 candidate = samplePointInRect(zone->bounds, extent, rng);
        if (!zoneContains(zone->bounds, zone->polygon, candidate)) continue;
        if (inBorderBand(candidate, extent)) continue;
        if (terrain.blocked(candidate, realm)) continue;
        out = candidate;
        return true;
    }
    return false;
}

Entity SpawnSystem::spawnMobAt(World& world, const Terrain& terrain, const ContentRegistry& content,
                               std::uint16_t mobIndex, Rarity rarity, Vec2 position, Realm realm,
                               double nowMillis, Rng& rng, int depth, std::uint16_t zone,
                               std::optional<double> facing, bool clockGated, bool announce, bool prestigePromotion,
                               const WakeClearance* wakeClearance) {
    if (mobIndex >= content.mobCount()) return NULL_ENTITY;

    const MobConfig& config = content.mob(mobIndex);
    if (isWorldRealm(realm) && config.segmentCount > 0 &&
        config.segmentBodyIndex != kInvalidIndex &&
        1 + config.segmentCount > mobCap - census_.mobs) return NULL_ENTITY;
    // A mob does not exist below its min_rarity, whoever asked for it -- nor
    // above apex: universal is a petal tier, and a console `spawn bee
    // universal` or a nest's offset past the top comes out an apex. Enforced
    // here rather than at each call site so a nest, a script and the ambient
    // roll cannot disagree about it.
    const bool nestedQueen=depth>0&&config.id=="queen_ant";
    if (rarity != Rarity::Ascendant)
        rarity = clampLadderRarity(nestedQueen?rarityIndex(rarity):std::max(rarityIndex(rarity), rarityIndex(config.minRarity)));

    if (config.id=="termite_mound" && zone!=kInvalidIndex) {
        bool occupied=false;
        Query<DungeonEntrance,MobType,Transform> entrances{world};entrances.without<Dead>();
        entrances.each([&](Entity,DungeonEntrance&,MobType& type,Transform& at){
            if(at.realm==realm && type.rarity==rarity && type.configIndex==mobIndex)occupied=true;
        });
        if(occupied)return NULL_ENTITY;
    }

    // In a prestige twin only the natural promotion can create an Eternal.
    // Body segments inherit the tier of the head which already passed this gate.
    const auto* authored = worldMaps ? worldMaps->forRealm(realm) : nullptr;
    const bool naturalEternal = authored && authored->prestigeLevel() == 2 &&
        zone != kInvalidIndex && rarity == Rarity::Unique;
    if ((rarity == Rarity::Ascendant || barredFromPrestige(rarity)) && prestigeRealm(realm) &&
        !naturalEternal &&
        !(rarity == Rarity::Unique && prestigePromotion)) return NULL_ENTITY;

    // A wild unique or apex is its biome's clock's to hand out, and nobody
    // else's. One asked for any other way while that clock is still cooling
    // down -- an operator's console, a band naming a mob whose floor sits
    // above super, a path nobody has written yet -- is gone before it exists:
    // no entity, no announcement, nothing on the wire. The clock's own upgrade
    // only ever asks while it is ready, so it always passes. After the floor,
    // because that is the tier the mob would actually stand at; roots only,
    // as with the announcement, because a centipede's segments carry the
    // head's tier and the head was already let in.
    if (depth == 0 && clockGated && !naturalEternal && bossCooldownLeft(rarity == Rarity::Ascendant ? Rarity::Apex : rarity, realm, nowMillis) > 0.0) {
        return NULL_ENTITY;
    }

    const MobStats stats = content.mobStats(mobIndex, rarity);

    const double jitter = rollSizeJitter(config, rarity, rng);
    // The same radius drives the mob's collisions and its replicated body.
    const double radius = stats.radius * jitter * kSpawnedMobBodyScale;

    Body body{radius, stats.mass};
    const auto* map = worldMaps ? worldMaps->forRealm(realm) : nullptr;
    body.wideMobWallHull = map && map->biome() == "ant_hell" &&
        (config.id == "baby_fire_ant" || config.id == "worker_fire_ant" ||
         config.id == "soldier_fire_ant" || config.id == "queen_fire_ant");
    const double wallRadius = mobWallRadius(&body);
    const Vec2 at = terrain.resolveCircle(position, wallRadius, realm);
    if (body.wideMobWallHull && terrain.resolveWall(at, wallRadius, realm).unresolved)
        return NULL_ENTITY;

    if (wakeClearance && distanceSq(at, position) > 0.0) {
        // Wall resolution can move a wake point well into the flower's lap.
        // Keep the record dormant if the resolved body fails the same guard.
        auto footprint = spawnFootprint(content, mobIndex, rarity);
        footprint.radius = std::max(footprint.radius, radius);
        const bool clear = wakeClearance->sparseRescue
            ? clearNaturalSpawn(zones_[zone], wakeClearance->viewers, at,
                                footprint.radius, footprint.tail, footprint.tailRadius, facing)
            : clearOfEveryLap(wakeClearance->viewers, realm, at, footprint, facing, kMinSpawnDistance);
        if (!clear) return NULL_ENTITY;
    }

    // An ambush nest lets flowers walk across it, but its mouth still
    // occupies space when placing another nest.
    if (config.ambush.present) {
        bool overlapsNest = false;
        Query<AmbushNest, Transform, Body>{world}.without<Dead>().each(
            [&](Entity, AmbushNest&, Transform& other, Body& otherBody) {
                const double gap = radius + otherBody.radius;
                if (other.realm == realm && distanceSq(at, other.position) < gap * gap)
                    overlapsNest = true;
            });
        if (overlapsNest) return NULL_ENTITY;
    }

    // Held rather than passed straight through: a centipede's body is laid out
    // along its head's facing, and the chain is built once the head is whole.
    // Rolled even when the caller chose it, so the stream stays the same.
    const double rolledAngle = rng.angle();
    const double angle = facing ? *facing : rolledAngle;

    const Entity e = world.create();
    world.add<MobTag>(e);
    if (config.id == "ghost") world.add<GhostTag>(e);
    world.add<Transform>(e, Transform{at, angle, realm});
    world.add<Motion>(e);
    // Mass is area, but it is the TIER's area: the reference derives mass from
    // the config size and the rarity step alone, so a mob that rolled a big
    // body is no harder to knock back than one that rolled a small one.
    world.add<Body>(e, body);
    world.add<Knockback>(e);
    world.add<Faction>(e, Faction{Team::Hostiles, false});
    world.add<Health>(e, Health{stats.health, stats.health, 0.0, 0.0});
    // Every mob is armoured, so this is unconditional: added at spawn even at
    // zero, the archetype is the same one a stripped mob sits in and a bur
    // costs no row move to land on.
    world.add<Armor>(e, Armor{stats.armor});
    if (stats.evasion > 0.0) world.add<Evasion>(e, Evasion{stats.evasion});
    // The config's cooldown is the gap between deliberate ATTACKS, which the AI
    // owns; touching a mob is throttled by the same rule for every mob.
    //
    // The item spawner is the one mob that is furniture rather than an enemy:
    // the reference excludes it from the contact-damage branch by name, so its
    // `damage: 50` never lands on anyone who walks into it.
    if (!harmlessOnContact(config)) {
        world.add<ContactDamage>(e, ContactDamage{stats.damage * mobContactScale(rarity), kMobHitIntervalMillis});
    }
    world.add<HitCooldowns>(e);
    world.add<Afflictions>(e);
    world.add<MobType>(e, MobType{mobIndex, rarity, jitter});
    if (const double undead = mobUndeadMillis(config, rarity); undead > 0.0) {
        world.add<RisesUndead>(e, RisesUndead{undead});
    }
    if (config.intangible) world.add<Intangible>(e);
    if (config.colony) world.add<ColonyMember>(e);

    const bool chainHead = config.segmentCount > 0 && config.segmentBodyIndex != kInvalidIndex;
    if (chainHead) {
        // A head is a segment of its own chain. The follow pass walks from
        // whatever has nothing ahead of it, so a head carrying no link would
        // not be a chain root at all and its body would never be placed.
        BodySegment link;
        link.head = true;
        link.chainHead = e;
        link.sharedHealth = config.sharedSegmentHealth;
        world.add<BodySegment>(e, link);
    }

    Bounty bounty;
    bounty.xp = stats.xp;
    if (prestigeRealm(realm)) {
        const double previous = content.mobStats(mobIndex, prestigeXpRarity(
            authored && authored->prestigeLevel() == 2 ? prestigeXpRarity(rarity) : rarity)).xp;
        // Bounded bonus on the old reward. The authored S/ET XP jumps are
        // too steep to pay the full promoted tier for routine prestige mobs.
        bounty.xp = std::min(stats.xp, previous * 1.5);
    }
    world.add<Bounty>(e, std::move(bounty));

    MobAi ai;
    ai.kind = config.id == "ghost" ? AiKind::Neutral :
        config.wildSpeedScale == 0.0 ? AiKind::Stationary : stats.ai;
    ai.aggroRange = stats.aggroRange;
    ai.wanderAngle = rng.angle();
    world.add<MobAi>(e, std::move(ai));

    world.add<AmbientMob>(e, AmbientMob{nowMillis, zone, nextSpawnOrder_++});
    if (zone != kInvalidIndex && config.id == "silverfish")
        world.add<SilverfishHome>(e, SilverfishHome{at});
    world.add<Replicated>(e, Replicated{net::EntityKind::Mob, 0, mobIndex, rarity, 0});
    if (netIds != nullptr) world.add<NetId>(e, NetId{netIds->next()});

    // The census is the population of the authored MAPS, every one of them --
    // what takeCensus counts, and so what mobCap is held against. Counted
    // here for every map, or a pass waking mobs on any map but the overworld
    // would go straight past the cap before the next census noticed. The arena
    // and the maze are counted by the spawner that fills them.
    if (isWorldRealm(realm)) {
        ++census_.mobs;
        ++census_.spawnedTotal;
    }

    // A ring that is AMMUNITION is fitted at spawn rather than on the first
    // tick the mob thinks: a dandelion standing beyond the AI's LOD stride can
    // still be shot at, and a ring handed out lazily would owe its first seed
    // to whenever the mob next got a turn.
    //
    // The SEATS are resolved here, against this mob's own body; the seeds
    // themselves are created at the very bottom of this function, with the
    // rest of the creates, for the reason stated there.
    if (config.petalRing.present && config.petalRing.shootOnHit) {
        MobPetalRing ring;
        ring.seats.assign(static_cast<std::size_t>(config.petalRing.count), NULL_ENTITY);
        ring.orbit = radius * config.petalRing.orbitScale;
        ring.seedRadius = radius * config.petalRing.hitScale;
        world.add<MobPetalRing>(e, ring);
    }

    if (depth < kMaxNestDepth) {
        if (!config.dungeon.present && config.periodicSpawn.present) {
            Spawner spawner;
            spawner.childConfigIndex = config.periodicSpawn.mobIndex;
            spawner.rarityOffset = config.periodicSpawn.rarityOffset;
            spawner.intervalMillis = config.periodicSpawn.intervalMillis;
            // Due immediately. The reference starts the clock at zero against a
            // wall-clock `now`, so a queen has a soldier out on the tick she
            // appears rather than standing alone for her first interval.
            spawner.nextSpawnMillis = nowMillis;
            spawner.childLifetimeMillis = config.periodicSpawn.lifetimeMillis;
            spawner.maxAlive = config.periodicSpawn.maxAlive;
            world.add<Spawner>(e, std::move(spawner));
        }
        if (!config.dungeon.present && (!config.spawnWaves.empty() || !config.hurtSpawn.empty())) {
            NestWaves waves;
            waves.mobIndex = mobIndex;
            // Seeded full: the first band fires on the first damage the hole
            // takes, and never before it.
            waves.previousHealth = stats.health;
            world.add<NestWaves>(e, std::move(waves));
        }
        // Armed, with nothing out: the brood waits for a flower (runNests).
        if (!config.dungeon.present && config.ambush.present) world.add<AmbushNest>(e);
        if (config.id == "bio_stem_cell" && map && map->biome() == "bio")
            world.add<StemCellBrood>(e);
        // Unclaimed: the dungeon is built the first time a flower walks in
        // (DungeonSystem).
        if (config.dungeon.present) world.add<DungeonEntrance>(e);
    }

    // Last, because each of these is a create() that can relocate the rows the
    // adds above were writing into. Nothing may touch `e` after this -- the
    // chain takes it by value, as a link, and never reads its components.
    //
    // A body is laid out whatever the nesting depth: it is not nest content but
    // the rest of the same animal, so a centipede that is itself an escort
    // still arrives whole rather than as a floating head.
    if (chainHead) {
        spawnBodyChain(world, terrain, content, e, config, rarity, at, realm, angle, nowMillis, rng,
                       depth + 1);
    }
    // Here with the other creates, and never above: every one of these
    // relocates the rows the adds at the top of this function were writing
    // into, which is why nothing may touch `e` by component pointer past this
    // line. The ring is re-fetched inside.
    if (config.petalRing.present && config.petalRing.shootOnHit) {
        spawnRingPetals(world, content, e, config.petalRing, stats, rarity);
    }
    if (depth < kMaxNestDepth) {
        for (const std::uint16_t child : config.initialSpawns) {
            spawnEscort(world, terrain, content, child, rarity, escortSpawnPoint(at, rng), realm,
                        e, nowMillis, rng, depth + 1);
        }
    }

    // A nest's escorts and a centipede's segments carry their parent's tier.
    if (depth == 0 && announce) announceIfNotable(content, e, mobIndex, rarity, at, realm);

    if (config.id == "ghost" && netIds != nullptr) {
        const Entity glint = world.create();
        world.add<Transform>(glint, Transform{at, 0.0, realm});
        world.add<Body>(glint, Body{12.0, 1.0});
        world.add<GhostSparkle>(glint, GhostSparkle{e});
        world.add<Replicated>(glint, Replicated{net::EntityKind::Effect, 0,
            static_cast<std::uint16_t>(GroundEffectKind::GhostSparkle), Rarity::Common, 0});
        world.add<NetId>(glint, NetId{netIds->next()});
    }

    return e;
}

void SpawnSystem::spawnBodyChain(World& world, const Terrain& terrain,
                                 const ContentRegistry& content, Entity head,
                                 const MobConfig& config, Rarity rarity, Vec2 headPosition,
                                 Realm realm, double headAngle, double nowMillis, Rng& rng,
                                 int depth) {
    // The body's own stats at the HEAD's tier: a mythic centipede is one long
    // mythic animal, not a big head towing a string of common beads.
    const MobStats bodyStats = content.mobStats(config.segmentBodyIndex, rarity);
    if (!(bodyStats.radius > 0.0)) return;
    // The first bead is spaced off the head's own body, the rest off a bead.
    const Body* headBody = world.tryGet<Body>(head);
    double leaderRadius = headBody != nullptr ? headBody->radius : bodyStats.radius;

    // Start behind the head, keeping each link on its leader's side of a wall.
    // Close to a wall the chain starts compressed and unfolds as the head moves.
    const Vec2 back = Vec2::fromAngle(headAngle + kPi);

    Entity ahead = head;
    Vec2 at = headPosition;
    for (int i = 1; i <= config.segmentCount; ++i) {
        // Admit the whole animal once its head was admitted. Cutting this
        // loop at the cap leaves bosses with a partial body (or just a head).
        // spawnMobAt reserved enough room for the complete chain.
        const double bodyRadius = bodyStats.radius * kSpawnedMobBodyScale;
        const double spacing = config.segmentSpacing(leaderRadius, bodyRadius);
        leaderRadius = bodyRadius;
        at = terrain.resolveLinkedCircle(at, back * spacing, kMobWallRadius, realm);
        // Segments belong to the animal, not to the band: the head is the one
        // thing the band counted, and the chain is rebuilt from scratch every
        // time that head comes back to life.
        const bool prestigeEternalSegment = rarity == Rarity::Unique && prestigeRealm(realm);
        const Entity segment = spawnMobAt(world, terrain, content, config.segmentBodyIndex, rarity,
                                          at, realm, nowMillis, rng, depth, kInvalidIndex,
                                          std::nullopt, true, true, prestigeEternalSegment);
        if (segment == NULL_ENTITY) break;

        BodySegment link;
        link.ahead = ahead;
        link.spacing = spacing;
        link.chainHead = head;
        link.segmentIndex = i;
        link.sharedHealth = config.sharedSegmentHealth;
        world.add<BodySegment>(segment, link);
        // After the add, which relocated the row the spawn had just written.
        // A segment faces the way its head does or the body starts out kinked.
        if (Transform* transform = world.tryGet<Transform>(segment)) {
            transform->angle = headAngle;
            at = transform->position;
        }
        // Both directions, here rather than only in the AI pass that maintains
        // them: a shared-health chain hit on its very first tick has to be
        // walkable from the head before anything has had a chance to think.
        if (BodySegment* leader = world.tryGet<BodySegment>(ahead)) leader->behind = segment;
        ahead = segment;
    }
}

void SpawnSystem::answerDeaths(World& world, const Terrain& terrain, const ContentRegistry& content,
                               const std::vector<Entity>& dead, Rng& rng, double nowMillis,
                               double dt, CommandBuffer& commands, const SquadEntityIndex* squads) {
    // The remains already lying about grow older first, so what is laid
    // below keeps every second of its own time.
    Query<MobRemains, Lifetime> remains{world};
    remains.each([&](Entity e, MobRemains&, Lifetime& lifetime) {
        lifetime.remainingSeconds -= dt;
        if (lifetime.remainingSeconds <= 0.0) commands.destroy(e);
    });

    const auto spawnBloodCell = [&](std::uint16_t index, Rarity tier, const Transform& anchor) {
        if (census_.mobs >= mobCap || index == kInvalidIndex) return false;
        const Entity child = spawnMobAt(world, terrain, content, index, tier,
            escortSpawnPoint(anchor.position, rng), anchor.realm, nowMillis, rng, 1,
            kInvalidIndex, std::nullopt, false, false, true);
        if (child == NULL_ENTITY) return false;
        const auto& where = world.get<Transform>(child);
        const auto& body = world.get<Body>(child);
        if (terrain.resolveWall(where.position, mobWallRadius(&body), where.realm).unresolved) {
            commands.destroy(child);
            return false;
        }
        world.add<StemCellOffspring>(child);
        world.get<Bounty>(child).xp = 0.0;
        return true;
    };

    for (const Entity e : dead) {
        const MobType* type = world.tryGet<MobType>(e);
        const Transform* transform = world.tryGet<Transform>(e);
        if (type == nullptr || transform == nullptr) continue;
        const MobConfig& config = content.mob(type->configIndex);
        const auto* subdivision = world.tryGet<CancerSubdivision>(e);
        if (subdivision && subdivision->spawnedCopy) continue;
        if (!subdivision && !config.deathSpawn.present && !config.allySpawn.present &&
            !(config.remainsMillis > 0.0))
            continue;
        // Copied out: everything created below may move the corpse's row.
        const Rarity rarity = type->rarity;
        const auto mobIndex = type->configIndex;
        const bool splits = subdivision && !world.has<Pet>(e);
        const Vec2 at = transform->position;
        const double angle = transform->angle;
        const Realm realm = transform->realm;
        const Body* body = world.tryGet<Body>(e);
        const double drawnRadius = (body != nullptr ? body->radius : 0.0) * config.visualScale;

        if (config.remainsMillis > 0.0 && drawnRadius > 0.0) {
            const Entity shell = world.create();
            world.add<Transform>(shell, Transform{at, angle, realm});
            world.add<MobRemains>(shell, MobRemains{drawnRadius});
            world.add<Lifetime>(shell, Lifetime{config.remainsMillis / 1000.0});
            Replicated replicated;
            replicated.kind = net::EntityKind::Effect;
            replicated.typeIndex = static_cast<std::uint16_t>(GroundEffectKind::OpenTomb);
            replicated.rarity = rarity;
            world.add<Replicated>(shell, replicated);
            if (netIds != nullptr) world.add<NetId>(shell, NetId{netIds->next()});
        }

        if (splits && census_.mobs < mobCap) {
            const double roll = rng.unit();
            const int offset = roll < .10 ? 1 : roll < .35 ? 0 : -1;
            const Rarity next = clampRarity(std::clamp(rarityIndex(rarity) + offset, 0,
                                                       rarityIndex(Rarity::Unique)));
            const Entity child = spawnMobAt(world, terrain, content, mobIndex, next, at, realm,
                                            nowMillis, rng, 1, kInvalidIndex);
            for (Entity copy = child; copy != NULL_ENTITY;) {
                const auto* link = world.tryGet<BodySegment>(copy);
                const Entity nextSegment = link ? link->behind : NULL_ENTITY;
                world.add<CancerSubdivision>(copy, CancerSubdivision{true});
                world.get<Bounty>(copy).xp = 0.0;
                copy = nextSegment;
            }
        }
        if (config.deathSpawn.present && rng.chance(config.deathSpawn.chance)) {
            std::uint16_t broodIndex = config.deathSpawn.mobIndex;
            int broodCount = config.deathSpawn.count;
            if (const NestBroodRung* rung =
                    broodRungForTier(config.deathSpawn.byRarity, rarityIndex(rarity))) {
                broodIndex = rung->mobIndex;
                broodCount = rung->mobIndex == kInvalidIndex ? 0 : rung->count;
            }
            // Out of the tomb itself, as a hole's waves come out of the hole,
            // and held to the ladder an escort is held to.
            for (int i = 0; i < broodCount; ++i) {
                if (config.id == "bio_stem_cell" &&
                    content.mob(broodIndex).id == "bio_biologist") {
                    if (census_.mobs >= mobCap) break;
                    std::vector<Bounty::Share> ranked;
                    if (const auto* bounty = world.tryGet<Bounty>(e)) {
                        for (const auto& share : bounty->contributors) {
                            if (share.damage > 0.0) ranked.push_back(refreshedShare(world, share));
                        }
                    }
                    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                        return a.damage > b.damage;
                    });
                    const auto* hp = world.tryGet<Health>(e);
                    std::vector<Entity> eligible;
                    selectLootRecipients(ranked, lootSlotsForRarity(rarity), squads, eligible,
                                         lootDamageFloor(rarity, hp ? hp->max : 0.0));
                    InheritedLootClaims claims;
                    for (const Entity player : eligible) {
                        if (!world.has<PlayerTag>(player)) continue;
                        const auto* account = world.tryGet<PlayerAccount>(player);
                        claims.recipients.push_back({player, account ? account->connection : 0});
                    }
                    const Entity child = spawnMobAt(world, terrain, content,
                        broodIndex, rarity, escortSpawnPoint(at, rng), realm,
                        nowMillis, rng, 1, kInvalidIndex);
                    if (child != NULL_ENTITY && !claims.recipients.empty())
                        world.add<InheritedLootClaims>(child, std::move(claims));
                } else if (spawnEscort(world, terrain, content, broodIndex, rarity,
                                       escortSpawnPoint(at, rng), realm, NULL_ENTITY, nowMillis,
                                       rng, 1) == NULL_ENTITY) {
                    break;
                }
            }
        }
        if (auto* brood = world.tryGet<StemCellBrood>(e); brood && !brood->redReleased) {
            brood->redReleased = true;
            spawnBloodCell(content.mobIndex("bio_red_blood_cell"), minionRarity(rarity),
                           Transform{at, angle, realm});
        }
        if (config.allySpawn.present && rng.chance(config.allySpawn.chance)) {
            for (int i = 0; i < config.allySpawn.count; ++i) {
                if (spawnEscort(world, terrain, content, config.allySpawn.mobIndex, rarity,
                                escortSpawnPoint(at, rng), realm, NULL_ENTITY, nowMillis, rng,
                                1) == NULL_ENTITY)
                    break;
            }
        }
    }
    // Keep each threshold spent for this stem cell's life. A lethal hit can
    // cross several thresholds on the same tick as the death spawn.
    const auto white = content.mobIndex("bio_white_blood_cell");
    const auto stems = Query<StemCellBrood>{world}.collect();
    for (const Entity stem : stems) {
        const auto& health = world.get<Health>(stem);
        const double damageFraction = 1.0 - health.current / health.max;
        const int crossed = std::clamp(static_cast<int>(std::floor((damageFraction + 1e-9) / 0.15)),
                                       0, 6);
        const Rarity tier = world.get<MobType>(stem).rarity;
        const Transform anchor = world.get<Transform>(stem);
        auto& brood = world.get<StemCellBrood>(stem);
        const int earned = std::max(brood.whiteEarned, crossed);
        int spawned = brood.whiteSpawned;
        while (spawned < earned && spawnBloodCell(white, tier, anchor)) ++spawned;
        auto& updated = world.get<StemCellBrood>(stem);
        updated.whiteEarned = earned;
        updated.whiteSpawned = spawned;
    }
}

// An escort is placed by its NEST, not by a band. It comes up at the parent's
// own centre (escortSpawnPoint, a unit off on a bearing of its own) and walks
// out from there, so it is routinely standing over the band's edge on ground
// no band covers soon after -- which is legal, and deliberately ungated: the
// band chose the nest, and the nest chose these. The same is true of a
// centipede's body segments (spawnBodyChain).
Entity SpawnSystem::spawnEscort(World& world, const Terrain& terrain, const ContentRegistry& content,
                                std::uint16_t childIndex, Rarity nestRarity, Vec2 at, Realm realm,
                                Entity parent, double nowMillis, Rng& rng, int depth) {
    if (census_.mobs >= mobCap) return NULL_ENTITY;
    // Every such path comes through here -- a hole's opening guard, its
    // waves, a queen's soldiers and a broken tomb's mummy -- so this is where
    // a minion normally stays at Ultra or below. Ant families explicitly
    // use one lower tier for soldiers, and the hole's own tier for its queen.
    Rarity childRarity=minionRarity(nestRarity);
    if(const auto* type=world.tryGet<MobType>(parent)){
        const auto& id=content.mob(type->configIndex).id;
        if(id=="ant_hole"||id=="queen_ant")
            childRarity=content.mob(childIndex).id=="queen_ant"?nestRarity:downgradeRarity(nestRarity);
    }
    const Entity child = spawnMobAt(world, terrain, content, childIndex, childRarity,
                                    at, realm, nowMillis, rng, depth, kInvalidIndex);
    if (child == NULL_ENTITY || parent == NULL_ENTITY) return child;

    // The leash, on all three paths that put a child into the world. Dragged
    // past the retreat radius from whatever made it, an escort drops its target
    // and walks back, so a hole cannot be stripped of its defenders by leading
    // them away one at a time (src/ecs/systems/enemyAI.ts:485).
    //
    // The parent's position is read out before the add, which relocates the row
    // it points into.
    Vec2 home = at;
    if (const Transform* anchor = world.tryGet<Transform>(parent)) home = anchor->position;
    // Emerging: it was put down in the middle of its parent, and the two must
    // not shove each other apart while it climbs out.
    HoleTether tether{parent, home, false, true};
    const auto* map = worldMaps ? worldMaps->forRealm(realm) : nullptr;
    const auto* parentType = world.tryGet<MobType>(parent);
    if (map && map->prestige() && map->templateId() == "desert" && parentType &&
        content.mob(parentType->configIndex).id == "fire_ant_hole")
        tether.retreatScale = 2.0;
    world.add<HoleTether>(child, tether);
    return child;
}

// ---------------------------------------------------------------------------
// The tick
// ---------------------------------------------------------------------------

void SpawnSystem::run(World& world, const Terrain& terrain, const ContentRegistry& content,
                      const std::vector<RealmPoint>& players, Rng& rng, double nowMillis, double dt,
                      CommandBuffer& commands) {
    bind(world);
    rebuildZones(content);
    // Stocking only appends. Waking can reorder the records, so discard the
    // indices from the preceding pass before another placement query.
    latentPlacementGrid_.clear();
    indexedLatentZones_.assign(zones_.size(), false);
    scatterBossClocks(nowMillis);
    gatherViewers(world, terrain, players);
    if(nowMillis>=nextPopulationMillis_) {
    trainingExclusions_.clear();
    const auto noteDummy=[&](std::uint16_t index, const Transform& at, const Body& body) {
        if(!isWorldRealm(at.realm))return;
        const auto& id=content.mob(index).id;
        if(id=="target_dummy" || id=="test_dummy")
            trainingExclusions_.push_back({at.realm,at.position,body.radius+450.0});
    };
    Query<Npc,Transform,Body>{world}.each([&](Entity e,Npc& npc,Transform& at,Body& body) {
        if(!world.has<Dead>(e))noteDummy(npc.configIndex,at,body);
    });
    Query<MobType,Transform,Body> dummyMobs{world};dummyMobs.without<Dead,Pet>();
    dummyMobs.each([&](Entity e,MobType& mob,Transform& at,Body& body) {
        if(!world.has<Npc>(e))noteDummy(mob.configIndex,at,body);
    });

    }
    expireEscorts(dt, commands);
    runNests(world, terrain, content, rng, nowMillis);

    Query<GhostSparkle, Transform>{world}.each([&](Entity sparkle, GhostSparkle& link, Transform& at) {
        if (!isStanding(world, link.ghost)) {
            commands.destroy(sparkle);
            return;
        }
        at = world.get<Transform>(link.ghost);
    });

    // EVERY tick, unlike everything else here, because a corpse does not wait:
    // combat marks a mob dead earlier in this very tick and the runtime reaps
    // it at the end of it, so a band that only looked twice a second would
    // miss nineteen kills in twenty and hand their slots to the band-wide
    // top-up instead -- which is to say, to somewhere else entirely.
    bankCasualties(world, terrain, content, viewers_, rng, nowMillis);

    // The census is O(mobs x players) and nothing about a population of a few
    // hundred changes meaningfully inside half a second. Stocking and waking
    // ride with it, in that order, so the whole thing is one pass. The census
    // says what is alive, the top-up writes records for whatever is missing,
    // and waking brings every record somebody can see to life, the new ones
    // included. A band that comes up short is full and awake again within
    // half a second, and the first tick of a cold server is a full map.
    if (nowMillis >= nextPopulationMillis_) {
        nextPopulationMillis_ = nowMillis + kPopulationIntervalMillis;
        cullExtraTopBosses(world, commands);
        trimMounds(world,content,commands);
        takeCensus(world, content, viewers_, nowMillis, commands);
        returnVanished(world, terrain, content, viewers_, rng, nowMillis);
        // A new DPS exclusion can invalidate an old sleeping position. Keep
        // the same stock/type/tier/deadline and rehome it inside its own band.
        int trainingRehomes=0;
        if(!trainingExclusions_.empty())for(std::size_t zi=0;zi<zones_.size() && trainingRehomes<64;++zi) {
            auto& zone=zones_[zi];
            bool nearby=false;
            for(const auto& dummy:trainingExclusions_)if(dummy.realm==zone.realm &&
                zone.bounds.left()<dummy.at.x+dummy.radius+1000 && zone.bounds.right()>dummy.at.x-dummy.radius-1000 &&
                zone.bounds.top()<dummy.at.y+dummy.radius+1000 && zone.bounds.bottom()>dummy.at.y-dummy.radius-1000){nearby=true;break;}
            if(!nearby)continue;
            for(std::size_t i=0;i<zone.latent.size() && trainingRehomes<64;++i) {
                auto& record=zone.latent[i];
                if(content.mob(record.mobIndex).neverAmbient)continue;
                const auto footprint=spawnFootprint(content,record.mobIndex,record.rarity);
                if(!nearTrainingDummy(zone.realm,record.position,footprint.radius,footprint.tail,footprint.tailRadius))continue;
                for(int attempt=0;attempt<8;++attempt) {
                    const Vec2 at=samplePointInRect(zone.bounds,terrain.realmExtent(zone.realm),rng);
                    if(!zoneContains(zone.bounds,zone.polygon,at)||terrain.blocked(at,zone.realm)||
                       nearTrainingDummy(zone.realm,at,footprint.radius,footprint.tail,footprint.tailRadius)||
                       crowdedAt(zone.realm,at,footprint.radius,kMinMobSpawnSpacing,static_cast<std::uint16_t>(zi),i,&terrain))continue;
                    const SpawnZone* roster=&zone;
                    for(const auto& region:regions_)if(region.realm==zone.realm && zoneContains(region.bounds,region.polygon,at)){roster=&region;break;}
                    const auto& mob=content.mob(record.mobIndex);
                    const auto inGroup=[&](std::uint16_t group){return group!=kInvalidIndex && std::any_of(mob.groups.begin(),mob.groups.end(),[&](const MobGroupMember& m){return m.group==group&&m.weight>0;});};
                    const auto* map=worldMaps?worldMaps->forRealm(zone.realm):nullptr;
                    const bool accepts=roster->resolved.empty()?(map&&inGroup(content.mobGroupIndex(map->defaultMobGroup()))):
                        std::any_of(roster->resolved.begin(),roster->resolved.end(),[&](const auto& row){return row.weight>0&&(row.mob==record.mobIndex||inGroup(row.group));});
                    if(!accepts || (roster->forceRarity>=0 && roster->forceRarity!=rarityIndex(record.rarity)))continue;
                    record.position=at;++trainingRehomes;
                    latentPlacementGrid_.clear();indexedLatentZones_.assign(zones_.size(),false);break;
                }
            }
        }
        // The one ambient spawn path, and it runs whether or not anybody is
        // anywhere near: the map is full at all times, and that is the whole
        // difference between this and a spawner that follows the players
        // around.
        stockSpawnZones(world, terrain, content, viewers_, rng, nowMillis);
        promoteLatent(world, terrain, content, viewers_, viewers_, rng, nowMillis);
        refillStarvedPrestigeBand(world, terrain, content, nowMillis);
        stockGhosts(world, terrain, content, nowMillis);
        wokenFrom_ = viewers_;
    } else {
        wakeAhead(world, terrain, content, rng, nowMillis);
    }

    // And that is all of it. There is no second pass behind the bands covering
    // the ground the author left unbanded -- a square no band covers grows
    // nothing, which is the point.
    //
    // The ARENA and the MAZE are not that rule's business: they are generated
    // realms with no object layer to draw a band on, and ModeSpawner
    // (mode_spawning.h) fills each one whole, twice a second
    // (kModeSpawnIntervalMillis), through spawnMob().
    // Their mobs are kept alive by the realm-occupied branch of takeCensus
    // rather than by any band, and none of them is ever latent.
}

void SpawnSystem::stockGhosts(World& world, const Terrain& terrain,
                              const ContentRegistry& content, double nowMillis) {
    constexpr int maxGhostsPerMap = 3;
    constexpr double respawnMillis = 90'000.0;
    if (!worldMaps) return;
    const auto ghost = content.mobIndex("ghost");
    if (ghost == kInvalidIndex) return;

    std::array<int, kMaxRealms> standing{};
    Query<GhostTag, Transform, AmbientMob>{world}.each(
        [&](Entity e, GhostTag&, Transform& at, AmbientMob& ambient) {
            if (ambient.independentGhost && isStanding(world, e))
                ++standing[realmIndex(at.realm)];
        });

    for (const auto& map : worldMaps->maps()) {
        const Realm realm = map.realm();
        const auto index = realmIndex(realm);
        if (standing[index] >= maxGhostsPerMap || nowMillis < nextGhostSpawnMillis_[index] ||
            riftRealms_[index] || inactiveLayoutRealms_[index]) continue;
        if (!map.usesReferenceSpawnCurve() ||
            map.templateId() == "hel" || map.biome() == "jurassic") continue;

        // Ghosts use their own random stream. Ordinary stock and boss rolls
        // do not depend on whether a ghost vacancy can be filled.
        Rng rng{static_cast<std::uint64_t>(nowMillis) + index};
        nextGhostSpawnMillis_[index] = nowMillis + 5'000.0;
        Vec2 at;
        if (!randomBandPoint(terrain, realm, rng, at)) continue;
        const auto zone = std::find_if(zones_.begin(), zones_.end(), [&](const SpawnZone& band) {
            return band.realm == realm && !band.singular && band.forceRarity < 0 &&
                   zoneContains(band.bounds, band.polygon, at);
        });
        if (zone == zones_.end()) continue;

        Rarity rarity = rollReferenceSpawnRarity(zone->difficulty, kNeutralSpawnLuck, rng);
        rarity = zone->prestige ? rollPrestigeAmbientRarity(rarity, rng) : rarity;
        rarity = std::min(rarity, zone->prestige ? Rarity::Super : Rarity::Ultra);
        const auto footprint = spawnFootprint(content, ghost, rarity);
        const auto facing = wakingFacing(viewers_, realm, at, footprint, kMinSpawnDistance, rng);
        if (!clearOfEveryLap(viewers_, realm, at, footprint, facing, kMinSpawnDistance)) continue;

        const Entity spawned = spawnMobAt(world, terrain, content, ghost, rarity, at, realm,
                                          nowMillis, rng, 0, kInvalidIndex, facing, false);
        if (spawned == NULL_ENTITY) continue;
        world.get<AmbientMob>(spawned).independentGhost = true;
        // spawnMobAt counts all new world bodies by default. This one is
        // counted against its own map limit, including on the next census.
        --census_.mobs;
        ++standing[index];
        nextGhostSpawnMillis_[index] = nowMillis + respawnMillis;
    }
}

void SpawnSystem::gatherViewers(World& world, const Terrain& terrain,
                                const std::vector<RealmPoint>& players) {
    // Every flower on an authored map, with the two facts a coordinate cannot
    // carry. A client that reported nothing keeps the default box, exactly as
    // the reference's `player.viewportWidth || VIEWPORT_WIDTH` does.
    worldViewers_.clear();
    playerBodies_->each([&](Entity e, PlayerTag&, Transform& transform) {
        // Every flower on an authored MAP, not only the overworld's: spawn
        // bands exist on every map and each one is stocked for the people
        // looking at it. The arena and the maze are populated whole, by
        // ModeSpawner, so their flowers are not viewers here.
        if (!isWorldRealm(transform.realm)) return;
        Viewer viewer;
        viewer.identity=e;
        viewer.position = transform.position;
        viewer.realm = transform.realm;
        if (const PlayerLocation* location = world.tryGet<PlayerLocation>(e)) {
            viewer.half = {location->viewport.x * 0.5 + kViewportBuffer,
                           location->viewport.y * 0.5 + kViewportBuffer};
        }
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e)) {
            viewer.luck = modifiers->luck;
        }
        // The hitbox the spawn clearance is measured from. Size modifiers put
        // it several times over the base, and a spawn gap measured from the
        // centre of a flower that big lands inside it.
        if (const Body* body = world.tryGet<Body>(e)) viewer.radius = body->radius;
        worldViewers_.push_back(viewer);
    });

    // The caller's list stays the list -- it decides WHO the bands are stocked
    // and the population kept for -- and each entry is only paired with the
    // flower standing on it. One that pairs with nothing is a bare coordinate
    // from a harness, and keeps the defaults above.
    viewers_.clear();
    viewers_.reserve(players.size());
    realmOccupied_.fill(false);
    for (const RealmPoint& player : players) {
        realmOccupied_[realmIndex(player.realm)] = true;
        if (!isWorldRealm(player.realm)) continue;
        const Vec2 position = player.position;
        Viewer viewer;
        viewer.sourceToken=viewers_.size()+1;
        viewer.position = position;
        // The realm is the caller's fact about the point, not something the
        // pairing below discovers: a coordinate on a second map's door and
        // the same numbers in the overworld are two different places, and a
        // viewer filed under the wrong one stocks the wrong map.
        viewer.realm = player.realm;
        double nearestDistSq = kViewerMatchRadius * kViewerMatchRadius;
        for (const Viewer& candidate : worldViewers_) {
            if (candidate.realm != player.realm) continue;
            const double distSq = distanceSq(candidate.position, position);
            if (distSq > nearestDistSq) continue;
            if(distSq==nearestDistSq) viewer.identity=NULL_ENTITY;
            else viewer.identity=candidate.identity;
            nearestDistSq = distSq;
            viewer.half = candidate.half;
            viewer.luck = candidate.luck;
            viewer.radius = candidate.radius;
        }
        const Vec2 viewport = (viewer.half - Vec2{kViewportBuffer, kViewportBuffer}) * 2.0;
        viewer.viewportCentre = boundedViewportCentre(position, viewport, terrain.cameraExtent(viewer.realm));
        viewers_.push_back(viewer);
    }
}

void SpawnSystem::expireEscorts(double dt, CommandBuffer& commands) {
    doomed_.clear();
    escorts_->each([&](Entity e, AmbientMob&, Lifetime& lifetime) {
        // A zero remainder means "no timer": only nest escorts are given one,
        // and an ambient mob must not evaporate because the field defaulted.
        if (lifetime.remainingSeconds <= 0.0) return;
        lifetime.remainingSeconds -= dt;
        if (lifetime.remainingSeconds <= 0.0) doomed_.push_back(e);
    });
    // Destroyed, not killed: an escort running out of time is bookkeeping, and
    // marking it Dead would pay out XP and loot for a mob nobody fought.
    for (const Entity e : doomed_) commands.destroy(e);
}

void SpawnSystem::takeCensus(World& world, const ContentRegistry& content,
                             const std::vector<Viewer>& viewers, double nowMillis,
                             CommandBuffer& commands) {
    const bool prioritiseVisible = census_.mobs >= mobCap * 9 / 10;
    census_.mobs = 0;
    doomed_.clear();

    // Band mobs the last census counted that are gone now. A KILLED one was
    // banked and taken off the list the tick it died, and one put to sleep was
    // never put on it, so anything missing was removed some other way -- an
    // operator's killall, the cull of a second unique -- and its band is owed
    // the slot back where it stood (returnVanished).
    vanished_.clear();
    for (const CensusMob& tracked : censusMobs_) {
        if (!world.isAlive(tracked.entity)) vanished_.push_back(tracked);
    }
    censusMobs_.clear();

    mobPlacements_.clear();
    placementGrid_.clear();
    // Recounted from the world rather than tracked through every death: a mob
    // dies in combat, is reaped by the runtime and is never reported here, so
    // a running total would drift downward forever and its band would stop
    // replacing anything.
    for (SpawnZone& zone : zones_) zone.liveMobs = 0;

    // Nobody connected means nobody has failed to see anything. The reference's
    // near-a-player test answers true when its box list is empty, and that
    // permissive default is what keeps an unattended server populated instead
    // of emptying itself and handing the next arrival a barren map.
    const bool unattended = viewers.empty();

    connectedRoots_.clear();
    ambientChains_->each([&](Entity e, BodySegment& segment, Transform& at, AmbientMob&) {
        if (!isWorldRealm(at.realm) || segment.ahead == NULL_ENTITY) return;
        Entity root = e;
        const BodySegment* link = &segment;
        for (int i = 0; i < kCentipedeSegmentCount && link->ahead != NULL_ENTITY; ++i) {
            const Entity leader = link->ahead;
            if (world.has<Dead>(leader)) break;
            link = world.tryGet<BodySegment>(leader);
            if (!link) break;
            root = leader;
        }
        auto* activity = root != e ? world.tryGet<AmbientMob>(root) : nullptr;
        if (!activity) return;
        connectedRoots_.emplace(e, root);
        // Seeing either end keeps the connected section alive. A broken
        // link has its own root and can go to sleep independently.
        if (!unattended && seenBy(viewers, at.realm, at.position,
                                 latentWakeMargin + kLatentSleepMargin + (world.tryGet<Body>(e) ? world.get<Body>(e).radius : 0.0))) {
            activity->lastNearPlayerMillis = nowMillis;
        }
    });

    ambient_->each([&](Entity e, MobTag&, Transform& transform, Body& body, MobType& type,
                       AmbientMob& ambient) {
        // An arena or maze mob is kept for as long as anyone is in its realm:
        // those realms are populated whole rather than by viewport, and the
        // reference exempts their mobs from the distance despawn on the same
        // condition (mazeSpawner.ts hasMazePlayers). Once the realm empties
        // the usual grace period runs and the population drains.
        if (!isWorldRealm(transform.realm)) {
            if (realmOccupied_[realmIndex(transform.realm)]) {
                ambient.lastNearPlayerMillis = nowMillis;
            } else if (nowMillis - ambient.lastNearPlayerMillis >= kMobDespawnDelayMillis) {
                doomed_.push_back(e);
            }
            return;
        }
        if (ambient.independentGhost) {
            if (realmOccupied_[realmIndex(transform.realm)]) ambient.lastNearPlayerMillis = nowMillis;
            else if (nowMillis - ambient.lastNearPlayerMillis >= kMobDespawnDelayMillis)
                doomed_.push_back(e);
            return;
        }
        // Each flower's OWN box, not one 1920x1080 rectangle for everybody: a
        // mob at the edge of an ultrawide screen is being drawn, and starting
        // its recycle clock is what makes it blink out in front of its owner
        // (src/server/playerState.ts:1041). Grown past the WAKE box by
        // kLatentSleepMargin, so nothing promoteLatent wakes is already
        // outside it.
        // An Undead mob is dying on a clock and owes its killer a death: it
        // stays up until it has had it, rather than going back to a record
        // that would wake whole.
        const bool nearAnyone =
            unattended || connectedRoots_.find(e) != connectedRoots_.end() ||
            world.has<KeepAwake>(e) || world.has<Undead>(e) ||
            seenBy(viewers, transform.realm, transform.position,
                   latentWakeMargin + kLatentSleepMargin + body.radius);

        // A band mob goes back to being a record, which loses nothing, so it
        // goes soon; anything else is destroyed for good, so it gets longer.
        const bool sleeps = ambient.zone < zones_.size();
        const double graceMillis = sleeps ? kLatentSleepDelayMillis : kMobDespawnDelayMillis;
        const bool reclaimOffscreen = prioritiseVisible && !unattended && sleeps &&
            !world.has<BodySegment>(e) && !world.has<KeepAwake>(e) && !world.has<Undead>(e) &&
            !seenBy(viewers, transform.realm, transform.position,
                    std::max(0.0, body.radius) + kViewportBuffer);
        if (nearAnyone && !reclaimOffscreen) {
            ambient.lastNearPlayerMillis = nowMillis;
        } else if ((reclaimOffscreen || nowMillis - ambient.lastNearPlayerMillis >= graceMillis) &&
                   (world.has<StemCellOffspring>(e) ||
                    !alwaysAwake(content, type, prestigeRealm(transform.realm)))) {
            // Nobody has been near it for the grace period, so it stops being
            // an entity. Whether that is the END of it depends on whether a
            // band is holding a slot for it: one the band placed goes back
            // into that band's records AT THE POSITION IT WANDERED TO, so the
            // map stays exactly as full as it was and the mob is still there
            // to be found. A nest's escort, a centipede's segment, an arena
            // mob or something an operator conjured has no band, and is simply
            // destroyed as it always was.
            if (sleeps) {
                Vec2 restAt = transform.position;
                if (const auto* home = world.tryGet<SilverfishHome>(e);
                    home && !seenBy(viewers, transform.realm, home->position,
                                   latentWakeMargin + body.radius)) {
                    restAt = home->position;
                }
                addLatent(ambient.zone,
                          LatentMob{restAt, nowMillis, type.configIndex, type.rarity,
                                    ambient.replacedSpecies, world.has<GhostTag>(e), true, false, ambient.managedPopulation},content);
                ++census_.demotedTotal;
                sleepers_.push_back(e);
            }
            // Left out of the counts on purpose: it is on its way out, and
            // counting it would suppress the replacement spawn for one pass.
            doomed_.push_back(e);
            return;
        }

        ++census_.mobs;
        if (ambient.zone < zones_.size()) {
            ++zones_[ambient.zone].liveMobs;
            censusMobs_.push_back(CensusMob{e, transform.position, ambient.zone,
                                           ambient.managedPopulation,
                                           rarityIndex(type.rarity) <= rarityIndex(Rarity::Super)
                                               ? (ambient.replacedSpecies != kInvalidIndex ? ambient.replacedSpecies : type.configIndex)
                                               : kInvalidIndex});
        }
        // The placement record the stocking pass spaces its next spawns
        // against.
        notePlacement(transform.realm, transform.position, body.radius*spawnSpacingScale(content.mob(type.configIndex)));
    });

    // A connected chain leaves together when its root is culled.
    if (!doomed_.empty() && !connectedRoots_.empty()) {
        std::sort(doomed_.begin(), doomed_.end());
        const auto roots = static_cast<std::ptrdiff_t>(doomed_.size());
        for (const auto& [segment, root] : connectedRoots_) {
            if (std::binary_search(doomed_.begin(), doomed_.begin() + roots, root)) {
                doomed_.push_back(segment);
                --census_.mobs;
            }
        }
    }

    // The escorts have no band slot of their own; the nest will grow a fresh
    // set when its record wakes again.
    if (!sleepers_.empty()) {
        std::sort(sleepers_.begin(), sleepers_.end());
        // Already going, on its own clock or as a sleeper, and so not counted.
        std::sort(doomed_.begin(), doomed_.end());
        const std::size_t goneAlready = doomed_.size();
        const auto slept = [&](Entity parent) {
            return std::binary_search(sleepers_.begin(), sleepers_.end(), parent);
        };
        const auto follow = [&](Entity e) {
            doomed_.push_back(e);
            --census_.mobs;   // it was counted on the walk above
        };
        dependents_->each([&](Entity e, MobTag&, AmbientMob&) {
            if (std::binary_search(doomed_.begin(),
                                   doomed_.begin() + static_cast<std::ptrdiff_t>(goneAlready), e)) {
                return;
            }
            const HoleTether* tether = world.tryGet<HoleTether>(e);
            if (tether != nullptr && slept(tether->hole)) follow(e);
        });
        sleepers_.clear();
    }

    for (const Entity e : doomed_) commands.destroy(e);
    census_.despawnedTotal += static_cast<int>(doomed_.size());

    census_.latent = latentCount();
}

bool SpawnSystem::crowdedWakingBody(const Terrain& terrain,const ContentRegistry& content,
        const LatentMob& record,const SpawnZone& zone,std::uint16_t zoneIndex,std::size_t recordIndex,
        std::optional<double> facing) {
    const auto footprint=spawnFootprint(content,record.mobIndex,record.rarity);
    if(crowdedAt(zone.realm,record.position,footprint.radius*spawnSpacingScale(content.mob(record.mobIndex)),kMinMobSpawnSpacing,zoneIndex,recordIndex,&terrain,false))return true;
    const auto& mob=content.mob(record.mobIndex);
    if(mob.segmentCount<=0 || mob.segmentBodyIndex==kInvalidIndex || !facing)return false;
    const auto& bodyConfig=content.mob(mob.segmentBodyIndex);
    const double radius=content.mobStats(mob.segmentBodyIndex,record.rarity).radius*
        bodyConfig.maxRandomSize()*bodyConfig.fixtureSizeScale(record.rarity)*kSpawnedMobBodyScale;
    Vec2 at=record.position;double leaderRadius=footprint.radius;
    const Vec2 back=Vec2::fromAngle(*facing+kPi);
    for(int segment=0;segment<mob.segmentCount;++segment) {
        const double spacing=mob.segmentSpacing(leaderRadius,radius);leaderRadius=radius;
        const Vec2 next=terrain.resolveLinkedCircle(at,back*spacing,kMobWallRadius,zone.realm);
        const Rect swept{std::min(at.x,next.x)-radius-kMinMobSpawnSpacing,
                         std::min(at.y,next.y)-radius-kMinMobSpawnSpacing,
                         std::abs(next.x-at.x)+2*(radius+kMinMobSpawnSpacing),
                         std::abs(next.y-at.y)+2*(radius+kMinMobSpawnSpacing)};
        placementGrid_.queryRect(zone.realm,swept,nearbyPlacements_);
        for(Entity key:nearbyPlacements_) {
            const auto& other=mobPlacements_[static_cast<std::size_t>(key)-1];
            const double reach=std::min(radius*spawnSpacingScale(bodyConfig),kPreliminarySpawnRadius)+other.radius+kMinMobSpawnSpacing;
            if(distanceToSegmentSq(other.position,at,next)<reach*reach &&
               !terrain.segmentBlocked(next,other.position,zone.realm))return true;
        }
        at=next;
    }
    return false;
}

double SpawnSystem::naturalSpawnGap(const SpawnZone& zone,const Viewer& viewer,double radius) const {
    if(!zone.prestige||zone.singular||zone.forceRarity>=0 || viewer.realm!=zone.realm || viewer.identity==NULL_ENTITY)
        return kMinSpawnDistance;
    const bool established=std::any_of(wokenFrom_.begin(),wokenFrom_.end(),[&](const Viewer& old) {
        return old.identity==viewer.identity && old.realm==viewer.realm && distanceSq(old.position,viewer.position)<350.0*350.0;
    });
    if(!established)return kMinSpawnDistance;
    const double dx=std::max(std::abs(viewer.position.x-zone.bounds.left()),std::abs(viewer.position.x-zone.bounds.right()));
    const double dy=std::max(std::abs(viewer.position.y-zone.bounds.top()),std::abs(viewer.position.y-zone.bounds.bottom()));
    const double full=kMinSpawnDistance+viewer.radius+radius;
    if(dx*dx+dy*dy<full*full)return 120.0;
    return kMinSpawnDistance;
}

bool SpawnSystem::clearNaturalSpawn(const SpawnZone& zone,const std::vector<Viewer>& viewers,Vec2 at,
        double radius,double tail,double tailRadius,std::optional<double> facing) const {
    for(const auto& viewer:viewers) {
        if(viewer.realm!=zone.realm)continue;
        const double gap=naturalSpawnGap(zone,viewer,radius);
        const double headReach=gap+radius+viewer.radius;
        if(distanceSq(viewer.position,at)<headReach*headReach)return false;
        if(tail>0.0) {
            const double reach=gap+tailRadius+viewer.radius;
            if(facing) {
                const Vec2 end=at-Vec2::fromAngle(*facing)*tail;
                if(distanceToSegmentSq(viewer.position,at,end)<reach*reach)return false;
            } else if(distanceSq(viewer.position,at)<(reach+tail)*(reach+tail))return false;
        }
    }
    return true;
}

bool SpawnSystem::nearTrainingDummy(Realm realm, Vec2 at, double radius, double tail,
                                    double tailRadius, std::optional<double> facing) const {
    for(const auto& dummy:trainingExclusions_) {
        if(dummy.realm!=realm)continue;
        const double gap=dummy.radius+radius;
        if(distanceSq(dummy.at,at)<gap*gap)return true;
        if(tail>0.0) {
            if(!facing) {
                const double allGap=dummy.radius+tail+tailRadius;
                if(distanceSq(dummy.at,at)<allGap*allGap)return true;
            } else {
                const Vec2 end=at-Vec2::fromAngle(*facing)*tail;
                const double chainGap=dummy.radius+tailRadius;
                if(distanceToSegmentSq(dummy.at,at,end)<chainGap*chainGap)return true;
            }
        }
    }
    return false;
}

bool SpawnSystem::crowdedAt(Realm realm, Vec2 position, double halfSize, double extraGap,
                            std::uint16_t zoneIndex, std::size_t ignoreLatent, const Terrain* placementTerrain, bool respectDormant) {
    // Keep the authored cold-stock spacing. Giant mob circles overlap during
    // normal play and movement separates them; reserving full visual/body
    // diameters here would silently lower the established monster density.
    halfSize=std::min(halfSize,kPreliminarySpawnRadius);
    if(nearTrainingDummy(realm,position,halfSize))return true;
    placementGrid_.query(realm, position, halfSize + extraGap, nearbyPlacements_);
    for (const Entity key : nearbyPlacements_) {
        const MobPlacement& mob = mobPlacements_[static_cast<std::size_t>(key) - 1];
        const double reach = halfSize + mob.radius + extraGap;
        if (distanceSq(mob.position, position) < reach * reach &&
            (!placementTerrain || !placementTerrain->segmentBlocked(position,mob.position,realm))) return true;
    }
    if(!respectDormant)return false;
    // The band's own records. They carry no radius -- a record is a type and a
    // point, and the body it will grow is rolled when it wakes -- so they are
    // spaced by the same nominal gap two spawn ATTEMPTS are, which is what
    // keeps a stocked band from settling into clumps.
    const double reach = halfSize + kPreliminarySpawnRadius + extraGap;
    const double reachSq = reach * reach;
    const auto& latent = zones_[zoneIndex].latent;
    constexpr std::size_t minIndexedPlacements = 96;
    if (latent.size() < minIndexedPlacements) {
        for (std::size_t i=0;i<latent.size();++i) {
            if(i!=ignoreLatent && distanceSq(latent[i].position, position) < reachSq &&
                (!placementTerrain || !placementTerrain->segmentBlocked(position,latent[i].position,realm))) return true;
        }
        return false;
    }
    if (!indexedLatentZones_[zoneIndex]) {
        for (std::size_t i = 0; i < latent.size(); ++i)
            latentPlacementGrid_.insert(makeEntity(static_cast<std::uint32_t>(i + 1), zoneIndex),
                                        realm, latent[i].position);
        indexedLatentZones_[zoneIndex] = true;
    }
    const Vec2 extent{reach, reach};
    latentPlacementGrid_.queryPointBuckets(realm, position - extent, position + extent, nearbyPlacements_);
    for (const Entity key : nearbyPlacements_) {
        if (entityGeneration(key) != zoneIndex || entityIndex(key)-1==ignoreLatent) continue;
        if (distanceSq(latent[entityIndex(key) - 1].position, position) < reachSq &&
            (!placementTerrain || !placementTerrain->segmentBlocked(position,latent[entityIndex(key)-1].position,realm))) return true;
    }
    return false;
}

void SpawnSystem::notePlacement(Realm realm, Vec2 position, double radius) {
    radius=std::min(radius,kPreliminarySpawnRadius);
    mobPlacements_.push_back({position, radius});
    // The grid refers to the placement record, whose position stays fixed
    // between population passes. Slot zero is reserved by SpatialGrid.
    placementGrid_.insert(static_cast<Entity>(mobPlacements_.size()), realm, position, std::abs(radius));
}

void SpawnSystem::addLatent(std::uint16_t zoneIndex, LatentMob record, const ContentRegistry& content) {
    auto& zone = zones_[zoneIndex];
    zone.latent.push_back(record);
    const auto footprint=spawnFootprint(content,record.mobIndex,record.rarity);
    zone.maxDormantRadius=std::max(zone.maxDormantRadius,footprint.radius);
    zone.maxDormantReach=std::max(zone.maxDormantReach,std::max(footprint.radius,footprint.tail+footprint.tailRadius));
    if (indexedLatentZones_[zoneIndex]) {
        latentPlacementGrid_.insert(makeEntity(static_cast<std::uint32_t>(zone.latent.size()), zoneIndex),
                                    zone.realm, record.position);
    }
}

void SpawnSystem::noteLiveBandMob(World& world, Entity e, std::uint16_t zone, const ContentRegistry& content) {
    const Transform* transform = world.tryGet<Transform>(e);
    if (transform == nullptr) return;
    const Body* body = world.tryGet<Body>(e);
    const auto* type=world.tryGet<MobType>(e);
    const double contactScale=type?spawnSpacingScale(content.mob(type->configIndex)):1.0;
    notePlacement(transform->realm, transform->position, body != nullptr ? body->radius*contactScale : 0.0);
    // Every segment exists already; index the same-pass real positions now,
    // before another cold or rescued record can wake into this animal's tail.
    Entity segment=e;
    for(int links=0;links<256;++links) {
        const auto* link=world.tryGet<BodySegment>(segment);
        if(!link || link->behind==NULL_ENTITY || !world.isAlive(link->behind))break;
        segment=link->behind;
        const auto* at=world.tryGet<Transform>(segment);const auto* hull=world.tryGet<Body>(segment);
        if(at && hull) {
            const auto* kind=world.tryGet<MobType>(segment);
            const double scale=kind?spawnSpacingScale(content.mob(kind->configIndex)):1.0;
            notePlacement(at->realm,at->position,hull->radius*scale);
        }
    }
    if (zone < zones_.size()) {
        const auto* ambient = world.tryGet<AmbientMob>(e);
        const auto* type = world.tryGet<MobType>(e);
        censusMobs_.push_back(CensusMob{e, transform->position, zone,
                                      ambient && ambient->managedPopulation,
                                      type && rarityIndex(type->rarity) <= rarityIndex(Rarity::Super)
                                          ? (ambient && ambient->replacedSpecies != kInvalidIndex
                                                 ? ambient->replacedSpecies : type->configIndex)
                                          : kInvalidIndex});
    }
}

bool SpawnSystem::seenBy(const std::vector<Viewer>& viewers, Realm realm, Vec2 position,
                         double margin) const {
    for (const Viewer& viewer : viewers) {
        if (viewer.realm != realm) continue;
        const Vec2 offset = position - viewer.viewportCentre;
        if (std::abs(offset.x) <= viewer.half.x + margin &&
            std::abs(offset.y) <= viewer.half.y + margin) {
            return true;
        }
    }
    return false;
}

void SpawnSystem::runNests(World& world, const Terrain& terrain, const ContentRegistry& content,
                           Rng& rng, double nowMillis) {
    // Both loops snapshot their nests first: spawning an escort creates
    // entities, which relocates the very columns the query would be walking.
    spawners_->collect(scratchChildren_);
    for (const Entity nest : scratchChildren_) {
        Spawner* spawner = world.tryGet<Spawner>(nest);
        const Transform* transform = world.tryGet<Transform>(nest);
        const MobType* type = world.tryGet<MobType>(nest);
        if (spawner == nullptr || transform == nullptr || type == nullptr) continue;

        // Handles rather than a counter: a counter leaks a slot every time a
        // child dies somewhere else, and the nest goes quiet forever.
        std::size_t live = 0;
        for (const Entity child : spawner->children) {
            if (world.isAlive(child)) spawner->children[live++] = child;
        }
        spawner->children.resize(live);

        // Weighed every tick, not only when a child is due, so "the last
        // player to hit it" means the last one and not the biggest hitter
        // over a whole interval.
        const Entity hitter = freshestAttacker(world, world.tryGet<Bounty>(nest),
                                               transform->realm, spawner->ledgerSeen);
        if (hitter != NULL_ENTITY) spawner->lastAttacker = hitter;

        if (nowMillis < spawner->nextSpawnMillis) continue;
        spawner->nextSpawnMillis = nowMillis + std::max(1.0, spawner->intervalMillis);
        if (static_cast<int>(live) >= spawner->maxAlive) continue;

        // Read everything out before spawning: `spawner` points into an
        // archetype column and does not survive a create().
        const std::uint16_t childIndex = spawner->childConfigIndex;
        const Rarity childRarity = clampLadderRarity(rarityIndex(type->rarity) + spawner->rarityOffset);
        const double lifetimeMillis = spawner->childLifetimeMillis;
        const Vec2 anchor = transform->position;
        const Realm nestRealm = transform->realm;
        const Entity attacker =
            attackerUsable(world, spawner->lastAttacker, nestRealm) ? spawner->lastAttacker
                                                                    : NULL_ENTITY;

        Vec2 childAt = escortSpawnPoint(anchor, rng);
        if (content.mob(type->configIndex).id == "pharaoh_beetle") {
            const auto* parent = world.tryGet<Body>(nest);
            if (parent == nullptr) continue;
            const auto escortTier = clampLadderRarity(std::max(rarityIndex(minionRarity(childRarity)),
                rarityIndex(content.mob(childIndex).minRarity)));
            const double childRadius = spawnFootprint(content, childIndex, escortTier).radius *
                kSpawnedMobBodyScale;
            const double clearance = parent->radius + childRadius + 4.0;
            const double bearing = (childAt - anchor).angle();
            bool placed = false;
            for (int direction = 0; direction < 12; ++direction) {
                const Vec2 point = anchor + Vec2::fromAngle(bearing + direction * kTau / 12, clearance);
                if (terrain.blocked(point, nestRealm) || terrain.segmentBlocked(anchor, point, nestRealm) ||
                    distanceSq(terrain.resolveCircle(point, childRadius, nestRealm), point) > 1e-6)
                    continue;
                childAt = point;
                placed = true;
                break;
            }
            if (!placed) continue;
        }
        const Entity child = spawnEscort(world, terrain, content, childIndex, childRarity,
                                         childAt, nestRealm, nest,
                                         nowMillis, rng, 1);
        if (child == NULL_ENTITY) continue;
        provokeIfNeutral(world, child, attacker);
        if (lifetimeMillis > 0.0) {
            world.add<Lifetime>(child, Lifetime{lifetimeMillis / 1000.0});
        }
        if (Spawner* again = world.tryGet<Spawner>(nest)) again->children.push_back(child);
    }

    waveNests_->collect(scratchChildren_);
    for (const Entity nest : scratchChildren_) {
        NestWaves* waves = world.tryGet<NestWaves>(nest);
        const Transform* transform = world.tryGet<Transform>(nest);
        const MobType* type = world.tryGet<MobType>(nest);
        const Health* health = world.tryGet<Health>(nest);
        if (waves == nullptr || transform == nullptr || type == nullptr || health == nullptr) {
            continue;
        }
        // Combat precedes this pass. A one-shot still crosses the hole's
        // health bands before the end-of-tick reaper removes its corpse.
        if(world.has<Dead>(nest)&&content.mob(waves->mobIndex).id!="ant_hole")continue;

        std::size_t live = 0;
        for (const Entity child : waves->children) {
            if (world.isAlive(child)) waves->children[live++] = child;
        }
        waves->children.resize(live);

        // A hole answers damage, not a clock. Healing (or being spawned) only
        // moves the mark, so an untouched hole never sends anything at all.
        const double current = health->current;
        const double previous = waves->previousHealth;
        waves->previousHealth = current;
        // Weighed every tick, band or no band, so a share's growth is always
        // this tick's hit and never a heal or a refused swing banked earlier.
        const Entity attacker = freshestAttacker(world, world.tryGet<Bounty>(nest),
                                                 transform->realm, waves->ledgerSeen);
        const MobConfig& config = content.mob(waves->mobIndex);
        // A zorr nest's spawnOnHurt has no authored waves: every band of its
        // health lets out the one rung of its table for the nest's tier.
        const bool brood = config.spawnWaves.empty();
        if (brood && config.hurtSpawn.empty()) continue;
        const int lastWave = brood ? std::max(1, config.hurtSpawnBands) - 1
                                   : static_cast<int>(config.spawnWaves.size()) - 1;
        const double maxHealth=std::max(1.0,health->max);
        const Rarity nestRarity=type->rarity;
        const Vec2 anchor=transform->position;
        const Realm nestRealm=transform->realm;
        if(current<previous){
            const int released=clamp(1+static_cast<int>(std::floor(clamp(1.0-current/maxHealth,0.0,1.0)*lastWave)),1,lastWave+1);
            while(waves->nextWave<released){
                if (brood) {
                    const NestBroodRung* rung = broodRungForTier(config.hurtSpawn, rarityIndex(nestRarity));
                    for (int k = 0; rung != nullptr && rung->mobIndex != kInvalidIndex && k < rung->count; ++k)
                        waves->pending.emplace_back(rung->mobIndex, attacker);
                    ++waves->nextWave;
                    continue;
                }
                for(const auto member:config.spawnWaves[waves->nextWave]){
                    if(config.id=="ant_hole"&&content.mob(member).id=="queen_ant")continue;
                    waves->pending.emplace_back(member,attacker);
                }
                ++waves->nextWave;
            }
            if(config.id=="ant_hole"&&!waves->queenReleased&&current<maxHealth*.5){
                const auto queen=content.mobIndex("queen_ant");
                if(queen!=kInvalidIndex)waves->pending.emplace_back(queen,attacker);
                waves->queenReleased=true;
            }
        }
        // A full live population delays the wave instead of discarding it.
        // Copy before creates: spawning can relocate the nest's ECS row.
        const auto pending=std::move(waves->pending);waves->pending.clear();
        for(std::size_t n=0;n<pending.size();++n){
            const auto child=spawnEscort(world,terrain,content,pending[n].first,nestRarity,
                escortSpawnPoint(anchor,rng),nestRealm,nest,nowMillis,rng,1);
            auto* again=world.tryGet<NestWaves>(nest);if(!again)break;
            if(child==NULL_ENTITY){again->pending.insert(again->pending.end(),pending.begin()+n,pending.end());break;}
            again->children.push_back(child);provokeIfNeutral(world,child,pending[n].second);
        }
    }

    // Ambush nests. Silent until a flower comes right up to one, then the
    // whole brood at once, and nothing after that. The nest's DEATH is not
    // decided here: it falls on the tick its last defender does, in combat
    // (CombatSystem::collapseClearedNest), so all this pass owns is when it
    // opens and what becomes of one whose brood went away unbeaten.
    ambushNests_->collect(scratchChildren_);
    for (const Entity nest : scratchChildren_) {
        AmbushNest* ambush = world.tryGet<AmbushNest>(nest);
        const Transform* transform = world.tryGet<Transform>(nest);
        const Body* body = world.tryGet<Body>(nest);
        const MobType* type = world.tryGet<MobType>(nest);
        if (ambush == nullptr || transform == nullptr || body == nullptr || type == nullptr) {
            continue;
        }

        if (ambush->released) {
            std::size_t live = 0;
            for (const Entity member : ambush->brood) {
                if (world.isAlive(member) && !world.has<Dead>(member)) {
                    ambush->brood[live++] = member;
                }
            }
            ambush->brood.resize(live);
            if (live > 0) continue;
            // Out, every one of them gone, and the nest still standing: they
            // were taken away rather than beaten -- recycled with nobody near,
            // or removed by an operator -- because a brood killed to the last
            // takes its nest with it before this pass ever runs. Armed again,
            // on a clean ledger: credit for a fight nobody finished is no
            // share of the next one.
            ambush->released = false;
            ambush->broodHealth = 0;
            if (Bounty* bounty = world.tryGet<Bounty>(nest)) bounty->contributors.clear();
            continue;
        }

        const AmbushSpec& spec = content.mob(type->configIndex).ambush;
        if (!spec.present) continue;
        // Read out before the first create() below, which relocates every row
        // these point into.
        const Vec2 mouth = transform->position;
        const Realm nestRealm = transform->realm;
        const Rarity nestRarity = type->rarity;
        const double rim = body->radius * (1.0 + spec.triggerRadiusScale);

        // Edge to edge, so a flower grown huge by its petals springs it from
        // as far off as it LOOKS; the gap itself is in the hole's own radii,
        // so it grows with the hole -- size roll included -- and a super hole
        // isn't sprung only by a flower already standing on its rim.
        Entity intruder = NULL_ENTITY;
        livingFlowers_->each([&](Entity flower, PlayerTag&, Transform& at, Body& flowerBody,
                                 Health& health) {
            if (intruder != NULL_ENTITY || at.realm != nestRealm || !health.alive()) return;
            const double reach = rim + flowerBody.radius;
            if (distanceSq(at.position, mouth) <= reach * reach) intruder = flower;
        });
        if (intruder == NULL_ENTITY) continue;

        double broodHealth = 0;
        for (int i = 0; i < spec.count; ++i) {
            const Entity child =
                spawnEscort(world, terrain, content, spec.mobIndex, nestRarity,
                            escortSpawnPoint(mouth, rng), nestRealm, nest, nowMillis, rng, 1);
            if (child == NULL_ENTITY) break;   // the global cap, nothing else
            // A hostile brood finds the flower on its own; a neutral one is
            // told who walked up.
            provokeIfNeutral(world, child, intruder);
            if (const Health* health = world.tryGet<Health>(child)) broodHealth += health->max;
            if (AmbushNest* again = world.tryGet<AmbushNest>(nest)) again->brood.push_back(child);
        }
        // Released even when the cap let nothing out: the empty brood re-arms
        // it on the next pass, which is the retry.
        if (AmbushNest* again = world.tryGet<AmbushNest>(nest)) {
            again->released = true;
            again->broodHealth = broodHealth;
        }
    }
}

// ---------------------------------------------------------------------------
// Spawn rectangles
// ---------------------------------------------------------------------------

void SpawnSystem::setRiftSpawning(double rarityProgress, const std::string& pyramid) {
    riftDifficulty_ = 55 * clamp(rarityProgress, 0.0, 1.0);
    if (!worldMaps) return;
    for (const MapData& map : worldMaps->maps()) {
        if (map.id().rfind("rift_", 0) != 0) continue;
        const auto index = realmIndex(map.realm());
        riftRealms_[index] = true;
        inactiveRiftRealms_[index] = map.id() != "rift_hel" &&
            (pyramid.empty() || (map.id().rfind("rift_pyramid", 0) == 0 && map.id() != pyramid));
    }
}

void SpawnSystem::clearRiftPopulation() {
    for (SpawnZone& zone : zones_) {
        if (!riftRealms_[realmIndex(zone.realm)]) continue;
        zone.latent.clear();
        zone.pendingSpecies.clear();
        zone.liveMobs = 0;
    }
    censusMobs_.erase(std::remove_if(censusMobs_.begin(), censusMobs_.end(), [&](const CensusMob& mob) {
        return mob.zone < zones_.size() && riftRealms_[realmIndex(zones_[mob.zone].realm)];
    }), censusMobs_.end());
}

void SpawnSystem::setActivePyramidRealms(Realm ordinary, Realm prestige, Realm prestigeTwo) {
    setActiveBiomeRealms("pyramid", ordinary, prestige, prestigeTwo);
}

void SpawnSystem::setActiveJurassicRealms(Realm ordinary, Realm prestige, Realm prestigeTwo) {
    setActiveBiomeRealms("jurassic", ordinary, prestige, prestigeTwo);
}

void SpawnSystem::setActiveBiomeRealms(const char* biome, Realm ordinary, Realm prestige, Realm prestigeTwo) {
    if (worldMaps == nullptr) return;
    auto inactive = inactiveLayoutRealms_;
    for (const MapData& map : worldMaps->maps()) {
        const Realm realm = map.realm();
        if (map.biome() == biome)
            inactive[realmIndex(realm)] = realm != ordinary && realm != prestige && realm != prestigeTwo;
    }
    if (inactive == inactiveLayoutRealms_) return;
    inactiveLayoutRealms_ = inactive;

    for (SpawnZone& zone : zones_) {
        if (!inactive[realmIndex(zone.realm)]) continue;
        zone.latent.clear();
        zone.pendingSpecies.clear();
        zone.liveMobs = 0;
    }
    censusMobs_.erase(std::remove_if(censusMobs_.begin(), censusMobs_.end(), [&](const CensusMob& mob) {
        return mob.zone < zones_.size() && inactive[realmIndex(zones_[mob.zone].realm)];
    }), censusMobs_.end());

    const auto clearRetired = [&](auto& states) {
        states.erase(std::remove_if(states.begin(), states.end(), [&](const auto& state) {
            return inactive[realmIndex(state.realm)];
        }), states.end());
    };
    clearRetired(superPopulations_);
    clearRetired(superFarmPopulations_);
    clearRetired(eternalPopulations_);
    clearRetired(hyperPopulations_);
    nextPopulationMillis_ = 0.0;
}

void SpawnSystem::rebuildZones(const ContentRegistry& content) {
    if (zoneMaps_ == worldMaps && zoneContentHash_ == content.contentHash()) return;
    zoneMaps_ = worldMaps;
    zoneContentHash_ = content.contentHash();
    zones_.clear();
    lowTierGaps_.clear();
    nextSparseRefill_.fill(0.0);
    refillAdmissions_.clear();
    livePlacementRealms_.fill(false);
    managedVacancies_.clear();
    regions_.clear();
    // Band indices are about to mean other bands, so a slot owed to one of
    // the old ones is owed to nobody.
    censusMobs_.clear();
    vanished_.clear();
    realmBiome_.fill(kInvalidIndex);
    prestigeJungleRealm_ = Realm::Arena;
    ordinaryJungleRealm_ = Realm::Arena;
    prestigeSewersRealm_ = Realm::Arena;
    prestigeHelRealm_ = Realm::Arena;
    prestigeFactoryRealm_ = Realm::Arena;
    prestigeTwoFactoryRealm_ = Realm::Arena;
    if (worldMaps == nullptr) return;

    // Every staged map's bands, each tagged with the realm it belongs to. One
    // flat list rather than a list per map: the passes that walk it are already
    // filtering (by viewport, by realm, by tier), and a second level of
    // indirection would buy nothing at these counts.
    for (const MapData& map : worldMaps->maps()) {
        // Pyramid layouts share a clock within each of their two map families.
        const std::uint16_t biome = bossClockFor(map.prestige()
            ? std::string(map.prestigeLevel() == 2 ? kPrestigeTwoMapPrefix : kPrestigeMapPrefix) + map.biome() : map.biome());
        realmBiome_[realmIndex(map.realm())] = biome;
        const bool prestigeJungle = isPrestigeJungle(map);
        const bool ordinaryJungle = !map.prestige() && map.templateId() == "jungle";
        const bool prestigeSewers = map.prestige() && map.templateId() == "sewers";
        const bool prestigeHel = map.prestige() && map.templateId() == "hel" &&
                                 map.usesReferenceSpawnCurve();
        if (prestigeJungle) prestigeJungleRealm_ = map.realm();
        if (ordinaryJungle) ordinaryJungleRealm_ = map.realm();
        if (prestigeSewers) prestigeSewersRealm_ = map.realm();
        if (prestigeHel) prestigeHelRealm_ = map.realm();
        if (map.templateId() == "computer") {
            if (map.prestigeLevel() == 1) prestigeFactoryRealm_ = map.realm();
            if (map.prestigeLevel() == 2) prestigeTwoFactoryRealm_ = map.realm();
        }
        for (const MapElement& element : map.elements()) {
            if (!element.isSpawnBand() && !element.isMobRegion()) continue;
            SpawnZone zone;
            zone.bounds = element.bounds;
            zone.polygon = element.polygon;
            zone.difficulty = element.difficulty;
            if (map.prestigeLevel() == 1 && map.biome() == "pyramid" &&
                element.isSpawnBand() && !element.singular && element.forceRarity < 0) {
                zone.difficulty += 15.0;
            }
            if (prestigeHel && element.forceRarity < 0) {
                // Raise the approach to the Ultra curve and keep the deepest
                // band on the curve that Prestige can promote to Super.
                if (element.difficulty >= 60.0)
                    zone.difficulty = std::max(kPrestigeHelSuperBand, element.difficulty);
                else if (element.difficulty >= 40.0)
                    zone.difficulty = std::max(kPrestigeHelUpperBand, element.difficulty);
            }
            zone.forceRarity = element.forceRarity;
            zone.realm = map.realm();
            zone.singular = element.singular;
            zone.prestige = map.prestige();

            // The rows are resolved to indices ONCE, here, rather than on every
            // spawn: a group name is a hash probe and a busy band rolls several
            // times a second. It is also the only moment both halves are in
            // hand -- the map, which has the names, and the content, which has
            // the groups -- so it is where a typo can be reported.
            zone.resolved.reserve(element.mobDistribution.size());
            for (const ZoneMobEntry& row : element.mobDistribution) {
                SpawnZone::ResolvedRow resolved;
                resolved.weight = row.weight;
                resolved.group = content.mobGroupIndex(row.name);
                // Groups win over mob ids, so naming a group is unambiguous
                // even if some mob shares its name.
                if (resolved.group == kInvalidIndex) resolved.mob = content.mobIndex(row.name);
                if (resolved.group == kInvalidIndex && resolved.mob == kInvalidIndex &&
                    unknownZoneMobs_.insert(row.name).second) {
                    contentWarning("[spawn] map \"%s\" names \"%s\", which is neither a mob group "
                                   "nor a mob\n",
                                   map.id().c_str(), row.name.c_str());
                }
                zone.resolved.push_back(resolved);
            }
            if (element.isMobRegion()) {
                // A region owns no population, so none of the bookkeeping
                // below applies to it: no target, no fill.
                regions_.push_back(std::move(zone));
                continue;
            }
            zone.biome = biome;

            // The OUTLINE's area, not the bounding box's: a diagonal band
            // covers about half its box, and sizing its population by the box
            // would pack it at twice the density of a rectangular band next
            // door.
            //
            // Rounded UP and never zero: the smallest bands on the map are a
            // few hundred units across and would otherwise be permanently
            // empty.
            //
            // Unless the band is SINGULAR, which is the one band whose size
            // says nothing about its population: it is drawn over everywhere
            // its one mob may be, so area buys reach, not numbers.
            double densityScale = 1.0;
            if (element.forceRarity < 0) {
                if (prestigeJungle) densityScale = kPrestigeJungleDensityScale;
                else if (prestigeSewers) densityScale = kPrestigeSewersDensityScale;
                else if (prestigeHel) densityScale = kPrestigeHelDensityScale;
                else if (ordinaryJungle && element.difficulty >= kDeepJungleDifficulty)
                    densityScale = kDeepJungleDensityScale;
                else if (!map.prestige() && map.templateId() == "hel" &&
                         element.difficulty >= kDeepHelDifficulty)
                    densityScale = kDeepHelDensityScale;
            }
            zone.targetMobs =
                element.singular
                    ? 1
                    : std::max(1, static_cast<int>(std::ceil(
                          kTargetMobDensity * element.density * element.area() * densityScale)));
            zones_.push_back(std::move(zone));
        }

        // A map with no band on it grows NOTHING. That is the rule, not a
        // failure -- the author draws where the mobs are -- but the symptom is
        // an empty world, which reads as a bug in the spawner rather than as a
        // gap in the data, so it is said out loud here as well as on the map's
        // own load line.
        //
        // Not at BOOT, though: this runs from SpawnSystem::run, and the server
        // returns out of its tick while nobody is playing, so an idle server
        // never reaches it. What an author staging a map sees in the boot log
        // is the map's own `[map] ... NO SPAWN BANDS` line (map_elements.cpp);
        // this one adds what to DO about it, the first time anyone joins.
        const bool hasBand = std::any_of(zones_.begin(), zones_.end(), [&](const SpawnZone& band) {
            return band.realm == map.realm();
        });
        if (!hasBand) {
            if (unknownZoneMobs_.insert("<" + map.id() + ":bands>").second) {
                contentWarning("[spawn] map \"%s\" has no spawn band on it, so no mob will ever "
                               "spawn there -- draw a `spawn` object with a `difficulty` to "
                               "populate it\n",
                               map.id().c_str());
            }
            continue;
        }

        // It has bands, but nothing anywhere says WHAT they grow: no band
        // names a roster, no region draws over the map and it declares no
        // `defaultMobGroup`. Every fill will roll kInvalidIndex and place
        // nothing, which looks identical to having no bands at all.
        const bool saysSomething =
            !map.defaultMobGroup().empty() ||
            std::any_of(regions_.begin(), regions_.end(),
                        [&](const SpawnZone& region) { return region.realm == map.realm(); }) ||
            std::any_of(zones_.begin(), zones_.end(), [&](const SpawnZone& band) {
                return band.realm == map.realm() && !band.resolved.empty();
            });
        if (!saysSomething && unknownZoneMobs_.insert("<" + map.id() + ":default>").second) {
            contentWarning("[spawn] map \"%s\" names no mob group anywhere -- no `mobs` on any "
                           "band, no mob region and no `defaultMobGroup`; its bands will stay "
                           "empty\n",
                           map.id().c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// Boss clocks
// ---------------------------------------------------------------------------

std::uint16_t SpawnSystem::bossClockFor(const std::string& biome) {
    for (std::size_t i = 0; i < bossClocks_.size(); ++i) {
        if (bossClocks_[i].biome == biome) return static_cast<std::uint16_t>(i);
    }
    BiomeBossClock clock;
    clock.biome = biome;
    for (auto it = restoredBossClocks_.begin(); it != restoredBossClocks_.end(); ++it) {
        if (it->biome != biome) continue;
        clock = std::move(*it);
        restoredBossClocks_.erase(it);
        break;
    }
    bossClocks_.push_back(std::move(clock));
    return static_cast<std::uint16_t>(bossClocks_.size() - 1);
}

void SpawnSystem::restoreBossClock(const std::string& biome, double uniqueReadyMillis,
                                   double apexReadyMillis) {
    BiomeBossClock restored;
    restored.biome = biome;
    restored.uniqueReadyMillis = uniqueReadyMillis;
    restored.apexReadyMillis = apexReadyMillis;
    restored.scattered = true;
    for (BiomeBossClock& clock : bossClocks_) {
        if (clock.biome == biome) {
            clock = std::move(restored);
            return;
        }
    }
    for (BiomeBossClock& pending : restoredBossClocks_) {
        if (pending.biome == biome) {
            pending = std::move(restored);
            return;
        }
    }
    restoredBossClocks_.push_back(std::move(restored));
}

void SpawnSystem::scatterBossClocks(double nowMillis) {
    std::vector<std::size_t> fresh;
    for (std::size_t i = 0; i < bossClocks_.size(); ++i) {
        auto& clock = bossClocks_[i];
        if (clock.scattered) continue;
        if (clock.biome.rfind(kPrestigeMapPrefix, 0) == 0) {
            clock.uniqueReadyMillis = nowMillis + kUniqueSpawnCooldownMillis;
            clock.scattered = true;
        } else {
            fresh.push_back(i);
        }
    }
    if (fresh.empty()) return;

    // Evenly spaced rather than each drawn uniformly: n independent draws
    // bunch up by chance, and the whole point is that two biomes' bosses do
    // not arrive together. A fresh order per tier, so the biome first in line
    // for a unique is not also first in line for an apex. The phase keeps the
    // first slot off zero -- nothing is ready the moment the server starts,
    // when every band is stocking cold and supers are being placed by the
    // dozen.
    const auto deal = [&](double cooldownMillis, bool apex) {
        for (std::size_t i = fresh.size(); i > 1; --i) {
            std::swap(fresh[i - 1], fresh[bossRng_.below(static_cast<std::uint32_t>(i))]);
        }
        const double phase = bossRng_.unit();
        const double slots = static_cast<double>(fresh.size());
        for (std::size_t slot = 0; slot < fresh.size(); ++slot) {
            const double ready =
                nowMillis + cooldownMillis * (static_cast<double>(slot) + phase) / slots;
            BiomeBossClock& clock = bossClocks_[fresh[slot]];
            (apex ? clock.apexReadyMillis : clock.uniqueReadyMillis) = ready;
        }
    };
    deal(kUniqueSpawnCooldownMillis, false);
    deal(kApexSpawnCooldownMillis, true);
    for (const std::size_t i : fresh) bossClocks_[i].scattered = true;
}

Rarity SpawnSystem::rollBossUpgrade(World& world, const SpawnZone& zone, double nowMillis) {
    if (zone.biome >= bossClocks_.size()) return Rarity::Super;
    const BiomeBossClock& clock = bossClocks_[zone.biome];
    // Only a READY clock draws at all, so the stream is untouched for every
    // super spawned while both are cooling down. A ready clock whose tier is
    // already standing in the biome does not draw either: it stays ready, and
    // the first super after that one dies gets the roll.
    const auto rolls = [&](double readyMillis, Rarity tier) {
        return nowMillis >= readyMillis && !topBossTaken(world, tier, zone.realm) &&
               bossRng_.chance(kBossUpgradeChance);
    };
    if (rolls(clock.apexReadyMillis, Rarity::Apex)) return Rarity::Apex;
    if (rolls(clock.uniqueReadyMillis, Rarity::Unique)) return Rarity::Unique;
    return Rarity::Super;
}

bool SpawnSystem::prestigeRealm(Realm realm) const {
    return worldMaps != nullptr && worldMaps->isPrestige(realm);
}

double SpawnSystem::bossCooldownLeft(Rarity rarity, Realm realm, double nowMillis) const {
    if (rarity != Rarity::Unique && rarity != Rarity::Apex) return 0.0;
    const std::uint16_t slot = realmBiome_[realmIndex(realm)];
    if (slot >= bossClocks_.size()) return 0.0;
    const BiomeBossClock& clock = bossClocks_[slot];
    const double ready = rarity == Rarity::Apex ? clock.apexReadyMillis : clock.uniqueReadyMillis;
    return std::max(0.0, ready - nowMillis);
}

void SpawnSystem::chargeBossClock(Rarity rarity, Realm realm, double nowMillis) {
    restartBossClock(realmBiome_[realmIndex(realm)], rarity, nowMillis);
}

void SpawnSystem::restartBossClock(std::uint16_t slot, Rarity rarity, double nowMillis) {
    if (slot >= bossClocks_.size()) return;
    BiomeBossClock& clock = bossClocks_[slot];
    if (rarity == Rarity::Apex) {
        clock.apexReadyMillis = nowMillis + kApexSpawnCooldownMillis;
    } else if (rarity == Rarity::Unique) {
        clock.uniqueReadyMillis = nowMillis + kUniqueSpawnCooldownMillis;
    }
}

std::uint32_t SpawnSystem::biomeKey(Realm realm) const {
    const std::uint16_t slot = realmBiome_[realmIndex(realm)];
    // Past every clock slot, so a map-less realm never matches a biome.
    return slot != kInvalidIndex ? slot : 0x10000u + static_cast<std::uint32_t>(realmIndex(realm));
}

bool SpawnSystem::topBossTaken(World& world, Rarity rarity, Realm realm, bool includeRituals) {
    const std::uint32_t key = biomeKey(realm);
    int standing = 0;
    ambient_->each([&](Entity e, MobTag&, Transform& transform, Body&, MobType& type,
                       AmbientMob& ambient) {
        const Rarity tier = type.rarity == Rarity::Ascendant && rarity == Rarity::Apex &&
            ambient.zone != kInvalidIndex ? Rarity::Apex : type.rarity;
        if (tier != rarity || !isWholeAnimal(world, e) ||
            world.has<HoleTether>(e) || world.has<WaveEnemy>(e) ||
            world.has<StemCellOffspring>(e)) return;
        if (isWorldRealm(transform.realm) && biomeKey(transform.realm) == key) ++standing;
    });
    if (includeRituals) {
        rituals_->each([&](Entity, SacrificeRitual& ritual, Transform& at) {
            if (ritual.mob == NULL_ENTITY && ritual.rarity == rarity &&
                biomeKey(at.realm) == key) ++standing;
        });
    }
    const int cap = rarity == Rarity::Unique
        ? (prestigeRealm(realm) ? kPrestigeEternalPopulationTarget : kEternalPopulationTarget)
        : (rarity == Rarity::Ascendant ? kNaturalApexPopulationTarget : kHyperPopulationTarget);
    return standing >= cap;
}

void SpawnSystem::noteStandingBosses(World& world, const CommandBuffer& commands) {
    // The final save can come before the spawner has ever run -- a server
    // nobody joined -- and the query is only made on the first bind.
    bind(world);
    // Pets never appear here: a pet carries no AmbientMob, and this is the
    // spawner's own population. Nor do the arena and the maze, which are not
    // biomes and never field a unique anyway.
    topBosses_.clear();
    ambient_->each([&](Entity e, MobTag&, Transform& transform, Body&, MobType& type,
                       AmbientMob& ambient) {
        if (type.rarity != Rarity::Unique && type.rarity != Rarity::Apex &&
            !(type.rarity == Rarity::Ascendant && ambient.zone != kInvalidIndex)) return;
        if (world.has<HoleTether>(e) || world.has<WaveEnemy>(e) ||
            world.has<StemCellOffspring>(e)) return;
        const auto* map = worldMaps ? worldMaps->forRealm(transform.realm) : nullptr;
        if (map && map->prestigeLevel() == 2 && !ambient.managedPopulation) return;
        if (!isWorldRealm(transform.realm) || !isWholeAnimal(world, e)) return;
        const Rarity tier = type.rarity == Rarity::Ascendant ? Rarity::Apex : type.rarity;
        topBosses_.push_back(TopBoss{biomeKey(transform.realm), tier, ambient.spawnOrder, e});
    });
    for (BiomeBossClock& clock : bossClocks_) clock.uniqueStanding = clock.apexStanding = false;
    // One already queued for removal -- an admin's killall, say -- is gone
    // by the end of the tick, and a save before then must not keep it alive.
    for (const TopBoss& boss : topBosses_) {
        if (!commands.destroying(boss.entity)) markStanding(boss.biome, boss.rarity);
    }
    // A pentagram still being drawn has spent its clock on a boss that is not
    // out yet, and a restart loses that boss just the same.
    rituals_->each([&](Entity e, SacrificeRitual& ritual, Transform& transform) {
        if (ritual.mob != NULL_ENTITY || commands.destroying(e)) return;
        markStanding(biomeKey(transform.realm), ritual.rarity);
    });
}

void SpawnSystem::markStanding(std::uint32_t biome, Rarity rarity) {
    if (biome >= bossClocks_.size()) return;
    BiomeBossClock& clock = bossClocks_[biome];
    if (rarity == Rarity::Apex) clock.apexStanding = true;
    if (rarity == Rarity::Unique) clock.uniqueStanding = true;
}

void SpawnSystem::cullExtraTopBosses(World& world, CommandBuffer& commands) {
    // The extras culled below leave the first of each tier standing, so the
    // flags this sets hold after the cull too.
    noteStandingBosses(world, commands);
    if (topBosses_.size() < 2) return;

    std::sort(topBosses_.begin(), topBosses_.end(), [](const TopBoss& a, const TopBoss& b) {
        if (a.biome != b.biome) return a.biome < b.biome;
        if (a.rarity != b.rarity) return rarityIndex(a.rarity) < rarityIndex(b.rarity);
        return a.order < b.order;
    });
    culled_.clear();
    int standing = 1;
    for (std::size_t i = 1; i < topBosses_.size(); ++i) {
        const TopBoss& first = topBosses_[i - 1];
        const TopBoss& extra = topBosses_[i];
        standing = extra.biome == first.biome && extra.rarity == first.rarity ? standing + 1 : 1;
        const int cap = extra.rarity == Rarity::Unique ? kEternalPopulationTarget : kHyperPopulationTarget;
        if (standing > cap && !world.has<KeepAwake>(extra.entity)) {
            culled_.push_back(extra.entity);
        }
    }
    if (culled_.empty()) return;

    // The body goes with its head. A boss is never recycled, so a unique
    // centipede's segments left behind would stand there for good -- and the
    // first of them would promote itself to a head of its own.
    Query<BodySegment> segments{world};
    segments.without<Dead>();
    const std::size_t heads = culled_.size();
    segments.each([&](Entity e, BodySegment& link) {
        if (link.chainHead == e) return;
        if (std::find(culled_.begin(), culled_.begin() + static_cast<std::ptrdiff_t>(heads),
                      link.chainHead) != culled_.begin() + static_cast<std::ptrdiff_t>(heads)) {
            culled_.push_back(e);
        }
    });
    for (const Entity e : culled_) commands.destroy(e);
    census_.despawnedTotal += static_cast<int>(heads);
}

std::uint16_t SpawnSystem::rollResolvedRows(const ContentRegistry& content,
                                           const std::vector<SpawnZone::ResolvedRow>& rows,
                                           Rarity rarity, Rng& rng) const {
    if (rows.empty()) return kInvalidIndex;

    double total = 0.0;
    for (const SpawnZone::ResolvedRow& row : rows) total += std::max(0.0, row.weight);
    // A table nobody weighted still spawns something rather than nothing.
    const SpawnZone::ResolvedRow* chosen = &rows.front();
    if (total > 0.0) {
        double roll = rng.unit() * total;
        for (const SpawnZone::ResolvedRow& row : rows) {
            roll -= std::max(0.0, row.weight);
            if (roll <= 0.0) {
                chosen = &row;
                break;
            }
        }
    }

    // A group defers to that group's own weights, so `ocean` in a garden band
    // spawns exactly what the ocean would.
    if (chosen->group != kInvalidIndex) {
        return chooseGroupMob(content, chosen->group, rarity, rng);
    }
    // Named outright, which bypasses the group roll entirely -- that roll
    // excludes `neverAmbient` mobs, and this is how a BAND puts one in the
    // world: the spawner's only way to. Not the only way there is. The target
    // dummy also stands as a map NPC -- the jungle's DPS row is a line of
    // `npcs` objects, placed by NpcSystem, not by any band -- and the console
    // puts one down with spawn_npc, or as a mob with spawn.
    if (chosen->mob == kInvalidIndex || chosen->mob >= content.mobCount()) return kInvalidIndex;
    if (rarityIndex(rarity) < rarityIndex(content.mob(chosen->mob).minRarity)) return kInvalidIndex;
    return chosen->mob;
}

std::uint16_t SpawnSystem::chooseZoneMobType(const ContentRegistry& content,
                                            const SpawnZone& zone, Vec2 at, Rarity rarity,
                                            Rng& rng) {
    // No distribution of its own: whatever this ground grows anyway. That is
    // what a band saying only `difficulty: 50` means -- the same mobs as the
    // meadow beside it, three tiers up.
    if (zone.resolved.empty()) return chooseRegionMobAt(content, zone.realm, at, rarity, rng);
    return rollResolvedRows(content, zone.resolved, rarity, rng);
}

// ---------------------------------------------------------------------------
// Stocking the map
// ---------------------------------------------------------------------------

void SpawnSystem::stockSupers(World& world, const Terrain& terrain,
                              const ContentRegistry& content, const std::vector<Viewer>& viewers,
                              Rng& rng, double nowMillis) {
    for (const SpawnZone& zone : zones_) {
        if (zone.prestige || inactiveLayoutRealms_[realmIndex(zone.realm)]) continue;
        const auto* map = worldMaps ? worldMaps->forRealm(zone.realm) : nullptr;
        if (map && map->templateId() == "super") continue;
        const auto found = std::find_if(superPopulations_.begin(), superPopulations_.end(),
            [&](const SuperMapState& state) { return state.realm == zone.realm; });
        if (found == superPopulations_.end()) {
            const double cooldown = map && map->templateId() == "hel" ? 120000.0 : SuperPopulation::respawnMillis;
            superPopulations_.push_back({zone.realm, SuperPopulation{2 * SuperPopulation::target, cooldown}});
        }
    }
    auto antHabitat = [&](std::uint16_t index) {
        if(index>=zones_.size())return -1;
        double weights[3]={0,0,0};
        for(const auto& row:zones_[index].resolved)if(row.mob!=kInvalidIndex && row.mob<content.mobCount()) {
            const auto& id=content.mob(row.mob).id;
            const int family=id.find("fire_ant")!=std::string::npos?1:id.find("termite")!=std::string::npos?2:
                             id.find("ant")!=std::string::npos?0:-1;
            if(family>=0)weights[family]+=std::max(0.0,row.weight);
        }
        int best=-1;for(int n=0;n<3;++n)if(weights[n]>0 && (best<0 || weights[n]>weights[best]))best=n;
        return best;
    };
    for (SuperMapState& state : superPopulations_) {
        const auto* map=worldMaps?worldMaps->forRealm(state.realm):nullptr;
        const bool antHell=map && map->templateId()=="ant_hell";
        int habitats[3]={0,0,0};
        int living = 0;
        ambient_->each([&](Entity e, MobTag&, Transform& transform, Body&, MobType& type, AmbientMob& ambient) {
            if (!world.has<HoleTether>(e) && !world.has<SuperFarmMob>(e) && transform.realm == state.realm && type.rarity == Rarity::Super && isWholeAnimal(world, e)) {
                ++living;const int family=antHabitat(ambient.zone);if(family>=0)++habitats[family];
            }
        });
        state.population.observe(living, nowMillis);
        std::vector<std::uint16_t> candidates;
        for (std::size_t i = 0; i < zones_.size(); ++i)
            if (zones_[i].realm == state.realm && !zones_[i].prestige && !zones_[i].singular)
                candidates.push_back(static_cast<std::uint16_t>(i));
        std::sort(candidates.begin(), candidates.end(), [&](auto a, auto b) {
            return zones_[a].difficulty > zones_[b].difficulty;
        });
        // Every authored habitat can host a Super; choosing only the highest
        // threat bands excluded the jungle's non-termite species.
        for (int attempt = 0; attempt < 80 && state.population.ready(nowMillis) && !candidates.empty(); ++attempt) {
            // Weight habitats by their actual area, not annotation count:
            // a dozen tiny termite plots must not outweigh the jungle floor.
            int requiredHabitat=-1;
            if(antHell) {
                constexpr int quotas[3]={8,8,4};int largest=0;
                for(int n=0;n<3;++n)if(quotas[n]-habitats[n]>largest &&
                    std::any_of(candidates.begin(),candidates.end(),[&](auto i){return antHabitat(i)==n;})) {
                    requiredHabitat=n;largest=quotas[n]-habitats[n];
                }
            }
            double totalArea = 0;std::vector<double> areas;
            for (auto candidate : candidates) {
                const auto& zone = zones_[candidate];
                double area = zone.bounds.w * zone.bounds.h;
                if (zone.polygon.size() >= 3) {
                    area = 0;
                    for (std::size_t i = 0; i < zone.polygon.size(); ++i) {
                        const auto a = zone.polygon[i];
                        const auto b = zone.polygon[(i + 1) % zone.polygon.size()];
                        area += a.x * b.y - b.x * a.y;
                    }
                    area = std::fabs(area) * 0.5;
                }
                areas.push_back(requiredHabitat>=0 && antHabitat(candidate)!=requiredHabitat ? 0.0 : std::max(1.0, area));totalArea += areas.back();
            }
            double roll = rng.unit() * totalArea;std::size_t chosen = 0;
            while (chosen + 1 < areas.size() && roll >= areas[chosen]) roll -= areas[chosen++];
            const auto index = candidates[chosen];
            if (stockZone(world, terrain, content, zones_[index], index, viewers, rng, nowMillis,
                          Slot::TopUp, {}, true)) {
                state.population.spawned();const int family=antHabitat(index);if(family>=0)++habitats[family];
            }
        }
    }
}

void SpawnSystem::trimMounds(World& world,const ContentRegistry& content,CommandBuffer& commands) {
    struct Mound {Entity entity;Realm realm;Rarity rarity;bool occupied;std::uint64_t order;};
    std::vector<Realm> occupied;
    Query<PlayerTag,Transform> players{world};
    players.each([&](Entity,PlayerTag&,Transform& at){occupied.push_back(at.realm);});
    std::vector<Mound> mounds;
    Query<DungeonEntrance,MobType,Transform,AmbientMob> all{world};all.without<Dead>();
    all.each([&](Entity e,DungeonEntrance& entrance,MobType& type,Transform& at,AmbientMob& mob){
        if(type.configIndex>=content.mobCount()||content.mob(type.configIndex).id!="termite_mound")return;
        const bool active=entrance.claimed&&std::find(occupied.begin(),occupied.end(),entrance.realm)!=occupied.end();
        mounds.push_back({e,at.realm,type.rarity,active,mob.spawnOrder});
    });
    std::stable_sort(mounds.begin(),mounds.end(),[](const Mound& a,const Mound& b){
        if(a.realm!=b.realm)return static_cast<int>(a.realm)<static_cast<int>(b.realm);
        if(a.rarity!=b.rarity)return rarityIndex(a.rarity)<rarityIndex(b.rarity);
        if(a.occupied!=b.occupied)return a.occupied;
        return a.order<b.order;
    });
    int kept=0;Realm realm=Realm::Overworld;Rarity rarity=Rarity::Common;
    for(std::size_t i=0;i<mounds.size();++i){const auto& mound=mounds[i];
        if(i==0||mound.realm!=realm||mound.rarity!=rarity){kept=0;realm=mound.realm;rarity=mound.rarity;}
        if(mound.occupied||kept==0){++kept;continue;}
        commands.destroy(mound.entity); // Cleanup is not a kill and grants no drops.
    }
}

void SpawnSystem::stockSuperFarms(World& world, const Terrain& terrain,
                                  const ContentRegistry& content,
                                  const std::vector<Viewer>& viewers,Rng& rng,double nowMillis) {
    if (!worldMaps) return;
    for (const auto& map : worldMaps->maps()) {
        const bool dedicatedFarm = map.templateId() == "super";
        const auto& farmId = map.biome() == "pyramid" ? map.biome() : map.id();
        if (inactiveLayoutRealms_[realmIndex(map.realm())] || map.prestige() ||
            (!dedicatedFarm && !map.usesReferenceSpawnCurve()) || !supportsSuperFarm(farmId)) continue;
        double highest = -1;
        for (const auto& zone : zones_)
            if (zone.realm==map.realm() && !zone.prestige && !zone.singular && zone.forceRarity<0)
                highest=std::max(highest,zone.difficulty);
        if (highest<0) continue;
        std::vector<std::uint16_t> candidates;
        for (std::size_t i=0;i<zones_.size();++i) {
            const auto& zone=zones_[i];
            if (zone.realm==map.realm() && !zone.prestige && !zone.singular && zone.forceRarity<0 &&
                (dedicatedFarm || zone.difficulty>=highest-kSuperFarmDifficultyWindow))
                candidates.push_back(static_cast<std::uint16_t>(i));
        }
        if (candidates.empty()) continue;
        const int farmPopulation = 2 * (map.templateId() == "hel" ? 36 :
            map.templateId() == "computer" ? kSuperFarmPopulation + 6 : kSuperFarmPopulation);
        auto state=std::find_if(superFarmPopulations_.begin(),superFarmPopulations_.end(),
            [&](const SuperMapState& entry){return entry.realm==map.realm();});
        if (state==superFarmPopulations_.end()) {
            superFarmPopulations_.push_back({map.realm(),SuperPopulation{farmPopulation, map.templateId() == "hel" ? 120000.0 : SuperPopulation::respawnMillis}});
            state=std::prev(superFarmPopulations_.end());
        }
        int living=0;
        std::vector<int> zoneLiving(zones_.size(), 0);
        ambient_->each([&](Entity e,MobTag&,Transform& at,Body&,MobType& type,AmbientMob&){
            if (at.realm==map.realm() && type.rarity==Rarity::Super && world.has<SuperFarmMob>(e) &&
                !world.has<Dead>(e) && isWholeAnimal(world,e)) {
                ++living;const auto zone=world.get<AmbientMob>(e).zone;
                if(zone<zoneLiving.size())++zoneLiving[zone];
            }
        });
        state->population.observe(living,nowMillis);
        // Keep one third of the stock in the deepest band(s), the rest spread
        // across the surrounding bands. Refill the largest deficit after the
        // existing 30s vacancy timer, so farming one room cannot permanently
        // move all its S replacements elsewhere. Ordinary U stock remains.
        const int deepest=static_cast<int>(std::count_if(candidates.begin(),candidates.end(),
            [&](auto i){return zones_[i].difficulty==highest;}));
        const double deepTarget=static_cast<double>(farmPopulation)/3/std::max(1,deepest);
        // The dedicated map is large enough that filling distant bands first
        // can make its entrance look empty. Give its entrance habitats a
        // share of the existing quota and retain the usual spawn clearance.
        std::vector<bool> entryBand(zones_.size(), false);
        int entryBands = 0;
        if (dedicatedFarm && !map.playerSpawns().empty()) {
            const auto& entry = map.playerSpawns().front()->bounds;
            const Vec2 centre{entry.x + entry.w * 0.5, entry.y + entry.h * 0.5};
            constexpr double entryReach = 1800.0;
            for (const auto index : candidates) {
                const auto& zone = zones_[index];
                const Vec2 midpoint{zone.bounds.x + zone.bounds.w * 0.5,
                                    zone.bounds.y + zone.bounds.h * 0.5};
                if (zone.difficulty < highest && distanceSq(centre, midpoint) <= entryReach * entryReach) {
                    entryBand[index] = true;
                    ++entryBands;
                }
            }
        }
        const double entryStock = entryBands > 0 ? 6.0 : 0.0;
        const double entryTarget = entryStock / std::max(1, entryBands);
        const double otherTarget = (farmPopulation - deepTarget * deepest - entryStock) /
            std::max(1, static_cast<int>(candidates.size()) - deepest - entryBands);
        std::vector<unsigned> attemptsByZone(zones_.size(), 0);
        for (int attempt=0;attempt<160 && state->population.ready(nowMillis);++attempt) {
            auto index=candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))];
            double largest=-1e9;
            for(const auto candidate:candidates){
                const double wanted=deepest==static_cast<int>(candidates.size())?
                    static_cast<double>(farmPopulation)/deepest:
                    zones_[candidate].difficulty==highest?deepTarget:
                    entryBand[candidate]?entryTarget:otherTarget;
                // A blocked/visible room must not consume every retry and
                // prevent safe rooms from receiving their replacements.
                const double deficit=wanted-zoneLiving[candidate]-attemptsByZone[candidate]+rng.unit()*.01;
                if(deficit>largest){largest=deficit;index=candidate;}
            }
            if (stockZone(world,terrain,content,zones_[index],index,viewers,rng,nowMillis,
                          Slot::TopUp,{},true,Rarity::Super,true)) {
                state->population.spawned();++zoneLiving[index];
            } else ++attemptsByZone[index];
        }
    }
}

void SpawnSystem::stockEternals(World& world, const Terrain& terrain,
                                const ContentRegistry& content, const std::vector<Viewer>& viewers,
                                Rng& rng, double nowMillis) {
    if(!worldMaps)return;
    for(const auto& map:worldMaps->maps()) {
        const auto& id = map.biome() == "pyramid" ? map.biome() : map.id();
        const bool deepArctic = hasDeepArcticEternalSlot(map);
        if (inactiveLayoutRealms_[realmIndex(map.realm())]) continue;
        if (!deepArctic && (map.prestige() || !map.usesReferenceSpawnCurve() ||
            (id!="garden"&&id!="desert"&&id!="computer"&&id!="ocean"&&id!="jungle"&&
             id!="hel"&&id!="ant_hell"&&id!="sewers"&&id!="pyramid"&&id!="cyberspace"))) continue;
        const int target = deepArctic ? 1 : kEternalPopulationTarget;
        const auto eligibleZone = [&](const SpawnZone& zone) {
            return zone.realm == map.realm() && zone.prestige == deepArctic &&
                !zone.singular && zone.forceRarity < 0;
        };
        auto state=std::find_if(eternalPopulations_.begin(),eternalPopulations_.end(),
                               [&](const EternalMapState& v){return v.realm==map.realm();});
        if(state==eternalPopulations_.end()) {
            eternalPopulations_.push_back({map.realm(),0});state=std::prev(eternalPopulations_.end());
        }
        int living=0;
        ambient_->each([&](Entity e,MobTag&,Transform& at,Body&,MobType& type,AmbientMob& mob){
            if ((!deepArctic || mob.managedPopulation) && !world.has<HoleTether>(e) &&
                at.realm==map.realm() && type.rarity==Rarity::Unique &&
               !world.has<Dead>(e) && isWholeAnimal(world,e))++living;
        });
        if(state->previousLiving>living){
            chargeBossClock(Rarity::Unique,map.realm(),nowMillis);
            const auto slot=realmBiome_[realmIndex(map.realm())];
            if(slot<bossClocks_.size())bossClocks_[slot].uniqueDeathCooldown=true;
        }
        state->previousLiving=living;
        if (living >= target || bossCooldownLeft(Rarity::Unique, map.realm(), nowMillis) > 0.0) continue;
        double highest=-1e9;
        for (const auto& zone : zones_)
            if (eligibleZone(zone)) highest = std::max(highest, zone.difficulty);
        std::vector<std::uint16_t> candidates;
        const double floor = deepArctic ? highest : std::min(highest, std::max(45.0, highest-20.0));
        for (std::size_t n=0; n<zones_.size(); ++n) {
            const auto& zone = zones_[n];
            if (eligibleZone(zone) && zone.difficulty >= floor)
                candidates.push_back(static_cast<std::uint16_t>(n));
        }
        int refill = 0;
        for (int attempt=0; attempt<240 && !candidates.empty() &&
             living < target && refill < 3; ++attempt) {
            const auto n=candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))];
            if(stockZone(world,terrain,content,zones_[n],n,viewers,rng,nowMillis,
                         Slot::TopUp,{},true,Rarity::Unique)) {
                ++refill;
                ++living;
                state->previousLiving = living;
            }
        }
    }
}

void SpawnSystem::stockHypers(World& world, const Terrain& terrain,
                                const ContentRegistry& content, const std::vector<Viewer>& viewers,
                                Rng& rng, double nowMillis) {
    if (!worldMaps) return;
    std::vector<const MapData*> candidatesByMap;
    std::vector<const MapData*> expansionMaps;
    for (const auto& map : worldMaps->maps()) {
        if (map.prestige() || inactiveLayoutRealms_[realmIndex(map.realm())]) continue;
        const auto& biome = map.biome();
        if (biome == "bio" || biome == "jurassic" || biome == "zorr_void" || biome == "zorr_arctic" ||
            biome == "zorr_glacier" || biome == "zorr_polar") {
            expansionMaps.push_back(&map);
        } else if (map.usesReferenceSpawnCurve() &&
                   (map.id() == "garden" || map.id() == "desert" || map.id() == "computer" ||
                    map.id() == "ocean" || map.id() == "jungle" || map.id() == "hel" ||
                    map.id() == "ant_hell" || map.id() == "sewers" || biome == "pyramid" ||
                    map.id() == "cyberspace")) {
            candidatesByMap.push_back(&map);
        }
    }
    // Copies and alternate layouts share their biome's slot. Give every
    // expansion map a turn to fill it, even when several are open together.
    if (expansionMaps.size() > 1) {
        const auto first = rng.below(static_cast<std::uint32_t>(expansionMaps.size()));
        std::rotate(expansionMaps.begin(), expansionMaps.begin() + first, expansionMaps.end());
    }
    candidatesByMap.insert(candidatesByMap.end(), expansionMaps.begin(), expansionMaps.end());
    std::array<int, kMaxRealms> standingByRealm{};
    ambient_->each([&](Entity e, MobTag&, Transform& at, Body&, MobType& type, AmbientMob& ambient) {
        if (world.has<HoleTether>(e) || world.has<Dead>(e) ||
            world.has<StemCellOffspring>(e) || !isWholeAnimal(world, e)) return;
        if (type.rarity == Rarity::Apex || (type.rarity == Rarity::Ascendant && ambient.zone != kInvalidIndex))
            ++standingByRealm[realmIndex(at.realm)];
    });
    // Apply losses before selecting a map. Otherwise a sibling visited first
    // can fill the shared slot before the map that lost it starts the cooldown.
    for (auto& state : hyperPopulations_) {
        const int standing = standingByRealm[realmIndex(state.realm)];
        if (state.previousLiving > standing && bossCooldownLeft(Rarity::Apex, state.realm, nowMillis) == 0.0)
            chargeBossClock(Rarity::Apex, state.realm, nowMillis);
        state.previousLiving = standing;
    }
    for (const auto* candidate : candidatesByMap) {
        const auto& map = *candidate;
        auto state=std::find_if(hyperPopulations_.begin(),hyperPopulations_.end(),
                               [&](const EternalMapState& v){return v.realm==map.realm();});
        if(state==hyperPopulations_.end()) {
            hyperPopulations_.push_back({map.realm(),0});state=std::prev(hyperPopulations_.end());
        }
        int living = standingByRealm[realmIndex(map.realm())];
        state->previousLiving=living;
        if(living>=kHyperPopulationTarget)continue;
        if (bossCooldownLeft(Rarity::Apex, map.realm(), nowMillis) > 0.0 ||
            topBossTaken(world, Rarity::Apex, map.realm())) continue;
        double highest=-1e9;
        for(const auto& zone:zones_)if(zone.realm==map.realm()&&!zone.prestige&&!zone.singular&&zone.forceRarity<0)
            highest=std::max(highest,zone.difficulty);
        std::vector<std::uint16_t> candidates;
        const double floor=std::min(highest,std::max(45.0,highest-20.0));
        for(std::size_t n=0;n<zones_.size();++n) {
            const auto& zone=zones_[n];
            if(zone.realm==map.realm()&&!zone.prestige&&!zone.singular&&zone.forceRarity<0&&zone.difficulty>=floor)
                candidates.push_back(static_cast<std::uint16_t>(n));
        }
        for (int attempt = 0; attempt < 240 && !candidates.empty() && living < kHyperPopulationTarget &&
             !topBossTaken(world, Rarity::Apex, map.realm()); ++attempt) {
            const auto n=candidates[rng.below(static_cast<std::uint32_t>(candidates.size()))];
            if(stockZone(world,terrain,content,zones_[n],n,viewers,rng,nowMillis,
                         Slot::TopUp,{},true,Rarity::Apex)) {
                ++living;
                state->previousLiving = living;
                // Apply the existing shared cooldown after one admitted spawn.
                chargeBossClock(Rarity::Apex, map.realm(), nowMillis);
                break;
            }
        }
    }
}

void SpawnSystem::stockSpawnZones(World& world, const Terrain& terrain,
                                  const ContentRegistry& content,
                                  const std::vector<Viewer>& viewers, Rng& rng,
                                  double nowMillis) {
    // No band anywhere means no ambient mob anywhere. There is no second pass
    // behind this one that would cover the ground the author left unbanded.
    if (zones_.empty()) return;
    stockSupers(world, terrain, content, viewers, rng, nowMillis);
    stockEternals(world, terrain, content, viewers, rng, nowMillis);
    stockHypers(world, terrain, content, viewers, rng, nowMillis);
    stockSuperFarms(world, terrain, content, viewers, rng, nowMillis);

    for (std::size_t index = 0; index < zones_.size(); ++index) {
        SpawnZone& zone = zones_[index];
        if (inactiveLayoutRealms_[realmIndex(zone.realm)]) continue;
        // The band's whole population, awake and asleep. Nothing about a
        // viewport appears in this test: a band on the far side of the map is
        // stocked to exactly the same number as the one under the player's
        // feet, which is the change this whole file is built around.
        //
        // A full band costs one subtraction per pass, which is what makes it
        // affordable to ask the question of every band on every map twice a
        // second. A short one is paid off in FULL, not a chunk a pass: a
        // record is a few dozen bytes, and a band left to fill itself over
        // half a minute is half a minute of a player watching mobs trickle in.
        const int owed = zone.targetMobs - static_cast<int>(zone.latent.size()) - zone.liveMobs;
        for (int n = 0; n < owed; ++n) {
            const auto replacement = zone.pendingSpecies.empty() ? kInvalidIndex
                                                                 : zone.pendingSpecies.back();
            if (!stockZone(world, terrain, content, zone, static_cast<std::uint16_t>(index),
                           viewers, rng, nowMillis, Slot::TopUp, {}, false, Rarity::Super,
                           false, replacement)) {
                // The outline had nowhere to put this one, so it has nowhere
                // to put the next either: drop the rest of the debt rather
                // than spinning on a band the terrain has since walled over.
                break;
            }
            if (!zone.pendingSpecies.empty()) zone.pendingSpecies.pop_back();
        }
    }
}

bool SpawnSystem::stockZone(World& world, const Terrain& terrain, const ContentRegistry& content,
                            SpawnZone& zone, std::uint16_t zoneIndex,
                            const std::vector<Viewer>& viewers, Rng& rng, double nowMillis,
                            Slot slot, Vec2 lostAt, bool fixedSuper, Rarity managedTier, bool superFarm,
                            std::uint16_t replacementType, double liveDensity, std::optional<Rarity> lastLowerTier) {
    if (inactiveLayoutRealms_[realmIndex(zone.realm)] || inactiveRiftRealms_[realmIndex(zone.realm)]) return false;
    std::size_t managedVacancy = managedVacancies_.size();
    if (fixedSuper) {
        for (std::size_t i = 0; i < managedVacancies_.size(); ++i) {
            const auto& vacancy = managedVacancies_[i];
            if (vacancy.zone >= zones_.size() || zones_[vacancy.zone].realm != zone.realm ||
                vacancy.rarity != managedTier || vacancy.superFarm != superFarm) continue;
            replacementType = vacancy.mobIndex;
            managedVacancy = i;
            break;
        }
    }
    const auto* authoredMap = worldMaps ? worldMaps->forRealm(zone.realm) : nullptr;
    // The dedicated farm uses the capped Super population and its vacancy
    // timer. Ordinary fill and casualty paths cannot create lower tiers here.
    if (authoredMap && authoredMap->templateId() == "super" && !superFarm) return false;
    // Everything about this happens in the band's OWN realm: the map it is
    // drawn on has its own size, its own walls and its own population, and the
    // same numbers on the overworld describe somewhere else entirely.
    const Vec2 extent = terrain.realmExtent(zone.realm);
    const auto rosterAt = [&](Vec2 point) -> const SpawnZone& {
        for (const auto& region : regions_)
            if (region.realm == zone.realm && zoneContains(region.bounds, region.polygon, point))
                return region;
        return zone;
    };
    const auto acceptsReplacement = [&](const SpawnZone& roster) {
        if (replacementType == kInvalidIndex) return false;
        const auto& previous = content.mob(replacementType);
        if (previous.neverAmbient) return false;
        const auto inGroup = [&](std::uint16_t group) {
            return group != kInvalidIndex &&
                std::any_of(previous.groups.begin(), previous.groups.end(),
                    [&](const MobGroupMember& member) {
                        return member.group == group && member.weight > 0.0;
                    });
        };
        if (roster.resolved.empty() && authoredMap)
            return inGroup(content.mobGroupIndex(authoredMap->defaultMobGroup()));
        return std::any_of(roster.resolved.begin(), roster.resolved.end(),
            [&](const auto& row) {
                return row.weight > 0.0 && (row.mob == replacementType || inGroup(row.group));
            });
    };
    bool reuseSpecies = acceptsReplacement(zone);
    if (!reuseSpecies) {
        for (const auto& region : regions_)
            if (region.realm == zone.realm && region.bounds.left() < zone.bounds.right() &&
                region.bounds.right() > zone.bounds.left() && region.bounds.top() < zone.bounds.bottom() &&
                region.bounds.bottom() > zone.bounds.top() && acceptsReplacement(region)) reuseSpecies = true;
    }
    // A managed vacancy can move between eligible habitats as before, but
    // cannot turn a mobile casualty into another surviving static species.
    if (managedVacancy < managedVacancies_.size() && !reuseSpecies) return false;
    // A singular band gets its one mob back ANYWHERE in its outline. The
    // scatter exists to keep a big band evenly full, and a band of one has no
    // evenness to keep; handing the slot back where it fell would put the next
    // queen in the room the last one was killed in, every time, which turns a
    // hunt across the map into a farm at one coordinate.
    const double scatter = slot != Slot::TopUp && !zone.singular ? kRespawnScatter : 0.0;

    Vec2 at;
    bool placed = false;
    for (int attempt = 0; attempt < kZonePlacementAttempts; ++attempt) {
        // Near where the last one was lost, or anywhere in the outline. See
        // kRespawnScatter: the difference between the two is the difference
        // between a band that is evenly full and one that is full on paper.
        const Vec2 candidate = lastLowerTier && attempt == 0 ? lostAt :
            scatter > 0.0
                ? samplePointNear(lostAt, lastLowerTier && attempt < 8 ? 380.0 : scatter, extent, rng)
                : samplePointInRect(zone.bounds, extent, rng);
        // Rejection sampling over the bounding box keeps the distribution
        // uniform over the outline. A candidate in a corner the polygon does
        // not cover is thrown away like any other unusable one, so a zone that
        // fills little of its box simply spends more of its attempts -- which
        // is why there are attempts rather than one shot.
        if (!zoneContains(zone.bounds, zone.polygon, candidate)) continue;
        if (inBorderBand(candidate, extent)) continue;
        if (terrain.blocked(candidate, zone.realm)) continue;
        if (reuseSpecies && !acceptsReplacement(rosterAt(candidate))) continue;
        // The nominal body, because the type is not rolled until a point has
        // been chosen. Whatever this places is measured again against its real
        // size below (an entity) or when it wakes (a record).
        // A dormant prestige record is not an entity. Actual full-body
        // clearance is checked when it wakes; rejecting its invisible stock
        // here can permanently starve a tiny occupied authored cell.
        if ((!zone.prestige || fixedSuper || zone.singular || zone.forceRarity>=0) &&
            nearAnyPlayer(viewers, zone.realm, candidate, kMinSpawnDistance,kPreliminarySpawnRadius)) {
            continue;
        }
        if (crowdedAt(zone.realm, candidate, kPreliminarySpawnRadius, kMinMobSpawnSpacing, zoneIndex)) {
            continue;
        }
        at = candidate;
        placed = true;
        break;
    }
    if (!placed) return false;

    // A band belongs to nobody's viewport, so anything charged to luck is
    // charged to whoever is standing nearest its centre -- the reference's own
    // attribution rule for a zone fill. The bounding box's centre, which for a
    // concave band is not inside it; it is an attribution tiebreak, not a
    // placement, so that costs nothing. With nobody near the band at all --
    // which is now the usual case, because a band stocks itself whether or not
    // anyone is there -- this is neutral luck, and neutral luck is exactly
    // what the difficulty anchors are stated at.
    const Vec2 centre{zone.bounds.x + zone.bounds.w * 0.5, zone.bounds.y + zone.bounds.h * 0.5};
    const double luck = nearestViewerLuck(viewers, zone.realm, centre);

    // The band's own DIFFICULTY, run through the one curve: this is where the
    // map's rarity progression comes from, and where every super in the world
    // comes from -- a difficulty-200 band is full of supers because its author
    // said so, not because a separate pass decided the world was owed one.
    //
    // Stopped at super. Everything above it belongs to the biome's clocks
    // (below): a band steep enough to roll uniques on its own would spawn them
    // far faster than kUniqueSpawnCooldownMillis allows.
    // Species-only overlays retain the band's threat while overriding its roster.
    const SpawnZone* roster = &rosterAt(at);
    const int forcedTier = roster->forceRarity >= 0 ? roster->forceRarity : zone.forceRarity;
    const bool rift = riftRealms_[realmIndex(zone.realm)];
    double rollDifficulty = zone.difficulty;
    if (rift) rollDifficulty = std::min(83.0, rollDifficulty + riftDifficulty_);
    // The band before the deepest one gets a mix of its own curve and the
    // deepest curve, so it has more Supers while still being a step below it.
    if (zone.prestige && zone.realm == prestigeHelRealm_ && forcedTier < 0 &&
        rollDifficulty >= kDeepHelDifficulty && rollDifficulty < kPrestigeHelSuperBand &&
        rng.chance(kPrestigeHelMidBandUpperRoll)) {
        rollDifficulty = kPrestigeHelSuperBand;
    }
    Rarity rarity = forcedTier >= 0 ? clampLadderRarity(forcedTier)
                                   : (worldMaps && worldMaps->forRealm(zone.realm) && worldMaps->forRealm(zone.realm)->usesReferenceSpawnCurve()
                                      ? rollReferenceSpawnRarity(rollDifficulty,luck,rng)
                                      : rollSpawnRarity(rollDifficulty, luck, rng));
    const Rarity maximum = rift ? Rarity::Legendary : Rarity::Super;
    if (rarityIndex(rarity) > rarityIndex(maximum)) rarity = maximum;
    if (fixedSuper) rarity = managedTier;
    const bool prestigeTwo = authoredMap && authoredMap->prestigeLevel() == 2 &&
        !fixedSuper && forcedTier < 0 && !zone.singular;
    if (prestigeTwo) {
        rarity = static_cast<Rarity>(std::min(rarityIndex(Rarity::Unique), rarityIndex(rarity) + 2));
        // Keep one cleared low slot available when the room otherwise has only
        // Eternals. Lower rolls are still free to follow the normal curve.
        if (lastLowerTier && rarity == Rarity::Unique) rarity = *lastLowerTier;
    }
    const bool managedArcticEternal = fixedSuper && managedTier == Rarity::Unique &&
        authoredMap && hasDeepArcticEternalSlot(*authoredMap);
    if (!prestigeTwo && zone.prestige && !managedArcticEternal) {
        // Authored fixed spawns keep their configured promotion. Natural
        // bands limit the promotion from the top ordinary tier.
        rarity = forcedTier >= 0 ? prestigeRarity(rarity) : rollPrestigeAmbientRarity(rarity, rng);
    }
    std::uint16_t type = roster == &zone ? chooseZoneMobType(content, zone, at, rarity, rng)
                                        : rollResolvedRows(content, roster->resolved, rarity, rng);
    if (reuseSpecies) type = replacementType;
    if (type == kInvalidIndex) return false;
    if (superFarm && content.mob(type).dungeon.present) return false;
    // A band naming a mob outright can name one below its own tier; the mob's
    // floor wins, exactly as it does on every other spawn path. Read the tier
    // AFTER the floor, because that is the tier that decides whether this is a
    // record or an event.
    rarity = clampLadderRarity(std::max(rarityIndex(rarity), rarityIndex(content.mob(type).minRarity)));
    // A mob whose floor sits above super cannot stand in a twin at all; the
    // slot goes back to the band and is rolled afresh.
    if ((rift && rarityIndex(rarity) > rarityIndex(maximum)) ||
        (zone.prestige && barredFromPrestige(rarity) &&
         !((prestigeTwo || managedArcticEternal) && rarity == Rarity::Unique))) return false;

    // Once in a long while the band does not grow what it rolled: a square
    // stands there instead, one spawn in 100,000 and one super in 10,000.
    // At the tier the band rolled, and before the boss upgrade below, so a
    // super square can come out the biome's unique like any other super.
    type = rollStandIn(content, type, rarity, rng);

    if (fixedSuper && content.mob(type).neverAmbient) return false;
    if (fixedSuper && rarityIndex(managedTier) >= rarityIndex(Rarity::Unique) &&
        (content.mob(type).dungeon.present || content.mob(type).intangible)) return false;

    // A super may come out a unique or an apex instead, if its biome's clock
    // for that tier has run down. After the type roll, which is safe because a
    // mob that is ambient at super is ambient at every tier above it; and never
    // for a permanent fixture, which is a DPS post rather than an event.
    // Prestige uses an independent Eternal clock and allows one standing boss.
    bool upgraded = false;
    if (rarity == Rarity::Super && !zone.prestige && !content.mob(type).neverAmbient) {
        if (!fixedSuper) {
            const auto* authored=worldMaps?worldMaps->forRealm(zone.realm):nullptr;
            // Reference maps use separate S and E populations, not random promotions.
            rarity = authored && authored->usesReferenceSpawnCurve() ? Rarity::Super :
                     rollBossUpgrade(world, zone, nowMillis);
        }
        upgraded = rarity != Rarity::Super;
        if (!fixedSuper && !upgraded) rarity = Rarity::Ultra;
    }

    const auto& mob = content.mob(type);
    const bool prestigePromotion = !prestigeTwo && zone.prestige && rarity == Rarity::Super &&
        mob.id != "ghost" &&
        !fixedSuper && forcedTier < 0 && !zone.singular &&
        !mob.neverAmbient && !mob.dungeon.present && !mob.intangible &&
        bossCooldownLeft(Rarity::Unique, zone.realm, nowMillis) == 0.0 &&
        std::any_of(viewers.begin(), viewers.end(),
            [&](const Viewer& viewer) { return viewer.realm == zone.realm; }) &&
        rng.chance(kPrestigeEternalChance) && !topBossTaken(world, Rarity::Unique, zone.realm);
    if (prestigePromotion) {
        rarity = Rarity::Unique;
        upgraded = true;
    }

    // Only the Super leafbug family gets the requested 50/50 variant roll.
    // Do this after boss upgrades so other rarities retain their authored weights.
    if (rarity == Rarity::Super) {
        const auto leafbug = content.mobIndex("leafbug");
        const auto golden = content.mobIndex("golden_leafbug");
        if (leafbug != kInvalidIndex && golden != kInvalidIndex &&
            (type == leafbug || type == golden)) {
            type = rng.unit() < 0.5 ? leafbug : golden;
        }
    }

    if ((prestigeTwo && rarity == Rarity::Unique) || !alwaysAwake(content, type, rarity, zone.prestige)) {
        // The ordinary case, and the whole reason the map can be full: a
        // position, a type and a tier. No entity, no components, no wire id.
        //
        // A replacement for a kill waits for its ordinary respawn delay whether
        // or not a player is nearby. Nothing else waits. A
        // top-up is a cold start and a returned slot is a refill, and the map
        // does not wait to be full: the first player onto a server, or an
        // operator after a killall, sees the band full on the next pass, on
        // their own screen too.
        double delay = 0.0;
        if (slot == Slot::Killed) {
            const bool jungleDelay = (zone.realm == ordinaryJungleRealm_ || zone.realm == prestigeJungleRealm_) &&
                !zone.singular && forcedTier < 0;
            const bool sewersRefill = zone.realm == prestigeSewersRealm_ &&
                zone.difficulty >= kPrestigeSewersBoxDifficulty && !zone.singular && forcedTier < 0;
            const bool prestigeHelRefill = zone.prestige && zone.realm == prestigeHelRealm_ &&
                zone.difficulty >= kPrestigeHelUpperBand && !zone.singular && forcedTier < 0;
            if (jungleDelay) delay = rng.range(kJungleRespawnMinMillis, kJungleRespawnMaxMillis);
            else if (isDeepPrestigeFactory(zone))
                delay = rng.range(kPrestigeFactoryRespawnMinMillis, kPrestigeFactoryRespawnMaxMillis);
            else if (sewersRefill || prestigeHelRefill || (authoredMap && !zone.prestige &&
                     authoredMap->templateId() == "hel" && zone.difficulty >= kDeepHelDifficulty &&
                     !zone.singular && forcedTier < 0)) delay = rng.range(3000.0, 7000.0);
            else {
                const double maxDelay = !zone.prestige && !zone.singular && forcedTier < 0
                    ? kOrdinaryInViewRespawnMaxMillis : kInViewRespawnMaxMillis;
                delay = rng.range(kInViewRespawnMinMillis, maxDelay);
            }
        }
        // Only an existing vacancy is accelerated. Cold stock and returning
        // sleepers remain immediately available, and quotas never grow.
        if (slot == Slot::Killed && authoredMap && authoredMap->prestige() &&
            rarityIndex(rarity) <= rarityIndex(authoredMap->prestigeLevel() == 2 ? Rarity::Unique : Rarity::Super) && !fixedSuper && !zone.singular && forcedTier < 0 &&
            liveDensity >= 0.0) {
            delay = std::max(500.0, delay * (lastLowerTier ? 0.15 : clamp(liveDensity, 0.15, 1.0)));
        }
        addLatent(zoneIndex, LatentMob{at, slot == Slot::Killed ? nowMillis + delay : 0.0,
                                       type, rarity,kInvalidIndex,false,false,false,fixedSuper},content);
        return true;
    }

    // A boss, or a permanent fixture. Neither is ever a record: a boss is an
    // event the whole server is told about the moment it happens and has to be
    // standing where it was announced, and a dummy is the DPS row.
    //
    // Now that the body is known, the lap test again with the real one: this
    // branch puts an ENTITY on the ground this instant, and a boss is the one
    // spawn whose radius is nothing like the nominal figure the point was
    // chosen with -- a super centipede brings ten of them. Refusing returns
    // the slot to the band, which tries again somewhere else on its next pass.
    // A natural Hyper can fill its usual slot at the new rarity.
    if (rarity == Rarity::Apex && (fixedSuper || upgraded) &&
        !mob.neverAmbient && !mob.dungeon.present && !mob.intangible &&
        !topBossTaken(world, Rarity::Ascendant, zone.realm) &&
        rng.chance(kNaturalApexChance)) {
        rarity = Rarity::Ascendant;
    }
    const SpawnFootprint footprint = spawnFootprint(content, type, rarity);
    const std::optional<double> facing =
        wakingFacing(viewers, zone.realm, at, footprint, kMinSpawnDistance, rng);
    if (!clearOfEveryLap(viewers, zone.realm, at, footprint, facing, kMinSpawnDistance)) {
        return false;
    }

    if(!content.mob(type).neverAmbient && nearTrainingDummy(zone.realm,at,footprint.radius,
            footprint.tail,footprint.tailRadius,facing))return false;
    const int section = sectionAt(at);
    if (content.mob(type).neverAmbient &&
        permanentFixtureExists(world, type, rarity, zone.realm, section)) {
        return false;
    }
    if (census_.mobs >= mobCap) return false;

    const Entity spawned = spawnMobAt(world, terrain, content, type, rarity, at, zone.realm,
                                      nowMillis, rng, 0, zoneIndex, facing, !(prestigeTwo && rarity == Rarity::Unique), true,
                                      prestigePromotion || (prestigeTwo && rarity == Rarity::Unique));
    if (spawned == NULL_ENTITY) return false;
    if (!zone.singular && forcedTier < 0 && mob.id == "leech")
        protectNewLeech(world, spawned, nowMillis);
    auto& ambient = world.get<AmbientMob>(spawned);
    ambient.managedPopulation = fixedSuper;
    // Routine world and farm Super quotas remain present on the map. Let
    // distant idle stock use the ordinary simulation cadence, while the
    // dedicated Eternal/Hyper clocks and scripted encounters stay full-rate.
    ambient.routineSuperQuota = fixedSuper && rarity == Rarity::Super &&
        !zone.prestige && !zone.singular && forcedTier < 0 &&
        !mob.dungeon.present && mob.id != "ghost";
    if (managedVacancy < managedVacancies_.size())
        managedVacancies_.erase(managedVacancies_.begin() + static_cast<std::ptrdiff_t>(managedVacancy));
    if (superFarm) world.add<SuperFarmMob>(spawned);
    ++zone.liveMobs;
    // Charged only now that the boss is standing. Every refusal above hands
    // the slot back to the band and rolls it afresh on a later pass, and a
    // clock spent on a spawn that never happened would be a whole cooldown
    // with no boss in it.
    if (upgraded && rarity != Rarity::Ascendant) restartBossClock(zone.biome, rarity, nowMillis);
    // Counted straight away, so the rest of this pass spaces itself against
    // what it has just placed rather than against the last census alone --
    // in the band's realm, or crowdedAt() would never see it.
    noteLiveBandMob(world, spawned, zoneIndex,content);
    return true;
}

void SpawnSystem::bankCasualties(World& world, const Terrain& terrain,
                                 const ContentRegistry& content,
                                 const std::vector<Viewer>& viewers, Rng& rng, double nowMillis) {
    if (zones_.empty()) return;

    casualtyList_.clear();
    casualties_->each([&](Entity e, MobTag&, Transform& transform, AmbientMob& ambient, Dead&) {
        if (ambient.zone >= zones_.size()) return;
        const auto* type = world.tryGet<MobType>(e);
        if (type && type->rarity == Rarity::Ascendant)
            restartBossClock(zones_[ambient.zone].biome, Rarity::Apex, nowMillis);
        const auto replacement = type && rarityIndex(type->rarity) <= rarityIndex(Rarity::Super)
            ? (ambient.replacedSpecies != kInvalidIndex ? ambient.replacedSpecies : type->configIndex)
            : kInvalidIndex;
        casualtyList_.push_back(Casualty{e, transform.position, ambient.zone, replacement,
                                       ambient.managedPopulation});
        // Claimed, once and for all. A corpse lies around for a while before
        // the reaper takes it, and a band that banked it on every pass in
        // between would breed a mob per pass out of one kill.
        ambient.zone = kInvalidIndex;
    });

    // Collected first, spent afterwards: stocking a boss creates an entity,
    // and a create() relocates the very rows the walk above is holding.
    //
    // A slot this cannot place -- it died in a pocket of wall, or against a
    // crowd -- is simply not placed. The band is then one short of its target,
    // which is exactly what the top-up pass exists to notice, so the mob comes
    // back somewhere else in the band rather than being lost.
    const auto densityRefill = [&](const Casualty& casualty) {
        if (casualty.managedPopulation || casualty.zone >= zones_.size()) return false;
        const auto& zone = zones_[casualty.zone];
        const auto* map = worldMaps ? worldMaps->forRealm(zone.realm) : nullptr;
        const auto* type = world.tryGet<MobType>(casualty.entity);
        return map && map->prestige() && !zone.singular && zone.forceRarity < 0 &&
            type && rarityIndex(type->rarity) <= rarityIndex(map->prestigeLevel() == 2 ? Rarity::Unique : Rarity::Super) && !world.has<SuperFarmMob>(casualty.entity);
    };
    // One live-only spatial snapshot per casualty batch, not a full scan per
    // death. Dead bodies and dormant records cannot make a cleared room full.
    SpatialGrid refillLiving;DensityRoots refillRoots{world};
    const bool needsDensity = std::any_of(casualtyList_.begin(), casualtyList_.end(), densityRefill);
    if (needsDensity) {
        Query<MobTag, Transform, Body, MobType> living{world};
        living.without<Dead, Pet>();
        living.each([&](Entity e, MobTag&, Transform& at, Body& body, MobType& type) {
            const Entity root=refillRoots.get(e);if(root==NULL_ENTITY)return;
            // Independent ghosts are stocked separately and may be invisible.
            // They do not replace an ordinary mob in the local population.
            const auto* ambient = world.tryGet<AmbientMob>(root);
            if (ambient && ambient->independentGhost) return;
            const auto* rootType=world.tryGet<MobType>(root);
            if(rootType&&content.mob(rootType->configIndex).neverAmbient&&!world.has<BodySegment>(root))return;
            refillLiving.insert(e, at.realm, at.position, body.radius);
        });
    }
    std::vector<Entity> densityHits;
    std::set<std::uint16_t> preservedBands;
    for (const Casualty& casualty : casualtyList_) {
        // Off the census list as well: the reaper is about to take the corpse,
        // and the next census must not read that as a mob that vanished and
        // hand its band the same slot a second time.
        const auto tracked =
            std::find_if(censusMobs_.begin(), censusMobs_.end(),
                         [&](const CensusMob& mob) { return mob.entity == casualty.entity; });
        if (tracked != censusMobs_.end()) {
            *tracked = censusMobs_.back();
            censusMobs_.pop_back();
        }
        if (casualty.managedPopulation) {
            const auto* type = world.tryGet<MobType>(casualty.entity);
            // Rare stand-ins are independent chance events, not a permanent
            // roster slot. Reroll their vacancy instead of pinning the quota
            // to a species that none of the authored habitats accepts.
            const auto& standIns = content.standInMobs();
            if (type && type->rarity != Rarity::Ascendant &&
                std::find(standIns.begin(), standIns.end(), type->configIndex) == standIns.end())
                managedVacancies_.push_back({casualty.zone, type->configIndex, type->rarity,
                                             world.has<SuperFarmMob>(casualty.entity)});
            continue;
        }
        if (world.has<SuperFarmMob>(casualty.entity)) continue;
        double liveDensity = -1.0;
        std::optional<Rarity> lastLowerTier;
        if (densityRefill(casualty)) {
            auto& band = zones_[casualty.zone];
            const auto* lostType=world.tryGet<MobType>(casualty.entity);
            band.refillAnchors[band.refillAnchorNext]={casualty.position,lostType?lostType->configIndex:kInvalidIndex,lostType?lostType->rarity:Rarity::Common};
            band.refillAnchorNext=(band.refillAnchorNext+1)%4;
            band.refillAnchorCount=std::min(4,band.refillAnchorCount+1);
            constexpr double reach = 900.0;
            const Body* lostBody = world.tryGet<Body>(casualty.entity);
            const double lostRadius = lostBody ? lostBody->radius : 0.0;
            const Viewer* seer = nullptr;
            double nearestSq = std::numeric_limits<double>::infinity();
            for (const Viewer& viewer : viewers) {
                if (viewer.realm != band.realm ||
                    !bodyOverlapsRoom(viewerRoom(viewer), casualty.position, lostRadius)) continue;
                const double gapSq = distanceSq(viewer.position, casualty.position);
                if (gapSq >= nearestSq ||
                    terrain.segmentBlocked(viewer.position, casualty.position, band.realm)) continue;
                seer = &viewer;
                nearestSq = gapSq;
            }

            const Rect room = seer ? viewerRoom(*seer) :
                Rect{casualty.position.x - reach, casualty.position.y - reach, 2.0 * reach, 2.0 * reach};
            const Vec2 observer = seer ? seer->position : casualty.position;
            const double left = std::max(band.bounds.x, room.left());
            const double top = std::max(band.bounds.y, room.top());
            const double right = std::min(band.bounds.right(), room.right());
            const double bottom = std::min(band.bounds.bottom(), room.bottom());
            const Rect area{left, top, std::max(0.0, right-left), std::max(0.0, bottom-top)};
            int openSamples = 0;
            for (int gy=0;gy<4;++gy) for (int gx=0;gx<4;++gx) {
                const Vec2 point{area.x+area.w*(gx+.5)/4,area.y+area.h*(gy+.5)/4};
                if (zoneContains(band.bounds,band.polygon,point) && !terrain.blocked(point,band.realm) &&
                    !terrain.segmentBlocked(observer,point,band.realm)) ++openSamples;
            }
            const double expected = std::max(1.0, band.targetMobs * area.w * area.h * openSamples / 16.0 /
                std::max(1.0, band.bounds.w * band.bounds.h));
            refillLiving.queryRect(band.realm, area, densityHits);
            std::set<Entity> aliveRoots;
            for (const Entity e : densityHits) {
                const auto& at=world.get<Transform>(e);const auto& body=world.get<Body>(e);
                const Entity root=refillRoots.get(e);
                if (world.isAlive(root) && !world.has<Dead>(root) &&
                    bodyOverlapsRoom(area, at.position, body.radius) &&
                    !terrain.segmentBlocked(observer, at.position, band.realm)) aliveRoots.insert(root);
            }
            const int alive=static_cast<int>(aliveRoots.size());
            liveDensity = alive / expected;
            const auto* map = worldMaps->forRealm(band.realm);
            if (seer && map && map->prestigeLevel() == 2 && lostType && lostType->rarity < Rarity::Unique &&
                !preservedBands.count(casualty.zone) &&
                std::none_of(aliveRoots.begin(), aliveRoots.end(), [&](Entity e) {
                    const auto* type = world.tryGet<MobType>(e);
                    return type && type->rarity < Rarity::Unique;
                })) lastLowerTier = lostType->rarity;
        }
        const bool stocked = stockZone(world, terrain, content, zones_[casualty.zone], casualty.zone, viewers, rng,
                  nowMillis, Slot::Killed, casualty.position, false, Rarity::Super, false,
                  casualty.mobIndex, liveDensity, lastLowerTier);
        if (stocked && lastLowerTier) preservedBands.insert(casualty.zone);
        auto& zone = zones_[casualty.zone];
        if (!stocked && casualty.mobIndex != kInvalidIndex &&
            zone.pendingSpecies.size() < static_cast<std::size_t>(zone.targetMobs))
            zone.pendingSpecies.push_back(casualty.mobIndex);
    }
}

void SpawnSystem::returnVanished(World& world, const Terrain& terrain,
                                 const ContentRegistry& content,
                                 const std::vector<Viewer>& viewers, Rng& rng, double nowMillis) {
    // Before the top-up, so the band is not short by the time it looks and the
    // slots go back where they were lost rather than anywhere in the outline.
    // A killall empties exactly the ground people were looking at; a uniform
    // top-up would hand each screen its area's share of the refill and pile
    // the rest onto ground that was never short.
    //
    // Ready at once, unlike a kill's replacement: nobody fought for this
    // ground, so there is no cleared screen to keep clear. Whatever cannot be
    // placed falls through to the top-up, as a casualty does.
    for (const CensusMob& lost : vanished_) {
        if (lost.zone >= zones_.size() || lost.managedPopulation) continue;
        auto& zone = zones_[lost.zone];
        const bool stocked = stockZone(world, terrain, content, zone, lost.zone, viewers, rng,
                                       nowMillis, Slot::Vanished, lost.position, false,
                                       Rarity::Super, false, lost.mobIndex);
        if (!stocked && lost.mobIndex != kInvalidIndex &&
            zone.pendingSpecies.size() < static_cast<std::size_t>(zone.targetMobs))
            zone.pendingSpecies.push_back(lost.mobIndex);
    }
    vanished_.clear();
}

// ---------------------------------------------------------------------------
// Waking it up
// ---------------------------------------------------------------------------

void SpawnSystem::wakeAhead(World& world, const Terrain& terrain, const ContentRegistry& content,
                            Rng& rng, double nowMillis) {
    // The default wake margin covers a WALK between passes. A bubble, a teleporter or
    // a join covers thousands of units in one tick, and waking only on the pass
    // left a flower arriving that way on an empty screen for up to half a
    // second, then filled it all at once. So a viewer that has come more than
    // half the margin from every point a wake was done from this pass is woken
    // round now -- and only that viewer: re-walking every band for everyone
    // every tick cost a sixth of a busy tick.
    const double movementAllowance = latentWakeMargin * 0.5;
    const double movementAllowanceSq = movementAllowance * movementAllowance;
    wakeSeers_.clear();
    for (const Viewer& viewer : viewers_) {
        bool covered = false;
        for (const Viewer& woken : wokenFrom_) {
            if (woken.realm != viewer.realm) continue;
            const Vec2 offset = viewer.viewportCentre - woken.viewportCentre;
            // A zoom or resize can expose unprepared ground without moving
            // the player. Reuse the old wake while it still covers the view.
            if (distanceSq(woken.position, viewer.position) <= movementAllowanceSq &&
                std::abs(offset.x) + viewer.half.x <= woken.half.x + latentWakeMargin &&
                std::abs(offset.y) + viewer.half.y <= woken.half.y + latentWakeMargin) {
                covered = true;
                break;
            }
        }
        if (!covered) wakeSeers_.push_back(viewer);
    }
    if (wakeSeers_.empty()) return;
    promoteLatent(world, terrain, content, wakeSeers_, viewers_, rng, nowMillis);
    wokenFrom_.insert(wokenFrom_.end(), wakeSeers_.begin(), wakeSeers_.end());
}

bool SpawnSystem::isDeepPrestigeFactory(const SpawnZone& zone) const {
    return (zone.realm == prestigeFactoryRealm_ || zone.realm == prestigeTwoFactoryRealm_) &&
           zone.difficulty >= kPrestigeFactoryDeepDifficulty &&
           !zone.singular && zone.forceRarity < 0;
}

void SpawnSystem::promoteLatent(World& world, const Terrain& terrain,
                                const ContentRegistry& content, const std::vector<Viewer>& seers,
                                const std::vector<Viewer>& viewers, Rng& rng, double nowMillis, bool redistribute) {
    // Nobody is looking at anything, so nothing has to be awake. This is also
    // what keeps an unattended server from paying for the population it holds:
    // the whole map is stocked and not one of it is simulated.
    if (seers.empty()) return;
    std::array<std::vector<Viewer>,kMaxRealms> realmSeers,realmViewers;
    for(const auto& viewer:seers)realmSeers[realmIndex(viewer.realm)].push_back(viewer);
    for(const auto& viewer:viewers)realmViewers[realmIndex(viewer.realm)].push_back(viewer);


    const int ambientBudget = mobCap >= 128 ? mobCap - 128 : mobCap;
    const double wakeMargin = census_.mobs >= mobCap * 4 / 5 ? 0.0 : latentWakeMargin;
    // At saturation, these areas may borrow existing escort headroom for
    // nearby enemies. The shared live-mob ceiling still applies.
    std::array<Realm, 2> prestigeHelWakeRealms{Realm::Arena, Realm::Arena};
    if (worldMaps) for (const auto& map : worldMaps->maps()) {
        if (map.templateId() == "hel" && map.usesReferenceSpawnCurve() &&
            map.prestigeLevel() >= 1 && map.prestigeLevel() <= 2)
            prestigeHelWakeRealms[map.prestigeLevel() - 1] = map.realm();
    }
    constexpr std::array reserveMin{kJungleMinAwake, kJungleMinAwake, kPrestigeSewersMinAwake,
                                    kPrestigeHelMinAwake, kPrestigeHelMinAwake,
                                    kPrestigeFactoryMinAwake, kPrestigeFactoryMinAwake};
    const std::array reserveRealms{ordinaryJungleRealm_, prestigeJungleRealm_, prestigeSewersRealm_,
                                  prestigeHelWakeRealms[0], prestigeHelWakeRealms[1],
                                  prestigeFactoryRealm_, prestigeTwoFactoryRealm_};
    std::array<int, reserveMin.size()> awake{};
    std::array<bool, reserveMin.size()> inView{};
    const auto prioritySlot = [&](const SpawnZone& zone) {
        if (zone.realm == ordinaryJungleRealm_) return 0;
        if (zone.realm == prestigeJungleRealm_) return 1;
        if (zone.realm == prestigeSewersRealm_ && zone.difficulty >= kPrestigeSewersBoxDifficulty) return 2;
        if (zone.prestige && zone.difficulty >= kPrestigeHelUpperBand &&
            !zone.singular && zone.forceRarity < 0) {
            if (zone.realm == prestigeHelWakeRealms[0]) return 3;
            if (zone.realm == prestigeHelWakeRealms[1]) return 4;
        }
        if (isDeepPrestigeFactory(zone)) return zone.realm == prestigeFactoryRealm_ ? 5 : 6;
        return -1;
    };
    const auto inRoom = [&](const Viewer& viewer, Vec2 at) {
        return std::abs(at.x - viewer.viewportCentre.x) <= viewer.half.x &&
               std::abs(at.y - viewer.viewportCentre.y) <= viewer.half.y &&
               !terrain.segmentBlocked(viewer.position, at, viewer.realm);
    };
    const auto inFactoryView = [&](Realm realm, Vec2 at) {
        return std::any_of(seers.begin(), seers.end(), [&](const Viewer& viewer) {
            return viewer.realm == realm && inRoom(viewer, at);
        });
    };
    bool canReserve = false;
    if (ambientBudget < mobCap && census_.mobs >= ambientBudget) {
        for (std::size_t slot = 0; slot < inView.size(); ++slot) {
            inView[slot] = std::any_of(seers.begin(), seers.end(), [&](const Viewer& viewer) {
                return viewer.realm == reserveRealms[slot];
            });
        }
        if (std::any_of(inView.begin(), inView.end(), [](bool present) { return present; })) {
            for (const auto& mob : censusMobs_) {
                if (mob.zone >= zones_.size()) continue;
                const auto& zone = zones_[mob.zone];
                const int slot = prioritySlot(zone);
                if (slot >= 0 && inView[slot] && awake[slot] < reserveMin[slot] &&
                    (isDeepPrestigeFactory(zone) ? inFactoryView(zone.realm, mob.position)
                                                 : seenBy(realmSeers[realmIndex(zone.realm)], zone.realm, mob.position, latentWakeMargin)))
                    ++awake[slot];
            }
        }
        for (std::size_t slot = 0; slot < inView.size(); ++slot)
            canReserve |= inView[slot] && awake[slot] < reserveMin[slot];
    }
    const int wakeBudget = canReserve ? mobCap : ambientBudget;
    if (census_.mobs >= wakeBudget) return;

    // Gathered first and woken nearest-first. There is no allowance per band:
    // a player arriving through a door, or wearing antennae that show half the
    // map, sees the whole of what is in front of them at once rather than
    // watching it fill in over several seconds. mobCap is the only limit, and
    // when it binds the order decides what stays asleep -- the far edge of the
    // widest view, never the ground somebody is standing on.
    wakeList_.clear();
    std::map<std::pair<std::uint16_t,Rarity>,SpawnFootprint> wakeFootprints;
    std::vector<double> zoneBodyRadii(zones_.size(),0.0);
    const auto footprintFor = [&](const LatentMob& record) -> const SpawnFootprint& {
        const auto key=std::make_pair(record.mobIndex,record.rarity);
        auto it=wakeFootprints.find(key);
        if(it==wakeFootprints.end())it=wakeFootprints.emplace(key,spawnFootprint(content,record.mobIndex,record.rarity)).first;
        return it->second;
    };
    const auto isRollingRefill = [](const LatentMob& record, const SpawnZone& zone, const MapData* map) {
        return !record.managedPopulation && map && map->prestige() && !zone.singular &&
            zone.forceRarity < 0 && (record.densityRelocated || (record.readyMillis > 0.0 && !record.previouslyAwake)) &&
            rarityIndex(record.rarity) <= rarityIndex(map->prestigeLevel() == 2 ? Rarity::Unique : Rarity::Super);
    };
    for (std::size_t index = 0; index < zones_.size(); ++index) {
        SpawnZone& zone = zones_[index];
        if (zone.latent.empty()) continue;
        const int slot = prioritySlot(zone);
        // Over budget the reserve only tops up priority maps to a floor. An
        // AFK farmer still needs whatever is sitting in their OWN viewport:
        // without that, moving was the only way to get a local wakeAhead pass
        // whose seer list did not already count other players' awake mobs
        // against the shared jungle floor (board #3866).
        const bool reserveExhausted = census_.mobs >= ambientBudget &&
            (slot < 0 || !inView[slot] || awake[slot] >= reserveMin[slot]);
        if(realmSeers[realmIndex(zone.realm)].empty())continue;
        // Radius maxima only grow while a band is stocked. An upper bound is
        // enough for coarse culling; rescanning hundreds of thousands of far
        // dormant records every population pass is unnecessary.
        if(zone.maxDormantReach<=0.0)for(const auto& record:zone.latent) {
            const auto& footprint=footprintFor(record);
            zone.maxDormantRadius=std::max(zone.maxDormantRadius,footprint.radius);
            zone.maxDormantReach=std::max(zone.maxDormantReach,std::max(footprint.radius,footprint.tail+footprint.tailRadius));
        }
        const double bodyReach=zone.maxDormantReach;
        zoneBodyRadii[index]=zone.maxDormantRadius;
        if (reserveExhausted && !zoneInView(zone.bounds, zone.realm, realmSeers[realmIndex(zone.realm)], bodyReach)) continue;
        // The cheap rejection, and the reason this is affordable at all: one
        // rectangle test per viewer says whether the band's records are worth
        // walking, and on a map with two dozen bands almost none of them are.
        if (!zoneInView(zone.bounds, zone.realm, realmSeers[realmIndex(zone.realm)], bodyReach + (reserveExhausted ? 0.0 : wakeMargin))) continue;

        // Wake the pre-stocked positions. If a player occupies one, keep
        // the resting record there until the body can safely wake.
        const auto* map = worldMaps ? worldMaps->forRealm(zone.realm) : nullptr;
        for (std::size_t i = 0; i < zone.latent.size(); ++i) {
            const LatentMob& record = zone.latent[i];
            if(record.mobIndex>=content.mobCount()) {
                wakeList_.push_back(WakeCandidate{static_cast<std::uint16_t>(index),static_cast<std::uint32_t>(i),0.0,std::nullopt});
                continue;
            }
            if (nowMillis < record.readyMillis) continue;
            // Strict viewport when the shared reserve is spent; otherwise the
            // ordinary wake margin (which collapses to zero near mobCap anyway).
            const auto& physicalFootprint=footprintFor(record);
            const double visibleReach=std::max(physicalFootprint.radius,physicalFootprint.tail+physicalFootprint.tailRadius);
            if (!seenBy(realmSeers[realmIndex(zone.realm)], zone.realm, record.position,
                        visibleReach + (reserveExhausted ? 0.0 : wakeMargin))) continue;
            // Nobody's lap, at the moment the body actually appears.
            //
            // The record's own placement test was made against where the
            // players were when it was WRITTEN, which for a replacement is up
            // to twelve seconds and three thousand units ago, and for a record
            // stocked cold is before anybody had arrived at all. Waking one on
            // the spot a flower is standing on materialises a mob already
            // touching it, and body damage is dealt on the tick after that --
            // which is the "a mob spawned on me and killed me" report.
            //
            // Allow for the largest body and for the whole length of a chain.
            const SpawnFootprint footprint =
                spawnFootprint(content, record.mobIndex, record.rarity);
            // A timed replacement behind a wall or just beyond the view stays
            // available to refill the player's cleared room. Cold stock and
            // previously awake animals still get the normal pre-wake margin.
            if (isRollingRefill(record, zone, map) && !record.densityRelocated &&
                std::none_of(realmSeers[realmIndex(zone.realm)].begin(),
                             realmSeers[realmIndex(zone.realm)].end(), [&](const Viewer& viewer) {
                    return bodyOverlapsRoom(viewerRoom(viewer), record.position, visibleReach) &&
                        !terrain.segmentBlocked(viewer.position, record.position, zone.realm);
                })) continue;
            const bool sparseRescue=record.densityRelocated && !record.managedPopulation && map && map->prestige() &&
                rarityIndex(record.rarity)<=rarityIndex(map->prestigeLevel()==2?Rarity::Unique:Rarity::Super);
            const std::optional<double> facing = wakingFacing(realmViewers[realmIndex(zone.realm)],zone.realm,record.position,
                footprint,sparseRescue?120.0:kMinSpawnDistance,rng);
            if(sparseRescue) {
                if(!clearNaturalSpawn(zone,realmViewers[realmIndex(zone.realm)],record.position,footprint.radius,footprint.tail,footprint.tailRadius,facing))continue;
            } else if(!clearOfEveryLap(realmViewers[realmIndex(zone.realm)],zone.realm,record.position,footprint,facing,kMinSpawnDistance))continue;
            if(!content.mob(record.mobIndex).neverAmbient && nearTrainingDummy(zone.realm,record.position,
                    footprint.radius,footprint.tail,footprint.tailRadius,facing))continue;
            double nearestSq = 0.0;
            nearestViewer(realmSeers[realmIndex(zone.realm)], zone.realm, record.position, &nearestSq);
            wakeList_.push_back(WakeCandidate{static_cast<std::uint16_t>(index),
                                              static_cast<std::uint32_t>(i), nearestSq, facing, sparseRescue});
        }
    }
    std::sort(wakeList_.begin(), wakeList_.end(),
              [](const WakeCandidate& a, const WakeCandidate& b) {
                  return a.distanceSq < b.distanceSq;
              });

    std::size_t attempted = 0;
    // Rolling refills only. Entering a stocked room must still reveal its
    // existing inhabitants immediately rather than drip-feeding cold stock.
    for(auto it=refillAdmissions_.begin();it!=refillAdmissions_.end();)
        if(nowMillis>=it->second.untilMillis)it=refillAdmissions_.erase(it);else ++it;
    for (const WakeCandidate candidate : wakeList_) {
        if (census_.mobs >= wakeBudget) break;
        SpawnZone& zone = zones_[candidate.zone];
        const LatentMob& record = zone.latent[candidate.index];
        if(record.mobIndex>=content.mobCount()) {
            wakeList_[attempted++]=candidate; // obsolete content is the one intentional discard.
            continue;
        }
        const auto* refillMap = worldMaps ? worldMaps->forRealm(zone.realm) : nullptr;
        const bool rollingRefill = isRollingRefill(record, zone, refillMap);
        const auto refillCell = std::make_tuple(zone.realm,
            static_cast<int>(std::floor(record.position.x / 900.0)),
            static_cast<int>(std::floor(record.position.y / 900.0)));
        RefillGate* gate=nullptr;
        if(rollingRefill) {
            auto& cell=refillAdmissions_[refillCell];
            if(nowMillis>=cell.untilMillis){cell.untilMillis=nowMillis+500.0;cell.admitted=0;}
            if(cell.admitted>=2){zone.latent[candidate.index].readyMillis=std::max(record.readyMillis,cell.untilMillis);continue;}
            gate=&cell;
        }
        const int slot = prioritySlot(zone);
        if (census_.mobs >= ambientBudget) {
            const bool reserved = slot >= 0 && inView[slot] && awake[slot] < reserveMin[slot];
            // Still wake a record that is inside a seer's room even after the
            // shared priority floor is full -- that is the AFK jungle case.
            if (!reserved && !std::any_of(seers.begin(), seers.end(), [&](const Viewer& viewer) {
                    return viewer.realm == zone.realm && inRoom(viewer, record.position);
                })) continue;
        }
        // Leave the record in the same slot if the complete animal won't fit.
        const int bodyCount = 1 + content.mob(record.mobIndex).segmentCount;
        if (bodyCount > mobCap - census_.mobs) continue;
        const Vec2 at = record.position;
        const auto facing = candidate.facing;
        // Once this band relocates dormant stock, every later wake in the
        // band must respect actual awake bodies, whichever candidate wins.
        if(livePlacementRealms_[realmIndex(zone.realm)] && !content.mob(record.mobIndex).neverAmbient &&
           crowdedWakingBody(terrain,content,record,zone,candidate.zone,candidate.index,facing))continue;
        // A mob across the factory wall cannot fill the reserve for this room.
        if (census_.mobs >= ambientBudget && isDeepPrestigeFactory(zone) && !inFactoryView(zone.realm, at))
            continue;
        const WakeClearance wakeClearance{realmViewers[realmIndex(zone.realm)], candidate.sparseRescue};
        const Entity spawned =
            spawnMobAt(world, terrain, content, record.mobIndex, record.rarity, at,
                       zone.realm, nowMillis, rng, 0, candidate.zone, facing, true,
                       !record.ghostAlreadyAnnounced, false, &wakeClearance);
        if (spawned == NULL_ENTITY) continue;
        wakeList_[attempted++] = candidate;
        if (gate) ++gate->admitted;
        // census_.mobs is spawnMobAt's to raise -- it is the cap every spawn
        // path shares, and counting it twice here would halve the ceiling for
        // everything else in the same pass.
        if (!record.previouslyAwake && content.mob(record.mobIndex).id == "leech")
            protectNewLeech(world, spawned, nowMillis);
        world.get<AmbientMob>(spawned).replacedSpecies = record.replacedSpecies;
        world.get<AmbientMob>(spawned).managedPopulation = record.managedPopulation;
        ++zone.liveMobs;
        if (slot >= 0) ++awake[slot];
        ++census_.promotedTotal;
        noteLiveBandMob(world, spawned, candidate.zone,content);
    }

    // Only the compacted, attempted records are removed. A record whose
    // type the content no longer defines cannot be woken and must not be
    // retried every pass forever, and dropping it lets the band stock a
    // replacement it can actually place. Removed highest index first within
    // each band, so a swap with the back never moves a record still to go.
    wakeList_.resize(attempted);
    std::sort(wakeList_.begin(), wakeList_.end(),
              [](const WakeCandidate& a, const WakeCandidate& b) {
                  return a.zone != b.zone ? a.zone < b.zone : a.index > b.index;
              });
    for (const WakeCandidate& candidate : wakeList_) {
        std::vector<LatentMob>& latent = zones_[candidate.zone].latent;
        latent[candidate.index] = latent.back();
        latent.pop_back();
    }
    latentPlacementGrid_.clear();indexedLatentZones_.assign(zones_.size(),false);
    if (!redistribute || seers.empty() || census_.mobs >= wakeBudget) return;
    // A band's remote stock cannot stand in for living mobs in an occupied
    // room. Move an eligible READY record within its own band, preserving its
    // type, tier and deadline. Never create a second slot or take live mobs.
    const bool hasPrestigeViewer=std::any_of(seers.begin(),seers.end(),[&](const Viewer& viewer) {
        const auto* map=worldMaps?worldMaps->forRealm(viewer.realm):nullptr;
        return map && map->prestige() && std::any_of(wokenFrom_.begin(),wokenFrom_.end(),[&](const Viewer& old) {
            return old.realm==viewer.realm && distanceSq(old.position,viewer.position)<350.0*350.0;
        });
    });
    if(!hasPrestigeViewer)return;
    SpatialGrid& roomLiving=roomLivingGrid_;roomLiving.clear();DensityRoots roomRoots{world};
    Query<MobTag,Transform,Body,MobType> roomMobs{world};roomMobs.without<Dead,Pet>();
    roomMobs.each([&](Entity e,MobTag&,Transform& at,Body& body,MobType& type) {
        const Entity root=roomRoots.get(e);if(root==NULL_ENTITY)return;
        const auto* rootType=world.tryGet<MobType>(root);
        if(rootType&&content.mob(rootType->configIndex).neverAmbient&&!world.has<BodySegment>(root))return;
        roomLiving.insert(e,at.realm,at.position,body.radius);
    });
    std::vector<Entity> roomHits;
    int moved=0;
    struct ReservedRoomPoint {
        Realm realm;
        Vec2 at;
        double spacingRadius;
        double lowerTierRadius;
    };
    std::vector<ReservedRoomPoint> reservedPoints;
    for(const auto& viewer:seers) {
        const auto* map=worldMaps?worldMaps->forRealm(viewer.realm):nullptr;
        if(!map || !map->prestige())continue;
        const bool established=std::any_of(wokenFrom_.begin(),wokenFrom_.end(),[&](const Viewer& old){
            return old.realm==viewer.realm && distanceSq(old.position,viewer.position)<350.0*350.0;
        });
        if(!established)continue;
        int viewerMoves=0, viewerAttempts=0, viewerDonorScans=0;
#ifdef FLIX_DENSITY_DIAGNOSTICS
        int need=0,eligible=0,terrainRejected=0,rosterRejected=0,safetyRejected=0,crowdRejected=0;
#endif
        std::vector<std::size_t> localZones;
        for(std::size_t zi=0;zi<zones_.size();++zi) {
            const auto& zone=zones_[zi];
            if(zone.realm==viewer.realm && !zone.singular && zone.forceRarity<0 && !zone.latent.empty())localZones.push_back(zi);
        }
        for(std::size_t n=localZones.size();n>1;--n)
            std::swap(localZones[n-1],localZones[rng.below(static_cast<std::uint32_t>(n))]);
        const Rect visibleRoom = viewerRoom(viewer);
        std::optional<int> visibleLiving;
        bool hasLowerTierLiving = false;
        for(const std::size_t zi:localZones) {
            if(viewerMoves>=2 || viewerAttempts>=48 || viewerDonorScans>=256 || moved>=64)break;
            auto& zone=zones_[zi];
            if(zone.realm!=viewer.realm || zone.singular || zone.forceRarity>=0 || zone.latent.empty())continue;
            const double reach=zoneBodyRadii[zi];
            // A neighbouring band's large body can be visible even when its
            // centre band is just offscreen. Restore that stock as well.
            const double left=std::max(zone.bounds.x,visibleRoom.x-reach);
            const double top=std::max(zone.bounds.y,visibleRoom.y-reach);
            const double right=std::min(zone.bounds.right(),visibleRoom.right()+reach);
            const double bottom=std::min(zone.bounds.bottom(),visibleRoom.bottom()+reach);
            if(right<=left || bottom<=top)continue;
            const Rect room{left,top,right-left,bottom-top};
            // The room census is the same for every intersecting band.
            if (!visibleLiving) {
                roomLiving.queryRect(viewer.realm, visibleRoom, roomHits);
                std::set<Entity> roots;
                for (Entity e : roomHits) {
                    const auto& at = world.get<Transform>(e);
                    const auto& body = world.get<Body>(e);
                    const Entity root = roomRoots.get(e);
                    // Check ordinary density independently of ghosts, while
                    // retaining ghost bodies in the placement grid.
                    const auto* ambient = world.tryGet<AmbientMob>(root);
                    if (ambient && ambient->independentGhost) continue;
                    if (world.isAlive(root) && !world.has<Dead>(root) &&
                        bodyOverlapsRoom(visibleRoom, at.position, body.radius) &&
                        !terrain.segmentBlocked(viewer.position, at.position, viewer.realm))
                        roots.insert(root);
                }
                visibleLiving = static_cast<int>(roots.size());
                hasLowerTierLiving = std::any_of(roots.begin(), roots.end(), [&](Entity root) {
                    const auto* type = world.tryGet<MobType>(root);
                    return type && type->rarity < Rarity::Unique;
                });
                // Earlier viewers in this pass may already have moved ready
                // stock into the same room. It has not been woken yet.
                if (map->prestigeLevel() == 2 && !hasLowerTierLiving) {
                    hasLowerTierLiving = std::any_of(reservedPoints.begin(), reservedPoints.end(),
                        [&](const ReservedRoomPoint& reserved) {
                            return reserved.realm == viewer.realm && reserved.lowerTierRadius > 0.0 &&
                                bodyOverlapsRoom(visibleRoom, reserved.at, reserved.lowerTierRadius) &&
                                !terrain.segmentBlocked(viewer.position, reserved.at, viewer.realm);
                        });
                }
            }
            int living = *visibleLiving;
            int open=0;
            for(int gy=0;gy<3;++gy)for(int gx=0;gx<3;++gx) {
                const Vec2 at{room.x+room.w*(gx+.5)/3,room.y+room.h*(gy+.5)/3};
                if(zoneContains(zone.bounds,zone.polygon,at) && !terrain.blocked(at,zone.realm) &&
                    !terrain.segmentBlocked(viewer.position,at,zone.realm))++open;
            }
            // Coarse probes can all land in walls in a narrow but valid
            // corridor. Give it a bounded real-point attempt rather than
            // permanently treating zero coarse samples as proof of no room.
            const int zoneAttemptStart=viewerAttempts;
            const int zoneAttemptBudget=open==0?4:16;
            const double expected=zone.targetMobs*room.w*room.h/std::max(1.0,zone.bounds.w*zone.bounds.h)*open/9.0;
            // A conservative floor, not a new density multiplier. An empty
            // narrow room still gets one attempt even if the coarse probes miss it.
            const int floor=std::max(1,static_cast<int>(std::floor(expected*.65)));
            // Eternal and higher bodies can meet the visible density floor by
            // themselves. Keep one lower-tier slot available for normal clearing.
            const bool needsLowerTier = map->prestigeLevel() == 2 && !hasLowerTierLiving;
            if(living>=floor && !needsLowerTier)continue;
#ifdef FLIX_DENSITY_DIAGNOSTICS
            ++need;
#endif
            const std::size_t start=rng.below(static_cast<std::uint32_t>(zone.latent.size()));
            std::array<std::pair<int,std::size_t>,64> donorOrder{};
            const std::size_t donorCount=std::min({zone.latent.size(),donorOrder.size(),static_cast<std::size_t>(256-viewerDonorScans)});
            for(std::size_t scan=0;scan<donorCount;++scan) {
                const std::size_t index=(start+scan)%zone.latent.size();const auto& record=zone.latent[index];
                int score=1;
                for(int n=0;n<zone.refillAnchorCount;++n) {
                    const auto& anchor=zone.refillAnchors[n];
                    if(record.mobIndex!=anchor.mobIndex || record.rarity!=anchor.rarity)continue;
                    const auto& body=footprintFor(record);
                    const double dx=std::max({visibleRoom.x-anchor.at.x,0.0,anchor.at.x-visibleRoom.right()});
                    const double dy=std::max({visibleRoom.y-anchor.at.y,0.0,anchor.at.y-visibleRoom.bottom()});
                    if(dx*dx+dy*dy<=body.radius*body.radius){score=0;break;}
                }
                donorOrder[scan]={score,index};
            }
            std::stable_sort(donorOrder.begin(),donorOrder.begin()+donorCount,[](const auto& a,const auto& b){return a.first<b.first;});
            // Prefer the existing lower-tier stock. If this habitat is
            // entirely Eternal, one untouched natural record can fill the
            // minimum with a Super body instead of leaving the room empty.
            const bool hasReadyLower = needsLowerTier && std::any_of(donorOrder.begin(),
                donorOrder.begin()+donorCount, [&](const auto& donor) {
                    const auto& record = zone.latent[donor.second];
                    return record.rarity < Rarity::Unique && !record.managedPopulation &&
                        record.readyMillis <= nowMillis && record.mobIndex < content.mobCount() &&
                        !content.mob(record.mobIndex).neverAmbient;
                });
            for(std::size_t scan=0;scan<donorCount && viewerAttempts<48 && viewerAttempts-zoneAttemptStart<zoneAttemptBudget && viewerDonorScans<256;++scan,++viewerDonorScans) {
                const std::size_t recordIndex=donorOrder[scan].second;
                auto& record=zone.latent[recordIndex];
                const bool lowerTierOnly = living >= floor;
                if((lowerTierOnly && (!needsLowerTier || hasLowerTierLiving)) ||
                   viewerMoves>=2 || moved>=64)break;
                if(record.managedPopulation || record.readyMillis>nowMillis || record.mobIndex>=content.mobCount() ||
                    content.mob(record.mobIndex).neverAmbient ||
                    rarityIndex(record.rarity)>rarityIndex(map->prestigeLevel()==2?Rarity::Unique:Rarity::Super))continue;
                const bool fillLower = needsLowerTier && !hasLowerTierLiving;
                const bool substituteLower = fillLower && !hasReadyLower && record.rarity == Rarity::Unique &&
                    !record.previouslyAwake && content.mob(record.mobIndex).minRarity <= Rarity::Super;
                if ((lowerTierOnly || fillLower) && record.rarity >= Rarity::Unique && !substituteLower) continue;
                // Do not transfer stock reserved for another player's view.
                const auto footprint=spawnFootprint(content,record.mobIndex,record.rarity);
                const auto placementFootprint = substituteLower
                    ? spawnFootprint(content,record.mobIndex,Rarity::Super) : footprint;
                const double donorReach=std::max(footprint.radius,footprint.tail+footprint.tailRadius);
                const bool rolling = isRollingRefill(record, zone, map);
                bool anotherView = false;
                for (const auto& other : realmViewers[realmIndex(zone.realm)]) {
                    if (viewer.sourceToken != 0 && other.sourceToken == viewer.sourceToken) continue;
                    if (std::abs(record.position.x - other.viewportCentre.x) > other.half.x + donorReach ||
                        std::abs(record.position.y - other.viewportCentre.y) > other.half.y + donorReach)
                        continue;
                    // A timed refill behind the other flower's wall cannot wake
                    // for that flower. Keep cold stock reserved for arrivals.
                    if (rolling && terrain.segmentBlocked(other.position, record.position, zone.realm)) continue;
                    anotherView = true;
                    break;
                }
                if(anotherView)continue;
#ifdef FLIX_DENSITY_DIAGNOSTICS
                ++eligible;
#endif
                if(std::min(zone.bounds.right(),visibleRoom.right()+placementFootprint.radius)<=std::max(zone.bounds.x,visibleRoom.x-placementFootprint.radius) ||
                   std::min(zone.bounds.bottom(),visibleRoom.bottom()+placementFootprint.radius)<=std::max(zone.bounds.y,visibleRoom.y-placementFootprint.radius))continue;
                std::array<Vec2,4> localAnchors{};int anchorCount=0;
                for(int n=0;n<zone.refillAnchorCount;++n) {
                    const Vec2 point=zone.refillAnchors[(zone.refillAnchorNext-1-n+4)%4].at;
                    const double dx=std::max({visibleRoom.x-point.x,0.0,point.x-visibleRoom.right()});
                    const double dy=std::max({visibleRoom.y-point.y,0.0,point.y-visibleRoom.bottom()});
                    if(dx*dx+dy*dy<=placementFootprint.radius*placementFootprint.radius && zoneContains(zone.bounds,zone.polygon,point))
                        localAnchors[anchorCount++]=point;
                }
                for(int attempt=0;attempt<8 && viewerAttempts<48 && viewerAttempts-zoneAttemptStart<zoneAttemptBudget;++attempt,++viewerAttempts) {
                    // The last real casualties are known navigable positions.
                    // Try them before broad random sampling misses a corridor.
                    // This still moves only an already-ready existing slot.
                    Vec2 point=attempt<anchorCount?localAnchors[attempt]:Vec2{
                        rng.range(std::max(zone.bounds.x,visibleRoom.x-placementFootprint.radius),std::min(zone.bounds.right(),visibleRoom.right()+placementFootprint.radius)),
                        rng.range(std::max(zone.bounds.y,visibleRoom.y-placementFootprint.radius),std::min(zone.bounds.bottom(),visibleRoom.bottom()+placementFootprint.radius))};
                    Body wallHull{placementFootprint.radius,1};
                    const auto& config=content.mob(record.mobIndex);
                    wallHull.wideMobWallHull=map->biome()=="ant_hell" && (config.id=="baby_fire_ant"||config.id=="worker_fire_ant"||config.id=="soldier_fire_ant"||config.id=="queen_fire_ant");
                    point=terrain.resolveCircle(point,mobWallRadius(&wallHull),zone.realm);
                    const double edgeX=std::max({visibleRoom.x-point.x,0.0,point.x-visibleRoom.right()});
                    const double edgeY=std::max({visibleRoom.y-point.y,0.0,point.y-visibleRoom.bottom()});
#ifdef FLIX_DENSITY_DIAGNOSTICS
                    if(static_cast<int>(nowMillis)%5000<=100 && viewerAttempts==0)std::printf("ROOM_PROBE map=%s t=%.0f zone=%zu mob=%s radius=%.1f room=%.0f,%.0f,%.0f,%.0f point=%.0f,%.0f edge=%d zoneok=%d blocked=%d ray=%d\n",map->id().c_str(),nowMillis,zi,config.id.c_str(),placementFootprint.radius,room.x,room.y,room.w,room.h,point.x,point.y,edgeX*edgeX+edgeY*edgeY>placementFootprint.radius*placementFootprint.radius,zoneContains(zone.bounds,zone.polygon,point),terrain.blocked(point,zone.realm),terrain.segmentBlocked(viewer.position,point,zone.realm));
#endif
                    if(edgeX*edgeX+edgeY*edgeY>placementFootprint.radius*placementFootprint.radius || !zoneContains(zone.bounds,zone.polygon,point) ||
                        terrain.blocked(point,zone.realm) || terrain.segmentBlocked(viewer.position,point,zone.realm)) {
#ifdef FLIX_DENSITY_DIAGNOSTICS
                        ++terrainRejected;
#endif
                        continue;
                    }
                    // Honor species overlays and fixed-tier annotations.
                    const SpawnZone* roster=&zone;
                    for(const auto& region:regions_)if(region.realm==zone.realm&&zoneContains(region.bounds,region.polygon,point)){roster=&region;break;}
                    if(roster->forceRarity>=0)continue;
                    const auto& mob=content.mob(record.mobIndex);
                    const auto inGroup=[&](std::uint16_t group){return std::any_of(mob.groups.begin(),mob.groups.end(),[&](const MobGroupMember& m){return m.group==group&&m.weight>0;});};
                    const bool accepts=roster->resolved.empty()?inGroup(content.mobGroupIndex(map->defaultMobGroup())):
                        std::any_of(roster->resolved.begin(),roster->resolved.end(),[&](const auto& row){return row.weight>0&&(row.mob==record.mobIndex||(row.group!=kInvalidIndex&&inGroup(row.group)));});
                    if(!accepts){
#ifdef FLIX_DENSITY_DIAGNOSTICS
                        ++rosterRejected;
#endif
                        continue;
                    }
                    if(crowdedAt(zone.realm,point,placementFootprint.radius*spawnSpacingScale(config),kMinMobSpawnSpacing,static_cast<std::uint16_t>(zi),recordIndex,&terrain,false))continue;
                    const auto facing=wakingFacing(realmViewers[realmIndex(zone.realm)],zone.realm,point,placementFootprint,120.0,rng);
                    if(!clearNaturalSpawn(zone,realmViewers[realmIndex(zone.realm)],point,placementFootprint.radius,placementFootprint.tail,placementFootprint.tailRadius,facing)){
#ifdef FLIX_DENSITY_DIAGNOSTICS
                        ++safetyRejected;
#endif
                        continue;
                    }
                    roomLiving.query(zone.realm,point,placementFootprint.radius+kMinMobSpawnSpacing,roomHits);
                    bool crowded=false;
                    for(Entity e:roomHits){const auto* body=world.tryGet<Body>(e);const auto* type=world.tryGet<MobType>(e);
                        const double scale=type?spawnSpacingScale(content.mob(type->configIndex)):1.0;
                        const double gap=std::min(placementFootprint.radius*spawnSpacingScale(config),kPreliminarySpawnRadius)+
                            std::min(body?body->radius*scale:0,kPreliminarySpawnRadius)+kMinMobSpawnSpacing;
                        if(distanceSq(world.get<Transform>(e).position,point)<gap*gap &&
                            !terrain.segmentBlocked(point,world.get<Transform>(e).position,zone.realm)){crowded=true;break;}}
                    for(const auto& reserved:reservedPoints) {
                        const double gap=std::min(placementFootprint.radius*spawnSpacingScale(config),kPreliminarySpawnRadius)+reserved.spacingRadius+kMinMobSpawnSpacing;
                        if(reserved.realm==zone.realm && distanceSq(reserved.at,point)<gap*gap){crowded=true;break;}
                    }
                    if(crowded){
#ifdef FLIX_DENSITY_DIAGNOSTICS
                        ++crowdRejected;
#endif
                        continue;
                    }
                    // Keep placement caches consistent until the next census.
                    if (substituteLower) record.rarity = Rarity::Super;
                    record.position=point;record.densityRelocated=true;zone.usesLivePlacement=true;livePlacementRealms_[realmIndex(zone.realm)]=true;
                    latentPlacementGrid_.clear();indexedLatentZones_.assign(zones_.size(),false);
                    reservedPoints.push_back({zone.realm, point,
                        std::min(placementFootprint.radius * spawnSpacingScale(config), kPreliminarySpawnRadius),
                        record.rarity < Rarity::Unique ? placementFootprint.radius : 0.0});
                    ++moved;
                    ++viewerMoves;
                    ++living;
                    ++*visibleLiving;
                    hasLowerTierLiving |= record.rarity < Rarity::Unique;
                    // The second wake still performs full body/player clearance.
                    break;
                }
            }
        }
#ifdef FLIX_DENSITY_DIAGNOSTICS
        if(static_cast<int>(nowMillis)%5000<=100)std::printf("DENSITY_DIAG map=%s at=%.0f,%.0f t=%.0f need=%d eligible=%d attempts=%d terrain=%d roster=%d safety=%d crowd=%d moved=%d\n",map->id().c_str(),viewer.position.x,viewer.position.y,nowMillis,need,eligible,viewerAttempts,terrainRejected,rosterRejected,safetyRejected,crowdRejected,viewerMoves);
#endif
    }
    if(moved>0) {
        latentPlacementGrid_.clear();indexedLatentZones_.assign(zones_.size(),false);
        promoteLatent(world,terrain,content,seers,viewers,rng,nowMillis,false);
    }

}

// ---------------------------------------------------------------------------
// Sustained local gaps that the normal promotion pass could not fill.
void SpawnSystem::refillStarvedPrestigeBand(World& world, const Terrain& terrain,
                                           const ContentRegistry& content, double nowMillis) {
    constexpr double gapMillis = 5000.0;
    constexpr double retryMillis = 1500.0;
    constexpr double perRealmMillis = 12000.0;
    const int ambientBudget = mobCap >= 128 ? mobCap - 128 : mobCap;
    if (!worldMaps || viewers_.empty() || census_.mobs >= ambientBudget) {
        lowTierGaps_.clear();
        return;
    }

    const auto fixedRoom = [&](const Viewer& viewer) {
        Viewer fixed = viewer;
        fixed.half = {kSpawnViewportHalfWidth, kSpawnViewportHalfHeight};
        fixed.viewportCentre = boundedViewportCentre(
            viewer.position, {kViewportWidth, kViewportHeight}, terrain.cameraExtent(viewer.realm));
        return viewerRoom(fixed);
    };
    std::array<int, kMaxRealms> occupants{};
    for (const auto& viewer : viewers_) ++occupants[realmIndex(viewer.realm)];
    const auto isCrowdedPrestige = [&](const Viewer& viewer) {
        if (viewer.identity == NULL_ENTITY || occupants[realmIndex(viewer.realm)] < 2) return false;
        const auto* map = worldMaps->forRealm(viewer.realm);
        return map && map->prestigeLevel() == 2;
    };
    if (std::none_of(viewers_.begin(), viewers_.end(), isCrowdedPrestige)) {
        lowTierGaps_.clear();
        return;
    }
    std::array<bool, kMaxRealms> crowdedRealms{};
    for (const auto& viewer : viewers_) {
        if (isCrowdedPrestige(viewer)) crowdedRealms[realmIndex(viewer.realm)] = true;
    }
    std::array<std::vector<Entity>, kMaxRealms> lowerBodies;
    DensityRoots roots{world};
    Query<MobTag, Transform, Body, MobType> living{world};
    living.without<Dead, Pet>();
    living.each([&](Entity entity, MobTag&, Transform& at, Body&, MobType&) {
        if (!crowdedRealms[realmIndex(at.realm)]) return;
        const auto* rootType = world.tryGet<MobType>(roots.get(entity));
        if (rootType && rootType->rarity < Rarity::Unique &&
            rootType->configIndex < content.mobCount() &&
            !content.mob(rootType->configIndex).neverAmbient)
            lowerBodies[realmIndex(at.realm)].push_back(entity);
    });
    // Each connected group has one stable observer. Marginal viewers do not
    // create independent refill demand or change the anchor as they move.
    constexpr double sharedRoomDistance = 1500.0;
    std::vector<const Viewer*> neighbors;
    for (const auto& viewer : viewers_) {
        if (isCrowdedPrestige(viewer)) neighbors.push_back(&viewer);
    }
    std::vector<const Viewer*> observers;
    std::vector<const Viewer*> group;
    for (auto& seed : neighbors) {
        if (!seed) continue;
        group.clear();
        group.push_back(seed);
        seed = nullptr;
        for (std::size_t i = 0; i < group.size(); ++i) {
            for (auto& neighbor : neighbors) {
                if (neighbor && neighbor->realm == group[i]->realm &&
                    distanceSq(neighbor->position, group[i]->position) <=
                        sharedRoomDistance * sharedRoomDistance) {
                    group.push_back(neighbor);
                    neighbor = nullptr;
                }
            }
        }
        if (group.size() < 2) continue;
        observers.push_back(*std::min_element(group.begin(), group.end(),
            [](const Viewer* left, const Viewer* right) {
                return left->identity < right->identity;
            }));
    }

    const auto hasLower = [&](const Viewer& viewer) {
        const Rect room = fixedRoom(viewer);
        const auto& bodies = lowerBodies[realmIndex(viewer.realm)];
        return std::any_of(bodies.begin(), bodies.end(), [&](Entity entity) {
            const auto& at = world.get<Transform>(entity);
            return bodyOverlapsRoom(room, at.position, world.get<Body>(entity).radius) &&
                !terrain.segmentBlocked(viewer.position, at.position, at.realm);
        });
    };
    std::vector<const Viewer*> healthy;
    for (const auto& viewer : viewers_) {
        if (isCrowdedPrestige(viewer) && hasLower(viewer)) healthy.push_back(&viewer);
    }
    const Viewer* starved = nullptr;
    double oldest = nowMillis;
    for (const Viewer* observer : observers) {
        const auto& viewer = *observer;
        if (std::find(healthy.begin(), healthy.end(), observer) != healthy.end()) {
            lowTierGaps_.erase(viewer.identity);
            continue;
        }
        auto& gap = lowTierGaps_.try_emplace(viewer.identity,
            LowTierGap{viewer.position, viewer.realm, nowMillis, nowMillis}).first->second;
        if (gap.realm != viewer.realm || distanceSq(gap.anchor, viewer.position) > 350.0 * 350.0)
            gap = {viewer.position, viewer.realm, nowMillis, nowMillis};
        gap.lastSeen = nowMillis;
        const bool neighborsHaveStock = std::any_of(healthy.begin(), healthy.end(),
            [&](const Viewer* other) {
                return other->realm == viewer.realm &&
                    distanceSq(other->position, viewer.position) <= sharedRoomDistance * sharedRoomDistance;
            });
        // If an overlapping room still has stock, let the ordinary path settle
        // through multiple cooldown intervals before correcting a local gap.
        const double wait = neighborsHaveStock ? 20000.0 : gapMillis;
        if (nowMillis - gap.since >= wait && gap.since < oldest &&
            nowMillis >= nextSparseRefill_[realmIndex(viewer.realm)]) {
            starved = &viewer;
            oldest = gap.since;
        }
    }
    for (auto it = lowTierGaps_.begin(); it != lowTierGaps_.end();) {
        if (it->second.lastSeen != nowMillis) it = lowTierGaps_.erase(it);
        else ++it;
    }
    if (!starved) return;
    const Realm realm = starved->realm;
    if (inactiveLayoutRealms_[realmIndex(realm)] || inactiveRiftRealms_[realmIndex(realm)]) return;
    auto& next = nextSparseRefill_[realmIndex(realm)];
    next = nowMillis + retryMillis;
    const Rect room = fixedRoom(*starved);
    ++census_.sparseProbeTotal;

    // Only transfer already-ready stock in the same band. No retry can
    // change a record's tier, spawn delay, or ownership.
    int examined = 0;
    for (std::size_t zoneIndex = 0; zoneIndex < zones_.size() && examined < 3; ++zoneIndex) {
        auto& zone = zones_[zoneIndex];
        if (zone.realm != realm || zone.singular || zone.forceRarity >= 0 ||
            !zoneContains(zone.bounds, zone.polygon, starved->position)) continue;
        for (std::size_t i = 0; i < zone.latent.size() && examined < 3; ++i) {
            const auto record = zone.latent[i];
            if (record.managedPopulation || record.readyMillis > nowMillis ||
                record.rarity >= Rarity::Unique || record.mobIndex >= content.mobCount()) continue;
            const auto& mob = content.mob(record.mobIndex);
            if (mob.neverAmbient || mob.intangible || mob.segmentCount || mob.ambush.present ||
                mob.dungeon.present || mob.periodicSpawn.present || !mob.spawnWaves.empty() ||
                !mob.hurtSpawn.empty() || mob.id == "ghost") continue;

            // Keep a donor that could already wake for another flower.
            const auto largest = spawnFootprint(content, record.mobIndex, record.rarity);
            if (clearOfEveryLap(viewers_, realm, record.position, largest, std::nullopt, kMinSpawnDistance) &&
                std::any_of(viewers_.begin(), viewers_.end(), [&](const Viewer& viewer) {
                    return viewer.realm == realm &&
                        bodyOverlapsRoom(fixedRoom(viewer), record.position, largest.radius) &&
                        !terrain.segmentBlocked(viewer.position, record.position, realm);
                })) continue;

            Rng spawnRng = sparseRefillRng_;
            const auto stats = content.mobStats(record.mobIndex, record.rarity);
            const double radius = stats.radius * rollSizeJitter(mob, record.rarity, sparseRefillRng_) * kSpawnedMobBodyScale;
            const Body hull{radius, stats.mass};
            const double wallRadius = mobWallRadius(&hull);
            const double left = std::max(zone.bounds.left(), room.left() - radius);
            const double right = std::min(zone.bounds.right(), room.right() + radius);
            const double top = std::max(zone.bounds.top(), room.top() - radius);
            const double bottom = std::min(zone.bounds.bottom(), room.bottom() + radius);
            if (left >= right || top >= bottom) continue;
            ++examined;
            ++census_.sparseMatureCandidatesTotal;
            for (int attempt = 0; attempt < 4096; ++attempt) {
                const Vec2 sample{sparseRefillRng_.range(left, right), sparseRefillRng_.range(top, bottom)};
                if (!bodyOverlapsRoom(room, sample, radius) ||
                    nearAnyPlayer(viewers_, realm, sample, kMinSpawnDistance, radius) ||
                    !zoneContains(zone.bounds, zone.polygon, sample) ||
                    inBorderBand(sample, terrain.realmExtent(realm)) || terrain.blocked(sample, realm) ||
                    terrain.segmentBlocked(starved->position, sample, realm)) continue;
                const auto resolved = terrain.resolveCircle(sample, wallRadius, realm);
                if (distanceSq(sample, resolved) > 0.0 ||
                    nearTrainingDummy(realm, sample, radius) ||
                    crowdedAt(realm, sample, radius * spawnSpacingScale(mob), kMinMobSpawnSpacing,
                              static_cast<std::uint16_t>(zoneIndex), i, &terrain)) continue;

                const SpawnZone* roster = &zone;
                for (const auto& region : regions_) {
                    if (region.realm == realm && zoneContains(region.bounds, region.polygon, sample)) {
                        roster = &region;
                        break;
                    }
                }
                const auto inGroup = [&](std::uint16_t group) {
                    return group != kInvalidIndex &&
                        std::any_of(mob.groups.begin(), mob.groups.end(), [&](const MobGroupMember& m) {
                            return m.group == group && m.weight > 0.0;
                        });
                };
                const auto* map = worldMaps->forRealm(realm);
                const bool accepts = roster->resolved.empty()
                    ? inGroup(content.mobGroupIndex(map->defaultMobGroup()))
                    : std::any_of(roster->resolved.begin(), roster->resolved.end(), [&](const auto& row) {
                        return row.weight > 0.0 && (row.mob == record.mobIndex || inGroup(row.group));
                    });
                if (roster->forceRarity >= 0 || !accepts) continue;
                const auto cell = std::make_tuple(realm, static_cast<int>(std::floor(sample.x / 900.0)),
                    static_cast<int>(std::floor(sample.y / 900.0)));
                const bool rolling = record.densityRelocated || (record.readyMillis > 0.0 && !record.previouslyAwake);
                if (rolling) {
                    const auto gate = refillAdmissions_.find(cell);
                    if (gate != refillAdmissions_.end() && nowMillis < gate->second.untilMillis &&
                        gate->second.admitted >= 2) continue;
                }
                const Entity spawned = spawnMobAt(world, terrain, content, record.mobIndex, record.rarity,
                    sample, realm, nowMillis, spawnRng, 0, static_cast<std::uint16_t>(zoneIndex),
                    std::nullopt, true, !record.ghostAlreadyAnnounced);
                if (spawned == NULL_ENTITY) continue;

                if (rolling) {
                    auto& gate = refillAdmissions_[cell];
                    if (nowMillis >= gate.untilMillis) gate = {nowMillis + 500.0, 0};
                    ++gate.admitted;
                }
                world.get<AmbientMob>(spawned).replacedSpecies = record.replacedSpecies;
                ++census_.sparsePlacedTotal;
                ++zone.liveMobs;
                ++census_.promotedTotal;
                noteLiveBandMob(world, spawned, static_cast<std::uint16_t>(zoneIndex), content);
                zone.latent[i] = zone.latent.back();
                zone.latent.pop_back();
                latentPlacementGrid_.clear();
                indexedLatentZones_.assign(zones_.size(), false);
                next = nowMillis + perRealmMillis;
                return;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Reading the latent population back
// ---------------------------------------------------------------------------

int SpawnSystem::latentCount() const {
    std::size_t total = 0;
    for (const SpawnZone& zone : zones_) total += zone.latent.size();
    return static_cast<int>(total);
}

void SpawnSystem::latentSites(Realm realm, std::vector<LatentSite>& out) const {
    for (const SpawnZone& zone : zones_) {
        if (zone.realm != realm) continue;
        for (const LatentMob& record : zone.latent) {
            out.push_back(
                LatentSite{record.position, record.mobIndex, record.rarity, zone.difficulty});
        }
    }
}

// ---------------------------------------------------------------------------
// Announcements
// ---------------------------------------------------------------------------

void SpawnSystem::announceIfNotable(const ContentRegistry& content, Entity entity,
                                    std::uint16_t mobIndex, Rarity rarity, Vec2 position,
                                    Realm realm) {
    if (mobIndex >= content.mobCount() || content.mob(mobIndex).neverAmbient) return;
    const bool ghost = content.mob(mobIndex).id == "ghost";
    const Rarity threshold = ghost ? kAnnouncedGhostRarity : kAnnouncedRarity;
    if (rarityIndex(rarity) < rarityIndex(threshold)) return;
    // A prestige twin's supers are its bands' everyday stock, not events: the
    // server would announce one every few seconds.
    if (const auto* map = worldMaps ? worldMaps->forRealm(realm) : nullptr;
        map && !ghost && (map->prestigeLevel() == 2 || (map->prestige() && rarity != Rarity::Unique))) return;
    // A permanent fixture is not an event. A dummy reaches this through a band
    // that names it outright, up to unique, or the console's spawn, and
    // announcing those would put a line in chat for a post nobody has to
    // fight. (The shipped DPS row is the jungle's map NPCs, which NpcSystem
    // places and which never come through here.)
    // Oldest first, so a server that never drains this keeps the announcements
    // somebody might still care about instead of the ones from an hour ago --
    // but lowest tier first of all. A band restocking in bulk (after a
    // killall, say) places dozens of supers in one pass, and the unique its
    // biome's clock made out of one of them must not be pushed out by the
    // rest.
    if (bossSpawns.size() >= kMaxPendingBossSpawns) {
        const auto victim = std::min_element(
            bossSpawns.begin(), bossSpawns.end(), [](const BossSpawn& a, const BossSpawn& b) {
                return rarityIndex(a.rarity) < rarityIndex(b.rarity);
            });
        if (rarityIndex(rarity) < rarityIndex(victim->rarity)) return;
        bossSpawns.erase(victim);
    }
    bossSpawns.push_back(BossSpawn{entity, mobIndex, rarity, position, realm});
}

} // namespace flix
