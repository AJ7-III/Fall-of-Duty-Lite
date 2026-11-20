#include "render.hpp"
#include "assets.hpp"
#include "game_data.hpp"
#include "light.hpp"

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

enum class Fill { Surface, Sky, Shadow };

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
        if (local.z + radius(forward) < near || local.z - radius(forward) > 110) return false;
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
    std::vector<uint16_t> depth = std::vector<uint16_t>(400 * 225);
    std::vector<Vec3> culledNormals;
    std::vector<uint8_t> worldLights;
    std::vector<uint16_t> edgeRow;
    std::vector<std::vector<Texture>> mipmaps;
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
    // Daylight in the space of the mesh being submitted, and the ground light where it stands.
    Vec3 sun{}, up{0, 1, 0};
    unsigned meshLight = 255;

    Clock::time_point timestamp() const { return measure ? Clock::now() : Clock::time_point{}; }

    void recordRasterTime(Clock::time_point start) {
        if (measure) stats.rasterUs += microseconds(start, Clock::now());
    }

    explicit Impl(const Assets& source, RenderLook look) : assets(source), enhanced(look == RenderLook::Enhanced) {
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
        for (const auto& weapon : source.weapon) {
            maxWeaponAlpha = std::max(maxWeaponAlpha, alphaCount(weapon));
            vertices = std::max(vertices, weapon.vertices.size());
        }
        const size_t worldAlpha = alphaCount(source.world) + 2 * maxActorAlpha + alphaCount(source.apache) + 3 * alphaCount(source.jet);
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
            mist();
            mipmaps.resize(source.textures.size());
            for (const auto& triangle : world.triangles) {
                const auto id = source.materials[triangle.material].texture;
                if (id < 0 || !mipmaps[id].empty()) continue;
                const Texture* previous = &source.textures[id];
                auto& levels = mipmaps[id];
                while (std::max(previous->width, previous->height) > 4) {
                    levels.push_back(halfTexture(*previous));
                    previous = &levels.back();
                }
            }
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
                const Mesh* topology = nullptr, const uint16_t* uniform = nullptr, const MotionBlend* motion = nullptr) {
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
            });
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

    template<Fill fill = Fill::Surface, bool detail = false, bool countPixels = false>
    void raster(const Primitive& t, bool opaque, const uint8_t* skyPixels = nullptr) {
        constexpr bool backdrop = fill == Fill::Sky, shadow = fill == Fill::Shadow;
        const auto& [a, b, c] = t.p;
        const Material* material = fill == Fill::Surface ? &assets.materials[t.material] : nullptr;
        const Texture* base = material && material->texture >= 0 ? &assets.textures[material->texture] : nullptr;
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
        const float dudx = gradient(a.u, b.u, c.u, dx);
        const float dvdx = gradient(a.v, b.v, c.v, dx);

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
        const bool hazy = detail && fill == Fill::Surface && std::min({a.depth, b.depth, c.depth}) < hazeStart;
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
            float u = (a.u * e0 + b.u * e1 + c.u * e2) * inverseArea;
            float v = (a.v * e0 + b.v * e1 + c.v * e2) * inverseArea;

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
                        color[at] = sampleSky(skyPixels, u * invZ, v * invZ, minX + x, y);
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
                    if (texture) {
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
                            if (const unsigned level = haze[newDepth >> 6]) pixel = veil(pixel, level);
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
            if (enhanced && detailed(primitive)) raster<Fill::Surface, true, countPixels>(primitive, opaque);
            else raster<Fill::Surface, false, countPixels>(primitive, opaque);
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

    void drawActor(const Actor& actor, const Snapshot& state, bool player) {
        if (!actor.visible || assets.rivalFrames.empty()) return;
        const auto& frames = !player && state.alternateRival && !assets.hanzFrames.empty() ? assets.hanzFrames : assets.rivalFrames;
        const bool separatePlayer = player && !assets.playerBody.vertices.empty();
        const auto& mesh = separatePlayer ? assets.playerBody : frames[std::min<size_t>(actor.frame, frames.size() - 1)];
        const auto& next = separatePlayer ? mesh : frames[std::min<size_t>(actor.nextFrame, frames.size() - 1)];
        const float fall = std::clamp(actor.fall, 0.0f, 1.0f);
        const float fallYaw = actor.fallVariant == 2 ? fall * 1.35f : 0;
        const float fallPitch = actor.fallVariant == 1 ? fall * 1.46f : -fall * 1.46f;
        auto transform = orientation(actor.position + Vec3{0, fall * 0.24f, 0}, actor.yaw + fallYaw, fallPitch, fall * 0.12f);
        const Bounds bounds{actor.position + Vec3{-2, -0.4f, -2}, actor.position + Vec3{2, 2.2f, 2}};
        if (!view.sees(bounds)) return;
        const Mesh* topology = mesh.triangles.empty() ? &frames.front() : &mesh;
        meshLight = standing(actor.position);
        const auto* uniform = player && !assets.playerUniform.empty() ? assets.playerUniform.data() : nullptr;
        const bool legacyMetal = !player && state.difficulty >= 9 && assets.playerUniform.empty() && assets.hanzFrames.empty();
        submit(mesh, state, transform, MeshSpace::World,
               mesh.vertexCount() == next.vertexCount() ? &next : nullptr, actor.frameBlend, legacyMetal, topology, uniform);
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
        for (int y = 0; y < view.height; ++y) {
            const float mix = std::clamp((float(y) - horizon) / view.height + 0.40f, 0.0f, 1.0f);
            const auto pixel = rgb(int(105 + mix * 75), int(119 + mix * 59), int(132 + mix * 52));
            std::fill_n(color + y * view.width, view.width, pixel);
        }
    }

    void drawSkyCube() {
        if (!enhanced || assets.sky[0].empty()) return;
        const auto start = timestamp();
        // Face axes match the panorama conversion in the exporter.
        static constexpr Vec3 faces[][3] = {
            {{0,0,1},{1,0,0},{0,1,0}}, {{0,0,-1},{-1,0,0},{0,1,0}},
            {{1,0,0},{0,0,-1},{0,1,0}}, {{-1,0,0},{0,0,1},{0,1,0}},
            {{0,1,0},{1,0,0},{0,0,-1}}, {{0,-1,0},{1,0,0},{0,0,1}},
        };
        const Vec3 eye = view.position;
        view.position = {};
        for (size_t face = 0; face < assets.sky.size(); ++face) {
            const auto& [normal, right, up] = faces[face];
            const Vertex corners[] = {
                {view.transform(normal - right - up), 0, 0}, {view.transform(normal + right - up), 1, 0},
                {view.transform(normal + right + up), 1, 1}, {view.transform(normal - right + up), 0, 1},
            };
            const Vertex first[] = {corners[0], corners[1], corners[2]};
            const Vertex second[] = {corners[0], corners[2], corners[3]};
            const auto submit = [&](Vertex a, Vertex b, Vertex c) {
                Primitive primitive{};
                if (!project(primitive, a, b, c)) return;
                const auto* pixels = assets.sky[face].data();
                if (countRenderWork && measure) raster<Fill::Sky, false, true>(primitive, false, pixels);
                else raster<Fill::Sky>(primitive, false, pixels);
            };
            clip(first, submit);
            clip(second, submit);
        }
        view.position = eye;
        recordRasterTime(start);
    }

    void drawTracer(const Snapshot& state) {
        if (state.tracer <= 0) return;
        Vertex a{view.transform(state.tracerStart), 0, 0};
        Vertex b{view.transform(state.tracerEnd), 0, 0};
        if (a.position.z < view.near && b.position.z < view.near) return;
        if (a.position.z < view.near) a = cut(a, b, view.near);
        if (b.position.z < view.near) b = cut(b, a, view.near);
        const auto p = view.project(a), q = view.project(b);
        const float dx = q.x - p.x, dy = q.y - p.y;
        const int count = int(std::clamp(std::max(std::abs(dx), std::abs(dy)), 1.0f, 2.0f * view.width));
        for (int i = 0; i <= count; ++i) {
            const float t = float(i) / count;
            const int x = int(p.x + dx * t), y = int(p.y + dy * t);
            if (x < 0 || x >= view.width || y < 0 || y >= view.height) continue;
            const float inverseZ = p.depth + (q.depth - p.depth) * t;
            const float z = enhanced ? encodedDepth(inverseZ) : inverseZ * view.near * 65534;
            const int at = y * view.width + x;
            if (z + 1 >= depth[at]) color[at] = blend(color[at], rgb(255, 195, 107), 192);
        }
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
        constexpr Camera overview{{-13.8f, 6.6f, -13.8f}, 0.60f, 0.32f, 59 * pi / 180};
        const bool title = state.match == Match::Start;
        const auto& camera = title ? overview : state.camera;
        view = makeView(camera, width, height, 0.10f);
        sun = light.sun;
        up = {0, 1, 0};
        std::fill_n(depth.data(), width * height, 0);
        sky(camera);
        drawWorld(state);
        if (!title) drawActor(state.rival, state, false);
        if (state.dead) drawActor(state.player, state, true);
        meshLight = 255;
        if (state.apacheActive) {
            submit(assets.apache, state,
                   orientation(state.apachePosition, state.apacheYaw, 0, 0), MeshSpace::World);
        }
        for (const auto& jet : state.jets) {
            if (jet.visible) submit(assets.jet, state, orientation(jet.position, jet.yaw, 0, 0), MeshSpace::World);
        }
        draw(solid, true);
        if (enhanced) {
            const auto shadows = timestamp();
            if (!title && state.rival.visible && !assets.rivalFrames.empty()) contactShadow(state.rival.position, 0.62f);
            if (state.dead && state.player.visible && !assets.rivalFrames.empty()) contactShadow(state.player.position, 0.62f);
            recordRasterTime(shadows);
        }
        drawSkyCube();
        draw(translucent, false);
        drawTracer(state);
        weather(state);
        if (!title && !state.dead && state.device == Device::None && state.scope < 0.6f &&
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

Renderer::Renderer(const Assets& assets, RenderLook look) : impl_(std::make_unique<Impl>(assets, look)) {}
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
    return sizeof(Impl) + mips + impl_->light.bytes() + (impl_->depth.capacity() + impl_->edgeRow.capacity()) * sizeof(uint16_t) +
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
