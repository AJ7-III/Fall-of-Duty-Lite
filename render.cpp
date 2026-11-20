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
#include <new>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#endif

namespace fod {
namespace {

using Clock = std::chrono::steady_clock;
#ifdef ESP_PLATFORM
constexpr bool countRenderWork = false;
#else
constexpr bool countRenderWork = true;
#endif

// Prefer ESP32-P4 on-chip L2MEM SRAM for the high-traffic depth buffer while preserving
// headroom for drivers and stacks; fall back cleanly to PSRAM when tight.
class DepthBuffer {
public:
    explicit DepthBuffer(size_t count) : count_(count) {
#ifdef ESP_PLATFORM
        const size_t bytes = count * sizeof(uint16_t);
        constexpr size_t reserveInternal = 96 * 1024;
        if (bytes && heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= bytes + reserveInternal) {
            data_ = static_cast<uint16_t*>(heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
        if (!data_ && bytes) {
            data_ = static_cast<uint16_t*>(heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!data_) throw std::bad_alloc();
        }
#else
        storage_.resize(count);
        data_ = storage_.data();
#endif
    }
    ~DepthBuffer() {
#ifdef ESP_PLATFORM
        if (data_) heap_caps_free(data_);
#endif
    }
    DepthBuffer(const DepthBuffer&) = delete;
    DepthBuffer& operator=(const DepthBuffer&) = delete;
    uint16_t* data() { return data_; }
    const uint16_t* data() const { return data_; }
    uint16_t& operator[](size_t i) { return data_[i]; }
    const uint16_t& operator[](size_t i) const { return data_[i]; }
    size_t size() const { return count_; }
    size_t capacity() const {
#ifdef ESP_PLATFORM
        return count_;
#else
        return storage_.capacity();
#endif
    }

private:
    uint16_t* data_ = nullptr;
    size_t count_ = 0;
#ifndef ESP_PLATFORM
    std::vector<uint16_t> storage_;
#endif
};

#if defined(__GNUC__) || defined(__clang__)
#define FOD_NOINLINE __attribute__((noinline))
#else
#define FOD_NOINLINE
#endif

inline int fastFloor(float x) {
    const int i = int(x);
    return i - int(x < float(i));
}

inline int fastCeil(float x) {
    const int i = int(x);
    return i + int(x > float(i));
}

inline float fastInvSqrt(float x) {
    uint32_t i;
    std::memcpy(&i, &x, sizeof(i));
    i = 0x5f3759dfu - (i >> 1);
    float y;
    std::memcpy(&y, &i, sizeof(y));
    return y * (1.5f - 0.5f * x * y * y);
}

constexpr float invSpan[9]{
    0.f, 1.f, 1.f / 2.f, 1.f / 3.f, 1.f / 4.f, 1.f / 5.f, 1.f / 6.f, 1.f / 7.f, 1.f / 8.f,
};

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
    Vec3 camera;
    Projected projected; // depth == 0 marks vertices behind the near plane
};

struct TexInfo {
    const uint16_t* pixels = nullptr;
    int width = 0, height = 0;
    int maskW = 0, maskH = 0;
    uint8_t logW = 0;
    bool pot = false;
    float fWidth = 0, fHeight = 0, invWidth = 0, invHeight = 0;
};

TexInfo makeTexInfo(const Texture& t) {
    const bool pot = t.width > 0 && t.height > 0 && (t.width & (t.width - 1)) == 0 && (t.height & (t.height - 1)) == 0;
    uint8_t logW = 0;
    if (pot) {
        int w = t.width;
        while (w > 1) { w >>= 1; ++logW; }
    }
    return {
        t.pixels.data(), t.width, t.height, t.width - 1, t.height - 1, logW, pot,
        float(t.width), float(t.height), t.width > 0 ? 1.f / float(t.width) : 0.f, t.height > 0 ? 1.f / float(t.height) : 0.f
    };
}

struct TextureSample {
    const TexInfo* fine;
    const TexInfo* coarse = nullptr;
    unsigned mix = 0;
};

enum class MeshSpace { World, View };

struct View {
    Vec3 position, right, up, forward, negOrigin;
    float focal, xSlope, ySlope, near;
    int width, height;

    Vec3 transform(Vec3 world) const {
        return {
            world.x * right.x + world.y * right.y + world.z * right.z + negOrigin.x,
            world.x * up.x + world.y * up.y + world.z * up.z + negOrigin.y,
            world.x * forward.x + world.y * forward.y + world.z * forward.z + negOrigin.z
        };
    }

