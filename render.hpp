#pragma once

#include "game.hpp"

#include <memory>

namespace fod {

struct MenuState;
enum class RenderLook { Enhanced, Classic };
struct RenderStats {
    uint32_t visibleChunks = 0, triangles = 0, depthPasses = 0;
    uint32_t setupUs = 0, rasterUs = 0;
};

class Renderer {
public:
    explicit Renderer(const Assets& assets, RenderLook look = RenderLook::Enhanced);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    void render(const Snapshot& snapshot, int width, int height, uint16_t* rgb565);
    const RenderStats& stats() const;
    uint64_t workingBytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The logical world viewport occupies rows 15 through 464. On the target the
// baseline PPA fills these pixels; the CPU only adds the logical overlays.
void drawHud(const Assets& assets, const Snapshot& snapshot, const MenuState& menu, uint16_t* framebuffer, bool rotated,
             Speaker speaker = Speaker::None);
void drawRecovery(const char* message, uint16_t* framebuffer, bool rotated);
void drawNumberedFrame(uint32_t sequence, uint16_t* framebuffer, bool rotated);
void scaleWorld(const uint16_t* source, int width, int height, uint16_t* framebuffer, bool rotated);

} // namespace fod
