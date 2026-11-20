#include "render.hpp"
#include "assets.hpp"
#include "atmosphere_masks.hpp"
#include "game_data.hpp"
#include "light.hpp"
#include "mission.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace fod {
namespace {

using Clock = std::chrono::steady_clock;
#ifdef ESP_PLATFORM
constexpr bool countRenderWork = false;
#else
constexpr bool countRenderWork = true;
#endif

uint32_t microseconds(Clock::time_point start, Clock::time_point end) {
    return uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

struct Projected {
    float x, y, depth, u, v;
};

struct CachedProjection {
    uint32_t index;
    Projected vertex;
};

struct Primitive {
    Projected p[3];
    uint16_t material;
    uint16_t patch = SceneLight::noPatch;
    uint8_t light[3]{}; // One level for each corner.
    bool mipmapped = false;
};

struct Shade {
    uint8_t corner[3];
    uint16_t patch = SceneLight::noPatch;
};

struct LitVertex {
    Vertex vertex;
    float light;
};

enum class Fill { Textured, Flat, Sky, Shadow };

struct PosedVertex {
    Vec3 world, camera;
};

struct TextureSample {
    const Texture* fine;
    const Texture* coarse = nullptr;
    unsigned mix = 0;
};

enum class MeshSpace { World, View };

struct View {
    Vec3 position, right, up, forward;
    float focal, xSlope, ySlope, near;
    int width, height;

    Vec3 transform(Vec3 world) const {
        const Vec3 offset = world - position;
        return {dot(offset, right), dot(offset, up), dot(offset, forward)};
    }

    bool sees(const Bounds& bounds) const {
        const Vec3 center = (bounds.min + bounds.max) * 0.5f;
        const Vec3 extent = (bounds.max - bounds.min) * 0.5f;
        const Vec3 local = transform(center);
        const auto radius = [extent](Vec3 normal) {
            return std::abs(normal.x) * extent.x + std::abs(normal.y) * extent.y + std::abs(normal.z) * extent.z;
        };
        const float zRadius = radius(forward);
        if (local.z + zRadius < near || local.z - zRadius > 110) return false;
        return xSlope * local.z - local.x + radius(forward * xSlope - right) >= 0 &&
               xSlope * local.z + local.x + radius(forward * xSlope + right) >= 0 &&
               ySlope * local.z - local.y + radius(forward * ySlope - up) >= 0 &&
               ySlope * local.z + local.y + radius(forward * ySlope + up) >= 0;
    }

    Projected project(Vertex v) const {
        const float inv = 1 / v.position.z;
        const float scale = focal * inv;
        return {
            v.position.x * scale + width * 0.5f, height * 0.5f - v.position.y * scale,
            inv, v.u * inv, v.v * inv
        };
    }
};

View makeView(const Camera& camera, int width, int height, float near) {
    const Vec3 front = forward(camera.yaw, camera.pitch);
    const Vec3 right{std::cos(camera.yaw), 0, -std::sin(camera.yaw)};
    const float slope = std::tan(camera.fov * 0.5f);
    return {camera.position, right, cross(front, right), front, height * 0.5f / slope,
            slope * width / height, slope, near, width, height};
}

constexpr uint16_t rgb(int r, int g, int b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

uint16_t multiply(uint16_t color, unsigned shade) {
    return uint16_t((((color >> 11) * shade >> 8) << 11) |
                    ((((color >> 5) & 63) * shade >> 8) << 5) |
                    ((color & 31) * shade >> 8));
}

// Ordered thresholds keep a light gradient from banding in five-bit color. A
// two-pixel pattern is fine enough to vanish when the picture is scaled up.
constexpr uint8_t shadeDither[4][4] = {
    {32, 160, 32, 160}, {224, 96, 224, 96}, {32, 160, 32, 160}, {224, 96, 224, 96},
};

uint16_t shaded(uint16_t color, unsigned shade, unsigned dither) {
    return uint16_t((((color >> 11) * shade + dither) >> 8) << 11 |
                    ((((color >> 5) & 63) * shade + dither) >> 8) << 5 |
                    (((color & 31) * shade + dither) >> 8));
}

uint16_t blend(uint16_t a, uint16_t b, unsigned alpha) {
    alpha = (alpha + 4) >> 3;
    const unsigned inv = 32 - alpha;
    constexpr uint32_t mask = 0x07e0f81f;
    const uint32_t from = ((uint32_t(a) << 16) | a) & mask;
    const uint32_t to = ((uint32_t(b) << 16) | b) & mask;
    const uint32_t value = ((from * inv + to * alpha) >> 5) & mask;
    return uint16_t(value | (value >> 16));
}

int edgeLuma(uint16_t color) { return color & 0x7e0; }

uint16_t softenDiagonal(uint16_t center, uint16_t top, uint16_t right, uint16_t bottom, uint16_t left) {
    // Only blend staircase corners. Both axes must cross a high contrast
    // boundary, with one neighbor on the center pixel's side.
    constexpr int edgeContrast = 8 * 32, sameSide = 3 * 32;
    if (std::abs(edgeLuma(top) - edgeLuma(bottom)) < edgeContrast ||
        std::abs(edgeLuma(left) - edgeLuma(right)) < edgeContrast) return center;
    const int mid = edgeLuma(center);
    if (std::abs(edgeLuma(top) - mid) < std::abs(edgeLuma(bottom) - mid)) std::swap(top, bottom);
    if (std::abs(edgeLuma(left) - mid) < std::abs(edgeLuma(right) - mid)) std::swap(left, right);
    if (std::abs(edgeLuma(bottom) - mid) > sameSide || std::abs(edgeLuma(right) - mid) > sameSide ||
        std::abs(edgeLuma(top) - edgeLuma(left)) > sameSide) return center;
    return blend(center, blend(top, left, 128), 64);
}

float edge(const Projected& a, const Projected& b, float x, float y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

Vertex cut(Vertex a, Vertex b, float z) {
    const float t = (z - a.position.z) / (b.position.z - a.position.z);
    return {{a.position.x + (b.position.x - a.position.x) * t,
             a.position.y + (b.position.y - a.position.y) * t, z},
            a.u + (b.u - a.u) * t, a.v + (b.v - a.v) * t};
}

LitVertex cut(const LitVertex& a, const LitVertex& b, float z) {
    const float t = (z - a.vertex.position.z) / (b.vertex.position.z - a.vertex.position.z);
    return {cut(a.vertex, b.vertex, z), a.light + (b.light - a.light) * t};
}

float depthOf(const Vertex& vertex) { return vertex.position.z; }
float depthOf(const LitVertex& vertex) { return vertex.vertex.position.z; }

bool projectSegment(const View& view, Vec3 from, Vec3 to, Projected& p, Projected& q) {
    Vertex a{from, 0, 0}, b{to, 0, 0};
    if (from.z < view.near && to.z < view.near) return false;
    if (from.z < view.near) a = cut(a, b, view.near);
    if (to.z < view.near) b = cut(b, a, view.near);
    p = view.project(a);
    q = view.project(b);
    return true;
}

struct SampleRange { int first, last; };

SampleRange visibleSamples(const View& view, const Projected& a, const Projected& b, int count, float pad) {
    float first = 0.f, last = 1.f;
    const auto clipAxis = [&](float from, float to, float size) {
        const float lo = -pad, hi = size + pad;
        if ((from < lo && to < lo) || (from > hi && to > hi)) return false;
        const float step = to - from;
        if (from < lo) first = std::max(first, (lo - from) / step);
        else if (from > hi) first = std::max(first, (hi - from) / step);
        if (to < lo) last = std::min(last, (lo - from) / step);
        else if (to > hi) last = std::min(last, (hi - from) / step);
        return first <= last;
    };
    if (!clipAxis(a.x, b.x, float(view.width)) || !clipAxis(a.y, b.y, float(view.height))) return {1, 0};
    // Keep the original sampling grid. Only skip samples beyond the padded viewport.
    return {std::max(0, int(first * count) - 1), std::min(count, int(last * count) + 1)};
}

float unitCycle(float phase) { return phase - std::trunc(phase); }

bool intact(const Snapshot& state, int16_t glass) {
    return glass < 0 || glass >= 64 || !(state.brokenGlass & (uint64_t(1) << glass));
}

struct Transform {
    Vec3 origin, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};

    Vec3 operator()(Vec3 v) const { return origin + x * v.x + y * v.y + z * v.z; }
};

Transform orientation(Vec3 origin, float yaw, float pitch, float roll) {
    const Vec3 f = forward(yaw, pitch);
    const Vec3 right{std::cos(yaw), 0, -std::sin(yaw)};
    const Vec3 up = cross(f, right);
    return {origin, right * std::cos(roll) + up * std::sin(roll),
            up * std::cos(roll) - right * std::sin(roll), f};
}

uint8_t lightFor(Vec3 normal, bool metallic) {
    const float mag = std::max(length(normal), 0.000001f);
    const float lit = std::abs(dot(normal, Vec3{-0.28f, 0.86f, 0.43f})) / mag;
    return uint8_t(std::clamp(190.0f + lit * 65 + (metallic ? 20 : 0), 0.0f, 255.0f));
}

// Use the exponent and top mantissa bits of reciprocal depth. This retains
// the 16-bit buffer while allocating precision over the entire view distance.
uint16_t encodedDepth(float inverseZ) {
    if (inverseZ <= 0) return 0;
    uint32_t bits;
    std::memcpy(&bits, &inverseZ, sizeof(bits));
    constexpr uint32_t first = 0x3b800000; // 1 / 256
    constexpr uint32_t last = first + (65534u << 11);
    return uint16_t(1 + ((std::clamp(bits, first, last) - first) >> 11));
}

float decodedDepth(uint16_t encoded) {
    const uint32_t bits = 0x3b800000 + (uint32_t(std::max<int>(encoded, 1) - 1) << 11);
    float inverseZ;
    std::memcpy(&inverseZ, &bits, sizeof(inverseZ));
    return 1 / inverseZ;
}

constexpr auto skyNoise() {
    std::array<uint8_t, 64 * 64> noise{};
    uint32_t seed = 12343;
    for (auto& value : noise) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        value = uint8_t(seed >> 30);
    }
    return noise;
}

constexpr auto skyDither = skyNoise();

constexpr auto skyPalette = [] {
    std::array<uint16_t, 256> colors{};
    for (int value = 0; value < int(colors.size()); ++value)
        colors[value] = rgb(std::max(0, value - 5), value, std::min(255, value + 15));
    return colors;
}();

// Low west-northwest Colorado Front Range sunset direction (matches dusk sun in light.cpp).
constexpr Vec3 sunsetSunDir{-0.8780f, 0.1497f, 0.4545f};

uint16_t rgbClamped(float r, float g, float b) {
    return rgb(std::clamp(int(r + 0.5f), 0, 255), std::clamp(int(g + 0.5f), 0, 255),
               std::clamp(int(b + 0.5f), 0, 255));
}

// Face axes match the panorama conversion in the exporter.
struct SkyAxes { Vec3 normal, right, vertical; };
constexpr SkyAxes skyAxes[]{
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
};

Vec3 duskGradient(float elevation) {
    struct Stop { float elevation; Vec3 color; };
    constexpr Stop palette[]{
        {-1.f, {74, 58, 65}}, {0.f, {185, 96, 76}}, {.15f, {150, 72, 76}},
        {.39f, {86, 58, 85}}, {.68f, {39, 43, 72}}, {1.f, {24, 30, 56}},
    };
    size_t next = 1;
    while (next + 1 < std::size(palette) && elevation > palette[next].elevation) ++next;
    const auto& a = palette[next - 1];
    const auto& b = palette[next];
    const float t = (elevation - a.elevation) / (b.elevation - a.elevation);
    return a.color + (b.color - a.color) * t;
}

Texture halfTexture(const Texture& source) {
    Texture texture{std::max(1, source.width / 2), std::max(1, source.height / 2), {}};
    texture.pixels.resize(texture.width * texture.height);
    for (int y = 0; y < texture.height; ++y) {
        for (int x = 0; x < texture.width; ++x) {
            unsigned red = 0, green = 0, blue = 0, count = 0;
            for (int sy = y * source.height / texture.height; sy < (y + 1) * source.height / texture.height; ++sy) {
                for (int sx = x * source.width / texture.width; sx < (x + 1) * source.width / texture.width; ++sx) {
                    const auto color = source.pixels[sy * source.width + sx];
                    red += color >> 11;
                    green += (color >> 5) & 63;
                    blue += color & 31;
                    ++count;
                }
            }
            texture.pixels[y * texture.width + x] = uint16_t(((red + count / 2) / count) << 11 |
                ((green + count / 2) / count) << 5 | ((blue + count / 2) / count));
        }
    }
    return texture;
}

} // namespace

struct Renderer::Impl {
    const Assets& assets;
    const bool enhanced;
    const bool invasionDusk;
    std::vector<uint16_t> depth;
    std::vector<Vec3> culledNormals;
    std::vector<uint8_t> worldLights;
    std::vector<uint16_t> edgeRow;
    std::vector<std::vector<Texture>> mipmaps;
    std::array<std::vector<uint16_t>, 6> sunsetSky;
    std::vector<PosedVertex> posedVertices;
    // Reused for each mesh and view. The world keeps its cache across visible chunks.
    std::array<CachedProjection, 128> projections{};
    std::vector<Primitive> solid, translucent;
    std::vector<std::pair<float, uint32_t>> visible;
    RenderStats stats{};
    bool measure = false;
    View view{};
    uint16_t* color = nullptr;
    SceneLight light;
    // Haze strength in thirty-seconds for the top ten bits of an encoded depth,
    // and the haze color scaled by each strength.
    std::array<uint8_t, 1024> haze{};
    std::array<uint32_t, 33> hazeBlend{};
    float hazeStart = 0;
    int duskMistBucket = -1;
    struct AirSprite {
        float x, y, width, height;
        uint16_t back, front, top, bottom;
        uint8_t alpha;
        const atmosphere::Mask* mask;
    };
    static constexpr size_t mistLayers = 2, smokePuffs = 10, fireParticles = 8, flarePuffs = 2;
    std::vector<AirSprite> airSprites;
    struct FireOffset { float phase, x, z; };
    struct FirePattern { std::array<FireOffset, fireParticles> flames, sparks; };
    std::vector<FirePattern> firePatterns;
    // Per-frame glowing muzzle orbs and shield rims collected during drawMissionActor.
    struct MuzzleGlow {
        Vec3 pos{};
        float radius = 0;
        uint16_t core = 0, halo = 0;
        float intensity = 0;
        bool star = false;
        bool plasma = false;
    };
    struct ShieldGlow {
        Vec3 pos{};
        float radius = 0, height = 0;
        uint16_t color = 0;
        float strength = 0;
    };
    std::array<MuzzleGlow, MissionSnapshot::maxActors> muzzleGlows{};
    size_t muzzleGlowCount = 0;
    std::array<ShieldGlow, MissionSnapshot::maxActors> shieldGlows{};
    size_t shieldGlowCount = 0;
    // Daylight in the space of the mesh being submitted, and the ground light where it stands.
    Vec3 sun{}, up{0, 1, 0};
    unsigned meshLight = 255;

