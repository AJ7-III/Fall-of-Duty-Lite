#include "briar_house.hpp"

#include "assets.hpp"
#include "game_world.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_map>
#include <utility>

namespace fod {
namespace {

struct Rect { float left, top, right, bottom; };
struct Wall { float x, y, endX, endY; };
enum class Side { None, West, North, East, South };

constexpr float scale = .05f, wallHeight = 3.35f, hallHeight = 6.35f, halfWall = 2.5f;
constexpr Rect site{-64, -89, 692, 514};
constexpr Rect house{52, 28, 680, 392};
constexpr Rect westWing{52, 92, 222, 332}, grandHall{270, 28, 466, 392}, eastWing{518, 92, 680, 332};
constexpr Rect garage{572, 92, 680, 392}, stairFlight{431, 174, 463, 229};
constexpr Wall garageBay{583, 392, 667, 392};
constexpr float frontDoorLeft = 308, frontDoorRight = 428, rearDoorLeft = 308, rearDoorRight = 428;

Vec3 position(float x, float y, float height = 0) {
    return {(x - 316) * scale, height, (212.5f - y) * scale};
}

constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

enum Surface {
    Grass, Pavers, Oak, Tile, Concrete, Plaster, Stucco, Trim, Roof, Hedge,
    Timber, Counter, Steel, Upholstery, Linen, WagonPaint, Rug, Brick, Glass, Ceiling, GardenWall,
    Ivory, Sage, Brass, Amber, DarkWood, Terracotta, Soil, YoungLeaf, Flower, Artwork,
    Marble, RedRunner, StainedGlass, MonumentStone, Asphalt, Tire, CarGlass,
    Meadow, ForestSlope, MountainRock, FarRidge, CabinCedar, CabinGreen, CabinCream, CabinGlass,
    Iron, Gravel, CabinSlate, RidgePine,
    AlienHull, AlienTrim, AlienGlow, AlienCore, Scorch,
    AlienSkin, AlienSinew, AlienTooth, AlienMaw, AlienTongue,
    CivSkin, CivHair, CivTop, CivPants, CivShoes,
    HeloOlive, HeloDark, HeloMark, HeloCross, Ember, SurfaceCount
};

constexpr std::array<uint16_t, SurfaceCount> colors{
    rgb(91, 113, 64), rgb(146, 141, 130), rgb(141, 103, 69), rgb(150, 156, 146), rgb(141, 141, 130),
    rgb(223, 213, 189), rgb(185, 173, 145), rgb(78, 68, 55), rgb(74, 84, 85), rgb(65, 94, 46),
    rgb(125, 88, 57), rgb(202, 194, 176), rgb(135, 147, 146), rgb(78, 105, 91), rgb(230, 221, 198),
    rgb(61, 96, 85), rgb(142, 77, 64), rgb(138, 91, 71), rgb(143, 198, 217),
    rgb(213, 209, 190), rgb(134, 132, 114), rgb(222, 218, 198), rgb(109, 130, 103), rgb(173, 136, 73),
    rgb(255, 225, 156), rgb(82, 52, 37), rgb(162, 94, 64), rgb(78, 61, 44), rgb(107, 134, 63),
    rgb(201, 169, 91), rgb(186, 177, 142), rgb(211, 207, 187), rgb(137, 53, 54), rgb(89, 122, 141),
    rgb(213, 204, 180), rgb(81, 85, 78), rgb(45, 44, 43), rgb(89, 121, 130),
    rgb(108, 122, 77), rgb(68, 91, 66), rgb(121, 126, 124), rgb(115, 130, 139),
    rgb(154, 98, 70), rgb(117, 137, 109), rgb(197, 177, 139), rgb(125, 168, 181),
    rgb(49, 56, 54), rgb(152, 143, 123), rgb(83, 100, 105), rgb(50, 72, 59),
    rgb(42, 49, 64), rgb(74, 86, 108), rgb(72, 245, 255), rgb(255, 88, 104), rgb(34, 32, 31),
    rgb(86, 50, 124), rgb(46, 28, 76), rgb(236, 230, 212), rgb(72, 14, 30), rgb(204, 88, 102),
    rgb(214, 170, 136), rgb(70, 50, 38), rgb(52, 86, 138), rgb(70, 74, 82), rgb(44, 42, 42),
    rgb(78, 86, 64), rgb(38, 42, 38), rgb(228, 228, 220), rgb(182, 34, 40), rgb(255, 128, 44),
};

unsigned grain(int x, int y) {
    uint32_t hash = uint32_t(x) * 0x8da6b343u + uint32_t(y) * 0xd8163841u + 0xa36b7a13u;
    hash = (hash ^ (hash >> 13)) * 0x85ebca6bu;
    return (hash ^ (hash >> 16)) & 15u;
}

uint16_t masonry(int x, int y, unsigned r, unsigned g, unsigned b) {
    const int row = y / 8, course = (x + row % 2 * 8) % 64;
    if (y % 8 == 0 || course % 16 == 0) return rgb(r - 23, g - 22, b - 19);
    const unsigned value = grain(course / 16, row) + grain(x, y) / 2;
    const unsigned edge = y % 8 == 1 ? 7 : 0;
    return rgb(r + value + edge, g + value + edge, b + value + edge);
}

constexpr Wall exterior[] {
    {52, 28, 52, 155}, {52, 192, 52, 273}, {52, 310, 52, 392},
    {52, 392, 103, 392}, {147, 392, 195, 392}, {239, 392, 270, 392},
    {466, 392, 496, 392}, {540, 392, garageBay.x, 392}, {garageBay.endX, 392, 680, 392},
    {680, 392, 680, 300}, {680, 258, 680, 183}, {680, 141, 680, 28},
    {680, 28, 633, 28}, {589, 28, 540, 28}, {496, 28, 466, 28},
    {270, 28, 239, 28}, {195, 28, 147, 28}, {103, 28, 52, 28},
};
constexpr Wall windows[] {
    {52, 155, 52, 192}, {52, 273, 52, 310},
    {103, 392, 147, 392}, {195, 392, 239, 392}, {496, 392, 540, 392},
    {680, 300, 680, 258}, {680, 183, 680, 141},
    {633, 28, 589, 28}, {540, 28, 496, 28}, {239, 28, 195, 28}, {147, 28, 103, 28},
};
constexpr Rect shed{-64, 194, -6, 258};
constexpr Rect rearHedge{380, -57, 484, -43}, frontHedge{244, 442, 365, 456};

Rect wallRect(Wall segment) {
    const bool vertical = segment.x == segment.endX;
    const float xPad = vertical ? halfWall : 0;
    const float yPad = vertical ? 0 : halfWall;
    return {std::min(segment.x, segment.endX) - xPad, std::min(segment.y, segment.endY) - yPad,
            std::max(segment.x, segment.endX) + xPad, std::max(segment.y, segment.endY) + yPad};
}

Side insideFace(Wall segment) {
    if (segment.x == segment.endX) return segment.y < segment.endY ? Side::East : Side::West;
    return segment.x < segment.endX ? Side::North : Side::South;
}

struct VertexHash {
    size_t operator()(const std::array<float, 5>& fields) const {
        size_t hash = 0;
        for (const float field : fields) hash = hash * 33u ^ std::hash<float>{}(field);
        return hash;
    }
};

template<typename V, typename Key>
std::vector<uint32_t> weld(std::vector<V>& vertices, Key key) {
    std::unordered_map<std::array<float, 5>, uint32_t, VertexHash> known;
    known.reserve(vertices.size());
    std::vector<uint32_t> remap;
    remap.reserve(vertices.size());
    uint32_t used = 0;
    for (size_t i = 0; i < vertices.size(); ++i) {
        const auto [at, inserted] = known.try_emplace(key(vertices[i]), used);
        if (inserted) vertices[used++] = vertices[i];
        remap.push_back(at->second);
    }
    vertices.resize(used);
    vertices.shrink_to_fit();
    return remap;
}

class Builder {
public:
    explicit Builder(Assets& data) : assets(data), firstMaterial(uint16_t(data.materials.size())) {
        repeats.fill(.5f);
        for (size_t i = 0; i < colors.size(); ++i) {
            const bool twoSided = i == Glass || i == Ceiling || i == AlienTooth || i == AlienMaw;
            const bool glow = i == Amber || i == AlienGlow || i == AlienCore || i == Ember;
            assets.materials.push_back({colors[i], -1, uint8_t(i == Glass ? 82 : 255), twoSided, glow});
        }
        texture(Tile, 32, .5f, [](int x, int y) {
            if (x % 8 == 0 || y % 8 == 0) return rgb(118, 120, 111);
            const auto surface = grain(x, y) / 3;
            return (x / 8 + y / 8) % 2 ? rgb(181 + surface, 182 + surface, 164 + surface) :
                                       rgb(132 + surface, 149 + surface, 129 + surface);
        });
        texture(Oak, 32, .5f, [](int x, int y) {
            if (y % 5 == 0 || (x + y / 5 * 11) % 32 == 0) return rgb(103, 74, 51);
            const auto value = unsigned((y / 5 * 3 + x * 7 % 5) % 14);
            return rgb(142 + value, 102 + value, 69 + value / 2);
        });
        texture(Grass, 32, .36f, [](int x, int y) {
            const auto value = grain(x / 3, y / 3) + grain(x, y) / 2;
            return rgb(79 + value, 99 + value, 52 + value / 2);
        });
        texture(Pavers, 32, .5f, [](int x, int y) {
            if (y % 8 == 0 || (x + y / 8 * 8) % 16 == 0) return rgb(119, 115, 108);
            const auto value = unsigned((x / 8 + y / 8 * 3) % 11);
            return rgb(148 + value, 143 + value, 133 + value);
        });
        texture(Stucco, 64, .36f, [](int x, int y) { return masonry(x, y, 177, 162, 131); });
        texture(GardenWall, 64, .34f, [](int x, int y) { return masonry(x, y, 125, 124, 105); });
        texture(Brick, 64, .65f, [](int x, int y) { return masonry(x, y, 128, 86, 67); });
        texture(Roof, 64, .46f, [](int x, int y) {
            const int course = y / 8, tile = (x + course % 2 * 4) % 64;
            if (y % 8 == 0 || tile % 8 == 0) return rgb(53, 60, 64);
            const auto value = grain(tile / 8, course) + grain(x, y) / 3;
            return rgb(67 + value, 77 + value, 81 + value);
        });
        texture(Hedge, 32, .85f, [](int x, int y) {
            const auto value = grain(x / 2, y / 2) * 2 + grain(x, y) / 3;
            return rgb(47 + value, 72 + value, 33 + value / 2);
        });
        texture(Timber, 32, .74f, [](int x, int y) {
            const auto value = grain(x + y / 8, 7) + grain(x, y / 3) / 4;
            return rgb(101 + value, 69 + value, 47 + value / 2);
        });
        texture(Concrete, 32, .51f, [](int x, int y) {
            const auto grit = grain(x, y) + grain(x / 5, y / 5) / 2;
            if (x == 0 || y == 0) return rgb(108, 109, 104);
            return rgb(133 + grit, 134 + grit, 125 + grit);
        });
        texture(Asphalt, 32, .61f, [](int x, int y) {
            const auto grit = grain(x, y) + grain(x / 3, y / 3) / 3;
            return rgb(76 + grit, 79 + grit, 73 + grit);
        });
        texture(Meadow, 32, .13f, [](int x, int y) {
            const auto variation = grain(x / 4, y / 4) + grain(x / 2, y / 2) / 2;
            return rgb(93 + variation, 110 + variation, 67 + variation / 2);
        });
        texture(MountainRock, 32, .16f, [](int x, int y) {
            const auto stone = grain(x / 4, (y + x / 2) / 3) + grain(x, y) / 2;
            return rgb(107 + stone, 113 + stone, 109 + stone);
        });
        texture(Gravel, 32, .54f, [](int x, int y) {
            const auto pebble = grain(x / 2, y / 2) * 2 + grain(x, y) / 2;
            return rgb(125 + pebble, 120 + pebble, 107 + pebble);
        });
        texture(CabinGlass, 32, .59f, [](int x, int y) {
            const unsigned reflection = (y < 18 + (x / 5 % 4) ? 10 : 0) + grain(x / 3, y / 4) / 3;
            return rgb(111 + reflection, 153 + reflection, 166 + reflection);
        });
        for (const auto siding : {CabinCedar, CabinGreen, CabinCream, CabinSlate}) {
            texture(siding, 32, .56f, [siding](int x, int y) {
                const auto color = colors[siding];
                const int light = int(grain(x / 2, y)) / 2 - (y % 6 == 0 ? 24 : 3);
                return rgb((color >> 11) * 8 + light, ((color >> 5) & 63) * 4 + light, (color & 31) * 8 + light);
            });
        }
        texture(Rug, 64, .5f, [](int x, int y) {
            const int edge = std::min({x, y, 63 - x, 63 - y});
            const int diamond = std::abs(2 * x - 63) + std::abs(2 * y - 63);
            if (edge <= 2 || edge == 10 || diamond == 34 || diamond == 36) return rgb(198, 155, 101);
            if (edge < 10) return (x + y) % 6 < 2 ? rgb(166, 120, 84) : rgb(77, 92, 82);
            if (diamond < 34) return diamond > 26 ? rgb(164, 116, 77) : rgb(94, 111, 99);
            return (x + y) % 9 == 0 ? rgb(165, 110, 83) : rgb(138, 70, 55);
        });
        texture(Artwork, 64, .5f, [](int x, int y) {
            const int dx = x - 31, dy = y - 26;
            const auto fleck = grain(x, y) / 3;
            if (4 * dx * dx + 2 * dy * dy < 460) {
                if (y < 21 || (dx < -6 && y < 32)) return rgb(76 + fleck, 59 + fleck, 43 + fleck);
                if ((x == 28 || x == 36) && y == 26) return rgb(87, 70, 51);
                return rgb(172 + fleck + unsigned(dx + 12) * 2, 126 + fleck + unsigned(dx + 12), 92 + fleck);
            }
            if (y > 36 && std::abs(dx) < 4 + (y - 36) * 2) {
                if (std::abs(dx) < std::max(1, 8 - (y - 38) / 2) && y < 54) return rgb(192 + fleck, 181 + fleck, 155);
                return rgb(56 + fleck, 72 + fleck, 67 + fleck);
            }
            return rgb(107 + fleck, 100 + fleck, 72 + fleck);
        });
        texture(Marble, 64, .44f, [](int x, int y) {
            const int seam = std::min(x % 16, y % 16);
            const bool pale = (x / 16 + y / 16) % 2 == 0;
            const auto grainValue = grain(x / 3, y / 3) / 2;
            const bool vein = ((x + y * 3 + (y / 5) * 3) % 22) < 2;
            if (seam == 0) return rgb(107, 103, 92);
            if (pale) return rgb(205 + grainValue - 23 * vein, 195 + grainValue - 19 * vein, 171 + grainValue - 14 * vein);
            return rgb(57 + grainValue + 18 * vein, 64 + grainValue + 16 * vein, 66 + grainValue + 12 * vein);
        });
        texture(RedRunner, 64, .53f, [](int x, int y) {
            const int border = std::min(x, 63 - x);
            if (border < 3 || border == 10) return rgb(189, 145, 82);
            if (border < 10) return (x + y) % 8 < 3 ? rgb(173, 111, 73) : rgb(73, 48, 52);
            const auto thread = grain(x, y) / 3;
            return rgb(138 + thread, 52 + thread, 52 + thread);
        });
        texture(MonumentStone, 32, .62f, [](int x, int y) {
            const auto fleck = grain(x / 2, y / 3) / 2;
            const bool vein = ((x + y * 2 + (y / 4) * 5) % 21) < 2;
            return rgb(212 + fleck - 20 * vein, 200 + fleck - 17 * vein, 176 + fleck - 14 * vein);
        });
    }

    void shiftX(float x) { xOffset = x; }

    void floor(Rect r, Surface material, float level = 0) {
        quad({position(r.left, r.bottom, level), position(r.left, r.top, level),
              position(r.right, r.top, level), position(r.right, r.bottom, level)}, material, -1, material == Rug);
    }

    void box(Rect r, float bottom, float top, Surface material, Side plastered = Side::None) {
        cuboid(corners(r, bottom), corners(r, top), material, plastered);
    }

    void box(const std::array<Vec3, 4>& lower, const std::array<Vec3, 4>& upper, Surface material) {
        cuboid(lower, upper, material, Side::None);
    }

    void taperedBox(Rect lower, Rect upper, float bottom, float top, Surface material) {
        cuboid(corners(lower, bottom), corners(upper, top), material, Side::None);
    }

    void fallenBox(Rect r, float bottom, float top, Surface material) {
        constexpr float sine = .57358f, cosine = .81915f;
        const float pivot = position(0, stairFlight.bottom).z;
        const auto tilt = [&](std::array<Vec3, 4> points) {
            for (auto& p : points) {
                const float z = p.z - pivot;
                p.z = pivot + z * cosine + p.y * sine;
                p.y = p.y * cosine - z * sine;
            }
            return points;
        };
        cuboid(tilt(corners(r, bottom)), tilt(corners(r, top)), material, Side::None);
    }

    void axle(float left, float right, float y, float height, float radius, Surface material) {
        constexpr int sides = 10;
        const auto a = position(left, y, height), b = position(right, y, height);
        for (int i = 0; i < sides; ++i) {
            const float angle = (2 * pi * i) / sides, next = (2 * pi * (i + 1)) / sides;
            const Vec3 p{0, std::cos(angle) * radius, std::sin(angle) * radius};
            const Vec3 q{0, std::cos(next) * radius, std::sin(next) * radius};
            quad({a + p, a + q, b + q, b + p}, material);
            face({a, a + q, a + p}, material);
            face({b, b + p, b + q}, material);
        }
    }

