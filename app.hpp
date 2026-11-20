#pragma once

#include "audio.hpp"
#include "game.hpp"

namespace fod {

#ifdef FOD_LOCAL_DUEL
constexpr std::array<int, 4> desktopRenderWidths{320, 400, 800, 1280};
constexpr int desktopHdWidth = desktopRenderWidths.back();

enum class MenuControls { Default, Keyboard, Gamepad, Xbox, PlayStation, Nintendo };

struct MenuLabels {
    const char* confirm = nullptr;
    const char* back = nullptr;
};

constexpr MenuLabels menuLabels(MenuControls controls) {
    switch (controls) {
    case MenuControls::Keyboard: return {"ENTER", "ESC"};
    case MenuControls::Gamepad: return {"BOTTOM BUTTON", "RIGHT BUTTON"};
    case MenuControls::Xbox: return {"A", "B"};
    case MenuControls::PlayStation: return {"CROSS", "CIRCLE"};
    case MenuControls::Nintendo: return {"B", "A"};
    default: return {};
    }
}
#endif

enum class StartItem : uint8_t { Campaign = 0, BotMatch = 1, MultiplayerLan = 2, MatchSetup = 3, Count = 4 };
enum class LaunchAction : uint8_t { None, Campaign, BotMatch, LocalDuel };

struct MenuState {
    std::array<char, 17> callsign{'P', 'L', 'A', 'Y', 'E', 'R', '\0'};
    int selection = int(StartItem::BotMatch), difficulty = defaultDifficulty, volume = 70, callsignCursor = 0;
    Opponent opponent = defaultOpponent;
    Arena selectedArena = Arena::ShipBox;
    LaunchAction pendingLaunch = LaunchAction::None;
    bool compact = false, voice = true, editing = false, setup = false, campaign = false, lanMenu = false;
#ifdef FOD_LOCAL_DUEL
    MenuControls controls = MenuControls::Default;
    int desktopRenderWidth = 0;
#endif

    int width() const {
#ifdef FOD_LOCAL_DUEL
        if (desktopRenderWidth) return desktopRenderWidth;
#endif
        return compact ? 320 : 400;
    }
    int height() const { return width() * 9 / 16; }

    bool operator==(const MenuState& other) const {
#ifdef FOD_LOCAL_DUEL
        if (controls != other.controls || desktopRenderWidth != other.desktopRenderWidth) return false;
#endif
        return callsign == other.callsign && selection == other.selection && difficulty == other.difficulty &&
               volume == other.volume && callsignCursor == other.callsignCursor && opponent == other.opponent &&
               selectedArena == other.selectedArena && pendingLaunch == other.pendingLaunch &&
               compact == other.compact && voice == other.voice && editing == other.editing && setup == other.setup &&
               campaign == other.campaign && lanMenu == other.lanMenu;
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
enum LanRow { LanStartRow, LanArenaRow, LanStatusRow, LanBackRow, LanRows };
constexpr int startX = 42, startWidth = 326;
struct StartCard {
    int y, height;
    const char* label;
};
constexpr std::array<StartCard, size_t(StartItem::Count)> startCards{{
    {183, 50, "CAMPAIGN"}, {241, 50, "BOT MATCH"},
    {299, 50, "MULTIPLAYER LAN"}, {357, 50, "MATCH SETUP"},
}};
constexpr const StartCard& startCard(StartItem item) { return startCards[size_t(item)]; }
constexpr int menuX = 177, menuY = 148, menuWidth = 446, menuRowHeight = 33;
constexpr int keyboardX = menuX + 12, keyboardY = 235, keyWidth = 42, keyHeight = 32, keyboardColumns = 10;
constexpr char callsignKeys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-<";

class App {
public:
    explicit App(const Assets& assets, uint32_t voiceSeed = 0);
    void tick(Input input);
    void startImmediate(bool asCampaign);
    void routeAudio();
    const Snapshot& state() const { return game.state(); }
    MenuState& settings() { return menu_; }
    const MenuState& settings() const { return menu_; }
    Game game;
    Mixer audio;

private:
    void menuInput(const Input& input);
    void startMenuInput(const Input& input);
    void lanMenuInput(const Input& input);
    void callsignInput(const Input& input);
    void beginMatch(bool retryCheckpoint);
    void startOrResume(const Input& input);
    void returnToStartMenu();
    void pauseMatch();
    void change(int direction);
    void stopAmbientLoops();
    Arena loadedArena_ = Arena::ShipBox;
    BriarHouseMode loadedBriarMode_ = BriarHouseMode::Standard;
    MenuState menu_{};
    uint32_t seed_ = 0x464f4425;
    uint32_t blockedActions_ = 0;
    bool connected_ = true, rotor_ = false, invasionHum_ = false;
};

} // namespace fod
