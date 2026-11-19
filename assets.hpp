#pragma once

#include "types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fod {

struct Vertex {
    Vec3 position;
    float u, v;
};

struct Triangle {
    uint32_t a, b, c;
    uint16_t material;
    int16_t glass = -1;
};

struct Bounds {
    Vec3 min, max;
};

struct Chunk {
    Bounds bounds;
    uint32_t first, count;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<Triangle> triangles;
    std::vector<Chunk> chunks;
    std::vector<Vec3> posePositions; // Frames that share the base mesh's UVs and triangles.

    size_t vertexCount() const { return posePositions.empty() ? vertices.size() : posePositions.size(); }
    Vec3 position(size_t i) const { return posePositions.empty() ? vertices[i].position : posePositions[i]; }
};

struct Material {
    uint16_t color;
    int16_t texture = -1;
    uint8_t alpha = 255;
    bool doubleSided = true;
};

struct Texture {
    int width, height;
    std::vector<uint16_t> pixels;
};

struct Clip {
    std::vector<int16_t> samples;
};

constexpr size_t voiceBankCount = 6;
constexpr size_t maxVoiceVariants = 16;
constexpr int skyFaceSize = 64;

struct EncodedVoice {
    std::vector<uint8_t> codes;
    uint32_t frames;
    int16_t predictor;
    uint8_t index;
};

struct ShotSurface {
    uint32_t surface;
    int16_t glass = -1;
};

struct Obstacle {
    float cx, cz, hw, hd, minY, maxY, yaw, cos, sin;
};

struct NavNode {
    Vec3 position;
    uint32_t first, count;
    bool cover, walkable;
};

struct NavEdge {
    uint32_t to;
    float cost;
};

struct RivalHitbox {
    Vec3 center, right, up, forward, half;
    float multiplier;
};

enum class WeaponTrack : uint8_t { Mp44Reload, SniperReload, PistolReload, SniperBolt, Mp44Empty, PistolEmpty, Count };

struct PackedWeaponPoint { int16_t x, y, z; };
static_assert(sizeof(PackedWeaponPoint) == 6);

struct WeaponPose {
    float seconds = 0;
    std::vector<PackedWeaponPoint> packed;
    std::vector<Vec3> positions;
};

struct Assets {
    Mesh world, weapon[3], apache, jet;
    std::vector<Mesh> rivalFrames;
    std::vector<std::vector<RivalHitbox>> rivalHitboxes;
    Mesh weaponAds[3];
    Mesh weaponReload[3], sniperBolt;
    std::array<std::vector<WeaponPose>, size_t(WeaponTrack::Count)> weaponTracks;
    std::vector<Mesh> hanzFrames;
    Mesh playerBody;
    std::vector<Material> materials;
    std::vector<uint16_t> playerUniform;
    std::vector<Texture> textures;
    std::array<std::vector<uint8_t>, 6> sky;
    std::vector<ShotSurface> shotSurfaces;
    std::vector<Vec3> shotVertices;
    std::vector<std::array<uint32_t, 3>> shotIndices; // Direct packs store consecutive triples.
    std::vector<Chunk> shotChunks;
    std::vector<Obstacle> obstacles;
    std::vector<NavNode> navNodes;
    std::vector<NavEdge> navEdges;
    std::vector<Clip> audio;
    std::array<std::vector<EncodedVoice>, voiceBankCount> voices;
    std::vector<uint8_t> glassHits;
    uint64_t storedBytes = 0, residentBytes = 0, peakBytes = 0;
    std::array<uint8_t, 20> sourceRevision{};
    uint32_t artSeed = 0;
    uint16_t walkFrameCount = 0, runFrameCount = 0;

    std::array<Vec3, 3> shotCorners(size_t i) const {
        if (shotIndices.empty()) {
            const size_t first = 3 * i;
            return {shotVertices[first], shotVertices[first + 1], shotVertices[first + 2]};
        }
        const auto& [a, b, c] = shotIndices[i];
        return {shotVertices[a], shotVertices[b], shotVertices[c]};
    }

    bool load(const char* path, std::string& error);
};

} // namespace fod
