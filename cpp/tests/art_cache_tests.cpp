#include "test.h"

#include "client/render/art_cache.h"
#include "client/render/mob_art.h"
#include "client/render/sprites.h"
#include "shared/game/config.h"
#include "fixture_content.h"
#include "render_rig.h"
#include "test_data.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The web tile cache rests on one property of the artwork: that a document
// without a SMIL timeline draws the same picture at every `timeSeconds`, so
// one rasterisation of it can stand in for every frame. `animated()` is what
// answers that, and getting it wrong would freeze a moving picture rather than
// merely cost frame time -- so it is pinned here, on the native build, where
// the cache itself is a deliberate no-op.

using namespace flix;

namespace {

const char* kStatic = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <rect width="100" height="100" fill="#3a7"/>
  <circle cx="50" cy="50" r="20" fill="#286"/>
</svg>)SVG";

const char* kAnimated = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <circle cx="50" cy="50" r="20" fill="#286">
    <animate attributeName="r" values="10;30;10" dur="2s" repeatCount="indefinite"/>
  </circle>
</svg>)SVG";

/// The timeline is on a nested node, which is where a check that only looked at
/// the root would miss it.
const char* kAnimatedChild = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <g transform="translate(10 10)">
    <g>
      <rect width="40" height="40" fill="#286">
        <animateTransform attributeName="transform" type="rotate"
                          values="0 20 20;360 20 20" dur="3s" repeatCount="indefinite"/>
      </rect>
    </g>
  </g>
</svg>)SVG";

} // namespace

TEST(svg_document_reports_whether_it_animates) {
    CHECK(!SvgDocument::fromString(kStatic).animated());
    CHECK(SvgDocument::fromString(kAnimated).animated());
    CHECK(SvgDocument::fromString(kAnimatedChild).animated());
}

TEST(hel_spider_legs_follow_distance_instead_of_clock_time) {
    const ContentRegistry& content = testsupport::shippedContent();
    const std::uint16_t mob = content.mobIndex("hel_spider");
    SpriteCache sprites;
    CHECK(sprites.build(content, testsupport::dataDir()));
    CHECK(sprites.mobDrawable(mob));
    const auto pixels = [&](double distance, double time) {
        Canvas canvas = Canvas::createVirtual(128, 128);
        MobMotion motion;
        motion.distance = distance;
        sprites.drawMob(canvas, mob, Rarity::Common, 64, 64, 112, 0, time,
                        false, 0, {1, 0}, &motion);
        return canvas.getImageData(0, 0, 128, 128);
    };

    const auto standing = pixels(0, 0);
    CHECK(standing == pixels(0, 2));
    CHECK(standing != pixels(20, 0));
}

TEST(imported_hel_and_mecha_sprites_face_the_in_game_heading) {
    const auto& content = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(content, testsupport::dataDir()));
    for (const char* name : {"hel_spider", "hel_wasp", "mecha_crab", "mecha_spider"}) {
        const auto id = content.mobIndex(name);
        CHECK(sprites.mobDrawable(id));
        if (!sprites.mobDrawable(id)) continue;
        const auto art = SvgDocument::fromString(content.mob(id).image);
        const auto sourcePixels = [&](double rotation) {
            Canvas canvas = Canvas::createVirtual(128, 128);
            canvas.translate(64.0f, 64.0f);
            canvas.rotate(static_cast<float>(rotation));
            art.renderFitted(canvas, -56.0f, -56.0f, 112.0f, 112.0f, 0.0f);
            return canvas.getImageData(0, 0, 128, 128);
        };
        for (const double heading : {0.0, kPi / 2.0}) {
            Canvas canvas = Canvas::createVirtual(128, 128);
            sprites.drawMob(canvas, id, Rarity::Common, 64, 64, 112, heading, 0.0);
            const auto actual = canvas.getImageData(0, 0, 128, 128);
            // Walking legs can differ from their SVG rest pose at phase zero.
            const auto aligned = sourcePixels(heading + 3.0 * kPi / 4.0);
            const auto unaligned = sourcePixels(heading);
            const int alignedDiff = testsupport::differingPixels(actual, aligned);
            const int unalignedDiff = testsupport::differingPixels(actual, unaligned);
            CHECK(alignedDiff >= 0);
            CHECK(alignedDiff * 2 < unalignedDiff);
        }
    }
}

