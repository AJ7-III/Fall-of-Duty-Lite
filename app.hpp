#pragma once

#include "audio.hpp"
#include "game.hpp"

namespace fod {

struct MenuState {
    std::array<char, 17> callsign{'P', 'L', 'A', 'Y', 'E', 'R', '\0'};
    int selection = 0, difficulty = 3, volume = 70, callsignCursor = 0;
    bool compact = false, voice = true, alternateRival = defaultAlternateRival, editing = false, setup = false;

    int width() const { return compact ? 320 : 400; }
    int height() const { return compact ? 180 : 225; }
};

enum MenuRow { PlayRow, DifficultyRow, RivalRow, VoiceRow, VolumeRow, RenderRow, CallsignRow, ExitRow, MenuRows };
constexpr int startX = 42, startWidth = 308, startY = 254, startHeight = 62;
constexpr int setupY = startY + startHeight + 12, setupHeight = 48;
constexpr int startSetupSelection = 1;
constexpr int menuX = 177, menuY = 148, menuWidth = 446, menuRowHeight = 33;
constexpr int keyboardX = menuX + 12, keyboardY = 235, keyWidth = 42, keyHeight = 32, keyboardColumns = 10;
constexpr char callsignKeys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-<";

class App {
public:
    explicit App(const Assets& assets);
    void tick(Input input);
    void routeAudio();
    const Snapshot& state() const { return game.state(); }
    MenuState& settings() { return menu_; }
    const MenuState& settings() const { return menu_; }
    Game game;
    Mixer audio;

private:
    void menuInput(const Input& input);
    void startOrResume(const Input& input);
    void select(int step);
    void change(int direction);
    void stopRotor();
    MenuState menu_{};
    uint32_t seed_ = 0x464f4425;
    uint32_t blockedActions_ = 0;
    bool connected_ = true, rotor_ = false;
};

} // namespace fod