    void collision(Rect r, float bottom, float top, bool blocksSight = true) {
        const auto center = position((r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
        assets.obstacles.push_back({center.x, center.z, (r.right - r.left) * scale * .5f,
                                    (r.bottom - r.top) * scale * .5f, bottom, top, 0, 1, 0, blocksSight});
    }

    void solid(Rect r, float bottom, float top, Surface material) {
        box(r, bottom, top, material);
        collision(r, bottom, top);
    }

    void prop(Rect r, float height, Surface material) { solid(r, 0, height, material); }

    void wall(Wall section, Surface material, float top = wallHeight, float bottom = 0) {
        const auto r = wallRect(section);
        box(r, bottom, top, material, material == Stucco ? insideFace(section) : Side::None);
        collision(r, bottom, top);
        constexpr float lip = .25f;
        if (bottom == 0) box({r.left - lip, r.top - lip, r.right + lip, r.bottom + lip}, .05f, .17f, Trim);
    }

    void window(Wall section) {
        constexpr float sill = 1.02f, lintel = 2.18f;
        const auto r = wallRect(section);
        const auto inside = insideFace(section);
        box(r, 0, sill, Stucco, inside);
        box(r, lintel, wallHeight, Stucco, inside);
        collision(r, 0, sill);
        collision(r, lintel, wallHeight);
        box(r, sill - .06f, sill, Trim);
        box(r, lintel, lintel + .06f, Trim);
        const auto a = position(section.x, section.y, sill);
        const auto b = position(section.endX, section.endY, sill);
        const Vec3 up{0, lintel - sill, 0};
        const auto id = int16_t(assets.glassHits.size());
        assets.glassHits.push_back(1);
        quad({a, b, b + up, a + up}, Glass, id);
        // The fixed sill and frame keep the opening out of the movement routes.
        // Visibility passes through the pane, and player bullets use its shot triangles.
        collision(r, sill, lintel, false);
    }

    void gabledRoof(Rect r, float eaves, float peak) {
        const float center = (r.top + r.bottom) * .5f;
        const auto southwest = position(r.left, r.bottom, eaves), southeast = position(r.right, r.bottom, eaves);
        const auto northwest = position(r.left, r.top, eaves), northeast = position(r.right, r.top, eaves);
        const auto westRidge = position(r.left, center, peak), eastRidge = position(r.right, center, peak);
        quad({southwest, westRidge, eastRidge, southeast}, Roof);
        quad({westRidge, northwest, northeast, eastRidge}, Roof);
        face({southwest, northwest, westRidge}, Stucco);
        face({northeast, southeast, eastRidge}, Stucco);
    }

    void lamp(float x, float y, float height, float radius, float strength) {
        assets.lamps.push_back({position(x, y, height), radius, strength});
    }

    void round(float x, float y, float bottom, float top, float base, float rim, Surface material) {
        constexpr int sides = 6;
        const Vec3 low = position(x, y, bottom), high = position(x, y, top);
        for (int i = 0; i < sides; ++i) {
            const float angle = 2 * pi * i / sides, next = 2 * pi * (i + 1) / sides;
            const Vec3 a{std::cos(angle) * scale, 0, std::sin(angle) * scale};
            const Vec3 b{std::cos(next) * scale, 0, std::sin(next) * scale};
            if (rim == 0) face({low + b * base, low + a * base, high}, material);
            else quad({low + b * base, low + a * base, high + a * rim, high + b * rim}, material);
            if (base > 0) face({low, low + a * base, low + b * base}, material);
            if (rim > 0) face({high, high + b * rim, high + a * rim}, material);
        }
    }

    void pictureFacingWest(float x, float start, float end, float bottom, float top) {
        box({x - .25f, start - 1.8f, x + .3f, end + 1.8f}, bottom - .08f, top + .08f, DarkWood);
        box({x - .34f, start - .7f, x - .2f, end + .7f}, bottom - .035f, top + .035f, Brass);
        quad({position(x - .39f, start, bottom), position(x - .39f, start, top),
              position(x - .39f, end, top), position(x - .39f, end, bottom)}, Artwork, -1, true);
    }

    void porticoGable() {
        const Vec3 left = position(282, 425, 4.12f), right = position(454, 425, 4.12f);
        const Vec3 ridge = position(368, 425, 5.62f), backRidge = position(368, 389, 5.62f);
        quad({left, position(282, 389, 4.12f), backRidge, ridge}, Roof);
        quad({ridge, backRidge, position(454, 389, 4.12f), right}, Roof);
        face({left, ridge, right}, Stucco);
    }

    void archPanel(float x, float y, float bottom, float spring, float radius, Surface material) {
        quad({position(x - radius, y, bottom), position(x - radius, y, spring),
              position(x + radius, y, spring), position(x + radius, y, bottom)}, material);
        constexpr int segments = 8;
        const auto rim = [&](int i) {
            const float angle = pi * (1 - i / float(segments));
            return position(x + std::cos(angle) * radius, y, spring + std::sin(angle) * radius * scale);
        };
        for (int i = 0; i < segments; ++i) face({position(x, y, spring), rim(i), rim(i + 1)}, material);
    }

    void beginDecoration() { shotVertexCount = uint32_t(assets.world.vertices.size()); }
    void beginBackdrop() { backdropVertexFirst = uint32_t(assets.world.vertices.size()); }

    void finish() {
        auto& mesh = assets.world;
        assets.shotVertices.reserve(shotVertexCount);
        for (uint32_t i = 0; i < shotVertexCount; ++i) assets.shotVertices.push_back(mesh.position(i));
        const auto shotRemap = weld(assets.shotVertices, [](Vec3 p) { return std::array{p.x, p.y, p.z, 0.f, 0.f}; });
        const auto vertexRemap = weld(mesh.vertices, [](const Vertex& v) {
            return std::array{v.position.x, v.position.y, v.position.z, v.u, v.v};
        });

        struct OrderedFace { Triangle triangle; std::array<int, 6> cell; };
        std::vector<OrderedFace> ordered;
        ordered.reserve(mesh.triangles.size());
        const auto spanClass = [](float length) { return length > 24.f ? 3 : length > 12.f ? 2 : length > 6.f ? 1 : 0; };
        for (const auto& t : mesh.triangles) {
            const auto a = mesh.position(vertexRemap[t.a]);
            const auto b = mesh.position(vertexRemap[t.b]);
            const auto c = mesh.position(vertexRemap[t.c]);
            Bounds bounds{a, a};
            expand(bounds, b);
            expand(bounds, c);
            const auto center = (a + b + c) * (1.f / 3);
            const auto span = bounds.max - bounds.min;
            // Long walls, roofs and the distant landscape must not inflate the bounds of nearby props.
            const bool backdrop = t.a >= backdropVertexFirst;
            const float cellSize = backdrop ? 14.f : 4.f;
            const std::array cell{int(backdrop), int(span.y > 5.f), spanClass(span.z), spanClass(span.x),
                                  int(std::floor(center.z / cellSize)), int(std::floor(center.x / cellSize))};
            ordered.push_back({t, cell});
        }
        std::stable_sort(ordered.begin(), ordered.end(), [](const OrderedFace& a, const OrderedFace& b) { return a.cell < b.cell; });
        assets.shotSurfaces.reserve(shotVertexCount / 2);
        assets.shotIndices.reserve(shotVertexCount / 2);
        for (uint32_t first = 0; first < ordered.size();) {
            const auto p = mesh.position(vertexRemap[ordered[first].triangle.a]);
            Chunk visual{{p, p}, first, 0};
            Chunk shot{{}, uint32_t(assets.shotSurfaces.size()), 0};
            uint32_t i = first;
            for (; i < ordered.size() && ordered[i].cell == ordered[first].cell; ++i) {
                const auto& t = ordered[i].triangle;
                const Triangle render{vertexRemap[t.a], vertexRemap[t.b], vertexRemap[t.c], t.material, t.glass};
                mesh.triangles[i] = render;
                for (const auto index : {render.a, render.b, render.c}) expand(visual.bounds, mesh.position(index));
                if (t.a < shotVertexCount) {
                    const std::array indices{shotRemap[t.a], shotRemap[t.b], shotRemap[t.c]};
                    if (!shot.count) shot.bounds = {assets.shotVertices[indices[0]], assets.shotVertices[indices[0]]};
                    for (const auto index : indices) expand(shot.bounds, assets.shotVertices[index]);
                    assets.shotSurfaces.push_back({t.material, t.glass});
                    assets.shotIndices.push_back(indices);
                    ++shot.count;
                }
            }
            visual.count = i - first;
            mesh.chunks.push_back(visual);
            if (shot.count) assets.shotChunks.push_back(shot);
            first = i;
        }
    }

    // Valley homes, recorded so the invasion can set some of them alight.
    std::vector<Vec3> homes;

    void smoke(Vec3 base, float height, Vec3 drift, float size) { assets.smoke.push_back({base, height, drift, size, .07f}); }

private:
    Assets& assets;
    uint16_t firstMaterial;
    uint32_t shotVertexCount = 0;
    uint32_t backdropVertexFirst = 0;
    std::array<float, SurfaceCount> repeats;
    float xOffset = 0;

    Vec3 position(float x, float y, float height = 0) const { return fod::position(x + xOffset, y, height); }

    std::array<Vec3, 4> corners(Rect r, float height) const {
        return {position(r.left, r.bottom, height), position(r.left, r.top, height),
                position(r.right, r.top, height), position(r.right, r.bottom, height)};
    }

    void cuboid(const std::array<Vec3, 4>& lower, const std::array<Vec3, 4>& upper, Surface material, Side plastered) {
        const auto& [a, b, c, d] = lower;
        const auto& [e, f, g, h] = upper;
        quad({d, c, b, a}, material);
        quad({e, f, g, h}, material);
        quad({a, b, f, e}, plastered == Side::West ? Plaster : material);
        quad({b, c, g, f}, plastered == Side::North ? Plaster : material);
        quad({c, d, h, g}, plastered == Side::East ? Plaster : material);
        quad({d, a, e, h}, plastered == Side::South ? Plaster : material);
    }

    template<typename Sample>
    void texture(Surface material, int size, float repeat, Sample sample) {
        Texture texture{size, size, {}};
        texture.pixels.reserve(size_t(size * size));
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x) texture.pixels.push_back(sample(x, y));
        assets.materials[firstMaterial + material].texture = int16_t(assets.textures.size());
        assets.textures.push_back(std::move(texture));
        repeats[material] = repeat;
    }

    template<size_t N>
    uint32_t vertices(const std::array<Vec3, N>& points, Surface surface, bool fit = false) {
        auto& mesh = assets.world;
        const auto first = uint32_t(mesh.vertices.size());
        if (assets.materials[firstMaterial + surface].texture < 0) {
            for (const auto p : points) mesh.vertices.push_back({p, 0, 0});
        } else if (fit) {
            constexpr float ends = .9999f;
            constexpr float u[]{0, 0, ends, ends}, v[]{ends, 0, 0, ends};
            for (size_t i = 0; i < N; ++i) mesh.vertices.push_back({points[i], u[i], v[i]});
        } else {
            const auto normal = cross(points[1] - points[0], points[2] - points[0]);
            const auto repeat = repeats[surface];
            const bool flat = std::abs(normal.y) >= std::max(std::abs(normal.x), std::abs(normal.z));
            const bool runsEast = flat || std::abs(normal.z) >= std::abs(normal.x);
            for (const auto p : points) mesh.vertices.push_back({p, (runsEast ? p.x : p.z) * repeat,
                                                                (flat ? p.z : p.y) * repeat});
        }
        return first;
    }

public:
    void quad(const std::array<Vec3, 4>& points, Surface surface, int16_t glass = -1, bool fit = false) {
        const auto first = vertices(points, surface, fit);
        const auto material = uint16_t(firstMaterial + surface);
        // The software renderer lights the front on the side facing away from the winding.
        assets.world.triangles.push_back({first, first + 2, first + 1, material, glass});
        assets.world.triangles.push_back({first, first + 3, first + 2, material, glass});
    }

    void face(const std::array<Vec3, 3>& points, Surface surface) {
        const auto first = vertices(points, surface);
        assets.world.triangles.push_back({first, first + 2, first + 1, uint16_t(firstMaterial + surface)});
    }

private:
    static void expand(Bounds& bounds, Vec3 p) {
        bounds.min = {std::min(bounds.min.x, p.x), std::min(bounds.min.y, p.y), std::min(bounds.min.z, p.z)};
        bounds.max = {std::max(bounds.max.x, p.x), std::max(bounds.max.y, p.y), std::max(bounds.max.z, p.z)};
    }
};

struct RailingRun {
    float start, end, inside, outside;
    int bays;
    bool runsEast;