TEST(silverfish_antennae_point_along_the_in_game_facing) {
    const auto& content = testsupport::shippedContent();
    const auto id = content.mobIndex("silverfish");
    CHECK(id != kInvalidIndex);
    if (id == kInvalidIndex) return;
    const auto art = SvgDocument::fromString(content.mob(id).image);
    SpriteCache sprites;
    CHECK(sprites.build(content, testsupport::dataDir()));
    CHECK(sprites.mobDrawable(id));
    for (const double bearing : {0.0, kPi / 2.0}) {
        Canvas actual = Canvas::createVirtual(128, 128);
        sprites.drawMob(actual, id, Rarity::Common, 64, 64, 112, bearing, 0.0);

        // The long antennae point upper-left in the source drawing.
        Canvas aligned = Canvas::createVirtual(128, 128);
        aligned.translate(64.0f, 64.0f);
        aligned.rotate(static_cast<float>(bearing + 3.0 * kPi / 4.0));
        art.renderFitted(aligned, -56.0f, -56.0f, 112.0f, 112.0f, 0.0f);
        CHECK(actual.getImageData(0, 0, 128, 128) == aligned.getImageData(0, 0, 128, 128));
    }
}

TEST(zorr_drawn_dinosaurs_face_the_in_game_heading) {
    // These are zorr's own drawings, which put the head at +x, the same way the
    // simulation's heading zero points, so they turn by the heading alone.
    const auto& content = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(content, testsupport::dataDir()));
    for (const char* name : {"jurassic_dodo", "jurassic_stegosaurus", "jurassic_raptor",
                             "jurassic_tyrannosaurus", "jurassic_ankylosaurus",
                             "jurassic_diplodocus", "jurassic_nigersaurus"}) {
        const auto id = content.mobIndex(name);
        CHECK(id != kInvalidIndex);
        if (id == kInvalidIndex) continue;
        CHECK(sprites.mobDrawable(id));
        const auto art = SvgDocument::fromString(content.mob(id).image);
        for (const double heading : {0.0, kPi / 2.0}) {
            Canvas actual = Canvas::createVirtual(128, 128);
            sprites.drawMob(actual, id, Rarity::Common, 64, 64, 112, heading, 0.0);
            Canvas turned = Canvas::createVirtual(128, 128);
            turned.translate(64.0f, 64.0f);
            turned.rotate(static_cast<float>(heading));
            art.renderFitted(turned, -56.0f, -56.0f, 112.0f, 112.0f, 0.0f);
            CHECK(actual.getImageData(0, 0, 128, 128) == turned.getImageData(0, 0, 128, 128));
        }
    }
}

TEST(silverfish_legs_move_with_distance_and_hold_when_still) {
    const auto& content = testsupport::shippedContent();
    const auto id = content.mobIndex("silverfish");
    SpriteCache sprites;
    CHECK(sprites.build(content, testsupport::dataDir()));
    CHECK(sprites.mobDrawable(id));
    const auto pixels = [&](double distance, double time) {
        Canvas canvas = Canvas::createVirtual(128, 128);
        MobMotion motion;
        motion.distance = distance;
        sprites.drawMob(canvas, id, Rarity::Common, 64, 64, 112, 0.0, time,
                        false, 0, {1, 0}, &motion);
        return canvas.getImageData(0, 0, 128, 128);
    };
    CHECK(pixels(0.0, 0.0) != pixels(20.0, 0.0));
    CHECK(pixels(20.0, 0.0) == pixels(20.0, 2.0));
}