    Clock::time_point timestamp() const { return measure ? Clock::now() : Clock::time_point{}; }

    void recordRasterTime(Clock::time_point start) {
        if (measure) stats.rasterUs += microseconds(start, Clock::now());
    }

    explicit Impl(const Assets& source, RenderLook look, size_t maxPixels)
        : assets(source), enhanced(look == RenderLook::Enhanced),
          invasionDusk(source.arena == Arena::BriarHouse && source.briarHouseMode == BriarHouseMode::Invasion),
          depth(maxPixels) {
        if (invasionDusk) {
            const size_t particles = source.mist.size() * mistLayers + source.smoke.size() * smokePuffs +
                                     source.fires.size() * fireParticles + source.roadFlares.size() * flarePuffs;
            airSprites.reserve(std::min<size_t>(particles, 180));
            firePatterns.resize(source.fires.size());
            for (size_t i = 0; i < source.fires.size(); ++i) {
                const float width = source.fires[i].width;
                for (size_t j = 0; j < fireParticles; ++j) {
                    const float flame = float(i) * 1.7f + j * 2.39996f;
                    const float spark = float(i) * 3.1f + j * 2.39996f;
                    firePatterns[i].flames[j] = {flame, std::sin(flame) * width * .69f, std::cos(flame) * width * .58f};
                    firePatterns[i].sparks[j] = {spark, std::sin(spark) * width, std::cos(spark) * width};
                }
            }
        }
        // Opaque primitives can flush in submission order. Keep every translucent
        // primitive for the depth-tested, back-to-front pass that follows.
        solid.reserve(256);
        const auto alphaCount = [&source](const Mesh& mesh) {
            return size_t(std::count_if(mesh.triangles.begin(), mesh.triangles.end(), [&source](const Triangle& triangle) {
                return source.materials[triangle.material].alpha < 255;
            }));
        };
        size_t maxActorAlpha = 0, maxWeaponAlpha = 0;
        size_t vertices = std::max(source.apache.vertices.size(), source.jet.vertices.size());
        for (const auto& frame : source.rivalFrames) {
            maxActorAlpha = std::max(maxActorAlpha, alphaCount(frame));
            vertices = std::max(vertices, frame.vertexCount());
        }
        for (const auto& frame : source.hanzFrames) {
            maxActorAlpha = std::max(maxActorAlpha, alphaCount(frame));
            vertices = std::max(vertices, frame.vertexCount());
        }
        maxActorAlpha = std::max(maxActorAlpha, alphaCount(source.playerBody));
        vertices = std::max(vertices, source.playerBody.vertices.size());
        vertices = std::max({vertices, source.alienTrooper.vertices.size(), source.alienEnforcer.vertices.size(),
                             source.alienRayGun.vertices.size(), source.alienRayRings.vertices.size(),
                             source.alienHeavyRayGun.vertices.size(), source.alienHeavyRayRings.vertices.size()});
        vertices = std::max(vertices, source.civilianMesh.vertices.size());
        vertices = std::max({vertices, source.transportHelo.vertices.size(), source.transportRotor.vertices.size()});
        vertices = std::max(vertices, source.alienDrone.vertices.size());
        vertices = std::max({vertices, source.alienUplink.vertices.size(), source.alienUplinkCore.vertices.size()});
        size_t fleetAlpha = 0;
        for (const auto& craft : source.alienFleet) {
            vertices = std::max(vertices, craft.vertices.size());
            fleetAlpha += alphaCount(craft);
        }
        size_t propAlpha = 0;
        if (!source.propInstances.empty()) {
            vertices = std::max({vertices, source.shipBoxTruck.vertices.size(), source.shipBoxCar.vertices.size()});
            const size_t truckAlpha = alphaCount(source.shipBoxTruck);
            const size_t carAlpha = alphaCount(source.shipBoxCar);
            for (const auto& inst : source.propInstances) {
                propAlpha += inst.kind == PropKind::Truck ? truckAlpha : carAlpha;
            }
        }
        for (const auto& weapon : source.weapon) {
            maxWeaponAlpha = std::max(maxWeaponAlpha, alphaCount(weapon));
            vertices = std::max(vertices, weapon.vertices.size());
        }
        const size_t worldAlpha = alphaCount(source.world) + propAlpha + fleetAlpha + 5 * maxActorAlpha +
                                  alphaCount(source.apache) + 3 * alphaCount(source.jet);
        translucent.reserve(2 * std::max(worldAlpha, maxWeaponAlpha)); // One near-plane split per triangle.
        posedVertices.resize(vertices);
        visible.reserve(source.world.chunks.size());

        const auto& world = source.world;
        if (!enhanced) worldLights.reserve(world.triangles.size());
        const bool oneSided = std::any_of(world.triangles.begin(), world.triangles.end(), [&source](const Triangle& triangle) {
            return !source.materials[triangle.material].doubleSided;
        });
        if (oneSided) culledNormals.resize(world.triangles.size());
        for (size_t i = 0; i < world.triangles.size(); ++i) {
            const auto& triangle = world.triangles[i];
            const Vec3 a = world.vertices[triangle.a].position;
            const Vec3 normal = cross(world.vertices[triangle.b].position - a, world.vertices[triangle.c].position - a);
            if (!enhanced) worldLights.push_back(lightFor(normal, false));
            if (oneSided) culledNormals[i] = normal;
        }
        if (enhanced) {
            light.bake(source);
            if (invasionDusk) {
                buildSunsetSky();
                updateDuskMist(0.72f, sunsetSunDir);
            } else {
                mist();
            }
            mipmaps.resize(source.textures.size());
            const auto buildMeshMips = [&](const Mesh& mesh) {
                for (const auto& triangle : mesh.triangles) {
                    const auto id = source.materials[triangle.material].texture;
                    if (id < 0 || !mipmaps[id].empty()) continue;
                    const Texture* previous = &source.textures[id];
                    auto& levels = mipmaps[id];
                    while (std::max(previous->width, previous->height) > 4) {
                        levels.push_back(halfTexture(*previous));
                        previous = &levels.back();
                    }
                }
            };
            buildMeshMips(world);
            if (!source.propInstances.empty()) {
                buildMeshMips(source.shipBoxTruck);
                buildMeshMips(source.shipBoxCar);
            }
        }
    }

    // The palette cools with elevation and away from the sun. Use the pack's
    // cloud luminance directly, so building either arena never changes the sky asset.
    void buildSunsetSky() {
        const auto atmosphere = [](Vec3 dir, float cloud) {
            Vec3 color = duskGradient(dir.y);
            const float sun = std::max(0.f, dot(dir, sunsetSunDir));
            const float horizon = std::max(0.f, 1.f - std::abs(dir.y) * 1.8f);
            const float corona = std::pow(sun, 14.f), disk = std::pow(sun, 192.f);
            color += Vec3{71, 44, 9} * (sun * sun * horizon) + Vec3{52, 82, 38} * corona
                   + Vec3{42, 46, 65} * disk;
            const Vec3 lowCloud{38.f + 32.f * sun, 24.f + 26.f * sun, 11.f};
            color += (Vec3{-16, -19, -10} * (1.f - horizon) + lowCloud * horizon) * (cloud * 2.f - 1.f);
            const float deck = atmosphere::noise(int(125.f + 54.f * dir.x + 30.f * dir.z),
                                                 int(360.f + 227.f * dir.y + 12.f * dir.z), 13);
            const float bank = std::clamp((deck * .64f + cloud * .36f - .35f) * 2.55f, 0.f, 1.f);
            const float elevation = std::clamp(1.f - 2.7f * std::abs(dir.y - .21f), 0.f, 1.f);
            color += Vec3{-54.f + 23.f * sun, -34.f, -19.f} * (bank * elevation);
            return rgbClamped(color.x, color.y, color.z);
        };
        constexpr int size = skyFaceSize;
        for (size_t face = 0; face < sunsetSky.size(); ++face) {
            auto& pixels = sunsetSky[face];
            pixels.resize(size * size);
            const auto& cloud = assets.sky[face];
            const bool hasCloud = cloud.size() == pixels.size();
            const auto& [normal, right, vertical] = skyAxes[face];
            for (int y = 0; y < size; ++y) {
                const float v = (2.f * y + 1.f) / size - 1.f;
                for (int x = 0; x < size; ++x) {
                    const float u = (2.f * x + 1.f) / size - 1.f;
                    const size_t index = size_t(y) * size + x;
                    const float luminance = hasCloud ? cloud[index] / 255.f : .5f;
                    pixels[index] = atmosphere(normalized(normal + right * u + vertical * v), luminance);
                }
            }
        }
    }

    // The color tracks the western sky. The depth curve only rebuilds as the mist thickens.
    void updateDuskMist(float density, Vec3 lookDir) {
        density = std::clamp(density, 0.25f, 1.0f);
        const float sunAlign = std::clamp(dot(lookDir, sunsetSunDir) * 0.5f + 0.5f, 0.f, 1.f);
        const float warm = sunAlign * sunAlign * 0.45f + sunAlign * 0.55f;
        const float cold = std::clamp((density - .72f) * 3.f, 0.f, .8f);
        const auto shade = rgbClamped(127.f - 35.f * cold, 113.f + 3.f * cold, 133.f + 19.f * cold);
        const uint16_t tone = blend(shade, rgb(222, 139, 102), unsigned(warm * (248.f - cold * 96.f)));
        for (uint32_t level = 0; level < hazeBlend.size(); ++level)
            hazeBlend[level] = (((uint32_t(tone) << 16) | tone) & 0x07e0f81f) * level;
        const int bucket = int(density * 40.f + 0.5f);
        if (bucket == duskMistBucket) return;
        duskMistBucket = bucket;
        const float start = 13.5f - 7.5f * density;
        const float reach = 35.f - 13.f * density;   // Smooth atmospheric roll-off
        const float limit = 0.74f + 0.16f * density; // Up to 0.90 in deep distance
        hazeStart = 1.f / start;
        for (size_t i = 0; i < haze.size(); ++i) {
            const float distance = decodedDepth(uint16_t(i * 64 + 32));
            haze[i] = distance > start ? uint8_t(32 * limit * (1 - std::exp((start - distance) / reach)) + 0.5f) : 0;
        }
    }

    // Distant geometry fades toward the horizon of the pack's own sky.
    void mist() {
        constexpr float start = 18, reach = 45, limit = 0.72f;
        unsigned sum = 0, count = 0;
        for (size_t face = 0; face < 4; ++face) {
            const auto& pixels = assets.sky[face];
            if (pixels.size() != size_t(skyFaceSize) * skyFaceSize) continue;
            for (int i = (skyFaceSize / 2 - 2) * skyFaceSize; i < (skyFaceSize / 2 + 2) * skyFaceSize; ++i) {
                sum += pixels[i];
                ++count;
            }
        }
        const uint16_t tone = count ? skyPalette[std::min(sum / count + 2, 255u)] : rgb(135, 143, 153);
        for (uint32_t level = 0; level < hazeBlend.size(); ++level)
            hazeBlend[level] = (((uint32_t(tone) << 16) | tone) & 0x07e0f81f) * level;
        hazeStart = 1 / start;
        for (size_t i = 0; i < haze.size(); ++i) {
            const float distance = decodedDepth(uint16_t(i * 64 + 32));
            haze[i] = distance > start ? uint8_t(32 * limit * (1 - std::exp((start - distance) / reach)) + 0.5f) : 0;
        }
    }

    bool project(Primitive& value, Vertex a, Vertex b, Vertex c) const {
        value.p[0] = view.project(a);
        value.p[1] = view.project(b);
        value.p[2] = view.project(c);
        return visibleProjection(value) != 0;
    }

    // Order the corners for the rasterizer. Return 1 for the front of a face,
    // -1 for its back and 0 when it stays off screen. A front face swaps its last two corners.
    int visibleProjection(Primitive& value) const {
        const float area = edge(value.p[0], value.p[1], value.p[2].x, value.p[2].y);
        if (std::abs(area) < 0.09f) return 0;
        if (area < 0) std::swap(value.p[1], value.p[2]);
        const float minX = std::min({value.p[0].x, value.p[1].x, value.p[2].x});
        const float maxX = std::max({value.p[0].x, value.p[1].x, value.p[2].x});
        const float minY = std::min({value.p[0].y, value.p[1].y, value.p[2].y});
        const float maxY = std::max({value.p[0].y, value.p[1].y, value.p[2].y});
        if (!(minX < view.width && maxX >= 0 && minY < view.height && maxY >= 0)) return 0;
        return area < 0 ? 1 : -1;
    }

    static void illuminate(Primitive& value, const Shade& shade, bool front) {
        value.patch = shade.patch;
        value.light[0] = shade.corner[0];
        value.light[1] = shade.corner[front ? 2 : 1];
        value.light[2] = shade.corner[front ? 1 : 2];
    }

    void enqueue(const Primitive& value) {
        if (assets.materials[value.material].alpha == 255) {
            if (solid.size() == solid.capacity()) draw(solid, true);
            solid.push_back(value);
        } else {
            translucent.push_back(value);
        }
        if (countRenderWork && measure) ++stats.triangles;
    }

