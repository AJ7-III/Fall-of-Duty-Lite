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
    bool emissive = false;
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
    bool blocksSight = true;
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

enum class Arena { ShipBox, BriarHouse };
enum class BriarHouseMode : uint8_t { Standard, Invasion };

struct ArenaExtent {
    float x = 15.6f, z = 15.6f;
};

constexpr const char* arenaLabel(Arena arena) {
    return arena == Arena::BriarHouse ? "BRIAR HOUSE" : "SHIP BOX";
}

struct SpawnPoint {
    Vec3 position;
    float yaw;
};

struct BakedLight {
    Vec3 position;
    float radius, strength;
};

struct FireSource {
    Vec3 base;
    float width, height;
};

// A column of smoke the renderer animates: puffs climb `height`, lean by `drift` at the top,
// grow from `size` and recycle every 1 / rate seconds.
struct SmokeSource {
    Vec3 base;
    float height;
    Vec3 drift;
    float size, rate;
};

// Low-lying valley mist bank that rolls gently across the grounds at dusk.
struct MistBank {
    Vec3 base;
    float width, height;
    Vec3 drift;
    float rate;
};

enum class PropKind : uint8_t { Truck, Car };

// Instanced high-detail Ship Box vehicle placed across the town without duplicating mesh RAM.
struct PropInstance {
    PropKind kind = PropKind::Car;
    Vec3 position{};
    float yaw = 0;
    float scale = 1.f;
    Bounds bounds{};
    float halfLen = 0, halfWidth = 0, height = 0;
};

// Where the transport's rotor sits on its airframe.
constexpr Vec3 transportRotorMount{0, 3.35f, .15f};

// Layout of Assets::alienTrooperPoses and Assets::civilianPoses.
constexpr uint16_t alienIdlePoses = 2, alienStridePoses = 6, alienAimPoses = 2, alienRecoilPoses = 2;
constexpr uint16_t alienTotalPoses = alienIdlePoses + alienStridePoses + alienAimPoses + alienRecoilPoses;
constexpr uint16_t civilianCowerPoses = 2, civilianRunPoses = 6, civilianStandPose = civilianCowerPoses + civilianRunPoses;
constexpr uint16_t civilianStandPoses = 2;

struct Assets {
    struct ArenaBaseline {
        size_t materials = 0, textures = 0, audio = 0;
    };
    Mesh world, weapon[3], apache, jet;
    std::vector<Mesh> rivalFrames;
    std::vector<std::vector<RivalHitbox>> rivalHitboxes;
    Mesh weaponAds[3];
    Mesh weaponReload[3], sniperBolt;
    std::array<std::vector<WeaponPose>, size_t(WeaponTrack::Count)> weaponTracks;
    std::vector<Mesh> hanzFrames;
    Mesh playerBody;
    Mesh alienTrooper, alienDrone;
    // Position-only poses that share alienTrooper's topology: idle breaths, lope, ray-gun aim, recoil.
    std::vector<Mesh> alienTrooperPoses;
    // Enforcers share the pose layout and carry bone armor across the shoulders and arms.
    Mesh alienEnforcer;
    std::vector<Mesh> alienEnforcerPoses;
    // Sculpted alien ray guns held by troopers and enforcers, with spinning accelerator rings.
    Mesh alienRayGun, alienRayRings;
    Mesh alienHeavyRayGun, alienHeavyRayRings;
    std::array<Vec3, alienTotalPoses> alienGunSockets{};
    Mesh alienUplink, alienUplinkCore;
    // Unarmed civilian: cower, run cycle and breathing poses share the same mesh and outfit remaps.
    Mesh civilianMesh;
    std::vector<Mesh> civilianPoses;
    // Utility helicopter for the campaign's evac bird; the rotor spins about its own origin.
    Mesh transportHelo, transportRotor;
    std::vector<FireSource> fires;
    std::vector<Vec3> roadFlares;
    std::vector<SmokeSource> smoke;
    std::vector<MistBank> mist;
    std::array<Mesh, 3> alienFleet;
    // Reusable high-detail Ship Box props extracted before arena switch (vehicles instanced; clutter stamped).
    Mesh shipBoxTruck, shipBoxCar;
    Mesh shipBoxCrate, shipBoxPallet, shipBoxBarrel, shipBoxLamp, shipBoxContainer;
    std::vector<PropInstance> propInstances;
    std::vector<Material> materials;
    std::vector<uint16_t> playerUniform;
    std::vector<uint16_t> commanderUniform;
    std::vector<uint16_t> enforcerUniform;
    std::array<std::vector<uint16_t>, 3> civilianUniforms;
    std::vector<Texture> textures;
    std::array<std::vector<uint8_t>, 6> sky;
    std::vector<ShotSurface> shotSurfaces;
    std::vector<Vec3> shotVertices;
    std::vector<std::array<uint32_t, 3>> shotIndices; // Direct packs store consecutive triples.
    std::vector<Chunk> shotChunks;
    std::vector<Obstacle> obstacles;
    std::vector<NavNode> navNodes;
    std::vector<NavEdge> navEdges;
    std::vector<BakedLight> lamps;
    std::vector<Clip> audio;
    std::array<std::vector<EncodedVoice>, voiceBankCount> voices;
    std::vector<uint8_t> glassHits;
    uint64_t storedBytes = 0, residentBytes = 0, peakBytes = 0;
    std::array<uint8_t, 20> sourceRevision{};
    uint32_t artSeed = 0;
    uint16_t walkFrameCount = 0, runFrameCount = 0;
    Arena arena = Arena::ShipBox;
    BriarHouseMode briarHouseMode = BriarHouseMode::Standard;
    ArenaBaseline briarBaseline;
    ArenaExtent extent;
    std::array<SpawnPoint, 4> spawns{{
        {{-13.5f, 0, -13.5f}, pi / 4}, {{13.5f, 0, 13.5f}, -3 * pi / 4},
        {{-13.5f, 0, 13.5f}, 3 * pi / 4}, {{13.5f, 0, -13.5f}, -pi / 4},
    }};

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
