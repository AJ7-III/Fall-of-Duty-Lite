#pragma once

#include "game.hpp"

#include <memory>

namespace fod {

struct MenuState;
enum class RenderLook { Enhanced, Classic };
struct RenderStats {
    // Work counts are collected by desktop benchmarks. The handheld records timings.
    uint32_t visibleChunks = 0, triangles = 0, depthPasses = 0;
    uint32_t setupUs = 0, rasterUs = 0;
};

class Renderer {
public:
    explicit Renderer(const Assets& assets, RenderLook look = RenderLook::Enhanced);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void render(const Snapshot& snapshot, int width, int height, uint16_t* rgb565, bool measure = true);
    const RenderStats& stats() const;
    uint64_t workingBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Reuses the render-owned source buffer when a menu is shown over a frozen world.
class WorldFrame {
public:
    WorldFrame(Renderer& renderer, uint16_t* pixels) : renderer_(renderer), pixels_(pixels) {}
    // Advance matchRevision for every match transition, including between presentations.
    bool render(const Snapshot& snapshot, uint32_t matchRevision, int width, int height);

private:
    Renderer& renderer_;
    uint16_t* pixels_;
    uint32_t matchRevision_ = 0;
    int width_ = 0, height_ = 0, difficulty_ = 0;
};

// The logical world viewport occupies rows 15 through 464. On the target the
// baseline PPA fills these pixels; the CPU only adds the logical overlays.
void drawHud(const Assets& assets, const Snapshot& snapshot, const MenuState& menu, uint16_t* framebuffer, bool rotated,
             Speaker speaker = Speaker::None);
void drawRecovery(const char* message, uint16_t* framebuffer, bool rotated);
void drawNumberedFrame(uint32_t sequence, uint16_t* framebuffer, bool rotated);
// The CPU scaler for desktops and captures. Smooth scaling filters the way the handheld's PPA does.
void scaleWorld(const uint16_t* source, int width, int height, uint16_t* framebuffer, bool rotated, bool smooth = false);

} // namespace fod