    template<class Corner, class Submit>
    void clip(const Corner* v, Submit submit) {
        unsigned mask = 0;
        for (unsigned i = 0; i < 3; ++i) mask |= unsigned(depthOf(v[i]) >= view.near) << i;
        if (mask == 0) return;
        if (mask == 7) {
            submit(v[0], v[1], v[2]);
            return;
        }
        Corner clipped[4]{};
        int n = 0;
        for (int i = 0; i < 3; ++i) {
            const int previous = (i + 2) % 3;
            const bool fromInside = (mask & (1u << previous)) != 0;
            const bool toInside = (mask & (1u << i)) != 0;
            if (fromInside != toInside) clipped[n++] = cut(v[previous], v[i], view.near);
            if (toInside) clipped[n++] = v[i];
        }
        for (int i = 1; i + 1 < n; ++i) submit(clipped[0], clipped[i], clipped[i + 1]);
    }

    void resetProjections() {
        for (auto& entry : projections) entry.index = UINT32_MAX;
    }

    template<class CameraVertex>
    Projected projectVertex(uint32_t index, const CameraVertex& vertex) {
        auto& entry = projections[index % projections.size()];
        if (entry.index != index) {
            const Vertex camera = vertex(index);
            entry = {index, camera.position.z >= view.near ? view.project(camera) : Projected{}};
        }
        return entry.vertex;
    }

    // The shader receives the side that faces the viewer and returns a light level for each corner.
    template<class CameraVertex, class Shader>
    void submitFace(const Triangle& tri, uint16_t material, const CameraVertex& vertex, Shader shade, bool mipmapped = false) {
        // Copy each projection before another index can reuse its cache slot.
        Primitive value{{projectVertex(tri.a, vertex), projectVertex(tri.b, vertex), projectVertex(tri.c, vertex)},
                        material, SceneLight::noPatch, {}, mipmapped};
        const auto& [a, b, c] = value.p;
        // Zero reciprocal depth marks vertices behind the near plane.
        if (a.depth > 0 && b.depth > 0 && c.depth > 0) {
            if (const int facing = visibleProjection(value)) {
                illuminate(value, shade(facing > 0), facing > 0);
                enqueue(value);
            }
            return;
        }
        if (a.depth == 0 && b.depth == 0 && c.depth == 0) return;

        const Vertex vertices[]{vertex(tri.a), vertex(tri.b), vertex(tri.c)};
        const Vec3 origin = vertices[0].position;
        const bool front = dot(cross(vertices[1].position - origin, vertices[2].position - origin), origin) > 0;
        const Shade lit = shade(front);
        const LitVertex corners[]{
            {vertices[0], float(lit.corner[0])}, {vertices[1], float(lit.corner[1])}, {vertices[2], float(lit.corner[2])},
        };
        clip(corners, [&](const LitVertex& x, const LitVertex& y, const LitVertex& z) {
            Primitive clipped{{}, material, lit.patch, {}, mipmapped};
            const int facing = project(clipped, x, y, z);
            if (!facing) return;
            enqueue(clipped);
        });
    }

    // Project a clipped face and round each corner's light. The result follows visibleProjection.
    int project(Primitive& value, const LitVertex& a, const LitVertex& b, const LitVertex& c) const {
        value.p[0] = view.project(a.vertex);
        value.p[1] = view.project(b.vertex);
        value.p[2] = view.project(c.vertex);
        const int facing = visibleProjection(value);
        const auto level = [](const LitVertex& corner) { return uint8_t(std::clamp(corner.light + 0.5f, 0.0f, 255.0f)); };
        value.light[0] = level(a);
        value.light[1] = level(facing > 0 ? c : b);
        value.light[2] = level(facing > 0 ? b : c);
        return facing;
    }

    struct MotionBlend {
        const WeaponPose* from;
        const WeaponPose* to;
        float fraction;

        Vec3 position(size_t i) const {
            if (from->packed.empty()) return from->positions[i] + (to->positions[i] - from->positions[i]) * fraction;
            const auto& a = from->packed[i];
            const auto& b = to->packed[i];
            return Vec3{a.x + (b.x - a.x) * fraction, a.y + (b.y - a.y) * fraction, a.z + (b.z - a.z) * fraction}
                * (1.f / 8192);
        }
    };

    void submit(const Mesh& mesh, const Snapshot& state, const Transform& transform, MeshSpace space,
                const Mesh* next = nullptr, float fraction = 0, bool metallic = false,
                const Mesh* topology = nullptr, const uint16_t* uniform = nullptr, const MotionBlend* motion = nullptr,
                bool mipmapped = false) {
        resetProjections();
        const auto& base = topology ? *topology : mesh;
        const size_t vertices = mesh.vertexCount();
        for (size_t i = 0; i < vertices; ++i) {
            Vec3 position = motion ? motion->position(i) : mesh.position(i);
            if (next) position += (next->position(i) - (motion ? mesh.position(i) : position)) * fraction;
            const Vec3 world = transform(position);
            posedVertices[i] = {world, space == MeshSpace::View ? world : view.transform(world)};
        }
        const auto vertex = [&](uint32_t i) { return Vertex{posedVertices[i].camera, base.vertices[i].u, base.vertices[i].v}; };
        for (const Triangle& tri : base.triangles) {
            if (!intact(state, tri.glass)) continue;
            const auto& a = posedVertices[tri.a];
            const auto& b = posedVertices[tri.b];
            const auto& c = posedVertices[tri.c];
            const auto material = uniform ? uniform[tri.material] : tri.material;
            if (assets.materials[material].alpha == 0) continue;
            const bool doubleSided = assets.materials[material].doubleSided;
            const auto faceNormal = [&] { return cross(b.world - a.world, c.world - a.world); };
            Vec3 normal{};
            if (!doubleSided) {
                normal = faceNormal();
                if (dot(normal, a.world - view.position) <= 0) continue;
            }
            submitFace(tri, material, vertex, [&](bool front) {
                const Vec3 turned = doubleSided ? faceNormal() : normal;
                const uint8_t level = enhanced ? SceneLight::surface(front ? turned * -1 : turned, sun, up, meshLight)
                                               : lightFor(turned, metallic);
                return Shade{{level, level, level}};
            }, mipmapped);
        }
    }

    void submitWorld(size_t first, size_t count, const Snapshot& state) {
        const auto& mesh = assets.world;
        const auto vertex = [&](uint32_t i) {
            Vertex value = mesh.vertices[i];
            value.position = view.transform(value.position);
            return value;
        };
        for (size_t i = first; i < first + count; ++i) {
            const Triangle& tri = mesh.triangles[i];
            if (!intact(state, tri.glass)) continue;
            if (!assets.materials[tri.material].doubleSided &&
                dot(culledNormals[i], mesh.vertices[tri.a].position - view.position) <= 0) continue;
            submitFace(tri, tri.material, vertex, [&](bool front) {
                if (!enhanced) return Shade{{worldLights[i], worldLights[i], worldLights[i]}};
                const FaceLight& face = light.faces[i];
                if (!face.back) {
                    // The light map covers the top of a ground face. Its underside stays dim.
                    if (front) return Shade{{255, 255, 255}, uint16_t(face.front[0] | face.front[1] << 8)};
                    return Shade{{96, 96, 96}};
                }
                if (front) return Shade{{face.front[0], face.front[1], face.front[2]}};
                return Shade{{face.back, face.back, face.back}};
            }, enhanced);
        }
    }

    static int texel(float value, int size) {
        const int integral = int(value);
        const int floored = integral - int(value < integral);
        if ((size & (size - 1)) == 0) return floored & (size - 1);
        int wrapped = floored % size;
        return wrapped < 0 ? wrapped + size : wrapped;
    }

    static uint16_t sampleSky(const uint8_t* face, float u, float v, int pixelX, int pixelY) {
        constexpr int size = skyFaceSize;
        constexpr int last = (size - 1) * 256;
        const int x = int(std::clamp(u, 0.0f, 1.0f) * last);
        const int y = int(std::clamp(v, 0.0f, 1.0f) * last);
        const int x0 = x >> 8, y0 = y >> 8;
        const int x1 = std::min(x0 + 1, size - 1), y1 = std::min(y0 + 1, size - 1);
        const int fx = x & 255, fy = y & 255;
        const int a = face[y0 * size + x0] * (256 - fx) + face[y0 * size + x1] * fx;
        const int b = face[y1 * size + x0] * (256 - fx) + face[y1 * size + x1] * fx;
        const int luma = (a * (256 - fy) + b * fy + 32768) >> 16;
        const int bias = skyDither[(pixelY & 63) * 64 + (pixelX & 63)];
        return skyPalette[std::min(luma + bias, 255)];
    }

    static uint16_t sampleSunsetSky(const uint16_t* face, float u, float v, int pixelX, int pixelY) {
        constexpr int size = skyFaceSize;
        constexpr int last = (size - 1) * 256;
        const int x = int(std::clamp(u, 0.0f, 1.0f) * last);
        const int y = int(std::clamp(v, 0.0f, 1.0f) * last);
        const int x0 = x >> 8, y0 = y >> 8;
        const int x1 = std::min(x0 + 1, size - 1), y1 = std::min(y0 + 1, size - 1);
        const int fx = x & 255, fy = y & 255;
        const int ix = 256 - fx, iy = 256 - fy;
        const uint16_t c00 = face[y0 * size + x0], c10 = face[y0 * size + x1];
        const uint16_t c01 = face[y1 * size + x0], c11 = face[y1 * size + x1];
        const int w00 = ix * iy, w10 = fx * iy, w01 = ix * fy, w11 = fx * fy;
        // Full-precision RGB565 bilinear + per-screen-pixel dither eliminates cubemap texel blocks & banding.
        const int dith = int(skyDither[(pixelY & 63) * 64 + (pixelX & 63)]) << 14;
        const int r = ((c00 >> 11) * w00 + (c10 >> 11) * w10 + (c01 >> 11) * w01 + (c11 >> 11) * w11 + dith) >> 16;
        const int g = (((c00 >> 5) & 63) * w00 + ((c10 >> 5) & 63) * w10 +
                       ((c01 >> 5) & 63) * w01 + ((c11 >> 5) & 63) * w11 + dith) >> 16;
        const int b = ((c00 & 31) * w00 + (c10 & 31) * w10 + (c01 & 31) * w01 + (c11 & 31) * w11 + dith) >> 16;
        return uint16_t((std::min(r, 31) << 11) | (std::min(g, 63) << 5) | std::min(b, 31));
    }

    // Faces whose light changes across them, or which reach into the haze, take
    // the detailed fill. Every other face keeps the plain inner loop.
    bool detailed(const Primitive& t) const {
        return t.patch != SceneLight::noPatch || t.light[0] != t.light[1] || t.light[0] != t.light[2] ||
               std::min({t.p[0].depth, t.p[1].depth, t.p[2].depth}) < hazeStart;
    }

    uint16_t veil(uint16_t pixel, unsigned level) const {
        constexpr uint32_t mask = 0x07e0f81f;
        const uint32_t from = ((uint32_t(pixel) << 16) | pixel) & mask;
        const uint32_t value = ((from * (32 - level) + hazeBlend[level]) >> 5) & mask;
        return uint16_t(value | (value >> 16));
    }

