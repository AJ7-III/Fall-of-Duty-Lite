#pragma once

#include "campaign_dialogue.hpp"
#include "types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
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

    void include(Vec3 p) {
        min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
        max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
    }
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
    uint8_t hazeShift = 0;
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
// Face axes match the panorama conversion in the exporter.
struct SkyAxes { Vec3 normal, right, vertical; };
inline constexpr SkyAxes skyAxes[]{
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
};
inline constexpr Vec3 sunsetSunDir{-0.8780f, 0.1497f, 0.4545f};
#ifndef ESP_PLATFORM
constexpr int desktopSkyFaceSize = 256;
using DesktopSky = std::array<std::vector<uint32_t>, std::size(skyAxes)>;
#endif

struct EncodedVoice {
    std::vector<uint8_t> codes;
    uint32_t frames;
    int16_t predictor;
    uint8_t index;
};

struct CampaignVoiceBank {
    struct Clip {
        uint32_t frames;
        uint32_t offset;
        int16_t predictor;
        uint8_t index;
        uint8_t rateDivisor;
    };
    std::array<Clip, campaignVoiceClipCount> clips{};
    std::unique_ptr<uint8_t, void (*)(void*)> codes{nullptr, &std::free};
    uint32_t codeBytes = 0;
};
inline constexpr char campaignVoiceFilename[] = "campaign-voices.fodv";

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
enum class ArenaScene : uint8_t { Estate, Timberline, Scout };

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
    std::vector<uint16_t> paintMaterials;
};

// Where the transport's rotor sits on its airframe.
constexpr Vec3 transportRotorMount{0, 3.35f, .15f};

// Layout of Assets::alienTrooperPoses and Assets::civilianPoses.
constexpr uint16_t alienIdlePoses = 2, alienStridePoses = 6, alienAimPoses = 2, alienRecoilPoses = 2;
constexpr uint16_t alienTotalPoses = alienIdlePoses + alienStridePoses + alienAimPoses + alienRecoilPoses;
constexpr uint16_t civilianCowerPoses = 2, civilianRunPoses = 6, civilianStandPose = civilianCowerPoses + civilianRunPoses;
constexpr uint16_t civilianStandPoses = 2;
#ifndef ESP_PLATFORM
enum class CampaignHuman : uint8_t { Ben, Nora, Walt, Delta, Lead, Partner, Count };
enum class CampaignPart : uint8_t { Body, LeftLeg, RightLeg, Head };
constexpr unsigned campaignIdlePoses = 4, campaignGaitPoses = 16, campaignCoverPoses = 3;
constexpr unsigned campaignWalk = campaignIdlePoses, campaignRun = campaignWalk + campaignGaitPoses;
constexpr unsigned campaignCover = campaignRun + campaignGaitPoses;
constexpr unsigned campaignFlinch = campaignCover + campaignCoverPoses;
constexpr unsigned campaignPoseCount = campaignFlinch + 1;
// Delta appends a braced aim to the common locomotion poses.
constexpr unsigned campaignAim = campaignCover;

struct PoseRegion {
    size_t first = 0, count = 0;
    size_t end() const { return first + count; }
};

struct CampaignPose {
    std::vector<Vec3> posePositions;
};

struct CampaignCharacter {
    Mesh body;
    std::vector<CampaignPose> poses;
    std::array<PoseRegion, 4> regions{};
    std::array<Vec3, campaignPoseCount> neck{};
};
#endif

struct ArenaGeometry {
    Mesh world;
    std::vector<FireSource> fires;
    std::vector<Vec3> roadFlares;
    std::vector<SmokeSource> smoke;
    std::vector<MistBank> mist;
    std::vector<PropInstance> propInstances;
    std::vector<ShotSurface> shotSurfaces;
    std::vector<Vec3> shotVertices;
    std::vector<std::array<uint32_t, 3>> shotIndices; // Direct packs store consecutive triples.
    std::vector<Chunk> shotChunks;
    // Ground faces on the playable hillside, sharing the rendered mesh's diagonal.
    std::vector<std::array<Vec3, 3>> hillside;
    std::vector<Obstacle> obstacles;
    std::vector<NavNode> navNodes;
    std::vector<NavEdge> navEdges;
    std::vector<BakedLight> lamps;
    std::vector<uint8_t> glassHits;
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

};

