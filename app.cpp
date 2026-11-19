#include "app.hpp"

#include <algorithm>
#include <cstring>

namespace fod {

App::App(const Assets& assets) : game(assets), audio(assets) {}

void App::stopRotor() {
    if (!rotor_) return;
    audio.stop(Sound::Rotor);
    rotor_ = false;
}

void App::routeAudio() {
    const Snapshot& snapshot = state();
    for (size_t i = 0; i < game.eventCount(); ++i) {
        const auto& event = game.events()[i];
        if (event.kind != EventKind::Sound) continue;
        const bool voice = event.sound >= Sound::VoiceStart && event.sound <= Sound::VoiceAlternateVictory;
        if (voice && !menu_.voice) continue;
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
    if (snapshot.apacheActive) {
        const Vec3 direction = snapshot.apachePosition - snapshot.camera.position;
        const float distance = length(direction);
        const Vec3 ear{std::cos(snapshot.camera.yaw), 0, -std::sin(snapshot.camera.yaw)};
        audio.loop(Sound::Rotor, 0.67f / (1 + distance * 0.055f),
                   dot(ear, direction) / std::max(distance, 0.01f) * 0.75f);
        rotor_ = true;
    } else {
        stopRotor();
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
        if (state().match != Match::Paused) menu_.alternateRival = !menu_.alternateRival;
        break;
    case VoiceRow:
        menu_.voice = !menu_.voice;
        if (!menu_.voice) audio.stopSpeech();
        break;
    case VolumeRow:
        menu_.volume = std::clamp(menu_.volume + 10 * direction, 0, 100);
        audio.setVolume(menu_.volume);
        break;
    case RenderRow: menu_.compact = !menu_.compact; break;
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
        game.start(seed_++, menu_.difficulty, menu_.alternateRival);
        routeAudio();
    }
    constexpr uint32_t actions = Fire | Reload | Stance | Swap | Streak | Jump;
    blockedActions_ = input.held & actions;
}

void App::menuInput(const Input& input) {
    const bool starting = state().match == Match::Start;
    if (starting && !menu_.setup) {
        if (input.edge(Up) || input.edge(Down)) menu_.selection = startSetupSelection - menu_.selection;
        const bool inColumn = input.touchX >= startX && input.touchX < startX + startWidth;
        const bool start = inColumn && input.touchY >= startY && input.touchY < startY + startHeight;
        const bool setup = inColumn && input.touchY >= setupY && input.touchY < setupY + setupHeight;
        if (start || (input.edge(Confirm) && menu_.selection == PlayRow)) {
            startOrResume(input);
        } else if (setup || (input.edge(Confirm) && menu_.selection == startSetupSelection)) {
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
        menu_.selection = PlayRow;
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
    } else if (resume || menu_.selection == PlayRow) {
        startOrResume(input);
    } else if (menu_.selection == ExitRow) {
        game.quit();
        if (state().match == Match::Start) audio.stopSpeech();
        audio.pauseSpeech(false);
        routeAudio();
        menu_.selection = 0;
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
            if (state().match == Match::Paused) audio.pauseSpeech(true);
            stopRotor();
            menu_.selection = PlayRow;
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