    template<Fill fill, bool detail = false, bool countPixels = false>
    void raster(const Primitive& t, bool opaque, const uint8_t* skyPixels = nullptr, const uint16_t* sunsetPixels = nullptr) {
        constexpr bool backdrop = fill == Fill::Sky, shadow = fill == Fill::Shadow;
        constexpr bool textured = fill == Fill::Textured, surface = textured || fill == Fill::Flat;
        constexpr bool usesUv = textured || backdrop;
        const auto& [a, b, c] = t.p;
        const Material* material = surface ? &assets.materials[t.material] : nullptr;
        const Texture* base = textured ? &assets.textures[material->texture] : nullptr;
        const int minX = int(std::max(0.0f, std::ceil(std::min({a.x, b.x, c.x}) - 0.5f)));
        const int minY = int(std::max(0.0f, std::ceil(std::min({a.y, b.y, c.y}) - 0.5f)));
        const int maxX = int(std::min(float(view.width - 1), std::floor(std::max({a.x, b.x, c.x}) - 0.5f)));
        const int maxY = int(std::min(float(view.height - 1), std::floor(std::max({a.y, b.y, c.y}) - 0.5f)));
        if (minX > maxX || minY > maxY) return;

        const float inverseArea = 1.0f / edge(a, b, c.x, c.y);
        const float dx[3]{b.y - c.y, c.y - a.y, a.y - b.y};
        const float dy[3]{c.x - b.x, a.x - c.x, b.x - a.x};
        const uint16_t flat = material ? multiply(material->color, t.light[0]) : 0;
        const float depthScale = view.near * 65534;

        const auto gradient = [&](float aa, float bb, float cc, const float* delta) {
            return (aa * delta[0] + bb * delta[1] + cc * delta[2]) * inverseArea;
        };
        const float dzdx = gradient(a.depth, b.depth, c.depth, dx);
        const float dudx = usesUv ? gradient(a.u, b.u, c.u, dx) : 0.f;
        const float dvdx = usesUv ? gradient(a.v, b.v, c.v, dx) : 0.f;

        float dzdy = 0, dudy = 0, dvdy = 0;
        const std::vector<Texture>* levels = nullptr;
        const auto mipAt = [&](float z, float uOverZ, float vOverZ) {
            const float inv = 1 / z;
            const float u = uOverZ * inv, v = vOverZ * inv;
            float footprint = std::max(
                std::max(std::abs(dudx - u * dzdx), std::abs(dudy - u * dzdy)) * base->width,
                std::max(std::abs(dvdx - v * dzdx), std::abs(dvdy - v * dzdy)) * base->height) * inv;
            const Texture* fine = base;
            for (const auto& next : *levels) {
                if (footprint <= 1.3f) break;
                if (footprint < 2.1f) return TextureSample{fine, &next, unsigned((footprint - 1.3f) * 320)};
                fine = &next;
                footprint *= 0.5f;
            }
            return TextureSample{fine};
        };
        TextureSample triangleTexture{base};
        bool perspectiveMips = false;
        if (base && t.mipmapped && !mipmaps[material->texture].empty()) {
            levels = &mipmaps[material->texture];
            dzdy = gradient(a.depth, b.depth, c.depth, dy);
            dudy = gradient(a.u, b.u, c.u, dy);
            dvdy = gradient(a.v, b.v, c.v, dy);
            // A wall or floor can cover many depths on one triangle. Sample
            // the local footprint on short spans to keep the near end sharp
            // and the far end stable. Small or shallow faces share one mip.
            perspectiveMips = std::max(maxX - minX, maxY - minY) >= 24 &&
                std::max({a.depth, b.depth, c.depth}) > 1.35f * std::min({a.depth, b.depth, c.depth});
            if (!perspectiveMips)
                triangleTexture = mipAt((a.depth + b.depth + c.depth) / 3, (a.u + b.u + c.u) / 3, (a.v + b.v + c.v) / 3);
        }

        // Light is 16.16 fixed point. It follows the corners across a face, or
        // comes from the ground map at the ends of each span.
        constexpr int spanPixels = 16;
        const GroundPatch* patch = detail && base && t.patch != SceneLight::noPatch ? &light.patches[t.patch] : nullptr;
        const bool smooth = detail && !patch && (t.light[0] != t.light[1] || t.light[0] != t.light[2]);
        const bool varying = smooth || patch;
        const bool hazy = detail && surface && std::min({a.depth, b.depth, c.depth}) < hazeStart;
        const int span = patch || perspectiveMips ? spanPixels : 0;
        const float rowLimit = float(maxX - minX + 1);
        // A sliver can change light faster than one level range per pixel. Bound the step so it stays an integer.
        const int cornerStep = smooth ? int(std::clamp(gradient(t.light[0], t.light[1], t.light[2], dx), -255.0f, 255.0f) * 65536) : 0;
        float rowCells = 0, columnCells = 0;
        if (patch) {
            if (!levels) dzdy = gradient(a.depth, b.depth, c.depth, dy);
            // Ground covered by one pixel, in light map cells per unit depth. A span
            // reads the map once, so it counts half its width.
            rowCells = std::max(std::abs(dzdx), std::abs(dzdy)) / light.cellSize();
            columnCells = spanPixels * 0.5f / (view.focal * light.cellSize());
        }

        for (int y = minY; y <= maxY; ++y) {
            float e0 = edge(b, c, minX + 0.5f, y + 0.5f);
            float e1 = edge(c, a, minX + 0.5f, y + 0.5f);
            float e2 = edge(a, b, minX + 0.5f, y + 0.5f);
            // Intersect the row with all three half planes. Skinny geometry no
            // longer pays for the empty corners of its bounding rectangle.
            int left = 0, right = maxX - minX;
            const float edges[3]{e0, e1, e2};
            for (int i = 0; i < 3; ++i) {
                // A nearly level edge crosses the row far off screen. Bound the crossing before it becomes an integer.
                if (dx[i] > 0) left = std::max(left, int(std::ceil(std::clamp(-edges[i] / dx[i], -1.0f, rowLimit))));
                else if (dx[i] < 0) right = std::min(right, int(std::floor(std::clamp(-edges[i] / dx[i], -1.0f, rowLimit))));
                else if (edges[i] < 0) right = -1;
            }
            if (left > right) continue;
            e0 += dx[0] * left;
            e1 += dx[1] * left;
            e2 += dx[2] * left;
            float z = (a.depth * e0 + b.depth * e1 + c.depth * e2) * inverseArea;
            float u = usesUv ? (a.u * e0 + b.u * e1 + c.u * e2) * inverseArea : 0.f;
            float v = usesUv ? (a.v * e0 + b.v * e1 + c.v * e2) * inverseArea : 0.f;

            int lit = t.light[0] << 16, lightStep = cornerStep;
            bool mapped = false;
            // The camera never rolls, so a row of level ground runs straight across
            // the light map. Find both ends of the row and step between them.
            float cellX = 0, cellY = 0, cellStepX = 0, cellStepY = 0, mapMix = 0;
            int mapLevel = 0;
            const auto mapLight = [&](int pixels) {
                const int x = int(cellX + cellStepX * pixels), y = int(cellY + cellStepY * pixels);
                const int fine = light.sample(x, y, mapLevel);
                if (mapMix <= 0) return fine << 8;
                return (fine << 8) + int((light.sample(x, y, mapLevel + 1) - fine) * mapMix);
            };
            if (patch) {
                const int count = right - left + 1;
                const float zEnd = z + dzdx * count;
                if (z > 0.002f && zEnd > 0.002f) {
                    const float inv = 1 / z, invEnd = 1 / zEnd;
                    const float uEnd = (u + dudx * count) * invEnd, vEnd = (v + dvdx * count) * invEnd;
                    cellX = patch->xu * u * inv + patch->xv * v * inv + patch->x0;
                    cellY = patch->zu * u * inv + patch->zv * v * inv + patch->z0;
                    cellStepX = (patch->xu * uEnd + patch->xv * vEnd + patch->x0 - cellX) / count;
                    cellStepY = (patch->zu * uEnd + patch->zv * vEnd + patch->z0 - cellY) / count;
                    // Pick the map level whose cells match the ground under one pixel.
                    float cells = std::max(rowCells * inv * inv, columnCells * inv);
                    while (cells > 2 && mapLevel + 2 < SceneLight::levels) {
                        cells *= 0.5f;
                        ++mapLevel;
                    }
                    mapMix = std::clamp(cells - 1, 0.0f, 1.0f) * 256;
                    lit = mapLight(0);
                    mapped = true;
                } else {
                    lit = 255 << 16;
                }
            } else if (smooth) {
                lit = int(std::clamp((t.light[0] * e0 + t.light[1] * e1 + t.light[2] * e2) * inverseArea, 0.0f, 255.0f) * 65536);
            }
            const uint8_t* dither = shadeDither[y & 3];
            int at = y * view.width + minX + left;
            for (int x = left; x <= right;) {
                const int end = span ? std::min(right, x + span - 1) : right;
                const float midpoint = (end - x) * 0.5f;
                const auto sample = perspectiveMips ? mipAt(z + dzdx * midpoint, u + dudx * midpoint, v + dvdx * midpoint)
                                                    : triangleTexture;
                const auto* texture = sample.fine;
                const bool halfSize = sample.coarse && texture->width == sample.coarse->width * 2 &&
                    texture->height == sample.coarse->height * 2;
                if (mapped) lightStep = (mapLight(x - left + spanPixels) - lit) / spanPixels;
                for (; x <= end; ++x, ++at, z += dzdx, u += dudx, v += dvdx, lit += lightStep) {
                    uint16_t newDepth = 0;
                    if constexpr (backdrop) {
                        if (depth[at]) continue;
                        const float invZ = 1.0f / z;
                        color[at] = sunsetPixels ? sampleSunsetSky(sunsetPixels, u * invZ, v * invZ, minX + x, y)
                                                 : sampleSky(skyPixels, u * invZ, v * invZ, minX + x, y);
                        if constexpr (countPixels) ++stats.depthPasses;
                        continue;
                    } else {
                        newDepth = enhanced ? encodedDepth(z) : uint16_t(std::clamp(z * depthScale, 0.0f, 65535.0f));
                        if (newDepth <= depth[at]) continue;
                    }
                    if constexpr (shadow) {
                        color[at] = shaded(color[at], std::min(unsigned(lit) >> 16, 255u), dither[(minX + x) & 3]);
                        continue;
                    }
                    uint16_t pixel = flat;
                    if constexpr (textured) {
                        const float invZ = 1.0f / z;
                        const float texU = u * invZ, texV = v * invZ;
                        const int tx = texel(texU * texture->width, texture->width);
                        const int ty = texel(texV * texture->height, texture->height);
                        pixel = texture->pixels[ty * texture->width + tx];
                        if (sample.coarse) {
                            const auto& coarse = *sample.coarse;
                            const int cx = halfSize ? tx / 2 : texel(texU * coarse.width, coarse.width);
                            const int cy = halfSize ? ty / 2 : texel(texV * coarse.height, coarse.height);
                            pixel = blend(pixel, coarse.pixels[cy * coarse.width + cx], sample.mix);
                        }
                        if (!varying) pixel = multiply(pixel, t.light[0]);
                    } else if (varying) {
                        pixel = material->color;
                    }
                    if constexpr (detail) {
                        if (varying) pixel = shaded(pixel, std::min(unsigned(lit) >> 16, 255u), dither[(minX + x) & 3]);
                        if (hazy) {
                            if (unsigned level = haze[newDepth >> 6]) {
                                // Emissive surfaces (ray gun cores, eyes, lanterns, flames, mothership glow)
                                // pierce through the sunset mist instead of washing out completely.
                                if (material && material->emissive) level >>= 1;
                                if (level) pixel = veil(pixel, level);
                            }
                        }
                    }
                    color[at] = opaque ? pixel : blend(color[at], pixel, material->alpha);
                    if (opaque) depth[at] = newDepth;
                    if constexpr (countPixels) ++stats.depthPasses;
                }
            }
        }
    }

    template<bool countPixels>
    void drawSurfaces(const std::vector<Primitive>& batch, bool opaque) {
        for (const auto& primitive : batch) {
            const bool detail = enhanced && detailed(primitive);
            if (assets.materials[primitive.material].texture >= 0) {
                if (detail) raster<Fill::Textured, true, countPixels>(primitive, opaque);
                else raster<Fill::Textured, false, countPixels>(primitive, opaque);
            } else {
                if (detail) raster<Fill::Flat, true, countPixels>(primitive, opaque);
                else raster<Fill::Flat, false, countPixels>(primitive, opaque);
            }
        }
    }

    void draw(std::vector<Primitive>& batch, bool opaque) {
        const auto start = timestamp();
        if (!opaque) {
            std::sort(batch.begin(), batch.end(), [](const Primitive& a, const Primitive& b) {
                return a.p[0].depth + a.p[1].depth + a.p[2].depth < b.p[0].depth + b.p[1].depth + b.p[2].depth;
            });
        }
        if (countRenderWork && measure) drawSurfaces<true>(batch, opaque);
        else drawSurfaces<false>(batch, opaque);
        batch.clear();
        recordRasterTime(start);
    }

    void drawWorld(const Snapshot& state) {
        resetProjections();
        if (assets.world.chunks.empty()) {
            submitWorld(0, assets.world.triangles.size(), state);
            return;
        }
        visible.clear();
        for (uint32_t i = 0; i < assets.world.chunks.size(); ++i) {
            const auto& chunk = assets.world.chunks[i];
            if (!view.sees(chunk.bounds)) continue;
            const Vec3 center = (chunk.bounds.min + chunk.bounds.max) * 0.5f - view.position;
            visible.emplace_back(dot(center, center), i);
        }
        std::sort(visible.begin(), visible.end());
        if (countRenderWork && measure) stats.visibleChunks = uint32_t(visible.size());
        for (const auto& item : visible) {
            const auto& chunk = assets.world.chunks[item.second];
            submitWorld(chunk.first, chunk.count, state);
        }
    }

    void drawActor(const Actor& actor, const Snapshot& state, bool player, bool alternate = false, float heightScale = 1.f) {
        if (!actor.visible || assets.rivalFrames.empty()) return;
        const auto& frames = alternate && !assets.hanzFrames.empty() ? assets.hanzFrames : assets.rivalFrames;
        const bool separatePlayer = player && !assets.playerBody.vertices.empty();
        const auto& mesh = separatePlayer ? assets.playerBody : frames[std::min<size_t>(actor.frame, frames.size() - 1)];
        const auto& next = separatePlayer ? mesh : frames[std::min<size_t>(actor.nextFrame, frames.size() - 1)];
        const float fall = std::clamp(actor.fall, 0.0f, 1.0f);
        const float fallYaw = actor.fallVariant == 2 ? fall * 1.35f : 0;
        const float fallPitch = actor.fallVariant == 1 ? fall * 1.46f : -fall * 1.46f;
        auto transform = orientation(actor.position + Vec3{0, fall * 0.24f, 0}, actor.yaw + fallYaw, fallPitch, fall * 0.12f);
        if (heightScale != 1.f) transform.y *= heightScale;
        const Bounds bounds{actor.position + Vec3{-2, -0.4f, -2}, actor.position + Vec3{2, 2.2f, 2}};
        if (!view.sees(bounds)) return;
        const Mesh* topology = mesh.triangles.empty() ? &frames.front() : &mesh;
        meshLight = standing(actor.position);
        const auto* uniform = player && !assets.playerUniform.empty() ? assets.playerUniform.data() : nullptr;
        const bool legacyMetal = !player && state.difficulty >= 9 && assets.playerUniform.empty() && assets.hanzFrames.empty();
        submit(mesh, state, transform, MeshSpace::World,
               mesh.vertexCount() == next.vertexCount() ? &next : nullptr, actor.frameBlend, legacyMetal, topology, uniform);
    }