namespace {

/// [x0, x1) by [y0, y1), in canvas pixels.
struct PixelBox {
    int x0, y0, x1, y1;
};

/// Mob `id` 112 px across on a 128 px canvas at heading zero, drawn as the
/// world renderer draws one that has walked `distance`, the clock at `time`.
std::vector<std::uint8_t> walkedPose(const SpriteCache& sprites, std::uint16_t id, double distance,
                                     double time) {
    Canvas canvas = Canvas::createVirtual(128, 128);
    MobMotion motion;
    motion.distance = distance;
    sprites.drawMob(canvas, id, Rarity::Common, 64, 64, 112, 0.0, time, false, 0, {1, 0}, &motion);
    return canvas.getImageData(0, 0, 128, 128);
}

int pixelsChangedIn(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                    const PixelBox& box) {
    int changed = 0;
    for (int y = box.y0; y < box.y1; ++y) {
        for (int x = box.x0; x < box.x1; ++x) {
            const std::size_t at = static_cast<std::size_t>(y * 128 + x) * 4;
            if (a[at] != b[at] || a[at + 1] != b[at + 1] || a[at + 2] != b[at + 2] ||
                a[at + 3] != b[at + 3]) {
                ++changed;
            }
        }
    }
    return changed;
}

/// Art recorded from zorr's walk cycles carries `data-motion="walk"` on its
/// root, which plays the timeline by distance walked: mob `name` keeps the
/// pose it stopped in however long it stands, and walking changes its picture
/// inside every box of `moving` and nowhere inside `still`.
void checkWalksWithDistance(const SpriteCache& sprites, const char* name, const PixelBox& still,
                            std::initializer_list<PixelBox> moving) {
    const ContentRegistry& content = testsupport::shippedContent();
    const auto id = content.mobIndex(name);
    CHECK(id != kInvalidIndex);
    if (id == kInvalidIndex) return;
    CHECK(sprites.mobDrawable(id));
    const auto standing = walkedPose(sprites, id, 0.0, 0.0);
    const auto walked = walkedPose(sprites, id, 20.0, 0.0);
    const bool holds = standing == walkedPose(sprites, id, 0.0, 2.0) &&
                       walked == walkedPose(sprites, id, 20.0, 2.0);
    if (!holds) std::printf("    %s moves while it stands\n", name);
    CHECK(holds);
    const int strayed = pixelsChangedIn(standing, walked, still);
    if (strayed != 0) std::printf("    %s: %d px change where nothing walks\n", name, strayed);
    CHECK(strayed == 0);
    int box = 0;
    for (const PixelBox& part : moving) {
        const int changed = pixelsChangedIn(standing, walked, part);
        if (changed == 0) std::printf("    %s: nothing moves in box %d\n", name, box);
        CHECK(changed > 0);
        ++box;
    }
}

} // namespace

TEST(arctic_spider_legs_walk_with_distance_round_a_body_that_holds_still) {
    SpriteCache sprites;
    CHECK(sprites.build(testsupport::shippedContent(), testsupport::dataDir()));
    checkWalksWithDistance(sprites, "arctic_spider", {56, 56, 72, 72},
                           {{8, 8, 120, 40}, {8, 88, 120, 120}});
    // The yoba's legs reach further forward than back, so the picture is
    // centred ahead of its body.
    checkWalksWithDistance(sprites, "arctic_spider_yoba", {46, 58, 60, 71},
                           {{8, 8, 120, 46}, {8, 80, 120, 120}});
}

TEST(digger_drills_turn_with_distance_behind_a_body_that_holds_still) {
    SpriteCache sprites;
    CHECK(sprites.build(testsupport::shippedContent(), testsupport::dataDir()));
    for (const char* name : {"arctic_frost_digger", "arctic_spider_digger", "void_alien_digger",
                             "void_grave_digger"}) {
        // The drill is the toothed ring round the body: it shows on all four sides.
        checkWalksWithDistance(sprites, name, {48, 48, 80, 80},
                               {{8, 48, 24, 80}, {104, 48, 120, 80},
                                {48, 8, 80, 24}, {48, 104, 80, 120}});
    }
}

