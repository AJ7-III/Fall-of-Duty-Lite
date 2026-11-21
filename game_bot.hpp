#pragma once

#include "game_data.hpp"
#include "game_random.hpp"
#include "game_world.hpp"

namespace fod::game_detail {

constexpr float botEye = 1.62f;
constexpr float botMuzzle = botEye * .88f;
constexpr double deg = 3.14159265358979323846 / 180;
constexpr double forever = std::numeric_limits<double>::infinity();

struct BotWeapon {
    Sound sound;
    int mag;
    float damage, spread;
    double interval, reload;
    float range[4];
};

constexpr BotWeapon botWeapons[] = {
    {Sound::Mp44, 30, 32, 1.4f, .1, 2.4, {-1, 0, 12, 30}},
    {Sound::Pistol, 12, 24, 1.1f, .26, 2.0, {-1, 0, 6, 16}},
};

inline float rangeUtility(float d, const BotWeapon& weapon) {
    const auto& r = weapon.range;
    if (d <= r[0] || d >= r[3]) return .05f;
    if (d < r[1]) return .05f + .95f * ((d - r[0]) / (r[1] - r[0]));
    if (d > r[2]) return .05f + .95f * ((r[3] - d) / (r[3] - r[2]));
    return 1;
}

struct BotTarget {
    Vec3 position, velocity;
    float eye;
    bool dead, sprint, prone, crouch, fired;
};

struct BotRound {
    Vec3 muzzle{}, direction{};
    Sound sound = Sound::Mp44;
    float damage = 0;
    bool fired = false;
};

struct SightSample { float height, lateral; int rank; };
constexpr SightSample sightSamples[] = {
    {1, 0, 3}, {.80f, 0, 0}, {.74f, -.30f, 1}, {.74f, .30f, 2}, {.45f, 0, 4}, {.14f, 0, 5},
};

inline Vec3 silhouette(Vec3 from, const BotTarget& target, int i) {
    const auto& s = sightSamples[i];
    if (!s.lateral) return atHeight(target.position, target.eye * s.height);
    const Vec3 d = target.position - from;
    const float distance = planar(d);
    Vec3 sample = atHeight(target.position, target.eye * s.height);
    if (distance > .0001f) sample += Vec3{d.z, 0, -d.x} * (s.lateral / distance);
    return sample;
}

class Navigation {
public:
    explicit Navigation(const ArenaGeometry& geometry) { useGeometry(geometry); }

    void useGeometry(const ArenaGeometry& geometry) {
        if (geometry_ == &geometry) return;
        geometry_ = &geometry;
        size_ = geometry.navNodes.size();
        g_.resize(size_);
        f_.resize(size_);
        parent_.resize(size_);
        state_.resize(size_);
        heap_.resize(size_);
        heapPosition_.resize(size_);
        orderedByX_ = std::is_sorted(geometry.navNodes.begin(), geometry.navNodes.end(),
            [](const NavNode& a, const NavNode& b) { return a.position.x < b.position.x; });
    }

    bool valid(int n) const { return n >= 0 && n < int(size_) && geometry_->navNodes[n].walkable; }
    int size() const { return int(size_); }
    const NavNode& node(int i) const { return geometry_->navNodes[i]; }

    int nearest(Vec3 p) const {
        constexpr int side = 26;
        if (size_ == side * side) {
            const int ix = int(std::floor((p.x + 15.) / step + .5));
            const int iz = int(std::floor((p.z + 15.) / step + .5));
            for (int ring = 0; ring <= 3; ++ring) {
                int best = -1;
                float score = infinity;
                for (int dx = -ring; dx <= ring; ++dx) {
                    for (int dz = -ring; dz <= ring; ++dz) {
                        if (std::max(std::abs(dx), std::abs(dz)) != ring ||
                            ix + dx < 0 || ix + dx >= side || iz + dz < 0 || iz + dz >= side) continue;
                        const int i = (ix + dx) * side + iz + dz;
                        if (!valid(i)) continue;
                        const float distance = planarSquared(node(i).position - p);
                        if (distance < score) {
                            score = distance;
                            best = i;
                        }
                    }
                }
                if (best >= 0) return best;
            }
            return -1;
        }
        if (orderedByX_) return nearestInColumns(p);
        int best = -1;
        float score = infinity;
        for (int i = 0; i < int(size_); ++i) {
            if (!valid(i)) continue;
            const float distance = planarSquared(node(i).position - p);
            if (distance < score) {
                score = distance;
                best = i;
            }
        }
        return best;
    }

