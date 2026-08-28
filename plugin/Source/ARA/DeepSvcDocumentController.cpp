#include "DeepSvcDocumentController.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../DebugLog.h"
#include "../Content/ContentArchive.h"
#include "../Content/Segmentation.h"
#include "../State/Parameters.h"
#include "../Utils/TimeCoordinate.h"
#include "DeepSvcEditorView.h"
#include "DeepSvcPlaybackRenderer.h"

// 对应 OpenTune Source/ARA/OpenTuneDocumentController.cpp
namespace deepsvc
{

namespace
{
// 诊断：把合成链路中的音频落盘为 WAV，用于定位「电音」问题
void dumpDebugWav (const juce::String& name, const float* data, size_t numSamples)
{
    if (data == nullptr || numSamples == 0)
        return;

    auto dir = directories::applicationSupport().getChildFile ("debug_audio");
    dir.createDirectory();
    auto file = dir.getChildFile (name + ".wav");
    file.deleteFile();

    juce::WavAudioFormat format;
    auto stream = file.createOutputStream();
    if (stream == nullptr)
        return;

    std::unique_ptr<juce::AudioFormatWriter> writer (
        format.createWriterFor (stream.get(), TimeCoordinate::kRenderSampleRate, 1, 24, {}, 0));
    if (writer == nullptr)
        return;

    stream.release();
    const float* const channels[] = { data };
    writer->writeFromFloatArrays (channels, 1, static_cast<int> (numSamples));
}

// 从修改的源音频（44.1kHz 单声道，覆盖整个源窗口）截取分段区间的样本。
// 分段区间用修改内部时间表示，与源窗口起点对齐
std::vector<float> extractSegmentPcm (const juce::AudioBuffer<float>& sourceAudio,
                                      const SegmentRange& range)
{
    const auto totalSamples = static_cast<int64_t> (sourceAudio.getNumSamples());
    if (totalSamples <= 0 || ! range.isValid())
        return {};

    const auto rate = TimeCoordinate::kRenderSampleRate;
    const auto begin = juce::jlimit<int64_t> (0, totalSamples,
                                              static_cast<int64_t> (range.startSeconds * rate));
    const auto end = juce::jlimit<int64_t> (begin, totalSamples,
                                            static_cast<int64_t> (range.endSeconds() * rate));
    const auto count = static_cast<size_t> (end - begin);
    if (count == 0)
        return {};

    std::vector<float> pcm (count);
    std::memcpy (pcm.data(),
                 sourceAudio.getReadPointer (0, static_cast<int> (begin)),
                 count * sizeof (float));
    return pcm;
}

juce::String ptrHex (const void* p)
{
    if (p == nullptr)
        return "null";
    return "0x" + juce::String::toHexString (reinterpret_cast<juce::int64> (p));
}

template <typename Object>
juce::String pidOf (const Object* object)
{
    if (object == nullptr)
        return "-";
    const juce::String id (object->getPersistentID());
    return id.isNotEmpty() ? id : juce::String ("-");
}

juce::String seqNameOf (const juce::ARARegionSequence* seq)
{
    if (seq == nullptr)
        return "-";
    const juce::String name (seq->getName());
    return name.isNotEmpty() ? name : juce::String ("-");
}

juce::String describeRegion (const juce::ARAPlaybackRegion* region)
{
    if (region == nullptr)
        return "region p=null";

    const auto* mod = region->getAudioModification();
    const auto* seq = region->getRegionSequence();

    return "region p=" + ptrHex (region)
         + " mod=" + ptrHex (mod)
         + " modPid=" + pidOf (mod)
         + " seq=" + ptrHex (seq)
         + " seqName=" + seqNameOf (seq)
         + " win=" + juce::String (region->getStartInAudioModificationTime(), 6)
         + "+" + juce::String (region->getDurationInAudioModificationTime(), 6)
         + " place=" + juce::String (region->getStartInPlaybackTime(), 6)
         + "+" + juce::String (region->getDurationInPlaybackTime(), 6);
}

juce::String describeMod (const juce::ARAAudioModification* mod)
{
    if (mod == nullptr)
        return "mod p=null";

    const auto* src = mod->getAudioSource();
    const auto& regions = mod->getPlaybackRegions();
    return "mod p=" + ptrHex (mod)
         + " pid=" + pidOf (mod)
         + " src=" + ptrHex (src)
         + " srcPid=" + pidOf (src)
         + " deactivated=" + juce::String (mod->isDeactivatedForUndoHistory() ? 1 : 0)
         + " regions=" + juce::String (static_cast<int> (regions.size()));
}

juce::String shadowSummary (const AudioModificationState* state)
{
    if (state == nullptr)
        return "shadow=miss";
    if (! state->hasContentState())
        return "shadow=empty pid=" + state->persistentId;

    int rendered = 0;
    int pitched = 0;
    for (const auto& segment : state->content->segments)
        for (const auto& slot : segment.slots)
        {
            if (slot.hasRenderedAudio())
                ++rendered;
            if (! slot.f0Values.empty())
                ++pitched;
        }

    return "shadow=hit pid=" + state->persistentId
         + " rev=" + juce::String (static_cast<juce::int64> (state->content->contentRevision))
         + " segs=" + juce::String (static_cast<int> (state->content->segments.size()))
         + " pitchedSlots=" + juce::String (pitched)
         + " renderedSlots=" + juce::String (rendered);
}
} // namespace

DeepSvcDocumentController::DeepSvcDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                                                      const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation (entry, instance)
    , jobManager (*this)
{
    debugLog ("ara documentController created p=" + ptrHex (this));
}

DeepSvcDocumentController::~DeepSvcDocumentController()
{
    debugLog ("ara documentController destroyed p=" + ptrHex (this));
    // 渲染器生命周期由宿主掌握，可能晚于 DC：先解除所有渲染器对 DC 的引用
    for (auto* renderer : playbackRenderers)
        if (renderer != nullptr)
            renderer->detachDocumentController (*this);
    playbackRenderers.clear();
}

//==============================================================================
// 投影查询

std::vector<DeepSvcDocumentController::PlaybackRegionProjection>
DeepSvcDocumentController::getPlaybackRegionProjections() const
{
    return buildProjections();
}

std::vector<DeepSvcDocumentController::PlaybackRegionProjection>
DeepSvcDocumentController::getPlaybackRegionProjectionsFor (
    const std::vector<juce::ARAPlaybackRegion*>& regions) const
{
    std::vector<PlaybackRegionProjection> result;
    for (const auto* region : regions)
        if (const auto* entry = findPlaybackRegion (const_cast<juce::ARAPlaybackRegion*> (region)))
            result.push_back (makeProjection (*entry));
    return result;
}

std::vector<DeepSvcDocumentController::PlaybackRegionProjection>
DeepSvcDocumentController::getEditorSelectionPlaybackRegionProjections() const
{
    return getPlaybackRegionProjectionsFor (editorSelectionPlaybackRegions);
}

std::optional<DeepSvcDocumentController::PlaybackRegionProjection>
DeepSvcDocumentController::getFocusedEditorPlaybackRegionProjection() const
{
    const auto projections = getEditorSelectionPlaybackRegionProjections();
    if (projections.empty())
        return std::nullopt;
    return projections.front();
}

void DeepSvcDocumentController::setEditorViewSelectionPlaybackRegions (
    std::vector<juce::ARAPlaybackRegion*> playbackRegions)
{
    editorSelectionPlaybackRegions = std::move (playbackRegions);
    ++editorSelectionRevision;
    reconcileEditorSelectionPlaybackRegions();
}

//==============================================================================
// 渲染器注册

void DeepSvcDocumentController::registerPlaybackRenderer (DeepSvcPlaybackRenderer& renderer)
{
    if (std::find (playbackRenderers.begin(), playbackRenderers.end(), &renderer) == playbackRenderers.end())
        playbackRenderers.push_back (&renderer);
}

void DeepSvcDocumentController::unregisterPlaybackRenderer (DeepSvcPlaybackRenderer& renderer)
{
    playbackRenderers.erase (std::remove (playbackRenderers.begin(), playbackRenderers.end(), &renderer),
                             playbackRenderers.end());
}

//==============================================================================
// 内容读取

const ModificationContent* DeepSvcDocumentController::findContent (ContentKey key) const
{
    const auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return nullptr;
    return &*modification->content;
}

const ContentSegment* DeepSvcDocumentController::findSegment (SegmentKey key) const
{
    const auto* content = findContent (key.content);
    if (content == nullptr)
        return nullptr;

    for (const auto& segment : content->segments)
        if (SegmentKey::microsFromSeconds (segment.range.startSeconds) == key.startMicros)
            return &segment;
    return nullptr;
}

ContentSegment* DeepSvcDocumentController::findSegmentForWrite (SegmentKey key)
{
    auto* modification = findAudioModificationByContentKey (key.content);
    if (modification == nullptr || ! modification->hasContentState())
        return nullptr;

    for (auto& segment : modification->content->segments)
        if (SegmentKey::microsFromSeconds (segment.range.startSeconds) == key.startMicros)
            return &segment;
    return nullptr;
}

uint64_t DeepSvcDocumentController::readContentRevision (ContentKey key) const
{
    const auto* content = findContent (key);
    return content != nullptr ? content->contentRevision : 0;
}

int DeepSvcDocumentController::readActiveSlot (SegmentKey key) const
{
    const auto* segment = findSegment (key);
    return segment != nullptr ? segment->activeSlot : 0;
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::readSourceAudio (ContentKey key)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr)
        return nullptr;
    return ensureSourceAudio (*modification);
}

