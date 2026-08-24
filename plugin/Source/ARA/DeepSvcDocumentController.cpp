#include "DeepSvcDocumentController.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cstring>

#include "../DebugLog.h"
#include "../Utils/TimeCoordinate.h"
#include "DeepSvcEditorView.h"
#include "DeepSvcPlaybackRenderer.h"

// 对应 OpenTune Source/ARA/OpenTuneDocumentController.cpp
namespace deepsvc
{

namespace
{

constexpr int kArchiveMagic = 0x44535643;  // 'DSVC'
constexpr int kArchiveVersion = 0;
constexpr int kMaxArchiveRecords = 65536;

// f0 帧率：16kHz 采样、hop 160，即 100fps
constexpr double kF0FrameSeconds = 0.01;

// 归档中的 persistentID 经恢复过滤器映射为当前文档中的 ID
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

DeepSvcDocumentController::DeepSvcDocumentController (
    const ARA::PlugIn::PlugInEntry* entry,
    const ARA::ARADocumentControllerHostInstance* instance)
    : juce::ARADocumentControllerSpecialisation (entry, instance)
    , jobManager (*this)
{
}

DeepSvcDocumentController::~DeepSvcDocumentController()
{
    // 渲染器生命周期由 DC 驱动：先解除所有渲染器对 DC 的引用
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
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve (regions.size());

    for (auto* playbackRegion : regions)
    {
        const auto* region = findPlaybackRegion (playbackRegion);
        if (region != nullptr && region->hasValidPlacement())
            projections.push_back (makeProjection (*region));
    }

    return projections;
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
    std::vector<juce::ARAPlaybackRegion*> regions)
{
    editorSelectionPlaybackRegions = std::move (regions);
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

std::vector<DeepSvcPlaybackRenderer*> DeepSvcDocumentController::publishModelChange()
{
    return playbackRenderers;
}

void DeepSvcDocumentController::refreshRegisteredRenderers (
    const std::vector<DeepSvcPlaybackRenderer*>& renderers)
{
    for (auto* renderer : renderers)
        if (renderer != nullptr)
            renderer->refreshRenderPlanFromDocument();
}

void DeepSvcDocumentController::reconcileEditorSelectionPlaybackRegions()
{
    editorSelectionPlaybackRegions.erase (
        std::remove_if (editorSelectionPlaybackRegions.begin(),
                        editorSelectionPlaybackRegions.end(),
                        [this] (juce::ARAPlaybackRegion* playbackRegion)
                        {
                            return findPlaybackRegion (playbackRegion) == nullptr;
                        }),
        editorSelectionPlaybackRegions.end());
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

uint64_t DeepSvcDocumentController::readContentRevision (ContentKey key) const
{
    const auto* content = findContent (key);
    return content != nullptr ? content->contentRevision : 0;
}

juce::String DeepSvcDocumentController::readTimbreFile (ContentKey key) const
{
    const auto* content = findContent (key);
    return content != nullptr ? content->timbreFile : juce::String();
}

//==============================================================================
// 内容写入

void DeepSvcDocumentController::applyF0 (ContentKey key,
                                         std::vector<float> times,
                                         std::vector<float> values)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    content.f0Times = std::move (times);
    content.f0Values = std::move (values);
    ++content.contentRevision;
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyRenderedAudio (
    ContentKey key,
    std::shared_ptr<const std::vector<float>> samples,
    const juce::String& fingerprint)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    content.renderedAudio = std::move (samples);
    content.renderedFingerprint = fingerprint;
    ++content.contentRevision;
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyTimbreFile (ContentKey key, const juce::String& timbreFile)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    content.timbreFile = timbreFile;
    ++content.contentRevision;
}

//==============================================================================
// 任务入口

void DeepSvcDocumentController::requestDetect (ContentKey key,
                                               double contentStartSeconds,
                                               double contentDurationSeconds,
                                               EngineEstimator estimator)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr)
        return;

    const auto audio = ensureSourceAudio (*modification);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    // 只检测选中区域的内容窗口
    const auto rate = static_cast<double> (TimeCoordinate::kRenderSampleRate);
    const auto totalSamples = static_cast<int64_t> (audio->getNumSamples());
    const auto startSample = std::clamp<int64_t> (
        static_cast<int64_t> (std::floor (contentStartSeconds * rate)), 0, totalSamples);
    const auto endSample = std::clamp<int64_t> (
        static_cast<int64_t> (std::ceil ((contentStartSeconds + contentDurationSeconds) * rate)),
        startSample, totalSamples);
    if (endSample <= startSample)
        return;

