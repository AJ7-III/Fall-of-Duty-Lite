#include "light.hpp"

#include <cmath>
#include <limits>

namespace fod {
namespace {

// Sky light fills the yard and a pale sun adds direction. Open ground sums to one.
constexpr float ambient = 0.74f, bounce = 0.78f, direct = 0.308f, exposure = 1.12f;
constexpr Vec3 sunward{0.4225f, 0.8450f, -0.3286f};

// Turf, kerbs and floor slabs stay below the clutter height and never shade the yard.
// A column that starts below the grounded height stands on the yard.
constexpr float clutter = 0.4f, grounded = 0.75f, eye = 0.12f, pad = 0.08f;
constexpr float groundTop = 0.08f, largeFace = 0.06f;
constexpr float escaped = std::numeric_limits<float>::infinity();

float shade(Vec3 normal, float sky, float sun) {
    const float hemisphere = bounce + (1 - bounce) * (0.5f + 0.5f * normal.y);
    return ambient * hemisphere * sky + direct * std::max(0.0f, dot(normal, sunward)) * sun;
}

uint8_t level(float light) { return uint8_t(std::clamp(light * exposure * 255 + 0.5f, 1.0f, 255.0f)); }

// Each column of the yard holds the height span its geometry occupies. A roof
// leaves the air below it open, and a wall or a crate reaches down to the ground.
struct Field {
    int size = 0;
    float minX = 0, minZ = 0, cell = 1, inverseCell = 1, top = 0;
    std::vector<float> low, high;

    int column(float x, float z) const {
        const float cx = (x - minX) * inverseCell, cz = (z - minZ) * inverseCell;
        if (!(cx >= 0 && cz >= 0 && cx < size && cz < size)) return -1;
        return int(cz) * size + int(cx);
    }

    bool solid(int i) const { return low[i] == 0 && high[i] > eye; }
    bool blocks(Vec3 p) const {
        const int i = column(p.x, p.z);
        return i >= 0 && p.y >= low[i] - pad && p.y <= high[i] + pad;
    }
    bool covered(Vec3 p) const {
        const int i = column(p.x, p.z);
        return i >= 0 && high[i] > p.y && low[i] - pad > p.y;
    }

    void mark(Vec3 p) {
        if (p.y < clutter) return;
        const int i = column(p.x, p.z);
        if (i < 0) return;
        low[i] = std::min(low[i], p.y);
        high[i] = std::max(high[i], p.y);
    }

    void add(Vec3 a, Vec3 b, Vec3 c) {
        if (std::max({a.y, b.y, c.y}) < clutter) return;
        const float maxX = minX + size * cell, maxZ = minZ + size * cell;
        if (std::max({a.x, b.x, c.x}) < minX || std::min({a.x, b.x, c.x}) > maxX ||
            std::max({a.z, b.z, c.z}) < minZ || std::min({a.z, b.z, c.z}) > maxZ) return;
        // Half-cell steps along two edges touch every column the face crosses.
        const Vec3 ab = b - a, ac = c - a;
        const float spacing = cell * 0.5f;
        const int rows = std::clamp(int(length(ab) / spacing) + 1, 1, 4096);
        const int columns = std::clamp(int(length(ac) / spacing) + 1, 1, 4096);
        for (int i = 0; i <= rows; ++i) {
            const float along = float(i) / rows;
            const Vec3 start = a + ab * along;
            const int last = int((1 - along) * columns);
            for (int j = 0; j <= last; ++j) mark(start + ac * (float(j) / columns));
            mark(start + ac * (1 - along));
        }
    }

    void settle() {
        for (size_t i = 0; i < low.size(); ++i) {
            if (high[i] < low[i]) continue;
            if (low[i] < grounded) low[i] = 0;
            top = std::max(top, high[i]);
        }
    }

