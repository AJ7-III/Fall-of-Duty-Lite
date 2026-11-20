#include "app.hpp"
#include "render.hpp"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
using namespace fod;
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

uint32_t button(int b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return Jump | Confirm;
    case SDL_CONTROLLER_BUTTON_B: return Stance | Back;
    case SDL_CONTROLLER_BUTTON_X: return Reload;
    case SDL_CONTROLLER_BUTTON_Y: return Swap;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return Sprint;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return Up | Streak;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Right;
    case SDL_CONTROLLER_BUTTON_START: return Pause;
    default: return 0;
    }
}

void transition(uint32_t& held, uint32_t& edges, uint32_t mask, bool pressed) {
    if (pressed) {
        edges |= mask & ~held;
        held |= mask;
    } else {
        held &= ~mask;
    }
}

void audioCallback(void* data, uint8_t* buffer, int bytes) {
    static_cast<Mixer*>(data)->mix(reinterpret_cast<int16_t*>(buffer), size_t(bytes) / (2 * sizeof(int16_t)));
}

float axis(int16_t value) { return std::clamp(float(value) / 32767, -1.0f, 1.0f); }

void deadZone(float& x, float& y, float dead, bool curve) {
    const float length = std::sqrt(x * x + y * y);
    const float t = std::clamp((length - dead) / (1 - dead), 0.0f, 1.0f);
    const float scale = (curve ? 0.37f * t + 0.63f * t * t : t) / std::max(length, 0.0001f);
    x *= scale;
    y *= scale;
}

struct Sdl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_GameController* controller = nullptr;
    SDL_JoystickID controllerId = -1;
    SDL_AudioDeviceID audio = 0;

    ~Sdl() {
        if (audio) SDL_CloseAudioDevice(audio);
        if (controller) SDL_GameControllerClose(controller);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    bool surface() {
        window = SDL_CreateWindow("Fall of Duty Lite | Ship Box", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  800, 480, SDL_WINDOW_RESIZABLE);
        if (!window) return false;
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) return false;
        SDL_RenderSetLogicalSize(renderer, 800, 480);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 800, 480);
        return texture != nullptr;
    }

    bool openController(int index) {
        if (controller || !SDL_IsGameController(index)) return false;
        controller = SDL_GameControllerOpen(index);
        if (!controller) return false;
        controllerId = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
        return true;
    }
};

} // namespace