    bool path(int first, int goal, std::vector<int>& out) {
        out.clear();
        if (!valid(first) || !valid(goal)) return false;
        if (first == goal) {
            out.push_back(first);
            return true;
        }
        std::fill(state_.begin(), state_.end(), 0);
        used_ = 0;
        g_[first] = 0;
        f_[first] = float(distance(first, goal));
        state_[first] = 1;
        sift(first, used_++);
        while (used_ > 0) {
            const int here = pop();
            state_[here] = 2;
            if (here == goal) {
                for (int i = goal; i != first; i = parent_[i]) out.push_back(i);
                out.push_back(first);
                std::reverse(out.begin(), out.end());
                return true;
            }
            const auto& from = node(here);
            for (uint32_t i = from.first; i < from.first + from.count; ++i) {
                const auto& edge = geometry_->navEdges[i];
                const int to = int(edge.to);
                if (state_[to] == 2) continue;
                const double cost = double(g_[here]) + edgeCost(edge.cost);
                if (cost >= (state_[to] == 1 ? g_[to] : infinity)) continue;
                g_[to] = float(cost);
                f_[to] = float(cost + distance(to, goal));
                parent_[to] = here;
                if (state_[to] == 1) {
                    sift(to, heapPosition_[to]);
                } else {
                    state_[to] = 1;
                    sift(to, used_++);
                }
            }
        }
        return false;
    }

private:
    static constexpr double step = 1.2;
    static constexpr double diagonal = step * 1.4142135623730951;
    const ArenaGeometry* geometry_ = nullptr;
    size_t size_;
    std::vector<float> g_, f_;
    std::vector<int> parent_;
    std::vector<uint8_t> state_;
    std::vector<int> heap_, heapPosition_;
    bool orderedByX_;
    int used_ = 0;

    int nearestInColumns(Vec3 p) const {
        const auto& nodes = geometry_->navNodes;
        const auto split = std::lower_bound(nodes.begin(), nodes.end(), p.x,
                                            [](const NavNode& n, float x) { return n.position.x < x; });
        const int first = int(split - nodes.begin());
        int best = -1;
        float bestDistance = infinity;
        const auto consider = [&](int i) {
            const auto& candidate = nodes[i];
            const float dx = candidate.position.x - p.x;
            if (dx * dx > bestDistance) return false;
            if (candidate.walkable) {
                const float distance = planarSquared(candidate.position - p);
                // Keep the first node on a tie, as in the original linear search.
                if (distance < bestDistance || (distance == bestDistance && i < best)) {
                    bestDistance = distance;
                    best = i;
                }
            }
            return true;
        };
        for (int i = first; i < int(size_) && consider(i); ++i) {}
        for (int i = first - 1; i >= 0 && consider(i); --i) {}
        return best;
    }

    static double edgeCost(float packed) {
        // The browser rounds the accumulated costs to Float32, with Number
        // precision for each edge and the heuristic. Restore the two canonical
        // grid costs before accumulation so equal-cost routes keep their order.
        if (packed == float(step)) return step;
        if (packed == float(diagonal)) return diagonal;
        return packed;
    }

    double distance(int from, int to) const {
        const double dx = double(node(from).position.x) - node(to).position.x;
        const double dz = double(node(from).position.z) - node(to).position.z;
        return std::sqrt(dx * dx + dz * dz);
    }

    void sift(int node, int index) {
        while (index > 0) {
            const int preceding = (index - 1) / 2;
            const int parent = heap_[preceding];
            if (f_[parent] <= f_[node]) break;
            heap_[index] = parent;
            heapPosition_[parent] = index;
            index = preceding;
        }
        heap_[index] = node;
        heapPosition_[node] = index;
    }

    int pop() {
        const int top = heap_[0];
        const int last = heap_[--used_];
        if (used_ == 0) return top;
        int i = 0;
        while (i < used_ / 2) {
            int child = 2 * i + 1;
            if (child + 1 < used_ && f_[heap_[child + 1]] < f_[heap_[child]]) ++child;
            const int n = heap_[child];
            if (f_[last] <= f_[n]) break;
            heap_[i] = n;
            heapPosition_[n] = i;
            i = child;
        }
        heap_[i] = last;
        heapPosition_[last] = i;
        return top;
    }
};

class Bot {
public:
    Bot(Navigation& nav, const World& world, Random& random) : nav_(nav), world_(world), random_(random) {
        path_.reserve(nav_.size());
    }

    Actor actor{};
    Vec3 velocity{};
    double radar = 0;
    bool dead = false, freeze = false, pendingAlert = false;

    void reset(Vec3 pos, float yaw, const game_data::Difficulty& d, double now, bool newMatch) {
        const double think = newMatch ? 0 : brain_.think;
        brain_ = {};
        brain_.think = think;
        brain_.aimYaw = brain_.idleYaw = yaw;
        brain_.idleAt = now;
        brain_.nextSuspicion = now + random_.between("bot.suspicion.initial", .15, .4);
        brain_.patrolAt = now + random_.between("bot.patrol.initial", .1, .35);
        brain_.weaponAt = now + 1;
        path_.clear();
        velocity = {};
        actor = {};
        actor.position = pos;
        actor.yaw = yaw;
        actor.pitch = .5f;
        actor.health = float(100 * d.health);
        dead = pendingAlert = false;
        radar = 0;
    }

    void spawnIntuition(const BotTarget& target, double error, double now) {
        Vec3 fix = target.position;
        fix.x += float(random_.between("bot.spawnIntuition.x", -error, error));
        fix.z += float(random_.between("bot.spawnIntuition.z", -error, error));
        const auto past = now - random_.between("bot.spawnIntuition.age", 1.5, 7);
        intel(fix, {}, now, past, true, .45);
    }

