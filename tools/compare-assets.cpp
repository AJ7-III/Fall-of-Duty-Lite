#include "assets.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using fod::Assets;
using fod::Mesh;
using fod::Vec3;

struct Projection {
    Vec3 eye;
    float yaw, pitch, fov;
};

struct Difference {
    size_t vertices = 0;
    size_t visibleVertices = 0;
    float meters = 0;
    float pixels = 0;
};

bool pixelPosition(const Projection& projection, Vec3 position, int width, int height, float& x, float& y) {
    const auto forward = fod::forward(projection.yaw, projection.pitch);
    const Vec3 right{std::cos(projection.yaw), 0, -std::sin(projection.yaw)};
    const Vec3 up = cross(forward, right);
    const Vec3 offset = position - projection.eye;
    const float z = dot(offset, forward);
    if (z <= 0.02f) return false;
    const float focal = float(height) / 2 / std::tan(projection.fov / 2);
    x = float(width) / 2 + dot(offset, right) * focal / z;
    y = float(height) / 2 - dot(offset, up) * focal / z;
    return x >= 0 && x < float(width) && y >= 0 && y < float(height);
}

void comparePoint(Vec3 a, Vec3 b, Difference& difference, const Projection& camera, int width, int height, Vec3 placement) {
    difference.meters = std::max(difference.meters, fod::length(a - b));
    ++difference.vertices;
    float ax = 0, ay = 0, bx = 0, by = 0;
    if (pixelPosition(camera, a + placement, width, height, ax, ay) &&
        pixelPosition(camera, b + placement, width, height, bx, by)) {
        ++difference.visibleVertices;
        difference.pixels = std::max(difference.pixels, std::hypot(ax - bx, ay - by));
    }
}

bool compareMesh(const Mesh& a, const Mesh& b, Difference& difference, const Projection& camera, int width, int height,
                 Vec3 placement = {}) {
    if (a.vertexCount() != b.vertexCount() || a.vertices.size() != b.vertices.size() || a.triangles.size() != b.triangles.size()) return false;
    for (size_t i = 0; i < a.triangles.size(); ++i) {
        const auto& u = a.triangles[i];
        const auto& v = b.triangles[i];
        if (u.a != v.a || u.b != v.b || u.c != v.c || u.material != v.material || u.glass != v.glass) return false;
    }
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        const auto& u = a.vertices[i];
        const auto& v = b.vertices[i];
        if (u.u != v.u || u.v != v.v) return false;
        comparePoint(u.position, v.position, difference, camera, width, height, placement);
    }
    for (size_t i = 0; i < a.posePositions.size(); ++i)
        comparePoint(a.posePositions[i], b.posePositions[i], difference, camera, width, height, placement);
    return true;
}

bool shotsEqual(const Assets& a, const Assets& b) {
    if (a.glassHits != b.glassHits || a.shotSurfaces.size() != b.shotSurfaces.size()) return false;
    for (size_t i = 0; i < a.shotSurfaces.size(); ++i) {
        const auto& u = a.shotSurfaces[i];
        const auto& v = b.shotSurfaces[i];
        const auto aCorners = a.shotCorners(i), bCorners = b.shotCorners(i);
        if (std::memcmp(aCorners.data(), bCorners.data(), aCorners.size() * sizeof(Vec3)) || u.surface != v.surface || u.glass != v.glass)
            return false;
    }
    return true;
}

bool otherAssetsEqual(const Assets& a, const Assets& b) {
    if (a.materials.size() != b.materials.size() || a.textures.size() != b.textures.size() || a.audio.size() != b.audio.size()) return false;
    for (size_t i = 0; i < a.materials.size(); ++i) {
        const auto& u = a.materials[i];
        const auto& v = b.materials[i];
        if (u.color != v.color || u.texture != v.texture || u.alpha != v.alpha || u.doubleSided != v.doubleSided) return false;
    }
    for (size_t i = 0; i < a.textures.size(); ++i) {
        const auto& u = a.textures[i];
        const auto& v = b.textures[i];
        if (u.width != v.width || u.height != v.height || u.pixels != v.pixels) return false;
    }
    for (size_t i = 0; i < a.audio.size(); ++i) {
        if (a.audio[i].samples != b.audio[i].samples) return false;
    }
    if (a.sky != b.sky) return false;
    for (size_t bank = 0; bank < a.voices.size(); ++bank) {
        const auto& u = a.voices[bank];
        const auto& v = b.voices[bank];
        if (u.size() != v.size()) return false;
        for (size_t clip = 0; clip < u.size(); ++clip) {
            if (u[clip].frames != v[clip].frames || u[clip].predictor != v[clip].predictor ||
                u[clip].index != v[clip].index || u[clip].codes != v[clip].codes) return false;
        }
    }
    return true;
}

