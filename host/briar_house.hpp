#pragma once

#include "assets.hpp"

namespace fod {

// Replace the loaded arena while reusing the pack's actors, weapons, and audio.
// Invasion mode adds the alien invasion backdrop, procedural alien rigs, uniforms, and sci-fi SFX.
void buildBriarHouse(Assets& assets, BriarHouseMode mode = BriarHouseMode::Standard);

} // namespace fod
