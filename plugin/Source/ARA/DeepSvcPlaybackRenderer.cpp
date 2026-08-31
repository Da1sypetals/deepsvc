#include "DeepSvcPlaybackRenderer.h"

#include "DeepSvcDocumentController.h"

#include <algorithm>

// 对应 OpenTune Source/ARA/OpenTunePlaybackRenderer.cpp
namespace deepsvc
{

namespace
{

bool shouldRenderAraPlaybackBlock (juce::AudioProcessor::Realtime realtime,
                                   bool rendererIsPlaying) noexcept
{
    if (realtime != juce::AudioProcessor::Realtime::yes)
        return true;

    return rendererIsPlaying;
}

// 从 44.1kHz 单声道源音频读取 content 时间处的样本（线性插值）
float readContentSample (const float* data, size_t numSamples, double contentSeconds) noexcept
{
    const double position = contentSeconds * TimeCoordinate::kRenderSampleRate;
    if (position < 0.0 || numSamples == 0)
        return 0.0f;

    const auto index = static_cast<size_t> (position);
    if (index + 1 >= numSamples)
        return index < numSamples ? data[index] : 0.0f;

    const float fraction = static_cast<float> (position - static_cast<double> (index));
    return data[index] * (1.0f - fraction) + data[index + 1] * fraction;
}

float readPlaybackSample (const float* data,
                          size_t numSamples,
                          double contentSeconds,
                          double sampleRate) noexcept
{
    if (contentSeconds < 0.0 || numSamples == 0 || sampleRate <= 0.0)
        return 0.0f;
    const auto index = static_cast<size_t> (contentSeconds * sampleRate);
    return index < numSamples ? data[index] : 0.0f;
}

} // namespace

DeepSvcPlaybackRenderer::DeepSvcPlaybackRenderer (ARA::PlugIn::DocumentController* araDocumentController,
                                                  DeepSvcDocumentController* deepSvcDocumentControllerRef)
    : juce::ARAPlaybackRenderer (araDocumentController)
    , documentController (deepSvcDocumentControllerRef)
{
}

DeepSvcPlaybackRenderer::~DeepSvcPlaybackRenderer()
{
    if (documentController != nullptr)
        documentController->unregisterPlaybackRenderer (*this);
    storeRenderPlan ({});
}

void DeepSvcPlaybackRenderer::detachDocumentController (DeepSvcDocumentController& owner)
{
    if (documentController == &owner)
    {
        storeRenderPlan ({});
        documentController = nullptr;
    }
}

std::vector<juce::ARAPlaybackRegion*> DeepSvcPlaybackRenderer::assignedPlaybackRegions() const
{
    return getPlaybackRegions<juce::ARAPlaybackRegion>();
}

void DeepSvcPlaybackRenderer::didAddPlaybackRegion (ARA::PlugIn::PlaybackRegion*) noexcept
{
    refreshFromDocument();
}

void DeepSvcPlaybackRenderer::willRemovePlaybackRegion (ARA::PlugIn::PlaybackRegion*) noexcept
{
    refreshFromDocument();
}

void DeepSvcPlaybackRenderer::refreshFromDocument()
{
    if (documentController != nullptr)
        storeRenderPlan (buildRenderPlan());
}

void DeepSvcPlaybackRenderer::prepareToPlay (double sampleRate,
                                             int maximumExpectedSamplesPerBlock,
                                             int numChannels,
                                             juce::AudioProcessor::ProcessingPrecision precision,
                                             AlwaysNonRealtime alwaysNonRealtime)
{
    juce::ignoreUnused (precision, alwaysNonRealtime);

    hostSampleRate = sampleRate;
    renderBuffer.setSize (juce::jmax (1, numChannels),
                          juce::jmax (1, maximumExpectedSamplesPerBlock),
                          false,
                          true,
                          true);

    if (documentController != nullptr)
        documentController->setHostSampleRate (sampleRate);

    // 直通 ↔ 渲染切换的交叉淡化窗口：10ms
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
    if (numSamples <= 0)
        return true;

    const auto plan = loadRenderPlan();
    const auto positionTime = positionInfo.getTimeInSeconds();

    bool planHasRenderedAudio = false;
    if (plan != nullptr)
        for (const auto& item : plan->items)
            planHasRenderedAudio = planHasRenderedAudio || item.audio != nullptr;

    const bool wantRender = planHasRenderedAudio
        && positionTime.hasValue()
        && shouldRenderAraPlaybackBlock (realtime, positionInfo.getIsPlaying());

    // 模式切换时开启交叉淡化窗口，直通与渲染内容之间的切换无爆音
    if (wantRender != (outputMode == RenderOutputMode::rendering))
    {
        outputMode = wantRender ? RenderOutputMode::rendering : RenderOutputMode::passthrough;
        crossfadeRemaining = crossfadeTotal;
    }

    const bool inTransition = crossfadeRemaining > 0;

    // 纯直通：宿主输入保持不变
    if (outputMode == RenderOutputMode::passthrough && ! inTransition)
        return true;

    if (outputMode == RenderOutputMode::rendering || inTransition)
    {
        renderBuffer.clear();
        if (plan != nullptr && positionTime.hasValue())
            renderItems (*plan, renderBuffer, *positionTime, numSamples, false);
    }

    if (inTransition)
    {
        // 淡出方向需要渲染信号参与交叉；宿主停止后位置信息缺失，渲染内容不可得，
        // 此时宿主输入通常已为静音，直接直通输入
        if (outputMode == RenderOutputMode::passthrough && ! positionTime.hasValue())
        {
            crossfadeRemaining = 0;
            return true;
        }

        // output = input * (1 - renderWeight) + render * renderWeight
        // 淡入 renderWeight 0→1，淡出 1→0；窗口最后一帧强制到达终点值
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

        // 淡化窗口之后的剩余部分
        if (fadeSamples < numSamples && outputMode == RenderOutputMode::rendering)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                auto* out = buffer.getWritePointer (channel, fadeSamples);
                const auto* render = renderBuffer.getReadPointer (channel, fadeSamples);
                juce::FloatVectorOperations::copy (out, render, numSamples - fadeSamples);
            }
        }
        // 直通方向：剩余部分保持宿主输入
        return true;
    }

    // 渲染模式：用渲染内容替换宿主输入（ARA 2 回放渲染语义）
    buffer.clear();
    const int channels = juce::jmin (buffer.getNumChannels(), renderBuffer.getNumChannels());
    for (int channel = 0; channel < channels; ++channel)
    {
        auto* out = buffer.getWritePointer (channel);
        const auto* render = renderBuffer.getReadPointer (channel);
        for (int sample = 0; sample < numSamples; ++sample)
            out[sample] += render[sample];
    }
    return true;
}

