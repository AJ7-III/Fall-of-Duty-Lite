#include "render.hpp"
#include "assets.hpp"
#include "game_data.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace fod {
namespace {

using Clock = std::chrono::steady_clock;
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
    uint8_t light;
    bool mipmapped = false;
};

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
    View view{};
    uint16_t* color = nullptr;

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
        worldLights.reserve(world.triangles.size());
        const bool oneSided = std::any_of(world.triangles.begin(), world.triangles.end(), [&source](const Triangle& triangle) {
            return !source.materials[triangle.material].doubleSided;
        });
        if (oneSided) culledNormals.resize(world.triangles.size());
        for (size_t i = 0; i < world.triangles.size(); ++i) {
            const auto& triangle = world.triangles[i];
            const Vec3 a = world.vertices[triangle.a].position;
            const Vec3 normal = cross(world.vertices[triangle.b].position - a, world.vertices[triangle.c].position - a);
            worldLights.push_back(lightFor(normal, false));
            if (oneSided) culledNormals[i] = normal;
        }
        if (enhanced) {
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

    bool project(Primitive& value, Vertex a, Vertex b, Vertex c) const {
        value.p[0] = view.project(a);
        value.p[1] = view.project(b);
        value.p[2] = view.project(c);
        return visibleProjection(value);
    }

    bool visibleProjection(Primitive& value) const {
        const float area = edge(value.p[0], value.p[1], value.p[2].x, value.p[2].y);
        if (std::abs(area) < 0.09f) return false;
        if (area < 0) std::swap(value.p[1], value.p[2]);
        const float minX = std::min({value.p[0].x, value.p[1].x, value.p[2].x});
        const float maxX = std::max({value.p[0].x, value.p[1].x, value.p[2].x});
        const float minY = std::min({value.p[0].y, value.p[1].y, value.p[2].y});
        const float maxY = std::max({value.p[0].y, value.p[1].y, value.p[2].y});
        return minX < view.width && maxX >= 0 && minY < view.height && maxY >= 0;
    }

    void enqueue(const Primitive& value) {
        if (assets.materials[value.material].alpha == 255) {
            if (solid.size() == solid.capacity()) draw(solid, true);
            solid.push_back(value);
        } else {
            translucent.push_back(value);
        }
        ++stats.triangles;
    }

    template<class Submit>
    void clip(const Vertex* v, Submit submit) {
        unsigned mask = 0;
        for (unsigned i = 0; i < 3; ++i) mask |= unsigned(v[i].position.z >= view.near) << i;
        if (mask == 0) return;
        if (mask == 7) {
            submit(v[0], v[1], v[2]);
            return;
        }
        Vertex clipped[4]{};
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

    template<class CameraVertex, class Shade>
    void submitFace(const Triangle& tri, uint16_t material, const CameraVertex& vertex, Shade shade, bool mipmapped = false) {
        // Copy each projection before another index can reuse its cache slot.
        Primitive value{{projectVertex(tri.a, vertex), projectVertex(tri.b, vertex), projectVertex(tri.c, vertex)},
                        material, 0, mipmapped};
        const auto& [a, b, c] = value.p;
        // Zero reciprocal depth marks vertices behind the near plane.
        if (a.depth > 0 && b.depth > 0 && c.depth > 0) {
            if (visibleProjection(value)) {
                value.light = shade();
                enqueue(value);
            }
            return;
        }
        if (a.depth == 0 && b.depth == 0 && c.depth == 0) return;

        const Vertex vertices[]{vertex(tri.a), vertex(tri.b), vertex(tri.c)};
        uint8_t light = 0; // Face lighting is always nonzero.
        clip(vertices, [&](Vertex x, Vertex y, Vertex z) {
            Primitive clipped{{}, material, 0, mipmapped};
            if (!project(clipped, x, y, z)) return;
            if (!light) light = shade();
            clipped.light = light;
            enqueue(clipped);
        });
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
            Vec3 position = mesh.position(i);
            if (motion) position = motion->position(i);
            else if (next) position += (next->position(i) - position) * fraction;
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
            submitFace(tri, material, vertex, [&] { return lightFor(doubleSided ? faceNormal() : normal, metallic); });
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
            submitFace(tri, tri.material, vertex, [&] { return worldLights[i]; }, enhanced);
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

    template<bool backdrop = false>
    void raster(const Primitive& t, bool opaque, const uint8_t* skyPixels = nullptr) {
        const auto& [a, b, c] = t.p;
        const Material* material = backdrop ? nullptr : &assets.materials[t.material];
        const Texture* base = backdrop ? nullptr :
            material->texture >= 0 ? &assets.textures[material->texture] : nullptr;
        const int minX = int(std::max(0.0f, std::ceil(std::min({a.x, b.x, c.x}) - 0.5f)));
        const int minY = int(std::max(0.0f, std::ceil(std::min({a.y, b.y, c.y}) - 0.5f)));
        const int maxX = int(std::min(float(view.width - 1), std::floor(std::max({a.x, b.x, c.x}) - 0.5f)));
        const int maxY = int(std::min(float(view.height - 1), std::floor(std::max({a.y, b.y, c.y}) - 0.5f)));
        if (minX > maxX || minY > maxY) return;

        const float inverseArea = 1.0f / edge(a, b, c.x, c.y);
        const float dx[3]{b.y - c.y, c.y - a.y, a.y - b.y};
        const uint16_t flat = backdrop ? 0 : multiply(material->color, t.light);
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
        if (!backdrop && base && t.mipmapped && !mipmaps[material->texture].empty()) {
            levels = &mipmaps[material->texture];
            const float dy[3]{c.x - b.x, a.x - c.x, b.x - a.x};
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

        for (int y = minY; y <= maxY; ++y) {
            float e0 = edge(b, c, minX + 0.5f, y + 0.5f);
            float e1 = edge(c, a, minX + 0.5f, y + 0.5f);
            float e2 = edge(a, b, minX + 0.5f, y + 0.5f);
            // Intersect the row with all three half planes. Skinny geometry no
            // longer pays for the empty corners of its bounding rectangle.
            int left = 0, right = maxX - minX;
            const float edges[3]{e0, e1, e2};
            for (int i = 0; i < 3; ++i) {
                if (dx[i] > 0) left = std::max(left, int(std::ceil(-edges[i] / dx[i])));
                else if (dx[i] < 0) right = std::min(right, int(std::floor(-edges[i] / dx[i])));
                else if (edges[i] < 0) right = -1;
            }
            if (left > right) continue;
            e0 += dx[0] * left;
            e1 += dx[1] * left;
            e2 += dx[2] * left;
            float z = (a.depth * e0 + b.depth * e1 + c.depth * e2) * inverseArea;
            float u = (a.u * e0 + b.u * e1 + c.u * e2) * inverseArea;
            float v = (a.v * e0 + b.v * e1 + c.v * e2) * inverseArea;
            int at = y * view.width + minX + left;
            for (int x = left; x <= right;) {
                const int end = perspectiveMips ? std::min(right, x + 15) : right;
                const float midpoint = (end - x) * 0.5f;
                const auto sample = perspectiveMips ? mipAt(z + dzdx * midpoint, u + dudx * midpoint, v + dvdx * midpoint)
                                                    : triangleTexture;
                const auto* texture = sample.fine;
                const bool halfSize = sample.coarse && texture->width == sample.coarse->width * 2 &&
                    texture->height == sample.coarse->height * 2;
                for (; x <= end; ++x, ++at, z += dzdx, u += dudx, v += dvdx) {
                    uint16_t newDepth = 0;
                    if constexpr (backdrop) {
                        if (depth[at]) continue;
                        const float invZ = 1.0f / z;
                        color[at] = sampleSky(skyPixels, u * invZ, v * invZ, minX + x, y);
                        ++stats.depthPasses;
                        continue;
                    } else {
                        newDepth = enhanced ? encodedDepth(z) : uint16_t(std::clamp(z * depthScale, 0.0f, 65535.0f));
                        if (newDepth <= depth[at]) continue;
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
                        pixel = multiply(pixel, t.light);
                    }
                    color[at] = opaque ? pixel : blend(color[at], pixel, material->alpha);
                    if (opaque) depth[at] = newDepth;
                    ++stats.depthPasses;
                }
            }
        }
    }

    void draw(std::vector<Primitive>& batch, bool opaque) {
        const auto start = Clock::now();
        if (!opaque) {
            std::sort(batch.begin(), batch.end(), [](const Primitive& a, const Primitive& b) {
                return a.p[0].depth + a.p[1].depth + a.p[2].depth < b.p[0].depth + b.p[1].depth + b.p[2].depth;
            });
        }
        for (const auto& primitive : batch) raster(primitive, opaque);
        batch.clear();
        stats.rasterUs += microseconds(start, Clock::now());
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
        stats.visibleChunks = uint32_t(visible.size());
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
        const auto* uniform = player && !assets.playerUniform.empty() ? assets.playerUniform.data() : nullptr;
        const bool legacyMetal = !player && state.difficulty >= 9 && assets.playerUniform.empty() && assets.hanzFrames.empty();
        submit(mesh, state, transform, MeshSpace::World,
               mesh.vertexCount() == next.vertexCount() ? &next : nullptr, actor.frameBlend, legacyMetal, topology, uniform);
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
        shift += Vec3{-0.055f * run, -0.11f * run - 0.31f * drop, -state.recoil};
        pitch -= 0.34f * run + 0.55f * drop + 0.12f * state.recoil;
        yaw += 0.21f * run;
        roll += 0.14f * run;
        const Vec3 pivot = state.weaponPosition;
        auto transform = orientation({}, yaw, pitch, roll);
        transform.origin = shift + pivot - transform(pivot);
        if (tracked) {
            const auto motion = weaponMotion(state);
            submit(mesh, state, transform, MeshSpace::View, nullptr, 0, false, nullptr, nullptr, &motion);
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
        const auto start = Clock::now();
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
                if (project(primitive, a, b, c)) raster<true>(primitive, false, assets.sky[face].data());
            };
            clip(first, submit);
            clip(second, submit);
        }
        view.position = eye;
        stats.rasterUs += microseconds(start, Clock::now());
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
        const auto start = Clock::now();
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
        stats.rasterUs += microseconds(start, Clock::now());
    }

    void render(const Snapshot& state, int width, int height, uint16_t* target) {
        if (!target || width < 3 || height < 5 || int64_t(width) * height > int64_t(depth.size())) return;
        const auto start = Clock::now();
        stats = {};
        color = target;
        constexpr Camera overview{{-13.8f, 6.6f, -13.8f}, 0.60f, 0.32f, 59 * pi / 180};
        const bool title = state.match == Match::Start;
        const auto& camera = title ? overview : state.camera;
        view = makeView(camera, width, height, 0.10f);
        std::fill_n(depth.data(), width * height, 0);
        sky(camera);
        drawWorld(state);
        if (!title) drawActor(state.rival, state, false);
        if (state.dead) drawActor(state.player, state, true);
        if (state.apacheActive) {
            submit(assets.apache, state,
                   orientation(state.apachePosition, state.apacheYaw, 0, 0), MeshSpace::World);
        }
        for (const auto& jet : state.jets) {
            if (jet.visible) submit(assets.jet, state, orientation(jet.position, jet.yaw, 0, 0), MeshSpace::World);
        }
        draw(solid, true);
        drawSkyCube();
        draw(translucent, false);
        drawTracer(state);
        weather(state);
        if (!title && !state.dead && state.device == Device::None && state.scope < 0.6f &&
            !assets.weapon[size_t(state.weapon)].triangles.empty()) {
            Camera firstPerson{{}, 0, 0, state.camera.fov};
            view = makeView(firstPerson, width, height, 0.012f);
            std::fill_n(depth.data(), width * height, 0);
            drawWeapon(state);
            draw(solid, true);
            draw(translucent, false);
        }
        if (enhanced) smoothDiagonals();
        const auto elapsed = microseconds(start, Clock::now());
        stats.setupUs = elapsed - std::min(elapsed, stats.rasterUs);
    }
};

Renderer::Renderer(const Assets& assets, RenderLook look) : impl_(std::make_unique<Impl>(assets, look)) {}
Renderer::~Renderer() = default;
void Renderer::render(const Snapshot& state, int width, int height, uint16_t* rgb565) { impl_->render(state, width, height, rgb565); }
const RenderStats& Renderer::stats() const { return impl_->stats; }
uint64_t Renderer::workingBytes() const {
    uint64_t mips = impl_->mipmaps.capacity() * sizeof(std::vector<Texture>);
    for (const auto& levels : impl_->mipmaps) {
        mips += levels.capacity() * sizeof(Texture);
        for (const auto& level : levels) mips += level.pixels.capacity() * sizeof(uint16_t);
    }
    return sizeof(Impl) + mips + (impl_->depth.capacity() + impl_->edgeRow.capacity()) * sizeof(uint16_t) +
           impl_->culledNormals.capacity() * sizeof(Vec3) + impl_->worldLights.capacity() +
           impl_->posedVertices.capacity() * sizeof(PosedVertex) +
           (impl_->solid.capacity() + impl_->translucent.capacity()) * sizeof(Primitive) +
           impl_->visible.capacity() * sizeof(std::pair<float, uint32_t>);
}

} // namespace fod