    bool sees(Vec3 center, Vec3 extent) const {
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

    bool sees(const Bounds& bounds) const {
        return sees((bounds.min + bounds.max) * 0.5f, (bounds.max - bounds.min) * 0.5f);
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
    const Vec3 up = cross(front, right);
    const Vec3 negOrigin{-dot(camera.position, right), -dot(camera.position, up), -dot(camera.position, front)};
    const float slope = std::tan(camera.fov * 0.5f);
    return {camera.position, right, up, front, negOrigin, height * 0.5f / slope,
            slope * width / height, slope, near, width, height};
}

constexpr uint16_t rgb(int r, int g, int b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }

uint16_t multiply(uint16_t color, unsigned shade) {
    return uint16_t((((color >> 11) * shade >> 8) << 11) |
                    ((((color >> 5) & 63) * shade >> 8) << 5) |
                    ((color & 31) * shade >> 8));
}

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

uint16_t addLight(uint16_t pixel, uint16_t tint, unsigned strength) {
    const unsigned red = (pixel >> 11) + (((tint >> 11) * strength + 128) >> 8);
    const unsigned green = ((pixel >> 5) & 63) + ((((tint >> 5) & 63) * strength + 128) >> 8);
    const unsigned blue = (pixel & 31) + (((tint & 31) * strength + 128) >> 8);
    return uint16_t((std::min(red, 31u) << 11) | (std::min(green, 63u) << 5) | std::min(blue, 31u));
}

struct EnergyColors { uint16_t halo, core; };
constexpr EnergyColors tractorCyan{rgb(39, 186, 246), rgb(183, 255, 254)};
constexpr EnergyColors tractorViolet{rgb(142, 89, 255), rgb(221, 210, 255)};
constexpr EnergyColors tractorJammed{rgb(232, 52, 174), rgb(255, 193, 229)};
constexpr EnergyColors tractorPulled{rgb(255, 114, 42), rgb(255, 231, 164)};
constexpr EnergyColors rayBlue{rgb(35, 132, 255), rgb(200, 253, 255)};
constexpr EnergyColors rayHot{chargedPlasmaTint, rgb(255, 222, 146)};

int edgeLuma(uint16_t color) { return color & 0x7e0; }

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

uint8_t lightFor(Vec3 normal, bool metallic, Vec3 lightDir = {-0.28f, 0.86f, 0.43f}) {
    const float lenSq = dot(normal, normal);
    const float inv = lenSq > 1e-12f ? fastInvSqrt(lenSq) : 1e6f;
    const float lit = std::abs(dot(normal, lightDir)) * inv;
    return uint8_t(std::clamp(190.0f + lit * 65 + (metallic ? 20 : 0), 0.0f, 255.0f));
}

// Use the exponent and top mantissa bits of reciprocal depth. This retains
// the 16-bit buffer while allocating precision over the entire view distance.
inline uint16_t encodedDepth(float inverseZ) {
    if (inverseZ <= 0) return 0;
    uint32_t bits;
    std::memcpy(&bits, &inverseZ, sizeof(bits));
    constexpr uint32_t first = 0x3b800000; // 1 / 256
    constexpr uint32_t last = first + (65534u << 11);
    return uint16_t(1 + ((std::clamp(bits, first, last) - first) >> 11));
}

// Branchless fast path when inverseZ is already bounded within [1/256, 255].
inline uint16_t encodedDepthFast(float inverseZ) {
    uint32_t bits;
    std::memcpy(&bits, &inverseZ, sizeof(bits));
    return uint16_t(1u + ((bits - 0x3b800000u) >> 11));
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
    DepthBuffer depth;
    std::vector<Vec3> culledNormals;
    std::vector<uint8_t> worldLights;
    std::vector<uint16_t> edgeRow;
    std::vector<std::vector<Texture>> mipmaps;
    std::vector<TexInfo> baseTexInfo;
    std::vector<std::vector<TexInfo>> mipTexInfo;
    // Conservative 8x8 tile Hi-Z occlusion snapshot (built at phase boundaries, zero per-row dirty tracking).
    std::vector<uint16_t> tileMin;
    int tilesX = 0, tilesY = 0;
    bool hiZBuilt = false;
    bool hiZAnyOpen = true;
    std::array<std::vector<uint16_t>, 6> sunsetSky;
    std::vector<PosedVertex> posedVertices;
    // Reused for world chunk rendering.
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
    // Deferred until world and weapon meshes have written depth.
    struct MuzzleGlow {
        Vec3 pos{};
        float radius = 0;
        uint16_t core = 0, halo = 0;
        float intensity = 0;
        bool star = false;
    };
    struct RayGlow {
        Vec3 muzzle, axis, right, up;
        float charge, recoil, scale;
        bool hot;
    };
    struct ShieldGlow {
        Vec3 pos{};
        float radius = 0, height = 0;
        uint16_t color = 0;
        float strength = 0;
    };
    std::array<MuzzleGlow, MissionSnapshot::maxActors> muzzleGlows{};
    size_t muzzleGlowCount = 0;
    std::array<RayGlow, MissionSnapshot::maxActors> rayGlows{};
    size_t rayGlowCount = 0;
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
        vertices = std::max({vertices, source.alienUplink.vertices.size(), source.alienUplinkCore.vertices.size(),
                             source.relayTerminal.vertices.size()});
        for (const auto& craft : source.alienFleet) {
            vertices = std::max(vertices, craft.vertices.size());
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
        const size_t worldAlpha = alphaCount(source.world) + propAlpha + 5 * maxActorAlpha +
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
            const auto buildMeshMips = [&](const Mesh& mesh, const uint16_t* remap = nullptr) {
                for (const auto& triangle : mesh.triangles) {
                    const uint16_t material = remap ? remap[triangle.material] : triangle.material;
                    const auto id = source.materials[material].texture;
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
            for (const auto& inst : source.propInstances) {
                const auto& mesh = inst.kind == PropKind::Truck ? source.shipBoxTruck : source.shipBoxCar;
                buildMeshMips(mesh, inst.paintMaterials.empty() ? nullptr : inst.paintMaterials.data());
            }
        }
        baseTexInfo.reserve(source.textures.size());
        for (const auto& t : source.textures) baseTexInfo.push_back(makeTexInfo(t));
        mipTexInfo.resize(mipmaps.size());
        for (size_t i = 0; i < mipmaps.size(); ++i) {
            mipTexInfo[i].reserve(mipmaps[i].size());
            for (const auto& level : mipmaps[i]) mipTexInfo[i].push_back(makeTexInfo(level));
        }
        const size_t approxTiles = (maxPixels >> 6) + 256;
        tileMin.reserve(approxTiles);
        edgeRow.reserve(std::min<size_t>(maxPixels, 1280));
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
        const int x0 = minX <= 0.5f ? 0 : fastCeil(minX - 0.5f);
        const int x1 = maxX >= float(view.width) - 0.5f ? view.width - 1 : fastFloor(maxX - 0.5f);
        if (x0 > x1) return 0;
        const int y0 = minY <= 0.5f ? 0 : fastCeil(minY - 0.5f);
        const int y1 = maxY >= float(view.height) - 0.5f ? view.height - 1 : fastFloor(maxY - 0.5f);
        if (y0 > y1) return 0;
        return area < 0 ? 1 : -1;
    }

    // Build a conservative 8x8 tile minimum-depth snapshot once at phase boundaries.
    // Because opaque depth writes only increase depth, a snapshot stays strictly
    // conservative for all subsequent geometry without any per-row dirty tracking.
    void buildHiZ() {
        tilesX = (view.width + 7) >> 3;
        tilesY = (view.height + 7) >> 3;
        const size_t count = size_t(tilesX) * size_t(tilesY);
        if (tileMin.size() < count) tileMin.resize(count);
        const int w = view.width, h = view.height;
        const uint16_t* dBase = depth.data();
        bool anyOpen = false;
        const int fullTilesX = w >> 3;
        const int fullTilesY = h >> 3;
        for (int ty = 0; ty < tilesY; ++ty) {
            const int y0 = ty << 3;
            const int rows = ty < fullTilesY ? 8 : (h - y0);
            uint16_t* dst = tileMin.data() + ty * tilesX;
            for (int tx = 0; tx < fullTilesX; ++tx) {
                const uint16_t* p = dBase + y0 * w + (tx << 3);
                uint16_t m = 65535;
                for (int dy = 0; dy < rows; ++dy) {
                    const uint16_t* row = p + dy * w;
                    const uint16_t r = std::min({row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]});
                    if (r < m) m = r;
                    if (m == 0) break;
                }
                dst[tx] = m;
                if (m == 0) anyOpen = true;
            }
            if (fullTilesX < tilesX) {
                const int x0 = fullTilesX << 3;
                const int cols = w - x0;
                const uint16_t* p = dBase + y0 * w + x0;
                uint16_t m = 65535;
                for (int dy = 0; dy < rows; ++dy) {
                    const uint16_t* row = p + dy * w;
                    for (int dx = 0; dx < cols; ++dx) {
                        if (row[dx] < m) m = row[dx];
                    }
                    if (m == 0) break;
                }
                dst[fullTilesX] = m;
                if (m == 0) anyOpen = true;
            }
        }
        hiZBuilt = true;
        hiZAnyOpen = anyOpen;
    }

    // Conservative screen-space AABB test against the frozen 8x8 tile depth snapshot.
    // Uses 2 divisions (minZ/maxZ) instead of 8 per-corner projections.
    bool occluded(const Bounds& bounds) const {
        if (!hiZBuilt) return false;
        const Vec3 p0 = view.transform(bounds.min);
        const float sx = bounds.max.x - bounds.min.x;
        const float sy = bounds.max.y - bounds.min.y;
        const float sz = bounds.max.z - bounds.min.z;
        const float rx = sx * view.right.x, ry = sy * view.right.y, rz = sz * view.right.z;
        const float ux = sx * view.up.x, uy = sy * view.up.y, uz = sz * view.up.z;
        const float fx = sx * view.forward.x, fy = sy * view.forward.y, fz = sz * view.forward.z;
        const float minZ = p0.z + std::min(0.f, fx) + std::min(0.f, fy) + std::min(0.f, fz);
        if (minZ <= view.near + 0.01f) return false;
        const float maxZ = p0.z + std::max(0.f, fx) + std::max(0.f, fy) + std::max(0.f, fz);
        const float maxInvZ = 1.0f / minZ;
        const uint16_t raw = enhanced ? encodedDepth(maxInvZ)
                                      : uint16_t(std::clamp(maxInvZ * view.near * 65534.0f, 0.0f, 65535.0f));
        if (raw >= 65535) return false;
        const uint16_t threshold = uint16_t(raw + 1);
        const float bMinX = p0.x + std::min(0.f, rx) + std::min(0.f, ry) + std::min(0.f, rz);
        const float bMaxX = p0.x + std::max(0.f, rx) + std::max(0.f, ry) + std::max(0.f, rz);
        const float bMinY = p0.y + std::min(0.f, ux) + std::min(0.f, uy) + std::min(0.f, uz);
        const float bMaxY = p0.y + std::max(0.f, ux) + std::max(0.f, uy) + std::max(0.f, uz);
        const float sNear = view.focal * maxInvZ;
        const float sFar = view.focal / maxZ;
        const float halfW = view.width * 0.5f, halfH = view.height * 0.5f;
        const float minX = bMinX * (bMinX < 0.f ? sNear : sFar) + halfW;
        const float maxX = bMaxX * (bMaxX > 0.f ? sNear : sFar) + halfW;
        const float minY = halfH - bMaxY * (bMaxY > 0.f ? sNear : sFar);
        const float maxY = halfH - bMinY * (bMinY < 0.f ? sNear : sFar);
        const int x0 = std::max(0, fastFloor(minX) - 1);
        const int x1 = std::min(view.width - 1, fastCeil(maxX) + 1);
        const int y0 = std::max(0, fastFloor(minY) - 1);
        const int y1 = std::min(view.height - 1, fastCeil(maxY) + 1);
        if (x0 > x1 || y0 > y1) return true;
        const int tx0 = x0 >> 3, tx1 = x1 >> 3, ty0 = y0 >> 3, ty1 = y1 >> 3;
        for (int ty = ty0; ty <= ty1; ++ty) {
            const uint16_t* row = tileMin.data() + ty * tilesX;
            for (int tx = tx0; tx <= tx1; ++tx) {
                if (row[tx] < threshold) return false;
            }
        }
        return true;
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
        submitClipped(vertices, material, shade(front), mipmapped);
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

    FOD_NOINLINE void submitClipped(const Vertex* vertices, uint16_t material, const Shade& lit, bool mipmapped) {
        const LitVertex corners[]{
            {vertices[0], float(lit.corner[0])}, {vertices[1], float(lit.corner[1])}, {vertices[2], float(lit.corner[2])},
        };
        clip(corners, [&](const LitVertex& x, const LitVertex& y, const LitVertex& z) {
            Primitive clipped{{}, material, lit.patch, {}, mipmapped};
            if (project(clipped, x, y, z)) enqueue(clipped);
        });
    }

    void submit(const Mesh& mesh, const Snapshot& state, const Transform& transform, MeshSpace space,
                const Mesh* next = nullptr, float fraction = 0, bool metallic = false,
                const Mesh* topology = nullptr, const uint16_t* uniform = nullptr, const MotionBlend* motion = nullptr,
                bool mipmapped = false, bool clearDepth = false) {
        const auto& base = topology ? *topology : mesh;
        const size_t vertices = mesh.vertexCount();
        const auto rotToView = [&](Vec3 v) -> Vec3 { return {dot(v, view.right), dot(v, view.up), dot(v, view.forward)}; };
        const Transform mv = space == MeshSpace::View
            ? transform
            : Transform{view.transform(transform.origin), rotToView(transform.x), rotToView(transform.y), rotToView(transform.z)};
        const Vec3 camSun = space == MeshSpace::View ? sun : rotToView(sun);
        const Vec3 camUp = space == MeshSpace::View ? up : Vec3{view.right.y, view.up.y, view.forward.y};
        constexpr Vec3 worldClassicLight{-0.28f, 0.86f, 0.43f};
        const Vec3 classicDir = space == MeshSpace::View ? worldClassicLight : rotToView(worldClassicLight);

        const float halfW = view.width * 0.5f, halfH = view.height * 0.5f, focal = view.focal, nearZ = view.near;
        const Vertex* baseVerts = base.vertices.data();
        PosedVertex* dst = posedVertices.data();
        float bMinX = 1e9f, bMaxX = -1e9f, bMinY = 1e9f, bMaxY = -1e9f;
        bool anyBehindNear = false;

        const bool blendNext = next != nullptr && fraction > 0.f;
        const Vec3* meshPose = mesh.posePositions.empty() ? nullptr : mesh.posePositions.data();
        const Vertex* meshVerts = mesh.vertices.data();
        const Vec3* nextPose = (blendNext && !next->posePositions.empty()) ? next->posePositions.data() : nullptr;
        const Vertex* nextVerts = blendNext ? next->vertices.data() : nullptr;

        for (size_t i = 0; i < vertices; ++i) {
            Vec3 pos;
            if (motion) {
                pos = motion->position(i);
                if (blendNext) {
                    const Vec3 mPos = meshPose ? meshPose[i] : meshVerts[i].position;
                    const Vec3 nPos = nextPose ? nextPose[i] : nextVerts[i].position;
                    pos += (nPos - mPos) * fraction;
                }
            } else {
                pos = meshPose ? meshPose[i] : meshVerts[i].position;
                if (blendNext) {
                    const Vec3 nPos = nextPose ? nextPose[i] : nextVerts[i].position;
                    pos += (nPos - pos) * fraction;
                }
            }
            const Vec3 cam = mv(pos);
            if (cam.z >= nearZ) {
                const float inv = 1.0f / cam.z;
                const float sc = focal * inv;
                const float px = cam.x * sc + halfW, py = halfH - cam.y * sc;
                dst[i] = {cam, {px, py, inv, baseVerts[i].u * inv, baseVerts[i].v * inv}};
                if (clearDepth) {
                    if (px < bMinX) bMinX = px;
                    if (px > bMaxX) bMaxX = px;
                    if (py < bMinY) bMinY = py;
                    if (py > bMaxY) bMaxY = py;
                }
            } else {
                dst[i] = {cam, {}};
                anyBehindNear = true;
            }
        }

        if (clearDepth) {
            if (anyBehindNear) {
                std::fill_n(depth.data(), view.width * view.height, 0);
            } else {
                const int x0 = std::max(0, fastFloor(bMinX) - 1);
                const int x1 = std::min(view.width - 1, fastCeil(bMaxX) + 1);
                const int y0 = std::max(0, fastFloor(bMinY) - 1);
                const int y1 = std::min(view.height - 1, fastCeil(bMaxY) + 1);
                if (x0 <= x1 && y0 <= y1) {
                    const int span = x1 - x0 + 1;
                    for (int y = y0; y <= y1; ++y) std::fill_n(depth.data() + y * view.width + x0, span, 0);
                }
            }
        }

        const bool anyBrokenGlass = state.brokenGlass != 0;
        for (const Triangle& tri : base.triangles) {
            if (anyBrokenGlass && !intact(state, tri.glass)) continue;
            const auto material = uniform ? uniform[tri.material] : tri.material;
            const auto& mat = assets.materials[material];
            if (mat.alpha == 0) continue;
            const auto& a = dst[tri.a];
            const auto& b = dst[tri.b];
            const auto& c = dst[tri.c];
            const bool doubleSided = mat.doubleSided;
            Vec3 normal{};
            if (!doubleSided) {
                normal = cross(b.camera - a.camera, c.camera - a.camera);
                if (dot(normal, a.camera) <= 0) continue;
            }
            const auto shadeFace = [&](bool front) -> Shade {
                if (invasionDusk && mat.emissive) return Shade{{255, 255, 255}};
                const Vec3 turned = doubleSided ? cross(b.camera - a.camera, c.camera - a.camera) : normal;
                const uint8_t level = enhanced ? SceneLight::surface(front ? turned * -1 : turned, camSun, camUp, meshLight)
                                               : lightFor(turned, metallic, classicDir);
                return Shade{{level, level, level}};
            };
            if (a.projected.depth > 0 && b.projected.depth > 0 && c.projected.depth > 0) {
                Primitive value{{a.projected, b.projected, c.projected}, material, SceneLight::noPatch, {}, mipmapped};
                if (const int facing = visibleProjection(value)) {
                    illuminate(value, shadeFace(facing > 0), facing > 0);
                    enqueue(value);
                }
                continue;
            }
            if (a.projected.depth == 0 && b.projected.depth == 0 && c.projected.depth == 0) continue;
            const Vertex verts[]{
                {a.camera, baseVerts[tri.a].u, baseVerts[tri.a].v},
                {b.camera, baseVerts[tri.b].u, baseVerts[tri.b].v},
                {c.camera, baseVerts[tri.c].u, baseVerts[tri.c].v},
            };
            const bool front = dot(cross(b.camera - a.camera, c.camera - a.camera), a.camera) > 0;
            submitClipped(verts, material, shadeFace(front), mipmapped);
        }
    }

    void submitWorld(size_t first, size_t count, const Snapshot& state) {
        const auto& mesh = assets.world;
        const bool hasOneSided = !culledNormals.empty();
        const bool anyBrokenGlass = state.brokenGlass != 0;
        const auto vertex = [&](uint32_t i) {
            Vertex value = mesh.vertices[i];
            value.position = view.transform(value.position);
            return value;
        };
        for (size_t i = first; i < first + count; ++i) {
            const Triangle& tri = mesh.triangles[i];
            if (anyBrokenGlass && !intact(state, tri.glass)) continue;
            if (hasOneSided && !assets.materials[tri.material].doubleSided &&
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
        const unsigned fx = unsigned((x & 255) >> 3), fy = unsigned((y & 255) >> 3);
        const unsigned ix = 32u - fx, iy = 32u - fy;
        const uint16_t* r0 = face + y0 * size;
        const uint16_t* r1 = face + y1 * size;
        constexpr uint32_t mask = 0x07e0f81f;
        const auto unpack = [](uint16_t c) -> uint32_t { return ((uint32_t(c) << 16) | c) & mask; };
        const uint32_t top = ((unpack(r0[x0]) * ix + unpack(r0[x1]) * fx) >> 5) & mask;
        const uint32_t bot = ((unpack(r1[x0]) * ix + unpack(r1[x1]) * fx) >> 5) & mask;
        const uint32_t d = uint32_t(skyDither[(pixelY & 63) * 64 + (pixelX & 63)]) << 3; // 0..24
        const uint32_t dBias = (d << 21) | (d << 11) | d;
        const uint32_t val = ((top * iy + bot * fy + dBias) >> 5) & mask;
        return uint16_t(val | (val >> 16));
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

    static inline uint32_t toFixedFloor(float v, float invSize, float size) {
        if (v < -16000.f || v > 16000.f) {
            v = (v > -1e8f && v < 1e8f) ? (v - float(fastFloor(v * invSize)) * size) : 0.f;
        }
        const float s = v * 65536.0f;
        const int32_t i = int32_t(s);
        return uint32_t(i - int32_t(s < float(i)));
    }

    static inline uint32_t toFixedStep(float v, float invSize, float size) {
        if (v < -16000.f || v > 16000.f) {
            v = (v > -1e8f && v < 1e8f) ? (v - float(fastFloor(v * invSize)) * size) : 0.f;
        }
        return uint32_t(int32_t(v * 65536.0f + (v >= 0.f ? 0.5f : -0.5f)));
    }

    static FOD_NOINLINE TextureSample selectMip(const TexInfo* base, const std::vector<TexInfo>& levels, float footprint) {
        const TexInfo* fine = base;
        for (const auto& next : levels) {
            if (footprint <= 1.3f) break;
            if (footprint < 2.1f) return TextureSample{fine, &next, unsigned((footprint - 1.3f) * 320)};
            fine = &next;
            footprint *= 0.5f;
        }
        return TextureSample{fine};
    }

    static FOD_NOINLINE uint16_t fetchTexelSlow(const TextureSample& samp, float uVal, float vVal) {
        const TexInfo* tex = samp.fine;
        const int tx = tex->pot ? (fastFloor(uVal * tex->fWidth) & tex->maskW) : texel(uVal * tex->fWidth, tex->width);
        const int ty = tex->pot ? (fastFloor(vVal * tex->fHeight) & tex->maskH) : texel(vVal * tex->fHeight, tex->height);
        uint16_t pixel = tex->pot ? tex->pixels[(ty << tex->logW) + tx] : tex->pixels[ty * tex->width + tx];
        if (samp.coarse) {
            const auto& coarse = *samp.coarse;
            const bool half = tex->width == coarse.width * 2 && tex->height == coarse.height * 2;
            const int cx = half ? (tx >> 1) : texel(uVal * coarse.fWidth, coarse.width);
            const int cy = half ? (ty >> 1) : texel(vVal * coarse.fHeight, coarse.height);
            pixel = blend(pixel, coarse.pot ? coarse.pixels[(cy << coarse.logW) + cx]
                                            : coarse.pixels[cy * coarse.width + cx], samp.mix);
        }
        return pixel;
    }

    static inline uint16_t fetchTexel(const TextureSample& samp, float uVal, float vVal) {
        const TexInfo* tex = samp.fine;
        if (tex->pot && !samp.coarse) {
            const int tx = fastFloor(uVal * tex->fWidth) & tex->maskW;
            const int ty = fastFloor(vVal * tex->fHeight) & tex->maskH;
            return tex->pixels[(ty << tex->logW) + tx];
        }
        return fetchTexelSlow(samp, uVal, vVal);
    }

    template<Fill fill, bool detail = false, bool countPixels = false>
    void raster(const Primitive& t, bool opaque, const uint8_t* skyPixels = nullptr, const uint16_t* sunsetPixels = nullptr) {
        constexpr bool backdrop = fill == Fill::Sky, shadow = fill == Fill::Shadow;
        constexpr bool textured = fill == Fill::Textured, surface = textured || fill == Fill::Flat;
        constexpr bool usesUv = textured || backdrop;
        const auto& [a, b, c] = t.p;
        const Material* material = surface ? &assets.materials[t.material] : nullptr;
        const TexInfo* base = textured ? &baseTexInfo[material->texture] : nullptr;

        const float fMinX = std::min({a.x, b.x, c.x}), fMaxX = std::max({a.x, b.x, c.x});
        const float fMinY = std::min({a.y, b.y, c.y}), fMaxY = std::max({a.y, b.y, c.y});
        const int minX = fMinX <= 0.5f ? 0 : fastCeil(fMinX - 0.5f);
        int minY = fMinY <= 0.5f ? 0 : fastCeil(fMinY - 0.5f);
        const int maxX = fMaxX >= float(view.width) - 0.5f ? view.width - 1 : fastFloor(fMaxX - 0.5f);
        int maxY = fMaxY >= float(view.height) - 0.5f ? view.height - 1 : fastFloor(fMaxY - 0.5f);
        if (minX > maxX || minY > maxY) return;

        const float area = edge(a, b, c.x, c.y);
        const float dx[3]{b.y - c.y, c.y - a.y, a.y - b.y};
        const float dy[3]{c.x - b.x, a.x - c.x, b.x - a.x};
        const uint16_t flat = material ? multiply(material->color, t.light[0]) : 0;
        const float depthScale = view.near * 65534;
        const bool useEnhanced = enhanced;
        const uint8_t light0 = t.light[0];
        const uint16_t matColor = material ? material->color : 0;
        const unsigned matAlpha = material ? material->alpha : 255u;
        const GroundPatch* patch = detail && base && t.patch != SceneLight::noPatch ? &light.patches[t.patch] : nullptr;
        const bool smooth = detail && !patch && (t.light[0] != t.light[1] || t.light[0] != t.light[2]);
        const bool varying = smooth || patch;
        const bool hazy = detail && surface && std::min({a.depth, b.depth, c.depth}) < hazeStart;
        const unsigned hazeShift = material ? std::max(unsigned(material->hazeShift), unsigned(material->emissive)) : 0u;

        float eRow[3]{
            edge(b, c, minX + 0.5f, minY + 0.5f),
            edge(c, a, minX + 0.5f, minY + 0.5f),
            edge(a, b, minX + 0.5f, minY + 0.5f),
        };

        // Tier 1: Micro-triangle fast path (<= 3x3 pixel bbox, non-patched surfaces).
        // Avoids invDx divisions, gradient setup, and mip selection on uncovered or occluded micro-tris.
        if constexpr (surface) {
            if (!patch && maxX - minX <= 2 && maxY - minY <= 2) {
                float invArea = 0.f;
                bool areaReady = false;
                TextureSample sample{base};
                bool mipReady = !textured || !t.mipmapped || mipTexInfo[material->texture].empty();
                for (int y = minY; y <= maxY; ++y, eRow[0] += dy[0], eRow[1] += dy[1], eRow[2] += dy[2]) {
                    float e0 = eRow[0], e1 = eRow[1], e2 = eRow[2];
                    int at = y * view.width + minX;
                    for (int x = minX; x <= maxX; ++x, ++at, e0 += dx[0], e1 += dx[1], e2 += dx[2]) {
                        if (e0 < 0.f || e1 < 0.f || e2 < 0.f) continue;
                        if (!areaReady) {
                            invArea = 1.0f / area;
                            areaReady = true;
                        }
                        const float z = (a.depth * e0 + b.depth * e1 + c.depth * e2) * invArea;
                        const uint16_t newDepth = useEnhanced ? encodedDepth(z)
                            : uint16_t(std::clamp(z * depthScale, 0.0f, 65535.0f));
                        if (newDepth <= depth[at]) continue;
                        uint16_t pixel = flat;
                        if constexpr (textured) {
                            if (!mipReady) {
                                const float dzdx = (a.depth * dx[0] + b.depth * dx[1] + c.depth * dx[2]) * invArea;
                                const float dzdy = (a.depth * dy[0] + b.depth * dy[1] + c.depth * dy[2]) * invArea;
                                const float dudx = (a.u * dx[0] + b.u * dx[1] + c.u * dx[2]) * invArea;
                                const float dudy = (a.u * dy[0] + b.u * dy[1] + c.u * dy[2]) * invArea;
                                const float dvdx = (a.v * dx[0] + b.v * dx[1] + c.v * dx[2]) * invArea;
                                const float dvdy = (a.v * dy[0] + b.v * dy[1] + c.v * dy[2]) * invArea;
                                const float invM = 1.0f / std::max((a.depth + b.depth + c.depth) * (1.f / 3.f), 1e-6f);
                                const float um = (a.u + b.u + c.u) * (1.f / 3.f) * invM;
                                const float vm = (a.v + b.v + c.v) * (1.f / 3.f) * invM;
                                const float fp = std::max(
                                    std::max(std::abs(dudx - um * dzdx), std::abs(dudy - um * dzdy)) * base->fWidth,
                                    std::max(std::abs(dvdx - vm * dzdx), std::abs(dvdy - vm * dzdy)) * base->fHeight) * invM;
                                sample = selectMip(base, mipTexInfo[material->texture], fp);
                                mipReady = true;
                            }
                            const float invZ = 1.0f / std::max(z, 1e-6f);
                            pixel = fetchTexel(sample, (a.u * e0 + b.u * e1 + c.u * e2) * invArea * invZ,
                                                       (a.v * e0 + b.v * e1 + c.v * e2) * invArea * invZ);
                            if constexpr (!detail) pixel = multiply(pixel, light0);
                        }
                        if constexpr (detail) {
                            if (varying) {
                                const float lVal = (t.light[0] * e0 + t.light[1] * e1 + t.light[2] * e2) * invArea;
                                const unsigned d = (y & 1) ? ((x & 1) ? 96u : 224u) : ((x & 1) ? 160u : 32u);
                                pixel = shaded(textured ? pixel : matColor, unsigned(std::clamp(int(lVal), 0, 255)), d);
                            } else if constexpr (textured) {
                                pixel = multiply(pixel, light0);
                            }
                            if (hazy) {
                                if (const unsigned level = unsigned(haze[newDepth >> 6]) >> hazeShift) pixel = veil(pixel, level);
                            }
                        }
                        color[at] = opaque ? pixel : blend(color[at], pixel, matAlpha);
                        if (opaque) depth[at] = newDepth;
                        if constexpr (countPixels) ++stats.depthPasses;
                    }
                }
                return;
            }
        }

        const float inverseArea = 1.0f / area;
        const auto gradient = [&](float aa, float bb, float cc, const float* delta) {
            return (aa * delta[0] + bb * delta[1] + cc * delta[2]) * inverseArea;
        };
        const float dzdx = gradient(a.depth, b.depth, c.depth, dx);
        const float dzdy = gradient(a.depth, b.depth, c.depth, dy);
        const float dudx = usesUv ? gradient(a.u, b.u, c.u, dx) : 0.f;
        const float dudy = usesUv ? gradient(a.u, b.u, c.u, dy) : 0.f;
        const float dvdx = usesUv ? gradient(a.v, b.v, c.v, dx) : 0.f;
        const float dvdy = usesUv ? gradient(a.v, b.v, c.v, dy) : 0.f;

        const std::vector<TexInfo>* levels = nullptr;
        const auto mipAtInv = [&](float inv, float uOverZ, float vOverZ) {
            const float uVal = uOverZ * inv, vVal = vOverZ * inv;
            const float footprint = std::max(
                std::max(std::abs(dudx - uVal * dzdx), std::abs(dudy - uVal * dzdy)) * base->fWidth,
                std::max(std::abs(dvdx - vVal * dzdx), std::abs(dvdy - vVal * dzdy)) * base->fHeight) * inv;
            return selectMip(base, *levels, footprint);
        };
        TextureSample triangleTexture{base};
        bool perspectiveMips = false;
        if constexpr (textured) {
            if (t.mipmapped && !mipTexInfo[material->texture].empty()) {
                levels = &mipTexInfo[material->texture];
                perspectiveMips = std::max(maxX - minX, maxY - minY) >= 24 &&
                    std::max({a.depth, b.depth, c.depth}) > 1.35f * std::min({a.depth, b.depth, c.depth});
                if (!perspectiveMips) {
                    const float zAvg = (a.depth + b.depth + c.depth) * (1.f / 3.f);
                    triangleTexture = mipAtInv(1.0f / std::max(zAvg, 1e-6f),
                                               (a.u + b.u + c.u) * (1.f / 3.f), (a.v + b.v + c.v) * (1.f / 3.f));
                }
            }
        }

        // Clamp horizontal edges once up front and precompute -1/dx[i] for non-horizontal edges.
        float invDx[3]{};
        for (int i = 0; i < 3; ++i) {
            if (dx[i] != 0.f) {
                invDx[i] = -1.0f / dx[i];
            } else if (dy[i] > 0.f) {
                const int skip = fastCeil(std::clamp(-eRow[i] / dy[i], 0.f, float(maxY - minY + 1)));
                if (skip > 0) {
                    minY += skip;
                    eRow[0] += dy[0] * skip;
                    eRow[1] += dy[1] * skip;
                    eRow[2] += dy[2] * skip;
                }
            } else if (dy[i] < 0.f) {
                const int keep = fastFloor(std::clamp(-eRow[i] / dy[i], -1.f, float(maxY - minY)));
                maxY = std::min(maxY, minY + keep);
            } else if (eRow[i] < 0.f) {
                return;
            }
        }
        if (minY > maxY) return;

        // Light is 16.16 fixed point.
        constexpr int spanPixels = 8;
        const float rowLimit = float(maxX - minX + 1);
        const float dlitdx = smooth ? gradient(t.light[0], t.light[1], t.light[2], dx) : 0.f;
        const float dlitdy = smooth ? gradient(t.light[0], t.light[1], t.light[2], dy) : 0.f;
        const int cornerStep = smooth ? int(std::clamp(dlitdx, -255.0f, 255.0f) * 65536.f) : 0;
        float rowCells = 0, columnCells = 0;
        if (patch) {
            rowCells = std::max(std::abs(dzdx), std::abs(dzdy)) / light.cellSize();
            columnCells = 16.0f * 0.5f / (view.focal * light.cellSize());
        }

        float zRow = (a.depth * eRow[0] + b.depth * eRow[1] + c.depth * eRow[2]) * inverseArea;
        float uRow = usesUv ? (a.u * eRow[0] + b.u * eRow[1] + c.u * eRow[2]) * inverseArea : 0.f;
        float vRow = usesUv ? (a.v * eRow[0] + b.v * eRow[1] + c.v * eRow[2]) * inverseArea : 0.f;
        float litRow = smooth ? (t.light[0] * eRow[0] + t.light[1] * eRow[1] + t.light[2] * eRow[2]) * inverseArea : 0.f;
        const int maxRelX = maxX - minX;

        for (int y = minY; y <= maxY; ++y, eRow[0] += dy[0], eRow[1] += dy[1], eRow[2] += dy[2],
                                           zRow += dzdy, uRow += dudy, vRow += dvdy, litRow += dlitdy) {
            int left = 0, right = maxRelX;
            for (int i = 0; i < 3; ++i) {
                if (dx[i] > 0.f) left = std::max(left, fastCeil(std::clamp(eRow[i] * invDx[i], -1.0f, rowLimit)));
                else if (dx[i] < 0.f) right = std::min(right, fastFloor(std::clamp(eRow[i] * invDx[i], -1.0f, rowLimit)));
            }
            if (left > right) continue;

            float z = zRow + dzdx * float(left);
            float u = usesUv ? uRow + dudx * float(left) : 0.f;
            float v = usesUv ? vRow + dvdx * float(left) : 0.f;
            int at = y * view.width + minX + left;
            const int rowBaseX = minX + left;
            const int rowCount = right - left + 1;

            if constexpr (backdrop) {
                const uint16_t* rowTiles = hiZBuilt ? (tileMin.data() + (y >> 3) * tilesX) : nullptr;
                float cachedInvZ = 0.f;
                int cachedX = -1;
                for (int x = left; x <= right;) {
                    const int px = minX + x;
                    if (rowTiles && rowTiles[px >> 3] > 0) {
                        const int skip = std::min(right - x + 1, 8 - (px & 7));
                        x += skip;
                        at += skip;
                        z += dzdx * float(skip);
                        u += dudx * float(skip);
                        v += dvdx * float(skip);
                        continue;
                    }
                    const int spanLen = std::min(spanPixels, right - x + 1);
                    const uint16_t* dPtr = depth.data() + at;
                    uint8_t openMask = 0;
                    int first = spanLen, last = -1;
                    for (int k = 0; k < spanLen; ++k) {
                        if (!dPtr[k]) {
                            openMask |= uint8_t(1u << k);
                            if (first == spanLen) first = k;
                            last = k;
                        }
                    }
                    if (!openMask) {
                        x += spanLen;
                        at += spanLen;
                        z += dzdx * float(spanLen);
                        u += dudx * float(spanLen);
                        v += dvdx * float(spanLen);
                        continue;
                    }
                    const float inv0 = (first == 0 && cachedX == x) ? cachedInvZ
                                                                    : (1.0f / std::max(z + dzdx * float(first), 1e-6f));
                    float uCur = (u + dudx * float(first)) * inv0;
                    float vCur = (v + dvdx * float(first)) * inv0;
                    if (first == last) {
                        color[at + first] = sunsetPixels ? sampleSunsetSky(sunsetPixels, uCur, vCur, px + first, y)
                                                         : sampleSky(skyPixels, uCur, vCur, px + first, y);
                        if constexpr (countPixels) ++stats.depthPasses;
                    } else {
                        const bool carry = first == 0 && spanLen == 8 && x + 8 <= right;
                        const int endOff = carry ? 8 : last;
                        const float inv1 = 1.0f / std::max(z + dzdx * float(endOff), 1e-6f);
                        if (carry) { cachedInvZ = inv1; cachedX = x + 8; }
                        const float invStep = invSpan[endOff - first];
                        const float du = ((u + dudx * float(endOff)) * inv1 - uCur) * invStep;
                        const float dv = ((v + dvdx * float(endOff)) * inv1 - vCur) * invStep;
                        for (int k = first; k <= last; ++k, uCur += du, vCur += dv) {
                            if (openMask & (1u << k)) {
                                color[at + k] = sunsetPixels ? sampleSunsetSky(sunsetPixels, uCur, vCur, px + k, y)
                                                             : sampleSky(skyPixels, uCur, vCur, px + k, y);
                                if constexpr (countPixels) ++stats.depthPasses;
                            }
                        }
                    }
                    x += spanLen;
                    at += spanLen;
                    z += dzdx * float(spanLen);
                    u += dudx * float(spanLen);
                    v += dvdx * float(spanLen);
                }
                continue;
            }

            const float zLastRow = z + dzdx * float(rowCount - 1);
            const bool fastZ = useEnhanced && z >= (1.f / 256.f) && zLastRow >= (1.f / 256.f) &&
                               z <= 255.f && zLastRow <= 255.f;
            const unsigned dEven = (y & 1) ? 224u : 32u;
            const unsigned dOdd = (y & 1) ? 96u : 160u;
            const unsigned dToggle = dEven ^ dOdd;

            if constexpr (shadow) {
                int lit = smooth ? int(std::clamp(litRow + dlitdx * float(left), 0.0f, 255.0f) * 65536.f) : (light0 << 16);
                unsigned dCur = (rowBaseX & 1) ? dOdd : dEven;
                for (int x = left; x <= right; ++x, ++at, z += dzdx, lit += cornerStep, dCur ^= dToggle) {
                    const uint16_t newDepth = fastZ ? encodedDepthFast(z)
                        : useEnhanced ? encodedDepth(z) : uint16_t(std::clamp(z * depthScale, 0.0f, 65535.0f));
                    if (newDepth <= depth[at]) continue;
                    color[at] = shaded(color[at], std::min(unsigned(lit) >> 16, 255u), dCur);
                }
                continue;
            }

            if constexpr (fill == Fill::Flat) {
                if constexpr (!detail) {
                    if (opaque && fastZ) {
                        for (int x = left; x <= right; ++x, ++at, z += dzdx) {
                            const uint16_t newDepth = encodedDepthFast(z);
                            if (newDepth <= depth[at]) continue;
                            color[at] = flat;
                            depth[at] = newDepth;
                            if constexpr (countPixels) ++stats.depthPasses;
                        }
                    } else {
                        for (int x = left; x <= right; ++x, ++at, z += dzdx) {
                            const uint16_t newDepth = fastZ ? encodedDepthFast(z)
                                : useEnhanced ? encodedDepth(z) : uint16_t(std::clamp(z * depthScale, 0.0f, 65535.0f));
                            if (newDepth <= depth[at]) continue;
                            color[at] = opaque ? flat : blend(color[at], flat, matAlpha);
                            if (opaque) depth[at] = newDepth;
                            if constexpr (countPixels) ++stats.depthPasses;
                        }
                    }
                } else {
                    int lit = smooth ? int(std::clamp(litRow + dlitdx * float(left), 0.0f, 255.0f) * 65536.f) : (light0 << 16);
                    unsigned dCur = (rowBaseX & 1) ? dOdd : dEven;
                    for (int x = left; x <= right; ++x, ++at, z += dzdx, lit += cornerStep, dCur ^= dToggle) {
                        const uint16_t newDepth = fastZ ? encodedDepthFast(z)
                            : useEnhanced ? encodedDepth(z) : uint16_t(std::clamp(z * depthScale, 0.0f, 65535.0f));
                        if (newDepth <= depth[at]) continue;
                        uint16_t pixel = varying ? shaded(matColor, std::min(unsigned(lit) >> 16, 255u), dCur) : flat;
                        if (hazy) {
                            if (const unsigned level = unsigned(haze[newDepth >> 6]) >> hazeShift) pixel = veil(pixel, level);
                        }
                        color[at] = opaque ? pixel : blend(color[at], pixel, matAlpha);
                        if (opaque) depth[at] = newDepth;
                        if constexpr (countPixels) ++stats.depthPasses;
                    }
                }
                continue;
            }

            // Fill::Textured: Early-Z 8-pixel span subdivision.
            // Defer GroundPatch row setup until at least one pixel on the row passes depth test.
            const float zRowStart = z, uRowStart = u, vRowStart = v;
            const int smoothStart = smooth ? int(std::clamp(litRow + dlitdx * float(left), 0.0f, 255.0f) * 65536.f)
                                           : (light0 << 16);
            bool patchReady = false, mapped = false;
            float cellX = 0, cellY = 0, cellStepX = 0, cellStepY = 0, mapMix = 0;
            int mapLevel = 0;
            const auto mapLight = [&](int pixels) {
                const int lx = int(cellX + cellStepX * pixels), ly = int(cellY + cellStepY * pixels);
                const int fine = light.sample(lx, ly, mapLevel);
                if (mapMix <= 0) return fine << 8;
                return (fine << 8) + int((light.sample(lx, ly, mapLevel + 1) - fine) * mapMix);
            };

            float cachedInvZ = 0.f;
            int cachedX = -1;
            int cachedMapLit = 0, cachedMapX = -1;
            for (int x = left; x <= right;) {
                const int spanLen = std::min(spanPixels, right - x + 1);
                uint16_t zBuf[8];
                uint8_t mask = 0;
                int first = 8, last = -1;
                const uint16_t* dPtr = depth.data() + at;
                float zScan = z;
                if (fastZ) {
                    for (int k = 0; k < spanLen; ++k, zScan += dzdx) {
                        const uint16_t nd = encodedDepthFast(zScan);
                        if (nd > dPtr[k]) {
                            zBuf[k] = nd;
                            mask |= uint8_t(1u << k);
                            if (first == 8) first = k;
                            last = k;
                        }
                    }
                } else {
                    for (int k = 0; k < spanLen; ++k, zScan += dzdx) {
                        const uint16_t nd = useEnhanced ? encodedDepth(zScan)
                            : uint16_t(std::clamp(zScan * depthScale, 0.0f, 65535.0f));
                        if (nd > dPtr[k]) {
                            zBuf[k] = nd;
                            mask |= uint8_t(1u << k);
                            if (first == 8) first = k;
                            last = k;
                        }
                    }
                }

                if (!mask) {
                    x += spanLen;
                    at += spanLen;
                    z = zScan;
                    u += dudx * float(spanLen);
                    v += dvdx * float(spanLen);
                    continue;
                }

                if constexpr (detail) {
                    if (patch && !patchReady) {
                        patchReady = true;
                        const float zEnd = zRowStart + dzdx * float(rowCount);
                        if (zRowStart > 0.002f && zEnd > 0.002f) {
                            const float inv = 1.0f / zRowStart, invEnd = 1.0f / zEnd;
                            const float uEnd = (uRowStart + dudx * float(rowCount)) * invEnd;
                            const float vEnd = (vRowStart + dvdx * float(rowCount)) * invEnd;
                            cellX = patch->xu * uRowStart * inv + patch->xv * vRowStart * inv + patch->x0;
                            cellY = patch->zu * uRowStart * inv + patch->zv * vRowStart * inv + patch->z0;
                            const float invCnt = 1.0f / float(rowCount);
                            cellStepX = (patch->xu * uEnd + patch->xv * vEnd + patch->x0 - cellX) * invCnt;
                            cellStepY = (patch->zu * uEnd + patch->zv * vEnd + patch->z0 - cellY) * invCnt;
                            float cells = std::max(rowCells * inv * inv, columnCells * inv);
                            while (cells > 2 && mapLevel + 2 < SceneLight::levels) {
                                cells *= 0.5f;
                                ++mapLevel;
                            }
                            mapMix = std::clamp(cells - 1, 0.0f, 1.0f) * 256.f;
                            mapped = true;
                        }
                    }
                }

                const float z0 = z + dzdx * float(first);
                const float zLast = z + dzdx * float(last);
                const int dSpan = last - first;

                // Short visible runs (1..3 px) or steep grazing spans: exact per-pixel 1/z with zero span setup.
                if (dSpan <= 2 || std::abs(zLast - z0) > 0.10f * std::min(z0, zLast)) {
                    const float midOff = float(first + last) * 0.5f;
                    const TextureSample samp = perspectiveMips
                        ? mipAtInv(1.0f / std::max(z + dzdx * midOff, 1e-6f), u + dudx * midOff, v + dvdx * midOff)
                        : triangleTexture;
                    for (int k = first; k <= last; ++k) {
                        if (!(mask & (1u << k))) continue;
                        const float invZ = 1.0f / std::max(z + dzdx * float(k), 1e-6f);
                        uint16_t pixel = fetchTexel(samp, (u + dudx * float(k)) * invZ, (v + dvdx * float(k)) * invZ);
                        if constexpr (detail) {
                            if (varying) {
                                const int relX = x - left + k;
                                const int lv = mapped ? mapLight(relX)
                                                      : smooth ? (smoothStart + cornerStep * relX) : (255 << 16);
                                pixel = shaded(pixel, std::min(unsigned(lv) >> 16, 255u),
                                               ((minX + x + k) & 1) ? dOdd : dEven);
                            } else {
                                pixel = multiply(pixel, light0);
                            }
                            if (hazy) {
                                if (const unsigned level = unsigned(haze[zBuf[k] >> 6]) >> hazeShift) pixel = veil(pixel, level);
                            }
                        } else {
                            pixel = multiply(pixel, light0);
                        }
                        color[at + k] = opaque ? pixel : blend(color[at + k], pixel, matAlpha);
                        if (opaque) depth[at + k] = zBuf[k];
                        if constexpr (countPixels) ++stats.depthPasses;
                    }
                    x += spanLen;
                    at += spanLen;
                    z = zScan;
                    u += dudx * float(spanLen);
                    v += dvdx * float(spanLen);
                    continue;
                }

                const bool fullSpan = (mask == uint8_t((1u << spanLen) - 1));
                const bool carry = fullSpan && spanLen == 8 && x + 8 <= right &&
                                   std::abs(zScan - z) <= 0.10f * std::min(z, zScan);
                const int endOff = carry ? 8 : last;
                const float invZ0 = (first == 0 && cachedX == x) ? cachedInvZ : (1.0f / std::max(z0, 1e-6f));
                const float invZ1 = 1.0f / std::max(carry ? zScan : zLast, 1e-6f);
                if (carry) {
                    cachedInvZ = invZ1;
                    cachedX = x + 8;
                }

                const TextureSample sample = perspectiveMips
                    ? mipAtInv(0.5f * (invZ0 + invZ1),
                               u + dudx * (float(first + last) * 0.5f),
                               v + dvdx * (float(first + last) * 0.5f))
                    : triangleTexture;
                const TexInfo* tex = sample.fine;
                const float u0 = (u + dudx * float(first)) * invZ0 * tex->fWidth;
                const float v0 = (v + dvdx * float(first)) * invZ0 * tex->fHeight;
                const float invStep = invSpan[endOff - first];
                const float du = ((u + dudx * float(endOff)) * invZ1 * tex->fWidth - u0) * invStep;
                const float dv = ((v + dvdx * float(endOff)) * invZ1 * tex->fHeight - v0) * invStep;

                int lit = light0 << 16, lightStep = cornerStep;
                unsigned dCur = ((minX + x + first) & 1) ? dOdd : dEven;
                if constexpr (detail) {
                    if (mapped) {
                        const int relFirst = x - left + first;
                        lit = (first == 0 && cachedMapX == x) ? cachedMapLit : mapLight(relFirst);
                        if (fullSpan && spanLen == 8) {
                            const int nextLit = mapLight(x - left + 8);
                            cachedMapLit = nextLit;
                            cachedMapX = x + 8;
                            lightStep = (nextLit - lit) >> 3;
                        } else {
                            const int nextLit = mapLight(x - left + last + 1);
                            lightStep = int(float(nextLit - lit) * invSpan[last - first + 1]);
                        }
                    } else if (smooth) {
                        lit = smoothStart + cornerStep * (x - left + first);
                    }
                }

                uint16_t* cOut = color + at;
                uint16_t* dOut = depth.data() + at;
                if (tex->pot) {
                    const uint16_t* texPixels = tex->pixels;
                    const int maskW = tex->maskW, maskH = tex->maskH;
                    const uint8_t logW = tex->logW;
                    uint32_t uFix = toFixedFloor(u0, tex->invWidth, tex->fWidth);
                    uint32_t vFix = toFixedFloor(v0, tex->invHeight, tex->fHeight);
                    const uint32_t duFix = toFixedStep(du, tex->invWidth, tex->fWidth);
                    const uint32_t dvFix = toFixedStep(dv, tex->invHeight, tex->fHeight);
                    const uint16_t* cPixels = sample.coarse ? sample.coarse->pixels : nullptr;
                    const uint8_t cLogW = sample.coarse ? sample.coarse->logW : 0;
                    const unsigned mix = sample.mix;

                    if (opaque && !cPixels && fullSpan) {
                        if constexpr (!detail) {
                            for (int k = 0; k < spanLen; ++k, uFix += duFix, vFix += dvFix) {
                                const int tx = int(uFix >> 16) & maskW;
                                const int ty = int(vFix >> 16) & maskH;
                                cOut[k] = multiply(texPixels[(ty << logW) + tx], light0);
                                dOut[k] = zBuf[k];
                            }
                        } else {
                            for (int k = 0; k < spanLen; ++k, uFix += duFix, vFix += dvFix, lit += lightStep, dCur ^= dToggle) {
                                const int tx = int(uFix >> 16) & maskW;
                                const int ty = int(vFix >> 16) & maskH;
                                uint16_t pixel = texPixels[(ty << logW) + tx];
                                pixel = varying ? shaded(pixel, std::min(unsigned(lit) >> 16, 255u), dCur)
                                                : multiply(pixel, light0);
                                if (hazy) {
                                    if (const unsigned level = unsigned(haze[zBuf[k] >> 6]) >> hazeShift) pixel = veil(pixel, level);
                                }
                                cOut[k] = pixel;
                                dOut[k] = zBuf[k];
                            }
                        }
                        if constexpr (countPixels) stats.depthPasses += uint32_t(spanLen);
                    } else {
                        for (int k = first; k <= last; ++k, uFix += duFix, vFix += dvFix, lit += lightStep, dCur ^= dToggle) {
                            if (!fullSpan && !(mask & (1u << k))) continue;
                            const int tx = int(uFix >> 16) & maskW;
                            const int ty = int(vFix >> 16) & maskH;
                            uint16_t pixel = texPixels[(ty << logW) + tx];
                            if (cPixels) pixel = blend(pixel, cPixels[((ty >> 1) << cLogW) + (tx >> 1)], mix);
                            if constexpr (detail) {
                                pixel = varying ? shaded(pixel, std::min(unsigned(lit) >> 16, 255u), dCur)
                                                : multiply(pixel, light0);
                                if (hazy) {
                                    if (const unsigned level = unsigned(haze[zBuf[k] >> 6]) >> hazeShift) pixel = veil(pixel, level);
                                }
                            } else {
                                pixel = multiply(pixel, light0);
                            }
                            cOut[k] = opaque ? pixel : blend(cOut[k], pixel, matAlpha);
                            if (opaque) dOut[k] = zBuf[k];
                            if constexpr (countPixels) ++stats.depthPasses;
                        }
                    }
                } else {
                    const uint16_t* texPixels = tex->pixels;
                    const int texW = tex->width, texH = tex->height;
                    const bool halfSize = sample.coarse && texW == sample.coarse->width * 2 && texH == sample.coarse->height * 2;
                    float uCur = u0, vCur = v0;
                    const float cScaleU = sample.coarse ? sample.coarse->fWidth * tex->invWidth : 0.f;
                    const float cScaleV = sample.coarse ? sample.coarse->fHeight * tex->invHeight : 0.f;
                    for (int k = first; k <= last; ++k, uCur += du, vCur += dv, lit += lightStep, dCur ^= dToggle) {
                        if (!fullSpan && !(mask & (1u << k))) continue;
                        const int tx = texel(uCur, texW);
                        const int ty = texel(vCur, texH);
                        uint16_t pixel = texPixels[ty * texW + tx];
                        if (sample.coarse) {
                            const auto& coarse = *sample.coarse;
                            const int cx = halfSize ? (tx >> 1) : texel(uCur * cScaleU, coarse.width);
                            const int cy = halfSize ? (ty >> 1) : texel(vCur * cScaleV, coarse.height);
                            pixel = blend(pixel, coarse.pixels[cy * coarse.width + cx], sample.mix);
                        }
                        if constexpr (detail) {
                            pixel = varying ? shaded(pixel, std::min(unsigned(lit) >> 16, 255u), dCur)
                                            : multiply(pixel, light0);
                            if (hazy) {
                                if (const unsigned level = unsigned(haze[zBuf[k] >> 6]) >> hazeShift) pixel = veil(pixel, level);
                            }
                        } else {
                            pixel = multiply(pixel, light0);
                        }
                        cOut[k] = opaque ? pixel : blend(cOut[k], pixel, matAlpha);
                        if (opaque) dOut[k] = zBuf[k];
                        if constexpr (countPixels) ++stats.depthPasses;
                    }
                }

                x += spanLen;
                at += spanLen;
                z = zScan;
                u += dudx * float(spanLen);
                v += dvdx * float(spanLen);
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

    FOD_NOINLINE void draw(std::vector<Primitive>& batch, bool opaque) {
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
            const Vec3 center = (chunk.bounds.min + chunk.bounds.max) * 0.5f;
            const Vec3 extent = (chunk.bounds.max - chunk.bounds.min) * 0.5f;
            if (!view.sees(center, extent)) continue;
            const Vec3 offset = center - view.position;
            visible.emplace_back(dot(offset, offset), i);
        }
        std::sort(visible.begin(), visible.end());
        for (const auto& item : visible) {
            const auto& chunk = assets.world.chunks[item.second];
            if (countRenderWork && measure) ++stats.visibleChunks;
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
        if (!view.sees(bounds) || occluded(bounds)) return;
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
        const Bounds bounds{actor.position - Vec3{extent.x, .6f, extent.z}, actor.position + extent};
        if (!view.sees(bounds) || occluded(bounds)) return;
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
            // The grip, bore and muzzle match the gameplay rig.
            const Mesh& gunMesh = armored ? assets.alienHeavyRayGun : assets.alienRayGun;
            const Mesh& ringMesh = armored ? assets.alienHeavyRayRings : assets.alienRayRings;
            if (!gunMesh.triangles.empty()) {
                const auto rig = alienGunFrame(transform(alienGunSocket(assets, actor)),
                                               ma.gunYaw + fall * .25f, ma.gunPitch + fall * 1.2f, ma.variant);
                const Transform gunXform{rig.grip, rig.right, rig.up, rig.axis};
                submit(gunMesh, state, gunXform, MeshSpace::World, nullptr, 0, true, nullptr, uniform);

                if (!ringMesh.triangles.empty()) {
                    const float charge = ma.gunCharge;
                    const float spin = float(state.realTime) * 3.4f + actor.position.x * .5f
                                     + charge * (4.f + 8.f * charge) + ma.gunRecoil * 5.f;
                    const float cs = std::cos(spin), sn = std::sin(spin);
                    const float pulseZ = (ma.gunCharge > 0.05f ? std::sin(float(state.realTime) * 18.f) * 0.025f * ma.gunCharge : 0.f)
                                       - 0.04f * ma.gunRecoil;
                    Transform ringXform = gunXform;
                    ringXform.origin = gunXform({0, rayBoreHeight, pulseZ});
                    ringXform.x = gunXform.x * cs + gunXform.y * sn;
                    ringXform.y = gunXform.y * cs - gunXform.x * sn;
                    submit(ringMesh, state, ringXform, MeshSpace::World, nullptr, 0, true, nullptr, uniform);
                }

                if (!ma.dead && (ma.gunCharge > .04f || ma.gunRecoil > .08f) && rayGlowCount < rayGlows.size()) {
                    rayGlows[rayGlowCount++] = {
                        rig.muzzle(ma.variant), rig.axis / s, rig.right / s, rig.up / s,
                        ma.gunCharge, ma.gunRecoil, s * rayLength(ma.variant),
                        ma.overcharging || (ma.chargedShot && ma.gunRecoil > .08f)
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
            if (ma.alerted && !ma.dead && (ma.gunCharge > .04f || ma.gunRecoil > .08f) &&
                rayGlowCount < rayGlows.size()) {
                rayGlows[rayGlowCount++] = {transform({0, 0, .52f}), transform.z, transform.x, transform.y,
                                            ma.gunCharge, ma.gunRecoil, .75f, false};
            }
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
        if (muzzleGlowCount < muzzleGlows.size()) {
            muzzleGlows[muzzleGlowCount++] = {center, .50f, rgb(255, 206, 176), rgb(255, 58, 108), pulse, true};
        }
    }

    // Nora's emitter kit at Timberline, and the dish console in the east room at Briar House.
    void drawOperationEquipment(const Snapshot& state) {
        const auto& m = *state.mission;
        const auto& op = m.operation;
        if (m.phase != MissionPhase::Operation || !op.kitVisible || assets.relayTerminal.chunks.empty()) return;
        const Vec3 base = op.kit;
        const auto bounds = assets.relayTerminal.chunks.front().bounds;
        if (!view.sees({bounds.min + base, bounds.max + base})) return;
        meshLight = standing(base);
        submit(assets.relayTerminal, state, orientation(base, 0, 0, 0), MeshSpace::World);
        if (muzzleGlowCount >= muzzleGlows.size()) return;
        const float time = float(state.realTime);
        const bool jammed = op.jammer >= 0;
        const float pulse = .65f + .25f * std::sin(time * (jammed ? 18.f : op.painting ? 9.f : 4.8f));
        muzzleGlows[muzzleGlowCount++] = {base + Vec3{0, 1.19f, 0}, .42f, rgb(246, 250, 237),
                                          jammed ? rgb(255, 57, 185) : rgb(57, 220, 255), pulse, jammed};
    }

    // A figure in shade dims with the ground under it, yet stays easy to pick out.
    unsigned standing(Vec3 position) const { return 128 + light.at(position.x, position.z) / 2; }

    // Darken the ground under a figure. The disc fades from a dark core to nothing at its rim.
    void contactShadow(Vec3 feet, float radius) {
        if (!view.sees({feet - Vec3{radius, 0, radius}, feet + Vec3{radius, 0.1f, radius}})) return;
        constexpr int segments = 10;
        constexpr float core = 0.45f, dark = 150, clear = 255;
        const Vec3 center = feet + Vec3{0, 0.07f, 0};
        static const auto directions = [] {
            std::array<Vec3, segments + 1> ring;
            for (int i = 0; i <= segments; ++i) {
                const float angle = i * (2 * pi / segments);
                ring[i] = {std::cos(angle), 0, std::sin(angle)};
            }
            return ring;
        }();
        const auto corner = [&](int i, float reach, float level) {
            return LitVertex{{view.transform(center + directions[i] * (radius * reach)), 0, 0}, level};
        };
        const auto fill = [&](const LitVertex& a, const LitVertex& b, const LitVertex& c) {
            const LitVertex corners[]{a, b, c};
            clip(corners, [&](const LitVertex& x, const LitVertex& y, const LitVertex& z) {
                Primitive primitive{};
                if (project(primitive, x, y, z)) raster<Fill::Shadow, true>(primitive, false);
            });
        };
        const LitVertex hub{{view.transform(center), 0, 0}, dark};
        auto inner = corner(0, core, dark), outer = corner(0, 1, clear);
        for (int i = 0; i < segments; ++i) {
            const auto innerNext = corner(i + 1, core, dark), outerNext = corner(i + 1, 1, clear);
            fill(hub, inner, innerNext);
            fill(inner, outer, outerNext);
            fill(inner, outerNext, innerNext);
            inner = innerNext;
            outer = outerNext;
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
        while (lastVisible && frames[lastVisible].scope >= 0.6f) --lastVisible;
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
            submit(mesh, state, transform, MeshSpace::View, aimOffset, fraction, false, nullptr, nullptr, &motion, false, true);
        } else {
            submit(mesh, state, transform, MeshSpace::View, pose, amount, false, nullptr, nullptr, nullptr, false, true);
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
        if (hiZBuilt && !hiZAnyOpen) {
            recordRasterTime(start);
            return;
        }
        const Vec3 eye = view.position;
        const Vec3 savedNeg = view.negOrigin;
        view.position = {};
        view.negOrigin = {};
        constexpr float pad = 1.004f;
        for (size_t face = 0; face < assets.sky.size(); ++face) {
            const auto& [normal, right, up] = skyAxes[face];
            const Vertex corners[] = {
                {view.transform(normal - right * pad - up * pad), -0.002f, -0.002f},
                {view.transform(normal + right * pad - up * pad),  1.002f, -0.002f},
                {view.transform(normal + right * pad + up * pad),  1.002f,  1.002f},
                {view.transform(normal - right * pad + up * pad), -0.002f,  1.002f},
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
        view.negOrigin = savedNeg;
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

    void drawEnergyOrb(Vec3 position, float radius, float power, EnergyColors palette, unsigned phase = 0) {
        const Vec3 center = view.transform(position);
        if (center.z <= view.near + .025f || power <= .01f) return;
        const Projected p = view.project({center, 0, 0});
        const float projectedRadius = radius * view.focal * p.depth;
        const float r = std::clamp(projectedRadius, 1.35f, std::max(1.35f, float(view.width) * .12f));
        if (p.x + r < 0 || p.x - r >= view.width || p.y + r < 0 || p.y - r >= view.height) return;
        const int x0 = std::max(0, fastFloor(p.x - r)), x1 = std::min(view.width - 1, fastCeil(p.x + r));
        const int y0 = std::max(0, fastFloor(p.y - r)), y1 = std::min(view.height - 1, fastCeil(p.y + r));
        const float invZ = 1.f / std::max(view.near, center.z - radius * .16f);
        const uint16_t z = enhanced ? encodedDepth(invZ) : uint16_t(std::min(65535.f, invZ * view.near * 65534.f));
        const float invR = 1.f / r;
        const float strength = std::min(1.f, .42f + projectedRadius * .40f) * power;
        for (int y = y0; y <= y1; ++y) {
            const float dy = (y + .5f - p.y) * invR;
            const unsigned sy = unsigned(int(dy * 22.f) + 32);
            for (int x = x0; x <= x1; ++x) {
                const int at = y * view.width + x;
                if (z + 2 < depth[at]) continue;
                const float dx = (x + .5f - p.x) * invR;
                const float d2 = dx * dx + dy * dy;
                if (d2 >= 1.f) continue;
                const float edge = 1.f - d2;
                float coronas = edge * edge;
                if (phase && projectedRadius > 2.5f) {
                    const unsigned sx = unsigned(int(dx * 22.f) + 32);
                    const unsigned grain = skyDither[((sy + (phase >> 3)) & 63u) * 64 + ((sx + (phase >> 2)) & 63u)];
                    coronas *= .76f + .12f * grain;
                }
                const float core = std::max(0.f, 1.f - d2 * 7.6f);
                const unsigned halo = unsigned(std::min(255.f, 137.f * coronas * strength));
                const unsigned white = unsigned(std::min(255.f, 236.f * core * core * strength));
                color[at] = addLight(addLight(color[at], palette.halo, halo), palette.core, white);
            }
        }
    }

    void drawRayCharge(const RayGlow& ray, float time, size_t index) {
        const EnergyColors palette = ray.hot ? rayHot : rayBlue;
        const float flash = ray.recoil * ray.recoil * ray.recoil * ray.recoil;
        const float charge = ray.charge * ray.charge;
        const Vec3 muzzle = ray.muzzle + ray.axis * (.045f * ray.scale);
        const float extent = ray.scale * (.13f + .24f * charge + .40f * flash);
        const Vec3 margin{extent, extent, extent};
        if (!view.sees({muzzle - margin, muzzle + margin})) return;
        const float clock = time * (ray.hot ? 13.f : 9.f) + float(index) * 2.39996f;
        const float pulse = .9f + .1f * std::sin(clock * 2.4f);
        drawEnergyOrb(muzzle, extent, (.28f * ray.charge + charge * .84f + flash * 1.30f) * pulse,
                      palette, 1u + unsigned(time * 96.f));
        if (charge > .12f) {
            for (int i = 0; i < 3; ++i) {
                const float angle = clock + i * (2.f * pi / 3.f);
                const float spread = ray.scale * (.16f + .16f * charge);
                const Vec3 radial = ray.right * std::cos(angle) + ray.up * std::sin(angle);
                const Vec3 flank = ray.right * -std::sin(angle) + ray.up * std::cos(angle);
                const Vec3 contact = muzzle + radial * spread - ray.axis * (.27f * ray.scale);
                const Vec3 elbow = muzzle + (radial * .60f + flank * .24f) * spread;
                drawViewTracer(view.transform(contact), view.transform(elbow), palette.halo, unsigned(174.f * charge));
                drawViewTracer(view.transform(elbow), view.transform(muzzle), palette.core, unsigned(214.f * charge));
                drawEnergyOrb(contact, .085f * ray.scale, charge, palette);
            }
        }
        if (flash > .12f) {
            const Vec3 tip = muzzle + ray.axis * (.48f * flash * ray.scale);
            drawEnergyOrb(tip, extent * .61f, flash * .84f, palette);
            for (const Vec3 side : {ray.right, ray.up}) {
                const Vec3 spread = side * (extent * .75f);
                drawViewTracer(view.transform(muzzle - spread), view.transform(muzzle + spread), palette.core,
                               unsigned(172.f * flash));
            }
        }
    }

    void drawPlasmaBolt(const PlasmaCloud& bolt, float time, size_t index) {
        const EnergyColors palette = bolt.charged ? rayHot : rayBlue;
        const float r = bolt.radius;
        const Vec3 margin{r * 9.f, r * 9.f, r * 9.f};
        if (!view.sees({bolt.position - margin, bolt.position + margin})) return;
        const Vec3 lateral = normalized(cross(std::abs(bolt.direction.y) > .85f ? Vec3{1, 0, 0} : Vec3{0, 1, 0},
                                              bolt.direction));
        const Vec3 normal = cross(bolt.direction, lateral);
        const float clock = time * 22.f + float(index) * 2.39996f;
        const unsigned phase = 1u + unsigned(time * 96.f) + unsigned(index) * 17u;
        const float wake = std::min(bolt.traveled, r * (bolt.charged ? 9.f : 6.5f));
        if (wake > r * 1.8f) {
            for (int i = 5; i >= 1; --i) {
                const float t = float(i) / 6.f;
                drawEnergyOrb(bolt.position - bolt.direction * (wake * t), r * (1.48f - .55f * t),
                              (1.f - t) * .44f, palette, phase + unsigned(i) * 7u);
            }
        }
        drawEnergyOrb(bolt.position, 2.15f * r, bolt.charged ? 1.40f : 1.12f, palette, phase);
        if (wake <= r * 1.8f) return;
        const Vec3 tail = bolt.position - bolt.direction * wake;
        drawViewTracer(view.transform(tail), view.transform(bolt.position), palette.core, bolt.charged ? 192 : 135);
        for (const float side : {-1.f, 1.f}) {
            const Vec3 offset = (lateral * std::cos(clock) + normal * std::sin(clock)) * (r * side);
            const Vec3 spark = bolt.position - bolt.direction * (wake * .34f) + offset;
            const Vec3 streak = tail - offset * .65f;
            drawViewTracer(view.transform(streak), view.transform(spark), palette.halo, 144);
            drawEnergyOrb(spark, r * .53f, .81f, palette);
        }
    }

    void drawPlasmaImpact(const MissionTracer& shot, float time) {
        const bool charged = shot.color == rayHot.halo;
        const EnergyColors palette = charged ? rayHot : rayBlue;
        const float life = std::min(1.f, shot.time / (charged ? 1.35f : 1.f));
        const float age = 1.f - life;
        const float spread = (charged ? 1.10f : .72f) * age;
        const Vec3 recoil = shot.end - shot.start;
        const Vec3 lateral = normalized(cross(std::abs(recoil.y) > .85f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}, recoil));
        const Vec3 normal = cross(recoil, lateral);
        const Vec3 center = shot.start + recoil * (.105f * age);
        drawEnergyOrb(center, .28f + spread * .59f, life * 1.36f, palette, 1u + unsigned(time * 90.f));
        const int rays = charged ? 10 : 7;
        for (int i = 0; i < rays; ++i) {
            const float angle = i * 2.39996f + shot.start.x * 1.13f + shot.start.y * .82f;
            const Vec3 direction = lateral * std::cos(angle) + normal * std::sin(angle);
            const Vec3 drift = (direction * (.58f + .13f * (i % 4)) + recoil * (.18f + .09f * (i % 3))) * spread
                             - Vec3{0, .20f * age * age, 0};
            drawViewTracer(view.transform(center + drift * .68f), view.transform(center + drift),
                           i % 3 ? palette.halo : palette.core, unsigned(life * 191.f));
            drawEnergyOrb(center + drift, .061f + .03f * life, .61f * life, palette);
        }
    }

    void drawMissionTracer(const MissionTracer& shot, float realTime) {
        if (shot.time <= 0) return;
        if (shot.style == TracerStyle::Ballistic) {
            drawTracer({shot.start, shot.end, shot.time}, shot.color);
            return;
        }
        if (shot.style == TracerStyle::PlasmaImpact) {
            drawPlasmaImpact(shot, realTime);
            return;
        }
        if (shot.style == TracerStyle::DropBeam) {
            const Vec3 low = shot.start.y < shot.end.y ? shot.start : shot.end;
            const Vec3 high = shot.start.y < shot.end.y ? shot.end : shot.start;
            const EnergyColors palette{shot.color, blend(shot.color, rgb(255, 249, 232), 180)};
            const float life = std::min(1.f, shot.time);
            drawTractorBeam({high, low, .23f, .84f, palette, life}, realTime);
            if (high.y - low.y < 4.f) drawEnergyOrb(low, .48f + .38f * (1.f - life), life, palette);
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
        const bool scan = shot.style == TracerStyle::Scanner;
        const uint16_t coreColor = link ? rgb(196, 255, 255) : rgb(236, 255, 255);
        const float baseWidth = scan ? 1.1f : 1.6f;
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
            const int halfW = std::clamp(int(baseWidth * resScale * persp * (0.75f + 0.35f * pulse)), 1, 7);
            const unsigned outerA = unsigned(std::clamp(int((scan ? 90.f : 175.f) * life * pulse), 28, 224));
            const unsigned coreA = unsigned(std::clamp(int((scan ? 130.f : 242.f) * life), 56, 248));

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
                    const unsigned alpha = outerA * remain / span;
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
        const EnergyColors palette = actor.variant > 0 ? tractorJammed : tractorCyan;
        const float strength = life * (.90f + .10f * std::sin(age * 24.f));
        drawTractorBeam({feet + Vec3{0, 21.f, 0}, feet, .22f, .85f + age * .30f, palette, strength}, time);
        if (p.z < 34.f) {
            drawObjectiveRing(feet, .41f + age * 1.5f, palette.core, unsigned(life * 171.f), false);
            drawObjectiveRing(feet, .24f + age * 1.0f, palette.halo, unsigned(life * 132.f), true);
        }
    }

    void drawShieldGlows(float time) {
        constexpr float hexHeight = 1.73205f;
        for (size_t i = 0; i < shieldGlowCount; ++i) {
            const auto& shield = shieldGlows[i];
            const Vec3 center = view.transform(shield.pos);
            if (center.z < view.near + .4f) continue;
            const auto p = view.project({center, 0, 0});
            const float rx = std::min(view.width * .20f, shield.radius * view.focal * p.depth);
            const float ry = std::min(view.height * .30f, shield.height * view.focal * p.depth);
            if (rx < 3.f || ry < 4.f || p.x + rx < 0 || p.x - rx >= view.width ||
                p.y + ry < 0 || p.y - ry >= view.height) continue;
            const int x0 = std::max(0, fastFloor(p.x - rx)), x1 = std::min(view.width - 1, fastCeil(p.x + rx));
            const int y0 = std::max(0, fastFloor(p.y - ry)), y1 = std::min(view.height - 1, fastCeil(p.y + ry));
            const float invX = 1.f / rx, invY = 1.f / ry;
            const float scanY = 1.f - 2.f * unitCycle(time * .82f + float(i) * .31f);
            const float shimmer = shield.strength * (.90f + .10f * std::sin(time * 10.f + float(i) * 2.1f));
            const uint16_t white = blend(shield.color, rgb(226, 249, 255), 175);
            for (int y = y0; y <= y1; ++y) {
                const float v = (float(y) + .5f - p.y) * invY;
                const float scan = std::max(0.f, 1.f - std::abs(v - scanY) * 12.f);
                for (int x = x0; x <= x1; ++x) {
                    const float u = (float(x) + .5f - p.x) * invX;
                    const float sphere = 1.f - u * u - v * v;
                    if (sphere <= 0.f) continue;
                    const float front = std::sqrt(sphere);
                    const float inverseZ = 1.f / std::max(view.near, center.z - shield.radius * front);
                    const uint16_t z = enhanced ? encodedDepth(inverseZ) : uint16_t(inverseZ * view.near * 65534.f);
                    const int at = y * view.width + x;
                    if (z + 2 < depth[at]) continue;

                    const float edge = 1.f - front;
                    const float coverage = std::min(1.f, sphere * rx * .65f);
                    float cells = 0.f;
                    if (rx >= 13.f) {
                        // Two offset lattices give the nearest center of the hexagonal cells.
                        const float tx = u * 4.6f + 5.f, ty = v * 5.1f + 6.f + .37f * front;
                        const float ax = unitCycle(tx + .5f) - .5f;
                        const float ay = (unitCycle(ty / hexHeight + .5f) - .5f) * hexHeight;
                        const float bx = unitCycle(tx) - .5f;
                        const float by = (unitCycle(ty / hexHeight) - .5f) * hexHeight;
                        const bool first = ax * ax + ay * ay < bx * bx + by * by;
                        const float hx = std::abs(first ? ax : bx), hy = std::abs(first ? ay : by);
                        const float side = std::max(hx, .5f * hx + .866025f * hy);
                        cells = std::max(0.f, (side - .435f) * 15.f) * (16.f + 25.f * scan);
                    }
                    const unsigned halo = unsigned((8.f + 128.f * edge * edge + cells + 23.f * scan) * shimmer * coverage);
                    const unsigned rim = unsigned(109.f * edge * edge * edge * shimmer * coverage);
                    color[at] = addLight(addLight(color[at], shield.color, halo), white, rim);
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
        const int x0 = std::max(0, fastCeil(left)), x1 = std::min(view.width, int(p.x + p.width));
        const int y0 = std::max(0, fastCeil(top)), y1 = std::min(view.height, int(p.y + p.height));
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

    void drawObjectiveRing(Vec3 center, float radius, uint16_t tint, unsigned alpha, bool dashed) {
        if (!view.sees({center - Vec3{radius, 0, radius}, center + Vec3{radius, .15f, radius}})) return;
        constexpr float c = .92388f, s = .38268f, d = .70711f;
        constexpr Vec3 loop[]{{1, 0, 0}, {c, 0, s}, {d, 0, d}, {s, 0, c}, {0, 0, 1}, {-s, 0, c},
                              {-d, 0, d}, {-c, 0, s}, {-1, 0, 0}, {-c, 0, -s}, {-d, 0, -d}, {-s, 0, -c},
                              {0, 0, -1}, {s, 0, -c}, {d, 0, -d}, {c, 0, -s}};
        const Vec3 base = center + Vec3{0, .075f, 0};
        for (size_t i = 0; i < std::size(loop); ++i) {
            const auto from = base + loop[i] * radius;
            const auto to = base + loop[(i + 1) % std::size(loop)] * radius;
            drawViewTracer(view.transform(from), view.transform(dashed ? from + (to - from) * .57f : to), tint, alpha);
        }
    }

    Transform fleetTransform(const MissionSnapshot& mission, size_t i, float time) const {
        const float phase = time * (.45f + float(i) * .13f) + float(i) * 1.7f;
        const Vec3 drift{0, std::sin(phase) * (i == 0 ? .55f : .36f), 0};
        return orientation(fleetHulls[i].center + mission.fleetShift[i] + drift,
                           .085f * std::sin(phase * .53f), .024f * std::cos(phase), .030f * std::sin(phase));
    }

    void drawFleetLights(const MissionSnapshot& mission, float time) {
        for (size_t i = 0; i < fleetHulls.size(); ++i) {
            const Mesh& mesh = assets.alienFleet[i];
            if (mesh.chunks.empty()) continue;
            const auto pose = fleetTransform(mission, i, time);
            const auto& ship = fleetHulls[i];
            const Vec3 span = ship.boundsExtent();
            if (!view.sees({pose.origin - span, pose.origin + span})) continue;
            const EnergyColors palette = i == 2 ? tractorViolet : i == 1 ? tractorPulled : tractorCyan;
            const float pulse = .74f + .22f * std::sin(time * 3.4f + float(i) * 1.3f);
            drawEnergyOrb(pose(ship.emitter()), ship.radius * .23f, pulse, palette);
            for (int k = 0; k < 6; ++k) {
                const float angle = k * (pi / 3.f) + pi / 14.f;
                const float brightness = .38f + .46f * std::max(0.f, std::sin(time * 3.9f - angle));
                const Vec3 vent{std::cos(angle) * ship.radius * .46f, -.35f * ship.height,
                                std::sin(angle) * ship.radius * .46f};
                drawEnergyOrb(pose(vent), ship.radius * .069f, brightness, palette);
            }
        }
    }

    struct TractorBeam {
        Vec3 source, foot;
        float aperture, spread;
        EnergyColors palette;
        float power;
    };

    void drawTractorBeam(const TractorBeam& beam, float time) {
        if (beam.power < .025f) return;
        const float poolRadius = beam.spread * 1.28f;
        const Vec3 extent{poolRadius, poolRadius, poolRadius};
        const Vec3 lo{std::min(beam.source.x, beam.foot.x), std::min(beam.source.y, beam.foot.y),
                      std::min(beam.source.z, beam.foot.z)};
        const Vec3 hi{std::max(beam.source.x, beam.foot.x), std::max(beam.source.y, beam.foot.y),
                      std::max(beam.source.z, beam.foot.z)};
        if (!view.sees({lo - extent, hi + extent})) return;

        const Vec3 from = view.transform(beam.source), to = view.transform(beam.foot);
        const Vec3 lengthwise = beam.foot - beam.source;
        const float span = length(to - from);
        if (span < .25f) return;
        const Vec3 axis = (to - from) / span;
        const Vec3 forward = lengthwise / span;
        const Vec3 rightward = normalized(cross(forward, std::abs(forward.y) > .9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
        const Vec3 sideways = cross(forward, rightward);
        const Vec3 grainAxis = rightward * 2.8f + sideways * 1.6f;
        const Vec3 grainView{dot(grainAxis, view.right), dot(grainAxis, view.up), dot(grainAxis, view.forward)};
        const float slope = (beam.spread - beam.aperture) / span;
        const Vec3 eye = from * -1.f;
        const float axialEye = dot(eye, axis);
        const float eyeRadius = beam.aperture + slope * axialEye;
        const float c = dot(eye, eye) - axialEye * axialEye - eyeRadius * eyeRadius;
        const Vec3 bVector = eye - axis * (axialEye + eyeRadius * slope);
        const float slopeSq = 1.f + slope * slope;

        // Project the endpoint discs' enclosing boxes to limit per-pixel work.
        const Vec3 radialExtent{std::sqrt(std::max(0.f, 1.f - axis.x * axis.x)),
                                std::sqrt(std::max(0.f, 1.f - axis.y * axis.y)),
                                std::sqrt(std::max(0.f, 1.f - axis.z * axis.z))};
        const auto bound = [&](Vec3 center, Vec3 radius, float& left, float& top, float& right, float& bottom) {
            for (const float z : {std::max(view.near, center.z - radius.z), std::max(view.near, center.z + radius.z)}) {
                const auto min = view.project({{center.x - radius.x, center.y + radius.y, z}, 0, 0});
                const auto max = view.project({{center.x + radius.x, center.y - radius.y, z}, 0, 0});
                left = std::min(left, min.x);
                right = std::max(right, max.x);
                top = std::min(top, min.y);
                bottom = std::max(bottom, max.y);
            }
        };
        float left = float(view.width), top = float(view.height), right = 0, bottom = 0;
        bound(from, radialExtent * beam.aperture, left, top, right, bottom);
        // Ground glow includes low objects and rough ground inside the footprint.
        bound(to, {poolRadius, poolRadius, poolRadius}, left, top, right, bottom);
        const int x0 = int(std::clamp(left, 0.f, float(view.width)));
        const int y0 = int(std::clamp(top, 0.f, float(view.height)));
        const int x1 = fastCeil(std::clamp(right, 0.f, float(view.width)));
        const int y1 = fastCeil(std::clamp(bottom, 0.f, float(view.height)));
        if (x0 >= x1 || y0 >= y1) return;

        const Vec3 vertical{view.right.y, view.up.y, view.forward.y};
        const float invFocal = 1.f / view.focal, invSpan = 1.f / span;
        const float poolInv = 1.f / (poolRadius * poolRadius);
        const float searchNear = std::max(view.near, std::min(from.z - beam.aperture, to.z - poolRadius));
        const float searchFar = std::max(from.z + beam.aperture, to.z + poolRadius);
        const auto pulse = [](float phase) { return 1.f - std::abs(2.f * unitCycle(phase) - 1.f); };

        for (int y = y0; y < y1; ++y) {
            const float dy = (view.height * .5f - y - .5f) * invFocal;
            for (int x = x0; x < x1; ++x) {
                const int at = y * view.width + x;
                const uint16_t surface = depth[at];
                const float sceneZ = !surface ? searchFar :
                                     enhanced ? decodedDepth(surface) : view.near * 65534.f / surface;
                if (sceneZ < searchNear) continue;
                const Vec3 ray{(x + .5f - view.width * .5f) * invFocal, dy, 1.f};

                float halo = 0, core = 0;
                if (surface && std::abs(sceneZ - to.z) < poolRadius) {
                    const Vec3 offset = ray * sceneZ - to;
                    const float height = std::abs(dot(offset, vertical));
                    const float d2 = (dot(offset, offset) - height * height) * poolInv;
                    if (height < .60f && d2 < 1.f) {
                        const float edge = 1.f - d2;
                        const float caustic = pulse(time * 1.19f + d2 * 2.6f);
                        halo = edge * edge * (28.f + 102.f * caustic * caustic) * (1.f - height / .60f);
                    }
                }

                // Clip the view ray against the axial caps before solving the tapered sides.
                const float axialRay = dot(ray, axis);
                float entry = searchNear, exit = std::min(searchFar, sceneZ);
                if (std::abs(axialRay) > .00001f) {
                    const float cap0 = -axialEye / axialRay, cap1 = (span - axialEye) / axialRay;
                    entry = std::max(entry, std::min(cap0, cap1));
                    exit = std::min(exit, std::max(cap0, cap1));
                } else if (axialEye < 0 || axialEye > span) {
                    exit = entry;
                }
                if (entry < exit) {
                    const float a = dot(ray, ray) - slopeSq * axialRay * axialRay;
                    const float b = dot(bVector, ray);
                    if (std::abs(a) < .00001f) {
                        if (b > .00001f) exit = std::min(exit, -c / (2.f * b));
                        else if (b < -.00001f) entry = std::max(entry, -c / (2.f * b));
                        else if (c > 0.f) exit = entry;
                    } else if (const float disc = b * b - a * c; disc > 0.f) {
                        const float root = std::sqrt(disc);
                        const float z0 = (-b - root) / a, z1 = (-b + root) / a;
                        if (a > 0.f) {
                            entry = std::max(entry, z0);
                            exit = std::min(exit, z1);
                        } else if (entry <= z1) {
                            exit = std::min(exit, z1);
                        } else {
                            entry = std::max(entry, z0);
                        }
                    } else if (a > 0.f) {
                        exit = entry;
                    }
                    if (entry < exit) {
                        const float z = (entry + exit) * .5f;
                        const float axial = axialEye + z * axialRay;
                        const float t = std::clamp(axial * invSpan, 0.f, 1.f);
                        const float radius = beam.aperture + slope * axial;
                        const float chord = std::min(1.f, (exit - entry) * .59f / radius);
                        const Vec3 offset = (ray * z - (from + axis * axial)) / radius;
                        const float inner = std::max(0.f, 1.f - dot(offset, offset) * 3.8f);
                        const float wave = pulse(time * .74f + t * 4.4f);
                        const float stria = pulse(4.f + time * .31f + t * 2.f + dot(offset, grainView));
                        const float fade = .66f + .34f * std::min(1.f, (1.f - t) * 7.f);
                        halo += chord * fade * (52.f + wave * wave * 75.f + 22.f * stria);
                        core = chord * fade * inner * inner * (31.f + 56.f * wave * wave);
                    }
                }
                if (halo > 1.f) {
                    const unsigned glow = unsigned(std::min(255.f, halo * beam.power));
                    const unsigned hot = unsigned(std::min(255.f, core * beam.power));
                    color[at] = addLight(addLight(color[at], beam.palette.halo, glow), beam.palette.core, hot);
                }
            }
        }

        // Small illuminated particles lift through the volume, leaving short streaks behind.
        const int motes = beam.spread > 2.f ? 18 : 8;
        for (int i = 0; i < motes; ++i) {
            const float t = 1.f - unitCycle(time * (.24f + .033f * (i % 4)) + float(i) * .618034f);
            const float angle = time * (.8f + .17f * (i % 3)) + i * 2.39996f;
            const float spread = (beam.aperture + (beam.spread - beam.aperture) * t) * (.23f + .17f * (i % 4));
            const Vec3 mid = beam.source + lengthwise * t +
                             (rightward * std::cos(angle) + sideways * std::sin(angle)) * spread;
            const float amount = beam.power * std::min(1.f, std::min(t, 1.f - t) * 9.f);
            drawEnergyOrb(mid, .05f + beam.spread * .023f, amount * .62f, beam.palette);
            drawViewTracer(view.transform(mid), view.transform(mid + lengthwise * std::min(.018f, 1.f - t)),
                           i % 3 ? beam.palette.halo : beam.palette.core, unsigned(amount * 125.f));
        }
        if (beam.spread > 2.f) {
            // Gaps in the traveling rings leave room for the brighter column behind them.
            constexpr int sides = 16;
            for (int ring = 0; ring < 3; ++ring) {
                const float t = unitCycle(time * .48f + float(ring) / 3.f);
                const float radius = (beam.aperture + (beam.spread - beam.aperture) * t) * .71f;
                const Vec3 middle = beam.source + lengthwise * t;
                const unsigned alpha = unsigned(beam.power * 100.f * std::min(1.f, std::min(t, 1.f - t) * 6.f));
                const auto point = [&](int side) {
                    const float angle = side * (2.f * pi / sides) + time * .86f + ring * .9f;
                    return view.transform(middle + (rightward * std::cos(angle) + sideways * std::sin(angle)) * radius);
                };
                Vec3 from = point(0);
                for (int side = 1; side <= sides; ++side) {
                    const Vec3 to = point(side);
                    if (side % 4) drawViewTracer(from, to, beam.palette.core, alpha);
                    from = to;
                }
            }
        }
        drawEnergyOrb(beam.source, beam.aperture * 1.75f, beam.power, beam.palette);
        drawEnergyOrb(beam.foot + Vec3{0, .14f, 0}, beam.spread * .36f, beam.power * .71f, beam.palette);
    }

    void drawOperationEffects(const Snapshot& state) {
        const auto& m = *state.mission;
        const auto& op = m.operation;
        const float time = float(state.realTime);
        const Vec3 flagshipEmitter = fleetTransform(m, 0, time)(fleetHulls[0].emitter());
        const bool operation = m.phase == MissionPhase::Operation;
        const bool steering = operation && m.chapter == MissionChapter::Array &&
                              (op.stage == OperationStage::Steer || op.stage == OperationStage::Commit);
        if (m.flagshipBeam) {
            const EnergyColors palette = steering && op.jammer >= 0 ? tractorJammed :
                                         steering && op.pulled ? tractorPulled : tractorCyan;
            const Vec3 foot = steering ? Vec3{op.ship.x, .06f, op.ship.z} :
                                        Vec3{flagshipEmitter.x, .06f, flagshipEmitter.z};
            const float power = !steering ? .77f : op.stage == OperationStage::Commit ?
                                .92f * std::clamp(1.f + m.fleetShift[0].y * .10f, 0.f, 1.f) : .92f;
            drawTractorBeam({flagshipEmitter, foot, 1.43f, 4.15f, palette, power}, time);
            if (steering) drawObjectiveRing(foot, 3.2f, palette.halo, unsigned(163.f * power), false);
        }
        // Nora's dish: a flash on each pulse, and a live link to the mothership while it is steered.
        if (m.dishPulse > 0.f || m.dishLink) {
            const Vec3 focus = dishFocus();
            const float glow = m.dishLink ? .72f + .2f * std::sin(time * 11.f) : m.dishPulse;
            drawGlowDisc(focus, .55f + .5f * m.dishPulse, rgb(255, 250, 236), rgb(255, 214, 150), glow, true);
            if (m.dishLink) {
                drawMissionTracer({focus, flagshipEmitter, .55f + .25f * std::sin(time * 7.f), rgb(255, 214, 150),
                                   TracerStyle::ShieldLink}, time);
            }
        }
        if (!operation) return;
        const auto pointer = [&](Vec3 at, bool valid) {
            const float pulse = .75f + .2f * std::sin(time * 12.f);
            drawObjectiveRing(at, .9f, valid ? rgb(236, 252, 255) : rgb(255, 96, 86), unsigned(220 * pulse), false);
        };
        if (m.chapter == MissionChapter::Timberline && op.stage == OperationStage::Call) {
            if (op.targetSet) {
                const EnergyColors palette = op.pulled ? tractorJammed : tractorViolet;
                const Vec3 belly = fleetTransform(m, 2, time)(fleetHulls[2].emitter());
                drawTractorBeam({belly, op.target + Vec3{0, .08f, 0}, .48f, 2.25f, palette, .84f}, time);
                drawObjectiveRing(op.target, 2.2f, palette.core, 185, true);
            }
            if (op.painting) pointer(op.aim, op.aimValid);
        }
        if (m.chapter == MissionChapter::Array && op.stage == OperationStage::Steer) {
            drawObjectiveRing(op.clearing, op.clearingRadius, op.clearingBurned ? rgb(120, 60, 44) : rgb(255, 96, 86),
                              op.clearingBurned ? 90 : 150, true);
            drawObjectiveRing(op.saddle, op.saddleRadius, rgb(110, 234, 146), unsigned(120 + 100 * op.lock), true);
            if (op.painting) pointer(op.aim, true);
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
        const auto wrap = [](float x, float span) { return x - float(fastFloor(x / span)) * span; };
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
        if (state.mission && !state.mission->cinematic) {
            if (state.mission->phase == MissionPhase::Extraction) {
                rainSquall(time, std::clamp((density - .855f) * 11.f, 0.f, 1.f), gust);
            } else if (state.mission->phase == MissionPhase::Boarding) {
                rainSquall(time, std::clamp((density - .855f) * 11.f, 0.f, 1.f), gust);
            } else if (state.mission->phase == MissionPhase::Operation &&
                       state.mission->chapter == MissionChapter::Array &&
                       state.mission->operation.stage >= OperationStage::Door) {
                rainSquall(time, .3f + .35f * state.mission->operation.lock, gust);
            }
        }
        recordRasterTime(start);
    }

    void weather(const Snapshot& state) {
        uint32_t seed = 48281;
        const float time = float(state.realTime);
        if (enhanced) {
            // Distant rain is short and faint. A few closer drops fall faster and draw longer streaks.
            struct Layer { int count, length; float speed; unsigned alpha; };
            static constexpr Layer layers[]{{36, 4, 630.f, 46}, {12, 9, 990.f, 76}};
            for (const auto& layer : layers) {
                const uint32_t offset = uint32_t(time * layer.speed);
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
        const uint32_t offset = uint32_t(time * 630.f);
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
        if (int(edgeRow.size()) < width) edgeRow.resize(width);
        uint16_t* prev = edgeRow.data();
        std::copy_n(color, width, prev);
        constexpr int edgeContrast = 8 * 32, sameSide = 3 * 32;
        for (int y = 1; y < view.height - 1; ++y) {
            auto* row = color + y * width;
            const auto* next = row + width;
            uint16_t left = row[0];
            prev[0] = left;
            int leftLuma = edgeLuma(left);
            uint16_t center = row[1];
            int midLuma = edgeLuma(center);
            for (int x = 1; x < width - 1; ++x) {
                const uint16_t right = row[x + 1];
                const int rightLuma = edgeLuma(right);
                const uint16_t top = prev[x];
                prev[x] = center;
                if (std::abs(leftLuma - rightLuma) >= edgeContrast) {
                    const uint16_t bottom = next[x];
                    const int topLuma = edgeLuma(top), botLuma = edgeLuma(bottom);
                    if (std::abs(topLuma - botLuma) >= edgeContrast) {
                        uint16_t t = top, b = bottom, l = left, r = right;
                        int tl = topLuma, bl = botLuma, ll = leftLuma, rl = rightLuma;
                        if (std::abs(tl - midLuma) < std::abs(bl - midLuma)) { std::swap(t, b); std::swap(tl, bl); }
                        if (std::abs(ll - midLuma) < std::abs(rl - midLuma)) { std::swap(l, r); std::swap(ll, rl); }
                        if (std::abs(bl - midLuma) <= sameSide && std::abs(rl - midLuma) <= sameSide &&
                            std::abs(tl - ll) <= sameSide) {
                            row[x] = blend(center, blend(t, l, 128), 64);
                        }
                    }
                }
                left = center;
                leftLuma = midLuma;
                center = right;
                midLuma = rightLuma;
            }
            prev[width - 1] = row[width - 1];
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
        rayGlowCount = 0;
        shieldGlowCount = 0;
        const bool missionActive = state.mission && state.mission->active;
        if (enhanced && invasionDusk) {
            updateDuskMist(missionActive ? state.mission->mistDensity : 0.74f, view.forward);
        }
        std::fill_n(depth.data(), width * height, 0);
        hiZBuilt = false;
        hiZAnyOpen = true;
        const bool hasSkyCube = enhanced && ((invasionDusk && !sunsetSky[0].empty()) || !assets.sky[0].empty());
        if (!hasSkyCube) sky(camera);
        drawWorld(state);
        const bool needPostWorldHiZ = !assets.propInstances.empty() || !title || hasSkyCube;
        if (needPostWorldHiZ) {
            if (!solid.empty()) draw(solid, true);
            buildHiZ();
        }
        for (const auto& inst : assets.propInstances) {
            const Mesh& propMesh = inst.kind == PropKind::Truck ? assets.shipBoxTruck : assets.shipBoxCar;
            if (propMesh.triangles.empty() || !view.sees(inst.bounds) || occluded(inst.bounds)) continue;
            auto transform = orientation(inst.position, inst.yaw, 0, 0);
            if (inst.scale != 1.f) {
                transform.x *= inst.scale;
                transform.y *= inst.scale;
                transform.z *= inst.scale;
            }
            meshLight = standing(inst.position);
            submit(propMesh, state, transform, MeshSpace::World, nullptr, 0, false, nullptr,
                   inst.paintMaterials.empty() ? nullptr : inst.paintMaterials.data(), nullptr, enhanced);
        }
        float rivalHeightScale = 1.f;
#ifdef FOD_LOCAL_DUEL
        rivalHeightScale = state.rivalHeightScale;
#endif
        if (!title) {
            if (missionActive) {
                for (const auto& ma : state.mission->actors) drawMissionActor(ma, state);
                drawUplink(state);
                drawOperationEquipment(state);
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
            for (size_t i = 0; i < assets.alienFleet.size(); ++i) {
                const Mesh& craft = assets.alienFleet[i];
                if (craft.chunks.empty()) continue;
                const auto pose = fleetTransform(*state.mission, i, float(state.realTime));
                const Vec3 span = fleetHulls[i].boundsExtent();
                if (view.sees({pose.origin - span, pose.origin + span})) {
                    submit(craft, state, pose, MeshSpace::World);
                }
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
            const float spin = unitCycle(float(state.realTime) * (23.0f / (2.f * pi))) * (2.f * pi);
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
            const auto effects = timestamp();
            const float time = float(state.realTime);
            drawOperationEffects(state);
            drawFleetLights(*state.mission, time);
            drawShieldGlows(time);
            for (const auto& actor : state.mission->actors) {
                if (actor.active && !actor.dead && actor.arrival > 0.f) drawArrival(actor, time);
            }
            for (size_t i = 0; i < muzzleGlowCount; ++i) {
                const auto& mg = muzzleGlows[i];
                drawGlowDisc(mg.pos, mg.radius, mg.core, mg.halo, mg.intensity, mg.star);
            }
            for (size_t i = 0; i < rayGlowCount; ++i) drawRayCharge(rayGlows[i], time, i);
            for (size_t i = 0; i < state.mission->plasma.size(); ++i) {
                const auto& cloud = state.mission->plasma[i];
                if (cloud.radius > 0.f) drawPlasmaBolt(cloud, time, i);
            }
            for (const auto& shot : state.mission->tracers) {
                if (shot.time > 0) drawMissionTracer(shot, time);
            }
            recordRasterTime(effects);
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
            hiZBuilt = false;
            // The weapon is posed in view space, so bring the daylight into that space with it.
            sun = {dot(light.sun, view.right), dot(light.sun, view.up), dot(light.sun, view.forward)};
            up = {view.right.y, view.up.y, view.forward.y};
            meshLight = standing(camera.position);
            Camera firstPerson{{}, 0, 0, state.camera.fov};
            view = makeView(firstPerson, width, height, 0.012f);
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
#ifdef ESP_PLATFORM
bool Renderer::depthInInternalRam() const { return esp_ptr_internal(impl_->depth.data()); }
#endif
uint64_t Renderer::workingBytes() const {
    uint64_t mips = impl_->mipmaps.capacity() * sizeof(std::vector<Texture>) +
                    impl_->mipTexInfo.capacity() * sizeof(std::vector<TexInfo>) +
                    impl_->baseTexInfo.capacity() * sizeof(TexInfo);
    for (const auto& levels : impl_->mipmaps) {
        mips += levels.capacity() * sizeof(Texture);
        for (const auto& level : levels) mips += level.pixels.capacity() * sizeof(uint16_t);
    }
    for (const auto& levels : impl_->mipTexInfo) mips += levels.capacity() * sizeof(TexInfo);
    uint64_t duskSky = 0;
    for (const auto& face : impl_->sunsetSky) duskSky += face.capacity() * sizeof(uint16_t);
    return sizeof(Impl) + mips + duskSky + impl_->light.bytes() +
           (impl_->depth.capacity() + impl_->edgeRow.capacity() + impl_->tileMin.capacity()) * sizeof(uint16_t) +
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
