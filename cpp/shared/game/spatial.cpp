#include "shared/game/spatial.h"

#include <algorithm>
#include <cmath>

#include "shared/game/realm.h"
#include "shared/game/terrain.h"

namespace flix {

namespace {

/// Sizes one layer to a span, keeping its buckets when the count is unchanged.
void fitLayer(int& cols, int& rows, std::vector<std::vector<Entity>>& buckets,
              std::vector<std::uint32_t>& bucketEpoch, double span, double invCellSize,
              int maxAxisCells) {
    const double side = std::isfinite(span) && span > 0 ? span : kWorldSize;
    const int axis = clamp(static_cast<int>(std::ceil(std::min(side, 1e9) * invCellSize)), 1,
                           maxAxisCells);
    if (axis == cols && axis == rows && !buckets.empty()) return;
    cols = axis;
    rows = axis;
    const std::size_t cells = static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows);
    buckets.assign(cells, {});
    bucketEpoch.assign(cells, 0);
}

} // namespace

SpatialGrid::SpatialGrid(double cellSize) {
    cellSize_ = (std::isfinite(cellSize) && cellSize > 1.0) ? cellSize : kDefaultCellSize;
    invCellSize_ = 1.0 / cellSize_;

    // No Terrain yet. The two generated realms are fixed shapes the class can
    // size on its own; the overworld gets the default world; every other map
    // realm gets ONE cell until sizeToRealms() says what shape it really is,
    // because sizing sixty layers nobody staged a map for to the full world
    // would be a few hundred thousand empty buckets.
    for (int i = 0; i < kMaxRealms; ++i) {
        const Realm realm = static_cast<Realm>(i);
        double span = 0.0;
        if (realm == Realm::Overworld) span = kWorldSize;
        else if (realm == Realm::Arena) span = kArenaWorldSize;
        else if (realm == Realm::Maze) span = activeMaze().worldSize();
        Layer& layer = layers_[static_cast<std::size_t>(i)];
        fitLayer(layer.cols, layer.rows, layer.buckets, layer.bucketEpoch, span, invCellSize_,
                 span > 0.0 ? kMaxAxisCells : 1);
    }
}

void SpatialGrid::sizeToRealms(const Terrain& terrain) {
    for (int i = 0; i < kMaxRealms; ++i) {
        const Realm realm = static_cast<Realm>(i);
        Layer& layer = layers_[static_cast<std::size_t>(i)];
        // A map realm nothing was staged for stays one cell: nothing can be
        // in it, and a layer the size of the world for it is pure waste.
        const bool live = !isWorldRealm(realm) || terrain.hasMap(realm);
        fitLayer(layer.cols, layer.rows, layer.buckets, layer.bucketEpoch,
                 terrain.realmSize(realm), invCellSize_, live ? kMaxAxisCells : 1);
    }
    // Every bucket is new or reused, and any that was reused may still hold
    // last tick's entities under a live epoch. Retire the lot.
    epoch_ = 1;
    distinctEntities_ = false;
    for (Layer& layer : layers_) {
        std::fill(layer.bucketEpoch.begin(), layer.bucketEpoch.end(), 0);
    }
    inserted_ = 0;
}

void SpatialGrid::fitEmptyRealm(Realm realm, double span) {
    Layer& target = layer(realm);
    fitLayer(target.cols, target.rows, target.buckets, target.bucketEpoch, span,
             invCellSize_, kMaxAxisCells);
}

int SpatialGrid::cellIndex(double offset, double invCellSize, int axisCells) {
    // After clamping to the positive range, integer truncation is floor.
    const double c = offset * invCellSize;
    // Written as failed comparisons so NaN lands in cell 0 instead of taking
    // an undefined trip through the cast.
    if (!(c > 0.0)) return 0;
    if (!(c < static_cast<double>(axisCells))) return axisCells - 1;
    return static_cast<int>(c);
}

void SpatialGrid::clearForDistinctEntities() {
    clear();
    distinctEntities_ = true;
}

void SpatialGrid::clear() {
    distinctEntities_ = false;
    inserted_ = 0;
    if (++epoch_ == 0) {
        // Once every four billion ticks the epoch wraps onto the value stale
        // buckets already carry, so retire them all for real. Never hit in a
        // session; cheap enough that it does not need to be.
        for (Layer& layer : layers_) {
            std::fill(layer.bucketEpoch.begin(), layer.bucketEpoch.end(), 0);
        }
        epoch_ = 1;
    }
}

