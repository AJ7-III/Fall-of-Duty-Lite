#include "briar_house.hpp"

#include "assets.hpp"
#include "game_world.hpp"
#include "mission.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <initializer_list>
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
    Meadow, ForestSlope, MountainRock, CabinCedar, CabinGreen, CabinCream, CabinGlass,
    Iron, Gravel, CabinSlate, RidgePine,
    AlienHull, AlienTrim, AlienGlow, AlienCore, Scorch,
    AlienSkin, AlienSinew, AlienTooth, AlienMaw, AlienGum,
    RayMetal, RayCoil, RayCore, RayOvercharge,
    CivSkin, CivHair, CivTop, CivPants, CivShoes,
    HeloOlive, HeloDark, HeloMark, HeloCross,
    GrassBladeDark, GrassBladeMid, GrassBladeGold, RoadLine,
    CivTrim, CivFabric, CivFeatures, CivGlasses, CivCap, CivCapBand,
    PineNeedles, PineShade, ScorchEdge, CabinLit, HeloPanel, HeloGlass, WreckSkin, WreckGlass, PodSkin,
    FirTips, PineBark, AspenBark, AspenBarkShade, AspenGold, AspenOchre, AspenAmber,
    AlpineScree, SurfaceCount
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
    rgb(108, 122, 77), rgb(68, 91, 66), rgb(121, 126, 124),
    rgb(154, 98, 70), rgb(117, 137, 109), rgb(197, 177, 139), rgb(125, 168, 181),
    rgb(49, 56, 54), rgb(152, 143, 123), rgb(83, 100, 105), rgb(50, 72, 59),
    rgb(42, 49, 64), rgb(74, 86, 108), rgb(72, 245, 255), rgb(255, 88, 104), rgb(34, 32, 31),
    rgb(67, 103, 113), rgb(33, 54, 69), rgb(217, 225, 195), rgb(24, 12, 31), rgb(121, 55, 81),
    rgb(56, 66, 88), rgb(132, 156, 182), rgb(166, 255, 255), rgb(255, 124, 78),
    rgb(214, 170, 136), rgb(70, 50, 38), rgb(52, 86, 138), rgb(70, 74, 82), rgb(44, 42, 42),
    rgb(78, 86, 64), rgb(38, 42, 38), rgb(228, 228, 220), rgb(182, 34, 40),
    rgb(54, 82, 42), rgb(88, 110, 58), rgb(118, 128, 66), rgb(194, 162, 78),
    rgb(192, 166, 109), rgb(41, 57, 78), rgb(54, 35, 31), rgb(55, 38, 34), rgb(68, 113, 94), rgb(39, 73, 60),
    rgb(65, 84, 62), rgb(37, 52, 47), rgb(40, 33, 28), rgb(183, 134, 83),
    rgb(117, 126, 95), rgb(100, 133, 142), rgb(76, 85, 68), rgb(106, 135, 143), rgb(67, 81, 100),
    rgb(81, 111, 78), rgb(104, 74, 59), rgb(169, 157, 138), rgb(94, 81, 77),
    rgb(196, 150, 65), rgb(150, 101, 40), rgb(213, 172, 78),
    rgb(113, 105, 98),
};

// Warm Colorado dusk alpenglow tinting applied to exterior materials in Invasion mode.
uint16_t duskTint(uint16_t c, int addR, int addG, int addB, int scalePct = 100) {
    const int r = std::clamp((((c >> 11) * 255 / 31) * scalePct) / 100 + addR, 0, 255);
    const int g = std::clamp(((((c >> 5) & 63) * 255 / 63) * scalePct) / 100 + addG, 0, 255);
    const int b = std::clamp((((c & 31) * 255 / 31) * scalePct) / 100 + addB, 0, 255);
    return rgb(unsigned(r), unsigned(g), unsigned(b));
}

unsigned grain(int x, int y) {
    uint32_t hash = uint32_t(x) * 0x8da6b343u + uint32_t(y) * 0xd8163841u + 0xa36b7a13u;
    hash = (hash ^ (hash >> 13)) * 0x85ebca6bu;
    return (hash ^ (hash >> 16)) & 15u;
}

int tileNoise(int x, int y, int size, int spacing) {
    const int count = size / spacing, i = x / spacing, j = y / spacing;
    const int u = x % spacing, v = y % spacing;
    const auto mix = [spacing](int a, int b, int t) { return (a * (spacing - t) + b * t) / spacing; };
    const int right = (i + 1) % count, below = (j + 1) % count;
    return mix(mix(grain(i, j), grain(right, j), u), mix(grain(i, below), grain(right, below), u), v);
}