    std::vector<float> pcm (static_cast<size_t> (endSample - startSample));
    std::memcpy (pcm.data(), audio->getReadPointer (0) + startSample, pcm.size() * sizeof (float));
    jobManager.submitDetect (key, std::move (pcm),
                             static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                             static_cast<double> (startSample) / rate, estimator);
}

void DeepSvcDocumentController::requestSynth (ContentKey key,
                                              const juce::String& timbreAbsolutePath,
                                              const EngineSynthParams& params,
                                              const juce::String& fingerprint)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr)
        return;

    const auto audio = ensureSourceAudio (*modification);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    std::vector<float> pcm (static_cast<size_t> (audio->getNumSamples()));
    std::memcpy (pcm.data(), audio->getReadPointer (0), pcm.size() * sizeof (float));

    if (jobManager.submitSynth (key, std::move (pcm),
                                static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                                timbreAbsolutePath, params) != 0)
        synthFingerprintsByKey[key] = fingerprint;
}

void DeepSvcDocumentController::cancelJobs (ContentKey key)
{
    jobManager.cancelJobsFor (key);
}

JobStatus DeepSvcDocumentController::jobStatusFor (ContentKey key) const
{
    return jobManager.statusFor (key);
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::ensureSourceAudio (AudioModificationState& modification)
{
    if (modification.hasContentState() && modification.content->sourceAudio != nullptr)
        return modification.content->sourceAudio;

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

    if (! modification.hasContentState())
    {
        ModificationContent content;
        if (const auto* sourceRef = findAudioSource (audioSource))
        {
            content.sourceWindow.sourcePersistentId = sourceRef->persistentId;
            content.sourceWindow.sourceEndSeconds = sourceRef->durationSeconds();
        }
        content.contentRevision = 1;
        modification.content = std::move (content);
    }

    modification.content->sourceAudio = mono;
    return mono;
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::readSourceAudio (ContentKey key)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr)
        return nullptr;
    return ensureSourceAudio (*modification);
}

void DeepSvcDocumentController::setAbBypass (bool bypass)
{
    if (abBypass == bypass)
        return;
    abBypass = bypass;
    refreshRegisteredRenderers (publishModelChange());
}

juce::String DeepSvcDocumentController::makeFingerprint (const EngineSynthParams& params,
                                                         const juce::String& timbreFile)
{
    juce::String text;
    text << (params.f0Estimator == EngineEstimator::fcpe ? "fcpe" : "rmvpe") << "|"
         << static_cast<int> (params.diffusionSteps) << "|"
         << juce::String (params.pitchShift, 2) << "|"
         << juce::String (params.cfgRate, 3) << "|"
         << juce::String (params.inputGainDb, 2) << "|"
         << (params.keepFirstVocoderOutput ? 1 : 0) << "|"
         << timbreFile;
    return text;
}

//==============================================================================
// JobManager::Listener（消息线程）

void DeepSvcDocumentController::jobStatusChanged (ContentKey, const JobStatus&)
{
    // 状态由编辑器轮询 jobStatusFor 读取，这里无需转发
}

void DeepSvcDocumentController::detectFinished (ContentKey key,
                                                double windowStartSeconds,
                                                std::vector<float> f0)
{
    std::vector<float> times (f0.size());
    for (size_t i = 0; i < f0.size(); ++i)
        times[i] = static_cast<float> (windowStartSeconds + static_cast<double> (i) * kF0FrameSeconds);
    applyF0 (key, std::move (times), std::move (f0));
}