    void retarget() {
        auto& b = brain_;
        b.visible = b.rayClear = b.hasIntel = b.freshIntel = b.hasSuspicion = b.holding = b.reached = false;
        b.lastSeen = b.intelAt = -forever;
        b.detection = b.threat = 0;
        b.burst = b.pathIndex = 0;
        b.destination = -1;
        path_.clear();
    }

    BotRound update(const BotTarget& target, const game_data::Difficulty& tuning, double now, double dt, Vec3 separation = {}) {
        radar = std::max(0., radar - dt);
        if (dead || freeze) return {};
        auto& b = brain_;
        if (b.reload > 0 && (b.reload -= dt) <= 0) b.clip[b.weapon] = botWeapons[b.weapon].mag;
        b.swap -= dt;
        b.fire -= dt;
        b.burstTimer -= dt;
        perceive(target, tuning, now, dt);
        b.think -= dt;
        if (b.think <= 0) {
            b.think = .12;
            think(target, tuning, now);
        }
        BotRound round = combat(target, tuning, now, dt);
        motor(target, tuning, now, dt, separation);
        return round;
    }

private:
    enum class Mode { Patrol, Hunt, Search, Combat, Retreat, Reload };
    struct Brain {
        Mode mode = Mode::Patrol;
        Vec3 last{}, lastVel{}, suspicion{}, suspicionVel{}, evaluated{}, lastMove{};
        std::array<int, 7> recent{};
        int recentCursor = 0, recentCount = 0, lastPatrol = -1;
        int destination = -1, previous = -1, pathIndex = 0, searchStops = 0;
        int aimSample = 1, weapon = 0, clip[2] = {30, 12}, burst = 0;
        bool visible = false, hasIntel = false, freshIntel = false, hasSuspicion = false, rayClear = false;
        bool reached = false, holding = false, sprint = false, flank = false, moving = false;
        double lastSeen = -forever, intelAt = -forever, suspicionAt = -forever, nextSuspicion = 0;
        double think = 0, holdUntil = 0, retreatUntil = 0, patrolAt = 0, weaponAt = 0;
        double reload = 0, swap = 0, fire = 0, burstTimer = 0, react = 0;
        double aimYaw = 0, aimPitch = 0, errYaw = 0, errPitch = 0, idleYaw = 0, idleAt = 0;
        double detection = 0, threat = 0, hurt = 0, distance = 0, stuck = 0, reversing = 0, shoulder = 0;
        float walkCycle = 0, runCycle = 0;
    };

    Navigation& nav_;
    const World& world_;
    Random& random_;
    Brain brain_{};
    std::vector<int> path_;

    void intel(Vec3 fix, Vec3 velocity, double now, double at, bool fresh, double detection = 0) {
        auto& b = brain_;
        b.last = b.suspicion = fix;
        b.lastVel = b.suspicionVel = {velocity.x, 0, velocity.z};
        b.hasIntel = b.hasSuspicion = true;
        b.suspicionAt = b.intelAt = at;
        b.nextSuspicion = now + random_.between("bot.intel.nextSuspicion", 2.5, 4);
        b.freshIntel = b.freshIntel || fresh;
        b.detection = std::max(b.detection, detection);
    }

    void error(const game_data::Difficulty& d, double scale) {
        const double e = d.aimError * deg * scale;
        brain_.errYaw = random_.between("bot.aimError.yaw", -e, e);
        brain_.errPitch = random_.between("bot.aimError.pitch", -e * .7, e * .7);
    }

    void perceive(const BotTarget& t, const game_data::Difficulty& d, double now, double dt) {
        auto& b = brain_;
        if (pendingAlert) {
            pendingAlert = false;
            b.hurt = std::min(1., b.hurt + .6);
            if (!t.dead) {
                intel(t.position, t.velocity, now, now, true, 1);
                b.holdUntil = 0;
            }
        }
        const Vec3 offset = t.position - actor.position;
        b.distance = planar(offset);
        const double since = now - b.lastSeen;
        double exposure = 0;
        const bool alert = b.detection > .5 || since < 6;
        const double front = (std::sin(actor.yaw) * offset.x + std::cos(actor.yaw) * offset.z);
        if (!t.dead && b.distance < d.vision && b.distance > .001 &&
            front / b.distance > (alert ? std::cos(1.31) : std::cos(.96))) {
            int bestRank = 6, seen = 0;
            const Vec3 eye = atHeight(actor.position, botEye);
            for (int i = 0; i < 6; ++i) {
                if (!world_.blocked(eye, silhouette(actor.position, t, i))) {
                    ++seen;
                    if (sightSamples[i].rank < bestRank) {
                        bestRank = sightSamples[i].rank;
                        b.aimSample = i;
                    }
                }
            }
            exposure = seen / 6.;
        }
        const bool seen = exposure >= d.exposure;
        if (seen) {
            double fill = (1.9 - 1.5 * (b.distance / d.vision)) * (.45 + .55 * exposure);
            fill *= t.sprint ? 1.6 : t.prone ? .35 : t.crouch ? .55 : 1;
            if (since < 6) fill *= 4;
            b.detection = std::min(1., b.detection + fill * dt);
        } else {
            b.detection = std::max(0., b.detection - .5 * dt);
        }
        const bool had = b.visible;
        b.visible = seen && b.detection >= 1;
        if (b.visible) {
            if (!had) {
                const double recontact = since < 6 ? .35 : 1;
                b.react = d.reactionMs / 1000 * random_.between("bot.reaction", .8, 1.3) * recontact;
                error(d, recontact == 1 ? 1.5 : .8);
            }
            intel(t.position, t.velocity, now, now, false);
            b.lastSeen = now;
        }
        if (t.fired && !t.dead) {
            const double over = std::max(0., b.distance - d.hearing);
            const double err = std::min(5., over * .25);
            Vec3 fix = t.position;
            fix.x += float(random_.between("bot.hearing.x", -err, err));
            fix.z += float(random_.between("bot.hearing.z", -err, err));
            intel(fix, t.velocity, now, now, true, over > 0 ? .55 : .75);
        }
        b.hurt *= std::exp(-1.2 * dt);
        b.threat = (b.visible ? 1 - b.distance / d.vision : 0) + b.hurt;
    }