void DeepSvcPlaybackRenderer::renderSourcePassthrough (juce::AudioBuffer<float>& buffer,
                                                       const juce::AudioPlayHead* playhead) noexcept
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || playhead == nullptr)
        return;

    const auto positionInfo = playhead->getPosition();
    const auto positionTime = positionInfo.hasValue() ? positionInfo->getTimeInSeconds()
                                                      : juce::Optional<double>();
    if (! positionTime.hasValue())
        return;

    const auto plan = loadRenderPlan();
    if (plan == nullptr)
        return;

    renderBuffer.clear();
    if (! renderItems (*plan, renderBuffer, *positionTime, numSamples, true))
        return;

    buffer.clear();
    const int channels = juce::jmin (buffer.getNumChannels(), renderBuffer.getNumChannels());
    for (int channel = 0; channel < channels; ++channel)
    {
        auto* out = buffer.getWritePointer (channel);
        const auto* render = renderBuffer.getReadPointer (channel);
        for (int sample = 0; sample < numSamples; ++sample)
            out[sample] += render[sample];
    }
}

bool DeepSvcPlaybackRenderer::renderItems (const RenderPlan& plan,
                                           juce::AudioBuffer<float>& output,
                                           double blockStartSeconds,
                                           int numSamples,
                                           bool useSourceAudio) noexcept
{
    bool renderedAny = false;
    const double blockEndSeconds = blockStartSeconds
        + static_cast<double> (numSamples) / hostSampleRate;

    for (const auto& item : plan.items)
    {
        const double itemEnd = item.timelineStartSeconds + item.timelineDurationSeconds;
        const double overlapStart = std::max (blockStartSeconds, item.timelineStartSeconds);
        const double overlapEnd = std::min (blockEndSeconds, itemEnd);
        if (overlapEnd <= overlapStart)
            continue;

        const float* audioData = nullptr;
        size_t audioNumSamples = 0;
        // 合成音频的时间原点是覆盖区间起点，源音频的原点是文件起点
        double audioTimeOrigin = 0.0;
        if (useSourceAudio)
        {
            if (item.sourceAudio != nullptr && item.sourceAudio->getNumSamples() > 0)
            {
                audioData = item.sourceAudio->getReadPointer (0);
                audioNumSamples = static_cast<size_t> (item.sourceAudio->getNumSamples());
            }
        }
        else if (item.audio != nullptr)
        {
            audioData = item.audio->data();
            audioNumSamples = item.audio->size();
            audioTimeOrigin = item.synthStartTime;
        }
        if (audioData == nullptr)
            continue;

        renderedAny = true;
        // 时间伸缩：content 时间 = contentStart + (playback - timelineStart) * 伸缩比
        const double contentRatio = item.timelineDurationSeconds > 0.0
            ? item.contentDurationSeconds / item.timelineDurationSeconds
            : 1.0;

        const int channels = output.getNumChannels();
        for (int channel = 0; channel < channels; ++channel)
        {
            auto* dest = output.getWritePointer (channel);
            for (int sample = 0; sample < numSamples; ++sample)
            {
                const double playbackTime = blockStartSeconds
                    + static_cast<double> (sample) / hostSampleRate;
                if (playbackTime < item.timelineStartSeconds || playbackTime >= itemEnd)
                    continue;
                const double contentTime = item.contentStartSeconds
                    + (playbackTime - item.timelineStartSeconds) * contentRatio;
                if (! useSourceAudio
                    && (contentTime < item.synthStartTime || contentTime >= item.synthEndTime))
                    continue;
                dest[sample] += useSourceAudio
                    ? readContentSample (audioData, audioNumSamples, contentTime - audioTimeOrigin)
                    : readPlaybackSample (audioData, audioNumSamples,
                                          contentTime - audioTimeOrigin, item.sampleRate);
            }
        }
    }
    return renderedAny;
}