struct Assets : ArenaGeometry {
    struct ArenaBaseline {
        size_t materials = 0, textures = 0, audio = 0;
    };
    Mesh weapon[3], apache, jet;
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
    // Left arm vertices in the braced pose, in the same order as the Enforcer mesh.
    std::vector<Vec3> alienGuardArm;
    size_t alienGuardArmFirst = 0;
    // Sculpted alien ray guns held by troopers and enforcers, with spinning accelerator rings.
    Mesh alienRayGun, alienRayRings;
    Mesh alienHeavyRayGun, alienHeavyRayRings;
    std::array<Vec3, alienTotalPoses> alienGunSockets{};
    Mesh alienUplink, alienUplinkCore;
    Mesh scoutCargoCrate;
    Mesh relayTerminal; // Nora's emitter kit and the dish console
    // Unarmed civilian: cower, run cycle and breathing poses share the same mesh and outfit remaps.
    Mesh civilianMesh;
    std::vector<Mesh> civilianPoses;
#ifndef ESP_PLATFORM
    std::array<CampaignCharacter, size_t(CampaignHuman::Count)> campaignPeople;
    std::array<PoseRegion, 2> alienLegs;
    Mesh desktopScout, desktopScoutGear;
    Mesh desktopAttackDrone, desktopJammer;
    DesktopSky desktopDusk, desktopTimberlineSky;
#endif
    // Utility helicopter for the campaign's evac bird; the rotor spins about its own origin.
    Mesh transportHelo, transportRotor;
    std::array<Mesh, 3> alienFleet;
    // Reusable high-detail Ship Box props extracted before arena switch (vehicles instanced; clutter stamped).
    Mesh shipBoxTruck, shipBoxCar;
    Mesh shipBoxCrate, shipBoxPallet, shipBoxBarrel, shipBoxLamp, shipBoxContainer;
    std::vector<Material> materials;
    std::vector<uint16_t> playerUniform;
    std::vector<uint16_t> commanderUniform;
    std::vector<uint16_t> enforcerUniform;
    std::vector<uint16_t> thermalUniform;
    std::array<std::vector<uint16_t>, 3> civilianUniforms;
    std::vector<Texture> textures;
    std::array<std::vector<uint8_t>, 6> sky;
    std::vector<Clip> audio;
    std::array<std::vector<EncodedVoice>, voiceBankCount> voices;
    std::shared_ptr<const CampaignVoiceBank> campaignVoices;
    uint64_t storedBytes = 0, residentBytes = 0, peakBytes = 0;
    std::array<uint8_t, 20> sourceRevision{};
    uint32_t artSeed = 0;
    uint16_t walkFrameCount = 0, runFrameCount = 0;
    Arena arena = Arena::ShipBox;
    BriarHouseMode briarHouseMode = BriarHouseMode::Standard;
    ArenaBaseline briarBaseline;
    std::shared_ptr<const ArenaGeometry> timberline;
    std::shared_ptr<const ArenaGeometry> scoutInterior;

#ifndef ESP_PLATFORM
    bool hasDesktopCampaignArt() const { return !campaignPeople.front().poses.empty(); }
#endif

    const ArenaGeometry& geometry(ArenaScene scene) const {
        if (scene == ArenaScene::Timberline && timberline) return *timberline;
        if (scene == ArenaScene::Scout && scoutInterior) return *scoutInterior;
        return *this;
    }

    bool load(const char* path, std::string& error);
    bool loadCampaignVoices(const char* path, std::string& error);
};

} // namespace fod
