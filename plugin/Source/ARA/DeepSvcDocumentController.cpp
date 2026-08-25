#include "DeepSvcDocumentController.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../DebugLog.h"
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
} // namespace

DeepSvcDocumentController::DeepSvcDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                                                      const ARA::ARADocumentControllerHostInstance* instance)
    : ARADocumentControllerSpecialisation (entry, instance)
    , jobManager (*this)
{
}

DeepSvcDocumentController::~DeepSvcDocumentController()
{
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

uint64_t DeepSvcDocumentController::readContentRevision (ContentKey key) const
{
    const auto* content = findContent (key);
    return content != nullptr ? content->contentRevision : 0;
}

int DeepSvcDocumentController::readActiveSlot (ContentKey key) const
{
    const auto* content = findContent (key);
    return content != nullptr ? content->activeSlot : 0;
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::readSourceAudio (ContentKey key, int slot)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr)
        return nullptr;
    return ensureSourceAudio (*modification, slot);
}

//==============================================================================
// 槽位状态（A/B，docs/ara.md 第 4.1 节）

void DeepSvcDocumentController::setActiveSlot (ContentKey key, int slot)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    slot = juce::jlimit (0, 1, slot);
    auto& content = *modification->content;
    if (content.activeSlot == slot)
        return;

    content.activeSlot = slot;
    ++content.contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::setSlotBypass (ContentKey key, int slot, bool bypass)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    slot = juce::jlimit (0, 1, slot);
    if (content.slots[static_cast<size_t> (slot)].bypass == bypass)
        return;

    content.slots[static_cast<size_t> (slot)].bypass = bypass;
    ++content.contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyTimbreFile (ContentKey key, int slot, const juce::String& timbreFile)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    slot = juce::jlimit (0, 1, slot);
    content.slots[static_cast<size_t> (slot)].timbreFile = timbreFile;
    notifyPersistedStateChanged (*modification);
}

void DeepSvcDocumentController::applySlotParams (ContentKey key, int slot, const EngineSynthParams& params)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    slot = juce::jlimit (0, 1, slot);
    content.slots[static_cast<size_t> (slot)].params = params;
    notifyPersistedStateChanged (*modification);
}

//==============================================================================
// 内容写入（任务完成时调用）

void DeepSvcDocumentController::applyF0 (ContentKey key,
                                         int slot,
                                         std::vector<float> times,
                                         std::vector<float> values)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    slot = juce::jlimit (0, 1, slot);
    auto& slotContent = content.slots[static_cast<size_t> (slot)];
    slotContent.f0Times = std::move (times);
    slotContent.f0Values = std::move (values);
    ++content.contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyRenderedAudio (ContentKey key,
                                                    int slot,
                                                    std::shared_ptr<const std::vector<float>> samples,
                                                    const EngineSynthParams& synthParams,
                                                    const juce::String& synthTimbreFile)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr || ! modification->hasContentState())
        return;

    auto& content = *modification->content;
    slot = juce::jlimit (0, 1, slot);
    auto& slotContent = content.slots[static_cast<size_t> (slot)];
    slotContent.renderedAudio = std::move (samples);
    slotContent.synthParams = synthParams;
    slotContent.synthTimbreFile = synthTimbreFile;
    ++content.contentRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

//==============================================================================
// 任务入口

void DeepSvcDocumentController::requestDetect (ContentKey key,
                                               int slot,
                                               EngineEstimator estimator)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    const auto audio = ensureSourceAudio (*modification, slot);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    // OpenTune 模型：检测覆盖修改的整个源内容，同源的所有音频块共享一份结果，
    // 显示时由钢琴卷按选中音频块的内容窗口裁剪
    std::vector<float> pcm (static_cast<size_t> (audio->getNumSamples()));
    std::memcpy (pcm.data(), audio->getReadPointer (0), pcm.size() * sizeof (float));
    jobManager.submitDetect ({ key, slot }, std::move (pcm),
                             static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                             estimator);
}