    // Distance to the first blocker or to the ground. Open sky returns escaped.
    float reach(Vec3 from, Vec3 direction, float first, float last) const {
        float ground = escaped;
        if (direction.y < 0) ground = std::max(0.0f, from.y / -direction.y);
        const float end = std::min(last, ground);
        for (float t = first; t < end; t += cell) {
            if (blocks(from + direction * t)) return t;
        }
        return ground < last ? ground : escaped;
    }

    bool sunlit(Vec3 from) const {
        if (!size || from.y > top + pad) return true;
        return reach(from, sunward, 0, (top + pad - from.y) / sunward.y + cell) == escaped;
    }
};

// Share of the sky a patch of open ground sees over the things that stand around it.
float openSky(const Field& field, int cx, int cz, const float (*directions)[2]) {
    // Steps lengthen with distance. The weight fades to nothing at the last step.
    static constexpr float steps[] = {1, 2, 3, 4.5f, 6, 8, 10.5f, 13.5f, 17, 21, 25, 29};
    constexpr float range = 30;
    float open = 0;
    for (int d = 0; d < 8; ++d) {
        float horizon = 0;
        for (const float step : steps) {
            const int x = cx + int(std::lround(directions[d][0] * step));
            const int z = cz + int(std::lround(directions[d][1] * step));
            if (x < 0 || z < 0 || x >= field.size || z >= field.size) break;
            const int i = z * field.size + x;
            if (!field.solid(i)) continue;
            horizon = std::max(horizon, (field.high[i] - eye) / (step * field.cell) * (1 - step / range));
        }
        open += 1 - horizon / std::sqrt(1 + horizon * horizon);
    }
    return open / 8;
}

void soften(std::vector<float>& values, int size) {
    std::vector<float> row(size);
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < size; ++y) {
            float* line = values.data() + y * size;
            std::copy_n(line, size, row.begin());
            for (int x = 1; x + 1 < size; ++x) line[x] = (row[x - 1] + 2 * row[x] + row[x + 1]) * 0.25f;
        }
        for (int x = 0; x < size; ++x) {
            for (int y = 0; y < size; ++y) row[y] = values[y * size + x];
            for (int y = 1; y + 1 < size; ++y) values[y * size + x] = (row[y - 1] + 2 * row[y] + row[y + 1]) * 0.25f;
        }
    }
}

