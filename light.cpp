#include "light.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace fod {
namespace {

inline float fastInvSqrt(float x) {
    uint32_t i;
    std::memcpy(&i, &x, sizeof(i));
    i = 0x5f3759dfu - (i >> 1);
    float y;
    std::memcpy(&y, &i, sizeof(y));
    return y * (1.5f - 0.5f * x * y * y);
}

// Sky light fills the yard and a pale sun adds direction. Open ground sums to one.
constexpr Vec3 sunward{0.4225f, 0.8450f, -0.3286f};

struct LightProfile {
    float ambient, bounce, direct, exposure;
    Vec3 sun;
};
constexpr LightProfile daylightProfile{.69f, .69f, .38f, 1.12f, sunward};
// Colorado dusk sunset: low west-northwest sun casting long alpenglow light against moody twilight shadows.
constexpr LightProfile basinProfile{.66f, .59f, .43f, 1.05f, {-.49f, .57f, .66f}};
constexpr LightProfile scoutProfile{.53f, .56f, .19f, .99f, {-.42f, .73f, .54f}};
constexpr LightProfile duskProfile{.59f, .69f, .52f, 1.1f, {-0.8190f, 0.3396f, 0.4594f}};

// Turf, kerbs and floor slabs stay below the clutter height and never shade the yard.
// A column that starts below the grounded height stands on the yard.
constexpr float clutter = 0.4f, grounded = 0.75f, eye = 0.12f, pad = 0.08f;
constexpr float groundTop = 0.08f, largeFace = 0.06f;
constexpr float escaped = std::numeric_limits<float>::infinity();

float shade(Vec3 normal, float sky, float sun, const LightProfile& p) {
    const float hemisphere = p.bounce + (1 - p.bounce) * (0.5f + 0.5f * normal.y);
    return p.ambient * hemisphere * sky + p.direct * std::max(0.0f, dot(normal, p.sun)) * sun;
}

uint8_t level(float light, const LightProfile& p) {
    return uint8_t(std::clamp(light * p.exposure * 255 + 0.5f, 1.0f, 255.0f));
}

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
        const float invRows = 1.0f / float(rows), invCols = 1.0f / float(columns);
        const float ax = (a.x - minX) * inverseCell, az = (a.z - minZ) * inverseCell;
        const float abx = ab.x * inverseCell * invRows, aby = ab.y * invRows, abz = ab.z * inverseCell * invRows;
        const float acx = ac.x * inverseCell * invCols, acy = ac.y * invCols, acz = ac.z * inverseCell * invCols;
        const auto markGrid = [&](float gx, float gy, float gz) {
            if (gy < clutter || !(gx >= 0 && gz >= 0 && gx < size && gz < size)) return;
            const int idx = int(gz) * size + int(gx);
            if (gy < low[idx]) low[idx] = gy;
            if (gy > high[idx]) high[idx] = gy;
        };
        for (int i = 0; i <= rows; ++i) {
            const float sx = ax + abx * i, sy = a.y + aby * i, sz = az + abz * i;
            const float rem = 1.0f - float(i) * invRows;
            const int last = int(rem * float(columns));
            float gx = sx, gy = sy, gz = sz;
            for (int j = 0; j <= last; ++j, gx += acx, gy += acy, gz += acz) markGrid(gx, gy, gz);
            markGrid(sx + ac.x * inverseCell * rem, sy + ac.y * rem, sz + ac.z * inverseCell * rem);
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
        float gx = (from.x + direction.x * first - minX) * inverseCell;
        float gy = from.y + direction.y * first;
        float gz = (from.z + direction.z * first - minZ) * inverseCell;
        const float dgx = direction.x, dgy = direction.y * cell, dgz = direction.z;
        const float fSize = float(size);
        for (float t = first; t < end; t += cell, gx += dgx, gy += dgy, gz += dgz) {
            if (gx >= 0 && gz >= 0 && gx < fSize && gz < fSize) {
                const int i = int(gz) * size + int(gx);
                if (gy >= low[i] - pad && gy <= high[i] + pad) return t;
            }
        }
        return ground < last ? ground : escaped;
    }

    float sunExposure(Vec3 from, Vec3 sunDir) const {
        if (!size || from.y > top + pad) return 1.f;
        const float blocker = reach(from, sunDir, 0, (top + pad - from.y) / sunDir.y + cell);
        // Distant blockers leave a little more light around a patch than nearby cover.
        return blocker == escaped ? 1.f : std::min(.23f, blocker * .034f);
    }
};

