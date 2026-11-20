#include "app.hpp"
#include "desktop_controls.hpp"
#include "render.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <random>
#include <utility>

namespace {
using namespace fod;
using namespace fod::desktop;
using Clock = std::chrono::steady_clock;

uint32_t key(SDL_Scancode k) {
    switch (k) {
    case SDL_SCANCODE_RETURN: return Confirm;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_P: return Pause;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return Sprint;
    case SDL_SCANCODE_Z: return Jump;
    case SDL_SCANCODE_C: return Stance;
    case SDL_SCANCODE_R: return Reload;
    case SDL_SCANCODE_X: return Swap;
    case SDL_SCANCODE_4: return Streak;
    case SDL_SCANCODE_SPACE: return Aim;
    case SDL_SCANCODE_UP: return Up | Streak;
    case SDL_SCANCODE_DOWN: return Down;
    case SDL_SCANCODE_LEFT: return Left;
    case SDL_SCANCODE_RIGHT: return Right;
    default: return 0;
    }
}

void audioCallback(void* data, uint8_t* buffer, int bytes) {
    static_cast<Mixer*>(data)->mix(reinterpret_cast<int16_t*>(buffer), size_t(bytes) / (2 * sizeof(int16_t)));
}

struct Sdl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    int canvasWidth = 0;
    SDL_GameController* controller = nullptr;
    SDL_JoystickID controllerId = -1;
    MenuControls controls = MenuControls::Keyboard;
    SDL_AudioDeviceID audio = 0;

    ~Sdl() {
        if (audio) SDL_CloseAudioDevice(audio);
        if (controller) SDL_GameControllerClose(controller);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    bool surface(int initialWidth, Arena arena, BriarHouseMode briarMode = BriarHouseMode::Standard) {
        const int width = std::max(800, initialWidth);
        const char* title = arena == Arena::BriarHouse
            ? (briarMode == BriarHouseMode::Invasion ? "Fall of Duty Lite | Mission 1: Briar House"
                                                     : "Fall of Duty Lite | Briar House")
            : "Fall of Duty Lite | Ship Box";
        window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  width, width * 3 / 5, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window) return false;
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) return false;
        return SDL_RenderSetLogicalSize(renderer, 800, 480) == 0;
    }

    bool presentation(int width, RenderLook look, bool reset = false) {
        if (width == canvasWidth && !reset) return true;
        auto* next = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, width, width * 3 / 5);
        if (!next) return false;
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(next, look == RenderLook::Enhanced ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
        if (texture) SDL_DestroyTexture(texture);
        texture = next;
        canvasWidth = width;
        return true;
    }

    bool openController(int index) {
        if (controller || !SDL_IsGameController(index)) return false;
        controller = SDL_GameControllerOpen(index);
        if (!controller) return false;
        controllerId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
        controls = controllerControls(controller);
        return true;
    }
};

} // namespace