    void suspicion(const BotTarget& t, const game_data::Difficulty& d, double now, bool force = false) {
        auto& b = brain_;
        if (t.dead || (b.hasIntel && !force) || (b.hasSuspicion && !force && now < b.nextSuspicion)) return;
        const double err = std::max(3.5, 10 - d.aggression * 5 + std::min(3., b.distance * .12));
        b.suspicion = t.position;
        b.suspicion.x += float(random_.between("bot.suspicion.x", -err, err));
        b.suspicion.z += float(random_.between("bot.suspicion.z", -err, err));
        b.suspicionVel = t.velocity * float(.45 + d.aggression * .35);
        b.hasSuspicion = true;
        b.suspicionAt = now;
        b.nextSuspicion = now + random_.between("bot.suspicion.next", 2, 3.8) * (1.25 - d.aggression * .45);
    }

    void chooseWeapon(const game_data::Difficulty& d, double now) {
        auto& b = brain_;
        if (b.reload > 0 || b.swap > 0) return;
        const int w = b.weapon, other = 1 - w;
        if (b.clip[w] == 0) {
            if (b.visible && b.distance < 9 && b.clip[other] > 0 && random_.draw("bot.weapon.quickSwap") < d.switchSkill) {
                b.weapon = other;
                b.swap = .6;
            } else {
                b.reload = botWeapons[w].reload;
            }
            b.burst = 0;
            return;
        }
        if (!b.visible && now - b.lastSeen > 3 && b.clip[w] < botWeapons[w].mag * .45 &&
            random_.draw("bot.weapon.topUp") < d.reloadSkill * .25) {
            b.reload = botWeapons[w].reload;
            return;
        }
        if (now < b.weaponAt) return;
        b.weaponAt = now + 1.5;
        const float distance = b.visible || b.hasIntel ? float(b.distance) : 10.f;
        const auto score = [&](int i) {
            const auto& weapon = botWeapons[i];
            const double ammo = .3 + .7 * b.clip[i] / weapon.mag;
            return rangeUtility(distance, weapon) * ammo * (i == 0 ? 1 : b.clip[0] == 0 ? 1.35 : .75);
        };
        if (score(other) > score(w) * (1 + (1 - d.switchSkill) * .9)) {
            b.weapon = other;
            b.swap = .6;
            b.burst = 0;
        }
    }

