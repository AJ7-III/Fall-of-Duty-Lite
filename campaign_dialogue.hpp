#pragma once

#include <cstddef>
#include <cstdint>

namespace fod {

enum class CampaignLine : uint8_t {
    None,
#define CAMPAIGN_LINE(id, speaker, text, profile) id,
#include "campaign_dialogue.inc"
#include "scout_dialogue.inc"
#undef CAMPAIGN_LINE
    Count,
};

constexpr size_t campaignVoiceClipCount = size_t(CampaignLine::CivilianLost) - 1;

struct CampaignDialogue {
    const char* speaker;
    const char* text;
};

inline constexpr CampaignDialogue campaignDialogue[] = {
    {"", ""},
#define CAMPAIGN_LINE(id, speaker, text, profile) {speaker, text},
#include "campaign_dialogue.inc"
#include "scout_dialogue.inc"
#undef CAMPAIGN_LINE
};

struct CampaignVoiceCue {
    CampaignLine line = CampaignLine::None;
    bool chirp = false;
};

} // namespace fod