TEST(arctic_tick_feelers_walk_with_distance_and_nothing_behind_them_moves) {
    SpriteCache sprites;
    CHECK(sprites.build(testsupport::shippedContent(), testsupport::dataDir()));
    checkWalksWithDistance(sprites, "arctic_tick", {8, 8, 92, 120}, {{92, 44, 120, 84}});
}

TEST(arctic_ice_hornet_sways_as_a_whole_with_distance) {
    // zorr's ice hornet has no walk cycle of its own; it sways about its
    // body's centre instead, which moves both of its ends.
    SpriteCache sprites;
    CHECK(sprites.build(testsupport::shippedContent(), testsupport::dataDir()));
    checkWalksWithDistance(sprites, "arctic_ice_hornet", {0, 0, 0, 0},
                           {{8, 32, 40, 96}, {88, 32, 120, 96}});
}

TEST(arctic_cave_spider_holds_one_pose) {
    const ContentRegistry& content = testsupport::shippedContent();
    SpriteCache sprites;
    CHECK(sprites.build(content, testsupport::dataDir()));
    const auto id = content.mobIndex("arctic_spider_cave");
    CHECK(id != kInvalidIndex);
    if (id == kInvalidIndex) return;
    CHECK(sprites.mobDrawable(id));
    const auto standing = walkedPose(sprites, id, 0.0, 0.0);
    CHECK(standing == walkedPose(sprites, id, 20.0, 0.0));
    CHECK(standing == walkedPose(sprites, id, 0.0, 2.0));
}

TEST(art_cache_is_a_no_op_off_the_web) {
#ifndef __EMSCRIPTEN__
    // Caching these tiles was measured four times SLOWER against the software
    // rasterizer, so the native build must keep drawing the artwork. The
    // contract is that drawCachedArt draws nothing and says so, leaving the
    // caller's renderFitted fallback as the only thing that paints.
    const SvgDocument art = SvgDocument::fromString(kStatic);
    Canvas canvas = Canvas::createVirtual(64, 64);
    CHECK(!drawCachedArt(canvas, art, 0.0, 0.0, 64.0, 64.0));

    std::size_t entries = 1, bytes = 1;
    artCacheStats(entries, bytes);
    CHECK(entries == 0);
    CHECK(bytes == 0);
#endif
}

using flix::testsupport::shippedContent;

TEST(icosahedron_art_animates_in_the_actual_petal_renderer) {
    const ContentRegistry& content = shippedContent();
    const auto id = content.petalIndex("icosahedron");
    CHECK(id != kInvalidIndex);
    if (id == kInvalidIndex) return;
    const auto art = SvgDocument::fromString(content.petal(id).image);
    CHECK(!art.empty());
    CHECK(art.animated());
    SpriteCache sprites;
    sprites.build(content, testsupport::dataDir());
    const auto frame = [&](double time) {
        Canvas canvas = Canvas::createVirtual(64, 64);
        sprites.drawPetal(canvas, id, Rarity::Common, 32, 32, 64, 0, time);
        return canvas.getImageData(0, 0, 64, 64);
    };
    CHECK(frame(0.0) != frame(0.75));
}

TEST(every_procedural_marker_in_the_shipped_mobs_compiles) {
    // The failure this pins is SILENT. An `image` naming a painter this build
    // does not have -- a typo, or a painter added to mobs.json and not to
    // mobArtFor -- is not a parse error and not a warning anybody reads: the
    // marker falls through to the document path, fails to parse as SVG, and
    // the mob draws as a flat rarity disc in the world, in the bestiary and on
    // its own tooltip.
    //
    // Asked of the '$' entries only. The garbage pile ships a literally empty
    // <svg/> and is MEANT to draw nothing from it (the renderer paints the
    // pile itself), so "declares an image" is not the same question as
    // "should draw something".
    const ContentRegistry& content = shippedContent();
    CHECK(content.loaded());
    SpriteCache sprites;
    sprites.build(content, testsupport::dataDir());

    int markers = 0;
    for (std::uint16_t i = 0; i < content.mobCount(); ++i) {
        const MobConfig& config = content.mob(i);
        if (config.image.empty() || config.image[0] != '$') continue;
        ++markers;
        if (!sprites.mobDrawable(i)) {
            std::printf("    mob '%s': image \"%s\" names nothing this build can draw\n",
                        config.id.c_str(), config.image.c_str());
        }
        CHECK(sprites.mobDrawable(i));
    }
    // The count is not the point, but a lookup that quietly found no markers
    // at all would pass this test for the wrong reason.
    CHECK(markers >= 7);
}

