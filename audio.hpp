#pragma once

#include "types.hpp"
#include "adpcm.hpp"
#include "assets.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace fod {

class Mixer {
public:
    explicit Mixer(const Assets& assets, uint32_t voiceSeed = 0);
    void setVolume(int percent);
    void play(Sound sound, float gain = 1, float pan = 0);
    void playCampaignVoice(CampaignVoiceCue cue);
    void loop(Sound sound, float gain, float pan = 0);
    void stop(Sound sound);
    void stopSpeech();
    void pauseSpeech(bool paused);
    // Exactly one producer (simulation) and one consumer (PCM/DMA thread).
    void mix(int16_t* stereo, size_t frames);
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    Speaker speaking() const { return speaking_.load(std::memory_order_relaxed); }

private:
    struct Command {
        Sound sound = Sound::Mp44;
        float gain = 0, pan = 0;
        bool loop = false;
        uint32_t generation = 0;
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
        const uint8_t* codes = nullptr;
        const int16_t* pcm = nullptr;
        const int16_t* chirp = nullptr;
        size_t cursor = 0, count = 0;
        size_t chirpFrames = 0;
        float gain = 0;
        Speaker speaker = Speaker::None;
        AdpcmState decoder;
        int sample = 0;
        bool campaign = false, halfRate = false;
    };
    void push(Command command);
    void applyEffect(Command command);
    void startSpeech(Command command);
    void startCampaignSpeech(CampaignVoiceCue cue);
    int decodeSpeechNibble(size_t nibble);
    int speechSample();
    uint32_t voiceDraw();

    const Assets& assets_;
    std::array<Command, 128> queue_{};
    std::array<Voice, 24> voices_{};
    std::array<Deck, voiceBankCount> decks_{};
    Speech speech_{};
    size_t speechGapFrames_ = 0;
    uint32_t voiceSeed_;
    uint32_t activeSpeechGeneration_ = 0;
    uint32_t activeCampaignRequest_ = 0;
    int campaignDuckFrames_ = 0;
    int campaignLastSample_ = 0, campaignFadeFrom_ = 0, campaignFadeFrames_ = 0;
    // Stops invalidate older commands even if the effects queue is full.
    std::atomic<uint32_t> speechGeneration_{0};
    // The most recent displayed cue wins, independent of the effects queue.
    std::atomic<uint32_t> campaignRequest_{0};
    std::array<std::atomic<uint32_t>, size_t(Sound::Count)> loopGenerations_;
    std::atomic<bool> speechPaused_{false};
    std::atomic<uint32_t> write_{0}, read_{0}, dropped_{0};
    std::atomic<int> volume_{70};
    std::atomic<Speaker> speaking_{Speaker::None};
};

} // namespace fod
