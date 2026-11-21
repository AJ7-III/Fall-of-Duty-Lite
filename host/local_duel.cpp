#include "assets.hpp"
#include "audio.hpp"
#include "desktop_controls.hpp"
#include "campaign_store.hpp"
#include "hud_font.hpp"
#include "render.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace fod;
using namespace fod::desktop;
using Clock = std::chrono::steady_clock;

class DuelPads {
public:
    void discover() {
        for (int index = 0; index < SDL_NumJoysticks(); ++index) open(index);
    }

    bool event(const SDL_Event& e, bool focused) {
        switch (e.type) {
        case SDL_CONTROLLERDEVICEADDED:
            open(e.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED: {
            const auto id = e.cdevice.which;
            const auto pad = find(id);
            if (pad != pads.end()) pads.erase(pad);
            for (auto& seat : seats) {
                if (seat != id) continue;
                seat = -1;
                lost = true;
                return true;
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP: {
            const auto pad = find(e.cbutton.which);
            if (pad == pads.end()) break;
            const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
            if (std::find(seats.begin(), seats.end(), pad->id) != seats.end()) {
                transition(pad->held, pad->pressed, button(e.cbutton.button), down);
                if (!focused) pad->pressed = 0;
            } else if (focused && down && e.cbutton.button == SDL_CONTROLLER_BUTTON_START) {
                const auto vacant = std::find(seats.begin(), seats.end(), -1);
                if (vacant == seats.end()) break;
                *vacant = pad->id;
                pad->held = pad->pressed = 0;
                return true;
            }
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            const auto pad = find(e.caxis.which);
            if (pad == pads.end()) break;
            const bool down = e.caxis.value > triggerThreshold;
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) transition(pad->held, pad->pressed, Aim, down);
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) transition(pad->held, pad->pressed, Fire, down);
            break;
        }
        default: break;
        }
        return false;
    }

    bool joined(int seat) const { return seats[seat] >= 0; }
    bool ready() const { return joined(0) && joined(1); }
    int instance(int seat) const { return seats[seat]; }

    const char* confirmLabel(int seat) const {
        return menuLabels(controls(seat)).confirm;
    }

    MenuControls controls(int seat) const {
        const auto pad = find(seats[seat]);
        return pad == pads.end() ? MenuControls::Gamepad : pad->controls;
    }

    std::array<Input, 2> sample(bool focused, std::optional<float> stickSensitivity) {
        std::array<Input, 2> inputs{};
        const bool lostSeat = std::exchange(lost, false);
        for (int seat = 0; seat < 2; ++seat) {
            auto& input = inputs[seat];
            const auto pad = find(seats[seat]);
            input.disconnected = lostSeat || !focused || pad == pads.end();
            if (pad == pads.end()) continue;
            samplePad(pad->controller.get(), pad->controls, stickSensitivity, input, pad->held, pad->pressed);
            input.held = pad->held;
            input.pressed = std::exchange(pad->pressed, 0u);
        }
        return inputs;
    }

    void clearPressed() {
        for (auto& pad : pads) pad.pressed = 0;
    }

    void restoreButtons() {
        for (auto& pad : pads) {
            pad.held &= Aim | Fire;
            pad.pressed = 0;
            for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
                const uint32_t mask = button(index);
                if (mask && SDL_GameControllerGetButton(pad.controller.get(), SDL_GameControllerButton(index))) {
                    pad.held |= mask;
                }
            }
        }
    }

private:
    struct Pad {
        std::unique_ptr<SDL_GameController, decltype(&SDL_GameControllerClose)> controller;
        SDL_JoystickID id;
        MenuControls controls;
        uint32_t held = 0, pressed = 0;
    };

    std::vector<Pad> pads;
    std::array<SDL_JoystickID, 2> seats{-1, -1};
    bool lost = false;

    template <typename Pads>
    static auto findPad(SDL_JoystickID id, Pads& collection) {
        return std::find_if(collection.begin(), collection.end(), [id](const Pad& pad) { return pad.id == id; });
    }

    std::vector<Pad>::iterator find(SDL_JoystickID id) { return findPad(id, pads); }
    std::vector<Pad>::const_iterator find(SDL_JoystickID id) const { return findPad(id, pads); }

    void open(int index) {
        const auto id = SDL_JoystickGetDeviceInstanceID(index);
        if (id < 0 || find(id) != pads.end() || !SDL_IsGameController(index)) return;
        if (auto* controller = SDL_GameControllerOpen(index)) {
            pads.push_back({{controller, SDL_GameControllerClose}, id, controllerControls(controller)});
        }
    }
};

struct DuelOverlay {
    SDL_Renderer* renderer;
    std::vector<SDL_Rect> pixels;
    static constexpr SDL_Color ink{12, 17, 18, 255}, panel{23, 31, 31, 255}, paper{235, 233, 213, 255};
    static constexpr SDL_Color muted{170, 184, 171, 255}, alert{235, 101, 79, 255};
    static constexpr std::array<SDL_Color, 2> accents{{{132, 211, 185, 255}, {241, 193, 108, 255}}};

    void color(SDL_Color value) const { SDL_SetRenderDrawColor(renderer, value.r, value.g, value.b, value.a); }

    void fill(SDL_Rect rect, SDL_Color value) const {
        if (rect.w <= 0 || rect.h <= 0) return;
        color(value);
        SDL_RenderFillRect(renderer, &rect);
    }

    void line(int x, int y, int toX, int toY, SDL_Color value) const {
        color(value);
        SDL_RenderDrawLine(renderer, x, y, toX, toY);
    }

    static int textWidth(std::string_view value, int scale) { return (int(value.size()) * 6 - 1) * scale; }

