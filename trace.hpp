#pragma once

#include "app.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace fod {

class ReplayTrace {
public:
    bool load(const char* path, std::string& error);
    void begin(App& app);
    bool step(App& app);
    size_t length() const { return inputs_.size(); }
    size_t unusedDraws() const;
    uint32_t missingDraws() const { return missingDraws_; }

private:
    struct Draw {
        uint32_t tick = 0;
        std::array<char, 40> site{};
        double value = 0;
        bool used = false;
    };
    static bool supply(void* data, uint32_t tick, const char* site, double& value);

    Fixture fixture_{};
    uint32_t seed_ = 0, missingDraws_ = 0;
    int difficulty_ = 3;
    bool alternate_ = false;
    size_t at_ = 0, firstDraw_ = 0;
    std::vector<Input> inputs_;
    std::vector<Draw> draws_;
};

void writeTraceTick(FILE* out, const App& app);

} // namespace fod
