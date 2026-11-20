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

} // namespace

App::App(const Assets& assets, uint32_t voiceSeed)
    : game(assets, voiceSeed), audio(assets, voiceSeed ? voiceSeed ^ 0x29ea2831u : 0u),
      loadedArena_(assets.arena), loadedBriarMode_(assets.briarHouseMode) {
    menu_.campaign = assets.arena == Arena::BriarHouse && assets.briarHouseMode == BriarHouseMode::Invasion;
    menu_.selectedArena = assets.arena;
    menu_.selection = menu_.campaign ? int(StartItem::Campaign) : int(StartItem::BotMatch);
}

void App::startImmediate(bool asCampaign) {
    menu_.campaign = asCampaign;
    menu_.setup = false;
    menu_.lanMenu = false;
    menu_.pendingLaunch = LaunchAction::None;
    menu_.selection = PlayRow;
    audio.setVolume(menu_.volume);
    audio.pauseSpeech(false);
    beginMatch(false);
}

void App::beginMatch(bool retryCheckpoint) {
    audio.stopSpeech();
    const auto seed = seed_++;
    if (menu_.campaign) {
        if (retryCheckpoint) game.startMission(seed, menu_.difficulty, true);
        else if (!game.advanceCampaign(seed, menu_.difficulty)) game.startCampaign(seed, menu_.difficulty);
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
            const Vec3 direction = event.position - snapshot.camera.position;
            const float distance = length(direction);
            const Vec3 ear{std::cos(snapshot.camera.yaw), 0, -std::sin(snapshot.camera.yaw)};
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
        audio.loop(Sound::InvasionHum, 0.16f, 0.f);
        invasionHum_ = true;
    } else if (invasionHum_) {
        audio.stop(Sound::InvasionHum);
        invasionHum_ = false;
    }
}

void App::change(int direction) {
    switch (menu_.selection) {
    case DifficultyRow:
        menu_.difficulty = std::clamp(menu_.difficulty + direction, 1, 10);
        game.setDifficulty(menu_.difficulty);
        break;
    case RivalRow:
        if (!(menu_.campaign && state().match != Match::Start) && state().match != Match::Paused) {
            constexpr int count = int(Opponent::FreeForAll) + 1;
            menu_.opponent = Opponent((int(menu_.opponent) + count + direction) % count);
        }
        break;
    case VoiceRow:
        menu_.voice = !menu_.voice;
        if (!menu_.voice) audio.stopSpeech();
        break;
    case VolumeRow:
        menu_.volume = std::clamp(menu_.volume + 10 * direction, 0, 100);
        audio.setVolume(menu_.volume);
        break;
    case RenderRow:
#ifdef FOD_LOCAL_DUEL
        if (menu_.desktopRenderWidth) {
            constexpr int count = int(desktopRenderWidths.size());
            const auto current = std::find(desktopRenderWidths.begin(), desktopRenderWidths.end(), menu_.desktopRenderWidth);
            const int index = int(current - desktopRenderWidths.begin());
            menu_.desktopRenderWidth = desktopRenderWidths[(index + count + direction) % count];
            break;
        }
#endif
        menu_.compact = !menu_.compact;
        break;
    case CallsignRow: menu_.editing = true; break;
    default: break;
    }
}

