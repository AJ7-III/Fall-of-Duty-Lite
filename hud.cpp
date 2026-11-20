#include "render.hpp"
#include "app.hpp"
#include "assets.hpp"
#include "game_world.hpp"
#include "hud_font.hpp"
#include "mission.hpp"

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
        for (char c : text) {
            const auto* rows = hud::glyph(c);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (rows[row] & (16 >> col)) rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
            x += 6 * scale;
        }
    }
};

struct MapPoint { int x, y; };

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
    const auto arena = assets.extent;
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
    for (const auto& box : assets.obstacles) {
        // The mansion's lintels and ceilings sit above the floor-plan routes.
        if (assets.arena == Arena::BriarHouse && box.minY > .25f) continue;
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
        for (const auto& ma : snap.mission->actors) {
            if (!ma.active || ma.dead) continue;
            if (ma.role == MissionRole::Commander) {
                drawContact(ma.actor, playerColor);
            } else if (ma.role == MissionRole::Civilian) {
                if (!ma.rescued) drawContact(ma.actor, gold);
            } else if (ma.radar > 0 || ma.alerted || scanning) {
                drawContact(ma.actor, ma.variant > 0 ? alert : alienCyan);
            }
        }
        if (snap.mission->hasWaypoint) {
            const auto wp = project(snap.mission->waypoint.x, snap.mission->waypoint.z);
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

void scope(const Painter& p, const Snapshot& snap) {
    if (snap.weapon != Weapon::Sniper || snap.scope < 0.6f || snap.dead || snap.device != Device::None) return;
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
        const uint16_t color = s.lethalMarker ? (s.headshot ? gold : alert) : paper;
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

    // Header badge + iconic stacked title block.
    p.rect(startX, 34, 3, 12, gold);
    p.text(startX + 10, 36, menu.campaign ? "BRIAR HOUSE / DELTA SQUAD" : "OFFLINE TACTICAL SHOOTER", gold, 1);
    p.text(startX, 54, "FALL OF DUTY", paper, 4);
    p.text(startX - 1, 90, "LITE", paper, 7);
    p.rect(startX, 144, 56, 3, gold);

    // Dynamic tactical briefing line for whichever mode is highlighted.
    char brief[64];
    switch (StartItem(menu.selection)) {
    case StartItem::Campaign:
        std::snprintf(brief, sizeof(brief), "MISSION 1: BRIAR HOUSE - OPERATION BRIAR");
        break;
    case StartItem::BotMatch:
        std::snprintf(brief, sizeof(brief), "%s / %s / %s %d", arenaLabel(menu.selectedArena),
                      opponentName(menu.opponent), difficultyName(menu.difficulty), menu.difficulty);
        break;
    case StartItem::MultiplayerLan:
        std::snprintf(brief, sizeof(brief), "LOCAL 2P SPLIT-SCREEN & LAN LOBBY");
        break;
    default:
        std::snprintf(brief, sizeof(brief), "DIFFICULTY, RIVAL, AUDIO & DISPLAY");
        break;
    }
    p.text(startX, 158, brief, muted, 1);

    // Four sleek stacked mode cards.
    char botSub[48], lanSub[48];
    std::snprintf(botSub, sizeof(botSub), "%s  /  %s  /  LVL %d",
                  arenaLabel(menu.selectedArena), opponentName(menu.opponent), menu.difficulty);
    std::snprintf(lanSub, sizeof(lanSub), "2P LOCAL DUEL  /  %s", arenaLabel(menu.selectedArena));

    struct Card {
        StartItem id;
        int y, h;
        const char* title;
        const char* sub;
        const char* badge;
    };
    const Card cards[]{
        {StartItem::Campaign, campaignY, campaignHeight, "CAMPAIGN", "MISSION 1: BRIAR HOUSE", "M1"},
        {StartItem::BotMatch, startY, startHeight, "BOT MATCH", botSub, "AI"},
        {StartItem::MultiplayerLan, lanY, lanHeight, "MULTIPLAYER LAN", lanSub, "2P"},
        {StartItem::MatchSetup, setupY, setupHeight, "MATCH SETUP", "DIFFICULTY, RIVAL, VOICE & DISPLAY", ""},
    };
    for (const auto& c : cards) {
        const bool active = menu.selection == int(c.id);
        p.rect(startX, c.y, startWidth, c.h, active ? gold : panel);
        if (active) {
            p.rect(startX - 7, c.y, 3, c.h, paper);
        } else {
            p.rect(startX, c.y + c.h - 1, startWidth, 1, lineColor);
        }
        p.text(startX + 16, c.y + (c.h >= 50 ? 10 : 8), c.title, active ? ink : paper, 2);
        p.text(startX + 16, c.y + (c.h >= 50 ? 31 : 27), c.sub, active ? rgb(46, 38, 22) : muted, 1);
        if (c.badge[0]) {
            const int bx = startX + startWidth - 46;
            const int by = c.y + (c.h - 16) / 2;
            p.rect(bx, by, 22, 16, active ? ink : lineColor);
            p.text(bx + 5, by + 5, c.badge, active ? gold : paper, 1);
        }
        p.text(startX + startWidth - 18, c.y + (c.h - 14) / 2, ">", active ? ink : muted, 2);
    }

    // Compact footer status (kept above y = 445 so the bottom gradient stays clean).
    char status[48];
    std::snprintf(status, sizeof(status), "%s  /  %s %d", menu.callsign.data(),
                  difficultyName(menu.difficulty), menu.difficulty);
    p.text(startX, 416, status, muted, 1);
#ifdef FOD_LOCAL_DUEL
    if (const auto labels = menuLabels(menu.controls); labels.confirm) {
        char hint[52];
        const bool canSwapMap = menu.selection == int(StartItem::BotMatch) ||
                                menu.selection == int(StartItem::MultiplayerLan);
        if (canSwapMap) {
            std::snprintf(hint, sizeof(hint), "%s SELECT  /  L-R MAP", labels.confirm);
        } else {
            std::snprintf(hint, sizeof(hint), "%s SELECT", labels.confirm);
        }
        p.text(startX, 434, hint, paper, 1);
    }
#else
    p.text(startX, 434, "TAP OR PRESS X TO SELECT", paper, 1);
#endif
    (void)arena;
}

void lanScreen(const Painter& p, const MenuState& menu) {
    p.rect(menuX - 8, 46, menuWidth + 16, 387, ink);
    p.rect(menuX - 8, 46, menuWidth + 16, 4, gold);
    p.text(menuX + 10, 66, "MULTIPLAYER LAN", paper, 4);
    p.text(menuX + 12, 114, "LOCAL 2-PLAYER SPLIT-SCREEN & LAN LOBBY", muted, 1);

    const char* labels[LanRows]{
#ifdef FOD_LOCAL_DUEL
        "START 2P LOCAL DUEL",
#else
        "LOCAL 2P (DESKTOP ONLY)",
#endif
        "ARENA", "NETWORK LAN", "BACK TO MAIN MENU"
    };
    const char* values[LanRows]{"", arenaLabel(menu.selectedArena), "LOCAL 2P READY", ""};
    for (int row = 0; row < LanRows; ++row) {
        const int py = menuY + row * menuRowHeight;
        const bool active = row == menu.selection;
        p.rect(menuX, py, menuWidth, menuRowHeight - 2, active ? lineColor : panel);
        if (active) p.rect(menuX, py, 3, menuRowHeight - 2, gold);
        p.text(menuX + 12, py + 9, labels[row], row == LanStartRow ? gold : paper);
        const std::string_view val(values[row]);
        p.text(menuX + menuWidth - 12 - int(val.size()) * 6, py + 12, val, active ? paper : muted, 1);
    }
    p.rect(menuX, menuY + LanRows * menuRowHeight + 18, menuWidth, 72, panel);
    p.rect(menuX, menuY + LanRows * menuRowHeight + 18, 3, 72, playerColor);
    p.text(menuX + 12, menuY + LanRows * menuRowHeight + 28, "PAIR TWO GAMEPADS FOR INSTANT 2P DUEL.", paper, 1);
    p.text(menuX + 12, menuY + LanRows * menuRowHeight + 44, "PRESS LEFT/RIGHT ON ARENA TO SWITCH MAPS.", muted, 1);
    p.text(menuX + 12, menuY + LanRows * menuRowHeight + 60, "MULTI-DEVICE LAN SOCKET PLAY COMING NEXT.", muted, 1);
#ifdef FOD_LOCAL_DUEL
    if (const auto lbl = menuLabels(menu.controls); lbl.confirm) {
        char hint[58];
        std::snprintf(hint, sizeof(hint), "%s SELECT / %s BACK", lbl.confirm, lbl.back);
        p.rect(menuX - 8, 435, menuWidth + 16, 22, ink);
        p.text(menuX + 12, 443, hint, muted, 1);
    }
#endif
}

char missionGrade(const MissionSnapshot& m) {
    if (m.phase != MissionPhase::Victory) return 'F';
    // S rank: every civilian, no checkpoint reloads, under four and a half minutes.
    if (m.civiliansSaved == m.civiliansTotal && m.retries == 0 && m.missionSeconds < 270.f) return 'S';
    if (m.civiliansSaved == m.civiliansTotal) return 'A';
    if (m.civiliansSaved >= 2) return 'B';
    return 'C';
}

void menus(const Painter& p, const Snapshot& s, const MenuState& menu, Arena arena) {
    if (s.match == Match::Start && menu.lanMenu) {
        lanScreen(p, menu);
        return;
    }
    if (s.match == Match::Start && !menu.setup) {
        startScreen(p, menu, arena);
        return;
    }
    const bool isCampaign = s.match != Match::Start && (menu.campaign || (s.mission && s.mission->active));
    const char* title = s.match == Match::Start ? (isCampaign ? "MISSION SETUP" : "MATCH SETUP")
        : s.match == Match::Paused ? "PAUSED"
        : isCampaign ? (s.mission && s.mission->phase == MissionPhase::Victory ? "MISSION COMPLETE"
                        : s.mission && s.mission->phase == MissionPhase::Failed ? "MISSION FAILED" : "MISSION ENDED")
        : (s.playerScore >= 10 ? "VICTORY" : s.leadingRivalScore() >= 10 ? "DEFEAT" : "MATCH ENDED");
    p.rect(menuX - 8, 46, menuWidth + 16, 387, ink);
    p.rect(menuX - 8, 46, menuWidth + 16, 4,
           isCampaign && s.match == Match::Results && s.mission && s.mission->phase == MissionPhase::Failed ? alert : accent);
    p.text(menuX + 10, 66, title, paper, isCampaign && s.match == Match::Results ? 4 : 5);
    if (isCampaign && s.match == Match::Results && s.mission) {
        const auto& m = *s.mission;
        char summary[112];
        const int mins = int(m.missionSeconds) / 60, secs = int(m.missionSeconds) % 60;
        if (m.phase == MissionPhase::Failed) {
            std::snprintf(summary, sizeof(summary), "%s  /  SAVED %d/%d  /  KILLS %d",
                          m.failReason[0] ? m.failReason : "MISSION FAILED", m.civiliansSaved, m.civiliansTotal, m.aliensKilled);
        } else {
            std::snprintf(summary, sizeof(summary), "SAVED %d/%d  /  ALIENS %d  /  %02d:%02d  /  RANK %c",
                          m.civiliansSaved, m.civiliansTotal, m.aliensKilled, mins, secs, missionGrade(m));
        }
        p.text(menuX + 12, 104, summary, m.phase == MissionPhase::Victory ? gold : alert, 1);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "HEADSHOTS %d  /  CHECKPOINT RELOADS %d  /  %s %d", m.headshots, m.retries,
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
        if (isCampaign) std::snprintf(location, sizeof(location), "%s / DELTA SQUAD MISSION 1", arenaLabel(arena));
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
    const int count = s.match == Match::Start ? MenuRows - 1 : MenuRows;
    const char* primary = s.match == Match::Start ? "BACK TO START"
        : s.match == Match::Paused ? "RESUME"
        : isCampaign ? (s.mission && s.mission->phase == MissionPhase::Failed && s.mission->checkpointReached
                        ? "RETRY CHECKPOINT" : "PLAY AGAIN")
        : "REMATCH";
    const char* labels[MenuRows]{primary, "DIFFICULTY", isCampaign ? "MISSION" : "RIVAL",
                                 isCampaign ? "RADIO SOUND" : "RIVAL VOICE", "VOLUME", "RENDER SIZE", "CALLSIGN",
                                 s.match == Match::Paused ? (isCampaign ? "END MISSION" : "END MATCH") : "MAIN MENU"};
    char difficulty[26], volume[12];
    std::snprintf(difficulty, sizeof(difficulty), "%s %d", difficultyName(menu.difficulty), menu.difficulty);
    std::snprintf(volume, sizeof(volume), "%d%%", menu.volume);
    const char* renderSize = menu.compact ? "320 X 180" : "400 X 225";
#ifdef FOD_LOCAL_DUEL
    char desktopSize[24];
    if (menu.desktopRenderWidth) {
        std::snprintf(desktopSize, sizeof(desktopSize), "%d X %d", menu.width(), menu.height());
        renderSize = desktopSize;
    }
#endif
    const char* values[MenuRows]{"", difficulty, isCampaign ? "BRIAR HOUSE" : opponentName(menu.opponent),
                                menu.voice ? "ON" : "OFF", volume, renderSize, menu.callsign.data(), ""};
    for (int row = 0; row < count; ++row) {
        const int py = menuY + row * menuRowHeight;
        const bool active = row == menu.selection;
        p.rect(menuX, py, menuWidth, menuRowHeight - 2, active ? lineColor : panel);
        if (active) p.rect(menuX, py, 3, menuRowHeight - 2, accent);
        p.text(menuX + 12, py + 9, labels[row], (row == PlayRow) ? accent : paper);
        const auto text = std::string_view(values[row]);
        p.text(menuX + menuWidth - 12 - int(text.size()) * 12, py + 9, text, active ? paper : muted);
    }
#ifdef FOD_LOCAL_DUEL
    if (const auto labels = menuLabels(menu.controls); labels.confirm) {
        char hint[58];
        if (s.match == Match::Start) {
            std::snprintf(hint, sizeof(hint), "%s SELECT / %s BACK", labels.confirm, labels.back);
        } else {
            std::snprintf(hint, sizeof(hint), "%s SELECT", labels.confirm);
        }
        p.rect(menuX - 8, 435, menuWidth + 16, 22, ink);
        p.text(menuX + 12, 443, hint, muted, 1);
    }
#endif
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
    constexpr int x = 148, y = 342, w = 504, h = 48;
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
        {"DAY ONE OF THE LANDINGS", gold, 1, 152, .2f},
        {"COLORADO FRONT RANGE  /  05:47", paper, 2, 170, .5f},
        {"DELTA SQUAD  -  OPERATION BRIAR", muted, 1, 206, 1.4f},
        {"EXTRACT DR. NORA MARSH AND HER FAMILY FROM BRIAR HOUSE.", paper, 1, 240, 2.f},
        {"WARHORSE 2-1 WAS HIT ON APPROACH.", alert, 1, 262, 3.1f},
    };
    p.rect(0, 15, 800, 450, ink); // Eyes fully shut behind the card
    p.rect(150, 140, 44, 2, gold);
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
    const Vec3 target = game_detail::atHeight(m.waypoint, 1.7f);
    const float range = game_detail::planar(m.waypoint - s.player.position);
    if (range < 2.5f) return;
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
    const uint16_t color = clamped ? muted : accent;
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
    char label[12];
    std::snprintf(label, sizeof(label), "%dM", int(range + 0.5f));
    const int width = int(std::strlen(label)) * 6;
    p.text(std::clamp(cx - width / 2, 4, 796 - width), cy + 11, label, color, 1);
}

void drawMissionHud(const Painter& p, const Assets& assets, const Snapshot& s) {
    const auto& m = *s.mission;
    const bool inIntro = m.phase == MissionPhase::Intro;
    // Cutscenes (the wake-up and the exfil) strip the combat HUD back to the story.
    const bool cutscene = inIntro || m.cinematic;
    const bool calmBeat = m.phase == MissionPhase::Regroup || m.phase == MissionPhase::Incoming;
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
        p.rect(0, 15, thickness, 450, rgb(117, 21, 16));
        p.rect(800 - thickness, 15, thickness, 450, rgb(117, 21, 16));
    }
    const bool showTactical = !inIntro || m.introProgress > 0.72f;
    if (showTactical && !cutscene) map(p, assets, s, 15, 28, 107, false);

    // Top objective bar (hidden during initial blackout so the opening cinema breathes).
    if (showTactical) {
        p.rect(185, 15, 430, 36, ink);
        p.rect(185, 15, 430, 2, (cutscene || m.phase == MissionPhase::Overwatch || calmBeat) ? gold : playerColor);
        const std::string_view obj(m.objective);
        p.text(400 - int(obj.size()) * 6, 20, obj, paper, 2);
        const std::string_view sub(m.subObjective);
        p.text(400 - int(sub.size()) * 3, 39, sub, muted, 1);
        if (m.phase == MissionPhase::Extraction && m.evacTotal > 0 && !m.cinematic) {
            // Hold progress fills as the evac birds close in.
            const float done = std::clamp(1.f - m.evacSeconds / m.evacTotal, 0.f, 1.f);
            p.rect(185, 51, 430, 3, panel);
            p.rect(185, 51, int(430 * done), 3, done >= 1.f ? gold : playerColor);
        }
    }
    if (m.phase == MissionPhase::Rescue || m.phase == MissionPhase::Regroup || m.phase == MissionPhase::Incoming) {
        objectiveMarker(p, s, m);
    }

    // Compact civilian + hostile counters.
    if (!cutscene) {
        char civText[24], hostText[24];
        std::snprintf(civText, sizeof(civText), "CIVS %d/%d", m.civiliansSaved, m.civiliansTotal);
        const char* enemyLabel = m.phase == MissionPhase::Overwatch ? "TARGETS %d" : "HOSTILES %d";
        std::snprintf(hostText, sizeof(hostText), enemyLabel, m.aliensRemaining);
        p.rect(625, 15, 104, 36, panel);
        p.text(632, 20, civText, m.civiliansLost > 0 ? alert : gold, 1);
        p.text(632, 34, hostText, m.aliensRemaining > 0 ? rgb(72, 248, 255) : playerColor, 1);
    }

    p.rect(738, 15, 62, 15, ink);
    p.text(746, 20, "PAUSE", muted, 1);

    if (inIntro && m.introProgress > 0.22f && m.titleCard <= 0.f) {
        p.text(328, 444, "JUMP TO STAND", muted, 1);
    }
    if (m.cinematic) p.text(340, 444, "JUMP TO SKIP", muted, 1);

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
        p.text(312, 415, "OPERATIVE DOWN", alert);
    }

    if (m.friendlyFireWarn) {
        p.rect(284, 58, 232, 24, ink);
        p.rect(284, 58, 232, 2, alert);
        p.text(302, 65, "CHECK YOUR FIRE!", alert, 2);
    }
    // Mission dialogue carries the instructions, so subtitles always show; the setting mutes radio sound.
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
    if (s.killConfirm > 0 && !cutscene) {
        const auto color = s.headshot ? gold : rgb(72, 248, 255);
        p.rect(267, 56, 266, 36, ink);
        p.rect(267, 56, 266, 2, color);
        const std::string_view label = s.headshot ? "CRITICAL KILL" : "ALIEN DOWN";
        p.text(400 - int(label.size()) * 9, 65, label, color, 3);
    }
    if (m.fade > 0.f) {
        // The exfil closes the view the way the wake-up opened it.
        const int lid = int(std::clamp(m.fade, 0.f, 1.f) * 225.f);
        p.rect(0, 15, 800, lid, ink);
        p.rect(0, 465 - lid, 800, lid, ink);
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
        drawMissionHud(p, assets, s);
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
            for (int x = 0; x < 800; ++x) {
                const uint16_t pixel = mix(pair[0][x], pair[1][x], row.weight);
                if (rotated) framebuffer[size_t(x) * 480 + 15 + 449 - y] = pixel;
                else framebuffer[(y + 15) * 800 + x] = pixel;
            }
        }
        return;
    }
    if (rotated) {
        for (int x = 0; x < 800; ++x) {
            const int sourceX = x * width / 800;
            uint16_t* column = framebuffer + size_t(x) * 480 + 15;
            for (int y = 0; y < 450; ++y) column[449 - y] = source[y * height / 450 * width + sourceX];
        }
    } else {
        for (int y = 0; y < 450; ++y) {
            const auto* row = source + y * height / 450 * width;
            uint16_t* out = framebuffer + (y + 15) * 800;
            for (int x = 0; x < 800; ++x) out[x] = row[x * width / 800];
        }
    }
}

} // namespace fod