    void text(int x, int y, std::string_view value, int scale, SDL_Color tint) {
        pixels.clear();
        for (char c : value) {
            const auto* glyph = hud::glyph(c);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (glyph[row] & (16 >> col)) pixels.push_back({x + col * scale, y + row * scale, scale, scale});
                }
            }
            x += 6 * scale;
        }
        if (pixels.empty()) return;
        color(tint);
        SDL_RenderFillRects(renderer, pixels.data(), int(pixels.size()));
    }

    void centered(int x, int y, std::string_view value, int scale, SDL_Color tint) {
        text(x - textWidth(value, scale) / 2, y, value, scale, tint);
    }

    void viewHud(SDL_Rect rect, const Snapshot& snapshot, int seat) {
        const int scale = std::clamp(rect.w / 240, 1, 4);
        const int pad = 6 * scale, cx = rect.x + rect.w / 2, cy = rect.y + rect.h / 2;
        const auto accent = accents[seat];

        const bool scoped = snapshot.weapon == Weapon::Sniper && snapshot.scope >= .6f;
        if (!snapshot.dead && scoped) {
            const int radius = rect.h * 2 / 5;
            fill({rect.x, rect.y, rect.w, cy - radius - rect.y}, ink);
            fill({rect.x, cy + radius + 1, rect.w, rect.y + rect.h - (cy + radius + 1)}, ink);
            pixels.clear();
            for (int y = -radius; y <= radius; ++y) {
                const int dx = int(std::sqrt(float(radius * radius - y * y)));
                pixels.push_back({rect.x, cy + y, cx - dx - rect.x, 1});
                pixels.push_back({cx + dx + 1, cy + y, rect.x + rect.w - (cx + dx + 1), 1});
            }
            color(ink);
            SDL_RenderFillRects(renderer, pixels.data(), int(pixels.size()));
            line(cx - radius, cy, cx + radius, cy, ink);
            line(cx, cy - radius, cx, cy + radius, ink);
            const int graduation = std::max(1, radius / 9);
            for (int i = -4; i <= 4; ++i) {
                line(cx + i * graduation, cy - scale, cx + i * graduation, cy + scale, paper);
                line(cx - scale, cy + i * graduation, cx + scale, cy + i * graduation, paper);
            }
        } else if (!snapshot.dead && snapshot.ads < .83f) {
            const int gap = std::max(3, rect.h / 32), length = std::max(2, gap / 2);
            line(cx - gap - length, cy, cx - gap, cy, paper);
            line(cx + gap, cy, cx + gap + length, cy, paper);
            line(cx, cy - gap - length, cx, cy - gap, paper);
            line(cx, cy + gap, cx, cy + gap + length, paper);
        }
        if (!snapshot.dead && snapshot.hitMarker > 0.f) {
            const auto tint = snapshot.lethalMarker ? alert : paper;
            for (int x : {-1, 1}) for (int y : {-1, 1})
                line(cx + x * 5 * scale, cy + y * 5 * scale, cx + x * 10 * scale, cy + y * 10 * scale, tint);
        }
        if (!snapshot.dead && !scoped && snapshot.muzzle > 0.f && snapshot.ads < .9f) {
            const int x = cx + int(rect.w * (.0425f - .0325f * snapshot.ads));
            const int y = cy + int(rect.h * (.053f - .027f * snapshot.ads));
            const int span = 2 * scale;
            line(x - span, y, x + span, y, accents[1]);
            line(x, y - span, x, y + span, accents[1]);
            fill({x - scale, y - scale, 2 * scale + 1, 2 * scale + 1}, paper);
        }
        if (snapshot.damage > .12f) {
            const int edge = snapshot.damage > .6f ? 3 * scale : scale;
            fill({rect.x, rect.y, edge, rect.h}, alert);
            fill({rect.x + rect.w - edge, rect.y, edge, rect.h}, alert);
        }
        fill({rect.x, rect.y, rect.w, 2 * scale}, accent);
        fill({rect.x + pad, rect.y + pad, 83 * scale, 19 * scale}, ink);
        text(rect.x + pad + 5 * scale, rect.y + pad + 6 * scale, seat == 0 ? "PLAYER 1" : "PLAYER 2", scale, accent);
        if (snapshot.dead) {
            fill({cx - 44 * scale, rect.y + rect.h - 20 * scale - pad, 88 * scale, 20 * scale}, ink);
            centered(cx, rect.y + rect.h - 14 * scale - pad, "RESPAWNING", scale, paper);
            return;
        }
        char ammo[18];
        std::snprintf(ammo, sizeof(ammo), "%02d / %02d", snapshot.clip, snapshot.reserve);
        const char* weapon = snapshot.weapon == Weapon::Mp44 ? "MP44" : snapshot.weapon == Weapon::Sniper ? "M40A3" : "USP .45";
        const int left = rect.x + rect.w - pad - 68 * scale;
        const int top = rect.y + rect.h - 29 * scale - pad;
        fill({left, top, 68 * scale, 29 * scale}, ink);
        text(left + 5 * scale, top + 4 * scale, weapon, scale, muted);
        text(left + 5 * scale, top + 16 * scale, ammo, scale, snapshot.clip ? paper : alert);
    }

    void score(int width, int top, int scale, int first, int second) {
        char value[16];
        std::snprintf(value, sizeof(value), "%02d  -  %02d", first, second);
        const int center = width / 2;
        text(center - 75 * scale, top + scale, "P1", scale, accents[0]);
        centered(center, top, value, scale + 1, paper);
        text(center + 64 * scale, top + scale, "P2", scale, accents[1]);
    }

    void menu(int width, int height, bool started, const Snapshot& state, const DuelPads& pads, int second, Arena arena, bool saveFailed) {
        const auto* mission = state.mission.get();
        const auto match = state.match;
        const int first = state.playerScore;
        const int scale = std::clamp(std::min(width / 350, height / 110), 1, 5);
        const int center = width / 2;
        const int boxWidth = std::min(width - 12 * scale, (mission ? 205 : 173) * scale);
        const int boxHeight = (mission ? 113 : 99) * scale;
        const int boxY = (height - boxHeight) / 2;
        const SDL_Rect box{center - boxWidth / 2, boxY, boxWidth, boxHeight};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fill({0, 0, width, height}, {6, 9, 10, 155});
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        fill(box, panel);
        fill({box.x, box.y, box.w / 2, 2 * scale}, accents[0]);
        fill({box.x + box.w / 2, box.y, box.w - box.w / 2, 2 * scale}, accents[1]);
        const int y = box.y + 11 * scale;
        char location[36];
        if (mission) {
            const bool won = mission->phase == MissionPhase::Victory;
            const char* title = !started ? "OPERATION BRIAR CO-OP" : match == Match::Paused ? "PAUSED" :
                mission->nextMissionReady() ? "MISSION COMPLETE" : won ? "CAMPAIGN COMPLETE" : "MISSION FAILED";
            const int titleScale = std::clamp((box.w - 12 * scale) / textWidth(title, 1), 1, scale + 1);
            centered(center, y, title, titleScale, match == Match::Results && !won ? alert : paper);
            std::snprintf(location, sizeof(location), "MISSION %d / %s", int(mission->chapter) + 1, missionName(mission->chapter));
            centered(center, y + 16 * scale, location, scale, muted);
            if (match == Match::Results) {
                char summary[52];
                const int seconds = int(mission->missionSeconds);
                if (saveFailed) std::snprintf(summary, sizeof(summary), "COULD NOT SAVE CAMPAIGN TO STORAGE");
                else if (!won) std::snprintf(summary, sizeof(summary), "%s", mission->failReason);
                else std::snprintf(summary, sizeof(summary), "%02d:%02d / FAMILY SAVED %d/%d",
                                   seconds / 60, seconds % 60, mission->civiliansSaved, mission->civiliansTotal);
                if (summary[0]) {
                    const int size = std::clamp((box.w - 12 * scale) / textWidth(summary, 1), 1, scale);
                    centered(center, y + 28 * scale, summary, size, won ? paper : alert);
                }
            } else if (!started) {
                centered(center, y + 28 * scale, "P1 LEADS / P2 PROVIDES COVER", scale, muted);
            }
        } else if (!started) {
            centered(center, y, "VERSUS DUEL", scale + 1, paper);
            std::snprintf(location, sizeof(location), "%s / FIRST TO 10", arenaLabel(arena));
            centered(center, y + 16 * scale, location, scale, muted);
        } else if (match == Match::Results) {
            centered(center, y, first == second ? "DRAW" : first > second ? "PLAYER 1 WINS" : "PLAYER 2 WINS",
                     scale + 1, first == second ? paper : accents[first > second ? 0 : 1]);
            std::snprintf(location, sizeof(location), "%s / FINAL SCORE", arenaLabel(arena));
            centered(center, y + 16 * scale, location, scale, muted);
        } else {
            centered(center, y, "PAUSED", scale + 1, paper);
            centered(center, y + 16 * scale, arenaLabel(arena), scale, muted);
        }
        const int readyY = y + (mission ? 43 : 34) * scale;
        centered(center - 35 * scale, readyY, pads.joined(0) ? "P1 READY" : "P1 OPEN", scale, accents[0]);
        centered(center + 35 * scale, readyY, pads.joined(1) ? "P2 READY" : "P2 OPEN", scale, accents[1]);
        char hint[62];
        const char* firstConfirm = pads.confirmLabel(0);
        if (!pads.ready()) std::snprintf(hint, sizeof(hint), "PRESS START/+ TO JOIN");
        else if (!started) std::snprintf(hint, sizeof(hint), "P1 / %s TO START", firstConfirm);
        else if (match == Match::Results) {
            const char* confirm = std::string_view(firstConfirm) == pads.confirmLabel(1) ? firstConfirm : "BOTTOM BUTTON";
            if (!mission) std::snprintf(hint, sizeof(hint), "%s FOR REMATCH", confirm);
            else {
                const char* action = "PLAY AGAIN";
                if (mission->nextMissionReady())
                    action = mission->chapter == MissionChapter::BriarHouse ? "FLY TO TIMBERLINE" : "RETURN TO BRIAR HOUSE";
                else if (mission->phase == MissionPhase::Failed && mission->checkpointStage != MissionCheckpoint::None)
                    action = "RETRY CHECKPOINT";
                std::snprintf(hint, sizeof(hint), "%s / %s", confirm, action);
            }
        } else {
            std::snprintf(hint, sizeof(hint), "PRESS START/+ TO RESUME");
        }
        const int hintScale = std::clamp((box.w - 12 * scale) / textWidth(hint, 1), 1, scale);
        centered(center, readyY + 18 * scale, hint, hintScale, paper);
        if (pads.joined(0)) std::snprintf(hint, sizeof(hint), "ESC OR P1 %s / BACK", menuLabels(pads.controls(0)).back);
        else std::snprintf(hint, sizeof(hint), "ESC / BACK");
        const int size = std::clamp((box.w - 12 * scale) / textWidth(hint, 1), 1, scale);
        centered(center, readyY + 33 * scale, hint, size, muted);
        if (started && match == Match::Paused)
            centered(center, readyY + 45 * scale, "DOWN / SETTINGS", scale, paper);
    }
};

