#pragma once

#include <cstdint>
#include <vector>

namespace flix {

class MapData;

/// Resolves the mobs named by a map's spawn bands and their children.
std::vector<std::uint16_t> mapMobIndices(const MapData& map);

} // namespace flix
