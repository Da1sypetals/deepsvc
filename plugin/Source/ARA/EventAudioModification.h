#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

#include "../Content/ContentKey.h"
#include "../Engine/EngineBridge.h"

namespace deepsvc
{

struct PitchData
{
    std::vector<float> f0Times;
    std::vector<float> f0Values;
};

struct SynthAudio
{
    std::shared_ptr<const std::vector<float>> samples;
    double synthStartTime { 0.0 };
    double synthEndTime { 0.0 };

    bool isValid() const noexcept
    {
        return samples != nullptr && ! samples->empty() && synthEndTime > synthStartTime;
    }

    bool covers (double fileTime) const noexcept
    {
        return isValid() && fileTime >= synthStartTime && fileTime < synthEndTime;
    }
};

struct EventSlot
{
    EngineSynthParams params;
    EngineSynthParams synthParams;
    juce::String synthTimbreFile;
    juce::String timbreFile;
    std::optional<PitchData> pitchData;
    std::optional<SynthAudio> synthAudio;
    bool bypass = false;

    bool hasSynthAudio() const noexcept
    {
        return synthAudio.has_value() && synthAudio->isValid();
    }
};

struct FileRange
{
    double startSeconds { 0.0 };
    double endSeconds { 0.0 };

    bool isValid() const noexcept { return endSeconds > startSeconds; }
    double durationSeconds() const noexcept { return endSeconds - startSeconds; }
};

class EventAudioModification : public juce::ARAAudioModification
{
public:
    EventAudioModification (juce::ARAAudioSource* audioSource,
                            ARA::ARAAudioModificationHostRef hostRef,
                            const juce::ARAAudioModification* optionalModificationToClone);

    std::array<EventSlot, 2> slots;
    int activeSlot = 0;
    uint64_t dataRevision { 0 };
    ContentKey contentKey;
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;

    EventSlot& active() noexcept { return slots[static_cast<size_t> (activeSlot)]; }
    const EventSlot& active() const noexcept { return slots[static_cast<size_t> (activeSlot)]; }
    EventSlot& slotAt (int slot) noexcept { return slots[static_cast<size_t> (juce::jlimit (0, 1, slot))]; }
    const EventSlot& slotAt (int slot) const noexcept
    {
        return slots[static_cast<size_t> (juce::jlimit (0, 1, slot))];
    }

    void bindContentKey();
    FileRange windowUnion() const;
};

} // namespace deepsvc