SDL_Rect fitView(SDL_Rect cell, int worldWidth, int worldHeight) {
    const int w = std::min(cell.w, cell.h * worldWidth / worldHeight);
    const int h = std::min(cell.h, cell.w * worldHeight / worldWidth);
    return {cell.x + (cell.w - w) / 2, cell.y + (cell.h - h) / 2, w, h};
}

class SplitScreenDesktop {
public:
    SplitScreenDesktop(const Assets& assets, RenderLook look, int sourceWidth, uint32_t seed,
                       std::optional<float> stickSensitivity = std::nullopt,
                       std::optional<MissionChapter> chapter = std::nullopt, const MenuState& settings = {})
        : app(assets), game(app.game), mixer(app.audio), assets(assets),
          worldRenderer(assets, look, size_t(splitRenderWidths.back()) * (splitRenderWidths.back() * 9 / 16)),
          pixels(size_t(sourceWidth) * (sourceWidth * 9 / 16)), width(sourceWidth), height(sourceWidth * 9 / 16),
          smooth(look == RenderLook::Enhanced), nextSeed(seed), stickSensitivity(stickSensitivity), chapter(chapter) {
        app.settings() = settings;
        app.settings().screen = MenuScreen::Home;
        app.settings().campaign = bool(chapter);
        app.settings().splitRenderWidth = width;
        mixer.setVolume(settings.volume);
        if (chapter) {
            campaignCanvas.resize(800 * 480);
            prepareCampaign();
        } else {
            game.startLocalDuel(nextSeed++);
        }
    }

