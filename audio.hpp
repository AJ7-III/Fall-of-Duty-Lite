#pragma once

#include "types.hpp"
#include "adpcm.hpp"
#include "assets.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

namespace fod {

class Mixer {
public:
    explicit Mixer(const Assets& assets);
    void setVolume(int percent);
    void play(Sound sound, float gain = 1, float pan = 0);
    void loop(Sound sound, float gain, float pan = 0);
    void stop(Sound sound) { loop(sound, 0); }
    void stopSpeech();
    void pauseSpeech(bool paused);
    // Exactly one producer (simulation) and one consumer (PCM/DMA thread).
    void mix(int16_t* stereo, size_t frames);
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    Speaker speaking() const { return speaking_.load(std::memory_order_relaxed); }

private:
    enum class Action : uint8_t { Sound, StopSpeech, PauseSpeech, ResumeSpeech };
    struct Command {
        Sound sound = Sound::Mp44;
        float gain = 0, pan = 0;
        bool loop = false;
        Action action = Action::Sound;
    };
    struct Voice {
        Command command{};
        size_t cursor = 0;
        bool active = false;
    };
    struct Deck {
        std::array<uint8_t, maxVoiceVariants> order{};
        uint8_t remaining = 0, last = 255;
    };
    struct Speech {
        const EncodedVoice* clip = nullptr;
        const int16_t* pcm = nullptr;
        size_t cursor = 0, count = 0;
        float gain = 0;
        Speaker speaker = Speaker::None;
        AdpcmState decoder;
    };
    void push(Command command);
    void apply(Command command);
    void startSpeech(Command command);
    int speechSample();

    const Assets& assets_;
    std::array<Command, 128> queue_{};
    std::array<Voice, 24> voices_{};
    std::array<Deck, voiceBankCount> decks_{};
    Speech speech_{};
    std::optional<Command> queuedSpeech_;
    uint32_t voiceSeed_ = 0x29ea2831;
    bool speechPaused_ = false;
    std::atomic<uint32_t> write_{0}, read_{0}, dropped_{0};
    std::atomic<int> volume_{70};
    std::atomic<Speaker> speaking_{Speaker::None};
};

} // namespace fod