int runDesktop(const fod::Assets& assets, bool compact, fod::RenderLook look) {
    using namespace fod;
    App app(assets);
    app.settings().compact = compact;
    Renderer renderer(assets, look);
    std::vector<uint16_t> world(400 * 225), framebuffer(800 * 480);
    WorldFrame worldFrame(renderer, world.data());
    uint32_t matchRevision = 0;
    Sdl sdl;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) < 0 || !sdl.surface()) {
        std::fprintf(stderr, "SDL display: %s\n", SDL_GetError());
        return 1;
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

    uint32_t keys = 0, pad = 0, edges = 0;
    int mouseX = 0, mouseY = 0;
    int16_t touchX = -1, touchY = -1;
    bool focused = true, unplugged = false, running = true;
    auto previous = Clock::now();
    double elapsed = 0, frameElapsed = 1;
    std::array<int16_t, 2 * audioHz / 60> silence{};

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN: case SDL_KEYUP:
                if (!event.key.repeat) transition(keys, edges, key(event.key.keysym.scancode), event.type == SDL_KEYDOWN);
                break;
            case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
                if (event.button.button == SDL_BUTTON_LEFT && app.state().match != Match::Playing && event.type == SDL_MOUSEBUTTONDOWN) {
                    touchX = int16_t(event.button.x);
                    touchY = int16_t(event.button.y);
                } else {
                    const uint32_t b = event.button.button == SDL_BUTTON_LEFT ? Fire : event.button.button == SDL_BUTTON_RIGHT ? Aim : 0u;
                    transition(keys, edges, b, event.type == SDL_MOUSEBUTTONDOWN);
                }
                break;
            case SDL_MOUSEMOTION:
                if (app.state().match == Match::Playing && focused && SDL_GetRelativeMouseMode()) {
                    mouseX += event.motion.xrel;
                    mouseY += event.motion.yrel;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (sdl.openController(event.cdevice.which)) unplugged = false;
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (sdl.controller && event.cdevice.which == sdl.controllerId) {
                    SDL_GameControllerClose(sdl.controller);
                    sdl.controller = nullptr;
                    sdl.controllerId = -1;
                    unplugged = true;
                    pad = edges = 0;
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
                if (event.cbutton.which == sdl.controllerId)
                    transition(pad, edges, button(event.cbutton.button), event.type == SDL_CONTROLLERBUTTONDOWN);
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (event.caxis.which != sdl.controllerId) break;
                // A trigger tap can begin and end between two simulation ticks.
                if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                    transition(pad, edges, Aim, event.caxis.value > 5500);
                if (event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                    transition(pad, edges, Fire, event.caxis.value > 5500);
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    focused = false;
                    keys = edges = 0;
                    mouseX = mouseY = 0;
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
                break;
            default: break;
            }
        }
        const auto now = Clock::now();
        const double delta = std::chrono::duration<double>(now - previous).count();
        previous = now;
        elapsed += std::min(delta, 4 * tickSeconds);
        frameElapsed += delta;

        int work = 0;
        while (elapsed >= tickSeconds && work++ < 4) {
            elapsed -= tickSeconds;
            Input input;
            const auto* held = SDL_GetKeyboardState(nullptr);
            input.moveX = float(held[SDL_SCANCODE_D]) - float(held[SDL_SCANCODE_A]);
            input.moveY = float(held[SDL_SCANCODE_W]) - float(held[SDL_SCANCODE_S]);
            input.lookX = float(mouseX) * 0.0023f;
            input.lookY = float(mouseY) * 0.0023f;
            if (sdl.controller) {
                float mx = axis(SDL_GameControllerGetAxis(sdl.controller, SDL_CONTROLLER_AXIS_LEFTX));
                float my = -axis(SDL_GameControllerGetAxis(sdl.controller, SDL_CONTROLLER_AXIS_LEFTY));
                float lx = axis(SDL_GameControllerGetAxis(sdl.controller, SDL_CONTROLLER_AXIS_RIGHTX));
                float ly = axis(SDL_GameControllerGetAxis(sdl.controller, SDL_CONTROLLER_AXIS_RIGHTY));
                deadZone(mx, my, 0.16f, false);
                deadZone(lx, ly, 0.18f, true);
                input.moveX += mx;
                input.moveY += my;
                input.lookX += lx * 0.056f;
                input.lookY += ly * 0.045f;
                transition(pad, edges, Aim, SDL_GameControllerGetAxis(sdl.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 5500);
                transition(pad, edges, Fire, SDL_GameControllerGetAxis(sdl.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 5500);
            }
            input.held = keys | pad;
            input.pressed = edges;
            input.touchX = touchX;
            input.touchY = touchY;
            input.disconnected = !focused || unplugged;
            if (unplugged && focused && (edges & Confirm)) unplugged = input.disconnected = false;
            const auto old = app.state().match;
            app.tick(input);
            if (!sdl.audio) app.audio.mix(silence.data(), silence.size() / 2);
            if (old != app.state().match) {
                ++matchRevision;
                elapsed = 0;
                SDL_SetRelativeMouseMode(app.state().match == Match::Playing ? SDL_TRUE : SDL_FALSE);
            }
            edges = 0;
            mouseX = mouseY = 0;
            touchX = touchY = -1;
        }
        if (frameElapsed >= 1.0 / 30) {
            frameElapsed = std::fmod(frameElapsed, 1.0 / 30);
            const auto& menu = app.settings();
            worldFrame.render(app.state(), matchRevision, menu.width(), menu.height());
            scaleWorld(world.data(), menu.width(), menu.height(), framebuffer.data(), false, look == RenderLook::Enhanced);
            drawHud(assets, app.state(), menu, framebuffer.data(), false, sdl.audio ? app.audio.speaking() : Speaker::None);
            SDL_UpdateTexture(sdl.texture, nullptr, framebuffer.data(), 800 * sizeof(uint16_t));
            SDL_RenderClear(sdl.renderer);
            SDL_RenderCopy(sdl.renderer, sdl.texture, nullptr, nullptr);
            SDL_RenderPresent(sdl.renderer);
        } else {
            SDL_Delay(1);
        }
    }
    return 0;
}