    ~SplitScreenDesktop() {
        if (audio) SDL_CloseAudioDevice(audio);
        for (auto* texture : textures) if (texture) SDL_DestroyTexture(texture);
        if (settingsTexture) SDL_DestroyTexture(settingsTexture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
    }

    bool open() {
        const char* title = chapter ? "Fall of Duty Lite | Operation Briar Co-op" :
            assets.arena == Arena::BriarHouse ? "Fall of Duty Lite | Briar House Versus Duel" : "Fall of Duty Lite | Versus Duel";
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  1200, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window) return false;
        SDL_SetWindowMinimumSize(window, 600, 360);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer || !createTextures()) return false;
        overlay.renderer = renderer;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec spec{};
            spec.freq = audioHz;
            spec.format = AUDIO_S16SYS;
            spec.channels = 2;
            spec.samples = 512;
            spec.callback = [](void* data, uint8_t* bytes, int count) {
                static_cast<Mixer*>(data)->mix(reinterpret_cast<int16_t*>(bytes), size_t(count) / (2 * sizeof(int16_t)));
            };
            spec.userdata = &mixer;
            audio = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
            if (audio) SDL_PauseAudioDevice(audio, 0);
        }
        pads.discover();
        return true;
    }

    bool pump() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN:
                if (playing() || !focused || event.key.repeat) break;
                switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE: menuKeys |= Back; break;
                case SDL_SCANCODE_RETURN: menuKeys |= Confirm; break;
                case SDL_SCANCODE_UP: menuKeys |= Up; break;
                case SDL_SCANCODE_DOWN: menuKeys |= Down; break;
                case SDL_SCANCODE_LEFT: menuKeys |= Left; break;
                case SDL_SCANCODE_RIGHT: menuKeys |= Right; break;
                default: break;
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    running = false;
                    break;
                }
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST || event.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    focused = false;
                    focusLost |= playing();
                    pads.clearPressed();
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    focused = true;
                    pads.restoreButtons();
                }
                requested = true;
                break;
            case SDL_RENDER_DEVICE_RESET:
                if (settingsTexture) SDL_DestroyTexture(settingsTexture);
                settingsTexture = nullptr;
                if (!createTextures()) return false;
                requested = true;
                break;
            case SDL_RENDER_TARGETS_RESET:
                worldDirty = requested = true;
                break;
            default:
                if (event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_B &&
                    focused && !playing() && !pads.joined(0)) menuKeys |= Back;
                if (pads.event(event, focused)) requested = true;
                break;
            }
        }
        return true;
    }

    void tick() {
        const bool interrupted = std::exchange(focusLost, false);
        auto inputs = pads.sample(focused && !interrupted, stickSensitivity);
        const bool presentPads = !inputs[0].disconnected && !inputs[1].disconnected;
        const auto before = game.state().match;
        const auto keys = std::exchange(menuKeys, 0u);
        Input navigation = inputs[0];
        navigation.pressed |= keys;
        navigation.disconnected = false;
        if (keys) app.settings().controls = MenuControls::Keyboard;
        else if (navigation.pressed && pads.joined(0)) app.settings().controls = pads.controls(0);
        if (started && before == Match::Paused && focused && !interrupted) {
            if (app.settings().screen == MenuScreen::Settings) {
                app.tick(navigation);
                requested = true;
                return;
            }
            if (navigation.edge(Down)) {
                app.settings().screen = MenuScreen::Settings;
                app.settings().selection = 0;
                requested = true;
                return;
            }
        }
        if (focused && !playing() && navigation.edge(Back)) {
            returnToTitle = true;
            running = false;
            return;
        }
        if (!started) {
            if (presentPads && inputs[0].edge(Confirm)) start();
        } else if (before == Match::Playing) {
            const auto previousTick = game.state().tick;
            if (chapter) game.stepLocalCampaign(inputs);
            else game.stepLocalDuel(inputs);
            worldDirty |= game.state().tick != previousTick;
            if (!chapter) playSounds();
            else if (game.state().match == Match::Paused) {
                mixer.pauseSpeech(true);
                mixer.stop(Sound::Rotor);
                mixer.stop(Sound::InvasionHum);
            } else {
                app.routeAudio();
                if (game.state().match == Match::Results && *chapter == MissionChapter::BriarHouse)
                    app.recordCampaignResult();
            }
        } else if (presentPads) {
            if (before == Match::Paused &&
                (inputs[0].edge(Confirm) || inputs[1].edge(Confirm) || inputs[0].edge(Pause) || inputs[1].edge(Pause))) {
                if (chapter) {
                    mixer.pauseSpeech(false);
                    game.resumeLocalCampaign(inputs);
                } else game.resumeLocalDuel();
                pads.clearPressed();
            } else if (before == Match::Results && (inputs[0].edge(Confirm) || inputs[1].edge(Confirm))) {
                if (chapter) {
                    mixer.stopSpeech();
                    game.continueLocalCampaign(nextSeed++, app.settings().difficulty);
                    app.routeAudio();
                    worldDirty = true;
                    pads.clearPressed();
                } else start();
            }
        }
        if (before != game.state().match) requested = true;
        if (!audio) mixer.mix(silence.data(), silence.size() / 2);
    }

    bool playing() const { return started && game.state().match == Match::Playing; }

    bool present() {
        static constexpr int margin = 14, gap = 12;
        int w = 0, h = 0;
        if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0) return false;
        if (w < 80 || h < 80) {
            requested = false;
            return true;
        }
        if (app.settings().screen == MenuScreen::Settings) {
            if (!settingsTexture) settingsTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                                                     SDL_TEXTUREACCESS_STREAMING, 800, 480);
            if (!settingsTexture) return false;
            settingsCanvas.assign(800 * 480, 0);
            drawHud(assets, game.state(), app.settings(), settingsCanvas.data(), false);
            const auto panel = fitView({0, 0, w, h}, 800, 480);
            overlay.color(DuelOverlay::ink);
            if (SDL_UpdateTexture(settingsTexture, nullptr, settingsCanvas.data(), 800 * sizeof(uint16_t)) != 0 ||
                SDL_RenderClear(renderer) != 0 || SDL_RenderCopy(renderer, settingsTexture, nullptr, &panel) != 0) return false;
            SDL_RenderPresent(renderer);
            requested = false;
            return true;
        }
        if (app.settings().splitRenderWidth != width) {
            width = app.settings().splitRenderWidth;
            height = width * 9 / 16;
            pixels.resize(size_t(width) * height);
            if (!createTextures()) return false;
        }
        if (worldDirty) {
            for (int seat = 0; seat < 2; ++seat) {
                const auto& view = game.localView(seat);
                worldRenderer.render(view, width, height, pixels.data(), false);
                const uint16_t* frame = pixels.data();
                int stride = width;
                if (chapter) {
                    scaleWorld(pixels.data(), width, height, campaignCanvas.data(), false, smooth);
                    auto hud = view;
                    hud.match = Match::Playing;
                    auto menu = app.settings();
                    menu.controls = pads.controls(seat);
                    drawHud(assets, hud, menu, campaignCanvas.data(), false);
                    frame = campaignCanvas.data();
                    stride = 800;
                }
                if (SDL_UpdateTexture(textures[seat], nullptr, frame, stride * int(sizeof(uint16_t))) != 0) return false;
            }
            worldDirty = false;
        }
        const int scoreScale = std::clamp(w / 520, 1, 4);
        const int top = chapter ? margin : margin + 7 * (scoreScale + 1) + gap;
        const int viewWidth = chapter ? 800 : width, viewHeight = chapter ? 480 : height;
        const int areaWidth = w - margin * 2, areaHeight = h - margin - top;
        const int sideWidth = (areaWidth - gap) / 2;
        const int stackHeight = (areaHeight - gap) / 2;
        const auto side = fitView({0, 0, sideWidth, areaHeight}, viewWidth, viewHeight);
        const auto stack = fitView({0, 0, areaWidth, stackHeight}, viewWidth, viewHeight);
        const bool stacked = stack.w * stack.h > side.w * side.h;
        const auto& first = game.localView(0);
        const auto& second = game.localView(1);
        overlay.color(DuelOverlay::ink);
        if (SDL_RenderClear(renderer) != 0) return false;
        for (int seat = 0; seat < 2; ++seat) {
            const SDL_Rect cell = stacked ? SDL_Rect{margin, top + seat * (stackHeight + gap), areaWidth, stackHeight} :
                SDL_Rect{margin + seat * (sideWidth + gap), top, sideWidth, areaHeight};
            const auto rect = fitView(cell, viewWidth, viewHeight);
            if (SDL_RenderCopy(renderer, textures[seat], nullptr, &rect) != 0) return false;
            if (started && !chapter) overlay.viewHud(rect, game.localView(seat), seat);
        }
        if (!playing()) overlay.menu(w, h, started, first, pads, second.playerScore, assets.arena, app.settings().saveFailed);
        if (!chapter) overlay.score(w, margin, scoreScale, first.playerScore, second.playerScore);
        SDL_RenderPresent(renderer);
        requested = false;
        return true;
    }

    App app;
    Game& game;
    DuelPads pads;
    bool running = true, requested = true, started = false, returnToTitle = false;

