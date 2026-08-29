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

struct WorkingRange
{
    double startSeconds { 0.0 };
    double endSeconds { 0.0 };

    bool isValid() const noexcept { return endSeconds > startSeconds; }
    double durationSeconds() const noexcept { return endSeconds - startSeconds; }
};

struct Slot
{
    EngineSynthParams params;
    EngineSynthParams synthParams;
    juce::String synthTimbreFile;
    juce::String timbreFile;
    std::optional<PitchData> pitchData;
    std::optional<SynthAudio> synthAudio;
    std::optional<double> lastSynthElapsedSeconds;
    bool bypass = false;

    bool hasSynthAudio() const noexcept
    {
        return synthAudio.has_value() && synthAudio->isValid();
    }

    bool pitchCoversWorkingRange (const WorkingRange& range) const noexcept
    {
        if (! range.isValid() || ! pitchData.has_value() || pitchData->f0Times.empty())
            return false;
        const double first = static_cast<double> (pitchData->f0Times.front());
        const double last = static_cast<double> (pitchData->f0Times.back());
        constexpr double kFrameSeconds = 0.01;
        constexpr double kEpsilon = 1.0e-3;
        return first <= range.startSeconds + kEpsilon
            && last + kFrameSeconds >= range.endSeconds - kEpsilon;
    }
};

class DeepSvcAudioModification : public juce::ARAAudioModification
{
public:
    DeepSvcAudioModification (juce::ARAAudioSource* audioSource,
                              ARA::ARAAudioModificationHostRef hostRef,
                              const juce::ARAAudioModification* optionalModificationToClone);

    std::array<Slot, 2> slots;
    int activeSlot = 0;
    uint64_t dataRevision { 0 };
    ContentKey contentKey;
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;

    Slot& active() noexcept { return slots[static_cast<size_t> (activeSlot)]; }
    const Slot& active() const noexcept { return slots[static_cast<size_t> (activeSlot)]; }
    Slot& slotAt (int slot) noexcept { return slots[static_cast<size_t> (juce::jlimit (0, 1, slot))]; }
    const Slot& slotAt (int slot) const noexcept
    {
        return slots[static_cast<size_t> (juce::jlimit (0, 1, slot))];
    }

    void bindContentKey();
    WorkingRange workingRange() const;
};

} // namespace deepsvc