    void think(const BotTarget& t, const game_data::Difficulty& d, double now) {
        auto& b = brain_;
        chooseWeapon(d, now);
        suspicion(t, d, now);
        const Mode previous = b.mode;
        const double since = now - b.lastSeen;
        const double retreatHealth = 38 * (1 - d.aggression * .55);
        if (b.clip[b.weapon] == 0 || (b.reload > 0 && (b.visible || previous == Mode::Reload))) b.mode = Mode::Reload;
        else if ((actor.health < retreatHealth && b.threat > .4) || now < b.retreatUntil) b.mode = Mode::Retreat;
        else if (b.visible || since < 6) b.mode = Mode::Combat;
        else if (b.hasIntel) b.mode = Mode::Search;
        else if (!t.dead) b.mode = Mode::Hunt;
        else b.mode = Mode::Patrol;
        const bool entered = previous != b.mode;
        if (entered) b.holding = false;
        switch (b.mode) {
            case Mode::Retreat:
            case Mode::Reload:
                if (entered && b.mode == Mode::Retreat) {
                    b.retreatUntil = now + random_.between("bot.retreat.duration", 3.5, 6.5);
                    b.destination = -1;
                } else if (entered && previous != Mode::Retreat && !b.reached) {
                    b.destination = -1;
                }
                if (b.destination < 0 || (b.reached && !world_.blocked(atHeight(actor.position, botEye), coverTarget(t)))) {
                    goTo(hiddenNode(t));
                }
                b.sprint = true;
                break;
            case Mode::Combat: {
                const Vec3 focus = b.visible ? t.position : b.last;
                const bool moved = planarSquared(focus - b.evaluated) > 3.2f * 3.2f;
                const bool good = b.visible && b.rayClear && rangeUtility(float(b.distance), botWeapons[b.weapon]) >= .55f;
                if (b.reached && good && !b.holding) {
                    b.holding = true;
                    b.holdUntil = now + random_.between("bot.combat.hold", d.hold[0], d.hold[1]);
                }
                if (b.holding && (!good || now > b.holdUntil || moved || b.hurt > .7)) {
                    b.holding = false;
                    b.holdUntil = 0;
                }
                const bool stale = b.destination < 0 || b.reached || moved ||
                    (now > b.holdUntil && b.pathIndex >= std::max(0, int(path_.size()) - 2));
                if (!b.holding && stale) {
                    b.flank = random_.draw("bot.combat.flank") < d.flank;
                    b.evaluated = focus;
                    const int node = attackNode(t, d, focus);
                    goTo(node >= 0 ? node : searchNode(t, now, 3.2, 10));
                    b.holdUntil = now + random_.between("bot.combat.route", 1, 1.8);
                }
                b.sprint = !b.visible && d.aggression > .25;
                break;
            }
            case Mode::Search:
                if (entered || b.freshIntel) {
                    b.searchStops = 5;
                    goTo(searchNode(t, now, 3.2, 10));
                } else if (b.destination < 0 || b.reached) {
                    if (b.reached && b.searchStops > 0) --b.searchStops;
                    if (now - b.intelAt > 24 && b.searchStops <= 0) b.hasIntel = false;
                    else {
                        const int node = searchNode(t, now, 3.2, 10);
                        if (node >= 0) goTo(node);
                        else b.hasIntel = false;
                    }
                }
                if (now - b.intelAt > 32) b.hasIntel = false;
                b.sprint = now - b.intelAt < 12 || d.aggression > .45;
                break;
            case Mode::Hunt:
                if (entered || b.destination < 0 || b.reached || now > b.holdUntil) {
                    if (entered || b.reached) suspicion(t, d, now, true);
                    goTo(searchNode(t, now, 5.5, 13));
                    b.holdUntil = now + random_.between("bot.hunt.route", 1.5, 2.8);
                }
                b.sprint = planarSquared(b.suspicion - actor.position) > 64 && d.aggression > .4;
                break;
            case Mode::Patrol:
                if ((b.destination < 0 && now > b.patrolAt) || b.reached) {
                    b.lastPatrol = patrolNode(t, d);
                    goTo(b.lastPatrol);
                    b.patrolAt = now + random_.between("bot.patrol.next", .2, .6);
                }
                b.sprint = false;
                break;
        }
        b.freshIntel = false;
    }

    float penalty(int node) const {
        const auto& b = brain_;
        float penalty = node == b.destination || node == b.previous || node == b.lastPatrol ? .22f : 1;
        const Vec3 position = nav_.node(node).position;
        for (int i = 0; i < b.recentCount; ++i) {
            const int recent = b.recent[i];
            if (node == recent) penalty *= .12f;
            else if (planarSquared(position - nav_.node(recent).position) < 2.8f * 2.8f) penalty *= .55f;
        }
        if (b.previous >= 0 && planarSquared(position - nav_.node(b.previous).position) < 2.6f * 2.6f) penalty *= .45f;
        return penalty;
    }

    template<class Score>
    int select(float minimum, float maximum, Score score) {
        int best = -1;
        double bestScore = 0;
        for (int i = 0; i < nav_.size(); ++i) {
            if (!nav_.valid(i)) continue;
            const auto& node = nav_.node(i);
            const Vec3 run = node.position - actor.position;
            const float d2 = planarSquared(run);
            if (d2 < minimum * minimum || d2 > maximum * maximum) continue;
            const double value = score(i, node, run, std::sqrt(d2), bestScore);
            if (value > bestScore) {
                best = i;
                bestScore = value;
            }
        }
        return best;
    }

