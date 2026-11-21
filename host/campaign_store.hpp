#pragma once

#include "campaign_progress.hpp"

#include <SDL_filesystem.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

namespace fod::desktop {

inline std::filesystem::path campaignPath() {
    char* dir = SDL_GetPrefPath("Fall of Duty", "Fall of Duty Lite");
    if (!dir) return {};
    auto path = std::filesystem::u8path(dir);
    SDL_free(dir);
    return path / "campaign-v1.sav";
}

inline CampaignProgress readCampaign(const std::filesystem::path& path) {
    CampaignProgress progress{};
    std::ifstream file(path, std::ios::binary);
    const bool valid = file.read(reinterpret_cast<char*>(&progress), sizeof(progress)) &&
                       file.peek() == std::ifstream::traits_type::eof() && !file.bad() && progress.available();
    return valid ? progress : CampaignProgress{};
}

inline bool storeCampaign(const std::filesystem::path& path, const CampaignProgress& progress) {
    if (path.empty()) return false;
    std::error_code error;
    if (progress.next == MissionChapter::BriarHouse) {
        std::filesystem::remove(path, error);
        return !error;
    }
    if (!progress.available()) return false;
    auto temporary = path;
    temporary += "." + std::to_string(std::random_device{}()) + ".tmp";
#ifdef _WIN32
    auto* file = _wfopen(temporary.c_str(), L"wbx");
#else
    auto* file = std::fopen(temporary.c_str(), "wbx");
#endif
    if (!file) return false;
    const bool wrote = std::fwrite(&progress, sizeof(progress), 1, file) == 1;
    const bool closed = std::fclose(file) == 0;
    if (wrote && closed) {
        std::filesystem::rename(temporary, path, error);
        if (!error) return true;
    }
    std::filesystem::remove(temporary, error);
    return false;
}

inline bool writeCampaign(const CampaignProgress& progress) {
    return storeCampaign(campaignPath(), progress);
}

} // namespace fod::desktop