bool compare(const Assets& reference, const Assets& variant, int width, int height) {
    constexpr Projection clearLane{{0.15f, 1.7f, -13.5f}, 0, 0, 70 * fod::pi / 180};
    constexpr Projection hip{{}, 0, 0, 70 * fod::pi / 180};
    constexpr Projection irons{{}, 0, 0, 55 * fod::pi / 180};
    constexpr Projection flyingIn{{-13.5f, 1.7f, -13.5f}, fod::pi / 4, -0.44f, 70 * fod::pi / 180};
    Difference yard, weapons, sights, mechanisms, rival, apache, jet;
    if (!compareMesh(reference.world, variant.world, yard, clearLane, width, height)) return false;
    for (size_t weapon = 0; weapon < 3; ++weapon) {
        if (!compareMesh(reference.weapon[weapon], variant.weapon[weapon], weapons, hip, width, height)
            || !compareMesh(reference.weaponAds[weapon], variant.weaponAds[weapon], sights, irons, width, height)
            || !compareMesh(reference.weaponReload[weapon], variant.weaponReload[weapon], mechanisms, hip, width, height)) return false;
    }
    if (!compareMesh(reference.sniperBolt, variant.sniperBolt, mechanisms, hip, width, height)) return false;
    const auto count = [](const fod::WeaponPose& pose) { return pose.packed.empty() ? pose.positions.size() : pose.packed.size(); };
    const auto position = [](const fod::WeaponPose& pose, size_t i) {
        if (pose.packed.empty()) return pose.positions[i];
        const auto p = pose.packed[i];
        return Vec3{float(p.x), float(p.y), float(p.z)} * (1.f / 8192);
    };
    for (size_t i = 0; i < reference.weaponTracks.size(); ++i) {
        const auto& a = reference.weaponTracks[i];
        const auto& b = variant.weaponTracks[i];
        if (a.size() != b.size()) return false;
        for (size_t j = 0; j < a.size(); ++j) {
            if (a[j].seconds != b[j].seconds || count(a[j]) != count(b[j])) return false;
            for (size_t k = 0; k < count(a[j]); ++k)
                comparePoint(position(a[j], k), position(b[j], k), mechanisms, hip, width, height, {});
        }
    }
    if (!compareMesh(reference.apache, variant.apache, apache, flyingIn, width, height, {9, 15, 9})) return false;
    if (!compareMesh(reference.jet, variant.jet, jet, flyingIn, width, height, {9, 15, 9})) return false;
    if (reference.rivalFrames.size() != variant.rivalFrames.size()) return false;
    for (size_t frame = 0; frame < reference.rivalFrames.size(); ++frame) {
        const Vec3 crossingLane{0.15f + (float(frame % 6) - 2.5f) * 0.2f, 0, 12};
        if (!compareMesh(reference.rivalFrames[frame], variant.rivalFrames[frame], rival, clearLane, width, height, crossingLane)) return false;
    }
    if (reference.hanzFrames.size() != variant.hanzFrames.size()) return false;
    for (size_t i = 0; i < reference.hanzFrames.size(); ++i) {
        if (!compareMesh(reference.hanzFrames[i], variant.hanzFrames[i], rival, clearLane, width, height, {0.15f, 0, 12})) return false;
    }
    if (!compareMesh(reference.playerBody, variant.playerBody, rival, clearLane, width, height, {0.15f, 0, 12})) return false;
    const char* parts[] = {"yard", "hip", "ads", "reload_and_bolt", "moving_rival", "apache", "strike_jet"};
    const Difference* differences[] = {&yard, &weapons, &sights, &mechanisms, &rival, &apache, &jet};
    for (size_t i = 0; i < std::size(parts); ++i) {
        const auto& d = *differences[i];
        std::printf("  %dx%d %-15s vertices=%zu in_view=%zu max_m=%.8f max_px=%.6f\n",
                    width, height, parts[i], d.vertices, d.visibleVertices, d.meters, d.pixels);
    }
    return true;
}

Assets load(const char* path) {
    using Clock = std::chrono::steady_clock;
    std::vector<double> milliseconds;
    Assets retained;
    for (int repeat = 0; repeat < 10; ++repeat) {
        Assets fresh;
        std::string error;
        const auto start = Clock::now();
        if (!fresh.load(path, error)) {
            std::fprintf(stderr, "%s: %s\n", path, error.c_str());
            std::exit(1);
        }
        milliseconds.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        retained = std::move(fresh);
    }
    std::sort(milliseconds.begin(), milliseconds.end());
    std::printf("%s disk=%llu resident=%llu peak=%llu host_load_p50_ms=%.3f host_load_max_ms=%.3f\n",
                path, static_cast<unsigned long long>(retained.storedBytes), static_cast<unsigned long long>(retained.residentBytes),
                static_cast<unsigned long long>(retained.peakBytes),
                (milliseconds[4] + milliseconds[5]) * 0.5, milliseconds.back());
    return retained;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s full.fodp candidate.fodp [candidate ...]\n", argv[0]);
        return 2;
    }
    const Assets reference = load(argv[1]);
    for (int i = 2; i < argc; ++i) {
        const Assets variant = load(argv[i]);
        if (!shotsEqual(reference, variant) || !otherAssetsEqual(reference, variant) ||
            !compare(reference, variant, 400, 225) || !compare(reference, variant, 320, 180)) {
            std::fprintf(stderr, "%s has an unexpected difference in collision geometry, topology, material, or audio.\n", argv[i]);
            return 1;
        }
        std::puts("  shot_triangles, glass_strengths, topology, textures, sky, PCM and ADPCM: exact");
    }
}