TEST(the_procedural_markers_name_painters_this_build_has) {
    // `$sponge:` is the odd one out: a palette marker expanded into a document
    // rather than a painter name, which is why this asks mobArtFor about the
    // painters by name instead of about every '$' string in the file.
    CHECK(mobArtFor("$rock") == MobArt::Rock);
    CHECK(mobArtFor("$cactus") == MobArt::Cactus);
    CHECK(mobArtFor("$sandstorm") == MobArt::Sandstorm);
    CHECK(mobArtFor("$scorpion") == MobArt::Scorpion);
    CHECK(mobArtFor("$crab") == MobArt::Crab);
    CHECK(mobArtFor("$leech") == MobArt::LeechHead);
    CHECK(mobArtFor("$leech_body") == MobArt::LeechBody);
    CHECK(mobArtFor("$spider") == MobArt::Spider);
    CHECK(mobArtFor("$nothing_by_that_name") == MobArt::None);
    CHECK(mobArtFor("<svg/>") == MobArt::None);
    CHECK(mobArtFor("") == MobArt::None);
}

TEST(a_super_square_is_repainted_green_with_a_derived_outline) {
    // petals.json repaints the square from super up (`rarityFills`): one fill
    // colour named, the outline worked out from it by the rule every gardn
    // body's outline follows. Below super it is the yellow it always was.
    const ContentRegistry& content = shippedContent();
    CHECK(content.loaded());
    const std::uint16_t square = content.petalIndex("square");
    CHECK(square != kInvalidIndex);
    SpriteCache sprites;
    sprites.build(content, testsupport::dataDir());

    // At 64 px the 32-unit document is doubled: the stroke runs down x = 4,
    // 6 px wide, and the middle of the box is all fill.
    const auto pixelsAt = [&](Rarity rarity) {
        Canvas canvas = Canvas::createVirtual(64, 64);
        sprites.drawPetal(canvas, square, rarity, 32, 32, 64, 0, 0);
        const std::vector<std::uint8_t> image = canvas.getImageData(0, 0, 64, 64);
        const auto rgb = [&](int x, int y) {
            const std::size_t at = (static_cast<std::size_t>(y) * 64 + x) * 4;
            return (std::uint32_t{image[at]} << 16) | (std::uint32_t{image[at + 1]} << 8) |
                   image[at + 2];
        };
        return std::pair<std::uint32_t, std::uint32_t>{rgb(32, 32), rgb(4, 32)};
    };

    const auto ultra = pixelsAt(Rarity::Ultra);
    CHECK(ultra.first == 0xFFE869u);
    CHECK(ultra.second == 0xCFBC55u);
    for (const Rarity tier : {Rarity::Super, Rarity::Unique, Rarity::Apex}) {
        const auto repainted = pixelsAt(tier);
        CHECK(repainted.first == 0x8AFF69u);
        CHECK(repainted.second == outlineOf(0x8AFF69u));
    }
    CHECK(outlineOf(0x8AFF69u) == 0x6ECC54u);
}

