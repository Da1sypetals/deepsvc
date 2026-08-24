#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

// 对应 OpenTune Source/ARA/PlaybackRegion.h
namespace deepsvc
{

// 宿主 PlaybackRegion 的放置快照：DC 在 ARA 通知里刷新，渲染与投影只读快照
struct PlaybackRegion
{
    juce::ARAPlaybackRegion* playbackRegion { nullptr };
    juce::String audioModificationPersistentId;
    double startInPlaybackTime { 0.0 };
    double startInModificationTime { 0.0 };
    double durationInPlaybackTime { 0.0 };
    double durationInModificationTime { 0.0 };
    bool timestretchEnabled { false };
    bool timestretchReflectingTempo { false };
    bool contentBasedFadeAtHead { false };
    bool contentBasedFadeAtTail { false };
    uint64_t placementRevision { 0 };
    std::optional<juce::Colour> displayColour;

    void updateFrom (juce::ARAPlaybackRegion* region);
    void updateDisplayColourFrom (juce::ARAPlaybackRegion* region);
    double endInPlaybackTime() const noexcept;
    bool hasValidPlacement() const noexcept;
};

} // namespace deepsvc