//==============================================================================
// 分段槽位状态（A/B，docs/ara.md 第 4.1 节）

void DeepSvcDocumentController::setActiveSlot (SegmentKey key, int slot)
{
    auto* segment = findSegmentForWrite (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    if (segment->activeSlot == slot)
        return;

    segment->activeSlot = slot;

    auto* modification = findAudioModificationByContentKey (key.content);
    ++modification->content->contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::setSlotBypass (SegmentKey key, int slot, bool bypass)
{
    auto* segment = findSegmentForWrite (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    if (segment->slots[static_cast<size_t> (slot)].bypass == bypass)
        return;

    segment->slots[static_cast<size_t> (slot)].bypass = bypass;

    auto* modification = findAudioModificationByContentKey (key.content);
    ++modification->content->contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyTimbreFile (SegmentKey key, int slot, const juce::String& timbreFile)
{
    auto* segment = findSegmentForWrite (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    segment->slots[static_cast<size_t> (slot)].timbreFile = timbreFile;
    notifyPersistedStateChanged (*findAudioModificationByContentKey (key.content));
}

void DeepSvcDocumentController::applySlotParams (SegmentKey key, int slot, const EngineSynthParams& params)
{
    auto* segment = findSegmentForWrite (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    segment->slots[static_cast<size_t> (slot)].params = params;
    notifyPersistedStateChanged (*findAudioModificationByContentKey (key.content));
}

//==============================================================================
// 内容写入（任务完成时调用）

void DeepSvcDocumentController::applyF0 (SegmentKey key,
                                         int slot,
                                         std::vector<float> times,
                                         std::vector<float> values)
{
    auto* segment = findSegmentForWrite (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    auto& slotContent = segment->slots[static_cast<size_t> (slot)];
    slotContent.f0Times = std::move (times);
    slotContent.f0Values = std::move (values);

    auto* modification = findAudioModificationByContentKey (key.content);
    ++modification->content->contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyRenderedAudio (SegmentKey key,
                                                    int slot,
                                                    std::shared_ptr<const std::vector<float>> samples,
                                                    const EngineSynthParams& synthParams,
                                                    const juce::String& synthTimbreFile)
{
    auto* segment = findSegmentForWrite (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    auto& slotContent = segment->slots[static_cast<size_t> (slot)];
    slotContent.renderedAudio = std::move (samples);
    slotContent.synthParams = synthParams;
    slotContent.synthTimbreFile = synthTimbreFile;

    auto* modification = findAudioModificationByContentKey (key.content);
    ++modification->content->contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

//==============================================================================
// 任务入口

void DeepSvcDocumentController::requestDetect (SegmentKey key,
                                               int slot,
                                               EngineEstimator estimator)
{
    auto* modification = findAudioModificationByContentKey (key.content);
    if (modification == nullptr)
        return;

    const auto* segment = findSegment (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    const auto audio = ensureSourceAudio (*modification);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    // 检测只覆盖本分段的区间：切分得到的两个分段各自检测自己那段
    auto pcm = extractSegmentPcm (*audio, segment->range);
    if (pcm.empty())
        return;

    jobManager.submitDetect ({ key, slot }, std::move (pcm),
                             static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                             estimator);
}

void DeepSvcDocumentController::requestSynth (SegmentKey key,
                                              int slot,
                                              const juce::String& timbreAbsolutePath,
                                              const EngineSynthParams& params)
{
    auto* modification = findAudioModificationByContentKey (key.content);
    if (modification == nullptr)
        return;

    const auto* segment = findSegment (key);
    if (segment == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    const auto audio = ensureSourceAudio (*modification);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    auto pcm = extractSegmentPcm (*audio, segment->range);
    if (pcm.empty())
        return;

    const JobKey jobKey { key, slot };
    pendingSynthParams[jobKey] = params;
    pendingSynthTimbres[jobKey] = segment->slots[static_cast<size_t> (slot)].timbreFile;

    dumpDebugWav ("synth_input", pcm.data(), pcm.size());
    debugLog ("requestSynth slot=" + juce::String (slot)
              + " segStart=" + juce::String (segment->range.startSeconds, 3)
              + " segDur=" + juce::String (segment->range.durationSeconds, 3)
              + " samples=" + juce::String (static_cast<int64_t> (pcm.size()))
              + " estimator=" + juce::String (static_cast<uint32_t> (params.f0Estimator))
              + " steps=" + juce::String (params.diffusionSteps)
              + " shift=" + juce::String (params.pitchShift)
              + " fineTuneCents=" + juce::String (params.pitchFineTuneCents)
              + " cfg=" + juce::String (params.cfgRate)
              + " gain=" + juce::String (params.inputGainDb)
              + " keepFirst=" + juce::String (params.keepFirstVocoderOutput ? 1 : 0)
              + " timbre=" + timbreAbsolutePath);

    jobManager.submitSynth (jobKey, std::move (pcm),
                            static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                            timbreAbsolutePath, params);
}

void DeepSvcDocumentController::cancelJobs (SegmentKey key, int slot)
{
    jobManager.cancelJobsFor ({ key, juce::jlimit (0, 1, slot) });
}

JobStatus DeepSvcDocumentController::jobStatusFor (SegmentKey key, int slot) const
{
    return jobManager.statusFor ({ key, juce::jlimit (0, 1, slot) });
}

//==============================================================================
// 源音频提取：modification 整个源窗口 → 44.1kHz 单声道，修改级共享缓存

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::ensureSourceAudio (AudioModificationState& modification)
{
    attachSource (modification);
    if (! modification.hasContentState())
        return nullptr;

    auto& content = *modification.content;
    if (content.sourceAudio != nullptr)
        return content.sourceAudio;

    if (modification.audioModification == nullptr)
        return nullptr;

    auto* audioSource = modification.audioModification->getAudioSource();
    if (audioSource == nullptr)
        return nullptr;

    juce::ARAAudioSourceReader reader (audioSource);
    const auto numSamples = reader.lengthInSamples;
    const auto numChannels = static_cast<int> (reader.numChannels);
    const double sourceRate = reader.sampleRate;
    if (numSamples <= 0 || numChannels <= 0 || sourceRate <= 0.0)
        return nullptr;

    juce::AudioBuffer<float> source (numChannels, static_cast<int> (numSamples));
    if (! reader.read (&source, 0, static_cast<int> (numSamples), 0, true, true))
        return nullptr;

    // 重采样到 44.1kHz 并混为单声道
    const double ratio = sourceRate / TimeCoordinate::kRenderSampleRate;
    const int outputSamples = static_cast<int> (static_cast<double> (numSamples) / ratio);

    auto mono = std::make_shared<juce::AudioBuffer<float>> (1, juce::jmax (1, outputSamples));
    mono->clear();

    // 插值器需要尾部余量，输入末尾补零
    constexpr int tailPadding = 128;
    juce::AudioBuffer<float> padded (numChannels, static_cast<int> (numSamples) + tailPadding);
    padded.clear();
    for (int channel = 0; channel < numChannels; ++channel)
        padded.copyFrom (channel, 0, source, channel, 0, static_cast<int> (numSamples));

    const float scale = 1.0f / static_cast<float> (numChannels);
    juce::AudioBuffer<float> resampled (numChannels, outputSamples);
    for (int channel = 0; channel < numChannels; ++channel)
    {
        juce::WindowedSincInterpolator interpolator;
        interpolator.reset();
        interpolator.process (ratio,
                              padded.getReadPointer (channel),
                              resampled.getWritePointer (channel),
                              outputSamples,
                              padded.getNumSamples(),
                              0);

        const auto* data = resampled.getReadPointer (channel);
        auto* out = mono->getWritePointer (0);
        for (int i = 0; i < outputSamples; ++i)
            out[i] += data[i] * scale;
    }

    content.sourceAudio = mono;
    // 源音频就绪属于内容变化：推进 revision，编辑器心跳会重新推送波形
    ++content.contentRevision;
    return mono;
}

//==============================================================================
// 分段划分与继承（docs/ara.md 第 4.1 节）

void DeepSvcDocumentController::reconcileSegments (AudioModificationState& modification)
{
    if (! modification.hasContentState() || modification.persistentId.isEmpty())
        return;

    auto& content = *modification.content;

    // 本修改下所有片段的内容窗口
    std::vector<SegmentRange> windows;
    for (const auto& region : playbackRegions)
    {
        if (region.audioModificationPersistentId != modification.persistentId)
            continue;
        const SegmentRange window { region.startInModificationTime, region.durationInModificationTime };
        if (window.isValid())
            windows.push_back (window);
    }

    if (windows.empty())
        return;

    const auto ranges = segmentation::computeRanges (windows);
    if (ranges.empty())
        return;

    // 布局未变则不动，避免每次属性更新都重建分段
    if (segmentation::layoutMatches (content.segments, ranges))
    {
        debugLog ("ara reconcileSegments skip pid=" + modification.persistentId
                  + " segs=" + juce::String (static_cast<int> (content.segments.size())));
        return;
    }

    juce::String oldLayout;
    for (const auto& segment : content.segments)
        oldLayout += juce::String (segment.range.startSeconds, 6) + "+"
                   + juce::String (segment.range.durationSeconds, 6) + ",";
    juce::String newLayout;
    for (const auto& range : ranges)
        newLayout += juce::String (range.startSeconds, 6) + "+"
                   + juce::String (range.durationSeconds, 6) + ",";
    debugLog ("ara reconcileSegments rebuild pid=" + modification.persistentId
              + " old=[" + oldLayout + "] new=[" + newLayout + "]");

    // 旧分段上进行中的任务失去写回目标：取消它们
    for (const auto& segment : content.segments)
    {
        const SegmentKey key { modification.contentIdentity,
                               SegmentKey::microsFromSeconds (segment.range.startSeconds) };
        const bool survives = std::any_of (ranges.begin(), ranges.end(),
                                           [&segment] (const SegmentRange& range)
                                           { return segment.range.matches (range, segmentation::kBoundaryTolerance); });
        if (survives)
            continue;
        for (int slot = 0; slot < 2; ++slot)
        {
            jobManager.cancelJobsFor ({ key, slot });
            pendingSynthParams.erase ({ key, slot });
            pendingSynthTimbres.erase ({ key, slot });
        }
    }

    content.segments = segmentation::rebuild (content.segments, ranges,
                                              TimeCoordinate::kRenderSampleRate);
    ++content.contentRevision;
    notifyPersistedStateChanged (modification);
}

void DeepSvcDocumentController::reconcileAllSegments()
{
    for (auto& modification : audioModifications)
        reconcileSegments (modification);
}

void DeepSvcDocumentController::dumpAraGraph (const juce::String& reason)
{
    auto* document = getDocument();
    debugLog ("ara dump begin reason=" + reason
              + " shadowMods=" + juce::String (static_cast<int> (audioModifications.size()))
              + " shadowRegions=" + juce::String (static_cast<int> (playbackRegions.size())));

    if (document == nullptr)
    {
        debugLog ("ara dump document=null");
        return;
    }

    int hostMods = 0;
    int hostRegions = 0;
    for (auto* source : document->getAudioSources())
    {
        const auto& mods = source->getAudioModifications();
        debugLog ("ara dump src p=" + ptrHex (source)
                  + " pid=" + pidOf (source)
                  + " deactivated=" + juce::String (source->isDeactivatedForUndoHistory() ? 1 : 0)
                  + " mods=" + juce::String (static_cast<int> (mods.size())));
        for (auto* mod : mods)
        {
            ++hostMods;
            debugLog ("ara dump   " + describeMod (mod) + " " + shadowSummary (findAudioModification (mod)));
            for (auto* region : mod->getPlaybackRegions())
            {
                ++hostRegions;
                debugLog ("ara dump     " + describeRegion (region));
            }
        }
    }

    debugLog ("ara dump end hostMods=" + juce::String (hostMods)
              + " hostRegions=" + juce::String (hostRegions));

    for (const auto& state : audioModifications)
    {
        bool foundInHost = false;
        if (document != nullptr)
            for (auto* source : document->getAudioSources())
                for (auto* mod : source->getAudioModifications())
                    if (mod == state.audioModification)
                        foundInHost = true;
        if (! foundInHost)
            debugLog ("ara dump orphanShadow p=" + ptrHex (state.audioModification)
                      + " " + shadowSummary (&state));
    }
}

//==============================================================================
// ARA 通知

void DeepSvcDocumentController::willBeginEditing (juce::ARADocument* document)
{
    juce::ignoreUnused (document);
    debugLog ("ara willBeginEditing");
    dumpAraGraph ("willBeginEditing");
}

void DeepSvcDocumentController::didUpdateAudioSourceProperties (juce::ARAAudioSource* audioSource)
{
    debugLog ("ara didUpdateAudioSourceProperties p=" + ptrHex (audioSource)
              + " pid=" + pidOf (audioSource));
    auto& sourceRef = ensureAudioSource (audioSource);
    sourceRef.updateFrom (audioSource);

    // 源形状变化（替换音频等）后，修改的源音频缓存失效
    for (auto& modification : audioModifications)
    {
        if (! modification.hasContentState()
            || modification.content->sourceWindow.sourcePersistentId != sourceRef.persistentId)
            continue;
        attachSource (modification);
        modification.content->sourceAudio.reset();
    }
}

void DeepSvcDocumentController::doUpdateAudioSourceContent (juce::ARAAudioSource* audioSource,
                                                            const juce::ARAContentUpdateScopes scopeFlags)
{
    debugLog ("ara doUpdateAudioSourceContent p=" + ptrHex (audioSource)
              + " pid=" + pidOf (audioSource)
              + " affectSamples=" + juce::String (scopeFlags.affectSamples() ? 1 : 0));
    // 对应 OpenTuneDocumentController.cpp 的 doUpdateAudioSourceContent：
    // 波形信号没变（只改了名称、颜色、标记等）时保留派生数据
    if (! scopeFlags.affectSamples())
        return;

    for (auto& modification : audioModifications)
    {
        if (modification.audioModification == nullptr || ! modification.hasContentState())
            continue;
        if (modification.audioModification->getAudioSource() != audioSource)
            continue;

        auto& content = *modification.content;
        content.sourceAudio.reset();
        for (auto& segment : content.segments)
        {
            for (auto& slot : segment.slots)
            {
                slot.renderedAudio.reset();
                slot.f0Times.clear();
                slot.f0Values.clear();
            }
        }
        ++content.contentRevision;
    }

    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioSource (juce::ARAAudioSource* audioSource)
{
    debugLog ("ara willDestroyAudioSource p=" + ptrHex (audioSource)
              + " pid=" + pidOf (audioSource));
    // 源销毁时，依赖它的修改内容一并失效
    for (auto& modification : audioModifications)
    {
        if (! modification.hasContentState())
            continue;
        if (modification.audioModification != nullptr
            && modification.audioModification->getAudioSource() == audioSource)
        {
            modification.content.reset();
        }
    }

    audioSources.erase (std::remove_if (audioSources.begin(), audioSources.end(),
                                        [audioSource] (const AudioSourceRef& ref)
                                        {
                                            return ref.audioSource == audioSource;
                                        }),
                                        audioSources.end());
}

void DeepSvcDocumentController::didAddAudioModificationToAudioSource (
    juce::ARAAudioSource* audioSource,
    juce::ARAAudioModification* audioModification)
{
    debugLog ("ara didAddAudioModificationToAudioSource src=" + ptrHex (audioSource)
              + " srcPid=" + pidOf (audioSource)
              + " " + describeMod (audioModification)
              + " " + shadowSummary (findAudioModification (audioModification)));
}

void DeepSvcDocumentController::willRemoveAudioModificationFromAudioSource (
    juce::ARAAudioSource* audioSource,
    juce::ARAAudioModification* audioModification)
{
    debugLog ("ara willRemoveAudioModificationFromAudioSource src=" + ptrHex (audioSource)
              + " srcPid=" + pidOf (audioSource)
              + " " + describeMod (audioModification)
              + " " + shadowSummary (findAudioModification (audioModification)));
}

void DeepSvcDocumentController::willUpdateAudioModificationProperties (
    juce::ARAAudioModification* audioModification,
    juce::ARAAudioModification::PropertiesPtr newProperties)
{
    juce::String newPid = "-";
    if (newProperties != nullptr && newProperties->persistentID != nullptr
        && newProperties->persistentID[0] != 0)
        newPid = juce::String (newProperties->persistentID);
    debugLog ("ara willUpdateAudioModificationProperties " + describeMod (audioModification)
              + " newPid=" + newPid);
}

void DeepSvcDocumentController::willDeactivateAudioModificationForUndoHistory (
    juce::ARAAudioModification* audioModification,
    bool deactivate)
{
    debugLog ("ara willDeactivateAudioModificationForUndoHistory deactivate="
              + juce::String (deactivate ? 1 : 0) + " " + describeMod (audioModification)
              + " " + shadowSummary (findAudioModification (audioModification)));
}

void DeepSvcDocumentController::didDeactivateAudioModificationForUndoHistory (
    juce::ARAAudioModification* audioModification,
    bool deactivate)
{
    debugLog ("ara didDeactivateAudioModificationForUndoHistory deactivate="
              + juce::String (deactivate ? 1 : 0) + " " + describeMod (audioModification)
              + " " + shadowSummary (findAudioModification (audioModification)));
}

void DeepSvcDocumentController::didUpdateAudioModificationProperties (juce::ARAAudioModification* audioModification)
{
    debugLog ("ara didUpdateAudioModificationProperties " + describeMod (audioModification)
              + " " + shadowSummary (findAudioModification (audioModification)));
    auto& modification = ensureAudioModification (audioModification);
    modification.updateIdentity (audioModification);
    attachSource (modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioModification (juce::ARAAudioModification* audioModification)
{
    debugLog ("ara willDestroyAudioModification " + describeMod (audioModification)
              + " " + shadowSummary (findAudioModification (audioModification)));
    auto* modification = findAudioModification (audioModification);
    if (modification == nullptr)
        return;

    const auto persistentId = modification->persistentId;
    playbackRegions.erase (std::remove_if (playbackRegions.begin(), playbackRegions.end(),
                                           [&persistentId] (const PlaybackRegion& region)
                                           {
                                               return persistentId.isNotEmpty()
                                                   && region.audioModificationPersistentId == persistentId;
                                           }),
                           playbackRegions.end());
    reconcileEditorSelectionPlaybackRegions();

    audioModifications.erase (std::remove_if (audioModifications.begin(), audioModifications.end(),
                                              [audioModification] (const AudioModificationState& state)
                                              {
                                                  return state.audioModification == audioModification;
                                              }),
                              audioModifications.end());

    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didAddPlaybackRegionToAudioModification (
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara didAddPlaybackRegionToAudioModification " + describeMod (audioModification)
              + " " + describeRegion (playbackRegion));
    auto& modification = ensureAudioModification (audioModification);
    auto& region = ensurePlaybackRegion (playbackRegion);
    region.updateFrom (playbackRegion);
    if (region.audioModificationPersistentId.isEmpty())
        region.audioModificationPersistentId = modification.persistentId;
    attachSource (modification);
    reconcileSegments (modification);
    refreshRegisteredRenderers (publishModelChange());
}

// 对应 OpenTuneDocumentController.cpp 的 didEndEditing（第 1017 行）：
// 编辑事务结束后重新划分分段（切分、复制、修剪的继承在此发生），
// 并物化所有内容的源音频，保证编辑器打开即有波形
void DeepSvcDocumentController::didEndEditing (juce::ARADocument* document)
{
    juce::ignoreUnused (document);
    debugLog ("ara didEndEditing");
    dumpAraGraph ("didEndEditing");

    for (auto& modification : audioModifications)
    {
        if (! modification.hasContentState())
            continue;
        if (modification.content->sourceWindow.sourcePersistentId.isEmpty())
            continue;
        ensureSourceAudio (modification);
    }

    reconcileAllSegments();
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource,
                                                                   bool enable)
{
    if (! enable)
        return;

    // 采样访问开启后立刻提取源音频缓存，保证后续检测/合成/回放可用
    for (auto& modification : audioModifications)
    {
        if (! modification.hasContentState())
            continue;
        if (modification.content->sourceWindow.sourcePersistentId.isEmpty())
            continue;
        const auto* sourceRef = findAudioSource (modification.content->sourceWindow.sourcePersistentId);
        if (sourceRef == nullptr || sourceRef->audioSource != audioSource)
            continue;
        ensureSourceAudio (modification);
    }
}

void DeepSvcDocumentController::didUpdateRegionSequenceProperties (juce::ARARegionSequence* regionSequence)
{
    debugLog ("ara didUpdateRegionSequenceProperties seq=" + ptrHex (regionSequence)
              + " seqName=" + seqNameOf (regionSequence));
    // 音轨颜色变化后刷新音频块的显示颜色
    for (auto& region : playbackRegions)
    {
        if (region.playbackRegion == nullptr)
            continue;
        if (region.playbackRegion->getRegionSequence() != regionSequence)
            continue;
        region.updateDisplayColourFrom (region.playbackRegion);
    }
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didAddPlaybackRegionToRegionSequence (
    juce::ARARegionSequence* regionSequence,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara didAddPlaybackRegionToRegionSequence seq=" + ptrHex (regionSequence)
              + " seqName=" + seqNameOf (regionSequence)
              + " " + describeRegion (playbackRegion));
}

void DeepSvcDocumentController::willRemovePlaybackRegionFromRegionSequence (
    juce::ARARegionSequence* regionSequence,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara willRemovePlaybackRegionFromRegionSequence seq=" + ptrHex (regionSequence)
              + " seqName=" + seqNameOf (regionSequence)
              + " " + describeRegion (playbackRegion));
}

void DeepSvcDocumentController::willUpdatePlaybackRegionProperties (
    juce::ARAPlaybackRegion* playbackRegion,
    juce::ARAPlaybackRegion::PropertiesPtr newProperties)
{
    juce::String next = "new=null";
    if (newProperties != nullptr)
        next = "newWin=" + juce::String (newProperties->startInModificationTime, 6)
             + "+" + juce::String (newProperties->durationInModificationTime, 6)
             + " newPlace=" + juce::String (newProperties->startInPlaybackTime, 6)
             + "+" + juce::String (newProperties->durationInPlaybackTime, 6)
             + " newSeqRef=" + ptrHex (newProperties->regionSequenceRef)
             + " flags=" + juce::String (static_cast<juce::int64> (newProperties->transformationFlags));
    debugLog ("ara willUpdatePlaybackRegionProperties " + describeRegion (playbackRegion)
              + " " + next);
}

void DeepSvcDocumentController::didUpdatePlaybackRegionProperties (juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara didUpdatePlaybackRegionProperties " + describeRegion (playbackRegion));
    auto& region = ensurePlaybackRegion (playbackRegion);
    region.updateFrom (playbackRegion);
    if (auto* modification = findAudioModification (region.audioModificationPersistentId))
        reconcileSegments (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willRemovePlaybackRegionFromAudioModification (
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara willRemovePlaybackRegionFromAudioModification " + describeMod (audioModification)
              + " " + describeRegion (playbackRegion));
    removePlaybackRegion (playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara willDestroyPlaybackRegion " + describeRegion (playbackRegion));
    removePlaybackRegion (playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers (publishModelChange());
}

//==============================================================================
// 归档：persistentID + JSON 字符串（docs/ara.md 第 4.2 节）。
// 与 OpenTune 的差异：OpenTune 记录 XML、渲染结果不入库、恢复后由渲染服务重建；
// 本插件的推理结果（音高、合成音频）直接入库，这是设计选择。

namespace
{

constexpr int kArchiveMagic = 0x44535643;  // 'DSVC'
constexpr int kArchiveVersion = 1;
constexpr int kMaxArchiveRecords = 65536;

juce::String mapRestoredPersistentId (const juce::String& archivedPersistentId,
                                      const juce::ARARestoreObjectsFilter* filter)
{
    if (archivedPersistentId.isEmpty())
        return {};

    if (filter == nullptr)
        return archivedPersistentId;

    auto* audioModification = filter->getAudioModificationToRestoreStateWithID (
        archivedPersistentId.toRawUTF8());
    if (audioModification == nullptr)
        return {};

    const auto& restoredPersistentId = audioModification->getPersistentID();
    return restoredPersistentId.empty() ? juce::String()
                                        : juce::String::fromUTF8 (restoredPersistentId.c_str());
}

} // namespace

bool DeepSvcDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output,
                                                        const juce::ARAStoreObjectsFilter* filter)
{
    std::vector<const AudioModificationState*> bindings;

    if (filter == nullptr)
    {
        for (const auto& modification : audioModifications)
            if (modification.persistentId.isNotEmpty() && modification.hasContentState())
                bindings.push_back (&modification);
    }
    else
    {
        const auto& modsToStore = filter->getAudioModificationsToStore();
        for (const auto& modification : audioModifications)
        {
            if (modification.persistentId.isEmpty() || ! modification.hasContentState())
                continue;

            const auto* araMod = modification.audioModification;
            if (araMod == nullptr)
                continue;

            const auto* basePtr = static_cast<const ARA::PlugIn::AudioModification*> (araMod);
            if (std::find (modsToStore.begin(), modsToStore.end(), basePtr) != modsToStore.end())
                bindings.push_back (&modification);
        }
    }

    if (bindings.size() > static_cast<size_t> (kMaxArchiveRecords))
        return false;

    juce::String stored;
    for (const auto* modification : bindings)
        stored += modification->persistentId + ",";
    debugLog ("ara doStoreObjects count=" + juce::String (static_cast<int> (bindings.size()))
              + " pids=[" + stored + "]");

    bool ok = output.writeInt (kArchiveMagic);
    ok = output.writeInt (kArchiveVersion) && ok;
    ok = output.writeInt (static_cast<int> (bindings.size())) && ok;

    for (const auto* modification : bindings)
    {
        const auto& content = *modification->content;

        auto* root = new juce::DynamicObject();
        auto* window = new juce::DynamicObject();
        window->setProperty ("start", content.sourceWindow.sourceStartSeconds);
        window->setProperty ("end", content.sourceWindow.sourceEndSeconds);
        root->setProperty ("sourceWindow", juce::var (window));
        root->setProperty ("segments", archive::segmentsToJson (content.segments));

        ok = output.writeString (modification->persistentId) && ok;
        ok = output.writeString (juce::JSON::toString (juce::var (root))) && ok;
    }

    return ok;
}

bool DeepSvcDocumentController::doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                                            const juce::ARARestoreObjectsFilter* filter)
{
    if (input.readInt() != kArchiveMagic)
        return false;
    if (input.readInt() != kArchiveVersion)
        return false;

    const int recordCount = input.readInt();
    if (recordCount < 0 || recordCount > kMaxArchiveRecords)
        return false;

    for (int i = 0; i < recordCount; ++i)
    {
        const auto archivedPersistentId = input.readString();
        const auto jsonText = input.readString();

        debugLog ("ara doRestore record archivedPid=" + archivedPersistentId
                  + " jsonBytes=" + juce::String (jsonText.length()));

        // 单条记录映射不上或损坏时跳过该条，对应 OpenTune restoreAudioModificationContent 的逐条容错
        const auto restoredPersistentId = mapRestoredPersistentId (archivedPersistentId, filter);
        if (restoredPersistentId.isEmpty())
        {
            debugLog ("ara doRestore skip noHostMap archivedPid=" + archivedPersistentId);
            continue;
        }

        auto* modification = findAudioModification (restoredPersistentId);
        if (modification == nullptr || modification->audioModification == nullptr)
        {
            debugLog ("ara doRestore skip noShadow restoredPid=" + restoredPersistentId);
            continue;
        }

        attachSource (*modification);
        if (! modification->hasContentState())
            continue;

        const auto json = juce::JSON::parse (jsonText);
        if (! json.isObject())
            continue;

        auto& content = *modification->content;

        // 源窗口校验：与当前源不匹配说明音频已被替换，跳过该记录
        const auto windowJson = json.getProperty ("sourceWindow", juce::var());
        const auto archivedStart = static_cast<double> (windowJson.getProperty ("start", -1.0));
        const auto archivedEnd = static_cast<double> (windowJson.getProperty ("end", -1.0));
        if (archivedStart < 0.0
            || std::abs (archivedStart - content.sourceWindow.sourceStartSeconds) > 1.0e-3
            || std::abs (archivedEnd - content.sourceWindow.sourceEndSeconds) > 1.0e-3)
        {
            debugLog ("ara doRestore skip windowMismatch restoredPid=" + restoredPersistentId
                      + " archived=" + juce::String (archivedStart, 6) + ".." + juce::String (archivedEnd, 6)
                      + " current=" + juce::String (content.sourceWindow.sourceStartSeconds, 6)
                      + ".." + juce::String (content.sourceWindow.sourceEndSeconds, 6));
            continue;
        }

        content.segments = archive::segmentsFromJson (json.getProperty ("segments", juce::var()));

        ++content.contentRevision;
        debugLog ("ara doRestore applied restoredPid=" + restoredPersistentId
                  + " segs=" + juce::String (static_cast<int> (content.segments.size())));
        // 恢复的内容对宿主是新增的：标记工程已修改，宿主才会保存
        notifyPersistedStateChanged (*modification);
    }

    refreshRegisteredRenderers (publishModelChange());
    return true;
}

//==============================================================================
// JobManager::Listener（消息线程）

void DeepSvcDocumentController::jobStatusChanged (JobKey key, const JobStatus& status)
{
    // 状态显示由编辑器经 jobStatusFor 轮询；这里只清理失败/取消任务的参数快照
    if (status.state == JobStatus::State::failed || status.state == JobStatus::State::cancelled)
    {
        pendingSynthParams.erase (key);
        pendingSynthTimbres.erase (key);
    }
}

void DeepSvcDocumentController::detectFinished (JobKey key, std::vector<float> f0)
{
    const auto frameCount = f0.size();
    const auto duration = static_cast<double> (frameCount) * 0.01;

    std::vector<float> times (frameCount);
    for (size_t i = 0; i < frameCount; ++i)
        times[i] = static_cast<float> (static_cast<double> (i) * 0.01);

    juce::ignoreUnused (duration);
    applyF0 (key.segment, key.slot, std::move (times), std::move (f0));
}

void DeepSvcDocumentController::synthFinished (JobKey key,
                                               std::vector<float> audio,
                                               std::vector<float> firstVocoder,
                                               std::vector<float>)
{
    debugLog ("synthFinished slot=" + juce::String (key.slot)
              + " audio=" + juce::String (static_cast<int64_t> (audio.size()))
              + " firstVocoder=" + juce::String (static_cast<int64_t> (firstVocoder.size())));
    dumpDebugWav ("synth_output_final", audio.data(), audio.size());
    if (! firstVocoder.empty())
        dumpDebugWav ("synth_output_first_vocoder", firstVocoder.data(), firstVocoder.size());

    // 输出声码器 level 1：用第一级声码器输出替换最终结果
    auto output = firstVocoder.empty() ? std::move (audio) : std::move (firstVocoder);

    EngineSynthParams synthParams;
    juce::String synthTimbre;
    if (const auto it = pendingSynthParams.find (key); it != pendingSynthParams.end())
    {
        synthParams = it->second;
        pendingSynthParams.erase (it);
    }
    if (const auto it = pendingSynthTimbres.find (key); it != pendingSynthTimbres.end())
    {
        synthTimbre = it->second;
        pendingSynthTimbres.erase (it);
    }

    applyRenderedAudio (key.segment, key.slot,
                        std::make_shared<const std::vector<float>> (std::move (output)),
                        synthParams, synthTimbre);
}

//==============================================================================
// 查找与维护

AudioSourceRef* DeepSvcDocumentController::findAudioSource (juce::ARAAudioSource* audioSource)
{
    for (auto& ref : audioSources)
        if (ref.audioSource == audioSource)
            return &ref;
    return nullptr;
}

AudioSourceRef* DeepSvcDocumentController::findAudioSource (const juce::String& persistentId)
{
    for (auto& ref : audioSources)
        if (ref.persistentId == persistentId)
            return &ref;
    return nullptr;
}

const AudioSourceRef* DeepSvcDocumentController::findAudioSource (const juce::String& persistentId) const
{
    for (const auto& ref : audioSources)
        if (ref.persistentId == persistentId)
            return &ref;
    return nullptr;
}

AudioSourceRef& DeepSvcDocumentController::ensureAudioSource (juce::ARAAudioSource* audioSource)
{
    if (auto* ref = findAudioSource (audioSource))
        return *ref;

    audioSources.emplace_back();
    auto& ref = audioSources.back();
    ref.updateFrom (audioSource);
    return ref;
}

AudioModificationState* DeepSvcDocumentController::findAudioModification (const juce::String& persistentId)
{
    for (auto& state : audioModifications)
        if (state.persistentId == persistentId)
            return &state;
    return nullptr;
}

const AudioModificationState* DeepSvcDocumentController::findAudioModification (const juce::String& persistentId) const
{
    for (const auto& state : audioModifications)
        if (state.persistentId == persistentId)
            return &state;
    return nullptr;
}

AudioModificationState* DeepSvcDocumentController::findAudioModification (juce::ARAAudioModification* audioModification)
{
    for (auto& state : audioModifications)
        if (state.audioModification == audioModification)
            return &state;
    return nullptr;
}

AudioModificationState* DeepSvcDocumentController::findAudioModificationByContentKey (const ContentKey& key)
{
    for (auto& state : audioModifications)
        if (state.contentIdentity == key)
            return &state;
    return nullptr;
}

const AudioModificationState* DeepSvcDocumentController::findAudioModificationByContentKey (const ContentKey& key) const
{
    for (const auto& state : audioModifications)
        if (state.contentIdentity == key)
            return &state;
    return nullptr;
}

AudioModificationState& DeepSvcDocumentController::ensureAudioModification (juce::ARAAudioModification* audioModification)
{
    if (auto* state = findAudioModification (audioModification))
        return *state;

    const juce::String pid (audioModification != nullptr ? juce::String (audioModification->getPersistentID())
                                                         : juce::String());
    if (pid.isNotEmpty())
        if (auto* byPid = findAudioModification (pid))
            debugLog ("ara shadowPidHitPointerMiss pid=" + pid
                      + " oldPtr=" + ptrHex (byPid->audioModification)
                      + " newPtr=" + ptrHex (audioModification)
                      + " " + shadowSummary (byPid));

    debugLog ("ara shadowCreateEmpty " + describeMod (audioModification));
    audioModifications.emplace_back();
    auto& state = audioModifications.back();
    state.updateIdentity (audioModification);
    state.contentIdentity = makeAudioModificationContentKey (state.persistentId);
    return state;
}

void DeepSvcDocumentController::attachSource (AudioModificationState& modification)
{
    if (modification.audioModification == nullptr)
        return;

    auto* audioSource = modification.audioModification->getAudioSource();
    if (audioSource == nullptr)
        return;

    const auto& sourceRef = ensureAudioSource (audioSource);

    if (! modification.hasContentState())
    {
        modification.content.emplace();
        modification.contentIdentity = bindAudioModificationIdentity (modification);
    }

    auto& content = *modification.content;
    content.sourceWindow.sourcePersistentId = sourceRef.persistentId;
    content.sourceWindow.sourceEndSeconds = sourceRef.durationSeconds();
}

ContentKey DeepSvcDocumentController::bindAudioModificationIdentity (AudioModificationState& modification)
{
    if (modification.contentIdentity.isValid())
        return modification.contentIdentity;
    modification.contentIdentity = makeAudioModificationContentKey (modification.persistentId);
    return modification.contentIdentity;
}

ContentKey DeepSvcDocumentController::makeAudioModificationContentKey (const juce::String& persistentId)
{
    ContentKey key;
    key.objectId = static_cast<uint64_t> (persistentId.hashCode64());
    return key;
}

PlaybackRegion* DeepSvcDocumentController::findPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    for (auto& region : playbackRegions)
        if (region.playbackRegion == playbackRegion)
            return &region;
    return nullptr;
}

const PlaybackRegion* DeepSvcDocumentController::findPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion) const
{
    for (const auto& region : playbackRegions)
        if (region.playbackRegion == playbackRegion)
            return &region;
    return nullptr;
}

PlaybackRegion& DeepSvcDocumentController::ensurePlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    if (auto* region = findPlaybackRegion (playbackRegion))
        return *region;

    playbackRegions.emplace_back();
    auto& region = playbackRegions.back();
    region.updateFrom (playbackRegion);
    return region;
}

bool DeepSvcDocumentController::removePlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    const auto before = playbackRegions.size();
    playbackRegions.erase (std::remove_if (playbackRegions.begin(), playbackRegions.end(),
                                           [playbackRegion] (const PlaybackRegion& region)
                                           {
                                               return region.playbackRegion == playbackRegion;
                                           }),
                           playbackRegions.end());
    return playbackRegions.size() != before;
}

//==============================================================================
// 投影构建

DeepSvcDocumentController::PlaybackRegionProjection
DeepSvcDocumentController::makeProjection (const PlaybackRegion& region) const
{
    PlaybackRegionProjection projection;
    projection.playbackRegion = region.playbackRegion;
    projection.audioModificationPersistentId = region.audioModificationPersistentId;
    projection.placementRevision = region.placementRevision;
    projection.startInPlaybackTime = region.startInPlaybackTime;
    projection.startInModificationTime = region.startInModificationTime;
    projection.durationInPlaybackTime = region.durationInPlaybackTime;
    projection.durationInModificationTime = region.durationInModificationTime;
    projection.timestretchEnabled = region.timestretchEnabled;
    projection.displayColour = region.displayColour;

    if (const auto* modification = findAudioModification (region.audioModificationPersistentId))
    {
        projection.contentKey = modification->contentIdentity;
        if (modification->hasContentState())
        {
            const auto& content = *modification->content;
            projection.contentRevision = content.contentRevision;
            projection.contentDurationSeconds = content.sourceWindow.durationSeconds();

            // 承载本片段的分段：片段窗口与分段区间重叠最大者
            if (const auto* segment = content.segmentOverlapping (projection.modificationRange()))
            {
                projection.segmentRange = segment->range;
                projection.segmentKey = SegmentKey {
                    modification->contentIdentity,
                    SegmentKey::microsFromSeconds (segment->range.startSeconds)
                };
                // 激活槽位有合成结果且未旁通时，回放走合成音频
                projection.hasRenderedAudio = segment->active().hasRenderedAudio()
                                           && ! segment->active().bypass;
            }
        }
    }
    return projection;
}

std::vector<DeepSvcDocumentController::PlaybackRegionProjection>
DeepSvcDocumentController::buildProjections() const
{
    std::vector<PlaybackRegionProjection> result;
    result.reserve (playbackRegions.size());
    for (const auto& region : playbackRegions)
        result.push_back (makeProjection (region));
    return result;
}

std::vector<DeepSvcPlaybackRenderer*> DeepSvcDocumentController::publishModelChange()
{
    return playbackRenderers;
}

void DeepSvcDocumentController::refreshRegisteredRenderers (const std::vector<DeepSvcPlaybackRenderer*>& renderers)
{
    for (auto* renderer : renderers)
        renderer->refreshFromDocument();
}

void DeepSvcDocumentController::reconcileEditorSelectionPlaybackRegions()
{
    editorSelectionPlaybackRegions.erase (
        std::remove_if (editorSelectionPlaybackRegions.begin(), editorSelectionPlaybackRegions.end(),
                        [this] (juce::ARAPlaybackRegion* region)
                        {
                            return findPlaybackRegion (region) == nullptr;
                        }),
        editorSelectionPlaybackRegions.end());
}

void DeepSvcDocumentController::notifyPersistedStateChanged (AudioModificationState& modification)
{
    // 对应 OpenTuneDocumentController.cpp 第 1877 行等的 notifyContentChanged 调用
    if (modification.audioModification != nullptr)
        modification.audioModification->notifyContentChanged (juce::ARAContentUpdateScopes(), true);
}

//==============================================================================
// 工厂

juce::ARAAudioModification* DeepSvcDocumentController::doCreateAudioModification (
    juce::ARAAudioSource* audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification* optionalModificationToClone)
{
    auto* created = new juce::ARAAudioModification (audioSource, hostRef, optionalModificationToClone);
    debugLog ("ara doCreateAudioModification created=" + ptrHex (created)
              + " clone=" + ptrHex (optionalModificationToClone)
              + " clonePid=" + pidOf (optionalModificationToClone)
              + " " + describeMod (optionalModificationToClone)
              + " src=" + ptrHex (audioSource)
              + " srcPid=" + pidOf (audioSource)
              + " srcModCount=" + juce::String (audioSource != nullptr
                    ? static_cast<int> (audioSource->getAudioModifications().size())
                    : -1));
    return created;
}

juce::ARAPlaybackRegion* DeepSvcDocumentController::doCreatePlaybackRegion (
    juce::ARAAudioModification* modification,
    ARA::ARAPlaybackRegionHostRef hostRef)
{
    auto* created = new juce::ARAPlaybackRegion (modification, hostRef);
    debugLog ("ara doCreatePlaybackRegion created=" + ptrHex (created)
              + " " + describeMod (modification));
    return created;
}

juce::ARAPlaybackRenderer* DeepSvcDocumentController::doCreatePlaybackRenderer()
{
    auto* renderer = new DeepSvcPlaybackRenderer (getDocumentController(), this);
    registerPlaybackRenderer (*renderer);
    renderer->refreshFromDocument();
    return renderer;
}

juce::ARAEditorView* DeepSvcDocumentController::doCreateEditorView()
{
    return new DeepSvcEditorView (getDocumentController(), *this);
}

} // namespace deepsvc

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<deepsvc::DeepSvcDocumentController>();
}
