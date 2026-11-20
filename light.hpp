#pragma once

#include "assets.hpp"

#include <algorithm>
#include <vector>

namespace fod {

// Baked light for one world triangle. A zero back value marks a ground face.
// Its first two bytes index the patch that places it on the ground light map.
struct FaceLight {
    uint8_t front[3];
    uint8_t back;
};

// Light map position in 8.8 fixed-point cells as a function of the texture coordinates.
struct GroundPatch {
    float xu, xv, x0, zu, zv, z0;
};

// Overcast daylight for Ship Box, derived from the pack's geometry while it loads.
// The front of a face is the side its winding turns away from.
class SceneLight {
public:
    static constexpr int size = 256, levels = 5;
    static constexpr uint16_t noPatch = 0xffff;

    void bake(const Assets& assets);

    // Light for a face that moves through the scene. The normal must face the viewer.
    static uint8_t surface(Vec3 normal, Vec3 sun, Vec3 up, unsigned scale);

    // Ground light at a world position, for whatever stands there.
    unsigned at(float x, float z) const {
        if (map_.empty()) return 255;
        return sample(int(((x - minX_) * inverseCell_ - 0.5f) * 256), int(((z - minZ_) * inverseCell_ - 0.5f) * 256), 0) >> 8;
    }

    // Bilinear ground light in 8.8 fixed point. Positions are 8.8 cells of the finest level.
    int sample(int x, int z, int level) const {
        const int span = size >> level;
        const int last = (span - 1) * 256 - 1;
        x = std::clamp(((x + 128) >> level) - 128, 0, last);
        z = std::clamp(((z + 128) >> level) - 128, 0, last);
        const uint8_t* row = map_.data() + offsets_[level] + (z >> 8) * span + (x >> 8);
        const int fx = x & 255, fz = z & 255;
        const int near = row[0] * 256 + (row[1] - row[0]) * fx;
        const int far = row[span] * 256 + (row[span + 1] - row[span]) * fx;
        return near + (((far - near) * fz) >> 8);
    }

    float cellSize() const { return cell_; }
    bool mapped() const { return !map_.empty(); }
    uint64_t bytes() const;

    std::vector<FaceLight> faces;
    std::vector<GroundPatch> patches;
    Vec3 sun;

private:
    std::vector<uint8_t> map_;
    int offsets_[levels]{};
    float minX_ = 0, minZ_ = 0, cell_ = 1, inverseCell_ = 1;
};

} // namespace fod