void App::startOrResume(const Input& input) {
    if (!connected_) return;
    menu_.selection = PlayRow;
    menu_.setup = false;
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

void App::returnToStartMenu() {
    menu_.setup = false;
    menu_.selection = int(menu_.campaign ? StartItem::Campaign : StartItem::BotMatch);
}

void App::startMenuInput(const Input& input) {
    moveSelection(menu_.selection, input, int(startCards.size()));
    const auto selected = StartItem(menu_.selection);
    if ((input.edge(Left) || input.edge(Right)) &&
        (selected == StartItem::BotMatch || selected == StartItem::MultiplayerLan)) {
        toggleArena(menu_.selectedArena);
    }
    int touch = -1;
    if (input.touchX >= startX && input.touchX < startX + startWidth) {
        for (size_t i = 0; i < startCards.size(); ++i) {
            const auto& card = startCards[i];
            if (input.touchY >= card.y && input.touchY < card.y + card.height) {
                touch = int(i);
                break;
            }
        }
    }
    if (touch >= 0) menu_.selection = touch;
    if (touch < 0 && !input.edge(Confirm)) return;

    switch (StartItem(menu_.selection)) {
    case StartItem::Campaign:
        menu_.campaign = true;
        if (loadedArena_ == Arena::BriarHouse && loadedBriarMode_ == BriarHouseMode::Invasion) startOrResume(input);
        else menu_.pendingLaunch = LaunchAction::Campaign;
        break;
    case StartItem::BotMatch:
        menu_.campaign = false;
        if (loadedArena_ == menu_.selectedArena &&
            (loadedArena_ == Arena::ShipBox || loadedBriarMode_ == BriarHouseMode::Standard)) startOrResume(input);
        else menu_.pendingLaunch = LaunchAction::BotMatch;
        break;
    case StartItem::MultiplayerLan:
        menu_.lanMenu = true;
        menu_.selection = LanStartRow;
        break;
    case StartItem::MatchSetup:
        menu_.setup = true;
        menu_.selection = DifficultyRow;
        break;
    case StartItem::Count:
        break;
    }
}

void App::lanMenuInput(const Input& input) {
    const auto close = [&] {
        menu_.lanMenu = false;
        menu_.selection = int(StartItem::MultiplayerLan);
    };
    if (input.edge(Back) || input.edge(Pause)) {
        close();
        return;
    }
    moveSelection(menu_.selection, input, LanRows);
    if (menu_.selection == LanArenaRow && (input.edge(Left) || input.edge(Right))) toggleArena(menu_.selectedArena);
    const int touch = touchMenuRow(input, LanRows);
    if (touch >= 0) menu_.selection = touch;
    if (touch < 0 && !input.edge(Confirm)) return;

    if (menu_.selection == LanStartRow) {
#ifdef FOD_LOCAL_DUEL
        menu_.lanMenu = false;
        menu_.pendingLaunch = LaunchAction::LocalDuel;
#endif
    } else if (menu_.selection == LanArenaRow) {
        toggleArena(menu_.selectedArena);
    } else if (menu_.selection == LanBackRow) {
        close();
    }
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
    const bool starting = state().match == Match::Start;
    if (starting && menu_.lanMenu) return lanMenuInput(input);
    if (starting && !menu_.setup) return startMenuInput(input);
    if (menu_.editing) return callsignInput(input);

    if (starting && (input.edge(Back) || input.edge(Pause))) {
        returnToStartMenu();
        return;
    }
    const int rows = starting ? MenuRows - 1 : MenuRows;
    moveSelection(menu_.selection, input, rows);
    if (input.edge(Left)) change(-1);
    if (input.edge(Right)) change(1);

    const int touchedRow = touchMenuRow(input, rows);
    const bool touch = touchedRow >= 0;
    if (touch) menu_.selection = touchedRow;
    const bool resume = state().match == Match::Paused && (input.edge(Pause) || input.edge(Back) ||
                        ((touch || input.edge(Confirm)) && menu_.selection == PlayRow));
    const bool confirm = touch || input.edge(Confirm);
    if (!resume && !confirm) return;
    if (starting && menu_.selection == PlayRow) {
        returnToStartMenu();
    } else if (resume || menu_.selection == PlayRow) {
        startOrResume(input);
    } else if (menu_.selection == ExitRow) {
        game.quit();
        if (state().match == Match::Start) {
            audio.stopSpeech();
            returnToStartMenu();
        } else {
            menu_.selection = PlayRow;
        }
        audio.pauseSpeech(false);
        routeAudio();
        stopAmbientLoops();
    } else {
        change(touch && input.touchX < menuX + menuWidth / 2 ? -1 : 1);
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
        stopAmbientLoops();
        menu_.selection = PlayRow;
    }
}

void App::pauseMatch() {
    game.pause();
    if (state().match == Match::Paused) {
        audio.pauseSpeech(true);
        menu_.selection = PlayRow;
    }
    stopAmbientLoops();
}

} // namespace fod