    Rect section(float left, float right, float near, float far) const {
        return runsEast ? Rect{left, near, right, far} : Rect{near, left, far, right};
    }
};

constexpr RailingRun frontRailing{106, 555, site.bottom, site.bottom + 5, 5, true};
constexpr RailingRun rearRailing{112, 568, site.top - 5, site.top, 5, true};
constexpr RailingRun westRailings[]{{5, 136, site.left - 5, site.left, 2, false},
                                   {283, 411, site.left - 5, site.left, 2, false}};
constexpr RailingRun lawnRailings[]{frontRailing, rearRailing, westRailings[0], westRailings[1]};

void railingBases(Builder& b) {
    for (const auto& run : lawnRailings) {
        const auto span = run.section(run.start, run.end, run.inside, run.outside);
        b.prop(span, .78f, GardenWall);
        b.box(run.section(run.start, run.end, run.inside - .7f, run.outside + .7f), .72f, .81f, MonumentStone);
        b.collision(span, .78f, 2.13f, false);
        for (int i = 1; i < run.bays; ++i) {
            const float x = run.start + i * (run.end - run.start) / run.bays;
            b.prop(run.section(x - 4.25f, x + 4.25f, run.inside - 1.45f, run.outside + 1.45f), 2.13f, GardenWall);
            b.box(run.section(x - 5, x + 5, run.inside - 2.25f, run.outside + 2.25f), 2.06f, 2.19f, MonumentStone);
        }
    }
}

void railingBars(Builder& b) {
    for (const auto& run : lawnRailings) {
        const float y = (run.inside + run.outside) * .5f;
        for (const float height : {.98f, 1.89f}) {
            b.box(run.section(run.start, run.end, y - .58f, y + .58f), height, height + .061f, Iron);
        }
        const float bayWidth = (run.end - run.start) / run.bays;
        const int intervals = int(std::round(bayWidth / 8.2f));
        for (int bay = 0; bay < run.bays; ++bay) {
            const float start = run.start + bayWidth * bay;
            for (int bar = 1; bar < intervals; ++bar) {
                const float x = start + bayWidth * bar / intervals;
                b.box(run.section(x - .45f, x + .45f, y - .38f, y + .38f), .8f, 2.025f, Iron);
            }
        }
    }
}

void grounds(Builder& b) {
    b.floor({site.left - 12, site.top - 12, site.right + 12, house.top}, Grass);
    b.floor({site.left - 12, house.top, house.left, house.bottom}, Grass);
    b.floor({house.right, house.top, site.right + 12, house.bottom}, Grass);
    b.floor({site.left - 12, house.bottom, site.right + 12, site.bottom + 12}, Grass);
    constexpr Rect drives[]{
        {-33, -24, 259, 3}, {-33, 3, -3, 177}, {-33, 177, 27, 194},
        {4, 194, 29, 258}, {-33, 258, 29, 285}, {-33, 285, -3, 468},
        {-33, 468, 439, 493}, {363, 430, 439, 468},
        {258, -40, 478, 28}, {265, 392, 475, 433}, {439, 468, garageBay.x, 493},
    };
    for (const auto r : drives) b.floor(r, Pavers, .015f);
    b.floor({garageBay.x - 6, house.bottom, garageBay.endX + 6, site.bottom}, Pavers, .014f);
    b.floor({garageBay.x, house.bottom, garageBay.endX, site.bottom}, Asphalt, .023f);
    b.prop(rearHedge, 2.05f, Hedge);
    b.prop(frontHedge, 2.05f, Hedge);
    b.prop(shed, 2.28f, Timber);
    b.gabledRoof({shed.left - 3, shed.top - 3, shed.right + 3, shed.bottom + 3}, 2.28f, 2.70f);
    b.prop({32, 282, 51, 325}, .47f, Timber);
    b.prop({35, 161, 51, 190}, .47f, Timber);
    b.prop({259, -22, 290, 9}, .88f, Counter);
    b.prop({264, -35, 285, -23}, .52f, Timber);
    b.prop({264, 10, 285, 23}, .52f, Timber);
    b.collision({188, 461, 196, 469}, 0, 3.16f);
    b.round(192, 465, 0, 3.16f, 3.8f, 1.9f, Timber);

    constexpr Rect gardenWalls[] {
        {site.left - 5, site.top - 5, rearRailing.start, site.top},
        {rearRailing.end, site.top - 5, site.right + 5, site.top},
        {site.left - 5, site.bottom, frontRailing.start, site.bottom + 5},
        {frontRailing.end, site.bottom, garageBay.x, site.bottom + 5},
        {garageBay.endX, site.bottom, site.right + 5, site.bottom + 5},
        {site.left - 5, site.top, site.left, westRailings[0].start},
        {site.left - 5, westRailings[0].end, site.left, westRailings[1].start},
        {site.left - 5, westRailings[1].end, site.left, site.bottom},
        {site.right, site.top, site.right + 5, site.bottom},
    };
    for (const auto r : gardenWalls) {
        b.prop(r, 2.1f, GardenWall);
        b.box({r.left - .25f, r.top - .25f, r.right + .25f, r.bottom + .25f}, 1.98f, 2.13f, Trim);
    }
    railingBases(b);
    b.collision({garageBay.x, site.bottom, garageBay.endX, site.bottom + 5}, 0, 2.1f, false);
    for (const float x : {garageBay.x, garageBay.endX}) {
        b.prop({x - 3, site.bottom - 2, x + 3, site.bottom + 7}, 2.2f, GardenWall);
    }
}

void roof(Builder& b, Rect plan, float ceiling, float ridge) {
    b.solid(plan, ceiling, ceiling + .17f, Roof);
    b.floor(plan, Ceiling, ceiling - .006f);
    const Rect eaves{plan.left - 4, plan.top - 4, plan.right + 4, plan.bottom + 4};
    b.box(eaves, ceiling + .15f, ceiling + .27f, Trim);
    b.gabledRoof(eaves, ceiling + .27f, ridge);
}

void shell(Builder& b) {
    b.floor({house.left, 28, grandHall.left, 92}, Marble);
    b.floor({grandHall.right, 28, house.right, 92}, Marble);
    b.floor(grandHall, Marble);
    b.floor({house.left, 332, grandHall.left, house.bottom}, Marble);
    b.floor({grandHall.right, 332, garage.left, house.bottom}, Marble);
    b.floor(westWing, Tile);
    b.floor({eastWing.left, eastWing.top, 572, eastWing.bottom}, Oak);
    b.floor(garage, Concrete);
    b.floor({222, 92, 270, 332}, Grass);
    b.floor({466, 92, 518, 332}, Grass);
    b.floor({222, 194, 270, 229}, Tile, .018f);
    b.floor({466, 234, 518, 269}, Oak, .018f);
    for (const auto wall : exterior) b.wall(wall, Stucco);
    for (const auto window : windows) b.window(window);

    // Lobby-to-lobby wings remain separate through the middle, with one crossover apiece.
    constexpr Wall wingWalls[]{
        {222, 92, 270, 92}, {222, 332, 270, 332}, {222, 92, 222, 194}, {222, 229, 222, 332},
        {466, 92, 518, 92}, {466, 332, 518, 332}, {518, 92, 518, 234}, {518, 269, 518, 332},
        {572, 92, 572, 177}, {572, 212, 572, 265}, {572, 299, 572, 392},
        {572, 92, 597, 92}, {642, 92, 680, 92},
    };
    for (const auto wall : wingWalls) b.wall(wall, Plaster);
    for (const auto wall : {Wall{270, 194, 222, 194}, Wall{222, 229, 270, 229},
                           Wall{518, 234, 466, 234}, Wall{466, 269, 518, 269}}) b.wall(wall, Stucco);
    b.wall({222, 194, 222, 229}, Plaster, wallHeight, 2.48f);
    b.wall({518, 234, 518, 269}, Plaster, wallHeight, 2.48f);
    b.wall({572, 177, 572, 212}, Plaster, wallHeight, 2.39f);
    b.wall({572, 265, 572, 299}, Plaster, wallHeight, 2.39f);
    b.wall({597, 92, 642, 92}, Plaster, wallHeight, 2.39f);
    b.wall(garageBay, Stucco, wallHeight, 2.63f);

    constexpr Wall hallWalls[]{
        {270, 92, 270, 194}, {270, 229, 270, 332},
        {466, 92, 466, 234}, {466, 269, 466, 332},
    };
    for (const auto wall : hallWalls) b.wall(wall, Plaster, hallHeight);
    b.wall({270, 194, 270, 229}, Plaster, hallHeight, 2.6f);
    b.wall({466, 234, 466, 269}, Plaster, hallHeight, 2.6f);
    for (const float x : {270.f, 466.f}) {
        b.wall({x, 28, x, 92}, Plaster, hallHeight, wallHeight);
        b.wall({x, 332, x, 392}, Plaster, hallHeight, wallHeight);
    }
    b.wall({270, 392, frontDoorLeft, 392}, Stucco, hallHeight);
    b.wall({frontDoorRight, 392, 466, 392}, Stucco, hallHeight);
    b.wall({frontDoorLeft, 392, frontDoorRight, 392}, Stucco, hallHeight, 3.25f);
    b.wall({466, 28, rearDoorRight, 28}, Stucco, hallHeight);
    b.wall({rearDoorLeft, 28, 270, 28}, Stucco, hallHeight);
    b.wall({rearDoorRight, 28, rearDoorLeft, 28}, Stucco, hallHeight, 3.25f);
    roof(b, grandHall, hallHeight, 7.52f);
    roof(b, {52, 28, 222, 392}, wallHeight, 4.78f);
    roof(b, {518, 28, 680, 392}, wallHeight, 4.78f);
    b.solid({222, 28, 270, 92}, wallHeight, wallHeight + .2f, Roof);
    b.solid({222, 332, 270, 392}, wallHeight, wallHeight + .2f, Roof);
    b.solid({466, 28, 518, 92}, wallHeight, wallHeight + .2f, Roof);
    b.solid({466, 332, 518, 392}, wallHeight, wallHeight + .2f, Roof);
    b.solid({222, 194, 270, 229}, wallHeight, wallHeight + .23f, Roof);
    b.solid({466, 234, 518, 269}, wallHeight, wallHeight + .23f, Roof);
    b.floor({222, 194, 270, 229}, Ceiling, wallHeight - .006f);
    b.floor({466, 234, 518, 269}, Ceiling, wallHeight - .006f);
}

void kitchen(Builder& b) {
    b.prop({130, 98, 200, 116}, .91f, Counter);
    b.prop({130, 153, 149, 212}, .91f, Counter);
    b.prop({129, 221, 149, 251}, 2.03f, Sage);
    b.floor({133, 162, 145, 184}, Steel, .925f);
    b.floor({153, 101, 192, 113}, Trim, .925f);
    b.prop({186, 194, 235, 273}, .92f, Timber);
    b.box({185, 193, 236, 274}, .89f, .95f, Counter);
    b.prop({166, 320, 222, 366}, .76f, Timber);
    constexpr Rect chairs[]{{158, 335, 171, 352}, {216, 335, 229, 352},
                            {187, 310, 203, 323}, {187, 362, 203, 375}};
    for (const auto seat : chairs) b.prop(seat, .49f, Timber);
}

void garageStructure(Builder& b) {
    b.prop({519, 143, 528, 172}, .92f, Timber);
    b.prop({519, 286, 528, 307}, .92f, Timber);
    b.prop({649, 112, 677, 142}, .94f, Timber);
    b.prop({662, 279, 677, 330}, 1.55f, Sage);

    b.collision({621, 150, 668, 248}, 0, 1.52f);
    b.taperedBox({623, 152, 666, 246}, {623, 150, 666, 248}, .32f, .55f, WagonPaint);
    b.taperedBox({623, 150, 666, 248}, {624, 153, 665, 246}, .55f, .89f, WagonPaint);
    b.taperedBox({626, 153, 663, 221}, {629, 159, 660, 212}, .89f, 1.49f, CarGlass);
    b.box({628, 158, 661, 213}, 1.48f, 1.55f, WagonPaint);
    for (const float axle : {169.f, 225.f}) {
        b.axle(621, 627, axle, .34f, .34f, Tire);
        b.axle(662, 668, axle, .34f, .34f, Tire);
    }
}

void sealedStairs(Builder& b) {
    // Keep the dead-end flight sealed from the sides as well as from the hall.
    b.collision(stairFlight, 0.f, 3.35f, false);
    constexpr int steps = 16;
    for (int i = 0; i < steps; ++i) {
        const float front = stairFlight.bottom - float(i) * 3.25f;
        const float rise = .18125f * (i + 1);
        b.box({stairFlight.left, front - 3.25f, stairFlight.right, front}, 0, rise, DarkWood);
        b.box({stairFlight.left - .27f, front - 3.25f, stairFlight.right + .27f, front + .18f},
              rise - .055f, rise, Timber);
    }
    b.box({430.7f, 172, 463.4f, 177}, 2.81f, 2.90f, Timber);
    b.box({435, 171.7f, 460, 174.2f}, 2.90f, 5.03f, DarkWood);
    b.box({436.7f, 174.2f, 458.3f, 174.5f}, 3.02f, 4.91f, Timber);
    b.box({454.6f, 174.5f, 456.4f, 175.f}, 3.86f, 3.94f, Brass);

    constexpr Rect caseFoot{429, stairFlight.bottom, 465, stairFlight.bottom + 8.f};
    b.collision({429, 220, 465, 237}, 0, 1.78f);
    b.fallenBox({429, caseFoot.top, 465, caseFoot.top + .85f}, 0, 2.33f, DarkWood);
    for (const float edge : {caseFoot.left, caseFoot.right - 1.6f}) {
        b.fallenBox({edge, caseFoot.top, edge + 1.6f, caseFoot.bottom}, 0, 2.33f, Timber);
    }
    for (const float level : {0.f, .58f, 1.15f, 1.73f, 2.26f}) {
        b.fallenBox(caseFoot, level, level + .075f, Timber);
        if (level < 2.2f) {
            b.fallenBox({434, caseFoot.top + .9f, 443, caseFoot.bottom - .8f}, level + .075f, level + .34f, RedRunner);
            b.fallenBox({448, caseFoot.top + 1.4f, 453, caseFoot.bottom - 1.f}, level + .075f, level + .42f, Sage);
        }
    }
}

void column(Builder& b, float x, float y, float height, float radius);

void hallStructure(Builder& b) {
    // Full-height bases cover rotations alongside the nave. The urn's plinth breaks the central shot.
    for (const float y : {108.f, 165.f, 277.f, 306.f}) {
        for (const float x : {286.f, 450.f}) {
            b.collision({x - 5, y - 5, x + 5, y + 5}, 0, hallHeight);
            column(b, x, y, hallHeight - .05f, 4.8f);
        }
    }
    b.prop({342, 191, 395, 232}, 1.83f, MonumentStone);
    b.prop({301, 127, 313, 169}, 1.0f, Upholstery);
    b.prop({423, 251, 435, 293}, 1.0f, Upholstery);

    // Entrance statuary masks the cross-hall sightline while leaving each exit wide.
    for (const float y : {48.f, 377.f}) {
        for (const float x : {258.f, 478.f}) b.prop({x - 8, y - 7, x + 8, y + 7}, 1.81f, MonumentStone);
    }
    for (const float x : {290.f, 446.f}) {
        b.collision({x - 7, 408, x + 7, 422}, 0, 4.12f);
        column(b, x, 415, 4.09f, 5.4f);
    }
}

void sash(Builder& b, Wall section) {
    const auto r = wallRect(section);
    const bool vertical = section.x == section.endX;
    const float start = vertical ? r.top : r.left, end = vertical ? r.bottom : r.right;
    const auto rail = [&](float a, float z, float lower, float upper, float lip) {
        const Rect trim = vertical ? Rect{r.left - lip, a, r.right + lip, z} :
                                    Rect{a, r.top - lip, z, r.bottom + lip};
        b.box(trim, lower, upper, Ivory);
    };
    rail(start - 1.8f, end + 1.8f, .96f, 1.075f, 1.15f);
    rail(start - 1.2f, end + 1.2f, 2.13f, 2.25f, .75f);
    rail(start - 1, start + .9f, 1.07f, 2.14f, .55f);
    rail(end - .9f, end + 1, 1.07f, 2.14f, .55f);
    rail(start, end, 1.64f, 1.69f, .15f);
    const float first = start + (end - start) / 3, second = end - (end - start) / 3;
    rail(first - .45f, first + .45f, 1.07f, 2.14f, .12f);
    rail(second - .45f, second + .45f, 1.07f, 2.14f, .12f);
}

void lantern(Builder& b, float x, float y, float height) {
    b.box({x - 2.8f, y - 1.6f, x + 2.8f, y + 1.6f}, height, height + .08f, Trim);
    b.box({x - 2.f, y - 1.2f, x + 2.f, y + 1.2f}, height + .08f, height + .32f, Amber);
    b.box({x - 2.8f, y - 1.6f, x + 2.8f, y + 1.6f}, height + .32f, height + .39f, Trim);
    b.lamp(x, y, height + .12f, 3.1f, .39f);
}

void column(Builder& b, float x, float y, float height, float radius) {
    b.round(x, y, 0, .18f, radius + 2.f, radius + 2.f, MonumentStone);
    b.round(x, y, .18f, .31f, radius + 1.3f, radius, Ivory);
    b.round(x, y, .31f, height - .28f, radius, radius * .83f, Ivory);
    b.round(x, y, height - .28f, height - .14f, radius * .83f, radius + 1.6f, MonumentStone);
    b.round(x, y, height - .14f, height, radius + 2.f, radius + 2.f, Ivory);
}

void paneledArch(Builder& b, float x, float y, float bottom, float spring, float width) {
    b.archPanel(x, y, bottom, spring, width, Ivory);
    b.archPanel(x, y + .23f, bottom + .12f, spring, width - 2, StainedGlass);
    b.box({x - .47f, y + .25f, x + .47f, y + .5f}, bottom + .1f, spring + (width - 2) * scale, Ivory);
    b.box({x - width + 2, y + .15f, x + width - 2, y + .6f}, spring - .04f, spring + .01f, Ivory);
}

void exteriorDetails(Builder& b) {
    for (const auto window : windows) sash(b, window);
    b.box({280, 390, 456, 426}, 3.91f, 4.125f, Ivory);
    b.porticoGable();
    b.box({281, 423.8f, 455, 426}, 4.06f, 4.16f, Marble);
    b.box({277, 392, 459, 428}, .013f, .083f, GardenWall);
    b.box({269, 415, 467, 433}, .012f, .048f, Marble);
    for (const float x : {frontDoorLeft, frontDoorRight}) {
        b.box({x - 1.5f, 388.5f, x + 1.5f, 395.5f}, .07f, 3.25f, Ivory);
    }
    b.box({304, 388.5f, 432, 396.5f}, 3.22f, 3.47f, Ivory);
    b.box({302, 24.6f, 434, 31}, 3.22f, 3.42f, Ivory);
    b.box({295, 17, 441, 30}, 3.37f, 3.53f, Roof);
    lantern(b, 299, 394.8f, 2.31f);
    lantern(b, 437, 394.8f, 2.31f);
    lantern(b, 297, 24.9f, 2.31f);
    lantern(b, 439, 24.9f, 2.31f);
    for (const float x : {294.f, 442.f}) paneledArch(b, x, 394.8f, 4.71f, 5.39f, 13);
    paneledArch(b, 368, 394.85f, 5.77f, 6.01f, 4.8f);

    for (const float x : {119.f, 612.f}) {
        b.box({x - 9, 145, x + 8, 169}, 4.0f, 5.57f, Brick);
        b.box({x - 11, 143, x + 10, 171}, 5.49f, 5.63f, Trim);
        b.round(x, 153, 5.62f, 5.87f, 2.8f, 2.6f, Terracotta);
        b.round(x, 162, 5.62f, 5.87f, 2.8f, 2.6f, Terracotta);
    }
}

void foliage(Builder& b, float x, float y, float height, float radius, Surface material) {
    b.round(x, y, height - .53f, height, radius * .4f, radius, material);
    b.round(x, y, height, height + .8f, radius, 0, material);
}

enum LandBand { EstateLawn, Estate, InsideVerge, InsideRoad, OutsideRoad, OutsideVerge, Lots,
                UpperMeadow, Foothills, Timberline, Ridge, HighRidge, LandBandCount };
using RoadSection = std::array<Vec3, LandBandCount>;

struct RoadBend {
    float x, y, grade;
    float foothillRise, ridgeRise, summitRise;
};

constexpr RoadBend street[] {
    {-170, 608, -.35f, 3, 12, 22}, {10, 650, -.15f, 4, 16, 24}, {250, 655, 0, 3, 12, 32},
    {510, 655, 0, 3.5f, 15, 23}, {785, 655, 0, 5, 19, 25}, {928, 618, .85f, 7, 24, 33},
    {1025, 520, 1.8f, 8, 16, 29}, {1060, 270, 2.9f, 9, 21, 35}, {1018, 53, 3.6f, 7, 28, 33},
    {906, -127, 3.45f, 8, 23, 27}, {765, -230, 2.45f, 9, 31, 36}, {543, -264, 1.48f, 8, 24, 33},
    {302, -277, 1.33f, 6, 17, 28}, {64, -244, 1.56f, 8, 26, 34}, {-146, -196, 1.95f, 5, 17, 30},
    {-298, -99, 1.6f, 5.5f, 25, 34}, {-371, 40, 1.03f, 5, 23, 29},
    {-390, 270, .6f, 5.5f, 17, 24}, {-345, 472, .25f, 3.5f, 13, 25},
};

Vec3 acrossStreet(Vec3 from, Vec3 to) {
    const Vec3 delta = to - from;
    return normalized({delta.z, 0, -delta.x});
}

Vec3 mix(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }

void pine(Builder& b, Vec3 base, float height, Surface shade, bool trunk = true) {
    constexpr int sides = 5;
    for (int i = 0; i < sides; ++i) {
        const float a = i * (2 * pi / sides), next = (i + 1) * (2 * pi / sides);
        const Vec3 left{std::cos(a), 0, std::sin(a)}, right{std::cos(next), 0, std::sin(next)};
        if (trunk) b.face({base + right * .13f, base + left * .13f, base + Vec3{0, height * .35f, 0}}, DarkWood);
        const Vec3 skirt = base + Vec3{0, height * .25f, 0};
        const Vec3 crown = base + Vec3{0, height * .49f, 0};
        b.face({skirt + right * (height * .23f), skirt + left * (height * .23f), base + Vec3{0, height * .79f, 0}}, shade);
        b.face({crown + right * (height * .175f), crown + left * (height * .175f), base + Vec3{0, height, 0}}, shade);
    }
}

enum class HomeStyle { Cottage, AFrame, SplitLevel };

struct Homesite {
    size_t segment;
    float along, setback, width, depth, eaves, ridge;
    Surface siding;
    bool frontGable;
    HomeStyle style = HomeStyle::Cottage;
};

struct HomeFrame {
    Builder& b;
    Vec3 base, along, outward;

    Vec3 at(float x, float z, float height) const { return base + along * x + outward * z + Vec3{0, height, 0}; }

    std::array<Vec3, 4> corners(Rect plan, float height) const {
        return {at(plan.left, plan.bottom, height), at(plan.left, plan.top, height),
                at(plan.right, plan.top, height), at(plan.right, plan.bottom, height)};
    }

    void box(Rect plan, float low, float high, Surface surface) const {
        b.box(corners(plan, low), corners(plan, high), surface);
    }

    void panel(float x, float width, float low, float high, float z, Surface material) const {
        b.quad({at(x, z, low), at(x + width, z, low), at(x + width, z, high), at(x, z, high)}, material);
    }

    void window(float x, float width, float low, float high, float z, Surface frame) const {
        panel(x, width, low, high, z - .022f, frame);
        panel(x + .09f, width - .18f, low + .09f, high - .09f, z - .034f, CabinGlass);
    }