TEST(the_square_mob_is_drawn_as_the_square_petal_is_at_every_tier) {
    // mobs.json draws the square mob as the square petal (`$petal:square`): the
    // petal's own sprite at the mob's tier, so the same box drawn either way is
    // the same pixels -- yellow below super, green from there up, in the world
    // and on a bestiary tile alike.
    const ContentRegistry& content = shippedContent();
    const std::uint16_t mob = content.mobIndex("square");
    const std::uint16_t petal = content.petalIndex("square");
    CHECK(mob != kInvalidIndex);
    CHECK(petal != kInvalidIndex);
    if (mob == kInvalidIndex || petal == kInvalidIndex) return;
    SpriteCache sprites;
    sprites.build(content, testsupport::dataDir());

    const auto pixels = [](const std::function<void(Canvas&)>& paint) {
        Canvas canvas = Canvas::createVirtual(64, 64);
        paint(canvas);
        return canvas.getImageData(0, 0, 64, 64);
    };
    for (const Rarity tier : {Rarity::Common, Rarity::Ultra, Rarity::Super, Rarity::Apex}) {
        const std::vector<std::uint8_t> asPetal =
            pixels([&](Canvas& c) { sprites.drawPetal(c, petal, tier, 32, 32, 64, 0, 0); });
        const std::vector<std::uint8_t> asMob =
            pixels([&](Canvas& c) { sprites.drawMob(c, mob, tier, 32, 32, 64, 0, 0); });
        CHECK(asMob == asPetal);
        const std::size_t middle = (32u * 64u + 32u) * 4u;
        const std::uint32_t fill = (std::uint32_t{asMob[middle]} << 16) |
                                   (std::uint32_t{asMob[middle + 1]} << 8) | asMob[middle + 2];
        CHECK(fill == (rarityIndex(tier) >= rarityIndex(Rarity::Super) ? 0x8AFF69u : 0xFFE869u));
    }

    // The bestiary's picture wears the tier of its cell.
    const auto tile = [&](Rarity tier) {
        const std::vector<std::uint8_t> image = pixels(
            [&](Canvas& c) { sprites.drawMobPicture(c, mob, tier, 32, 32, 64, 48, 0); });
        const std::size_t middle = (32u * 64u + 32u) * 4u;
        return (std::uint32_t{image[middle]} << 16) | (std::uint32_t{image[middle + 1]} << 8) |
               image[middle + 2];
    };
    CHECK(tile(Rarity::Mythic) == 0xFFE869u);
    CHECK(tile(Rarity::Unique) == 0x8AFF69u);
}

TEST(a_mob_drawn_as_a_petal_that_does_not_exist_is_a_plain_disc) {
    ContentRegistry content;
    std::string error;
    CHECK(test::loadFixtureContent(content, testsupport::tempDir("flix_art_cache_petal_art"), R"({
        "lost": {"name":"Lost","health":1,"damage":1,"size":1,"image":"$petal:nope"},
        "found": {"name":"Found","health":1,"damage":1,"size":1,"image":"$petal:basic"}
    })",
                                   R"({
        "basic": {"name":"Basic","damage":5,"health":5,"size":1,"cooldown":1000,"count":1,
                  "image":"<svg viewBox=\"0 0 2 2\"><rect width=\"2\" height=\"2\" fill=\"#fff\"/></svg>"}
    })",
                                   error));
    SpriteCache sprites;
    sprites.build(content, testsupport::dataDir());
    CHECK(!sprites.mobDrawable(content.mobIndex("lost")));
    CHECK(sprites.mobDrawable(content.mobIndex("found")));
    bool warned = false;
    for (const std::string& w : sprites.warnings()) {
        warned = warned || w.find("mob lost: draws petal 'nope'") != std::string::npos;
    }
    CHECK(warned);
}

namespace {

/// Width and height of a PNG from the first 32 characters of its base64: the
/// IHDR chunk every PNG opens with holds them at bytes 16..23, big-endian.
bool pngSize(const std::string& base64, std::uint32_t& width, std::uint32_t& height) {
    static const std::string digits =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (base64.size() < 32) return false;
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < 32; i += 4) {
        std::uint32_t quad = 0;
        for (std::size_t j = 0; j < 4; ++j) {
            const std::size_t at = digits.find(base64[i + j]);
            if (at == std::string::npos) return false;
            quad = quad << 6 | static_cast<std::uint32_t>(at);
        }
        bytes.push_back(static_cast<std::uint8_t>(quad >> 16));
        bytes.push_back(static_cast<std::uint8_t>(quad >> 8));
        bytes.push_back(static_cast<std::uint8_t>(quad));
    }
    const auto word = [&](std::size_t at) {
        return std::uint32_t{bytes[at]} << 24 | std::uint32_t{bytes[at + 1]} << 16 |
               std::uint32_t{bytes[at + 2]} << 8 | bytes[at + 3];
    };
    width = word(16);
    height = word(20);
    return true;
}

