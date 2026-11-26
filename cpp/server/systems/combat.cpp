#include "shared/game/computer_set.h"
#include "shared/game/body_contact.h"
#include "server/systems/combat.h"
#include "server/combat_talents.h"
#include "shared/game/domino.h"
#include "server/systems/flower_mobs.h"
#include "server/hyper_petal_loadout.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "server/loot_eligibility.h"
#include "server/training_safety.h"
#include "server/systems/movement.h"
#include "server/systems/petals.h"
#include "server/systems/spawning.h"
#include "server/workers.h"
#include "shared/game/npc.h"
#include "shared/game/realm.h"

namespace flix {

namespace {

// Visit samples are shorter than most tick phases. Read the WASM monotonic
// clock directly instead of going through WASI's integer time conversion.
double contactSampleMillis() {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

/// How far a common colony mob reaches to link with another (the psionic
/// connection), centre to centre. It grows with rarity exactly as a mob's
/// body does (kMobSizeScale, relative to common), and NOT with the config's
/// `size`: a baby termite connects as far as a soldier of its tier.
constexpr double kColonyLinkRange = 100.0;

double colonyReach(const World& world, Entity e) {
    const MobType* type = world.tryGet<MobType>(e);
    const Rarity rarity = type != nullptr ? type->rarity : Rarity::Common;
    const std::size_t tier = static_cast<std::size_t>(ladderIndex(clampRarity(rarityIndex(rarity))));
    return kColonyLinkRange * kMobSizeScale[tier] / kMobSizeScale[0];
}

/// The entity one step up the ownership chain, or NULL_ENTITY at the top.
///
/// A projectile prefers creditTo over owner because a pet's shot is fired by
/// the pet but answerable to the player; every other link has one owner.
Entity ownerOf(const World& world, Entity e) {
    if (const Projectile* projectile = world.tryGet<Projectile>(e)) {
        return projectile->creditTo != NULL_ENTITY ? projectile->creditTo : projectile->owner;
    }
    if (const PetalInstance* petal = world.tryGet<PetalInstance>(e)) return petal->owner;
    if (const Pet* pet = world.tryGet<Pet>(e)) return pet->owner;
    if (const MobRingPetal* petal = world.tryGet<MobRingPetal>(e)) return petal->mob;
    if (const GroundEffect* effect = world.tryGet<GroundEffect>(e)) return effect->owner;
    return NULL_ENTITY;
}

bool riftPortalProtected(const World& world, Entity player, double nowMillis) {
    const auto* rift = world.tryGet<RiftPlayer>(player);
    return rift && rift->portalProtected(nowMillis);
}

bool woreCorruption(const World& world, Entity player) {
    if (const auto* visuals = world.tryGet<PlayerVisuals>(player)) return visuals->equippedCorruption;
    return false;
}

bool isWildGambler(const World& world, Entity entity) {
    const auto* mob = world.tryGet<MobType>(entity);
    return mob && !world.has<Pet>(entity) && content().mob(mob->configIndex).id == "gambler";
}

bool isGamblerAttack(const World& world, Entity source) {
    for (int hop = 0; hop < kMaxOwnerHops && source != NULL_ENTITY; ++hop) {
        if (isWildGambler(world, source)) return true;
        source = ownerOf(world, source);
    }
    return false;
}

struct TeamInfo {
    Team team = Team::Neutral;
    bool friendlyFire = false;
    bool known = false;
};

/// The side an entity fights on, walking up to whoever owns it.
///
/// A petal carries no Faction of its own: it is on its flower's side by
/// definition, and giving each of the eight a copy would be eight more fields
/// to keep in step with a player joining a PvP arena.
TeamInfo teamOf(const World& world, Entity e) {
    Entity current = e;
    for (int hop = 0; hop < kMaxOwnerHops && current != NULL_ENTITY; ++hop) {
        if (const Faction* faction = world.tryGet<Faction>(current)) {
            return {faction->team, faction->friendlyFireEnabled, true};
        }
        // A wild body with missing faction data is still a hostile mob,
        // not an environmental hazard allowed to damage every side.
        if(world.has<MobTag>(current) && !world.has<Pet>(current))
            return {Team::Hostiles,false,true};
        current = ownerOf(world, current);
    }
    return {};
}

bool permitsPeacefulShots(const World& world, Entity player) {
    const auto* visuals = world.tryGet<PlayerVisuals>(player);
    return !visuals || !(visuals->corrupted || visuals->equippedCorruption ||
                        visuals->nearbyCorruption || visuals->hyperCorruptionExposed);
}

bool peacefulPlayerSide(const World& world, Entity entity, Entity player) {
    const auto side = teamOf(world, entity);
    return side.known && side.team == Team::Players && !side.friendlyFire &&
           permitsPeacefulShots(world, player);
}

/// Whether a flower has been turned hostile to every other flower.
///
/// Corruption is the Flower petal's 5% break outcome and it is symmetric on
/// purpose: a corrupted flower may attack anyone anywhere in the world and
/// anyone may fight it back, because a one-way version would leave it
/// untouchable by everything except mobs.
bool isCorrupted(const World& world, Entity player) {
    if (player == NULL_ENTITY) return false;
    const PlayerVisuals* visuals = world.tryGet<PlayerVisuals>(player);
    return visuals != nullptr && visuals->corrupted;
}

bool equippedCorruptionAllowsPvp(const World& world, Entity a, Entity b) {
    const auto* av = world.tryGet<PlayerVisuals>(a);
    const auto* bv = world.tryGet<PlayerVisuals>(b);
    if ((!av || !(av->equippedCorruption || av->nearbyCorruption)) &&
        (!bv || !(bv->equippedCorruption || bv->nearbyCorruption || bv->hyperCorruptionExposed))) return false;
    const auto* ap = world.tryGet<PlayerProgress>(a);
    const auto* bp = world.tryGet<PlayerProgress>(b);
    return ap && bp && ap->level >= 20 && bp->level >= 20;
}

/// Whether two flowers are one person: the two bodies a splitter gives one
/// connection (server/splitter.cpp). False for anything that is not a flower
/// with an account, and for two bots, whose connection is 0.
bool oneConnection(const World& world, Entity a, Entity b) {
    const PlayerAccount* first = world.tryGet<PlayerAccount>(a);
    const PlayerAccount* second = world.tryGet<PlayerAccount>(b);
    return first != nullptr && second != nullptr && first->connection != 0 &&
           first->connection == second->connection;
}

/// Whether `e` is a petal that ignores mobs (PetalConfig::ignoresMobs).
bool ignoresMobs(const World& world, Entity e) {
    const PetalInstance* petal = world.tryGet<PetalInstance>(e);
    return petal != nullptr && petal->ignoresMobs;
}

/// Whether `e` is a mob's doing: a mob's or an NPC's body (a pet's too), a
/// seat on a mob's ring, or a shot or field one of them put out. Walks the
/// FIRER rather than the credit -- a pet's shot answers to its flower, but a
/// mob fired it.
bool mobsDoing(const World& world, Entity e) {
    Entity current = e;
    for (int hop = 0; hop < kMaxOwnerHops && current != NULL_ENTITY; ++hop) {
        if (!world.isAlive(current)) return false;
        if (world.has<MobTag>(current) || world.has<NpcTag>(current) ||
            world.has<MobRingPetal>(current)) {
            return true;
        }
        if (world.has<PlayerTag>(current)) return false;
        const Projectile* shot = world.tryGet<Projectile>(current);
        current = shot != nullptr ? shot->owner : ownerOf(world, current);
    }
    return false;
}

bool isWildMob(const World& world, Entity entity) {
    return world.has<MobTag>(entity) && !world.has<Pet>(entity);
}

/// Whether one flower's petals may swing at another's.
///
/// The two ways are not the same shape. BOTH duellists must be in the arena --
/// friendlyFireEnabled is the flag the arena sets -- while EITHER side being
/// corrupted is enough. Nothing here looks at distance: the caller only
/// reaches it after an overlap test, and the arena sits far enough from the
/// world that no pair spanning the two could ever touch.
void shortenUndeadWithScar(World& world, Entity victim, Rarity rarity, double nowMillis) {
    auto* undead = world.tryGet<Undead>(victim);
    if (!undead || undead->shortenedByScar || undead->untilMillis <= nowMillis) return;
    const auto* mob = world.tryGet<MobType>(victim);
    const int targetTier = mob ? rarityIndex(mob->rarity) : undead->bandageTier;
    if (petalTier(rarity) < targetTier) return;
    undead->untilMillis -= 0.25 * (undead->untilMillis - nowMillis);
    undead->shortenedByScar = true;
}

bool canPetalsDamagePlayer(const World& world, Entity attacker, Entity victim) {
    if (world.has<WavePlayer>(attacker) || world.has<WavePlayer>(victim)) return false;
    if (isCorrupted(world, attacker) || isCorrupted(world, victim) ||
        equippedCorruptionAllowsPvp(world, attacker, victim)) return true;
    const Faction* attackerFaction = world.tryGet<Faction>(attacker);
    const Faction* victimFaction = world.tryGet<Faction>(victim);
    return attackerFaction != nullptr && attackerFaction->friendlyFireEnabled &&
           victimFaction != nullptr && victimFaction->friendlyFireEnabled;
}

/// The actor behind a hit: a shot's mob, a petal's flower, a pet's owner.
///
/// Damage arrives from the thing that touched the victim, but "who killed me"
/// has to name something the player recognises -- a projectile handle is
/// already gone by the time the Died message is written.
Entity attributedSource(const World& world, Entity source) {
    Entity current = source;
    for (int hop = 0; hop < kMaxOwnerHops; ++hop) {
        const Entity owner = ownerOf(world, current);
        if (owner == NULL_ENTITY || !world.isAlive(owner)) return current;
        current = owner;
    }
    return current;
}

/// The tier a slow is landing against. Players have no rarity of their own and
/// count as common, so stallPower() neither helps nor hinders a slow on them.
Rarity rarityOf(const World& world, Entity e) {
    if (const MobType* type = world.tryGet<MobType>(e)) return type->rarity;
    if (const Replicated* replicated = world.tryGet<Replicated>(e)) return replicated->rarity;
    return Rarity::Common;
}

/// The tier of whatever LANDED a hit, which is the tier a salt measures
/// itself against: the mob or pet that bit, or the shot, petal, field or ring
/// seed that touched -- each made at a tier of its own, which for a mob's shot
/// or seed is the mob's. Read off the thing's own component rather than its
/// Replicated copy, which a body that is never sent does not have.
Rarity hitRarity(const World& world, Entity source) {
    if (const MobType* type = world.tryGet<MobType>(source)) return type->rarity;
    if (const Projectile* shot = world.tryGet<Projectile>(source)) return shot->rarity;
    if (const PetalInstance* petal = world.tryGet<PetalInstance>(source)) return petal->rarity;
    if (const GroundEffect* effect = world.tryGet<GroundEffect>(source)) return effect->rarity;
    if (const MobRingPetal* seed = world.tryGet<MobRingPetal>(source)) {
        if (const MobType* type = world.tryGet<MobType>(seed->mob)) return type->rarity;
    }
    return rarityOf(world, source);
}

/// Whether a projectile should test against this entity at all. Drops and
/// ground effects are in the broadphase because they have bodies, not because
/// they are targets.
///
/// Other SHOTS are targets, and deliberately so: two of them meeting is the
/// bullet-vs-bullet rule, and canDamage() is what keeps a volley from
/// detonating against itself -- both shots resolve to the same player or the
/// same team and are refused before any damage is exchanged.
bool isShootable(const World& world, Entity e) {
    return !world.has<DropTag>(e) && (!world.has<GroundEffectTag>(e) || world.has<Health>(e));
}

/// What being hit COSTS the shot that hit it.
///
/// Symmetric with the damage flowing the other way: a shot pays the victim's
/// body damage, and a victim that happens to be another shot charges its own
/// damage stat. That is the whole of penetration -- nothing counts hits, the
/// pool simply runs out.
///
/// A PETAL charges its damage stat, the same way another shot does. A ring
/// eating an incoming volley is a real defence and it has to survive this
/// change: a basic petal's ten points is a common missile's whole pool, so the
/// ring still stops what it always stopped, and only a shot fat enough to
/// outlast it now gets through. It is read off the config rather than off a
/// component because a petal carries no ContactDamage -- the melee pass
/// resolves a ring's damage from the registry too.
///
/// A FLOWER is the exception and returns infinity. It has no body damage to
/// charge with, and post-hit invulnerability means a shot that survived would
/// sit inside the victim until its range expired rather than landing again.
/// Mob shots have always been consumed by the flower they hit, and they still
/// are.
double bodyDamageOf(const World& world, const ContentRegistry& content, Entity victim) {
    if (world.has<PlayerTag>(victim)) return std::numeric_limits<double>::infinity();
    if (const Projectile* shot = world.tryGet<Projectile>(victim)) return shot->damage;
    if (const PetalInstance* petal = world.tryGet<PetalInstance>(victim)) {
        const double damage = content.petalStats(petal->configIndex, petal->rarity).damage;
        return damage > 0.0 ? damage : kProjectileDefaultBodyDamage;
    }
    if (const ContactDamage* contact = world.tryGet<ContactDamage>(victim)) return contact->amount;
    return kProjectileDefaultBodyDamage;
}

/// Fold the live poison stacks back into the scalar summary on Afflictions.
///
/// The stacks are what tick; the summary exists because replication asks
/// Afflictions::poisoned() whether to light the poisoned state bit, and
/// because "how fast is this mob losing health" is worth having in one place.
/// Rates ADD, exactly as poisonStackSystem applies every live stack in turn.
void summarisePoison(Afflictions& afflictions, double nowMillis) {
    double total = 0;
    double until = 0;
    double strongest = 0;
    Entity owner = NULL_ENTITY;
    for (const PoisonStack& stack : afflictions.poisonStacks) {
        if (stack.untilMillis <= nowMillis || stack.perSecond <= 0.0) continue;
        total += stack.perSecond;
        until = std::max(until, stack.untilMillis);
        // Named for the client and for a kill notice, both of which want one
        // culprit; the ledger credit is per stack and does not go through here.
        if (stack.perSecond > strongest) {
            strongest = stack.perSecond;
            owner = stack.source;
        }
    }
    afflictions.poisonPerSecond = total;
    afflictions.poisonUntilMillis = until;
    afflictions.poisonSource = owner;
}

/// The victim radius a ground field reaches with, which is deliberately not
/// the same number for every kind.
///
/// A pollen puff tests the mob's CONFIG radius for its tier -- the reference
/// kept that from the legacy loop and says so at ecsRuntime.ts:745 -- so a mob
/// that rolled a big body is no easier to dust than one that rolled a small
/// one. A web and a uranium cloud test the body that is actually there.
double fieldTargetRadius(const World& world, const ContentRegistry& content, Entity victim,
                         GroundEffectKind kind, const Body* body) {
    if (kind == GroundEffectKind::Poison) {
        if (const MobType* type = world.tryGet<MobType>(victim)) {
            return content.mobStats(type->configIndex, type->rarity).radius;
        }
    }
    return body != nullptr ? body->radius : 0.0;
}

/// What a petal pays to land a hit: the mob's own `damage` stat, which is
/// exactly what its body would have dealt anyway. One, not zero, for a victim
/// carrying no stats at all -- the reference's explicit fallback.
double contactDamageOf(const World& world, Entity mob) {
    const ContactDamage* contact = world.tryGet<ContactDamage>(mob);
    return contact != nullptr ? contact->amount : 1.0;
}

/// What one strike from this mob takes off.
///
/// The spec's own number when it states one, and otherwise the mob's tier-
/// scaled `damage` -- which is what lets a mythic jellyfish shock harder than a
/// common one without a second rarity ladder in mobs.json.
double lightningDamageOf(const MobConfig& config, const MobStats& stats) {
    return config.lightning.damage > 0.0 ? config.lightning.damage : stats.damage;
}

/// The gap between two strikes from this mob: the spec's, else the mob's own
/// attack cadence, else the default. Deliberately NOT rarity-scaled -- neither
/// is the `cooldown` a volley reads.
double lightningCooldownOf(const MobConfig& config) {
    if (config.lightning.cooldownMillis > 0.0) return config.lightning.cooldownMillis;
    if (config.cooldownMillis > 0.0) return config.cooldownMillis;
    return kDefaultLightningCooldownMillis;
}

/// Write a swing into the victim's contributor ledger.
///
/// The number SWUNG, never the number that fitted in the health that was left.
/// trackDamage() stores what the caller computed, and the ledger is what ranks
/// contributors for the mob's four to twenty-five loot slots -- clamping to the
/// remainder would rank whoever lands the killing blow by the sliver they took
/// rather than by the hit they threw, and sort them under the chip damage that
/// softened it up.
void creditSwing(World& world, Entity victim, Entity source, double amount,
                 bool landedDirectDamage = false) {
    Bounty* bounty = world.tryGet<Bounty>(victim);
    if (bounty == nullptr) return;
    Entity credited = CombatSystem::creditedPlayer(world, source);
    // A petal the Champion's Crown lent is entered under the crown's wearer
    // (PetalInstance::creditTo) -- the ledger alone: who the petal may hit,
    // and whose number rises off the mob, are still its own flower's.
    bool arcticEmeraldHit = false;
    if (const PetalInstance* petal = world.tryGet<PetalInstance>(source)) {
        arcticEmeraldHit = petal->increasesMobExperience && landedDirectDamage &&
            isWildMob(world, victim) && !world.has<NpcTag>(victim) && !world.has<WaveEnemy>(victim);
        if (petal->creditTo != NULL_ENTITY && world.isAlive(petal->creditTo) &&
            world.has<PlayerTag>(petal->creditTo)) {
            credited = petal->creditTo;
        }
    }
    if (credited == NULL_ENTITY) return;
    const auto* account = world.tryGet<PlayerAccount>(credited);
    const net::ConnectionId owner = account ? account->connection : 0;
    const auto credit = [&](Entity target, Bounty& ledger, double damage) {
        const auto* mob = world.tryGet<MobType>(target);
        const auto* location = world.tryGet<Transform>(target);
        const bool worldBoss = mob && (mob->rarity == Rarity::Apex || mob->rarity == Rarity::Ascendant) &&
            location && isWorldRealm(location->realm) &&
            isWildMob(world, target) && !world.has<NpcTag>(target) && !world.has<WaveEnemy>(target);
        const std::string_view accountId = owner != 0 && worldBoss
            ? std::string_view(account->userId) : std::string_view{};
        ledger.credit(credited, damage, owner, accountId, target == victim && arcticEmeraldHit);
    };
    credit(victim, *bounty, amount);

    // A swing inside a dungeon is a swing on the nest outside it, by the rule
    // an ambush nest's brood follows below: the nest cannot be hit, and its
    // ledger is what decides who it pays when its dungeon is cleared.
    if (const DungeonDweller* dweller = world.tryGet<DungeonDweller>(victim)) {
        const Entity nest = dweller->entrance;
        if (!world.isAlive(nest)) return;
        const DungeonEntrance* entrance = world.tryGet<DungeonEntrance>(nest);
        const Health* nestHealth = world.tryGet<Health>(nest);
        Bounty* nestBounty = world.tryGet<Bounty>(nest);
        if (entrance == nullptr || nestHealth == nullptr || nestBounty == nullptr ||
            !(entrance->broodHealth > 0.0)) {
            return;
        }
        credit(nest, *nestBounty, amount * nestHealth->max / entrance->broodHealth);
        return;
    }

    // A swing on an ambush nest's brood is a swing on the nest: the nest itself
    // cannot be hit, and its ledger is what decides who it pays out to when it
    // falls with the last of them. Scaled so the whole brood is worth the
    // nest's full health -- the ledger's 1% loot floor is a share of THAT, and
    // a super hole's ultra-capped ants would otherwise never reach it.
    //
    // A nest that fell earlier this tick still takes the credit, as any corpse
    // does: its drops are not rolled until the loot pass.
    const HoleTether* tether = world.tryGet<HoleTether>(victim);
    if (tether == nullptr) return;
    const Entity nest = tether->hole;
    if (!world.isAlive(nest)) return;
    const AmbushNest* ambush = world.tryGet<AmbushNest>(nest);
    const Health* nestHealth = world.tryGet<Health>(nest);
    Bounty* nestBounty = world.tryGet<Bounty>(nest);
    if (ambush == nullptr || nestHealth == nullptr || nestBounty == nullptr ||
        !(ambush->broodHealth > 0.0)) {
        return;
    }
    credit(nest, *nestBounty, amount * nestHealth->max / ambush->broodHealth);
}

/// Which segment of a shared-health chain holds the pool: the one at the FRONT
/// of whatever is still joined to `e`.
///
/// A leech is one animal, so its health has to be one number, one ledger and
/// one death -- which means one entity has to own all three. The front segment
/// is that entity because it is already the chain's root everywhere else: the
/// AI steers it, the followers trace it, and repairChains() promotes whatever
/// ends up at the front when a leader is lost. Resolving by WALKING rather than
/// by reading `chainHead` is what keeps a half that outlived its head a whole
/// animal instead of a body with no pool.
///
/// Returns `e` itself for anything that is not a shared chain, which is every
/// mob in the game but the leech.
Entity poolOwner(const World& world, Entity e) {
    const BodySegment* segment = world.tryGet<BodySegment>(e);
    if (segment == nullptr || !segment->sharedHealth) return e;

    Entity owner = e;
    // Bounded by the longest chain that can exist, plus the head. A cycle is
    // cut by the AI pass on the tick after it forms, and until then this walk
    // must end rather than spin the tick.
    for (int hop = 0; hop <= kCentipedeSegmentCount; ++hop) {
        const BodySegment* link = world.tryGet<BodySegment>(owner);
        if (link == nullptr || !link->sharedHealth) return owner;
        const Entity ahead = link->ahead;
        // A leader the world has taken away -- despawned off-screen, or reaped
        // after a tick this one somehow survived -- leaves this segment at the
        // front. A leader marked Dead is NOT skipped: the pool is empty and
        // the hit belongs to the corpse, where canHit() refuses it.
        if (ahead == NULL_ENTITY || !world.isAlive(ahead)) return owner;
        owner = ahead;
    }
    return owner;
}

bool isBurning(const World& world, Entity victim, double nowMillis) {
    const Entity target = poolOwner(world, victim);
    const auto* radiation = world.tryGet<Burn>(target);
    if (radiation && radiation->damage > 0.0 && nowMillis < radiation->untilMillis)
        return true;
    const auto* status = world.tryGet<Afflictions>(target);
    return status &&
        ((status->burnPerSecond > 0.0 && nowMillis < status->burnUntilMillis) ||
         std::any_of(status->burns.begin(), status->burns.end(),
             [nowMillis](const BurnStack& burn) {
                 return burn.perSecond > 0.0 && nowMillis < burn.untilMillis;
             }));
}

/// What a swing lands on `victim`: `damage`, plus a claw's `critDamage` while
/// the victim is still above kClawCritHealthFraction of its health.
///
/// Asked of the ANIMAL, as every status is: a leech's beads all draw on one
/// pool, and whether the bonus is due is a question about that pool. Read
/// before the hit, so the blow that carries a mob below the line is the last
/// one to get it.
double swingDamage(const World& world, double damage, double critDamage, Entity victim) {
    if (!(critDamage > 0.0)) return damage;
    const Health* health = world.tryGet<Health>(poolOwner(world, victim));
    if (health == nullptr || !(health->max > 0.0)) return damage;
    return health->fraction() > kClawCritHealthFraction ? damage + critDamage : damage;
}

/// Fang's heal: `amount` back onto the flower, never past its max, and through
/// the gates every heal in the game passes -- a dandelion's lockout, and a
/// corpse, which only a revive may stand back up.
void stealLife(World& world, Entity player, double amount, double nowMillis) {
    if (!(amount > 0.0) || !world.isAlive(player) || world.has<Dead>(player)) return;
    if (CombatSystem::healingBlocked(world, player, nowMillis)) return;
    Health* health = world.tryGet<Health>(player);
    if (health == nullptr || !health->alive()) return;
    CombatSystem::landHeal(world, player, *health, amount);
}

} // namespace

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

struct CombatSystem::Queries {
    // Copy eligible targets in filing order so peaceful queries scan adjacent records.
    std::vector<FiledShot> nonPeacefulByCell;
    std::vector<std::size_t> nonPeacefulEnds;
    struct ContactPosition {
        Vec2 position{};
        std::size_t firstPossibleShove = SIZE_MAX;
    };
    std::vector<ContactPosition> contactPositions;
    std::vector<std::uint64_t> omittedMovers;
    std::vector<ShotCell> shotSortScratch;
    std::array<bool, kMaxRealms> needsLooseShotBounds{};
    explicit Queries(World& world)
        : afflicted(world), auras(world), contact(world), meleeTargets(world), strikers(world), petals(world),
          projectiles(world), filedShots(world), fields(world), cooldowns(world),
          auraCooldowns(world), colony(world) {
        // A dead flower projects nothing, which is the same guard the
        // reference's pre-movement pass opens with.
        auras.without<Dead>();
        // A corpse stops fighting the instant it is marked, not at the end of
        // the tick when the reaper gets to it -- otherwise a mob killed by the
        // first petal in the ring still gets its contact hit in.
        afflicted.without<Dead>();
        // Petals and projectiles carry their damage in their own config and
        // component; a ContactDamage on either would otherwise be counted a
        // second time by this pass.
        contact.without<PetalInstance, Projectile, Dead>();
        petals.without<Dead>();
        projectiles.without<Dead>();
        fields.without<Dead>();
        strikers.without<Dead>();
        colony.without<Dead>();
    }

