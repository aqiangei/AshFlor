// Soil wears the florr wiki's picture, and its icon is drawn at gardn's size.
#include "test.h"
#include "client/ui/item_tile.h"
#include "server_harness.h"
#include <cstdio>
#include <string>
using namespace flix;
using flix::testsupport::dataDir;

TEST(soil_icon_is_drawn_at_gardns_size) {
    std::string error;
    CHECK(loadContent(dataDir(), error));
    SpriteCache sprites;
    sprites.build(content(), dataDir());
    const std::uint16_t soil = content().petalIndex("soil");
    const std::uint16_t basic = content().petalIndex("basic");
    if (soil == kInvalidIndex || basic == kInvalidIndex) { CHECK(false); return; }
    CHECK(sprites.petalDrawable(soil));

    // basic hugs its box and gardn draws it 23 units wide: one gardn unit.
    const double unit = ui::petalIconMetric(basic, 1.0).diameter / 23.0;
    const double size = content().petal(soil).size;
    const double picture = ui::petalIconMetric(soil, size, &sprites).diameter * sprites.petalCoverage(soil);
    std::printf("  soil coverage %.4f, picture %.3f units (gardn draws 24)\n", sprites.petalCoverage(soil),
                picture / (unit * size));
    CHECK_NEAR(picture / (unit * size), 24.0, 0.24);
}