bool endsWith(const std::string& text, const std::string& tail) {
    return text.size() >= tail.size() &&
           text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

TEST(egg_petals_are_vector_ellipses_and_no_two_share_their_colours) {
    // An egg is zorr's egg drawing -- a fill, and a band of outline just inside
    // its edge -- written as svg ellipses: sharp at any size, a few hundred
    // bytes. Players tell eggs apart by those two colours, and zorr paints
    // three pairs alike (alien/angel, igloo/snowman, ghost/white hole: #2223
    // #1293 #2232), so no two eggs may share both.
    const ContentRegistry& content = shippedContent();
    CHECK(content.loaded());
    SpriteCache sprites;
    sprites.build(content, testsupport::dataDir());
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> looks;
    int eggs = 0;
    for (std::uint16_t i = 0; i < content.petalCount(); ++i) {
        const PetalConfig& petal = content.petal(i);
        if (!endsWith(petal.id, "egg")) continue;
        ++eggs;
        CHECK(petal.image.find("<image") == std::string::npos);
        Canvas canvas = Canvas::createVirtual(128, 128);
        sprites.drawPetal(canvas, i, Rarity::Common, 64, 64, 128, 0, 0);
        const std::vector<std::uint8_t> image = canvas.getImageData(0, 0, 128, 128);
        const auto at = [](int x) { return (64u * 128u + static_cast<std::size_t>(x)) * 4u; };
        const auto rgb = [&](int x) {
            return std::uint32_t{image[at(x)]} << 16 | std::uint32_t{image[at(x) + 1]} << 8 |
                   image[at(x) + 2];
        };
        int edge = 0;
        while (edge < 64 && image[at(edge) + 3] == 0) ++edge;
        CHECK(edge < 64);
        // 3 px in from the left edge is inside the outline band, which is at
        // least 5 px wide on a 128 px egg.
        const auto look = std::make_pair(rgb(64), rgb(edge + 3));
        const auto [same, fresh] = looks.emplace(look, petal.id);
        if (!fresh) {
            std::printf("    %s has the colours of %s\n", petal.id.c_str(), same->second.c_str());
        }
        CHECK(fresh);
    }
    CHECK(eggs >= 61);
}

TEST(petal_pictures_stay_within_the_download_budget) {
    // petals.json is the largest file a player downloads before the first
    // frame, over a 20 Mbit/s server link everyone joining shares: it may not
    // grow past the 6,860,279 bytes it had before the petal art passes, and no
    // picture embeds a bitmap over 256 px a side (2 px per frame unit, sharper
    // than any petal is drawn).
    std::string text;
    CHECK(testsupport::readText(testsupport::dataFile("petals.json"), text));
    CHECK(text.size() <= 6860279u);
    const ContentRegistry& content = shippedContent();
    int bitmaps = 0;
    for (std::uint16_t i = 0; i < content.petalCount(); ++i) {
        const std::string& image = content.petal(i).image;
        for (std::size_t at = image.find("base64,"); at != std::string::npos;
             at = image.find("base64,", at + 7)) {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            CHECK(pngSize(image.substr(at + 7, 32), width, height));
            ++bitmaps;
            if (width > 256 || height > 256) {
                std::printf("    %s embeds a %ux%u bitmap\n", content.petal(i).id.c_str(), width, height);
            }
            CHECK(width <= 256 && height <= 256);
        }
    }
    CHECK(bitmaps > 0);
}

TEST(mob_pictures_stay_within_the_download_budget) {
    // mobs.json is downloaded before the first frame too, over the same shared
    // link: the walk cycles may not take it past 4 MB.
    std::string text;
    CHECK(testsupport::readText(testsupport::dataFile("mobs.json"), text));
    CHECK(text.size() <= 4u * 1024u * 1024u);
}