fod::LaunchAction runDesktop(const fod::Assets& assets, int initialWidth, fod::RenderLook look,
                             std::optional<float> stickSensitivity, bool autoStart, fod::MenuState* carry) {
    using namespace fod;
    App app(assets, std::random_device{}());
    if (carry) {
        auto& dst = app.settings();
        dst.callsign = carry->callsign;
        dst.difficulty = carry->difficulty;
        dst.volume = carry->volume;
        dst.opponent = carry->opponent;
        dst.selectedArena = carry->selectedArena;
        dst.compact = carry->compact;
        dst.voice = carry->voice;
        dst.desktopRenderWidth = carry->desktopRenderWidth ? carry->desktopRenderWidth : initialWidth;
        dst.controls = carry->controls != MenuControls::Default ? carry->controls : MenuControls::Keyboard;
    } else {
        app.settings().desktopRenderWidth = initialWidth;
        app.settings().controls = MenuControls::Keyboard;
    }
    if (autoStart) {
        const bool isCampaign = assets.arena == Arena::BriarHouse && assets.briarHouseMode == BriarHouseMode::Invasion;
        app.startImmediate(isCampaign);
    }
    constexpr int maxWorldPixels = desktopHdWidth * (desktopHdWidth * 9 / 16);
    constexpr int maxCanvasPixels = desktopHdWidth * (desktopHdWidth * 3 / 5);
    Renderer renderer(assets, look, maxWorldPixels);
    std::vector<uint16_t> world(maxWorldPixels), framebuffer(maxCanvasPixels);
    WorldFrame worldFrame(renderer, world.data());
    std::optional<MenuFrameKey> lastMenu;
    uint32_t matchRevision = 0;
    Sdl sdl;
    SDL_SetMainReady();
    configureControllers();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) < 0 ||
        !sdl.surface(app.settings().width(), assets.arena, assets.briarHouseMode) ||
        !sdl.presentation(std::max(800, app.settings().width()), look)) {
        std::fprintf(stderr, "SDL display: %s\n", SDL_GetError());
        return LaunchAction::None;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
        SDL_AudioSpec spec{};
        spec.freq = audioHz;
        spec.format = AUDIO_S16SYS;
        spec.channels = 2;
        spec.samples = 512;
        spec.callback = &audioCallback;
        spec.userdata = &app.audio;
        sdl.audio = SDL_OpenAudioDevice(nullptr, 0, &spec, nullptr, 0);
    }
    if (sdl.audio) SDL_PauseAudioDevice(sdl.audio, 0);
    else std::fprintf(stderr, "SDL audio: %s\n", SDL_GetError());
    for (int i = 0; i < SDL_NumJoysticks() && !sdl.controller; ++i) sdl.openController(i);
    if (!carry || carry->controls == MenuControls::Default) app.settings().controls = sdl.controls;
    if (autoStart) {
        SDL_RaiseWindow(sdl.window);
        if (wantsRelativeMouse(app.state().match, app.settings().controls, true)) {
            SDL_SetRelativeMouseMode(SDL_TRUE);
        }
    }

    uint32_t keys = 0, pad = 0, edges = 0, mouseButtons = 0, mouseEdges = 0;
    int mouseX = 0, mouseY = 0;
    int16_t touchX = -1, touchY = -1;
    bool focused = true, focusLost = false, controllerLost = false, running = true;
    auto previous = Clock::now();
    double elapsed = 0;
    std::array<int16_t, 2 * audioHz / 60> silence{};

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                const uint32_t action = key(event.key.keysym.scancode);
                if (action && !event.key.repeat) {
                    const bool down = event.type == SDL_KEYDOWN;
                    transition(keys, edges, action, down);
                    if (down && app.state().match != Match::Playing) app.settings().controls = MenuControls::Keyboard;
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
                const bool down = event.type == SDL_MOUSEBUTTONDOWN;
                const bool left = event.button.button == SDL_BUTTON_LEFT;
                if (left && down && app.state().match != Match::Playing) {
                    touchX = int16_t(event.button.x);
                    touchY = int16_t(event.button.y);
                    app.settings().controls = MenuControls::Keyboard;
                } else if (SDL_GetRelativeMouseMode()) {
                    const uint32_t action = left ? Fire : event.button.button == SDL_BUTTON_RIGHT ? Aim : 0u;
                    transition(mouseButtons, mouseEdges, action, down);
                } else if (left && down && focused) {
                    // The first click takes control of the camera without firing.
                    app.settings().controls = MenuControls::Keyboard;
                }
                break;
            }
            case SDL_MOUSEMOTION:
                if (app.state().match == Match::Playing && focused && SDL_GetRelativeMouseMode()) {
                    mouseX += event.motion.xrel;
                    mouseY += event.motion.yrel;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (sdl.openController(event.cdevice.which) && app.state().match != Match::Playing) {
                    app.settings().controls = sdl.controls;
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (sdl.controller && event.cdevice.which == sdl.controllerId) {
                    SDL_GameControllerClose(sdl.controller);
                    sdl.controller = nullptr;
                    sdl.controllerId = -1;
                    sdl.controls = app.settings().controls = MenuControls::Keyboard;
                    controllerLost = true;
                    pad = edges = 0;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
                if (event.cbutton.which == sdl.controllerId) {
                    const uint32_t action = button(event.cbutton.button);
                    const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
                    transition(pad, edges, action, down);
                    if (down && action) app.settings().controls = sdl.controls;
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (event.caxis.which != sdl.controllerId) break;
                // A trigger tap can begin and end between two simulation ticks.
                if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                    transition(pad, edges, Aim, event.caxis.value > triggerThreshold);
                if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                    transition(pad, edges, Fire, event.caxis.value > triggerThreshold);
                if (activePadAxis(sdl.controller, event.caxis.axis, event.caxis.value)) {
                    app.settings().controls = sdl.controls;
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    focused = false;
                    focusLost = true;
                    keys = edges = mouseButtons = mouseEdges = 0;
                    mouseX = mouseY = 0;
                    if (SDL_GetRelativeMouseMode()) SDL_SetRelativeMouseMode(SDL_FALSE);
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
                if (event.window.event == SDL_WINDOWEVENT_EXPOSED || event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    event.window.event == SDL_WINDOWEVENT_RESTORED || event.window.event == SDL_WINDOWEVENT_SHOWN) lastMenu.reset();
                break;
            case SDL_RENDER_DEVICE_RESET:
                if (!sdl.presentation(std::max(800, app.settings().width()), look, true)) {
                    std::fprintf(stderr, "SDL display reset: %s\n", SDL_GetError());
                    return LaunchAction::None;
                }
                [[fallthrough]];
            case SDL_RENDER_TARGETS_RESET:
                lastMenu.reset();
                break;
            default: break;
            }
        }
        const auto now = Clock::now();
        const double delta = std::chrono::duration<double>(now - previous).count();
        previous = now;
        elapsed += std::min(delta, 4 * tickSeconds);

        int work = 0;
        while (elapsed >= tickSeconds && work < 4) {
            ++work;
            elapsed -= tickSeconds;
            Input input;
            const auto* held = SDL_GetKeyboardState(nullptr);
            input.moveX = float(held[SDL_SCANCODE_D]) - float(held[SDL_SCANCODE_A]);
            input.moveY = float(held[SDL_SCANCODE_W]) - float(held[SDL_SCANCODE_S]);
            const bool mouseActive = wantsRelativeMouse(app.state().match, app.settings().controls, focused) &&
                                     SDL_GetRelativeMouseMode();
            if (mouseActive) {
                input.lookX = float(mouseX) * 0.0023f;
                input.lookY = float(mouseY) * 0.0023f;
            }
            if (sdl.controller) {
                samplePad(sdl.controller, sdl.controls, stickSensitivity, input, pad, edges);
            }
            input.held = keys | pad | (mouseActive ? mouseButtons : 0u);
            input.pressed = edges | (mouseActive ? mouseEdges : 0u);
            input.touchX = touchX;
            input.touchY = touchY;
            // Pause once on loss, then allow keyboard and mouse menu input.
            const bool lostController = std::exchange(controllerLost, false);
            const bool lostFocus = std::exchange(focusLost, false);
            input.disconnected = lostController || lostFocus || !focused;
            const auto old = app.state().match;
            app.tick(input);
            if (app.settings().pendingLaunch != LaunchAction::None) {
                const auto action = app.settings().pendingLaunch;
                if (carry) *carry = app.settings();
                return action;
            }
            if (!sdl.audio) app.audio.mix(silence.data(), silence.size() / 2);
            if (old != app.state().match) {
                ++matchRevision;
                elapsed = 0;
            }
            const auto capture = wantsRelativeMouse(app.state().match, app.settings().controls, focused) ? SDL_TRUE : SDL_FALSE;
            if (SDL_GetRelativeMouseMode() != capture) SDL_SetRelativeMouseMode(capture);
            if (!capture) mouseButtons = 0;
            edges = 0;
            mouseEdges = 0;
            mouseX = mouseY = 0;
            touchX = touchY = -1;
        }
        const bool playing = app.state().match == Match::Playing;
        // Present the latest simulation state once after the tick, including after catch-up.
        if (work == 0 && (playing || lastMenu)) {
            SDL_Delay(1);
            continue;
        }
        const auto& menu = app.settings();
        const auto speaker = sdl.audio ? app.audio.speaking() : Speaker::None;
        std::optional<MenuFrameKey> menuKey;
        if (!playing) {
            menuKey.emplace(matchRevision, app.state().match, menu, speaker);
            if (menuKey == lastMenu) continue;
        }
        const int width = menu.width(), height = menu.height(), canvas = std::max(800, width);
        if (!sdl.presentation(canvas, look)) {
            std::fprintf(stderr, "SDL presentation: %s\n", SDL_GetError());
            return LaunchAction::None;
        }
        worldFrame.render(app.state(), matchRevision, width, height);
        if (width == canvas) {
            // The world covers logical rows 15 through 464. Preserve all the rendered pixels.
            std::copy_n(world.data(), width * height, framebuffer.data() + width * (15 * width / 800));
        } else {
            scaleWorld(world.data(), width, height, framebuffer.data(), false, look == RenderLook::Enhanced);
        }
        drawDesktopHud(assets, app.state(), menu, framebuffer.data(), canvas, speaker);
        if (SDL_UpdateTexture(sdl.texture, nullptr, framebuffer.data(), canvas * sizeof(uint16_t)) < 0 ||
            SDL_RenderClear(sdl.renderer) < 0 || SDL_RenderCopy(sdl.renderer, sdl.texture, nullptr, nullptr) < 0) {
            SDL_Delay(1);
            continue;
        }
        SDL_RenderPresent(sdl.renderer);
        lastMenu = menuKey;
    }
    if (carry) *carry = app.settings();
    return LaunchAction::None;
}