    Query<Afflictions, Health> afflicted;
    /// Flowers that might be wearing a raindrop. Every loadout is walked each
    /// tick rather than cached: a slot's petal, tier and broken flag all change
    /// under the ring pipeline, and ten array reads are cheaper than keeping a
    /// second copy of them honest.
    Query<PlayerTag, Loadout, Transform> auras;
    /// Petals are excluded and handled separately: their damage, reload and
    /// riders all come out of the petal config, and reading it once beats
    /// mirroring six numbers onto every petal entity every tick.
    Query<ContactDamage, Transform, Body> contact;
    Query<Transform, Body> meleeTargets;
    /// Mobs that might shock at range. Every mob is walked and the config
    /// decides -- the same trade gatherAuras makes with loadouts: one config
    /// read per mob per tick beats keeping a second copy of the spec honest
    /// through summons, rarity rolls and a hot content reload.
    Query<MobTag, MobType, Transform, Body> strikers;
    Query<PetalInstance, Transform, Body> petals;
    Query<Projectile, Transform, Body, Motion> projectiles;
    /// Every shot with a body, corpses included: the set the server's main
    /// grid leaves out, filed by fileShots() instead. Dead ones are kept for the
    /// same reason the main grid keeps them -- the passes refuse a corpse
    /// themselves, and filing is not the place to decide what they may see.
    Query<ProjectileTag, Transform, Body> filedShots;
    Query<GroundEffect, Transform> fields;
    Query<HitCooldowns> cooldowns;
    Query<AuraCooldowns> auraCooldowns;
    /// Every standing colony mob: who a hit on one of them may be shared with.
    Query<ColonyMember, Transform> colony;
};

CombatSystem::CombatSystem() = default;
CombatSystem::~CombatSystem() = default;

void CombatSystem::bind(World& world) {
    if (boundWorld_ == &world && queries_) return;
    queries_ = std::make_unique<Queries>(world);
    boundWorld_ = &world;
}

// ---------------------------------------------------------------------------
// Faction and credit
// ---------------------------------------------------------------------------

Entity CombatSystem::creditedPlayer(const World& world, Entity source) {
    Entity current = source;
    for (int hop = 0; hop < kMaxOwnerHops && current != NULL_ENTITY; ++hop) {
        if (!world.isAlive(current)) return NULL_ENTITY;
        if (world.has<PlayerTag>(current)) return current;
        current = ownerOf(world, current);
    }
    return NULL_ENTITY;
}

void CombatSystem::mergePracticeDamage(std::vector<PracticeDamage>& rows, const PracticeDamage& hit) {
    const auto match = std::find_if(rows.begin(), rows.end(), [&](const PracticeDamage& row) {
        return row.source == hit.source && row.petal == hit.petal && row.rarity == hit.rarity;
    });
    if (match == rows.end()) rows.push_back(hit);
    else match->amount += hit.amount;
}

std::vector<CombatSystem::PracticeDamage> CombatSystem::practiceDamage(
    Entity player, double nowMillis, Entity dummy) const {
    std::vector<PracticeDamage> result;
    const auto found = practiceDamage_.find(player);
    if (found == practiceDamage_.end() ||
        (dummy != NULL_ENTITY && found->second.dummy != dummy)) return result;
    const auto second = static_cast<std::int64_t>(nowMillis / 1000.0);
    for (const auto& bucket : found->second.buckets) {
        if (bucket.second < 0 || bucket.second > second || second - bucket.second >= kPracticeSeconds) continue;
        for (const auto& hit : bucket.damage) mergePracticeDamage(result, hit);
    }
    std::sort(result.begin(), result.end(), [](const PracticeDamage& a, const PracticeDamage& b) {
        return a.amount > b.amount;
    });
    return result;
}

bool CombatSystem::isPracticeDummy(const World& world, const ContentRegistry& registry, Entity entity) {
    const auto* npc = world.tryGet<Npc>(entity);
    return npc && registry.mob(npc->configIndex).id == "target_dummy";
}

void CombatSystem::recordPracticeDamage(const World& world, Entity dummy, Entity source,
                                        DamageKind kind, double amount, double nowMillis) {
    const auto* npc = world.tryGet<Npc>(dummy);
    const auto* mob = !npc && onPracticeHit ? world.tryGet<MobType>(dummy) : nullptr;
    const auto& registry = content();
    if ((!npc && !mob) || registry.mob(npc ? npc->configIndex : mob->configIndex).id != "target_dummy") return;
    const Entity player = creditedPlayer(world, source);
    if (player == NULL_ENTITY) return;

    PracticeDamage hit;
    hit.amount = amount;
    const auto* shot = world.tryGet<Projectile>(source);
    const auto* summon = world.tryGet<Pet>(shot ? shot->owner : source);
    const auto* lightning = world.tryGet<LightningTarget>(source);
    if (kind == DamageKind::Reflect) {
        hit.source = PracticeSource::Reflect;
    } else if (summon) {
        hit.source = PracticeSource::Summon;
        if (const auto* loadout = world.tryGet<Loadout>(player);
            loadout && summon->slot < kLoadoutActiveSlots) {
            const auto petal = equippedPetal(registry, *loadout, summon->slot);
            hit.petal = petal.configIndex;
            hit.rarity = petal.rarity;
        }
    } else if (const auto* petal = world.tryGet<PetalInstance>(source)) {
        hit = {PracticeSource::Petal, petal->configIndex, petal->rarity, amount};
    } else if (shot && shot->petalConfigIndex != kNoPetal) {
        hit = {PracticeSource::Petal, shot->petalConfigIndex, shot->rarity, amount};
    } else if (lightning && lightning->fromMagicEye) {
        hit = {PracticeSource::Petal, registry.petalIndex("magic_eye"), lightning->rarity, amount};
    } else if (kind == DamageKind::Poison) {
        hit.source = PracticeSource::Poison;
    } else if (kind == DamageKind::Burn) {
        hit.source = PracticeSource::Burn;
    } else if (world.has<GroundEffect>(source)) {
        hit.source = PracticeSource::Field;
    }

    if (onPracticeHit) onPracticeHit(player, dummy, hit, nowMillis);
    if (!npc) return; // Admin-spawned mobs have the live meter only.
    auto& target = practiceDamage_[player];
    if (target.dummy != NULL_ENTITY && target.dummy != dummy) {
        if (world.isAlive(target.dummy) && nowMillis - target.lastHitMillis < 1000.0 * kPracticeSeconds) return;
        target = PracticeTarget{};
    }
    target.dummy = dummy;
    target.lastHitMillis = nowMillis;
    const auto second = static_cast<std::int64_t>(nowMillis / 1000.0);
    auto& bucket = target.buckets[second % kPracticeSeconds];
    if (bucket.second != second) {
        bucket.second = second;
        bucket.damage.clear();
    }
    mergePracticeDamage(bucket.damage, hit);
}

bool CombatSystem::gamblerIgnores(const World& world, Entity mob, Entity target) {
    return isWildGambler(world, mob) && woreCorruption(world, creditedPlayer(world, target));
}

bool CombatSystem::canDamage(const World& world, Entity source, Entity victim) {
    if (const auto* shot = world.tryGet<Projectile>(source); shot && shot->wildOnly && !isWildMob(world, victim)) {
        return false;
    }
    if (const auto* shot = world.tryGet<Projectile>(victim); shot && shot->wildOnly &&
        (creditedPlayer(world, source) != NULL_ENTITY || world.has<Pet>(source))) {
        return false;
    }
    if (victim == NULL_ENTITY || source == victim) return false;

    // Resolving BOTH sides to the player behind them covers every self-harm
    // pair at once -- petal vs own flower, pet vs own petal, shot vs the pet
    // that fired it -- without a rule per pair.
    const Entity sourcePlayer = source != NULL_ENTITY ? creditedPlayer(world, source)
                                                      : NULL_ENTITY;
    const Entity victimPlayer = creditedPlayer(world, victim);
    if (sourcePlayer != NULL_ENTITY && victimPlayer != NULL_ENTITY &&
        (sourcePlayer == victimPlayer || world.has<WavePlayer>(sourcePlayer) ||
         world.has<WavePlayer>(victimPlayer))) return false;
    // ONE PERSON, HOWEVER MANY BODIES. A splitter gives one connection two
    // flowers, and in the PVP ring -- the one place flowers may hurt each
    // other at all -- the pair would otherwise be duellists: the parked half
    // stands there while the steered one walks its ring through it, and the
    // petal killed you with your own petals. Asked of the CONNECTION rather
    // than of any split bookkeeping, because that is the thing the two bodies
    // actually share and because combat has no business knowing what a
    // splitter is. A bot's connection is 0 and two bots are still enemies.
    if (oneConnection(world, sourcePlayer, victimPlayer)) return false;
    // Equip Corruption to become neutral to the Gambler and its ring.
    if ((victimPlayer != NULL_ENTITY && sourcePlayer == NULL_ENTITY &&
         woreCorruption(world, victimPlayer) && isGamblerAttack(world, source)) ||
        (sourcePlayer != NULL_ENTITY && victimPlayer == NULL_ENTITY &&
         woreCorruption(world, sourcePlayer) && isGamblerAttack(world, victim))) return false;
    // A petal that ignores mobs (basil) and anything a mob does pass through
    // each other, either way round. Here rather than in each pass so a body,
    // a ring seat, a shot, a field and a shock are all one rule.
    if ((ignoresMobs(world, source) && mobsDoing(world, victim)) ||
        (ignoresMobs(world, victim) && mobsDoing(world, source))) {
        return false;
    }

    const TeamInfo attacker = teamOf(world, source);
    const TeamInfo defender = teamOf(world, victim);
    // An entity with no side at all is scenery or a hazard: it hurts, and is
    // hurt by, everything. Refusing here instead would silently disarm any
    // spawner that forgot a Faction.
    if (!attacker.known || !defender.known) return true;
    if (attacker.team != defender.team) return true;
    // Friendly-fire permission belongs to player PvP, never wild mobs.
    if (attacker.team == Team::Hostiles) return false;
    // Corruption is checked on the resolved PLAYER and not on the Faction
    // beside it, because a petal is spawned with a copy of its flower's
    // Faction: a flower corrupted after its ring was strung would otherwise
    // keep swinging harmlessly.
    if (isCorrupted(world, sourcePlayer) || isCorrupted(world, victimPlayer) ||
        equippedCorruptionAllowsPvp(world, sourcePlayer, victimPlayer)) return true;
    // Either side being in a PvP region is enough. Requiring both would leave
    // a duellist's pets -- which carry no flag of their own -- unable to fight.
    return attacker.friendlyFire || defender.friendlyFire;
}

Entity CombatSystem::ricochetTarget(World& world, Entity petal, double nowMillis) {
    const auto* instance = world.tryGet<PetalInstance>(petal);
    const auto* at = world.tryGet<Transform>(petal);
    const auto* owner = instance ? world.tryGet<Transform>(instance->owner) : nullptr;
    if (!instance || !at || !owner) return NULL_ENTITY;
    const auto& flight = instance->ricochet;
    const auto valid = [&](Entity mob) {
        const auto* target = world.tryGet<Transform>(mob);
        const auto* faction = world.tryGet<Faction>(mob);
        return target && world.has<MobTag>(mob) && target->realm == at->realm &&
               !world.has<Pet>(mob) && (!faction || faction->team != Team::Players) &&
               !flight.visitedMob(mob) &&
               distanceSq(target->position, owner->position) <= 450.0 * 450.0 &&
               canHit(world, mob, petal, nowMillis);
    };
    if (valid(flight.target)) return flight.target;
    Entity best = NULL_ENTITY;
    const bool longRicochet = hyperTechnique("icosahedron", instance->rarity, instance->mimicked) ==
                             HyperTechnique::LongRicochet;
    const double reach = 180.0 * (longRicochet ? 1.15 : 1.0);
    double nearest = reach * reach;
    Query<MobTag, Transform, Body> mobs{world};
    mobs.each([&](Entity mob, MobTag&, Transform& target, Body&) {
        const double gap = distanceSq(target.position, at->position);
        if (gap < nearest && valid(mob)) {
            nearest = gap;
            best = mob;
        }
    });
    return best;
}

bool CombatSystem::canHit(const World& world, Entity victim, Entity source, double nowMillis,
                          bool ignorePostHitWindow) {
    if (!world.isAlive(victim) || world.has<Dead>(victim)) return false;
    const Health* health = world.tryGet<Health>(victim);
    if (!health) return false;
    // A body already at zero is a corpse this tick has not reaped yet. Hitting
    // it again would mark it Dead a second time and pay its bounty twice.
    if (health->current <= 0.0) return false;
    if (nowMillis < health->invulnerableUntilMillis ||
        (!ignorePostHitWindow && nowMillis < health->hitCooldownUntilMillis)) return false;
    // Kept out of every broadphase already; this is for whatever reaches a
    // victim some other way.
    if (world.has<Intangible>(victim)) return false;
    if (const auto* ghost = ghostBodyOrRing(world, victim)) {
        const auto* visuals = world.tryGet<PlayerVisuals>(creditedPlayer(world, source));
        if (!visuals || !visuals->canSeeGhost(ghost->rarity)) return false;
    }
    if (const auto* ghost = ghostBodyOrRing(world, source)) {
        const auto* visuals = world.tryGet<PlayerVisuals>(creditedPlayer(world, victim));
        if (!visuals || !visuals->canSeeGhost(ghost->rarity)) return false;
    }
    // The players' own NPCs are not something anybody hits: their own side
    // cannot, and nothing on the other side is meant to be fighting them --
    // a mob wandering through the oracle is not an attack on it. An NPC on any
    // other side takes its hits (see applyDamage), which is what a target
    // dummy is for.
    if (world.has<NpcTag>(victim)) {
        const Faction* faction = world.tryGet<Faction>(victim);
        if (faction == nullptr || faction->team == Team::Players) return false;
    }
    return canDamage(world, source, victim);
}

// ---------------------------------------------------------------------------
// The one damage path
// ---------------------------------------------------------------------------

DamageResult CombatSystem::applyDamage(World& world, Entity victim, Entity source,
                                       double amount, double nowMillis, DamageKind kind, bool critical) {
    return applyDamageWithRuby(world, victim, source, amount, nowMillis, kind, NULL_ENTITY,
                               std::nullopt, critical);
}

DamageResult CombatSystem::applyDamageWithRuby(World& world, Entity victim, Entity source,
                                               double amount, double nowMillis, DamageKind kind,
                                               Entity checkedTarget, std::optional<Vec2> impactPoint,
                                               bool critical) {
    if (amount > 0.0 &&
        (riftPortalProtected(world, victim, nowMillis) ||
         riftPortalProtected(world, creditedPlayer(world, source), nowMillis))) {
        DamageResult result;
        result.refused = true;
        return result;
    }
    if(const auto* petal=world.tryGet<PetalInstance>(source); petal &&
       content().petal(petal->configIndex).id=="void_sad" && amount>0) {
        if(const auto* ownerAt=world.tryGet<Transform>(petal->owner)) {
            const auto origin=*ownerAt;int observers=0;
            Query<PlayerTag,Transform>{world}.each([&](Entity e,PlayerTag&,Transform& at){
                if(at.realm!=origin.realm || !isStanding(world,e))return;
                const auto* location=world.tryGet<PlayerLocation>(e);
                Vec2 viewport=location?location->viewport:Vec2{kViewportWidth,kViewportHeight};
                const Vec2 centre=terrain?boundedViewportCentre(at.position,viewport,terrain->cameraExtent(at.realm)):at.position;
                if(std::abs(origin.position.x-centre.x)<=viewport.x*.5 && std::abs(origin.position.y-centre.y)<=viewport.y*.5)++observers;
            });
            amount/=std::max(1,observers);
        }
    }
    // Kitty observes the actual damaging body, including a released shot.
    // The flower itself and every other standing player count as observers.
    if(amount>0 && !resolvingRuby_ && isDirectHit(kind)) {
        const auto* petal=world.tryGet<PetalInstance>(source);
        const auto* shot=world.tryGet<Projectile>(source);
        const Entity owner=petal?petal->owner:shot?shot->owner:NULL_ENTITY;
        const auto* loadout=world.tryGet<Loadout>(owner);
        const auto* at=world.tryGet<Transform>(source);
        if(loadout && at && (petal || (shot && shot->petalConfigIndex!=kNoPetal))) {
            const Rarity rarity=petal?petal->rarity:shot->rarity;
            const auto& registry = content();
            bool eligible=false;
            for(int slot=0;slot<std::min(loadout->activeSlots,kLoadoutActiveSlots);++slot) {
                const auto item = equippedPetal(registry, *loadout, slot);
                if (!item.empty() && registry.petal(item.configIndex).id == "void_kitty" &&
                    petalTier(rarity) <= petalTier(item.rarity)) {
                    eligible = true;
                    break;
                }
            }
            if(eligible) {
                bool watched=false;
                const auto* body=world.tryGet<Body>(source);
                const double radius=body?body->radius:0.0;
                Query<PlayerTag,Transform>{world}.each([&](Entity observer,PlayerTag&,Transform& position){
                    if(watched || position.realm!=at->realm || !isStanding(world,observer))return;
                    const auto* location=world.tryGet<PlayerLocation>(observer);
                    Vec2 viewport=location?location->viewport:Vec2{kViewportWidth,kViewportHeight};
                    if(!(viewport.x>0 && viewport.y>0))viewport={kViewportWidth,kViewportHeight};
                    const Vec2 centre=terrain?boundedViewportCentre(position.position,viewport,terrain->cameraExtent(position.realm)):position.position;
                    if(std::abs(centre.x-at->position.x)<=viewport.x*.5+radius &&
                       std::abs(centre.y-at->position.y)<=viewport.y*.5+radius)watched=true;
                });
                const auto* modifiers=world.tryGet<PlayerModifiers>(owner);
                const double luck=modifiers?std::max(1.0,modifiers->luck):1.0;
                if(!watched && rng_.chance(std::min(1.0,.125*luck)))amount*=4.0;
            }
        }
    }
    if (kind != DamageKind::Lightning || resolvingRuby_ || amount <= 0.0 ||
        world.has<Pet>(victim) ||
        (!world.has<MobTag>(victim) && !isPracticeDummy(world, content(), victim))) {
        return applyDamageOnce(world, victim, source, amount, nowMillis, kind, checkedTarget, impactPoint, critical);
    }
    const Entity dealer = creditedPlayer(world, source);
    const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(dealer);
    double chance = modifiers != nullptr ? modifiers->lightningRepeatChance : 0.0;
    if (chance > 0.0) {
        const auto* health = world.tryGet<Health>(poolOwner(world, victim));
        if (health && health->fraction() < 0.5) {
            chance *= hyperAmount(wornHyperTechnique(world, content(), dealer, "ruby"),
                                  HyperTechnique::RubyEcho);
        }
    }
    const int limit = modifiers != nullptr ? std::clamp(modifiers->maxLightningRepeats, 0, 3) : 0;
    if (chance <= 0.0 || limit == 0) {
        return applyDamageOnce(world, victim, source, amount, nowMillis, kind, checkedTarget, impactPoint, critical);
    }
    // Hold the guard across the initial hit too: colony sharing and reflected
    // damage must not multiply the repeat budget. Never retain ECS pointers
    // across a hit, which can move both victim and dealer to new archetypes.
    struct RepeatGuard {
        bool& flag;
        explicit RepeatGuard(bool& value) : flag(value) { flag = true; }
        ~RepeatGuard() { flag = false; }
    } guard(resolvingRuby_);
    DamageResult result = applyDamageOnce(world, victim, source, amount, nowMillis, kind, checkedTarget, impactPoint, critical);
    if (result.refused || result.dodged) return result;
    for (int repeat = 0; repeat < limit && !result.killed; ++repeat) {
        if (!rubyRng_.chance(clamp(chance, 0.0, 1.0))) break;
        const DamageResult extra = applyDamageOnce(world, victim, source, amount, nowMillis, kind, NULL_ENTITY, impactPoint, critical);
        if (extra.refused || extra.dodged) break;
        result.applied += extra.applied;
        result.undeadMobOverkill += extra.undeadMobOverkill;
        result.killed = extra.killed;
        if (events_ != nullptr) {
            const Transform* target = world.tryGet<Transform>(victim);
            const Transform* origin = world.tryGet<Transform>(source);
            if (target != nullptr && origin != nullptr) {
                events_->lightning(origin->position, (target->position - origin->position).length(),
                                   target->realm, {target->position}, false, source);
            }
        }
    }
    return result;
}

DamageResult CombatSystem::applyDamageOnce(World& world, Entity victim, Entity source,
                                       double amount, double nowMillis, DamageKind kind,
                                       Entity checkedTarget, std::optional<Vec2> impactPoint,
                                       bool critical) {
    DamageResult result;
    bool toiletBoosted = false;
    if (const auto* petal = world.tryGet<PetalInstance>(source); petal &&
        content().petal(petal->configIndex).id == "toilet_paper") {
        const auto* status = world.tryGet<Afflictions>(petal->owner);
        toiletBoosted = status && nowMillis < status->toiletBoostUntilMillis;
        if (toiletBoosted) amount *= 10.0;
    }
    if (const auto* burrow = world.tryGet<BurrowState>(victim);
        burrow && burrow->active && nowMillis < burrow->untilMillis) {
        const bool directOrLightning = kind == DamageKind::Direct || kind == DamageKind::Lightning;
        const bool checksBurrow = directOrLightning ||
            (kind == DamageKind::Piercing && world.has<Projectile>(source));
        const Entity attacker = creditedPlayer(world, source);
        const auto* visuals = world.tryGet<PlayerVisuals>(attacker);
        const bool corruptedAttack = world.has<PlayerProgress>(victim) && attacker != victim &&
            visuals && (visuals->corrupted || visuals->equippedCorruption);
        if (corruptedAttack || (checksBurrow && !canNoticeBurrowed(world, source, victim))) {
            const auto* underground = world.tryGet<BurrowState>(attacker);
            const auto* from = world.tryGet<Transform>(attacker);
            const auto* to = world.tryGet<Transform>(victim);
            const bool mutualBurrow = attacker != victim && underground && underground->active &&
                nowMillis < underground->untilMillis && isStanding(world, attacker) &&
                from && to && from->realm == to->realm;
            const bool riftAttack = directOrLightning && world.has<RiftPlayer>(victim) &&
                world.has<RiftPlayer>(attacker);
            if (!mutualBurrow || !(corruptedAttack || riftAttack)) {
                result.refused = true;
                return result;
            }
            if (corruptedAttack) amount *= 0.10;
        }
    }
    if (kind == DamageKind::Poison && world.has<PoisonImmune>(victim)) {
        result.refused = true;
        return result;
    }
    // Non-finite damage reaches here from a config that multiplied a zero by
    // an infinity; it must cost a refused hit, not a NaN health bar that no
    // comparison can ever bring back below zero.
    if (!std::isfinite(amount) || amount == 0.0) {
        result.refused = true;
        return result;
    }

    // Colony recursion receives an already scaled share, not a fresh attack.
    if (amount > 0.0 && kind == DamageKind::Lightning && !sharingColonyHit_)
        amount *= lightningTalentScale(world, source);

    // A shared chain answers as ONE mob from here down: armour, the ledger,
    // the XP, the loot slots and the death all belong to the pool's owner,
    // whichever of its ten bodies the hit actually landed on. Done at the top
    // rather than at the health write so there is no second rule about which
    // of the two entities each of those consequences attaches to.
    //
    // The struck segment is kept for exactly one purpose: the floating number
    // pops where the player hit, not up at the head.
    const Entity struck = victim;
    victim = poolOwner(world, victim);
    if ((checkedTarget == NULL_ENTITY || victim != checkedTarget) &&
        !canHit(world, victim, source, nowMillis, kind == DamageKind::Sponge)) {
        // One refusal still pays the ledger. Every reference path writes the
        // contribution BEFORE it tests whether the mob is already dead --
        // trackDamage() at playerState.ts:2751 and creditDamage() at
        // projectileCollision.ts:437 both sit above their `isDead` continue --
        // so a swing that lands on something another petal killed earlier in
        // this same tick still buys its dealer a loot slot. That is the
        // difference between two players sharing a kill and one of them
        // getting nothing.
        //
        // Only that refusal: a same-side hit never reaches trackDamage at all,
        // and there is no ledger left on an entity the reaper has taken.
        const Health* health = world.tryGet<Health>(victim);
        const bool corpse = world.isAlive(victim) && health != nullptr &&
                            (world.has<Dead>(victim) || health->current <= 0.0);
        if (corpse && canDamage(world, source, victim)) {
            creditSwing(world, victim, source, amount);
        }
        result.refused = true;
        return result;
    }

    // A colony shares the hit. Whatever one termite is struck for is split
    // evenly over every colony mob CONNECTED to it (see below) that this
    // source could hit right now, the struck one included, and each share
    // comes back through here as its own hit -- so the ledgers, the numbers
    // and the deaths all land on the termite that took the share (the dodge
    // and the armour are taken once, by the struck one -- below), and a
    // cluster of twenty takes a twentieth each. Anywhere, not only in a
    // dungeon: the connection is distance and nothing else. Positive damage
    // only: a heal is not shared, and a lone termite has nobody to share with.
    if (amount > 0.0 && !sharingColonyHit_ && world.has<ColonyMember>(victim) &&
        world.tryGet<Transform>(victim) != nullptr) {
        // Only the termites CONNECTED to the struck one share: those
        // standing within reach of it -- the larger of its reach and
        // theirs (colonyReach). Directly, never through a neighbour: a
        // termite next to a connected one but out of the struck one's
        // reach takes nothing.
        colonyScratch_.clear();
        const Transform& struckAt = world.get<Transform>(victim);
        const Realm realm = struckAt.realm;
        const Vec2 centre = struckAt.position;
        const double struckReach = colonyReach(world, victim);
        bind(world);
        queries_->colony.each([&](Entity member, ColonyMember&, Transform& transform) {
            if (member == victim || transform.realm != realm) return;
            const double reach = std::max(struckReach, colonyReach(world, member));
            if (distanceSq(transform.position, centre) > reach * reach) return;
            if (canHit(world, member, source, nowMillis)) colonyScratch_.push_back(member);
        });
        if (!colonyScratch_.empty()) {
            // The hit is dodged and blunted ONCE, by the termite it struck,
            // and only what gets through is shared. Blunting each share
            // instead would let every termite's flat armour come off a
            // twentieth of the hit -- a colony of twenty with one point
            // each would shrug off anything under twenty.
            if (isDirectHit(kind) && rollDodge(world, victim, nowMillis)) {
                result.dodged = true;
                return result;
            }
            if (armorBlunts(kind)) {
                const double armor = effectiveArmor(world, victim, nowMillis);
                const auto* share = world.tryGet<ElementalOffenseShare>(source);
                if (armor != 0.0) amount = std::max(0.0, amount - armor * (share ? share->value : 1.0));
            }
            if (amount <= 0.0) return result;
            return shareHit(world, colonyScratch_, victim, source, amount, nowMillis, kind,
                            sharingColonyHit_, critical);
        }
    }

    // A relic's wearer shares what it takes with the other wearers in its
    // squad: the hit is split evenly over every one of them in the same realm
    // that this source could hit right now, the struck flower included, and
    // each share comes back through here as that flower's own hit -- its own
    // dodge, armour, shield and cotton. Positive damage only, and never a
    // share of a share. Nobody squadded, or no other wearer: the flower takes
    // it all, as it would without the relic.
    if (amount > 0.0 && !sharingRelicHit_ && squads != nullptr && world.has<PlayerTag>(victim)) {
        const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(victim);
        const Transform* struckAt = world.tryGet<Transform>(victim);
        const std::vector<SquadBody>* squad = squads->membersOf(victim);
        if (modifiers != nullptr && modifiers->sharesDamage && struckAt != nullptr &&
            squad != nullptr) {
            const Realm realm = struckAt->realm;
            const auto* focus = hyperDefending(world, victim)
                ? wornHyperTechnique(world, content(), victim, "mysterious_relic") : nullptr;
            const double radius = hyperAmount(focus, HyperTechnique::WideRelic, 0.0);
            relicScratch_.clear();
            for (const SquadBody& member : *squad) {
                const Entity body = member.body;
                if (body == victim || !world.isAlive(body) || world.has<Dead>(body)) continue;
                if (!world.has<PlayerTag>(body)) continue;
                const PlayerModifiers* theirs = world.tryGet<PlayerModifiers>(body);
                const Transform* at = world.tryGet<Transform>(body);
                if (theirs == nullptr || !theirs->sharesDamage || at == nullptr ||
                    at->realm != realm) {
                    continue;
                }
                if (radius > 0.0 && distanceSq(struckAt->position, at->position) > radius * radius) continue;
                if (canHit(world, body, source, nowMillis)) relicScratch_.push_back(body);
            }
            if (!relicScratch_.empty()) {
                return shareHit(world, relicScratch_, victim, source, amount, nowMillis, kind,
                                sharingRelicHit_, critical);
            }
        }
    }

    // TypeScript's mob health writer is `max(0, health - amount)`.  Glitch is
    // authored with negative damage, so its contact deliberately heals mobs;
    // PVP's separate player damage path rejects non-positive values.  Preserve
    // that asymmetry instead of sanitising the content into another petal.
    if (amount < 0.0) {
        if (!world.has<MobTag>(victim)) {
            result.refused = true;
            return result;
        }
        // Negative damage is a HEAL, and a dandelion's lockout refuses it like
        // any other. This is the one heal that arrives down the damage path,
        // which is why the lockout has to be asked here as well as in the
        // petal system -- a lockout that covered every route but this one
        // would be a glitch petal quietly topping up the mob a dandelion is
        // holding down.
        //
        // Only the health WRITE is skipped. The contact still happened, so the
        // swing is not refused: the petal that delivered it flashes the mob
        // and pays for the swing out of its own health exactly as it does when
        // nothing is locked, which is the rule resolveMelee states as "a swing
        // of nothing is still a swing".
        const bool blocked = healingBlocked(world, victim, nowMillis);
        Health& health = world.get<Health>(victim);
        if (!blocked) health.current -= amount;
        if (isDirectHit(kind)) {
            health.flashUntilMillis = std::max(health.flashUntilMillis,
                                               nowMillis + kHurtFlashMillis);
        }
        result.applied = blocked ? 0.0 : amount;
        if (!blocked) creditSwing(world, victim, source, amount);
        mirrorSharedChain(world, victim, false, NULL_ENTITY);
        return result;
    }

    // A high-tier egg's escort can absorb a wild mob's body impact. Weaker
    // attackers chip less from an H summon; attacks from higher tiers and
    // projectiles still use their ordinary damage.
    if (kind == DamageKind::Direct && world.has<Pet>(victim) && world.has<MobTag>(source) && world.has<MobType>(source) &&
        !world.has<Pet>(source)) {
        const auto& pet = world.get<Pet>(victim);
        if (const auto* loadout = world.tryGet<Loadout>(pet.owner); loadout && pet.slot < kLoadoutActiveSlots) {
            const auto worn = equippedPetal(content(), *loadout, pet.slot);
            const Rarity attacker = world.get<MobType>(source).rarity;
            if (!worn.empty() && (worn.rarity == Rarity::Unique || isHyperPetal(worn.rarity)) &&
                petalIsEgg(content().petal(worn.configIndex)) && petalTier(attacker) <= petalTier(worn.rarity)) {
                const int tierGap = petalTier(worn.rarity) - petalTier(attacker);
                const double maxShare = tierGap >= 2 ? 0.16 : tierGap == 1 ? 0.23 : 0.32;
                amount = std::min(amount, world.get<Health>(victim).max * maxShare);
            }
        }
    }
    // Jurassic amulets grant their best bonus to the wearer and nearby squad.
    // Compute it once for each landed direct petal hit, before armour.
    if (!sharingColonyHit_ && kind == DamageKind::Direct && amount > 0.0 &&
        isWildMob(world, victim) &&
        (world.has<PetalInstance>(source) ||
         (world.has<Projectile>(source) && world.has<PlayerTag>(world.get<Projectile>(source).owner)))) {
        const Entity owner = creditedPlayer(world, source);
        const auto* origin = world.tryGet<Transform>(owner);
        double bonus = 0.0, flat = 0.0;
        const auto consider = [&](Entity ally) {
            const auto* stats = world.tryGet<PlayerModifiers>(ally);
            if (!stats || stats->jurassicAmuletBonus <= 0.0 || !isStanding(world, ally)) return;
            const auto* at = world.tryGet<Transform>(ally);
            if (!origin || !at || at->realm != origin->realm ||
                distanceSq(at->position, origin->position) > 600.0 * 600.0) return;
            if (stats->jurassicAmuletBonus > bonus) {
                bonus = stats->jurassicAmuletBonus;
                flat = stats->jurassicAmuletFlatDamage;
            }
        };
        consider(owner);
        if (const auto* members = squads ? squads->membersOf(owner) : nullptr)
            for (const auto& member : *members) consider(member.body);
        if (bonus > 0.0) amount = amount * (1.0 + bonus) + std::min(flat, amount * .08);
    }

    // Evasion happens before durability, armor and shields are spent.
    const auto* petal = world.tryGet<PetalInstance>(victim);
    const auto* technique = petal && isHyperPetal(petal->rarity) && !petal->mimicked &&
        petal->slot < kLoadoutActiveSlots ? hyperTechniqueInfo(content().petal(petal->configIndex).id, petal->rarity)
                                        : nullptr;
    const bool physicalMob = kind == DamageKind::Direct && isWildMob(world, source) &&
                             !world.has<MobRingPetal>(source);
    double evasionBonus = 0.0;
    if (technique && technique->technique == HyperTechnique::PokerOdds) {
        const auto* health = world.tryGet<Health>(victim);
        if (health && health->fraction() < 0.25 &&
            (physicalMob || (kind == DamageKind::Direct && world.has<Projectile>(source) &&
                             isWildMob(world, reflectionTarget(world, source))))) {
            evasionBonus = technique->amount;
        }
    }
    if ((kind == DamageKind::Direct ||
         (kind == DamageKind::Recoil && world.has<PetalTag>(victim))) &&
        !sharingColonyHit_ && rollDodge(world, victim, nowMillis, evasionBonus)) {
        result.dodged = true;
        if (physicalMob && hyperDefending(world, victim)) {
            const auto* ward = wornHyperTechnique(world, content(), victim, "talisman_of_evasion");
            const auto* afflictions = world.tryGet<Afflictions>(poolOwner(world, source));
            if (ward && (!afflictions || !afflictions->slowed(nowMillis))) {
                applySlow(world, source, ward->amount, 450.0, Rarity::Apex, nowMillis);
            }
        }
        return result;
    }

    if (technique && hyperDefending(world, petal->owner)) {
        const auto* mods = world.tryGet<PlayerModifiers>(petal->owner);
        const int parts = mods ? computerPartCount(mods->computerParts[rarityIndex(Rarity::Apex)]) : 0;
        if (kind == DamageKind::Recoil && isWildMob(world, source) && !world.has<MobRingPetal>(source)) {
            amount *= hyperAmount(technique, HyperTechnique::RockBrace);
            amount *= hyperAmount(technique, HyperTechnique::BasicRally);
            if (parts >= 3) amount *= hyperAmount(technique, HyperTechnique::CodeParity);
            const auto id = technique->id;
            if (id == "leaf" || id == "golden_leaf" || id == "magic_leaf" || id == "dead_leaf") {
                amount *= hyperAmount(wornHyperTechnique(world, content(), petal->owner, "dead_leaf"),
                                      HyperTechnique::DeadLeafShelter);
            }
        } else if (kind == DamageKind::Direct && world.has<Projectile>(source) &&
                   isWildMob(world, reflectionTarget(world, source))) {
            amount *= hyperAmount(technique, HyperTechnique::CornCover);
            if (parts >= 4) amount *= hyperAmount(technique, HyperTechnique::CubeBracing);
            if (technique->technique == HyperTechnique::CardLastStand) {
                const auto* health = world.tryGet<Health>(victim);
                if (health && health->fraction() <= 0.5) amount *= technique->amount;
            }
        } else if (kind == DamageKind::Lightning && technique->technique == HyperTechnique::RubberGround &&
                   isWildMob(world, reflectionTarget(world, source))) {
            amount *= technique->amount;
        }
    }

    // Armour: the victim's flat reduction on every DIRECT hit, less whatever a
    // bur has stripped. Mobs carry one, and so does a bone petal -- whose
    // armour also comes off the Recoil it pays for its own hits, which is the
    // one blow a petal on the ring routinely takes.
    //
    // DIRECT ONLY: poison and a sponge repayment arrive as a per-tick drip --
    // thirty slivers a second -- and a flat subtraction from each of them is not a tax, it is immunity.
    // Armour answers hits; poison is what gets through it.
    //
    // Below zero the subtraction ADDS, which is what a stripped mob is for.
    // A colony's share was blunted whole, before it was split (above).
    if (armorBlunts(kind) && !sharingColonyHit_) {
        double armor = effectiveArmor(world, victim, nowMillis);
        if (armor != 0.0 && isDirectHit(kind)) {
            if (const auto* share = world.tryGet<ElementalOffenseShare>(source)) armor *= share->value;
        }
        if (armor > 0.0 && kind == DamageKind::Recoil && isWildMob(world, source) &&
            !world.has<MobRingPetal>(source)) {
            const auto* armorState = world.tryGet<Armor>(victim);
            const auto* bone = armorState && armorState->bone ? world.tryGet<PetalInstance>(victim) : nullptr;
            if (bone && hyperDefending(world, bone->owner)) {
                armor *= hyperAmount(hyperTechniqueInfo("bone", bone->rarity, bone->mimicked),
                                     HyperTechnique::BoneGuard);
            }
        }
        if (armor != 0.0) amount = std::max(0.0, amount - armor);
    }

    if (amount > 0 && isDirectHit(kind)) {
        const auto* pet = world.tryGet<Pet>(victim);
        const auto* ring = pet ? world.tryGet<Loadout>(pet->owner) : nullptr;
        if (ring) for (Entity ward : ring->spawned) {
            auto* body = world.tryGet<PetalInstance>(ward);
            if (!body || !isStanding(world, ward) || content().petal(body->configIndex).id != "magic_nazar_amulet") continue;
            if (++body->wardHits >= static_cast<unsigned>(2 + ladderIndex(body->rarity) / 3)) {
                if (auto* hp = world.tryGet<Health>(ward)) hp->current = 0;
                world.add<Dead>(ward, Dead{source});
            }
            result.refused = true;
            return result;
        }
    }
    const bool directPlayerHit = isDirectHit(kind) && world.has<PlayerTag>(victim);
    if (amount > 0.0 && (isDirectHit(kind) || kind == DamageKind::Recoil)) {
        if (auto* targetPetal = world.tryGet<PetalInstance>(victim)) {
            const auto* mods = world.tryGet<PlayerModifiers>(targetPetal->owner);
            const auto* targetAt = world.tryGet<Transform>(victim);
            if (mods && mods->amberPetalReduction > 0.0 && targetAt &&
                targetPetal->slot > 0 && targetPetal->slot < kLoadoutActiveSlots) {
                if (const auto* ring = world.tryGet<Loadout>(targetPetal->owner)) {
                    double reduction = 0.0;
                    for (const Entity e : ring->spawned) {
                        const auto* amber = world.tryGet<PetalInstance>(e);
                        if (!amber || amber->owner != targetPetal->owner ||
                            amber->slot + 1 != targetPetal->slot ||
                            ladderIndex(amber->rarity) < ladderIndex(targetPetal->rarity) ||
                            content().petal(amber->configIndex).id != "amber" || !isStanding(world, e))
                            continue;
                        const auto* at = world.tryGet<Transform>(e);
                        if (at && at->realm == targetAt->realm)
                            reduction = std::max(reduction, .025 + .008 * ladderIndex(amber->rarity));
                    }
                    amount *= 1.0 - reduction;
                }
            }
            if (nowMillis < targetPetal->chromosomeShieldUntilMillis) {
                const double absorbed = std::min(amount, targetPetal->chromosomeShield);
                targetPetal->chromosomeShield -= absorbed;
                amount -= absorbed;
            }
        } else if (directPlayerHit && kind == DamageKind::Direct && isWildMob(world, source)) {
            const auto* mods = world.tryGet<PlayerModifiers>(victim);
            const auto* mobAt = world.tryGet<Transform>(source);
            const auto* mobBody = world.tryGet<Body>(source);
            const auto* ring = world.tryGet<Loadout>(victim);
            if (mods && mods->scalesBodyReduction > 0.0 && mobAt && mobBody && ring) {
                double reduction = 0.0;
                for (const Entity e : ring->spawned) {
                    const auto* scales = world.tryGet<PetalInstance>(e);
                    if (!scales || scales->owner != victim ||
                        content().petal(scales->configIndex).id != "scales" || !isStanding(world, e))
                        continue;
                    const auto* at = world.tryGet<Transform>(e);
                    const auto* body = world.tryGet<Body>(e);
                    if (!at || !body || at->realm != mobAt->realm) continue;
                    const double reach = mobBody->radius + body->radius;
                    if (distanceSq(at->position, mobAt->position) < reach * reach)
                        reduction = std::max(reduction, .04 + .009 * ladderIndex(scales->rarity));
                }
                amount *= 1.0 - reduction;
            }
        }
    }

    if (auto* petal = world.tryGet<PetalInstance>(victim);
        petal && amount > 0.0 && nowMillis < petal->chromosomeShieldUntilMillis &&
        (isDirectHit(kind) || kind == DamageKind::Recoil)) {
        const double absorbed = std::min(amount, petal->chromosomeShield);
        petal->chromosomeShield -= absorbed;
        amount -= absorbed;
    }

    // Root's armour, which is the flower's answer to the Armor a mob carries
    // above -- and the opposite kind of thing. A mob's is a standing property
    // of the mob; a flower's is AMMUNITION the petal system banked, and one
    // stack is spent here to blunt this hit by what the stack is worth.
    //
    // Spent even when it absorbs the blow whole. A stack that survived the
    // hits it stopped would make ten of them a permanent reduction rather
    // than a bank, and the post-hit window below is what keeps a single mob
    // from draining the bank in one tick's worth of contact.
    //
    // Direct only, for the reason the mob's armour is: a flat subtraction
    // from each sliver of a poison drip is immunity, not a tax.
    if(directPlayerHit && kind==DamageKind::Lightning) {
        const auto* modifiers=world.tryGet<PlayerModifiers>(victim);
        if(modifiers)amount=std::max(0.0,amount-modifiers->lightningReduction);
    }
    if (directPlayerHit && kind != DamageKind::Lightning) {
        if (ArmorStackState* armor = world.tryGet<ArmorStackState>(victim)) {
            if (armor->stacks > 0 && armor->perStack > 0.0) {
                --armor->stacks;
                amount = std::max(0.0, amount - armor->perStack);
            }
        }
    }

    // A shield is extra HP laid over the flower's own: it takes the damage
    // first and runs down as it does, and only what it could not hold reaches
    // the bar. EVERY kind drains it, drips included -- a flat per-hit
    // subtraction had to spare poison because it never ran out, but a pool
    // that empties is a tax on a drip, not immunity to it. After the root
    // stack, so a stack's blunting is never paid for out of the shield.
    if (world.has<PlayerTag>(victim)) {
        if (ShieldState* shield = world.tryGet<ShieldState>(victim)) {
            const double absorbed = std::clamp(shield->amount, 0.0, amount);
            shield->amount -= absorbed;
            amount -= absorbed;
        }
    }
    if (auto* pet = world.tryGet<Pet>(victim); pet &&
        nowMillis < pet->crystalLeafShieldUntilMillis && mobsDoing(world, source) &&
        creditedPlayer(world, source) == NULL_ENTITY) {
        const double absorbed = std::min(amount, pet->crystalLeafShield);
        pet->crystalLeafShield -= absorbed;
        amount -= absorbed;
    }
    if (amount <= 0.0) {
        // The flower still gets the brief post-hit protection after a shield
        // absorbs the full number; this was a legitimate hit, not a rejected
        // target. Callers therefore still arm their attacker cooldown.
        //
        // The FLOWER only. A mob's invulnerability window is shared by
        // everything attacking it, so granting one here would let the weakest
        // petal in a ring -- the one armour happens to absorb whole -- lock the
        // mob for 50 ms against every other petal and every other player.
        // Nothing but armour can zero a mob's hit, so this is where that would
        // have started.
        if (directPlayerHit) {
            Health& health = world.get<Health>(victim);
            health.hitCooldownUntilMillis = std::max(health.hitCooldownUntilMillis,
                                                     nowMillis + kPostHitInvulnerabilityMillis);
        }
        return result;
    }

    if (directPlayerHit && kind == DamageKind::Direct && world.has<MobTag>(source) &&
        !world.has<Pet>(source) && !world.has<MobRingPetal>(source) &&
        hyperDefending(world, victim)) {
        // The ward sees only damage left after root armor and existing shields.
        // Its cooldown belongs to the flower and survives loadout swaps.
        const auto* loadout = world.tryGet<Loadout>(victim);
        if (loadout) {
            const auto& registry = content();
            const auto nazar = registry.petalIndex("nazar_amulet");
            const double maxHealth = world.get<Health>(victim).max;
            bool wardReady = false;
            for (const auto petal : loadout->spawned) {
                auto* instance = world.tryGet<PetalInstance>(petal);
                if (!instance || !isHyperPetal(instance->rarity) || instance->mimicked ||
                    instance->slot >= kLoadoutActiveSlots || world.has<Dead>(petal)) continue;
                if (instance->configIndex == nazar) wardReady = true;
            }
            const auto* timers = world.tryGet<HyperPetalTimers>(victim);
            if (wardReady && amount >= maxHealth * 0.30 &&
                (!timers || nowMillis >= timers->wardReadyAtMillis)) {
                world.ensure<HyperPetalTimers>(victim).wardReadyAtMillis = nowMillis + 30000.0;
                amount *= 1.0 - hyperAmount(hyperTechniqueInfo("nazar_amulet", Rarity::Apex),
                                            HyperTechnique::NazarWard, 0.0);
            }
        }
    }
    if (directPlayerHit) {
        const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(victim);
        const double durationMillis = modifiers ? modifiers->spongeDamageDurationMillis : 0.0;
        if (durationMillis > 0.0) {
            SpongeDamageEffect effect;
            effect.remainingDamage = amount;
            effect.damagePerSecond = amount / (durationMillis / 1000.0);
            effect.source = source;
            effect.readyAtMillis = nowMillis + kPostHitInvulnerabilityMillis;
            world.ensure<SpongeDamageState>(victim).effects.push_back(effect);
            Health& health = world.get<Health>(victim);
            health.hitCooldownUntilMillis =
                std::max(health.hitCooldownUntilMillis,
                         nowMillis + kPostHitInvulnerabilityMillis);
            return result;
        }
    }

    // Cotton, then Magic Cotton, catch the damage left before it reaches the
    // bar. This runs after Sponge, so deferred damage is caught as it is
    // repaid. Poison drips can also spend the remaining cotton or mana.
    if (world.has<PlayerTag>(victim)) {
        amount = soakIntoCotton(world, victim, source, amount, nowMillis, kind);
        if (amount <= 0.0) {
            // Caught whole. A landed hit still earns the flower the post-hit
            // window, or mob contact -- paced by that window and nothing else
            // -- would strip the cotton on the very next tick. Re-fetched: a
            // cotton breaking relocates rows.
            if (directPlayerHit) {
                Health& health = world.get<Health>(victim);
                health.hitCooldownUntilMillis =
                    std::max(health.hitCooldownUntilMillis,
                             nowMillis + kPostHitInvulnerabilityMillis);
            }
            return result;
        }
    }

    Health& health = world.get<Health>(victim);
    // Icosahedron gets one protected trip to a confirmed second enemy, even
    // against a mob whose contact damage exceeds its whole health pool.
    // Once the second hit lands, normal lethal recoil resumes immediately.
    if (const auto* petal = world.tryGet<PetalInstance>(victim)) {
        const auto& flight = petal->ricochet;
        if (flight.enabled && flight.active && !flight.returning &&
            flight.protectSecondHit && flight.hits == 1 && nowMillis < flight.deadlineMillis) {
            amount = std::min(amount, std::max(0.0, health.current - 1.0));
        }
    }
    // What the health bar actually lost, which is what the caller is told and
    // what the floating number reads. The LEDGER is credited the full swing
    // instead -- see creditSwing().
    const bool invulnerable = world.has<NpcTag>(victim);
    // Training NPCs never lose HP: their damage readout must not be capped
    // by a decorative health pool (which differs between rarity tiers).
    const double applied = invulnerable ? amount : std::min(amount, health.current);
    const bool canTriggerToiletPaper = !invulnerable && applied > 0.0 && world.has<PlayerTag>(victim);
    const bool wasAtArmThreshold = canTriggerToiletPaper && health.fraction() >= 0.9;
    // The same loss against the whole bar, for the client's hit flash. Taken
    // now: `health` dangles past trySecondChance below.
    const double share = health.max > 0.0 ? applied / health.max : 0.0;
    // An NPC's bar never moves. The hit LANDS -- armour took its share above,
    // it flashes, it is numbered and a target dummy counts it -- and nothing
    // comes off, so nothing an NPC is hit by can kill it.
    if (!invulnerable) {
        health.current -= applied;
        if (applied > 0 && isDirectHit(kind) && source != victim) {
            const auto* arc = kind == DamageKind::Lightning ? world.tryGet<LightningTarget>(source) : nullptr;
            // Two batteries must not feed each other's retaliation.
            if (!arc || !arc->fromBattery) {
                ++health.directHurtSerial;
                if (directPlayerHit) health.directHurtPlayer = creditedPlayer(world, source);
            }
        }
        if (health.current < 0.0) health.current = 0.0;
    }
    bool fatal = !invulnerable && health.current <= 0.0;
    // A salt's repayment flashes the attacker as gardn's does, though it is
    // no hit of its own anywhere else in this function.
    if (isDirectHit(kind) || kind == DamageKind::Reflect) {
        health.flashUntilMillis = std::max(health.flashUntilMillis, nowMillis + kHurtFlashMillis);
    }
    result.applied = applied;
    if (amount > applied && world.has<Undead>(victim) &&
        world.has<RisesUndead>(victim) && isWildMob(world, victim)) {
        result.undeadMobOverkill = amount - applied;
    }
    if (applied > 0.0 && (invulnerable || (onPracticeHit && world.has<MobTag>(victim)))) {
        recordPracticeDamage(world, victim, source, kind, applied, nowMillis);
    }
    if (directPlayerHit) {
        health.hitCooldownUntilMillis =
            std::max(health.hitCooldownUntilMillis,
                     nowMillis + kPostHitInvulnerabilityMillis);
    }

    // A heal and hit can occur between equipment updates. Check the
    // pre-hit health for arming and the post-hit health before any rescue.
    if (canTriggerToiletPaper) {
        const auto* loadout = world.tryGet<Loadout>(victim);
        auto* status = world.tryGet<Afflictions>(victim);
        const bool trigger = !fatal && health.fraction() <= 0.1 &&
            (wasAtArmThreshold || (status && status->toiletArmed));
        if ((wasAtArmThreshold || trigger) && loadout && hasActiveToiletPaper(content(), *loadout)) {
            auto& toilet = status ? *status : world.ensure<Afflictions>(victim);
            toilet.toiletArmed = !trigger;
            if (trigger) toilet.toiletBoostUntilMillis = nowMillis + kToiletPaperBurstMillis;
        }
    }

    // Second Chance turns a killing blow on a flower into 1 HP. It is asked
    // here rather than at the call sites because every lethal path in the game
    // funnels through this function, where the reference has to remember to
    // ask on five of them -- one of which is a poison tick.
    //
    // `health` dangles from this call on: the lockout is a component, and
    // adding one relocates the victim's row.
    const bool savedBySecondChance = fatal && trySecondChance(world, victim, nowMillis);
    if (savedBySecondChance) fatal = false;
    if (fatal && world.has<PlayerTag>(victim)) {
        const auto* bar = world.tryGet<Loadout>(victim);
        if (bar) for (Entity ward : bar->spawned) {
            const auto* item = world.tryGet<PetalInstance>(ward);
            if (!item || !isStanding(world,ward) || content().petal(item->configIndex).id != "magic_yggdrasil") continue;
            world.add<Dead>(ward, Dead{victim});
            auto& saved = world.get<Health>(victim);
            saved.current = std::max(1.0, saved.max * .10);
            saved.hitCooldownUntilMillis = nowMillis + kPostHitInvulnerabilityMillis;
            fatal = false;
            break;
        }
    }
    // A worn bandage turns what is left of a killing blow into Undead: the
    // flower stands at 1 HP until the bandage's time is up, and dies then.
    // After Second Chance, which spares the flower outright.
    if (fatal && tryUndead(world, victim, source, nowMillis)) fatal = false;

    creditSwing(world, victim, source, amount, applied > 0.0 && kind == DamageKind::Direct);

    if (isDirectHit(kind) && isWildMob(world, victim) && !fatal) {
        Entity flower = source;
        if (const auto* shot = world.tryGet<Projectile>(source)) flower = shot->owner;
        else if (const auto* petal = world.tryGet<PetalInstance>(source)) flower = petal->owner;
        if (auto* input = world.tryGet<PlayerInput>(flower)) {
            input->summonFocus = victim;
            input->summonFocusUntilMillis = nowMillis + 2000.0;
        }
    }

    // Every kind narrates itself. A poison tick and a sponge repayment reach
    // the reference's client as ordinary damage on the same two channels a
    // petal hit does -- markPoisonDamaged() and emitPlayerDamaged() -- and the
    // client already colours and offsets a poison number away from the petal
    // hit that landed in the same tick, so nothing is buried by reporting one.
    //
    // Those two channels are the whole of what is NUMBERED: `enemiesDamaged`
    // carries mobs and `playerDamaged` carries flowers. An NPC is numbered
    // like the mob it is built from: a target dummy that showed no numbers
    // would have nothing to count. Everything else that holds a pool -- a
    // petal paying for its swing, a seed on a mob's ring, a shot -- is
    // reported for the flash alone (flashStruck).
    const bool numbered =
        world.has<MobTag>(victim) || world.has<PlayerTag>(victim) || world.has<NpcTag>(victim);
    if (!numbered) flashStruck(world, victim, applied, share, fatal);
    if (events_ != nullptr && numbered) {
        // Reported against the body that was HIT. For everything but a leech
        // that is the victim itself; for a leech it is the segment the petal
        // touched, so the number rises off the bead the player is looking at
        // while the pool it came out of lives up at the head.
        const Entity shown = world.isAlive(struck) ? struck : victim;
        if (const NetId* id = world.tryGet<NetId>(shown)) {
            const Transform* transform = world.tryGet<Transform>(shown);
            std::uint8_t flags{0};
            if (toiletBoosted || critical) flags |= net::DamageCritical;
            if (kind == DamageKind::Poison) flags |= net::DamagePoison;
            // Every strike in the game reaches here as DamageKind::Lightning --
            // the petal cutter's burst, a jellyfish's reach and a firefly's
            // touch alike -- so the cyan is decided once, where the number is
            // reported, rather than at each of the three triggers.
            if (kind == DamageKind::Lightning) flags |= net::DamageLightning;
            // Whose hit it was, for the one viewer it belongs to (see
            // net::DamageByViewer). Resolved now, while the source still
            // exists: a petal that breaks on this very swing is gone by the
            // time the snapshot is written.
            const Entity dealer = creditedPlayer(world, source);
            const PlayerAccount* account =
                dealer != NULL_ENTITY ? world.tryGet<PlayerAccount>(dealer) : nullptr;
            Vec2 numberAt = transform ? transform->position : Vec2{};
            if (!impactPoint && kind == DamageKind::Direct && world.has<PetalInstance>(source)) {
                const auto* origin = world.tryGet<Transform>(source);
                if (origin && transform && origin->realm == transform->realm) {
                    impactPoint = origin->position;
                }
            }
            // An impact can be on screen even when the centre of a large mob is outside it.
            if (impactPoint && transform) {
                numberAt = *impactPoint;
                if (const auto* body = world.tryGet<Body>(shown)) {
                    const Vec2 offset = numberAt - transform->position;
                    const double distance = offset.length();
                    if (distance > body->radius && distance > 0.0) {
                        numberAt = transform->position + offset * (body->radius / distance);
                    }
                }
            }
            events_->damage(id->value, applied, numberAt,
                            transform ? transform->realm : Realm::Overworld, flags, dealer,
                            account != nullptr ? account->connection : 0, share, shown);
        }
    }

    // Salt: a share of what this hit took off the flower goes back to whoever
    // is behind it, as gardn's `damage_reflection` does. The health bar's
    // loss, not the swing, so armour, a shell, a cotton and overkill shrink the
    // repayment exactly as they shrank the hit -- and a killing blow still
    // took something, so it is still repaid.
    //
    // The share is the salt's against its own tier and climbs the tier ladder
    // from there: three times it for every tier the thing that landed the hit
    // is below the salt, a third for every tier above.
    //
    // Direct hits only, like everything else a flower wears against hits.
    // gardn reflects poison as well, but here a drip is thirty slivers a
    // second and each would be its own number over the mob. A sponge's
    // deferred repayment is a drip too, so what a sponge holds back is never
    // reflected.
    //
    // Nested, as cotton's soak is. The attacker's row may move under this
    // call, which every caller already allows for: none of them carries a
    // pointer into the world across an applyDamage().
    if (directPlayerHit && applied > 0.0) {
        const auto* modifiers = world.tryGet<PlayerModifiers>(victim);
        if (modifiers && modifiers->retaliationBlast > 0) {
            if (const auto* pos = world.tryGet<Transform>(victim)) {
                const auto origin = *pos;
                const double power = modifiers->retaliationBlast * modifiers->petalDamageScale;
                const double radius = explosionTalentRadius(world, victim, 85.0);
                Entity blast = world.create();
                world.add<Transform>(blast, origin);
                GroundEffect field{GroundEffectKind::Poison, victim, radius, 0, 1, Rarity::Common, power, 0};
                field.showExplosion = true;
                world.add<GroundEffect>(blast, field);
                world.add<GroundEffectTag>(blast);
                world.add<Lifetime>(blast, Lifetime{net::kTickSeconds * 1.5});
            }
            modifiers = world.tryGet<PlayerModifiers>(victim);
        }
        const double cactus = physicalMob && hyperDefending(world, victim) &&
            world.get<Health>(victim).fraction() < 0.5
            ? hyperAmount(wornHyperTechnique(world, content(), victim, "cactus"), HyperTechnique::CactusThorns, 0.0)
            : 0.0;
        if (cactus > 0.0 || (modifiers && modifiers->damageReflection > 0.0)) {
            const Rarity incomingTier = hitRarity(world, source);
            double salt = modifiers ? modifiers->damageReflection *
                reflectionScale(modifiers->damageReflectionRarity, incomingTier) : 0.0;
            const Entity target = reflectionTarget(world, source);
            if (modifiers && modifiers->wideReflection && salt > 0) {
                const auto* at = world.tryGet<Transform>(victim);
                const auto copy = at ? *at : Transform{};
                if (modifiers->magicSponge || modifiers->spongeDamageDurationMillis > 0) salt *= .1;
                std::vector<Entity> nearby;
                Query<MobTag,Transform,Body> mobs{world};
                mobs.without<Dead>();
                mobs.each([&](Entity e, MobTag&, Transform& other, Body& body) {
                    const double reach = 170.0 + body.radius;
                    if (at && other.realm == copy.realm && !world.has<Pet>(e) &&
                        distanceSq(copy.position,other.position) < reach * reach) nearby.push_back(e);
                });
                for (Entity e : nearby) applyDamage(world,e,victim,applied*salt,nowMillis,DamageKind::Reflect);
                salt = 0.0;
            }
            if (target != NULL_ENTITY && (cactus > 0 || salt > 0)) {
                if (kind == DamageKind::Direct && petalTier(incomingTier) >= rarityIndex(Rarity::Apex) &&
                    isWildMob(world, target) && hyperDefending(world, victim)) {
                    salt *= hyperAmount(wornHyperTechnique(world, content(), victim, "salt"), HyperTechnique::SaltRetort);
                }
                applyDamage(world, target, victim, applied * std::max(salt, cactus), nowMillis, DamageKind::Reflect);
            }
        }
    }

    // A dandelion sheds a seed when it is hit. Booked here rather than fired
    // here: letting one go retires an entity and creates another, and the
    // damage path is walked by every system that deals any. The mob AI pass
    // pays it off (see MobAiSystem::tickPetalRings).
    //
    // DIRECT hits only, so the poison already on a dandelion does not empty it
    // thirty times a second, and never a fatal one -- a corpse fires nothing.
    // Capped at the seats it HAS rather than the seats still filled: counting
    // the live ones means walking the ring on every hit, and the pass that
    // spends the debt drops whatever it cannot fill anyway.
    if (isDirectHit(kind) && !fatal) {
        if (MobPetalRing* ring = world.tryGet<MobPetalRing>(victim)) {
            if (ring->pending < static_cast<int>(ring->seats.size())) ++ring->pending;
        }
    }

    if (fatal) {
        // Marked, never destroyed here. Loot, the death notice and replication
        // all still have to read this entity later in the same tick; the
        // reaper is the only thing that removes it.
        //
        // `health` dangles from this line on: adding a component relocates the
        // entity to another archetype.
        const Entity killer = attributedSource(world, source);
        world.add<Dead>(victim, Dead{killer});
        result.killed = true;
        const auto* blade = world.tryGet<PetalInstance>(source);
        const auto* species = world.tryGet<MobType>(victim);
        const auto* location = world.tryGet<Transform>(victim);
        if (blade && species && location && world.has<MobTag>(victim) && !world.has<Pet>(victim) &&
            !world.has<NpcTag>(victim) && !isBossRarity(species->rarity) &&
            petalTier(species->rarity) <= petalTier(blade->rarity) &&
            content().petal(blade->configIndex).id == "magic_void_scar" && rng_.chance(.15)) {
            if (auto* state = world.tryGet<PetalSlotState>(blade->owner); state && state->scarRecruits.size() < 4) {
                for (std::size_t i = 0; i < content().petalCount(); ++i) {
                    const auto& egg = content().petal(i);
                    if (petalIsEgg(egg) && egg.petMobIndex == species->configIndex) {
                        state->scarRecruits.push_back({blade->slot,species->configIndex,species->rarity,*location});
                        break;
                    }
                }
            }
        }
        deaths_.push_back({victim, killer, world.has<PlayerTag>(victim)});
        // A player drops nothing: the account keeps the inventory, and the
        // Died message the server sends is the whole of the consequence.
        if (!world.has<PlayerTag>(victim)) awardBounty(world, victim);
        mirrorSharedChain(world, victim, true, killer);
        collapseClearedNest(world, victim, killer);
        return result;
    }
    if(directPlayerHit && physicalMob && applied>0) {
        const auto* modifiers=world.tryGet<PlayerModifiers>(victim);
        const double heal=modifiers?modifiers->bodyHitHeal:0.0;
        if (modifiers && modifiers->bodyHitMana > 0) {
            if (auto* mana = world.tryGet<ManaPool>(victim); mana && mana->has()) {
                const auto* skills = world.tryGet<PlayerSkillTree>(victim);
                mana->current = std::min(mana->max, mana->current + modifiers->bodyHitMana *
                    (skills ? skills->skills.effectScale(SkillId::ManaRegen) : 1.0));
            }
        }
        if(heal>0)stealLife(world,victim,heal,nowMillis);
    }
    mirrorSharedChain(world, victim, false, NULL_ENTITY);
    return result;
}

DamageResult CombatSystem::shareHit(World& world, std::vector<Entity> group, Entity victim,
                                    Entity source, double amount, double nowMillis,
                                    DamageKind kind, bool& guard, bool critical) {
    // `group` arrives as a copy: the shares re-enter applyDamage, which
    // reuses the scratch list it was gathered into.
    group.push_back(victim);
    const double share = amount / static_cast<double>(group.size());
    guard = true;
    DamageResult total;
    total.refused = true;
    for (const Entity member : group) {
        const DamageResult part = applyDamage(world, member, source, share, nowMillis, kind, critical);
        total.applied += part.applied;
        total.undeadMobOverkill += part.undeadMobOverkill;
        if (!part.refused) total.refused = false;
        if (member == victim) {
            total.killed = part.killed;
            total.dodged = part.dodged;
        }
    }
    guard = false;
    return total;
}

void CombatSystem::flashStruck(World& world, Entity body, double amount, double share,
                               bool fatal) {
    if (events_ == nullptr) return;
    // A petal in one of its flower's loadout slots has a tile on that
    // flower's bar, which flashes with it. Not a lent Basic: its slot is
    // kLentPetalSlot, and the borrower's bar has no tile for it.
    Entity wearer = NULL_ENTITY;
    std::uint8_t slot = 0;
    if (const PetalInstance* petal = world.tryGet<PetalInstance>(body)) {
        if (petal->slot < kLoadoutActiveSlots) {
            wearer = petal->owner;
            slot = petal->slot;
        }
    }
    if (fatal && wearer == NULL_ENTITY) return;
    const NetId* id = world.tryGet<NetId>(body);
    const Transform* transform = world.tryGet<Transform>(body);
    if (id == nullptr || transform == nullptr) return;
    WireEvent& event = events_->damage(id->value, amount, transform->position, transform->realm,
                                       net::DamageFlashOnly, NULL_ENTITY, 0, share, body);
    event.wornBy = wearer;
    event.wornSlot = slot;
    event.wearerOnly = fatal;
}

// ---------------------------------------------------------------------------
// Shared segment health
// ---------------------------------------------------------------------------

void CombatSystem::mirrorSharedChain(World& world, Entity owner, bool fatal, Entity killer) {
    const BodySegment* head = world.tryGet<BodySegment>(owner);
    if (head == nullptr || !head->sharedHealth) return;
    const Health* pool = world.tryGet<Health>(owner);
    if (pool == nullptr) return;

    // Read the pool out before anything structural happens below, and walk the
    // chain into a list for the same reason: adding Dead to a segment moves it
    // to another archetype and invalidates every column pointer this walk
    // would otherwise still be holding.
    const double fraction = pool->fraction();
    const double flashUntil = pool->flashUntilMillis;

    chainScratch_.clear();
    Entity at = head->behind;
    for (int hop = 0; hop < kCentipedeSegmentCount && at != NULL_ENTITY; ++hop) {
        if (!world.isAlive(at)) break;
        const BodySegment* link = world.tryGet<BodySegment>(at);
        if (link == nullptr || !link->sharedHealth) break;
        chainScratch_.push_back(at);
        at = link->behind;
    }

    for (const Entity segment : chainScratch_) {
        if (Health* health = world.tryGet<Health>(segment)) {
            // By FRACTION, not by the number: the pool is the owner's bar, and
            // a segment authored with a different max would otherwise show a
            // bar that disagrees with the animal it belongs to. What the client
            // is sent is the fraction anyway.
            health->current = health->max * fraction;
            health->flashUntilMillis = std::max(health->flashUntilMillis, flashUntil);
        }
        if (!fatal || world.has<Dead>(segment)) continue;
        // One animal, one death. The segments carry no contributor ledger --
        // every swing was credited to the pool's owner -- so they pay no XP,
        // reserve no loot slot and enter nobody's kill gallery. They are the
        // body of a mob that has already paid out, and this is what takes them
        // off the map with it.
        world.add<Dead>(segment, Dead{killer});
        deaths_.push_back({segment, killer, false});
    }
}

void CombatSystem::fell(World& world, Entity victim, Entity killer) {
    if (!world.isAlive(victim) || world.has<Dead>(victim)) return;
    if (Health* health = world.tryGet<Health>(victim)) health->current = 0.0;
    world.add<Dead>(victim, Dead{killer});
    deaths_.push_back({victim, killer, world.has<PlayerTag>(victim)});
    if (!world.has<PlayerTag>(victim)) awardBounty(world, victim);
}

void CombatSystem::collapseClearedNest(World& world, Entity fallen, Entity killer) {
    const HoleTether* tether = world.tryGet<HoleTether>(fallen);
    if (tether == nullptr) return;
    const Entity nest = tether->hole;
    if (!world.isAlive(nest) || world.has<Dead>(nest)) return;
    const AmbushNest* ambush = world.tryGet<AmbushNest>(nest);
    if (ambush == nullptr || !ambush->released) return;
    for (const Entity member : ambush->brood) {
        if (member != fallen && world.isAlive(member) && !world.has<Dead>(member)) return;
    }

    // The last of them, so the nest goes with it: the same death any kill is
    // -- marked, recorded and paid off its ledger -- with the one who finished
    // the brood as the one who finished the nest. Its ledger holds every swing
    // its brood took (creditSwing), which is what the drops and the XP rank.
    //
    // `ambush` dangles from here on: adding Dead relocates the nest's row.
    world.add<Dead>(nest, Dead{killer});
    deaths_.push_back({nest, killer, false});
    awardBounty(world, nest);
}

void CombatSystem::popBubbles(World& world, const ContentRegistry& content) {
    for (const DeathRecord& death : deaths_) {
        if (death.wasPlayer || !world.isAlive(death.entity)) continue;
        const MobType* type = world.tryGet<MobType>(death.entity);
        if (type == nullptr || content.mob(type->configIndex).id != "bubble") continue;
        // The flower that popped it, whether by body, petal, shot or pet --
        // combat has already walked the credit back to the flower. A bubble
        // finished by a mob, or by a flower that fell the same tick, throws
        // nobody.
        const Entity flower = death.killer;
        if (flower == NULL_ENTITY || !world.isAlive(flower) || !world.has<PlayerTag>(flower) ||
            !isStanding(world, flower)) {
            continue;
        }
        const Transform* bubble = world.tryGet<Transform>(death.entity);
        const Transform* at = world.tryGet<Transform>(flower);
        Motion* motion = world.tryGet<Motion>(flower);
        if (bubble == nullptr || at == nullptr || motion == nullptr || at->realm != bubble->realm) {
            continue;
        }
        // Clear of the bubble, as a petal's pop throws the flower clear of the
        // petal, and added to whatever the flower was already doing.
        const Vec2 bearing = (at->position - bubble->position).normalized();
        if (bearing.lengthSq() < 1e-12) continue;
        motion->velocity +=
            bearing * shoveSpeed(kBubblePopDistance, net::kTickSeconds, /*decaysFirst=*/true);
        // A cluster stacks with the flower's existing running momentum.
    }
}

void CombatSystem::resolveMobDeathBlasts(World& world, const SpatialGrid& grid,
                                        const ContentRegistry& content, double nowMillis) {
    // Applying damage can append deaths and relocate the victim's components.
    // Read each corpse by value before visiting the grid.
    std::vector<Entity> candidates;
    std::vector<Entity> marbles;
    Query<Projectile,Transform,Dead>{world}.collect(marbles);
    for(Entity marble:marbles) {
        const auto state=world.get<Projectile>(marble);
        if (state.childShards && state.petalConfigIndex != kNoPetal && isStanding(world,state.owner)) {
            world.get<Projectile>(marble).childShards = 0;
            const auto at = world.get<Transform>(marble);
            const bool coral = content.petal(state.petalConfigIndex).id == "magic_coral";
            for (unsigned i = 0; i < state.childShards; ++i) {
                const auto angle = at.angle + kTau*(i + .5)/state.childShards;
                spawnPetalShard(world,state.owner,Transform{at.position,angle,at.realm},state.petalConfigIndex,
                                state.rarity,state.damage,0,coral && rng_.chance(.2),allocateNetId ? allocateNetId() : 0);
            }
        }
        if(state.fragmented || state.petalConfigIndex==kNoPetal ||
           content.petal(state.petalConfigIndex).id!="magic_marble" || !world.isAlive(state.owner))continue;
        world.get<Projectile>(marble).fragmented=true;
        const auto at=world.get<Transform>(marble);
        const auto glass=content.petalIndex("glass");if(glass==kNoPetal)continue;
        const auto* modifiers=world.tryGet<PlayerModifiers>(state.owner);
        const double damage=60.0*referenceOffenseScale(content.petal(state.petalConfigIndex),state.rarity)*state.effectStrength*(modifiers?modifiers->petalDamageScale:1.0);
        const auto* ownerSide=world.tryGet<Faction>(state.owner);const Faction side=ownerSide?*ownerSide:Faction{Team::Players,false};
        for(int i=0;i<3;++i) {
            const double angle=at.angle+i*kTau/3.0;Entity fragment=world.create();
            world.add<ProjectileTag>(fragment);world.add<Transform>(fragment,Transform{at.position,angle,at.realm});
            world.add<Motion>(fragment,Motion{Vec2::fromAngle(angle,180)});world.add<Body>(fragment,Body{3,1});
            world.add<Health>(fragment,Health{state.effectStrength,state.effectStrength});world.add<Faction>(fragment,side);world.add<HitCooldowns>(fragment);
            Projectile shot;shot.owner=state.owner;shot.creditTo=state.creditTo;shot.damage=damage;shot.remainingDistance=180;
            shot.petalConfigIndex=glass;shot.rarity=state.rarity;shot.lastPosition=at.position;shot.slot=state.slot;
            world.add<Projectile>(fragment,shot);world.add<Lifetime>(fragment,Lifetime{1});
            Replicated art;art.kind=net::EntityKind::Projectile;art.typeIndex=glass;art.rarity=state.rarity;
            world.add<Replicated>(fragment,art);
            if(allocateNetId)world.add<NetId>(fragment,NetId{allocateNetId()});
        }
    }
    for (std::size_t i = 0; i < deaths_.size(); ++i) {
        const DeathRecord death = deaths_[i];
        if (const auto* mine=world.tryGet<PlantedMine>(death.entity); mine && !mine->detonated) {
            const auto state=*mine;
            const auto* location=world.tryGet<Transform>(death.entity);
            if(location && world.isAlive(state.owner) && nowMillis<state.expiresAtMillis) {
                const auto origin=*location;
                world.get<PlantedMine>(death.entity).detonated=true;
                const double radius=explosionTalentRadius(world,state.owner,36.0+24.0*ladderIndex(state.rarity));
                queryBodies(grid,origin.realm,origin.position,radius,candidates);
                for(const Entity target:candidates) {
                    const auto* targetAt=world.tryGet<Transform>(target);
                    if(!isWildMob(world,target) || !targetAt ||
                       distanceSq(targetAt->position,origin.position)>radius*radius ||
                       !canHit(world,target,state.owner,nowMillis))continue;
                    const Vec2 offset=targetAt->position-origin.position;
                    auto hit=applyDamage(world,target,state.owner,300.0*referenceOffenseScale(content.petal(content.petalIndex("void_landmine")),state.rarity)*state.combinationStrength,nowMillis);
                    if(!hit.refused && !hit.dodged)applyKnockback(world,target,offset,
                        (1.05+.02*ladderIndex(state.rarity))*100.0);
                }
                if(events_)events_->explosion(origin.position,radius,origin.realm);
            }
            candidates.clear();
            continue;
        }
        if (death.wasPlayer) {
            const auto* loadout=world.tryGet<Loadout>(death.entity);
            const auto* location=world.tryGet<Transform>(death.entity);
            if(!loadout || !location)continue;
            const auto origin=*location;
            // Read the equipped slots before any recursive blast can relocate a body.
            const auto slots=loadout->slots;
            const int active=std::min(loadout->activeSlots,kLoadoutActiveSlots);
            for(int slot=0;slot<active;++slot) {
                const auto& item=slots[slot];
                if(item.empty() || content.petal(item.configIndex).id!="void_antimatter")continue;
                const double radius=explosionTalentRadius(world,death.entity,150.0+15.0*ladderIndex(item.rarity));
                const double damage=1200.0*referenceOffenseScale(content.petal(item.configIndex),item.rarity);
                candidates.clear();queryBodies(grid,origin.realm,origin.position,radius,candidates);
                for(const Entity target:candidates) {
                    const auto* position=world.tryGet<Transform>(target);
                    const auto* body=world.tryGet<Body>(target);
                    if(!position || target==death.entity || !canHit(world,target,death.entity,nowMillis))continue;
                    const double reach=radius+(body?body->radius:0.0);
                    if(distanceSq(position->position,origin.position)<=reach*reach)
                        applyDamage(world,target,death.entity,damage,nowMillis);
                }
                if(events_)events_->explosion(origin.position,radius,origin.realm);
            }
            continue;
        }
        const auto* type = world.tryGet<MobType>(death.entity);
        const auto* at = world.tryGet<Transform>(death.entity);
        const auto* body = world.tryGet<Body>(death.entity);
        if (!type || !at || !body) continue;
        const auto& mob = content.mob(type->configIndex);
        if (mob.deathBlastRadius <= 0.0) continue;
        const bool nuke = mob.id == "arctic_nuke";
        const auto* contact = world.tryGet<ContactDamage>(death.entity);
        const double damage = nuke && contact ? contact->amount :
            mob.deathBlastDamage * kMobDamageScale[rarityIndex(ladderRarity(type->rarity))];
        if (damage <= 0.0) continue;

        const Transform origin = *at;
        const double reach = explosionTalentRadius(world, death.entity,
            nuke ? mob.deathBlastRadius : body->radius + mob.deathBlastRadius);
        const bool summonedNuke = nuke && world.has<Pet>(death.entity);
        if (nuke && events_) events_->explosion(origin.position, reach, origin.realm);
        candidates.clear();
        queryBodies(grid, origin.realm, origin.position, reach, candidates);
        for (const Entity target : candidates) {
            const auto* victim = world.tryGet<Transform>(target);
            if (!victim || distanceSq(victim->position, origin.position) > reach * reach ||
                (summonedNuke && !isWildMob(world, target)) ||
                !canHit(world, target, death.entity, nowMillis)) continue;
            const Vec2 offset = victim->position - origin.position;
            const auto hit = applyDamage(world, target, death.entity, damage, nowMillis);
            if (!nuke && !hit.refused && !hit.dodged && !hit.killed)
                applyKnockback(world, target, offset, 35.0);
        }
    }
}

void CombatSystem::awardBounty(World& world, Entity victim) {
    const Bounty* bounty = world.tryGet<Bounty>(victim);
    if (bounty == nullptr || bounty->xp <= 0.0) return;

    // TypeScript ranks positive contributors and grants the mob's FULL XP to
    // each eligible looter; it does not divide one pool in damage proportion.
    // The ranking itself is the loot system's -- one rule, so the players a
    // corpse pays XP to are exactly the ones it reserves its drops for, squads
    // included.
    std::vector<Bounty::Share> shares;
    for (const Bounty::Share& share : bounty->contributors) {
        if (share.damage <= 0.0) continue;
        // Refresh the body so a player who died and rejoined still ranks on
        // the damage they already dealt (connection-keyed ledger).
        const Bounty::Share live = refreshedShare(world, share);
        if (world.has<PlayerTag>(live.player)) shares.push_back(live);
    }
    std::stable_sort(shares.begin(), shares.end(),
                     [](const Bounty::Share& a, const Bounty::Share& b) {
                         return a.damage > b.damage;
                     });
    Rarity rarity = Rarity::Common;
    if (const MobType* type = world.tryGet<MobType>(victim)) rarity = type->rarity;
    const Health* health = world.tryGet<Health>(victim);
    std::vector<Entity> recipients;
    selectLootRecipients(shares, lootSlotsForRarity(rarity), squads, recipients,
                         lootDamageFloor(rarity, health != nullptr ? health->max : 0.0));

    // The corpse's XP is not split. Hyper adjusts each recipient's share
    // from their own contribution, including players admitted by a squad.
    const double baseXp = bounty->xp * (world.has<WaveEnemy>(victim)
        ? 1.0 : mobKillXpMultiplier(rarity));
    const double maxHealth = health ? health->max : 0.0;
    for (const Entity recipient : recipients) {
        double xp = baseXp;
        if (rarity == Rarity::Apex) {
            const auto* account = world.tryGet<PlayerAccount>(recipient);
            const int tenths = hyperRewardTenths(*bounty, recipient,
                account ? account->connection : 0, maxHealth,
                account ? std::string_view(account->userId) : std::string_view{});
            xp *= tenths / 10.0;
        }
        if (std::any_of(shares.begin(), shares.end(), [recipient](const Bounty::Share& share) {
            return share.player == recipient && share.arcticEmeraldHit;
        })) {
            xp *= 2.5;
        }
        if (const auto* tree = world.tryGet<PlayerSkillTree>(recipient)) {
            xp *= tree->skills.effectScale(SkillId::Absorbing);
        }
        xp = std::round(xp);
        if (!world.has<RiftPlayer>(recipient) && !grantXp(world, recipient, xp)) continue;
        // The arena leaderboard counts the XP earned inside the ring
        // (src/server/playerManager.ts:929); a flower outside it has no score.
        if (ArenaScore* arena = world.tryGet<ArenaScore>(recipient)) arena->score += xp;
    }
}

bool CombatSystem::grantXp(World& world, Entity player, double xp) {
    PlayerProgress* progress = world.tryGet<PlayerProgress>(player);
    if (progress == nullptr || world.has<RiftPlayer>(player)) return false;
    progress->totalXp += xp;
    const int level = levelFromTotalXp(progress->totalXp).level;
    if (level == progress->level) return true;
    progress->level = level;

    // Level-up stat recalculation and full heal happen immediately in
    // addXPToPlayer(), before the death broadcast. Waiting for next tick's
    // petal pass leaves one snapshot with the new level and old health/damage.
    const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(player);
    const double healthScale = modifiers ? modifiers->maxHealthScale : 1.0;
    const Transform* at = world.tryGet<Transform>(player);
    const bool arena = at != nullptr && at->realm == Realm::Arena;
    if (Health* health = world.tryGet<Health>(player)) {
        // The ring's pool is flat (PVP_MAX_HEALTH); a level gained in it
        // still heals, as the reference's level-up does.
        health->max = arena ? kArenaMaxHealth : std::round(maxHealthForLevel(level) * healthScale);
        // Not an Undead flower, which nothing heals: it keeps its 1 HP.
        if (!world.has<Undead>(player)) health->current = health->max;
    }
    if (ContactDamage* contact = world.tryGet<ContactDamage>(player)) {
        // The worn cutter's bonus rides along, or the level-up would strip it
        // until the next petal pass folded it back on.
        contact->amount = bodyDamageForLevel(level) + (modifiers ? modifiers->bodyDamageBonus : 0.0);
    }
    return true;
}

bool CombatSystem::trySecondChance(World& world, Entity victim, double nowMillis) {
    if (!world.has<PlayerTag>(victim)) return false;
    // Skills are disabled inside the PvP arena, and friendly fire is the only
    // thing that marks a flower as being in one.
    if (const Faction* faction = world.tryGet<Faction>(victim)) {
        if (faction->friendlyFireEnabled) return false;
    }
    const PlayerSkillTree* tree = world.tryGet<PlayerSkillTree>(victim);
    if (tree == nullptr) return false;
    // {window, lockout}, and {0, 0} for a tier the talent does not define --
    // the reference's `if (!duration) return false` makes those a no-op rather
    // than an extrapolation, so a corrupt record cannot buy immortality.
    const std::array<double, 2> effect = secondChanceEffect(tree->skills.level(SkillId::SecondChance));
    if (effect[0] <= 0.0) return false;

    // ensure(), not tryGet(): a flower that has never been saved carries no
    // lockout, and the first save is exactly where one starts.
    SecondChance& lockout = world.ensure<SecondChance>(victim);
    if (nowMillis < lockout.readyAtMillis) return false;
    lockout.readyAtMillis = nowMillis + effect[1];
    lockout.activeUntilMillis = nowMillis + effect[0];

    Health& health = world.get<Health>(victim);
    health.current = 1.0;
    // The talent's window is real invulnerability -- the bar shows it -- and
    // runs alongside the 50 ms post-hit one (hitCooldownUntilMillis) rather
    // than adding to it; every tier is longer, so the talent's is what holds.
    health.invulnerableUntilMillis = std::max(health.invulnerableUntilMillis,
                                              lockout.activeUntilMillis);
    return true;
}

bool CombatSystem::tryUndead(World& world, Entity victim, Entity source, double nowMillis) {
    if (world.has<PlayerTag>(victim)) {
        const auto* visuals = world.tryGet<PlayerVisuals>(victim);
        if (visuals && visuals->corruptionBlocksBandage()) return false;
    }
    // Already Undead: death is put off until the bandage's time runs out, and
    // no blow brings it nearer. The clock is the first blow's.
    if (world.has<Undead>(victim)) {
        world.get<Health>(victim).current = 1.0;
        return true;
    }
    // A mob of an undead kind (the mummy beetle) stands up out of the blow
    // that empties it, for its tier's time, and goes on fighting.
    if (const RisesUndead* rises = world.tryGet<RisesUndead>(victim)) {
        const Undead undead{nowMillis + rises->millis, attributedSource(world, source), 0.0, 1.0};
        // `rises` dangles from here: adding the component relocates the row.
        world.add<Undead>(victim, undead);
        world.get<Health>(victim).current = 1.0;
        return true;
    }
    if (!world.has<PlayerTag>(victim)) return false;
    const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(victim);
    if (modifiers == nullptr || !(modifiers->undeadMillis > 0.0)) return false;
    const Undead undead{nowMillis + modifiers->undeadMillis, attributedSource(world, source),
                        modifiers->unrevivableMillis, modifiers->undeadBodyDamageScale, modifiers->undeadBandageTier};
    // `modifiers` dangles from here: adding the component relocates the row.
    world.add<Undead>(victim, undead);
    world.get<Health>(victim).current = 1.0;
    return true;
}

void CombatSystem::tickUndead(World& world, double nowMillis) {
    Query<Undead> walking{world};
    walking.without<Dead>();
    std::vector<Entity> expired;
    walking.each([&](Entity e, Undead& undead) {
        const auto* visuals = world.tryGet<PlayerVisuals>(e);
        const bool bandageBlocked = world.has<PlayerTag>(e) && visuals && visuals->corruptionBlocksBandage();
        if (nowMillis >= undead.untilMillis || bandageBlocked) expired.push_back(e);
    });
    for (const Entity e : expired) {
        const Undead undead = world.get<Undead>(e);
        world.remove<Undead>(e);
        // The death that was put off, to the one who dealt it -- or to
        // nobody, when they have been reaped since.
        const Entity killer = world.isAlive(undead.killer) ? undead.killer : NULL_ENTITY;
        fell(world, e, killer);
        if (!world.has<PlayerTag>(e)) {
            // Everything else a mob's killing blow does (applyDamage).
            mirrorSharedChain(world, e, true, killer);
            collapseClearedNest(world, e, killer);
        }
    }
}

// ---------------------------------------------------------------------------
// Riders: knockback, poison, slow
// ---------------------------------------------------------------------------

namespace {

/// What the knockback a body takes is multiplied by: a flower's worn rubbers,
/// 1 for everything else.
double knockbackTakenScale(const World& world, Entity e) {
    const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e);
    return modifiers != nullptr && modifiers->knockbackTakenScale > 0.0
               ? modifiers->knockbackTakenScale
               : 1.0;
}

/// What a body collision costs a body, as a multiple of the hit: a flower's
/// worn discs, 1 for everything else.
double collisionDamageTakenScale(const World& world, Entity e) {
    const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e);
    double scale = modifiers != nullptr && modifiers->collisionDamageTakenScale >= 0.0
               ? modifiers->collisionDamageTakenScale
               : 1.0;
    // Rift keeps the flat PVP pool; ambient mob contact still uses the ordinary
    // rarity ladder, so a Rare spider already exceeds 100 HP. Brake only mob
    // contact into a RiftPlayer — flower-vs-flower is unchanged.
    if (world.has<RiftPlayer>(e)) scale *= kRiftMobContactDamageScale;
    return scale;
}

} // namespace

