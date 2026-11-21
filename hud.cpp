#include "render.hpp"
#include "app.hpp"
#include "assets.hpp"
#include "game_world.hpp"
#include "hud_font.hpp"
#include "mission.hpp"
#include "timberline_data.hpp"
#include "scout_data.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace fod {
namespace {

constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) {
    return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

constexpr uint16_t paper = rgb(233, 233, 214), muted = rgb(167, 177, 162), panel = rgb(17, 24, 27);
constexpr uint16_t ink = rgb(9, 15, 18), lineColor = rgb(63, 83, 81), alert = rgb(243, 96, 68);
constexpr uint16_t accent = rgb(212, 223, 110), playerColor = rgb(119, 211, 183);
constexpr uint16_t gold = rgb(232, 181, 90);
constexpr char gameTitle[] = "FALL OF DUTY LITE";

struct Painter {
    uint16_t* pixels;
    bool rotated;
    int left = 0, top = 0, right = 800, bottom = 480;
#ifdef FOD_LOCAL_DUEL
    int canvasWidth = 800;
#endif

    size_t index(int x, int y, bool rotate) const {
#ifdef FOD_LOCAL_DUEL
        if (!rotate) return size_t(y) * canvasWidth + x;
#endif
        return rotate ? size_t(x) * 480 + 479 - y : size_t(y) * 800 + x;
    }

    int raster(int logical) const {
#ifdef FOD_LOCAL_DUEL
        return canvasWidth == 800 ? logical : logical * canvasWidth / 800;
#else
        return logical;
#endif
    }

    void pixel(int x, int y, uint16_t color) const {
#ifdef FOD_LOCAL_DUEL
        if (canvasWidth != 800) {
            rect(x, y, 1, 1, color);
            return;
        }
#endif
        if (x >= left && x < right && y >= top && y < bottom) pixels[index(x, y, rotated)] = color;
    }

    void rect(int x, int y, int width, int height, uint16_t color) const {
        const int l = std::clamp(x, left, right), r = std::clamp(x + width, left, right);
        const int t = std::clamp(y, top, bottom), b = std::clamp(y + height, top, bottom);
        if (r <= l || b <= t) return;
        if (rotated) {
            for (int col = l; col < r; ++col) std::fill_n(pixels + index(col, b - 1, true), b - t, color);
        } else {
            const int startX = raster(l), span = raster(r) - startX, endY = raster(b);
            for (int row = raster(t); row < endY; ++row) {
                std::fill_n(pixels + index(startX, row, false), span, color);
            }
        }
    }

    void shadeTitle() const {
        constexpr int solidEnd = 260, fadeEnd = 554;
        rect(0, 15, solidEnd, 450, ink);
        const int first = raster(solidEnd), last = raster(fadeEnd), startY = raster(15), endY = raster(465);
        for (int x = first; x < last; ++x) {
            const unsigned light = (x - first) * 256 / (last - first);
            const unsigned shade = 256 - light;
            for (int y = startY; y < endY; ++y) {
                auto& pixel = pixels[index(x, y, rotated)];
                const unsigned r = ((pixel >> 11) * light + (ink >> 11) * shade + 128) >> 8;
                const unsigned g = (((pixel >> 5) & 63) * light + ((ink >> 5) & 63) * shade + 128) >> 8;
                const unsigned b = ((pixel & 31) * light + (ink & 31) * shade + 128) >> 8;
                pixel = uint16_t((r << 11) | (g << 5) | b);
            }
        }
    }

    void line(int x, int y, int endX, int endY, uint16_t color) const {
        if (std::max(x, endX) < left || std::min(x, endX) >= right ||
            std::max(y, endY) < top || std::min(y, endY) >= bottom) return;
        const int dx = std::abs(endX - x), dy = -std::abs(endY - y);
        const int stepX = endX > x ? 1 : -1, stepY = endY > y ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            pixel(x, y, color);
            if (x == endX && y == endY) break;
            const int e = error * 2;
            if (e >= dy) { error += dy; x += stepX; }
            if (e <= dx) { error += dx; y += stepY; }
        }
    }

    void text(int x, int y, std::string_view text, uint16_t color, int scale = 2) const {
        if (rotated) {
            for (char c : text) {
                const auto* rows = hud::glyph(c);
                for (int col = 0; col < 5; ++col) {
                    const uint8_t bit = uint8_t(16 >> col);
                    for (int row = 0; row < 7; ++row) {
                        if (rows[row] & bit) rect(x + col * scale, y + row * scale, scale, scale, color);
                    }
                }
                x += 6 * scale;
            }
            return;
        }
        for (char c : text) {
            const auto* rows = hud::glyph(c);
            for (int row = 0; row < 7; ++row) {
                const uint8_t mask = rows[row];
                if (!mask) continue;
                for (int col = 0; col < 5; ++col) {
                    if (mask & (16 >> col)) rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
            x += 6 * scale;
        }
    }
};

struct MapPoint { int x, y; };

uint16_t operationColor(const OperationView& op) {
    if (op.jammer >= 0) return rgb(255, 92, 191);
    if (op.pulled && (op.stage == OperationStage::Call || op.stage == OperationStage::Steer)) return rgb(255, 133, 54);
    if (op.stage == OperationStage::Steer && op.lock > 0.f) return rgb(110, 234, 146);
    return rgb(104, 232, 250);
}

// The Signal button as printed on the controller in use.
const char* signalButton(const MenuState& menu) {
#ifdef FOD_LOCAL_DUEL
    switch (menu.controls) {
    case MenuControls::Keyboard: return "F";
    case MenuControls::Xbox: return "LB";
    case MenuControls::Nintendo: return "L";
    case MenuControls::Default: return "F / L1";
    default: return "L1";
    }
#else
    (void)menu;
    return "L1";
#endif
}

void map(const Painter& p, const Assets& assets, const Snapshot& snap, int x, int y, int size, bool laptop) {
    const bool scanning = !laptop && snap.uavSeconds > 0;
    const auto rim = scanning ? playerColor : lineColor;
    p.rect(x, y, size, size, rim);
    Painter clipped = p;
    clipped.left = std::max(p.left, x + 1);
    clipped.top = std::max(p.top, y + 1);
    clipped.right = std::min(p.right, x + size - 1);
    clipped.bottom = std::min(p.bottom, y + size - 1);
    if (clipped.left >= clipped.right || clipped.top >= clipped.bottom) return;
    clipped.rect(x, y, size, size, panel);
    const auto& geometry = assets.geometry(snap.mission ? missionScene(snap.mission->chapter, snap.mission->operation.stage)
                                                       : ArenaScene::Estate);
    const auto arena = geometry.extent;
    const int midX = x + size / 2, midY = y + size / 2;
    const float scale = laptop ? (size - 6) / (2 * std::max(arena.x, arena.z)) : size / 44.0f;
    const Vec3 origin = laptop ? Vec3{} : snap.player.position;
    const float yaw = laptop ? 0 : snap.playerYaw;
    const float cs = std::cos(yaw), sn = std::sin(yaw);
    const auto project = [&](float wx, float wz) {
        const float dx = (wx - origin.x) * scale, dz = (wz - origin.z) * scale;
        return MapPoint{midX + int(std::lround(dx * cs - dz * sn)), midY - int(std::lround(dx * sn + dz * cs))};
    };
    const auto outline = [&](float cx, float cz, float hw, float hd, float bc, float bs, uint16_t color) {
        const auto corner = [&](float w, float d) { return project(cx + bc * w + bs * d, cz - bs * w + bc * d); };
        const MapPoint points[]{corner(-hw, -hd), corner(hw, -hd), corner(hw, hd), corner(-hw, hd)};
        for (int i = 0; i < 4; ++i) {
            const auto from = points[i], to = points[(i + 1) % 4];
            clipped.line(from.x, from.y, to.x, to.y, color);
        }
    };
    outline(0, 0, arena.x, arena.z, 1, 0, muted);
    // Allow a pixel of rounding at the rim when turning the radar square into world bounds.
    const float radarHalf = (size * .5f + 1.f) * (std::abs(cs) + std::abs(sn)) / scale;
    for (const auto& box : geometry.obstacles) {
        // The mansion's lintels and ceilings sit above the floor-plan routes.
        if (assets.arena == Arena::BriarHouse && box.minY > .25f) continue;
        const float reach = radarHalf + (box.hw + box.hd) * 1.001f;
        if (std::abs(box.cx - origin.x) > reach || std::abs(box.cz - origin.z) > reach) continue;
        outline(box.cx, box.cz, box.hw, box.hd, box.cos, box.sin, lineColor);
    }
    if (scanning) {
        const float sweep = float(snap.scaledTime * 1.8) - yaw;
        const float radius = size * .71f;
        for (int i = 5; i >= 0; --i) {
            const float angle = sweep - i * .14f;
            const int light = 106 - i * 12;
            clipped.line(midX, midY, midX + int(std::cos(angle) * radius), midY + int(std::sin(angle) * radius),
                         rgb(light / 2, light, light * 3 / 4));
        }
    }
    const auto drawContact = [&](const Actor& rival, uint16_t color) {
        const auto contact = project(rival.position.x, rival.position.z);
        const int radius = scanning ? 3 + int(1.5 + 1.5 * std::sin(snap.scaledTime * 5.5)) : 2;
        for (int row = -radius; row <= radius; ++row) {
            const int span = int(std::sqrt(float(radius * radius - row * row)));
            clipped.rect(contact.x - span, contact.y + row, span * 2 + 1, 1, color);
        }
    };
    if (snap.mission && snap.mission->active) {
        constexpr uint16_t alienCyan = rgb(72, 248, 255);
        const auto& m = *snap.mission;
#ifdef FOD_LOCAL_DUEL
        if (snap.campaignSeat >= 0 && snap.rival.visible)
            drawContact(snap.rival, snap.rival.health > 0.f ? rgb(157, 188, 255) : alert);
#endif
        const auto drawRing = [&](Vec3 center, float radius, uint16_t edge) {
            if (game_detail::planarSquared(center - origin) > (radarHalf + radius) * (radarHalf + radius)) return;
            constexpr float c = .92388f, s = .38268f, d = .70711f;
            constexpr float ring[][2]{{1, 0}, {c, s}, {d, d}, {s, c}, {0, 1}, {-s, c}, {-d, d}, {-c, s},
                                      {-1, 0}, {-c, -s}, {-d, -d}, {-s, -c}, {0, -1}, {s, -c}, {d, -d}, {c, -s}};
            auto from = project(center.x + radius * c, center.z - radius * s);
            for (const auto& offset : ring) {
                const auto to = project(center.x + radius * offset[0], center.z + radius * offset[1]);
                clipped.line(from.x, from.y, to.x, to.y, edge);
                from = to;
            }
        };
        if (m.holdRadius > 0.f) {
            const bool inside = game_detail::planarSquared(m.holdCenter - origin) < m.holdRadius * m.holdRadius;
            const auto edge = m.phase == MissionPhase::Operation ? operationColor(m.operation) : playerColor;
            drawRing(m.holdCenter, m.holdRadius, inside ? edge : alert);
        }
        const auto& op = m.operation;
        if (m.phase == MissionPhase::Operation) {
            if (op.stage == OperationStage::Steer) {
                drawRing(op.clearing, op.clearingRadius, op.clearingBurned ? muted : alert);
                drawRing(op.saddle, op.saddleRadius, playerColor);
                drawRing(op.ship, 1.5f, operationColor(op));
                drawRing(op.ship, 3.f, operationColor(op));
            } else if (op.stage == OperationStage::Call && op.targetSet) {
                drawRing(op.target, 2.2f, operationColor(op));
            }
            if (op.kitVisible) drawRing(op.kit, .8f, op.jammer >= 0 ? operationColor(op) : gold);
        }
        for (size_t i = 0; i < m.actors.size(); ++i) {
            const auto& ma = m.actors[i];
            if (!ma.active || ma.dead) continue;
            if (int(i) == op.jammer) {
                drawContact(ma.actor, operationColor(op));
            } else if (ma.role == MissionRole::Commander) {
                drawContact(ma.actor, playerColor);
            } else if (ma.role == MissionRole::Civilian) {
                if (!civilianRescued(ma.civilian) || m.phase == MissionPhase::Operation ||
                    m.phase == MissionPhase::Boarding) drawContact(ma.actor, gold);
            } else if (ma.radar > 0 || ma.alerted || scanning) {
                drawContact(ma.actor, ma.variant > 0 ? alert : alienCyan);
            }
        }
        if (snap.mission->hasWaypoint) {
            auto wp = project(snap.mission->waypoint.x, snap.mission->waypoint.z);
            // In the expansive town map, pin distant objectives cleanly to the radar rim.
            const int lim = size / 2 - 6;
            const int rx = wp.x - midX, ry = wp.y - midY;
            if (std::abs(rx) > lim || std::abs(ry) > lim) {
                const float k = float(lim) / float(std::max(std::abs(rx), std::abs(ry)));
                wp.x = midX + int(std::lround(rx * k));
                wp.y = midY + int(std::lround(ry * k));
            }
            clipped.line(wp.x - 4, wp.y, wp.x, wp.y - 4, accent);
            clipped.line(wp.x, wp.y - 4, wp.x + 4, wp.y, accent);
            clipped.line(wp.x + 4, wp.y, wp.x, wp.y + 4, accent);
            clipped.line(wp.x, wp.y + 4, wp.x - 4, wp.y, accent);
        }
    } else {
        if (snap.radarContact && snap.rival.visible) drawContact(snap.rival, alert);
        if (snap.freeForAll && snap.secondRadarContact && snap.secondRival.visible) drawContact(snap.secondRival, gold);
    }
    if (!laptop && snap.apacheActive) {
        const auto contact = project(snap.apachePosition.x, snap.apachePosition.z);
        clipped.text(contact.x - 2, contact.y - 4, "H", accent, 1);
    }
    if (!laptop) {
        const int northRadius = size / 2 - 9;
        clipped.text(midX - int(sn * northRadius) - 2, midY - int(cs * northRadius) - 3, "N", muted, 1);
        for (int row = 0; row < 7; ++row) {
            const int span = row / 2;
            clipped.rect(midX - span, midY + row - 4, 2 * span + 1, 1, paper);
        }
        return;
    }
    const auto player = project(snap.player.position.x, snap.player.position.z);
    clipped.rect(player.x - 2, player.y - 2, 5, 5, paper);
    clipped.line(player.x, player.y, player.x + int(9 * std::sin(snap.playerYaw)),
                 player.y - int(9 * std::cos(snap.playerYaw)), paper);
    const auto mark = project(snap.cursorX, snap.cursorZ);
    clipped.rect(mark.x - 8, mark.y, 17, 1, accent);
    clipped.rect(mark.x, mark.y - 8, 1, 17, accent);
    clipped.rect(mark.x - 3, mark.y - 3, 7, 7, paper);
    clipped.rect(mark.x - 2, mark.y - 2, 5, 5, ink);
}

void drawThermalScope(const Painter& p) {
    constexpr int left = 400 - thermalLensHalfWidth, right = 400 + thermalLensHalfWidth;
    constexpr int top = 240 - thermalLensHalfHeight, bottom = 240 + thermalLensHalfHeight;
    constexpr int corner = 20;
    constexpr uint16_t frame = rgb(7, 13, 18), rim = rgb(59, 94, 103), reticle = rgb(142, 233, 224);

    p.rect(0, 15, 800, top - 15, frame);
    p.rect(0, bottom + 1, 800, 464 - bottom, frame);
    p.rect(0, top, left, bottom - top + 1, frame);
    p.rect(right + 1, top, 799 - right, bottom - top + 1, frame);
    for (int i = 0; i < corner; ++i) {
        const int inset = corner - i;
        for (int x : {left + i, right - i}) {
            p.rect(x, top, 1, inset, frame);
            p.rect(x, bottom - inset + 1, 1, inset, frame);
            p.pixel(x, top + inset, rim);
            p.pixel(x, bottom - inset, rim);
        }
    }
    p.line(left + corner, top, right - corner, top, rim);
    p.line(left + corner, bottom, right - corner, bottom, rim);
    p.line(left, top + corner, left, bottom - corner, rim);
    p.line(right, top + corner, right, bottom - corner, rim);

    // Interrupted arms leave the target visible around the aim point.
    for (int side : {-1, 1}) {
        p.line(400 + 9 * side, 240, 400 + 35 * side, 240, reticle);
        p.line(400 + 35 * side, 236, 400 + 35 * side, 244, reticle);
        p.line(400, 240 + 9 * side, 400, 240 + 28 * side, reticle);
        p.line(396, 240 + 28 * side, 404, 240 + 28 * side, reticle);
    }
    p.rect(399, 239, 3, 3, frame);
    p.pixel(400, 240, reticle);
    p.text(left + 29, top + 14, "THERMAL / WHITE HOT", reticle, 1);
}

void scope(const Painter& p, const Snapshot& snap) {
    if (snap.weapon != Weapon::Sniper || snap.scope < 0.6f || snap.dead || snap.device != Device::None) return;
    if (thermalScope(snap)) {
        drawThermalScope(p);
        return;
    }
    constexpr int radius = 182;
    // Column spans are the contiguous direction on the rotated framebuffer.
    for (int x = 0; x < 800; ++x) {
        const int dx = x - 400;
        if (std::abs(dx) >= radius) {
            p.rect(x, 15, 1, 450, 0);
            continue;
        }
        const int dy = int(std::sqrt(float(radius * radius - dx * dx)));
        p.rect(x, 15, 1, 225 - dy, 0);
        p.rect(x, 240 + dy, 1, 225 - dy, 0);
    }
    p.rect(398, 61, 3, 358, ink);
    p.rect(221, 239, 358, 3, ink);
    for (int i = -4; i <= 4; ++i) {
        p.rect(400 + 16 * i, 237, 1, 7, paper);
        p.rect(397, 240 + 16 * i, 7, 1, paper);
    }
    p.rect(399, 239, 3, 3, alert);
}

void reticle(const Painter& p, const Snapshot& s) {
    if (s.dead || s.device != Device::None) return;
    const bool scoped = s.weapon == Weapon::Sniper && s.scope >= 0.6f;
    if (!scoped && s.ads < 0.83f) {
        const int spread = 6 + int(10 * (1 - s.ads));
        p.rect(391 - spread, 239, 8, 2, paper);
        p.rect(402 + spread, 239, 8, 2, paper);
        p.rect(399, 231 - spread, 2, 8, paper);
        p.rect(399, 242 + spread, 2, 8, paper);
    }
    if (s.hitMarker > 0) {
        const int outer = s.lethalMarker ? 17 : 10;
        const int inner = s.lethalMarker ? 7 : 4;
        const bool shieldHit = s.guardHit && !s.lethalMarker;
        const uint16_t color = s.lethalMarker ? (s.headshot ? gold : alert)
                             : shieldHit ? rgb(88, 240, 255) : paper;
        for (int x : {-1, 1}) {
            for (int y : {-1, 1}) {
                p.line(400 + outer * x, 241 + outer * y, 400 + inner * x, 241 + inner * y, ink);
                p.line(400 + outer * x, 240 + outer * y, 400 + inner * x, 240 + inner * y, color);
                if (s.lethalMarker)
                    p.line(399 + outer * x, 240 + outer * y, 399 + inner * x, 240 + inner * y, color);
            }
        }
    }
    if (!scoped && s.muzzle > 0 && s.ads < 0.9f) {
        const int x = 434 - int(26 * s.ads), y = 264 - int(12 * s.ads);
        p.line(x - 7, y, x + 7, y, accent);
        p.line(x, y - 7, x, y + 7, accent);
        p.rect(x - 2, y - 2, 5, 5, paper);
    }
}

const char* difficultyName(int level) {
    if (level <= 2) return "RECRUIT";
    if (level <= 4) return "REGULAR";
    if (level <= 7) return "HARDENED";
    if (level == 8) return "VETERAN";
    return "TERMINATOR";
}

const char* opponentName(Opponent opponent) {
    switch (opponent) {
    case Opponent::Hanz: return "HANZ";
    case Opponent::FreeForAll: return "FREE-FOR-ALL";
    case Opponent::IPwndUrMom: return "IPWNDURMOM";
    }
    return "IPWNDURMOM";
}

std::string_view fighterName(const MenuState& menu, int slot) {
    if (slot < 0) return menu.callsign.data();
    return opponentName(slot == 0 ? Opponent::IPwndUrMom : Opponent::Hanz);
}

uint16_t fighterColor(int slot) {
    if (slot < 0) return playerColor;
    return slot == 0 ? alert : gold;
}

void freeForAllScore(const Painter& p, const Snapshot& s, const MenuState& menu, int x, int y, int width) {
    const int column = width / 3;
    p.rect(x, y, width, 37, ink);
    const int scores[]{s.playerScore, s.rivalScore, s.secondRivalScore};
    for (int i = 0; i < 3; ++i) {
        const int center = x + column * i + column / 2;
        const auto name = fighterName(menu, i - 1);
        const auto color = i == 0 && s.killConfirm > 0 && s.headshot ? gold : fighterColor(i - 1);
        if (i) p.rect(x + column * i, y + 5, 1, 27, lineColor);
        p.text(center - int(name.size()) * 3, y + 4, name, muted, 1);
        char points[12];
        const int length = std::snprintf(points, sizeof(points), "%02d", scores[i]);
        p.text(center - length * 6, y + 17, points, color);
    }
}

void drawKillNotice(const Painter& p, const KillNotice& notice, const MenuState& menu, int y) {
    const auto attacker = fighterName(menu, notice.attacker), victim = fighterName(menu, notice.victim);
    constexpr std::string_view action = " TOOK OUT ";
    constexpr int advance = 12;
    const int width = int(attacker.size() + action.size() + victim.size()) * advance;
    const int x = 400 - width / 2;
    p.rect(x - 8, y, width + 16, 20, ink);
    p.text(x, y + 3, attacker, fighterColor(notice.attacker));
    const int actionX = x + int(attacker.size()) * advance;
    p.text(actionX, y + 3, action, paper);
    p.text(actionX + int(action.size()) * advance, y + 3, victim, fighterColor(notice.victim));
}

void startScreen(const Painter& p, const MenuState& menu, Arena arena) {
    p.shadeTitle();

    constexpr uint16_t selectedPanel = rgb(34, 43, 42);
    constexpr uint16_t cardAccents[]{gold, playerColor, playerColor,
#ifdef FOD_LOCAL_DUEL
                                      rgb(156, 193, 226),
#endif
                                      muted};
    const uint16_t focus = cardAccents[size_t(menu.selection)];
    p.text(startX, 53, "FALL OF DUTY", paper, 4);
    p.text(startX - 1, 89, "LITE", paper, 7);
    p.rect(startX, 146, 56, 3, focus);

    char botSub[54];
    std::snprintf(botSub, sizeof(botSub), "%s / %s%s / LEVEL %d", arenaLabel(menu.selectedArena),
                  menu.opponent == Opponent::FreeForAll ? "" : "DUEL VS ",
                  menu.opponent == Opponent::FreeForAll ? "FREE-FOR-ALL" : opponentName(menu.opponent), menu.difficulty);
    const char* descriptions[]{"OPERATION BRIAR / THREE MISSIONS", botSub, "",
#ifdef FOD_LOCAL_DUEL
                               "CAMPAIGN CO-OP OR VERSUS DUEL",
#endif
                               "AUDIO, DISPLAY & CALLSIGN"};
    for (size_t i = 0; i < startCards.size(); ++i) {
        const auto& card = startCards[i];
        const bool selected = menu.selection == int(i), secondary = i == size_t(StartItem::ChangeMatch);
        const int x = startX + (secondary ? 10 : 0);
        const uint16_t tint = cardAccents[i];
        p.rect(x, card.y, startX + startWidth - x, card.height, selected ? selectedPanel : panel);
        p.rect(x, card.y, 3, card.height, selected ? tint : lineColor);
        p.rect(x + 3, card.y + card.height - 1, startX + startWidth - x - 3, 1, lineColor);
        p.text(x + 15, card.y + (secondary ? 8 : 7), card.label, selected ? tint : paper, 2);
        if (!secondary) p.text(x + 15, card.y + 29, descriptions[i], selected ? paper : muted, 1);
        if (selected) p.text(startX + startWidth - 20, card.y + (secondary ? 8 : 15), ">", tint, 2);
    }

    const std::string_view background(arenaLabel(arena));
    const int backdropLabel = int(background.size()) * 6;
    p.rect(774 - backdropLabel - 12, 431, backdropLabel + 24, 18, panel);
    p.text(774 - backdropLabel, 437, background, paper, 1);

    char status[48];
    std::snprintf(status, sizeof(status), "CALLSIGN / %s", menu.callsign.data());
    p.text(startX, 416, status, muted, 1);
#ifdef FOD_LOCAL_DUEL
    if (const auto labels = menuLabels(menu.controls); labels.confirm) {
        char hint[52];
        std::snprintf(hint, sizeof(hint), "%s SELECT", labels.confirm);
        p.text(startX, 434, hint, paper, 1);
    }
#else
    p.text(startX, 434, "TAP OR PRESS X TO SELECT", paper, 1);
#endif
}

constexpr float sRankSeconds = 5 * 60.f;

char missionGrade(const MissionSnapshot& m) {
    if (m.phase != MissionPhase::Victory) return 'F';
    // S rank: every civilian, no checkpoint reloads, under five minutes.
    if (m.civiliansSaved == m.civiliansTotal && m.retries == 0 && m.missionSeconds < sRankSeconds) return 'S';
    if (m.civiliansSaved == m.civiliansTotal) return 'A';
    if (m.civiliansSaved >= 2) return 'B';
    return 'C';
}

void menus(const Painter& p, const Snapshot& s, const MenuState& menu, Arena arena) {
    using A = MenuAction;
    if (menu.loadedLaunch != LaunchAction::None) {
        const char* destination = menu.loadedLaunch == LaunchAction::ContinueCampaign ? missionName(menu.progress.next)
            : menu.loadedLaunch == LaunchAction::Campaign ? "OPERATION BRIAR" : arenaLabel(menu.selectedArena);
        p.rect(menuX - 8, 58, menuWidth + 16, 136, ink);
        p.text(menuX + 12, 79, destination, paper, 3);
        p.text(menuX + 12, 119, "READY TO START", muted, 1);
        p.rect(menuX, menuY, menuWidth, menuRowHeight - 2, panel);
        p.text(menuX + 12, menuY + 9, "BACK", paper);
        return;
    }
    if (s.match == Match::Start && menu.screen == MenuScreen::Home) {
        startScreen(p, menu, arena);
        return;
    }
    const bool home = menu.screen == MenuScreen::Home;
    const bool isCampaign = menu.campaign || (s.mission && s.mission->active);
    const auto chapter = s.mission ? s.mission->chapter : MissionChapter::BriarHouse;
    const bool campaignWon = s.mission && s.mission->campaign && chapter == MissionChapter::Array &&
                             s.mission->phase == MissionPhase::Victory;
    const char* title = menu.screen == MenuScreen::Campaign ? "OPERATION BRIAR"
        : menu.screen == MenuScreen::LocalPlayers ? "LOCAL 2 PLAYER"
        : menu.screen == MenuScreen::CoOpSetup ? "CAMPAIGN CO-OP"
        : menu.screen == MenuScreen::VersusSetup ? "VERSUS DUEL"
        : menu.screen == MenuScreen::Settings ? "SETTINGS"
        : menu.screen == MenuScreen::BotSetup ? "CHANGE MATCH"
        : s.match == Match::Paused ? "PAUSED"
        : isCampaign ? (campaignWon ? "CAMPAIGN COMPLETE"
                        : s.mission && s.mission->phase == MissionPhase::Victory ? "MISSION COMPLETE"
                        : s.mission && s.mission->phase == MissionPhase::Failed ? "MISSION FAILED" : "MISSION ENDED")
        : (s.playerScore >= 10 ? "VICTORY" : s.leadingRivalScore() >= 10 ? "DEFEAT" : "MATCH ENDED");
    const auto rows = menuOptions(menu, s.match);
    const bool showSaveError = menu.saveFailed && (menu.screen == MenuScreen::Campaign ||
        menu.screen == MenuScreen::CoOpSetup || (home && isCampaign && s.match == Match::Results));
    const int contentBottom = menu.editing ? 417 : menuY + rows.count * menuRowHeight + (showSaveError ? 26 : 8);
    p.rect(menuX - 8, 46, menuWidth + 16, contentBottom - 46, ink);
    p.rect(menuX - 8, 46, menuWidth + 16, 4,
           isCampaign && s.match == Match::Results && s.mission && s.mission->phase == MissionPhase::Failed ? alert : accent);
    p.text(menuX + 10, 66, title, paper, std::strlen(title) > 14 || (isCampaign && s.match == Match::Results) ? 4 : 5);
    if (!home) {
        const char* subtitle = menu.screen == MenuScreen::Campaign
#ifdef FOD_LOCAL_DUEL
            ? "THREE MISSIONS / CO-OP REQUIRES TWO GAMEPADS"
#else
            ? "THREE MISSIONS / SOLO CAMPAIGN"
#endif
            : menu.screen == MenuScreen::LocalPlayers ? "TWO GAMEPADS / ONE SCREEN"
            : menu.screen == MenuScreen::CoOpSetup ? "OPERATION BRIAR / TWO GAMEPADS / SHARED CHECKPOINTS"
            : menu.screen == MenuScreen::VersusSetup ? "TWO GAMEPADS / FIRST TO TEN"
            : menu.screen == MenuScreen::BotSetup ? "FIRST TO TEN" : "AUDIO, DISPLAY & CALLSIGN";
        p.text(menuX + 12, 119, subtitle, muted, 1);
    } else if (isCampaign && s.match == Match::Results && s.mission) {
        const auto& m = *s.mission;
        char summary[112];
        const int mins = int(m.missionSeconds) / 60, secs = int(m.missionSeconds) % 60;
        if (chapter != MissionChapter::BriarHouse && m.phase == MissionPhase::Failed) {
            std::snprintf(summary, sizeof(summary), "%s  /  %s %d/3",
                          m.failReason, missionName(chapter), int(chapter) + 1);
        } else if (chapter != MissionChapter::BriarHouse) {
            const int total = int(m.earlierMissionSeconds + m.missionSeconds);
            std::snprintf(summary, sizeof(summary), "%s %d/3  /  %02d:%02d  /  CAMPAIGN %02d:%02d",
                          missionName(chapter), int(chapter) + 1, mins, secs, total / 60, total % 60);
        } else if (m.phase == MissionPhase::Failed) {
            std::snprintf(summary, sizeof(summary), "%s  /  RESCUED %d/%d  /  KILLS %d",
                          m.failReason[0] ? m.failReason : "MISSION FAILED", m.civiliansSaved, m.civiliansTotal, m.aliensKilled);
        } else {
            std::snprintf(summary, sizeof(summary), "SAVED %d/%d  /  ALIENS %d  /  %02d:%02d  /  RANK %c",
                          m.civiliansSaved, m.civiliansTotal, m.aliensKilled, mins, secs, missionGrade(m));
        }
        p.text(menuX + 12, 104, summary, m.phase == MissionPhase::Victory ? gold : alert, 1);
        char detail[112];
        std::snprintf(detail, sizeof(detail), "CRITICALS %d  /  INTERRUPTS %d  /  SHIELDS %d  /  RETRIES %d  /  %s %d",
                      m.criticalKills, m.chargeInterrupts, m.shieldsBroken, m.retries,
                      difficultyName(menu.difficulty), menu.difficulty);
        p.text(menuX + 12, 117, detail, muted, 1);
        if (m.epilogue[0]) p.text(menuX + 12, 131, m.epilogue, paper, 1);
    } else if (s.freeForAll && s.match != Match::Start) {
        freeForAllScore(p, s, menu, menuX + 12, 106, 420);
    } else if (s.match == Match::Results) {
        char score[64];
        std::snprintf(score, sizeof(score), "%s %02d / %02d %s", menu.callsign.data(), s.playerScore, s.rivalScore,
                      s.alternateRival ? "HANZ" : "IPWNDURMOM");
        p.text(menuX + 12, 118, score, muted);
    } else {
        char location[44];
        if (isCampaign) std::snprintf(location, sizeof(location), "MISSION %d / %s", int(chapter) + 1, missionName(chapter));
        else std::snprintf(location, sizeof(location), "%s / FIRST TO TEN", arenaLabel(arena));
        p.text(menuX + 12, 118, location, muted);
    }
    if (menu.editing) {
        p.rect(menuX, menuY, menuWidth, 266, panel);
        p.text(menuX + 14, menuY + 13, "CALLSIGN", muted);
        p.text(menuX + 14, menuY + 43, menu.callsign.data(), paper, 3);
        p.rect(menuX + 14 + menu.callsignCursor * 18, menuY + 68, 15, 2, accent);
        for (int i = 0; i < 40; ++i) {
            const int x = keyboardX + (i % keyboardColumns) * keyWidth;
            const int y = keyboardY + (i / keyboardColumns) * keyHeight;
            p.rect(x, y, keyWidth - 2, keyHeight - 2, ink);
            const char* label = callsignKeys + i;
            const std::string_view key = *label == ' ' ? "SPC" : *label == '<' ? "DEL" : std::string_view(label, 1);
            const int scale = key.size() == 1 ? 2 : 1;
            p.text(x + (keyWidth - int(key.size()) * 6 * scale) / 2, y + (keyHeight - 7 * scale) / 2, key, paper, scale);
        }
        p.rect(menuX + 12, 375, menuWidth - 24, 33, lineColor);
        p.text(menuX + 23, 383, "DONE", paper);
#ifdef FOD_LOCAL_DUEL
        if (const auto labels = menuLabels(menu.controls); labels.confirm) {
            char hint[40];
            std::snprintf(hint, sizeof(hint), "%s TO EDIT / %s TO SAVE",
                          menu.controls == MenuControls::Keyboard ? "ARROWS" : "D-PAD", labels.confirm);
            p.text(menuX + 185, 385, hint, muted, 1);
        } else
#endif
        p.text(menuX + 207, 385, "DPAD TO EDIT / X TO SAVE", muted, 1);
        return;
    }
    const char* primary = menu.screen == MenuScreen::BotSetup ? "START MATCH"
        : menu.screen == MenuScreen::VersusSetup ? "JOIN VERSUS DUEL"
        : s.match == Match::Paused ? "RESUME"
        : isCampaign ? (s.mission && s.mission->nextMissionReady() ? (chapter == MissionChapter::BriarHouse ? "FLY TO TIMBERLINE" : "RETURN TO BRIAR HOUSE")
                        : s.mission && s.mission->phase == MissionPhase::Failed && s.mission->checkpointStage != MissionCheckpoint::None
                        ? "RETRY CHECKPOINT" : "PLAY AGAIN")
        : "REMATCH";
    for (int row = 0; row < rows.count; ++row) {
        const int py = menuY + row * menuRowHeight;
        const bool active = row == menu.selection;
        p.rect(menuX, py, menuWidth, menuRowHeight - 2, active ? lineColor : panel);
        if (active) p.rect(menuX, py, 3, menuRowHeight - 2, accent);
        const char* label = "";
        const char* value = "";
        char number[32];
        switch (rows.actions[size_t(row)]) {
        case A::Play: label = primary; break;
        case A::ChangeMatch: label = "CHANGE MATCH"; break;
        case A::Settings: label = "SETTINGS"; break;
        case A::Back: label = "BACK"; break;
        case A::Exit: label = s.match == Match::Paused ? (isCampaign ? "END MISSION" : "END MATCH") : "MAIN MENU"; break;
        case A::Solo: label = menu.progress.available() ? "NEW SOLO CAMPAIGN" : "SOLO CAMPAIGN"; break;
        case A::CoOp:
            label = menu.screen == MenuScreen::LocalPlayers ? "CAMPAIGN CO-OP" : "2 PLAYER CO-OP";
            if (menu.screen == MenuScreen::LocalPlayers) value = "3 MISSIONS";
            break;
        case A::Versus: label = "VERSUS DUEL"; value = "FIRST TO TEN"; break;
        case A::StartCoOp: label = menu.progress.available() ? "NEW CO-OP CAMPAIGN" : "JOIN CAMPAIGN CO-OP"; break;
        case A::Continue:
            label = menu.progress.coOp ? "CONTINUE CO-OP" : "CONTINUE";
            value = missionName(menu.progress.next);
            break;
        case A::Arena: label = "ARENA"; value = arenaLabel(menu.selectedArena); break;
        case A::Format: label = "MODE"; value = menu.opponent == Opponent::FreeForAll ? "FREE-FOR-ALL" : "DUEL"; break;
        case A::Rival: label = "OPPONENT"; value = opponentName(menu.opponent); break;
        case A::Difficulty:
            label = "DIFFICULTY";
            std::snprintf(number, sizeof(number), "%s %d", difficultyName(menu.difficulty), menu.difficulty);
            value = number;
            break;
        case A::Voice: label = "VOICES"; value = menu.voice ? "ON" : "OFF"; break;
        case A::Volume:
            label = "VOLUME";
            std::snprintf(number, sizeof(number), "%d%%", menu.volume);
            value = number;
            break;
        case A::Render:
            label = "RENDER SIZE";
            std::snprintf(number, sizeof(number), "%d X %d", menu.width(), menu.height());
            value = number;
            break;
        case A::Callsign: label = "CALLSIGN"; value = menu.callsign.data(); break;
        }
        p.text(menuX + 12, py + 9, label, active ? accent : paper);
        const auto text = std::string_view(value);
        p.text(menuX + menuWidth - 12 - int(text.size()) * 12, py + 9, text, active ? paper : muted);
    }
    if (showSaveError)
        p.text(menuX + 12, contentBottom - 19, "COULD NOT SAVE CAMPAIGN TO STORAGE", alert, 1);
    const char* confirm = "CROSS";
    const char* back = "CIRCLE";
#ifdef FOD_LOCAL_DUEL
    if (const auto labels = menuLabels(menu.controls); labels.confirm) {
        confirm = labels.confirm;
        back = labels.back;
    }
#endif
    char hint[72];
    std::snprintf(hint, sizeof(hint), "%s SELECT / %s %s", confirm, back,
                  home ? (s.match == Match::Paused ? "RESUME" : "MAIN MENU") : "BACK");
    p.rect(menuX - 8, contentBottom + 2, menuWidth + 16, 22, ink);
    p.text(menuX + 12, contentBottom + 10, hint, muted, 1);
}

void comms(const Painter& p, Speaker speaker, int y) {
    const std::string_view name = speaker == Speaker::Hanz ? "HANZ" : "IPWNDURMOM";
    p.rect(15, y, 29 + int(name.size()) * 12, 20, panel);
    p.rect(15, y, 2, 20, playerColor);
    // Speaker cone with two sound waves, at the panel's native resolution.
    p.rect(22, y + 7, 3, 6, playerColor);
    for (int col = 0; col < 4; ++col) p.rect(25 + col, y + 7 - col, 1, 6 + col * 2, playerColor);
    p.line(31, y + 7, 32, y + 8, playerColor);
    p.line(32, y + 8, 32, y + 11, playerColor);
    p.line(32, y + 11, 31, y + 12, playerColor);
    p.line(34, y + 5, 36, y + 7, playerColor);
    p.line(36, y + 7, 36, y + 12, playerColor);
    p.line(36, y + 12, 34, y + 14, playerColor);
    p.text(41, y + 3, name, paper);
}

void drawRadioBanner(const Painter& p, const MissionSnapshot& m) {
    if (m.radioTimer <= 0 || !m.radioLine[0]) return;
    constexpr int x = 148, w = 504, h = 48;
    // Keep subtitles below the thermal aperture even while the player lowers the rifle.
    const int y = timberlineRetreat(m) ? 465 - h : 342;
    // Delta Squad speaks over the radio in teal; the Marsh family speaks in person in gold.
    const bool squad = !m.radioSpeaker[0] || !std::strncmp(m.radioSpeaker, "DELTA", 5);
    const uint16_t voice = squad ? playerColor : gold;
    p.rect(x, y, w, h, ink);
    p.rect(x, y, 4, h, voice);
    p.rect(x, y, w, 1, lineColor);
    p.rect(x, y + h - 1, w, 1, lineColor);
    p.text(x + 12, y + 6, m.radioSpeaker[0] ? m.radioSpeaker : "DELTA-1", voice, 1);
    const std::string_view line(m.radioLine);
    constexpr size_t maxChars = 76;
    if (line.size() <= maxChars) {
        p.text(x + 12, y + 20, line, paper, 1);
    } else {
        size_t split = line.rfind(' ', maxChars);
        if (split == std::string_view::npos || split < 24) split = maxChars;
        p.text(x + 12, y + 18, line.substr(0, split), paper, 1);
        const size_t next = split < line.size() && line[split] == ' ' ? split + 1 : split;
        p.text(x + 12, y + 31, line.substr(next), paper, 1);
    }
}

// The opening card types out over the closed eyes before the wake-up.
void drawTitleCard(const Painter& p, float t) {
    const struct { const char* text; uint16_t color; int scale, y; float at; } lines[]{
        {"DAY ONE OF THE LANDINGS  /  DUSK", rgb(248, 126, 58), 1, 152, .2f},
        {"COLORADO FRONT RANGE  /  19:47", paper, 2, 170, .5f},
        {"DELTA SQUAD  -  OPERATION BRIAR", muted, 1, 206, 1.4f},
        {"EXTRACT DR. NORA MARSH AND HER FAMILY FROM BRIAR HOUSE.", paper, 1, 240, 2.f},
        {"WARHORSE 2-1 WAS HIT ON FINAL APPROACH.", alert, 1, 262, 3.1f},
    };
    p.rect(0, 15, 800, 450, ink); // Eyes fully shut behind the card
    p.rect(150, 140, 56, 2, rgb(248, 118, 52));
    for (const auto& line : lines) {
        const std::string_view text(line.text);
        const size_t shown = size_t(std::clamp((t - line.at) * 42.f, 0.f, float(text.size())));
        if (shown) p.text(150, line.y, text.substr(0, shown), line.color, line.scale);
    }
    p.text(340, 444, "JUMP TO SKIP", muted, 1);
}

// Project a world point into the 800 x 480 HUD; the world view fills rows 15 to 464.
bool projectToHud(const Snapshot& s, Vec3 point, float& x, float& y) {
    const Camera& c = s.camera;
    const Vec3 front = forward(c.yaw, c.pitch);
    const Vec3 right{std::cos(c.yaw), 0, -std::sin(c.yaw)};
    const Vec3 up = cross(front, right);
    const Vec3 d = point - c.position;
    const float z = dot(d, front);
    const float focal = 225.f / std::tan(c.fov * 0.5f);
    const float depth = std::max(std::abs(z), 0.05f);
    x = 400.f + dot(d, right) * focal / depth;
    y = 240.f - dot(d, up) * focal / depth;
    return z > 0.1f;
}

// A diamond and range over the current location objective. Off screen it rides the
// edge of the view and points the way.
void objectiveMarker(const Painter& p, const Snapshot& s, const MissionSnapshot& m) {
    if (!m.hasWaypoint || s.scope > 0.3f || s.ads > 0.7f || s.dead) return;
    const bool operation = m.phase == MissionPhase::Operation;
    const bool saddle = operation && m.operation.stage == OperationStage::Steer;
    const bool landing = operation && m.operation.stage == OperationStage::Fight;
    const bool mark = operation && m.operation.stage == OperationStage::Call && m.operation.jammer < 0;
    const Vec3 target = game_detail::atHeight(m.waypoint, saddle ? 4.f : mark ? .05f : 1.7f);
    const float range = game_detail::planar(m.waypoint - s.player.position);
    if (range < (m.waypointIsRoute ? .8f : 2.5f)) return;
    float x = 0, y = 0;
    const bool ahead = projectToHud(s, target, x, y);
    constexpr float left = 40, right = 760, top = 74, bottom = 326;
    bool clamped = !ahead || x < left || x > right || y < top || y > bottom;
    if (!ahead) {
        // Dividing by |z| keeps left and right true behind the camera; pin it to that side.
        x = 400.f + (x < 400.f ? -1.f : 1.f) * 1000.f;
        y = 240.f;
    }
    if (clamped) {
        const float dx = x - 400.f, dy = y - 200.f;
        const float sx = dx != 0 ? (dx > 0 ? (right - 400.f) : (400.f - left)) / std::abs(dx) : 1e9f;
        const float sy = dy != 0 ? (dy > 0 ? (bottom - 200.f) : (200.f - top)) / std::abs(dy) : 1e9f;
        const float k = std::min({sx, sy, 1.f});
        x = 400.f + dx * k;
        y = 200.f + dy * k;
    }
    const int cx = int(x), cy = int(y);
    const uint16_t color = clamped ? muted : operation && !m.waypointIsRoute ? operationColor(m.operation) : accent;
    for (int r = 6; r <= 7; ++r) {
        p.line(cx - r, cy, cx, cy - r, color);
        p.line(cx, cy - r, cx + r, cy, color);
        p.line(cx + r, cy, cx, cy + r, color);
        p.line(cx, cy + r, cx - r, cy, color);
    }
    p.rect(cx - 1, cy - 1, 3, 3, color);
    if (clamped) {
        // Chevron pointing out toward the objective.
        const float dx = x - 400.f, dy = y - 200.f;
        const float len = std::max(1.f, std::sqrt(dx * dx + dy * dy));
        const float ux = dx / len, uy = dy / len;
        const int tipX = cx + int(ux * 13), tipY = cy + int(uy * 13);
        p.line(tipX, tipY, cx + int(ux * 8 - uy * 4), cy + int(uy * 8 + ux * 4), color);
        p.line(tipX, tipY, cx + int(ux * 8 + uy * 4), cy + int(uy * 8 - ux * 4), color);
    }
    char label[20];
    const char* tag = m.waypointIsRoute ? "ROUTE %dM" : saddle ?
        (game_detail::planarSquared(m.waypoint - m.operation.saddle) < 1.f ? "SADDLE %dM" : "NORTH TURN %dM") :
        landing ? "LANDING %dM" : mark ? "MARK %dM" : "%dM";
    std::snprintf(label, sizeof(label), tag, int(range + 0.5f));
    const int width = int(std::strlen(label)) * 6;
    p.text(std::clamp(cx - width / 2, 4, 796 - width), cy + 11, label, color, 1);
}

// Timberline tracks readings and ridge progress. The Array shows the lock on the saddle.
void drawOperationProgress(const Painter& p, const MissionSnapshot& m) {
    const auto& op = m.operation;
    constexpr int x = 185, y = 51, gap = 2;
    const auto tint = operationColor(op);
    if (timberlineRetreat(m)) {
        constexpr int legs = int(std::size(mission_detail::timberline::fallbacks));
        constexpr int width = (430 - (legs - 1) * gap) / legs;
        for (int i = 0; i < legs; ++i)
            p.rect(x + i * (width + gap), y, width, 3, i < op.retreatLeg ? playerColor : i == op.retreatLeg ? tint : panel);
        return;
    }
    if (m.chapter == MissionChapter::Timberline) {
        constexpr int calls = OperationView::callsNeeded;
        constexpr int step = (430 + gap) / calls, width = step - gap;
        for (int i = 0; i < calls; ++i) {
            p.rect(x + step * i, y, width, 4, panel);
            if (i < op.calls) p.rect(x + step * i, y, width, 4, playerColor);
            if (i == op.calls && op.targetSet) p.rect(x + step * i, y, int(width * (.15f + .85f * op.lock)), 4, tint);
        }
        return;
    }
    const bool capture = op.stage == OperationStage::ScoutApproach;
    if (!capture && op.stage != OperationStage::Steer && op.stage != OperationStage::ScoutTransfer &&
        op.stage != OperationStage::ScoutBridge && op.stage != OperationStage::ScoutReclaim) return;
    p.rect(x, y, 430, 4, panel);
    p.rect(x, y, int(430 * std::clamp(capture ? m.scout.capture : op.lock, 0.f, 1.f)), 4,
           op.pulled || m.scout.autoReturn ? alert : playerColor);
}

void drawSpotter(const Painter& p, const Snapshot& s, const MissionSnapshot& m) {
    const int slot = m.operation.spotted;
    if (slot < 4 || !thermalScope(s)) return;
    const auto& a = m.actors[size_t(slot)];
    if (!a.active || a.dead) return;
    const bool drone = a.role == MissionRole::AlienDrone;
    const Vec3 point = game_detail::atHeight(a.actor.position, drone ? 0.f : 1.25f);
    float x = 0.f, y = 0.f;
    const uint16_t tint = rgb(246, 214, 120);
    if (projectToHud(s, point, x, y) && std::abs(x - 400) < thermalLensHalfWidth - 26 &&
        std::abs(y - 240) < thermalLensHalfHeight - 26) {
        const int l = int(x) - 14, r = int(x) + 14, top = int(y) - 15, bot = int(y) + 15;
        for (int side : {l, r}) {
            p.line(side, top, side, bot, tint);
            p.line(side, top, side == l ? l + 5 : r - 5, top, tint);
            p.line(side, bot, side == l ? l + 5 : r - 5, bot, tint);
        }
    }
    char label[28];
    const int range = int(game_detail::planar(a.actor.position - s.player.position) + .5f);
    std::snprintf(label, sizeof(label), "DELTA-1 / %s %dM", drone ? "DRONE" : "CONTACT", range);
    const int width = int(std::strlen(label)) * 6;
    p.text(400 + thermalLensHalfWidth - 29 - width, 240 - thermalLensHalfHeight + 14, label, tint, 1);
}

// Draw the called ship's destination and the pointer over intervening scenery.
void drawSignalMarkers(const Painter& p, const Snapshot& s, const MissionSnapshot& m) {
    if (m.phase != MissionPhase::Operation || s.scope > .3f) return;
    const auto& op = m.operation;
    const auto mark = [&](Vec3 at, float height, uint16_t color, const char* format, bool box) {
        float x = 0, y = 0;
        if (!projectToHud(s, game_detail::atHeight(at, height), x, y) || x < 20 || x > 780 || y < 60 || y > 440) return;
        const int cx = int(x), cy = int(y);
        if (box) {
            p.line(cx - 6, cy - 6, cx + 6, cy - 6, color);
            p.line(cx + 6, cy - 6, cx + 6, cy + 6, color);
            p.line(cx + 6, cy + 6, cx - 6, cy + 6, color);
            p.line(cx - 6, cy + 6, cx - 6, cy - 6, color);
        } else {
            p.line(cx - 5, cy, cx + 5, cy, color);
            p.line(cx, cy - 5, cx, cy + 5, color);
        }
        if (!format) return;
        char label[24];
        std::snprintf(label, sizeof(label), format, int(game_detail::planar(at - s.player.position) + .5f));
        const int width = int(std::strlen(label)) * 6;
        p.text(std::clamp(cx - width / 2, 4, 796 - width), cy + 10, label, color, 1);
    };
    const auto tint = operationColor(op);
    if (op.stage == OperationStage::Call) {
        if (op.targetSet) mark(op.target, .2f, tint, op.pulled ? "PULLED %dM" : "LANDING %dM", true);
        if (op.painting && op.aimValid) mark(op.aim, 0.f, paper, nullptr, false);
    } else if (op.stage == OperationStage::Steer) {
        if (!op.clearingBurned) mark(op.clearing, .2f, alert, "KEEP CLEAR %dM", false);
        mark(op.ship, 2.5f, tint, op.pulled ? "DRIFTING %dM" : "BEAM %dM", true);
        if (op.painting) mark(op.aim, 0.f, paper, nullptr, false);
    } else if (op.stage == OperationStage::ScoutCargo) {
        mark(mission_detail::scout::cargoChute, .65f, playerColor, "CARGO CHUTE %dM", true);
    } else if (op.stage == OperationStage::ScoutTransfer) {
        mark(mission_detail::scout::portChute, .65f, playerColor, "PORT %dM", true);
        mark(mission_detail::scout::starboardChute, .65f, gold, "STARBOARD %dM", true);
    } else if ((op.stage == OperationStage::ScoutBridge || op.stage == OperationStage::ScoutReclaim) && m.flagshipBeam) {
        mark(op.clearing, .4f, alert, op.clearingBurned ? "CLEARING LOST" : "NORTH CLEARING", false);
        mark(op.saddle, .4f, playerColor, "WEST SADDLE", false);
        mark(op.ship, .7f, tint, op.pulled ? "BEAM DRIFT" : "LANDING BEAM", true);
    } else if (op.stage == OperationStage::ScoutDescent) {
        using namespace mission_detail::scout;
        mark(op.clearingBurned ? roadApproach : clearing, .4f, playerColor,
             op.clearingBurned ? "SOUTH ROAD" : "NORTH CLEARING", true);
    }
}

// The Signal button and what it does right now, low in the centre of the view.
void drawSignalPrompt(const Painter& p, const MissionSnapshot& m, const MenuState& menu, bool held) {
    if (!m.signalReady || !m.signalHint[0]) return;
    const std::string_view button(signalButton(menu)), hint(m.signalHint);
    const int bw = int(button.size()) * 6 + 10;
    const int width = bw + 10 + int(hint.size()) * 6 + 12;
    const int x = 400 - width / 2, y = 398;
    const auto& op = m.operation;
    const bool warning = op.jammer >= 0 || (op.painting && !op.aimValid) ||
                         std::string_view(m.signalHint).find("TOO CLOSE") != std::string_view::npos ||
                         std::string_view(m.signalHint).find("NO CLEAR") != std::string_view::npos ||
                         std::string_view(m.signalHint).find("GET BACK") != std::string_view::npos;
    p.rect(x, y, width, 18, ink);
    p.rect(x + 3, y + 3, bw, 12, held ? accent : lineColor);
    p.text(x + 8, y + 6, button, held ? ink : paper, 1);
    p.text(x + bw + 13, y + 6, hint, warning ? alert : held ? accent : paper, 1);
}

void drawMissionHud(const Painter& p, const Assets& assets, const Snapshot& s, const MenuState& menu) {
    const auto& m = *s.mission;
    const bool inIntro = m.phase == MissionPhase::Intro;
    // Cutscenes (the wake-up and the exfil) strip the combat HUD back to the story.
    const bool piloting = m.operation.stage == OperationStage::ScoutFlight;
#ifdef FOD_LOCAL_DUEL
    const bool support = s.campaignSeat == 1;
#else
    constexpr bool support = false;
#endif
    const bool cutscene = inIntro || m.cinematic || piloting;
    const bool thermal = !cutscene && thermalScope(s);
    const bool calmBeat = m.phase == MissionPhase::Regroup || m.phase == MissionPhase::Incoming;
    const float shock = playerPlasmaShock(s);
    if (!cutscene) {
        scope(p, s);
        reticle(p, s);
    }
    // Concussion wake-up eyelid bars, heartbeat vignette, and cinematic letterbox framing.
    if (m.concussion > 0.01f) {
        const float blink1 = inIntro && m.introProgress > 0.16f && m.introProgress < 0.30f
            ? std::sin((m.introProgress - 0.16f) / 0.14f * pi) * 0.58f : 0.f;
        const float blink2 = inIntro && m.introProgress > 0.44f && m.introProgress < 0.56f
            ? std::sin((m.introProgress - 0.44f) / 0.12f * pi) * 0.36f : 0.f;
        const float close = std::clamp(m.concussion * (1.f - 0.45f * m.introProgress) + blink1 + blink2, 0.f, 1.f);
        const int lid = int(close * 222.f);
        if (lid > 0) {
            p.rect(0, 15, 800, lid, ink);
            p.rect(0, 465 - lid, 800, lid, ink);
        }
        const int side = int((m.concussion * 26.f) + m.heartbeatPulse * 14.f);
        if (side > 0) {
            const uint16_t edgeColor = m.heartbeatPulse > 0.25f ? rgb(68, 18, 18) : ink;
            p.rect(0, 15, side, 450, edgeColor);
            p.rect(800 - side, 15, side, 450, edgeColor);
        }
    }
    if (m.titleCard > 0.f) drawTitleCard(p, m.titleCard);
    if (m.letterbox > 0.01f) {
        const int bar = int(m.letterbox * 42.f);
        if (bar > 0) {
            p.rect(0, 15, 800, bar, ink);
            p.rect(0, 465 - bar, 800, bar, ink);
        }
    }
    if (s.damage > 0.12f) {
        const int thickness = 2 + int(std::clamp(s.damage, 0.0f, 1.0f) * 5);
        const auto tint = shock > 0.f ? rgb(35, 79, 169) : rgb(117, 21, 16);
        p.rect(0, 15, thickness, 450, tint);
        p.rect(800 - thickness, 15, thickness, 450, tint);
    }
    if (shock > .02f && !s.dead && !cutscene) {
        const float strength = shock / plasmaShockDuration;
        uint32_t noise = uint32_t(s.realTime * 26.0) + 11;
        const auto jitter = [&](unsigned max) {
            noise = noise * 1664525u + 1013904223u;
            return int((noise >> 16) % max);
        };
        const uint16_t shell = rgb(50, unsigned(85 + 88 * strength), unsigned(136 + 112 * strength));
        const uint16_t core = rgb(unsigned(66 + 143 * strength), unsigned(125 + 113 * strength), 255);
        for (int side : {1, -1}) {
            const int edge = side > 0 ? 0 : 799;
            for (int fork = 0; fork < 3; ++fork) {
                const int y = 58 + fork * 124 + jitter(66);
                const int reach = 19 + int(63 * strength) + jitter(18);
                const int elbowX = edge + side * reach / 2, elbowY = y - 34 + jitter(55);
                const int tipX = edge + side * reach;
                p.line(edge, y, elbowX, elbowY, core);
                p.line(elbowX, elbowY, tipX, elbowY - 18 + jitter(35), core);
                p.line(elbowX, elbowY, elbowX + side * (6 + jitter(22)), elbowY + 22, shell);
            }
        }
    }
    const bool showTactical = !inIntro || m.introProgress > 0.72f;
    if (showTactical && !cutscene) map(p, assets, s, 15, 28, 107, false);

    // Top objective bar (hidden during initial blackout so the opening cinema breathes).
    if (showTactical) {
        const bool operation = m.phase == MissionPhase::Operation;
        const auto status = operation ? operationColor(m.operation) : playerColor;
        p.rect(185, 15, 430, 36, ink);
        p.rect(185, 15, 430, 2, (cutscene || m.phase == MissionPhase::Overwatch || calmBeat) ? gold : status);
        const std::string_view obj = support && piloting ? "GUIDE THE APPROACH" : m.objective;
        p.text(400 - int(obj.size()) * 6, 20, obj, paper, 2);
        const std::string_view sub = support && piloting ? "LOOK FOR BEACONS / HOLD SIGNAL TO STABILIZE" : m.subObjective;
        const bool warning = operation && (m.operation.jammer >= 0 || m.operation.pulled);
        p.text(400 - int(sub.size()) * 3, 39, sub, warning ? status : muted, 1);
        if (operation) {
            drawOperationProgress(p, m);
        } else if (m.phase == MissionPhase::Extraction && m.evacTotal > 0.f && !m.cinematic) {
            const float done = std::clamp(1.f - m.evacSeconds / m.evacTotal, 0.f, 1.f);
            p.rect(185, 51, 430, 3, panel);
            p.rect(185, 51, int(430 * done), 3, done >= 1.f ? gold : playerColor);
        }
    }
    if (!cutscene) {
        objectiveMarker(p, s, m);
        drawSpotter(p, s, m);
    }

    // Compact civilian + hostile counters.
    if (!cutscene) {
        char civText[24], hostText[24];
        bool danger = m.chapter == MissionChapter::BriarHouse && m.civiliansLost > 0;
        if (m.chapter == MissionChapter::BriarHouse) {
            std::snprintf(civText, sizeof(civText), "CIVS %d/%d", m.civiliansSaved, m.civiliansTotal);
        } else if (timberlineRetreat(m)) {
            const float range = m.operation.pursuitRange;
            danger = range >= 0.f && range < 8.f;
            if (range >= 0.f) std::snprintf(civText, sizeof(civText), "PURSUIT %dM", int(range + .5f));
            else std::snprintf(civText, sizeof(civText), "DROPS INBOUND");
        } else if (m.chapter == MissionChapter::Timberline) {
            std::snprintf(civText, sizeof(civText), "READINGS %d/%d", m.operation.calls, OperationView::callsNeeded);
        } else if (m.operation.stage == OperationStage::Steer) {
            std::snprintf(civText, sizeof(civText), "DISH %dM", int(m.operation.dishDistance));
            danger = m.operation.dishDistance < 14.f;
        } else {
            std::snprintf(civText, sizeof(civText), "M3/3");
        }
        const char* enemyLabel = m.phase == MissionPhase::Overwatch ? "TARGETS %d" : "HOSTILES %d";
        std::snprintf(hostText, sizeof(hostText), enemyLabel, m.aliensRemaining);
        p.rect(625, 15, 104, 36, panel);
        p.text(632, 20, civText, danger ? alert : gold, 1);
        p.text(632, 34, hostText, m.aliensRemaining > 0 ? rgb(72, 248, 255) : playerColor, 1);
    }

    p.rect(738, 15, 62, 15, ink);
#ifdef FOD_LOCAL_DUEL
    if (s.campaignSeat >= 0) p.text(746, 20, support ? "PLAYER 2" : "PLAYER 1", support ? gold : playerColor, 1);
    else
#endif
        p.text(746, 20, "PAUSE", muted, 1);

    if (!support && inIntro && m.introProgress > 0.22f && m.titleCard <= 0.f) {
        p.text(328, 444, "JUMP TO STAND", muted, 1);
    }
    if (m.cinematic && !support) p.text(340, 444, "JUMP TO SKIP", muted, 1);
    if (piloting) {
        const auto offset = std::clamp(m.scout.flightError * 7.f, -56.f, 56.f);
        const int reticle = 400 + int(offset);
        const auto color = std::abs(m.scout.flightError) < 2.7f ? playerColor : alert;
        p.rect(328, 406, 144, 34, ink);
        p.line(342, 427, 458, 427, muted);
        p.rect(398, 421, 4, 13, paper);
        p.rect(reticle - 2, 423, 5, 9, color);
        char beacon[18];
        std::snprintf(beacon, sizeof(beacon), "BEACON %d/%d", m.scout.beaconIndex + 1, m.scout.beaconCount);
        p.text(370, 409, beacon, color, 1);
#ifdef FOD_LOCAL_DUEL
        if (support) {
            char prompt[32];
            std::snprintf(prompt, sizeof(prompt), "HOLD %s TO STABILIZE", signalButton(menu));
            const std::string_view label = s.stabilizingScout ? "STABILIZING" : prompt;
            p.rect(280, 442, 240, 17, ink);
            p.text(400 - int(label.size()) * 3, 447, label, s.stabilizingScout ? playerColor : paper, 1);
        }
#endif
    }

    if (!s.dead && !cutscene) {
        char ammo[18], hpText[16];
        std::snprintf(ammo, sizeof(ammo), "%02d / %02d", s.clip, s.reserve);
        const int hp = int(std::ceil(std::clamp(s.player.health, 0.f, 100.f)));
        std::snprintf(hpText, sizeof(hpText), "HP %3d", hp);
        p.rect(654, 383, 131, 16, panel);
        p.rect(658, 393, int(123 * std::clamp(s.player.health / 100.f, 0.f, 1.f)), 3,
               hp > 45 ? playerColor : hp > 20 ? gold : alert);
        p.text(665, 385, hpText, hp > 25 ? paper : alert, 1);
        p.rect(654, 403, 131, 48, panel);
        p.text(665, 410, s.weapon == Weapon::Mp44 ? "MP44" : s.weapon == Weapon::Sniper ? "M40A3" : "USP .45", muted, 1);
        p.text(665, 426, ammo, s.clip ? paper : alert, 2);
    } else if (s.dead) {
        p.rect(288, 407, 224, 31, panel);
        std::string_view status = "OPERATIVE DOWN";
#ifdef FOD_LOCAL_DUEL
        if (s.campaignSeat >= 0 && s.rival.health > 0.f && m.phase != MissionPhase::Failed) status = "REJOINING SQUAD";
#endif
        p.text(312, 415, status, alert);
    }

    if (m.friendlyFireWarn) {
        p.rect(284, 58, 232, 24, ink);
        p.rect(284, 58, 232, 2, alert);
        p.text(302, 65, "CHECK YOUR FIRE!", alert, 2);
    } else if (m.warningLock && !cutscene && !s.dead) {
        const bool flash = (int(s.realTime * 10.0) & 1) == 0;
        const uint16_t warnColor = flash ? rgb(255, 112, 58) : alert;
        p.rect(224, 58, 352, 20, ink);
        p.rect(224, 58, 352, 2, warnColor);
        p.text(242, 64, "PLASMA CHARGE - HIT CHEST OR TAKE COVER", warnColor, 1);
    }
    if (m.tacticalCalloutTimer > 0.f && m.tacticalCallout[0] && !cutscene && !s.dead &&
        !(thermal && m.signalReady)) {
        const std::string_view badge(m.tacticalCallout);
        const int bw = int(badge.size()) * 6 + 24;
        const int bx = 400 - bw / 2;
        const int y = thermal ? 398 : 316;
        const uint16_t c = m.tacticalCalloutColor ? m.tacticalCalloutColor : gold;
        p.rect(bx, y, bw, 18, ink);
        p.rect(bx, y, 3, 18, c);
        p.rect(bx + bw - 3, y, 3, 18, c);
        p.text(bx + 12, y + 5, badge, c, 1);
    }
    if (!cutscene && !s.dead) {
        drawSignalMarkers(p, s, m);
        if (!support) drawSignalPrompt(p, m, menu, m.operation.painting);
    }
    // Subtitles always show, since people talk over the action; the setting only mutes radio sound.
    drawRadioBanner(p, m);
    if (s.damage > 0.12f && !s.dead) {
        const float bearing = s.damageFromYaw - s.playerYaw;
        const int x = 400 + int(std::sin(bearing) * 64), y = 240 - int(std::cos(bearing) * 64);
        p.rect(x - 3, y - 3, 7, 7, alert);
    }
    if ((s.airstrikeReady || s.apacheReady) && !cutscene) {
        p.rect(15, 422, 194, 28, panel);
        p.text(24, 431, s.airstrikeReady ? "UP  AIRSTRIKE" : "UP  APACHE", accent);
    }
    if (s.device == Device::Transmitter) {
        p.rect(298, 378, 203, 71, panel);
        p.rect(298, 378, 203, 3, accent);
        p.text(312, 391, "APACHE CONTROL", paper, 2);
        p.rect(312, 419, int(174 * std::clamp(s.deviceAmount, 0.0f, 1.0f)), 9, accent);
    }
    if (s.killConfirm > 0 && !cutscene && !(thermal && (m.warningLock || m.friendlyFireWarn))) {
        const auto color = s.headshot ? gold : rgb(72, 248, 255);
        p.rect(267, 56, 266, thermal ? 28 : 36, ink);
        p.rect(267, 56, 266, 2, color);
        const std::string_view label = s.headshot ? "CRITICAL KILL" : "ALIEN DOWN";
        const int scale = thermal ? 2 : 3;
        p.text(400 - int(label.size()) * 3 * scale, thermal ? 63 : 65, label, color, scale);
    }
    if (m.fade > 0.f) {
        // The exfil closes the view the way the wake-up opened it.
        const int lid = int(std::clamp(m.fade, 0.f, 1.f) * 225.f);
        const auto fade = m.operation.stage == OperationStage::ScoutBoard ? rgb(214, 225, 255) : ink;
        p.rect(0, 15, 800, lid, fade);
        p.rect(0, 465 - lid, 800, lid, fade);
    }
}

void paintHud(const Painter& p, const Assets& assets, const Snapshot& s, const MenuState& menu, Speaker speaker) {
    p.rect(0, 0, 800, 15, ink);
    p.rect(0, 465, 800, 15, ink);
    const bool speaking = menu.voice && menu.volume > 0 && speaker != Speaker::None;
    if (s.match != Match::Playing) {
        menus(p, s, menu, assets.arena);
        if (s.match == Match::Results) {
            if (speaking) comms(p, speaker, 138);
            if (s.freeForAll && s.killNotice.remaining > 0) drawKillNotice(p, s.killNotice, menu, 412);
        }
        return;
    }
    if (s.mission && s.mission->active) {
        drawMissionHud(p, assets, s, menu);
        return;
    }
    scope(p, s);
    reticle(p, s);
    if (s.damage > 0.12f) {
        const int thickness = 2 + int(std::clamp(s.damage, 0.0f, 1.0f) * 5);
        p.rect(0, 15, thickness, 450, rgb(117, 21, 16));
        p.rect(800 - thickness, 15, thickness, 450, rgb(117, 21, 16));
    }
    map(p, assets, s, 15, 28, 107, false);
    char score[18], ammo[18];
    std::snprintf(score, sizeof(score), "%02d  -  %02d", s.playerScore, s.rivalScore);
    std::snprintf(ammo, sizeof(ammo), "%02d / %02d", s.clip, s.reserve);
    const bool showKillNotice = s.freeForAll && s.killNotice.remaining > 0 && s.device != Device::Laptop;
    if (s.freeForAll) {
        freeForAllScore(p, s, menu, 232, 15, 336);
        if (showKillNotice) drawKillNotice(p, s.killNotice, menu, 54);
    } else {
        p.rect(330, 15, 140, 30, ink);
        p.text(353, 23, score, paper);
    }
    p.rect(738, 15, 62, 15, ink);
    p.text(746, 20, "PAUSE", muted, 1);
    if (!s.dead) {
        p.rect(654, 403, 131, 48, panel);
        p.text(665, 410, s.weapon == Weapon::Mp44 ? "MP44" : s.weapon == Weapon::Sniper ? "M40A3" : "USP .45", muted, 1);
        p.text(665, 426, ammo, s.clip ? paper : alert, 2);
    } else {
        p.rect(307, 407, 186, 31, panel);
        p.text(324, 415, "RESPAWNING", paper);
    }
    if (s.uavSeconds > 0) {
        char countdown[20];
        std::snprintf(countdown, sizeof(countdown), "UAV ACTIVE %02dS", int(std::ceil(s.uavSeconds)));
        p.rect(15, 138, 107, 14, panel);
        p.text(21, 142, countdown, playerColor, 1);
    }
    const int commsY = s.uavSeconds > 0 ? 155 : 138;
    if (speaking) comms(p, speaker, commsY);
    if (s.strikeInbound) p.text(16, speaking ? commsY + 23 : 155, "AIRSTRIKE INBOUND", alert, 1);
    if (s.damage > 0.12f && !s.dead) {
        const float bearing = s.damageFromYaw - s.playerYaw;
        const int x = 400 + int(std::sin(bearing) * 64), y = 240 - int(std::cos(bearing) * 64);
        p.rect(x - 3, y - 3, 7, 7, alert);
    }
    if (s.airstrikeReady || s.apacheReady) {
        p.rect(15, 422, 194, 28, panel);
        p.text(24, 431, s.airstrikeReady ? "UP  AIRSTRIKE" : "UP  APACHE", accent);
    }
    if (s.device == Device::Laptop) {
        const int slide = int((1 - std::clamp(s.deviceAmount, 0.0f, 1.0f)) * 440);
        p.rect(230, 52 + slide, 341, 395, ink);
        p.rect(230, 52 + slide, 341, 4, accent);
        p.text(247, 68 + slide, "AIRSTRIKE TARGETING", paper);
        map(p, assets, s, 253, 91 + slide, 294, true);
        p.text(251, 398 + slide, "R2 MARK / UP CLOSE", accent, 2);
    } else if (s.device == Device::Transmitter) {
        p.rect(298, 378, 203, 71, panel);
        p.rect(298, 378, 203, 3, accent);
        p.text(312, 391, "APACHE CONTROL", paper, 2);
        p.rect(312, 419, int(174 * std::clamp(s.deviceAmount, 0.0f, 1.0f)), 9, accent);
    }
    if (s.killConfirm > 0) {
        const auto color = s.headshot ? gold : playerColor;
        if (!s.freeForAll) {
            p.rect(329, 15, 47, 30, lineColor);
            p.text(353, 23, std::string_view(score, 2), color);
        }
        const int y = showKillNotice ? 76 : 54;
        p.rect(267, y, 266, 39, ink);
        p.rect(267, y, 266, 2, color);
        const std::string_view label = s.headshot ? "HEADSHOT KILL" : "ENEMY DOWN";
        p.text(400 - int(label.size()) * 9, y + 10, label, color, 3);
    }
}

} // namespace

void drawHud(const Assets& assets, const Snapshot& s, const MenuState& menu, uint16_t* framebuffer, bool rotated, Speaker speaker) {
    paintHud(Painter{framebuffer, rotated}, assets, s, menu, speaker);
}

#ifdef FOD_LOCAL_DUEL
void drawDesktopHud(const Assets& assets, const Snapshot& s, const MenuState& menu, uint16_t* framebuffer,
                    int canvasWidth, Speaker speaker) {
    paintHud(Painter{framebuffer, false, 0, 0, 800, 480, canvasWidth}, assets, s, menu, speaker);
}
#endif

void drawLoading(const char* title, uint16_t* framebuffer, bool rotated) {
    const Painter p{framebuffer, rotated};
    p.rect(0, 0, 800, 480, ink);
    p.rect(100, 150, 600, 180, panel);
    p.rect(100, 150, 600, 4, gold);
    p.text(130, 177, gameTitle, paper, 4);
    p.text(130, 225, "LOADING", accent, 3);
    if (title) p.text(130, 268, std::string_view(title).substr(0, 44), muted);
}

void drawRecovery(const char* message, uint16_t* framebuffer, bool rotated) {
    const Painter p{framebuffer, rotated};
    p.rect(0, 0, 800, 480, ink);
    p.rect(100, 115, 600, 250, panel);
    p.rect(100, 115, 600, 4, alert);
    p.text(130, 142, gameTitle, paper, 4);
    p.text(130, 190, "LOAD GAME DATA", accent, 3);
    p.text(130, 232, "INSERT SD WITH FALL-OF-DUTY.FODP", muted);
    if (message) p.text(130, 259, std::string_view(message).substr(0, 44), alert);
    p.rect(130, 301, 540, 43, lineColor);
    p.text(146, 315, "TAP TO RETRY / CONTROLLER X", paper);
}

void drawNumberedFrame(uint32_t sequence, uint16_t* framebuffer, bool rotated) {
    const Painter p{framebuffer, rotated};
    p.rect(0, 0, 800, 480, sequence & 1 ? rgb(10, 20, 113) : rgb(71, 14, 29));
    p.rect(0, 0, 800, 15, rgb(191, 68, 41));
    p.rect(0, 465, 800, 15, rgb(41, 161, 109));
    p.text(8, 22, "1", paper, 6);
    p.text(755, 22, "2", paper, 6);
    p.text(8, 412, "3", paper, 6);
    p.text(755, 412, "4", paper, 6);
    char label[32];
    std::snprintf(label, sizeof(label), "FRAME %06lu", static_cast<unsigned long>(sequence));
    p.text(112, 184, label, paper, 8);
    p.rect(392, 232, 16, 16, sequence & 1 ? uint16_t(0xffff) : uint16_t(0));
    p.rect(int((sequence * 31) % 737), 278, 63, 50, accent);
}

void scaleWorld(const uint16_t* source, int width, int height, uint16_t* framebuffer, bool rotated, bool smooth) {
    if (smooth) {
        // Positions are in thirty-seconds of a source pixel, measured between pixel centers.
        struct Tap { int index, weight; };
        const auto tap = [](int output, int outputs, int inputs) {
            const int position = std::clamp((2 * output + 1) * inputs * 16 / outputs - 16, 0, (inputs - 1) * 32);
            const int index = std::min(position >> 5, inputs - 2);
            return Tap{index, position - index * 32};
        };
        const auto mix = [](uint16_t a, uint16_t b, int weight) {
            constexpr uint32_t mask = 0x07e0f81f;
            const uint32_t from = ((uint32_t(a) << 16) | a) & mask, to = ((uint32_t(b) << 16) | b) & mask;
            const uint32_t value = ((from * (32 - weight) + to * weight) >> 5) & mask;
            return uint16_t(value | (value >> 16));
        };
        std::array<Tap, 800> columns;
        for (int x = 0; x < 800; ++x) columns[x] = tap(x, 800, width);
        // Stretch each source row once, then blend the two rows around every output row.
        std::array<uint16_t, 800> lines[2];
        int held[2]{-1, -1};
        for (int y = 0; y < 450; ++y) {
            const Tap row = tap(y, 450, height);
            const uint16_t* pair[2];
            for (int i = 0; i < 2; ++i) {
                const int wanted = row.index + i, slot = wanted & 1;
                if (held[slot] != wanted) {
                    const auto* from = source + wanted * width;
                    for (int x = 0; x < 800; ++x)
                        lines[slot][x] = mix(from[columns[x].index], from[columns[x].index + 1], columns[x].weight);
                    held[slot] = wanted;
                }
                pair[i] = lines[slot].data();
            }
            const uint16_t* p0 = pair[0];
            const uint16_t* p1 = pair[1];
            const int w = row.weight;
            if (rotated) {
                uint16_t* dst = framebuffer + 15 + 449 - y;
                for (int x = 0; x < 800; ++x) dst[size_t(x) * 480] = mix(p0[x], p1[x], w);
            } else {
                uint16_t* out = framebuffer + (y + 15) * 800;
                if (w == 0) std::memcpy(out, p0, 800 * sizeof(uint16_t));
                else for (int x = 0; x < 800; ++x) out[x] = mix(p0[x], p1[x], w);
            }
        }
        return;
    }
    if (!rotated && width == 400 && height == 225) {
        for (int sy = 0; sy < 225; ++sy) {
            const uint16_t* row = source + sy * 400;
            uint16_t* out0 = framebuffer + (sy * 2 + 15) * 800;
            uint32_t* out32 = reinterpret_cast<uint32_t*>(out0);
            for (int sx = 0; sx < 400; ++sx) {
                const uint32_t p = row[sx];
                out32[sx] = p | (p << 16);
            }
            std::memcpy(out0 + 800, out0, 800 * sizeof(uint16_t));
        }
        return;
    }
    if (!rotated && width == 320 && height == 180) {
        // Exact 2.5x nearest-neighbor scaling: every 2 source pixels map to 5 output pixels (0,0,0,1,1),
        // matching x * 320 / 800 bit for bit without per-pixel division.
        int prevSy = -1;
        for (int y = 0; y < 450; ++y) {
            const int sy = (y * 2) / 5;
            uint16_t* out = framebuffer + (y + 15) * 800;
            if (sy == prevSy) {
                std::memcpy(out, out - 800, 800 * sizeof(uint16_t));
                continue;
            }
            prevSy = sy;
            const uint16_t* row = source + sy * 320;
            uint16_t* dst = out;
            for (int sx = 0; sx < 320; sx += 2) {
                const uint16_t a = row[sx], b = row[sx + 1];
                dst[0] = a; dst[1] = a; dst[2] = a; dst[3] = b; dst[4] = b;
                dst += 5;
            }
        }
        return;
    }
    if (rotated) {
        std::array<uint16_t, 450> yMap;
        for (int y = 0; y < 450; ++y) yMap[y] = uint16_t(y * height / 450);
        for (int x = 0; x < 800; ++x) {
            const uint16_t* colSrc = source + x * width / 800;
            uint16_t* column = framebuffer + size_t(x) * 480 + 15;
            for (int y = 0; y < 450; ++y) column[449 - y] = colSrc[size_t(yMap[y]) * width];
        }
    } else {
        std::array<uint16_t, 800> xMap;
        for (int x = 0; x < 800; ++x) xMap[x] = uint16_t(x * width / 800);
        for (int y = 0; y < 450; ++y) {
            const auto* row = source + y * height / 450 * width;
            uint16_t* out = framebuffer + (y + 15) * 800;
            for (int x = 0; x < 800; ++x) out[x] = row[xMap[x]];
        }
    }
}

} // namespace fod