struct SkyStep {
    int8_t dx, dz;
    float weight; // (1 - step / range) / (step * cell)
};

// Share of the sky a patch of open ground sees over the things that stand around it.
float openSky(const Field& field, int cx, int cz, const SkyStep (*rays)[12]) {
    const int size = field.size;
    const float* low = field.low.data();
    const float* high = field.high.data();
    float open = 0;
    for (int d = 0; d < 8; ++d) {
        float horizon = 0;
        const SkyStep* ray = rays[d];
        for (int s = 0; s < 12; ++s) {
            const int x = cx + ray[s].dx;
            const int z = cz + ray[s].dz;
            if (unsigned(x) >= unsigned(size) || unsigned(z) >= unsigned(size)) break;
            const int i = z * size + x;
            if (low[i] != 0 || high[i] <= eye) continue;
            const float h = (high[i] - eye) * ray[s].weight;
            if (h > horizon) horizon = h;
        }
        open += 1.0f - horizon * fastInvSqrt(1.0f + horizon * horizon);
    }
    return open * 0.125f;
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

float faceLight(const Field& field, Vec3 point, Vec3 normal, bool thorough, const LightProfile& p) {
    constexpr float strength = 0.5f, enclosed = 0.75f;
    const Vec3 lifted = point + normal * 0.3f;
    float sky = 1 - strength * occlusion(field, point, normal, thorough);
    if (field.size && field.covered(lifted)) sky *= enclosed;
    const float sun = dot(normal, p.sun) > 0 ? field.sunExposure(lifted, p.sun) : 0.f;
    return shade(normal, sky, sun, p);
}

float lampLight(const Field& field, const std::vector<BakedLight>& lamps, Vec3 point, Vec3 normal) {
    float light = 0;
    const Vec3 sample = point + normal * 0.18f;
    for (const auto& lamp : lamps) {
        const Vec3 offset = lamp.position - sample;
        const float distanceSquared = dot(offset, offset);
        const float toward = dot(normal, offset);
        if (toward <= 0 || distanceSquared >= lamp.radius * lamp.radius) continue;
        const float distance = std::sqrt(distanceSquared);
        if (field.size && field.reach(sample, offset / distance, field.cell, distance - field.cell * 1.8f) != escaped) continue;
        const float falloff = 1 - distance / lamp.radius;
        light += lamp.strength * falloff * falloff * (0.35f + 0.65f * toward / distance);
    }
    return light;
}

} // namespace

uint8_t SurfaceLight::level(Vec3 normal, unsigned scale) const {
    const float lenSq = dot(normal, normal);
    const float inverse = lenSq > 1e-12f ? fastInvSqrt(lenSq) : 1e6f;
    const float light = fill + (dot(normal, lift) + std::max(0.f, dot(normal, beam))) * inverse;
    return uint8_t(std::clamp(light * float(scale) + 0.5f, 1.0f, 255.0f));
}

uint64_t SceneLight::bytes() const {
    return map_.capacity() + faces.capacity() * sizeof(FaceLight) + patches.capacity() * sizeof(GroundPatch);
}