uint16_t masonry(int x, int y, int r, int g, int b) {
    const int row = y / 8, course = (x + row % 2 * 8) % 64;
    const int joint = std::min(course % 16, y % 8);
    const int surface = int(grain(course / 16, row)) + int(grain(x, y)) / 2;
    const int bevel = joint == 0 ? -24 : y % 8 == 1 ? 9 : y % 8 == 7 ? -8 : 0;
    const int relief = bevel + surface + tileNoise(x, y, 64, 16) / 2;
    return rgb(r + relief, g + relief, b + relief);
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

uint32_t floatBits(float v) {
    if (v == 0.f) v = 0.f; // Canonicalize -0.0f so it matches +0.0f.
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

uint32_t hashKey5(const std::array<uint32_t, 5>& fields) {
    uint32_t h = 2166136261u;
    for (uint32_t f : fields) h = (h ^ f) * 16777619u;
    return h ^ (h >> 16);
}

// Flat open-addressed weld avoids per-vertex heap nodes in PSRAM during arena construction.
template<typename V, typename Key>
std::vector<uint32_t> weld(std::vector<V>& vertices, Key key) {
    const size_t n = vertices.size();
    std::vector<uint32_t> remap;
    remap.reserve(n);
    if (!n) return remap;
    size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    const size_t mask = cap - 1;
    std::vector<uint32_t> table(cap, UINT32_MAX);
    std::vector<std::array<uint32_t, 5>> keys;
    keys.reserve(n);
    uint32_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto raw = key(vertices[i]);
        const std::array<uint32_t, 5> k{floatBits(raw[0]), floatBits(raw[1]), floatBits(raw[2]),
                                        floatBits(raw[3]), floatBits(raw[4])};
        size_t slot = hashKey5(k) & mask;
        for (;;) {
            const uint32_t entry = table[slot];
            if (entry == UINT32_MAX) {
                table[slot] = used;
                keys.push_back(k);
                vertices[used] = vertices[i];
                remap.push_back(used++);
                break;
            }
            if (keys[entry] == k) {
                remap.push_back(entry);
                break;
            }
            slot = (slot + 1) & mask;
        }
    }
    vertices.resize(used);
    vertices.shrink_to_fit();
    return remap;
}

class Builder {
public:
    explicit Builder(Assets& data, bool invasionDusk = false)
        : assets(data), firstMaterial(uint16_t(data.materials.size())), dusk(invasionDusk) {
        repeats.fill(.5f);
        buildMaterials();
        buildTextures();
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

    // Oriented box collision aligned with unit horizontal vector `along` (local X) and its perpendicular (local Z).
    void orientedCollision(Vec3 center, Vec3 along, float halfAlong, float halfOut, float bottom, float top,
                           bool blocksSight = true) {
        const Vec3 a = normalized({along.x, 0.f, along.z});
        const float cs = a.x, sn = -a.z;
        assets.obstacles.push_back({center.x, center.z, halfAlong, halfOut, bottom, top,
                                    std::atan2(sn, cs), cs, sn, blocksSight});
    }

    bool isDusk() const { return dusk; }
    const Assets& data() const { return assets; }
    uint16_t surfaceMat(Surface s) const { return uint16_t(firstMaterial + s); }

    // Stamp a cached static Ship Box prop (crate, pallet, barrel, lamp pole, container) directly
    // into assets.world with full UVs, original high-res textures, and baked dusk shadows.
    void stampMesh(const Mesh& src, Vec3 at, float yaw, float s = 1.f, int16_t swapNonTexMat = -1) {
        if (src.triangles.empty()) return;
        const float cs = std::cos(yaw), sn = std::sin(yaw);
        auto& world = assets.world;
        const uint32_t baseVert = uint32_t(world.vertices.size());
        for (const auto& v : src.vertices) {
            const Vec3 p{v.position.x * s, v.position.y * s, v.position.z * s};
            const Vec3 w{at.x + cs * p.x + sn * p.z, at.y + p.y, at.z - sn * p.x + cs * p.z};
            world.vertices.push_back({w, v.u, v.v});
        }
        for (const auto& t : src.triangles) {
            uint16_t mat = t.material;
            if (swapNonTexMat >= 0 && mat < assets.materials.size() && assets.materials[mat].texture < 0) {
                mat = uint16_t(swapNonTexMat);
            }
            world.triangles.push_back({baseVert + t.a, baseVert + t.b, baseVert + t.c, mat, -1});
        }
    }

    // Place a high-detail instanced Ship Box vehicle (Truck or Car) with exact rotated culling
    // bounds and solid oriented collision for cover, navigation, bullets, and dusk shadows.
    bool placeVehicle(PropKind kind, Vec3 at, float yaw, uint16_t paint, uint16_t cargoPaint = 0, float s = 1.f) {
        const Mesh& src = kind == PropKind::Truck ? assets.shipBoxTruck : assets.shipBoxCar;
        if (src.triangles.empty() || src.chunks.empty()) return false;
        const Bounds lb = src.chunks.front().bounds;
        const float cs = std::cos(yaw), sn = std::sin(yaw);
        Bounds wb{{1e9f, 1e9f, 1e9f}, {-1e9f, -1e9f, -1e9f}};
        for (int ix = 0; ix < 2; ++ix) {
            const float x = (ix ? lb.max.x : lb.min.x) * s;
            for (int iy = 0; iy < 2; ++iy) {
                const float y = (iy ? lb.max.y : lb.min.y) * s;
                for (int iz = 0; iz < 2; ++iz) {
                    const float z = (iz ? lb.max.z : lb.min.z) * s;
                    expand(wb, {at.x + cs * x + sn * z, at.y + y, at.z - sn * x + cs * z});
                }
            }
        }
        const float halfLen = (lb.max.x - lb.min.x) * 0.5f * s;
        const float halfWidth = (lb.max.z - lb.min.z) * 0.5f * s;
        const float height = (lb.max.y - lb.min.y) * s;
        PropInstance prop{kind, at, yaw, s, wb, halfLen, halfWidth, height, {}};
        // Paint IDs in the Ship Box pack. Also check the texture slot before
        // recoloring an extracted mesh from a different pack.
        if (kind == PropKind::Car) {
            paintVehicle(prop, src, 40, 31, paint);
        } else {
            paintVehicle(prop, src, 26, 23, paint);
            paintVehicle(prop, src, 38, 30, cargoPaint);
        }
        assets.propInstances.push_back(std::move(prop));
        const Vec3 along{cs, 0.f, -sn};
        // Trim slightly inward from side mirrors so collision hugs the metal body cleanly.
        orientedCollision(at, along, std::max(0.5f, halfLen - 0.12f), std::max(0.5f, halfWidth - 0.16f),
                          at.y, at.y + height, true);
        return true;
    }

    // Ship Box-style leaning 3D tapered grass tuft cluster (3-4 blades across 3 dusk shades).
    void grassTuft(Vec3 at, uint32_t seed) {
        const auto nextRand = [&]() -> float {
            seed = seed * 1664525u + 1013904223u;
            return float((seed >> 8) & 0xFFFFu) / 65535.f;
        };
        const int blades = 3 + int(nextRand() * 2.f);
        static constexpr Surface shades[3]{GrassBladeDark, GrassBladeMid, GrassBladeGold};
        for (int i = 0; i < blades; ++i) {
            const float a = nextRand() * 2.f * pi;
            const float leanA = a + (nextRand() - 0.5f) * 1.4f;
            const float dist = nextRand() * 0.18f;
            const float halfW = 0.045f + nextRand() * 0.032f;
            const float h = 0.22f + nextRand() * 0.26f;
            const float lean = 0.06f + nextRand() * 0.14f;
            const Vec3 base = at + Vec3{std::cos(a) * dist, 0.f, std::sin(a) * dist};
            const Vec3 side{std::cos(a + pi * 0.5f) * halfW, 0.f, std::sin(a + pi * 0.5f) * halfW};
            const Vec3 tip = base + Vec3{std::cos(leanA) * lean, h, std::sin(leanA) * lean};
            face({base - side, base + side, tip}, shades[(seed >> 24) % 3]);
        }
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

    void beginDecoration() {
        shotVertexCount = uint32_t(assets.world.vertices.size());
        shotTriangleCount = assets.world.triangles.size();
    }
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
        assets.shotSurfaces.reserve(shotTriangleCount);
        assets.shotIndices.reserve(shotTriangleCount);
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

    void fire(Vec3 base, float width, float height) { assets.fires.push_back({base, width, height}); }
    void flare(Vec3 tip) { assets.roadFlares.push_back(tip); }
    void smoke(Vec3 base, float height, Vec3 drift, float size) { assets.smoke.push_back({base, height, drift, size, .07f}); }
    void mist(Vec3 base, float width, float height, Vec3 drift, float rate) {
        assets.mist.push_back({base, width, height, drift, rate});
    }

private:
    Assets& assets;
    uint16_t firstMaterial;
    bool dusk = false;
    uint32_t shotVertexCount = 0;
    size_t shotTriangleCount = 0;
    uint32_t backdropVertexFirst = 0;
    std::array<float, SurfaceCount> repeats;
    float xOffset = 0;

    void paintVehicle(PropInstance& prop, const Mesh& mesh, uint16_t id, int16_t texture, uint16_t finish) {
        if (!finish || size_t(texture) >= assets.briarBaseline.textures || id >= assets.briarBaseline.materials ||
            assets.materials[id].texture != texture ||
            std::none_of(mesh.triangles.begin(), mesh.triangles.end(), [id](const Triangle& t) { return t.material == id; })) return;

        Material material = assets.materials[id];
        const auto& source = assets.textures[texture];
        if (source.pixels.empty()) return;
        const auto unpack = [](uint16_t pixel) {
            return std::array<int, 3>{(pixel >> 11) * 255 / 31, ((pixel >> 5) & 63) * 255 / 63, (pixel & 31) * 255 / 31};
        };
        const auto luminance = [](const std::array<int, 3>& c) { return (c[0] + 2 * c[1] + c[2]) / 4; };
        const auto tint = unpack(finish);
        int64_t total = 0;
        for (const auto pixel : source.pixels) total += luminance(unpack(pixel));
        const int midtone = int(total / int64_t(source.pixels.size()));

        Texture painted{source.width, source.height, {}};
        painted.pixels.reserve(source.pixels.size());
        for (const auto pixel : source.pixels) {
            const auto c = unpack(pixel);
            const int shade = (luminance(c) - midtone) * 5 / 4;
            // Keep warm rust spots and the original texture's dents and clear-coat streaks.
            const int rust = std::clamp((c[0] - c[1] - 10) * 3, 0, 64);
            const auto channel = [&](size_t n) {
                return unsigned(std::clamp(((tint[n] + shade) * (64 - rust) + c[n] * rust) / 64, 0, 255));
            };
            painted.pixels.push_back(rgb(channel(0), channel(1), channel(2)));
        }
        material.texture = int16_t(assets.textures.size());
        assets.textures.push_back(std::move(painted));
        if (prop.paintMaterials.empty()) {
            prop.paintMaterials.resize(assets.briarBaseline.materials);
            for (size_t i = 0; i < prop.paintMaterials.size(); ++i) prop.paintMaterials[i] = uint16_t(i);
        }
        prop.paintMaterials[id] = uint16_t(assets.materials.size());
        assets.materials.push_back(material);
    }

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

    void buildMaterials() {
        for (size_t i = 0; i < colors.size(); ++i) {
            const bool twoSided = i == Glass || i == Ceiling || i == AlienTooth || i == AlienMaw ||
                                  i == ScorchEdge || i == PineBark ||
                                  i == GrassBladeDark || i == GrassBladeMid || i == GrassBladeGold;
            const bool glow = i == Amber || i == AlienGlow || i == AlienCore || i == CabinLit ||
                              i == RayCore || i == RayOvercharge;
            uint16_t baseColor = colors[i];
            if (dusk) {
                if (i == MountainRock) baseColor = rgb(126, 86, 76);  // Sunset-lit granite faces
                else if (i == ForestSlope) baseColor = rgb(62, 58, 52);    // Dusk twilight pines
                else if (i == RidgePine) baseColor = rgb(47, 65, 55);
                else if (i == Meadow) baseColor = rgb(104, 86, 58);        // Golden-ochre evening meadow
                else if (i == CabinGlass) baseColor = rgb(214, 134, 78);   // Sunset-reflecting town windows
                else if (i == Glass) baseColor = rgb(208, 136, 98);        // Warm amber-tinted estate glazing
                else if (i == GrassBladeDark) baseColor = rgb(72, 82, 44);
                else if (i == GrassBladeMid) baseColor = rgb(118, 108, 54);
                else if (i == GrassBladeGold) baseColor = rgb(172, 132, 64);
                else if (i == RoadLine) baseColor = rgb(188, 146, 68);
                else if (i == Hedge || i == YoungLeaf) baseColor = duskTint(baseColor, 14, -8, -14, 88);
                else if (i == Roof || i == GardenWall || i == Stucco || i == MonumentStone || i == Pavers ||
                         i == Gravel || i == CabinCedar || i == CabinCream || i == CabinGreen || i == CabinSlate) {
                    baseColor = duskTint(baseColor, 18, -4, -18, 92);
                }
            }
            assets.materials.push_back({baseColor, -1, uint8_t(i == Glass ? 82 : i == ScorchEdge ? 104 : 255),
                                        twoSided, glow});
        }
    }

    void buildTextures() {
        texture(Tile, 32, .5f, [](int x, int y) {
            if (x % 8 == 0 || y % 8 == 0) return rgb(118, 120, 111);
            const auto surface = grain(x, y) / 3;
            return (x / 8 + y / 8) % 2 ? rgb(181 + surface, 182 + surface, 164 + surface) :
                                       rgb(132 + surface, 149 + surface, 129 + surface);
        });
        texture(Oak, 64, .47f, [](int x, int y) {
            const int row = y / 8, board = (x + row * 8) % 32;
            if (y % 8 == 0 || board == 0) return rgb(84, 60, 42);
            const int coarse = tileNoise(x, y, 64, 8);
            const int fleck = (x + coarse * 2 + y * 9) % 19 < 3 ? -9 : 0;
            const int light = int(grain(row, board / 16)) + coarse + fleck + (y % 8 == 1 ? 9 : 0);
            return rgb(133 + light, 90 + light, 57 + light / 2);
        });
        texture(Grass, 64, .33f, [](int x, int y) {
            const int blades = int(grain(x, y)) / 2, earth = tileNoise(x, y, 64, 8);
            return rgb(74 + earth, 78 + earth * 2 + blades, 43 + earth + blades / 2);
        });
        texture(Pavers, 64, .39f, [](int x, int y) {
            const int course = y / 8, stone = (x + course % 2 * 8) % 16;
            const int edge = std::min(stone, y % 8);
            const int grit = int(grain(x, y)) / 2;
            if (edge == 0) return rgb(95 + grit, 89 + grit, 78 + grit);
            const int tint = int(grain((x + course % 2 * 8) / 16, course)) * 2 - 9;
            const int bevel = edge == 1 ? 8 : y % 8 == 7 ? -9 : 0;
            return rgb(139 + tint + grit + bevel, 126 + tint + grit + bevel, 107 + tint + grit + bevel);
        });
        texture(Stucco, 64, .36f, [](int x, int y) { return masonry(x, y, 177, 162, 131); });
        texture(GardenWall, 64, .34f, [](int x, int y) { return masonry(x, y, 125, 124, 105); });
        texture(Brick, 64, .65f, [](int x, int y) { return masonry(x, y, 128, 86, 67); });
        texture(Roof, 64, .41f, [](int x, int y) {
            const int course = y / 8, tile = (x + course % 2 * 4) % 64;
            const int grainValue = int(grain(tile / 8, course)) + int(grain(x, y)) / 3;
            const int seam = y % 8 == 0 || tile % 8 == 0 ? -24 : y % 8 == 1 ? 10 : -(y % 8);
            return rgb(73 + grainValue + seam, 83 + grainValue + seam, 93 + grainValue + seam);
        });
        texture(Hedge, 32, .85f, [](int x, int y) {
            const auto value = grain(x / 2, y / 2) * 2 + grain(x, y) / 3;
            return rgb(47 + value, 72 + value, 33 + value / 2);
        });
        texture(Timber, 32, .74f, [](int x, int y) {
            const auto value = grain(x + y / 8, 7) + grain(x, y / 3) / 4;
            return rgb(101 + value, 69 + value, 47 + value / 2);
        });
        texture(Concrete, 64, .33f, [](int x, int y) {
            const int age = tileNoise(x, y, 64, 16), grainValue = int(grain(x, y));
            const int aggregate = (grainValue == 15 ? 12 : grainValue < 2 ? -12 : 0) + age * 2;
            return rgb(115 + aggregate, 113 + aggregate, 105 + aggregate);
        });
        texture(Asphalt, 64, .48f, [](int x, int y) {
            const unsigned grainValue = grain(x, y);
            const int aggregate = tileNoise(x, y, 64, 8) + (grainValue > 13 ? 19 : grainValue / 2);
            return rgb(62 + aggregate, 66 + aggregate, 68 + aggregate);
        });
        texture(Meadow, 64, .15f, [](int x, int y) {
            const int clumps = tileNoise(x, y, 64, 8) * 2, grit = int(grain(x, y)) / 2;
            return rgb(81 + clumps + grit, 91 + clumps + grit, 51 + clumps / 2 + grit);
        });
        const auto granite = [](int x, int y) {
            const int fold = tileNoise(x, y, 64, 16) * 3;
            const int stratum = (y * 3 + x + 3 * tileNoise(x, y, 64, 8)) & 31;
            const int ledge = stratum < 3 ? -25 : stratum < 8 ? 14 : -stratum / 3;
            const int grit = int(grain(x / 3, y / 2)) + int(grain(x, y)) / 3;
            return fold + ledge + grit;
        };
        texture(MountainRock, 64, .18f, [&](int x, int y) {
            const int stone = granite(x, y);
            return rgb(111 + stone, 96 + stone, 89 + stone);
        });
        texture(AlpineScree, 32, .43f, [](int x, int y) {
            const int pebble = int(grain(x / 2, y / 2)) * 2 + int(grain(x, y)) / 3;
            const int channel = tileNoise(x, y, 32, 8) * 3;
            return rgb(76 + pebble + channel, 74 + pebble + channel, 64 + pebble + channel);
        });
        for (const Surface needles : {PineNeedles, FirTips}) {
            texture(needles, 32, .52f, [needles](int x, int y) {
                const int canopy = tileNoise(x, y, 32, 8) * 2;
                const int spray = (x + y * 3 + tileNoise(x, y, 32, 4)) & 15;
                const int twig = spray < 3 ? -11 : spray < 7 ? 11 : int(grain(x, y)) / 3;
                const int light = canopy + twig;
                return needles == FirTips ? rgb(39 + light, 70 + light, 66 + light)
                                          : rgb(47 + light, 70 + light, 44 + light);
            });
        }
        for (const Surface leaf : {AspenGold, AspenOchre, AspenAmber}) {
            texture(leaf, 32, .85f, [leaf](int x, int y) {
                const int bunch = tileNoise(x, y, 32, 8) * 3;
                const int fleck = int(grain(x / 2, y / 2)) - 7;
                const int light = bunch + fleck;
                if (leaf == AspenAmber) return rgb(184 + light, 140 + light, 52 + light / 2);
                if (leaf == AspenGold) return rgb(169 + light, 112 + light, 36 + light / 2);
                return rgb(116 + light, 77 + light, 37 + light / 2);
            });
        }
        if (dusk) {
            texture(ForestSlope, 32, .12f, [](int x, int y) {
                const int grove = 3 * tileNoise(x, y, 32, 8);
                const int fleck = (int(grain(x / 2, y / 3)) - 7) * 2;
                return rgb(43 + grove + fleck, 54 + grove + fleck, 44 + grove + fleck / 2);
            });
            texture(Scorch, 32, .49f, [](int x, int y) {
                const int soot = tileNoise(x, y, 32, 8) + int(grain(x, y)) / 3;
                const int ash = grain(x, y) == 15 && grain(x / 2, y / 3) > 9 ? 17 : 0;
                return rgb(24 + soot + ash, 23 + soot + ash, 22 + soot + ash);
            });
            texture(WreckSkin, 32, .82f, [](int x, int y) {
                const int seam = y % 16 == 0 ? -16 : y % 16 == 1 && x % 5 < 2 ? 26 : 0;
                const int heat = tileNoise(x, y, 32, 8) * 2 + int(grain(x, y)) / 2;
                return rgb(45 + heat + seam, 49 + heat + seam, 39 + heat + seam);
            });
            texture(PodSkin, 32, 1.3f, [](int x, int y) {
                const int seam = y % 16 == 0 ? -18 : y % 16 == 1 ? 18 : 0;
                const int marks = (x * 2 + y + grain(x / 5, y / 6)) % 19 == 0 ? 17 : 0;
                const int light = tileNoise(x, y, 32, 8) + seam + marks;
                return rgb(51 + light, 64 + light, 84 + light);
            });
        }
        texture(Gravel, 32, .54f, [](int x, int y) {
            const auto pebble = grain(x / 2, y / 2) * 2 + grain(x, y) / 2;
            return rgb(125 + pebble, 120 + pebble, 107 + pebble);
        });
        texture(CabinGlass, 32, .59f, [](int x, int y) {
            const int horizon = 15 + tileNoise(x, 0, 32, 8) / 3;
            const int cloud = tileNoise(x, y, 32, 8);
            if (y < horizon) return rgb(98 + cloud * 2, 137 + cloud, 159 + cloud);
            const int ridge = y < horizon + 3 ? 15 : 0;
            return rgb(78 + cloud + ridge, 106 + cloud + ridge, 118 + cloud + ridge);
        });
        for (const auto siding : {CabinCedar, CabinGreen, CabinCream, CabinSlate}) {
            texture(siding, 32, .56f, [siding](int x, int y) {
                const auto color = colors[siding];
                const int seam = y % 8 == 0 ? -21 : y % 8 == 1 ? 9 : -y % 8;
                const int light = seam + tileNoise(x, y, 32, 8) / 2 + int(grain(x / 4, y)) / 3;
                return rgb((color >> 11) * 8 + light, ((color >> 5) & 63) * 4 + light, (color & 31) * 8 + light);
            });
        }
        texture(Plaster, 32, .47f, [](int x, int y) {
            const int age = tileNoise(x, y, 32, 8) + int(grain(x, y)) / 4;
            return rgb(199 + age, 191 + age, 171 + age);
        });
        texture(Linen, 32, .68f, [](int x, int y) {
            const int weave = (x % 2 == y % 2 ? 9 : 0) + tileNoise(x, y, 32, 8) / 2;
            return rgb(212 + weave, 197 + weave, 171 + weave);
        });
        texture(Upholstery, 32, .77f, [](int x, int y) {
            const int stitch = (x + (y % 4 < 2 ? 1 : 0)) % 4 == 0 ? 14 : 0;
            const int light = stitch + tileNoise(x, y, 32, 8);
            return rgb(61 + light, 86 + light, 73 + light);
        });
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

    template<typename Sample>
    void texture(Surface material, int size, float repeat, Sample sample) {
        Texture texture{size, size, {}};
        texture.pixels.reserve(size_t(size * size));
        const bool warmExterior = dusk && (material == Grass || material == Pavers || material == Stucco ||
                                           material == GardenWall || material == Roof || material == Hedge ||
                                           material == Asphalt || material == Meadow || material == MountainRock ||
                                           material == Gravel || material == CabinGlass || material == CabinCedar ||
                                           material == CabinGreen || material == CabinCream || material == CabinSlate ||
                                           material == MonumentStone || material == Brick);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                uint16_t px = sample(x, y);
                if (warmExterior) {
                    if (material == CabinGlass) px = duskTint(px, 78, -8, -68, 96);
                    else if (material == MountainRock) px = duskTint(px, 28, -10, -24, 92);
                    else if (material == Grass || material == Meadow || material == Hedge) px = duskTint(px, 20, -8, -16, 86);
                    else px = duskTint(px, 20, -4, -20, 92);
                }
                texture.pixels.push_back(px);
            }
        }
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
    std::array<uint16_t, 2> mountainMaterials(Texture texture) {
        const uint16_t material = uint16_t(assets.materials.size());
        const auto slot = int16_t(assets.textures.size());
        assets.materials.push_back({rgb(255, 255, 255), slot, 255, false});
        // The rear range rises above the valley haze but still recedes into the sky.
        assets.materials.push_back({rgb(255, 255, 255), slot, 255, false, false, 1});
        assets.textures.push_back(std::move(texture));
        return {material, uint16_t(material + 1)};
    }

    void backdropQuad(const std::array<Vertex, 4>& vertices, uint16_t material) {
        auto& world = assets.world;
        const auto first = uint32_t(world.vertices.size());
        world.vertices.insert(world.vertices.end(), vertices.begin(), vertices.end());
        world.triangles.push_back({first, first + 2, first + 1, material, -1});
        world.triangles.push_back({first, first + 3, first + 2, material, -1});
    }

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

// In Invasion mode, wide breached openings connect Briar House out into all four town streets.
constexpr RailingRun breachedRailings[]{
    {106, 304, site.bottom, site.bottom + 5, 2, true},
    {432, 555, site.bottom, site.bottom + 5, 2, true},
    {112, 304, site.top - 5, site.top, 2, true},
    {432, 568, site.top - 5, site.top, 2, true},
    westRailings[0],
    westRailings[1],
};

void railingBases(Builder& b) {
    const auto addRun = [&](const RailingRun& run) {
        const auto span = run.section(run.start, run.end, run.inside, run.outside);
        b.prop(span, .78f, GardenWall);
        b.box(run.section(run.start, run.end, run.inside - .7f, run.outside + .7f), .72f, .81f, MonumentStone);
        b.collision(span, .78f, 2.13f, false);
        for (int i = 1; i < run.bays; ++i) {
            const float x = run.start + i * (run.end - run.start) / run.bays;
            b.prop(run.section(x - 4.25f, x + 4.25f, run.inside - 1.45f, run.outside + 1.45f), 2.13f, GardenWall);
            b.box(run.section(x - 5, x + 5, run.inside - 2.25f, run.outside + 2.25f), 2.06f, 2.19f, MonumentStone);
        }
    };
    if (b.isDusk()) {
        for (const auto& run : breachedRailings) addRun(run);
    } else {
        for (const auto& run : lawnRailings) addRun(run);
    }
}

void railingBars(Builder& b) {
    const auto addRun = [&](const RailingRun& run) {
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
    };
    if (b.isDusk()) {
        for (const auto& run : breachedRailings) addRun(run);
    } else {
        for (const auto& run : lawnRailings) addRun(run);
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

    const auto addGardenWall = [&](Rect r) {
        b.prop(r, 2.1f, GardenWall);
        b.box({r.left - .25f, r.top - .25f, r.right + .25f, r.bottom + .25f}, 1.98f, 2.13f, Trim);
    };
    addGardenWall({site.left - 5, site.top - 5, rearRailing.start, site.top});
    addGardenWall({rearRailing.end, site.top - 5, site.right + 5, site.top});
    addGardenWall({site.left - 5, site.bottom, frontRailing.start, site.bottom + 5});
    addGardenWall({frontRailing.end, site.bottom, garageBay.x, site.bottom + 5});
    addGardenWall({garageBay.endX, site.bottom, site.right + 5, site.bottom + 5});
    addGardenWall({site.left - 5, site.top, site.left, westRailings[0].start});
    addGardenWall({site.left - 5, westRailings[1].end, site.left, site.bottom});
    if (b.isDusk()) {
        // Breach wide tactical passages on the west orchard wall and east flank into town!
        addGardenWall({site.left - 5, westRailings[0].end, site.left, 165});
        addGardenWall({site.left - 5, 265, site.left, westRailings[1].start});
        addGardenWall({site.right, site.top, site.right + 5, 160});
        addGardenWall({site.right, 285, site.right + 5, site.bottom});
    } else {
        addGardenWall({site.left - 5, westRailings[0].end, site.left, westRailings[1].start});
        addGardenWall({site.right, site.top, site.right + 5, site.bottom});
    }
    railingBases(b);
    if (!b.isDusk()) {
        b.collision({garageBay.x, site.bottom, garageBay.endX, site.bottom + 5}, 0, 2.1f, false);
    }
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

Vec3 radial(float angle) { return {std::cos(angle), 0, std::sin(angle)}; }

void treeTrunk(Builder& b, Vec3 base, Vec3 upper, float width, float turn, Surface bark, Surface shade) {
    constexpr int sides = 5;
    for (int i = 0; i < sides; ++i) {
        const Vec3 left = radial(turn + i * (2 * pi / sides));
        const Vec3 right = radial(turn + (i + 1) * (2 * pi / sides));
        b.quad({base + right * width, base + left * width,
                upper + left * (width * .51f), upper + right * (width * .51f)}, i % 3 ? bark : shade);
    }
}

void pine(Builder& b, Vec3 base, float height, Surface shade, bool trunk = true) {
    constexpr float skirt[]{.19f, .34f, .50f, .65f, .77f};
    constexpr float apex[]{.55f, .71f, .84f, .94f, 1.f};
    constexpr float width[]{.22f, .189f, .145f, .106f, .073f};
    const int seed = int(base.x * 27.f) + int(base.z * 81.f);
    const float turn = grain(seed, 8) * .41f;
    const float breadth = .67f + .038f * grain(seed, 11);
    const Vec3 lean{(int(grain(seed, 9)) - 7) * .009f * height, 0, (int(grain(seed, 10)) - 7) * .009f * height};
    const auto crown = [&](float t) { return base + lean * t + Vec3{0, t * height, 0}; };
    const bool spruce = grain(seed, 27) < 7;
    if (trunk) {
        treeTrunk(b, base, crown(.55f), height * .023f, turn, PineBark, DarkWood);
    } else {
        // Crossed trunk faces keep distant crowns grounded without a full trunk mesh.
        for (int i = 0; i < 2; ++i) {
            const Vec3 edge = radial(turn + i * (pi * .5f)) * (height * .018f);
            b.face({base + edge, base - edge, crown(.53f)}, PineBark);
        }
    }

    constexpr int maxSides = 6;
    const int sides = trunk && spruce ? maxSides : 5;
    const float arc = 2 * pi / sides;
    for (int layer = 0; layer < int(std::size(skirt)); ++layer) {
        if (!trunk && (layer % 2 || (!spruce && layer == 2))) continue;
        const float rotation = turn + layer * .58f;
        const float spread = height * width[layer] * breadth * (spruce ? .86f : 1.f);
        const float skirtHeight = skirt[layer] + (spruce ? -.045f : .035f);
        const float peakHeight = !trunk && !spruce && layer == 0 ? .86f : apex[layer];
        const Vec3 peak = crown(peakHeight) + radial(rotation + .8f) * (spread * .17f);
        std::array<Vec3, maxSides> collar{};
        for (int i = 0; i < sides; ++i) {
            const float lift = .024f + .004f * grain(seed + i * 7, layer * 5);
            const float reach = .76f + .020f * grain(seed + layer * 11, i + 33);
            collar[i] = crown(skirtHeight + lift) + radial(rotation + i * arc) * (spread * reach);
        }
        for (int i = 0; i < sides; ++i) {
            const Vec3 next = collar[(i + 1) % sides];
            if (trunk) {
                const float t = rotation + (i + .52f) * arc;
                const float reach = .84f + .044f * grain(seed - i * 3, layer + 24);
                const float droop = (.010f * grain(seed + i, layer + 1) - .085f) * (1.f - .13f * layer);
                const Vec3 tip = crown(skirtHeight + droop) + radial(t) * (spread * reach);
                const Surface needles = (i + layer + int(grain(seed, 49))) % 5 < 2 ?
                                        (spruce ? FirTips : PineNeedles) : shade;
                if (layer < 3) {
                    const Vec3 branch = mix(tip, peak, .31f) + Vec3{0, height * .046f, 0};
                    b.quad({tip, collar[i], peak, branch}, needles);
                    b.quad({next, tip, branch, peak}, (i + layer) % 3 ? shade : PineShade);
                } else {
                    b.face({tip, collar[i], peak}, needles);
                    b.face({next, tip, peak}, shade);
                }
            } else {
                b.face({next, collar[i], peak}, (i + layer) % 3 ? shade : RidgePine);
            }
        }
    }
}

void aspen(Builder& b, Vec3 base, float height) {
    const int seed = int(base.x * 61.f) + int(base.z * 17.f);
    const float turn = grain(seed, 37) * .41f;
    const Vec3 lean{(int(grain(seed, 15)) - 7) * .028f, 0, (int(grain(seed, 18)) - 7) * .028f};
    const Vec3 top = base + lean + Vec3{0, height * .86f, 0};
    treeTrunk(b, base, top, height * .015f, turn, AspenBark, AspenBarkShade);

    constexpr int sides = 6;
    for (int cluster = 0; cluster < 4; ++cluster) {
        const Vec3 branch = radial(turn + cluster * 2.39996f);
        const float altitude = .49f + cluster * .107f;
        const float radius = height * (.157f - .014f * cluster);
        const Vec3 crotch = base + lean * altitude + Vec3{0, height * (altitude - .075f), 0};
        const Vec3 heart = base + lean * altitude + Vec3{0, height * altitude, 0}
                         + branch * (radius * (cluster == 3 ? .24f : .79f));
        treeTrunk(b, crotch, heart, height * .005f, turn, AspenBark, AspenBarkShade);
        const Vec3 high = heart + branch * (.19f * radius) + Vec3{0, radius * 1.16f, 0};
        const Vec3 low = heart - Vec3{0, radius * .66f, 0};
        const auto rim = [&](int corner) {
            const float angle = turn + cluster + (corner % sides) * (2 * pi / sides);
            const float reach = .86f + .022f * grain(seed + corner % sides, cluster + 13);
            return heart + radial(angle) * (radius * reach) + Vec3{0, radius * (.07f * (corner % sides) - .14f), 0};
        };
        const auto shoulder = [&](Vec3 edge) {
            return mix(heart, edge, .64f) + Vec3{0, radius * .77f, 0};
        };
        for (int i = 0; i < sides; ++i) {
            const Vec3 a = rim(i), c = rim(i + 1);
            const Vec3 bough = shoulder(a), next = shoulder(c);
            b.quad({c, a, bough, next}, (cluster + i) % 3 ? AspenGold : AspenOchre);
            b.face({next, bough, high}, AspenAmber);
            b.face({a, c, low}, (cluster + i) % 4 ? AspenOchre : AspenGold);
        }
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
        b.quad({first - fascia, next - fascia, next, first}, b.isDusk() ? Timber : Ivory);
    }
    const auto chimney = [&](float height) { return corners(h - 1.5f, plot.depth * .41f, h - .85f, plot.depth * .41f + .72f, height); };
    b.box(chimney(plot.eaves - .3f), chimney(plot.ridge + .3f), Brick);
    if (b.isDusk()) {
        frame.box({h - 1.59f, plot.depth * .41f - .09f, h - .76f, plot.depth * .41f + .81f},
                  plot.ridge + .27f, plot.ridge + .44f, MonumentStone);
        for (const float x : {-h + .015f, h - .125f}) {
            frame.panel(x, .11f, .05f, plot.eaves - .04f, -.02f, Timber);
        }
    }

    frame.panel(-.6f, 1.2f, .06f, 2.02f, -.024f, Ivory);
    frame.panel(-.53f, 1.06f, .06f, 1.93f, -.034f, DarkWood);
    frame.panel(-.35f, .7f, 1.42f, 1.73f, -.042f, CabinGlass);
    for (const float x : {-h + .66f, h - 2.14f}) {
        for (float y = .85f; y + 1.1f < plot.eaves; y += 2.16f) {
            frame.window(x, 1.47f, y, y + 1.11f, 0, Ivory);
            if (b.isDusk() && (plot.segment % 3 == 1 || (plot.frontGable && x < 0))) {
                frame.panel(x + .115f, 1.24f, y + .11f, y + 1.f, -.04f, CabinLit);
                frame.panel(x + .08f, 1.30f, y + .48f, y + .53f, -.05f, Timber);
            }
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

void home(Builder& b, const Homesite& plot, Vec3 from, Vec3 to, bool solidTown = false) {
    const Vec3 outward = acrossStreet(from, to), along{-outward.z, 0, outward.x};
    const Vec3 streetCenter = mix(from, to, plot.along);
    const HomeFrame frame{b, streetCenter + outward * plot.setback + Vec3{0, solidTown ? .02f : .54f, 0}, along, outward};
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
    if (solidTown) {
        // Solid footprint collision for every neighborhood home so players & aliens fight around them cleanly.
        const Vec3 center = frame.base + outward * (plot.depth * .5f);
        b.orientedCollision(center, along, plot.width * .5f + .15f, plot.depth * .5f + .15f, 0.f, 6.2f, true);
        // Warm porch lantern lighting the dusk street in front of the cottage.
        const Vec3 lampAt = frame.base - outward * 0.45f + Vec3{0, 2.3f, 0};
        b.lamp((lampAt.x / scale) + 316.f, 212.5f - (lampAt.z / scale), 2.3f, 7.5f, .62f);
    }
    if (plot.segment == 1 || plot.segment == 2 || plot.segment == 4 || plot.segment == 11 || plot.segment == 13) {
        mailbox({b, verge + outward * 1.35f, along, outward}, 2.35f);
    }
    if (plot.segment == 2) {
        frame.apron(verge, -3.55f, 1.45f, -.72f, .08f, Gravel);
        const Vec3 parking = verge + along * -3.55f + outward * .48f + Vec3{0, .085f, 0};
        if (!solidTown || !b.placeVehicle(PropKind::Car, parking + outward * 2.15f - Vec3{0, .06f, 0},
                                         std::atan2(-outward.z, outward.x), rgb(61, 149, 153))) {
            parkedPickup({b, parking, along, outward});
            if (solidTown) {
                b.orientedCollision(parking + outward * 1.95f, along, 0.96f, 2.0f, 0.f, 1.65f, true);
            }
        }
    }
}

void boulder(Builder& b, Vec3 center, float width, float height) {
    constexpr int sides = 6;
    const int seed = int(center.x * 51.f + center.z * 77.f);
    const float turn = grain(seed, 8) * .29f;
    const Vec3 crown = center + radial(turn) * (.25f * width) + Vec3{0, height, 0};
    std::array<Vec3, sides> base{}, ledge{};
    for (int i = 0; i < sides; ++i) {
        const Vec3 dir = radial(turn + i * (2 * pi / sides));
        base[i] = center + dir * (width * (.43f + .019f * grain(seed, i)));
        ledge[i] = mix(base[i], crown, .20f + .024f * grain(seed + i, 18));
    }
    for (int i = 0; i < sides; ++i) {
        const int next = (i + 1) % sides;
        b.quad({base[next], base[i], ledge[i], ledge[next]}, AlpineScree);
        b.face({ledge[next], ledge[i], crown}, i % 3 ? MountainRock : AlpineScree);
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

float smoothStep(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

float slopeNoise(float x, float z) {
    const int i = int(std::floor(x)), j = int(std::floor(z));
    const float u = smoothStep(x - i), v = smoothStep(z - j);
    const float a = float(grain(i, j)), b = float(grain(i + 1, j));
    const float c = float(grain(i, j + 1)), d = float(grain(i + 1, j + 1));
    return (a + (b - a) * u + (c - a + (a + d - b - c) * u) * v) * (1.f / 15.f);
}

uint16_t mountainColor(Vec3 p, float distance, bool dusk) {
    const float folds = slopeNoise(.14f * p.x + 8.f, .14f * p.z);
    const float scree = slopeNoise(p.x * .65f + p.y * .28f, p.z * .65f - p.y * .31f);
    const float grain = slopeNoise(p.x * 3.4f, p.z * 3.4f);
    // The stony seams run across adjacent faces and tilt where the rock has folded.
    const float seam = p.y * .81f + p.x * .31f - p.z * .22f + folds * 3.6f;
    const float stratum = seam - std::floor(seam);
    const float ledge = smoothStep((stratum - .14f) * 6.f) - smoothStep((stratum - .55f) * 4.f);
    Vec3 rock = mix({94, 102, 124}, {163, 141, 130}, smoothStep((folds - .17f) * 1.4f));
    rock += Vec3{1, .87f, .78f} * (24.f * scree + 13.f * grain + ledge * 19.f - 19.f);

    const float grove = slopeNoise(p.x * .26f - 9.f, p.z * .26f + 4.f);
    const float needles = slopeNoise(p.x * 1.75f, p.z * 1.75f);
    const Vec3 grass = mix({91, 100, 65}, {157, 144, 96}, scree) + Vec3{1, 1, .8f} * (grain * 14.f);
    const Vec3 evergreens = mix({38, 58, 54}, {74, 103, 79}, needles * .73f + grove * .27f);
    const float timber = smoothStep((20.f + 10.f * (grove - .5f) - p.y) * .23f) *
                         smoothStep((grove - .19f) * 3.5f);
    const float meadow = smoothStep((7.3f - p.y) * .21f);
    const float gravelCut = smoothStep((scree - .62f) * 3.0f);
    rock = mix(rock, mix(grass, evergreens, timber), std::max(meadow, timber) * (1.f - .48f * gravelCut));

    const float gullies = slopeNoise(p.x * .37f + p.y * .22f, p.z * .29f - p.y * .31f);
    const float freezeLine = 25.8f + 7.f * (folds - .5f) + 5.f * (gullies - .5f) + 1.8f * scree + 8.f * distance;
    const float frosting = smoothStep((p.y - freezeLine) * .27f);
    const float exposed = smoothStep((gullies - .46f) * 2.7f) * (.91f - .30f * frosting);
    const float snow = frosting * (1.f - exposed);
    const Vec3 ice = mix({169, 190, 213}, dusk ? Vec3{252, 229, 215} : Vec3{244, 244, 241},
                         std::clamp(.67f * folds + .26f * scree, 0.f, 1.f));
    Vec3 color = mix(rock, ice, snow);
    // Muted cool colors separate the high range from the nearer foothills.
    color = mix(color, mix({100, 121, 158}, {195, 207, 232}, snow), .46f * distance);
    return rgb(unsigned(std::clamp(color.x, 0.f, 255.f)), unsigned(std::clamp(color.y, 0.f, 255.f)),
               unsigned(std::clamp(color.z, 0.f, 255.f)));
}

void mountainSlope(Builder& b, const RoadSection& start, const RoadSection& end, size_t sector, bool lane) {
    enum Layer { LotEdge, MeadowEdge, Foothill, TreeLine, Crags, RidgeLine, SnowLine, Summit, Basin,
                 Buttress, HighPass, SkyRidge, LayerCount };
    constexpr int slices = 7;
    constexpr float skyline[]{40, 36, 48, 43, 39, 38, 43, 54, 47, 36, 46, 52, 43, 45, 38, 42, 49, 40, 39};
    static_assert(std::size(skyline) == std::size(street));
    using Profile = std::array<Vec3, LayerCount>;
    const auto profile = [&](const RoadSection& road, size_t i) -> Profile {
        const Vec3 summit = road[HighRidge];
        const Vec3 normal = normalized({summit.x - road[Ridge].x, 0, summit.z - road[Ridge].z});
        const Vec3 basin = summit + normal * 7.f - Vec3{0, 9.f, 0};
        Vec3 peak = summit + normal * (24.f + .27f * grain(int(i), 72));
        peak.y = skyline[i];
        const Vec3 buttress = mix(basin, peak, .42f) + Vec3{0, 2.5f - .38f * grain(int(i), 28), 0};
        const Vec3 highPass = mix(basin, peak, .76f) + Vec3{0, 2.5f - .31f * grain(int(i), 35), 0};
        return {road[Lots], road[UpperMeadow], road[Foothills], road[Timberline],
                mix(road[Timberline], road[Ridge], .54f), road[Ridge],
                mix(road[Ridge], summit, .52f), summit, basin, buttress, highPass, peak};
    };

    std::array<Profile, slices + 1> slope{};
    slope.front() = profile(start, sector);
    slope.back() = profile(end, (sector + 1) % std::size(skyline));
    const Vec3 outward = acrossStreet(start[Lots], end[Lots]);
    constexpr float roughness[]{0.f, .54f, 1.12f, 2.10f, 2.90f, 4.f, 5.1f, 5.9f, 3.f, 5.6f, 6.5f, 8.f};
    const int seed = int(sector) * 23;
    const float crestAt = .19f + .043f * grain(seed, 25);
    const float crestWidth = 2.35f + .16f * grain(seed, 61);
    const float crestMass = .44f + .052f * grain(seed, 81);
    for (int col = 1; col < slices; ++col) {
        const float along = float(col) / slices;
        const float envelope = 4.f * along * (1.f - along);
        const float crown = std::max(0.f, 1.f - std::abs(along - crestAt) * crestWidth);
        const float fold = (crown * 1.7f - .67f) * envelope * crestMass;
        const float rearFold = envelope * (.65f - 2.5f * std::abs(along - (1.f - crestAt)));
        for (size_t row = 0; row < LayerCount; ++row) {
            const float variation = float(grain(seed + col, int(row) + 9)) / 7.5f - 1.f;
            const float relief = lane && row == Foothill ? 0.f : roughness[row];
            const float ridge = row >= Basin ? rearFold : fold;
            slope[col][row] = mix(slope.front()[row], slope.back()[row], along)
                            + Vec3{0, relief * (.39f * variation + ridge), 0}
                            - outward * (relief * (.39f * variation + .63f * ridge));
        }
    }

    const auto spot = [&](size_t row, float along, float ascent) {
        const float x = along * slices;
        const int col = std::min(slices - 1, int(x));
        const float t = x - col;
        const Vec3 a = slope[col][row], c = slope[col + 1][row];
        const Vec3 d = slope[col][row + 1], e = slope[col + 1][row + 1];
        if (t + ascent <= 1.f) return a + (c - a) * t + (d - a) * ascent;
        return e + (d - e) * (1.f - t) + (c - e) * (1.f - ascent);
    };

    // Bake each slope in its mesh coordinates. Neighboring slopes use the same
    // world-space color fields, so timber and snow cross the sector boundaries.
    constexpr int width = 64, height = 128;
    Texture texture{width, height, {}};
    texture.pixels.reserve(size_t(width * height));
    for (int y = 0; y < height; ++y) {
        const float ridge = y * (float(SkyRidge) / (height - 1));
        const size_t row = std::min<size_t>(HighPass, size_t(ridge));
        const float distance = smoothStep((ridge - float(RidgeLine)) / (SkyRidge - RidgeLine));
        for (int x = 0; x < width; ++x) {
            const Vec3 p = spot(row, float(x) / (width - 1), ridge - row);
            texture.pixels.push_back(mountainColor(p, distance, b.isDusk()));
        }
    }
    const auto [foothillMaterial, rangeMaterial] = b.mountainMaterials(std::move(texture));
    const auto vertex = [&](int col, size_t row) {
        return Vertex{slope[col][row], (.5f + col * (float(width - 1) / slices)) / width,
                      (.5f + row * (float(height - 1) / float(SkyRidge))) / height};
    };
    for (int col = 0; col < slices; ++col) {
        for (size_t row = lane ? Foothill : LotEdge; row < SkyRidge; ++row) {
            b.backdropQuad({vertex(col, row + 1), vertex(col, row), vertex(col + 1, row), vertex(col + 1, row + 1)},
                           row >= Basin ? rangeMaterial : foothillMaterial);
        }
    }
    if (!lane) {
        const bool aspens = sector == 1 || sector == 8 || sector == 12 || sector == 16;
        const int count = sector % 2 ? 4 : 5;
        for (int i = 0; i < count; ++i) {
            const float t = .056f + .175f * i + .008f * grain(seed, i);
            const float height = 4.4f + .28f * grain(seed, i + 41);
            const Vec3 base = spot(i == 4 ? MeadowEdge : LotEdge, t, .24f + .028f * grain(seed + i, 14));
            if (aspens && i < 3) aspen(b, base - Vec3{0, .18f, 0}, height * .88f);
            else pine(b, base - Vec3{0, .23f, 0}, height, i % 3 ? RidgePine : PineNeedles);
        }
    }
    // Groves follow the lower slopes and leave open scree chutes between them.
    const int clusterSize = 4 + int(grain(seed, 37)) / 3;
    for (int i = 0; i < clusterSize; ++i) {
        const float t = .10f + .027f * grain(seed, 31) + .035f * i + .007f * grain(seed + i, 32);
        const Vec3 base = spot(Foothill, t, .12f + .047f * grain(seed + i, 38));
        const float height = 4.3f + .24f * grain(seed + i, 43);
        pine(b, base - Vec3{0, .30f, 0}, height, i % 3 ? RidgePine : PineShade, false);
    }
    if (sector % 3 != 0) {
        const Vec3 talus = spot(TreeLine, .59f + .018f * grain(seed, 7), .20f + .03f * grain(seed, 43));
        boulder(b, talus - Vec3{0, .42f, 0}, 2.2f + .13f * grain(seed, 66), .8f + .08f * grain(seed, 68));
    }
}

void mountainCommunity(Builder& b, bool playableTown = false) {
    constexpr size_t count = std::size(street);
    constexpr size_t junction = 3;
    std::array<Vec3, count> road{};
    std::array<RoadSection, count> land{};
    for (size_t i = 0; i < count; ++i) road[i] = position(street[i].x, street[i].y, playableTown ? 0.f : street[i].grade);
    for (size_t i = 0; i < count; ++i) {
        const Vec3 point = road[i];
        const Vec3 incoming = acrossStreet(road[(i + count - 1) % count], point);
        const Vec3 outgoing = acrossStreet(point, road[(i + 1) % count]);
        const Vec3 miter = (incoming + outgoing) / (1 + dot(incoming, outgoing));
        const auto offset = [&](float across, float up) { return point + miter * across + Vec3{0, up, 0}; };
        const auto plateau = [&](float halfWidth, float halfDepth) {
            const float inset = 1 / std::max(std::abs(point.x) / halfWidth, std::abs(point.z) / halfDepth);
            return Vec3{point.x * inset, playableTown ? -.015f : -.14f, point.z * inset};
        };
        const auto lots = offset(14.f, playableTown ? 0.f : .88f);
        const auto foothills = offset(25.f + .31f * grain(i, 3), street[i].foothillRise);
        const auto ridge = offset(39.f + .68f * grain(i, 7), street[i].ridgeRise);
        const float forestDepth = .26f + .022f * grain(i, 18);
        land[i] = {plateau(18.5f, 14.35f), plateau(21.7f, 18.f),
                   offset(-3.7f, playableTown ? -.012f : -.09f), offset(-2.9f, 0.f),
                   offset(2.9f, 0.f), offset(3.75f, playableTown ? -.012f : -.07f),
                   lots, mix(lots, foothills, .53f), foothills,
                   mix(foothills, ridge, forestDepth) - Vec3{0, .55f, 0}, ridge,
                   offset(55.f + .75f * grain(i, 4), street[i].summitRise)};
    }
    // First emit the playable town road, yards, homes and street cover (bullet-solid in Invasion mode).
    for (size_t i = 0; i < count; ++i) {
        const auto& start = land[i];
        const auto& end = land[(i + 1) % count];
        constexpr std::array<Surface, Lots> bands{Meadow, Meadow, Gravel, Asphalt, Gravel, Meadow};
        for (size_t j = 0; j < bands.size(); ++j) {
            if (i == junction && j >= OutsideRoad) break;
            b.quad({start[j + 1], start[j], end[j], end[j + 1]}, bands[j]);
        }
        if (playableTown) {
            // Natural foothill perimeter boundary along the outer lot line keeps players on the
            // level valley town while leaving full sightlines to the soaring Colorado ridges.
            const Vec3 a = start[Lots], c = end[Lots];
            const Vec3 mid = (a + c) * .5f;
            const Vec3 edge = c - a;
            const float halfLen = std::max(0.5f, game_detail::planar(edge) * .5f + .6f);
            const Vec3 dir = normalized({edge.x, 0.f, edge.z});
            const Vec3 out = acrossStreet(a, c);
            b.orientedCollision(mid + out * 0.6f, dir, halfLen, 0.8f, 0.f, 4.5f, false);
        }
    }

    // The estate driveway joins the paved loop at its low, level southern bend.
    b.floor({garageBay.x, site.bottom, garageBay.endX, 599}, Asphalt, .02f);
    if (playableTown) {
        // Paved connector walkways out of the breached front portico and north rear gates into the town loop.
        b.floor({304, site.bottom, 432, 600}, Pavers, .018f);
        b.floor({304, -222, 432, site.top}, Pavers, .018f);
        b.floor({-338, 165, site.left, 265}, Gravel, .016f);
        b.floor({site.right, 160, 1005, 285}, Gravel, .016f);
    }
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
    for (const auto& plot : plots) home(b, plot, road[plot.segment], road[(plot.segment + 1) % count], playableTown);

    if (playableTown) b.beginBackdrop();
    uphillLane(b, land[junction], land[junction + 1]);
    for (size_t i = 0; i < count; ++i) {
        mountainSlope(b, land[i], land[(i + 1) % count], i, i == junction);
    }
}

void gardenDetails(Builder& b) {
    railingBars(b);
    for (const float x : {garageBay.x, garageBay.endX}) {
        b.box({x - 3.7f, site.bottom - 3, x + 3.7f, site.bottom + 8}, 2.16f, 2.29f, MonumentStone);
    }
    if (!b.isDusk()) {
        constexpr float gateY = site.bottom + 2.5f;
        for (const float height : {.27f, 1.54f, 1.89f}) {
            b.box({garageBay.x + 3, gateY - .6f, garageBay.endX - 3, gateY + .6f}, height, height + .058f, Tire);
        }
        for (int i = 0; i < 13; ++i) {
            const float x = garageBay.x + 6.f * (i + 1);
            b.box({x - .35f, gateY - .4f, x + .35f, gateY + .4f}, .235f, 1.97f, Tire);
        }
        b.box({623.6f, gateY - .9f, 626.4f, gateY + .9f}, .91f, 1.06f, Trim);
    }
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
    const bool town = assets.briarHouseMode == BriarHouseMode::Invasion;
    const int columns = town ? 79 : 62;
    const int rows = town ? 53 : 49;
    const int stride = town ? 20 : 12;
    const float originX = town ? -440.f : -54.f;
    const float originY = town ? -330.f : -79.f;
    std::vector<int> grid(size_t(columns * rows), -1);
    for (int x = 0; x < columns; ++x) {
        for (int y = 0; y < rows; ++y) {
            const auto p = position(originX + float(x * stride), originY + float(y * stride));
            if (std::abs(p.x) > assets.extent.x - .6f || std::abs(p.z) > assets.extent.z - .6f) continue;
            if (world.walkBlocked(p, p)) continue;
            const auto eye = atHeight(p, 1.45f);
            const bool cover = world.blocked(eye, eye + Vec3{.8f, 0, 0}) ||
                               world.blocked(eye, eye + Vec3{-.8f, 0, 0}) ||
                               world.blocked(eye, eye + Vec3{0, 0, .8f}) ||
                               world.blocked(eye, eye + Vec3{0, 0, -.8f});
            grid[size_t(x * rows + y)] = int(assets.navNodes.size());
            assets.navNodes.push_back({p, 0, 0, cover, true});
        }
    }
    for (int x = 0; x < columns; ++x) {
        for (int y = 0; y < rows; ++y) {
            const int source = grid[size_t(x * rows + y)];
            if (source < 0) continue;
            auto& from = assets.navNodes[source];
            from.first = uint32_t(assets.navEdges.size());
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    if ((!dx && !dy) || x + dx < 0 || x + dx >= columns || y + dy < 0 || y + dy >= rows) continue;
                    const int destination = grid[size_t((x + dx) * rows + y + dy)];
                    if (destination < 0) continue;
                    const auto to = assets.navNodes[destination].position;
                    if (world.walkBlocked(from.position, to)) continue;
                    assets.navEdges.push_back({uint32_t(destination), planar(to - from.position)});
                }
            }
            from.count = uint32_t(assets.navEdges.size()) - from.first;
        }
    }

    // Drop isolated grid samples in the tiny gaps between furniture or outside the valley ring.
    int start = 0;
    const Vec3 spawn = town ? position(368, 305) : assets.spawns[0].position;
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

uint64_t meshBytes(const Mesh& mesh) {
    return allocated(mesh.vertices) + allocated(mesh.triangles) + allocated(mesh.chunks) + allocated(mesh.posePositions);
}

uint64_t arenaBytes(const Assets& a) {
    uint64_t bytes = allocated(a.shotVertices) + allocated(a.shotIndices) + allocated(a.shotSurfaces) +
                     allocated(a.shotChunks) + allocated(a.obstacles) + allocated(a.navNodes) + allocated(a.navEdges) +
                     allocated(a.lamps) + allocated(a.glassHits) + allocated(a.materials) + allocated(a.textures) +
                     allocated(a.audio) + allocated(a.fires) + allocated(a.roadFlares) + allocated(a.smoke) +
                     allocated(a.mist) + allocated(a.propInstances) +
                     allocated(a.commanderUniform) + allocated(a.enforcerUniform);
    for (const auto* mesh : {&a.world, &a.alienTrooper, &a.alienEnforcer, &a.alienDrone, &a.alienRayGun, &a.alienRayRings,
                             &a.alienHeavyRayGun, &a.alienHeavyRayRings, &a.alienUplink, &a.alienUplinkCore, &a.civilianMesh,
                             &a.relayTerminal,
                             &a.transportHelo, &a.transportRotor, &a.shipBoxTruck, &a.shipBoxCar, &a.shipBoxCrate,
                             &a.shipBoxPallet, &a.shipBoxBarrel, &a.shipBoxLamp, &a.shipBoxContainer}) {
        bytes += meshBytes(*mesh);
    }
    for (const auto& craft : a.alienFleet) bytes += meshBytes(craft);
    for (const auto& prop : a.propInstances) bytes += allocated(prop.paintMaterials);
    for (const auto* poses : {&a.alienTrooperPoses, &a.alienEnforcerPoses, &a.civilianPoses}) {
        bytes += allocated(*poses);
        for (const auto& pose : *poses) bytes += meshBytes(pose);
    }
    for (const auto& texture : a.textures) bytes += allocated(texture.pixels);
    for (const auto& clip : a.audio) bytes += allocated(clip.samples);
    for (const auto& uniform : a.civilianUniforms) bytes += allocated(uniform);
    return bytes;
}

void roadFlare(Builder& b, float x, float y) {
    b.box({x - 2.2f, y - 0.8f, x + 2.2f, y + 0.8f}, .02f, .07f, AlienCore);
    b.flare(position(x + 2.f, y, .075f));
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

// Ground aliens face +z with their feet at y = 0.
struct CreaturePose {
    float phase = 0;   // Lope cycle 0..1
    bool stride = false;
    float breathe = 0; // Idle chest swell 0..1
    float jaw = 1;     // Mouth opening 0..1
    float writhe = 0;  // Tendril and tongue wave, radians
    float aim = 0;     // 0 = low-ready carry, 1 = raised two-hand ray gun aim
    float recoil = 0;  // 0..1 ray gun firing kickback
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
    void tube(std::initializer_list<Vec3> p, std::initializer_list<float> ru, std::initializer_list<float> rv, int sides,
              Vec3 ref, uint16_t mat, bool capStart = true) {
        loft(p.begin(), ru.begin(), rv.begin(), p.size(), sides, ref, mat, capStart);
    }
    template<size_t N>
    void tube(const std::array<Vec3, N>& p, const std::array<float, N>& ru, const std::array<float, N>& rv, int sides,
              Vec3 ref, uint16_t mat, bool capStart = true) {
        loft(p.data(), ru.data(), rv.data(), N, sides, ref, mat, capStart);
    }

private:
    // Keep only the two rings being stitched. A zero end radius closes the loft to a point.
    void loft(const Vec3* p, const float* ru, const float* rv, size_t n, int sides,
              Vec3 ref, uint16_t mat, bool capStart) {
        ++part;
        std::array<Vec3, 10> angles{}, previous{}, current{};
        assert(n > 1 && sides >= 3 && size_t(sides) <= angles.size());
        for (int k = 0; k < sides; ++k) {
            const float a = 2 * pi * k / sides;
            angles[k] = {std::cos(a), std::sin(a), 0};
        }
        const auto ring = [&](size_t i, auto& out) {
            const Vec3 t = normalized(p[std::min(i + 1, n - 1)] - p[i > 0 ? i - 1 : 0]);
            Vec3 u = ref - t * dot(ref, t);
            if (length(u) < 1e-3f) u = std::abs(t.y) < .9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            u = normalized(u);
            const Vec3 v = cross(t, u);
            for (int k = 0; k < sides; ++k) {
                out[k] = p[i] + u * (angles[k].x * ru[i]) + v * (angles[k].y * rv[i]);
            }
        };
        ring(0, previous);
        const auto first = previous;
        const bool tip = ru[n - 1] <= 0 && rv[n - 1] <= 0;
        for (size_t i = 1; i < n; ++i) {
            const bool closing = tip && i == n - 1;
            if (!closing) ring(i, current);
            const Vec3 inside = (p[i - 1] + p[i]) * .5f;
            for (int k = 0; k < sides; ++k) {
                const int k1 = (k + 1) % sides;
                if (closing) tri(previous[k], previous[k1], p[i], inside, mat);
                else quad(previous[k], previous[k1], current[k1], current[k], inside, mat);
            }
            previous = current;
        }
        if (capStart) {
            for (int k = 0; k < sides; ++k) tri(p[0], first[(k + 1) % sides], first[k], p[1], mat);
        }
    }

public:
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

Vec3 writeCreature(CreatureWriter& w, const CreaturePose& q, uint16_t skin, uint16_t sinew, uint16_t tooth,
                   uint16_t maw, uint16_t gum, uint16_t eye, bool armored) {
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
        const Vec3 hip{s * .22f, 0, -.04f};
        const Vec3 knee = hip + Vec3{s * (q.stride ? .09f : .22f), -std::cos(thigh), std::sin(thigh)} * .5f;
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

    // The face fills the front of the torso. Its perimeter, lips and throat share edges.
    const auto chestAt = [&](Vec3 p) { return Vec3{p.x * puff, hipY + p.y, p.z * puff}; };
    const Vec3 chest = chestAt({0, .53f, .07f});
    w.tube({chestAt({0, -.13f, -.05f}), chestAt({0, .04f, -.03f}), chestAt({0, .30f, -.02f})},
           {.20f, .31f, .29f}, {.14f, .21f, .19f}, 7, side, skin);
    const std::array<Vec3, 8> shell{{{-.37f, .79f, .19f}, {0, .83f, .26f}, {.37f, .79f, .19f},
        {.54f, .58f, .12f}, {.34f, .29f, .14f}, {0, .11f, .21f}, {-.34f, .29f, .14f}, {-.54f, .58f, .12f}}};
    const float open = .52f + .48f * q.jaw;
    const std::array<Vec3, 8> lip{{{-.12f, .58f, .41f}, {0, .69f, .46f}, {.12f, .58f, .41f},
        {.184f, .48f, .40f}, {.13f, .40f - .13f * open, .43f}, {0, .38f - .24f * open, .45f},
        {-.13f, .40f - .13f * open, .43f}, {-.184f, .48f, .40f}}};
    const Vec3 spine = chestAt({0, .52f, -.25f}), throat = chestAt({0, .46f, .25f});
    for (size_t i = 0; i < shell.size(); ++i) {
        const size_t j = (i + 1) % shell.size();
        const Vec3 a = chestAt(shell[i]), b = chestAt(shell[j]);
        const Vec3 c = chestAt(lip[j]), d = chestAt(lip[i]);
        const Vec3 innerC = c + (throat - c) * .20f, innerD = d + (throat - d) * .20f;
        w.tri(b, a, spine, chest, skin);
        w.quad(a, b, c, d, chest, i == 0 || i == 1 || i == 4 || i == 5 ? sinew : skin);
        w.quad(d, c, innerC, innerD, chest, gum);
        w.tri(innerD, innerC, throat, chest, maw);
    }
    // Inward-hooked teeth follow the two chest jaws instead of forming a circular grille.
    for (size_t i = 0; i < lip.size(); ++i) {
        if (i == 0 || i == 1) continue;
        const Vec3 d = chestAt(lip[i]), c = chestAt(lip[(i + 1) % lip.size()]);
        for (float f : {.28f, .72f}) {
            const Vec3 root = d + (c - d) * f + Vec3{0, 0, .012f};
            const Vec3 tip = root + (throat - root) * (f < .5f ? .54f : .36f) + Vec3{0, 0, .047f};
            w.spike(root, tip, c - d, .022f, tooth);
        }
    }
    for (float s : {-1.f, 1.f}) {
        const Vec3 hinge = chestAt({s * .25f, .51f, .30f});
        w.tube({hinge, chestAt({s * .25f, .46f, .42f}), chestAt({s * .19f, .31f - .1f * open, .46f}),
                chestAt({s * .11f, .27f - .12f * open, .48f})},
               {.085f, .074f, .052f, 0}, {.08f, .066f, .042f, 0}, 5, side, skin);
        w.spike(chestAt({s * .145f, .54f, .42f}), chestAt({s * .08f, .41f, .50f}), side, .035f, tooth);

        // The carapace forms a raised shoulder and shades both pairs of eyes.
        w.tube({chestAt({s * .19f, .75f, -.19f}), chestAt({s * .39f, .79f, -.05f}),
                chestAt({s * .58f, .62f, .08f}), chestAt({s * .66f, .55f, .10f})},
               {.16f, .22f, .16f, 0}, {.16f, .23f, .14f, 0}, 7, up, sinew);
        for (const Vec3 spot : {Vec3{.119f, .681f, .429f}, Vec3{.263f, .617f, .362f}}) {
            const Vec3 center = chestAt({s * spot.x, spot.y, spot.z});
            const Vec3 across{s * .059f, .034f, -.031f}, lid{0, .020f, -.012f};
            w.quad(center - across * 1.2f, center - lid * 1.75f, center + across * 1.2f, center + lid * 1.75f, chest, sinew);
            const Vec3 pupil = center + Vec3{0, 0, .014f};
            w.quad(pupil - across, pupil - lid, pupil + across, pupil + lid, chest, eye);
        }
        for (int i = 0; i < 3; ++i) {
            const Vec3 root = chestAt({s * (.32f + .044f * i), .34f + .071f * i, .26f - .02f * i});
            w.spike(root, root + Vec3{s * .13f, .063f, -.08f}, up, .045f, armored ? tooth : skin);
        }
    }

    for (int l = 0; l < 2; ++l) {
        const auto& leg = legs[l];
        const float s = l == 0 ? -1.f : 1.f;
        w.tube({leg.hip + Vec3{0, .07f, 0}, leg.hip, (leg.hip + leg.knee) * .5f, leg.knee,
                (leg.knee + leg.ankle) * .5f, leg.ankle},
               {.17f, .18f, .17f, .11f, .1f, .075f}, {.17f, .19f, .18f, .12f, .11f, .08f}, 6, ahead, skin);
        const Vec3 kneePlate = leg.knee + Vec3{s * .04f, .02f, .106f};
        const Vec3 bootPlate = leg.ankle + Vec3{s * .04f, .08f, .061f};
        w.tube({kneePlate, kneePlate + (bootPlate - kneePlate) * .50f, bootPlate},
               {.089f, .078f, 0}, {.042f, .048f, 0}, 5, side, sinew);
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

    // Long, heavy arms: the right arm wields the alien ray gun (low-ready at idle/stride,
    // raised forward when aiming, kicking back on recoil), while the left arm balances or
    // reaches across to brace beneath the emitter barrel during aimed ray shots.
    const auto armJoints = [&](int a, Vec3& shoulder, Vec3& elbow, Vec3& wrist) {
        const float s = a == 0 ? -1.f : 1.f;
        const float swing = q.stride ? -std::sin(cycle) * (a == 0 ? 1.f : -1.f) : std::sin(q.writhe * .5f + a) * .12f;
        shoulder = {s * .52f, hipY + .64f, .1f};
        if (a == 1) {
            // Right gun-hand: carries low-ready while loping, snaps up to shoulder-aim when firing.
            const Vec3 readyElbow = shoulder + Vec3{.16f, -.28f, .14f + .08f * swing};
            const Vec3 readyWrist = readyElbow + Vec3{-.10f, .02f + .04f * q.breathe, .34f + .06f * swing};
            const Vec3 aimElbow = shoulder + Vec3{.06f, -.16f + .06f * q.recoil, .26f - .08f * q.recoil};
            const Vec3 aimWrist = {0.34f, hipY + .54f + .10f * q.recoil + .03f * q.breathe, .60f - .12f * q.recoil};
            elbow = readyElbow + (aimElbow - readyElbow) * q.aim;
            wrist = readyWrist + (aimWrist - readyWrist) * q.aim;
        } else {
            // Left arm: wide predatory swing during stride, sweeps inward to brace under the barrel when aiming.
            const Vec3 freeElbow = shoulder + Vec3{-.22f, -.3f, .06f + .14f * swing};
            const Vec3 freeWrist = freeElbow + Vec3{-.1f, -.1f + .06f * q.breathe, .32f + .1f * swing};
            const Vec3 braceElbow = shoulder + Vec3{.08f, -.32f + .04f * q.recoil, .28f - .06f * q.recoil};
            const Vec3 braceWrist = {.27f, hipY + .37f + .09f * q.recoil, .78f - .10f * q.recoil};
            elbow = freeElbow + (braceElbow - freeElbow) * q.aim;
            wrist = freeWrist + (braceWrist - freeWrist) * q.aim;
        }
    };
    Vec3 grip{};
    for (int a = 0; a < 2; ++a) {
        const float s = a == 0 ? -1.f : 1.f;
        Vec3 shoulder, elbow, wrist;
        armJoints(a, shoulder, elbow, wrist);
        w.tube({shoulder, shoulder + (elbow - shoulder) * .45f, elbow, elbow + (wrist - elbow) * .4f, wrist},
               {.14f, .14f, .1f, .11f, .07f}, {.13f, .13f, .095f, .1f, .065f}, 6, ahead, skin);
        const Vec3 dir = normalized(wrist - elbow);
        const Vec3 knuckle = wrist + dir * .12f;
        if (a == 1) grip = wrist + dir * .075f + Vec3{0, .028f, 0};
        const Vec3 across = normalized(cross(dir, up)) * s;
        w.tube({wrist, knuckle}, {.075f, .07f}, {.04f, .04f}, 5, across, sinew);
        for (int c = 0; c < 4; ++c) {
            const float spread = (c - 1.5f) * .04f;
            const Vec3 root = knuckle + across * spread;
            const Vec3 curl = dir * .12f + Vec3{0, -.06f - .015f * std::abs(c - 1.5f), 0} + across * (spread * .7f);
            w.spike(root, root + curl + dir * .08f + Vec3{0, -.06f, 0}, across, .02f, tooth);
        }
    }

    // Three rear feelers rise behind the carapace, leaving the shoulders as the silhouette.
    const struct { Vec3 root, dir, curl; float length; } tendrils[]{
        {{-.32f, .80f, -.17f}, {-.44f, .8f, -.6f}, {-.5f, -.35f, 0}, .53f},
        {{.30f, .82f, -.18f}, {.58f, .95f, -.53f}, {.5f, -.25f, 0}, .62f},
        {{0, .73f, -.31f}, {.15f, .4f, -1}, {0, -.5f, 0}, .57f},
    };
    for (size_t t = 0; t < std::size(tendrils); ++t) {
        const auto& spec = tendrils[t];
        std::array<Vec3, 5> joints{spec.root + Vec3{0, hipY, 0}};
        Vec3 dir = normalized(spec.dir);
        for (int k = 1; k <= 4; ++k) {
            const float wave = std::sin(q.writhe + t * 1.3f + k * .8f) * .35f;
            dir = normalized(dir + spec.curl * .3f + cross(dir, up) * wave);
            joints[k] = joints[k - 1] + dir * (spec.length * .25f);
        }
        w.tube(joints, {.08f, .058f, .04f, .022f, 0}, {.07f, .052f, .036f, .02f, 0}, 5, chest - joints[0], sinew, false);
    }

    if (!armored) return grip;
    // Bone armor follows the shoulder line, forearms and spine.
    for (int a = 0; a < 2; ++a) {
        const float s = a == 0 ? -1.f : 1.f;
        for (int k = 0; k < 2; ++k) {
            const Vec3 root{s * (.32f + .12f * float(k)), hipY + .84f - .05f * float(k), .08f - .08f * float(k)};
            w.spike(root, root + Vec3{s * (.16f + .06f * float(k)), .3f - .08f * float(k), -.12f}, ahead, .055f, tooth);
        }
        Vec3 shoulder, elbow, wrist;
        armJoints(a, shoulder, elbow, wrist);
        const Vec3 mid = elbow + (wrist - elbow) * .35f + Vec3{s * .06f, .04f, 0};
        w.spike(mid, mid + (elbow - wrist) * .55f + Vec3{s * .14f, .1f, 0}, up, .045f, tooth);
    }
    for (int k = 0; k < 4; ++k) {
        const Vec3 root{0, hipY + .3f + .14f * float(k), -.18f - .02f * float(k)};
        w.spike(root, root + Vec3{0, .1f, -.2f - .03f * float(k)}, side, .045f, tooth);
    }
    return grip;
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
    scratch.reserve(raw);
    auto& first = frames.front().posePositions;
    first.reserve(base.vertices.size());
    for (const auto& v : base.vertices) first.push_back(v.position);
    for (size_t p = 1; p < poses.size(); ++p) {
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
    // 0..1: Idle breaths (holding ray gun at low-ready)
    for (int i = 0; i < alienIdlePoses; ++i) poses.push_back({0, false, float(i), .65f + .25f * i, 1.7f * i, .15f, 0});
    // 2..7: Predatory lope stride carrying ray gun forward
    for (int i = 0; i < alienStridePoses; ++i) {
        const float phase = float(i) / alienStridePoses;
        poses.push_back({phase, true, 0, .75f + .15f * std::sin(4 * pi * phase), 2 * pi * phase, .28f, 0});
    }
    // 8..9: Aim & Charge (two-handed ray gun brace, jaw snarling, tendrils flared)
    for (int i = 0; i < alienAimPoses; ++i) {
        poses.push_back({0, false, .35f + .55f * i, .88f + .12f * i, 2.2f * i + .6f, 1.f, 0});
    }
    // 10..11: Fire & Recoil (snappy kickback recoil & roaring jaw)
    for (int i = 0; i < alienRecoilPoses; ++i) {
        poses.push_back({0, false, 1.f - .3f * i, 1.f, 3.1f + 1.4f * i, 1.f, 1.f - .35f * i});
    }
    for (const bool armored : {false, true}) {
        buildPosedMesh(armored ? assets.alienEnforcer : assets.alienTrooper,
                       armored ? assets.alienEnforcerPoses : assets.alienTrooperPoses, poses,
                       [&](const CreaturePose& pose, std::vector<Vertex>& vertices, std::vector<Triangle>* triangles) {
            CreatureWriter w(vertices, triangles);
            assets.alienGunSockets[size_t(&pose - poses.data())] =
                writeCreature(w, pose, mat(AlienSkin), mat(AlienSinew), mat(AlienTooth), mat(AlienMaw), mat(AlienGum),
                              mat(AlienGlow), armored);
        });
    }
}

// Retro-futuristic alien ray guns: sleek bio-metallic receiver, glowing plasma capacitor bulb,
// forked emitter prongs, and separate concentric accelerator rings that spin in the renderer.
// The grip is the origin and the barrel fires along +z from the shared bore.
void buildRayGuns(Assets& assets, uint16_t firstMat) {
    const auto mat = [&](Surface s) { return uint16_t(firstMat + s); };
    const Vec3 up{0, 1, 0}, side{1, 0, 0};

    const auto makeGun = [&](Mesh& body, Mesh& rings, bool heavy) {
        body = {};
        rings = {};
        const float len = rayLength(heavy);
        const float wide = heavy ? 1.24f : 1.0f;
        const uint16_t metal = mat(RayMetal);
        const uint16_t hull = mat(AlienHull);
        const uint16_t trim = mat(AlienTrim);
        const uint16_t coil = mat(RayCoil);
        const uint16_t glow = heavy ? mat(AlienCore) : mat(AlienGlow);
        const uint16_t core = heavy ? mat(RayOvercharge) : mat(RayCore);

        {
            CreatureWriter w(body.vertices, &body.triangles);
            // Swept ergonomic grip tucked into the alien's right claw.
            w.tube({{0, -.18f, -.08f}, {0, -.06f, -.03f}, {0, .05f, .02f}},
                   {.032f * wide, .038f * wide, .042f * wide}, {.042f, .048f, .052f}, 6, side, hull);
            // Sculpted teardrop receiver housing tapering smoothly into the accelerator barrel.
            w.tube({{0, .08f, -.22f * len}, {0, .08f, -.08f * len}, {0, .085f, .10f * len},
                    {0, .085f, .30f * len}, {0, .08f, .54f * len}, {0, .08f, .76f * len}},
                   {.045f * wide, .092f * wide, .086f * wide, .056f * wide, .038f * wide, .024f * wide},
                   {.052f * wide, .105f * wide, .095f * wide, .062f * wide, .040f * wide, .026f * wide},
                   8, up, metal);
            // A bright capacitor sits in a slotted metal cage above the receiver.
            const auto capacitor = [&](float z) { return Vec3{0, .185f * wide, z * len}; };
            w.tube({capacitor(-.22f), capacitor(-.17f), capacitor(.16f), capacitor(.21f)},
                   {.026f * wide, .069f * wide, .069f * wide, .026f * wide},
                   {.026f * wide, .069f * wide, .069f * wide, .026f * wide}, 6, up, glow);
            w.tube({capacitor(-.235f), capacitor(-.20f), capacitor(-.15f)},
                   {.039f * wide, .081f * wide, .075f * wide},
                   {.039f * wide, .081f * wide, .075f * wide}, 6, up, coil);
            w.tube({capacitor(.13f), capacitor(.17f), capacitor(.222f)},
                   {.077f * wide, .082f * wide, .037f * wide},
                   {.077f * wide, .082f * wide, .037f * wide}, 6, up, trim);
            for (float s : {-1.f, 1.f}) {
                const Vec3 offset{s * .045f * wide, .056f * wide, 0};
                const Vec3 lip{s * .012f * wide, -.018f, 0};
                w.quad(capacitor(-.19f) + offset, capacitor(.17f) + offset, capacitor(.17f) + offset + lip,
                       capacitor(-.19f) + offset + lip, capacitor(0), hull);
                const Vec3 back{s * .081f * wide, .035f, -.17f * len}, front{s * .058f * wide, .063f, .33f * len};
                const Vec3 ridge{s * .125f * wide, .075f, .005f * len}, ridgeUp = ridge + Vec3{0, .054f, 0};
                w.quad(back, front, ridge, ridgeUp, {0, .08f, 0}, trim);
                w.tri(back + Vec3{0, .098f, .025f}, ridgeUp, front, {0, .08f, 0}, coil);
                const Vec3 track = ridge + Vec3{s * .003f, .019f, 0};
                w.quad(track + Vec3{0, 0, -.056f}, track + Vec3{0, .014f, .064f},
                       front + Vec3{s * .009f, .018f, -.040f}, front + Vec3{s * .009f, .007f, -.040f},
                       {0, .08f, 0}, core);
                if (heavy) {
                    w.spike({s * .109f * wide, .11f, .02f}, {s * .19f, .23f, -.25f}, up, .041f, trim);
                    w.spike({s * .110f * wide, .04f, .22f}, {s * .16f, -.045f, .57f}, up, .035f, coil);
                }
            }
            // A coaxial emitter collar and straight rails mark the actual firing axis.
            const Vec3 bore{0, rayBoreHeight, 0};
            const auto axial = [&](float z) { return bore + Vec3{0, 0, z * len}; };
            w.tube({axial(.56f), axial(.66f), axial(.80f), axial(.90f)},
                   {.047f * wide, .091f * wide, .083f * wide, .064f * wide},
                   {.049f * wide, .091f * wide, .083f * wide, .064f * wide}, 8, up, trim);
            w.tube({axial(.70f), axial(.79f), axial(.90f), axial(rayMuzzle)},
                   {.036f * wide, .053f * wide, .040f * wide, 0},
                   {.036f * wide, .053f * wide, .040f * wide, 0}, 6, up, core);
            const int prongs = heavy ? 4 : 3;
            for (int p = 0; p < prongs; ++p) {
                const float a = 2 * pi * p / prongs + pi * .25f;
                const Vec3 radial{std::cos(a), std::sin(a), 0};
                const Vec3 root = axial(.67f) + radial * (.076f * wide);
                const Vec3 knuckle = axial(.79f) + radial * (.119f * wide);
                const Vec3 tip = axial(rayMuzzle) + radial * (.093f * wide);
                w.tube({root, knuckle, tip}, {.022f * wide, .027f * wide, 0},
                       {.025f * wide, .026f * wide, 0}, 4, radial, coil);
                w.spike(knuckle + radial * (.013f * wide), tip, radial, .016f * wide, core);
            }
            finalizeProceduralMesh(body);
            for (auto& v : body.vertices) v.u = v.v = 0;
        }
        {
            // Spinning concentric Tesla-style accelerator rings centered at {0, 0, z} (mounted at barrel axis {0, 0.08, 0}).
            CreatureWriter w(rings.vertices, &rings.triangles);
            const int ringCount = heavy ? 3 : 2;
            for (int r = 0; r < ringCount; ++r) {
                const float frac = float(r) / float(ringCount - 1);
                const float z = (.28f + .36f * frac) * len;
                const float rad = (.125f - .036f * frac) * wide;
                const float thick = .015f * wide;
                constexpr int segs = 10;
                std::array<Vec3, segs + 1> loop{};
                std::array<float, segs + 1> ru{}, rv{};
                ru.fill(thick);
                rv.fill(thick * 1.15f);
                for (int k = 0; k <= segs; ++k) {
                    const float a = 2 * pi * k / segs;
                    loop[k] = {std::cos(a) * rad, std::sin(a) * rad, z};
                }
                w.tube(loop, ru, rv, 4, Vec3{0, 0, 1}, (r & 1) ? glow : coil, false);
                // Flared contacts make the accelerator's rotation visible at a distance.
                for (int f = 0; f < 3; ++f) {
                    const float a = (2 * pi * f / 3.f) + float(r) * .7f;
                    const Vec3 dir{std::cos(a), std::sin(a), 0};
                    w.spike(Vec3{dir.x * rad, dir.y * rad, z},
                            Vec3{dir.x * (rad + .042f * wide), dir.y * (rad + .042f * wide), z + .02f},
                            Vec3{0, 0, 1}, .014f * wide, core);
                }
            }
            finalizeProceduralMesh(rings);
            for (auto& v : rings.vertices) v.u = v.v = 0;
        }
    };

    makeGun(assets.alienRayGun, assets.alienRayRings, false);
    makeGun(assets.alienHeavyRayGun, assets.alienHeavyRayRings, true);
}

// Unarmed civilians in everyday clothes: cowering with hands over the head while pinned,
// running for the back door once rescued, and standing hugging themselves when safe.
enum class CivilianMode { Cower, Run, Stand };
struct CivilianPose {
    CivilianMode mode = CivilianMode::Cower;
    float phase = 0;   // Run cycle 0..1
    float shiver = 0;  // Cower tremble 0..1
};

void writeCivilian(CreatureWriter& w, const CivilianPose& q, uint16_t firstMat) {
    const auto mat = [&](Surface s) { return uint16_t(firstMat + s); };
    const auto skin = mat(CivSkin), hair = mat(CivHair), top = mat(CivTop), pants = mat(CivPants), shoes = mat(CivShoes);
    const Vec3 up{0, 1, 0}, side{1, 0, 0}, ahead{0, 0, 1};
    struct { Vec3 hip, knee, ankle, toe, shoulder, elbow, wrist, hand; } limb[2];
    Vec3 pelvis, waist, chest, shoulders, neck, head, headUp;
    if (q.mode == CivilianMode::Cower) {
        const float t = q.shiver * .025f;
        for (int l = 0; l < 2; ++l) {
            const float s = l == 0 ? -1.f : 1.f;
            limb[l].ankle = {s * .15f, .08f, -.05f};
            limb[l].toe = {s * .16f, .03f, .14f};
            limb[l].knee = {s * .18f, .5f, .2f};
            limb[l].hip = {s * .11f, .3f, -.15f};
            limb[l].shoulder = {s * .2f, .78f + t, .08f};
            limb[l].elbow = {s * .25f, .92f + t, .27f};
            limb[l].wrist = {s * .12f, 1.02f + t, .24f};
            limb[l].hand = {s * .072f, 1.045f + t, .17f};
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
        const float lean = run ? .13f : 0.f;
        const float breath = run ? 0.f : q.shiver * .024f;
        for (int l = 0; l < 2; ++l) {
            const float s = l == 0 ? -1.f : 1.f;
            auto& leg = limb[l];
            leg.hip.y += hipY;
            leg.knee.y += hipY;
            leg.ankle.y += hipY;
            leg.toe = {leg.ankle.x, std::max(.03f, leg.ankle.y - .05f), leg.ankle.z + .17f};
            leg.shoulder = {s * .21f, hipY + .54f + breath, .02f + lean};
            if (run) {
                const float swing = -std::sin(cycle) * (l == 0 ? 1.f : -1.f);
                leg.elbow = leg.shoulder + Vec3{s * .03f, -.24f, -.12f * swing};
                leg.wrist = leg.elbow + Vec3{-s * .02f, .04f, .2f - .08f * swing};
            } else {
                leg.elbow = leg.shoulder + Vec3{s * .07f, -.26f, .02f};
                leg.wrist = {s * .18f, hipY + .09f + breath, .16f};
            }
            leg.hand = leg.wrist + normalized(leg.wrist - leg.elbow) * .08f;
        }
        pelvis = {0, hipY - .02f, 0};
        waist = {0, hipY + .18f, .01f + lean * .3f};
        chest = {0, hipY + .41f + breath, .03f + lean * .7f};
        shoulders = {0, hipY + .54f + breath, .02f + lean};
        neck = {0, hipY + .64f + breath, .03f + lean};
        head = {0, hipY + .76f + breath, .04f + lean * 1.1f};
        headUp = normalized(Vec3{run ? 0.f : q.shiver * .08f, 1, lean * .78f});
    }

    // Torso in the top colour, hips in the trouser colour.
    w.tube({pelvis + Vec3{0, -.04f, 0}, pelvis, waist}, {.16f, .17f, .15f}, {.1f, .11f, .1f}, 8, side, pants);
    w.tube({waist, chest, shoulders, neck}, {.155f, .19f, .2f, .07f}, {.1f, .12f, .11f, .065f}, 8, side, top);
    w.tube({neck, neck + (head - neck) * .5f}, {.05f, .05f}, {.05f, .05f}, 6, side, skin);
    // Brow, cheek and jaw planes give a recognizable face at the handheld resolution.
    const Vec3 face = normalized(cross(side, headUp));
    const Vec3 faceSide = cross(headUp, face);
    const auto facial = [&](float x, float y, float z) { return head + faceSide * x + headUp * y + face * z; };
    const auto facePatch = [&](float x, float y, float z, float hw, float hh, uint16_t material) {
        w.quad(facial(x - hw, y - hh, z), facial(x + hw, y - hh, z), facial(x + hw, y + hh, z),
               facial(x - hw, y + hh, z), head, material);
    };
    w.tube({facial(0, -.107f, .016f), facial(0, -.056f, .006f), facial(0, .029f, -.002f), facial(0, .104f, -.01f)},
           {.063f, .085f, .083f, 0}, {.054f, .086f, .089f, 0}, 8, faceSide, skin);
    w.tube({head + headUp * .01f - face * .025f, head + headUp * .07f - face * .02f, head + headUp * .13f - face * .015f},
           {.093f, .086f, 0}, {.071f, .083f, 0}, 8, faceSide, hair, false);
    w.spike(facial(0, -.018f, .075f), facial(0, -.043f, .115f), faceSide, .023f, skin);
    facePatch(0, -.064f, .087f, .025f, .0045f, mat(CivFeatures));
    w.tube({facial(0, .041f, -.012f), facial(0, .069f, -.012f), facial(0, .09f, -.014f)},
           {.098f, .101f, .096f}, {.101f, .104f, .100f}, 8, faceSide, mat(CivCapBand), false);
    w.tube({facial(0, .073f, -.012f), facial(0, .125f, -.014f), facial(0, .153f, -.019f)},
           {.097f, .087f, 0}, {.101f, .086f, 0}, 8, faceSide, mat(CivCap), false);
    facePatch(0, .005f, .103f, .014f, .004f, mat(CivGlasses));
    for (int l = 0; l < 2; ++l) {
        const float s = l == 0 ? -1.f : 1.f;
        facePatch(s * .039f, .012f, .079f, .020f, .008f, mat(Ivory));
        facePatch(s * .038f, .011f, .082f, .009f, .009f, mat(CivFeatures));
        facePatch(s * .042f, .028f, .081f, .023f, .005f, hair);
        const auto glasses = mat(CivGlasses);
        for (const float peak : {-.014f, .022f}) facePatch(s * .04f, peak, .103f, .030f, .0045f, glasses);
        for (const float edge : {.013f, .067f}) facePatch(s * edge, .004f, .103f, .004f, .018f, glasses);
        w.spike(facial(s * .08f, -.030f, -.008f), facial(s * .101f, -.019f, -.006f),
                headUp, .022f, skin);

        // Contrasting lapels and pockets are placed on the jacket front.
        const Vec3 upper = shoulders + ahead * .082f;
        const Vec3 lower = chest + ahead * .124f;
        w.tri(upper + Vec3{s * .04f, .04f, .015f}, upper + Vec3{s * .133f, 0, .013f},
              lower + Vec3{s * .03f, 0, 0}, chest, mat(CivFabric));
        const Vec3 pocket = lower + Vec3{s * .11f, -.089f, -.018f};
        w.quad(pocket + Vec3{-.04f, .03f, .002f}, pocket + Vec3{.04f, .03f, .002f},
               pocket + Vec3{.036f, -.057f, -.006f}, pocket + Vec3{-.036f, -.057f, -.006f}, chest, mat(CivFabric));
        w.quad(pocket + Vec3{-.04f, .024f, .006f}, pocket + Vec3{.04f, .024f, .006f},
               pocket + Vec3{.04f, .012f, .006f}, pocket + Vec3{-.04f, .012f, .006f}, chest, mat(CivTrim));

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
    std::vector<CivilianPose> poses;
    for (int i = 0; i < civilianCowerPoses; ++i) poses.push_back({CivilianMode::Cower, 0, float(i)});
    for (int i = 0; i < civilianRunPoses; ++i) poses.push_back({CivilianMode::Run, float(i) / civilianRunPoses, 0});
    for (int i = 0; i < civilianStandPoses; ++i) poses.push_back({CivilianMode::Stand, 0, float(i)});
    buildPosedMesh(assets.civilianMesh, assets.civilianPoses, poses,
                   [&](const CivilianPose& pose, std::vector<Vertex>& vertices, std::vector<Triangle>* triangles) {
        CreatureWriter w(vertices, triangles);
        writeCivilian(w, pose, firstMat);
    });
}

// A utility helicopter in two pieces: the airframe (groups 0 and 1, cabin and tail) and the
// main rotor (group 2, centred on the mast head at y = 0). Built facing +z, wheels at y = 0.
struct HeloMats { uint16_t olive, dark, glass, panel, mark, cross; };

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
        w.quad({-.58f, 1.88f, 3.08f}, {.58f, 1.88f, 3.08f}, {.73f, 2.39f, 2.18f}, {-.73f, 2.39f, 2.18f}, inside, m.dark);
        for (int s = -1; s <= 1; s += 2) {
            const float x = float(s);
            w.quad({x * .042f, 1.93f, 3.075f}, {x * .52f, 1.93f, 3.075f},
                   {x * .655f, 2.34f, 2.27f}, {x * .045f, 2.34f, 2.27f}, inside, m.glass);
            w.quad({x * .558f, 1.86f, 3.08f}, {x * .727f, 2.35f, 2.24f}, {x * .946f, 2.25f, 1.82f},
                   {x * .946f, 1.59f, 1.9f}, inside, m.dark);
            w.quad({x * .608f, 1.85f, 3.015f}, {x * .746f, 2.29f, 2.235f}, {x * .944f, 2.18f, 1.92f},
                   {x * .944f, 1.65f, 2.f}, inside, m.glass);
            const auto side = [&](float outward, float fore, float aft, float bottom, float top, uint16_t mat) {
                w.quad({x * outward, bottom, fore}, {x * outward, top, fore}, {x * outward, top, aft},
                       {x * outward, bottom, aft}, inside, mat);
            };
            side(1.057f, 1.43f, -.20f, .86f, 2.23f, m.panel);
            side(1.064f, 1.34f, -.09f, 1.60f, 2.15f, m.dark);
            side(1.072f, 1.27f, .68f, 1.66f, 2.09f, m.glass);
            side(1.072f, .59f, -.02f, 1.66f, 2.09f, m.glass);
            side(1.075f, .07f, -.03f, 1.26f, 1.31f, m.dark);
            side(1.047f, -1.04f, -1.3f, 1.44f, 1.73f, m.dark);
            // The exhaust scrolls sweep down the upper nacelle.
            w.tube({{x * .46f, 2.55f, -.71f}, {x * .49f, 2.54f, -1.31f}, {x * .42f, 2.49f, -1.91f}},
                   {.12f, .15f, .08f}, {.17f, .16f, .09f}, 6, up, m.dark);
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
        // A small steering wheel and belly access plates break up the nose silhouette at Dustoff.
        w.tube({{0, .89f, 2.57f}, {0, .33f, 2.63f}}, {.064f, .045f}, {.064f, .045f}, 5, ahead, m.panel);
        w.tube({{-.082f, .23f, 2.64f}, {.082f, .23f, 2.64f}},
               {.188f, .188f}, {.188f, .188f}, 8, up, m.dark);
        w.quad({-.38f, .551f, 1.45f}, {.38f, .551f, 1.45f}, {.51f, .529f, .10f}, {-.51f, .529f, .10f},
               inside, m.panel);
        w.quad({-.40f, .536f, -.03f}, {.40f, .536f, -.03f}, {.39f, .541f, -.92f}, {-.39f, .541f, -.92f},
               inside, m.dark);
        for (int plate = 0; plate < 3; ++plate) {
            const float z = -.18f - .21f * plate;
            w.quad({-.34f, .525f, z}, {.34f, .525f, z}, {.34f, .525f, z - .048f}, {-.34f, .525f, z - .048f},
                   inside, m.panel);
        }
        w.group = 1;
        // Tail boom, fin, stabilator and tail rotor.
        w.tube({{0, 1.65f, -2.4f}, {0, 1.85f, -5.f}, {0, 2.f, -7.3f}}, {.42f, .3f, .22f}, {.36f, .26f, .2f}, 6, up, m.olive);
        w.tube({{0, 1.95f, -7.f}, {0, 3.4f, -7.75f}}, {.55f, .32f}, {.07f, .05f}, 4, ahead, m.olive);
        w.tube({{-1.15f, 2.f, -6.8f}, {1.15f, 2.f, -6.8f}}, {.34f, .34f}, {.04f, .04f}, 4, ahead, m.olive);
        const Vec3 tailRotor{.14f, 3.f, -7.6f};
        w.tube({tailRotor, tailRotor + Vec3{.10f, 0, 0}}, {.13f, .1f}, {.13f, .1f}, 6, up, m.dark);
        for (int blade = 0; blade < 4; ++blade) {
            const float a = blade * pi / 2 + .43f;
            const Vec3 d{0, std::cos(a), std::sin(a)}, across{0, -d.z, d.y};
            w.quad(tailRotor + d * .1f - across * .065f, tailRotor + d * .82f - across * .065f,
                   tailRotor + d * .9f + across * .09f, tailRotor + d * .14f + across * .08f, tailRotor, m.dark);
        }
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
    const HeloMats m{mat(HeloOlive), mat(HeloDark), mat(HeloGlass), mat(HeloPanel), mat(HeloMark), mat(HeloCross)};
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

void scorchMark(Builder& b, Rect bounds, float height, float yaw = 0.f) {
    constexpr int sides = 12;
    const Vec3 center = position((bounds.left + bounds.right) * .5f, (bounds.top + bounds.bottom) * .5f, height);
    const float width = (bounds.right - bounds.left) * scale * .5f;
    const float length = (bounds.bottom - bounds.top) * scale * .5f;
    const int seed = int(bounds.left * 2 + bounds.bottom * 5);
    std::array<Vec3, sides> burn{}, edge{};
    for (int i = 0; i < sides; ++i) {
        const float a = i * 2.f * pi / sides;
        const float spread = .69f + .025f * grain(seed, i);
        const Vec3 drift = turnY({std::cos(a) * width, 0, std::sin(a) * length}, yaw);
        burn[i] = center + drift * (spread * .69f);
        edge[i] = center + drift * spread;
    }
    for (int i = 0; i < sides; ++i) {
        const int next = (i + 1) % sides;
        b.face({center, burn[next], burn[i]}, Scorch);
        b.quad({burn[i], burn[next], edge[next], edge[i]}, ScorchEdge);
    }
}

// Warhorse 2-1, the Delta Squad insertion bird, down in the road beyond the front gate: fuselage
// rolled onto its side, tail torn away, rotor buckled into the asphalt, and still burning.
void warhorseWreck(Builder& b) {
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    CreatureWriter w(vertices, &triangles);
    // Surfaces stand in for materials here; the cabin cross is painted over on an assault bird.
    writeTransport(w, {WreckSkin, HeloDark, WreckGlass, HeloOlive, WreckSkin, WreckSkin}, true, true, 2.6f);
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
    scorchMark(b, {313, 557, 434, 712}, .023f, -.19f);
    scorchMark(b, {317, 522, 368, 630}, .025f, -.26f);
    for (int i = 0; i < 12; ++i) {
        const Vec3 at = site + Vec3{(int(grain(i, 33)) - 7) * .3f, .047f, (int(grain(i, 37)) - 6) * .66f};
        const float yaw = .31f * float(grain(i, 43));
        const Vec3 spur = turnY({.15f + .025f * grain(i, 51), .034f, .09f}, yaw);
        const Vec3 side = turnY({-.11f, 0, .16f}, yaw);
        b.face({at - side, at + side, at + spur}, i % 3 ? HeloDark : HeloPanel);
    }
    const Vec3 engine = place({{0, 2.5f, -.5f}, 0, 0});
    b.fire(engine, 1.1f, 1.9f);
    b.fire(site + Vec3{-2.4f, .02f, .6f}, .72f, .95f);
    b.smoke(engine + Vec3{0, .8f, 0}, 22.f, Vec3{-5.f, 0, 10.f}, 1.9f);
}

// Dr. Marsh's radio array on the east wing ridge, still aimed at the mothership it found.
void markerArray(Builder& b) {
    const Vec3 foot = position(612, 62, 3.75f);
    const Vec3 mothership = position(260, -520, 36.f);
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

// Watertight spatial vertex clustering for high-poly Ship Box vehicles reused across the
// valley. Preserves original collision bounds and separates UV islands so textures never stretch.
void simplifyVehicleMesh(Mesh& mesh, float cell) {
    if (mesh.vertices.empty() || mesh.triangles.empty() || mesh.chunks.empty()) return;
    const Bounds originalBounds = mesh.chunks.front().bounds;
    const float invCell = 1.0f / cell;

    // Step 1: cluster 3D positions into a spatial grid so adjacent materials share exact seam vertices.
    size_t vertexCapacity = 16;
    while (vertexCapacity < mesh.vertices.size() * 2) vertexCapacity <<= 1;
    const size_t vertexMask = vertexCapacity - 1;
    std::vector<Vec3> posSum;
    posSum.reserve(mesh.vertices.size() / 4 + 16);
    std::vector<uint32_t> vertPos(mesh.vertices.size());
    {
        // Release the clustering table before allocating the vertex and triangle tables.
        struct PosEntry { int32_t x, y, z; uint32_t id; };
        std::vector<PosEntry> posTable(vertexCapacity, {0, 0, 0, UINT32_MAX});
        std::vector<uint32_t> posCount;
        posCount.reserve(mesh.vertices.size() / 4 + 16);
        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
            const Vec3 p = mesh.vertices[i].position;
            const int32_t gx = int32_t(std::floor(p.x * invCell));
            const int32_t gy = int32_t(std::floor(p.y * invCell));
            const int32_t gz = int32_t(std::floor(p.z * invCell));
            const uint32_t h = uint32_t(gx) * 73856093u ^ uint32_t(gy) * 19349663u ^ uint32_t(gz) * 83492791u;
            size_t slot = (h ^ (h >> 16)) & vertexMask;
            for (;;) {
                auto& e = posTable[slot];
                if (e.id == UINT32_MAX) {
                    const uint32_t id = uint32_t(posSum.size());
                    e = {gx, gy, gz, id};
                    posSum.push_back(p);
                    posCount.push_back(1);
                    vertPos[i] = id;
                    break;
                }
                if (e.x == gx && e.y == gy && e.z == gz) {
                    posSum[e.id] += p;
                    posCount[e.id]++;
                    vertPos[i] = e.id;
                    break;
                }
                slot = (slot + 1) & vertexMask;
            }
        }
        for (size_t i = 0; i < posSum.size(); ++i) posSum[i] *= 1.0f / float(posCount[i]);
    }

    // Step 2: emit vertices keyed by (posCluster, material, coarse UV island) and drop degenerate/duplicate tris.
    struct VertKey { uint32_t pc; uint16_t mat; int16_t ub, vb; uint32_t id; };
    std::vector<VertKey> vTable(vertexCapacity, {0, 0, 0, 0, UINT32_MAX});
    std::vector<Vertex> newVerts;
    std::vector<uint32_t> uvCount;
    newVerts.reserve(posSum.size() + 64);
    uvCount.reserve(posSum.size() + 64);

    auto getVertex = [&](uint32_t origIdx, uint16_t mat) -> uint32_t {
        const uint32_t pc = vertPos[origIdx];
        const auto& v = mesh.vertices[origIdx];
        const int16_t ub = int16_t(std::clamp(int(std::floor(v.u * 4.f)), -32000, 32000));
        const int16_t vb = int16_t(std::clamp(int(std::floor(v.v * 4.f)), -32000, 32000));
        uint32_t h = pc * 2654435761u ^ (uint32_t(mat) * 2246822519u) ^
                     (uint32_t(uint16_t(ub)) * 3266489917u) ^ (uint32_t(uint16_t(vb)) * 668265263u);
        size_t slot = (h ^ (h >> 16)) & vertexMask;
        for (;;) {
            auto& e = vTable[slot];
            if (e.id == UINT32_MAX) {
                const uint32_t id = uint32_t(newVerts.size());
                e = {pc, mat, ub, vb, id};
                newVerts.push_back({posSum[pc], v.u, v.v});
                uvCount.push_back(1);
                return id;
            }
            if (e.pc == pc && e.mat == mat && e.ub == ub && e.vb == vb) {
                newVerts[e.id].u += v.u;
                newVerts[e.id].v += v.v;
                uvCount[e.id]++;
                return e.id;
            }
            slot = (slot + 1) & vertexMask;
        }
    };

    struct TriKey { uint32_t a, b, c; uint16_t mat; bool used; };
    size_t tCap = 16;
    while (tCap < mesh.triangles.size() * 2) tCap <<= 1;
    const size_t tMask = tCap - 1;
    std::vector<TriKey> tTable(tCap, {0, 0, 0, 0, false});
    std::vector<Triangle> newTris;
    newTris.reserve(mesh.triangles.size() / 3 + 16);

    for (const auto& t : mesh.triangles) {
        const uint32_t pa = vertPos[t.a], pb = vertPos[t.b], pc = vertPos[t.c];
        if (pa == pb || pb == pc || pc == pa) continue;
        uint32_t s0 = pa, s1 = pb, s2 = pc;
        if (s0 > s1) std::swap(s0, s1);
        if (s1 > s2) std::swap(s1, s2);
        if (s0 > s1) std::swap(s0, s1);
        uint32_t h = s0 * 73856093u ^ s1 * 19349663u ^ s2 * 83492791u ^ uint32_t(t.material) * 2654435761u;
        size_t slot = (h ^ (h >> 16)) & tMask;
        bool dup = false;
        for (;;) {
            auto& e = tTable[slot];
            if (!e.used) { e = {s0, s1, s2, t.material, true}; break; }
            if (e.a == s0 && e.b == s1 && e.c == s2 && e.mat == t.material) { dup = true; break; }
            slot = (slot + 1) & tMask;
        }
        if (dup) continue;
        newTris.push_back({getVertex(t.a, t.material), getVertex(t.b, t.material),
                           getVertex(t.c, t.material), t.material, t.glass});
    }
    for (size_t i = 0; i < newVerts.size(); ++i) {
        const float inv = 1.0f / float(uvCount[i]);
        newVerts[i].u *= inv;
        newVerts[i].v *= inv;
    }
    newVerts.shrink_to_fit();
    newTris.shrink_to_fit();
    mesh.vertices = std::move(newVerts);
    mesh.triangles = std::move(newTris);
    mesh.chunks = {{originalBounds, 0, uint32_t(mesh.triangles.size())}};
}

// Harvest reusable high-detail textured props (box truck, sedan, wooden crates, stacked pallets,
// metal barrels, streetlamp poles, olive military shipping container) directly from Ship Box's
// loaded world mesh before buildBriarHouse replaces assets.world. Cached once on Assets.
void extractShipBoxProps(Assets& assets) {
    if (assets.arena != Arena::ShipBox || assets.world.triangles.empty() || !assets.shipBoxTruck.triangles.empty()) {
        return;
    }
    const auto& src = assets.world;
    const auto extract = [&](float cx, float cz, float yaw, float hw, float hd, float minY, float maxY,
                             auto acceptMat, bool centerXZ = true) -> Mesh {
        Mesh out;
        const float cs = std::cos(yaw), sn = std::sin(yaw);
        Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const auto& t : src.triangles) {
            if (!acceptMat(t.material)) continue;
            const Vertex& va = src.vertices[t.a];
            const Vertex& vb = src.vertices[t.b];
            const Vertex& vc = src.vertices[t.c];
            const Vec3 c = (va.position + vb.position + vc.position) * (1.f / 3.f);
            if (c.y < minY || c.y > maxY) continue;
            const float dx = c.x - cx, dz = c.z - cz;
            if (std::abs(cs * dx - sn * dz) > hw || std::abs(sn * dx + cs * dz) > hd) continue;
            const uint32_t base = uint32_t(out.vertices.size());
            for (const Vertex* vp : {&va, &vb, &vc}) {
                const float px = cs * (vp->position.x - cx) - sn * (vp->position.z - cz);
                const float pz = sn * (vp->position.x - cx) + cs * (vp->position.z - cz);
                const Vec3 lp{px, vp->position.y, pz};
                lo = {std::min(lo.x, lp.x), std::min(lo.y, lp.y), std::min(lo.z, lp.z)};
                hi = {std::max(hi.x, lp.x), std::max(hi.y, lp.y), std::max(hi.z, lp.z)};
                out.vertices.push_back({lp, vp->u, vp->v});
            }
            out.triangles.push_back({base, base + 1, base + 2, t.material, -1});
        }
        if (out.vertices.empty()) return out;
        const float shiftX = centerXZ ? (lo.x + hi.x) * 0.5f : 0.f;
        const float shiftZ = centerXZ ? (lo.z + hi.z) * 0.5f : 0.f;
        const float shiftY = lo.y;
        for (auto& v : out.vertices) {
            v.position.x -= shiftX;
            v.position.y -= shiftY;
            v.position.z -= shiftZ;
        }
        finalizeProceduralMesh(out);
        return out;
    };

    const auto vehicleMat = [](uint16_t m) {
        return m != 0 && m != 10 && m != 112 && m != 113 && m != 114;
    };
    assets.shipBoxTruck = extract(9.9f, 10.2f, -0.6f + pi * 0.5f, 3.55f, 1.48f, 0.005f, 3.2f, vehicleMat);
    assets.shipBoxCar = extract(-10.9f, -9.9f, 0.65f, 2.45f, 1.18f, -0.08f, 1.9f, vehicleMat);
    simplifyVehicleMesh(assets.shipBoxTruck, 0.08f);
    simplifyVehicleMesh(assets.shipBoxCar, 0.08f);
    assets.shipBoxCrate = extract(-6.6f, 10.0f, -0.3f, 0.80f, 0.80f, 0.005f, 1.45f, [](uint16_t m) { return m == 16; });
    assets.shipBoxPallet = extract(-10.1f, 9.6f, 0.5f, 0.95f, 0.80f, 0.005f, 0.85f, [](uint16_t m) { return m == 16; });
    assets.shipBoxBarrel = extract(-7.8f, 5.6f, 0.f, 0.40f, 0.40f, 0.005f, 1.05f, [](uint16_t m) { return m == 13; });
    assets.shipBoxLamp = extract(-20.0f, 11.0f, 0.f, 2.1f, 0.55f, 0.005f, 8.9f, vehicleMat, false);
    assets.shipBoxContainer = extract(-4.3f, 5.16f, 0.f, 3.15f, 1.28f, 0.005f, 2.62f,
                                      [](uint16_t m) { return m == 14 || m == 15; });
}

void crashedPod(Builder& b, float x, float y, bool solid) {
    scorchMark(b, {x - 32, y - 25, x + 27, y + 28}, .024f, .3f);
    b.round(x, y, .02f, .24f, 18, 12, Scorch);
    b.round(x, y, .17f, .43f, 9.6f, 9.2f, AlienTrim);
    b.round(x, y, .42f, 1.10f, 9.2f, 7.3f, PodSkin);
    b.round(x, y, 1.10f, 1.54f, 7.3f, 3.8f, PodSkin);
    b.round(x, y, 1.54f, 1.66f, 3.8f, 2.9f, AlienTrim);
    // Separate the emitter into inlaid strips between shield petals.
    const Vec3 base = position(x, y, .02f);
    for (int i = 0; i < 6; ++i) {
        const float a = float(i) * (pi / 3.f) + .25f;
        const Vec3 dir{std::cos(a), 0, std::sin(a)}, side{-dir.z, 0, dir.x};
        const Vec3 foot = base + dir * .93f + Vec3{0, .04f, 0};
        const Vec3 knee = base + dir * .58f + Vec3{0, .49f, 0};
        const Vec3 hinge = base + dir * .37f + Vec3{0, 1.40f, 0};
        b.quad({foot - side * .08f, foot + side * .08f, knee + side * .17f, knee - side * .17f}, AlienTrim);
        b.face({knee - side * .17f, knee + side * .17f, hinge}, i == 1 ? Iron : AlienTrim);
        const Vec3 slot = base + dir * .516f + Vec3{0, .82f, 0};
        const Vec3 raised = slot - dir * .057f + Vec3{0, .32f, 0};
        b.quad({slot - side * .052f, slot + side * .052f, raised + side * .029f,
                raised - side * .029f}, i == 1 ? AlienCore : AlienGlow);
        if (i % 2 == 0) {
            const Vec3 shard = base + dir * (.91f + .02f * grain(i, int(x)));
            b.face({shard - side * .11f, shard + side * .08f, shard + dir * .16f + Vec3{0, .33f, 0}}, RayCore);
            b.face({shard + side * .08f, shard - dir * .18f, shard + dir * .16f + Vec3{0, .33f, 0}}, AlienTrim);
        }
    }
    if (solid) b.collision({x - 10, y - 10, x + 10, y + 10}, 0.f, 1.8f, true);
    b.lamp(x, y, 1.2f, 8.8f, 1.15f);
}

// Tactical street cover includes reused Ship Box cargo and civilian vehicles.
void townCombatProps(Builder& b) {
    const auto& data = b.data();
    const auto coverWall = [&](float x0, float y0, float x1, float y1, float h = 1.12f) {
        b.prop({x0, y0, x1, y1}, h, GardenWall);
        b.box({x0 - 1, y0 - 1, x1 + 1, y1 + 1}, h - .06f, h + .04f, MonumentStone);
    };
    const auto crateAt = [&](float x, float y, float yaw, float s = 1.f) {
        const Vec3 at = position(x, y, 0.01f);
        if (!data.shipBoxCrate.triangles.empty()) {
            b.stampMesh(data.shipBoxCrate, at, yaw, s);
            const float half = 0.66f * s;
            b.orientedCollision(at, {std::cos(yaw), 0, -std::sin(yaw)}, half, half, 0.f, 1.30f * s, true);
        } else {
            b.prop({x - 12 * s, y - 12 * s, x + 12 * s, y + 12 * s}, 1.25f * s, Timber);
        }
    };
    const auto palletAt = [&](float x, float y, float yaw) {
        const Vec3 at = position(x, y, 0.01f);
        if (!data.shipBoxPallet.triangles.empty()) {
            b.stampMesh(data.shipBoxPallet, at, yaw, 1.f);
            b.orientedCollision(at, {std::cos(yaw), 0, -std::sin(yaw)}, 0.84f, 0.70f, 0.f, 0.64f, false);
        }
    };
    const auto barrelAt = [&](float x, float y) {
        const Vec3 at = position(x, y, 0.01f);
        if (!data.shipBoxBarrel.triangles.empty()) {
            b.stampMesh(data.shipBoxBarrel, at, 0.f, 1.f);
            b.collision({x - 6.5f, y - 6.5f, x + 6.5f, y + 6.5f}, 0.f, 0.96f, true);
        }
    };
    const auto containerAt = [&](float x, float y, float yaw) {
        const Vec3 at = position(x, y, 0.01f);
        if (!data.shipBoxContainer.triangles.empty()) {
            b.stampMesh(data.shipBoxContainer, at, yaw, 1.f);
            b.orientedCollision(at, {std::cos(yaw), 0, -std::sin(yaw)}, 3.08f, 1.28f, 0.f, 2.62f, true);
        }
    };
    const auto shipTruck = [&](float x, float y, float yaw, Vec3 fallbackAlong, Vec3 fallbackOut,
                               uint16_t paint, uint16_t cargo) {
        const Vec3 at = position(x, y, .01f);
        if (!b.placeVehicle(PropKind::Truck, at, yaw, paint, cargo)) {
            parkedPickup({b, at, fallbackAlong, fallbackOut});
            b.orientedCollision(at + fallbackOut * 1.95f, fallbackAlong, 0.96f, 2.0f, 0.f, 1.65f, true);
        }
    };
    const auto shipCar = [&](float x, float y, float yaw, uint16_t paint) {
        b.placeVehicle(PropKind::Car, position(x, y, .01f), yaw, paint);
    };

    // South Boulevard: Warhorse 2-1 crash fuselage cover + scattered Delta Squad Ship Box supply
    // crates/barrels/pallets, abandoned Ship Box cars, stone barricades & breakout flares.
    b.collision({344, 588, 398, 656}, 0.f, 1.75f, true);
    coverWall(215, 608, 265, 620, 1.05f);
    coverWall(495, 608, 545, 620, 1.05f);
    shipCar(148, 620, 0.22f, rgb(168, 61, 70));
    shipCar(562, 578, -0.34f, rgb(190, 139, 66));
    // Delta Squad emergency supply drop & barricade crates outside the breached front gates.
    crateAt(278, 536, 0.14f, 0.92f);
    crateAt(298, 532, -0.18f, 0.72f);
    barrelAt(262, 542);
    crateAt(458, 536, -0.22f, 0.92f);
    palletAt(482, 540, 0.15f);
    barrelAt(446, 548);
    // Scattered cargo tossed from Warhorse 2-1 on South Boulevard.
    crateAt(310, 626, 0.42f, 0.85f);
    barrelAt(424, 636);
    barrelAt(436, 624);
    roadFlare(b, 316, 526);
    roadFlare(b, 420, 526);

    // East Ridge Avenue (Ben Marsh's district): crashed pod, Ship Box box truck & sedan,
    // roadside stone walls reinforced with Ship Box crates and oil drums.
    crashedPod(b, 845, 255, true);
    coverWall(875, 488, 930, 500, 1.12f);
    coverWall(965, 345, 978, 395, 1.15f);
    coverWall(935, 130, 948, 180, 1.10f);
    shipTruck(982, 465, pi * 0.5f - 0.08f, {0, 0, 1}, {1, 0, 0}, rgb(178, 92, 46), rgb(190, 163, 111));
    shipCar(918, 215, pi * 0.5f + 0.24f, rgb(91, 122, 187));
    crateAt(956, 412, 0.25f, 0.90f);
    barrelAt(942, 405);
    barrelAt(948, 392);
    palletAt(860, 492, 0.08f);
    crateAt(928, 114, -0.30f, 0.88f);
    roadFlare(b, 922, 438);

    // North Valley Avenue & Town Clearing LZ (Dr. Nora Marsh + Extraction LZ):
    // Fortified Dustoff staging perimeter with olive-green Ship Box military container,
    // Ship Box box trucks, supply crates, pallets, oil drums, and four crimson LZ flares!
    roadFlare(b, 155, -245);
    roadFlare(b, 265, -245);
    roadFlare(b, 155, -195);
    roadFlare(b, 265, -195);
    coverWall(132, -162, 186, -150, 1.08f);
    coverWall(234, -162, 290, -150, 1.08f);
    containerAt(58, -242, 0.16f);
    crateAt(98, -274, 0.12f, 0.95f);
    crateAt(118, -276, -0.15f, 0.78f);
    palletAt(62, -196, 0.32f);
    barrelAt(108, -248);
    barrelAt(120, -240);
    shipTruck(336, -252, 0.34f, {1, 0, 0}, {0, 0, 1}, rgb(110, 141, 160), rgb(177, 183, 177));
    shipCar(28, -172, -0.20f, rgb(200, 194, 168));
    // North Valley Avenue (Dr. Nora Marsh's street)
    coverWall(508, -192, 566, -180, 1.15f);
    shipTruck(602, -188, 0.06f, {1, 0, 0}, {0, 0, 1}, rgb(199, 181, 148), rgb(126, 134, 119));
    crateAt(490, -186, -0.18f, 0.90f);
    barrelAt(474, -192);
    palletAt(580, -235, -0.12f);
    crashedPod(b, 735, -165, true);
    roadFlare(b, 535, -206);

    // West Timberline Lane (Walt Marsh's district): Ship Box forestry box truck, abandoned sedan,
    // crashed pod, supply crates, barrels, and sunset barricades.
    crashedPod(b, -225, 420, true);
    coverWall(-285, 248, -272, 298, 1.15f);
    coverWall(-265, 96, -252, 146, 1.10f);
    shipTruck(-345, 151, pi * 0.5f - 0.10f, {0, 0, -1}, {-1, 0, 0}, rgb(75, 119, 87), rgb(147, 151, 103));
    shipCar(-352, 330, pi * 0.5f + 0.20f, rgb(99, 143, 76));
    crateAt(-278, 316, 0.28f, 0.90f);
    palletAt(-262, 332, -0.15f);
    barrelAt(-292, 308);
    crateAt(-264, 78, -0.22f, 0.85f);
    barrelAt(-254, 64);
    roadFlare(b, -295, 242);
}

// High-detail town dressing emitted after beginDecoration() so grass tufts, lamp poles, fences,
// and road markings never bloat bullet shot geometry while adding crisp silhouettes & dusk glow.
void townDetails(Builder& b) {
    const auto& data = b.data();
    const int16_t amberMat = int16_t(b.surfaceMat(Amber));

    // 1. Reused Ship Box yard streetlamps ringing the town loop & North Valley Clearing LZ!
    const struct { float x, y, yaw; } poles[]{
        {86, 598, -pi * 0.5f},  {478, 604, -pi * 0.5f}, {790, 610, -pi * 0.6f},
        {944, 520, pi},         {992, 305, pi},         {955, 70, pi * 0.85f},
        {755, -205, pi * 0.5f}, {515, -240, pi * 0.5f}, {308, -172, -pi * 0.65f},
        {112, -174, -pi * 0.35f}, {-145, -170, pi * 0.4f},
        {-342, 35, 0.12f},      {-364, 232, 0.f},       {-318, 435, -0.25f},
    };
    for (const auto& p : poles) {
        const Vec3 at = position(p.x, p.y, 0.f);
        constexpr float s = 0.78f;
        if (!data.shipBoxLamp.triangles.empty()) {
            b.stampMesh(data.shipBoxLamp, at, p.yaw, s, amberMat);
            b.collision({p.x - 4.f, p.y - 4.f, p.x + 4.f, p.y + 4.f}, 0.f, 4.5f, false);
        }
        // Warm dusk streetlamp glow pool beneath the cantilevered arm (+1.55 * s along local X).
        const float cs = std::cos(p.yaw), sn = std::sin(p.yaw);
        const Vec3 head = at + Vec3{cs * 1.55f * s, 6.2f * s, -sn * 1.55f * s};
        b.lamp((head.x / scale) + 316.f, 212.5f - (head.z / scale), 5.6f, 11.5f, 0.88f);
    }

    // 2. Subtle painted Colorado mountain-road dashed center lines curving around the town loop.
    constexpr size_t segs = std::size(street);
    for (size_t i = 0; i < segs; ++i) {
        const Vec3 a = position(street[i].x, street[i].y, 0.014f);
        const Vec3 c = position(street[(i + 1) % segs].x, street[(i + 1) % segs].y, 0.014f);
        const Vec3 delta = c - a;
        const float len = game_detail::planar(delta);
        if (len < 3.f) continue;
        const Vec3 dir = normalized({delta.x, 0.f, delta.z});
        const Vec3 side{-dir.z * 0.075f, 0.f, dir.x * 0.075f};
        for (float d = 1.6f; d + 1.8f < len - 1.2f; d += 4.6f) {
            const Vec3 p0 = a + dir * d;
            const Vec3 p1 = a + dir * (d + 2.0f);
            b.quad({p0 - side, p1 - side, p1 + side, p0 + side}, RoadLine);
        }
    }

    // 3. Rustic split-rail timber fences framing yards & meadows along the town loop.
    const auto railFence = [&](Vec3 start, Vec3 end, int bays) {
        const Vec3 span = end - start;
        const Vec3 dir = normalized({span.x, 0, span.z});
        const Vec3 side{-dir.z * 0.055f, 0, dir.x * 0.055f};
        for (int i = 0; i <= bays; ++i) {
            const Vec3 p = start + span * (float(i) / float(bays));
            b.box({p - side - dir * 0.055f, p + side - dir * 0.055f, p + side + dir * 0.055f, p - side + dir * 0.055f},
                  {p - side - dir * 0.055f + Vec3{0, 1.05f, 0}, p + side - dir * 0.055f + Vec3{0, 1.05f, 0},
                   p + side + dir * 0.055f + Vec3{0, 1.05f, 0}, p - side + dir * 0.055f + Vec3{0, 1.05f, 0}}, DarkWood);
        }
        for (const float railY : {0.42f, 0.84f}) {
            const Vec3 s2 = side * 0.65f;
            b.box({start - s2 + Vec3{0, railY, 0}, end - s2 + Vec3{0, railY, 0},
                   end + s2 + Vec3{0, railY, 0}, start + s2 + Vec3{0, railY, 0}},
                  {start - s2 + Vec3{0, railY + 0.09f, 0}, end - s2 + Vec3{0, railY + 0.09f, 0},
                   end + s2 + Vec3{0, railY + 0.09f, 0}, start + s2 + Vec3{0, railY + 0.09f, 0}}, Timber);
        }
    };
    railFence(position(-115, 558), position(65, 588), 4);
    railFence(position(735, 578), position(848, 560), 3);
    railFence(position(855, 80), position(825, -45), 3);
    railFence(position(305, -128), position(468, -142), 4);
    railFence(position(-160, -108), position(-260, -40), 3);
    railFence(position(-240, 315), position(-210, 445), 3);

    // 4. Lush Ship Box-style 3D grass tufts scattered across Briar House lawns, wall edges,
    // road verges, meadow borders, North Town Clearing LZ, and sunset-backlit West Timberline!
    uint32_t rng = 0x51a7c3d1u;
    const auto jitter = [&](float range) -> float {
        rng = rng * 1664525u + 1013904223u;
        return (float((rng >> 8) & 0xFFFFu) / 65535.f * 2.f - 1.f) * range;
    };
    const auto scatterRect = [&](float x0, float y0, float x1, float y1, int count) {
        for (int i = 0; i < count; ++i) {
            rng = rng * 1664525u + 1013904223u;
            const float fx = float((rng >> 8) & 0xFFFFu) / 65535.f;
            rng = rng * 1664525u + 1013904223u;
            const float fy = float((rng >> 8) & 0xFFFFu) / 65535.f;
            b.grassTuft(position(x0 + (x1 - x0) * fx, y0 + (y1 - y0) * fy, 0.01f), rng ^ uint32_t(i * 2654435761u));
        }
    };
    // Briar House front, west, east, and rear lawns + hedge footings (avoiding paved paths).
    scatterRect(-48, 405, 250, 502, 46);
    scatterRect(478, 405, 570, 502, 24);
    scatterRect(-52, -75, 42, 185, 30);
    scatterRect(-52, 268, 42, 400, 22);
    scatterRect(52, -78, 250, 20, 32);
    scatterRect(486, -78, 678, 20, 30);
    // South Boulevard grassy verges & estate frontage
    scatterRect(-140, 530, 295, 595, 40);
    scatterRect(442, 530, 572, 595, 22);
    scatterRect(675, 530, 880, 598, 28);
    // East Ridge Avenue inner & outer verges
    scatterRect(745, 295, 925, 515, 34);
    scatterRect(745, -110, 925, 148, 30);
    // North Valley Avenue & North Town Clearing LZ meadow rim
    scatterRect(28, -285, 148, -115, 26);
    scatterRect(275, -285, 495, -115, 34);
    scatterRect(495, -165, 765, -95, 26);
    // West Timberline Lane (backlit golden-green grass tufts against the Colorado sunset!)
    scatterRect(-285, 275, -115, 505, 36);
    scatterRect(-285, -115, -115, 155, 34);
    // Extra grass clusters hugging the outer town loop gravel shoulder.
    for (size_t i = 0; i < segs; ++i) {
        const Vec3 a = position(street[i].x, street[i].y, 0.01f);
        const Vec3 c = position(street[(i + 1) % segs].x, street[(i + 1) % segs].y, 0.01f);
        const Vec3 out = acrossStreet(a, c);
        for (int k = 0; k < 3; ++k) {
            const float t = 0.18f + 0.32f * float(k);
            const Vec3 spot = mix(a, c, t) + out * (4.1f + jitter(0.6f)) + Vec3{jitter(0.5f), 0.f, jitter(0.5f)};
            b.grassTuft(spot, rng ^ uint32_t(i * 97 + k * 31));
        }
    }
}

void invasionDetails(Builder& b) {
    crashedPod(b, 90, 436, false);
    // Scorched blast mark and shattered debris right outside the front portico, where the crash threw us.
    scorchMark(b, {338, 398, 396, 424}, .024f, -.08f);
    b.box({254, 405, 266, 413}, .02f, .22f, AlienHull);
    b.box({257, 407, 263, 411}, .22f, .31f, AlienGlow);
    b.box({458, 403, 470, 412}, .02f, .19f, AlienTrim);
    // Plasma scars across the front lawn and through the Grand Hall where the first wave came in.
    scorchMark(b, {300, 468, 352, 480}, .021f, -.25f);
    scorchMark(b, {402, 452, 440, 461}, .021f, .43f);
    scorchMark(b, {352, 334, 386, 346}, .008f);
    // Military crimson road flares marking the driveway approach and breached rear patio.
    roadFlare(b, 574, 448); // Driveway gate approach
    roadFlare(b, 344, -14); // Rear patio left
    roadFlare(b, 392, -14); // Rear patio right
    warhorseWreck(b);
    markerArray(b);
}

void invasionBackdrop(Builder& b) {
    // Homes across the valley burn under the landings. The fleet itself only renders once the
    // mission begins, so the title screen shows the smoke and sunset mist but not its cause.
    const size_t count = b.homes.size();
    for (size_t i = 0; i < count && i < 12; i += 3) {
        const Vec3 home = b.homes[i];
        b.fire(home + Vec3{0, 3.8f, 0}, 1.55f, 2.4f);
        b.smoke(home + Vec3{0, 4.f, 0}, 26.f, Vec3{-6.f, 0, 9.f}, 2.2f);
    }
    // Descending dusk valley mist banks rolling across all four town districts, streets and ridges.
    const struct { float x, y, h, width, height, dx, dz, rate; } banks[]{
        // South Boulevard & Warhorse wreck approach
        {240, 615, .85f, 8.0f, 2.6f, 4.2f, -1.5f, .042f},
        {480, 640, .95f, 8.4f, 2.7f, 4.6f, -1.2f, .038f},
        {80, 625, 1.0f, 8.2f, 2.6f, 3.8f, -1.8f, .040f},
        {710, 600, .95f, 8.0f, 2.6f, 4.2f, -1.4f, .043f},
        // East Ridge Avenue (Ben's district)
        {940, 460, .95f, 8.6f, 2.8f, 4.4f, -1.2f, .039f},
        {980, 240, .95f, 8.8f, 2.8f, 4.8f, -1.0f, .037f},
        {920, 10, 1.0f, 8.4f, 2.7f, 4.5f, 1.2f, .040f},
        // North Valley Avenue & Town Clearing LZ (Nora's district & Extraction)
        {590, -210, .95f, 8.8f, 2.8f, 4.6f, 1.3f, .038f},
        {240, -230, .95f, 9.2f, 2.9f, 5.0f, 1.1f, .036f},
        {-80, -200, 1.0f, 8.6f, 2.8f, 4.4f, 1.4f, .039f},
        // West Timberline Lane (Walt's district, backlit by the Colorado sunset!)
        {-315, 20, .95f, 8.6f, 2.8f, 4.6f, 1.5f, .038f},
        {-335, 250, .95f, 9.0f, 2.9f, 4.8f, 1.2f, .036f},
        {-280, 470, .95f, 8.4f, 2.7f, 4.2f, -1.0f, .041f},
        // Estate lawns & inner valley connectors
        {170, 455, .72f, 5.8f, 2.0f, 2.8f, -1.1f, .050f},
        {368, -76, .85f, 7.2f, 2.4f, 3.8f, 1.0f, .042f},
        // Outer valley foothills & ridges
        {350, 770, 1.8f, 12.f, 3.8f, 6.0f, -2.0f, .030f},
        {780, -280, 2.1f, 12.f, 3.8f, 5.5f, 1.5f, .031f},
        {-220, -240, 2.0f, 12.f, 3.8f, 5.4f, 1.6f, .030f},
    };
    for (const auto& m : banks) {
        b.mist(position(m.x, m.y, m.h), m.width, m.height, {m.dx, 0, m.dz}, m.rate);
    }
}

void buildOperationMeshes(Assets& assets, uint16_t firstMat) {
    const auto mat = [firstMat](Surface surface) { return uint16_t(firstMat + surface); };
    MeshWriter terminal{assets.relayTerminal};
    terminal.box({-.52f, .025f, -.48f}, {.52f, .13f, .48f}, mat(HeloDark));
    terminal.box({-.28f, .13f, -.23f}, {.28f, .89f, .23f}, mat(HeloOlive));
    terminal.box({-.46f, .86f, -.34f}, {.46f, 1.15f, .34f}, mat(HeloDark));
    terminal.box({-.32f, 1.147f, -.24f}, {.32f, 1.17f, .21f}, mat(Steel));
    terminal.box({-.245f, 1.167f, -.195f}, {.245f, 1.18f, .095f}, mat(AlienGlow));
    for (float side : {-1.f, 1.f}) {
        const float x = side * .34f;
        terminal.box({x - .035f, 1.13f, -.21f}, {x + .035f, 1.21f, .27f}, mat(HeloMark));
        terminal.box({x - .024f, 1.15f, -.29f}, {x + .024f, 1.62f, -.244f}, mat(RayMetal));
    }
    terminal.box({-.41f, .95f, .34f}, {.41f, 1.f, .352f}, mat(RayCore));
    terminal.box({-.41f, .95f, -.352f}, {.41f, 1.f, -.34f}, mat(RayCore));
    finalizeProceduralMesh(assets.relayTerminal);

}

// Radial armor plates, recessed engine bays, and bevels catch the sunset on the silhouette.
void buildFleet(Assets& assets) {
    const auto material = [&](uint16_t tint, bool lit = false) {
        const auto id = uint16_t(assets.materials.size());
        assets.materials.push_back({tint, -1, 255, true, lit, 2});
        return id;
    };
    const uint16_t recess = material(rgb(19, 23, 34));
    const uint16_t rim = material(rgb(68, 91, 112));
    const uint16_t bevel = material(rgb(129, 150, 156));
    const uint16_t beacons = material(rgb(255, 175, 116), true);
    for (size_t i = 0; i < fleetHulls.size(); ++i) {
        const FleetHull& hull = fleetHulls[i];
        auto& craft = assets.alienFleet[i];
        craft = {};
        MeshWriter w{craft};
        const bool flagship = i == 0;
        const int panels = flagship ? 16 : i == 1 ? 12 : 14;
        const uint16_t armor = material(i == 2 ? rgb(67, 56, 89) : i == 1 ? rgb(95, 74, 74) : rgb(46, 64, 89));
        const uint16_t panel = material(i == 2 ? rgb(104, 88, 133) : i == 1 ? rgb(131, 101, 93) : rgb(81, 105, 129));
        const uint16_t channel = material(i == 2 ? rgb(165, 105, 255) : i == 1 ? rgb(255, 136, 75) : rgb(43, 178, 250), true);
        const uint16_t lamp = material(i == 2 ? rgb(225, 207, 255) : i == 1 ? rgb(255, 223, 168) : rgb(149, 252, 255), true);
        const uint16_t lens = material(i == 2 ? rgb(239, 218, 255) : rgb(209, 255, 248), true);
        const auto point = [&](float r, float y, float a) {
            return Vec3{std::cos(a) * r * hull.radius, y * hull.height, std::sin(a) * r * hull.radius};
        };
        const auto band = [&](float a, float b, float ra, float ya, float rb, float yb, uint16_t mat) {
            w.quad(point(ra, ya, a), point(ra, ya, b), point(rb, yb, b), point(rb, yb, a), mat);
        };
        for (int j = 0; j < panels; ++j) {
            const float step = 2.f * pi / panels;
            const float a = j * step, b = (j + 1) * step;
            // The continuous hull shows through the gaps between the individual armor plates.
            band(a, b, .16f, .46f, .40f, .32f, armor);
            band(a, b, .40f, .32f, .82f, .12f, panel);
            band(a, b, .82f, .12f, .965f, .031f, rim);
            band(a, b, .965f, .031f, .98f, -.030f, recess);
            band(a, b, .98f, -.030f, .86f, -.091f, rim);
            band(a, b, .86f, -.091f, .66f, -.245f, armor);
            band(a, b, .66f, -.245f, .35f, -.318f, recess);
            band(a, b, .35f, -.318f, .24f, -.36f, armor);
            w.tri(point(.16f, .46f, a), point(.16f, .46f, b), {0, hull.height * .52f, 0}, panel);

            // Raised shell plates, a recessed window belt, and vent tiles across the belly.
            const float start = a + step * .036f, end = b - step * .036f;
            band(start, end, .44f, .319f, .82f, .147f, j % 3 ? panel : rim);
            band(start, end, .82f, .147f, 1.008f, .013f, j % 4 ? armor : panel);
            band(start, end, 1.008f, .013f, 1.012f, -.024f, bevel);
            band(start, end, 1.012f, -.024f, .89f, -.111f, armor);
            band(start, end, .89f, -.111f, .68f, -.265f, j % 4 ? panel : rim);
            band(start, end, .68f, -.265f, .40f, -.339f, armor);
            band(start, end, .40f, -.339f, .35f, -.333f, rim);
            band(a + step * .11f, b - step * .11f, .980f, .011f, .988f, -.012f, channel);
            for (int k = 0; k < 3; ++k) {
                const float vent = a + step * (.14f + .25f * k);
                const float stop = vent + step * .146f;
                band(vent, stop, .72f, -.245f, .60f, -.300f, recess);
                band(vent, stop, .686f, -.270f, .63f, -.294f, k == 1 ? lamp : channel);
            }
            band(a + step * .13f, b - step * .13f, .485f, -.323f, .443f, -.343f, j % 4 ? channel : beacons);

            // The emitter is recessed inside a heavy collar. Its lens and the beam share an origin.
            band(a, b, .24f, -.36f, .218f, -.448f, rim);
            band(a, b, .218f, -.448f, .181f, -.49f, bevel);
            band(a, b, .181f, -.49f, .15f, -.503f, channel);
            band(a, b, .15f, -.503f, .116f, -.458f, recess);
            band(a, b, .116f, -.458f, .087f, -.465f, lamp);
            w.tri(point(.087f, -.465f, a), point(.087f, -.465f, b), {0, hull.height * -.471f, 0}, lens);
        }
        finalizeProceduralMesh(craft);
    }
}

void buildAlienMeshes(Assets& assets, uint16_t firstMat) {
    const uint16_t hull = uint16_t(firstMat + AlienHull);
    const uint16_t trim = uint16_t(firstMat + AlienTrim);
    const uint16_t glow = uint16_t(firstMat + AlienGlow);
    const uint16_t core = uint16_t(firstMat + AlienCore);

    buildCreature(assets, firstMat);
    buildRayGuns(assets, firstMat);
    buildCivilian(assets, firstMat);
    buildTransport(assets, firstMat);
    buildOperationMeshes(assets, firstMat);

    CreatureWriter tower{assets.alienUplink.vertices, &assets.alienUplink.triangles};
    tower.tube({{0, .035f, 0}, {0, .20f, 0}, {0, 1.f, 0}, {0, 1.25f, 0}},
               {.64f, .56f, .24f, .40f}, {.64f, .56f, .24f, .40f}, 8, {1, 0, 0}, hull);
    for (int i = 0; i < 3; ++i) {
        const float angle = 2 * pi * i / 3;
        const Vec3 dir{std::sin(angle), 0, std::cos(angle)}, side{dir.z, 0, -dir.x};
        tower.tube({dir * .36f + Vec3{0, .91f, 0}, dir * .87f + Vec3{0, .55f, 0},
                    dir * 1.06f + Vec3{0, .04f, 0}}, {.10f, .11f, .06f}, {.10f, .12f, .06f}, 4, side, trim);
        tower.spike(dir * .40f + Vec3{0, 1.16f, 0}, dir * .59f + Vec3{0, 2.15f, 0}, side, .12f, trim);
    }
    finalizeProceduralMesh(assets.alienUplink);
    CreatureWriter crystal{assets.alienUplinkCore.vertices, &assets.alienUplinkCore.triangles};
    crystal.tube({{0, -.26f, 0}, {0, -.08f, 0}, {0, .08f, 0}, {0, .26f, 0}},
                 {.06f, .37f, .33f, 0}, {.06f, .37f, .33f, 0}, 6, {1, 0, 0}, core);
    finalizeProceduralMesh(assets.alienUplinkCore);

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
    // Swept stabilizers with luminous vanes visible from below.
    for (const float s : {-1.f, 1.f}) {
        const Vec3 shoulder{s * .26f, .03f, .19f}, corner{s * .69f, .055f, -.32f},
                   rear{s * .40f, -.02f, -.31f}, keel{s * .33f, -.13f, .01f};
        d.tri(shoulder, corner, rear, trim);
        d.tri(shoulder, keel, corner, hull);
        d.tri(corner, keel, rear, hull);
        d.tri(keel + Vec3{0, -.009f, .051f}, corner + Vec3{-s * .06f, -.025f, .005f},
              keel + Vec3{s * .09f, .005f, -.087f}, glow);
    }
    // Recessed scanner with a bright inner lens and an underside plasma emitter.
    d.box({-.19f, -.079f, .37f}, {.19f, .09f, .46f}, hull);
    d.quad({-.14f, -.039f, .465f}, {.14f, -.039f, .465f}, {.082f, .058f, .47f},
           {-.082f, .058f, .47f}, glow);
    d.quad({-.062f, -.014f, .477f}, {.062f, -.014f, .477f}, {.056f, .031f, .477f},
           {-.056f, .031f, .477f}, uint16_t(firstMat + RayCore));
    d.box({-.11f, -.22f, -.11f}, {.11f, -.16f, .11f}, core);
    finalizeProceduralMesh(assets.alienDrone);
    buildFleet(assets);
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

    // Ben's work jacket, Nora's burgundy layers and glasses, Walt's canvas coat and knit cap.
    const uint16_t hidden = uint16_t(assets.materials.size());
    assets.materials.push_back({0, -1, 0});
    const struct { uint16_t top, pants, shoes, hair, skin, fabric, trim; } outfits[3]{
        {addMat(rgb(53, 86, 139)), addMat(rgb(66, 78, 94)), addMat(rgb(44, 42, 42)), addMat(rgb(104, 67, 36)),
         addMat(rgb(222, 180, 148)), addMat(rgb(38, 57, 91)), addMat(rgb(220, 175, 93))},
        {addMat(rgb(164, 66, 82)), addMat(rgb(62, 79, 110)), addMat(rgb(83, 65, 55)), addMat(rgb(46, 35, 31)),
         addMat(rgb(166, 112, 84)), addMat(rgb(97, 43, 57)), addMat(rgb(218, 174, 145))},
        {addMat(rgb(193, 171, 120)), addMat(rgb(56, 90, 76)), addMat(rgb(63, 50, 41)), addMat(rgb(175, 166, 152)),
         addMat(rgb(213, 179, 153)), addMat(rgb(109, 78, 55)), addMat(rgb(220, 205, 162))},
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
        remap[briarFirstMat + CivFabric] = outfits[i].fabric;
        remap[briarFirstMat + CivTrim] = outfits[i].trim;
        if (i != 1) remap[briarFirstMat + CivGlasses] = hidden;
        if (i != 2) {
            remap[briarFirstMat + CivCap] = hidden;
            remap[briarFirstMat + CivCapBand] = hidden;
        }
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
    // Enforcers wear blood-red hide over the same body, with crimson-hot ray cannon coils.
    if (size_t(briarFirstMat + RayOvercharge) < assets.enforcerUniform.size()) {
        assets.enforcerUniform[briarFirstMat + AlienSkin] = addMat(rgb(112, 30, 44));
        assets.enforcerUniform[briarFirstMat + AlienSinew] = addMat(rgb(58, 16, 26));
        assets.enforcerUniform[briarFirstMat + RayCoil] = addMat(rgb(172, 108, 118));
    }
}

void synthesizeMissionAudio(Assets& assets) {
    if (assets.audio.size() < size_t(Sound::Count)) assets.audio.resize(size_t(Sound::Count));
    // Single-precision synthesis avoids software double-precision libm emulation on ESP32-P4.
    const auto writeClip = [&](Sound id, float seconds, auto generator) {
        auto& samples = assets.audio[size_t(id)].samples;
        const size_t count = size_t(seconds * float(audioHz));
        samples.resize(count);
        const float invHz = 1.0f / float(audioHz);
        for (size_t i = 0; i < count; ++i) {
            const float t = float(i) * invHz;
            const float value = std::clamp(generator(t, seconds), -1.0f, 1.0f);
            samples[i] = int16_t(value * 24000.0f);
        }
    };
    constexpr float tau = 2.0f * pi;
    // Alien ray gun blast: resonant retro-sci-fi frequency whip with harmonic shimmer and sub-punch.
    writeClip(Sound::PlasmaFire, 0.25f, [](float t, float dur) {
        const float env = std::exp(-t * 12.5f) * (1.0f - t / dur);
        const float e18 = std::exp(-t * 18.0f);
        const float whip = 1750.0f * e18 + 280.0f;
        const float phase = tau * (280.0f * t + (1750.0f / 18.0f) * (1.0f - e18));
        const float ring = std::sin(tau * 54.0f * t) * 0.22f + 0.78f;
        const float crackle = 0.16f * std::exp(-t * 36.0f) * std::sin(tau * (whip * 2.4f) * t);
        const float punch = 0.28f * std::exp(-t * 28.0f) * std::sin(tau * 145.0f * t);
        return env * (ring * (0.58f * std::sin(phase) + 0.28f * std::sin(phase * 1.99f)) + crackle + punch);
    });
    // Alien drone energy burst: sharper metallic pulse.
    writeClip(Sound::DroneFire, 0.17f, [](float t, float dur) {
        const float env = std::exp(-t * 18.0f) * (1.0f - t / dur);
        const float phase = tau * (1850.0f * t - 520.0f * std::log(1.0f + t * 22.0f));
        return env * (0.58f * std::sin(phase) + 0.36f * std::sin(phase * 1.5f));
    });
    // Alien neutralization warble.
    writeClip(Sound::AlienDown, 0.38f, [](float t, float dur) {
        const float env = std::exp(-t * 5.5f) * (1.0f - t / dur);
        const float vibrato = std::sin(tau * 34.0f * t);
        const float freq = 520.0f - 320.0f * (t / dur) + 65.0f * vibrato;
        return env * 0.72f * std::sin(tau * freq * t);
    });
    // Concussion wake-up tinnitus + low impact rumble.
    writeClip(Sound::ConcussionRing, 2.2f, [](float t, float dur) {
        const float attack = std::min(1.0f, t / 0.08f);
        const float decay = std::exp(-t * 1.35f) * (1.0f - t / dur);
        const float ring = 0.24f * std::sin(tau * 2850.0f * t) + 0.10f * std::sin(tau * 2862.0f * t);
        const float thump = 0.65f * std::exp(-t * 7.5f) * std::sin(tau * (78.0f - 32.0f * t) * t);
        return attack * (decay * ring + thump);
    });
    // Tactical Delta Squad radio squelch chirp.
    writeClip(Sound::RadioChirp, 0.14f, [](float t, float dur) {
        const float env = (t < 0.055f || t > 0.08f) ? std::sin(pi * std::fmod(t, 0.07f) / 0.07f) : 0.0f;
        const float freq = t < 0.07f ? 1120.0f : 1480.0f;
        return 0.42f * env * (1.0f - 0.3f * t / dur) * std::sin(tau * freq * t);
    });
    // Warm two-tone rescue confirmation chime.
    writeClip(Sound::CivilianSaved, 0.34f, [](float t, float dur) {
        const float env = std::exp(-t * 5.0f) * (1.0f - t / dur);
        const float freq = t < 0.12f ? 660.0f : 990.0f;
        return 0.52f * env * (std::sin(tau * freq * t) + 0.25f * std::sin(2.0f * tau * freq * t));
    });
    // Low muffled double heartbeat ("lub-dub") for the concussed awakening.
    writeClip(Sound::Heartbeat, 0.72f, [](float t, float) {
        const auto thump = [](float dt, float freq) {
            if (dt < 0.0f || dt > 0.22f) return 0.0f;
            const float env = std::sin(pi * std::min(1.0f, dt / 0.035f)) * std::exp(-dt * 24.0f);
            return env * std::sin(tau * (freq - 24.0f * dt) * dt);
        };
        return 0.85f * thump(t, 64.0f) + 0.62f * thump(t - 0.24f, 52.0f);
    });
    // Deep eerie alien warhorn / tripod brass blast echoing across the mountain valley.
    writeClip(Sound::AlienHorn, 2.4f, [](float t, float dur) {
        const float attack = std::min(1.0f, t / 0.28f);
        const float release = std::clamp((dur - t) / 0.65f, 0.0f, 1.0f);
        const float env = attack * release;
        const float f0 = 76.0f - 11.0f * (t / dur);
        const float w = tau * f0 * t;
        const float grit = std::sin(w) * 0.55f + std::sin(w * 2.01f) * 0.28f +
                           std::sin(w * 3.02f) * 0.16f + std::sin(w * 4.98f) * 0.08f;
        const float mod = 0.82f + 0.18f * std::sin(tau * 4.5f * t);
        return env * mod * 0.78f * std::tanh(grit * 1.4f);
    });
    // Creature roar: a rattling, buzzing throat with a hissing upper formant, rising then dropping.
    writeClip(Sound::AlienRoar, 0.95f, [](float t, float dur) {
        const float attack = std::min(1.0f, t / 0.05f);
        const float release = std::clamp((dur - t) / 0.35f, 0.0f, 1.0f);
        const float f0 = 92.0f + 58.0f * std::sin(pi * std::min(1.0f, t / dur));
        const float w = tau * f0 * t;
        float buzz = 0;
        for (int h = 1; h <= 7; ++h) buzz += std::sin(w * float(h)) / float(h);
        const uint32_t hash = uint32_t(t * float(audioHz)) * 2654435761u;
        const float hiss = (float((hash >> 9) & 1023) / 511.5f - 1.0f) * (0.22f + 0.18f * std::sin(tau * 7.0f * t));
        const float formant = 0.2f * std::sin(tau * (820.0f + 340.0f * t) * t);
        const float rattle = 0.72f + 0.28f * std::sin(tau * 31.0f * t);
        return attack * release * 0.8f * std::tanh((buzz * 0.55f * rattle + hiss * 0.5f + formant) * 1.6f);
    });
    // Subtle eerie looping mothership sub-bass ambience for quiet tension moments.
    writeClip(Sound::InvasionHum, 2.0f, [](float t, float) {
        const float p1 = tau * 48.0f * t;
        const float p2 = tau * 72.0f * t;
        const float swell = 0.72f + 0.28f * std::sin(tau * 0.5f * t);
        return swell * (0.20f * std::sin(p1) + 0.11f * std::sin(p2) + 0.04f * std::sin(tau * 144.0f * t));
    });
    // Rising alien ray-gun capacitor coil whine before an overcharge or focused burst.
    writeClip(Sound::RayCharge, 0.62f, [](float t, float dur) {
        const float k = t / dur;
        const float env = std::min(1.0f, t / 0.06f) * (1.0f - 0.15f * k * k);
        const float freq = 320.0f + 1180.0f * k * k;
        const float w = tau * (320.0f * t + (1180.0f / 3.0f) * t * k * k);
        const float pulse = 0.72f + 0.28f * std::sin(tau * (18.0f + 28.0f * k) * t);
        return env * pulse * (0.48f * std::sin(w) + 0.24f * std::sin(w * 2.01f) + 0.12f * std::sin(tau * freq * 0.5f * t));
    });
    writeClip(Sound::PlasmaShock, .39f, [](float t, float dur) {
        const uint32_t tick = uint32_t(t * float(audioHz));
        const uint32_t noise = (tick ^ (tick >> 5)) * 2654435761u;
        const float crackle = float((noise >> 16) & 255u) / 127.5f - 1.0f;
        const float pulse = .52f + .48f * std::sin(tau * 57.f * t);
        const float buzz = std::sin(tau * (153.f * t + 5.1f * std::log1p(19.f * t)));
        return (1.f - t / dur) * (.32f * pulse * buzz + .57f * crackle * std::exp(-t * 11.f));
    });
    // Crystalline energy-shield shatter + descending plasma discharge.
    writeClip(Sound::ShieldBreak, 0.34f, [](float t, float dur) {
        const float env = std::exp(-t * 9.5f) * (1.0f - t / dur);
        const uint32_t hash = uint32_t(t * float(audioHz)) * 2654435761u;
        const float spark = (float((hash >> 9) & 1023) / 511.5f - 1.0f) * std::exp(-t * 22.0f);
        const float chime = 0.42f * std::sin(tau * (2100.0f - 1100.0f * (t / dur)) * t) +
                            0.30f * std::sin(tau * (1420.0f - 720.0f * (t / dur)) * t);
        const float drop = 0.32f * std::sin(tau * (480.0f - 340.0f * (t / dur)) * t);
        return env * (chime + drop + 0.38f * spark);
    });
}

} // namespace

void optimizeShipBoxWorld(Assets& assets) {
    auto& world = assets.world;
    // The shipped pack puts the parked vehicles in these seven visual chunks.
    // Keep other pack layouts on their original geometry.
    if (assets.arena != Arena::ShipBox || world.chunks.size() != 175 ||
        world.triangles.size() != 38754 || world.vertices.size() != 42058 ||
        !world.posePositions.empty()) return;
    constexpr std::array<size_t, 7> vehicles{3, 4, 5, 6, 7, 87, 88};
    constexpr std::array<uint32_t, 7> counts{7641, 2212, 3531, 14849, 2663, 2188, 1023};
    for (size_t i = 0; i < vehicles.size(); ++i) {
        if (world.chunks[vehicles[i]].count != counts[i]) return;
    }

    Mesh next;
    next.chunks.reserve(world.chunks.size());
    std::vector<uint32_t> remap(world.vertices.size(), UINT32_MAX);
    const auto copyTriangle = [](const Mesh& source, Triangle t, Mesh& dest, auto& indices) {
        for (auto* index : {&t.a, &t.b, &t.c}) {
            auto& mapped = indices[*index];
            if (mapped == UINT32_MAX) {
                mapped = uint32_t(dest.vertices.size());
                dest.vertices.push_back(source.vertices[*index]);
            }
            *index = mapped;
        }
        dest.triangles.push_back(t);
    };
    const auto smallTriangle = [&](const Triangle& t) {
        // Keep panels and long trim, such as the truck's roll-door ribs, at full detail.
        constexpr float maxEdgeSquared = 0.55f * 0.55f;
        const Vec3 a = world.vertices[t.a].position;
        const Vec3 b = world.vertices[t.b].position;
        const Vec3 c = world.vertices[t.c].position;
        return dot(a - b, a - b) < maxEdgeSquared && dot(b - c, b - c) < maxEdgeSquared &&
               dot(c - a, c - a) < maxEdgeSquared;
    };
    for (size_t i = 0; i < world.chunks.size(); ++i) {
        const auto& chunk = world.chunks[i];
        const auto first = uint32_t(next.triangles.size());
        const bool vehicle = std::find(vehicles.begin(), vehicles.end(), i) != vehicles.end();
        Mesh opaque;
        std::vector<uint32_t> local;
        if (vehicle) {
            opaque.chunks.push_back(chunk);
            local.assign(world.vertices.size(), UINT32_MAX);
        }
        for (size_t j = chunk.first; j < size_t(chunk.first) + chunk.count; ++j) {
            const auto& t = world.triangles[j];
            const auto& material = assets.materials[t.material];
            if (vehicle && t.glass < 0 && material.alpha == 255 && material.doubleSided && smallTriangle(t)) {
                copyTriangle(world, t, opaque, local);
            } else {
                copyTriangle(world, t, next, remap);
            }
        }
        if (!opaque.triangles.empty()) {
            const auto begin = uint32_t(next.vertices.size());
            simplifyVehicleMesh(opaque, 0.05f);
            next.vertices.insert(next.vertices.end(), opaque.vertices.begin(), opaque.vertices.end());
            for (auto t : opaque.triangles) {
                t.a += begin;
                t.b += begin;
                t.c += begin;
                next.triangles.push_back(t);
            }
        }
        const auto count = uint32_t(next.triangles.size()) - first;
        if (count) next.chunks.push_back({chunk.bounds, first, count});
    }
    next.vertices.shrink_to_fit();
    next.triangles.shrink_to_fit();
    next.chunks.shrink_to_fit();
    assets.residentBytes = assets.residentBytes - std::min(assets.residentBytes, meshBytes(world)) + meshBytes(next);
    world = std::move(next);
}

void buildBriarHouse(Assets& assets, BriarHouseMode mode) {
    if (assets.arena == Arena::BriarHouse && assets.briarHouseMode == mode) return;
    const uint64_t shared = assets.residentBytes - std::min(assets.residentBytes, arenaBytes(assets));
    if (assets.arena == Arena::ShipBox) {
        assets.briarBaseline = {assets.materials.size(), assets.textures.size(), assets.audio.size()};
        extractShipBoxProps(assets);
    } else {
        // Meshes carried over from the pack still reference its original material and texture IDs.
        assets.materials.resize(assets.briarBaseline.materials);
        assets.textures.resize(assets.briarBaseline.textures);
        assets.audio.resize(assets.briarBaseline.audio);
    }
    assets.propInstances.clear();
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
    assets.alienRayGun = {};
    assets.alienRayRings = {};
    assets.alienHeavyRayGun = {};
    assets.alienHeavyRayRings = {};
    assets.alienGunSockets = {};
    assets.alienUplink = {};
    assets.alienUplinkCore = {};
    assets.relayTerminal = {};
    assets.civilianMesh = {};
    assets.civilianPoses.clear();
    assets.transportHelo = {};
    assets.transportRotor = {};
    assets.smoke.clear();
    assets.mist.clear();
    assets.fires.clear();
    assets.roadFlares.clear();
    assets.alienDrone = {};
    for (auto& craft : assets.alienFleet) craft = {};
    assets.commanderUniform.clear();
    assets.enforcerUniform.clear();
    for (auto& u : assets.civilianUniforms) u.clear();
    assets.arena = Arena::BriarHouse;
    assets.briarHouseMode = mode;
    if (mode == BriarHouseMode::Invasion) {
        // Open the entire surrounding Colorado mountain town loop and neighborhood yards.
        assets.extent.x = 52.f;
        assets.extent.z = 40.f;
    } else {
        assets.extent.x = 19.5f;
        assets.extent.z = 15.6f;
    }
    assets.spawns = {{
        {position(322, 495), 1.06f}, {position(399, -78), -1.95f},
        {position(-27, 282), .85f}, {position(645, 285), -pi / 2},
    }};

    const uint16_t briarFirstMaterial = uint16_t(assets.materials.size());
    Builder builder(assets, mode == BriarHouseMode::Invasion);
    grounds(builder);
    shell(builder);
    hallStructure(builder);
    builder.shiftX(-72);
    kitchen(builder);
    builder.shiftX(0);
    garageStructure(builder);
    sealedStairs(builder);
    if (mode == BriarHouseMode::Invasion) {
        // Playable town props and cover stay bullet-solid before beginDecoration().
        townCombatProps(builder);
    }
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
    if (mode == BriarHouseMode::Invasion) {
        townDetails(builder);
        mountainCommunity(builder, true);
        invasionBackdrop(builder);
    } else {
        builder.beginBackdrop();
        mountainCommunity(builder, false);
    }
    builder.finish();
    assets.world.triangles.shrink_to_fit();
    navigation(assets);
    if (mode == BriarHouseMode::Invasion) {
        buildAlienMeshes(assets, briarFirstMaterial);
        buildMissionUniforms(assets, briarFirstMaterial);
        synthesizeMissionAudio(assets);
    }
    assets.residentBytes = shared + arenaBytes(assets);
    assets.peakBytes = std::max(assets.peakBytes, assets.residentBytes);
}

} // namespace fod