void CombatSystem::applyKnockback(World& world, Entity victim, Vec2 offset, double strength) {
    if (!std::isfinite(strength) || strength <= 0.0) return;
    if (!world.isAlive(victim)) return;
    // Only things that move can be pushed. Testing for Motion rather than
    // adding Knockback to everything keeps a nest or a drop out of the
    // archetype churn for a push it would ignore anyway.
    if (!world.has<Motion>(victim)) return;

    const double mass = bodyMass(world.tryGet<Body>(victim));
    Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;   // exactly co-located: no direction to push along
    strength *= knockbackTakenScale(world, victim);

    // Effective knockback is the petal stat divided by mob mass.
    // Movement drains it next tick: a flower's becomes momentum its friction
    // spends (shoveSpeed), a mob's is spent over about a third of a second
    // (kKnockbackSpendDecay). Worn rubbers scale it (knockbackTakenScale,
    // above), and no second mass division is involved.
    auto& pending = world.ensure<Knockback>(victim).impulse;
    const Vec2 impulse = direction * (strength / mass);
    // Several petals can hit in one tick. A later weak contact must not erase
    // a Heavy or Jelly hit, and overlapping copies must not add their forces.
    if (impulse.lengthSq() >= pending.lengthSq()) pending = impulse;
}

namespace {

/// What a body weighs for a contact split: its Body's mass, or a flower's 1
/// for anything that has none.
double massOf(World& world, Entity e) { return bodyMass(world.tryGet<Body>(e)); }

/// A flower bounces off a mob the way gardn's does (Collision.cc
/// _cancel_movement): the approach is cancelled and reversed -- twice the
/// closing speed, floored -- plus a constant kick, all added to the flower's
/// velocity so friction spends it over the next few ticks. On top, the
/// overlap itself is taken back in proportion to the mob's share of the
/// pair's mass, which is gardn's _deal_push.
///
/// It used to move the flower 25 units in one tick. The camera is pinned to
/// the flower, so that jump shook the whole screen on every touch, and it
/// shoved a flower brushing a mob exactly as hard as one running into it.
///
/// The flower's share is stepped THROUGH THE WALLS, containment guard
/// included, like any other flower move, and whatever of it a wall refuses
/// the mob takes back, if it walks at all. A mob meets walls as a point, so it
/// walks right up to a wall with the flower in between. Written straight into
/// the position, the share was a jump of a hundred units or more there --
/// through a sewers rail in one tick, or so deep into a dirt bank that the
/// push-out found no face in reach and the flower could not walk out -- and
/// once the mob's centre had passed the pinned flower, the same push threw it
/// out the far side of the mob. Now a flower with its back to a wall holds the
/// mob off at its skin. In the open nothing is refused and the mob is not
/// moved, exactly as before. No terrain (a focused simulation) means no walls,
/// and the share is a plain displacement.
///
/// `offset` runs from the mob to the flower. Lands before the
/// damage/invulnerability branch, so an invulnerable player still bounces. A
/// plain field write, safe inside the candidate loop.
void bounceOffMob(World& world, const Terrain* terrain, Entity player, Entity mob,
                  bool mobGivesWay, Vec2 offset, double overlap) {
    Transform* transform = world.tryGet<Transform>(player);
    if (transform == nullptr) return;
    const Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;

    const double mobMass = massOf(world, mob);
    const double flowerMass = massOf(world, player);
    const double mobShare = mobMass / (flowerMass + mobMass);
    if (overlap > 0.0) {
        const double share = overlap * mobShare;
        if (terrain == nullptr) {
            transform->position += direction * share;
        } else {
            const Vec2 before = transform->position;
            const Body* body = world.tryGet<Body>(player);
            stepCollide(*terrain, transform->realm, transform->position, direction * share,
                        body != nullptr ? body->radius : kPlayerBaseRadius, 1.0, true, true);
            const Vec2 moved = transform->position - before;
            const double refused =
                share - std::max(0.0, moved.x * direction.x + moved.y * direction.y);
            Transform* mobTransform = mobGivesWay ? world.tryGet<Transform>(mob) : nullptr;
            if (refused > 0.0 && mobTransform != nullptr) {
                stepCollide(*terrain, mobTransform->realm, mobTransform->position,
                            direction * -refused, mobWallRadius(world.tryGet<Body>(mob)), 1.0, true, true);
            }
        }
    }

    Motion* motion = world.tryGet<Motion>(player);
    if (motion == nullptr) return;
    const Motion* mobMotion = world.tryGet<Motion>(mob);
    const Vec2 mobVelocity = mobMotion != nullptr ? mobMotion->velocity : Vec2{0, 0};
    const Vec2 relative = mobVelocity - motion->velocity;
    const double closing = clamp(direction.x * relative.x + direction.y * relative.y,
                                 kGardnBounceMinClosing, kGardnBounceMaxClosing);
    motion->velocity +=
        direction * ((kGardnBounceKick + 2.0 * closing) * knockbackTakenScale(world, player));
}

/// A duellist's swing shoves the other flower kMobContactKnockback units,
/// delivered as momentum for the reason bounceOffMob is: the distance is the
/// same, it just arrives over a few ticks instead of in one.
void shoveFlower(World& world, Entity player, Vec2 offset) {
    const Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;
    const double speed = shoveSpeed(kMobContactKnockback, net::kTickSeconds, /*decaysFirst=*/true) *
                         knockbackTakenScale(world, player);
    if (Motion* motion = world.tryGet<Motion>(player); motion != nullptr && speed > 0.0) {
        motion->velocity += direction * speed;
    } else if (Transform* transform = world.tryGet<Transform>(player)) {
        transform->position += direction * kMobContactKnockback;
    }
}

/// The other half of that bump, for a `gardn_ai` mob: gardn knocks the mob
/// back off the flower too, which is what makes a pursuer bounce and re-close
/// instead of burrowing into the flower it caught. Added to the velocity the
/// mob carries (see gardnStep), so it decays out over the next few ticks. A
/// plain field write, safe inside the candidate loop. `offset` runs from the
/// mob to the flower, as it does for the flower's shove.
void recoilOffFlower(World& world, Entity mob, Entity flower, Vec2 offset) {
    Motion* motion = world.tryGet<Motion>(mob);
    if (motion == nullptr) return;
    const Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;
    const double flowerMass = massOf(world, flower);
    const double flowerShare = flowerMass / (flowerMass + massOf(world, mob));
    motion->velocity -= direction * (kGardnContactRecoil * flowerShare);
}

/// The mob's half of gardn's _deal_push, for a bead of a gardn chain: the
/// flower's share of the pair's mass, times the overlap, moves the BEAD back
/// off the flower, so with the flower's own share (bounceOffMob) the two end
/// the tick just touching. Stepped through the walls as the point a mob meets
/// them as. The chain pass then pulls the bead back to its spacing from the
/// one ahead, which is what makes a flower leaning on a centipede swing its
/// body rather than stretch it. `offset` runs from the mob to the flower. A
/// plain field write, safe inside the candidate loop.
void yieldToFlower(World& world, const Terrain* terrain, Entity mob, Entity flower, Vec2 offset,
                   double overlap) {
    if (!(overlap > 0.0)) return;
    Transform* transform = world.tryGet<Transform>(mob);
    if (transform == nullptr) return;
    const Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;
    const double flowerMass = massOf(world, flower);
    const double share = overlap * flowerMass / (flowerMass + massOf(world, mob));
    if (terrain == nullptr) {
        transform->position -= direction * share;
    } else {
        stepCollide(*terrain, transform->realm, transform->position, direction * -share,
                    mobWallRadius(world.tryGet<Body>(mob)), 1.0, true, true);
    }
}

/// A glitch mob's touch -- body or shot -- leaves the flower glitched.
///
/// Infection is a property of TOUCH, not of damage: playerState.ts sets it
/// beside the contact bump and above the invulnerability branch, and
/// server.ts's projectile hook sets it before the damage it may then refuse.
/// So the callers place this outside the hit gates, and the mark lands on a
/// flower that is bouncing off the mob during respawn protection too. It is
/// never cleared here: the bit lives on PlayerVisuals until the body is
/// despawned, which is what keeps a corpse glitched, and a respawn is a fresh
/// entity. A plain field write, so it is safe inside a candidate loop that
/// must not relocate archetype rows.
void markGlitched(World& world, Entity player) {
    if (PlayerVisuals* visuals = world.tryGet<PlayerVisuals>(player)) visuals->glitched = true;
}

} // namespace

