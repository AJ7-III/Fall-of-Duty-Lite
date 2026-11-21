#pragma once

#include "assets.hpp"

namespace fod {

// Reduce the shipped Ship Box visual mesh for play. Load the original pack for campaign prop extraction.
void optimizeShipBoxWorld(Assets& assets);

// Replace the loaded arena while reusing the pack's actors, weapons, and audio.
// Invasion mode adds the alien invasion backdrop, procedural alien rigs, uniforms, and sci-fi SFX.
enum class CampaignArt { Compact, Desktop };
void buildBriarHouse(Assets& assets, BriarHouseMode mode = BriarHouseMode::Standard,
                     CampaignArt art = CampaignArt::Compact);

} // namespace fod
