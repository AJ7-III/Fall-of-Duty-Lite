#pragma once

#include "app.hpp"

#include <SDL.h>
#include <algorithm>
#include <optional>
#include <string_view>

namespace fod::desktop {

inline void configureControllers() {
#ifdef SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS
    // Use the same physical face-button positions on every controller.
    // Nintendo's south button is printed B, while SDL's positional code is A.
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0", SDL_HINT_OVERRIDE);
#endif
}

#if SDL_VERSION_ATLEAST(2, 0, 12)
constexpr MenuControls controllerControls(SDL_GameControllerType type) {
    switch (type) {
    case SDL_CONTROLLER_TYPE_XBOX360: case SDL_CONTROLLER_TYPE_XBOXONE:
        return MenuControls::Xbox;
    case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4:
#if SDL_VERSION_ATLEAST(2, 0, 14)
    case SDL_CONTROLLER_TYPE_PS5:
#endif
        return MenuControls::PlayStation;
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
        return MenuControls::Nintendo;
    default:
        return MenuControls::Gamepad;
    }
}
#endif

inline MenuControls controllerControls(SDL_GameController* controller) {
    const char* name = SDL_GameControllerName(controller);
    if (name && std::string_view(name).find("8BitDo Pro 2") != std::string_view::npos) {
        return MenuControls::Nintendo;
    }
#if SDL_VERSION_ATLEAST(2, 0, 12)
    return controllerControls(SDL_GameControllerGetType(controller));
#else
    return MenuControls::Gamepad;
#endif
}

inline uint32_t button(int b) {
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

inline void transition(uint32_t& held, uint32_t& edges, uint32_t mask, bool pressed) {
    if (pressed) {
        edges |= mask & ~held;
        held |= mask;
    } else {
        held &= ~mask;
    }
}

inline float axis(int16_t value) { return std::clamp(float(value) / 32767, -1.0f, 1.0f); }

inline void deadZone(float& x, float& y, float dead, bool curve) {
    const float length = std::sqrt(x * x + y * y);
    const float t = std::clamp((length - dead) / (1 - dead), 0.0f, 1.0f);
    const float scale = (curve ? 0.37f * t + 0.63f * t * t : t) / std::max(length, 0.0001f);
    x *= scale;
    y *= scale;
}

constexpr int triggerThreshold = 5500;
constexpr float moveDeadZone = .16f, lookDeadZone = .18f;

inline bool activePadAxis(SDL_GameController* controller, int changedAxis, int16_t value) {
    if (changedAxis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || changedAxis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
        return value > triggerThreshold;
    }
    if (changedAxis < SDL_CONTROLLER_AXIS_LEFTX || changedAxis > SDL_CONTROLLER_AXIS_RIGHTY) return false;
    // SDL lists each stick's X and Y axes consecutively.
    const auto otherAxis = SDL_GameControllerAxis(changedAxis ^ 1);
    const float component = axis(value), other = axis(SDL_GameControllerGetAxis(controller, otherAxis));
    const float dead = changedAxis <= SDL_CONTROLLER_AXIS_LEFTY ? moveDeadZone : lookDeadZone;
    return component * component + other * other > dead * dead;
}

constexpr bool wantsRelativeMouse(Match match, MenuControls controls, bool focused) {
    return focused && match == Match::Playing && controls == MenuControls::Keyboard;
}

inline void samplePad(SDL_GameController* controller, MenuControls controls, std::optional<float> stickSensitivity,
                      Input& input, uint32_t& held, uint32_t& edges) {
    float mx = axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
    float my = -axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
    float lx = axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
    float ly = axis(SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
    deadZone(mx, my, moveDeadZone, false);
    deadZone(lx, ly, lookDeadZone, true);
    input.moveX += mx;
    input.moveY += my;
    const float sensitivity = stickSensitivity.value_or(controls == MenuControls::Nintendo ? 0.8f : 1.0f);
    input.lookX += lx * 0.056f * sensitivity;
    input.lookY += ly * 0.045f * sensitivity;
    transition(held, edges, Aim, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > triggerThreshold);
    transition(held, edges, Fire, SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > triggerThreshold);
}

} // namespace fod::desktop