void CombatSystem::applyPoison(World& world, Entity victim, Entity source, double perSecond,
                               double durationMillis, double nowMillis, bool stackByEmitter,
                               PoisonOrigin origin) {
    if (!std::isfinite(perSecond) || perSecond <= 0.0) return;
    if (!std::isfinite(durationMillis) || durationMillis <= 0.0) return;
    // A status lands on the ANIMAL, and a shared chain is one animal: poison
    // dripped into the tail has to tick against the pool the tail draws on, or
    // it is a bite the leech never feels. See poolOwner().
    victim = poolOwner(world, victim);
    if (!world.isAlive(victim) || !world.has<Health>(victim)) return;
    if (world.has<PoisonImmune>(victim)) return;

    // Credited to the PLAYER rather than to the petal that applied it: the
    // petal is often destroyed before the poison finishes ticking, and a kill
    // whose source has been reaped would award nobody anything.
    const Entity credited = creditedPlayer(world, source);
    if (riftPortalProtected(world, victim, nowMillis) ||
        riftPortalProtected(world, credited, nowMillis)) return;
    const Entity attribution = credited != NULL_ENTITY ? credited : source;

    if (credited != NULL_ENTITY) if (const auto* tree=world.tryGet<PlayerSkillTree>(credited)) {
        perSecond *= 1.0 + tree->skills.poisonBonus();
        if(tree->skills.concentratedPoison()){perSecond*=1.2;durationMillis/=1.2;}
    }
    if (world.has<MobTag>(source) && !world.has<Pet>(source) && hyperDefending(world, victim)) {
        durationMillis *= hyperAmount(wornHyperTechnique(world, content(), victim, "lotus"),
                                      HyperTechnique::GuardLotus);
    }
    const double untilMillis = nowMillis + durationMillis;
    Afflictions& afflictions = world.ensure<Afflictions>(victim);

    // A flower carries exactly one bite, refreshed rather than accumulated:
    // playerState.ts overwrites poisonDamage and poisonUntil outright, so a
    // common mob's nip replaces a mythic's and may even shorten it.
    if (world.has<PlayerTag>(victim)) {
        afflictions.poisonPerSecond = perSecond;
        afflictions.poisonSource = attribution;
        afflictions.uraniumSelfPoison = origin == PoisonOrigin::UraniumSelf;
        afflictions.poisonUntilMillis = untilMillis;
        return;
    }

    afflictions.pruneStacks(nowMillis);
    Entity emitter = stackByEmitter ? source : NULL_ENTITY;
    if (const auto* field = stackByEmitter ? world.tryGet<GroundEffect>(source) : nullptr;
        field && field->stackedDamageEmitter != NULL_ENTITY) {
        emitter = field->stackedDamageEmitter;
    }
    const auto* field = world.tryGet<GroundEffect>(source);
    const bool critical = field && field->criticalSpill;
    auto found=std::find_if(afflictions.poisonStacks.begin(),afflictions.poisonStacks.end(),
        [&](const PoisonStack& stack){return stack.source==attribution && stack.emitter==emitter;});
    if (PoisonStack* existing = found==afflictions.poisonStacks.end()?nullptr:&*found) {
        // gardn's outlast rule: a fresh bite takes over only when it would
        // last longer, and then it carries its own rate in with it. Without
        // the guard a short weak poison stomps a long strong one; without the
        // rate coming along, a long weak one silently keeps the strong rate.
        if (existing->untilMillis < untilMillis) {
            existing->perSecond = perSecond;
            existing->untilMillis = untilMillis;
            existing->critical = critical;
        }
    } else {
        afflictions.poisonStacks.push_back({attribution, perSecond, untilMillis, emitter, critical});
    }
    summarisePoison(afflictions, nowMillis);
}

void CombatSystem::applySlow(World& world, Entity victim, double factor, double durationMillis,
                             Rarity sourceRarity, double nowMillis, bool frost, bool freeze) {
    if (!std::isfinite(factor) || factor >= 1.0) return;
    if (!std::isfinite(durationMillis) || durationMillis <= 0.0) return;
    // The head is also the only body a slow could possibly mean: the segments
    // do not steer, they trace whatever is in front of them.
    victim = poolOwner(world, victim);
    if (!world.isAlive(victim) || !world.has<Health>(victim)) return;
    // A flower is never slowed by anything. applyMobSlow() is the reference's
    // one and only slow implementation -- both the petal bridge and the web
    // field resolve to it -- and it opens by refusing anything without
    // MobKind, so no web, honey petal or pincer has ever cost a player speed.
    //
    // The guard is not academic here: a thrown web outlives the flower that
    // threw it by up to ten seconds, and an orphaned one loses its side along
    // with its owner, which makes it a hazard that hurts everything. Without
    // this it would halve the speed of whoever walked through it.
    if (!world.has<MobTag>(victim)) return;

    // stallPower() is the rarity gate: out-tiering a mob buys reliability, not
    // a deeper slow, so it scales how much of the factor lands and nothing else.
    const double power = stallPower(sourceRarity, rarityOf(world, victim));
    const double landed = clamp(1.0 - (1.0 - factor) * power, 0.0, 1.0);
    if (landed >= 1.0) return;

    Afflictions& afflictions = world.ensure<Afflictions>(victim);
    if (freeze) {
        if (nowMillis >= afflictions.freezeUntilMillis) afflictions.freezeFactor = 1.0;
        afflictions.freezeFactor = std::min(afflictions.freezeFactor, landed);
        afflictions.freezeUntilMillis = std::max(afflictions.freezeUntilMillis, nowMillis + durationMillis);
        return;
    }
    if (frost) {
        afflictions.applyFrost(landed, durationMillis, nowMillis);
        return;
    }
    if (!afflictions.slowed(nowMillis)) {
        afflictions.slowFactor = landed;
        afflictions.slowUntilMillis = nowMillis + durationMillis;
        return;
    }
    // Lower factor is a deeper slow, so min() keeps the stronger of the two.
    afflictions.slowFactor = std::min(afflictions.slowFactor, landed);
    afflictions.slowUntilMillis = std::max(afflictions.slowUntilMillis, nowMillis + durationMillis);
}

void CombatSystem::slowFlower(World& world, Entity victim, double factor,
                              double durationMillis, double nowMillis) {
    if (!std::isfinite(factor) || factor >= 1.0) return;
    if (!std::isfinite(durationMillis) || durationMillis <= 0.0) return;
    if (!world.isAlive(victim) || !world.has<PlayerTag>(victim)) return;
    // Surface webs cannot apply or refresh their slow while a flower is underground.
    if (const auto* burrow = world.tryGet<BurrowState>(victim);
        burrow && burrow->active && nowMillis < burrow->untilMillis) return;
    // No rarity gate: a flower has no tier of its own and counts as common,
    // where stallPower() is 1 for every source.
    const double landed = clamp(factor, 0.0, 1.0);

    Afflictions& afflictions = world.ensure<Afflictions>(victim);
    if (!afflictions.webbed(nowMillis)) {
        afflictions.webbedFactor = landed;
        afflictions.webbedUntilMillis = nowMillis + durationMillis;
        return;
    }
    afflictions.webbedFactor = std::min(afflictions.webbedFactor, landed);
    afflictions.webbedUntilMillis =
        std::max(afflictions.webbedUntilMillis, nowMillis + durationMillis);
}

void CombatSystem::applyArmorShred(World& world, Entity victim, double amount, double nowMillis) {
    applyArmorShred(world, victim, amount, nowMillis, 1.0);
}

void CombatSystem::applyArmorShred(World& world, Entity victim, double amount,
                                   double nowMillis, double durationScale) {
    if (!std::isfinite(amount) || amount <= 0.0) return;
    // A status lands on the ANIMAL, and a shared chain is one animal: a bur
    // that stripped only the tail it touched would strip armour nothing reads,
    // because the damage path bills the pool's owner. See poolOwner().
    victim = poolOwner(world, victim);
    if (!world.isAlive(victim) || !world.has<Health>(victim)) return;
    // Armour is a mob stat, so a strip is a mob debuff. A petal landing on
    // another flower in the arena takes nothing off it, and an orphaned bur
    // cannot follow a slow's old bug into hurting whoever walks past. An NPC
    // wears its mob's armour, so it can be stripped of it: a dummy that a bur
    // could not touch would under-report every bur build.
    if (!world.has<MobTag>(victim) && !world.has<NpcTag>(victim)) return;

    // Deepest wins, expiry never comes closer -- applySlow's rule, for the
    // reason Afflictions::armorShred gives: a strip that ACCUMULATED across
    // every contact of a spinning ring would have no bound at all.
    Afflictions& afflictions = world.ensure<Afflictions>(victim);
    if (nowMillis >= afflictions.armorShredUntilMillis) afflictions.armorShred = 0.0;
    afflictions.armorShred = std::max(afflictions.armorShred, amount);
    afflictions.armorShredUntilMillis =
        std::max(afflictions.armorShredUntilMillis, nowMillis + kArmorShredMillis * durationScale);
}

void CombatSystem::applyNoHeal(World& world, Entity victim, double durationMillis,
                               double nowMillis) {
    if (!std::isfinite(durationMillis) || durationMillis <= 0.0) return;
    // A status lands on the ANIMAL, and a shared chain is one animal: the
    // lockout is asked of whoever owns the pool, so a lock on the tail alone
    // would be a dandelion that never holds a leech down. See poolOwner().
    victim = poolOwner(world, victim);
    if (!world.isAlive(victim) || !world.has<Health>(victim)) return;

    // Longest wins and the expiry never comes closer, the rule the slow and
    // the armour strip above both run on. There is no strength to dilute --
    // healing is either locked or it is not -- so this is the whole of it.
    Afflictions& afflictions = world.ensure<Afflictions>(victim);
    afflictions.noHealUntilMillis =
        std::max(afflictions.noHealUntilMillis, nowMillis + durationMillis);
}

bool CombatSystem::healingBlocked(const World& world, Entity entity, double nowMillis) {
    // An Undead flower is dying on a clock, and nothing mends that.
    if (world.has<Undead>(entity)) return true;
    const Afflictions* afflictions = world.tryGet<Afflictions>(entity);
    return afflictions != nullptr && afflictions->healBlocked(nowMillis);
}

void CombatSystem::landHeal(World& world, Entity entity, Health& health, double amount,
                            double petalAmount) {
    if (!(amount > 0.0)) return;
    const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(entity);
    // Flowers only: every player heal path lands here. Mob recovery uses the
    // same entry without PlayerModifiers and stays at authored rates.
    if (modifiers != nullptr) amount *= kPlayerHealMultiplier;
    // A worn basil grows the heal before the bar sees it, so what spills past
    // max -- and what an amulet keeps of it -- is the grown heal's overflow.
    if (modifiers != nullptr) {
        amount *= 1.0 + modifiers->healBonus;
        if (modifiers->manaShieldFraction > 0) {
            if (auto* shield = world.tryGet<ShieldState>(entity)) {
                const double converted = std::min(std::max(0.0, health.max - shield->amount), amount * modifiers->manaShieldFraction);
                shield->amount += converted;
                amount -= converted;
            }
        }
    }
    // Heal and continuous damage cancel: each heal point first strips the same
    // amount of poison/burn DPS before any HP is restored. If DoT still exceeds
    // the heal, the leftover rate is just slowed; leftover heal then lands.
    if (Afflictions* afflictions = world.tryGet<Afflictions>(entity)) {
        auto cutRate = [&](double& rate) {
            if (!(amount > 0.0) || !(rate > 0.0)) return;
            const double cut = std::min(rate, amount);
            rate -= cut;
            amount -= cut;
        };
        if (!afflictions->poisonStacks.empty()) {
            for (PoisonStack& stack : afflictions->poisonStacks) cutRate(stack.perSecond);
            summarisePoison(*afflictions, 0.0);
        } else {
            cutRate(afflictions->poisonPerSecond);
        }
        for (BurnStack& stack : afflictions->burns) cutRate(stack.perSecond);
        cutRate(afflictions->burnPerSecond);
        if (!(amount > 0.0) && !(petalAmount > 0.0 && modifiers != nullptr &&
                                 modifiers->hemoglobinHealBonus > 0.0)) {
            return;
        }
    }
    const double hemoglobin = modifiers != nullptr
        ? petalAmount * modifiers->hemoglobinHealBonus * kPlayerHealMultiplier : 0.0;
    const double before = health.current;
    health.current = std::min(health.max, health.current + amount + hemoglobin);
    const double spill = amount - std::max(0.0, std::min(health.max - before, amount));
    if (!(spill > 0.0)) return;
    if (modifiers == nullptr || !(modifiers->overhealShield > 0.0)) return;
    // Looked up rather than ensured: every flower is built with one, and
    // adding a column here would move the row `health` points into.
    ShieldState* shield = world.tryGet<ShieldState>(entity);
    // Never lowers a bigger shield a shield petal put up.
    if (shield == nullptr || shield->amount >= health.max) return;
    shield->amount = std::min(health.max, shield->amount +
                             spill * modifiers->overhealShield * modifiers->chloroplastShieldScale);
}

double CombatSystem::effectiveArmor(const World& world, Entity victim, double nowMillis) {
    const Armor* armor = world.tryGet<Armor>(victim);
    if (armor == nullptr) return 0.0;
    const Afflictions* afflictions = world.tryGet<Afflictions>(victim);
    const double shred = afflictions != nullptr ? afflictions->shred(nowMillis) : 0.0;
    // Not clamped at zero: past it the mob takes EXTRA, which is the whole
    // reason a bur strips 1.5x what the tier it matches is wearing.
    const auto* pet = world.tryGet<Pet>(victim);
    const auto* mods = pet ? world.tryGet<PlayerModifiers>(pet->owner) : nullptr;
    return armor->amount + (mods ? mods->summonedArmor : 0.0) - shred;
}

double CombatSystem::evasionOf(const World& world, Entity victim) {
    if (const Evasion* evasion = world.tryGet<Evasion>(victim)) return evasion->chance;
    if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(victim)) {
        return modifiers->evasion;
    }
    return 0.0;
}

Entity CombatSystem::reflectionTarget(const World& world, Entity source) {
    if (source == NULL_ENTITY || !world.isAlive(source)) return NULL_ENTITY;
    Entity actor = attributedSource(world, source);
    // A seed on a mob's own ring is a piece of the mob, as a petal is of its
    // flower, but no owner link names the mob: ownerOf() never walks it.
    if (const MobRingPetal* seed = world.tryGet<MobRingPetal>(actor)) actor = seed->mob;
    if (actor == NULL_ENTITY || !world.isAlive(actor)) return NULL_ENTITY;
    // attributedSource() stops at the last LIVE link, so a shot that outlived
    // its mob names the shot itself -- and a shot is not who hit anybody.
    if (!world.has<MobTag>(actor) && !world.has<PlayerTag>(actor)) return NULL_ENTITY;
    return actor;
}

bool CombatSystem::rollDodge(World& world, Entity victim, double nowMillis, double bonus) {
    const double evasion = std::min(1.0, evasionOf(world, victim) + bonus);
    // Nothing is drawn for a victim that cannot dodge, so the stream only
    // moves when evasion is actually in the fight.
    if (!(evasion > 0.0) || !rng_.chance(evasion)) return false;
    // A miss still spends the attack. Mob contact on a flower is paced by the
    // post-hit window and nothing else, so a miss that left it shut would be
    // retried on the very next tick and a 3% dodge would dodge nothing. The
    // flower only, for the reason the shield-absorbed branch gives: a mob's
    // window is shared by every petal and every player attacking it.
    if (world.has<PlayerTag>(victim)) {
        Health& health = world.get<Health>(victim);
        health.hitCooldownUntilMillis =
            std::max(health.hitCooldownUntilMillis, nowMillis + kPostHitInvulnerabilityMillis);
    }
    return true;
}

double CombatSystem::soakIntoCotton(World& world, Entity flower, Entity source, double amount,
                                    double nowMillis, DamageKind kind) {
    const Loadout* loadout = world.tryGet<Loadout>(flower);
    if (loadout == nullptr || !(amount > 0.0)) return amount;

    // Gathered before the first is struck: a cotton the hit breaks is marked
    // Dead, which relocates rows while the ring's list would still be walked.
    //
    // One per SLOT. Every instance of a shared-pool slot mirrors the whole
    // pool, so taking a share from two of them would spend it twice; the
    // fold at the top of the next tick is what tells them apart again.
    cottonScratch_.clear();
    const auto* mana = world.tryGet<ManaPool>(flower);
    const auto magicCotton = mana && mana->has() && mana->current > 0.0
        ? content().petalIndex("magic_cotton") : kInvalidIndex;
    const bool guardBodyContact = magicCotton != kInvalidIndex && kind == DamageKind::Direct &&
        hyperDefending(world, flower) && isWildMob(world, source) && !world.has<MobRingPetal>(source);
    double damagePerMana = 0.0;
    std::array<bool, kLoadoutSlots> seen{};
    for (const Entity petal : loadout->spawned) {
        const PetalInstance* instance = world.tryGet<PetalInstance>(petal);
        if (instance == nullptr) continue;
        if (magicCotton != kInvalidIndex && instance->configIndex == magicCotton &&
            instance->slot < kLoadoutActiveSlots && !world.has<Dead>(petal)) {
            // All copies draw from the same pool. Using the best rate first
            // can already spend as much mana as the hit needs.
            double rate = magicCottonDamagePerMana(instance->rarity);
            if (guardBodyContact) {
                rate *= hyperAmount(hyperTechniqueInfo("magic_cotton", instance->rarity, instance->mimicked),
                                    HyperTechnique::ManaCotton);
            }
            damagePerMana = std::max(damagePerMana, rate);
            continue;
        }
        // Rubber catches lightning and nothing else.
        if (!instance->soaksOwnerDamage &&
            !(instance->soaksOwnerLightning && kind == DamageKind::Lightning)) {
            continue;
        }
        if (instance->slot >= kLoadoutSlots || seen[instance->slot]) continue;
        // One this tick has already broken, or one spent by being used, has
        // nothing left to catch with.
        if (world.has<Dead>(petal)) continue;
        const Health* health = world.tryGet<Health>(petal);
        if (health == nullptr || health->current <= 0.0) continue;
        seen[instance->slot] = true;
        cottonScratch_.push_back(petal);
    }

    // In ring order, each taking what it can and passing the rest on: a second
    // cotton catches what overflowed the first before any of it reaches the
    // flower.
    for (const Entity cotton : cottonScratch_) {
        if (!(amount > 0.0)) break;
        const Health* health = world.tryGet<Health>(cotton);
        if (health == nullptr || health->current <= 0.0) continue;
        const double caught = std::min(amount, health->current);
        // An ordinary hit on the petal: it flashes, it breaks, its own armour
        // applies, and the slot reloads by the one path every petal does.
        // What the FLOWER is spared is the whole of `caught` either way.
        double wear = caught;
        const auto* petal = world.tryGet<PetalInstance>(cotton);
        if (petal && petal->soaksOwnerDamage && isHyperPetal(petal->rarity) && !petal->mimicked &&
            kind == DamageKind::Direct && hyperDefending(world, flower) &&
            isWildMob(world, reflectionTarget(world, source))) {
            wear *= hyperAmount(hyperTechniqueInfo("cotton", Rarity::Apex), HyperTechnique::CottonPadding);
        }
        const DamageResult hit = applyDamage(world, cotton, source, wear, nowMillis, kind);
        if (hit.refused || hit.dodged) continue;
        amount -= caught;
    }

    // Normal Cotton may have broken and moved ECS rows. Read the mana again
    // before converting the remaining hit, spending only what is absorbed.
    if (amount > 0.0 && damagePerMana > 0.0) {
        if (auto* pool = world.tryGet<ManaPool>(flower); pool && pool->has() && pool->current > 0.0) {
            const double capacity = pool->current * damagePerMana;
            const double caught = std::min(amount, capacity);
            const double cost = std::min(pool->current, caught / damagePerMana);
            if (spendPetalMana(world, flower, squads, cost)) amount -= caught;
        }
    }
    return amount;
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void CombatSystem::run(World& world, const SpatialGrid& grid, const ContentRegistry& content,
                       double nowMillis, double dt, CommandBuffer& commands, EventQueue& events) {
    // Combat's only structural change is the Dead tag, and that one is
    // deliberately immediate rather than deferred: the loot and death passes
    // later in THIS tick are the readers, and a deferred flush lands after
    // them. Nothing here needs the command buffer.
    (void)commands;

    beginTick(world, content, nowMillis, dt, events);
    runContactPhase(world, grid, content, nowMillis);
    runWorldPhase(world, grid, content, nowMillis, dt);
}

void CombatSystem::beginTick(World& world, const ContentRegistry& content,
                             double nowMillis, double dt, EventQueue& events) {
    bind(world);
    if (!practiceDamage_.empty() && nowMillis >= nextPracticePruneMillis_) {
        nextPracticePruneMillis_ = nowMillis + 1000.0 * kPracticeSeconds;
        for (auto it = practiceDamage_.begin(); it != practiceDamage_.end();) {
            if (!world.isAlive(it->first) ||
                nowMillis - it->second.lastHitMillis >= 1000.0 * kPracticeSeconds) {
                it = practiceDamage_.erase(it);
            } else {
                ++it;
            }
        }
    }
    events_ = &events;
    deaths_.clear();
    shotLandings_.clear();
    ++tick_;

    // Standing damage first, so a mob that was already dying from poison is
    // dead before the petals that poisoned it swing again -- and its bounty is
    // paid once, by the poison, rather than to whoever happened to touch it.
    // This is the reference's order too: registerAfflictionSystems lands in
    // Phase.Combat ahead of registerMobCollisionSystem, and says so
    // (src/server/ecsRuntime.ts:651-654, "legacy ticked poison before the
    // melee pass"). Do not move it behind contact.
    tickAfflictions(world, content, nowMillis, dt);
    tickSpongeDamage(world, nowMillis, dt);
    tickShieldDecay(world, dt);
    tickUndead(world, nowMillis);
}

void CombatSystem::runContactPhase(World& world, const SpatialGrid& grid,
                                   const ContentRegistry& content, double nowMillis,
                                   const std::vector<ContactBodyPlacement>* placements, bool profile, bool sampleVisits) {
    using Clock = std::chrono::steady_clock;
    auto* timing = profile ? &contactTiming_ : nullptr;
    if (timing) {
        *timing = {};
        timing->sampleVisits = sampleVisits;
    }
    auto start = timing ? Clock::now() : Clock::time_point{};
    const auto mark = [&](double& elapsed) {
        const auto now = Clock::now();
        elapsed = std::chrono::duration<double, std::milli>(now - start).count();
        start = now;
    };
    melee_.clear();
    auras_.clear();
    // The raindrop field runs in updatePlayerPreMovement, ahead of the movement
    // window and therefore ahead of the flower's own body and ring contact. It
    // matters: a mob the field finishes off is already Dead when the ring
    // swings, so the ring neither hits it again nor is credited for it.
    gatherAuras(world, content);
    resolveAuras(world, grid, nowMillis);
    if (timing) mark(timing->aurasMs);
    gatherMeleeTargets();
    gatherContact(world, content, nowMillis);
    if (timing) mark(timing->gatherBodiesMs);
    if (placements) {
        // Preserve the full grid's pre-aura geometry and bucket order. Only
        // remove bodies that hostile sources already skip before any hit test.
        hostileTargets_.clear();
        for (const auto& body : *placements) {
            const std::size_t slot = entityIndex(body.entity);
            if (slot >= meleeTargets_.size()) continue;
            const auto& target = meleeTargets_[slot];
            if (target.entity != body.entity || target.hostileBody) continue;
            hostileTargets_.insert(body.entity, body.realm, body.position, body.radius);
        }
    }
    if (timing) mark(timing->hostileGridMs);
    gatherPetals(world, content, nowMillis);
    if (timing) mark(timing->gatherPetalsMs);
    fileShots(world);
    const auto& hostileTargets = placements ? hostileTargets_ : grid;
    if (timing) {
        mark(timing->fileShotsMs);
        timing->setupMs = timing->aurasMs + timing->gatherBodiesMs + timing->hostileGridMs +
                          timing->gatherPetalsMs + timing->fileShotsMs;
    }
    prepareMeleeCandidates(world, grid, hostileTargets, placements != nullptr, timing);
    if (timing) mark(timing->parallelSearchMs);
    resolveMelee(world, grid, content, nowMillis, hostileTargets, placements != nullptr, timing);
    // After the emitter's own body swing: the beam is a separate lightning
    // ray, and a mob finished by contact must not also take a beam tick from
    // a gather that still held a live pointer into it.
    tickLaserBeams(world, grid, content, nowMillis);
    if (!timing) return;
    mark(timing->orderedMs);
    timing->orderedMs = std::max(0.0, timing->orderedMs - timing->fallbackSearchMs);
    const auto count = [](ContactDensity& density, Vec2 position, unsigned kind) {
        ++density.total[kind];
        if (density.peak && (position - density.hotspot).lengthSq() <= kProbeHotspotRadius * kProbeHotspotRadius)
            ++density.local[kind];
    };
    if (placements) for (const auto& body : *placements) {
        const auto index = entityIndex(body.entity);
        if (index >= meleeTargets_.size()) continue;
        const auto& target = meleeTargets_[index];
        if (target.entity != body.entity) continue;
        auto& density = timing->realms[realmIndex(body.realm)];
        if (target.player) count(density, body.position, 0);
        if (target.mob) count(density, body.position, 1);
        if (target.petal) count(density, body.position, 2);
        if (target.pet) count(density, body.position, 4);
    }
    for (const auto& cell : shotCells_)
        count(timing->realms[cell.key >> 48], filedShots_[cell.slot].position, 3);
    mark(timing->densityMs);
}

void CombatSystem::tickMobLightning(World& world, const SpatialGrid& grid,
                                    const ContentRegistry& content, double nowMillis) {
    gatherMobLightning(world, content);
    resolveMobLightning(world, grid, nowMillis);
}

void CombatSystem::runWorldPhase(World& world, const SpatialGrid& grid,
                                 const ContentRegistry& content, double nowMillis, double dt,
                                 bool bodyGridOnly) {
    // Refiled after movement for the reason the server rebuilds its main grid
    // there: the shots have flown, and the mobs' own shots were fired, since
    // the contact phase filed them.
    fileShots(world);

    // TypeScript advances projectiles and then world fields after mobs move.
    // Damage fields are commutative within this phase, while projectile impact
    // remains the discrete collision whose post-movement position matters.
    // Lightning at range goes here, after the mobs have moved: a jellyfish
    // shocks from where it IS rather than from where it was, and the positions
    // the client is sent to draw arms to are the ones this tick's snapshot
    // carries. The other trigger -- a firefly's touch -- fires in the contact
    // phase above, where the collision it needs is resolved.
    tickMobLightning(world, grid, content, nowMillis);
    tickProjectiles(world, grid, content, nowMillis, dt, bodyGridOnly);
    tickGroundEffects(world, grid, content, nowMillis, dt, bodyGridOnly);
    // Last, so a bubble that went down to poison, a petal or a shot this tick
    // -- in any phase -- pops all the same.
    popBubbles(world, content);
    resolveMobDeathBlasts(world, grid, content, nowMillis);

    if (tick_ % kCooldownPruneTicks == 0) {
        queries_->cooldowns.each([&](Entity, HitCooldowns& cooldowns) { cooldowns.prune(nowMillis); });
        // The reference drops a mob's aura timestamps the moment it leaves the
        // world (forgetEnemyFromRaindropAura) because the map is keyed by mob
        // id and would otherwise grow by one entry per mob ever seen in range.
        // A deadline sweep is the same rule: every entry is stale 500 ms after
        // it was written, whether or not the mob it names still exists.
        queries_->auraCooldowns.each([&](Entity, AuraCooldowns& cooldowns) {
            cooldowns.hits.prune(nowMillis);
        });
    }

    // Not left dangling between ticks: a stray applyDamage() from a test or a
    // later system must not write into an EventQueue that has been cleared.
    events_ = nullptr;
}

static double petalFireTalentScale(const World& world, Entity player) {
    const auto* tree = world.tryGet<PlayerSkillTree>(player);
    return tree ? tree->skills.effectScale(SkillId::FirePower) : 1.0;
}

void CombatSystem::ignite(World& world, Entity victim, Entity emitter, Entity owner,
                          double rate, double duration, double now, bool fraction) {
    if (fraction) {
        const auto* health = world.tryGet<Health>(victim);
        rate *= health ? health->max : 0.0;
    }
    if (rate <= 0.0 || duration <= 0.0) return;
    const Entity player = creditedPlayer(world, owner);
    if (riftPortalProtected(world, victim, now) || riftPortalProtected(world, player, now)) return;
    if (player != NULL_ENTITY && creditedPlayer(world, victim) != player)
        rate *= petalFireTalentScale(world, player);
    auto& burns = world.ensure<Afflictions>(victim).burns;
    const auto* field = world.tryGet<GroundEffect>(emitter);
    const bool critical = field && field->criticalSpill;
    auto existing = std::find_if(burns.begin(), burns.end(),
        [emitter](const BurnStack& burn) { return burn.emitter == emitter; });
    if (existing != burns.end()) {
        existing->perSecond = rate;
        existing->untilMillis = now + duration;
        existing->owner = owner;
        existing->critical = critical;
    } else {
        const auto* field = world.tryGet<FireBombField>(emitter);
        const bool fireBomb = field && !field->independentStacks;
        if (fireBomb && std::count_if(burns.begin(), burns.end(), [&](const BurnStack& burn) {
                return burn.fireBomb && burn.owner == owner && burn.untilMillis > now;
            }) >= kFireBombBurnLayers) return;
        const auto* share = world.tryGet<ElementalOffenseShare>(emitter);
        const double weight = share ? share->value : 1.0;
        // Elemental's fragments consume only their fraction of a burn stack.
        // Retain the same saturation budget for ordinary emitters.
        if (burns.size() >= 32) {
            double occupied = 0.0;
            for (const auto& burn : burns) occupied += burn.budgetWeight;
            if (occupied + weight > 32.0 + 1e-9) return;
        }
        burns.push_back({emitter, owner, rate, now + duration, weight, fireBomb, critical});
    }
}

void CombatSystem::applyBurn(World& world, Entity victim, Entity source,
                             double perSecond, double nowMillis) {
    if (perSecond <= 0.0 || !isWildMob(world, victim) || !isStanding(world, victim)) return;
    const Entity owner = creditedPlayer(world, source);
    if (owner == NULL_ENTITY || riftPortalProtected(world, owner, nowMillis)) return;
    perSecond *= petalFireTalentScale(world, owner);
    auto& status = world.ensure<Afflictions>(victim);
    if (nowMillis < status.burnUntilMillis && perSecond < status.burnPerSecond) return;
    status.burnPerSecond = perSecond;
    status.burnSource = owner;
    status.burnUntilMillis = std::max(status.burnUntilMillis, nowMillis + 3000.0);
}

void CombatSystem::tickAfflictions(World& world, const ContentRegistry& content,
                                   double nowMillis, double dt) {
    constexpr double burnSeconds = 3.0;
    std::vector<PoisonTick> legacyBurns;
    Query<Burn, Health>{world}.without<Dead>().each([&](Entity e, Burn& burn, Health&) {
        if (burn.damage <= 0.0) return;
        const double secondsLeft = (burn.untilMillis - nowMillis) / 1000.0;
        if (secondsLeft < 0.0) {
            burn = {};
            return;
        }
        const double rate = burn.damage * (2.0 / burnSeconds) *
                      std::clamp((nowMillis - burn.startedAtMillis) / (burnSeconds * 1000.0), 0.0, 1.0);
        if (rate > 0.0) legacyBurns.push_back({e, burn.source, rate * dt});
    });
    poison_.clear();
    burns_.clear();
    queries_->afflicted.each([&](Entity e, Afflictions& afflictions, Health&) {
        auto& burns = afflictions.burns;
        burns.erase(std::remove_if(burns.begin(), burns.end(),
            [nowMillis](const BurnStack& burn) { return nowMillis >= burn.untilMillis; }), burns.end());
        for (const auto& burn : burns) burns_.push_back({e, burn.owner, burn.perSecond * dt, burn.critical});
        if (afflictions.burnPerSecond > 0.0) {
            const double remaining = afflictions.burnUntilMillis - (nowMillis - dt * 1000.0);
            const double seconds = std::clamp(remaining / 1000.0, 0.0, dt);
            if (seconds > 0.0)
                burns_.push_back({e, afflictions.burnSource, afflictions.burnPerSecond * seconds});
            if (nowMillis >= afflictions.burnUntilMillis) {
                afflictions.burnPerSecond = 0.0;
                afflictions.burnUntilMillis = 0.0;
                afflictions.burnSource = NULL_ENTITY;
            }
        }
        if (afflictions.slowFactor < 1.0 && nowMillis >= afflictions.slowUntilMillis) {
            afflictions.slowFactor = 1.0;
            afflictions.slowUntilMillis = 0;
        }
        if (afflictions.webbedFactor < 1.0 && nowMillis >= afflictions.webbedUntilMillis) {
            afflictions.webbedFactor = 1.0;
            afflictions.webbedUntilMillis = 0;
        }
        // Armour grows back. Afflictions::shred() already reads zero past the
        // expiry, so this is housekeeping rather than the rule -- it keeps a
        // mob that was stripped an hour ago from carrying the number, and keeps
        // an inspector's dump honest about what is actually on it.
        if (afflictions.armorShred != 0.0 && nowMillis >= afflictions.armorShredUntilMillis) {
            afflictions.armorShred = 0.0;
            afflictions.armorShredUntilMillis = 0;
        }
        // A mob holds one stack per poisoning player and EVERY one of them
        // ticks, so three flowers running blue_iris on the same boss deal
        // three times the damage and each is credited its own share. A single
        // slot would keep only the strongest, which loses the other two their
        // XP and loot ranking as well as their damage.
        if (!afflictions.poisonStacks.empty()) {
            afflictions.pruneStacks(nowMillis);
            for (const PoisonStack& stack : afflictions.poisonStacks) {
                if (stack.perSecond > 0.0) {
                    poison_.push_back({e, stack.source, stack.perSecond * dt, stack.critical});
                }
            }
            summarisePoison(afflictions, nowMillis);
            return;
        }

        if (afflictions.poisonPerSecond <= 0.0) return;
        if (nowMillis < afflictions.poisonUntilMillis) {
            // Poison armor is a flower's modifier, and the scalar slot is the
            // flower's path -- a mob never reaches this branch.
            if (afflictions.uraniumSelfPoison) {
                if (const auto* health = world.tryGet<Health>(e)) {
                    afflictions.poisonPerSecond = std::min(afflictions.poisonPerSecond,
                        health->max * kUraniumSelfPoisonHealthFraction);
                }
            }
            double dps = afflictions.poisonPerSecond;
            if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e)) {
                dps = std::max(0.0, dps - modifiers->poisonArmor);
            }
            if (dps > 0.0) poison_.push_back({e, afflictions.poisonSource, dps * dt});
        } else {
            afflictions.poisonPerSecond = 0;
            afflictions.poisonUntilMillis = 0;
            afflictions.poisonSource = NULL_ENTITY;
            afflictions.uraniumSelfPoison = false;
        }
    });

    // A flower gets one flat ice reduction per tick. Share it over all burn
    // sources so that stacking still preserves each attacker's contribution.
    std::unordered_map<Entity, double> iceScales;
    const auto gatherBurn = [&](const PoisonTick& burn) {
        if (burn.amount > 0.0 && world.has<Loadout>(burn.victim))
            iceScales[burn.victim] += burn.amount;
    };
    for (const auto& burn : legacyBurns) gatherBurn(burn);
    for (const auto& burn : burns_) gatherBurn(burn);
    for (auto& [victim, scale] : iceScales) {
        const auto& loadout = world.get<Loadout>(victim);
        const auto* tree = world.tryGet<PlayerSkillTree>(victim);
        const int slots = std::min(loadout.activeSlots, tree ? tree->skills.loadoutSlots() : kLoadoutActiveSlots);
        double reduction = 0.0;
        for (int slot = 0; slot < slots; ++slot) {
            const auto equipped = equippedPetal(content, loadout, slot);
            if (!equipped.empty() && content.petal(equipped.configIndex).id == "arctic_ice")
                reduction += 15.0 * petalStrengthScale(equipped.rarity) * dt;
        }
        scale = std::max(0.0, 1.0 - reduction / scale);
    }
    const auto applyBurn = [&](const PoisonTick& burn, Entity source) {
        const auto* modifiers = world.tryGet<PlayerModifiers>(burn.victim);
        const auto ice = iceScales.find(burn.victim);
        const double resistance = modifiers ? 1.0 - modifiers->fireResistance : 1.0;
        const double iceScale = ice != iceScales.end() ? ice->second : 1.0;
        applyDamage(world, burn.victim, source, burn.amount * resistance * iceScale,
                    nowMillis, DamageKind::Burn, burn.critical);
    };
    for (const auto& burn : legacyBurns) {
        applyBurn(burn, world.isAlive(burn.source) ? burn.source : NULL_ENTITY);
    }
    for (const auto& burn : burns_) {
        const auto* owner = world.tryGet<Transform>(burn.source);
        const auto* victim = world.tryGet<Transform>(burn.victim);
        const Entity source = owner && victim && owner->realm == victim->realm
            ? burn.source : NULL_ENTITY;
        applyBurn(burn, source);
    }
    for (const PoisonTick& tick : poison_) {
        // A source that has since been reaped becomes environmental damage.
        // Passing the stale handle would be worse than useless: entity slots
        // are recycled, so it could name something on the victim's own side
        // and the poison would quietly stop working.
        const Entity source = world.isAlive(tick.source) ? tick.source : NULL_ENTITY;
        // A mob's poison is reported purple and nudged sideways so it cannot
        // land under the petal hit that arrived in the same tick. A flower's
        // is reported in the ordinary colour: playerDamaged carries one number
        // for every cause, and the reference has never tinted one of them.
        const DamageKind kind = world.has<PlayerTag>(tick.victim) ? DamageKind::Periodic
                                                                  : DamageKind::Poison;
        applyDamage(world, tick.victim, source, tick.amount, nowMillis, kind, tick.critical);
    }

}