std::shared_ptr<const DeepSvcPlaybackRenderer::RenderPlan>
DeepSvcPlaybackRenderer::loadRenderPlan() const noexcept
{
    return std::atomic_load_explicit (&renderPlan, std::memory_order_acquire);
}

void DeepSvcPlaybackRenderer::storeRenderPlan (const std::shared_ptr<const RenderPlan>& plan) noexcept
{
    std::atomic_store_explicit (&renderPlan, plan, std::memory_order_release);
}

std::shared_ptr<const DeepSvcPlaybackRenderer::RenderPlan>
DeepSvcPlaybackRenderer::buildRenderPlan() const
{
    auto nextPlan = std::make_shared<RenderPlan>();

    if (documentController == nullptr)
        return nextPlan;

    const auto regions = getPlaybackRegions<juce::ARAPlaybackRegion>();
    const auto projections = documentController->getPlaybackRegionProjectionsFor (regions);
    nextPlan->items.reserve (projections.size());

    for (const auto& projection : projections)
    {
        if (! projection.hasValidPlacement() || ! projection.contentKey.isValid())
            continue;

        const auto* modification = documentController->findModification (projection.contentKey);
        if (modification == nullptr)
            continue;

        RenderItem item;
        item.contentKey = projection.contentKey;
        item.sourceAudio = modification->sourceAudio;

        const auto& slot = modification->active();
        if (slot.hasSynthAudio() && ! slot.bypass
            && slot.synthAudio->samples != nullptr && ! slot.synthAudio->samples->empty())
        {
            item.audio = slot.synthAudio->samples;
            item.sampleRate = slot.synthAudio->sampleRate;
            item.synthStartTime = slot.synthAudio->synthStartTime;
            item.synthEndTime = slot.synthAudio->synthEndTime;
        }

        item.timelineStartSeconds = projection.startInPlaybackTime;
        item.timelineDurationSeconds = projection.durationInPlaybackTime;
        item.contentStartSeconds = projection.startInModificationTime;
        item.contentDurationSeconds = projection.durationInModificationTime;
        nextPlan->items.push_back (std::move (item));
    }

    return nextPlan;
}

} // namespace deepsvc
