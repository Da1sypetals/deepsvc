#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "../Content/ContentKey.h"
#include "../Utils/ContentTimelineProjection.h"
#include "../Utils/TimeCoordinate.h"

// 对应 OpenTune Source/ARA/OpenTunePlaybackRenderer.h
namespace deepsvc
{

struct RenderBlockSpan
{
    int destinationStartSample { 0 };
    int samplesToCopy { 0 };
    double overlapStartSeconds { 0.0 };
};

inline std::optional<RenderBlockSpan> computeRegionBlockRenderSpan (double blockStartSeconds,
                                                                    int blockSamples,
                                                                    double hostSampleRate,
                                                                    double playbackStartSeconds,
                                                                    double playbackEndSeconds) noexcept
{
    if (blockSamples <= 0 || hostSampleRate <= 0.0)
        return std::nullopt;

    const double blockEndSeconds = blockStartSeconds
        + (static_cast<double> (blockSamples) / hostSampleRate);
    const double overlapStartSeconds = juce::jmax (blockStartSeconds, playbackStartSeconds);
    const double overlapEndSeconds = juce::jmin (blockEndSeconds, playbackEndSeconds);
    if (! (overlapEndSeconds > overlapStartSeconds))
        return std::nullopt;

    const int destinationStartSample = juce::jlimit (
        0, blockSamples,
        static_cast<int> (TimeCoordinate::secondsToSamplesFloor (overlapStartSeconds - blockStartSeconds,
                                                                 hostSampleRate)));
    const int destinationEndSample = juce::jlimit (
        destinationStartSample, blockSamples,
        static_cast<int> (TimeCoordinate::secondsToSamplesCeil (overlapEndSeconds - blockStartSeconds,
                                                                hostSampleRate)));

    RenderBlockSpan span;
    span.destinationStartSample = destinationStartSample;
    span.samplesToCopy = destinationEndSample - destinationStartSample;
    span.overlapStartSeconds = overlapStartSeconds;
    return span.samplesToCopy > 0 ? std::optional<RenderBlockSpan> (span) : std::nullopt;
}

// 渲染门控：非实时总是渲染；实时只在播放时渲染。停止或无内容时直通宿主输入，
// 模式切换经交叉淡入避免爆音。
bool shouldRenderAraPlaybackBlock (juce::AudioProcessor::Realtime realtime,
                                   bool rendererIsPlaying) noexcept;

class DeepSvcDocumentController;

class DeepSvcPlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    DeepSvcPlaybackRenderer (ARA::PlugIn::DocumentController* araDc,
                             DeepSvcDocumentController* docController);
    ~DeepSvcPlaybackRenderer() override;

    struct PlaybackRegionRenderItem
    {
        ContentTimelineProjection projection;
        std::shared_ptr<const std::vector<float>> renderedAudio;  // 44.1kHz 单声道
        double contentDurationSeconds { 0.0 };

        double endInPlaybackTime() const noexcept
        {
            return projection.timelineStartSeconds + projection.timelineDurationSeconds;
        }
    };

    struct RenderPlan
    {
        std::vector<juce::ARAPlaybackRegion*> playbackRegions;
        std::vector<PlaybackRegionRenderItem> items;
        bool abBypass { false };
    };

    // 由 DC 在析构前调用：先发布空渲染计划，再解除对 DC 的引用
    void detachDocumentController (DeepSvcDocumentController& owner);

    // 宿主分配给本实例渲染的音频块（Event FX 语义：实例所在的音频事件）
    std::vector<juce::ARAPlaybackRegion*> assignedPlaybackRegions() const;

    void refreshRenderPlanFromDocument();

    void prepareToPlay (double sampleRate,
                        int maximumSamplesPerBlock,
                        int numChannels,
                        juce::AudioProcessor::ProcessingPrecision precision,
                        AlwaysNonRealtime alwaysNonRealtime) override;
    void releaseResources() override;

    bool processBlock (juce::AudioBuffer<float>& buffer,
                       juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

protected:
    void didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;
    void willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;

private:
    double hostSampleRate = 44100.0;
    int numChannels = 2;
    int maximumSamplesPerBlock = 512;
    juce::AudioBuffer<float> renderBuffer;

    enum class RenderOutputMode { passthrough, rendering };
    RenderOutputMode outputMode = RenderOutputMode::passthrough;
    int crossfadeRemaining = 0;
    int crossfadeTotal = 0;

    DeepSvcDocumentController* documentController = nullptr;

    std::shared_ptr<const RenderPlan> currentPlan { std::make_shared<RenderPlan>() };

    std::shared_ptr<const RenderPlan> buildRenderPlan (
        std::vector<juce::ARAPlaybackRegion*> playbackRegions) const;
    void publishRenderPlanFor (std::vector<juce::ARAPlaybackRegion*> playbackRegions);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeepSvcPlaybackRenderer)
};

} // namespace deepsvc