    int searchNode(const BotTarget& t, double now, double base, double maximum) {
        const auto& b = brain_;
        const Vec3 raw = b.hasIntel ? b.last : b.suspicion;
        const bool visible = b.visible && !t.dead;
        Vec3 likely = visible ? t.position : b.hasIntel || b.hasSuspicion ? raw : t.position;
        const Vec3 velocity = visible ? t.velocity : b.hasIntel ? b.lastVel : b.suspicionVel;
        const float speed = planar(velocity);
        const double age = std::max(0., now - (b.hasIntel ? b.intelAt : b.suspicionAt));
        if (speed > .8f) likely += velocity * float(std::min(6.5, speed * (.5 + std::min(visible ? 0. : age, 4.) * .22)) / speed);
        const double radius = std::min(maximum, base + age * .32 + (5 - std::max(0, b.searchStops)) * .8);
        const Vec3 ahead = likely - actor.position;
        const float current = planar(ahead);
        const int best = select(1.8f, infinity, [&](int i, const NavNode& node, Vec3 run, float distance, double) {
            const float pred2 = planarSquared(node.position - likely);
            if (pred2 > radius * radius) return 0.;
            const float pred = std::sqrt(pred2);
            const double toward = current > .001f && distance > .001f ?
                std::max(0.f, (ahead.x * run.x + ahead.z * run.z) / (current * distance)) : .5;
            const double push = current > .001f ? .75 + .45 * std::max(0.f, (current - pred) / current) : 1;
            return 1 / (1 + pred * .38) * std::min(1., distance / 5.) * (.65 + .35 * toward) * push *
                (node.cover ? 1.12 : 1) * penalty(i) * random_.between("bot.search.score", .9, 1.1);
        });
        if (best >= 0) return best;
        const int led = nav_.nearest(likely);
        return led >= 0 ? led : nav_.nearest(raw);
    }

    int attackNode(const BotTarget& t, const game_data::Difficulty& d, Vec3 focus) {
        const auto& b = brain_;
        focus.y = b.visible ? t.position.y + t.eye * .8f : (b.hasIntel ? b.last.y : actor.position.y) + botEye * .8f;
        const Vec3 from = actor.position - focus;
        const float current = planar(from);
        const double firstAngle = b.flank ? std::atan2(from.x, from.z) : 0;
        return select(1.8f, 15, [&](int i, const NavNode& node, Vec3 run, float distance, double best) {
            const Vec3 offset = node.position - focus;
            const float fight = planar(offset);
            if (fight < 2.5f) return 0.;
            const double toward = current > .001f && distance > .001f ?
                std::max(0.f, (-from.x * run.x - from.z * run.z) / (current * distance)) : .5;
            const double closes = current > .001f ? (current - fight) / current : 0;
            double score = rangeUtility(fight, botWeapons[b.weapon]) * std::min(1., fight / 6.) /
                (1 + distance * .1) * (.72 + .35 * toward) * (.85 + std::max(-.25, closes) * .45);
            if (score < best * .25) return 0.;
            if (world_.blocked(atHeight(node.position, botEye), focus)) score *= .06;
            if (node.cover) score *= 1 + d.cover * .9;
            score *= penalty(i);
            if (b.flank) {
                const double swing = std::abs(angleDelta(firstAngle, std::atan2(offset.x, offset.z)));
                score *= swing > .9 && swing < 2.3 ? 1.4 : .75;
            }
            return score * random_.between("bot.attack.score", .85, 1.15);
        });
    }

    Vec3 coverTarget(const BotTarget& t) const {
        const auto& b = brain_;
        if (b.visible) return atHeight(t.position, t.eye);
        return atHeight(b.hasIntel ? b.last : actor.position, botEye);
    }

    int hiddenNode(const BotTarget& t) {
        const Vec3 focus = coverTarget(t);
        return select(0, 18, [&](int, const NavNode& node, Vec3, float run, double best) {
            const double threat = std::min(16.f, planar(node.position - focus));
            const double score = (node.cover ? 1.5 : .7) / (1 + run * .1) * (.4 + threat / 16) *
                random_.between("bot.hide.score", .9, 1.1);
            if (score * 1.4 <= best) return 0.;
            return score * (world_.blocked(atHeight(node.position, botEye), focus) ? 1.4 : .15);
        });
    }

    int randomNear(float radius) {
        for (int attempt = 0; attempt < 14; ++attempt) {
            const int n = int(random_.draw("bot.patrol.sample") * nav_.size());
            if (nav_.valid(n) && planarSquared(nav_.node(n).position - actor.position) < radius * radius) return n;
        }
        return -1;
    }

    int patrolNode(const BotTarget& t, const game_data::Difficulty& d) {
        const auto& b = brain_;
        int best = -1;
        double bestScore = 0;
        for (int attempt = 0; attempt < 18; ++attempt) {
            const int n = randomNear(13);
            if (n < 0 || n == b.lastPatrol || n == b.previous) continue;
            const auto& node = nav_.node(n);
            const Vec3 step = node.position - actor.position;
            const float dist = planar(step);
            if (dist < 3.4f) continue;
            const double ahead = .55 + .45 * (std::sin(actor.yaw) * step.x + std::cos(actor.yaw) * step.z) / dist;
            const double central = 1.15 - .3 * std::min(1., planar(node.position) / 15.);
            double bias = 1;
            if (b.hasSuspicion && !t.dead) {
                const float now = planar(b.suspicion - actor.position), then = planar(b.suspicion - node.position);
                bias = .8 + std::clamp((now - then) / std::max(1.f, now), -.2f, .6f);
            }
            const double score = std::min(1., dist / 8.) * ahead * central * (node.cover ? 1 + d.cover * .35 : 1) *
                bias * penalty(n) * random_.between("bot.patrol.score", .85, 1.15);
            if (score > bestScore) {
                best = n;
                bestScore = score;
            }
        }
        return best >= 0 ? best : randomNear(12);
    }