private:
    Mixer& mixer;
    const Assets& assets;
    Renderer worldRenderer;
    std::vector<uint16_t> pixels;
    std::vector<uint16_t> campaignCanvas;
    std::array<int16_t, 2 * audioHz / 60> silence{};
    int width, height;
    bool smooth;
    uint32_t nextSeed;
    std::optional<float> stickSensitivity;
    std::optional<MissionChapter> chapter;
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::array<SDL_Texture*, 2> textures{};
    SDL_Texture* settingsTexture = nullptr;
    std::vector<uint16_t> settingsCanvas;
    uint32_t menuKeys = 0;
    SDL_AudioDeviceID audio = 0;
    DuelOverlay overlay{};
    bool focused = true, focusLost = false, worldDirty = true;

    bool createTextures() {
        for (auto& texture : textures) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                        chapter ? 800 : width, chapter ? 480 : height);
            if (!texture) return false;
#if SDL_VERSION_ATLEAST(2, 0, 12)
            SDL_SetTextureScaleMode(texture, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
        }
        worldDirty = true;
        return true;
    }

    void start() {
        started = requested = worldDirty = true;
        pads.clearPressed();
        if (chapter) {
            prepareCampaign();
            app.routeAudio();
        } else game.startLocalDuel(nextSeed++);
    }

    void prepareCampaign() {
        const auto& saved = app.settings().progress;
        if (app.settings().pendingLaunch == LaunchAction::ContinueCoOp && saved.available()) {
            app.settings().difficulty = saved.difficulty;
            game.resumeCampaign(nextSeed++, saved);
        } else game.startLocalCampaign(nextSeed++, app.settings().difficulty, *chapter);
    }

    void playSounds() {
        for (size_t i = 0; i < game.eventCount(); ++i) {
            const auto& event = game.events()[i];
            if (event.kind != EventKind::Sound || (!app.settings().voice && isVoiceSound(event.sound))) continue;
            float gain = event.volume;
            if (event.positional) {
                const float first = length(event.position - game.localView(0).camera.position);
                const float second = length(event.position - game.localView(1).camera.position);
                gain /= 1.f + std::min(first, second) * .12f;
            }
            mixer.play(event.sound, gain);
        }
    }
};

class SdlSession {
public:
    ~SdlSession() { SDL_Quit(); }
};

class SplitScreenLoop {
public:
    static constexpr auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(tickSeconds));
    
    Clock::time_point deadline() const { return nextTick; }

    bool frame(SplitScreenDesktop& duel, Clock::time_point now) {
        bool advanced = false;
        if (duel.playing()) {
            nextTick = std::max(nextTick, now - 3 * period);
            while (duel.playing() && now >= nextTick) {
                duel.tick();
                nextTick += period;
                advanced = true;
            }
        } else {
            duel.tick();
            nextTick = now + period;
        }
        return (!advanced && !duel.requested) || duel.present();
    }

private:
    Clock::time_point nextTick = Clock::now();
};

std::optional<bool> runSplitScreen(const Assets& assets, RenderLook look, int& width, std::optional<float> stickSensitivity,
                                   std::optional<MissionChapter> chapter, MenuState& settings) {
    SdlSession session;
    SDL_SetMainReady();
    configureControllers();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL split-screen: %s\n", SDL_GetError());
        return std::nullopt;
    }
    SplitScreenDesktop duel(assets, look, width, std::random_device{}(), stickSensitivity, chapter, settings);
    duel.app.setProgressWriter(fod::desktop::writeCampaign);
    if (!duel.open()) {
        std::fprintf(stderr, "SDL split-screen display: %s\n", SDL_GetError());
        return std::nullopt;
    }

    SplitScreenLoop loop;
    while (duel.running) {
        if (!duel.pump()) break;
        if (!duel.running) break;
        if (!loop.frame(duel, Clock::now())) break;
        if (!duel.running) break;
        if (duel.playing()) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(loop.deadline() - Clock::now());
            if (remaining.count() > 0) SDL_WaitEventTimeout(nullptr, int(remaining.count()));
        } else {
            if (!SDL_WaitEvent(nullptr)) break;
        }
    }
    if (!duel.running) {
        width = duel.app.settings().splitRenderWidth;
        settings.voice = duel.app.settings().voice;
        settings.volume = duel.app.settings().volume;
        settings.callsign = duel.app.settings().callsign;
        settings.progress = duel.app.settings().progress;
        settings.saveFailed = duel.app.settings().saveFailed;
        return duel.returnToTitle;
    }
    std::fprintf(stderr, "SDL split-screen frame: %s\n", SDL_GetError());
    return std::nullopt;
}

} // namespace

