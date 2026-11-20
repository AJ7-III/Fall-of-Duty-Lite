#include "mission_internal.hpp"

namespace fod::mission_detail {

using namespace game_detail;

namespace {
constexpr uint16_t plasmaImpactColor = 0x4fdf;
}

void MissionController::launchPlasma(Vec3 origin, Vec3 dir, bool charged, float damage, int ally, float range,
                                      const Snapshot& snap, float barrel) {
    for (size_t i = 0; i < state_.plasma.size(); ++i) {
        auto& cloud = state_.plasma[i];
        if (cloud.radius > 0.f) continue;
        // The world is static apart from glass. Cache the first obstacle so flying shots only
        // check moving actors each tick. Trace through the barrel too when the gun is against cover.
        const Vec3 bore = origin - dir * barrel;
        auto obstruction = world_.shot(bore, dir, snap.brokenGlass);
        obstruction.distance -= barrel;
        cloud = {origin, dir, plasmaRadius * (charged ? 1.24f : 1.f), charged};
        plasmaFlights_[i] = {obstruction, range, damage, ally};
        return;
    }
}

void MissionController::updatePlasma(Snapshot& snap, float dt, MissionHost& host) {
    for (size_t i = 0; i < state_.plasma.size(); ++i) {
        auto& cloud = state_.plasma[i];
        if (cloud.radius == 0.f) continue;
        auto& flight = plasmaFlights_[i];
        const int glass = flight.obstruction.glass;
        if (glass >= 0 && glass < 64 && (snap.brokenGlass & (uint64_t(1) << glass))) {
            flight.obstruction = world_.shot(cloud.position, cloud.direction, snap.brokenGlass);
        }
        const float step = std::min(flight.remaining, (cloud.charged ? 34.f : 28.f) * dt);
        const float wall = flight.obstruction.hit ? flight.obstruction.distance : infinity;
        float distance = std::min(step, wall);
        int target = -2;
        const auto test = [&](int slot, Vec3 feet, float radius, float height) {
            const Vec3 bottom = feet - Vec3{0, cloud.radius, 0};
            const float width = radius + cloud.radius, span = height + 2.f * cloud.radius;
            const Vec3 delta = cloud.position - bottom;
            const bool inside = planarSquared(delta) < width * width && delta.y >= 0.f && delta.y <= span;
            const float hit = inside ? 0.f : cylinder(cloud.position, cloud.direction, bottom, width, span);
            if (hit < distance) {
                target = slot;
                distance = hit;
            }
        };
        if (flight.damage > 0.f && !snap.dead) test(-1, snap.player.position, .40f, snap.eyeHeight + .15f);
        if (flight.ally >= 0 && !agents_[flight.ally].isDead()) {
            const auto& ally = agents_[flight.ally];
            test(flight.ally, ally.actor.position, .38f, flight.ally == 0 ? 1.85f : 1.12f);
        }
        const Vec3 position = cloud.position + cloud.direction * distance;
        if (target != -2 || wall <= step || step == flight.remaining) {
            // Pull a wall impact slightly forward so its soft particles survive depth testing.
            const Vec3 flash = target == -2 && wall <= step ? position - cloud.direction * .06f : position;
            addTracer(flash, flash - cloud.direction, cloud.charged ? chargedPlasmaTint : plasmaImpactColor,
                      TracerStyle::PlasmaImpact, cloud.charged ? 1.35f : 1.f);
            if (target == -1) {
                hurtPlayer(flight.damage, position - cloud.direction, snap, host);
            } else if (target > 0) {
                damageAgent(target, flight.damage * .7f, false, false, snap, host);
            } else if (target == -2 && wall <= step && flight.obstruction.glass >= 0) {
                host.hitWorld(flight.obstruction, position);
            }
            cloud.radius = 0.f;
            continue;
        }
        cloud.position = position;
        cloud.traveled += step;
        flight.remaining -= step;
        flight.obstruction.distance -= step;
    }
}

} // namespace fod::mission_detail