void SpatialGrid::insert(Entity e, Realm realm, Vec2 position, double radius) {
    if (e == NULL_ENTITY) return;
    // A NaN position cannot be found by any query, so filing it would only
    // give a later reader a corrupt candidate to trip over.
    if (!std::isfinite(position.x) || !std::isfinite(position.y)) return;
    if (!std::isfinite(radius) || radius < 0.0) radius = 0.0;

    const std::uint32_t idx = entityIndex(e);
    auto& stamp = defaultScratch_.stamp;
    if (idx >= stamp.size()) {
        // Grown here rather than in query(), so the read path never allocates.
        stamp.resize(std::max<std::size_t>(static_cast<std::size_t>(idx) + 1, stamp.size() * 2), 0);
    }

    Layer& target = layer(realm);
    const auto file = [&](std::size_t bucket) {
        if (target.bucketEpoch[bucket] != epoch_) {
            target.buckets[bucket].clear();
            target.bucketEpoch[bucket] = epoch_;
        }
        target.buckets[bucket].push_back(e);
    };
    if (radius == 0.0) {
        file(target.bucketAt(cellX(realm, position.x), cellY(realm, position.y)));
    } else {
        const int x0 = cellX(realm, position.x - radius);
        const int x1 = cellX(realm, position.x + radius);
        const int y0 = cellY(realm, position.y - radius);
        const int y1 = cellY(realm, position.y + radius);
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) file(target.bucketAt(cx, cy));
        }
    }
    ++inserted_;
}

void SpatialGrid::query(Realm realm, Vec2 center, double radius, std::vector<Entity>& out) const {
    query(realm, center, radius, out, defaultScratch_);
}

void SpatialGrid::query(Realm realm, Vec2 center, double radius, std::vector<Entity>& out,
                        QueryScratch& scratch) const {
    if (!std::isfinite(radius) || radius < 0.0) radius = 0.0;
    queryRect(realm, Vec2{center.x - radius, center.y - radius},
              Vec2{center.x + radius, center.y + radius}, out, scratch);
}

void SpatialGrid::queryRect(Realm realm, const Rect& area, std::vector<Entity>& out) const {
    queryRect(realm, Vec2{area.left(), area.top()}, Vec2{area.right(), area.bottom()}, out);
}

void SpatialGrid::queryRect(Realm realm, Vec2 min, Vec2 max, std::vector<Entity>& out) const {
    queryRect(realm, min, max, out, defaultScratch_);
}

void SpatialGrid::queryRect(Realm realm, Vec2 min, Vec2 max, std::vector<Entity>& out,
                            QueryScratch& scratch) const {
    queryBuckets(realm, min, max, out, &scratch);
}

void SpatialGrid::queryPointBuckets(Realm realm, Vec2 min, Vec2 max, std::vector<Entity>& out) const {
    queryBuckets(realm, min, max, out, nullptr);
}

void SpatialGrid::queryBuckets(Realm realm, Vec2 min, Vec2 max, std::vector<Entity>& out,
                               QueryScratch* scratch) const {
    out.clear();
    if (!std::isfinite(min.x) || !std::isfinite(min.y) || !std::isfinite(max.x) || !std::isfinite(max.y)) {
        return;
    }
    if (min.x > max.x) std::swap(min.x, max.x);
    if (min.y > max.y) std::swap(min.y, max.y);

    const Layer& source = layer(realm);
    const int x0 = cellX(realm, min.x);
    const int x1 = cellX(realm, max.x);
    const int y0 = cellY(realm, min.y);
    const int y1 = cellY(realm, max.y);
    if (distinctEntities_ && x0 == x1 && y0 == y1) {
        const auto bucket = source.bucketAt(x0, y0);
        if (source.bucketEpoch[bucket] == epoch_) {
            const auto& entries = source.buckets[bucket];
            out.assign(entries.begin(), entries.end());
        }
        return;
    }

    if (scratch) {
        if (scratch->stamp.size() < defaultScratch_.stamp.size())
            scratch->stamp.resize(defaultScratch_.stamp.size(), 0);
        if (++scratch->epoch == 0) {
            std::fill(scratch->stamp.begin(), scratch->stamp.end(), 0);
            scratch->epoch = 1;
        }
    }

    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const std::size_t b = source.bucketAt(cx, cy);
            if (source.bucketEpoch[b] != epoch_) continue;   // last tick's contents
            const auto& bucket = source.buckets[b];
            if (!scratch) {
                out.insert(out.end(), bucket.begin(), bucket.end());
                continue;
            }
            for (const Entity e : bucket) {
                const std::uint32_t idx = entityIndex(e);
                if (scratch->stamp[idx] == scratch->epoch) continue;
                scratch->stamp[idx] = scratch->epoch;
                out.push_back(e);
            }
        }
    }
}

std::size_t SpatialGrid::reservedEntries() const {
    std::size_t total = 0;
    for (const Layer& layer : layers_) {
        for (const std::vector<Entity>& bucket : layer.buckets) total += bucket.capacity();
    }
    return total;
}

} // namespace flix