std::optional<bool> runLocalSession(const fod::Assets& assets, fod::RenderLook look, int& width,
                                    std::optional<float> stickSensitivity, std::optional<fod::MissionChapter> chapter,
                                    fod::MenuState& settings) {
    return runSplitScreen(assets, look, width, stickSensitivity, chapter, settings);
}

#ifdef FOD_DUEL_DESKTOP_TEST
#if SDL_VERSION_ATLEAST(2, 0, 14)
void require(bool condition, const char* expression, int line);
#define CHECK(condition) require(bool(condition), #condition, __LINE__)

namespace {
class VirtualPad {
public:
    VirtualPad() {
        const int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                                    SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        CHECK(index >= 0);
        handle = SDL_JoystickOpen(index);
        CHECK(handle);
        id = SDL_JoystickInstanceID(handle);
    }

    ~VirtualPad() {
        disconnect();
        SDL_JoystickClose(handle);
    }

    void set(SDL_GameControllerButton button, bool down) {
        CHECK(SDL_JoystickSetVirtualButton(handle, button, down) == 0);
        SDL_JoystickUpdate();
    }

    void set(SDL_GameControllerAxis axis, int16_t value) {
        CHECK(SDL_JoystickSetVirtualAxis(handle, axis, value) == 0);
        SDL_JoystickUpdate();
    }

    void disconnect() const {
        if (!SDL_JoystickGetAttached(handle)) return;
        for (int index = 0; index < SDL_NumJoysticks(); ++index) {
            if (SDL_JoystickGetDeviceInstanceID(index) != id) continue;
            CHECK(SDL_JoystickDetachVirtual(index) == 0);
            return;
        }
        CHECK(false);
    }

    SDL_JoystickID id;

private:
    SDL_Joystick* handle = nullptr;
};
} // namespace

