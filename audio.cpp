#include "audio.hpp"
#include "assets.hpp"

#include <algorithm>

namespace fod {
namespace {
constexpr float speechLevel = .90f;
constexpr size_t minSpeechGap = audioHz * 3 / 4;
constexpr size_t maxSpeechGap = audioHz * 3 / 2;
}

Mixer::Mixer(const Assets& assets, uint32_t voiceSeed) : assets_(assets), voiceSeed_(voiceSeed ? voiceSeed : 0x29ea2831u) {
    for (auto& generation : loopGenerations_) generation.store(0, std::memory_order_relaxed);
}

void Mixer::setVolume(int percent) { volume_.store(std::clamp(percent, 0, 100), std::memory_order_relaxed); }

void Mixer::push(Command command) {
    const uint32_t write = write_.load(std::memory_order_relaxed);
    if (write - read_.load(std::memory_order_acquire) == queue_.size()) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    queue_[write % queue_.size()] = command;
    write_.store(write + 1, std::memory_order_release);
}

void Mixer::play(Sound sound, float gain, float pan) {
    const auto generation = sound >= Sound::VoiceStart ? speechGeneration_.load(std::memory_order_relaxed) : 0u;
    push({sound, gain, pan, false, generation});
}

void Mixer::loop(Sound sound, float gain, float pan) {
    if (gain <= 0) {
        stop(sound);
        return;
    }
    const size_t index = size_t(sound);
    if (index >= loopGenerations_.size()) return;
    push({sound, gain, pan, true, loopGenerations_[index].load(std::memory_order_relaxed)});
}

void Mixer::stop(Sound sound) {
    const size_t index = size_t(sound);
    if (index < loopGenerations_.size()) loopGenerations_[index].fetch_add(1, std::memory_order_release);
}

void Mixer::stopSpeech() { speechGeneration_.fetch_add(1, std::memory_order_release); }
void Mixer::pauseSpeech(bool paused) { speechPaused_.store(paused, std::memory_order_release); }

uint32_t Mixer::voiceDraw() {
    voiceSeed_ ^= voiceSeed_ << 13;
    voiceSeed_ ^= voiceSeed_ >> 17;
    voiceSeed_ ^= voiceSeed_ << 5;
    return voiceSeed_;
}

void Mixer::startSpeech(Command command) {
    const auto index = static_cast<size_t>(command.sound);
    const auto bank = index - static_cast<size_t>(Sound::VoiceStart);
    const auto& clips = assets_.voices[bank];
    speech_ = {};
    speech_.gain = std::clamp(command.gain, 0.0f, 1.5f) * speechLevel;
    speech_.speaker = command.sound >= Sound::VoiceAlternateStart ? Speaker::Hanz : Speaker::IPwndUrMom;
    if (clips.empty()) {
        if (index < assets_.audio.size()) {
            const auto& samples = assets_.audio[index].samples;
            speech_.pcm = samples.data();
            speech_.count = samples.size();
        }
        return;
    }
    auto& deck = decks_[bank];
    if (!deck.remaining) {
        deck.remaining = static_cast<uint8_t>(clips.size());
        for (uint8_t i = 0; i < deck.remaining; ++i) deck.order[i] = i;
        for (int i = deck.remaining - 1; i > 0; --i) {
            std::swap(deck.order[i], deck.order[voiceDraw() % unsigned(i + 1)]);
        }
        const auto next = deck.remaining - 1;
        if (next && deck.order[next] == deck.last) std::swap(deck.order[next], deck.order[0]);
    }
    deck.last = deck.order[--deck.remaining];
    const auto& clip = clips[deck.last];
    speech_.clip = &clip;
    speech_.count = clip.frames;
    speech_.decoder = {clip.predictor, clip.index};
}

int Mixer::speechSample() {
    if (speech_.pcm) return speech_.pcm[speech_.cursor++];
    const auto cursor = speech_.cursor++;
    if (!cursor) return speech_.decoder.predictor;
    const auto nibble = cursor - 1;
    const auto code = (speech_.clip->codes[nibble / 2] >> ((nibble & 1) * 4)) & 15;
    return speech_.decoder.decode(code);
}

void Mixer::applyEffect(Command command) {
    const auto index = static_cast<size_t>(command.sound);
    if (index >= assets_.audio.size() || assets_.audio[index].samples.empty()) return;
    if (command.loop) {
        if (command.generation != loopGenerations_[index].load(std::memory_order_acquire)) return;
        for (auto& voice : voices_) {
            if (!voice.active || !voice.command.loop || voice.command.sound != command.sound) continue;
            voice.command = command;
            voice.active = command.gain > 0;
            return;
        }
    }
    if (command.gain <= 0) return;
    auto voice = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.active; });
    if (voice == voices_.end()) {
        voice = std::min_element(voices_.begin(), voices_.end(), [](const Voice& a, const Voice& b) {
            const float priorityA = a.command.gain + (a.command.loop ? 2 : 0);
            const float priorityB = b.command.gain + (b.command.loop ? 2 : 0);
            return priorityA < priorityB;
        });
        if (voice->command.loop) return;
    }
    *voice = {command, 0, true};
}