void CombatSystem::tickSpongeDamage(World& world, double nowMillis, double dt) {
    spongeTicks_.clear();
    Query<SpongeDamageState, Health> deferred{world};
    deferred.without<Dead>();
    deferred.each([&](Entity player, SpongeDamageState& state, Health& health) {
        // A new hit must not pause every earlier repayment. Only actual
        // invulnerability stops the clock; each debt has its own start time.
        if (nowMillis < health.invulnerableUntilMillis) return;

        std::size_t kept = 0;
        for (SpongeDamageEffect& effect : state.effects) {
            if (nowMillis < effect.readyAtMillis) {
                state.effects[kept++] = effect;
                continue;
            }
            const double amount = std::min(effect.remainingDamage,
                                           effect.damagePerSecond * std::max(0.0, dt));
            if (amount > 0.0) {
                spongeTicks_.push_back({player, effect.source, amount});
                effect.remainingDamage -= amount;
            }
            if (effect.remainingDamage > 0.001) state.effects[kept++] = effect;
        }
        state.effects.resize(kept);
    });

    for (const PoisonTick& tick : spongeTicks_) {
        const Entity source = world.isAlive(tick.source) ? tick.source : NULL_ENTITY;
        applyDamage(world, tick.victim, source, tick.amount, nowMillis, DamageKind::Sponge);
    }
}

void CombatSystem::tickShieldDecay(World& world, double dt) {
    // gardn's (Process/Health.cc): relative to the shield itself, so it is the
    // same ~15 s time constant on a 22-point shell as on a 500-point stack --
    // a slice of max health would wipe any shield in a second on a grown
    // flower. Continuous rather than gardn's per-tick factor so it does not
    // depend on the tick rate, and snapped to zero below half a point.
    const double keep = std::exp(-std::max(0.0, dt) / kShieldDecaySeconds);
    Query<ShieldState> shields{world};
    shields.each([&](Entity, ShieldState& shield) {
        if (shield.amount > 0.0) {
            shield.amount *= keep;
            // A petal's per-tick recovery can be smaller than the decay cutoff.
            if (!shield.gainedPassiveShieldThisTick && shield.amount < kShieldDropBelow)
                shield.amount = 0.0;
        }
        shield.gainedPassiveShieldThisTick = false;
    });
}

Entity CombatSystem::spawnPetalShard(World& world, Entity owner, const Transform& at,
                                       std::uint16_t index, Rarity rarity, double damage,
                                       std::uint8_t children, bool elemental, std::uint32_t netId, double health) {
    const auto* team = world.tryGet<Faction>(owner);
    const Faction side = team ? *team : Faction{Team::Players,false};
    const Entity shard = world.create();
    world.add<ProjectileTag>(shard);
    world.add<Transform>(shard,at);
    world.add<Motion>(shard,Motion{Vec2::fromAngle(at.angle,240)});
    world.add<Body>(shard,Body{children ? 5.0 : 3.0,1});
    world.add<Faction>(shard,side);
    world.add<Health>(shard,Health{health,health});
    world.add<HitCooldowns>(shard);
    Projectile shot;
    shot.owner = shot.creditTo = owner;
    shot.damage = damage;
    shot.remainingDistance = 180;
    shot.lastPosition = at.position;
    shot.rarity = rarity;
    shot.petalConfigIndex = index;
    shot.fragmented = true;
    shot.childShards = children;
    shot.coralElements = elemental;
    world.add<Projectile>(shard,shot);
    world.add<Lifetime>(shard,Lifetime{.75});
    Replicated art;
    art.kind = net::EntityKind::Projectile;
    art.typeIndex = index;
    art.rarity = rarity;
    world.add<Replicated>(shard,art);
    if (netId) world.add<NetId>(shard,NetId{netId});
    return shard;
}

void CombatSystem::createSpill(World& world, Entity owner, Vec2 at, Rarity rarity,
                               const SpillSpec& spill, double scale, std::uint32_t netId,
                               double durabilityScale, Rng* rng) {
    const auto* ownerAt = world.tryGet<Transform>(owner);
    if (!ownerAt || spill.kind == SpillKind::None) return;
    const Realm realm = ownerAt->realm;
    if (spill.kind == SpillKind::Elixir) scale = petalHealScale(rarity);
    GroundEffectKind kind = GroundEffectKind::AcidSpill;
    switch (spill.kind) {
        case SpillKind::Acid: break;
        case SpillKind::Lava: kind = GroundEffectKind::LavaSpill; break;
        case SpillKind::Oil: kind = GroundEffectKind::OilSpill; break;
        case SpillKind::Blood: kind = GroundEffectKind::Blood; break;
        case SpillKind::Elixir: kind = GroundEffectKind::ElixirSpill; break;
        case SpillKind::Latrine: kind = GroundEffectKind::LatrineSpill; break;
        case SpillKind::PacifyingNitrogen:
        case SpillKind::Nitrogen: kind = GroundEffectKind::NitrogenSpill; break;
        case SpillKind::None: return;
    }
    // Each puddle stacks separately. Bound the oldest fields across all
    // loadout slots so fast bomb builds cannot grow an unlimited field list.
    int count = 0;
    Entity oldest = NULL_ENTITY;
    const double lifetimeMillis = spillLifetimeMillis(spill);
    double oldestAge = -1.0;
    Query<GroundEffect, Lifetime> fields{world};
    fields.without<Dead>();
    fields.each([&](Entity e, GroundEffect& field, Lifetime&) {
        if (field.owner != owner || !isFluidField(field.kind)) return;
        ++count;
        if (field.spreadAgeSeconds > oldestAge) {
            oldest = e;
            oldestAge = field.spreadAgeSeconds;
        }
    });
    if (count >= kFluidPoolLimit) world.add<Dead>(oldest);
    const Entity e = world.create();
    world.add<GroundEffectTag>(e);
    world.add<Transform>(e, Transform{at, 0.0, realm});
    const auto* modifiers = world.tryGet<PlayerModifiers>(owner);
    const double baseRadius = kind == GroundEffectKind::Blood
        ? bloodPoolRadius(rarity) : spillBaseRadius(spill, rarity);
    const double radius = baseRadius * (modifiers ? modifiers->spillRadiusScale : 1.0);
    double rate = spill.strength * scale * fluidDamageScale(spill.kind);
    bool critical = false;
    if (modifiers && isOffensiveSpill(spill.kind)) {
        rate *= modifiers->spillDamageScale;
        // One Opal roll for the pool, including its poison or burn after the target leaves.
        if (rng && modifiers->opalTiers[petalTier(rarity)]) {
            const int tier = std::min(petalTier(rarity), rarityIndex(Rarity::Apex));
            critical = rng->chance(1.0 / (12.0 - tier));
            if (critical) rate *= 1.5 + tier * (1.5 / 8.0);
        }
    }
    GroundEffect field{kind, owner, radius * 0.24, rate, spill.frostFactor, rarity};
    field.criticalSpill = critical;
    if (kind == GroundEffectKind::Blood) {
        field.armorReduction = rate;
        field.damagePerSecond = 0.0;
    }
    field.suppressesAggro = spill.kind == SpillKind::PacifyingNitrogen;
    field.spreadStartRadius = field.radius;
    field.spreadTargetRadius = radius * (1.0 + (modifiers ? modifiers->soapCoverageBonus : 0.0));
    const double baseGrowth = spill.growthPerSecond(rarity) * (modifiers ? modifiers->spillRadiusScale : 1.0);
    field.radialGrowthPerSecond = baseGrowth * (modifiers ? modifiers->spillGrowthScale : 1.0);
    // Reserve up to the initial footprint's fraction of the local range for gradual growth.
    // Let unboosted pools reach the old hit boundary within half a second (or half their life).
    field.gradualSpreadDistance = std::min(field.spreadTargetRadius * 0.24,
                                          baseGrowth * std::min(0.5, lifetimeMillis / 2000.0));
    if (const auto* motion = world.tryGet<Motion>(owner)) {
        field.spillDrift = motion->velocity * 0.35;
        const double speed = field.spillDrift.length();
        if (speed > 160.0) field.spillDrift *= 160.0 / speed;
    }
    field.poisonDurationMillis = spill.lingerMillis;
    world.add<GroundEffect>(e, field);
    if(spill.health>0) {
        const double durability=spill.health*petalStrengthScale(rarity)*durabilityScale;
        world.add<Health>(e,Health{durability,durability});world.add<Body>(e,Body{field.radius,1.0});
        const auto* side=world.tryGet<Faction>(owner);world.add<Faction>(e,side?*side:Faction{Team::Players,false});
    }
    world.add<Lifetime>(e, Lifetime{lifetimeMillis / 1000.0});
    Replicated rep;
    rep.kind = net::EntityKind::Effect;
    rep.typeIndex = static_cast<std::uint16_t>(kind);
    rep.rarity = rarity;
    world.add<Replicated>(e, rep);
    if (netId != 0) world.add<NetId>(e, NetId{netId});
}

void CombatSystem::tickGroundEffects(World& world, const SpatialGrid& grid,
                                     const ContentRegistry& content, double nowMillis, double dt,
                                     bool bodyGridOnly) {
    struct FluidRepeller { Entity source; Realm realm; Vec2 at; double force, radius; };
    std::vector<FluidRepeller> repellers;
    bool gatheredRepellers = false;
    fields_.clear();
    queries_->fields.each([&](Entity e, GroundEffect& effect, Transform& transform) {
        if (isFluidField(effect.kind) && effect.spreadTargetRadius > 0.0) {
            if (!gatheredRepellers) {
                Query<MobTag, MobType, Transform>{world}.without<Dead>().each(
                    [&](Entity e, MobTag&, MobType& type, Transform& at) {
                        const auto& mob = content.mob(type.configIndex);
                        if (mob.id == "void_white_hole" && mob.gravityForce < 0.0 &&
                            mob.gravityRadius > 0.0 && isStanding(world, e))
                            repellers.push_back({e, at.realm, at.position, mob.gravityForce, mob.gravityRadius});
                    });
                gatheredRepellers = true;
            }
            // 0.34 s is the former visual expansion duration, now authoritative.
            constexpr double spreadSeconds = 0.34;
            const double before = std::min(effect.spreadAgeSeconds, spreadSeconds);
            effect.spreadAgeSeconds += std::max(0.0, dt);
            const double after = std::min(effect.spreadAgeSeconds, spreadSeconds);
            Vec2 acceleration{};
            bool repelled = false;
            for (const auto& field : repellers) {
                if (field.realm != transform.realm ||
                    (world.has<Pet>(field.source) && !canDamage(world, field.source, effect.owner))) continue;
                const Vec2 toward = field.at - transform.position;
                const double gap = toward.length();
                if (gap <= 1.0 || gap >= field.radius ||
                    (terrain && terrain->segmentBlocked(transform.position, field.at, transform.realm))) continue;
                acceleration += toward / gap * (field.force * (1.0 - gap / field.radius));
                repelled = true;
            }
            if (repelled) {
                effect.spillGravityVelocity += acceleration * std::max(0.0, dt);
                // Reuse the candidate's existing fluid speed ceiling, not rarity-scaled force.
                const double speed = effect.spillGravityVelocity.length();
                if (speed > 160.0) effect.spillGravityVelocity *= 160.0 / speed;
            } else effect.spillGravityVelocity = {};
            const Vec2 next = transform.position + effect.spillDrift * (after - before) +
                effect.spillGravityVelocity * std::max(0.0, dt);
            if (!terrain || !terrain->segmentBlocked(transform.position, next, transform.realm))
                transform.position = next;
            else { effect.spillDrift = {}; effect.spillGravityVelocity = {}; }
            const double fraction = after / spreadSeconds;
            const double landingRadius = effect.spreadTargetRadius - effect.gradualSpreadDistance;
            const double growth = std::min(effect.gradualSpreadDistance, effect.radialGrowthPerSecond * effect.spreadAgeSeconds);
            effect.radius = landingRadius + (effect.spreadStartRadius - landingRadius) *
                std::pow(1.0 - fraction, 3.0) + growth;
            if (auto* body = world.tryGet<Body>(e)) body->radius = effect.radius;
        }
        // In-flight or disabled fields still age; zero radius must not make
        // their Lifetime immortal.
        // A strike's burst is a pollen puff in every respect the field pass
        // cares about -- same reach rule, same rim rule, same once-per-victim
        // chip -- and differs only in the colour of the number it reports.
        const DamageKind hitKind = (world.has<LightningBurst>(e) ||
                                   effect.kind == GroundEffectKind::ElectricWeb) ? DamageKind::Lightning
                                                                : DamageKind::Direct;
        fields_.push_back({e, effect.kind, hitKind, transform.position, effect.radius,
                           effect.damagePerSecond, effect.slowFactor, effect.rarity,
                           effect.damagePerHit, effect.damageIntervalMillis, transform.realm,
                           effect.slowsFlowers, effect.owner, effect.poisonDurationMillis,
                           effect.ownerPoisonPerSecond, effect.slowDurationMillis,
                           effect.activatesAtMillis, effect.burnDamage, effect.armorReduction,
                           effect.burnPerSecond, effect.stackedDamageEmitter});
    });

    for (const FieldSource& field : fields_) {
        if (!world.isAlive(field.effect) || world.has<Dead>(field.effect)) continue;
        if (field.kind == GroundEffectKind::BlinkLanding) {
            // The marker and the AI share one landing deadline, even on slow ticks.
            const auto* ai = world.tryGet<MobAi>(field.owner);
            const auto* ownerAt = world.tryGet<Transform>(field.owner);
            if (!ai || !ownerAt || !isStanding(world, field.owner) ||
                ownerAt->realm != field.realm || nowMillis >= ai->blinkLandingMillis)
                world.add<Dead>(field.effect);
            continue;
        }
        if (isFluidField(field.kind)) {
            const auto* ownerAt = world.tryGet<Transform>(field.owner);
            if (!ownerAt || ownerAt->realm != field.realm) {
                world.add<Dead>(field.effect);
                continue;
            }
        }
        const bool breakable = world.has<Health>(field.effect);
        if(const auto* target=world.tryGet<LightningTarget>(field.effect)){
            if(world.has<Dead>(field.effect))continue;
            const Entity victim=target->victim;
            const auto* at=world.tryGet<Transform>(victim);
            if (at && at->realm == field.realm && isStanding(world, victim)) {
                const double burn = target->burnPerSecond;
                const double venom = target->poisonPerSecond;
                const auto hit = applyDamage(world, victim, field.effect, field.damagePerHit,
                                             nowMillis, DamageKind::Lightning);
                if (!hit.refused && !hit.dodged && !hit.killed) {
                    if (burn > 0.0 && (isWildMob(world, victim) || isPracticeDummy(world, content, victim))) {
                        ignite(world, poolOwner(world, victim), field.effect, field.owner,
                               burn, 3000.0, nowMillis);
                    }
                    if (venom > 0) applyPoison(world, victim, field.effect, venom, 1500, nowMillis, true);
                }
            }
            world.add<Dead>(field.effect);continue;
        }
        if (nowMillis < field.activatesAtMillis) continue;
        auto& effect = world.get<GroundEffect>(field.effect);
        const bool blast = effect.showExplosion;
        if (Lifetime* lifetime = world.tryGet<Lifetime>(field.effect)) {
            lifetime->remainingSeconds -= dt;
            if (lifetime->remainingSeconds <= 0.0 && !blast) {
                world.add<Dead>(field.effect);
                continue;
            }
        }
        if (blast) {
            if (events_) events_->explosion(field.position, field.radius, field.realm);
            effect.showExplosion = false;
            effect.damagePerHit = 0.0;
        }
        if (field.kind == GroundEffectKind::Cryo) effect.slowFactor = 1.0;
        if (field.ownerPoisonPerSecond > 0.0 && field.poisonDurationMillis > 0.0) {
            poisonFieldOwner(world, field, nowMillis);
        }
        if (field.radius <= 0.0) continue;
        if (field.damagePerSecond <= 0.0 && field.damagePerHit <= 0.0 &&
            field.slowFactor >= 1.0 && field.burnDamage <= 0.0 && field.armorReduction <= 0.0 &&
            field.burnPerSecond <= 0.0) continue;
        const bool fluid = isFluidField(field.kind);
        const bool damagingFluid = fluid && field.kind != GroundEffectKind::ElixirSpill;
        queryBodies(grid, field.realm, field.position, field.radius + kBroadphasePad, candidates_,
                    NULL_ENTITY, false, std::nullopt, nullptr, bodyGridOnly);
        const auto* ruby = world.tryGet<PlayerModifiers>(field.owner);
        int concentrated = 0;
        Entity focus = NULL_ENTITY;
        if (blast && ruby && ruby->concentratesBlasts && field.damagePerHit > 0) {
            for (Entity e : candidates_) {
                const auto* at = world.tryGet<Transform>(e);
                if (!at || at->realm != field.realm || world.has<Pet>(e) ||
                    (!world.has<MobTag>(e) && !isPracticeDummy(world, content, e)) ||
                    !canHit(world,e,field.effect,nowMillis)) continue;
                const double reach = field.radius + fieldTargetRadius(world,content,e,field.kind,world.tryGet<Body>(e));
                if (distanceSq(at->position,field.position) < reach * reach && rng_.chance(1.0 / ++concentrated)) focus = e;
            }
        }
        for (const Entity victim : candidates_) {
            if (world.has<Dead>(field.effect)) break;
            if (damagingFluid && !isWildMob(world, victim) &&
                !isPracticeDummy(world, content, victim)) continue;
            const Transform* transform = world.tryGet<Transform>(victim);
            if (transform == nullptr) continue;
            // Circle overlap, not centre-in-radius: every one of the three
            // reference fields adds the victim's own radius to its reach, so a
            // boss whose body edge is in the cloud is irradiated even though
            // its centre is 200 units outside it. A centre test shrinks the
            // effective area against a big mob by (r/(r+R))^2, which is most of
            // a web's or a puff's job.
            const Body* body = world.tryGet<Body>(victim);
            const double reach =
                field.radius + (breakable && body ? body->radius
                    : fieldTargetRadius(world, content, victim, field.kind, body));
            const double gapSq = distanceSq(transform->position, field.position);
            // The uranium pulse excludes on a strict `>` (playerState.ts:2686)
            // where pollen and web exclude on `>=` (groundEffects.ts:163, :231),
            // so a victim touching at exactly the rim is irradiated but is
            // neither dusted nor webbed.
            const bool radiation = field.kind == GroundEffectKind::Radiation ||
                field.kind == GroundEffectKind::Plutonium || field.kind == GroundEffectKind::Corium;
            const bool inside = radiation ? gapSq <= reach * reach
                                    : gapSq < reach * reach;
            if (!inside) continue;
            if (fluid && terrain &&
                terrain->segmentBlocked(field.position, transform->position, field.realm)) continue;
            if (field.kind == GroundEffectKind::ElixirSpill) {
                const auto* squad = squads ? squads->membersOf(field.owner) : nullptr;
                const bool ally = victim == field.owner || (squad && std::any_of(squad->begin(), squad->end(),
                    [victim](const SquadBody& member) { return member.body == victim; }));
                if (ally && world.has<PlayerTag>(victim) && isStanding(world, victim) &&
                    !healingBlocked(world, victim, nowMillis)) {
                    if (auto* health = world.tryGet<Health>(victim))
                        landHeal(world, victim, *health, field.damagePerSecond * dt,
                                 field.damagePerSecond * dt);
                }
                continue;
            }
            if (!canHit(world, victim, field.effect, nowMillis)) continue;
            if (damagingFluid) {
                if(field.kind==GroundEffectKind::NitrogenSpill) {
                    const Entity target=poolOwner(world,victim);
                    if (world.get<GroundEffect>(field.effect).suppressesAggro) {
                        if (auto* ai = world.tryGet<MobAi>(target)) {
                            ai->target = NULL_ENTITY;
                            ai->aggroSuppressedUntilMillis = std::max(ai->aggroSuppressedUntilMillis, nowMillis + 1500);
                        }
                        continue;
                    }
                    const double factor=1.0-(1.0-field.slowFactor)*stallPower(field.rarity,rarityOf(world,target));
                    if(factor<1) {
                        auto& status=world.ensure<Afflictions>(target);
                        if(factor<=status.frostSpeed(nowMillis)) {
                            status.frostFactor=factor;status.frostUntilMillis=nowMillis+500;
                        }
                    }
                } else if(field.kind==GroundEffectKind::LatrineSpill) {
                    applyPoison(world,victim,field.effect,field.damagePerSecond,field.poisonDurationMillis,nowMillis,true);
                } else if (field.kind == GroundEffectKind::Blood ||
                           field.kind == GroundEffectKind::BloodSpill) {
                    const double shred = field.kind == GroundEffectKind::Blood
                        ? field.armorReduction : field.damagePerSecond;
                    applyArmorShred(world, victim, shred, nowMillis);
                } else if (field.kind == GroundEffectKind::LavaSpill) {
                    ignite(world, victim, field.effect, field.owner, field.damagePerSecond,
                           field.poisonDurationMillis, nowMillis);
                    if (field.slowFactor < 1.0) {
                        const Entity target = poolOwner(world, victim);
                        const double power = stallPower(field.rarity, rarityOf(world, target));
                        const double factor = 1.0 - (1.0 - field.slowFactor) * power;
                        if (factor < 1.0) {
                            auto& status = world.ensure<Afflictions>(target);
                            if (factor <= status.frostSpeed(nowMillis)) {
                                status.frostFactor = factor;
                                status.frostUntilMillis = nowMillis + 500.0;
                            }
                        }
                    }
                } else {
                    double scale = field.kind == GroundEffectKind::OilSpill &&
                        isBurning(world, victim, nowMillis) ? 2.0 : 1.0;
                    // Acid and oil deal poison directly rather than scheduling a poison stack.
                    const Entity player = creditedPlayer(world, field.owner);
                    if (const auto* tree = world.tryGet<PlayerSkillTree>(player))
                        scale *= 1.0 + tree->skills.poisonBonus();
                    applyDamage(world, victim, field.effect, field.damagePerSecond * dt * scale,
                                nowMillis, DamageKind::Poison, effect.criticalSpill);
                }
                continue;
            }

            const bool independentDamage = field.stackedDamageEmitter != NULL_ENTITY;
            if (field.burnPerSecond > 0.0) {
                if (independentDamage || world.has<FireBombField>(field.effect)) {
                    const bool radiationBurn = independentDamage && field.kind == GroundEffectKind::Radiation;
                    if (radiationBurn || isWildMob(world, victim) || isPracticeDummy(world, content, victim))
                        ignite(world, poolOwner(world, victim),
                               independentDamage ? field.stackedDamageEmitter : field.effect, field.owner,
                               field.burnPerSecond, kFireBombBurnMillis, nowMillis);
                } else {
                    applyBurn(world, victim, field.effect, field.burnPerSecond, nowMillis);
                }
            }

            if (field.armorReduction > 0.0 && isWildMob(world, victim))
                applyArmorShred(world, victim, field.armorReduction, nowMillis);

            // A hostile NPC stands in a field like a mob does -- canHit above
            // has already turned away the friendly ones.
            const bool discreteHit = field.damagePerHit > 0.0 &&
                (world.has<MobTag>(victim) || world.has<NpcTag>(victim)) &&
                !world.has<Pet>(victim);
            if (discreteHit && (focus == NULL_ENTITY || victim == focus)) {
                const HitCooldowns* cooldowns = world.tryGet<HitCooldowns>(field.effect);
                if (cooldowns == nullptr || cooldowns->ready(victim, nowMillis)) {
                    // A discrete chip, not a drip: a puff bites once every
                    // 500 ms and a lightning burst exactly once. The reference
                    // reports both through markEnemyDamaged(), the ordinary
                    // hit channel, and credits them through creditDamage(),
                    // which provokes a neutral mob. Direct is how a C++ chip
                    // buys both of those -- the flash it also lights is what
                    // steerAggressive reads as "something just hurt me".
                    // A strong mob must not erase this low-health area petal in
                    // one touch: cap each contact at a quarter of full durability.
                    const Health* durability = world.tryGet<Health>(field.effect);
                    const double recoil = durability
                        ? std::min(contactDamageOf(world, victim), durability->max * 0.25)
                        : 0.0;
                    const DamageResult hit = applyDamage(world, victim, field.effect,
                                                         field.damagePerHit * std::clamp(concentrated,1,5), nowMillis,
                                                         field.hitKind);
                    if (!hit.refused) {
                        world.ensure<HitCooldowns>(field.effect)
                            .arm(victim, nowMillis + field.damageIntervalMillis);
                        // Use the same overlap and cadence for both sides of
                        // pollen contact. No extra tick-rate durability tax.
                        if (breakable && !hit.dodged && recoil > 0.0 &&
                            world.get<HitCooldowns>(field.effect).ready(field.effect, nowMillis)) {
                            // One wear payment per cloud interval, regardless of crowd size.
                            world.get<HitCooldowns>(field.effect).arm(
                                field.effect, nowMillis + field.damageIntervalMillis);
                            applyDamage(world, field.effect, victim, recoil,
                                        nowMillis, DamageKind::Recoil);
                        }
                    }
                }
            }
            // Mixed radiation can deliver both riders. Other fields retain
            // their original choice of a discrete hit or continuous damage.
            const bool continuousDamage = field.damagePerSecond > 0.0 &&
                (!discreteHit || radiation);
            if (continuousDamage && field.poisonDurationMillis > 0.0) {
                // Credited through the field to its owner, as a petal's bite
                // is, so a mob the cloud kills pays out to whoever wore it.
                applyPoison(world, victim, field.effect, field.damagePerSecond,
                            field.poisonDurationMillis, nowMillis, independentDamage);
            } else if (continuousDamage) {
                applyDamage(world, victim, field.effect, field.damagePerSecond * dt, nowMillis,
                            DamageKind::Periodic);
            }
            if (field.burnDamage > 0.0 && isStanding(world, victim)) {
                const Entity player = creditedPlayer(world, field.owner);
                const Entity target = poolOwner(world, victim);
                if (!isStanding(world, target) || riftPortalProtected(world, player, nowMillis)) continue;
                const double burnDamage = creditedPlayer(world, target) == player
                    ? field.burnDamage : field.burnDamage * petalFireTalentScale(world, player);
                auto& burn = world.ensure<Burn>(target);
                const bool active = burn.damage > 0.0 && nowMillis < burn.untilMillis;
                if (!active || burnDamage >= burn.damage) {
                    if (!active) burn.startedAtMillis = nowMillis;
                    burn.source = field.owner;
                    burn.damage = burnDamage;
                    burn.untilMillis = nowMillis + 3000.0;
                }
            }
            // Refreshed every tick while inside, so the linger is the tail
            // after walking out rather than the length of the debuff.
            if (field.slowsFlowers && world.has<PlayerTag>(victim)) {
                slowFlower(world, victim, field.slowFactor, kGroundEffectSlowLingerMillis,
                           nowMillis);
            } else {
                const double duration = field.slowDurationMillis > 0.0
                    ? field.slowDurationMillis : kGroundEffectSlowLingerMillis;
                applySlow(world, victim, field.slowFactor, duration, field.rarity, nowMillis);
            }
        }
    }
}

void CombatSystem::poisonFieldOwner(World& world, const FieldSource& field, double nowMillis) {
    const Entity owner = field.owner;
    if (!world.isAlive(owner) || world.has<Dead>(owner) || !world.has<PlayerTag>(owner)) return;
    // A cloud left behind in the realm the flower just walked out of is not
    // the flower's any more.
    const Transform* at = world.tryGet<Transform>(owner);
    if (at == nullptr || at->realm != field.realm) return;
    const bool uranium = world.get<GroundEffect>(field.effect).uraniumSelfPoison;
    double rate = field.ownerPoisonPerSecond;
    if (uranium) {
        if (const auto* health = world.tryGet<Health>(owner)) {
            rate = std::min(rate, health->max * kUraniumSelfPoisonHealthFraction);
        }
    }
    if (const auto* afflictions = world.tryGet<Afflictions>(owner);
        afflictions && nowMillis < afflictions->poisonUntilMillis &&
        afflictions->poisonPerSecond > rate) {
        return;
    }
    applyPoison(world, owner, NULL_ENTITY, rate, field.poisonDurationMillis, nowMillis,
                false, uranium ? PoisonOrigin::UraniumSelf : PoisonOrigin::Ordinary);
}

void CombatSystem::gatherAuras(World& world, const ContentRegistry& content) {
    // One name lookup for the whole pass. The registry is a hash map and every
    // flower would otherwise pay for it once per equipped slot.
    const std::uint16_t raindrop = content.petalIndex("raindrop");
    if (raindrop == kNoPetal) return;

    queries_->auras.each([&](Entity e, PlayerTag&, Loadout& loadout, Transform& transform) {
        // Damage and radius are maximised INDEPENDENTLY, which is what the
        // reference does: two raindrops of different tiers give the better
        // number of each rather than the better petal's pair.
        double bestDamage = 0;
        double bestRadius = 0;
        for (int i = 0; i < kLoadoutActiveSlots; ++i) {
            // Resolved, so a mimic beside a raindrop is one at the mimic's tier.
            const EquippedPetal slot = equippedPetal(content, loadout, i);
            if (slot.configIndex != raindrop) continue;
            // A broken raindrop's field switches off until the slot reloads,
            // and raindrop reloads slowly, so this is a visible outage rather
            // than a technicality.
            if (loadout.slots[static_cast<std::size_t>(i)].broken) continue;
            bestDamage = std::max(bestDamage, content.petalStats(raindrop, slot.rarity).damage);
            bestRadius = std::max(bestRadius,
                                  kRaindropAuraBaseRadius +
                                      ladderIndex(slot.rarity) * kRaindropAuraRadiusPerRarity);
        }
        if (bestRadius <= 0.0 || bestDamage <= 0.0) return;

        // The field is petal output, so it takes the petal curve -- the same
        // multiplier the ring's own contact takes, not the flower's body one.
        const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e);
        auras_.push_back({e, transform.position, bestRadius,
                          bestDamage * (modifiers != nullptr ? modifiers->petalDamageScale : 1.0),
                          transform.realm});
    });
}

void CombatSystem::resolveAuras(World& world, const SpatialGrid& grid, double nowMillis) {
    for (const AuraSource& aura : auras_) {
        if (!world.isAlive(aura.player) || world.has<Dead>(aura.player)) continue;

        grid.query(aura.realm, aura.position, aura.radius + kBroadphasePad, candidates_);
        for (const Entity victim : candidates_) {
            // Pets are not in the reference's enemy grid at all, so the field
            // sweeps wild mobs and never the flower's own summons. A hostile
            // NPC is swept like a mob; applyDamage's canHit turns away the
            // friendly ones.
            if ((!world.has<MobTag>(victim) && !world.has<NpcTag>(victim)) ||
                world.has<Pet>(victim)) {
                continue;
            }
            if (!world.isAlive(victim) || world.has<Dead>(victim)) continue;
            const Transform* transform = world.tryGet<Transform>(victim);
            if (transform == nullptr) continue;

            // Circle overlap against the mob's LIVE radius, excluding on
            // exactly touching -- the field's own test, not the pollen puff's
            // config-radius one.
            const Body* body = world.tryGet<Body>(victim);
            const double reach = aura.radius + (body != nullptr ? body->radius : 0.0);
            if (distanceSq(transform->position, aura.position) >= reach * reach) continue;

            const AuraCooldowns* armed = world.tryGet<AuraCooldowns>(aura.player);
            if (armed != nullptr && !armed->hits.ready(victim, nowMillis)) continue;
            // Stamped before the hit and regardless of what the hit does, as
            // the reference stamps it: dwelling in the field costs a chip every
            // 500 ms rather than one on every tick of contact.
            world.ensure<AuraCooldowns>(aura.player)
                .hits.arm(victim, nowMillis + kRaindropAuraDamageIntervalMillis);
            applyDamage(world, victim, aura.player, aura.damage, nowMillis);
        }
    }
}

void CombatSystem::gatherMeleeTargets() {
    std::fill(meleeTargets_.begin(), meleeTargets_.end(), MeleeTarget{});
    queries_->meleeTargets.eachArchetype([&](Archetype& layout) {
        const auto* transforms = layout.data<Transform>();
        const auto* mobTypes = layout.data<MobType>();
        MeleeTarget target;
        target.loosePetal = layout.has(componentId<LoosePetal>());
        target.petal = layout.has(componentId<PetalInstance>());
        target.player = layout.has(componentId<PlayerTag>());
        target.mob = layout.has(componentId<MobTag>());
        target.pet = layout.has(componentId<Pet>());
        target.npc = layout.has(componentId<NpcTag>());
        const Entity* entities = layout.entities();
        const Body* bodies = layout.data<Body>();
        for (std::size_t i = 0; i < layout.count(); ++i) {
            target.entity = entities[i];
            target.radius = bodies[i].radius;
            target.squareHalf = mobTypes ? squareMobHalfExtent(&mobTypes[i], target.radius) : 0.0;
            const std::size_t slot = entityIndex(target.entity);
            if (slot >= meleeTargets_.size()) meleeTargets_.resize(slot + 1);
            meleeTargets_[slot] = target;
            auto& positions = queries_->contactPositions;
            if (slot >= positions.size()) positions.resize(slot + 1);
            positions[slot] = {transforms[i].position, SIZE_MAX};
        }
    });
}

void CombatSystem::gatherContact(World& world, const ContentRegistry& content, double nowMillis) {
    queries_->contact.each([&](Entity e, ContactDamage& contact, Transform& transform, Body& body) {
        MeleeSource source;
        source.attacker = e;
        source.position = transform.position;
        source.realm = transform.realm;
        source.radius = body.radius;
        source.damage = contact.amount;
        source.hitIntervalMillis = contact.intervalMillis;
        auto& target = meleeTargets_[entityIndex(e)];
        const auto* type = world.tryGet<MobType>(e);
        source.isMobBody = target.mob;
        if (source.isMobBody && type)
            source.radius *= mobBodyContactScale(content.mob(type->configIndex).id);
        if (target.squareHalf > 0) source.radius = target.squareHalf * 1.4142135623730951;
        source.isNpcBody = target.npc;
        source.isPet = target.pet;
        source.isPlayerBody = target.player;
        if (source.isMobBody && !source.isPet && !source.isPlayerBody && !world.has<ProjectileTag>(e)) {
            const auto* side = world.tryGet<Faction>(e);
            source.isHostileBody = !side || side->team == Team::Hostiles;
            target.hostileBody = source.isHostileBody;
        }
        source.givesWay = source.isMobBody && !anchoredAgainstKnockback(world, content, e);
        // A seed of a mob's ring is a piece of that mob's body, so it hits
        // like one: the same bounce off the flower (bounceOffMob), and the
        // one-contact-per-tick rule shared with the hull, so an animal wearing
        // ten of them still only touches a flower once a tick. It brings the
        // ring PETAL's own riders along, resolved at spawn -- a dandelion's
        // seed head locks healing exactly as its loose seeds do.
        if (const MobRingPetal* seed = world.tryGet<MobRingPetal>(e)) {
            source.isMobRing = true;
            source.noHealDurationMillis = seed->noHealDurationMillis;
        }
        // A mob's contact bounces a flower gardn's way (bounceOffMob), which
        // resolveMelee applies itself; no knockback stat rides on it.
        source.knockback = 0.0;

        // The creature this body is, and at what tier: a mob's own, or the one
        // an NPC wears -- whose touch carries everything its mob's does.
        std::uint16_t configIndex = kInvalidIndex;
        Rarity rarity = Rarity::Common;
        if (type) {
            configIndex = type->configIndex;
            rarity = type->rarity;
        } else if (const Npc* npc = world.tryGet<Npc>(e)) {
            configIndex = npc->configIndex;
            rarity = npc->rarity;
        }
        if (configIndex != kInvalidIndex) {
            const MobConfig& config = content.mob(configIndex);
            const MobStats stats = content.mobStats(configIndex, rarity);
            source.poisonPerSecond = stats.poisonPerSecond;
            source.poisonDurationMillis = stats.poisonDurationMillis;
            source.rarity = rarity;
            source.glitchInfecting = config.glitchInfecting;
            source.swallows = source.isMobBody && !source.isPet &&
                              rarity == Rarity::Apex && config.id == "worm";
            source.suppressPlayerShove = config.id == "leech" || config.id == "leech_body";
            // Wild only: a pet's velocity is published raw by its own pass,
            // which would drop the recoil on the next tick anyway.
            source.gardnRecoil = source.isMobBody && !source.isPet && stats.gardnMotion;
            source.yieldsToFlower = source.givesWay && config.gardnChain;
            if (config.lightning.present && config.lightning.onContact) {
                // Past the BODY. A flower touching this mob has its centre a
                // whole body radius away, so a reach measured from the centre
                // stops reaching the very thing that triggered it as soon as
                // the mob is bigger than the reach -- which an ultra firefly
                // is. See LightningSpec::radius.
                source.lightningRadius = body.radius + config.lightning.radius;
                source.lightningDamage = lightningDamageOf(config, stats);
                source.lightningCooldownMillis = lightningCooldownOf(config);
            }
        }
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(e)) {
            source.damage *= modifiers->damageScale;
        }
        // An Undead flower's body hits for what its bandage leaves of it.
        if (const Undead* undead = world.tryGet<Undead>(e)) {
            source.damage *= undead->bodyDamageScale;
        }
        // recalculatePlayerStats() stores the flower's body damage ROUNDED and
        // every contact hit deals exactly that integer, so a level-10 flower
        // with the rare Damage talent hits for 44 rather than 44.4 and a mob
        // sitting on an exact health boundary dies on the same hit it dies on
        // in the reference. Mob contact damage is unrounded on both sides.
        if (source.isPlayerBody) source.damage = std::round(source.damage);
        if(source.isPlayerBody)if(const auto* tree=world.tryGet<PlayerSkillTree>(e))
            if(tree->skills.bodyToxicity()){source.poisonPerSecond=source.damage*0.6;source.poisonDurationMillis=1000;}

        // A body that does nothing on contact is not gathered -- but a shock IS
        // something it does, so a mob whose whole weapon is the strike still
        // reaches resolveMelee rather than being dropped for dealing no bump.
        if (source.damage <= 0.0 && source.poisonPerSecond <= 0.0 &&
            source.lightningRadius <= 0.0) {
            return;
        }
        melee_.push_back(source);
    });
}