void testLocalDuelDesktop(const fod::Assets& assets, bool live, int width) {
    if (!live) {
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
        SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SdlSession session;
    SDL_SetMainReady();
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "1", SDL_HINT_OVERRIDE);
#endif
    configureControllers();
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
    CHECK(!SDL_GetHintBoolean(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, SDL_TRUE));
#endif
    const auto nintendo = menuLabels(controllerControls(SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO));
    CHECK(std::string_view(nintendo.confirm) == "B" && std::string_view(nintendo.back) == "A");
    CHECK(button(SDL_CONTROLLER_BUTTON_A) == (Confirm | Jump));
    CHECK(button(SDL_CONTROLLER_BUTTON_B) == (Back | Stance));
    CHECK(button(SDL_CONTROLLER_BUTTON_X) == Reload);
    CHECK(button(SDL_CONTROLLER_BUTTON_Y) == Swap);
    CHECK(wantsRelativeMouse(Match::Playing, MenuControls::Keyboard, true));
    CHECK(!wantsRelativeMouse(Match::Playing, MenuControls::Nintendo, true));
    CHECK(!wantsRelativeMouse(Match::Paused, MenuControls::Keyboard, true));
    CHECK(!wantsRelativeMouse(Match::Playing, MenuControls::Keyboard, false));
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) == 0);
    // An unfilled lobby must allow Escape, or the first player's Back button.
    if (!live) {
        MenuState settings;
        settings.screen = MenuScreen::VersusSetup;
        int renderWidth = width;
        const auto escape = [](Uint32, void*) -> Uint32 {
            SDL_Event event{};
            event.type = SDL_KEYDOWN;
            event.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
            SDL_PushEvent(&event);
            return 0;
        };
        CHECK(SDL_AddTimer(100, escape, nullptr) != 0);
        const auto back = runSplitScreen(assets, RenderLook::Enhanced, renderWidth, {}, {}, settings);
        CHECK(back && *back && settings.screen == MenuScreen::VersusSetup);
        CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) == 0);
    }
    VirtualPad first, second;
    if (!live) {
        SplitScreenDesktop waiting(assets, RenderLook::Enhanced, width, 9);
        CHECK(waiting.open());
        const auto press = [&](SDL_GameControllerButton button) {
            SDL_Event event{};
            event.type = SDL_CONTROLLERBUTTONDOWN;
            event.cbutton.which = first.id;
            event.cbutton.button = Uint8(button);
            CHECK(SDL_PushEvent(&event) == 1 && waiting.pump());
            waiting.tick();
        };
        press(SDL_CONTROLLER_BUTTON_START);
        CHECK(waiting.pads.joined(0) && !waiting.pads.joined(1));
        press(SDL_CONTROLLER_BUTTON_B);
        CHECK(waiting.returnToTitle && !waiting.running && !waiting.started);
    }
    SplitScreenDesktop duel(assets, RenderLook::Enhanced, width, 8);
    CHECK(duel.open());
    CHECK(duel.present());
    CHECK(duel.pump());
    const auto tick = [&duel] { CHECK(duel.pump()); duel.tick(); };
    const auto tap = [&duel](VirtualPad& pad, SDL_GameControllerButton button) {
        pad.set(button, true);
        CHECK(SDL_WaitEventTimeout(nullptr, 100) == 1);
        CHECK(duel.pump());
        pad.set(button, false);
        CHECK(duel.pump());
    };
    tick();
    tap(first, SDL_CONTROLLER_BUTTON_START);
    tap(second, SDL_CONTROLLER_BUTTON_START);
    CHECK(duel.pads.instance(0) == first.id && duel.pads.instance(1) == second.id);
    CHECK(!duel.started);
    tap(first, SDL_CONTROLLER_BUTTON_A);
    tick();
    CHECK(duel.playing());
    const auto firstPosition = duel.game.localView(0).player.position;
    const auto secondYaw = duel.game.localView(1).playerYaw;
    first.set(SDL_CONTROLLER_AXIS_LEFTY, -26000);
    second.set(SDL_CONTROLLER_AXIS_RIGHTX, 22000);
    auto* controller = SDL_GameControllerFromInstanceID(second.id);
    CHECK(controller);
    uint32_t held = 0, pressed = 0;
    Input standard, nintendoAim, customized;
    samplePad(controller, MenuControls::Gamepad, std::nullopt, standard, held, pressed);
    samplePad(controller, MenuControls::Nintendo, std::nullopt, nintendoAim, held, pressed);
    samplePad(controller, MenuControls::Nintendo, 0.6f, customized, held, pressed);
    CHECK(standard.lookX > 0);
    CHECK(std::abs(nintendoAim.lookX - standard.lookX * 0.8f) < 0.000001f);
    CHECK(std::abs(customized.lookX - standard.lookX * 0.6f) < 0.000001f);
    for (int i = 0; i < 8; ++i) tick();
    first.set(SDL_CONTROLLER_AXIS_LEFTY, 0);
    second.set(SDL_CONTROLLER_AXIS_RIGHTX, 0);
    CHECK(length(duel.game.localView(0).player.position - firstPosition) > .1f);
    CHECK(duel.game.localView(1).playerYaw > secondYaw + .05f);
    first.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    CHECK(duel.pump());
    first.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    tick();
    CHECK(duel.game.localView(0).clip == 29 && duel.game.localView(1).clip == 30);
    CHECK(duel.present());

    first.disconnect();
    tick();
    CHECK(duel.game.state().match == Match::Paused);
    CHECK(duel.pads.instance(1) == second.id);
    VirtualPad replacement;
    CHECK(duel.pump());
    tap(replacement, SDL_CONTROLLER_BUTTON_START);
    CHECK(duel.pads.instance(0) == replacement.id && duel.pads.instance(1) == second.id);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    tap(second, SDL_CONTROLLER_BUTTON_A);
    tick();
    CHECK(duel.playing());
    for (int i = 0; i < 20; ++i) tick();
    CHECK(duel.game.localView(0).clip == 29);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    tick();
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    tick();
    CHECK(duel.game.localView(0).clip == 28);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    tick();

    // SDL may suppress the release of a button held while focus moves to another app.
    replacement.set(SDL_CONTROLLER_BUTTON_B, true);
    tick();
    SDL_Event focus{};
    focus.type = SDL_WINDOWEVENT;
    focus.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    CHECK(SDL_PushEvent(&focus) == 1);
    tick();
    CHECK(duel.game.state().match == Match::Paused);
    replacement.set(SDL_CONTROLLER_BUTTON_B, false);
    SDL_FlushEvent(SDL_CONTROLLERBUTTONUP);
    focus.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    CHECK(SDL_PushEvent(&focus) == 1);
    tick();
    CHECK(duel.game.state().match == Match::Paused);
    tap(second, SDL_CONTROLLER_BUTTON_START);
    tick();
    CHECK(duel.playing());
    for (int i = 0; i < 30; ++i) tick();
    CHECK(!duel.game.localView(0).prone && !duel.game.localView(0).crouch);

    // Use a known clear lane in the real Ship Box map to test match point and the menu flow.
    std::array<Fixture, 2> fixtures{};
    fixtures[0].player = {-14.3f, 0, -12.5f};
    fixtures[1].player = {-14.3f, 0, -7.5f};
    fixtures[0].yaw = 0;
    fixtures[1].yaw = pi;
    fixtures[0].weapon = Weapon::Sniper;
    fixtures[0].playerScore = 9;
    duel.game.setupLocalDuelFixture(fixtures);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    for (int i = 0; i < 45; ++i) tick();
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    tick();
    CHECK(duel.game.state().match == Match::Results && duel.game.localView(0).playerScore == 10);
    CHECK(duel.present());
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
    tap(second, SDL_CONTROLLER_BUTTON_A);
    tick();
    CHECK(duel.playing());
    CHECK(duel.game.localView(0).playerScore == 0 && duel.game.localView(1).playerScore == 0);

    SplitScreenLoop loop;
    const auto initialTick = duel.game.state().tick;
    CHECK(loop.frame(duel, loop.deadline() + std::chrono::seconds(1)));
    CHECK(duel.game.state().tick == initialTick + 4);
    CHECK(loop.frame(duel, loop.deadline() - SplitScreenLoop::period / 2));
    CHECK(duel.game.state().tick == initialTick + 4);

    if (live) {
        std::array<Fixture, 2> views{};
        views[1].player = {13.5f, 0, 13.5f};
        views[1].yaw = -3 * pi / 4;
        views[1].weapon = Weapon::Sniper;
        duel.game.setupLocalDuelFixture(views);
        std::vector<double> frameMs;
        SplitScreenLoop liveLoop;
        Clock::time_point firstSample{}, lastSample{};
        int missed = 0;
        for (int frame = 0; frame < 326; ++frame) {
            const auto next = liveLoop.deadline();
            while (Clock::now() < next) SDL_Delay(1);
            const auto start = Clock::now();
            replacement.set(SDL_CONTROLLER_AXIS_RIGHTX, frame % 160 < 80 ? 11500 : -11500);
            second.set(SDL_CONTROLLER_AXIS_RIGHTX, frame % 120 < 60 ? -13500 : 13500);
            replacement.set(SDL_CONTROLLER_AXIS_LEFTY, frame % 130 < 65 ? -13000 : 10000);
            replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, frame % 90 < 22 ? 32767 : -32768);
            second.set(SDL_CONTROLLER_AXIS_TRIGGERLEFT, frame % 120 < 75 ? 32767 : -32768);
            CHECK(duel.pump());
            CHECK(liveLoop.frame(duel, Clock::now()));
            CHECK(duel.playing());
            if (frame >= 26) {
                const auto finish = Clock::now();
                if (frame == 26) firstSample = start;
                lastSample = start;
                frameMs.push_back(std::chrono::duration<double, std::milli>(finish - start).count());
                if (finish > next + SplitScreenLoop::period) ++missed;
            }
        }
        std::sort(frameMs.begin(), frameMs.end());
        const double fps = double(frameMs.size() - 1) / std::chrono::duration<double>(lastSample - firstSample).count();
        std::printf("%dx%d per view, live SDL: %.1f fps, p50 %.2f ms, p95 %.2f ms, %d/%zu missed deadlines.\n",
                    width, width * 9 / 16, fps, frameMs[149], frameMs[284], missed, frameMs.size());
    }
    if (!live) {
        tap(replacement, SDL_CONTROLLER_BUTTON_START); tick();
        CHECK(duel.game.state().match == Match::Paused);
        tap(replacement, SDL_CONTROLLER_BUTTON_DPAD_DOWN); tick();
        CHECK(duel.app.settings().screen == MenuScreen::Settings && duel.present());
        tap(replacement, SDL_CONTROLLER_BUTTON_DPAD_DOWN); tick();
        const auto volume = duel.app.settings().volume;
        tap(replacement, SDL_CONTROLLER_BUTTON_DPAD_LEFT); tick();
        CHECK(duel.app.settings().volume == volume - 10);
        tap(replacement, SDL_CONTROLLER_BUTTON_B); tick();
        CHECK(duel.app.settings().screen == MenuScreen::Home && !duel.playing());
    }
    SDL_Event close{};
    close.type = SDL_WINDOWEVENT;
    close.window.event = SDL_WINDOWEVENT_CLOSE;
    CHECK(SDL_PushEvent(&close) == 1 && duel.pump() && !duel.running);
    std::puts("Local duel simulation, controller flow, reconnect, match point, rematch and window close passed.");
}