// Sky hidden near a point on a face, weighted toward close blockers.
float occlusion(const Field& field, Vec3 point, Vec3 normal, bool thorough) {
    constexpr float start = 0.35f, range = 2.2f, tilt = 0.5736f, lift = 0.8192f; // 55 degrees
    const Vec3 side = normalized(cross(normal, std::abs(normal.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
    const Vec3 other = cross(normal, side);
    const auto hidden = [&](Vec3 direction) {
        const float t = field.reach(point, direction, start, range);
        return t < range ? 1 - t / range : 0.0f;
    };
    float sum = 1.5f * hidden(normal), weight = 1.5f;
    const int rays = thorough ? 8 : 4;
    for (int i = 0; i < rays; ++i) {
        // Four rays follow the diagonals, where a wall meets the ground. Four more fill the gaps.
        constexpr float half = 0.70710678f;
        static constexpr float around[8][2] = {{half, half}, {-half, half}, {-half, -half}, {half, -half},
                                               {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
        sum += hidden(normal * tilt + (side * around[i][0] + other * around[i][1]) * lift);
        weight += 1;
    }
    return sum / weight;
}

float faceLight(const Field& field, Vec3 point, Vec3 normal, bool thorough) {
    constexpr float strength = 0.5f, enclosed = 0.75f;
    const Vec3 lifted = point + normal * 0.3f;
    float sky = 1 - strength * occlusion(field, point, normal, thorough);
    if (field.size && field.covered(lifted)) sky *= enclosed;
    const bool sun = dot(normal, sunward) > 0 && field.sunlit(lifted);
    return shade(normal, sky, sun ? 1.0f : 0.0f);
}

} // namespace

uint8_t SceneLight::surface(Vec3 normal, Vec3 sun, Vec3 up, unsigned scale) {
    // The same daylight as shade(), folded into one reciprocal for meshes that move every frame.
    constexpr float level = ambient * (bounce + (1 - bounce) * 0.5f) * exposure;
    constexpr float lift = ambient * (1 - bounce) * 0.5f * exposure, beam = direct * exposure;
    const float inverse = 1 / std::max(length(normal), 0.000001f);
    const float light = level + (lift * dot(normal, up) + beam * std::max(0.0f, dot(normal, sun))) * inverse;
    return uint8_t(std::clamp(light * float(scale) + 0.5f, 1.0f, 255.0f));
}

uint64_t SceneLight::bytes() const {
    return map_.capacity() + faces.capacity() * sizeof(FaceLight) + patches.capacity() * sizeof(GroundPatch);
}

void SceneLight::bake(const Assets& assets) {
    sun = sunward;
    const auto& world = assets.world;
    Field field;

    // The yard is wherever the rivals can walk, with room for the walls around it.
    Bounds yard{{1e9f, 0, 1e9f}, {-1e9f, 0, -1e9f}};
    for (const auto& node : assets.navNodes) {
        yard.min.x = std::min(yard.min.x, node.position.x);
        yard.min.z = std::min(yard.min.z, node.position.z);
        yard.max.x = std::max(yard.max.x, node.position.x);
        yard.max.z = std::max(yard.max.z, node.position.z);
    }
    if (!assets.navNodes.empty() && !world.triangles.empty()) {
        const float half = std::max(yard.max.x - yard.min.x, yard.max.z - yard.min.z) * 0.5f + 5;
        field.size = size;
        field.cell = cell_ = 2 * half / size;
        field.inverseCell = inverseCell_ = 1 / cell_;
        field.minX = minX_ = (yard.min.x + yard.max.x) * 0.5f - half;
        field.minZ = minZ_ = (yard.min.z + yard.max.z) * 0.5f - half;
        field.low.assign(size * size, escaped);
        field.high.assign(size * size, -escaped);
        for (const auto& triangle : world.triangles) {
            // Glass lets the sky through, and a pane can break.
            if (triangle.glass >= 0 || assets.materials[triangle.material].alpha < 255) continue;
            field.add(world.vertices[triangle.a].position, world.vertices[triangle.b].position,
                      world.vertices[triangle.c].position);
        }
        field.settle();
    }

    if (field.size) {
        // Turn the eight compass rays by a different angle in each cell. The
        // softening pass then averages the rays into an even penumbra.
        float directions[16][8][2];
        for (int turn = 0; turn < 16; ++turn) {
            for (int d = 0; d < 8; ++d) {
                const float angle = (d + turn / 16.0f) * (pi / 4);
                directions[turn][d][0] = std::cos(angle);
                directions[turn][d][1] = std::sin(angle);
            }
        }
        constexpr float footprint = 0.34f;
        std::vector<float> light(size * size);
        for (int z = 0; z < size; ++z) {
            for (int x = 0; x < size; ++x) {
                const int i = z * size + x;
                if (field.solid(i)) {
                    light[i] = footprint;
                    continue;
                }
                const Vec3 point{field.minX + (x + 0.5f) * field.cell, eye, field.minZ + (z + 0.5f) * field.cell};
                const unsigned turn = (unsigned(x) * 73856093u ^ unsigned(z) * 19349663u) >> 7;
                float sky = openSky(field, x, z, directions[turn & 15]);
                // A roof hides the zenith. Low cover hides more of the sky than a lamp arm.
                if (field.high[i] > eye) sky *= std::clamp((field.low[i] - eye) / 8, 0.45f, 1.0f);
                light[i] = shade({0, 1, 0}, sky, field.sunlit(point) ? 1.0f : 0.0f);
            }
        }
        soften(light, size);

        int total = 0;
        for (int l = 0; l < levels; ++l) {
            offsets_[l] = total;
            total += (size >> l) * (size >> l);
        }
        map_.resize(total);
        for (int z = 0; z < size; ++z) {
            for (int x = 0; x < size; ++x) {
                // Open ground continues past the rim of the map.
                const bool rim = x == 0 || z == 0 || x == size - 1 || z == size - 1;
                map_[z * size + x] = rim ? 255 : level(light[z * size + x]);
            }
        }
        for (int l = 1; l < levels; ++l) {
            const int span = size >> l;
            const uint8_t* fine = map_.data() + offsets_[l - 1];
            uint8_t* coarse = map_.data() + offsets_[l];
            for (int z = 0; z < span; ++z) {
                for (int x = 0; x < span; ++x) {
                    const uint8_t* from = fine + 2 * z * 2 * span + 2 * x;
                    coarse[z * span + x] = uint8_t((from[0] + from[1] + from[2 * span] + from[2 * span + 1] + 2) >> 2);
                }
            }
        }
    }

    faces.resize(world.triangles.size());
    for (size_t i = 0; i < world.triangles.size(); ++i) {
        const auto& triangle = world.triangles[i];
        const Vertex* corners[]{&world.vertices[triangle.a], &world.vertices[triangle.b], &world.vertices[triangle.c]};
        const Vec3 a = corners[0]->position, b = corners[1]->position, c = corners[2]->position;
        const Vec3 turned = cross(b - a, c - a);
        const float doubled = length(turned);
        auto& face = faces[i];
        if (doubled < 1e-9f) {
            face = {{level(ambient), level(ambient), level(ambient)}, level(ambient)};
            continue;
        }
        const Vec3 normal = turned * (-1 / doubled);

        // Flat textured faces at ground level take their light from the map.
        const float du1 = corners[1]->u - corners[0]->u, dv1 = corners[1]->v - corners[0]->v;
        const float du2 = corners[2]->u - corners[0]->u, dv2 = corners[2]->v - corners[0]->v;
        const float determinant = du1 * dv2 - du2 * dv1;
        if (field.size && normal.y > 0.999f && std::max({a.y, b.y, c.y}) <= groundTop &&
            assets.materials[triangle.material].texture >= 0 && std::abs(determinant) > 1e-9f &&
            patches.size() < noPatch) {
            const float scale = 256 * inverseCell_ / determinant;
            GroundPatch patch{};
            patch.xu = ((b.x - a.x) * dv2 - (c.x - a.x) * dv1) * scale;
            patch.xv = ((c.x - a.x) * du1 - (b.x - a.x) * du2) * scale;
            patch.x0 = ((a.x - minX_) * inverseCell_ - 0.5f) * 256 - patch.xu * corners[0]->u - patch.xv * corners[0]->v;
            patch.zu = ((b.z - a.z) * dv2 - (c.z - a.z) * dv1) * scale;
            patch.zv = ((c.z - a.z) * du1 - (b.z - a.z) * du2) * scale;
            patch.z0 = ((a.z - minZ_) * inverseCell_ - 0.5f) * 256 - patch.zu * corners[0]->u - patch.zv * corners[0]->v;
            face = {{uint8_t(patches.size() & 255), uint8_t(patches.size() >> 8), 0}, 0};
            patches.push_back(patch);
            continue;
        }

        const Vec3 center = (a + b + c) / 3;
        if (doubled * 0.5f >= largeFace) {
            // Corners carry their own light so a wall darkens toward the ground.
            for (int corner = 0; corner < 3; ++corner) {
                face.front[corner] = level(faceLight(field, corners[corner]->position, normal, true));
            }
        } else {
            face.front[0] = face.front[1] = face.front[2] = level(faceLight(field, center, normal, false));
        }
        face.back = level(faceLight(field, center, normal * -1, false));
    }
}

} // namespace fod
