#pragma once

#include "audio.hpp"
#include "game.hpp"

namespace fod {

struct MenuState {
    std::array<char, 17> callsign{'P', 'L', 'A', 'Y', 'E', 'R', '\0'};
    int selection = 0, difficulty = defaultDifficulty, volume = 70, callsignCursor = 0;
    Opponent opponent = defaultOpponent;
    bool compact = false, voice = true, editing = false, setup = false;

    int width() const { return compact ? 320 : 400; }
    int height() const { return compact ? 180 : 225; }

    bool operator==(const MenuState& other) const {
        return callsign == other.callsign && selection == other.selection && difficulty == other.difficulty &&
               volume == other.volume && callsignCursor == other.callsignCursor && opponent == other.opponent &&
               compact == other.compact && voice == other.voice && editing == other.editing && setup == other.setup;
    }
};

// The composed menu can stay on screen until the match, settings, or overlays change.
struct MenuFrameKey {
    MenuFrameKey(uint32_t revision, Match match, const MenuState& settings, Speaker audible,
                 bool controllerPrompt = false, bool marker = false)
        : revision(revision), settings(settings),
          speaker(match == Match::Results && settings.voice && settings.volume > 0 ? audible : Speaker::None),
          controllerPrompt(controllerPrompt), marker(marker) {}

    bool operator==(const MenuFrameKey& other) const {
        return revision == other.revision && settings == other.settings && speaker == other.speaker &&
               controllerPrompt == other.controllerPrompt && marker == other.marker;
    }

    uint32_t revision;
    MenuState settings;
    Speaker speaker;
    bool controllerPrompt, marker;
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
    explicit App(const Assets& assets, uint32_t voiceSeed = 0);
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