    void drawMissionActor(const MissionActor& ma, const Snapshot& state) {
        if (!ma.active || !ma.actor.visible) return;
        const auto& actor = ma.actor;
        const Vec3 extent = ma.role == MissionRole::AlienDrone ? Vec3{1.f, .6f, 1.f}
                          : ma.role == MissionRole::AlienTrooper ? Vec3{1.9f, 2.9f, 1.9f} : Vec3{2.f, 2.2f, 2.f};
        if (!view.sees({actor.position - Vec3{extent.x, .6f, extent.z}, actor.position + extent})) return;
        const float fall = std::clamp(actor.fall, 0.f, 1.f);
        meshLight = standing(actor.position);
        if (ma.role == MissionRole::Civilian && !assets.civilianPoses.empty()) {
            const auto& poses = assets.civilianPoses;
            const auto transform = orientation(actor.position + Vec3{0, fall * 0.2f, 0}, actor.yaw, -fall * 1.46f, fall * 0.12f);
            const auto& outfit = assets.civilianUniforms[ma.variant % 3];
            const auto& pose = poses[std::min<size_t>(actor.frame, poses.size() - 1)];
            const auto& next = poses[std::min<size_t>(actor.nextFrame, poses.size() - 1)];
            submit(pose, state, transform, MeshSpace::World, &next, actor.frameBlend, false, &assets.civilianMesh,
                   outfit.empty() ? nullptr : outfit.data());
            return;
        }
        if (ma.role == MissionRole::Commander || ma.role == MissionRole::Civilian) {
            if (assets.rivalFrames.empty()) return;
            const auto& frames = assets.rivalFrames;
            const auto& mesh = frames[std::min<size_t>(actor.frame, frames.size() - 1)];
            const auto& next = frames[std::min<size_t>(actor.nextFrame, frames.size() - 1)];
            const float fallPitch = -fall * 1.46f;
            auto transform = orientation(actor.position + Vec3{0, fall * 0.24f, 0}, actor.yaw, fallPitch, fall * 0.12f);
            if (ma.role == MissionRole::Civilian && ma.civilian == CivilianOutcome::Pinned) transform.y *= 0.78f;
            const Mesh* topology = mesh.triangles.empty() ? &frames.front() : &mesh;
            const uint16_t* uniform = nullptr;
            if (ma.role == MissionRole::Commander && !assets.commanderUniform.empty()) {
                uniform = assets.commanderUniform.data();
            } else if (ma.role == MissionRole::Civilian && !assets.civilianUniforms[ma.variant % 3].empty()) {
                uniform = assets.civilianUniforms[ma.variant % 3].data();
            }
            submit(mesh, state, transform, MeshSpace::World,
                   mesh.vertexCount() == next.vertexCount() ? &next : nullptr, actor.frameBlend, false, topology, uniform);
            return;
        }
        if (ma.role == MissionRole::AlienTrooper) {
            if (assets.alienTrooper.triangles.empty()) return;
            const bool armored = ma.variant > 0 && !assets.alienEnforcerPoses.empty();
            const auto& poses = armored ? assets.alienEnforcerPoses : assets.alienTrooperPoses;
            const auto& body = armored ? assets.alienEnforcer : assets.alienTrooper;
            const bool posed = !poses.empty();
            // Pose frames carry the gait; the rigid bob and sway only stand in without them.
            const bool slide = !posed && !ma.dead && actor.speed > 0.3f;
            const float bob = slide ? std::abs(std::sin(actor.phase * 2 * pi)) * 0.08f : 0.f;
            const float sway = slide ? std::sin(actor.phase * 2 * pi) * 0.07f : 0.f;
            const float staggerRoll = !ma.dead ? std::sin(float(state.realTime) * 28.f) * 0.16f * ma.stagger : 0.f;
            const float staggerPitch = !ma.dead ? -0.18f * ma.stagger : 0.f;
            auto transform = orientation(actor.position + Vec3{0, fall * 0.22f + bob, 0}, actor.yaw,
                                         -fall * 1.45f + staggerPitch, sway + staggerRoll);
            const float s = alienScale(ma.variant);
            if (ma.variant > 0) {
                transform.x *= s;
                transform.y *= s;
                transform.z *= s;
            }
            const uint16_t* uniform = (ma.variant > 0 && !assets.enforcerUniform.empty())
                ? assets.enforcerUniform.data() : nullptr;
            if (posed) {
                const auto& pose = poses[std::min<size_t>(actor.frame, poses.size() - 1)];
                const auto& next = poses[std::min<size_t>(actor.nextFrame, poses.size() - 1)];
                submit(pose, state, transform, MeshSpace::World, &next, actor.frameBlend, false, &body, uniform);
            } else {
                submit(assets.alienTrooper, state, transform, MeshSpace::World, nullptr, 0, false, nullptr, uniform);
            }
            // Render the alien's animated ray gun in its right claw, with spinning accelerator rings!
            const Mesh& gunMesh = armored ? assets.alienHeavyRayGun : assets.alienRayGun;
            const Mesh& ringMesh = armored ? assets.alienHeavyRayRings : assets.alienRayRings;
            if (!gunMesh.triangles.empty()) {
                const auto rig = alienGunFrame(transform(alienGunSocket(assets, actor)),
                                               ma.gunYaw + fall * .25f, ma.gunPitch + fall * 1.2f, ma.variant);
                const Transform gunXform{rig.grip, rig.right, rig.up, rig.axis};
                submit(gunMesh, state, gunXform, MeshSpace::World, nullptr, 0, true, nullptr, uniform);

                if (!ringMesh.triangles.empty()) {
                    const float spinSpeed = 2.4f + 13.f * ma.gunCharge + 18.f * ma.gunRecoil + (ma.overcharging ? 9.f : 0.f);
                    const float spin = float(state.realTime) * spinSpeed + float(ma.actor.phase) * 6.f;
                    const float cs = std::cos(spin), sn = std::sin(spin);
                    const float pulseZ = (ma.gunCharge > 0.05f ? std::sin(float(state.realTime) * 18.f) * 0.025f * ma.gunCharge : 0.f)
                                       - 0.04f * ma.gunRecoil;
                    Transform ringXform = gunXform;
                    ringXform.origin = gunXform({0, rayBoreHeight, pulseZ});
                    ringXform.x = gunXform.x * cs + gunXform.y * sn;
                    ringXform.y = gunXform.y * cs - gunXform.x * sn;
                    submit(ringMesh, state, ringXform, MeshSpace::World, nullptr, 0, true, nullptr, uniform);
                }

                // Composite the forming cloud once the world and weapon have written depth.
                if (!ma.dead && (ma.gunCharge > 0.04f || ma.gunRecoil > 0.08f) && muzzleGlowCount < muzzleGlows.size()) {
                    const Vec3 muzzlePos = rig.muzzle(ma.variant);
                    const bool hot = ma.overcharging;
                    const float power = std::max(ma.gunCharge * (ma.overcharging ? 1.15f : 0.72f), ma.gunRecoil * 1.0f);
                    const float pulse = 0.88f + 0.16f * std::sin(float(state.realTime) * (ma.overcharging ? 34.f : 20.f));
                    muzzleGlows[muzzleGlowCount++] = {
                        muzzlePos,
                        (0.06f + 0.10f * std::min(power, 1.15f)) * pulse * s,
                        0, 0,
                        std::min(1.f, power),
                        hot, true
                    };
                }
            }
            if (!ma.dead && ma.shield > 0.01f && shieldGlowCount < shieldGlows.size()) {
                const uint16_t shieldColor = ma.linked ? rgb(72, 238, 255) : rgb(255, 86, 128);
                shieldGlows[shieldGlowCount++] = {
                    actor.position + Vec3{0, 1.15f * s, 0},
                    0.78f * s, 1.05f * s, shieldColor,
                    std::clamp(0.45f + 0.55f * ma.shield, 0.35f, 1.f)
                };
            }
            return;
        }
        if (ma.role == MissionRole::AlienDrone) {
            if (assets.alienDrone.triangles.empty()) return;
            const float pitch = ma.dead ? .55f : ma.alerted ? ma.gunPitch : actor.speed > .5f ? .14f : 0.f;
            auto transform = orientation(actor.position, ma.alerted && !ma.dead ? ma.gunYaw : actor.yaw,
                                         pitch, ma.dead ? .45f : 0.f);
            submit(assets.alienDrone, state, transform, MeshSpace::World);
        }
    }

    void drawUplink(const Snapshot& state) {
        if (!state.mission->uplinkVisible || assets.alienUplink.triangles.empty()) return;
        const auto bounds = assets.alienUplink.chunks.front().bounds;
        if (!view.sees({bounds.min + uplinkPosition, bounds.max + uplinkPosition})) return;
        meshLight = standing(uplinkPosition);
        submit(assets.alienUplink, state, orientation(uplinkPosition, 0, 0, 0), MeshSpace::World);
        if (state.mission->uplinkHealth <= 0.f) return;
        const auto center = uplinkPosition + uplinkEmitter;
        const float spin = float(state.realTime) * 2.5f;
        submit(assets.alienUplinkCore, state, orientation(center, spin, 0, 0), MeshSpace::World);
        const float pulse = .62f + .22f * std::sin(spin * 2.f);
        muzzleGlows[muzzleGlowCount++] = {center, .50f, rgb(255, 206, 176), rgb(255, 58, 108), pulse, true};
    }

    // A figure in shade dims with the ground under it, yet stays easy to pick out.
    unsigned standing(Vec3 position) const { return 128 + light.at(position.x, position.z) / 2; }

    // Darken the ground under a figure. The disc fades from a dark core to nothing at its rim.
    void contactShadow(Vec3 feet, float radius) {
        if (!view.sees({feet - Vec3{radius, 0, radius}, feet + Vec3{radius, 0.1f, radius}})) return;
        constexpr int segments = 10;
        constexpr float core = 0.45f, dark = 150, clear = 255;
        const Vec3 center = feet + Vec3{0, 0.07f, 0};
        const auto corner = [&](int i, float reach, float level) {
            const float angle = i * (2 * pi / segments);
            return LitVertex{{view.transform(center + Vec3{std::cos(angle), 0, std::sin(angle)} * (radius * reach)), 0, 0}, level};
        };
        const auto fill = [&](const LitVertex& a, const LitVertex& b, const LitVertex& c) {
            const LitVertex corners[]{a, b, c};
            clip(corners, [&](const LitVertex& x, const LitVertex& y, const LitVertex& z) {
                Primitive primitive{};
                if (project(primitive, x, y, z)) raster<Fill::Shadow, true>(primitive, false);
            });
        };
        const LitVertex hub{{view.transform(center), 0, 0}, dark};
        for (int i = 0; i < segments; ++i) {
            const auto inner = corner(i, core, dark), innerNext = corner(i + 1, core, dark);
            const auto outer = corner(i, 1, clear), outerNext = corner(i + 1, 1, clear);
            fill(hub, inner, innerNext);
            fill(inner, outer, outerNext);
            fill(inner, outerNext, innerNext);
        }
    }

    MotionBlend weaponMotion(const Snapshot& state) const {
        const auto weapon = state.weapon;
        auto which = static_cast<WeaponTrack>(weapon);
        float seconds = state.weaponAnimationTime;
        if (state.weaponState == WeaponState::Bolt) {
            which = WeaponTrack::SniperBolt;
        } else if (weapon == Weapon::Sniper) {
            constexpr float firstFeedEnd = .79f, repeatFeed = .38f, closeAt = 1.17f, closeDuration = .45f;
            const float closeStart = state.reloadDuration - closeDuration;
            if (seconds >= closeStart) seconds = closeAt + seconds - closeStart;
            else if (seconds >= closeAt) seconds = firstFeedEnd + std::fmod(seconds - firstFeedEnd, repeatFeed);
        } else if (state.emptyReload) {
            which = weapon == Weapon::Mp44 ? WeaponTrack::Mp44Empty : WeaponTrack::PistolEmpty;
        }
        const auto& track = assets.weaponTracks[size_t(which)];
        const auto upper = std::upper_bound(track.begin(), track.end(), seconds, [](float time, const WeaponPose& pose) {
            return time < pose.seconds;
        });
        const auto& a = upper == track.begin() ? track.front() : *(upper - 1);
        const auto& b = upper == track.end() ? a : *upper;
        const float fraction = b.seconds > a.seconds ? std::clamp((seconds - a.seconds) / (b.seconds - a.seconds), 0.f, 1.f) : 0.f;
        return {&a, &b, fraction};
    }

    void drawWeapon(const Snapshot& state) {
        const auto weapon = size_t(state.weapon);
        const auto& mesh = assets.weapon[weapon];
        using namespace game_data;
        const AimFrame* timelines[]{mp44Aim, m40a3Aim, usp45Aim};
        const size_t frameCounts[]{std::size(mp44Aim), std::size(m40a3Aim), std::size(usp45Aim)};
        const auto* frames = timelines[weapon];
        size_t lastVisible = frameCounts[weapon] - 1;
        while (lastVisible && frames[lastVisible].scope >= 0.6) --lastVisible;
        const Vec3 hipPosition{float(frames[0].position[0]), float(frames[0].position[1]), float(frames[0].position[2])};
        const Vec3 hipRotation{float(frames[0].rotation[0]), float(frames[0].rotation[1]), float(frames[0].rotation[2])};
        const float travel = float(frames[lastVisible].position[0]) - hipPosition.x;
        const float fraction = std::clamp(std::abs(travel) > 0.0001f ? (state.weaponPosition.x - hipPosition.x) / travel : state.ads,
                                          0.0f, 1.0f);
        const auto& aim = assets.weaponAds[weapon];
        const auto& reload = assets.weaponReload[weapon];
        const auto& bolt = assets.sniperBolt;
        const Mesh* pose = &aim;
        float amount = fraction;
        const bool tracked = !assets.weaponTracks[0].empty() &&
            (state.weaponState == WeaponState::Reload || (state.weapon == Weapon::Sniper && state.weaponState == WeaponState::Bolt));
        if (!tracked && state.weaponState == WeaponState::Reload) {
            pose = &reload;
            amount = std::clamp(1.0f - std::abs(state.reload * 2 - 1), 0.0f, 1.0f);
        } else if (!tracked && state.weapon == Weapon::Sniper && state.weaponState == WeaponState::Bolt) {
            pose = &bolt;
            amount = std::clamp(state.bolt, 0.0f, 1.0f);
        }
        Vec3 shift{};
        float pitch = 0, yaw = 0, roll = 0;
        if (!tracked && pose->vertexCount() != mesh.vertexCount()) {
            pose = nullptr;
            shift = state.weaponPosition - hipPosition;
            const auto rotation = (state.weaponRotation - hipRotation) * (pi / 180);
            pitch = rotation.x;
            yaw = rotation.y;
            roll = rotation.z;
        }
        const float run = state.sprint ? 1.0f : 0.0f;
        const float drop = state.lower;
        const bool sniper = state.weapon == Weapon::Sniper;
        const float recoilPitch = sniper ? 0.4f : 0.12f;
        shift += Vec3{-0.055f * run, -0.11f * run - 0.31f * drop, -state.recoil};
        pitch -= 0.34f * run + 0.55f * drop + recoilPitch * state.recoil;
        yaw += 0.21f * run;
        roll += 0.14f * run;
        const Vec3 pivot = state.weaponPosition;
        auto transform = orientation({}, yaw, pitch, roll);
        transform.origin = shift + pivot - transform(pivot);
        if (tracked) {
            const auto motion = weaponMotion(state);
            // Keep the remaining aim motion as the scope lowers into the hip-based clip.
            const Mesh* aimOffset = sniper && fraction > 0 && aim.vertexCount() == mesh.vertexCount() ? &aim : nullptr;
            submit(mesh, state, transform, MeshSpace::View, aimOffset, fraction, false, nullptr, nullptr, &motion);
        } else {
            submit(mesh, state, transform, MeshSpace::View, pose, amount);
        }
    }