void SceneLight::bake(const Assets& assets, const ArenaGeometry& geometry) {
    const bool dusk = assets.arena == Arena::BriarHouse && assets.briarHouseMode == BriarHouseMode::Invasion;
    const LightProfile& profile = &geometry == assets.scoutInterior.get() ? scoutProfile :
                                  &geometry == assets.timberline.get() ? basinProfile : dusk ? duskProfile : daylightProfile;
    sun = profile.sun;
    movingCoefficients_ = {profile.ambient * (.5f + .5f * profile.bounce) * profile.exposure,
                           profile.ambient * .5f * (1.f - profile.bounce) * profile.exposure,
                           profile.direct * profile.exposure};
    const auto& world = geometry.world;
    Field field;

    // The yard is wherever the rivals can walk, with room for the walls around it.
    Bounds yard{{1e9f, 0, 1e9f}, {-1e9f, 0, -1e9f}};
    for (const auto& node : geometry.navNodes) {
        yard.min.x = std::min(yard.min.x, node.position.x);
        yard.min.z = std::min(yard.min.z, node.position.z);
        yard.max.x = std::max(yard.max.x, node.position.x);
        yard.max.z = std::max(yard.max.z, node.position.z);
    }
    if (!geometry.navNodes.empty() && !world.triangles.empty()) {
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
        // Let instanced Ship Box trucks and cars cast real directional sunset shadows onto roads & lawns.
        for (const auto& inst : geometry.propInstances) {
            const float cs = std::cos(inst.yaw), sn = std::sin(inst.yaw);
            const Vec3 along{cs, 0, -sn}, side{sn, 0, cs};
            const float hl = inst.halfLen * 0.92f, hw = inst.halfWidth * 0.88f, h = inst.height;
            const Vec3 p0 = inst.position - along * hl - side * hw + Vec3{0, h, 0};
            const Vec3 p1 = inst.position + along * hl - side * hw + Vec3{0, h, 0};
            const Vec3 p2 = inst.position + along * hl + side * hw + Vec3{0, h, 0};
            const Vec3 p3 = inst.position - along * hl + side * hw + Vec3{0, h, 0};
            const Vec3 g0{p0.x, 0.42f, p0.z}, g1{p1.x, 0.42f, p1.z}, g2{p2.x, 0.42f, p2.z}, g3{p3.x, 0.42f, p3.z};
            field.add(p0, p1, p2);
            field.add(p0, p2, p3);
            field.add(g0, g1, g2);
            field.add(g0, g2, g3);
        }
        field.settle();
    }

    if (field.size) {
        // Precompute integer step offsets and horizon weights for all 16 rotated octants.
        static constexpr float steps[12] = {1, 2, 3, 4.5f, 6, 8, 10.5f, 13.5f, 17, 21, 25, 29};
        constexpr float range = 30;
        SkyStep skyRays[16][8][12];
        for (int turn = 0; turn < 16; ++turn) {
            for (int d = 0; d < 8; ++d) {
                const float angle = (d + turn / 16.0f) * (pi / 4);
                const float dx = std::cos(angle), dz = std::sin(angle);
                for (int s = 0; s < 12; ++s) {
                    skyRays[turn][d][s] = {
                        int8_t(std::lround(dx * steps[s])),
                        int8_t(std::lround(dz * steps[s])),
                        (1.0f - steps[s] / range) / (steps[s] * field.cell)
                    };
                }
            }
        }
        constexpr float footprint = 0.34f;
        std::vector<float> light(size * size);
        const bool hasLamps = !geometry.lamps.empty();
        for (int z = 0; z < size; ++z) {
            const float worldZ = field.minZ + (z + 0.5f) * field.cell;
            for (int x = 0; x < size; ++x) {
                const int i = z * size + x;
                if (field.solid(i)) {
                    light[i] = footprint;
                    continue;
                }
                const Vec3 point{field.minX + (x + 0.5f) * field.cell, eye, worldZ};
                const unsigned turn = (unsigned(x) * 73856093u ^ unsigned(z) * 19349663u) >> 7;
                float sky = openSky(field, x, z, skyRays[turn & 15]);
                // A roof hides the zenith. Low cover hides more of the sky than a lamp arm.
                if (field.high[i] > eye) sky *= std::clamp((field.low[i] - eye) / 8, 0.45f, 1.0f);
                light[i] = shade({0, 1, 0}, sky, field.sunExposure(point, profile.sun), profile);
                if (hasLamps) light[i] += lampLight(field, geometry.lamps, point, {0, 1, 0});
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
                map_[z * size + x] = rim ? (dusk ? 220 : 255) : level(light[z * size + x], profile);
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
        auto& face = faces[i];
        if (assets.materials[triangle.material].emissive) {
            face = {{255, 255, 255}, 255};
            continue;
        }
        const Vertex* corners[]{&world.vertices[triangle.a], &world.vertices[triangle.b], &world.vertices[triangle.c]};
        const Vec3 a = corners[0]->position, b = corners[1]->position, c = corners[2]->position;
        const Vec3 turned = cross(b - a, c - a);
        const float doubled = length(turned);
        if (doubled < 1e-9f) {
            face = {{level(profile.ambient, profile), level(profile.ambient, profile), level(profile.ambient, profile)},
                    level(profile.ambient, profile)};
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
        const auto lit = [&](Vec3 point, Vec3 direction, bool thorough) {
            const float daylight = faceLight(field, point, direction, thorough, profile);
            return level(daylight + lampLight(field, geometry.lamps, point, direction), profile);
        };
        if (doubled * 0.5f >= largeFace) {
            // Corners carry their own light so a wall darkens toward the ground.
            for (int corner = 0; corner < 3; ++corner) {
                face.front[corner] = lit(corners[corner]->position, normal, true);
            }
        } else {
            face.front[0] = face.front[1] = face.front[2] = lit(center, normal, false);
        }
        face.back = lit(center, normal * -1, false);
    }
}

} // namespace fod