void Mixer::mix(int16_t* stereo, size_t frames) {
    uint32_t read = read_.load(std::memory_order_relaxed);
    const uint32_t write = write_.load(std::memory_order_acquire);
    const auto speechGeneration = speechGeneration_.load(std::memory_order_acquire);
    if (speechGeneration != activeSpeechGeneration_) {
        speech_ = {};
        speechGapFrames_ = 0;
        activeSpeechGeneration_ = speechGeneration;
    }
    const bool speechPaused = speechPaused_.load(std::memory_order_acquire);
    const bool canSpeak = !speechPaused && speech_.cursor == speech_.count && !speechGapFrames_;
    Command speechCandidate;
    unsigned candidates = 0;
    while (read != write) {
        const auto command = queue_[read++ % queue_.size()];
        if (command.sound < Sound::VoiceStart) {
            applyEffect(command);
        } else if (command.sound < Sound::Count && canSpeak && command.gain > 0 &&
                   command.generation == activeSpeechGeneration_) {
            // Pick one eligible request from this block. Busy requests expire here.
            if (++candidates == 1 || voiceDraw() % candidates == 0) speechCandidate = command;
        }
    }
    read_.store(read, std::memory_order_release);
    if (candidates) startSpeech(speechCandidate);

    struct ActiveVoice {
        Voice* voice;
        const int16_t* samples;
        size_t count;
        int left, right;
    };
    std::array<ActiveVoice, std::tuple_size_v<decltype(voices_)>> active;
    size_t activeCount = 0;
    const int volume = volume_.load(std::memory_order_relaxed);
    for (auto& voice : voices_) {
        if (!voice.active) continue;
        if (voice.command.loop && voice.command.generation !=
            loopGenerations_[size_t(voice.command.sound)].load(std::memory_order_acquire)) {
            voice.active = false;
            continue;
        }
        const float pan = std::clamp(voice.command.pan, -1.0f, 1.0f);
        const float gain = std::clamp(voice.command.gain, 0.0f, 1.5f);
        const auto& samples = assets_.audio[static_cast<size_t>(voice.command.sound)].samples;
        active[activeCount++] = {
            &voice, samples.data(), samples.size(),
            int(gain * (1 - std::max(0.0f, pan)) * volume * 2.56f),
            int(gain * (1 + std::min(0.0f, pan)) * volume * 2.56f),
        };
    }

    const int speechGain = int(speech_.gain * volume * 2.56f);
    const size_t speechFrames = speechPaused ? 0 : std::min(frames, speech_.count - speech_.cursor);
    for (size_t frame = 0; frame < frames; ++frame) {
        int sumLeft = 0, sumRight = 0;
        for (size_t i = 0; i < activeCount;) {
            const auto& mix = active[i];
            auto& voice = *mix.voice;
            const int sample = mix.samples[voice.cursor++];
            sumLeft += sample * mix.left;
            sumRight += sample * mix.right;
            if (voice.cursor == mix.count) {
                voice.cursor = 0;
                if (!voice.command.loop) {
                    voice.active = false;
                    active[i] = active[--activeCount];
                    continue;
                }
            }
            ++i;
        }
        if (frame < speechFrames) {
            const int voiced = speechSample() * speechGain;
            sumLeft += voiced;
            sumRight += voiced;
        }
        stereo[frame * 2] = int16_t(std::clamp(sumLeft / 256, -32768, 32767));
        stereo[frame * 2 + 1] = int16_t(std::clamp(sumRight / 256, -32768, 32767));
    }
    if (!speechPaused) {
        if (speechFrames && speech_.cursor == speech_.count) {
            speechGapFrames_ = minSpeechGap + voiceDraw() % (maxSpeechGap - minSpeechGap + 1);
        }
        speechGapFrames_ -= std::min(speechGapFrames_, frames - speechFrames);
    }
    speaking_.store(speechGain && speechFrames ? speech_.speaker : Speaker::None, std::memory_order_relaxed);
}

} // namespace fod
