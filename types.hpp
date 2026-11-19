#pragma once

#include <cmath>
#include <cstdint>

namespace fod {

constexpr float pi = 3.14159265358979323846f;
constexpr double tickSeconds = 1.0 / 60.0;

struct Vec3 {
    float x = 0, y = 0, z = 0;

    Vec3 operator+(Vec3 v) const { return {x + v.x, y + v.y, z + v.z}; }
    Vec3 operator-(Vec3 v) const { return {x - v.x, y - v.y, z - v.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return *this * (1.0f / s); }
    Vec3& operator+=(Vec3 v) { return *this = *this + v; }
    Vec3& operator-=(Vec3 v) { return *this = *this - v; }
    Vec3& operator*=(float s) { return *this = *this * s; }
};

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalized(Vec3 v) { return v / std::fmax(length(v), 0.000001f); }
inline Vec3 forward(float yaw, float pitch) {
    const float c = std::cos(pitch);
    return {std::sin(yaw) * c, -std::sin(pitch), std::cos(yaw) * c};
}

enum Button : uint32_t {
    Fire = 1u << 0, Aim = 1u << 1, Sprint = 1u << 2, Jump = 1u << 3, Stance = 1u << 4,
    Reload = 1u << 5, Swap = 1u << 6, Streak = 1u << 7, Pause = 1u << 8,
    Confirm = 1u << 9, Back = 1u << 10, Up = 1u << 11, Down = 1u << 12,
    Left = 1u << 13, Right = 1u << 14
};

struct Input {
    float moveX = 0, moveY = 0;
    // Yaw/pitch radians before the weapon's ADS multiplier; positive pitch looks down.
    float lookX = 0, lookY = 0;
    uint32_t held = 0, pressed = 0;
    int16_t touchX = -1, touchY = -1; // edge in 800 x 480 landscape coordinates
    bool disconnected = false;

    bool down(Button b) const { return (held & b) != 0; }
    bool edge(Button b) const { return (pressed & b) != 0; }
};

enum class Sound : uint8_t {
    Mp44, Sniper, Pistol, Reload, Bolt, Dry, Footstep, Impact, Glass, Hit,
    Death, Uav, Laptop, Airstrike, Rotor, Rain, KillConfirm, HeadshotConfirm, VoiceStart, VoiceKill,
    VoiceVictory, VoiceAlternateStart, VoiceAlternateKill, VoiceAlternateVictory, Count
};

enum class Speaker : uint8_t { None, IPwndUrMom, Hanz };
constexpr bool defaultAlternateRival = true;

constexpr int audioHz = 24000;

} // namespace fod