    void goTo(int node) {
        auto& b = brain_;
        const int old = b.destination;
        b.destination = node;
        b.reached = false;
        b.pathIndex = 0;
        path_.clear();
        if (node < 0) return;
        if (!nav_.path(nav_.nearest(actor.position), node, path_)) {
            b.destination = -1;
            return;
        }
        if (old >= 0 && old != node) b.previous = old;
        const int last = (b.recentCursor + int(b.recent.size()) - 1) % int(b.recent.size());
        if (b.recentCount == 0 || b.recent[last] != node) {
            b.recent[b.recentCursor] = node;
            b.recentCursor = (b.recentCursor + 1) % int(b.recent.size());
            b.recentCount = std::min(b.recentCount + 1, int(b.recent.size()));
        }
        b.reversing = 0;
    }

    void recover(const BotTarget& t, const game_data::Difficulty& d, double now) {
        auto& b = brain_;
        b.stuck = b.reversing = 0;
        b.pathIndex = 0;
        b.destination = -1;
        b.reached = false;
        path_.clear();
        int goal = -1;
        switch (b.mode) {
            case Mode::Retreat:
            case Mode::Reload: goal = hiddenNode(t); break;
            case Mode::Combat:
                goal = attackNode(t, d, b.visible ? t.position : b.last);
                if (goal < 0) goal = searchNode(t, now, 3.2, 10);
                break;
            case Mode::Search: goal = searchNode(t, now, 3.2, 10); break;
            case Mode::Hunt:
                suspicion(t, d, now, true);
                goal = searchNode(t, now, 5.5, 13);
                b.holdUntil = now + random_.between("bot.hunt.recover", 1.2, 2.2);
                break;
            case Mode::Patrol: goal = patrolNode(t, d); break;
        }
        if (goal >= 0) goTo(goal);
    }

    bool engaged(double now) const {
        return brain_.visible || (brain_.hasIntel && now - brain_.lastSeen < 2.8);
    }

    BotRound combat(const BotTarget& t, const game_data::Difficulty& d, double now, double dt) {
        auto& b = brain_;
        const double settle = std::exp(-d.aimSpeed * dt);
        b.errYaw *= settle;
        b.errPitch *= settle;
        if (!engaged(now) || t.dead) {
            b.burst = 0;
            b.rayClear = false;
            b.aimYaw += angleDelta(b.aimYaw, actor.yaw) * (1 - std::exp(-6 * dt));
            b.aimPitch *= std::exp(-6 * dt);
            return {};
        }
        const Vec3 theory = atHeight(actor.position, botMuzzle);
        Vec3 target;
        if (b.visible) {
            int rank = 6;
            for (int i = 0; i < 6; ++i) {
                if (sightSamples[i].rank >= rank) continue;
                const Vec3 sample = silhouette(actor.position, t, i);
                if (!world_.blocked(theory, sample)) {
                    rank = sightSamples[i].rank;
                    target = sample;
                    b.aimSample = i;
                }
            }
            b.rayClear = rank < 6;
            if (!b.rayClear) target = silhouette(actor.position, t, b.aimSample);
        } else {
            target = atHeight(b.last, 1.3f);
            b.rayClear = !world_.blocked(theory, target);
        }
        const Vec3 offset = target - theory;
        const double yaw = std::atan2(offset.x, offset.z);
        const double pitch = -std::atan2(offset.y, planar(offset));
        const double chase = 1 - std::exp(-d.aimSpeed * 2.2 * dt);
        b.aimYaw += angleDelta(b.aimYaw, yaw + b.errYaw) * chase;
        b.aimPitch += (pitch + b.errPitch - b.aimPitch) * chase;
        if (b.react > 0) {
            b.react -= dt;
            return {};
        }
        if (b.reload > 0 || b.swap > 0 || !b.clip[b.weapon]) return {};
        const double off = std::abs(angleDelta(b.aimYaw, yaw));
        if (b.burst > 0) {
            if (b.fire <= 0 && off < std::max(8., d.onTarget * 2.5) * deg) return fire(t, d, dt);
        } else if (b.burstTimer <= 0 && off < d.onTarget * deg && b.rayClear) {
            if (b.visible || random_.draw("bot.burst.suppress") < d.aggression * .55) {
                b.burst = int(std::floor(random_.between("bot.burst.length", d.burst[0], d.burst[1]) + .5));
                error(d, 1);
            } else {
                b.burstTimer = .4;
            }
        }
        return {};
    }

