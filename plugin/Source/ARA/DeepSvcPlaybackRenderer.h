#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "../Content/ContentKey.h"
#include "../Utils/TimeCoordinate.h"

// 对应 OpenTune Source/ARA/OpenTunePlaybackRenderer.h
namespace deepsvc
{

class DeepSvcDocumentController;

class DeepSvcPlaybackRenderer : public juce::ARAPlaybackRenderer
{
public:
    DeepSvcPlaybackRenderer (ARA::PlugIn::DocumentController* araDocumentController,
                             DeepSvcDocumentController* deepSvcDocumentController);
    ~DeepSvcPlaybackRenderer() override;

    // DC 销毁前先解除引用（渲染器生命周期由宿主掌握，可能晚于 DC）
    void detachDocumentController (DeepSvcDocumentController& owner);

    void prepareToPlay (double sampleRate,
                        int maximumExpectedSamplesPerBlock,
                        int numChannels,
                        juce::AudioProcessor::ProcessingPrecision precision,
                        AlwaysNonRealtime alwaysNonRealtime) override;
    void releaseResources() override;
    bool processBlock (juce::AudioBuffer<float>& buffer,
                       juce::AudioProcessor::Realtime realtime,
                       const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept override;

    using juce::ARAPlaybackRenderer::processBlock;

    void didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;
    void willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept override;

    // 宿主旁通：渲染源音频（docs/ara.md 第 4.3 节）
    void renderSourcePassthrough (juce::AudioBuffer<float>& buffer,
                                  const juce::AudioPlayHead* playhead) noexcept;

    // 消息线程：文档内容变化后调用，重建渲染计划
    void refreshFromDocument();

    // 本实例渲染的音频块（编辑器焦点解析用）
    std::vector<juce::ARAPlaybackRegion*> assignedPlaybackRegions() const;

private:
    struct RenderItem
    {
        ContentKey contentKey;
        std::shared_ptr<const std::vector<float>> audio;
        double synthStartTime = 0.0;
        double synthEndTime = 0.0;
        std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;
        double timelineStartSeconds = 0.0;
        double timelineDurationSeconds = 0.0;
        double contentStartSeconds = 0.0;
        double contentDurationSeconds = 0.0;
    };

    struct RenderPlan
    {
        std::vector<RenderItem> items;
    };

    // 对应 OpenTunePlaybackRenderer::RenderOutputMode
    enum class RenderOutputMode
    {
        passthrough,
        rendering
    };

    std::shared_ptr<const RenderPlan> loadRenderPlan() const noexcept;
    void storeRenderPlan (const std::shared_ptr<const RenderPlan>& plan) noexcept;
    std::shared_ptr<const RenderPlan> buildRenderPlan() const;

    // 把计划内与 [blockStartSeconds, +numSamples/hostSampleRate) 重叠的项渲染进 output；
    // useSourceAudio=true 渲染源音频（宿主旁通），false 渲染合成音频。返回是否有任何写入
    bool renderItems (const RenderPlan& plan,
                      juce::AudioBuffer<float>& output,
                      double blockStartSeconds,
                      int numSamples,
                      bool useSourceAudio) noexcept;

    DeepSvcDocumentController* documentController = nullptr;

    double hostSampleRate = 44100.0;
    juce::AudioBuffer<float> renderBuffer;

    // 经 std::atomic_load_explicit / std::atomic_store_explicit 访问
    std::shared_ptr<const RenderPlan> renderPlan;

    // 直通 ↔ 渲染模式切换时的交叉淡化（仅音频线程访问），
    // 对应 OpenTunePlaybackRenderer 的 crossfadeTotal_/crossfadeRemaining_/outputMode_
    RenderOutputMode outputMode = RenderOutputMode::passthrough;
    int crossfadeTotal = 0;
    int crossfadeRemaining = 0;

    JUCE_DECLARE_NON_COPYABLE (DeepSvcPlaybackRenderer)
};

} // namespace deepsvc