void DeepSvcDocumentController::requestSynth (ContentKey key,
                                              int slot,
                                              const juce::String& timbreAbsolutePath,
                                              const EngineSynthParams& params)
{
    auto* modification = findAudioModificationByContentKey (key);
    if (modification == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    const auto audio = ensureSourceAudio (*modification, slot);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    std::vector<float> pcm (static_cast<size_t> (audio->getNumSamples()));
    std::memcpy (pcm.data(), audio->getReadPointer (0), pcm.size() * sizeof (float));

    const JobKey jobKey { key, slot };
    pendingSynthParams[jobKey] = params;
    if (const auto* modificationContent = findContent (key))
        pendingSynthTimbres[jobKey] = modificationContent->slots[static_cast<size_t> (slot)].timbreFile;

    dumpDebugWav ("synth_input", pcm.data(), pcm.size());
    debugLog ("requestSynth slot=" + juce::String (slot)
              + " samples=" + juce::String (static_cast<int64_t> (pcm.size()))
              + " estimator=" + juce::String (static_cast<uint32_t> (params.f0Estimator))
              + " steps=" + juce::String (params.diffusionSteps)
              + " shift=" + juce::String (params.pitchShift)
              + " cfg=" + juce::String (params.cfgRate)
              + " gain=" + juce::String (params.inputGainDb)
              + " keepFirst=" + juce::String (params.keepFirstVocoderOutput ? 1 : 0)
              + " timbre=" + timbreAbsolutePath);

    jobManager.submitSynth (jobKey, std::move (pcm),
                            static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                            timbreAbsolutePath, params);
}

void DeepSvcDocumentController::cancelJobs (ContentKey key, int slot)
{
    jobManager.cancelJobsFor ({ key, juce::jlimit (0, 1, slot) });
}

JobStatus DeepSvcDocumentController::jobStatusFor (ContentKey key, int slot) const
{
    return jobManager.statusFor ({ key, juce::jlimit (0, 1, slot) });
}

//==============================================================================
// 源音频提取：modification 整个源窗口 → 44.1kHz 单声道，按槽位缓存

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::ensureSourceAudio (AudioModificationState& modification, int slot)
{
    attachSource (modification);
    if (! modification.hasContentState())
        return nullptr;

    auto& content = *modification.content;
    slot = juce::jlimit (0, 1, slot);
    auto& slotContent = content.slots[static_cast<size_t> (slot)];
    if (slotContent.sourceAudio != nullptr)
        return slotContent.sourceAudio;

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

    slotContent.sourceAudio = mono;
    return mono;
}

//==============================================================================
// ARA 通知

void DeepSvcDocumentController::didUpdateAudioSourceProperties (juce::ARAAudioSource* audioSource)
{
    auto& sourceRef = ensureAudioSource (audioSource);
    sourceRef.updateFrom (audioSource);

    // 源形状变化（替换音频等）后，槽位的源音频缓存失效
    for (auto& modification : audioModifications)
    {
        if (! modification.hasContentState()
            || modification.content->sourceWindow.sourcePersistentId != sourceRef.persistentId)
            continue;
        attachSource (modification);
        for (auto& slot : modification.content->slots)
            slot.sourceAudio.reset();
    }
}

void DeepSvcDocumentController::doUpdateAudioSourceContent (juce::ARAAudioSource* audioSource,
                                                            const juce::ARAContentUpdateScopes scopeFlags)
{
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
        for (auto& slot : content.slots)
        {
            slot.sourceAudio.reset();
            slot.renderedAudio.reset();
            slot.f0Times.clear();
            slot.f0Values.clear();
        }
        ++content.contentRevision;
    }

    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioSource (juce::ARAAudioSource* audioSource)
{
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

void DeepSvcDocumentController::didUpdateAudioModificationProperties (juce::ARAAudioModification* audioModification)
{
    auto& modification = ensureAudioModification (audioModification);
    modification.updateIdentity (audioModification);
    attachSource (modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioModification (juce::ARAAudioModification* audioModification)
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
    auto& modification = ensureAudioModification (audioModification);
    auto& region = ensurePlaybackRegion (playbackRegion);
    region.updateFrom (playbackRegion);
    if (region.audioModificationPersistentId.isEmpty())
        region.audioModificationPersistentId = modification.persistentId;
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource,
                                                                   bool enable)
{
    if (! enable)
        return;

    // 采样访问开启后立刻提取源音频缓存（两个槽位），保证后续检测/合成/回放可用
    for (auto& modification : audioModifications)
    {
        if (! modification.hasContentState())
            continue;
        if (modification.content->sourceWindow.sourcePersistentId.isEmpty())
            continue;
        const auto* sourceRef = findAudioSource (modification.content->sourceWindow.sourcePersistentId);
        if (sourceRef == nullptr || sourceRef->audioSource != audioSource)
            continue;
        ensureSourceAudio (modification, 0);
        ensureSourceAudio (modification, 1);
    }
}

void DeepSvcDocumentController::didUpdateRegionSequenceProperties (juce::ARARegionSequence* regionSequence)
{
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

void DeepSvcDocumentController::didUpdatePlaybackRegionProperties (juce::ARAPlaybackRegion* playbackRegion)
{
    auto& region = ensurePlaybackRegion (playbackRegion);
    region.updateFrom (playbackRegion);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willRemovePlaybackRegionFromAudioModification (
    juce::ARAAudioModification*,
    juce::ARAPlaybackRegion* playbackRegion)
{
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
// 归档：persistentID + JSON 字符串（docs/ara.md 第 4.2 节）。
// 与 OpenTune 的差异：OpenTune 记录 XML、渲染结果不入库、恢复后由渲染服务重建；
// 本插件的推理结果（音高、合成音频）直接入库，这是设计选择。

namespace
{

constexpr int kArchiveMagic = 0x44535643;  // 'DSVC'
constexpr int kArchiveVersion = 0;
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

juce::var floatVectorToJson (const std::vector<float>& values)
{
    juce::Array<juce::var> array;
    array.ensureStorageAllocated (static_cast<int> (values.size()));
    for (const float value : values)
        array.add (static_cast<double> (value));
    return juce::var (array);
}

std::vector<float> floatVectorFromJson (const juce::var& json)
{
    std::vector<float> values;
    if (const auto* array = json.getArray())
    {
        values.reserve (static_cast<size_t> (array->size()));
        for (const auto& element : *array)
            values.push_back (static_cast<float> (static_cast<double> (element)));
    }
    return values;
}

juce::var slotToJson (const SlotContent& slot)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("params", parameters::synthParamsToJson (slot.params));
    object->setProperty ("timbreFile", slot.timbreFile);
    object->setProperty ("bypass", slot.bypass);
    object->setProperty ("f0Times", floatVectorToJson (slot.f0Times));
    object->setProperty ("f0Values", floatVectorToJson (slot.f0Values));
    if (slot.hasRenderedAudio())
        object->setProperty ("renderedAudio", floatVectorToJson (*slot.renderedAudio));
    if (slot.hasRenderedAudio())
    {
        object->setProperty ("synthParams", parameters::synthParamsToJson (slot.synthParams));
        object->setProperty ("synthTimbreFile", slot.synthTimbreFile);
    }
    return juce::var (object);
}

void slotFromJson (SlotContent& slot, const juce::var& json)
{
    slot.params = parameters::synthParamsFromJson (json.getProperty ("params", juce::var()));
    slot.timbreFile = json.getProperty ("timbreFile", juce::String()).toString();
    slot.bypass = static_cast<bool> (json.getProperty ("bypass", false));
    slot.f0Times = floatVectorFromJson (json.getProperty ("f0Times", juce::var()));
    slot.f0Values = floatVectorFromJson (json.getProperty ("f0Values", juce::var()));

    const auto rendered = floatVectorFromJson (json.getProperty ("renderedAudio", juce::var()));
    if (! rendered.empty())
    {
        slot.renderedAudio = std::make_shared<const std::vector<float>> (std::move (rendered));
        slot.synthParams = parameters::synthParamsFromJson (json.getProperty ("synthParams", juce::var()));
        slot.synthTimbreFile = json.getProperty ("synthTimbreFile", juce::String()).toString();
    }
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

    bool ok = output.writeInt (kArchiveMagic);
    ok = output.writeInt (kArchiveVersion) && ok;
    ok = output.writeInt (static_cast<int> (bindings.size())) && ok;

    for (const auto* modification : bindings)
    {
        const auto& content = *modification->content;

        auto* root = new juce::DynamicObject();
        root->setProperty ("activeSlot", content.activeSlot);
        auto* window = new juce::DynamicObject();
        window->setProperty ("start", content.sourceWindow.sourceStartSeconds);
        window->setProperty ("end", content.sourceWindow.sourceEndSeconds);
        root->setProperty ("sourceWindow", juce::var (window));
        juce::Array<juce::var> slotsJson;
        for (const auto& slot : content.slots)
            slotsJson.add (slotToJson (slot));
        root->setProperty ("slots", juce::var (slotsJson));

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

        // 单条记录映射不上或损坏时跳过该条，对应 OpenTune restoreAudioModificationContent 的逐条容错
        const auto restoredPersistentId = mapRestoredPersistentId (archivedPersistentId, filter);
        if (restoredPersistentId.isEmpty())
            continue;

        auto* modification = findAudioModification (restoredPersistentId);
        if (modification == nullptr || modification->audioModification == nullptr)
            continue;

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
            continue;

        content.activeSlot = juce::jlimit (0, 1, static_cast<int> (json.getProperty ("activeSlot", 0)));
        if (const auto* slotsJson = json.getProperty ("slots", juce::var()).getArray())
            for (int s = 0; s < juce::jmin (2, slotsJson->size()); ++s)
                slotFromJson (content.slots[static_cast<size_t> (s)], (*slotsJson)[s]);

        ++content.contentRevision;
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
    applyF0 (key.content, key.slot, std::move (times), std::move (f0));
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

    applyRenderedAudio (key.content, key.slot,
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
            // 激活槽位有合成结果且未旁通时，回放走合成音频
            projection.hasRenderedAudio = content.active().hasRenderedAudio() && ! content.active().bypass;
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