    void sky(const Camera& camera) {
        const float horizon = view.height * 0.5f - std::tan(camera.pitch) * view.focal;
        if (invasionDusk) {
            for (int y = 0; y < view.height; ++y) {
                const float elevation = std::clamp((horizon - float(y)) / float(view.height), -1.f, 1.f);
                const Vec3 tone = duskGradient(elevation);
                std::fill_n(color + y * view.width, view.width, rgbClamped(tone.x, tone.y, tone.z));
            }
            return;
        }
        for (int y = 0; y < view.height; ++y) {
            const float mix = std::clamp((float(y) - horizon) / view.height + 0.40f, 0.0f, 1.0f);
            const auto pixel = rgb(int(105 + mix * 75), int(119 + mix * 59), int(132 + mix * 52));
            std::fill_n(color + y * view.width, view.width, pixel);
        }
    }

    void drawSkyCube() {
        if (!enhanced) return;
        const bool useSunset = invasionDusk && !sunsetSky[0].empty();
        if (!useSunset && assets.sky[0].empty()) return;
        const auto start = timestamp();
        const Vec3 eye = view.position;
        view.position = {};
        for (size_t face = 0; face < assets.sky.size(); ++face) {
            const auto& [normal, right, up] = skyAxes[face];
            const Vertex corners[] = {
                {view.transform(normal - right - up), 0, 0}, {view.transform(normal + right - up), 1, 0},
                {view.transform(normal + right + up), 1, 1}, {view.transform(normal - right + up), 0, 1},
            };
            const Vertex first[] = {corners[0], corners[1], corners[2]};
            const Vertex second[] = {corners[0], corners[2], corners[3]};
            const auto submit = [&](Vertex a, Vertex b, Vertex c) {
                Primitive primitive{};
                if (!project(primitive, a, b, c)) return;
                const auto* luma = useSunset ? nullptr : assets.sky[face].data();
                const auto* sunset = useSunset ? sunsetSky[face].data() : nullptr;
                if (countRenderWork && measure) raster<Fill::Sky, false, true>(primitive, false, luma, sunset);
                else raster<Fill::Sky>(primitive, false, luma, sunset);
            };
            clip(first, submit);
            clip(second, submit);
        }
        view.position = eye;
        recordRasterTime(start);
    }

    void drawGlowDisc(Vec3 worldPos, float radius, uint16_t coreColor, uint16_t haloColor, float intensity, bool star) {
        const Vertex centre{view.transform(worldPos), 0, 0};
        if (centre.position.z < view.near + 0.05f) return;
        const auto p = view.project(centre);
        const float pixels = std::min(float(view.width) * 0.09f, radius * view.focal / centre.position.z);
        if (pixels < 0.8f) return;
        const int r = int(pixels + 1.f), cx = int(p.x), cy = int(p.y);
        if (cx + r < 0 || cx - r >= view.width || cy + r < 0 || cy - r >= view.height) return;
        const uint16_t z = enhanced ? encodedDepth(p.depth) : uint16_t(std::clamp(p.depth * view.near * 65534.f, 0.f, 65535.f));
        const float inv = 1.f / std::max(1.f, pixels * pixels);
        const int x0 = std::max(0, cx - r), x1 = std::min(view.width - 1, cx + r);
        const int y0 = std::max(0, cy - r), y1 = std::min(view.height - 1, cy + r);
        for (int y = y0; y <= y1; ++y) {
            const float dy = float(y - cy);
            for (int x = x0; x <= x1; ++x) {
                const float dx = float(x - cx);
                const float d2 = (dx * dx + dy * dy) * inv;
                if (d2 >= 1.f) continue;
                const int at = y * view.width + x;
                if (z + 2 < depth[at]) continue;
                const float soft = 1.f - d2;
                const float fall = soft * soft * soft;
                const unsigned alpha = unsigned(std::clamp(fall * intensity * 245.f, 0.f, 245.f));
                // Smooth radial color transition from incandescent white core to saturated plasma halo.
                const unsigned coreMix = unsigned(std::clamp((1.f - d2 * 2.6f) * 255.f, 0.f, 255.f));
                const uint16_t tint = coreMix > 4 ? blend(haloColor, coreColor, coreMix) : haloColor;
                color[at] = blend(color[at], tint, alpha);
            }
        }
        if (star && pixels >= 2.2f) {
            // Crisp four-point plasma flare rays across the glowing muzzle.
            const int arm = std::min(int(pixels * 1.85f), view.width / 10);
            for (int d = -arm; d <= arm; ++d) {
                const float fall = 1.f - std::abs(float(d)) / float(arm + 1);
                const unsigned a = unsigned(std::clamp(fall * fall * intensity * 200.f, 0.f, 200.f));
                if (cx + d >= 0 && cx + d < view.width && cy >= 0 && cy < view.height) {
                    const int at = cy * view.width + cx + d;
                    if (z + 2 >= depth[at]) color[at] = blend(color[at], coreColor, a);
                }
                if (cy + d >= 0 && cy + d < view.height && cx >= 0 && cx < view.width) {
                    const int at = (cy + d) * view.width + cx;
                    if (z + 2 >= depth[at]) color[at] = blend(color[at], haloColor, a * 3 / 4);
                }
            }
        }
    }

    void drawPlasmaCloud(Vec3 position, float radius, float intensity, unsigned phase, bool charged) {
        const Vec3 center = view.transform(position);
        if (center.z < view.near + .025f) return;
        const auto projected = view.project({center, 0, 0});
        const float pixels = std::clamp(radius * view.focal / center.z, 1.3f, std::max(1.3f, view.width * .11f));
        const int cx = int(projected.x), cy = int(projected.y);
        const int r = int(pixels + 1.f);
        const int x0 = std::max(0, cx - r), x1 = std::min(view.width - 1, cx + r);
        const int y0 = std::max(0, cy - r), y1 = std::min(view.height - 1, cy + r);
        if (x0 > x1 || y0 > y1) return;
        const float invZ = 1.f / std::max(view.near, center.z - radius * .22f);
        const uint16_t z = enhanced ? encodedDepth(invZ) : uint16_t(std::min(65535.f, invZ * view.near * 65534.f));
        constexpr uint16_t fog = rgb(50, 113, 227), skyBlue = rgb(101, 199, 255), spark = rgb(211, 243, 255);
        const float unit = 1.f / pixels;
        for (int y = y0; y <= y1; ++y) {
            const float dy = (y - cy) * unit;
            const unsigned sy = unsigned(int(dy * 13.f) + 16);
            const int row = y * view.width;
            for (int x = x0; x <= x1; ++x) {
                const int at = row + x;
                if (z + 3 < depth[at]) continue;
                const float dx = (x - cx) * unit;
                const float d = dx * dx + dy * dy;
                if (d > .99f) continue;
                const unsigned sx = unsigned(int(dx * 13.f) + 16);
                const unsigned a = skyDither[((sy * 2 + phase / 4) & 63u) * 64 + ((sx * 2 + phase / 3) & 63u)];
                const unsigned b = skyDither[((sy + phase / 6) & 63u) * 64 + ((sx - phase / 8) & 63u)];
                // Offset lobes and granular turbulence keep the outline and interior in motion.
                const float bulge = dx * (dy > 0 ? -.23f : .21f) + dy * .11f;
                const float density = std::clamp(.89f - d + bulge + .085f * float(int(a) - 1), 0.f, 1.f);
                const unsigned alpha = unsigned(221.f * intensity * density);
                if (alpha < 5) continue;
                const auto tint = blend(fog, skyBlue, std::min(255u, unsigned(density * 126.f) + b * 29u));
                color[at] = blend(color[at], tint, alpha);
            }
        }
        if (pixels < 2.25f || intensity < .22f) {
            if (cx >= x0 && cx <= x1 && cy >= y0 && cy <= y1 && z + 3 >= depth[cy * view.width + cx])
                color[cy * view.width + cx] = blend(color[cy * view.width + cx], spark, unsigned(173.f * intensity));
            return;
        }

        // Short forked arcs live inside the mist and share its depth test.
        uint32_t seed = (phase + 1) * 0x045d9f3bu;
        const auto random = [&] {
            seed = seed * 1664525u + 1013904223u;
            return float(seed >> 24) / 255.f - .5f;
        };
        for (int fork = 0; fork < (charged ? 3 : 2); ++fork) {
            const float side = fork % 2 ? -1.f : 1.f;
            float x = float(cx) - side * pixels * .52f, y = float(cy) + random() * pixels;
            for (int segment = 0; segment < 4; ++segment) {
                const float nextX = x + side * pixels * .27f;
                const float nextY = cy + random() * pixels * 1.28f;
                const int count = std::max(1, int(std::max(std::abs(nextX - x), std::abs(nextY - y))));
                for (int i = 0; i <= count; ++i) {
                    const float t = float(i) / count;
                    const int px = int(x + (nextX - x) * t), py = int(y + (nextY - y) * t);
                    if (px < x0 || px > x1 || py < y0 || py > y1) continue;
                    const int at = py * view.width + px;
                    if (z + 3 >= depth[at]) color[at] = blend(color[at], spark, unsigned(182.f * intensity));
                }
                x = nextX;
                y = nextY;
            }
        }
    }

    void drawMissionTracer(const MissionTracer& shot, float realTime) {
        if (shot.time <= 0) return;
        if (shot.style == TracerStyle::Ballistic) {
            drawTracer({shot.start, shot.end, shot.time}, shot.color);
            return;
        }
        if (shot.style == TracerStyle::PlasmaImpact) {
            const float life = std::min(1.f, shot.time);
            drawPlasmaCloud(shot.start, .16f + .26f * (1.f - life), life,
                            unsigned(realTime * 44.f), shot.time > 1.f);
            return;
        }
        Projected p, q;
        if (!projectSegment(view, view.transform(shot.start), view.transform(shot.end), p, q)) return;
        const float dx = q.x - p.x, dy = q.y - p.y;
        const int count = int(std::clamp(std::max(std::abs(dx), std::abs(dy)), 1.0f, 1.8f * view.width));
        const float resScale = float(view.width) / 400.f;
        const bool link = shot.style == TracerStyle::ShieldLink;
        const auto visible = count > 48 ? visibleSamples(view, p, q, count, 9.f + (link ? 2.2f * resScale : 0.f))
                                        : SampleRange{0, count};
        if (visible.first > visible.last) return;
        const float segLen = std::sqrt(dx * dx + dy * dy);
        if (segLen < 0.5f) return;
        const float nx = -dy / segLen, ny = dx / segLen; // Screen-space perpendicular normal
        const bool drop = shot.style == TracerStyle::DropBeam;
        const bool scan = shot.style == TracerStyle::Scanner;
        const uint16_t coreColor = drop ? blend(shot.color, rgb(235, 249, 255), 164)
                                        : link ? rgb(196, 255, 255) : rgb(236, 255, 255);
        const float baseWidth = drop ? 5.6f : scan ? 1.1f : 1.6f;
        const float life = std::clamp(shot.time, 0.f, 1.f);
        const float phaseShift = (1.f - life) * 12.f + realTime * (link ? 9.f : 0.f);

        for (int i = visible.first; i <= visible.last; ++i) {
            const float t = float(i) / float(count);
            const float inverseZ = p.depth + (q.depth - p.depth) * t;
            const uint16_t z = enhanced ? encodedDepth(inverseZ) : uint16_t(std::clamp(inverseZ * view.near * 65534.f, 0.f, 65535.f));
            const float wave = link ? std::sin(t * 14.f - phaseShift) * 2.2f * resScale : 0.f;
            const float pulse = !scan ? (.73f + .27f * std::sin(t * 22.f - phaseShift)) : 1.f;
            const float cx = p.x + dx * t + nx * wave;
            const float cy = p.y + dy * t + ny * wave;
            const float persp = std::clamp(inverseZ * 10.f, 0.55f, 2.2f);
            const float taper = drop ? .38f + .62f * t : 1.f;
            const int halfW = std::clamp(int(baseWidth * resScale * persp * taper * (0.75f + 0.35f * pulse)), 1, 7);
            const unsigned outerA = unsigned(std::clamp(int((scan ? 90.f : 175.f) * life * pulse), drop ? 0 : 28, 224));
            const unsigned coreA = unsigned(std::clamp(int((scan ? 130.f : 242.f) * life), drop ? 0 : 56, 248));

            for (int off = -halfW; off <= halfW; ++off) {
                const int px = int(cx + nx * float(off));
                const int py = int(cy + ny * float(off));
                if (px < 0 || px >= view.width || py < 0 || py >= view.height) continue;
                const int at = py * view.width + px;
                if (z + 2 < depth[at]) continue;
                const int ad = std::abs(off);
                if (ad == 0) {
                    color[at] = blend(color[at], coreColor, coreA);
                } else {
                    const unsigned remain = unsigned(halfW + 1 - ad);
                    const unsigned span = unsigned(halfW + 1);
                    const unsigned alpha = drop ? outerA * remain * remain / (span * span) : outerA * remain / span;
                    color[at] = blend(color[at], shot.color, alpha);
                }
            }
        }

    }

    void drawArrival(const MissionActor& actor, float time) {
        const float life = actor.arrival, age = 1.f - life;
        const Vec3 feet = actor.actor.position + Vec3{0, .10f, 0};
        const auto p = view.transform(feet);
        if (p.z < view.near || p.z > 70.f || std::abs(p.x) > view.xSlope * p.z + 2.5f) return;
        const float strength = life * (.77f + .16f * std::sin(age * 23.f));
        const uint16_t tint = actor.variant > 0 ? rgb(255, 116, 163) : rgb(64, 211, 255);
        drawMissionTracer({feet + Vec3{0, 21.f, 0}, feet, strength, tint, TracerStyle::DropBeam}, time);
        drawGlowDisc(feet + Vec3{0, .12f, 0}, .54f + age * .45f, rgb(217, 250, 255), tint, strength, false);
        if (p.z > 31.f) return;
        const float radius = .37f + age * 1.10f;
        constexpr float c = .86603f;
        constexpr Vec3 outline[]{{1, 0, 0}, {c, 0, .5f}, {.5f, 0, c}, {0, 0, 1}, {-.5f, 0, c}, {-c, 0, .5f},
                                 {-1, 0, 0}, {-c, 0, -.5f}, {-.5f, 0, -c}, {0, 0, -1}, {.5f, 0, -c}, {c, 0, -.5f}};
        Vec3 from = view.transform(feet + outline[std::size(outline) - 1] * radius);
        const unsigned alpha = unsigned(strength * .76f * 220.f);
        for (const Vec3 offset : outline) {
            const Vec3 to = view.transform(feet + offset * radius);
            drawViewTracer(from, to, tint, alpha);
            from = to;
        }
    }

