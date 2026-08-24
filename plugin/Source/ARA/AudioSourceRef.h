#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// 对应 OpenTune Source/ARA/AudioSource.h 的精简版：源身份与形状快照
namespace deepsvc
{

struct AudioSourceRef
{
    juce::ARAAudioSource* audioSource { nullptr };
    juce::String persistentId;
    double sampleRate { 0.0 };
    int numChannels { 0 };
    int64_t numSamples { 0 };

    void updateFrom (juce::ARAAudioSource* source)
    {
        audioSource = source;
        if (source == nullptr)
            return;
        persistentId = juce::String (source->getPersistentID());
        sampleRate = source->getSampleRate();
        numChannels = source->getChannelCount();
        numSamples = source->getSampleCount();
    }

    double durationSeconds() const noexcept
    {
        return sampleRate > 0.0 ? static_cast<double> (numSamples) / sampleRate : 0.0;
    }
};

} // namespace deepsvc