    void apron(Vec3 roadEdge, float x, float halfWidth, float front, float grade, Surface material) const {
        const Vec3 sill = at(x, front, grade), road = roadEdge + along * x;
        const Vec3 shoulder = along * halfWidth;
        b.quad({road - shoulder, road + shoulder, sill + shoulder, sill - shoulder}, material);
    }
};

void cabin(const HomeFrame& frame, const Homesite& plot) {
    auto& b = frame.b;
    const auto at = [&](float x, float z, float height) { return frame.at(x, z, height); };
    const auto corners = [&](float left, float front, float right, float back, float height) {
        return frame.corners({left, front, right, back}, height);
    };
    const float h = plot.width * .5f;
    const auto walls = [&](float rise) { return corners(-h, 0, h, plot.depth, rise); };
    b.box(walls(-1.5f), walls(.05f), GardenWall);
    b.box(walls(.05f), walls(plot.eaves), plot.siding);

    auto roof = corners(-h - .31f, -.37f, h + .31f, plot.depth + .34f, plot.eaves);
    auto gableWall = walls(plot.eaves);
    if (plot.frontGable) {
        std::rotate(roof.begin(), roof.begin() + 1, roof.end());
        std::rotate(gableWall.begin(), gableWall.begin() + 1, gableWall.end());
    }
    const auto& [backLeft, frontLeft, frontRight, backRight] = roof;
    const Vec3 gable{0, plot.ridge - plot.eaves, 0};
    const auto peakLeft = (backLeft + frontLeft) * .5f + gable, peakRight = (frontRight + backRight) * .5f + gable;
    b.quad({backLeft, peakLeft, peakRight, backRight}, Roof);
    b.quad({peakLeft, frontLeft, frontRight, peakRight}, Roof);
    b.face({gableWall[0], gableWall[1], (gableWall[0] + gableWall[1]) * .5f + gable}, plot.siding);
    b.face({gableWall[2], gableWall[3], (gableWall[2] + gableWall[3]) * .5f + gable}, plot.siding);
    const std::array roofline{backLeft, peakLeft, frontLeft, frontRight, peakRight, backRight};
    for (size_t i = 0; i < roofline.size(); ++i) {
        const auto first = roofline[i], next = roofline[(i + 1) % roofline.size()];
        const Vec3 fascia{0, .16f, 0};
        b.quad({first - fascia, next - fascia, next, first}, Ivory);
    }
    const auto chimney = [&](float height) { return corners(h - 1.5f, plot.depth * .41f, h - .85f, plot.depth * .41f + .72f, height); };
    b.box(chimney(plot.eaves - .3f), chimney(plot.ridge + .3f), Brick);

    frame.panel(-.6f, 1.2f, .06f, 2.02f, -.024f, Ivory);
    frame.panel(-.53f, 1.06f, .06f, 1.93f, -.034f, DarkWood);
    frame.panel(-.35f, .7f, 1.42f, 1.73f, -.042f, CabinGlass);
    for (const float x : {-h + .66f, h - 2.14f}) {
        for (float y = .85f; y + 1.1f < plot.eaves; y += 2.16f) {
            frame.window(x, 1.47f, y, y + 1.11f, 0, Ivory);
            frame.panel(x + .70f, .07f, y + .065f, y + 1.03f, -.045f, Ivory);
        }
    }
    for (const float side : {-1.f, 1.f}) {
        const auto sideWindow = [&](float overhang, float front, float back, float bottom, float top, Surface surface) {
            const float x = side * (h + overhang);
            const float near = side > 0 ? front : back, far = side > 0 ? back : front;
            b.quad({at(x, near, bottom), at(x, far, bottom), at(x, far, top), at(x, near, top)}, surface);
        };
        sideWindow(.022f, 1.1f, 2.62f, .83f, 1.96f, Ivory);
        sideWindow(.035f, 1.2f, 2.52f, .93f, 1.86f, CabinGlass);
    }
    if (plot.frontGable) {
        b.box(corners(-2.18f, -1.05f, 2.18f, .005f, -.40f), corners(-2.18f, -1.05f, 2.18f, .005f, .07f), Timber);
        b.quad({at(-2.2f, -.01f, 2.63f), at(2.2f, -.01f, 2.63f),
                at(2.2f, -1.35f, 2.23f), at(-2.2f, -1.35f, 2.23f)}, Roof);
        for (const float x : {-1.87f, 1.87f}) {
            b.box(corners(x - .06f, -1.06f, x + .06f, -.94f, .07f), corners(x - .06f, -1.06f, x + .06f, -.94f, 2.34f), Timber);
        }
    }
}

void aFrame(const HomeFrame& frame, const Homesite& plot) {
    auto& b = frame.b;
    const float h = plot.width * .5f;
    const auto at = [&](float x, float z, float y) { return frame.at(x, z, y); };
    frame.box({-h, 0, h, plot.depth}, -1.4f, .34f, GardenWall);
    const Vec3 left = at(-h - .27f, -.45f, .32f), right = at(h + .27f, -.45f, .32f);
    const Vec3 peak = at(0, -.45f, plot.ridge), end = at(0, plot.depth + .4f, plot.ridge);
    b.quad({at(-h - .27f, plot.depth + .4f, .32f), left, peak, end}, Roof);
    b.quad({right, at(h + .27f, plot.depth + .4f, .32f), end, peak}, Roof);
    b.face({at(-h, 0, .32f), at(h, 0, .32f), at(0, 0, plot.ridge)}, plot.siding);
    b.face({at(h, plot.depth, .32f), at(-h, plot.depth, .32f), at(0, plot.depth, plot.ridge)}, plot.siding);
    const Vec3 trim{0, .20f, 0};
    b.quad({left - trim, peak - trim, peak, left}, Ivory);
    b.quad({peak - trim, right - trim, right, peak}, Ivory);

    frame.window(-2.6f, 5.2f, .64f, 2.52f, 0, DarkWood);
    frame.window(-1.56f, 3.12f, 2.82f, 4.52f, 0, DarkWood);
    b.face({at(-1.07f, -.035f, 4.78f), at(1.07f, -.035f, 4.78f), at(0, -.035f, plot.ridge - 1.05f)}, CabinGlass);
    for (const float mullion : {-.89f, .85f}) frame.panel(mullion, .10f, .65f, 2.54f, -.048f, DarkWood);
    frame.panel(-.09f, .18f, 2.84f, 4.55f, -.05f, DarkWood);
    frame.panel(-.61f, 1.22f, .36f, 2.16f, -.061f, Trim);
    frame.window(-.46f, .92f, 1.28f, 2.04f, -.062f, DarkWood);
    frame.box({-h + .25f, -1.67f, h - .25f, 0}, -.03f, .4f, Timber);
    frame.box({-1.2f, -2.02f, 1.2f, -1.66f}, -.41f, .18f, MonumentStone);
    frame.box({h - .4f, 2.4f, h + .43f, 3.3f}, -.7f, 4.35f, GardenWall);
    frame.box({h - .48f, 2.32f, h + .52f, 3.38f}, 4.31f, 4.49f, Trim);
}

void splitLevel(const HomeFrame& frame, const Homesite& plot) {
    auto& b = frame.b;
    const float h = plot.width * .5f;
    const Rect lower{-h, 0, h, plot.depth}, upper{-h - .22f, -.12f, h + .05f, plot.depth - .1f};
    frame.box(lower, -1.7f, 2.17f, GardenWall);
    frame.box(upper, 2.17f, 4.45f, plot.siding);
    const Vec3 lipLeft = frame.at(-h - .7f, -.74f, 4.7f), lipRight = frame.at(h + .64f, -.74f, 4.7f);
    const Vec3 endLeft = frame.at(-h - .7f, plot.depth + .38f, 5.6f), endRight = frame.at(h + .64f, plot.depth + .38f, 5.6f);
    b.quad({endLeft, lipLeft, lipRight, endRight}, Roof);
    const Vec3 fascia{0, .28f, 0};
    b.quad({lipLeft - fascia, lipRight - fascia, lipRight, lipLeft}, DarkWood);
    b.quad({endLeft - fascia, lipLeft - fascia, lipLeft, endLeft}, DarkWood);
    b.quad({lipRight - fascia, endRight - fascia, endRight, lipRight}, DarkWood);
    b.quad({endRight - fascia, endLeft - fascia, endLeft, endRight}, DarkWood);
    b.quad({frame.at(-h - .22f, plot.depth - .1f, 4.44f), frame.at(-h - .22f, -.12f, 4.44f),
            frame.at(-h - .22f, -.12f, 4.78f), frame.at(-h - .22f, plot.depth - .1f, 5.52f)}, plot.siding);
    b.quad({frame.at(h + .05f, -.12f, 4.44f), frame.at(h + .05f, plot.depth - .1f, 4.44f),
            frame.at(h + .05f, plot.depth - .1f, 5.52f), frame.at(h + .05f, -.12f, 4.78f)}, plot.siding);
    frame.panel(-h - .22f, 2 * h + .27f, 4.41f, 4.78f, -.121f, plot.siding);
    b.quad({frame.at(h + .05f, plot.depth - .1f, 4.44f), frame.at(-h - .22f, plot.depth - .1f, 4.44f),
            frame.at(-h - .22f, plot.depth - .1f, 5.52f), frame.at(h + .05f, plot.depth - .1f, 5.52f)}, plot.siding);
    const float bay = (plot.width - .75f) / 4;
    for (int i = 0; i < 4; ++i) frame.window(-h + .37f + i * bay, bay - .075f, 2.43f, 4.11f, -.121f, DarkWood);

    frame.panel(-h + .38f, 3.05f, .04f, 2.14f, -.02f, Trim);
    frame.panel(-h + .46f, 2.89f, .04f, 1.99f, -.036f, Sage);
    for (int i = 1; i < 5; ++i) frame.panel(-h + .47f, 2.86f, i * .38f, i * .38f + .033f, -.05f, DarkWood);
    frame.window(1.22f, 1.93f, .71f, 1.87f, 0, Ivory);
    frame.panel(.0f, 1.02f, .05f, 2.12f, -.027f, DarkWood);
    frame.window(.14f, .74f, 1.21f, 1.96f, -.035f, DarkWood);
    frame.box({-h + .05f, -1.56f, h - .05f, -.06f}, 2.13f, 2.33f, Timber);
    frame.box({-h + .06f, -1.57f, h - .04f, -1.50f}, 3.15f, 3.23f, DarkWood);
    for (int i = 0; i <= 12; ++i) {
        const float x = -h + .13f + i * (plot.width - .26f) / 12;
        frame.box({x - .033f, -1.56f, x + .033f, -1.49f}, 2.32f, 3.16f, DarkWood);
    }
}

void mailbox(const HomeFrame& site, float x) {
    site.box({x - .065f, -.071f, x + .065f, .071f}, -.16f, .98f, Timber);
    site.box({x - .145f, -.24f, x + .145f, .25f}, .85f, 1.11f, Sage);
    site.panel(x - .107f, .214f, .895f, 1.06f, -.251f, Iron);
    site.box({x + .148f, -.06f, x + .174f, .12f}, .97f, 1.035f, RedRunner);
}

void parkedPickup(const HomeFrame& frame) {
    frame.box({-.89f, 0, .89f, 3.9f}, .34f, .87f, WagonPaint);
    frame.box({-.84f, 1.02f, .84f, 2.25f}, .87f, 1.57f, CarGlass);
    frame.box({-.86f, 1.04f, .86f, 2.25f}, 1.53f, 1.66f, WagonPaint);
    frame.box({-.84f, .82f, .84f, 2.34f}, .88f, 1.f, WagonPaint);
    frame.box({-.90f, -.04f, .90f, .058f}, .29f, .4f, Steel);
    frame.panel(-.31f, .62f, .51f, .69f, -.008f, Iron);
    for (const float x : {-.77f, .46f}) frame.panel(x, .31f, .58f, .76f, -.016f, Ivory);
    for (const float x : {-.81f, .81f}) {
        for (const float axle : {.78f, 3.19f}) {
            frame.box({x - .135f, axle - .26f, x + .135f, axle + .26f}, .03f, .53f, Tire);
        }
    }
}

void home(Builder& b, const Homesite& plot, Vec3 from, Vec3 to) {
    const Vec3 outward = acrossStreet(from, to), along{-outward.z, 0, outward.x};
    const Vec3 streetCenter = mix(from, to, plot.along);
    const HomeFrame frame{b, streetCenter + outward * plot.setback + Vec3{0, .54f, 0}, along, outward};
    b.homes.push_back(frame.base);
    const Vec3 verge = streetCenter + outward * 2.94f + Vec3{0, .035f, 0};
    switch (plot.style) {
    case HomeStyle::AFrame:
        aFrame(frame, plot);
        frame.apron(verge, 0, 1.17f, -1.9f, .03f, Gravel);
        break;
    case HomeStyle::SplitLevel:
        splitLevel(frame, plot);
        frame.apron(verge, -plot.width * .5f + 1.9f, 1.63f, -.03f, .03f, Concrete);
        break;
    case HomeStyle::Cottage:
        cabin(frame, plot);
        frame.apron(verge, 0, plot.frontGable ? 1.14f : .76f, plot.frontGable ? -1.045f : 0.f, .056f, Pavers);
        break;
    }
    if (plot.segment == 1 || plot.segment == 2 || plot.segment == 4 || plot.segment == 11 || plot.segment == 13) {
        mailbox({b, verge + outward * 1.35f, along, outward}, 2.35f);
    }
    if (plot.segment == 2) {
        frame.apron(verge, -3.55f, 1.45f, -.72f, .08f, Gravel);
        const Vec3 parking = verge + along * -3.55f + outward * .48f + Vec3{0, .085f, 0};
        parkedPickup({b, parking, along, outward});
    }
}

void boulder(Builder& b, Vec3 center, float width, float height) {
    constexpr std::array<Vec3, 5> outline{{{-.7f, 0, .48f}, {.24f, 0, .65f}, {.66f, 0, .08f},
                                          {.25f, 0, -.57f}, {-.55f, 0, -.39f}}};
    const Vec3 crown = center + Vec3{-.1f * width, height, .11f * width};
    for (size_t i = 0; i < outline.size(); ++i) {
        const auto a = center + outline[i] * width;
        const auto edge = center + outline[(i + 1) % outline.size()] * width;
        b.face({a, edge, crown}, MountainRock);
    }
}

void uphillLane(Builder& b, const RoadSection& west, const RoadSection& east) {
    constexpr std::array center{.42f, .44f, .60f, .56f, .37f};
    static_assert(center.size() == Foothills - OutsideRoad + 1);
    constexpr std::array halfWidth{2.35f, 2.23f, 1.9f, 1.85f, 3.25f};
    using LaneSection = std::array<Vec3, 6>;
    std::array<LaneSection, center.size()> lane{};
    for (size_t i = 0; i < center.size(); ++i) {
        const auto left = west[OutsideRoad + i], right = east[OutsideRoad + i];
        const float width = game_detail::planar(right - left);
        const float edge = halfWidth[i] / width, shoulder = (halfWidth[i] + .42f) / width;
        lane[i] = {left, mix(left, right, center[i] - shoulder), mix(left, right, center[i] - edge),
                   mix(left, right, center[i] + edge), mix(left, right, center[i] + shoulder), right};
    }
    for (size_t i = 1; i < lane.size(); ++i) {
        const Surface land = i < 3 ? Meadow : ForestSlope;
        const Surface surfaces[]{land, Gravel, i < 3 ? Asphalt : Gravel, Gravel, land};
        for (size_t j = 0; j < std::size(surfaces); ++j) {
            b.quad({lane[i][j], lane[i - 1][j], lane[i - 1][j + 1], lane[i][j + 1]}, surfaces[j]);
        }
    }

    const auto arrival = (lane.back()[2] + lane.back()[3]) * .5f;
    const auto along = normalized({east[Foothills].x - west[Foothills].x, 0, east[Foothills].z - west[Foothills].z});
    const Vec3 outward{along.z, 0, -along.x};
    const HomeFrame overlook{b, arrival + outward * 1.75f + Vec3{0, .9f, 0}, along, outward};
    constexpr Homesite lodge{0, 0, 0, 8.95f, 5.45f, 3.42f, 6.7f, CabinCedar, true};
    cabin(overlook, lodge);
    overlook.apron(arrival, 0, 1.35f, -1.04f, .055f, Pavers);
    const Vec3 support{0, .64f, 0};
    const auto foot = lane[2][4] + along * .28f - Vec3{0, .11f, 0};
    const auto head = lane[3][4] + along * .28f - Vec3{0, .11f, 0};
    const Vec3 back = along * .51f;
    b.quad({head, foot, foot + support, head + support}, GardenWall);
    b.quad({foot + back, head + back, head + support + back, foot + support + back}, GardenWall);
    b.quad({foot + support, foot + support + back, head + support + back, head + support}, MonumentStone);
    b.quad({foot, foot + back, foot + support + back, foot + support}, GardenWall);
    b.quad({head + back, head, head + support, head + support + back}, GardenWall);
    mailbox({b, lane[1][4] + along * .57f, along, outward}, 0);
    for (const auto spot : {lane[2][1] - along * .72f, lane[3][1] - along * .81f, lane[3][4] + along * 1.12f}) {
        boulder(b, spot - Vec3{0, .22f, 0}, 1.62f, .92f);
    }

    for (const auto& spot : {lane[2][1] - along * 2.75f, lane[3][4] + along * 2.75f, lane[4][1] - along * 2.75f}) {
        pine(b, spot - Vec3{0, .54f, 0}, 6.2f, ForestSlope);
    }
}

void mountainCommunity(Builder& b) {
    constexpr size_t count = std::size(street);
    constexpr size_t junction = 3;
    std::array<Vec3, count> road{};
    std::array<RoadSection, count> land{};
    for (size_t i = 0; i < count; ++i) road[i] = position(street[i].x, street[i].y, street[i].grade);
    for (size_t i = 0; i < count; ++i) {
        const Vec3 point = road[i];
        const Vec3 incoming = acrossStreet(road[(i + count - 1) % count], point);
        const Vec3 outgoing = acrossStreet(point, road[(i + 1) % count]);
        const Vec3 miter = (incoming + outgoing) / (1 + dot(incoming, outgoing));
        const auto offset = [&](float across, float up) { return point + miter * across + Vec3{0, up, 0}; };
        const auto plateau = [&](float halfWidth, float halfDepth) {
            const float inset = 1 / std::max(std::abs(point.x) / halfWidth, std::abs(point.z) / halfDepth);
            return Vec3{point.x * inset, -.14f, point.z * inset};
        };
        const auto lots = offset(14.f, .88f);
        const auto foothills = offset(25.f + .31f * grain(i, 3), street[i].foothillRise);
        const auto ridge = offset(39.f + .68f * grain(i, 7), street[i].ridgeRise);
        const float forestDepth = .26f + .022f * grain(i, 18);
        land[i] = {plateau(18.5f, 14.35f), plateau(21.7f, 18.f), offset(-3.7f, -.09f), offset(-2.9f, 0.f),
                   offset(2.9f, 0.f), offset(3.75f, -.07f), lots, mix(lots, foothills, .53f), foothills,
                   mix(foothills, ridge, forestDepth) - Vec3{0, .55f, 0}, ridge,
                   offset(55.f + .75f * grain(i, 4), street[i].summitRise)};
    }
    for (size_t i = 0; i < count; ++i) {
        const auto& start = land[i];
        const auto& end = land[(i + 1) % count];
        constexpr std::array<Surface, Lots> bands{Meadow, Meadow, Gravel, Asphalt, Gravel, Meadow};
        for (size_t j = 0; j < bands.size(); ++j) {
            if (i == junction && j >= OutsideRoad) break;
            b.quad({start[j + 1], start[j], end[j], end[j + 1]}, bands[j]);
        }
        constexpr Surface mountains[]{ForestSlope, ForestSlope, ForestSlope, MountainRock, FarRidge};
        constexpr float irregularity[]{0.f, .52f, .85f, 1.2f, 3.4f, 2.7f};
        std::array<Vec3, HighRidge - Lots + 1> spur{};
        const float split = .38f + .018f * grain(i, 16);
        const Vec3 outward = acrossStreet(road[i], road[(i + 1) % count]);
        for (size_t j = 0; j < spur.size(); ++j) {
            const float roughness = i == junction && Lots + j <= Foothills ? 0.f : irregularity[j];
            spur[j] = mix(start[Lots + j], end[Lots + j], split) - outward * roughness +
                      Vec3{0, roughness * (grain(i, j + 17) / 7.5f - 1), 0};
        }
        for (size_t j = 0; j < std::size(mountains); ++j) {
            if (i == junction && Lots + j < Foothills) continue;
            const auto material = mountains[j];
            b.quad({start[Lots + j + 1], start[Lots + j], spur[j], spur[j + 1]}, material);
            b.quad({spur[j + 1], spur[j], end[Lots + j], end[Lots + j + 1]}, material);
        }
        const auto onSlope = [&](size_t band, float along, float ascent) {
            const bool left = along < split;
            const float t = left ? along / split : (along - split) / (1 - split);
            const Vec3 nearLeft = left ? start[band] : spur[band - Lots];
            const Vec3 nearRight = left ? spur[band - Lots] : end[band];
            const Vec3 farLeft = left ? start[band + 1] : spur[band + 1 - Lots];
            const Vec3 farRight = left ? spur[band + 1 - Lots] : end[band + 1];
            // Sample the same triangular surface used to draw the hillside.
            if (t + ascent <= 1) return nearLeft + (nearRight - nearLeft) * t + (farLeft - nearLeft) * ascent;
            return farRight + (farLeft - farRight) * (1 - t) + (nearRight - farRight) * (1 - ascent);
        };
        if (i != junction) {
            for (int tree = 0; tree < 4; ++tree) {
                const float t = .12f + .23f * tree + .004f * grain(i, tree);
                const auto base = onSlope(tree % 3 ? Lots : UpperMeadow, t, .1f + .046f * grain(i, tree + 8));
                pine(b, base - Vec3{0, .27f, 0}, 4.6f + .16f * grain(i, tree + 11), tree % 3 ? RidgePine : Hedge);
            }
        }
        // Give each hillside a stand of trees with rocky gaps between the stands.
        const int trees = 3 + grain(i, 37) / 3;
        for (int tree = 0; tree < trees; ++tree) {
            const float t = .15f + .037f * grain(i, 38) + .022f * grain(i, tree + 41);
            const auto base = onSlope(Foothills, t, .13f + .048f * grain(i, tree + 49));
            const float height = 4.2f + .19f * grain(i, tree + 54);
            pine(b, base - Vec3{0, .35f, 0}, height, tree % 3 ? RidgePine : ForestSlope, false);
        }
    }

    // The estate driveway joins the paved loop at its low, level southern bend.
    b.floor({garageBay.x, site.bottom, garageBay.endX, 599}, Asphalt, .02f);
    uphillLane(b, land[junction], land[junction + 1]);
    constexpr Homesite plots[]{
        {1, .54f, 6.7f, 8.4f, 5.9f, 3.45f, 5.95f, CabinCream, true},
        {2, .50f, 7.65f, 9.5f, 6.55f, 0.f, 7.55f, CabinCedar, true, HomeStyle::AFrame},
        {4, .64f, 7.55f, 9.6f, 6.2f, 0.f, 0.f, CabinSlate, false, HomeStyle::SplitLevel},
        {6, .4f, 6.25f, 8.6f, 5.5f, 4.6f, 6.7f, CabinGreen, true},
        {7, .47f, 6.6f, 8.7f, 5.2f, 3.4f, 5.4f, CabinCedar, false},
        {9, .55f, 6.75f, 7.1f, 5.5f, 4.1f, 6.05f, CabinCream, true},
        {10, .5f, 8.5f, 7.8f, 5.6f, 3.4f, 5.4f, CabinGreen, false},
        {11, .55f, 5.6f, 10.35f, 6.2f, 0.f, 0.f, CabinSlate, false, HomeStyle::SplitLevel},
        {12, .51f, 7.1f, 7.2f, 5.1f, 3.9f, 5.85f, CabinCream, false},
        {13, .51f, 7.25f, 9.35f, 6.05f, 0.f, 7.25f, CabinGreen, true, HomeStyle::AFrame},
        {15, .55f, 6.1f, 6.8f, 5.25f, 3.35f, 5.5f, CabinCream, false},
        {16, .59f, 6.6f, 8.7f, 5.4f, 4.3f, 6.6f, CabinCedar, true},
        {17, .59f, 7.05f, 7.35f, 5.15f, 3.5f, 5.35f, CabinGreen, false},
        {18, .49f, 6.45f, 8.1f, 5.45f, 3.4f, 5.68f, CabinCream, true},
    };
    for (const auto& plot : plots) home(b, plot, road[plot.segment], road[(plot.segment + 1) % count]);
}

void gardenDetails(Builder& b) {
    railingBars(b);
    for (const float x : {garageBay.x, garageBay.endX}) {
        b.box({x - 3.7f, site.bottom - 3, x + 3.7f, site.bottom + 8}, 2.16f, 2.29f, MonumentStone);
    }
    constexpr float gateY = site.bottom + 2.5f;
    for (const float height : {.27f, 1.54f, 1.89f}) {
        b.box({garageBay.x + 3, gateY - .6f, garageBay.endX - 3, gateY + .6f}, height, height + .058f, Tire);
    }
    for (int i = 0; i < 13; ++i) {
        const float x = garageBay.x + 6.f * (i + 1);
        b.box({x - .35f, gateY - .4f, x + .35f, gateY + .4f}, .235f, 1.97f, Tire);
    }
    b.box({623.6f, gateY - .9f, 626.4f, gateY + .9f}, .91f, 1.06f, Trim);
    for (const auto hedge : {rearHedge, frontHedge}) {
        for (int i = 0; i < 8; ++i) {
            const float x = hedge.left + 7 + (hedge.right - hedge.left - 14) * i / 7;
            const float y = (hedge.top + hedge.bottom) * .5f;
            b.round(x, y, 1.98f, 2.14f + grain(i, 7) * .011f, 7.5f, 1.3f, i % 3 ? Hedge : YoungLeaf);
        }
    }
    // Formal beds and urns belong to the forecourt, behind the traverse lines.
    for (const float x : {184.f, 521.f}) {
        b.floor({x - 54, 405, x + 54, 437}, Soil, .021f);
        for (int i = -3; i <= 3; ++i) {
            const float dx = x + float(i) * 14.7f;
            b.round(dx, 419, .025f, .76f, 7.4f, 0, i % 2 ? YoungLeaf : Hedge);
            if (i % 2) b.round(dx + 2, 425, .13f, .23f, 2.5f, 1.f, Flower);
        }
        b.round(x, 401, .04f, .62f, 5.f, 7.f, Marble);
        b.round(x, 401, .62f, .81f, 6.8f, 6.8f, Ivory);
        b.round(x, 401, .80f, 1.33f, 6.5f, 0, YoungLeaf);
    }
    foliage(b, 185, 459, 3.03f, 16, Hedge);
    foliage(b, 202, 472, 3.31f, 15, Hedge);
    foliage(b, 190, 464, 3.99f, 17, YoungLeaf);
    for (int i = 0; i < 6; ++i) {
        b.round(43, 164 + i * 4.6f, .47f, .68f, 2.5f, 0, YoungLeaf);
        b.round(41, 287 + i * 6.3f, .47f, .69f, 3.4f, 0, YoungLeaf);
    }
    b.box({shed.right, 208, shed.right + 1.5f, 237}, .04f, 1.95f, DarkWood);
    b.box({shed.right + 1.1f, 219, shed.right + 2.f, 221}, .94f, 1.06f, Brass);
}

void pendant(Builder& b, float x, float y, float drop, float radius, float strength) {
    b.box({x - .35f, y - .35f, x + .35f, y + .35f}, drop + .19f, wallHeight, Brass);
    b.round(x, y, drop, drop + .25f, 6, 3.4f, Ivory);
    b.round(x, y, drop - .045f, drop + .011f, 4.5f, 4.5f, Amber);
    b.lamp(x, y, drop - .12f, radius, strength);
}

void kitchenDetails(Builder& b) {
    for (float y : {201.f, 221.f, 241.f, 261.f}) {
        b.box({234.8f, y - 8, 235.65f, y + 8}, .12f, .79f, Sage);
        b.box({235.65f, y - 3.1f, 236.2f, y + 3.1f}, .65f, .697f, Brass);
    }
    for (float x : {196.f, 210.f, 224.f}) {
        b.box({x - 5.5f, 272.8f, x + 5.5f, 273.65f}, .13f, .79f, Sage);
        b.box({x - 2.f, 273.65f, x + 2.f, 274.2f}, .65f, .70f, Brass);
    }
    b.box({152, 115.7f, 195, 116.5f}, .11f, .87f, Steel);
    b.box({156, 116.5f, 191, 117}, .24f, .60f, Trim);
    b.box({156, 117, 191, 117.65f}, .63f, .68f, Brass);
    for (const float x : {160.f, 168.f, 176.f, 184.f}) b.box({x, 116.5f, x + 2.5f, 117.4f}, .73f, .79f, Trim);
    b.floor({134, 164, 144, 182}, Trim, .936f);
    b.box({130.4f, 171, 132, 173}, .91f, 1.15f, Steel);
    b.box({131, 171, 137, 173}, 1.12f, 1.16f, Steel);
    b.round(217, 243, .95f, 1.11f, 3, 5, Terracotta);
    b.round(215, 244, 1.08f, 1.20f, 2.35f, 1.5f, Flower);
    b.round(219, 243, 1.08f, 1.18f, 2.35f, 1.5f, YoungLeaf);
    b.floor({177, 334, 212, 352}, Linen, .777f);
    b.round(194, 343, .777f, .91f, 6.5f, 8, Ivory);
    pendant(b, 213, 228, 2.45f, 6.4f, 1.02f);
    pendant(b, 194, 343, 2.54f, 5.0f, .74f);
    pendant(b, 220, 120, 2.65f, 4.8f, .72f);
}

void garageDetails(Builder& b) {
    b.floor({520, 97, 559, 329}, RedRunner, .018f);
    for (const float y : {122.f, 237.f, 318.f}) b.pictureFacingWest(569.f, y - 10.5f, y + 10.5f, 1.36f, 2.36f);
    b.round(524, 155, .92f, 1.18f, 3.8f, 2.f, Terracotta);
    b.round(524, 297, .92f, 1.17f, 3.4f, 2.1f, Ivory);
    pendant(b, 552, 147, 2.59f, 4.9f, .87f);
    pendant(b, 552, 282, 2.53f, 4.8f, .73f);

    for (const float x : {garageBay.x, garageBay.endX}) {
        b.box({x - 1.35f, 388.45f, x + 1.35f, 395.2f}, 0, 2.72f, Ivory);
    }
    b.box({garageBay.x - 2, 388.2f, garageBay.endX + 2, 395.2f}, 2.63f, 2.76f, Ivory);
    b.box({garageBay.x + 1, 393.2f, garageBay.endX - 1, 394.3f}, 2.565f, 2.662f, Sage);
    b.box({garageBay.x + 1, 394.3f, garageBay.endX - 1, 394.75f}, 2.578f, 2.61f, Steel);
    for (int panel = 0; panel < 4; ++panel) {
        const float height = 2.79f + .126f * panel;
        b.box({garageBay.x + 1, 389.4f, garageBay.endX - 1, 390.85f}, height, height + .106f, Sage);
    }
    b.box({garageBay.x, 391, garageBay.endX, 394.7f}, .005f, .033f, Steel);
    lantern(b, 574, 395.7f, 2.08f);
    for (const float x : {627.f, 661.f}) {
        b.floor({x - 1.5f, 255, x + 1.5f, 388}, Asphalt, .006f);
    }
    b.box({676.9f, 95, 677.6f, 389}, .18f, .62f, Sage);
    b.box({676.75f, 95, 677.05f, 389}, .62f, .66f, Ivory);

    constexpr Rect bonnet{625, 220, 664, 245};
    b.box(bonnet, .887f, .919f, WagonPaint);
    b.box({622.4f, 151.5f, 624.4f, 246.9f}, .568f, .608f, Steel);
    b.box({664.6f, 151.5f, 666.6f, 246.9f}, .568f, .608f, Steel);
    b.box({622, 247.2f, 667, 250}, .347f, .438f, Steel);
    b.box({622, 148.4f, 667, 151.25f}, .347f, .438f, Steel);
    b.box({634, 247.9f, 656.5f, 248.6f}, .548f, .761f, Trim);
    for (const float h : {.60f, .68f, .738f}) b.box({635.2f, 248.6f, 655.3f, 249}, h, h + .024f, Steel);
    for (const float x : {629.f, 661.f}) {
        b.box({x - 3.8f, 247.05f, x + 3.8f, 248.45f}, .571f, .762f, Steel);
        b.box({x - 2.7f, 248.45f, x + 2.7f, 248.85f}, .609f, .728f, Ivory);
        b.box({x - 3.f, 149.4f, x + 3.f, 150.7f}, .574f, .73f, Terracotta);
    }
    b.box({639.5f, 249.55f, 651, 250.2f}, .421f, .514f, Ivory);
    for (const float axle : {169.f, 225.f}) {
        b.axle(620.5f, 621.15f, axle, .34f, .145f, Steel);
        b.axle(667.85f, 668.55f, axle, .34f, .145f, Steel);
    }
    for (const float y : {164.f, 186.f, 210.f}) {
        b.taperedBox({625.5f, y - 1.2f, 663.5f, y + 1.2f},
                     {628.5f, y - 1.2f, 660.5f, y + 1.2f}, .888f, 1.49f, WagonPaint);
    }
    b.box({623.8f, 194.8f, 624.15f, 200}, .765f, .806f, Steel);
    b.box({664.85f, 194.8f, 665.2f, 200}, .765f, .806f, Steel);

    b.box({649, 112, 677, 142}, .91f, .979f, Steel);
    b.box({674, 111.4f, 677, 142.6f}, 1.16f, 2.13f, DarkWood);
    for (const float y : {115.f, 122.f, 131.f, 137.f}) {
        b.box({672.1f, y - .6f, 674.3f, y + .6f}, 1.34f, 1.98f, Steel);
        b.box({671.75f, y - 1.22f, 673.2f, y + 1.22f}, 1.33f, 1.48f, Terracotta);
    }
    for (const float y : {286.f, 305.f, 324.f}) {
        b.box({661.65f, y - 6.2f, 676.5f, y + 6.2f}, 1.51f, 1.59f, Timber);
        b.box({662, y - 4.3f, 672, y + 3.7f}, 1.59f, 1.89f, y == 305 ? Sage : Terracotta);
        b.box({661.25f, y - 6.8f, 662.1f, y + 6.8f}, .22f, 1.46f, Sage);
        b.box({660.9f, y - 2.7f, 661.3f, y + 2.7f}, 1.18f, 1.235f, Steel);
    }
    for (const float y : {140.f, 263.f, 354.f}) {
        b.box({601.2f, y - 11, 605.8f, y + 11}, 3.14f, 3.35f, Trim);
        b.box({601.6f, y - 9, 605.4f, y + 9}, 3.09f, 3.15f, Amber);
        b.lamp(603.5f, y, 3.01f, 5.7f, .79f);
    }
}

void staircaseDetails(Builder& b) {
    constexpr float x = stairFlight.left;
    for (int i = 0; i < 6; ++i) {
        const float y = 226.f - i * 9.4f, tread = .18125f * (1 + i * 2.9f);
        b.box({x - .9f, y - .6f, x + .25f, y + .6f}, tread, tread + .75f, Timber);
    }
    b.taperedBox({x - 1.1f, 225.5f, x + .45f, 227.25f},
                 {x - 1.1f, 177.5f, x + .45f, 179.1f}, .99f, 3.60f, DarkWood);
    b.box({423, 233, 428, 238}, .004f, .052f, RedRunner);
    b.box({433, 238, 438, 245}, .004f, .063f, Sage);
    b.box({458, 234, 462, 239}, .003f, .055f, DarkWood);
    b.lamp(442, 204, 3.67f, 4.5f, .47f);
}

void urn(Builder& b, float x, float y, float base, float size) {
    b.round(x, y, base, base + .13f, size * .58f, size * .58f, Ivory);
    b.round(x, y, base + .13f, base + .57f, size * .33f, size, MonumentStone);
    b.round(x, y, base + .57f, base + .96f, size, size * .59f, MonumentStone);
    b.round(x, y, base + .96f, base + 1.04f, size * .59f, size * .86f, Brass);
}

void chandelier(Builder& b, float y) {
    constexpr float x = 368, height = 4.26f;
    b.box({x - .65f, y - .65f, x + .65f, y + .65f}, height, hallHeight, Brass);
    b.round(x, y, height - .16f, height + .14f, 4.f, 6.4f, Brass);
    b.box({x - 17, y - .9f, x + 17, y + .9f}, height, height + .095f, Brass);
    b.box({x - .9f, y - 14, x + .9f, y + 14}, height, height + .095f, Brass);
    for (const float off : {-1.f, 1.f}) {
        b.round(x + off * 17, y, height + .075f, height + .36f, 4.f, 2.1f, Amber);
        b.round(x, y + off * 14, height + .075f, height + .36f, 4.f, 2.1f, Amber);
    }
    b.lamp(x, y, height - .20f, 10.5f, 1.2f);
}

void hallDetails(Builder& b) {
    b.floor({329, 35, 408, 188}, RedRunner, .018f);
    b.floor({329, 235, 408, 388}, RedRunner, .018f);
    b.box({339, 188, 398, 235}, .015f, .17f, MonumentStone);
    b.box({340, 189, 397, 234}, 1.71f, 1.86f, Ivory);
    urn(b, 368, 211, 1.86f, 16);
    for (const float x : {273.f, 463.f}) {
        b.box({x - 1.7f, 31, x + 1.7f, 389}, 5.87f, 6.04f, Ivory);
        b.box({x - 2.1f, 31, x + 2.1f, 389}, 5.71f, 5.85f, MonumentStone);
    }
    for (const float y : {88.f, 174.f, 258.f, 336.f}) {
        b.box({271, y - 2.35f, 465, y + 2.35f}, 5.98f, hallHeight, Ivory);
    }
    for (const float x : {317.f, 368.f, 419.f}) paneledArch(b, x, 30.65f, 4.22f, 5.18f, 13);
    for (const float y : {119.f, 302.f}) chandelier(b, y);

    // Give the two covered crossovers distinct casing and a continuous threshold.
    for (const float x : {222.f, 270.f}) {
        for (const float y : {194.f, 229.f}) b.box({x - 3, y - 1.1f, x + 3, y + 1.1f}, .03f, 2.52f, Ivory);
        b.box({x - 3, 192.9f, x + 3, 230.1f}, 2.43f, 2.55f, Ivory);
    }
    for (const float x : {466.f, 518.f}) {
        for (const float y : {234.f, 269.f}) b.box({x - 3, y - 1.1f, x + 3, y + 1.1f}, .03f, 2.52f, Timber);
        b.box({x - 3, 232.9f, x + 3, 270.1f}, 2.43f, 2.55f, Timber);
    }

    for (const float y : {48.f, 377.f}) {
        for (const float x : {258.f, 478.f}) urn(b, x, y, 1.81f, 7.f);
    }
    b.box({301, 127, 313, 169}, .88f, 1.015f, RedRunner);
    b.box({423, 251, 435, 293}, .88f, 1.015f, RedRunner);

    for (const float x : {187.f, 554.f}) {
        pendant(b, x, 61, 2.74f, 5.6f, .85f);
        pendant(b, x, 358, 2.74f, 5.6f, .85f);
    }
    b.lamp(338, 187, 3.12f, 5.6f, .57f);
    b.lamp(401, 235, 3.12f, 5.6f, .57f);
}

void navigation(Assets& assets) {
    using namespace game_detail;
    const World world(assets);
    constexpr int columns = 62, rows = 49, stride = 12;
    std::array<int, columns * rows> grid;
    grid.fill(-1);
    for (int x = 0; x < columns; ++x) {
        for (int y = 0; y < rows; ++y) {
            const auto p = position(-54 + x * stride, -79 + y * stride);
            if (world.walkBlocked(p, p)) continue;
            const auto eye = atHeight(p, 1.45f);
            const bool cover = world.blocked(eye, eye + Vec3{.8f, 0, 0}) ||
                               world.blocked(eye, eye + Vec3{-.8f, 0, 0}) ||
                               world.blocked(eye, eye + Vec3{0, 0, .8f}) ||
                               world.blocked(eye, eye + Vec3{0, 0, -.8f});
            grid[x * rows + y] = int(assets.navNodes.size());
            assets.navNodes.push_back({p, 0, 0, cover, true});
        }
    }
    for (int x = 0; x < columns; ++x) {
        for (int y = 0; y < rows; ++y) {
            const int source = grid[x * rows + y];
            if (source < 0) continue;
            auto& from = assets.navNodes[source];
            from.first = uint32_t(assets.navEdges.size());
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    if ((!dx && !dy) || x + dx < 0 || x + dx >= columns || y + dy < 0 || y + dy >= rows) continue;
                    const int destination = grid[(x + dx) * rows + y + dy];
                    if (destination < 0) continue;
                    const auto to = assets.navNodes[destination].position;
                    if (world.walkBlocked(from.position, to)) continue;
                    assets.navEdges.push_back({uint32_t(destination), planar(to - from.position)});
                }
            }
            from.count = uint32_t(assets.navEdges.size()) - from.first;
        }
    }