    void drawShieldGlows(float realTime) {
        const auto plot = [&](int x, int y, uint16_t z, uint16_t tint, unsigned a) {
            if (x < 0 || x >= view.width || y < 0 || y >= view.height) return;
            const int at = y * view.width + x;
            if (z + 8 >= depth[at]) color[at] = blend(color[at], tint, a);
        };
        for (size_t i = 0; i < shieldGlowCount; ++i) {
            const auto& sg = shieldGlows[i];
            const Vertex centre{view.transform(sg.pos), 0, 0};
            if (centre.position.z < view.near + 0.4f) continue;
            const auto p = view.project(centre);
            const float rx = std::min(float(view.width) * 0.20f, sg.radius * view.focal / centre.position.z);
            const float ry = std::min(float(view.height) * 0.30f, sg.height * view.focal / centre.position.z);
            if (rx < 4.f || ry < 6.f) continue;
            const uint16_t z = enhanced ? encodedDepth(p.depth) : uint16_t(std::clamp(p.depth * view.near * 65534.f, 0.f, 65535.f));
            const float shimmer = 0.76f + 0.24f * std::sin(realTime * 11.f + float(i) * 2.1f);
            const unsigned rimA = unsigned(std::clamp(sg.strength * shimmer * 155.f, 44.f, 172.f));
            // Connected octagonal energy barrier contour around shielded Enforcers & linked Troopers.
            constexpr int corners = 8;
            float vx[corners], vy[corners];
            for (int c = 0; c < corners; ++c) {
                const float a = (2.f * pi * float(c)) / float(corners) + pi / 8.f;
                vx[c] = p.x + std::cos(a) * rx;
                vy[c] = p.y + std::sin(a) * ry;
            }
            for (int c = 0; c < corners; ++c) {
                const int next = (c + 1) % corners;
                const float dx = vx[next] - vx[c], dy = vy[next] - vy[c];
                const int seg = std::clamp(int(std::max(std::abs(dx), std::abs(dy))), 1, 96);
                for (int s = 0; s <= seg; ++s) {
                    const float t = float(s) / float(seg);
                    const int px = int(vx[c] + dx * t), py = int(vy[c] + dy * t);
                    plot(px, py, z, sg.color, rimA);
                    plot(px + 1, py, z, sg.color, rimA / 2);
                    plot(px, py + 1, z, sg.color, rimA / 2);
                }
            }
        }
    }

    void queueAir(Vec3 world, float width, float height, float thickness, float opacity,
                  uint16_t top, uint16_t bottom, const atmosphere::Mask& mask) {
        const Vec3 at = view.transform(world);
        if (at.z < view.near + .12f || at.z > 96.f || airSprites.size() == airSprites.capacity()) return;
        const auto p = view.project({at, 0, 0});
        const float rx = std::min(width * view.focal * p.depth, view.width * .33f);
        const float ry = std::min(height * view.focal * p.depth, view.height * .28f);
        if (rx < 1.f || ry < 1.f || p.x + rx < 0 || p.x - rx >= view.width ||
            p.y + ry < 0 || p.y - ry >= view.height) return;
        const float nearFade = std::clamp((at.z - thickness - .18f) * .8f, 0.f, 1.f);
        const unsigned alpha = unsigned(std::clamp(opacity * nearFade, 0.f, 255.f));
        if (alpha < 5) return;
        const auto zAt = [&](float z) {
            const float inv = 1.f / std::max(view.near, z);
            return enhanced ? encodedDepth(inv) : uint16_t(std::min(65535.f, inv * view.near * 65534.f));
        };
        airSprites.push_back({
            p.x, p.y, rx, ry, zAt(at.z + thickness), zAt(at.z - thickness), top, bottom, uint8_t(alpha), &mask
        });
    }

    void drawAir(const AirSprite& p) {
        constexpr int texels = atmosphere::maskSize, shift = 10;
        const float left = p.x - p.width, top = p.y - p.height;
        const int x0 = std::max(0, int(std::ceil(left))), x1 = std::min(view.width, int(p.x + p.width));
        const int y0 = std::max(0, int(std::ceil(top))), y1 = std::min(view.height, int(p.y + p.height));
        if (x0 >= x1 || y0 >= y1) return;
        const int stride = p.width >= 37.f ? 2 : 1;
        const float tx = float(texels << shift) / (2.f * p.width), ty = float(texels) / (2.f * p.height);
        const int du = int(tx * stride), first = int((x0 - left) * tx);
        const int samples = (x1 - x0 + stride - 1) / stride;
        const float samplesPerUnit = 1.f / std::max(du, 1);
        const unsigned falloff = 256u * 256u / unsigned(std::max(1, int(p.front) - p.back));
        for (int y = y0; y < y1; ++y) {
            const int row = std::clamp(int((y - top) * ty), 0, texels - 1);
            const auto span = p.mask->spans[row];
            if (span.end == 0) continue;
            const auto* density = p.mask->pixels.data() + row * texels;
            int begin = 0, end = samples;
            if (samples > 24 && du > 0) {
                // Leave a sample of slack at either edge to preserve the exact fixed-point lookup.
                begin = std::max(0, int(((span.first << shift) - first) * samplesPerUnit) - 1);
                end = std::min(samples, int(((span.end << shift) - first) * samplesPerUnit) + 2);
            }
            const uint16_t tint = p.top == p.bottom ? p.top : blend(p.top, p.bottom, unsigned(row * 255 / (texels - 1)));
            const int offset = y * view.width;
            for (int i = begin, u = first + begin * du; i < end; ++i, u += du) {
                const unsigned alpha = unsigned(p.alpha) * density[std::min(texels - 1, u >> shift)] >> 8;
                if (alpha < 4) continue;
                const int x = x0 + i * stride;
                const auto pixel = [&](int i) {
                    const unsigned world = depth[i];
                    if (world > p.front) return;
                    const unsigned fade = world > p.back ? 256u - ((world - p.back) * falloff >> 8) : 256u;
                    color[i] = blend(color[i], tint, alpha * fade >> 8);
                };
                pixel(offset + x);
                if (stride == 2 && x + 1 < x1) pixel(offset + x + 1);
            }
        }
    }

    void valleyMist(float time, float density, float gust) {
        const float baseAlpha = 61.f + density * 100.f;
        for (size_t b = 0; b < assets.mist.size(); ++b) {
            const auto& bank = assets.mist[b];
            const float sun = std::max(0.f, dot(normalized(bank.base - view.position), sunsetSunDir));
            const auto tint = blend(rgb(111, 128, 157), rgb(219, 162, 132), unsigned(sun * 203.f));
            for (size_t i = 0; i < mistLayers; ++i) {
                const float life = unitCycle(time * bank.rate + float(b) * .29f + float(i) / mistLayers);
                const float fade = 4.f * life * (1.f - life);
                const Vec3 at = bank.base + bank.drift * (life - .5f + gust * .18f) +
                                Vec3{0, .15f + i * .42f - .52f * density, 0};
                queueAir(at, bank.width * (.92f + .27f * life), bank.height * (.37f + i * .20f),
                         .65f + i * .31f, baseAlpha * fade * (i ? .64f : 1.f), tint, tint,
                         atmosphere::clouds[(b + i) % atmosphere::clouds.size()]);
            }
        }
    }

    void drawTracer(const Tracer& shot, uint16_t tint = rgb(255, 195, 107)) {
        if (shot.time <= 0) return;
        const unsigned alpha = unsigned(std::clamp(int(shot.time * 220.f), 72, 220));
        drawViewTracer(view.transform(shot.start), view.transform(shot.end), tint, alpha);
    }

    void drawViewTracer(Vec3 from, Vec3 to, uint16_t tint, unsigned alpha) {
        if (alpha < 4) return;
        Projected p, q;
        if (!projectSegment(view, from, to, p, q)) return;
        const float dx = q.x - p.x, dy = q.y - p.y;
        const int count = int(std::clamp(std::max(std::abs(dx), std::abs(dy)), 1.0f, 2.0f * view.width));
        const auto visible = count > 32 ? visibleSamples(view, p, q, count, 2.f) : SampleRange{0, count};
        for (int i = visible.first; i <= visible.last; ++i) {
            const float t = float(i) / count;
            const int x = int(p.x + dx * t), y = int(p.y + dy * t);
            if (x < 0 || x >= view.width || y < 0 || y >= view.height) continue;
            const float inverseZ = p.depth + (q.depth - p.depth) * t;
            const float z = enhanced ? encodedDepth(inverseZ) : inverseZ * view.near * 65534;
            const int at = y * view.width + x;
            if (z + 1 >= depth[at]) color[at] = blend(color[at], tint, alpha);
        }
    }

    void smokePlumes(float time, float gust) {
        std::array<Vec3, smokePuffs> curls{};
        for (size_t j = 0; j < curls.size(); ++j) {
            const float curl = time * .52f + j * 1.7f;
            curls[j] = {std::sin(curl), 0, std::cos(curl)};
        }
        for (size_t i = 0; i < assets.smoke.size(); ++i) {
            const auto& src = assets.smoke[i];
            for (size_t j = 0; j < smokePuffs; ++j) {
                const float life = unitCycle(time * src.rate + float(j) / smokePuffs + float(i) * .37f);
                const float rise = life * (.47f + .53f * life);
                const Vec3 at = src.base + src.drift * (rise * (.8f + gust * .25f)) +
                                Vec3{curls[j].x * rise * 1.8f, src.height * rise, curls[j].z * life};
                const float spread = src.size * (.46f + life * 1.4f);
                const float strength = std::min(1.f, life * 12.f) * (1.f - life);
                const unsigned heat = unsigned(180.f * (1.f - life) * (1.f - life));
                const auto rim = blend(rgb(111, 111, 134), rgb(134, 86, 69), heat);
                const auto soot = blend(rgb(47, 51, 65), rgb(58, 37, 34), heat);
                queueAir(at, spread, spread * 1.32f, spread * .48f, 243.f * strength, rim, soot,
                         atmosphere::clouds[(i + j) % atmosphere::clouds.size()]);
            }
        }
    }

    void firePlumes(float time, float gust) {
        constexpr uint16_t orange = rgb(255, 76, 10), yellow = rgb(255, 208, 88), hot = rgb(255, 247, 180);
        for (size_t i = 0; i < assets.fires.size(); ++i) {
            const auto& src = assets.fires[i];
            for (size_t j = 0; j < fireParticles; ++j) {
                const auto& pattern = firePatterns[i].flames[j];
                const float life = unitCycle(time * (.84f + .10f * (j % 4)) + pattern.phase * .55f);
                const float height = src.height * (.22f + .32f * life);
                const Vec3 offset{pattern.x + life * (.38f + .31f * gust),
                                  height * .68f + src.height * life * .48f, pattern.z};
                const float light = 4.f * life * (1.f - life);
                const float width = src.width * (.47f - life * .20f);
                queueAir(src.base + offset, width, height, .12f + width * .24f, light * 253.f,
                         orange, (j & 1) ? yellow : hot, atmosphere::flames[j % atmosphere::flames.size()]);
            }
        }
    }

    void fireSparks(float time, float gust) {
        for (size_t i = 0; i < assets.fires.size(); ++i) {
            const auto& src = assets.fires[i];
            if (view.transform(src.base).z > 42.f) continue;
            for (size_t j = 0; j < fireParticles; ++j) {
                const auto& pattern = firePatterns[i].sparks[j];
                const float life = unitCycle(time * (.43f + (j % 4) * .11f) + pattern.phase);
                const Vec3 rise{pattern.x + life * (1.8f + gust),
                                src.height * (.46f + life * (1.8f + .15f * j)), pattern.z};
                const Vec3 tip = src.base + rise;
                drawTracer({tip - Vec3{.09f, .15f + life * .12f, 0}, tip, (1.f - life) * .79f},
                           life < .3f ? rgb(255, 213, 111) : rgb(240, 115, 48));
            }
        }
    }

    void flareSmoke(float time, float gust) {
        for (size_t i = 0; i < assets.roadFlares.size(); ++i) {
            const Vec3 tip = assets.roadFlares[i];
            for (size_t j = 0; j < flarePuffs; ++j) {
                const float age = unitCycle(time * .44f + i * .23f + float(j) / flarePuffs);
                const Vec3 drift{age * age * (.76f + .38f * gust), .13f + age * .94f, -.24f * age};
                queueAir(tip + drift, .14f + age * .34f, .24f + age * .25f, .17f, 120.f * (1.f - age),
                         rgb(118, 82, 91), rgb(229, 63, 60), atmosphere::clouds[(i + j) % atmosphere::clouds.size()]);
            }
        }
    }

    void rainSquall(float time, float strength, float gust) {
        if (strength < .06f) return;
        const auto wrap = [](float x, float span) { return x - std::floor(x / span) * span; };
        uint32_t seed = 41281u;
        const auto random = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return float(seed >> 16) / 65536.f;
        };
        const Vec3 wind{1.6f + gust * 1.1f, 0, -.55f};
        const uint16_t tint = rgbClamped(138.f + strength * 30.f, 153.f + strength * 30.f, 177.f + strength * 26.f);
        const unsigned alpha = unsigned((strength * .51f) * 220.f);
        const int drops = enhanced ? 150 : 94;
        for (int i = 0; i < drops; ++i) {
            const float x = random() * 26.f, z = random() * 26.f, start = random() * 8.f;
            const Vec3 at{view.position.x + wrap(x + time * wind.x - view.position.x, 26.f) - 13.f,
                          wrap(start - time * (10.f + i % 4), 8.f),
                          view.position.z + wrap(z + time * wind.z - view.position.z, 26.f) - 13.f};
            const auto projected = view.transform(at);
            if (projected.z < 2.8f || std::abs(projected.x) > view.xSlope * projected.z + .3f ||
                std::abs(projected.y) > view.ySlope * projected.z + .6f) continue;
            const Vec3 end = at + Vec3{wind.x * .10f, -.38f - (i % 3) * .065f, wind.z * .10f};
            drawViewTracer(projected, view.transform(end), tint, alpha);
        }
    }

