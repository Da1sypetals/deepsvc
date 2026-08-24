#include "DeepSvcPlaybackRenderer.h"

#include "DeepSvcDocumentController.h"
#include "../DebugLog.h"

#include <algorithm>

// 对应 OpenTune Source/ARA/OpenTunePlaybackRenderer.cpp
namespace deepsvc
{

bool shouldRenderAraPlaybackBlock (juce::AudioProcessor::Realtime realtime,
                                   bool rendererIsPlaying) noexcept
{
    if (realtime != juce::AudioProcessor::Realtime::yes)
        return true;

    return rendererIsPlaying;
}

namespace
{

juce::ARAPlaybackRegion* toJucePlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    return static_cast<juce::ARAPlaybackRegion*> (playbackRegion);
}

// 从 44.1kHz 单声道合成音频读取 content 时间处的样本（线性插值）
float readRenderedSample (const std::vector<float>& audio, double contentSeconds) noexcept
{
    const double position = contentSeconds * TimeCoordinate::kRenderSampleRate;
    if (position < 0.0 || audio.empty())
        return 0.0f;

    const auto index = static_cast<size_t> (position);
    if (index + 1 >= audio.size())
        return index < audio.size() ? audio[index] : 0.0f;

    const float fraction = static_cast<float> (position - static_cast<double> (index));
    return audio[index] * (1.0f - fraction) + audio[index + 1] * fraction;
}

} // namespace

DeepSvcPlaybackRenderer::DeepSvcPlaybackRenderer (ARA::PlugIn::DocumentController* araDc,
                                                  DeepSvcDocumentController* docController)
    : juce::ARAPlaybackRenderer (araDc)
    , documentController (docController)
{
}

DeepSvcPlaybackRenderer::~DeepSvcPlaybackRenderer()
{
    if (documentController != nullptr)
        documentController->unregisterPlaybackRenderer (*this);
}

void DeepSvcPlaybackRenderer::detachDocumentController (DeepSvcDocumentController& owner)
{
    if (documentController == &owner)
    {
        std::atomic_store_explicit (&currentPlan,
                                    std::shared_ptr<const RenderPlan> (std::make_shared<RenderPlan>()),
                                    std::memory_order_release);
        documentController = nullptr;
    }
}

std::vector<juce::ARAPlaybackRegion*> DeepSvcPlaybackRenderer::assignedPlaybackRegions() const
{
    if (const auto plan = std::atomic_load_explicit (&currentPlan, std::memory_order_acquire))
        return plan->playbackRegions;
    return {};
}

void DeepSvcPlaybackRenderer::didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion (playbackRegion);
    auto plan = std::atomic_load_explicit (&currentPlan, std::memory_order_acquire);
    auto regions = plan != nullptr ? plan->playbackRegions : std::vector<juce::ARAPlaybackRegion*> {};
    if (jucePlaybackRegion != nullptr
        && std::find (regions.begin(), regions.end(), jucePlaybackRegion) == regions.end())
        regions.push_back (jucePlaybackRegion);

    debugLog ("renderer didAddPlaybackRegion r=" + juce::String::toHexString (reinterpret_cast<int64_t> (this))
              + " total=" + juce::String (regions.size()));
    publishRenderPlanFor (std::move (regions));
}

void DeepSvcPlaybackRenderer::willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion* playbackRegion) noexcept
{
    auto* jucePlaybackRegion = toJucePlaybackRegion (playbackRegion);
    auto plan = std::atomic_load_explicit (&currentPlan, std::memory_order_acquire);
    auto regions = plan != nullptr ? plan->playbackRegions : std::vector<juce::ARAPlaybackRegion*> {};
    regions.erase (std::remove (regions.begin(), regions.end(), jucePlaybackRegion), regions.end());
    debugLog ("renderer willRemovePlaybackRegion total=" + juce::String (regions.size()));
    publishRenderPlanFor (std::move (regions));
}

void DeepSvcPlaybackRenderer::refreshRenderPlanFromDocument()
{
    auto plan = std::atomic_load_explicit (&currentPlan, std::memory_order_acquire);
    publishRenderPlanFor (plan != nullptr ? plan->playbackRegions
                                          : std::vector<juce::ARAPlaybackRegion*> {});
}

std::shared_ptr<const DeepSvcPlaybackRenderer::RenderPlan>
DeepSvcPlaybackRenderer::buildRenderPlan (std::vector<juce::ARAPlaybackRegion*> regions) const
{
    auto nextPlan = std::make_shared<RenderPlan>();
    nextPlan->playbackRegions = std::move (regions);
    if (documentController == nullptr)
        return nextPlan;

    nextPlan->abBypass = documentController->isAbBypass();

    const auto projections = documentController->getPlaybackRegionProjectionsFor (nextPlan->playbackRegions);
    nextPlan->items.reserve (projections.size());

    for (const auto& projection : projections)
    {
        if (! projection.isPlaybackRenderable())
            continue;

        const auto* content = documentController->findContent (projection.contentKey);
        if (content == nullptr || content->renderedAudio == nullptr)
            continue;

        PlaybackRegionRenderItem item;
        item.projection = projection.toTimelineProjection();
        item.renderedAudio = content->renderedAudio;
        item.contentDurationSeconds = projection.contentDurationSeconds;
        nextPlan->items.push_back (std::move (item));
    }

    return nextPlan;
}

void DeepSvcPlaybackRenderer::publishRenderPlanFor (std::vector<juce::ARAPlaybackRegion*> regions)
{
    std::atomic_store_explicit (&currentPlan, buildRenderPlan (std::move (regions)),
                                std::memory_order_release);
}