    // Drop isolated grid samples in the tiny gaps between furniture.
    int start = 0;
    const Vec3 spawn = assets.spawns[0].position;
    for (int i = 1; i < int(assets.navNodes.size()); ++i)
        if (planarSquared(assets.navNodes[i].position - spawn) < planarSquared(assets.navNodes[start].position - spawn))
            start = i;
    std::vector<uint32_t> reachable{uint32_t(start)};
    std::vector<uint8_t> visited(assets.navNodes.size());
    visited[start] = 1;
    for (size_t i = 0; i < reachable.size(); ++i) {
        const auto& node = assets.navNodes[reachable[i]];
        for (uint32_t j = node.first; j < node.first + node.count; ++j) {
            const auto to = assets.navEdges[j].to;
            if (visited[to]) continue;
            visited[to] = 1;
            reachable.push_back(to);
        }
    }
    for (size_t i = 0; i < assets.navNodes.size(); ++i) assets.navNodes[i].walkable = visited[i];
}

template<typename T>
uint64_t allocated(const std::vector<T>& items) { return items.capacity() * sizeof(T); }

uint64_t arenaBytes(const Assets& a) {
    uint64_t bytes = allocated(a.world.vertices) + allocated(a.world.triangles) + allocated(a.world.chunks) +
                     allocated(a.shotVertices) + allocated(a.shotIndices) + allocated(a.shotSurfaces) +
                     allocated(a.shotChunks) + allocated(a.obstacles) + allocated(a.navNodes) +
                     allocated(a.navEdges) + allocated(a.lamps) + allocated(a.glassHits) +
                     allocated(a.materials) + allocated(a.textures) +
                     allocated(a.alienTrooper.vertices) + allocated(a.alienTrooper.triangles) +
                     allocated(a.alienTrooperPoses) +
                     allocated(a.alienDrone.vertices) + allocated(a.alienDrone.triangles) +
                     allocated(a.commanderUniform) + allocated(a.enforcerUniform);
    for (const auto& craft : a.alienFleet) {
        bytes += allocated(craft.vertices) + allocated(craft.triangles) + allocated(craft.chunks);
    }
    for (const auto& pose : a.alienTrooperPoses) bytes += allocated(pose.posePositions);
    bytes += allocated(a.alienEnforcer.vertices) + allocated(a.alienEnforcer.triangles);
    for (const auto& pose : a.alienEnforcerPoses) bytes += allocated(pose.posePositions);
    bytes += allocated(a.civilianMesh.vertices) + allocated(a.civilianMesh.triangles);
    bytes += allocated(a.transportHelo.vertices) + allocated(a.transportHelo.triangles) +
             allocated(a.transportRotor.vertices) + allocated(a.transportRotor.triangles) + allocated(a.smoke);
    for (const auto& pose : a.civilianPoses) bytes += allocated(pose.posePositions);
    for (const auto& texture : a.textures) bytes += allocated(texture.pixels);
    for (const auto& u : a.civilianUniforms) bytes += allocated(u);
    return bytes;
}

void roadFlare(Builder& b, float x, float y) {
    b.box({x - 2.2f, y - 0.8f, x + 2.2f, y + 0.8f}, .02f, .07f, AlienCore);
    b.round(x + 2.f, y, .05f, .16f, 1.5f, 0.f, Amber);
    b.lamp(x + 2.f, y, .26f, 4.4f, .62f);
}

struct MeshWriter {
    Mesh& mesh;
    void tri(Vec3 a, Vec3 b, Vec3 c, uint16_t mat) {
        const uint32_t i = uint32_t(mesh.vertices.size());
        mesh.vertices.push_back({a, 0, 0});
        mesh.vertices.push_back({b, 0, 0});
        mesh.vertices.push_back({c, 0, 0});
        mesh.triangles.push_back({i, i + 2, i + 1, mat, -1});
    }
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint16_t mat) {
        const uint32_t i = uint32_t(mesh.vertices.size());
        mesh.vertices.push_back({a, 0, 0});
        mesh.vertices.push_back({b, 0, 0});
        mesh.vertices.push_back({c, 0, 0});
        mesh.vertices.push_back({d, 0, 0});
        mesh.triangles.push_back({i, i + 2, i + 1, mat, -1});
        mesh.triangles.push_back({i, i + 3, i + 2, mat, -1});
    }
    void box(Vec3 lo, Vec3 hi, uint16_t mat) {
        const Vec3 p000{lo.x, lo.y, lo.z}, p100{hi.x, lo.y, lo.z};
        const Vec3 p010{lo.x, hi.y, lo.z}, p110{hi.x, hi.y, lo.z};
        const Vec3 p001{lo.x, lo.y, hi.z}, p101{hi.x, lo.y, hi.z};
        const Vec3 p011{lo.x, hi.y, hi.z}, p111{hi.x, hi.y, hi.z};
        quad(p001, p101, p111, p011, mat); // +z front
        quad(p100, p000, p010, p110, mat); // -z back
        quad(p000, p001, p011, p010, mat); // -x left
        quad(p101, p100, p110, p111, mat); // +x right
        quad(p011, p111, p110, p010, mat); // +y top
        quad(p000, p100, p101, p001, mat); // -y bottom
    }
};

void finalizeProceduralMesh(Mesh& mesh) {
    if (mesh.vertices.empty()) return;
    const auto remap = weld(mesh.vertices, [](const Vertex& v) {
        return std::array{v.position.x, v.position.y, v.position.z, v.u, v.v};
    });
    Bounds bounds{mesh.vertices.front().position, mesh.vertices.front().position};
    for (const auto& v : mesh.vertices) {
        bounds.min = {std::min(bounds.min.x, v.position.x), std::min(bounds.min.y, v.position.y), std::min(bounds.min.z, v.position.z)};
        bounds.max = {std::max(bounds.max.x, v.position.x), std::max(bounds.max.y, v.position.y), std::max(bounds.max.z, v.position.z)};
    }
    for (auto& tri : mesh.triangles) {
        tri.a = remap[tri.a];
        tri.b = remap[tri.b];
        tri.c = remap[tri.c];
    }
    mesh.chunks = {{bounds, 0, uint32_t(mesh.triangles.size())}};
    mesh.triangles.shrink_to_fit();
}

// The ground alien: a hulking, hunched predator with a split fanged maw, a long tongue,
// bone claws and tendrils whipping off its back. Built facing +z with its feet at y = 0.
struct CreaturePose {
    float phase = 0;   // Lope cycle 0..1
    bool stride = false;
    float breathe = 0; // Idle chest swell 0..1
    float jaw = 1;     // Mouth opening 0..1
    float writhe = 0;  // Tendril and tongue wave, radians
};

class CreatureWriter {
public:
    explicit CreatureWriter(std::vector<Vertex>& vertices, std::vector<Triangle>* triangles)
        : vertices_(vertices), triangles_(triangles) {}
    uint16_t part = 0; // Stored in u while welding so separate parts never merge.
    uint8_t group = 0; // Stored in v: which rigid piece a vertex belongs to