void DeepSvcDocumentController::synthFinished (ContentKey key,
                                               std::vector<float> audio,
                                               std::vector<float> firstVocoder,
                                               std::vector<float> f0)
{
    // 输出声码器选项：请求了第一级声码器输出（level 1）则以它为渲染结果
    auto samples = std::make_shared<const std::vector<float>> (
        ! firstVocoder.empty() ? std::move (firstVocoder) : std::move (audio));

    // 指纹以提交时为准：requestSynth 记录，完成时取出
    juce::String fingerprint;
    if (const auto it = synthFingerprintsByKey.find (key); it != synthFingerprintsByKey.end())
    {
        fingerprint = it->second;
        synthFingerprintsByKey.erase (it);
    }

    applyRenderedAudio (key, std::move (samples), fingerprint);

    std::vector<float> times (f0.size());
    for (size_t i = 0; i < f0.size(); ++i)
        times[i] = static_cast<float> (static_cast<double> (i) * kF0FrameSeconds);
    applyF0 (key, std::move (times), std::move (f0));
}

//==============================================================================
// ARA 通知

void DeepSvcDocumentController::didUpdateAudioSourceProperties (juce::ARAAudioSource* audioSource)
{
    ensureAudioSource (audioSource).updateFrom (audioSource);
}

void DeepSvcDocumentController::doUpdateAudioSourceContent (juce::ARAAudioSource* audioSource,
                                                            const juce::ARAContentUpdateScopes scopeFlags)
{
    juce::ignoreUnused (scopeFlags);
    auto* source = findAudioSource (audioSource);
    if (source == nullptr)
        return;

    source->updateFrom (audioSource);

    // 源音频内容变化后，依赖它的检测结果与合成结果全部失效
    for (auto& modification : audioModifications)
    {
        if (modification.audioModification == nullptr
            || modification.audioModification->getAudioSource() != audioSource
            || ! modification.hasContentState())
            continue;

        auto& content = *modification.content;
        content.f0Times.clear();
        content.f0Values.clear();
        content.renderedAudio.reset();
        content.renderedFingerprint.clear();
        ++content.contentRevision;
    }
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioSource (juce::ARAAudioSource* audioSource)
{
    audioSources.erase (std::remove_if (audioSources.begin(), audioSources.end(),
                                        [audioSource] (const AudioSourceRef& source)
                                        {
                                            return source.audioSource == audioSource;
                                        }),
                        audioSources.end());
}

void DeepSvcDocumentController::didUpdateAudioModificationProperties (
    juce::ARAAudioModification* audioModification)
{
    auto& modification = ensureAudioModification (audioModification);
    modification.updateIdentity (audioModification);
    bindAudioModificationIdentity (modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioModification (
    juce::ARAAudioModification* audioModification)
{
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
                                              [audioModification] (const AudioModificationState& m)
                                              {
                                                  return m.audioModification == audioModification;
                                              }),
                              audioModifications.end());

    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didAddPlaybackRegionToAudioModification (
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    auto& modification = ensureAudioModification (audioModification);
    auto& region = ensurePlaybackRegion (playbackRegion);
    region.updateFrom (playbackRegion);
    if (region.audioModificationPersistentId.isEmpty())
        region.audioModificationPersistentId = modification.persistentId;
    debugLog ("dc didAddPlaybackRegion valid=" + juce::String (region.hasValidPlacement() ? 1 : 0)
              + " start=" + juce::String (region.startInPlaybackTime, 3)
              + " cached=" + juce::String (playbackRegions.size()));
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didUpdatePlaybackRegionProperties (juce::ARAPlaybackRegion* playbackRegion)
{
    auto& region = ensurePlaybackRegion (playbackRegion);
    region.updateFrom (playbackRegion);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willRemovePlaybackRegionFromAudioModification (
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    juce::ignoreUnused (audioModification);
    removePlaybackRegion (playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    removePlaybackRegion (playbackRegion);
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers (publishModelChange());
}

//==============================================================================
// 归档（版本 0，不兼容任何其他格式）

bool DeepSvcDocumentController::doRestoreObjectsFromStream (
    juce::ARAInputStream& input,
    const juce::ARARestoreObjectsFilter* filter)
{
    if (input.readInt() != kArchiveMagic)
        return false;
    if (input.readInt() != kArchiveVersion)
        return false;

    const int recordCount = input.readInt();
    if (recordCount < 0 || recordCount > kMaxArchiveRecords)
        return false;

    struct PendingRecord
    {
        AudioModificationState* target = nullptr;
        ModificationContent content;
    };
    std::vector<PendingRecord> pending;
    pending.reserve (static_cast<size_t> (recordCount));

    for (int i = 0; i < recordCount; ++i)
    {
        const auto archivedPersistentId = input.readString();
        const auto restoredPersistentId = mapRestoredPersistentId (archivedPersistentId, filter);
        if (restoredPersistentId.isEmpty())
            return false;

        auto* target = findAudioModification (restoredPersistentId);
        if (target == nullptr || target->audioModification == nullptr)
            return false;

        ModificationContent content;
        content.timbreFile = input.readString();
        content.renderedFingerprint = input.readString();

        const int f0Count = input.readInt();
        if (f0Count < 0)
            return false;
        content.f0Times.resize (static_cast<size_t> (f0Count));
        content.f0Values.resize (static_cast<size_t> (f0Count));
        if (f0Count > 0)
        {
            const auto byteCount = static_cast<size_t> (f0Count) * sizeof (float);
            if (input.read (content.f0Times.data(), static_cast<int> (byteCount)) != static_cast<int> (byteCount))
                return false;
            if (input.read (content.f0Values.data(), static_cast<int> (byteCount)) != static_cast<int> (byteCount))
                return false;
        }

        const int renderedCount = input.readInt();
        if (renderedCount < 0)
            return false;
        if (renderedCount > 0)
        {
            std::vector<float> samples (static_cast<size_t> (renderedCount));
            const auto byteCount = static_cast<size_t> (renderedCount) * sizeof (float);
            if (input.read (samples.data(), static_cast<int> (byteCount)) != static_cast<int> (byteCount))
                return false;
            content.renderedAudio = std::make_shared<const std::vector<float>> (std::move (samples));
        }

        // 内容窗口：整个源
        if (const auto* source = findAudioSource (target->audioModification->getAudioSource()))
        {
            content.sourceWindow.sourcePersistentId = source->persistentId;
            content.sourceWindow.sourceStartSeconds = 0.0;
            content.sourceWindow.sourceEndSeconds = source->durationSeconds();
        }
        content.contentRevision = 1;

        pending.push_back (PendingRecord { target, std::move (content) });
    }

    for (auto& record : pending)
        record.target->content = std::move (record.content);

    refreshRegisteredRenderers (publishModelChange());
    return true;
}

bool DeepSvcDocumentController::doStoreObjectsToStream (
    juce::ARAOutputStream& output,
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

    bool ok = output.writeInt (kArchiveMagic);
    ok = output.writeInt (kArchiveVersion) && ok;
    ok = output.writeInt (static_cast<int> (bindings.size())) && ok;

    for (const auto* modification : bindings)
    {
        const auto& content = *modification->content;
        ok = output.writeString (modification->persistentId) && ok;
        ok = output.writeString (content.timbreFile) && ok;
        ok = output.writeString (content.renderedFingerprint) && ok;

        const auto f0Count = static_cast<int> (content.f0Times.size());
        ok = output.writeInt (f0Count) && ok;
        if (f0Count > 0)
        {
            const auto byteCount = content.f0Times.size() * sizeof (float);
            ok = output.write (content.f0Times.data(), byteCount) && ok;
            ok = output.write (content.f0Values.data(), byteCount) && ok;
        }

        const auto renderedCount = content.renderedAudio != nullptr
            ? static_cast<int> (content.renderedAudio->size())
            : 0;
        ok = output.writeInt (renderedCount) && ok;
        if (renderedCount > 0)
            ok = output.write (content.renderedAudio->data(),
                               content.renderedAudio->size() * sizeof (float)) && ok;
    }

    return ok;
}

//==============================================================================
// 工厂

juce::ARAPlaybackRenderer* DeepSvcDocumentController::doCreatePlaybackRenderer()
{
    auto* renderer = new DeepSvcPlaybackRenderer (getDocumentController(), this);
    registerPlaybackRenderer (*renderer);
    renderer->refreshRenderPlanFromDocument();
    return renderer;
}

juce::ARAEditorView* DeepSvcDocumentController::doCreateEditorView()
{
    return new DeepSvcEditorView (getDocumentController(), *this);
}

//==============================================================================
// 模型查找

AudioSourceRef* DeepSvcDocumentController::findAudioSource (juce::ARAAudioSource* audioSource)
{
    const auto it = std::find_if (audioSources.begin(), audioSources.end(),
                                  [audioSource] (const AudioSourceRef& source)
                                  {
                                      return source.audioSource == audioSource;
                                  });
    return it != audioSources.end() ? &*it : nullptr;
}

AudioSourceRef* DeepSvcDocumentController::findAudioSource (const juce::String& persistentId)
{
    const auto it = std::find_if (audioSources.begin(), audioSources.end(),
                                  [&persistentId] (const AudioSourceRef& source)
                                  {
                                      return source.persistentId == persistentId;
                                  });
    return it != audioSources.end() ? &*it : nullptr;
}

const AudioSourceRef* DeepSvcDocumentController::findAudioSource (const juce::String& persistentId) const
{
    const auto it = std::find_if (audioSources.begin(), audioSources.end(),
                                  [&persistentId] (const AudioSourceRef& source)
                                  {
                                      return source.persistentId == persistentId;
                                  });
    return it != audioSources.end() ? &*it : nullptr;
}

AudioSourceRef& DeepSvcDocumentController::ensureAudioSource (juce::ARAAudioSource* audioSource)
{
    if (auto* existing = findAudioSource (audioSource))
        return *existing;

    AudioSourceRef source;
    source.updateFrom (audioSource);
    audioSources.push_back (std::move (source));
    return audioSources.back();
}

AudioModificationState* DeepSvcDocumentController::findAudioModification (const juce::String& persistentId)
{
    const auto it = std::find_if (audioModifications.begin(), audioModifications.end(),
                                  [&persistentId] (const AudioModificationState& m)
                                  {
                                      return m.persistentId == persistentId;
                                  });
    return it != audioModifications.end() ? &*it : nullptr;
}

const AudioModificationState* DeepSvcDocumentController::findAudioModification (
    const juce::String& persistentId) const
{
    const auto it = std::find_if (audioModifications.begin(), audioModifications.end(),
                                  [&persistentId] (const AudioModificationState& m)
                                  {
                                      return m.persistentId == persistentId;
                                  });
    return it != audioModifications.end() ? &*it : nullptr;
}

AudioModificationState* DeepSvcDocumentController::findAudioModification (
    juce::ARAAudioModification* audioModification)
{
    const auto it = std::find_if (audioModifications.begin(), audioModifications.end(),
                                  [audioModification] (const AudioModificationState& m)
                                  {
                                      return m.audioModification == audioModification;
                                  });
    return it != audioModifications.end() ? &*it : nullptr;
}

AudioModificationState* DeepSvcDocumentController::findAudioModificationByContentKey (const ContentKey& key)
{
    const auto it = std::find_if (audioModifications.begin(), audioModifications.end(),
                                  [&key] (const AudioModificationState& m)
                                  {
                                      return m.contentIdentity == key;
                                  });
    return it != audioModifications.end() ? &*it : nullptr;
}

const AudioModificationState* DeepSvcDocumentController::findAudioModificationByContentKey (
    const ContentKey& key) const
{
    const auto it = std::find_if (audioModifications.begin(), audioModifications.end(),
                                  [&key] (const AudioModificationState& m)
                                  {
                                      return m.contentIdentity == key;
                                  });
    return it != audioModifications.end() ? &*it : nullptr;
}

AudioModificationState& DeepSvcDocumentController::ensureAudioModification (
    juce::ARAAudioModification* audioModification)
{
    if (auto* existing = findAudioModification (audioModification))
        return *existing;

    AudioModificationState modification;
    modification.updateIdentity (audioModification);
    bindAudioModificationIdentity (modification);

    // 内容窗口：整个源
    if (auto* source = findAudioSource (audioModification->getAudioSource()))
    {
        ModificationContent content;
        content.sourceWindow.sourcePersistentId = source->persistentId;
        content.sourceWindow.sourceStartSeconds = 0.0;
        content.sourceWindow.sourceEndSeconds = source->durationSeconds();
        content.contentRevision = 1;
        modification.content = std::move (content);
    }

    audioModifications.push_back (std::move (modification));
    return audioModifications.back();
}

ContentKey DeepSvcDocumentController::makeAudioModificationContentKey (const juce::String& persistentId)
{
    if (persistentId.isEmpty())
        return {};

    const auto existingId = araObjectIdsByPersistentId.find (persistentId);
    if (existingId != araObjectIdsByPersistentId.end())
        return { DomainKind::ARAAudioModification, existingId->second };

    uint64_t objectId = static_cast<uint64_t> (persistentId.hashCode64());
    if (objectId == 0)
        objectId = 1469598103934665603ULL;

    while (true)
    {
        const auto existingPersistentId = araPersistentIdsByObjectId.find (objectId);
        if (existingPersistentId == araPersistentIdsByObjectId.end()
            || existingPersistentId->second == persistentId)
            break;

        objectId = objectId * 1099511628211ULL + 1469598103934665603ULL;
        if (objectId == 0)
            objectId = 1;
    }

    araPersistentIdsByObjectId[objectId] = persistentId;
    araObjectIdsByPersistentId[persistentId] = objectId;
    return { DomainKind::ARAAudioModification, objectId };
}

ContentKey DeepSvcDocumentController::bindAudioModificationIdentity (AudioModificationState& modification)
{
    modification.contentIdentity = makeAudioModificationContentKey (modification.persistentId);
    return modification.contentIdentity;
}

PlaybackRegion* DeepSvcDocumentController::findPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    const auto it = std::find_if (playbackRegions.begin(), playbackRegions.end(),
                                  [playbackRegion] (const PlaybackRegion& region)
                                  {
                                      return region.playbackRegion == playbackRegion;
                                  });
    return it != playbackRegions.end() ? &*it : nullptr;
}

const PlaybackRegion* DeepSvcDocumentController::findPlaybackRegion (
    juce::ARAPlaybackRegion* playbackRegion) const
{
    const auto it = std::find_if (playbackRegions.begin(), playbackRegions.end(),
                                  [playbackRegion] (const PlaybackRegion& region)
                                  {
                                      return region.playbackRegion == playbackRegion;
                                  });
    return it != playbackRegions.end() ? &*it : nullptr;
}

PlaybackRegion& DeepSvcDocumentController::ensurePlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    if (auto* existing = findPlaybackRegion (playbackRegion))
        return *existing;

    PlaybackRegion region;
    region.updateFrom (playbackRegion);
    playbackRegions.push_back (std::move (region));
    return playbackRegions.back();
}

bool DeepSvcDocumentController::removePlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    const auto oldSize = playbackRegions.size();
    playbackRegions.erase (std::remove_if (playbackRegions.begin(), playbackRegions.end(),
                                           [playbackRegion] (const PlaybackRegion& region)
                                           {
                                               return region.playbackRegion == playbackRegion;
                                           }),
                           playbackRegions.end());
    return playbackRegions.size() != oldSize;
}

DeepSvcDocumentController::PlaybackRegionProjection
DeepSvcDocumentController::makeProjection (const PlaybackRegion& placement) const
{
    PlaybackRegionProjection projection;
    projection.playbackRegion = placement.playbackRegion;
    projection.audioModificationPersistentId = placement.audioModificationPersistentId;
    projection.placementRevision = placement.placementRevision;
    projection.startInPlaybackTime = placement.startInPlaybackTime;
    projection.startInModificationTime = placement.startInModificationTime;
    projection.durationInPlaybackTime = placement.durationInPlaybackTime;
    projection.durationInModificationTime = placement.durationInModificationTime;
    projection.timestretchEnabled = placement.timestretchEnabled;
    projection.displayColour = placement.displayColour;

    const auto* modification = findAudioModification (placement.audioModificationPersistentId);
    if (modification == nullptr || ! modification->hasContentState())
        return projection;

    const auto& content = *modification->content;
    projection.contentKey = modification->contentKey();
    projection.contentRevision = content.contentRevision;
    projection.contentDurationSeconds = content.sourceWindow.durationSeconds();
    projection.hasRenderedAudio = content.renderedAudio != nullptr && ! content.renderedAudio->empty();

    return projection;
}

std::vector<DeepSvcDocumentController::PlaybackRegionProjection>
DeepSvcDocumentController::buildProjections() const
{
    std::vector<PlaybackRegionProjection> projections;
    projections.reserve (playbackRegions.size());

    for (const auto& region : playbackRegions)
        if (region.hasValidPlacement())
            projections.push_back (makeProjection (region));

    return projections;
}

} // namespace deepsvc

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<deepsvc::DeepSvcDocumentController>();
}