    void campaignAtmosphere(const Snapshot& state) {
        const auto start = timestamp();
        const float time = float(state.realTime);
        const float density = state.mission && state.mission->active ? state.mission->mistDensity : .74f;
        const float gust = std::sin(time * .61f) * .66f + std::sin(time * 1.17f) * .34f;
        airSprites.clear();
        valleyMist(time, density, gust);
        smokePlumes(time, gust);
        firePlumes(time, gust);
        flareSmoke(time, gust);
        std::sort(airSprites.begin(), airSprites.end(),
                  [](const AirSprite& a, const AirSprite& b) { return a.back < b.back; });
        for (const auto& sprite : airSprites) drawAir(sprite);
        fireSparks(time, gust);
        for (size_t i = 0; i < assets.roadFlares.size(); ++i) {
            const float glow = .72f + .20f * std::sin(time * 9.3f + i);
            drawGlowDisc(assets.roadFlares[i] + Vec3{0, .025f, 0}, .25f, rgb(255, 235, 197),
                         rgb(255, 56, 45), glow, false);
        }
        if (state.mission && state.mission->phase == MissionPhase::Extraction && !state.mission->cinematic) {
            rainSquall(time, std::clamp((density - .855f) * 11.f, 0.f, 1.f), gust);
        }
        recordRasterTime(start);
    }

    void weather(const Snapshot& state) {
        uint32_t seed = 48281;
        if (enhanced) {
            // Distant rain is short and faint. A few closer drops fall faster and draw longer streaks.
            struct Layer { int count, length; float speed; unsigned alpha; };
            static constexpr Layer layers[]{{36, 4, 630, 46}, {12, 9, 990, 76}};
            for (const auto& layer : layers) {
                const uint32_t offset = uint32_t(state.realTime * layer.speed);
                for (int i = 0; i < layer.count; ++i) {
                    seed = seed * 1664525u + 1013904223u;
                    const int x = int((seed >> 9) % unsigned(view.width - 4));
                    const int y = int(((seed >> 22) + offset) % unsigned(view.height - layer.length));
                    for (int n = 0; n < layer.length; ++n) {
                        const int at = (y + n) * view.width + x + n / 3;
                        color[at] = blend(color[at], rgb(176, 192, 206), layer.alpha);
                    }
                }
            }
            return;
        }
        const uint32_t offset = uint32_t(state.realTime * 630);
        for (int i = 0; i < 31; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const int x = int((seed >> 9) % unsigned(view.width - 2));
            const int y = int(((seed >> 22) + offset) % unsigned(view.height - 4));
            for (int n = 0; n < 4; ++n) {
                const int at = (y + n) * view.width + x + n / 3;
                color[at] = blend(color[at], rgb(168, 186, 202), 36);
            }
        }
    }

    void smoothDiagonals() {
        const auto start = timestamp();
        const int width = view.width;
        edgeRow.resize(width);
        std::copy_n(color, width, edgeRow.begin());
        for (int y = 1; y < view.height - 1; ++y) {
            auto* row = color + y * width;
            const auto* next = row + width;
            uint16_t left = row[0];
            edgeRow[0] = left;
            for (int x = 1; x < width - 1; ++x) {
                const uint16_t center = row[x];
                row[x] = softenDiagonal(center, edgeRow[x], row[x + 1], next[x], left);
                edgeRow[x] = left = center;
            }
            edgeRow[width - 1] = row[width - 1];
        }
        recordRasterTime(start);
    }

    void render(const Snapshot& state, int width, int height, uint16_t* target, bool collectStats) {
        if (!target || width < 5 || height < 10 || int64_t(width) * height > int64_t(depth.size())) return;
        measure = collectStats;
        const auto start = timestamp();
        stats = {};
        color = target;
        constexpr Camera shipBox{{-13.8f, 6.6f, -13.8f}, 0.60f, 0.32f, 59 * pi / 180};
        constexpr Camera briarHouse{{13.3f, 8.3f, -24.4f}, -.46f, .13f, 72 * pi / 180};
        const auto& overview = assets.arena == Arena::BriarHouse ? briarHouse : shipBox;
        const bool title = state.match == Match::Start;
        const auto& camera = title ? overview : state.camera;
        view = makeView(camera, width, height, 0.10f);
        sun = light.sun;
        up = {0, 1, 0};
        muzzleGlowCount = 0;
        shieldGlowCount = 0;
        const bool missionActive = state.mission && state.mission->active;
        if (enhanced && invasionDusk) {
            updateDuskMist(missionActive ? state.mission->mistDensity : 0.74f, view.forward);
        }
        std::fill_n(depth.data(), width * height, 0);
        sky(camera);
        drawWorld(state);
        for (const auto& inst : assets.propInstances) {
            const Mesh& propMesh = inst.kind == PropKind::Truck ? assets.shipBoxTruck : assets.shipBoxCar;
            if (propMesh.triangles.empty() || !view.sees(inst.bounds)) continue;
            auto transform = orientation(inst.position, inst.yaw, 0, 0);
            if (inst.scale != 1.f) {
                transform.x *= inst.scale;
                transform.y *= inst.scale;
                transform.z *= inst.scale;
            }
            meshLight = standing(inst.position);
            submit(propMesh, state, transform, MeshSpace::World, nullptr, 0, false, nullptr, nullptr, nullptr, enhanced);
        }
        float rivalHeightScale = 1.f;
#ifdef FOD_LOCAL_DUEL
        rivalHeightScale = state.rivalHeightScale;
#endif
        if (!title) {
            if (missionActive) {
                for (const auto& ma : state.mission->actors) drawMissionActor(ma, state);
                drawUplink(state);
            } else {
                drawActor(state.rival, state, false, state.alternateRival, rivalHeightScale);
                if (state.freeForAll) drawActor(state.secondRival, state, false, true);
            }
        }
        if (state.dead) {
#ifdef FOD_LOCAL_DUEL
            if (state.localDuel) drawActor(state.player, state, false, !state.alternateRival);
            else
#endif
                drawActor(state.player, state, true);
        }
        meshLight = 255;
        if (!title && missionActive) {
            const float hover = float(std::sin(state.realTime * 0.45) * 0.55);
            const Transform lift = orientation({0, hover, 0}, 0, 0, 0);
            for (const auto& craft : assets.alienFleet) {
                if (craft.triangles.empty()) continue;
                if (!craft.chunks.empty()) {
                    Bounds b = craft.chunks.front().bounds;
                    b.min.y += hover;
                    b.max.y += hover;
                    if (!view.sees(b)) continue;
                }
                submit(craft, state, lift, MeshSpace::World);
            }
        }
        if (state.apacheActive) {
            submit(assets.apache, state,
                   orientation(state.apachePosition, state.apacheYaw, 0, 0), MeshSpace::World);
        }
        if (!title && missionActive && state.mission->evacVisible && !assets.transportHelo.triangles.empty()) {
            // The evac bird, with its rotor spun about the mast on top of the airframe's attitude.
            const auto body = orientation(state.mission->evacPos, state.mission->evacYaw, state.mission->evacPitch, 0);
            submit(assets.transportHelo, state, body, MeshSpace::World);
            const float spin = float(std::fmod(state.realTime * 23.0, 2 * pi));
            const float c = std::cos(spin), s = std::sin(spin);
            Transform rotor = body;
            rotor.origin = body(transportRotorMount);
            rotor.x = body.x * c - body.z * s;
            rotor.z = body.x * s + body.z * c;
            submit(assets.transportRotor, state, rotor, MeshSpace::World);
        }
        for (const auto& jet : state.jets) {
            if (jet.visible) submit(assets.jet, state, orientation(jet.position, jet.yaw, 0, 0), MeshSpace::World);
        }
        draw(solid, true);
        if (enhanced) {
            const auto shadows = timestamp();
            if (!title && missionActive) {
                for (const auto& ma : state.mission->actors) {
                    if (!ma.active || !ma.actor.visible) continue;
                    const Vec3 ground{ma.actor.position.x, 0.f, ma.actor.position.z};
                    contactShadow(ground, ma.role == MissionRole::AlienDrone ? 0.48f
                                          : ma.role == MissionRole::AlienTrooper ? 0.78f : 0.62f);
                }
            } else {
                if (!title && state.rival.visible && !assets.rivalFrames.empty()) contactShadow(state.rival.position, 0.62f);
                if (!title && state.freeForAll && state.secondRival.visible && !assets.rivalFrames.empty()) {
                    contactShadow(state.secondRival.position, 0.62f);
                }
            }
            if (state.dead && state.player.visible && !assets.rivalFrames.empty()) contactShadow(state.player.position, 0.62f);
            recordRasterTime(shadows);
        }
        drawSkyCube();
        draw(translucent, false);
        if (invasionDusk) campaignAtmosphere(state);
        if (missionActive) {
            drawShieldGlows(float(state.realTime));
            for (const auto& actor : state.mission->actors) {
                if (actor.active && !actor.dead && actor.arrival > 0.f) drawArrival(actor, float(state.realTime));
            }
            const unsigned plasmaPhase = unsigned(state.realTime * 42.0);
            for (size_t i = 0; i < muzzleGlowCount; ++i) {
                const auto& mg = muzzleGlows[i];
                if (mg.plasma) drawPlasmaCloud(mg.pos, mg.radius, mg.intensity, plasmaPhase + unsigned(i) * 11u, mg.star);
                else drawGlowDisc(mg.pos, mg.radius, mg.core, mg.halo, mg.intensity, mg.star);
            }
            for (size_t i = 0; i < state.mission->plasma.size(); ++i) {
                const auto& cloud = state.mission->plasma[i];
                if (cloud.radius <= 0.f) continue;
                const unsigned phase = plasmaPhase + unsigned(i) * 19u;
                drawPlasmaCloud(cloud.position - cloud.direction * (cloud.radius * .94f),
                                cloud.radius * .79f, .25f, phase + 7u, false);
                drawPlasmaCloud(cloud.position, cloud.radius * 1.24f, cloud.charged ? .98f : .85f,
                                phase, cloud.charged);
            }
            for (const auto& shot : state.mission->tracers) {
                if (shot.time > 0) drawMissionTracer(shot, float(state.realTime));
            }
        } else {
#ifdef FOD_LOCAL_DUEL
            if (state.localDuel) drawTracer(state.rivalTracers[0]);
            else
#endif
                drawTracer({state.tracerStart, state.tracerEnd, state.tracer});
            if (state.freeForAll) {
                for (const auto& shot : state.rivalTracers) drawTracer(shot);
            }
        }
        if (assets.arena == Arena::ShipBox) weather(state);
        const bool introWeaponHidden = missionActive && state.lower >= 0.99f &&
                                       (state.mission->phase == MissionPhase::Intro || state.mission->cinematic);
        if (!title && !state.dead && !introWeaponHidden && state.device == Device::None && state.scope < 0.6f &&
            !assets.weapon[size_t(state.weapon)].triangles.empty()) {
            // The weapon is posed in view space, so bring the daylight into that space with it.
            sun = {dot(light.sun, view.right), dot(light.sun, view.up), dot(light.sun, view.forward)};
            up = {view.right.y, view.up.y, view.forward.y};
            meshLight = standing(camera.position);
            Camera firstPerson{{}, 0, 0, state.camera.fov};
            view = makeView(firstPerson, width, height, 0.012f);
            std::fill_n(depth.data(), width * height, 0);
            drawWeapon(state);
            draw(solid, true);
            draw(translucent, false);
        }
        if (enhanced) smoothDiagonals();
        if (measure) {
            const auto elapsed = microseconds(start, Clock::now());
            stats.setupUs = elapsed - std::min(elapsed, stats.rasterUs);
        }
    }
};

Renderer::Renderer(const Assets& assets, RenderLook look, size_t maxPixels)
    : impl_(std::make_unique<Impl>(assets, look, maxPixels)) {}
Renderer::~Renderer() = default;
void Renderer::render(const Snapshot& state, int width, int height, uint16_t* rgb565, bool measure) {
    impl_->render(state, width, height, rgb565, measure);
}
const RenderStats& Renderer::stats() const { return impl_->stats; }
uint64_t Renderer::workingBytes() const {
    uint64_t mips = impl_->mipmaps.capacity() * sizeof(std::vector<Texture>);
    for (const auto& levels : impl_->mipmaps) {
        mips += levels.capacity() * sizeof(Texture);
        for (const auto& level : levels) mips += level.pixels.capacity() * sizeof(uint16_t);
    }
    uint64_t duskSky = 0;
    for (const auto& face : impl_->sunsetSky) duskSky += face.capacity() * sizeof(uint16_t);
    return sizeof(Impl) + mips + duskSky + impl_->light.bytes() + (impl_->depth.capacity() + impl_->edgeRow.capacity()) * sizeof(uint16_t) +
           impl_->airSprites.capacity() * sizeof(Impl::AirSprite) +
           impl_->firePatterns.capacity() * sizeof(Impl::FirePattern) +
           impl_->culledNormals.capacity() * sizeof(Vec3) + impl_->worldLights.capacity() +
           impl_->posedVertices.capacity() * sizeof(PosedVertex) +
           (impl_->solid.capacity() + impl_->translucent.capacity()) * sizeof(Primitive) +
           impl_->visible.capacity() * sizeof(std::pair<float, uint32_t>);
}

bool WorldFrame::render(const Snapshot& snapshot, uint32_t matchRevision, int width, int height) {
    if (snapshot.match != Match::Playing && matchRevision == matchRevision_ && width == width_ && height == height_ &&
        snapshot.difficulty == difficulty_) return false;
    renderer_.render(snapshot, width, height, pixels_, false);
    matchRevision_ = matchRevision;
    width_ = width;
    height_ = height;
    difficulty_ = snapshot.difficulty;
    return true;
}

} // namespace fod