    // Vertices go out in call order for every pose. Winding is chosen to face away from
    // `inside`, and only the base pose's triangles are kept.
    void tri(Vec3 a, Vec3 b, Vec3 c, Vec3 inside, uint16_t mat) {
        const uint32_t i = push(a), j = push(b), k = push(c);
        if (!triangles_) return;
        const bool out = dot(cross(b - a, c - a), (a + b + c) * (1.f / 3) - inside) >= 0;
        triangles_->push_back(out ? Triangle{i, k, j, mat, -1} : Triangle{i, j, k, mat, -1});
    }
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 inside, uint16_t mat) {
        const uint32_t i = push(a), j = push(b), k = push(c), l = push(d);
        if (!triangles_) return;
        const bool out = dot(cross(b - a, c - a), (a + b + c + d) * .25f - inside) >= 0;
        if (out) {
            triangles_->push_back({i, k, j, mat, -1});
            triangles_->push_back({i, l, k, mat, -1});
        } else {
            triangles_->push_back({i, j, k, mat, -1});
            triangles_->push_back({i, k, l, mat, -1});
        }
    }
    // Loft rings through the joints. A zero radius on the last joint closes it to a point.
    void tube(const std::vector<Vec3>& p, const std::vector<float>& ru, const std::vector<float>& rv, int sides,
              Vec3 ref, uint16_t mat, bool capStart = true) {
        ++part;
        const size_t n = p.size();
        std::vector<std::vector<Vec3>> rings(n);
        for (size_t i = 0; i < n; ++i) {
            const Vec3 t = normalized(p[std::min(i + 1, n - 1)] - p[i > 0 ? i - 1 : 0]);
            Vec3 u = ref - t * dot(ref, t);
            if (length(u) < 1e-3f) u = std::abs(t.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            u = normalized(u);
            const Vec3 v = cross(t, u);
            for (int k = 0; k < sides; ++k) {
                const float a = 2 * pi * k / sides;
                rings[i].push_back(p[i] + u * (std::cos(a) * ru[i]) + v * (std::sin(a) * rv[i]));
            }
        }
        const bool tip = ru[n - 1] <= 0 && rv[n - 1] <= 0;
        for (size_t i = 0; i + 1 < n; ++i) {
            const Vec3 inside = (p[i] + p[i + 1]) * .5f;
            for (int k = 0; k < sides; ++k) {
                const int k1 = (k + 1) % sides;
                if (tip && i + 2 == n) tri(rings[i][k], rings[i][k1], p[n - 1], inside, mat);
                else quad(rings[i][k], rings[i][k1], rings[i + 1][k1], rings[i + 1][k], inside, mat);
            }
        }
        if (capStart) {
            for (int k = 0; k < sides; ++k) tri(p[0], rings[0][(k + 1) % sides], rings[0][k], p[1], mat);
        }
    }
    // A slim cone, for teeth and claws.
    void spike(Vec3 base, Vec3 tip, Vec3 across, float width, uint16_t mat) {
        ++part;
        const Vec3 axis = normalized(tip - base);
        const Vec3 side = normalized(across - axis * dot(across, axis));
        const Vec3 depth = cross(axis, side);
        const Vec3 a = base + side * width, b = base - side * width, c = base + depth * (width * .8f);
        const Vec3 inside = base + axis * (length(tip - base) * .3f);
        tri(a, b, tip, inside, mat);
        tri(b, c, tip, inside, mat);
        tri(c, a, tip, inside, mat);
    }

private:
    uint32_t push(Vec3 at) {
        vertices_.push_back({at, float(part), float(group)});
        return uint32_t(vertices_.size() - 1);
    }
    std::vector<Vertex>& vertices_;
    std::vector<Triangle>* triangles_;
};

Vec3 pitchAbout(Vec3 point, Vec3 pivot, float angle) {
    // Positive angles tip +z upward.
    const Vec3 d = point - pivot;
    const float c = std::cos(angle), s = std::sin(angle);
    return pivot + Vec3{d.x, d.y * c + d.z * s, -d.y * s + d.z * c};
}

void writeCreature(CreatureWriter& w, const CreaturePose& q, uint16_t skin, uint16_t sinew, uint16_t tooth,
                   uint16_t maw, uint16_t tongue, uint16_t eye, bool armored = false) {
    const float cycle = 2 * pi * q.phase;
    const Vec3 up{0, 1, 0}, side{1, 0, 0}, ahead{0, 0, 1};

    // Legs first, so the lowest foot can be planted on the ground. Idle is a wide, bent crouch.
    struct Leg { Vec3 hip, knee, ankle; };
    Leg legs[2];
    float lowest = 10;
    for (int l = 0; l < 2; ++l) {
        const float s = l == 0 ? -1.f : 1.f;
        const float swing = q.stride ? std::sin(cycle) * (l == 0 ? 1.f : -1.f) : 0.f;
        const float lift = q.stride ? std::max(0.f, std::cos(cycle) * (l == 0 ? 1.f : -1.f)) : 0.f;
        const float thigh = q.stride ? swing * .55f + .1f : (l == 0 ? .28f : .05f);
        const float bend = q.stride ? .45f + lift * .75f : .7f;
        const Vec3 hip{s * .2f, 0, -.02f};
        const Vec3 knee = hip + Vec3{s * (q.stride ? .12f : .28f), -std::cos(thigh), std::sin(thigh)} * .5f;
        const Vec3 ankle = knee + Vec3{s * (q.stride ? 0.f : -.06f), -std::cos(thigh - bend), std::sin(thigh - bend)} * .5f;
        legs[l] = {hip, knee, ankle};
        lowest = std::min(lowest, ankle.y);
    }
    const float hipY = .1f - lowest;
    for (auto& leg : legs) {
        leg.hip.y += hipY;
        leg.knee.y += hipY;
        leg.ankle.y += hipY;
    }
    const float puff = 1.f + .05f * q.breathe;

    // Torso: tight waist flaring into a massive, forward-hunched chest and back.
    w.tube({{0, hipY - .08f, -.02f}, {0, hipY + .14f, 0}, {0, hipY + .36f, .07f}, {0, hipY + .56f, .15f},
            {0, hipY + .72f, .14f}, {0, hipY + .8f, .25f}},
           {.23f, .21f, .33f * puff, .46f * puff, .4f, .16f}, {.17f, .16f, .23f, .31f * puff, .28f, .15f}, 8, side, skin);
    // Slabs of pectoral muscle, and dark sinew cords wrapping the torso.
    for (int a = 0; a < 2; ++a) {
        const float s = a == 0 ? -1.f : 1.f;
        w.tube({{s * .05f, hipY + .62f, .36f * puff}, {s * .17f, hipY + .58f, .38f * puff}, {s * .32f, hipY + .6f, .3f}},
               {.09f, .1f, .07f}, {.05f, .06f, .045f}, 6, up, skin);
        w.tube({{s * .36f, hipY + .7f, .2f}, {s * .2f, hipY + .44f, .32f}, {s * .1f, hipY + .2f, .2f}, {s * .02f, hipY + .02f, .17f}},
               {.028f, .03f, .026f, .02f}, {.028f, .03f, .026f, .02f}, 4, ahead, sinew);
        w.tube({{s * .42f, hipY + .6f, -.1f}, {s * .3f, hipY + .36f, -.2f}, {s * .16f, hipY + .14f, -.16f}},
               {.03f, .03f, .022f}, {.03f, .03f, .022f}, 4, ahead, sinew);
    }
    // Hulking shoulder masses.
    for (int a = 0; a < 2; ++a) {
        const float s = a == 0 ? -1.f : 1.f;
        w.tube({{s * .2f, hipY + .7f, .1f}, {s * .38f, hipY + .72f, .1f}, {s * .54f, hipY + .62f, .1f}},
               {.16f, .17f, .12f}, {.15f, .16f, .12f}, 6, up, skin);
    }

    for (int l = 0; l < 2; ++l) {
        const auto& leg = legs[l];
        const float s = l == 0 ? -1.f : 1.f;
        w.tube({leg.hip + Vec3{0, .07f, 0}, leg.hip, (leg.hip + leg.knee) * .5f, leg.knee,
                (leg.knee + leg.ankle) * .5f, leg.ankle},
               {.17f, .18f, .17f, .11f, .1f, .075f}, {.17f, .19f, .18f, .12f, .11f, .08f}, 6, ahead, skin);
        // Broad clawed foot.
        const Vec3 heel = leg.ankle + Vec3{0, -.05f, -.07f};
        const Vec3 ball{leg.ankle.x + s * .03f, .05f, leg.ankle.z + .2f};
        w.tube({heel, leg.ankle + Vec3{0, -.04f, .06f}, ball, ball + Vec3{0, -.02f, .1f}},
               {.06f, .075f, .05f, 0}, {.08f, .11f, .09f, 0}, 5, up, sinew);
        for (int c = 0; c < 2; ++c) {
            const Vec3 root = ball + Vec3{s * (c ? .05f : -.03f), 0, .04f};
            w.spike(root, root + Vec3{0, -.04f, .1f}, side, .015f, tooth);
        }
    }

    // Long, heavy arms reach wide and forward, swinging against the stride.
    const Vec3 chest{0, hipY + .56f, .15f};
    for (int a = 0; a < 2; ++a) {
        const float s = a == 0 ? -1.f : 1.f;
        const float swing = q.stride ? -std::sin(cycle) * (a == 0 ? 1.f : -1.f) : std::sin(q.writhe * .5f + a) * .15f;
        const Vec3 shoulder{s * .52f, hipY + .64f, .1f};
        const Vec3 elbow = shoulder + Vec3{s * .22f, -.3f, .06f + .14f * swing};
        const Vec3 wrist = elbow + Vec3{s * .1f, -.1f + .06f * q.breathe, .32f + .1f * swing};
        w.tube({shoulder, shoulder + (elbow - shoulder) * .45f, elbow, elbow + (wrist - elbow) * .4f, wrist},
               {.14f, .14f, .1f, .11f, .07f}, {.13f, .13f, .095f, .1f, .065f}, 6, ahead, skin);
        const Vec3 dir = normalized(wrist - elbow);
        const Vec3 knuckle = wrist + dir * .12f;
        const Vec3 across = normalized(cross(dir, up)) * s;
        w.tube({wrist, knuckle}, {.075f, .07f}, {.04f, .04f}, 5, across, sinew);
        for (int c = 0; c < 4; ++c) {
            const float spread = (c - 1.5f) * .04f;
            const Vec3 root = knuckle + across * spread;
            const Vec3 curl = dir * .12f + Vec3{0, -.06f - .015f * std::abs(c - 1.5f), 0} + across * (spread * .7f);
            w.spike(root, root + curl + dir * .08f + Vec3{0, -.06f, 0}, across, .02f, tooth);
        }
    }

    // Head: cranium and upper jaw tip up, lower jaw drops, leaving a wide fanged maw.
    const Vec3 hinge{0, hipY + .88f, .38f};
    const float upper = .16f + .18f * q.jaw, lower = .34f + .42f * q.jaw;
    const auto upperAt = [&](Vec3 offset) { return pitchAbout(hinge + offset, hinge, upper); };
    const auto lowerAt = [&](Vec3 offset) { return pitchAbout(hinge + offset, hinge, -lower); };
    const auto upperDir = [&](Vec3 d) { return pitchAbout(hinge + d, hinge, upper) - hinge; };
    w.tube({hinge + Vec3{0, .06f, -.16f}, hinge + Vec3{0, .09f, -.02f}, upperAt({0, .05f, .14f}),
            upperAt({0, 0, .28f}), upperAt({0, -.02f, .35f})},
           {.12f, .15f, .11f, .065f, 0}, {.14f, .2f, .21f, .17f, 0}, 7, up, skin);
    w.tube({hinge + Vec3{0, -.08f, -.02f}, lowerAt({0, -.1f, .15f}), lowerAt({0, -.11f, .27f}), lowerAt({0, -.11f, .33f})},
           {.06f, .05f, .035f, 0}, {.17f, .17f, .12f, 0}, 6, up, skin);

    // Grin arcs that carry the teeth, and the dark throat between them.
    constexpr int arc = 9;
    std::array<Vec3, arc> top{}, bottom{};
    for (int i = 0; i < arc; ++i) {
        const float a = -1.35f + 2.7f * i / (arc - 1);
        top[i] = upperAt({.19f * std::sin(a), -.065f, .06f + .24f * std::cos(a)});
        bottom[i] = lowerAt({.18f * std::sin(a), -.09f, .04f + .23f * std::cos(a)});
    }
    ++w.part;
    const Vec3 throat = hinge + Vec3{0, -.06f, -.05f};
    for (int i = 0; i + 1 < arc; ++i) {
        w.tri(throat, top[i], top[i + 1], hinge + Vec3{0, .25f, 0}, maw);
        w.tri(throat, bottom[i + 1], bottom[i], hinge + Vec3{0, -.25f, 0}, maw);
    }
    for (int i : {0, arc - 2}) w.quad(top[i], top[i + 1], bottom[i + 1], bottom[i], throat, maw);
    for (int i = 1; i + 1 < arc; ++i) {
        const Vec3 along = top[i + 1] - top[i - 1];
        const float centre = 1.f - std::abs(i - (arc - 1) * .5f) / ((arc - 1) * .5f);
        const float fang = (i % 2 ? .12f : .085f) * (.6f + .5f * centre);
        w.spike(top[i], top[i] + Vec3{0, -fang, .012f}, along, .024f, tooth);
        w.spike(bottom[i], bottom[i] + Vec3{0, fang * .8f, .012f}, along, .021f, tooth);
    }

    // Four glowing pit-eyes on the front of the snout, just above the upper fangs.
    const struct { float x, y, z, size; } eyeSpots[]{{.058f, .034f, .305f, .022f}, {.108f, .014f, .298f, .019f}};
    for (int e = 0; e < 4; ++e) {
        const float s = e % 2 ? 1.f : -1.f;
        const auto& spot = eyeSpots[e / 2];
        const Vec3 base = upperAt({s * spot.x, spot.y, spot.z});
        const Vec3 aim = upperDir(normalized(Vec3{s * .3f, .05f, 1})) * (spot.size * 1.6f);
        w.spike(base, base + aim, side, spot.size, eye);
    }

    // Long tongue lolling out over the chin.
    std::vector<Vec3> lick{hinge + Vec3{0, -.07f, .03f}};
    const Vec3 steps[]{{0, -.04f, .19f}, {0, -.12f, .13f}, {0, -.12f, .04f}, {0, -.1f, -.03f}};
    for (int k = 0; k < 4; ++k) {
        const float sway = std::sin(q.writhe + k * .9f) * .025f * (k + 1);
        lick.push_back(lick.back() + steps[k] + Vec3{sway, 0, 0});
    }
    w.tube(lick, {.028f, .03f, .026f, .018f, 0}, {.055f, .06f, .052f, .034f, 0}, 6, up, tongue);

    // Thick tendrils whipping up and out of the back and shoulders.
    const struct { Vec3 root, dir, curl; float length; } tendrils[]{
        {{-.14f, .8f, .04f}, {-.35f, 1, -.25f}, {-1, -.35f, 0}, .9f},
        {{.14f, .8f, .04f}, {.35f, 1, -.25f}, {1, -.35f, 0}, .9f},
        {{-.44f, .76f, .04f}, {-1, .75f, -.15f}, {0, 1, 0}, .78f},
        {{.44f, .76f, .04f}, {1, .75f, -.15f}, {0, 1, 0}, .78f},
        {{0, .74f, -.12f}, {.15f, .85f, -1}, {0, 1, 0}, .8f},
        {{-.24f, .52f, -.15f}, {-.6f, .25f, -1}, {-.5f, .8f, 0}, .6f},
    };
    for (int t = 0; t < 6; ++t) {
        const auto& spec = tendrils[t];
        std::vector<Vec3> joints{spec.root + Vec3{0, hipY, 0}};
        Vec3 dir = normalized(spec.dir);
        for (int k = 1; k <= 4; ++k) {
            const float wave = std::sin(q.writhe + t * 1.3f + k * .8f) * .35f;
            dir = normalized(dir + spec.curl * .3f + cross(dir, up) * wave);
            joints.push_back(joints.back() + dir * (spec.length * .25f));
        }
        w.tube(joints, {.08f, .058f, .04f, .022f, 0}, {.07f, .052f, .036f, .02f, 0}, 5, chest - joints[0], sinew);
    }

    if (!armored) return;
    // Enforcers: a bone crest swept back off the skull, spikes off the shoulders and spine,
    // and blades along the forearms.
    for (int h = 0; h < 3; ++h) {
        const float x = float(h - 1) * .075f;
        const Vec3 root = hinge + Vec3{x, .21f - std::abs(x) * .6f, -.05f};
        w.spike(root, root + Vec3{x * 1.6f, .24f - std::abs(x), -.36f}, side, .05f, tooth);
    }
    for (int a = 0; a < 2; ++a) {
        const float s = a == 0 ? -1.f : 1.f;
        for (int k = 0; k < 2; ++k) {
            const Vec3 root{s * (.32f + .12f * float(k)), hipY + .84f - .05f * float(k), .08f - .08f * float(k)};
            w.spike(root, root + Vec3{s * (.16f + .06f * float(k)), .3f - .08f * float(k), -.12f}, ahead, .055f, tooth);
        }
        const float swing = q.stride ? -std::sin(cycle) * (a == 0 ? 1.f : -1.f) : std::sin(q.writhe * .5f + a) * .15f;
        const Vec3 shoulder{s * .52f, hipY + .64f, .1f};
        const Vec3 elbow = shoulder + Vec3{s * .22f, -.3f, .06f + .14f * swing};
        const Vec3 wrist = elbow + Vec3{s * .1f, -.1f + .06f * q.breathe, .32f + .1f * swing};
        const Vec3 mid = elbow + (wrist - elbow) * .35f + Vec3{s * .06f, .04f, 0};
        w.spike(mid, mid + (elbow - wrist) * .55f + Vec3{s * .14f, .1f, 0}, up, .045f, tooth);
    }
    for (int k = 0; k < 4; ++k) {
        const Vec3 root{0, hipY + .3f + .14f * float(k), -.18f - .02f * float(k)};
        w.spike(root, root + Vec3{0, .1f, -.2f - .03f * float(k)}, side, .045f, tooth);
    }
}

// Build the base mesh from the first pose and every pose as a position-only frame on its
// topology. Writers emit vertices in the same order for every pose.
template <typename Pose, typename Write>
void buildPosedMesh(Mesh& base, std::vector<Mesh>& frames, const std::vector<Pose>& poses, Write write) {
    base = {};
    write(poses[0], base.vertices, &base.triangles);
    const size_t raw = base.vertices.size();
    const auto remap = weld(base.vertices, [](const Vertex& v) {
        return std::array{v.position.x, v.position.y, v.position.z, v.u, 0.f};
    });
    for (auto& v : base.vertices) v.u = v.v = 0;
    Bounds bounds{base.vertices.front().position, base.vertices.front().position};
    for (const auto& v : base.vertices) {
        bounds.min = {std::min(bounds.min.x, v.position.x), std::min(bounds.min.y, v.position.y), std::min(bounds.min.z, v.position.z)};
        bounds.max = {std::max(bounds.max.x, v.position.x), std::max(bounds.max.y, v.position.y), std::max(bounds.max.z, v.position.z)};
    }
    for (auto& tri : base.triangles) {
        tri.a = remap[tri.a];
        tri.b = remap[tri.b];
        tri.c = remap[tri.c];
    }
    base.chunks = {{bounds, 0, uint32_t(base.triangles.size())}};
    base.triangles.shrink_to_fit();

    frames.assign(poses.size(), Mesh{});
    std::vector<Vertex> scratch;
    for (size_t p = 0; p < poses.size(); ++p) {
        scratch.clear();
        write(poses[p], scratch, nullptr);
        auto& positions = frames[p].posePositions;
        positions.assign(base.vertices.size(), Vec3{});
        for (size_t i = 0; i < raw && i < scratch.size(); ++i) positions[remap[i]] = scratch[i].position;
        positions.shrink_to_fit();
    }
}

void buildCreature(Assets& assets, uint16_t firstMat) {
    const auto mat = [&](Surface s) { return uint16_t(firstMat + s); };
    std::vector<CreaturePose> poses;
    for (int i = 0; i < alienIdlePoses; ++i) poses.push_back({0, false, float(i), .75f + .25f * i, 1.7f * i});
    for (int i = 0; i < alienStridePoses; ++i) {
        const float phase = float(i) / alienStridePoses;
        poses.push_back({phase, true, 0, .85f + .15f * std::sin(4 * pi * phase), 2 * pi * phase});
    }
    for (const bool armored : {false, true}) {
        buildPosedMesh(armored ? assets.alienEnforcer : assets.alienTrooper,
                       armored ? assets.alienEnforcerPoses : assets.alienTrooperPoses, poses,
                       [&](const CreaturePose& pose, std::vector<Vertex>& vertices, std::vector<Triangle>* triangles) {
            CreatureWriter w(vertices, triangles);
            writeCreature(w, pose, mat(AlienSkin), mat(AlienSinew), mat(AlienTooth), mat(AlienMaw), mat(AlienTongue),
                          mat(AlienGlow), armored);
        });
    }
}

// Unarmed civilians in everyday clothes: cowering with hands over the head while pinned,
// running for the back door once rescued, and standing hugging themselves when safe.
enum class CivilianMode { Cower, Run, Stand };
struct CivilianPose {
    CivilianMode mode = CivilianMode::Cower;
    float phase = 0;   // Run cycle 0..1
    float shiver = 0;  // Cower tremble 0..1
};

