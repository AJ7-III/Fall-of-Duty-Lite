#include "render.hpp"
#include "app.hpp"
#include "assets.hpp"
#include "game_world.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
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

    static size_t index(int x, int y, bool rotate) {
        return rotate ? size_t(x) * 480 + 479 - y : size_t(y) * 800 + x;
    }

    void pixel(int x, int y, uint16_t color) const {
        if (x >= left && x < right && y >= top && y < bottom) pixels[index(x, y, rotated)] = color;
    }

    void rect(int x, int y, int width, int height, uint16_t color) const {
        const int l = std::clamp(x, left, right), r = std::clamp(x + width, left, right);
        const int t = std::clamp(y, top, bottom), b = std::clamp(y + height, top, bottom);
        if (r <= l || b <= t) return;
        if (rotated) {
            for (int col = l; col < r; ++col) std::fill_n(pixels + index(col, b - 1, true), b - t, color);
        } else {
            for (int row = t; row < b; ++row) std::fill_n(pixels + index(l, row, false), r - l, color);
        }
    }

    void shadeTitle() const {
        constexpr int solidEnd = 260, fadeEnd = 554;
        rect(0, 15, solidEnd, 450, ink);
        for (int x = solidEnd; x < fadeEnd; ++x) {
            const unsigned light = (x - solidEnd) * 256 / (fadeEnd - solidEnd);
            const unsigned shade = 256 - light;
            for (int y = 15; y < 465; ++y) {
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
            const auto* rows = glyph(c);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (rows[row] & (16 >> col)) rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
            x += 6 * scale;
        }
    }

    static const uint8_t* glyph(char c) {
        static constexpr uint8_t alphabet[][7] = {
            {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
            {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
            {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
            {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
            {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
            {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
            {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
            {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
            {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
        };
        static constexpr uint8_t numbers[][7] = {
            {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
            {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
            {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
            {14,17,17,15,1,1,14}
        };
        static constexpr uint8_t space[7]{}, slash[7]{1,1,2,4,8,16,16}, minus[7]{0,0,0,31,0,0,0};
        static constexpr uint8_t plus[7]{0,4,4,31,4,4,0}, dot[7]{0,0,0,0,0,12,12};
        static constexpr uint8_t colon[7]{0,12,12,0,12,12,0}, underscore[7]{0,0,0,0,0,0,31};
        static constexpr uint8_t left[7]{0,2,4,8,4,2,0}, right[7]{0,8,4,2,4,8,0};
        static constexpr uint8_t percent[7]{25,25,2,4,8,19,19};
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return alphabet[c - 'A'];
        if (c >= '0' && c <= '9') return numbers[c - '0'];
        switch (c) {
        case '/': return slash;
        case '-': return minus;
        case '+': return plus;
        case '.': return dot;
        case ':': return colon;
        case '_': return underscore;
        case '<': return left;
        case '>': return right;
        case '%': return percent;
        default: return space;
        }
    }
};

struct MapPoint { int x, y; };

void map(const Painter& p, const Assets& assets, const Snapshot& snap, int x, int y, int size, bool laptop) {
    const bool scanning = !laptop && snap.uavSeconds > 0;
    const auto rim = scanning ? playerColor : lineColor;
    p.rect(x, y, size, size, rim);
    const Painter clipped{
        p.pixels, p.rotated, std::max(p.left, x + 1), std::max(p.top, y + 1),
        std::min(p.right, x + size - 1), std::min(p.bottom, y + size - 1)
    };
    if (clipped.left >= clipped.right || clipped.top >= clipped.bottom) return;
    clipped.rect(x, y, size, size, panel);
    constexpr float arena = game_detail::boundary;
    const int midX = x + size / 2, midY = y + size / 2;
    const float scale = laptop ? (size - 6) / (2 * arena) : size / 44.0f;
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
    outline(0, 0, arena, arena, 1, 0, muted);
    for (const auto& box : assets.obstacles) {
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
    if (snap.radarContact && snap.rival.visible) drawContact(snap.rival, alert);
    if (snap.freeForAll && snap.secondRadarContact && snap.secondRival.visible) drawContact(snap.secondRival, gold);
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

void startScreen(const Painter& p, const MenuState& menu) {
    p.shadeTitle();
    p.text(startX, 55, "SHIP BOX / OFFLINE DUEL", gold);
    p.text(startX, 96, "FALL OF DUTY", paper, 4);
    p.text(startX - 1, 134, "LITE", paper, 8);
    p.text(startX, 213, "FIRST TO TEN KILLS", muted);

    const bool playFocused = menu.selection == PlayRow;
    p.rect(startX, startY, startWidth, startHeight, gold);
    if (playFocused) p.rect(startX - 7, startY, 3, startHeight, paper);
    p.text(startX + 18, startY + 20, menu.opponent == Opponent::FreeForAll ? "START FFA" : "START DUEL", ink, 3);
    p.text(startX + startWidth - 36, startY + 20, ">", ink, 3);

    p.rect(startX, setupY, startWidth, setupHeight, panel);
    p.rect(startX, setupY + setupHeight - 1, startWidth, 1, playFocused ? lineColor : gold);
    if (!playFocused) p.rect(startX - 7, setupY, 3, setupHeight, gold);
    p.text(startX + 18, setupY + 17, "MATCH SETUP", paper);
    p.text(startX + startWidth - 30, setupY + 17, ">", playFocused ? muted : gold);

    char matchup[40];
    std::snprintf(matchup, sizeof(matchup), "%s %d  /  %s", difficultyName(menu.difficulty),
                  menu.difficulty, opponentName(menu.opponent));
    p.text(startX, 417, matchup, muted);
}

void menus(const Painter& p, const Snapshot& s, const MenuState& menu) {
    if (s.match == Match::Start && !menu.setup) {
        startScreen(p, menu);
        return;
    }
    const char* title = s.match == Match::Start ? "MATCH SETUP" : s.match == Match::Paused ? "PAUSED" :
                        s.playerScore >= 10 ? "VICTORY" : s.leadingRivalScore() >= 10 ? "DEFEAT" : "MATCH ENDED";
    p.rect(menuX - 8, 46, menuWidth + 16, 387, ink);
    p.rect(menuX - 8, 46, menuWidth + 16, 4, accent);
    p.text(menuX + 10, 66, title, paper, 5);
    if (s.freeForAll && s.match != Match::Start) {
        freeForAllScore(p, s, menu, menuX + 12, 106, 420);
    } else if (s.match == Match::Results) {
        char score[64];
        std::snprintf(score, sizeof(score), "%s %02d / %02d %s", menu.callsign.data(), s.playerScore, s.rivalScore,
                      s.alternateRival ? "HANZ" : "IPWNDURMOM");
        p.text(menuX + 12, 118, score, muted);
    } else {
        p.text(menuX + 12, 118, "SHIP BOX / FIRST TO TEN", muted);
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
        p.text(menuX + 207, 385, "DPAD TO EDIT / X TO SAVE", muted, 1);
        return;
    }
    const int count = s.match == Match::Start ? MenuRows - 1 : MenuRows;
    const char* primary = s.match == Match::Start ? "BACK TO START" : s.match == Match::Paused ? "RESUME" : "REMATCH";
    const char* labels[MenuRows]{primary, "DIFFICULTY", "RIVAL", "RIVAL VOICE", "VOLUME", "RENDER SIZE", "CALLSIGN",
                                 s.match == Match::Paused ? "END MATCH" : "MAIN MENU"};
    char difficulty[26], volume[12];
    std::snprintf(difficulty, sizeof(difficulty), "%s %d", difficultyName(menu.difficulty), menu.difficulty);
    std::snprintf(volume, sizeof(volume), "%d%%", menu.volume);
    const char* values[MenuRows]{"", difficulty, opponentName(menu.opponent), menu.voice ? "ON" : "OFF",
                                volume, menu.compact ? "320 X 180" : "400 X 225", menu.callsign.data(), ""};
    for (int row = 0; row < count; ++row) {
        const int py = menuY + row * menuRowHeight;
        const bool active = row == menu.selection;
        p.rect(menuX, py, menuWidth, menuRowHeight - 2, active ? lineColor : panel);
        if (active) p.rect(menuX, py, 3, menuRowHeight - 2, accent);
        p.text(menuX + 12, py + 9, labels[row], (row == PlayRow) ? accent : paper);
        const auto text = std::string_view(values[row]);
        p.text(menuX + menuWidth - 12 - int(text.size()) * 12, py + 9, text, active ? paper : muted);
    }
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

} // namespace

void drawHud(const Assets& assets, const Snapshot& s, const MenuState& menu, uint16_t* framebuffer, bool rotated, Speaker speaker) {
    const Painter p{framebuffer, rotated};
    p.rect(0, 0, 800, 15, ink);
    p.rect(0, 465, 800, 15, ink);
    const bool speaking = menu.voice && menu.volume > 0 && speaker != Speaker::None;
    if (s.match != Match::Playing) {
        menus(p, s, menu);
        if (s.match == Match::Results) {
            if (speaking) comms(p, speaker, 138);
            if (s.freeForAll && s.killNotice.remaining > 0) drawKillNotice(p, s.killNotice, menu, 412);
        }
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
