#include "app.hpp"
#include "assets.hpp"
#include "mission.hpp"

#include <algorithm>
#include <cstring>

namespace fod {

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
    audio.stopSpeech();
    if (asCampaign) game.startMission(seed_++, menu_.difficulty, false);
    else game.start(seed_++, menu_.difficulty, menu_.opponent);
    routeAudio();
}

void App::stopRotor() {
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
    for (size_t i = 0; i < game.eventCount(); ++i) {
        const auto& event = game.events()[i];
        if (event.kind != EventKind::Sound) continue;
        const bool voice = event.sound >= Sound::VoiceStart && event.sound <= Sound::VoiceAlternateVictory;
        if (voice && !menu_.voice) continue;
        if (event.sound == Sound::RadioChirp && !menu_.voice) continue;
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

void App::select(int step) {
    const int count = state().match == Match::Start ? MenuRows - 1 : MenuRows;
    menu_.selection = (menu_.selection + count + step) % count;
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
        audio.stopSpeech();
        if (menu_.campaign) {
            const bool retryCheckpoint = state().match == Match::Results && state().mission &&
                                         state().mission->phase == MissionPhase::Failed && game.canRetryCheckpoint();
            game.startMission(seed_++, menu_.difficulty, retryCheckpoint);
        } else {
            game.start(seed_++, menu_.difficulty, menu_.opponent);
        }
        routeAudio();
    }
    constexpr uint32_t actions = Fire | Reload | Stance | Swap | Streak | Jump;
    blockedActions_ = input.held & actions;
}

void App::menuInput(const Input& input) {
    const bool starting = state().match == Match::Start;
    if (starting && menu_.lanMenu) {
        if (input.edge(Back) || input.edge(Pause)) {
            menu_.lanMenu = false;
            menu_.selection = int(StartItem::MultiplayerLan);
            return;
        }
        if (input.edge(Up)) menu_.selection = (menu_.selection + LanRows - 1) % LanRows;
        if (input.edge(Down)) menu_.selection = (menu_.selection + 1) % LanRows;
        if (menu_.selection == LanArenaRow && (input.edge(Left) || input.edge(Right))) {
            menu_.selectedArena = menu_.selectedArena == Arena::ShipBox ? Arena::BriarHouse : Arena::ShipBox;
        }
        const bool touch = input.touchX >= menuX && input.touchX < menuX + menuWidth &&
                           input.touchY >= menuY && input.touchY < menuY + menuRowHeight * LanRows;
        if (touch) menu_.selection = (input.touchY - menuY) / menuRowHeight;
        if (!touch && !input.edge(Confirm)) return;
        if (menu_.selection == LanStartRow) {
#ifdef FOD_LOCAL_DUEL
            menu_.lanMenu = false;
            menu_.pendingLaunch = LaunchAction::LocalDuel;
#endif
        } else if (menu_.selection == LanArenaRow) {
            menu_.selectedArena = menu_.selectedArena == Arena::ShipBox ? Arena::BriarHouse : Arena::ShipBox;
        } else if (menu_.selection == LanBackRow) {
            menu_.lanMenu = false;
            menu_.selection = int(StartItem::MultiplayerLan);
        }
        return;
    }

    if (starting && !menu_.setup) {
        constexpr int items = int(StartItem::Count);
        if (input.edge(Up)) menu_.selection = (menu_.selection + items - 1) % items;
        if (input.edge(Down)) menu_.selection = (menu_.selection + 1) % items;
        if ((input.edge(Left) || input.edge(Right)) &&
            (menu_.selection == int(StartItem::BotMatch) || menu_.selection == int(StartItem::MultiplayerLan))) {
            menu_.selectedArena = menu_.selectedArena == Arena::ShipBox ? Arena::BriarHouse : Arena::ShipBox;
        }
        const bool inColumn = input.touchX >= startX && input.touchX < startX + startWidth;
        const bool hitCampaign = inColumn && input.touchY >= campaignY && input.touchY < campaignY + campaignHeight;
        const bool hitBotMatch = inColumn && input.touchY >= startY && input.touchY < startY + startHeight;
        const bool hitLan = inColumn && input.touchY >= lanY && input.touchY < lanY + lanHeight;
        const bool hitSetup = inColumn && input.touchY >= setupY && input.touchY < setupY + setupHeight;
        if (hitCampaign) menu_.selection = int(StartItem::Campaign);
        else if (hitBotMatch) menu_.selection = int(StartItem::BotMatch);
        else if (hitLan) menu_.selection = int(StartItem::MultiplayerLan);
        else if (hitSetup) menu_.selection = int(StartItem::MatchSetup);

        const bool activate = hitCampaign || hitBotMatch || hitLan || hitSetup || input.edge(Confirm);
        if (!activate) return;

        if (menu_.selection == int(StartItem::Campaign)) {
            menu_.campaign = true;
            if (loadedArena_ == Arena::BriarHouse && loadedBriarMode_ == BriarHouseMode::Invasion) {
                startOrResume(input);
            } else {
                menu_.pendingLaunch = LaunchAction::Campaign;
            }
        } else if (menu_.selection == int(StartItem::BotMatch)) {
            menu_.campaign = false;
            const bool loadedMatches = loadedArena_ == menu_.selectedArena &&
                                       (loadedArena_ == Arena::ShipBox || loadedBriarMode_ == BriarHouseMode::Standard);
            if (loadedMatches) {
                startOrResume(input);
            } else {
                menu_.pendingLaunch = LaunchAction::BotMatch;
            }
        } else if (menu_.selection == int(StartItem::MultiplayerLan)) {
            menu_.lanMenu = true;
            menu_.selection = LanStartRow;
        } else if (menu_.selection == int(StartItem::MatchSetup)) {
            menu_.setup = true;
            menu_.selection = DifficultyRow;
        }
        return;
    }
    if (menu_.editing) {
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
        return;
    }

    if (starting && (input.edge(Back) || input.edge(Pause))) {
        menu_.setup = false;
        menu_.selection = menu_.campaign ? int(StartItem::Campaign) : int(StartItem::BotMatch);
        return;
    }
    if (input.edge(Up)) select(-1);
    if (input.edge(Down)) select(1);
    if (input.edge(Left)) change(-1);
    if (input.edge(Right)) change(1);

    const bool touch = input.touchX >= menuX && input.touchX < menuX + menuWidth && input.touchY >= menuY &&
                       input.touchY < menuY + menuRowHeight * (starting ? MenuRows - 1 : MenuRows);
    if (touch) menu_.selection = (input.touchY - menuY) / menuRowHeight;
    const bool resume = state().match == Match::Paused && (input.edge(Pause) || input.edge(Back) ||
                        ((touch || input.edge(Confirm)) && menu_.selection == PlayRow));
    const bool confirm = touch || input.edge(Confirm);
    if (!resume && !confirm) return;
    if (starting && menu_.selection == PlayRow) {
        menu_.setup = false;
        menu_.selection = menu_.campaign ? int(StartItem::Campaign) : int(StartItem::BotMatch);
    } else if (resume || menu_.selection == PlayRow) {
        startOrResume(input);
    } else if (menu_.selection == ExitRow) {
        game.quit();
        if (state().match == Match::Start) {
            audio.stopSpeech();
            menu_.selection = menu_.campaign ? int(StartItem::Campaign) : int(StartItem::BotMatch);
        } else {
            menu_.selection = PlayRow;
        }
        audio.pauseSpeech(false);
        routeAudio();
        stopRotor();
    } else {
        change(touch && input.touchX < menuX + menuWidth / 2 ? -1 : 1);
    }
}

void App::tick(Input input) {
    if (input.disconnected) {
        if (connected_) {
            connected_ = false;
            game.pause();
            if (state().match == Match::Paused) {
                audio.pauseSpeech(true);
                menu_.selection = PlayRow;
            }
            stopRotor();
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
        game.pause();
        audio.pauseSpeech(true);
        menu_.selection = 0;
        stopRotor();
        return;
    }
    const uint32_t wasBlocked = blockedActions_;
    blockedActions_ &= input.held;
    input.held &= ~wasBlocked;
    input.pressed &= ~wasBlocked;
    game.step(input);
    routeAudio();
    if (state().match == Match::Results) {
        stopRotor();
        menu_.selection = 0;
    }
}

} // namespace fod