void writeCivilian(CreatureWriter& w, const CivilianPose& q, uint16_t skin, uint16_t hair, uint16_t top, uint16_t pants,
                   uint16_t shoes) {
    const Vec3 up{0, 1, 0}, side{1, 0, 0}, ahead{0, 0, 1};
    struct { Vec3 hip, knee, ankle, toe, shoulder, elbow, wrist, hand; } limb[2];
    Vec3 pelvis, waist, chest, shoulders, neck, head, headUp;
    if (q.mode == CivilianMode::Cower) {
        const float t = q.shiver * .012f;
        for (int l = 0; l < 2; ++l) {
            const float s = l == 0 ? -1.f : 1.f;
            limb[l].ankle = {s * .15f, .08f, -.05f};
            limb[l].toe = {s * .16f, .03f, .14f};
            limb[l].knee = {s * .18f, .5f, .2f};
            limb[l].hip = {s * .11f, .3f, -.15f};
            limb[l].shoulder = {s * .2f, .78f + t, .08f};
            limb[l].elbow = {s * .25f, .92f + t, .27f};
            limb[l].wrist = {s * .1f, 1.03f + t, .22f};
            limb[l].hand = {s * .04f, 1.05f + t, .17f};
        }
        // Hips sunk toward the heels, back hunched over the knees, head tucked down.
        pelvis = {0, .3f, -.16f};
        waist = {0, .45f, -.1f};
        chest = {0, .65f, .02f + t};
        shoulders = {0, .78f + t, .08f};
        neck = {0, .84f + t, .14f};
        head = {0, .92f + t, .22f};
        headUp = normalized(Vec3{0, 1, .9f});
    } else {
        const bool run = q.mode == CivilianMode::Run;
        const float cycle = 2 * pi * q.phase;
        float lowest = 10;
        for (int l = 0; l < 2; ++l) {
            const float s = l == 0 ? -1.f : 1.f;
            const float swing = run ? std::sin(cycle) * (l == 0 ? 1.f : -1.f) : 0.f;
            const float lift = run ? std::max(0.f, std::cos(cycle) * (l == 0 ? 1.f : -1.f)) : 0.f;
            const float thigh = run ? swing * .6f + .15f : .02f;
            const float bend = run ? .3f + lift * 1.1f : .05f;
            limb[l].hip = {s * .1f, 0, 0};
            limb[l].knee = limb[l].hip + Vec3{s * .01f, -std::cos(thigh), std::sin(thigh)} * .42f;
            limb[l].ankle = limb[l].knee + Vec3{0, -std::cos(thigh - bend), std::sin(thigh - bend)} * .42f;
            lowest = std::min(lowest, limb[l].ankle.y);
        }
        const float hipY = .08f - lowest;
        const float lean = run ? .1f : 0.f;
        for (int l = 0; l < 2; ++l) {
            const float s = l == 0 ? -1.f : 1.f;
            auto& leg = limb[l];
            leg.hip.y += hipY;
            leg.knee.y += hipY;
            leg.ankle.y += hipY;
            leg.toe = {leg.ankle.x, std::max(.03f, leg.ankle.y - .05f), leg.ankle.z + .17f};
            leg.shoulder = {s * .21f, hipY + .56f, .02f + lean};
            if (run) {
                const float swing = -std::sin(cycle) * (l == 0 ? 1.f : -1.f);
                leg.elbow = leg.shoulder + Vec3{s * .03f, -.24f, -.12f * swing};
                leg.wrist = leg.elbow + Vec3{-s * .02f, .04f, .2f - .08f * swing};
            } else {
                // Arms wrapped around the chest.
                leg.elbow = leg.shoulder + Vec3{s * .02f, -.24f, .08f};
                leg.wrist = {-s * .07f, hipY + .44f, .17f};
            }
            leg.hand = leg.wrist + normalized(leg.wrist - leg.elbow) * .08f;
        }
        pelvis = {0, hipY - .02f, 0};
        waist = {0, hipY + .18f, .01f + lean * .3f};
        chest = {0, hipY + .42f, .03f + lean * .7f};
        shoulders = {0, hipY + .56f, .02f + lean};
        neck = {0, hipY + .64f, .03f + lean};
        head = {0, hipY + .76f, .04f + lean * 1.1f};
        headUp = up;
    }

    // Torso in the top colour, hips in the trouser colour.
    w.tube({pelvis + Vec3{0, -.04f, 0}, pelvis, waist}, {.16f, .17f, .15f}, {.1f, .11f, .1f}, 8, side, pants);
    w.tube({waist, chest, shoulders, neck}, {.155f, .19f, .2f, .07f}, {.1f, .12f, .11f, .065f}, 8, side, top);
    w.tube({neck, neck + (head - neck) * .5f}, {.05f, .05f}, {.05f, .05f}, 6, side, skin);
    // Head with a hair cap set back from the face.
    const Vec3 face = normalized(cross(side, headUp));
    w.tube({head - headUp * .11f, head - headUp * .04f, head + headUp * .05f, head + headUp * .11f},
           {.065f, .095f, .09f, 0}, {.06f, .085f, .085f, 0}, 7, face, skin);
    w.tube({head + headUp * .01f - face * .025f, head + headUp * .07f - face * .02f, head + headUp * .13f - face * .015f},
           {.1f, .088f, 0}, {.092f, .078f, 0}, 7, face, hair, false);
    for (int l = 0; l < 2; ++l) {
        const auto& b = limb[l];
        w.tube({b.hip + Vec3{0, .04f, 0}, b.hip, b.knee, b.ankle}, {.08f, .085f, .06f, .045f}, {.085f, .09f, .065f, .05f},
               6, ahead, pants);
        w.tube({b.ankle + Vec3{0, -.02f, -.05f}, b.ankle + Vec3{0, -.03f, .03f}, b.toe}, {.045f, .05f, .035f},
               {.05f, .055f, .045f}, 5, up, shoes);
        w.tube({b.shoulder, b.elbow, b.wrist}, {.06f, .05f, .04f}, {.06f, .05f, .04f}, 6, ahead, top);
        w.tube({b.wrist, b.hand}, {.035f, .03f}, {.022f, .02f}, 5, up, skin);
    }
}

void buildCivilian(Assets& assets, uint16_t firstMat) {
    const auto mat = [&](Surface s) { return uint16_t(firstMat + s); };
    std::vector<CivilianPose> poses;
    for (int i = 0; i < civilianCowerPoses; ++i) poses.push_back({CivilianMode::Cower, 0, float(i)});
    for (int i = 0; i < civilianRunPoses; ++i) poses.push_back({CivilianMode::Run, float(i) / civilianRunPoses, 0});
    poses.push_back({CivilianMode::Stand, 0, 0});
    buildPosedMesh(assets.civilianMesh, assets.civilianPoses, poses,
                   [&](const CivilianPose& pose, std::vector<Vertex>& vertices, std::vector<Triangle>* triangles) {
        CreatureWriter w(vertices, triangles);
        writeCivilian(w, pose, mat(CivSkin), mat(CivHair), mat(CivTop), mat(CivPants), mat(CivShoes));
    });
}

// A utility helicopter in two pieces: the airframe (groups 0 and 1, cabin and tail) and the
// main rotor (group 2, centred on the mast head at y = 0). Built facing +z, wheels at y = 0.
struct HeloMats { uint16_t olive, dark, glass, mark, cross; };

void writeTransport(CreatureWriter& w, const HeloMats& m, bool body, bool rotor, float bladeLength = 7.3f) {
    const Vec3 up{0, 1, 0}, ahead{0, 0, 1};
    if (body) {
        w.group = 0;
        // Fuselage: rounded nose, deep cabin, tapering into the boom.
        w.tube({{0, 1.25f, 3.5f}, {0, 1.35f, 3.1f}, {0, 1.5f, 2.3f}, {0, 1.55f, 1.2f}, {0, 1.55f, -1.3f}, {0, 1.65f, -2.4f}},
               {.15f, .55f, .8f, .95f, .95f, .55f}, {.2f, .6f, .85f, 1.f, 1.f, .5f}, 8, up, m.olive);
        // Engines above the cabin, then the mast.
        w.tube({{0, 2.42f, 1.05f}, {0, 2.55f, .45f}, {0, 2.55f, -1.4f}, {0, 2.4f, -2.05f}}, {.1f, .36f, .36f, .14f},
               {.28f, .6f, .6f, .28f}, 6, up, m.olive);
        w.tube({{0, 2.8f, .15f}, {0, 3.3f, .15f}}, {.12f, .1f}, {.12f, .1f}, 6, ahead, m.dark);
        // Glazing: windscreen, cockpit sides and cabin windows.
        ++w.part;
        const Vec3 inside{0, 1.5f, 1.f};
        w.quad({-.5f, 1.95f, 3.f}, {.5f, 1.95f, 3.f}, {.62f, 2.34f, 2.25f}, {-.62f, 2.34f, 2.25f}, inside, m.glass);
        for (int s = -1; s <= 1; s += 2) {
            const float x = float(s);
            w.quad({x * .87f, 1.6f, 2.8f}, {x * .87f, 2.1f, 2.7f}, {x * .9f, 2.15f, 2.f}, {x * .9f, 1.6f, 2.f}, inside, m.glass);
            w.quad({x * 1.02f, 1.75f, .95f}, {x * 1.02f, 2.12f, .95f}, {x * 1.02f, 2.12f, -.15f}, {x * 1.02f, 1.75f, -.15f},
                   inside, m.glass);
            // Medevac marking on each cabin door: a red cross on a white square.
            w.quad({x * 1.015f, 1.05f, -.35f}, {x * 1.015f, 1.62f, -.35f}, {x * 1.015f, 1.62f, -.95f}, {x * 1.015f, 1.05f, -.95f},
                   inside, m.mark);
            w.quad({x * 1.025f, 1.27f, -.42f}, {x * 1.025f, 1.4f, -.42f}, {x * 1.025f, 1.4f, -.88f}, {x * 1.025f, 1.27f, -.88f},
                   inside, m.cross);
            w.quad({x * 1.025f, 1.1f, -.58f}, {x * 1.025f, 1.57f, -.58f}, {x * 1.025f, 1.57f, -.72f}, {x * 1.025f, 1.1f, -.72f},
                   inside, m.cross);
            // Main gear: strut and wheel.
            w.tube({{x * .7f, 1.f, .9f}, {x * 1.12f, .36f, .9f}}, {.06f, .05f}, {.06f, .05f}, 4, ahead, m.dark);
            w.tube({{x * 1.f, .28f, .9f}, {x * 1.04f, .28f, .9f}, {x * 1.24f, .28f, .9f}, {x * 1.28f, .28f, .9f}, {x * 1.29f, .28f, .9f}},
                   {.16f, .28f, .28f, .16f, 0}, {.16f, .28f, .28f, .16f, 0}, 8, up, m.dark);
        }
        w.group = 1;
        // Tail boom, fin, stabilator and tail rotor.
        w.tube({{0, 1.65f, -2.4f}, {0, 1.85f, -5.f}, {0, 2.f, -7.3f}}, {.42f, .3f, .22f}, {.36f, .26f, .2f}, 6, up, m.olive);
        w.tube({{0, 1.95f, -7.f}, {0, 3.4f, -7.75f}}, {.55f, .32f}, {.07f, .05f}, 4, ahead, m.olive);
        w.tube({{-1.15f, 2.f, -6.8f}, {1.15f, 2.f, -6.8f}}, {.34f, .34f}, {.04f, .04f}, 4, ahead, m.olive);
        w.tube({{.1f, 3.f, -7.6f}, {.16f, 3.f, -7.6f}}, {.95f, .95f}, {.95f, .95f}, 10, up, m.dark);
        w.tube({{0, .55f, -6.6f}, {0, .18f, -6.65f}}, {.05f, .05f}, {.05f, .05f}, 4, ahead, m.dark);
    }
    if (rotor) {
        w.group = 2;
        w.tube({{0, -.08f, 0}, {0, .12f, 0}}, {.24f, .18f}, {.24f, .18f}, 6, ahead, m.dark);
        for (int blade = 0; blade < 4; ++blade) {
            const float a = float(blade) * pi / 2;
            const Vec3 dir{std::sin(a), 0, std::cos(a)};
            const float l = bladeLength * (blade % 2 ? 1.f : .8f);
            w.tube({dir * .22f, dir * (l * .5f) + Vec3{0, -.05f, 0}, dir * (l - .1f) + Vec3{0, -.15f, 0}, dir * l + Vec3{0, -.16f, 0}},
                   {.04f, .035f, .03f, 0}, {.25f, .25f, .21f, 0}, 4, up, m.dark);
        }
    }
}

void buildTransport(Assets& assets, uint16_t firstMat) {
    const auto mat = [&](Surface s) { return uint16_t(firstMat + s); };
    const HeloMats m{mat(HeloOlive), mat(HeloDark), mat(CarGlass), mat(HeloMark), mat(HeloCross)};
    {
        assets.transportHelo = {};
        CreatureWriter w(assets.transportHelo.vertices, &assets.transportHelo.triangles);
        writeTransport(w, m, true, false);
        finalizeProceduralMesh(assets.transportHelo);
    }
    {
        assets.transportRotor = {};
        CreatureWriter w(assets.transportRotor.vertices, &assets.transportRotor.triangles);
        writeTransport(w, m, false, true);
        finalizeProceduralMesh(assets.transportRotor);
    }
    for (auto* mesh : {&assets.transportHelo, &assets.transportRotor}) {
        for (auto& v : mesh->vertices) v.u = v.v = 0;
    }
}

Vec3 turnX(Vec3 p, float a) { const float c = std::cos(a), s = std::sin(a); return {p.x, p.y * c - p.z * s, p.y * s + p.z * c}; }
Vec3 turnY(Vec3 p, float a) { const float c = std::cos(a), s = std::sin(a); return {p.x * c + p.z * s, p.y, -p.x * s + p.z * c}; }
Vec3 turnZ(Vec3 p, float a) { const float c = std::cos(a), s = std::sin(a); return {p.x * c - p.y * s, p.x * s + p.y * c, p.z}; }

// Licking flames: a few tapering, twisted columns of hot and hotter light.
void flames(Builder& b, Vec3 at, float height, float spread, int tongues) {
    for (int i = 0; i < tongues; ++i) {
        const float a = float(i) * 2.399f;
        const Vec3 base = at + Vec3{std::cos(a), 0, std::sin(a)} * (spread * (.3f + .7f * float(i % 3) / 2.f));
        const float h = height * (.55f + .45f * float((i * 7) % 5) / 4.f), r = .18f + .1f * float(i % 2);
        const auto ring = [&](float y, float radius, float twist) {
            std::array<Vec3, 4> c{};
            for (int k = 0; k < 4; ++k) {
                const float t = twist + float(k) * pi / 2;
                c[size_t(k)] = base + Vec3{std::cos(t) * radius, y, std::sin(t) * radius};
            }
            return c;
        };
        b.box(ring(0, r, a), ring(h * .55f, r * .7f, a + .5f), Ember);
        b.box(ring(h * .55f, r * .7f, a + .5f), ring(h, .02f, a + 1.f), Amber);
    }
}

// Warhorse 2-1, the Delta Squad insertion bird, down in the road beyond the front gate: fuselage
// rolled onto its side, tail torn away, rotor buckled into the asphalt, and still burning.
void warhorseWreck(Builder& b) {
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    CreatureWriter w(vertices, &triangles);
    // Surfaces stand in for materials here; the cabin cross is painted over on an assault bird.
    writeTransport(w, {HeloOlive, HeloDark, Iron, HeloOlive, HeloOlive}, true, true, 2.6f);
    // Lying along the line of sight from the portico, so it frames the opening view without
    // hiding the patrol picking through it.
    const Vec3 site = position(372, 622);
    const auto pose = [&](Vec3 p, int group) {
        if (group == 0) return site + turnY(turnX(turnZ(p - Vec3{0, 1.5f, 0}, -1.22f), .1f), .22f);
        if (group == 1) return site + Vec3{1.6f, 0, -6.4f} + turnY(turnZ(p - Vec3{0, 1.8f, -2.4f}, 1.45f), .95f);
        return site + Vec3{-2.4f, .1f, -1.2f} + turnY(turnX(p, .16f), .7f);
    };
    // Rest each piece on the road.
    std::array<float, 3> lowest{1e9f, 1e9f, 1e9f};
    for (const auto& v : vertices) {
        const int g = std::clamp(int(v.v), 0, 2);
        lowest[size_t(g)] = std::min(lowest[size_t(g)], pose(v.position, g).y);
    }
    const auto place = [&](const Vertex& v) {
        const int g = std::clamp(int(v.v), 0, 2);
        return pose(v.position, g) - Vec3{0, lowest[size_t(g)] + .08f, 0};
    };
    for (const auto& t : triangles) {
        b.face({place(vertices[t.a]), place(vertices[t.c]), place(vertices[t.b])}, Surface(t.material));
    }
    b.floor({318, 560, 430, 700}, Scorch, .02f);
    const Vec3 engine = place({{0, 2.5f, -.5f}, 0, 0});
    flames(b, engine, 1.6f, .9f, 7);
    flames(b, site + Vec3{-2.4f, .02f, .6f}, .9f, .7f, 4);
    b.smoke(engine + Vec3{0, .8f, 0}, 22.f, Vec3{-5.f, 0, 10.f}, 1.9f);
}

// Dr. Marsh's radio array on the east wing ridge, still aimed at the mothership it found.
void markerArray(Builder& b) {
    const Vec3 foot = position(612, 62, 3.75f);
    const Vec3 mothership = position(250, -235, 31.f);
    const Vec3 hub = foot + Vec3{0, 1.35f, 0};
    const Vec3 aim = normalized(mothership - hub);
    const Vec3 right = normalized(cross(aim, Vec3{0, 1, 0}));
    const Vec3 upward = cross(right, aim);
    const std::array<Vec3, 4> lower{foot + Vec3{-.3f, 0, -.3f}, foot + Vec3{-.3f, 0, .3f}, foot + Vec3{.3f, 0, .3f}, foot + Vec3{.3f, 0, -.3f}};
    const std::array<Vec3, 4> upper{hub + Vec3{-.16f, 0, -.16f}, hub + Vec3{-.16f, 0, .16f}, hub + Vec3{.16f, 0, .16f}, hub + Vec3{.16f, 0, -.16f}};
    b.box(lower, upper, Steel);
    // A shallow dish: a fan of panels from the hub out to the rim, faced both ways.
    constexpr int panels = 14;
    const auto rimPoint = [&](int i, float radius, float depth) {
        const float t = 2 * pi * float(i) / panels;
        return hub + aim * depth + (right * std::cos(t) + upward * std::sin(t)) * radius;
    };
    for (int i = 0; i < panels; ++i) {
        const Vec3 a = rimPoint(i, .55f, .12f), bq = rimPoint(i + 1, .55f, .12f);
        const Vec3 c = rimPoint(i + 1, 1.75f, .62f), d = rimPoint(i, 1.75f, .62f);
        b.quad({a, bq, c, d}, Ivory);
        b.quad({d, c, bq, a}, Ivory);
        b.face({hub + aim * .02f, bq, a}, Ivory);
        b.face({hub + aim * .02f, a, bq}, Ivory);
    }
    // Feed horn on three struts at the focus, with a red warning light.
    const Vec3 focus = hub + aim * 1.55f;
    for (int i = 0; i < 3; ++i) {
        const Vec3 rim = rimPoint(i * panels / 3, 1.7f, .6f);
        const Vec3 across = normalized(cross(focus - rim, aim)) * .03f;
        b.quad({rim - across, rim + across, focus + across, focus - across}, Steel);
        b.quad({focus - across, focus + across, rim + across, rim - across}, Steel);
    }
    b.box({focus + Vec3{-.12f, -.12f, -.12f}, focus + Vec3{-.12f, -.12f, .12f}, focus + Vec3{.12f, -.12f, .12f}, focus + Vec3{.12f, -.12f, -.12f}},
          {focus + Vec3{-.12f, .12f, -.12f}, focus + Vec3{-.12f, .12f, .12f}, focus + Vec3{.12f, .12f, .12f}, focus + Vec3{.12f, .12f, -.12f}}, HeloDark);
    const Vec3 beacon = hub + upward * 1.85f + aim * .62f;
    b.box({beacon + Vec3{-.05f, 0, -.05f}, beacon + Vec3{-.05f, 0, .05f}, beacon + Vec3{.05f, 0, .05f}, beacon + Vec3{.05f, 0, -.05f}},
          {beacon + Vec3{-.05f, .1f, -.05f}, beacon + Vec3{-.05f, .1f, .05f}, beacon + Vec3{.05f, .1f, .05f}, beacon + Vec3{.05f, .1f, -.05f}}, AlienCore);
}

