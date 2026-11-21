#include "app.hpp"
#include "assets.hpp"
#include "mission.hpp"

#include <algorithm>
#include <cstring>

namespace fod {
namespace {

void moveSelection(int& row, const Input& input, int rows) {
    const int direction = int(input.edge(Down)) - int(input.edge(Up));
    if (direction) row = (row + rows + direction) % rows;
}

int touchMenuRow(const Input& input, int rows) {
    const int y = input.touchY - menuY;
    if (input.touchX < menuX || input.touchX >= menuX + menuWidth || y < 0 || y >= rows * menuRowHeight) return -1;
    return y / menuRowHeight;
}

void toggleArena(Arena& arena) {
    arena = arena == Arena::ShipBox ? Arena::BriarHouse : Arena::ShipBox;
}

#ifdef FOD_LOCAL_DUEL
template <size_t N>
int cycleWidth(const std::array<int, N>& choices, int width, int direction) {
    const auto current = std::find(choices.begin(), choices.end(), width);
    const int index = int(current - choices.begin());
    return choices[size_t((index + int(N) + direction) % int(N))];
}
#endif

} // namespace

MenuOptions menuOptions(const MenuState& menu, Match match) {
    using A = MenuAction;
    MenuOptions rows;
    switch (menu.screen) {
    case MenuScreen::Campaign:
        if (menu.progress.available()) rows.add(A::Continue);
        rows.add(A::Solo);
#ifdef FOD_LOCAL_DUEL
        rows.add(A::CoOp);
#endif
        rows.add(A::Difficulty);
        break;
    case MenuScreen::LocalPlayers:
        rows.add(A::CoOp);
        rows.add(A::Versus);
        break;
    case MenuScreen::CoOpSetup:
        if (menu.progress.available() && menu.progress.coOp) rows.add(A::Continue);
        rows.add(A::StartCoOp);
        rows.add(A::Difficulty);
        break;
    case MenuScreen::VersusSetup:
        rows.add(A::Play);
        rows.add(A::Arena);
        break;
    case MenuScreen::BotSetup:
        rows.add(A::Play);
        rows.add(A::Arena);
        rows.add(A::Format);
        if (menu.opponent != Opponent::FreeForAll) rows.add(A::Rival);
        rows.add(A::Difficulty);
        break;
    case MenuScreen::Settings:
        rows.add(A::Voice);
        rows.add(A::Volume);
        rows.add(A::Render);
        rows.add(A::Callsign);
        break;
    case MenuScreen::Home:
        rows.add(A::Play);
        if (match == Match::Paused) {
            rows.add(A::Settings);
            rows.add(A::Difficulty);
        } else if (!menu.campaign) rows.add(A::ChangeMatch);
        rows.add(A::Exit);
        return rows;
    }
    rows.add(A::Back);
    return rows;
}

App::App(const Assets& assets, uint32_t voiceSeed)
    : game(assets, voiceSeed), audio(assets, voiceSeed ? voiceSeed ^ 0x29ea2831u : 0u),
      loadedArena_(assets.arena), loadedBriarMode_(assets.briarHouseMode) {
    menu_.campaign = assets.arena == Arena::BriarHouse && assets.briarHouseMode == BriarHouseMode::Invasion;
    menu_.selectedArena = assets.arena;
    menu_.selection = menu_.campaign ? int(StartItem::Campaign) : int(StartItem::BotMatch);
}

void App::startImmediate(bool asCampaign, MissionChapter chapter) {
    menu_.campaign = asCampaign;
    menu_.screen = MenuScreen::Home;
    chapterReplay_ = asCampaign && chapter != MissionChapter::BriarHouse;
    menu_.pendingLaunch = LaunchAction::None;
    menu_.selection = 0;
    audio.setVolume(menu_.volume);
    audio.pauseSpeech(false);
    if (asCampaign && chapter != MissionChapter::BriarHouse) {
        audio.stopSpeech();
        game.startChapterReplay(seed_++, menu_.difficulty, chapter);
        routeAudio();
    } else {
        beginMatch(false);
    }
}

void App::beginMatch(bool retryCheckpoint) {
    audio.stopSpeech();
    const auto seed = seed_++;
    if (menu_.campaign) {
        if (retryCheckpoint) game.startMission(seed, menu_.difficulty, true);
        else if (!game.advanceCampaign(seed, menu_.difficulty)) {
            chapterReplay_ = false;
            game.startCampaign(seed, menu_.difficulty);
        }
    } else {
        game.start(seed, menu_.difficulty, menu_.opponent);
    }
    routeAudio();
}

void App::stopAmbientLoops() {
    if (rotor_) {
        audio.stop(Sound::Rotor);
        rotor_ = false;
    }
    if (invasionHum_) {
        audio.stop(Sound::InvasionHum);
        invasionHum_ = false;
    }
}

void App::routeAudio() {
    const Snapshot& snapshot = state();
#ifdef FOD_LOCAL_DUEL
    const auto* partner = snapshot.campaignSeat == 0 ? &game.localView(1).camera : nullptr;
#endif
    if (snapshot.match == Match::Results && snapshot.mission && snapshot.mission->active) {
        audio.playCampaignVoice({});
    } else if (menu_.voice && snapshot.match == Match::Playing) {
        if (const auto* cue = game.campaignVoiceCue()) audio.playCampaignVoice(*cue);
    }
    for (size_t i = 0; i < game.eventCount(); ++i) {
        const auto& event = game.events()[i];
        if (event.kind != EventKind::Sound) continue;
        if (!menu_.voice && (isVoiceSound(event.sound) || event.sound == Sound::RadioChirp)) continue;
        float gain = event.volume, pan = 0;
        if (event.positional) {
            Vec3 direction = event.position - snapshot.camera.position;
            float yaw = snapshot.camera.yaw;
#ifdef FOD_LOCAL_DUEL
            if (partner) {
                const Vec3 second = event.position - partner->position;
                if (dot(second, second) < dot(direction, direction)) {
                    direction = second;
                    yaw = partner->yaw;
                }
            }
#endif
            const float distance = length(direction);
            const Vec3 ear{std::cos(yaw), 0, -std::sin(yaw)};
            pan = dot(ear, direction) / std::max(distance, 0.01f) * 0.8f;
            gain /= 1 + distance * 0.12f;
        }
        audio.play(event.sound, gain, pan);
    }
    // One rotor loop follows the nearer of the gunship and the campaign's evac bird.
    const bool evac = snapshot.mission && snapshot.mission->active && snapshot.mission->evacVisible &&
                      snapshot.match == Match::Playing;
    if (snapshot.apacheActive || evac) {
        Vec3 direction = snapshot.apacheActive ? snapshot.apachePosition - snapshot.camera.position : Vec3{1e3f, 0, 0};
        if (evac) {
            const Vec3 bird = snapshot.mission->evacPos - snapshot.camera.position;
            if (length(bird) < length(direction)) direction = bird;
        }
        const float distance = length(direction);
        const Vec3 ear{std::cos(snapshot.camera.yaw), 0, -std::sin(snapshot.camera.yaw)};
        audio.loop(Sound::Rotor, 0.67f / (1 + distance * 0.055f),
                   dot(ear, direction) / std::max(distance, 0.01f) * 0.75f);
        rotor_ = true;
    } else if (rotor_) {
        audio.stop(Sound::Rotor);
        rotor_ = false;
    }
    if (snapshot.match == Match::Playing && snapshot.mission && snapshot.mission->active) {
        const auto stage = snapshot.mission->operation.stage;
        const float hum = stage == OperationStage::ScoutFlight ? .36f : scoutDeck(stage) ? .25f : .16f;
        audio.loop(Sound::InvasionHum, hum, 0.f);
        invasionHum_ = true;
    } else if (invasionHum_) {
        audio.stop(Sound::InvasionHum);
        invasionHum_ = false;
    }
}

void App::change(MenuAction action, int direction) {
    switch (action) {
    case MenuAction::Difficulty:
        menu_.difficulty = std::clamp(menu_.difficulty + direction, 1, 10);
        game.setDifficulty(menu_.difficulty);
        break;
    case MenuAction::Arena: toggleArena(menu_.selectedArena); break;
    case MenuAction::Format:
        if (menu_.opponent == Opponent::FreeForAll) menu_.opponent = menu_.lastRival;
        else {
            menu_.lastRival = menu_.opponent;
            menu_.opponent = Opponent::FreeForAll;
        }
        break;
    case MenuAction::Rival:
        menu_.opponent = menu_.opponent == Opponent::Hanz ? Opponent::IPwndUrMom : Opponent::Hanz;
        break;
    case MenuAction::Voice:
        menu_.voice = !menu_.voice;
        if (!menu_.voice) audio.stopSpeech();
        break;
    case MenuAction::Volume:
        menu_.volume = std::clamp(menu_.volume + 10 * direction, 0, 100);
        audio.setVolume(menu_.volume);
        break;
    case MenuAction::Render:
#ifdef FOD_LOCAL_DUEL
        if (menu_.splitRenderWidth) {
            menu_.splitRenderWidth = cycleWidth(splitRenderWidths, menu_.splitRenderWidth, direction);
            break;
        }
        if (menu_.desktopRenderWidth) {
            menu_.desktopRenderWidth = cycleWidth(desktopRenderWidths, menu_.desktopRenderWidth, direction);
            break;
        }
#endif
        menu_.compact = !menu_.compact;
        break;
    case MenuAction::Callsign: menu_.editing = true; break;
    default: break;
    }
}

void App::startOrResume(const Input& input) {
    if (!connected_) return;
    menu_.selection = 0;
    menu_.screen = MenuScreen::Home;
    audio.setVolume(menu_.volume);
    audio.pauseSpeech(false);
    if (state().match == Match::Paused) game.resume(input.down(Fire));
    else {
        const auto& snapshot = state();
        const bool retryCheckpoint = menu_.campaign && snapshot.match == Match::Results && snapshot.mission &&
                                     snapshot.mission->phase == MissionPhase::Failed && game.canRetryCheckpoint();
        beginMatch(retryCheckpoint);
    }
    constexpr uint32_t actions = Fire | Reload | Stance | Swap | Streak | Jump | Signal;
    blockedActions_ = input.held & actions;
}

void App::returnToStartMenu(StartItem item) {
    menu_.screen = MenuScreen::Home;
    menu_.selection = int(item);
}

void App::launch(LaunchAction action, const Input& input) {
#ifdef FOD_LOCAL_DUEL
    if (!connected_) return;
#endif
    if (action == LaunchAction::LocalDuel || action == LaunchAction::LocalCampaign || action == LaunchAction::ContinueCoOp) {
        menu_.pendingLaunch = action;
        return;
    }
    menu_.campaign = action != LaunchAction::BotMatch;
    const bool ready = menu_.campaign
        ? loadedArena_ == Arena::BriarHouse && loadedBriarMode_ == BriarHouseMode::Invasion
        : loadedArena_ == menu_.selectedArena && (loadedArena_ == Arena::ShipBox || loadedBriarMode_ == BriarHouseMode::Standard);
    if (!ready) {
        menu_.pendingLaunch = action;
        return;
    }
    if (!connected_) {
        queueLoadedLaunch(action);
        return;
    }
    chapterReplay_ = false;
    if (action == LaunchAction::ContinueCampaign && menu_.progress.available()) {
        menu_.difficulty = menu_.progress.difficulty;
        menu_.screen = MenuScreen::Home;
        menu_.selection = 0;
        audio.setVolume(menu_.volume);
        audio.pauseSpeech(false);
        audio.stopSpeech();
        game.resumeCampaign(seed_++, menu_.progress);
        routeAudio();
        blockedActions_ = input.held;
    } else startOrResume(input);
}

void App::startMenuInput(const Input& input) {
    moveSelection(menu_.selection, input, int(startCards.size()));
    bool touched = false;
    if (input.touchX >= startX && input.touchX < startX + startWidth) {
        for (size_t i = 0; i < startCards.size(); ++i) {
            const auto& card = startCards[i];
            if (input.touchY < card.y || input.touchY >= card.y + card.height) continue;
            menu_.selection = int(i);
            touched = true;
            break;
        }
    }
    if (!touched && !input.edge(Confirm)) return;
    switch (StartItem(menu_.selection)) {
    case StartItem::Campaign:
        menu_.screen = MenuScreen::Campaign;
        break;
    case StartItem::BotMatch: launch(LaunchAction::BotMatch, input); return;
    case StartItem::ChangeMatch: menu_.screen = MenuScreen::BotSetup; break;
#ifdef FOD_LOCAL_DUEL
    case StartItem::LocalPlayers: menu_.screen = MenuScreen::LocalPlayers; break;
#endif
    case StartItem::Settings: menu_.screen = MenuScreen::Settings; break;
    case StartItem::Count: return;
    }
    menu_.selection = 0;
}

void App::leaveScreen() {
    const auto previous = menu_.screen;
    menu_.screen = MenuScreen::Home;
    if (state().match != Match::Start) {
        menu_.selection = menuOptions(menu_, state().match).index(
            previous == MenuScreen::Settings ? MenuAction::Settings : MenuAction::ChangeMatch);
        return;
    }
    switch (previous) {
    case MenuScreen::Settings: returnToStartMenu(StartItem::Settings); break;
    case MenuScreen::BotSetup: returnToStartMenu(StartItem::ChangeMatch); break;
#ifdef FOD_LOCAL_DUEL
    case MenuScreen::CoOpSetup:
        menu_.screen = menu_.coOpFromCampaign ? MenuScreen::Campaign : MenuScreen::LocalPlayers;
        menu_.selection = menuOptions(menu_, Match::Start).index(MenuAction::CoOp);
        break;
    case MenuScreen::VersusSetup:
        menu_.screen = MenuScreen::LocalPlayers;
        menu_.selection = menuOptions(menu_, Match::Start).index(MenuAction::Versus);
        break;
    case MenuScreen::LocalPlayers: returnToStartMenu(StartItem::LocalPlayers); break;
#endif
    default: returnToStartMenu(StartItem::Campaign); break;
    }
}

void App::recordCampaignResult() {
    const auto& s = state();
    if (chapterReplay_ || s.match != Match::Results || !s.mission || !s.mission->campaign ||
        s.mission->phase != MissionPhase::Victory) return;
    menu_.progress = game.nextCampaignProgress();
    menu_.saveFailed = writeProgress_ && !writeProgress_(menu_.progress);
}

void App::callsignInput(const Input& input) {
    const bool finishTouch = input.touchX >= menuX && input.touchX < menuX + menuWidth &&
                             input.touchY >= 374 && input.touchY < 410;
    if (input.edge(Confirm) || input.edge(Back) || input.edge(Pause) || finishTouch) {
        menu_.editing = false;
        return;
    }
    const int size = int(menu_.callsign.size()) - 1;
    auto& name = menu_.callsign;
    const int gridX = input.touchX - keyboardX, gridY = input.touchY - keyboardY;
    if (gridX >= 0 && gridX < keyboardColumns * keyWidth && gridY >= 0 && gridY < 4 * keyHeight) {
        const char key = callsignKeys[gridY / keyHeight * keyboardColumns + gridX / keyWidth];
        const int length = int(std::strlen(name.data()));
        if (key == '<') {
            const int index = std::min(menu_.callsignCursor, length);
            if (index > 0) {
                std::memmove(name.data() + index - 1, name.data() + index, size_t(length - index + 1));
                menu_.callsignCursor = index - 1;
            }
        } else if (menu_.callsignCursor < size) {
            name[menu_.callsignCursor++] = key;
        }
        return;
    }
    if (input.edge(Left)) menu_.callsignCursor = std::max(0, menu_.callsignCursor - 1);
    if (input.edge(Right)) menu_.callsignCursor = std::min({size, menu_.callsignCursor + 1, int(std::strlen(name.data()))});
    if (menu_.callsignCursor < size && (input.edge(Up) || input.edge(Down))) {
        static constexpr char alphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
        const char current = name[menu_.callsignCursor];
        const char* found = current ? std::strchr(alphabet, current) : alphabet;
        const int offset = found ? int(found - alphabet) : 0;
        const int span = int(sizeof(alphabet)) - 1;
        name[menu_.callsignCursor] = alphabet[(offset + span + (input.edge(Up) ? 1 : -1)) % span];
    }
}

void App::menuInput(const Input& input) {
    const Match match = state().match;
    if (menu_.editing) return callsignInput(input);
    if (match == Match::Start && menu_.screen == MenuScreen::Home) return startMenuInput(input);
    if (input.edge(Back) || input.edge(Pause)) {
        if (menu_.screen != MenuScreen::Home) leaveScreen();
        else if (match == Match::Paused) startOrResume(input);
        else if (match == Match::Results) exitMatch();
        return;
    }
    const auto rows = menuOptions(menu_, match);
    moveSelection(menu_.selection, input, rows.count);
    const int touched = touchMenuRow(input, rows.count);
    if (touched >= 0) menu_.selection = touched;
    const auto action = rows.actions[size_t(menu_.selection)];
    if (touched < 0 && !input.edge(Confirm)) {
        const int direction = int(input.edge(Right)) - int(input.edge(Left));
        if (direction && action != MenuAction::Callsign) change(action, direction);
        return;
    }
    switch (action) {
    case MenuAction::Play:
        if (menu_.screen == MenuScreen::VersusSetup) launch(LaunchAction::LocalDuel, input);
        else if (menu_.screen == MenuScreen::BotSetup) launch(LaunchAction::BotMatch, input);
        else startOrResume(input);
        break;
    case MenuAction::Solo: launch(LaunchAction::Campaign, input); break;
    case MenuAction::CoOp:
        menu_.coOpFromCampaign = menu_.screen == MenuScreen::Campaign;
        menu_.screen = MenuScreen::CoOpSetup;
        menu_.selection = 0;
        break;
    case MenuAction::Versus:
        menu_.screen = MenuScreen::VersusSetup;
        menu_.selection = 0;
        break;
    case MenuAction::StartCoOp: launch(LaunchAction::LocalCampaign, input); break;
    case MenuAction::Continue:
        launch(menu_.progress.coOp ? LaunchAction::ContinueCoOp : LaunchAction::ContinueCampaign, input);
        break;
    case MenuAction::ChangeMatch:
        menu_.screen = MenuScreen::BotSetup;
        menu_.selection = 0;
        break;
    case MenuAction::Settings:
        menu_.screen = MenuScreen::Settings;
        menu_.selection = 0;
        break;
    case MenuAction::Back: leaveScreen(); break;
    case MenuAction::Exit: exitMatch(); break;
    default: change(action, touched >= 0 && input.touchX < menuX + menuWidth / 2 ? -1 : 1); break;
    }
}

void App::tick(Input input) {
    if (input.disconnected) {
        if (connected_) {
            connected_ = false;
            pauseMatch();
        }
        blockedActions_ = 0;
        if (input.touchX < 0) return;
    } else {
        connected_ = true;
    }
    if (menu_.loadedLaunch != LaunchAction::None) {
        if (input.edge(Back) || input.edge(Pause) || touchMenuRow(input, 1) == 0) {
            menu_.loadedLaunch = LaunchAction::None;
        } else if (connected_) {
            const auto action = menu_.loadedLaunch;
            menu_.loadedLaunch = LaunchAction::None;
            launch(action, input);
            blockedActions_ = input.held;
        }
        return;
    }
    if (state().match != Match::Playing) {
        menuInput(input);
        return;
    }
    if (input.edge(Pause) || (input.touchX >= 735 && input.touchY >= 0 && input.touchY < 30)) {
        pauseMatch();
        return;
    }
    const uint32_t wasBlocked = blockedActions_;
    blockedActions_ &= input.held;
    input.held &= ~wasBlocked;
    input.pressed &= ~wasBlocked;
    game.step(input);
    routeAudio();
    if (state().match == Match::Results) {
        recordCampaignResult();
        stopAmbientLoops();
        menu_.selection = 0;
    }
}

void App::pauseMatch() {
    if (state().match != Match::Playing) return;
    game.pause();
    audio.pauseSpeech(true);
    menu_.selection = 0;
    stopAmbientLoops();
}

void App::exitMatch() {
    game.quit();
    audio.pauseSpeech(false);
    if (state().match == Match::Start) {
        audio.stopSpeech();
        returnToStartMenu(menu_.campaign ? StartItem::Campaign : StartItem::BotMatch);
    } else {
        menu_.selection = 0;
        routeAudio();
    }
    stopAmbientLoops();
}

} // namespace fod
