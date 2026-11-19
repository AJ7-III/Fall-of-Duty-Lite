#include "audio.hpp"
#include "assets.hpp"

#include <algorithm>

namespace fod {
namespace {
constexpr float speechLevel = .90f;
}

Mixer::Mixer(const Assets& assets) : assets_(assets) {}

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

void Mixer::play(Sound sound, float gain, float pan) { push({sound, gain, pan, false}); }
void Mixer::loop(Sound sound, float gain, float pan) { push({sound, gain, pan, true}); }

void Mixer::stopSpeech() { push({Sound::VoiceStart, 0, 0, false, Action::StopSpeech}); }
void Mixer::pauseSpeech(bool paused) {
    push({Sound::VoiceStart, 0, 0, false, paused ? Action::PauseSpeech : Action::ResumeSpeech});
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
            voiceSeed_ ^= voiceSeed_ << 13;
            voiceSeed_ ^= voiceSeed_ >> 17;
            voiceSeed_ ^= voiceSeed_ << 5;
            std::swap(deck.order[i], deck.order[voiceSeed_ % unsigned(i + 1)]);
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

void Mixer::apply(Command command) {
    if (command.action != Action::Sound) {
        if (command.action == Action::StopSpeech) {
            speech_ = {};
            queuedSpeech_.reset();
        } else {
            speechPaused_ = command.action == Action::PauseSpeech;
        }
        return;
    }
    const auto index = static_cast<size_t>(command.sound);
    if (command.sound >= Sound::VoiceStart && command.sound < Sound::Count) {
        if (command.gain <= 0) return;
        if (speech_.cursor == speech_.count) startSpeech(command);
        else if (!queuedSpeech_) queuedSpeech_ = command;
        return;
    }
    if (index >= assets_.audio.size() || assets_.audio[index].samples.empty()) return;
    if (command.loop) {
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
    while (read != write) {
        apply(queue_[read % queue_.size()]);
        ++read;
    }
    read_.store(read, std::memory_order_release);

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
        const float pan = std::clamp(voice.command.pan, -1.0f, 1.0f);
        const float gain = std::clamp(voice.command.gain, 0.0f, 1.5f);
        const auto& samples = assets_.audio[static_cast<size_t>(voice.command.sound)].samples;
        active[activeCount++] = {
            &voice, samples.data(), samples.size(),
            int(gain * (1 - std::max(0.0f, pan)) * volume * 2.56f),
            int(gain * (1 + std::min(0.0f, pan)) * volume * 2.56f),
        };
    }

    int speechGain = int(speech_.gain * volume * 2.56f);
    Speaker audible = Speaker::None;
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
        if (!speechPaused_ && speech_.cursor < speech_.count) {
            const int voiced = speechSample() * speechGain;
            if (speechGain) audible = speech_.speaker;
            sumLeft += voiced;
            sumRight += voiced;
            if (speech_.cursor == speech_.count && queuedSpeech_) {
                startSpeech(*queuedSpeech_);
                queuedSpeech_.reset();
                speechGain = int(speech_.gain * volume * 2.56f);
            }
        }
        stereo[frame * 2] = int16_t(std::clamp(sumLeft / 256, -32768, 32767));
        stereo[frame * 2 + 1] = int16_t(std::clamp(sumRight / 256, -32768, 32767));
    }
    speaking_.store(audible, std::memory_order_relaxed);
}

} // namespace fod