void testLocalCampaignDesktop(const fod::Assets& assets) {
    SdlSession session;
    SDL_SetMainReady();
    configureControllers();
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) == 0);
    VirtualPad first, second;
    const auto tapIn = [](SplitScreenDesktop& lobby, VirtualPad& pad, SDL_GameControllerButton button) {
        pad.set(button, true);
        CHECK(SDL_WaitEventTimeout(nullptr, 100) == 1 && lobby.pump());
        pad.set(button, false);
        CHECK(lobby.pump());
        lobby.tick();
    };
    {
        MenuState settings;
        settings.pendingLaunch = LaunchAction::ContinueCoOp;
        auto& save = settings.progress;
        save.coOp = 1;
        save.next = MissionChapter::Timberline;
        save.seconds = 276.5f;
        save.difficulty = 7;
        save.saved = save.boarded = 2;
        SplitScreenDesktop returning(assets, RenderLook::Enhanced, 400, 9, {}, MissionChapter::BriarHouse, settings);
        CHECK(returning.open());
        tapIn(returning, first, SDL_CONTROLLER_BUTTON_START);
        tapIn(returning, first, SDL_CONTROLLER_BUTTON_A);
        CHECK(!returning.started);
        tapIn(returning, second, SDL_CONTROLLER_BUTTON_START);
        tapIn(returning, first, SDL_CONTROLLER_BUTTON_A);
        CHECK(returning.playing());
        for (int seat = 0; seat < 2; ++seat) {
            const auto& view = returning.game.localView(seat);
            CHECK(view.campaignSeat == seat && view.mission && view.mission->chapter == save.next);
            CHECK(view.mission->earlierMissionSeconds == save.seconds &&
                  view.mission->civiliansBoarded == 2 && !view.mission->actors[1].active);
        }
        CHECK(returning.app.settings().difficulty == save.difficulty);
    }
    SplitScreenDesktop coOp(assets, RenderLook::Enhanced, 400, 12, {}, MissionChapter::BriarHouse);
    CHECK(coOp.open() && coOp.present());
    const auto tick = [&] { CHECK(coOp.pump()); coOp.tick(); };
    const auto tap = [&](VirtualPad& pad, SDL_GameControllerButton button) {
        tapIn(coOp, pad, button);
    };
    tap(first, SDL_CONTROLLER_BUTTON_START);
    tap(first, SDL_CONTROLLER_BUTTON_A);
    CHECK(!coOp.started);
    tap(second, SDL_CONTROLLER_BUTTON_START);
    tap(first, SDL_CONTROLLER_BUTTON_A);
    CHECK(coOp.playing() && coOp.game.state().mission->phase == MissionPhase::Intro);
    for (int i = 0; i < 65; ++i) tick();
    tap(first, SDL_CONTROLLER_BUTTON_A);
    for (int i = 0; i < 65; ++i) tick();
    tap(first, SDL_CONTROLLER_BUTTON_A);
    CHECK(coOp.game.state().mission->phase == MissionPhase::Overwatch);

    const auto& leader = coOp.game.localView(0);
    const auto& partner = coOp.game.localView(1);
    const float yaw = partner.playerYaw;
    const auto leadPosition = leader.player.position;
    second.set(SDL_CONTROLLER_AXIS_RIGHTX, 22000);
    for (int i = 0; i < 10; ++i) tick();
    second.set(SDL_CONTROLLER_AXIS_RIGHTX, 0);
    CHECK(partner.playerYaw > yaw + .05f && length(leader.player.position - leadPosition) < .01f);
    second.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    tick();
    CHECK(leader.clip == 30 && partner.clip == 29 && coOp.present());

    second.disconnect();
    tick();
    const auto pausedTick = leader.tick;
    CHECK(leader.match == Match::Paused && partner.match == Match::Paused);
    tap(first, SDL_CONTROLLER_BUTTON_A);
    CHECK(leader.match == Match::Paused && leader.tick == pausedTick);
    VirtualPad replacement;
    CHECK(coOp.pump());
    tap(replacement, SDL_CONTROLLER_BUTTON_START);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    tap(first, SDL_CONTROLLER_BUTTON_A);
    CHECK(coOp.playing());
    for (int i = 0; i < 20; ++i) tick();
    CHECK(partner.clip == 29);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    tick();
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    tick();
    CHECK(partner.clip == 28);
    replacement.set(SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
    tap(first, SDL_CONTROLLER_BUTTON_START);
    tap(first, SDL_CONTROLLER_BUTTON_B);
    CHECK(coOp.returnToTitle && !coOp.running);
    std::puts("Campaign co-op lobby, saved chapter rejoin, split views, independent controls and reconnect passed.");
}
#else
void testLocalDuelDesktop(const fod::Assets&, bool, int) {
    std::puts("Virtual controller integration tests require SDL 2.0.14 or later.");
}
void testLocalCampaignDesktop(const fod::Assets&) {}
#endif
#endif