void CombatSystem::gatherPetals(World& world, const ContentRegistry& content, double nowMillis) {
    // Clustered petals of one equipped slot share stats. The gather only
    // reads equipment, and the cache expires before combat applies any hits.
    Entity lastOwner = NULL_ENTITY;
    int lastIndex = -1;
    Rarity lastRarity = Rarity::Common;
    PetalStats stats;
    queries_->petals.each([&](Entity e, PetalInstance& petal, Transform& transform, Body& body) {
        // Spent shots leave a zero-HP ring instance until the next reload fold.
        // It must not hit once more alongside the newly released projectile.
        if(!isStanding(world,e))return;
        const PetalConfig& config = content.petal(petal.configIndex);
        if (config.noPhysics) return;   // a pure modifier has no body to hit with
        if (petal.owner != lastOwner || petal.configIndex != lastIndex || petal.rarity != lastRarity) {
            stats = computerSetStats(content, world, petal.owner, petal.configIndex, petal.rarity,
                                     content.petalStats(petal.configIndex, petal.rarity));
            lastOwner = petal.owner;
            lastIndex = petal.configIndex;
            lastRarity = petal.rarity;
        }
        // A loose petal swings at mobs only if it has DAMAGE to swing with --
        // the moon does, wax does not. Knockback alone is not enough: wax's
        // default 5 would make it a "zero swing" against every mob pressed
        // flush against it, shoving each one on every tick of contact.
        const bool loose = world.has<LoosePetal>(e);
        if (loose && stats.damage <= 0.0) return;

        const bool inert = stats.damage <= 0.0 && (!config.lightningDamage||stats.lightningDamagePerHit<=0.0) && stats.critDamage <= 0.0 &&
                           stats.poisonPerSecond <= 0.0 && stats.slowFactor >= 1.0 &&
                           stats.knockback == 0.0 && stats.armorReduction <= 0.0 &&
                           stats.noHealDurationMillis <= 0.0 && config.burnPerSecond <= 0.0;
        if (inert && config.id != "job_application" && config.id != "oxygen" &&
            config.id != "void_cyanide") return;

        MeleeSource source;
        source.attacker = e;
        source.position = transform.position;
        source.realm = transform.realm;
        source.radius = body.radius;
        if (const auto* step = world.tryGet<ThornFlightStep>(e); step && step->realm == transform.realm) {
            source.sweepDelta = step->start - source.position;
            source.sweepHalfLength = source.sweepDelta.length() * 0.5;
        }
        double offenseScale = petal.combinationStrength;
        if (config.id == "void_elemental") {
            if (const auto* share = world.tryGet<ElementalOffenseShare>(e)) offenseScale *= share->value;
        }
        source.damage = petalContactDamage(config, stats, petal.rarity) * offenseScale;
        if (config.id == "magic_void_nut") {
            const auto* progress = world.tryGet<PlayerProgress>(petal.owner);
            source.damage *= 1.0 + 2.0 * std::clamp(progress ? progress->level : 1, 0, 250) / 250.0;
        }
        if (config.id == "magic_pearl")
            source.damage *= 1.0 + std::clamp(petal.flightVelocity.length() / kPearlLaunchSpeed, 0.0, 1.0);
        if (config.id == "magic_void_trident") {
            const auto* motion = world.tryGet<Motion>(petal.owner);
            source.damage *= 1.0 + 4.0 * std::clamp((motion ? motion->velocity.length() : 0.0) / (20.0 * kPlayerMaxSpeed), 0.0, 1.0);
        }
        if (config.id == "magic_void_scythe") source.damage += petal.scytheManaDamage * offenseScale;
        if (const auto* mods = world.tryGet<PlayerModifiers>(petal.owner); mods && petalTier(petal.rarity) <= mods->magicOpalTier)
            if (const auto* roll = world.tryGet<MagicOpalRoll>(petal.owner)) source.damage *= roll->multiplier;
        if (config.id == "magic_claw") {
            const auto* pool = world.tryGet<ManaPool>(petal.owner);
            const double fraction = pool && pool->has() ? std::clamp(pool->current / pool->max, 0.0, 1.0) : 0.0;
            source.damage *= 5.0 * fraction;
        }
        if (config.id == "magic_bone") {
            if (const auto* mods = world.tryGet<PlayerModifiers>(petal.owner))
                source.wildDamageScale *= 1.0 + mods->magicBoneDamageBonus[ladderIndex(petal.rarity)];
        }
        if (!petal.mimicked) {
            source.wildDamageScale *= stats.wildLightningScale;
            source.wildRecoilScale = stats.wildRecoilScale;
        }
        source.hitIntervalMillis = stats.damageIntervalMillis;
        source.petalBreakable = stats.breakable;
        source.jellyKnockback = config.id == "jelly";
        source.knockback = stats.knockback;
        source.poisonPerSecond = stats.poisonPerSecond * offenseScale;
        source.poisonDurationMillis = stats.poisonDurationMillis;
        source.slowFactor = stats.slowFactor;
        source.slowDurationMillis = stats.slowDurationMillis;
        source.inflictsFrost = config.id == "arctic_frost" || config.id == "void_elemental";
        source.inflictsFreeze = config.id == "arctic_icicle";
        source.noHealDurationMillis = stats.noHealDurationMillis;
        source.armorReduction = config.id == "blood" ? 0.0 : stats.armorReduction;
        source.armorDamageFraction = config.armorDamageFraction;
        if (config.id != "magic_fire")
            source.burnPerSecond = petalBaseBurnRate(config, petal.rarity) * offenseScale;
        if (config.id == "void_fire") {
            if (const auto* mods = world.tryGet<PlayerModifiers>(petal.owner))
                source.burnPerSecond *= mods->petalDamageScale;
        }
            if (config.id == "torch") {
                if (const auto* tech = hyperTechniqueInfo("torch", petal.rarity, petal.mimicked);
                    tech && tech->technique == HyperTechnique::TorchWildfire)
                    source.burnPerSecond *= tech->amount;
            }
        source.burnMillis = config.burnMillis;
        if (config.spill.onHit && config.spill.kind != SpillKind::None) source.impactSpill = &config.spill;
        source.critDamage = stats.critDamage * offenseScale;
        if (config.id == "dice" || config.id == "magic_dice") {
            const auto* tree = world.tryGet<PlayerSkillTree>(petal.owner);
            const double luck = tree ? tree->skills.luck() : 0.0;
            const bool magic = config.id == "magic_dice";
            source.critChance = (magic ? 0.02 : 0.05) + std::clamp(luck / kLuckTalent.back(), 0.0, 1.0) * (magic ? 0.005 : 0.02);
            source.critMultiplier = magic ? 1000.0 : 35.0;
        }
        if (const auto* modifiers = world.tryGet<PlayerModifiers>(petal.owner);
            modifiers && modifiers->opalTiers[petalTier(petal.rarity)] && source.critChance == 0.0) {
            const int tier = std::min(petalTier(petal.rarity), rarityIndex(Rarity::Apex));
            source.critChance = 1.0 / (12.0 - tier);
            source.critMultiplier = 1.5 + tier * (1.5 / 8.0);
        }
        if (config.id == "domino") source.damage *= dominoDamageMultiplier(petal.dominoPips, petal.rarity);
        if (petal.cardSuit != 0) source.damage *= cardDamageScale(petal.cardSuit);
        source.lifesteal = stats.lifesteal;
        source.selfDamage = petalContactSelfDamage(config, petal.rarity);
        source.owner = petal.owner;
        source.rarity = petal.rarity;
        source.isPetal = true;
        source.shortensUndead = config.id == "void_scar";
        source.isLoose = loose;
        if (config.armorPiercing) source.hitKind = DamageKind::Piercing;
        if (config.lightningDamage) source.hitKind = DamageKind::Lightning;
        // A tomato hits harder the longer it has been out; a reload starts it
        // over, since the reload is a new petal.
        if (petalGrowsOverTime(config)) {
            source.damage *= tomatoGrowth(nowMillis - petal.spawnedAtMillis);
        } else if (config.id == "magic_tomato" && petal.magicTomatoRipe) {
            source.damage *= kMagicTomatoGrowth;
        }
        if (petalShredsShots(config)) source.shotDamageScale = kPlankShotDamageScale;
        // The flower's damage bonus is a property of the flower, not of the
        // petal entity, so it is read here rather than baked in at spawn --
        // swapping a damage petal in must affect the ring on the same tick.
        //
        // The PETAL curve, not the body one. getDamageMultiplier() puts
        // everything a petal does -- ring contact, a shot, a puff, a pulse --
        // on the steep effect table, where a body slam takes the gentle stat
        // table; sharing one factor costs a fully talented ring most of its
        // damage.
        //
        // A claw's bonus is petal damage too, and takes the same curve.
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(petal.owner)) {
            if (config.id == "triangle")
                source.damage += triangleBonusDamage(source.damage, petal.rarity, modifiers->triangleCounts);
            source.damage *= modifiers->petalDamageScale;
            if ((source.hitKind == DamageKind::Direct || source.hitKind == DamageKind::Piercing))
                source.damage *= modifiers->jurassicDirectDamageScale;
            source.critDamage *= modifiers->petalDamageScale;
        }
        melee_.push_back(source);
    });
}

bool CombatSystem::eligibleMeleeCandidate(const MeleeSource& source, Entity victim) const {
    const auto slot = entityIndex(victim);
    if (victim == source.attacker || slot >= meleeTargets_.size()) return false;
    const auto& target = meleeTargets_[slot];
    if (target.entity != victim || (source.isHostileBody && target.hostileBody)) return false;
    if (target.player && (source.isLoose || (source.isPlayerBody && !source.isPetal))) return false;
    // Rings have their own swings; loose petals can only take body contact.
    return source.isPetal || !target.petal ||
           (target.loosePetal && (source.isMobBody || source.isMobRing || source.isNpcBody));
}

void CombatSystem::prepareMeleeCandidates(const World& world, const SpatialGrid& grid,
                                          const SpatialGrid& hostileTargets, bool bodyGridOnly, ContactTiming* timing) {
    const unsigned limit = workers ? std::min(workerLimit, workers->count()) : 0;
    if (!limit || melee_.size() < 256) {
        meleePlans_.clear();
        return;
    }

    // Flower bumps and PvP can move a flower at any point. A mob can give
    // ground during its own swing. Remember when later plans may need a
    // live query. Knockback impulses themselves are integrated next tick.
    for (std::size_t i = 0; i < melee_.size(); ++i) {
        const auto& source = melee_[i];
        if (source.givesWay || source.yieldsToFlower) {
            auto& first = queries_->contactPositions[entityIndex(source.attacker)].firstPossibleShove;
            first = std::min(first, i);
        }
    }
    constexpr std::size_t kBatchSize = 24;
    constexpr std::size_t kMaxCandidatesPerLane = 128 * 1024;
    // The gathered sources already bound the number of plan headers. Bound
    // the variable-size candidate data per lane, so a large active world can
    // still distribute sparse queries for the last sources in simulation order.
    meleePlans_.resize(melee_.size());
    queries_->omittedMovers.resize(melee_.size());
    meleeScratch_.resize(limit + 1);
    for (auto& scratch : meleeScratch_) scratch.planned.clear();
    workers->run((meleePlans_.size() + kBatchSize - 1) / kBatchSize, limit,
                 [&](std::size_t batch, unsigned lane) {
        auto& scratch = meleeScratch_[lane];
        const auto end = std::min((batch + 1) * kBatchSize, meleePlans_.size());
        for (std::size_t i = batch * kBatchSize; i < end; ++i) {
            const auto& source = melee_[i];
            auto& plan = meleePlans_[i];
            plan.ready = false;
            if (!world.isAlive(source.attacker) || world.has<Dead>(source.attacker)) continue;
            if (scratch.planned.size() == kMaxCandidatesPerLane) continue;
            const auto& shotBounds = shotBounds_[realmIndex(source.realm)];
            const bool hasShots = shotBounds.firstRow <= shotBounds.lastRow;
            plan.player = hasShots ? creditedPlayer(world, source.attacker) : NULL_ENTITY;
            plan.excludePeaceful = hasShots &&
                                   peacefulPlayerSide(world, source.attacker, plan.player);
            queryBodies(source.isHostileBody ? hostileTargets : grid, source.realm,
                        source.position + source.sweepDelta * 0.5, source.radius + source.sweepHalfLength + kBroadphasePad, scratch.candidates,
                        plan.player, plan.excludePeaceful, source.radius, &scratch.grid, bodyGridOnly);
            if (timing) plan.raw = scratch.candidates.size();
            // A packed per-lane buffer keeps dense sources from paying for
            // another query merely because their own list is over 192.
            auto& omitted = queries_->omittedMovers[i];
            omitted = 0;
            const bool roundSource = meleeTargets_[entityIndex(source.attacker)].squareHalf == 0 &&
                                     source.radius >= 0;
            plan.begin = scratch.planned.size();
            plan.lane = lane;
            bool full = false;
            for (Entity victim : scratch.candidates) {
                if (!eligibleMeleeCandidate(source, victim)) continue;
                const auto slot = entityIndex(victim);
                const auto& target = meleeTargets_[slot];
                const auto& initial = queries_->contactPositions[slot];
                if (roundSource && !target.player && target.squareHalf == 0 && target.radius >= 0) {
                    // Both optional contact slacks are included. Rounding
                    // outward leaves the exact tangency to the live resolver.
                    const double reach = source.radius + target.radius +
                                         kMobCollisionBuffer + kNpcTouchSlack + 1e-6;
                    if ((initial.position - source.position).lengthSq() > reach * reach) {
                        if (i >= initial.firstPossibleShove) omitted |= UINT64_C(1) << (slot % 64);
                        continue;
                    }
                }
                if (scratch.planned.size() == kMaxCandidatesPerLane) {
                    scratch.planned.resize(plan.begin);
                    full = true;
                    break;
                }
                scratch.planned.push_back(victim);
            }
            plan.end = scratch.planned.size();
            plan.ready = !full;
        }
    });
}