    BotRound fire(const BotTarget& target, const game_data::Difficulty& d, double dt) {
        auto& b = brain_;
        const auto& w = botWeapons[b.weapon];
        --b.clip[b.weapon];
        --b.burst;
        b.fire = std::max(b.fire, -dt) + w.interval;
        radar = 2;
        if (b.burst <= 0 || !b.clip[b.weapon]) {
            b.burstTimer = random_.between("bot.burst.cooldown", d.burstCooldown[0], d.burstCooldown[1]);
        }
        const double strafe = std::abs(target.velocity.x) + std::abs(target.velocity.z) > 3 ? 1.25 : 1;
        const double spread = w.spread * deg * d.spread * (actor.speed > .5f ? 1.6 : 1) * strafe;
        const auto yaw = float(b.aimYaw + random_.between("bot.shot.yaw", -spread, spread));
        const auto pitch = float(b.aimPitch + random_.between("bot.shot.pitch", -spread, spread) * .75);
        const Vec3 dir = forward(yaw, pitch);
        // The sampled rig omits live chest-joint corrections. Keep the
        // canonical mount and muzzle offsets plus the animated shoulder raise.
        const float c = std::cos(actor.pitch), s = std::sin(actor.pitch);
        const Vec3 origin = atHeight(actor.position, 1.34f + float(.27 * b.shoulder) - .015f * c - .82f * s) +
            forward(actor.yaw, 0) * (.17f - .015f * s + .82f * c) +
            Vec3{std::cos(actor.yaw), 0, -std::sin(actor.yaw)} * .065f;
        b.errPitch -= .35 * deg;
        b.errYaw += random_.between("bot.shot.recoil", -.18, .18) * deg;
        return {origin, dir, w.sound, float(w.damage * d.damage), true};
    }

    void motor(const BotTarget& t, const game_data::Difficulty& d, double now, double dt, Vec3 separation) {
        auto& b = brain_;
        float intended = 0, moveYaw = actor.yaw;
        const bool planted = d.plantFeet && b.visible && b.burst > 0 && !b.sprint;
        if (!planted && b.destination >= 0 && b.pathIndex < int(path_.size())) {
            while (b.pathIndex + 1 < int(path_.size()) &&
                   !world_.walkBlocked(actor.position, nav_.node(path_[b.pathIndex + 1]).position)) ++b.pathIndex;
            const Vec3 offset = nav_.node(path_[b.pathIndex]).position - actor.position;
            const float distance = planar(offset);
            if (distance < .55f) {
                b.reached = ++b.pathIndex >= int(path_.size());
            } else {
                intended = b.sprint && !b.visible ? 6.2f : b.mode == Mode::Patrol ? 2.4f : 4.f;
                moveYaw = std::atan2(offset.x, offset.z);
                velocity.x = offset.x / distance * intended;
                velocity.z = offset.z / distance * intended;
            }
        }
        if (intended == 0) velocity.x = velocity.z = 0;
        if (b.moving && intended == 0) {
            b.idleYaw = actor.yaw;
            b.idleAt = now;
        }
        b.moving = intended > 0;
        const bool fight = engaged(now);
        const double look = fight ? b.aimYaw :
            b.moving ? moveYaw : b.idleYaw + std::sin((now - b.idleAt) * .55) * 1.2;
        actor.yaw += float(angleDelta(actor.yaw, look) * (1 - std::exp(-(fight ? 11 : 7) * dt)));
        const Vec3 previous = actor.position;
        velocity += separation;
        world_.move(actor.position, velocity, float(dt), 1.85f, .4f);
        const Vec3 moved = actor.position - previous;
        actor.speed = intended > 0 ? planar(moved) / float(std::max(dt, 1e-6)) : 0;
        actor.trackMotion(velocity, std::min(1.f, float(dt * 10)));
        if (intended > 0 && actor.speed < intended * .25f) {
            b.stuck += dt;
            if (b.stuck > .42) recover(t, d, now);
        } else {
            b.stuck = 0;
        }
        if (intended > 0 && actor.speed > .35f) {
            const Vec3 dir = normalized(Vec3{moved.x, 0, moved.z});
            const bool canBreak = b.mode == Mode::Hunt || b.mode == Mode::Patrol || b.mode == Mode::Search;
            if (canBreak && dot(dir, b.lastMove) < -.78f) {
                b.reversing += dt;
                if (b.reversing > .32) recover(t, d, now);
            } else {
                b.reversing = std::max(0., b.reversing - dt * 2.5);
            }
            b.lastMove = normalized(b.lastMove * .55f + dir * .45f);
        } else if (intended == 0) {
            b.reversing = 0;
        }
        // Use the clip durations in the source GLB and the browser's speed ratios.
        if (actor.speed >= 3.1f) {
            b.runCycle += float(dt / .7) * std::clamp(actor.speed / 4.6f, .75f, 1.5f);
            b.runCycle -= std::floor(b.runCycle);
            actor.phase = b.runCycle;
        } else if (actor.speed >= .4f) {
            b.walkCycle += float(dt / (31. / 30.)) * std::clamp(actor.speed / 1.55f, .6f, 1.8f);
            b.walkCycle -= std::floor(b.walkCycle);
            actor.phase = b.walkCycle;
        } else {
            actor.phase = 0;
        }
        const bool shoulder = b.visible || (b.hasIntel && now - b.lastSeen < 4);
        const float wanted = shoulder ? float(b.aimPitch) : .5f;
        const double blend = 1 - std::exp(-9 * dt);
        actor.pitch += (wanted - actor.pitch) * float(blend);
        b.shoulder += ((shoulder ? 1 : 0) - b.shoulder) * blend;
    }
};

} // namespace fod::game_detail