void DeepSvcPlaybackRenderer::prepareToPlay (double sampleRate,
                                             int maximumSamplesPerBlockCount,
                                             int numChannelsCount,
                                             juce::AudioProcessor::ProcessingPrecision precision,
                                             AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ignoreUnused (precision, alwaysNonRealtime);

    hostSampleRate = sampleRate;
    numChannels = numChannelsCount;
    maximumSamplesPerBlock = maximumSamplesPerBlockCount;
    renderBuffer.setSize (juce::jmax (1, numChannels),
                          juce::jmax (1, maximumSamplesPerBlock),
                          false, true, true);

    // 直通与渲染输出之间的交叉淡入窗口：10ms
    crossfadeTotal = juce::jlimit (128, 2048, static_cast<int> (sampleRate * 0.01));
    crossfadeRemaining = 0;
    outputMode = RenderOutputMode::passthrough;
}

void DeepSvcPlaybackRenderer::releaseResources()
{
    renderBuffer.setSize (0, 0);
}

bool DeepSvcPlaybackRenderer::processBlock (juce::AudioBuffer<float>& buffer,
                                            juce::AudioProcessor::Realtime realtime,
                                            const juce::AudioPlayHead::PositionInfo& positionInfo) noexcept
{
    const int numSamples = buffer.getNumSamples();

    const auto plan = std::atomic_load_explicit (&currentPlan, std::memory_order_acquire);
    const auto positionTime = positionInfo.getTimeInSeconds();

    const bool hasRenderContent = plan != nullptr && ! plan->items.empty()
        && ! plan->abBypass && positionTime.hasValue();
    const bool wantRender = hasRenderContent
        && shouldRenderAraPlaybackBlock (realtime, positionInfo.getIsPlaying());

    if (wantRender != (outputMode == RenderOutputMode::rendering))
    {
        outputMode = wantRender ? RenderOutputMode::rendering : RenderOutputMode::passthrough;
        crossfadeRemaining = crossfadeTotal;
    }

    const bool inTransition = crossfadeRemaining > 0;

    if (outputMode == RenderOutputMode::passthrough && ! inTransition)
        return true;

    if (outputMode == RenderOutputMode::rendering || inTransition)
    {
        renderBuffer.clear();
        if (plan != nullptr && positionTime.hasValue())
        {
            const double blockStartSeconds = *positionTime;
            for (const auto& region : plan->items)
            {
                const auto overlap = computeRegionBlockRenderSpan (blockStartSeconds,
                                                                   numSamples,
                                                                   hostSampleRate,
                                                                   region.projection.timelineStartSeconds,
                                                                   region.endInPlaybackTime());
                if (! overlap.has_value() || region.renderedAudio == nullptr)
                    continue;

                const auto& audio = *region.renderedAudio;
                for (int sample = 0; sample < overlap->samplesToCopy; ++sample)
                {
                    const double playbackTime = overlap->overlapStartSeconds
                        + static_cast<double> (sample) / hostSampleRate;
                    const double contentTime = region.projection.projectTimelineTimeToContent (playbackTime);
                    const float value = readRenderedSample (audio, contentTime);

                    const int destination = overlap->destinationStartSample + sample;
                    const int channels = juce::jmin (renderBuffer.getNumChannels(), buffer.getNumChannels());
                    for (int channel = 0; channel < channels; ++channel)
                        renderBuffer.getWritePointer (channel)[destination] += value;
                }
            }
        }
    }

    if (inTransition)
    {
        // 淡出方向需要渲染信号参与交叉。宿主停止后若不再提供有效时间，
        // 渲染内容不可得，此时宿主输入通常已为静音，直接直通输入。
        if (outputMode == RenderOutputMode::passthrough && ! positionTime.hasValue())
        {
            crossfadeRemaining = 0;
            return true;
        }

        const int fadeSamples = juce::jmin (crossfadeRemaining, numSamples);
        const int elapsedBase = crossfadeTotal - crossfadeRemaining;
        const bool fadingIntoRender = (outputMode == RenderOutputMode::rendering);
        const int channels = juce::jmin (buffer.getNumChannels(), renderBuffer.getNumChannels());
        for (int channel = 0; channel < channels; ++channel)
        {
            auto* out = buffer.getWritePointer (channel);
            const auto* in = buffer.getReadPointer (channel);
            const auto* render = renderBuffer.getReadPointer (channel);
            for (int sample = 0; sample < fadeSamples; ++sample)
            {
                const int windowFrame = elapsedBase + sample;
                float weight = static_cast<float> (windowFrame) / static_cast<float> (crossfadeTotal);
                if (windowFrame + 1 == crossfadeTotal)
                    weight = 1.0f;
                const float renderWeight = fadingIntoRender ? weight : (1.0f - weight);
                out[sample] = in[sample] * (1.0f - renderWeight) + render[sample] * renderWeight;
            }
        }
        crossfadeRemaining -= fadeSamples;

        if (fadeSamples < numSamples && outputMode == RenderOutputMode::rendering)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                auto* out = buffer.getWritePointer (channel, fadeSamples);
                const auto* render = renderBuffer.getReadPointer (channel, fadeSamples);
                juce::FloatVectorOperations::copy (out, render, numSamples - fadeSamples);
            }
        }
        return true;
    }

    // 渲染模式：用渲染内容替换宿主输入（ARA 2 回放渲染语义）
    buffer.clear();
    const int channels = juce::jmin (buffer.getNumChannels(), renderBuffer.getNumChannels());
    for (int channel = 0; channel < channels; ++channel)
        juce::FloatVectorOperations::copy (buffer.getWritePointer (channel),
                                           renderBuffer.getReadPointer (channel),
                                           numSamples);
    return true;
}

} // namespace deepsvc