void CombatSystem::resolveMelee(World& world, const SpatialGrid& grid, const ContentRegistry& registry,
                                double nowMillis, const SpatialGrid& hostileTargets, bool bodyGridOnly, ContactTiming* timing) {
    using Clock = std::chrono::steady_clock;
    using Phase = ContactVisitPhase;
    struct VisitSample {
        ContactVisitTiming* timing;
        Phase phase = Phase::Lookup;
        double start;

        explicit VisitSample(ContactVisitTiming* timing)
            : timing(timing), start(timing ? contactSampleMillis() : 0.0) {}

        void mark(Phase next) {
            if (!timing) return;
            const double now = contactSampleMillis();
            const auto index = static_cast<std::size_t>(phase);
            timing->ms[index] += now - start;
            ++timing->intervals[index];
            start = now;
            phase = next;
        }

        ~VisitSample() {
            mark(Phase::Count);
            if (timing) ++timing->visits;
        }
    };
    // Rotate the sample across ticks and continue the stride across sources,
    // including those with fewer candidates than the stride.
    std::size_t sampleOffset = tick_ * 37;
    const bool sampleVisits = timing && timing->sampleVisits;
    if (sampleVisits) {
        constexpr int reads = 32;
        const double start = contactSampleMillis();
        double end = start;
        for (int i = 0; i < reads; ++i) end = contactSampleMillis();
        timing->sampleClockNs = (end - start) * 1e6 / reads;
    }
    // A wall can push a mob back during its swing. Any later plan that may
    // have dropped that mob must query live candidates in the original order.
    // Mask collisions only cause additional live queries.
    std::array<std::uint64_t, kMaxRealms> displaced{};
    std::vector<Entity> flowerSharedPoolsHit;
    std::vector<Entity> beanNeighbors;
    for (std::size_t i = 0; i < melee_.size(); ++i) {
        const MeleeSource& source = melee_[i];
        if (!world.isAlive(source.attacker) || world.has<Dead>(source.attacker)) continue;
        if (source.isPlayerBody) flowerSharedPoolsHit.clear();

        // The grid files each entity under every cell its own radius touches,
        // so a query at the attacker's radius already returns everything whose
        // circle could overlap; the exact test below is the one that decides.
        // Resolved once per source, for the shots below: the answer canDamage()
        // would reach for every one of them.
        const auto& shotBounds = shotBounds_[realmIndex(source.realm)];
        const bool hasShots = shotBounds.firstRow <= shotBounds.lastRow;
        const Entity sourcePlayer = hasShots ? creditedPlayer(world, source.attacker) : NULL_ENTITY;
        const double sourceHalf = squareMobHalfExtent(world, source.attacker);
        // Two peaceful player-side entities cannot damage each other.
        // Skip whole cells of allied shots around a crowded boss.
        const bool excludePeaceful = hasShots &&
                                     peacefulPlayerSide(world, source.attacker, sourcePlayer);
        const MeleePlan* plan = i < meleePlans_.size() ? &meleePlans_[i] : nullptr;
        const bool usePlan = plan && plan->ready && plan->player == sourcePlayer &&
                             plan->excludePeaceful == excludePeaceful &&
                             !(queries_->omittedMovers[i] & displaced[realmIndex(source.realm)]);
        if (!usePlan) {
            const auto started = timing ? Clock::now() : Clock::time_point{};
            queryBodies(source.isHostileBody ? hostileTargets : grid,
                        source.realm, source.position + source.sweepDelta * 0.5, source.radius + source.sweepHalfLength + kBroadphasePad,
                        candidates_, sourcePlayer, excludePeaceful, source.radius, nullptr, bodyGridOnly);
            if (timing) timing->fallbackSearchMs +=
                std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        }
        // Only the candidate search is parallel. Resolve from live transforms
        // and health in the original source and target order.
        const auto& candidates = usePlan ? meleeScratch_[plan->lane].planned : candidates_;
        const auto first = usePlan ? plan->begin : std::size_t{0};
        const auto last = usePlan ? plan->end : candidates.size();
        double beanDamageScale = 1.0;
        if (first != last && source.isPetal) {
            const auto* petal = world.tryGet<PetalInstance>(source.attacker);
            const auto* owner = world.tryGet<Transform>(source.owner);
            if (petal && owner && registry.petal(petal->configIndex).id == "bean") {
                constexpr double allyRange = 220.0;
                grid.query(owner->realm, owner->position, allyRange, beanNeighbors);
                const bool hasAlly = std::any_of(beanNeighbors.begin(), beanNeighbors.end(), [&](Entity e) {
                    if (e == source.owner || !world.has<PlayerTag>(e) || !isStanding(world, e) ||
                        oneConnection(world, source.owner, e) || canDamage(world, source.owner, e)) return false;
                    const auto* at = world.tryGet<Transform>(e);
                    return at && at->realm == owner->realm &&
                           distanceSq(at->position, owner->position) <= allyRange * allyRange;
                });
                if (!hasAlly) beanDamageScale = 1.5;
            }
        }
        std::size_t overlaps = 0;
        bool ricochetHit = false;
        bool checkedShove = false;
        std::size_t at = first;
        for (; at < last; ++at) {
            if (ricochetHit) break;
            VisitSample sample(sampleVisits && (sampleOffset++ % kContactVisitSampleStride == 0)
                ? &timing->realms[realmIndex(source.realm)].sampled : nullptr);
            const Entity victim = candidates[at];
            if (!usePlan && !eligibleMeleeCandidate(source, victim)) continue;
            MeleeTarget& targetBody = meleeTargets_[entityIndex(victim)];
            const Transform* transform = world.tryGet<Transform>(victim);
            if (transform == nullptr) continue;

            if (source.isPetal && targetBody.player) {
                // Flower vs flower is a wholly separate collision in the
                // reference -- applyPvpDamage() has exactly one caller, and it
                // is the petal loop -- with its own gate, its own throttle and
                // its own cost to the petal.
                sample.mark(Phase::Pvp);
                const auto* instance = world.tryGet<PetalInstance>(source.attacker);
                if (instance && uprightVoidWeapon(registry.petal(instance->configIndex).id) &&
                    !uprightWeaponTouches(transform->position - source.position, source.radius,
                        referencePetalHalfWidth(registry.petal(instance->configIndex).id, instance->rarity)
                            * source.radius / referencePetalLengthRadius(registry.petal(instance->configIndex).id, instance->rarity), targetBody.radius)) continue;
                resolvePetalPvp(world, source, victim, transform->position, targetBody.radius,
                                nowMillis, beanDamageScale);
                continue;
            }
            // A pet and a wild mob meet in the separation kernel's contact
            // list, which is built with the same gap the kernel keeps between
            // bodies rather than at exact circle overlap.
            sample.mark(Phase::Geometry);
            const bool petPair = source.isMobBody && targetBody.mob && source.isPet != targetBody.pet;

            // A flower and an NPC meet flush, never overlapping: the flower
            // was put back out against the NPC's skin before this pass ran,
            // so touching is measured with kNpcTouchSlack on top. Either way
            // round -- the NPC's bite and the flower's body slam alike.
            // A mob and a loose petal get the slack too: the movement pass
            // eases them apart, and a pair that has stopped pressing settles
            // against each other's skin rather than inside it.
            const bool flushPair = (source.isNpcBody && targetBody.player) ||
                                   (source.isMobBody && !source.givesWay && targetBody.player) ||
                                   ((source.isPlayerBody || source.isPet) && targetBody.npc) ||
                                   (targetBody.loosePetal && !source.isPetal) || source.isLoose;

            Vec2 contactCentre = source.position;
            if (source.sweepHalfLength > 0 && (targetBody.mob || targetBody.npc)) {
                const Vec2 rel = transform->position - source.position;
                const double along = (rel.x * source.sweepDelta.x + rel.y * source.sweepDelta.y) /
                                     source.sweepDelta.lengthSq();
                contactCentre += source.sweepDelta * std::clamp(along, 0.0, 1.0);
            }
            const Vec2 offset = transform->position - contactCentre;
            const double reach = source.radius + targetBody.radius +
                                 (petPair ? kMobCollisionBuffer : 0.0) +
                                 (flushPair ? kNpcTouchSlack : 0.0);
            const double targetHalf = targetBody.squareHalf;
            if(sourceHalf>0 || targetHalf>0) {
                const auto* sourceAt=world.tryGet<Transform>(source.attacker);
                if(shapedBodyContact(contactCentre,source.radius,sourceHalf,sourceAt?sourceAt->angle:0,
                    transform->position,targetBody.radius,targetHalf,transform->angle,
                    (petPair?kMobCollisionBuffer:0)+(flushPair?kNpcTouchSlack:0)).depth<0) continue;
            } else if (offset.lengthSq() > reach * reach) continue;
            const auto insideJet=[&](Entity jet,Vec2 centre,double radius,Vec2 target,double targetRadius) {
                const auto* instance=world.tryGet<PetalInstance>(jet);
                if(!instance)return true;
                const double halfWidth=referencePetalHalfWidth(registry.petal(instance->configIndex).id,instance->rarity);
                if(halfWidth<=0)return true;
                if (uprightVoidWeapon(registry.petal(instance->configIndex).id))
                    return uprightWeaponTouches(target-centre, radius, halfWidth * radius / referencePetalLengthRadius(registry.petal(instance->configIndex).id, instance->rarity), targetRadius);
                const double span=std::max(0.0,radius-halfWidth);
                const Vec2 axis=Vec2::fromAngle(instance->facingAngle,1.0),relative=target-centre;
                const double along=std::clamp(relative.x*axis.x+relative.y*axis.y,-span,span);
                return (relative-axis*along).lengthSq()<=(halfWidth+targetRadius)*(halfWidth+targetRadius);
            };
            if(!insideJet(source.attacker,source.position,source.radius,transform->position,targetBody.radius) ||
               !insideJet(victim,transform->position,targetBody.radius,source.position,source.radius))continue;


            if (timing) ++overlaps;

            // A summoned firefly discharges on the tick its body reaches a wild
            // mob, as a wild one does on reaching a flower -- above the body's
            // own hit, for the reason given there.
            if (petPair && source.isPet && source.lightningRadius > 0.0) {
                sample.mark(Phase::Writeback);
                dischargeOnContact(world, grid, source, nowMillis);
            }
            sample.mark(Phase::Eligibility);

            // A TypeScript mob bump is independent of damage: it still lands
            // during respawn invulnerability, is not throttled by the damage
            // cooldown, and occurs before a lethal hit is handled. A hostile
            // NPC's body bumps exactly as its mob's does.
            if (const auto* ghost = ghostBodyOrRing(world, source.attacker)) {
                const auto* visuals = world.tryGet<PlayerVisuals>(creditedPlayer(world, victim));
                if (!visuals || !visuals->canSeeGhost(ghost->rarity)) continue;
            }
            const bool mobTouchesPlayer = (source.isMobBody || source.isMobRing ||
                                           source.isNpcBody) &&
                                          targetBody.player &&
                                          !world.has<Dead>(victim) &&
                                          world.has<Health>(victim) &&
                                          canDamage(world, source.attacker, victim);
            if (mobTouchesPlayer) {
                const auto* burrow = world.tryGet<BurrowState>(victim);
                if (burrow && burrow->active && nowMillis < burrow->untilMillis) {
                    const auto* ring = world.tryGet<MobRingPetal>(source.attacker);
                    const Entity observer = ring ? ring->mob : source.attacker;
                    if (!canNoticeBurrowed(world, observer, victim)) continue;
                }
                const auto* grace = source.isMobBody && source.suppressPlayerShove
                    ? world.tryGet<NewLeechContactGrace>(source.attacker) : nullptr;
                if (grace && nowMillis < grace->untilMillis) continue;
                sample.mark(Phase::Writeback);
                // resolvePlayerMobContact() breaks after its first collision:
                // one flower wedged in a pile takes one hit/bump per tick, not
                // a full stack. Preserve that rule across C++'s source-first
                // combat loop.
                const bool firstBump = !targetBody.contacted;
                // The flower's bounce BEFORE the mob's recoil: gardn sizes it
                // off the velocities the two met with. Recoiling the mob first
                // has it already moving away by the time the closing speed is
                // read, so the flower is barely turned and walks the mob along
                // -- a ladybug shoved across the map by a flower leaning on it.
                if (firstBump) {
                    targetBody.contacted = true;
                    if (!source.suppressPlayerShove) {
                        checkedShove |= source.givesWay;
                        bounceOffMob(world, terrain, victim, source.attacker, source.givesWay, offset,
                                     source.radius + targetBody.radius - offset.length());
                    }
                }
                // Every mob touching the flower recoils, the one-bump rule
                // notwithstanding: that rule stops a pile stacking shoves on
                // the FLOWER, and each mob in the pile still hit it.
                if (source.gardnRecoil) recoilOffFlower(world, source.attacker, victim, offset);
                if (source.yieldsToFlower) {
                    checkedShove = true;
                    yieldToFlower(world, terrain, source.attacker, victim, offset,
                                  source.radius + targetBody.radius - offset.length());
                }
                if (!firstBump) continue;
                if (source.glitchInfecting) markGlitched(world, victim);

                // A firefly discharges on the tick its body reaches a flower.
                //
                // Here, ABOVE the body's own hit, on purpose. Both land on the
                // same flower on the same tick and the first of them opens the
                // 50 ms post-hit window that refuses the second, so whichever
                // goes first is the one the player feels -- and a firefly whose
                // shock is silently eaten by its own bump is a firefly that
                // never shocks anybody.
                //
                // Nothing about being HIT reaches this block: a petal landing
                // on the firefly is the other direction through resolveMelee
                // and throws nothing, which is the whole difference between a
                // firefly and a thorn.
                if (source.lightningRadius > 0.0) {
                    dischargeOnContact(world, grid, source, nowMillis);
                }
                sample.mark(Phase::Eligibility);
            }

            if (const auto* petal = world.tryGet<PetalInstance>(source.attacker)) {
                if (!petal->ricochet.allowsHit(victim, nowMillis)) continue;
            }
            if(const auto* silk=world.tryGet<AttachedSilk>(source.attacker);silk && silk->target!=victim)continue;
            if (!canHit(world, victim, source.attacker, nowMillis)) continue;
            Entity sharedPool = NULL_ENTITY;
            if (source.isPlayerBody) {
                // Several overlapping segments may be parts of the same health pool.
                const auto* link = world.tryGet<BodySegment>(victim);
                if (link && link->sharedHealth) {
                    sharedPool = poolOwner(world, victim);
                    if (std::find(flowerSharedPoolsHit.begin(), flowerSharedPoolsHit.end(), sharedPool) !=
                        flowerSharedPoolsHit.end()) continue;
                }
            }

            // Almost nothing is throttled in the reference, and the throttle
            // that survives is not keyed the way a hit ledger usually is.
            //
            // Mob body contact is paced by the victim's own 50 ms post-hit
            // window and by nothing else; pet/wild contact by nothing at all;
            // a flower's body by nothing. Only a petal whose config names a
            // `damageCooldown` waits, and that wait is keyed on the petal
            // INSTANCE with no victim in the key -- one glass petal lands one
            // hit per window however many mobs it is sitting on, rather than
            // one per mob, which would make it a full-rate area attack.
            const bool wild = isWildMob(world, victim);
            const auto* instance = source.isPetal && isHyperPetal(source.rarity) &&
                (wild || world.has<Projectile>(victim))
                ? world.tryGet<PetalInstance>(source.attacker) : nullptr;
            const auto* technique = instance && instance->slot < kLoadoutActiveSlots ?
                hyperTechniqueInfo(registry.petal(instance->configIndex).id, instance->rarity, instance->mimicked)
                : nullptr;
            const bool guarding = technique && hyperDefending(world, source.owner);

            const bool attacking = technique && hyperAttacking(world, source.owner);
            double interval = (mobTouchesPlayer || petPair) ? 0.0 : std::max(0.0, source.hitIntervalMillis);
            if (wild && guarding) interval *= hyperAmount(technique, HyperTechnique::GlassPhase);
            if (wild && attacking && technique->technique == HyperTechnique::GlassPressure) {
                const auto* state = world.tryGet<Afflictions>(poolOwner(world, victim));
                if (state && state->slowed(nowMillis)) interval *= technique->amount;
            }
            // Read, do not create: an attacker that has never landed a hit
            // should not pay an archetype move just for being near something.
            const HitCooldowns* armed =
                interval > 0.0 ? world.tryGet<HitCooldowns>(source.attacker) : nullptr;
            if (armed != nullptr &&
                !(source.isPetal ? armed->globalReady(nowMillis) : armed->ready(victim, nowMillis))) {
                continue;
            }

            // A claw's bonus is decided here, per victim, off the health the
            // victim has BEFORE this hit.
            sample.mark(Phase::Formula);
            // A training dummy reports the Magic Bone hit a wild target would take.
            const auto* practiceNpc = !wild && source.isPetal && source.wildDamageScale != 1.0
                ? world.tryGet<Npc>(victim) : nullptr;
            const bool practiceBone = practiceNpc &&
                registry.mob(practiceNpc->configIndex).id == "target_dummy" &&
                registry.petal(world.get<PetalInstance>(source.attacker).configIndex).id == "magic_bone";
            const double baseDamage = (wild || practiceBone)
                ? source.damage * source.wildDamageScale : source.damage;
            double swing = swingDamage(world, baseDamage * beanDamageScale, source.critDamage, victim);
            if (source.isPetal) {
                const auto& id = registry.petal(world.get<PetalInstance>(source.attacker).configIndex).id;
                if (id == "magic_iris") {
                    const auto* affliction = world.tryGet<Afflictions>(poolOwner(world, victim));
                    swing = affliction && affliction->poisoned(nowMillis) ? std::min(swing, affliction->poisonPerSecond * world.get<PetalInstance>(source.attacker).combinationStrength) : 0.0;
                }
                if (wild && (id == "void_gunpowder" || id == "magic_void_gunpowder") && isBurning(world, victim, nowMillis))
                    swing *= id == "void_gunpowder" ? 5.0 : 10.0;
            }
            if (wild && technique && technique->technique == HyperTechnique::TomatoRipening) {
                const double age = nowMillis - instance->spawnedAtMillis;
                if (age >= 5000.0) swing *= tomatoGrowth(age + technique->amount) / tomatoGrowth(age);
            }
            const bool critical = source.critChance > 0 && rng_.chance(source.critChance);
            if (critical) swing *= source.critMultiplier;
            if (wild && technique) {
                const Entity target = poolOwner(world, victim);
                const auto* health = world.tryGet<Health>(target);
                const double fraction = health ? health->fraction() : 0.0;
                switch (technique->technique) {
                    case HyperTechnique::BeanSolo: {
                        const auto* ledger = world.tryGet<Bounty>(target);
                        if (attacking && (!ledger || std::none_of(ledger->contributors.begin(), ledger->contributors.end(),
                            [&](const Bounty::Share& share) {
                                return share.damage > 0.0 && share.player != source.owner &&
                                       !oneConnection(world, share.player, source.owner);
                            }))) {
                            swing *= technique->amount;
                        }
                        break;
                    }
                    case HyperTechnique::ClawFollowup:
                        if (attacking && fraction > 0.75 && fraction <= kClawCritHealthFraction)
                            swing += source.critDamage * technique->amount;
                        break;
                    case HyperTechnique::RiceCleanup:
                        if (attacking && fraction < 0.25) swing *= technique->amount;
                        break;
                    case HyperTechnique::DiceFloor:
                        if (!critical) swing *= technique->amount;
                        break;
                    case HyperTechnique::TriangleStrike: {
                        const auto* mods = world.tryGet<PlayerModifiers>(source.owner);
                        if (attacking && mods && mods->triangleCounts[petalTier(instance->rarity)] >= 2)
                            swing *= technique->amount;
                        break;
                    }
                    default: break;
                }
            }
            if(const auto* petal=world.tryGet<FlowerMobPetal>(source.attacker)) {
                if(petal->damageMax>swing)swing=rng_.range(swing,petal->damageMax);
                if(petal->critChance>0 && rng_.chance(petal->critChance))
                    swing*=petal->critMultiplier;
            }
            // The first Icosahedron hit keeps its normal damage; each bounce
            // retains 75% of the previous hit, using the petal's current stats.
            if (const auto* petal = world.tryGet<PetalInstance>(source.attacker)) {
                if (petal->ricochet.enabled && petal->ricochet.active) {
                    swing *= std::pow(0.75, petal->ricochet.hits);
                }
            }
            // A plank's 20x, against the things that are not bodies: a shot,
            // a petal, a seat on a mob's ring.
            if (source.shotDamageScale != 1.0 &&
                (world.has<ProjectileTag>(victim) || world.has<PetalInstance>(victim) ||
                 world.has<MobRingPetal>(victim))) {
                swing *= source.shotDamageScale;
                const auto* shot = world.tryGet<Projectile>(victim);
                if (guarding && technique->technique == HyperTechnique::PlankIntercept && shot &&
                    (shot->petalConfigIndex == kNoPetal ||
                     !registry.petal(shot->petalConfigIndex).lightningDamage) &&
                    isWildMob(world, reflectionTarget(world, victim))) {
                    swing *= technique->amount;
                }
            }
            // A disc's cut, on the one kind of hit it is for: a body colliding
            // with the flower. florr's "mobs and flowers" -- but no flower's
            // body hurts another here (see above), so a mob, a seat on its
            // ring or a hostile NPC is every collision there is. A petal, a
            // shot, a field or a shock goes past it whole.
            if (mobTouchesPlayer) {
                swing *= collisionDamageTakenScale(world, victim);
                if (source.isMobBody && !source.isMobRing && !source.isPet && hyperDefending(world, victim)) {
                    swing *= hyperAmount(wornHyperTechnique(world, registry, victim, "disc"),
                                         HyperTechnique::GuardDisc);
                }
            }
            sample.mark(Phase::Writeback);
            const auto beforeHitEvents = events_ ? events_->events().size() : 0;
            // The direct hit's victim was checked above, and only reads and
            // rolls intervene. A shared pool still checks its own recipient.
            DamageResult hit = applyDamageWithRuby(world, victim, source.attacker, swing,
                                                   nowMillis, source.hitKind, victim);
            if (sharedPool != NULL_ENTITY && !hit.refused) flowerSharedPoolsHit.push_back(sharedPool);
            if (mobTouchesPlayer && source.swallows &&
                !hit.refused && !hit.dodged && !hit.killed) {
                const auto* prior = world.tryGet<WormSwallow>(victim);
                if (!prior || nowMillis >= prior->safeUntilMillis) {
                    world.ensure<WormSwallow>(victim) = {source.attacker, nowMillis + 1250, nowMillis + 10000};
                }
            }
            if (critical && events_) {
                if (const auto* id=world.tryGet<NetId>(victim)) events_->markCriticalSince(beforeHitEvents,id->value);
            }
            // A swing of NOTHING is still a swing. canHit() has already vouched
            // for this victim one line above -- alive, not a corpse, not
            // invulnerable, on the other side -- so the only refusal
            // applyDamage can add here is a damage figure of zero, and several
            // petals carry one deliberately: iris and blue_iris are pure
            // poison, bubble and bomb pure utility. The reference never looks
            // at the number; it applies every rider and charges the petal its
            // own health BELOW damageMob(), so a zero-damage petal poisons,
            // shoves and wears out exactly like any other. Read as a refusal,
            // those four petals landed nothing at all and never broke.
            //
            // It can still MISS. applyDamage refuses a zero before it rolls
            // evasion, so the roll for one is made here -- an iris is an attack
            // like any other, and a fly that could not side-step the one petal
            // that needs no damage to kill it would not be evasive at all.
            const bool zeroSwing = source.isPetal && swing == 0.0;
            if (zeroSwing && hit.refused && rollDodge(world, victim, nowMillis)) {
                hit.dodged = true;
            }
            // A miss lands nothing, costs the petal nothing -- it touched
            // nothing -- and still spends the pacing below, as a swing does.
            const bool landed = !hit.dodged && (!hit.refused || zeroSwing);
            // Petal knockback is set after the hit using its own stat and the
            // victim's mass, exactly like playerState.ts. Mob contact already
            // bounced the flower above (bounceOffMob).
            //
            // Every rider sits BELOW the reference's already-dead `continue`
            // and inside its not-invulnerable branch, so a refused hit lands
            // none of them: it credits the ledger and nothing else.
            if(landed && wild && !hit.killed && source.isPetal && !world.has<AttachedSilk>(source.attacker)) {
                const auto* instance=world.tryGet<PetalInstance>(source.attacker);
                if(instance && (registry.petal(instance->configIndex).id=="silk" || registry.petal(instance->configIndex).id=="arctic_banana")) {
                    const bool detached=registry.petal(instance->configIndex).id=="silk";
                    world.add<AttachedSilk>(source.attacker,AttachedSilk{source.owner,victim});
                    if(detached) if(auto* loadout=world.tryGet<Loadout>(source.owner)) {
                        auto& ring=loadout->spawned;
                        ring.erase(std::remove(ring.begin(),ring.end(),source.attacker),ring.end());
                    }
                    if(auto* replica=world.tryGet<Replicated>(source.attacker))replica->kind=net::EntityKind::Projectile;
                }
            }
            if (landed && source.isPetal) {
                const auto* petal = world.tryGet<PetalInstance>(source.attacker);
                if (petal && registry.petal(petal->configIndex).id == "coral" &&
                    (wild || isPracticeDummy(world, registry, victim))) {
                    const auto index = petal->configIndex;
                    if (auto* health = world.tryGet<Health>(source.attacker)) {
                        const double halfHealth = health->max * 0.5;
                        health->current = 0.0;
                        const double direction = offset.angle();
                        for (const double spread : {-0.55, 0.55}) {
                            const Entity shard = spawnPetalShard(world, source.owner,
                                Transform{source.position, direction + spread, source.realm},
                                index, source.rarity, source.damage * 0.5, 0, false,
                                allocateNetId ? allocateNetId() : 0, halfHealth);
                            // Both shards start inside the contact target. Let them fly out.
                            world.get<HitCooldowns>(shard).arm(victim, nowMillis + 1000.0);
                        }
                    }
                    break;
                }
            }
            if (landed && source.isPetal && world.has<ThornFlightStep>(source.attacker) &&
                (wild || isPracticeDummy(world, registry, victim))) {
                if (auto* at = world.tryGet<Transform>(source.attacker)) at->position = contactCentre;
                if (auto* health = world.tryGet<Health>(source.attacker)) health->current = 0;
                break;
            }
            if (landed && source.impactSpill && (wild || isPracticeDummy(world, registry, victim))) {
                const auto* at = world.tryGet<Transform>(victim);
                if (at) {
                    double scale = petalStrengthScale(source.rarity);
                    if (const auto* petal = world.tryGet<PetalInstance>(source.attacker)) {
                        const auto* technique = hyperTechniqueInfo(
                            registry.petal(petal->configIndex).id, source.rarity, petal->mimicked);
                        scale *= hyperAmount(technique, HyperTechnique::AcidPoolBoost);
                    }
                    createSpill(world, source.owner, at->position, source.rarity, *source.impactSpill,
                                scale, allocateNetId ? allocateNetId() : 0, 1.0, &rng_);
                }
                if (auto* health = world.tryGet<Health>(source.attacker)) health->current = 0.0;
                break;
            }
            if (landed && !hit.killed && wild && source.shortensUndead)
                shortenUndeadWithScar(world, victim, source.rarity, nowMillis);
            if (landed && wild && source.isPetal) {
                const auto* petal = world.tryGet<PetalInstance>(source.attacker);
                const auto* mob = world.tryGet<MobType>(victim);
                if (petal && mob && petalTier(source.rarity) >= rarityIndex(mob->rarity)) {
                    const auto& id = registry.petal(petal->configIndex).id;
                    if (id == "oxygen" && !hit.killed) {
                        const Entity target=poolOwner(world,victim);
                        const double extension = oxygenBurnExtensionMillis(source.rarity);
                        const double limit = nowMillis + kAfflictionExtensionCapMillis;
                        if(auto* status=world.tryGet<Afflictions>(target)) {
                            if(status->burnPerSecond>0 && status->burnUntilMillis>nowMillis && status->burnUntilMillis<limit)
                                status->burnUntilMillis=std::min(limit,status->burnUntilMillis+extension);
                            for(auto& burn:status->burns)
                                if(burn.untilMillis>nowMillis && burn.untilMillis<limit)
                                    burn.untilMillis=std::min(limit,burn.untilMillis+extension);
                        }
                        if(auto* burn=world.tryGet<Burn>(target);burn && burn->damage>0 &&
                           burn->untilMillis>nowMillis && burn->untilMillis<limit)
                            burn->untilMillis=std::min(limit,burn->untilMillis+extension);
                    } else if (id == "void_cyanide" && !hit.killed) {
                        if (auto* status = world.tryGet<Afflictions>(poolOwner(world, victim))) {
                            const double extension = cyanidePoisonExtensionMillis(source.rarity);
                            const double limit = nowMillis + kAfflictionExtensionCapMillis;
                            for (auto& poison : status->poisonStacks) {
                                if (poison.untilMillis > nowMillis && poison.untilMillis < limit)
                                    poison.untilMillis = std::min(limit, poison.untilMillis + extension);
                            }
                            summarisePoison(*status, nowMillis);
                        }
                    } else if (id == "job_application" && !hit.killed) {
                        Entity brain = victim;
                        if (const auto* segment = world.tryGet<BodySegment>(victim);
                            segment && segment->chainHead != NULL_ENTITY && world.isAlive(segment->chainHead)) {
                            brain = segment->chainHead;
                        }
                        const auto* brainType = world.tryGet<MobType>(brain);
                        auto* ai = world.tryGet<MobAi>(brain);
                        if (ai && brainType && isWildMob(world, brain) &&
                            petalTier(source.rarity) >= rarityIndex(brainType->rarity)) {
                            ai->target = NULL_ENTITY;
                            ai->unseenTarget = NULL_ENTITY;
                            ai->aggroSuppressedUntilMillis = nowMillis + 5000.0;
                        }
                    } else if (id == "cancer" || id == "magic_cancer") {
                        const Entity target = poolOwner(world, victim);
                        const auto* segment = world.tryGet<BodySegment>(target);
                        if ((!segment || segment->head) &&
                            !world.has<GhostTag>(target) && !world.has<WaveEnemy>(target) &&
                            !world.has<DungeonDweller>(target) && !world.has<DungeonEntrance>(target) &&
                            !world.has<RiftPlayer>(source.owner) &&
                            !world.has<CancerSubdivision>(target)) {
                            world.add<CancerSubdivision>(target);
                        }
                    }
                }
            }
            if (landed && !hit.killed) {
                if (source.burnPerSecond > 0.0 && (wild || isPracticeDummy(world, registry, victim)))
                    ignite(world, victim, source.attacker, source.owner, source.burnPerSecond,
                              source.burnMillis, nowMillis);
                if (wild && source.armorDamageFraction > 0.0) {
                    const Entity target = poolOwner(world, victim);
                    const auto* armor = world.tryGet<Armor>(target);
                    const auto* status = world.tryGet<Afflictions>(target);
                    const double effective = armor
                        ? std::max(0.0, armor->amount - (status ? status->shred(nowMillis) : 0.0)) : 0.0;
                    if (effective > 0.0)
                        applyDamage(world, victim, source.attacker, effective * source.armorDamageFraction,
                                    nowMillis, DamageKind::Periodic);
                }
                const auto* wildTechnique = wild ? technique : nullptr;
                if (source.isPetal) {
                    double shove = guarding ? hyperAmount(wildTechnique, HyperTechnique::WildShove) : 1.0;
                    if (wild && guarding && technique->technique == HyperTechnique::PentagonShove) {
                        const auto* armor = world.tryGet<Armor>(poolOwner(world, victim));
                        if (armor && armor->amount > 0.0) shove *= technique->amount;
                    }
                    double push = source.knockback * shove;
                    Vec2 direction = offset;
                    if (source.jellyKnockback && push != 0.0) {
                        const auto* mob = wild ? world.tryGet<MobType>(victim) : nullptr;
                        if (mob) {
                            const auto grade = [](Rarity rarity) {
                                return rarity == Rarity::Ascendant ? kLadderRarityCount
                                                                    : petalTier(rarity);
                            };
                            const int mobTier = grade(mob->rarity);
                            const int gap = mobTier - grade(source.rarity);
                            if (gap > 0) {
                                const double resistance = std::ldexp(1.0, -gap);
                                push *= resistance;
                                if (mobTier >= rarityIndex(Rarity::Unique)) {
                                    // Upper tiers may still be nudged. The ceiling is in
                                    // world distance, after applyKnockback divides by mass.
                                    const double limit = 4.0 * kTileSize * resistance *
                                                         massOf(world, victim);
                                    push = std::clamp(push, -limit, limit);
                                }
                            }
                        }
                        if (direction.lengthSq() <= 1e-12) {
                            // A centre-on-centre hit still pushes outward from the flower.
                            if (const auto* owner = world.tryGet<Transform>(source.owner))
                                direction = source.position - owner->position;
                            if (direction.lengthSq() <= 1e-12) {
                                const auto* at = world.tryGet<Transform>(source.attacker);
                                direction = Vec2::fromAngle(at ? at->angle : 0.0, 1.0);
                            }
                        }
                    }
                    applyKnockback(world, victim, push < 0.0 ? direction * -1.0 : direction,
                                   std::abs(push));
                } else if (wild && source.isPlayerBody) {
                    const auto* input = world.tryGet<PlayerInput>(source.attacker);
                    if (input && input->current.attacking() && !input->current.defending()) {
                        const double shove = hyperAmount(wornHyperTechnique(world, registry, source.attacker, "cutter"),
                                                         HyperTechnique::CutterShove, 0.0);
                        if (shove > 0.0) applyKnockback(world, victim, offset, shove);
                    }
                }
                if (wild && guarding && technique->technique == HyperTechnique::JellyWake) {
                    const auto* prior = world.tryGet<Afflictions>(poolOwner(world, victim));
                    if (!prior || !prior->slowed(nowMillis)) {
                        applySlow(world, victim, technique->amount, 400.0, source.rarity, nowMillis);
                    }
                }
                if (wild && technique && technique->technique == HyperTechnique::DustWake) {
                    const auto* input = world.tryGet<PlayerInput>(source.owner);
                    const auto* state = world.tryGet<Afflictions>(poolOwner(world, victim));
                    if (input && input->current.moveStrength > 0.0 && (!state || !state->slowed(nowMillis))) {
                        applySlow(world, victim, technique->amount, 350.0, source.rarity, nowMillis);
                    }
                }
                if (wild && technique && technique->technique == HyperTechnique::SawbladeNotch) {
                    const auto* loadout = world.tryGet<Loadout>(source.owner);
                    const auto* blade = world.tryGet<PetalInstance>(source.attacker);
                    const Entity armored = poolOwner(world, victim);
                    const auto* armor = world.tryGet<Armor>(armored);
                    const auto* afflictions = world.tryGet<Afflictions>(armored);
                    if (loadout && blade && loadout->slots[blade->slot].configIndex == blade->configIndex &&
                        armor && armor->amount > 0.0 && (!afflictions || afflictions->shred(nowMillis) == 0.0)) {
                        applyArmorShred(world, victim, armor->amount * technique->amount,
                                        nowMillis, 1000.0 / kArmorShredMillis);
                    }
                }
                if (wild && source.isPetal) {
                    const auto& inst = world.get<PetalInstance>(source.attacker);
                    const auto& kind = registry.petal(inst.configIndex).id;
                    if (kind == "magic_neurotoxin") {
                        const Entity target = poolOwner(world, victim);
                        if (petalTier(rarityOf(world, target)) <= petalTier(inst.rarity))
                            world.ensure<Afflictions>(target).veerUntilMillis = nowMillis + 5000;
                    }
                    if (kind == "magic_bur") {
                        const double attack = bodyDamageOf(world, registry, victim);
                        if (std::isfinite(attack)) applyArmorShred(world, victim, std::min(12.0 * petalStrengthScale(inst.rarity), attack * (.02 + .035 * ladderIndex(inst.rarity))), nowMillis);
                    }
                }
                const double poisonWindow = guarding ?
                    hyperAmount(wildTechnique, HyperTechnique::WildPoisonWindow) : 1.0;
                const auto* petal = source.poisonPerSecond > 0.0
                    ? world.tryGet<PetalInstance>(source.attacker) : nullptr;
                const bool stackPoison = petal && contactPoisonStacksByPetal(registry.petal(petal->configIndex).id);
                applyPoison(world, victim, source.attacker, source.poisonPerSecond / poisonWindow,
                            source.poisonDurationMillis * poisonWindow, nowMillis, stackPoison);
                applySlow(world, victim, source.slowFactor,
                          source.slowDurationMillis * hyperAmount(wildTechnique, HyperTechnique::WildSlow),
                          source.rarity, nowMillis, source.inflictsFrost, source.inflictsFreeze);
                applyNoHeal(world, victim,
                            source.noHealDurationMillis * hyperAmount(wildTechnique, HyperTechnique::WildNoHeal),
                            nowMillis);
                // A rider like the other two, and for the same reason it has to
                // be: armour can absorb a bur's own damage whole, and a strip
                // that only landed when the damage did would be a petal that
                // cannot counter the one thing it exists to counter. `landed`
                // covers that -- a swing of nothing is still a swing.
                applyArmorShred(world, victim, source.armorReduction, nowMillis,
                                hyperAmount(wildTechnique, HyperTechnique::WildShred));
            }

            if (landed && source.selfDamage > 0.0) {
                if (auto* ownerHealth = world.tryGet<Health>(source.owner); ownerHealth && ownerHealth->current > 1.0)
                    ownerHealth->current = std::max(1.0, ownerHealth->current - source.selfDamage);
            }

            // A risen wild mob stays at 1 HP until its timer expires. Its
            // bites still feed Fang for the damage that made it past armour.
            const double lifestealBase = hit.applied + hit.undeadMobOverkill;
            if (source.lifesteal > 0.0 && lifestealBase > 0.0) {
                double healing = lifestealBase * source.lifesteal;
                if (wild && guarding && technique->technique == HyperTechnique::FangRecovery) {
                    const auto* health = world.tryGet<Health>(source.owner);
                    if (health && health->fraction() < 0.40) {
                        // landHeal multiplies the whole heal, including this bonus.
                        const auto* mods = world.tryGet<PlayerModifiers>(source.owner);
                        const double scale = mods ? kPlayerHealMultiplier * (1.0 + mods->healBonus) : 1.0;
                        healing += std::min(healing * technique->amount, health->max * 0.02 / scale);
                    }
                }
                stealLife(world, source.owner, healing, nowMillis);
                auto* tooth = world.tryGet<PetalInstance>(source.attacker);
                if (wild && tooth && !tooth->magicToothManaRefunded &&
                    registry.petal(tooth->configIndex).id == "magic_tooth") {
                    if (auto* mana = world.tryGet<ManaPool>(source.owner)) {
                        mana->current = std::min(mana->max, mana->current + magicToothManaPerReload(source.rarity));
                        tooth->magicToothManaRefunded = true;
                    }
                }
                // Every bite, a fixed draught by tier, however hard it landed.
                if (tooth && registry.petal(tooth->configIndex).id == "magic_fang" &&
                    (wild || isTrainingDummy(world, victim))) {
                    if (auto* mana = world.tryGet<ManaPool>(source.owner); mana && mana->has())
                        mana->current = std::min(mana->max, mana->current + magicFangManaPerBite(source.rarity));
                }
            }

            if (wild && technique && hit.applied > 0.0) {
                if (technique->technique == HyperTechnique::CoinSweep && hit.killed) {
                    world.ensure<HyperPetalTimers>(source.owner).coinPickupUntilMillis = nowMillis + 4000.0;
                }
                if (technique->technique == HyperTechnique::BloodRecovery && attacking) {
                    const auto* owner = world.tryGet<Health>(source.owner);
                    const auto* timers = world.tryGet<HyperPetalTimers>(source.owner);
                    if (owner && owner->fraction() < 0.30 && !healingBlocked(world, source.owner, nowMillis) &&
                        (!timers || nowMillis >= timers->bloodNeedleReadyAtMillis)) {
                        const auto* mods = world.tryGet<PlayerModifiers>(source.owner);
                        const double scale = mods ? kPlayerHealMultiplier * (1.0 + mods->healBonus) : 1.0;
                        const double heal = std::min(hit.applied * technique->amount, owner->max * 0.005 / scale);
                        world.ensure<HyperPetalTimers>(source.owner).bloodNeedleReadyAtMillis = nowMillis + 1000.0;
                        stealLife(world, source.owner, heal, nowMillis);
                    }
                }
            }

            // Record the bounce only after a real hit, and re-fetch after damage
            // may have moved an ECS row. Clearing the target prevents a second
            // hit in this same candidate loop (or on a previously visited mob).
            if (landed && source.isPetal && world.has<MobTag>(victim)) {
                if (auto* petal = world.tryGet<PetalInstance>(source.attacker)) {
                    petal->ricochet.landed(victim, nowMillis);
                    if (petal->ricochet.active && petal->ricochet.hits == 1) {
                        petal->ricochet.target = ricochetTarget(world, source.attacker, nowMillis);
                        petal->ricochet.protectSecondHit =
                            petal->ricochet.target != NULL_ENTITY;
                    }
                    // Arming the next target now protects the first contact,
                    // but movement must get a tick before the next bounce lands.
                    ricochetHit = petal->ricochet.enabled && petal->ricochet.active;
                }
            }

            // The petal pays for the hit out of its own health, at the mob's
            // full damage stat, in the same block and on the same tick as the
            // damage it dealt. This IS the reload cycle: a ring that hits
            // thirty times a second and never bleeds is unbreakable, and the
            // break/reload rhythm is most of what paces the fight.
            //
            // Exempt only for an unbreakable petal (glass/infinity). The
            // ordinary 50ms hit throttle must not disable durability loss.
            // Recoil rather than Direct, but
            // a blow as far as armour goes: a bone shrugs off the part of the
            // bite its armour covers. The petal flashes for what it lost, and
            // carries no number: a petal is on neither of the reference's two
            // damage-report channels (see flashStruck).
            //
            // A hostile NPC charges it too, at its mob's damage: a dummy that
            // let a ring hit for free would flatter every build tested on it.
            // (A players'-side NPC never gets here -- canHit refused it.)
            //
            // Not a loose petal: the mobs it touches bite it already (the
            // looseVictim rule above), and charging the same contact twice
            // would halve the moon's life for nothing the player did.
            if (landed && source.isPetal && !source.isLoose && source.petalBreakable &&
                (world.has<MobTag>(victim) || world.has<NpcTag>(victim))) {
                const double recoil = contactDamageOf(world, victim);
                const double wear = wild ? recoil * source.wildRecoilScale : recoil;
                applyDamage(world, source.attacker, victim, wear, nowMillis, DamageKind::Recoil);
                const auto* livePetal = world.tryGet<PetalInstance>(source.attacker);
                if (wild && livePetal && hit.applied > 0.0 && !hit.killed &&
                    (source.hitKind == DamageKind::Direct || source.hitKind == DamageKind::Piercing) && isStanding(world, source.attacker)) {
                    const auto& id = registry.petal(livePetal->configIndex).id;
                    const bool followup = (id == "sawblade" && ladderRarity(source.rarity) >= Rarity::Unique) ||
                                          (id == "rock" && isHyperPetal(source.rarity));
                    if (followup) {
                        // One extra pass for these durable contacts, on the
                        // simulation clock. It spends durability as well.
                        const auto second = applyDamage(world, victim, source.attacker, swing * 0.5, nowMillis);
                        if (!second.refused && !second.dodged)
                            applyDamage(world, source.attacker, victim, wear * 0.5, nowMillis, DamageKind::Recoil);
                    }
                }
            }

            // Re-fetched, never cached across the calls above: adding Dead or
            // Knockback to the victim relocates archetype rows, and the
            // attacker's own row may be one of the ones that moved.
            if (interval > 0.0) {
                HitCooldowns& cooldowns = world.ensure<HitCooldowns>(source.attacker);
                if (source.isPetal) cooldowns.armGlobal(nowMillis + interval);
                else cooldowns.arm(victim, nowMillis + interval);
            }

        }
        if (checkedShove && !meleePlans_.empty()) {
            const auto slot = entityIndex(source.attacker);
            const auto* at = world.tryGet<Transform>(source.attacker);
            const auto before = queries_->contactPositions[slot].position;
            if (at && (before.x != at->position.x || before.y != at->position.y))
                displaced[realmIndex(source.realm)] |= UINT64_C(1) << (slot % 64);
        }
        if (timing) {
            if (usePlan) ++timing->planned; else ++timing->fallback;
            const auto raw = usePlan ? plan->raw : candidates.size();
            auto& density = timing->realms[realmIndex(source.realm)];
            ++density.sources;
            density.raw += raw;
            density.overlaps += overlaps;
            density.visits += at - first + (at < last && !ricochetHit ? 1 : 0);
            if (raw > density.peak) {
                density.peak = raw;
                density.hotspot = source.position;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Lightning
// ---------------------------------------------------------------------------

void CombatSystem::tickLaserBeams(World& world, const SpatialGrid& grid,
                                  const ContentRegistry& content, double nowMillis) {
    // Gather then apply: applyDamage and ensure<HitCooldowns> relocate rows.
    struct Pending {
        Entity petal = NULL_ENTITY;
        Entity victim = NULL_ENTITY;
        double amount = 0;
    };
    std::vector<Pending> pending;
    Query<PetalInstance, Transform> lasers{world};
    lasers.without<Dead>();
    lasers.without<LoosePetal>();
    lasers.each([&](Entity e, PetalInstance& petal, Transform& at) {
        const PetalConfig& config = content.petal(petal.configIndex);
        if (!petalIsLaser(config)) return;
        PetalStats stats = computerSetStats(content, world, petal.owner, petal.configIndex,
                                            petal.rarity,
                                            content.petalStats(petal.configIndex, petal.rarity));
        double dps = laserBeamDamagePerSecond(stats);
        if (dps <= 0.0) return;
        if (const PlayerModifiers* modifiers = world.tryGet<PlayerModifiers>(petal.owner)) {
            dps *= modifiers->petalDamageScale;
        }
        const double amount = dps * (kPetalHitIntervalMillis / 1000.0);
        if (amount <= 0.0) return;

        // Orbital bearing: wiki keeps the beam pointing outward with the ring.
        const double heading = petal.facingAngle;
        const Vec2 axis = Vec2::fromAngle(heading);
        const Vec2 start = at.position;
        const Vec2 end = start + axis * kLaserBeamLength;
        const Vec2 mid = (start + end) * 0.5;
        queryBodies(grid, at.realm, mid,
                    kLaserBeamLength * 0.5 + kLaserBeamHalfWidth + kBroadphasePad, candidates_,
                    petal.owner);

        const HitCooldowns* cool = world.tryGet<HitCooldowns>(e);
        for (const Entity victim : candidates_) {
            if (victim == e) continue;
            // Wiki: the beam ignores enemy projectiles.
            if (isFiledShot(victim) || world.has<Projectile>(victim)) continue;
            if (!world.isAlive(victim) || world.has<Dead>(victim)) continue;
            if (world.has<PetalInstance>(victim) || world.has<GroundEffectTag>(victim)) continue;
            const Transform* victimAt = world.tryGet<Transform>(victim);
            const Body* body = world.tryGet<Body>(victim);
            if (victimAt == nullptr || body == nullptr || victimAt->realm != at.realm) continue;

            const Vec2 toVictim = victimAt->position - start;
            const double along = clamp(toVictim.x * axis.x + toVictim.y * axis.y, 0.0,
                                       kLaserBeamLength);
            const Vec2 nearest = start + axis * along;
            const double reach = kLaserBeamHalfWidth + body->radius;
            const double half = squareMobHalfExtent(world, victim);
            if (half > 0.0) {
                if (!segmentTouchesSquare(start, end, victimAt->position, half, victimAt->angle,
                                          kLaserBeamHalfWidth)) {
                    continue;
                }
            } else if ((victimAt->position - nearest).lengthSq() > reach * reach) {
                continue;
            }

            const Entity pool = poolOwner(world, victim);
            if (cool != nullptr && !cool->ready(pool, nowMillis)) continue;
            if (!canHit(world, pool, e, nowMillis)) continue;
            pending.push_back({e, victim, amount});
        }
    });

    for (const Pending& hit : pending) {
        if (!world.isAlive(hit.petal) || world.has<Dead>(hit.petal)) continue;
        if (!world.isAlive(hit.victim) || world.has<Dead>(hit.victim)) continue;
        // Several queued segments can share one health pool. Recheck the
        // ledger here, while keeping the struck segment for damage numbers.
        const Entity pool = poolOwner(world, hit.victim);
        HitCooldowns& cooldown = world.ensure<HitCooldowns>(hit.petal);
        if (!cooldown.ready(pool, nowMillis)) continue;
        cooldown.arm(pool, nowMillis + kPetalHitIntervalMillis);
        applyDamage(world, hit.victim, hit.petal, hit.amount, nowMillis, DamageKind::Lightning);
    }
}

void CombatSystem::strikeLightning(World& world, const SpatialGrid& grid, Entity source, Vec2 at,
                                   Realm realm, double radius, double damage, double nowMillis) {
    if (radius <= 0.0 || !(damage > 0.0)) return;

    grid.query(realm, at, radius + kBroadphasePad, strikeCandidates_);
    strikeVictims_.clear();
    strikeArms_.clear();
    for (const Entity victim : strikeCandidates_) {
        if (victim == source) continue;
        // Stated outright rather than left to the faction rule: a wild mob's
        // shock is aimed at flowers and a pet's at wild mobs (strikesAt).
        // Washing over the mobs beside a wild one would turn a jellyfish shoal
        // into a mutual suicide pact, and canDamage() would not have stopped
        // that on its own -- it lets anything through whose side it cannot
        // resolve.
        if (!strikesAt(world, source, victim)) continue;
        const Transform* transform = world.tryGet<Transform>(victim);
        if (transform == nullptr || transform->realm != realm) continue;
        // Centres inside the disc. The stricter of the two rules a field could
        // use -- it does not add the victim's own radius -- so every arm drawn
        // ends on a flower that was certainly inside the flash.
        if (distanceSq(transform->position, at) > radius * radius) continue;
        // Asked here rather than left to applyDamage so that a teammate is not
        // given a bolt to look at. canHit() is deliberately NOT what is asked:
        // a flower inside its respawn window is still standing in the strike
        // and still gets an arm, and applyDamage refuses the damage on its own.
        if (!canDamage(world, source, victim)) continue;
        strikeVictims_.push_back(victim);
        strikeArms_.push_back(transform->position);
    }
    if (strikeVictims_.empty()) return;

    // Reported BEFORE the damage lands, and from positions read before it
    // lands: a strike can kill, replication drops a dead entity in the same
    // tick, and a bolt to the flower it just felled is exactly the bolt to
    // draw. This is why the arms ride the event instead of the client working
    // them out from its entity table.
    if (events_ != nullptr) {
        // The queue draws the nearest of them when there are more than fit
        // (EventQueue::lightning). The DAMAGE list is not trimmed; every
        // flower inside the radius is hit whether or not an arm was drawn.
        events_->lightning(at, radius, realm, strikeArms_, false, source);
    }

    // Entities, not pointers: applyDamage can mark a flower Dead, which
    // relocates the row every Transform* above came out of.
    for (const Entity victim : strikeVictims_) {
        double dealt = damage;
        if (world.has<RiftPlayer>(victim) && isWildMob(world, source)) {
            const auto* mob = world.tryGet<MobType>(source);
            if (mob && content().mob(mob->configIndex).id == "jellyfish") {
                // Rift has a flat health pool. The ocean's rarity ladder can
                // otherwise make a Jellyfish pulse lethal from outside contact.
                constexpr double kRiftJellyfishPulseCap = kArenaMaxHealth * 0.40;
                dealt = std::min(dealt, kRiftJellyfishPulseCap);
            }
        }
        applyDamage(world, victim, source, dealt, nowMillis, DamageKind::Lightning);
    }
}

void CombatSystem::gatherMobLightning(World& world, const ContentRegistry& content) {
    strikers_.clear();
    queries_->strikers.each([&](Entity e, MobTag&, MobType& type, Transform& transform,
                                Body& body) {
        const MobConfig& config = content.mob(type.configIndex);
        if (!config.lightning.present || config.lightning.strikeRange <= 0.0) return;
        // A pet strikes too, at the wild mobs its owner is fighting rather
        // than at flowers: strikesAt() is what keeps it off its owner.
        LightningSource striker;
        striker.mob = e;
        striker.position = transform.position;
        // Both reaches are stated against the skin -- see LightningSpec. An
        // ultra jellyfish is 315 units across; measured from its centre its
        // 300-unit shock never left its own body and it could not hurt a flower
        // standing on top of it, let alone one nearby.
        striker.radius = body.radius + config.lightning.radius;
        striker.damage = lightningDamageOf(config, content.mobStats(type.configIndex, type.rarity));
        striker.strikeRange = body.radius + config.lightning.strikeRange;
        striker.cooldownMillis = lightningCooldownOf(config);
        striker.realm = transform.realm;
        strikers_.push_back(striker);
    });
}

void CombatSystem::resolveMobLightning(World& world, const SpatialGrid& grid, double nowMillis) {
    for (const LightningSource& striker : strikers_) {
        if (!world.isAlive(striker.mob) || world.has<Dead>(striker.mob)) continue;
        // The clock first: it is the cheap half, and a mob still charging is
        // spared the broadphase query entirely. Read, not created -- a mob that
        // has never struck pays no archetype move for standing in an empty sea.
        const LightningClock* clock = world.tryGet<LightningClock>(striker.mob);
        if (clock != nullptr && nowMillis - clock->lastStrikeMillis < striker.cooldownMillis) {
            continue;
        }
        // Something has to be in reach before a strike is thrown. Without this
        // the mob would flash on its cooldown forever with nothing to hit,
        // which is a bolt on every client's screen once a second per jellyfish.
        if (!targetWithin(world, grid, striker.mob, striker.position, striker.realm,
                          striker.strikeRange, nowMillis)) {
            continue;
        }
        // Stamped before the strike and whatever the strike turns out to do,
        // exactly as every other cooldown here is stamped: a shock that lands
        // on a flower already inside its respawn window still costs the charge.
        world.ensure<LightningClock>(striker.mob).lastStrikeMillis = nowMillis;
        strikeLightning(world, grid, striker.mob, striker.position, striker.realm, striker.radius,
                        striker.damage, nowMillis);
    }
}

void CombatSystem::dischargeOnContact(World& world, const SpatialGrid& grid,
                                      const MeleeSource& source, double nowMillis) {
    // Read before it is created: a mob that has never struck should not pay
    // an archetype move just for touching something while it is still
    // charging.
    const LightningClock* clock = world.tryGet<LightningClock>(source.attacker);
    if (clock != nullptr && nowMillis - clock->lastStrikeMillis < source.lightningCooldownMillis) {
        return;
    }
    world.ensure<LightningClock>(source.attacker).lastStrikeMillis = nowMillis;
    // From the MOB, not from what it touched: the bolt lands where the firefly
    // is, and everything standing around it is inside the same flash.
    strikeLightning(world, grid, source.attacker, source.position, source.realm,
                    source.lightningRadius, source.lightningDamage, nowMillis);
}

bool CombatSystem::strikesAt(World& world, Entity source, Entity victim) {
    if (world.has<Pet>(source)) return world.has<MobTag>(victim) && !world.has<Pet>(victim);
    return world.has<PlayerTag>(victim);
}

bool CombatSystem::targetWithin(World& world, const SpatialGrid& grid, Entity mob, Vec2 at,
                                Realm realm, double range, double nowMillis) {
    grid.query(realm, at, range + kBroadphasePad, strikeCandidates_);
    for (const Entity candidate : strikeCandidates_) {
        if (!strikesAt(world, mob, candidate)) continue;
        const Transform* transform = world.tryGet<Transform>(candidate);
        if (transform == nullptr || transform->realm != realm) continue;
        if (distanceSq(transform->position, at) > range * range) continue;
        // The full hit test, not just the faction one: a corpse and a flower
        // still inside its respawn window are both standing there, and neither
        // is a reason to spend a charge. A strike ALREADY under way still
        // washes over them -- see strikeLightning.
        if (!canHit(world, candidate, mob, nowMillis)) continue;
        return true;
    }
    return false;
}

void CombatSystem::resolvePetalPvp(World& world, const MeleeSource& source, Entity victim,
                                   Vec2 victimPosition, double victimRadius, double nowMillis,
                                   double beanDamageScale) {
    const PetalInstance* petal = world.tryGet<PetalInstance>(source.attacker);
    if (petal == nullptr) return;
    const Entity owner = petal->owner;
    // The reference skips its own flower by id and its splitter half by socket
    // -- one person, however many bodies -- before anything is charged. Here
    // the ring resolves back to its owner for the first, and the second is the
    // connection rule canDamage() keys off: a split flower's two halves share
    // one. Both are tested before the throttle is armed and the swing paid
    // for below. canDamage() would refuse the hit itself, but too late: a
    // corrupted half, or a duellist in the ring, would wear its own ring down
    // on the half it parked.
    if (owner == victim) return;
    if (oneConnection(world, owner, victim)) return;
    if (!canPetalsDamagePlayer(world, owner, victim)) return;

    // A petal centred on a different flower is still a corruption hit.
    // Ordinary arena duels keep their existing zero-distance rule.
    const Vec2 offset = victimPosition - source.position;
    const double reach = source.radius + victimRadius;
    const double gapSq = offset.lengthSq();
    if (gapSq >= reach * reach) return;
    if (gapSq == 0.0 && !isCorrupted(world, owner) && !isCorrupted(world, victim) &&
        !equippedCorruptionAllowsPvp(world, owner, victim)) return;

    // Keyed on the VICTIM here, where the same petal's window against mobs is
    // keyed on the instance alone: the reference's PvP key carries the other
    // flower's id, so one petal reaches two duellists in a tick and neither of
    // them twice. A petal that declares a `damageCooldown` brings its own.
    const double interval = source.hitIntervalMillis > 0.0 ? source.hitIntervalMillis
                                                           : kPvpPetalHitIntervalMillis;
    const HitCooldowns* armed = world.tryGet<HitCooldowns>(source.attacker);
    if (armed != nullptr && !armed->ready(victim, nowMillis)) return;
    // Stamped before the swing and whatever the swing turns out to do, which
    // is where the reference stamps it: a petal that swings at an invulnerable
    // flower still waits its 250 ms, and still pays for the swing below.
    world.ensure<HitCooldowns>(source.attacker).arm(victim, nowMillis + interval);

    // A claw opens on a fresh duellist exactly as it opens on a fresh mob.
    double swing = swingDamage(world, source.damage * beanDamageScale, source.critDamage, victim);
    const bool critical = source.critChance > 0 && rng_.chance(source.critChance);
    if (critical) swing *= source.critMultiplier;
    const auto beforeHitEvents = events_ ? events_->events().size() : 0;
    const DamageResult hit =
        applyDamage(world, victim, source.attacker, swing, nowMillis, source.hitKind);
    if (critical && events_) {
        if (const auto* id=world.tryGet<NetId>(victim)) events_->markCriticalSince(beforeHitEvents,id->value);
    }
    // And a fang drinks from one: what the victim's bar lost, never what a
    // shield, a cotton or a sponge took instead.
    if (source.lifesteal > 0.0 && hit.applied > 0.0) {
        stealLife(world, owner, hit.applied * source.lifesteal, nowMillis);
    }
    // Away from the FLOWER rather than from the petal: applyPvpDamage measures
    // from the attacker's own centre, and a spinning ring puts its petals on
    // the far side of the victim half the time. A refused swing -- dead,
    // invulnerable, same side -- shoves nobody, because the reference returns
    // above its knockback.
    if (!hit.refused && !hit.dodged && world.isAlive(owner)) {
        if (const Transform* attacker = world.tryGet<Transform>(owner)) {
            shoveFlower(world, victim, victimPosition - attacker->position);
        }
    }
    // The duel is where this petal earns its place: a flower that cannot heal
    // for ten seconds has lost its rose, its passive regeneration and its
    // yggdrasil at once. Gated on the same refusal the shove is -- a swing at
    // an invulnerable flower locks nothing, and neither does one it dodged.
    if (!hit.refused && !hit.dodged) {
        if (!hit.killed && source.shortensUndead)
            shortenUndeadWithScar(world, victim, source.rarity, nowMillis);
        applyNoHeal(world, victim, source.noHealDurationMillis, nowMillis);
    }

    // A flat point, never the victim's damage stat, and charged whatever the
    // swing did: the reference pays it outside applyPvpDamage's early returns,
    // so a swing at an invulnerable flower still wears the ring down. Skipped
    // only for a petal without a breakable pool -- the same exemption
    // that makes glass and infinity unbreakable against mobs. The environment
    // is the source because the reference attributes this to nobody, and
    // naming the victim would let their own faction refuse the cost.
    if (source.petalBreakable) {
        applyDamage(world, source.attacker, NULL_ENTITY, kPvpPetalSelfDamage, nowMillis,
                    DamageKind::Periodic);
    }
}

std::optional<CombatSystem::ShotImpact> CombatSystem::sweepShotTarget(
        const ShotSource& shot, const ShotTarget& target) {
    if(target.capsuleHalfWidth>0) {
        const double width=target.capsuleHalfWidth;
        const double halfSpan = std::max(0.0, target.radius - width);
        // Use a loose bound so hits near the capsule edge always reach the
        // existing swept-segment calculation and keep its contact position.
        const double bound = 2.0 * (shot.radius + width + halfSpan);
        const Vec2 fromTarget = target.position - shot.from;
        const Vec2 toTarget = target.position - shot.position;
        if (std::min(fromTarget.x, toTarget.x) > bound ||
            std::max(fromTarget.x, toTarget.x) < -bound ||
            std::min(fromTarget.y, toTarget.y) > bound ||
            std::max(fromTarget.y, toTarget.y) < -bound) return std::nullopt;
        const Vec2 span=Vec2::fromAngle(target.angle,halfSpan);
        const Vec2 a=shot.from,b=shot.position,c=target.position-span,d=target.position+span;
        const Vec2 u=b-a,v=d-c,w=a-c;
        const auto dot=[](Vec2 x,Vec2 y){return x.x*y.x+x.y*y.y;};
        const double uu=dot(u,u),vv=dot(v,v),uv=dot(u,v),uw=dot(u,w),vw=dot(v,w);
        double s=0,t=0;
        if(uu<1e-12)t=vv>1e-12?std::clamp(vw/vv,0.0,1.0):0;
        else if(vv<1e-12)s=std::clamp(-uw/uu,0.0,1.0);
        else {
            const double den=uu*vv-uv*uv;
            if(den>1e-12)s=std::clamp((uv*vw-uw*vv)/den,0.0,1.0);
            t=(uv*s+vw)/vv;
            if(t<0){t=0;s=std::clamp(-uw/uu,0.0,1.0);}
            else if(t>1){t=1;s=std::clamp((uv-uw)/uu,0.0,1.0);}
        }
        const Vec2 nearest=a+u*s,other=c+v*t,offset=other-nearest;
        if(offset.lengthSq()>(shot.radius+width)*(shot.radius+width))return std::nullopt;
        return ShotImpact{target.entity,offset,offset.lengthSq(),nearest};
    }
    // Sweep the whole segment. A fast shot can move past a body in one tick.
    const Vec2 toVictim = target.position - shot.from;
    Vec2 displacement;
    if (shot.flownLength > 1e-9) {
        const double along = clamp(toVictim.x * shot.axis.x + toVictim.y * shot.axis.y,
                                   0.0, shot.flownLength);
        displacement = shot.flown * (along / shot.flownLength);
    }
    const Vec2 nearest = shot.from + displacement;
    const Vec2 offset = target.position - nearest;
    const double distanceSquared = offset.lengthSq();
    if (target.squareHalf > 0) {
        if (!segmentTouchesSquare(shot.from, shot.position, target.position, target.squareHalf,
                                  target.angle, shot.radius)) return std::nullopt;
    } else {
        const double reach = shot.radius + target.radius;
        if (distanceSquared > reach * reach) return std::nullopt;
    }
    return ShotImpact{target.entity, offset, distanceSquared, nearest};
}

std::optional<CombatSystem::ShotImpact> CombatSystem::sweepLiveShotTarget(
        const World& world, const ShotSource& shot, Entity victim) {
    const auto* transform = world.tryGet<Transform>(victim);
    const auto* body = world.tryGet<Body>(victim);
    if (!transform || !body) return std::nullopt;
    ShotTarget target{victim,transform->position,body->radius,transform->angle,squareMobHalfExtent(world,victim)};
    if(const auto* projectile=world.tryGet<Projectile>(victim))target.capsuleHalfWidth=projectile->collisionHalfWidth;
    return sweepShotTarget(shot,target);
}

void CombatSystem::collectShotCandidates(const World& world, const SpatialGrid& grid,
                                        const ShotSource& shot, bool wildOnly, bool bodyGridOnly,
                                        bool frozen, std::vector<Entity>& entities,
                                        std::vector<ShotCandidate>& hits,
                                        SpatialGrid::QueryScratch* scratch) const {
    const auto* filed = filedShot(shot.entity);
    const auto* faction = world.tryGet<Faction>(shot.entity);
    const bool hostile = faction && faction->team == Team::Hostiles;
    const Vec2 midpoint = shot.from + shot.flown * 0.5;
    // Every point on the swept segment is at most half its length from
    // the midpoint. Round, immobile targets outside that enclosing circle
    // cannot hit. The main body grid keeps its original padded query.
    std::optional<double> shotReach;
    if (!queries_->needsLooseShotBounds[realmIndex(shot.realm)]) {
        shotReach = shot.radius + shot.flownLength * 0.5 + 1e-6;
    }
    queryBodies(grid, shot.realm, midpoint,
                shot.radius + shot.flownLength * 0.5 + shot.travelled + kBroadphasePad,
                entities, filed ? filed->player : NULL_ENTITY, filed && filed->peaceful,
                shotReach, scratch, bodyGridOnly);

    for (const Entity victim : entities) {
        if (victim == shot.entity) continue;
        // An explicit hostile faction takes precedence over ownership and
        // never damages another hostile. Mirror reflection already ran.
        if (hostile) {
            const auto* victimFaction = world.tryGet<Faction>(victim);
            if (victimFaction && victimFaction->team == Team::Hostiles) continue;
        }
        if (!frozen) {
            if (!isShootable(world, victim) || (wildOnly && !isWildMob(world, victim))) continue;
            if (auto impact = sweepLiveShotTarget(world, shot, victim))
                hits.push_back({*impact, false});
            continue;
        }
        const auto slot = entityIndex(victim);
        if (slot >= shotTargets_.size()) continue;
        const auto& target = shotTargets_[slot];
        if (target.entity != victim || (wildOnly && !target.wild)) continue;
        // Spawned mobs carry Knockback. A mob without it can instead be
        // moved immediately by an earlier projectile. Keep it in order even
        // if its frozen position would miss, and sweep it live when reached.
        if (target.movesOnImpact) {
            hits.push_back({ShotImpact{victim, {}, 0, {}}, true});
        } else if (auto impact = sweepShotTarget(shot, target)) {
            hits.push_back({*impact, false});
        }
    }
}

void CombatSystem::prepareShotImpacts(World& world, const SpatialGrid& grid,
                                     bool bodyGridOnly) {
    shotPlans_.clear();
    const unsigned limit = workers ? std::min(workerLimit, workers->count()) : 0;
    if (!limit || shots_.size() < 128) return;

    std::fill(shotTargets_.begin(), shotTargets_.end(), ShotTarget{});
    queries_->meleeTargets.eachArchetype([&](Archetype& layout) {
        if (layout.has(componentId<DropTag>()) ||
            (layout.has(componentId<GroundEffectTag>()) && !layout.has(componentId<Health>()))) return;
        const auto* mobTypes = layout.data<MobType>();
        const bool mob = layout.has(componentId<MobTag>());
        const bool wild = mob && !layout.has(componentId<Pet>());
        const bool movesOnImpact = mob && !layout.has(componentId<Knockback>());
        const Entity* entities = layout.entities();
        const Transform* transforms = layout.data<Transform>();
        const Body* bodies = layout.data<Body>();
        const Projectile* projectiles=layout.data<Projectile>();
        for (std::size_t i = 0; i < layout.count(); ++i) {
            const Entity entity = entities[i];
            const auto slot = entityIndex(entity);
            if (slot >= shotTargets_.size()) shotTargets_.resize(slot + 1);
            const auto& at = transforms[i];
            const double half = mobTypes ? squareMobHalfExtent(&mobTypes[i], bodies[i].radius) : 0.0;
            shotTargets_[slot] = {entity, at.position, bodies[i].radius, at.angle, half, wild, movesOnImpact};
            if(projectiles)shotTargets_[slot].capsuleHalfWidth=projectiles[i].collisionHalfWidth;
        }
    });

    constexpr std::size_t batchSize = 24;
    constexpr std::size_t maxImpactsPerLane = 16 * 1024;
    // Plan all sources in firing order. Each lane still bounds its impact
    // storage and falls back to a live sweep when that storage fills.
    shotPlans_.resize(shots_.size());
    shotScratch_.resize(limit + 1);
    for (auto& scratch : shotScratch_) scratch.planned.clear();
    workers->run((shotPlans_.size() + batchSize - 1) / batchSize, limit,
                 [&](std::size_t batch, unsigned lane) {
        auto& scratch = shotScratch_[lane];
        const auto end = std::min((batch + 1) * batchSize, shotPlans_.size());
        for (std::size_t i = batch * batchSize; i < end; ++i) {
            const auto& shot = shots_[i];
            const auto* projectile = world.tryGet<Projectile>(shot.entity);
            if (!projectile || projectile->remainingDistance <= 0 || world.has<Dead>(shot.entity) ||
                scratch.planned.size() == maxImpactsPerLane) continue;
            const auto begin = scratch.planned.size();
            collectShotCandidates(world, grid, shot, shot.wildOnly, bodyGridOnly, true,
                                  scratch.candidates, scratch.planned, &scratch.grid);
            if (scratch.planned.size() > maxImpactsPerLane) {
                scratch.planned.resize(begin);
                continue;
            }
            shotPlans_[i] = {begin, scratch.planned.size(), lane, true};
        }
    });
}

void CombatSystem::reflectShotsOffMirrors(World& world, const ContentRegistry& content) {
    const std::uint16_t mirrorIndex = content.petalIndex("void_mirror");
    if (mirrorIndex >= content.petalCount()) return;
    mirrors_.clear();
    queries_->petals.each([&](Entity e, PetalInstance& petal, Transform& at, Body& body) {
        if (petal.configIndex != mirrorIndex || world.has<Dead>(e)) return;
        if (!world.isAlive(petal.owner) || world.has<Dead>(petal.owner)) return;
        mirrors_.push_back({petal.owner, at.position, 4.0 * body.radius,
                            petalTier(petal.rarity), at.realm});
    });
    if (mirrors_.empty()) return;

    // Keep the first matching mirror within each map in the original order.
    std::stable_sort(mirrors_.begin(), mirrors_.end(), [](const MirrorGuard& a, const MirrorGuard& b) {
        return realmIndex(a.realm) < realmIndex(b.realm);
    });
    struct MirrorRange { std::size_t begin = 0; std::size_t end = 0; };
    std::array<MirrorRange, kMaxRealms> ranges{};
    for (std::size_t begin = 0; begin < mirrors_.size();) {
        const Realm realm = mirrors_[begin].realm;
        std::size_t end = begin + 1;
        while (end < mirrors_.size() && mirrors_[end].realm == realm) ++end;
        ranges[realmIndex(realm)] = {begin, end};
        begin = end;
    }

    queries_->projectiles.each([&](Entity e, Projectile& projectile, Transform& at, Body& body,
                                   Motion& motion) {
        const auto range = ranges[realmIndex(at.realm)];
        if (range.begin == range.end || world.has<Dead>(e)) return;
        const int tier = rarityIndex(projectile.rarity);
        // Only missiles: a shot lying still (a mine, a web) is not turned.
        if (motion.velocity.lengthSq() < 1e-9) return;
        // Swept from where the shot was last tick: a fast one would otherwise
        // step clean over the reach between two ticks.
        const Vec2 from = projectile.lastPosition;
        const Vec2 path = at.position - from;
        const double span = path.lengthSq();
        for (std::size_t i = range.begin; i < range.end; ++i) {
            const MirrorGuard& mirror = mirrors_[i];
            if (tier > mirror.tier + 1) continue;
            const Vec2 toMirror = mirror.position - from;
            const double along = span > 1e-12
                ? std::clamp((toMirror.x * path.x + toMirror.y * path.y) / span, 0.0, 1.0)
                : 1.0;
            const double reach = mirror.reach + body.radius;
            if ((toMirror - path * along).lengthSq() > reach * reach) continue;
            if (!canDamage(world, e, mirror.owner)) continue;

            projectile.owner = mirror.owner;
            projectile.creditTo = mirror.owner;
            if (auto* side = world.tryGet<Faction>(e)) {
                if (const auto* flower = world.tryGet<Faction>(mirror.owner)) *side = *flower;
            }
            if (tier > mirror.tier) projectile.damage *= 0.5;
            projectile.glitchInfecting = false;
            projectile.lastPosition = at.position;
            motion.velocity = -motion.velocity;
            at.angle += kPi;
            return;
        }
    });
}

void CombatSystem::tickProjectiles(World& world, const SpatialGrid& grid,
                                   const ContentRegistry& content, double nowMillis, double dt,
                                   bool bodyGridOnly) {
    reflectShotsOffMirrors(world, content);
    shots_.clear();
    queries_->projectiles.each([&](Entity e, Projectile& projectile, Transform& transform,
                                   Body& body, Motion& motion) {
        // Movement has already flown the shot this tick, so the budget spent
        // is the distance it just covered -- not one it is about to.
        const double speed = motion.velocity.length();
        Vec2 from=projectile.lastPosition,to=transform.position;
        double radius=body.radius;
        if(projectile.collisionHalfWidth>0) {
            const Vec2 extent=Vec2::fromAngle(transform.angle,std::max(0.0,body.radius-projectile.collisionHalfWidth));
            from-=extent;to+=extent;radius=projectile.collisionHalfWidth;
        }
        const Vec2 flown = to - from;
        const double length = flown.length();
        const Vec2 axis = length > 1e-9 ? flown * (1.0 / length) : Vec2{};
        shots_.push_back({e, to, from, radius,
                          speed * dt, body.mass, speed, transform.realm,
                          flown, axis, length, projectile.wildOnly});
    });

    prepareShotImpacts(world, grid, bodyGridOnly);
    std::vector<Entity> terminalShots;
    for (std::size_t index = 0; index < shots_.size(); ++index) {
        const ShotSource& shot = shots_[index];
        if (!world.isAlive(shot.entity) || world.has<Dead>(shot.entity)) continue;
        Projectile* projectile = world.tryGet<Projectile>(shot.entity);
        if (projectile == nullptr) continue;

        // MovementSystem already spent the distance budget while flying the
        // shot. Subtracting the same travel again halves every projectile's
        // configured range.
        if (projectile->remainingDistance <= 0.0) {
            if (!projectile->bouncesOffWalls || shot.flownLength <= 1e-9) {
                world.add<Dead>(shot.entity);
                continue;
            }
            terminalShots.push_back(shot.entity);
        }
        // Copied before anything structural: the component moves the moment a
        // victim is marked Dead.
        const double damage = projectile->damage;
        const double effectStrength=projectile->effectStrength;
        const std::uint16_t petalIndex = projectile->petalConfigIndex;
        const Rarity rarity = projectile->rarity;
        const bool infecting = projectile->glitchInfecting;
        const bool wildOnly = projectile->wildOnly;
        const Entity owner = projectile->owner;
        const std::uint8_t slot = projectile->slot;
        // A blueberry's shot lands as lightning, exactly as its body does.
        const DamageKind hitKind =
            petalIndex != kNoPetal && content.petal(petalIndex).lightningDamage
                ? DamageKind::Lightning
                : petalIndex != kNoPetal && content.petal(petalIndex).armorPiercing ? DamageKind::Piercing : DamageKind::Direct;

        const auto* plan = index < shotPlans_.size() ? &shotPlans_[index] : nullptr;
        const bool planned = plan && plan->ready && shot.wildOnly == wildOnly;
        if (!planned) {
            liveShotCandidates_.clear();
            // An overflowing worker leaves its immutable target geometry available.
            // Movable targets are swept live, in the original hit order.
            const bool reuseTargets = plan && !plan->ready && shot.wildOnly == wildOnly &&
                                      projectile->remainingDistance > 0.0;
            collectShotCandidates(world, grid, shot, wildOnly, bodyGridOnly, reuseTargets,
                                  candidates_, liveShotCandidates_);
        }
        const auto& candidates = planned ? shotScratch_[plan->lane].planned : liveShotCandidates_;
        const auto first = planned ? plan->begin : std::size_t{0};
        const auto last = planned ? plan->end : candidates.size();
        if (first == last) continue;

        // Resolve solid cover along the swept path before touching players behind it.
        const auto entryDistance = [&](Entity victim) {
            const auto& at = world.get<Transform>(victim);
            const auto& body = world.get<Body>(victim);
            const Vec2 delta = at.position - shot.from;
            const double along = delta.x * shot.axis.x + delta.y * shot.axis.y;
            const double reach = shot.radius + body.radius;
            const double across = std::max(0.0, delta.lengthSq() - along * along);
            return std::max(0.0, along - std::sqrt(std::max(0.0, reach * reach - across)));
        };
        Entity meteor = NULL_ENTITY;
        double coverDistance = std::numeric_limits<double>::infinity();
        const TeamInfo shotTeam = teamOf(world, shot.entity);
        if (shotTeam.known && shotTeam.team == Team::Hostiles && !wildOnly) {
            for (auto i = first; i < last; ++i) {
                const Entity victim = candidates[i].impact.victim;
                if (!world.has<PlantedMeteor>(victim) || !canHit(world, victim, shot.entity, nowMillis) ||
                    !sweepLiveShotTarget(world, shot, victim)) continue;
                const double distance = entryDistance(victim);
                if (distance < coverDistance) {
                    coverDistance = distance;
                    meteor = victim;
                }
            }
        }

        // Geometry is read-only. Check health and relations live, in the
        // same candidate order, before sorting the hits for this projectile.
        impacts_.clear();
        for (auto i = first; i < last; ++i) {
            const auto& candidate = candidates[i];
            const Entity victim = candidate.impact.victim;
            const auto impact = candidate.testLive ? sweepLiveShotTarget(world, shot, victim)
                                                  : std::optional<ShotImpact>{candidate.impact};
            if (!impact) continue;
            if (meteor != NULL_ENTITY && victim != meteor && entryDistance(victim) >= coverDistance) continue;
            // Above canHit() on purpose: a glitch shot marks the flower it
            // touches whether or not the hit itself is refused, and a
            // flower under respawn protection is not in `impacts_` at all.
            if (infecting && world.has<PlayerTag>(victim) && !world.has<Dead>(victim) &&
                canDamage(world, shot.entity, victim)) {
                markGlitched(world, victim);
            }
            if (!canHit(world, victim, shot.entity, nowMillis)) continue;
            impacts_.push_back(*impact);
        }
        if (impacts_.empty()) continue;

        // Nearest first. A shot arriving into a clump spends its health pool
        // front to back, and ordering by distance is the only answer that does
        // not depend on grid bucket order.
        std::sort(impacts_.begin(), impacts_.end(),
                         [meteor](const ShotImpact& a, const ShotImpact& b) {
                             if (meteor != NULL_ENTITY && (a.victim == meteor || b.victim == meteor)) {
                                 return a.victim != meteor;
                             }
                             return a.distanceSquared < b.distanceSquared;
                         });

        // Only resolved once something is actually going to be hit: this is a
        // per-shot table lookup and most shots spend their life hitting
        // nothing at all.
        const bool hasStats = petalIndex != kNoPetal && !world.get<Projectile>(shot.entity).fragmented;
        const PetalStats stats =
            hasStats ? content.petalStats(petalIndex, rarity) : PetalStats{};
        const double reloadInterval = hasStats ? stats.damageIntervalMillis
                                               : kPetalHitIntervalMillis;
        const bool summons =
            hasStats && content.petal(petalIndex).projectile.summonMobIndex != kInvalidIndex;
        const bool shatteringMarble = hasStats && content.petal(petalIndex).id == "magic_marble";

        for (const ShotImpact& impact : impacts_) {
            if (!world.isAlive(shot.entity) || world.has<Dead>(shot.entity)) break;
            // Re-tested per victim rather than trusted from the gather: an
            // earlier impact in this same pass may have killed this one, and a
            // corpse must not pay out twice.
            if (!canHit(world, impact.victim, shot.entity, nowMillis)) continue;

            // The shot's own ledger, so a body it is PASSING THROUGH is hit
            // once per damage interval and not once per tick of overlap. It is
            // added at spawn precisely so arming it here cannot relocate the
            // shot out from under this loop.
            if (const HitCooldowns* ledger = world.tryGet<HitCooldowns>(shot.entity)) {
                if (!ledger->ready(impact.victim, nowMillis)) continue;
            }

            // Charged BEFORE the hit lands, because a victim marked Dead by it
            // no longer has the components this reads.
            const double rawCost = bodyDamageOf(world, content, impact.victim);
            const bool marble = shatteringMarble || (hasStats && content.petal(petalIndex).id == "marble");
            const double cost=marble && std::isfinite(rawCost)?std::max(0.0,rawCost-stats.petalArmor):rawCost;
            const bool victimIsShot = world.has<Projectile>(impact.victim);
            const bool victimIsMob = world.has<MobTag>(impact.victim);

            double shotDamage = damage;
            if (const auto* mods = world.tryGet<PlayerModifiers>(owner); hasStats && mods && petalTier(rarity) <= mods->magicOpalTier)
                if (const auto* roll = world.tryGet<MagicOpalRoll>(owner)) shotDamage *= roll->multiplier;
            if (hasStats && content.petal(petalIndex).id == "magic_job_application" && isCorrupted(world, impact.victim))
                shotDamage *= 100.0;
            if (const auto* modifiers = world.tryGet<PlayerModifiers>(owner);
                hasStats && modifiers && modifiers->opalTiers[petalTier(rarity)]) {
                const int tier = std::min(petalTier(rarity), rarityIndex(Rarity::Apex));
                if (rng_.chance(1.0 / (12.0 - tier)))
                    shotDamage *= 1.5 + tier * (1.5 / 8.0);
            }
            const Vec2 numberAt = shot.flownLength > 1e-9
                ? shot.from + shot.axis * std::min(shot.flownLength, entryDistance(impact.victim))
                : impact.at;
            DamageResult hit = applyDamageWithRuby(world, impact.victim, shot.entity, shotDamage,
                                                   nowMillis, hitKind, impact.victim, numberAt);
            // A shot of zero is refused before applyDamage rolls evasion, and
            // its riders below land regardless -- so it is rolled here, as a
            // zero-damage petal's swing is in resolveMelee.
            if (hit.refused && damage == 0.0 && rollDodge(world, impact.victim, nowMillis)) {
                hit.dodged = true;
            }

            // RE-FETCHED, never carried across applyDamage(). Marking the
            // victim Dead moves it between archetypes, and an archetype
            // swap-removes -- so when the victim is ANOTHER SHOT, the row that
            // slides into its slot may be this one's. A ledger pointer taken
            // before the call is exactly the dangling write the bullet-vs-
            // bullet rule made reachable.
            //
            // Armed on a dodge too: a shot passing through a fly gets one
            // roll per damage interval, not one per tick of overlap.
            if (HitCooldowns* ledger = world.tryGet<HitCooldowns>(shot.entity)) {
                ledger->arm(impact.victim, nowMillis + reloadInterval);
            }

            // A dodged shot flew past: no riders, and none of its pool spent
            // on a body it never touched.
            if (hit.dodged) continue;

            // Recorded rather than answered: a summon is a create, and this
            // loop may not change the world's shape under the shot.
            const bool electron = hasStats &&
                (content.petal(petalIndex).id == "electron_missile" || content.petal(petalIndex).id=="arctic_electron_missile" ||
                 content.petal(petalIndex).id == "magic_electron_missile");
            const bool plasma=hasStats && content.petal(petalIndex).id=="void_plasma";
            const auto* landed = world.tryGet<Projectile>(shot.entity);
            if (landed && landed->coralElements && !hit.refused && !hit.dodged && !hit.killed) {
                const double rate = landed->damage * .2;
                applyBurn(world, impact.victim, shot.entity, rate, nowMillis);
                applyPoison(world,impact.victim,shot.entity,rate,2000,nowMillis,true);
            }
            const auto& ammoId = hasStats ? content.petal(petalIndex).id : std::string{};
            const bool magicImpact = hasStats && (ammoId == "magic_rocket" || ammoId == "magic_grapes" ||
                ammoId == "magic_arctic_blueberries" || ammoId == "magic_cherry");
            const bool impactEffect = electron || plasma || magicImpact;
            if (!hit.refused && (victimIsMob ? (summons || impactEffect) :
                               (impactEffect && isPracticeDummy(world, content, impact.victim)))) {
                shotLandings_.push_back({owner, slot, petalIndex, rarity, shot.realm,
                                         impact.at, impact.victim,effectStrength});
            }

            // Riders belong to flesh. A shot cannot be poisoned, slowed or
            // shoved -- it has no Afflictions, and nothing but its own flight
            // (MovementSystem::moveProjectiles) moves it.
            if (!hit.killed && !victimIsShot && hasStats) {
                // The petal's own `knockback` stat is the RING's, not the
                // volley's: projectileCollision.ts stamps a flat force on a
                // mob whatever fired it. What replaces the flat number here is
                // the shot's momentum, which is the same value for stock
                // ammunition and grows only when the shot itself does -- so a
                // mob takes that push ALONE. Queueing the flat force as well
                // would shove it twice for one hit.
                if (victimIsMob) {
                    pushFromImpact(world, impact.victim, impact.offset, shot.mass, shot.speed);
                } else {
                    const auto& ammo = content.petal(petalIndex).id;
                    const bool wildStinger = !hit.refused && world.has<PlayerTag>(impact.victim) &&
                        isWildMob(world, owner) && (ammo == "hornet_missile" || ammo == "wasp_missile");
                    // A short fixed shove keeps a high-tier volley from throwing
                    // flowers across an entire corridor.
                    const double strength = wildStinger ? 48.0 : stats.knockback;
                    applyKnockback(world, impact.victim, impact.offset, strength);
                }
                const auto& ammoConfig = content.petal(petalIndex);
                if (!hit.refused && ammoConfig.burnPerSecond > 0 &&
                    (isWildMob(world, impact.victim) || isPracticeDummy(world, content, impact.victim)))
                    ignite(world, impact.victim, shot.entity, owner,
                        ammoConfig.burnPerSecond * referenceOffenseScale(ammoConfig,rarity)*effectStrength, ammoConfig.burnMillis, nowMillis);
                applyPoison(world, impact.victim, shot.entity, stats.poisonPerSecond*effectStrength,
                            stats.poisonDurationMillis, nowMillis, ammoConfig.id == "magic_viper");
                const bool icicle = ammoConfig.id == "arctic_icicle";
                const auto* shooter = world.tryGet<MobType>(owner);
                bool coldVolley = false;
                if (shooter && (icicle || ammoConfig.id == "arctic_snowball")) {
                    const auto& species = content.mob(shooter->configIndex).id;
                    coldVolley = species == "arctic_snowman" || species == "arctic_tick" ||
                                 species == "arctic_ice_dragon" || species == "arctic_ice_hornet";
                }
                if (coldVolley && !hit.refused && isStanding(world, impact.victim)) {
                    // The hornet's cold status uses its own brief orbit debuff.
                    // The equippable icicle's immobilization scales separately.
                    const double factor = icicle ? 0.60 : stats.slowFactor;
                    const double duration = icicle ? 200.0 : stats.slowDurationMillis;
                    if (world.has<PlayerTag>(impact.victim)) {
                        world.ensure<Afflictions>(impact.victim).applyFrost(factor, duration, nowMillis);
                    } else {
                        applySlow(world, impact.victim, factor, duration, rarity, nowMillis, true);
                    }
                } else if (!coldVolley) {
                    applySlow(world, impact.victim, stats.slowFactor, stats.slowDurationMillis,
                              rarity, nowMillis, false, icicle);
                }
                applyNoHeal(world, impact.victim, stats.noHealDurationMillis, nowMillis);
                if (content.petal(petalIndex).id == "antibody")
                    applyArmorShred(world, impact.victim, stats.armorReduction, nowMillis);
            }

            // Hyper-only PvE shots spend their single impact on contact.
            // Other ammunition can penetrate while its durability pool lasts.
            if (!world.isAlive(shot.entity) || world.has<Dead>(shot.entity)) break;
            if (((wildOnly || electron || impact.victim == meteor) && !hit.refused) ||
                !std::isfinite(cost) || (!marble && cost <= 0.0)) {
                world.add<Dead>(shot.entity);
                break;
            }
            Health* health = world.tryGet<Health>(shot.entity);
            if (health == nullptr) {
                world.add<Dead>(shot.entity);
                break;
            }
            // Magic keeps the armor, but even a gentle impact wears the cracked shell.
            const bool wearsShell = shatteringMarble && !hit.refused && !hit.dodged;
            const double durabilitySpent = wearsShell
                ? std::max(cost, health->max / kMagicMarbleImpactsBeforeShatter) : cost;
            health->current -= durabilitySpent;
            if (health->current <= (shatteringMarble ? health->max * 1e-9 : 0.0)) {
                health->current = 0.0;
                world.add<Dead>(shot.entity);
                break;
            }
            // Through it and still flying: the hit cost it, so it flashes.
            const double share = health->max > 0.0 ? durabilitySpent / health->max : 0.0;
            flashStruck(world, shot.entity, durabilitySpent, share, false);
        }
    }
    for (Entity terminal : terminalShots) {
        if (world.isAlive(terminal) && !world.has<Dead>(terminal)) world.add<Dead>(terminal);
    }
}

namespace {

/// Columns and rows of the shot index fit in 24 bits each. A coordinate past
/// the last one -- sixty-four units times sixteen million, far outside any
/// realm -- shares the border cell rather than wrapping into another row.
constexpr std::uint32_t kShotCellLimit = 1u << 24;

std::uint32_t shotCellCoord(double at, double cellSize) {
    const double c = std::floor(at / cellSize);
    // Failed comparisons, so NaN lands in cell 0 rather than in a cast.
    if (!(c > 0.0)) return 0;
    if (!(c < static_cast<double>(kShotCellLimit))) return kShotCellLimit - 1;
    return static_cast<std::uint32_t>(c);
}

std::uint64_t shotCellKey(Realm realm, std::uint32_t row, std::uint32_t column) {
    return (static_cast<std::uint64_t>(realmIndex(realm)) << 48) |
           (static_cast<std::uint64_t>(row) << 24) | column;
}

} // namespace

void CombatSystem::fileShots(World& world) {
    // Retire last phase's slots before the list they index is dropped.
    for (const FiledShot& filed : filedShots_) {
        const std::uint32_t index = entityIndex(filed.entity);
        if (index < shotSlot_.size()) shotSlot_[index] = kNoShotSlot;
    }
    filedShots_.clear();
    shotCells_.clear();
    shotRuns_.clear();
    shotBounds_.fill(ShotRealmBounds{});
    queries_->needsLooseShotBounds.fill(false);

    // All ordinary shots with the same first ownership link resolve to the
    // same player in this read-only pass. Rebuild after movement or reflection.
    struct OwnerEntry {
        Entity owner{}, credit{};
        bool valid{}, peaceful{};
    };
    std::array<OwnerEntry, 256> owners{};
    bool hasPeaceful = false, hasNonPeaceful = false;
    queries_->filedShots.eachArchetype([&](Archetype& layout) {
        const auto* transforms = layout.data<Transform>();
        const auto* bodies = layout.data<Body>();
        const auto* projectiles = layout.data<Projectile>();
        const auto* factions = layout.data<Faction>();
        const bool isPlayer = layout.has(componentId<PlayerTag>());
        const bool hybrid = isPlayer || layout.has(componentId<MobTag>()) || layout.has(componentId<MobType>());
        for (std::size_t i = 0; i < layout.count(); ++i) {
            const Entity e = layout.entities()[i];
            const auto& transform = transforms[i];
            const auto& body = bodies[i];
            const std::uint32_t index = entityIndex(e);
            if (index >= shotSlot_.size()) {
                shotSlot_.resize(std::max<std::size_t>(static_cast<std::size_t>(index) + 1,
                                                       shotSlot_.size() * 2),
                                 kNoShotSlot);
            }
            const std::uint32_t slot = static_cast<std::uint32_t>(filedShots_.size());
            shotSlot_[index] = slot;
            // Resolved now, once, rather than per pair: nothing in a combat phase
            // destroys an entity, so the answer cannot change before it is read.
            Entity player;
            bool peaceful;
            if (projectiles && factions && !isPlayer) {
                const Projectile& shot = projectiles[i];
                const Entity owner = shot.creditTo != NULL_ENTITY ? shot.creditTo : shot.owner;
                auto& cached = owners[entityIndex(owner) % owners.size()];
                if (!cached.valid || cached.owner != owner) {
                    const Entity credit = creditedPlayer(world, e);
                    cached = {owner, credit, true, permitsPeacefulShots(world, credit)};
                }
                player = cached.credit;
                peaceful = factions[i].team == Team::Players && !factions[i].friendlyFireEnabled &&
                           cached.peaceful;
            } else {
                player = creditedPlayer(world, e);
                peaceful = peacefulPlayerSide(world, e, player);
            }
            filedShots_.push_back({e, player, transform.position, body.radius, peaceful});

            // A position no query can reach is not filed, as the main grid files
            // none: it is still a shot, it is just never a candidate.
            const Vec2 at = transform.position;
            if (!std::isfinite(at.x) || !std::isfinite(at.y)) continue;
            hasPeaceful |= peaceful;
            hasNonPeaceful |= !peaceful;
            auto& bounds = shotBounds_[realmIndex(transform.realm)];
            // A hybrid mob may move when hit or have a square collider. An
            // oversized capsule can extend beyond its filing radius as well.
            if (hybrid || body.radius < 0 ||
                (projectiles && projectiles[i].collisionHalfWidth > body.radius)) {
                queries_->needsLooseShotBounds[realmIndex(transform.realm)] = true;
            }
            if (body.radius > bounds.maxRadius) bounds.maxRadius = body.radius;
            const std::uint32_t row = shotCellCoord(at.y, kShotCellSize);
            bounds.firstRow = std::min(bounds.firstRow, row);
            bounds.lastRow = std::max(bounds.lastRow, row);
            shotCells_.push_back(
                {shotCellKey(transform.realm, row, shotCellCoord(at.x, kShotCellSize)), slot});
        }
    });
    // Slot as the tiebreak, so shots sharing a cell come back in filing order
    // and a query's answer does not depend on how the sort felt about ties.
    if (shotCells_.size() < 1024) {
        std::sort(shotCells_.begin(), shotCells_.end(), [](const ShotCell& a, const ShotCell& b) {
            return a.key != b.key ? a.key < b.key : a.slot < b.slot;
        });
    } else {
        // Filing appends in slot order. Stable byte passes preserve that tie
        // order and skip constant bytes (common for nearby shots on one map).
        auto& scratch = queries_->shotSortScratch;
        scratch.resize(shotCells_.size());
        std::uint64_t varying = 0;
        const auto firstKey = shotCells_.front().key;
        for (const auto& cell : shotCells_) varying |= cell.key ^ firstKey;
        for (unsigned shift = 0; varying; varying >>= 8, shift += 8) {
            if (!(varying & 255)) continue;
            std::array<std::size_t, 256> offsets{};
            for (const auto& cell : shotCells_) ++offsets[(cell.key >> shift) & 255];
            std::size_t begin = 0;
            for (auto& offset : offsets) {
                const auto count = offset;
                offset = begin;
                begin += count;
            }
            for (const auto& cell : shotCells_) scratch[offsets[(cell.key >> shift) & 255]++] = cell;
            shotCells_.swap(scratch);
        }
    }
    auto& eligible = queries_->nonPeacefulByCell;
    auto& ends = queries_->nonPeacefulEnds;
    eligible.clear();
    ends.clear();
    const bool mixed = hasPeaceful && hasNonPeaceful;
    for (std::size_t i = 0; i < shotCells_.size(); ++i) {
        const auto& cell = shotCells_[i];
        const auto& shot = filedShots_[cell.slot];
        const Entity player = shot.player;
        if (shotRuns_.empty() || shotRuns_.back().key != cell.key) {
            if (mixed) ends.push_back(eligible.size());
            shotRuns_.push_back({cell.key, i, i + 1, player, shot.peaceful});
        } else {
            auto& run = shotRuns_.back();
            run.end = i + 1;
            if (run.commonPlayer != player) run.commonPlayer = NULL_ENTITY;
            run.allPeaceful = run.allPeaceful && shot.peaceful;
        }
        if (mixed && !shot.peaceful) eligible.push_back(shot);
    }
    if (mixed) ends.push_back(eligible.size());
}

void CombatSystem::queryShots(Realm realm, Vec2 center, double radius,
                              std::vector<Entity>& out, Entity excludePlayer,
                              bool excludePeacefulShots,
                              std::optional<double> meleeRadius) const {
    const auto& bounds = shotBounds_[realmIndex(realm)];
    if (bounds.firstRow > bounds.lastRow) return;
    if (!std::isfinite(center.x) || !std::isfinite(center.y)) return;
    if (!(radius >= 0.0)) radius = 0.0;   // negative, and NaN
    // Widened by the biggest shot, because a shot is filed only under its
    // centre: one whose edge reaches into the circle may sit a radius outside.
    const double reach = (meleeRadius ? std::min(radius, *meleeRadius) : radius) + bounds.maxRadius;
    const std::uint32_t x0 = shotCellCoord(center.x - reach, kShotCellSize);
    const std::uint32_t x1 = shotCellCoord(center.x + reach, kShotCellSize);
    const std::uint32_t y0 = std::max(shotCellCoord(center.y - reach, kShotCellSize), bounds.firstRow);
    const std::uint32_t y1 = std::min(shotCellCoord(center.y + reach, kShotCellSize), bounds.lastRow);
    for (std::uint32_t row = y0; row <= y1; ++row) {
        const std::uint64_t first = shotCellKey(realm, row, x0);
        const std::uint64_t last = shotCellKey(realm, row, x1);
        auto it = std::lower_bound(shotRuns_.begin(), shotRuns_.end(), first,
                                   [](const ShotCellRun& cell, std::uint64_t key) {
                                       return cell.key < key;
                                   });
        for (; it != shotRuns_.end() && it->key <= last; ++it) {
            if (excludePlayer != NULL_ENTITY && it->commonPlayer == excludePlayer) continue;
            if (excludePeacefulShots && it->allPeaceful) continue;
            const auto append = [&](const FiledShot& shot) {
                if (excludePlayer != NULL_ENTITY && shot.player == excludePlayer) return;
                if (meleeRadius) {
                    const double contactReach = *meleeRadius + shot.radius;
                    if ((shot.position - center).lengthSq() > contactReach * contactReach) return;
                }
                out.push_back(shot.entity);
            };
            if (excludePeacefulShots && !queries_->nonPeacefulEnds.empty()) {
                const auto run = it - shotRuns_.begin();
                for (auto i = queries_->nonPeacefulEnds[run]; i < queries_->nonPeacefulEnds[run + 1]; ++i)
                    append(queries_->nonPeacefulByCell[i]);
            } else {
                for (auto i = it->begin; i < it->end; ++i)
                    append(filedShots_[shotCells_[i].slot]);
            }
        }
    }
}

const CombatSystem::FiledShot* CombatSystem::filedShot(Entity e) const {
    const std::uint32_t index = entityIndex(e);
    if (index >= shotSlot_.size()) return nullptr;
    const std::uint32_t slot = shotSlot_[index];
    if (slot == kNoShotSlot || filedShots_[slot].entity != e) return nullptr;
    return &filedShots_[slot];
}

void CombatSystem::queryBodies(const SpatialGrid& grid, Realm realm, Vec2 center, double radius,
                               std::vector<Entity>& out, Entity excludeShotPlayer,
                               bool excludePeacefulShots,
                               std::optional<double> meleeRadius, SpatialGrid::QueryScratch* scratch,
                               bool bodyGridOnly) const {
    if (scratch) grid.query(realm, center, radius, out, *scratch);
    else grid.query(realm, center, radius, out);
    // The server's grid files no shots, but a focused simulation that builds
    // its grid from every body does -- and must not be handed a shot twice.
    if (!bodyGridOnly && !filedShots_.empty()) {
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](Entity e) { return isFiledShot(e); }),
                  out.end());
    }
    queryShots(realm, center, radius, out, excludeShotPlayer, excludePeacefulShots, meleeRadius);
}

void CombatSystem::pushFromImpact(World& world, Entity victim, Vec2 offset, double shotMass,
                                  double shotSpeed) {
    if (!world.isAlive(victim)) return;
    // Flowers are not pushed from here. applyKnockback above already writes
    // the flower's displacement and movement drains it; doubling that up would
    // be two shoves for one hit.
    if (!world.has<MobTag>(victim)) return;

    Transform* transform = world.tryGet<Transform>(victim);
    if (transform == nullptr) return;
    const Body* body = world.tryGet<Body>(victim);
    const double push = projectilePush(shotMass, shotSpeed, body ? body->mass : 1.0);
    if (!(push > 0.0)) return;

    const Vec2 direction = offset.normalized();
    if (direction.lengthSq() < 1e-12) return;   // exactly co-located: no direction to push along

    // Owed to Knockback::carry rather than to the pending impulse. The impulse
    // holds ONE pending shove and the next hit replaces it, so a petal landing
    // on the same tick would erase the shot's momentum outright; the carry
    // accumulates, and the movement pass spends it as a recoil through the
    // wall resolver. A plain field write, safe inside the impact loop. Only a
    // mob without the component -- none is spawned that way -- falls back to
    // an immediate displacement.
    if (Knockback* kb = world.tryGet<Knockback>(victim)) {
        // A four-pea burst, especially fired during a bubble dash, must not
        // bank an unbounded chain of momentum shoves on one monster.
        const double limit = std::max(kProjectileMaxPush, kb->carry.length());
        kb->carry = (kb->carry + direction * push).clampedLength(limit);
        return;
    }
    transform->position += direction * push;
}

} // namespace flix