void invasionDetails(Builder& b) {
    // Scorched impact crater, crashed alien drop pod, and glowing crystalline shards on the front-west lawn.
    b.floor({64, 414, 116, 458}, Scorch, .022f);
    b.round(90, 436, .02f, .24f, 18, 12, Scorch);
    b.round(90, 436, .18f, 1.38f, 9.5f, 5.2f, AlienHull);
    b.round(90, 436, 1.38f, 1.66f, 5.2f, 2.2f, AlienTrim);
    b.round(90, 436, .52f, .86f, 8.6f, 7.6f, AlienGlow);
    b.lamp(90, 436, 1.15f, 8.2f, 1.15f);
    // Radiating plasma crystal shards around the crater rim.
    b.round(73, 424, .02f, .46f, 2.4f, .4f, AlienGlow);
    b.round(108, 444, .02f, .38f, 2.0f, .3f, AlienGlow);
    b.round(96, 419, .02f, .32f, 1.8f, .3f, AlienTrim);
    // Scorched blast mark and shattered debris right outside the front portico, where the crash threw us.
    b.floor({338, 398, 396, 424}, Scorch, .024f);
    b.box({254, 405, 266, 413}, .02f, .22f, AlienHull);
    b.box({257, 407, 263, 411}, .22f, .31f, AlienGlow);
    b.box({458, 403, 470, 412}, .02f, .19f, AlienTrim);
    // Plasma scars across the front lawn and through the Grand Hall where the first wave came in.
    b.floor({300, 468, 352, 480}, Scorch, .021f);
    b.floor({402, 452, 440, 461}, Scorch, .021f);
    b.floor({352, 334, 386, 346}, Scorch, .008f);
    // Military crimson road flares marking the Delta Squad perimeter & rear evac patio.
    roadFlare(b, 574, 448); // Driveway gate approach
    roadFlare(b, 344, -14); // Rear patio evac left
    roadFlare(b, 392, -14); // Rear patio evac right
    warhorseWreck(b);
    markerArray(b);
}

void invasionBackdrop(Builder& b) {
    // Homes across the valley burn under the landings. The fleet itself only renders once the
    // mission begins, so the title screen shows the smoke but not its cause.
    const size_t count = b.homes.size();
    for (size_t i = 0; i < count && i < 9; i += 3) {
        const Vec3 home = b.homes[i];
        flames(b, home + Vec3{0, 3.2f, 0}, 2.2f, 1.6f, 5);
        b.smoke(home + Vec3{0, 4.f, 0}, 26.f, Vec3{-6.f, 0, 9.f}, 2.2f);
    }
}

void buildAlienMeshes(Assets& assets, uint16_t firstMat) {
    const uint16_t hull = uint16_t(firstMat + AlienHull);
    const uint16_t trim = uint16_t(firstMat + AlienTrim);
    const uint16_t glow = uint16_t(firstMat + AlienGlow);
    const uint16_t core = uint16_t(firstMat + AlienCore);

    buildCreature(assets, firstMat);
    buildCivilian(assets, firstMat);
    buildTransport(assets, firstMat);

    // Hovering alien scout drone (centered around y = 0, ~0.9m span, facing +z).
    assets.alienDrone = {};
    MeshWriter d{assets.alienDrone};
    constexpr int sides = 8;
    constexpr float r = .44f, hTop = .20f, hBot = -.18f;
    for (int i = 0; i < sides; ++i) {
        const float a0 = 2 * pi * i / sides, a1 = 2 * pi * (i + 1) / sides;
        const Vec3 e0{std::sin(a0) * r, 0, std::cos(a0) * r};
        const Vec3 e1{std::sin(a1) * r, 0, std::cos(a1) * r};
        d.tri(e0, e1, {0, hTop, 0}, i % 2 ? hull : trim);
        d.tri(e1, e0, {0, hBot, 0}, hull);
    }
    // Swept side winglets
    d.box({-.62f, -.05f, -.18f}, {-.36f, .07f, .14f}, trim);
    d.box({.36f, -.05f, -.18f}, {.62f, .07f, .14f}, trim);
    // Glowing forward scanner eye & underside plasma emitter
    d.box({-.13f, -.07f, .36f}, {.13f, .09f, .49f}, glow);
    d.box({-.11f, -.22f, -.11f}, {.11f, -.16f, .11f}, core);
    finalizeProceduralMesh(assets.alienDrone);

    // Alien fleet crafts (each welded with tight bounds for O(1) frustum culling; hidden on title screen).
    const struct { float cx, cy, alt, r, h; bool beam; } specs[3]{
        {250.f, -235.f, 31.f, 240.f, 7.6f, true},  // Primary mothership over northern ridge
        {430.f, 770.f, 23.f, 128.f, 4.4f, false},  // Southern flanking saucer
        {-210.f, 120.f, 29.f, 95.f, 3.6f, false},  // Western scout cruiser
    };
    for (int s = 0; s < 3; ++s) {
        auto& craft = assets.alienFleet[s];
        craft = {};
        MeshWriter f{craft};
        constexpr int n = 10;
        const Vec3 center = position(specs[s].cx, specs[s].cy, specs[s].alt);
        const float rOuter = specs[s].r * scale;
        const float rInner = rOuter * .52f;
        const float rCore = rOuter * .24f;
        const float height = specs[s].h;
        const Vec3 topPeak = center + Vec3{0, height * .68f, 0};
        const Vec3 botPeak = center - Vec3{0, height * .46f, 0};
        for (int i = 0; i < n; ++i) {
            const float a0 = 2 * pi * i / n, a1 = 2 * pi * (i + 1) / n;
            const Vec3 d0{std::cos(a0), 0, std::sin(a0)}, d1{std::cos(a1), 0, std::sin(a1)};
            const Vec3 out0 = center + d0 * rOuter, out1 = center + d1 * rOuter;
            const Vec3 up0 = center + d0 * rInner + Vec3{0, height * .34f, 0};
            const Vec3 up1 = center + d1 * rInner + Vec3{0, height * .34f, 0};
            const Vec3 dn0 = center + d0 * rInner - Vec3{0, height * .26f, 0};
            const Vec3 dn1 = center + d1 * rInner - Vec3{0, height * .26f, 0};
            const Vec3 glow0 = center + d0 * rCore - Vec3{0, height * .36f, 0};
            const Vec3 glow1 = center + d1 * rCore - Vec3{0, height * .36f, 0};
            f.quad(out0, out1, up1, up0, hull);
            f.tri(up0, up1, topPeak, trim);
            f.quad(dn0, dn1, out1, out0, hull);
            f.quad(glow0, glow1, dn1, dn0, glow);
            f.tri(botPeak, glow1, glow0, core);
        }
        if (specs[s].beam) {
            constexpr int beamSides = 6;
            const float topR = rCore * .52f, botR = rCore * .85f;
            const Vec3 ground = position(specs[s].cx, specs[s].cy, 0.f);
            const Vec3 emit = center - Vec3{0, height * .40f, 0};
            for (int i = 0; i < beamSides; ++i) {
                const float a0 = 2 * pi * i / beamSides, a1 = 2 * pi * (i + 1) / beamSides;
                const Vec3 d0{std::cos(a0), 0, std::sin(a0)}, d1{std::cos(a1), 0, std::sin(a1)};
                f.quad(ground + d0 * botR, ground + d1 * botR, emit + d1 * topR, emit + d0 * topR, glow);
            }
        }
        finalizeProceduralMesh(craft);
    }
}

void buildMissionUniforms(Assets& assets, uint16_t briarFirstMat) {
    const uint32_t baseCount = uint32_t(assets.materials.size());
    const auto addMat = [&](uint16_t color, bool emissive = false) -> uint16_t {
        const uint16_t idx = uint16_t(assets.materials.size());
        assets.materials.push_back({color, -1, 255, true, emissive});
        return idx;
    };
    const auto makeRemap = [&](uint16_t primary, uint16_t secondary, uint16_t gear) {
        std::vector<uint16_t> remap(assets.materials.size());
        for (size_t i = 0; i < remap.size(); ++i) remap[i] = uint16_t(i);
        if (assets.rivalFrames.empty()) return remap;
        // Remap materials used by the base rival mesh while leaving warm skin tones untouched.
        for (const auto& tri : assets.rivalFrames.front().triangles) {
            if (tri.material >= baseCount) continue;
            const auto& m = assets.materials[tri.material];
            const int r = (m.color >> 11) * 8;
            const int g = ((m.color >> 5) & 63) * 4;
            const int b = (m.color & 31) * 8;
            // Preserve skin-like hues (warm peach/tan with red clearly above blue) and dark eyes/boots.
            const bool skin = r > 155 && g > 105 && b > 70 && (r - b) > 38 && (r - g) > 15;
            const bool dark = r < 52 && g < 52 && b < 52;
            if (skin) continue;
            if (dark) remap[tri.material] = gear;
            else if (r + g + b > 340) remap[tri.material] = primary;
            else remap[tri.material] = secondary;
        }
        return remap;
    };

    // Delta-1 Commander: US tactical olive camo + charcoal plate carrier + matte black helmet/boots.
    const uint16_t deltaOlive = addMat(rgb(76, 98, 63));
    const uint16_t deltaVest = addMat(rgb(54, 64, 52));
    const uint16_t deltaGear = addMat(rgb(38, 43, 39));
    assets.commanderUniform = makeRemap(deltaOlive, deltaVest, deltaGear);

    // Three distinct civilians: navy jacket, crimson sweater, cream coat, each with their own
    // hair and skin tone, on the unarmed procedural civilian.
    const struct { uint16_t top, pants, shoes, hair, skin; } outfits[3]{
        {addMat(rgb(52, 86, 138)), addMat(rgb(70, 74, 82)), addMat(rgb(44, 42, 42)), addMat(rgb(96, 66, 40)),
         addMat(rgb(222, 180, 148))},
        {addMat(rgb(168, 62, 66)), addMat(rgb(60, 78, 118)), addMat(rgb(92, 62, 40)), addMat(rgb(28, 24, 24)),
         addMat(rgb(150, 104, 74))},
        {addMat(rgb(204, 192, 154)), addMat(rgb(54, 96, 92)), addMat(rgb(56, 48, 42)), addMat(rgb(176, 172, 166)),
         addMat(rgb(232, 196, 168))},
    };
    for (int i = 0; i < 3; ++i) {
        auto& remap = assets.civilianUniforms[i];
        remap.resize(baseCount);
        for (size_t m = 0; m < remap.size(); ++m) remap[m] = uint16_t(m);
        remap[briarFirstMat + CivTop] = outfits[i].top;
        remap[briarFirstMat + CivPants] = outfits[i].pants;
        remap[briarFirstMat + CivShoes] = outfits[i].shoes;
        remap[briarFirstMat + CivHair] = outfits[i].hair;
        remap[briarFirstMat + CivSkin] = outfits[i].skin;
    }

    // Alien Enforcer uniform: dark charcoal armor + crimson glowing visor, chest core & plasma rail.
    const uint16_t enforcerHull = addMat(rgb(30, 26, 36));
    const uint16_t enforcerTrim = addMat(rgb(72, 44, 54));
    assets.enforcerUniform.resize(assets.materials.size());
    for (size_t i = 0; i < assets.enforcerUniform.size(); ++i) assets.enforcerUniform[i] = uint16_t(i);
    if (size_t(briarFirstMat + AlienCore) < assets.enforcerUniform.size()) {
        assets.enforcerUniform[briarFirstMat + AlienHull] = enforcerHull;
        assets.enforcerUniform[briarFirstMat + AlienTrim] = enforcerTrim;
        assets.enforcerUniform[briarFirstMat + AlienGlow] = uint16_t(briarFirstMat + AlienCore);
    }
    // Enforcers wear blood-red hide over the same body.
    if (size_t(briarFirstMat + AlienSinew) < assets.enforcerUniform.size()) {
        assets.enforcerUniform[briarFirstMat + AlienSkin] = addMat(rgb(112, 30, 44));
        assets.enforcerUniform[briarFirstMat + AlienSinew] = addMat(rgb(58, 16, 26));
    }
}

void synthesizeMissionAudio(Assets& assets) {
    if (assets.audio.size() < size_t(Sound::Count)) assets.audio.resize(size_t(Sound::Count));
    const auto writeClip = [&](Sound id, double seconds, auto generator) {
        auto& samples = assets.audio[size_t(id)].samples;
        const size_t count = size_t(seconds * audioHz);
        samples.resize(count);
        for (size_t i = 0; i < count; ++i) {
            const double t = double(i) / audioHz;
            const double value = std::clamp(generator(t, seconds), -1.0, 1.0);
            samples[i] = int16_t(value * 24000.0);
        }
    };
    // Crisp alien plasma bolt: high downward frequency sweep with harmonic bite.
    writeClip(Sound::PlasmaFire, 0.22, [](double t, double dur) {
        const double env = std::exp(-t * 15.0) * (1.0 - t / dur);
        const double phase = 2.0 * pi * (1350.0 * t - 420.0 * std::log(1.0 + t * 18.0));
        return env * (0.62 * std::sin(phase) + 0.30 * std::sin(phase * 2.03) + 0.14 * std::sin(2.0 * pi * 210.0 * t));
    });
    // Alien drone energy burst: sharper metallic pulse.
    writeClip(Sound::DroneFire, 0.17, [](double t, double dur) {
        const double env = std::exp(-t * 18.0) * (1.0 - t / dur);
        const double phase = 2.0 * pi * (1850.0 * t - 520.0 * std::log(1.0 + t * 22.0));
        return env * (0.58 * std::sin(phase) + 0.36 * std::sin(phase * 1.5));
    });
    // Alien neutralization warble.
    writeClip(Sound::AlienDown, 0.38, [](double t, double dur) {
        const double env = std::exp(-t * 5.5) * (1.0 - t / dur);
        const double vibrato = std::sin(2.0 * pi * 34.0 * t);
        const double freq = 520.0 - 320.0 * (t / dur) + 65.0 * vibrato;
        return env * 0.72 * std::sin(2.0 * pi * freq * t);
    });
    // Concussion wake-up tinnitus + low impact rumble.
    writeClip(Sound::ConcussionRing, 2.2, [](double t, double dur) {
        const double attack = std::min(1.0, t / 0.08);
        const double decay = std::exp(-t * 1.35) * (1.0 - t / dur);
        const double ring = 0.24 * std::sin(2.0 * pi * 2850.0 * t) + 0.10 * std::sin(2.0 * pi * 2862.0 * t);
        const double thump = 0.65 * std::exp(-t * 7.5) * std::sin(2.0 * pi * (78.0 - 32.0 * t) * t);
        return attack * (decay * ring + thump);
    });
    // Tactical Delta Squad radio squelch chirp.
    writeClip(Sound::RadioChirp, 0.14, [](double t, double dur) {
        const double env = (t < 0.055 || t > 0.08) ? std::sin(pi * std::fmod(t, 0.07) / 0.07) : 0.0;
        const double freq = t < 0.07 ? 1120.0 : 1480.0;
        return 0.42 * env * (1.0 - 0.3 * t / dur) * std::sin(2.0 * pi * freq * t);
    });
    // Warm two-tone rescue confirmation chime.
    writeClip(Sound::CivilianSaved, 0.34, [](double t, double dur) {
        const double env = std::exp(-t * 5.0) * (1.0 - t / dur);
        const double freq = t < 0.12 ? 660.0 : 990.0;
        return 0.52 * env * (std::sin(2.0 * pi * freq * t) + 0.25 * std::sin(4.0 * pi * freq * t));
    });
    // Low muffled double heartbeat ("lub-dub") for the concussed awakening.
    writeClip(Sound::Heartbeat, 0.72, [](double t, double) {
        const auto thump = [](double dt, double freq) {
            if (dt < 0.0 || dt > 0.22) return 0.0;
            const double env = std::sin(pi * std::min(1.0, dt / 0.035)) * std::exp(-dt * 24.0);
            return env * std::sin(2.0 * pi * (freq - 24.0 * dt) * dt);
        };
        return 0.85 * thump(t, 64.0) + 0.62 * thump(t - 0.24, 52.0);
    });
    // Deep eerie alien warhorn / tripod brass blast echoing across the mountain valley.
    writeClip(Sound::AlienHorn, 2.4, [](double t, double dur) {
        const double attack = std::min(1.0, t / 0.28);
        const double release = std::clamp((dur - t) / 0.65, 0.0, 1.0);
        const double env = attack * release;
        const double f0 = 76.0 - 11.0 * (t / dur);
        const double w = 2.0 * pi * f0 * t;
        const double grit = std::sin(w) * 0.55 + std::sin(w * 2.01) * 0.28 +
                            std::sin(w * 3.02) * 0.16 + std::sin(w * 4.98) * 0.08;
        const double mod = 0.82 + 0.18 * std::sin(2.0 * pi * 4.5 * t);
        return env * mod * 0.78 * std::tanh(grit * 1.4);
    });
    // Creature roar: a rattling, buzzing throat with a hissing upper formant, rising then dropping.
    writeClip(Sound::AlienRoar, 0.95, [](double t, double dur) {
        const double attack = std::min(1.0, t / 0.05);
        const double release = std::clamp((dur - t) / 0.35, 0.0, 1.0);
        const double f0 = 92.0 + 58.0 * std::sin(pi * std::min(1.0, t / dur));
        const double w = 2.0 * pi * f0 * t;
        double buzz = 0;
        for (int h = 1; h <= 7; ++h) buzz += std::sin(w * h) / h;
        const uint32_t hash = uint32_t(t * audioHz) * 2654435761u;
        const double hiss = (double((hash >> 9) & 1023) / 511.5 - 1.0) * (0.22 + 0.18 * std::sin(2.0 * pi * 7.0 * t));
        const double formant = 0.2 * std::sin(2.0 * pi * (820.0 + 340.0 * t) * t);
        const double rattle = 0.72 + 0.28 * std::sin(2.0 * pi * 31.0 * t);
        return attack * release * 0.8 * std::tanh((buzz * 0.55 * rattle + hiss * 0.5 + formant) * 1.6);
    });
    // Subtle eerie looping mothership sub-bass ambience for quiet tension moments.
    writeClip(Sound::InvasionHum, 2.0, [](double t, double) {
        const double p1 = 2.0 * pi * 48.0 * t;
        const double p2 = 2.0 * pi * 72.0 * t;
        const double swell = 0.72 + 0.28 * std::sin(2.0 * pi * 0.5 * t);
        return swell * (0.20 * std::sin(p1) + 0.11 * std::sin(p2) + 0.04 * std::sin(2.0 * pi * 144.0 * t));
    });
}

void darkenInvasionSky(Assets& assets) {
    // Deepen the sky luma slightly for an ominous alien-invasion dusk while keeping clouds legible.
    for (auto& face : assets.sky) {
        for (uint8_t& luma : face) luma = uint8_t(int(luma) * 68 / 100);
    }
}

} // namespace

void buildBriarHouse(Assets& assets, BriarHouseMode mode) {
    if (assets.arena == Arena::BriarHouse && assets.briarHouseMode == mode) return;
    const uint64_t shared = assets.residentBytes - std::min(assets.residentBytes, arenaBytes(assets));
    assets.world = {};
    assets.shotVertices = decltype(assets.shotVertices){};
    assets.shotIndices = decltype(assets.shotIndices){};
    assets.shotSurfaces = decltype(assets.shotSurfaces){};
    assets.shotChunks = decltype(assets.shotChunks){};
    assets.obstacles = decltype(assets.obstacles){};
    assets.navNodes = decltype(assets.navNodes){};
    assets.navEdges = decltype(assets.navEdges){};
    assets.lamps = decltype(assets.lamps){};
    assets.glassHits = decltype(assets.glassHits){};
    assets.alienTrooper = {};
    assets.alienTrooperPoses.clear();
    assets.alienEnforcer = {};
    assets.alienEnforcerPoses.clear();
    assets.civilianMesh = {};
    assets.civilianPoses.clear();
    assets.transportHelo = {};
    assets.transportRotor = {};
    assets.smoke.clear();
    assets.alienDrone = {};
    for (auto& craft : assets.alienFleet) craft = {};
    assets.commanderUniform.clear();
    assets.enforcerUniform.clear();
    for (auto& u : assets.civilianUniforms) u.clear();
    assets.arena = Arena::BriarHouse;
    assets.briarHouseMode = mode;
    assets.extent.x = 19.5f;
    assets.spawns = {{
        {position(322, 495), 1.06f}, {position(399, -78), -1.95f},
        {position(-27, 282), .85f}, {position(645, 285), -pi / 2},
    }};

    const uint16_t briarFirstMaterial = uint16_t(assets.materials.size());
    Builder builder(assets);
    grounds(builder);
    shell(builder);
    hallStructure(builder);
    builder.shiftX(-72);
    kitchen(builder);
    builder.shiftX(0);
    garageStructure(builder);
    sealedStairs(builder);
    builder.beginDecoration();
    exteriorDetails(builder);
    gardenDetails(builder);
    if (mode == BriarHouseMode::Invasion) invasionDetails(builder);
    builder.shiftX(-72);
    kitchenDetails(builder);
    builder.shiftX(0);
    garageDetails(builder);
    staircaseDetails(builder);
    hallDetails(builder);
    builder.beginBackdrop();
    mountainCommunity(builder);
    if (mode == BriarHouseMode::Invasion) invasionBackdrop(builder);
    builder.finish();
    navigation(assets);
    if (mode == BriarHouseMode::Invasion) {
        buildAlienMeshes(assets, briarFirstMaterial);
        buildMissionUniforms(assets, briarFirstMaterial);
        synthesizeMissionAudio(assets);
        darkenInvasionSky(assets);
    }
    assets.residentBytes = shared + arenaBytes(assets);
    assets.peakBytes = std::max(assets.peakBytes, assets.residentBytes);
}

} // namespace fod
