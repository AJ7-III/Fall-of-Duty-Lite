#pragma once

#include <cstdint>

namespace fod {

enum class MissionChapter : uint8_t { BriarHouse, Timberline, Array };

// Version 1 on the supported little-endian platforms. Only completed missions are saved.
struct CampaignProgress {
    uint8_t version = 1;
    MissionChapter next = MissionChapter::BriarHouse;
    uint8_t difficulty = 5, saved = 0, boarded = 0, noraStayed = 0, coOp = 0, reserved = 0;
    float seconds = 0;

    bool available() const {
        return version == 1 && (next == MissionChapter::Timberline || next == MissionChapter::Array) &&
               difficulty >= 1 && difficulty <= 10 && saved <= 3 && boarded <= 2 && noraStayed <= 1 &&
               saved == boarded + noraStayed && coOp <= 1 && reserved == 0 && seconds >= 0 && seconds < 1.0e9f;
    }
};
static_assert(sizeof(CampaignProgress) == 12);

} // namespace fod
