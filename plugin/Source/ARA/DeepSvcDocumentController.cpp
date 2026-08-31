#include "DeepSvcDocumentController.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../Content/ContentArchive.h"
#include "../DebugLog.h"
#include "../State/Parameters.h"
#include "../Utils/Resample.h"
#include "../Utils/TimeCoordinate.h"
#include "DeepSvcEditorView.h"
#include "DeepSvcPlaybackRenderer.h"

namespace deepsvc
{

namespace
{

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

std::vector<float> extractWorkingRangePcm (const juce::AudioBuffer<float>& sourceAudio,
                                          const WorkingRange& range)
{
    const auto totalSamples = static_cast<int64_t> (sourceAudio.getNumSamples());
    if (totalSamples <= 0 || ! range.isValid())
        return {};

    const auto rate = TimeCoordinate::kRenderSampleRate;
    const auto begin = juce::jlimit<int64_t> (0, totalSamples,
                                              static_cast<int64_t> (range.startSeconds * rate));
    const auto end = juce::jlimit<int64_t> (begin, totalSamples,
                                            static_cast<int64_t> (range.endSeconds * rate));
    const auto count = static_cast<size_t> (end - begin);
    if (count == 0)
        return {};

    std::vector<float> pcm (count);
    std::memcpy (pcm.data(),
                 sourceAudio.getReadPointer (0, static_cast<int> (begin)),
                 count * sizeof (float));
    return pcm;
}

std::vector<float> f0TimesForWorkingRange (double startSeconds, size_t frameCount)
{
    std::vector<float> times (frameCount);
    for (size_t i = 0; i < frameCount; ++i)
        times[i] = static_cast<float> (startSeconds + static_cast<double> (i) * 0.01);
    return times;
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

juce::String regionSequenceNameOf (const juce::ARARegionSequence* regionSequence)
{
    if (regionSequence == nullptr)
        return "-";
    const juce::String name (regionSequence->getName());
    return name.isNotEmpty() ? name : juce::String ("-");
}

juce::String describeRegion (const juce::ARAPlaybackRegion* region)
{
    if (region == nullptr)
        return "region p=null";

    const auto* audioModification = region->getAudioModification();
    const auto* regionSequence = region->getRegionSequence();

    return "region p=" + ptrHex (region)
         + " mod=" + ptrHex (audioModification)
         + " modPid=" + pidOf (audioModification)
         + " seq=" + ptrHex (regionSequence)
         + " seqName=" + regionSequenceNameOf (regionSequence)
         + " win=" + juce::String (region->getStartInAudioModificationTime(), 6)
         + "+" + juce::String (region->getDurationInAudioModificationTime(), 6)
         + " place=" + juce::String (region->getStartInPlaybackTime(), 6)
         + "+" + juce::String (region->getDurationInPlaybackTime(), 6);
}

juce::String describeAudioModification (const juce::ARAAudioModification* audioModification)
{
    if (audioModification == nullptr)
        return "mod p=null";

    const auto* audioSource = audioModification->getAudioSource();
    const auto& regions = audioModification->getPlaybackRegions();
    return "mod p=" + ptrHex (audioModification)
         + " pid=" + pidOf (audioModification)
         + " src=" + ptrHex (audioSource)
         + " srcPid=" + pidOf (audioSource)
         + " deactivated=" + juce::String (audioModification->isDeactivatedForUndoHistory() ? 1 : 0)
         + " regions=" + juce::String (static_cast<int> (regions.size()));
}

juce::String modificationSummary (const DeepSvcAudioModification* modification)
{
    if (modification == nullptr)
        return "modification=miss";

    int rendered = 0;
    int pitched = 0;
    for (const auto& slot : modification->slots)
    {
        if (slot.hasSynthAudio())
            ++rendered;
        if (slot.pitchData.has_value() && ! slot.pitchData->f0Values.empty())
            ++pitched;
    }

    return "modification pid=" + pidOf (modification)
         + " rev=" + juce::String (static_cast<juce::int64> (modification->dataRevision))
         + " pitchedSlots=" + juce::String (pitched)
         + " renderedSlots=" + juce::String (rendered);
}

std::optional<juce::Colour> colourOf (const juce::ARAPlaybackRegion* region)
{
    if (region == nullptr)
        return std::nullopt;
    if (const ARA::ARAColor* color = region->getEffectiveColor())
        return juce::Colour::fromFloatRGBA (color->r, color->g, color->b, 1.0f);
    return std::nullopt;
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
    for (auto* renderer : playbackRenderers)
        if (renderer != nullptr)
            renderer->detachDocumentController (*this);
    playbackRenderers.clear();
}

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
    for (auto* region : regions)
        if (region != nullptr)
            result.push_back (makeProjection (region));
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

void DeepSvcDocumentController::fillSynthPlayback (SynthAudio& synth) const
{
    if (synth.engineSamples == nullptr || synth.engineSamples->empty())
        std::abort();

    if (hostSampleRate == TimeCoordinate::kRenderSampleRate)
    {
        synth.samples = synth.engineSamples;
        synth.sampleRate = hostSampleRate;
        return;
    }

    auto resampled = resampleMono (synth.engineSamples->data(),
                                   synth.engineSamples->size(),
                                   TimeCoordinate::kRenderSampleRate,
                                   hostSampleRate);
    debugLog ("synth playback resample engine="
              + juce::String (static_cast<int64_t> (synth.engineSamples->size()))
              + " hostSr=" + juce::String (hostSampleRate, 1)
              + " playback=" + juce::String (static_cast<int64_t> (resampled.size())));
    synth.samples = std::make_shared<const std::vector<float>> (std::move (resampled));
    synth.sampleRate = hostSampleRate;
}

void DeepSvcDocumentController::rebuildAllSynthPlayback()
{
    forEachModification ([this] (DeepSvcAudioModification& modification)
    {
        for (auto& slot : modification.slots)
            if (slot.synthAudio.has_value() && slot.synthAudio->isValid())
                fillSynthPlayback (*slot.synthAudio);
    });
}

void DeepSvcDocumentController::setHostSampleRate (double sampleRate)
{
    if (!(sampleRate > 0.0))
        std::abort();
    if (hostSampleRate == sampleRate)
        return;
    hostSampleRate = sampleRate;
    rebuildAllSynthPlayback();
    refreshRegisteredRenderers (publishModelChange());
}

DeepSvcAudioModification* DeepSvcDocumentController::asModification (juce::ARAAudioModification* audioModification) const
{
    return dynamic_cast<DeepSvcAudioModification*> (audioModification);
}

const DeepSvcAudioModification* DeepSvcDocumentController::asModification (
    const juce::ARAAudioModification* audioModification) const
{
    return dynamic_cast<const DeepSvcAudioModification*> (audioModification);
}

void DeepSvcDocumentController::forEachModification (const std::function<void (DeepSvcAudioModification&)>& fn)
{
    auto* document = getDocument();
    if (document == nullptr)
        return;
    for (auto* source : document->getAudioSources())
        for (auto* modification : source->getAudioModifications<DeepSvcAudioModification>())
            if (modification != nullptr)
                fn (*modification);
}

void DeepSvcDocumentController::forEachModification (const std::function<void (const DeepSvcAudioModification&)>& fn) const
{
    const_cast<DeepSvcDocumentController*> (this)->forEachModification (
        [&] (DeepSvcAudioModification& modification) { fn (modification); });
}

DeepSvcAudioModification* DeepSvcDocumentController::findModification (ContentKey key)
{
    DeepSvcAudioModification* found = nullptr;
    forEachModification ([&] (DeepSvcAudioModification& modification)
    {
        if (modification.contentKey == key)
            found = &modification;
    });
    return found;
}

const DeepSvcAudioModification* DeepSvcDocumentController::findModification (ContentKey key) const
{
    const DeepSvcAudioModification* found = nullptr;
    forEachModification ([&] (const DeepSvcAudioModification& modification)
    {
        if (modification.contentKey == key)
            found = &modification;
    });
    return found;
}

DeepSvcAudioModification* DeepSvcDocumentController::findModificationByPersistentId (const juce::String& persistentId)
{
    if (persistentId.isEmpty())
        return nullptr;
    DeepSvcAudioModification* found = nullptr;
    forEachModification ([&] (DeepSvcAudioModification& modification)
    {
        if (pidOf (&modification) == persistentId)
            found = &modification;
    });
    return found;
}

const DeepSvcAudioModification* DeepSvcDocumentController::findModificationByPersistentId (const juce::String& persistentId) const
{
    if (persistentId.isEmpty())
        return nullptr;
    const DeepSvcAudioModification* found = nullptr;
    forEachModification ([&] (const DeepSvcAudioModification& modification)
    {
        if (pidOf (&modification) == persistentId)
            found = &modification;
    });
    return found;
}

uint64_t DeepSvcDocumentController::readContentRevision (ContentKey key) const
{
    const auto* modification = findModification (key);
    return modification != nullptr ? modification->dataRevision : 0;
}

int DeepSvcDocumentController::readActiveSlot (ContentKey key) const
{
    const auto* modification = findModification (key);
    return modification != nullptr ? modification->activeSlot : 0;
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::readSourceAudio (ContentKey key)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return nullptr;
    return ensureSourceAudio (*modification);
}

void DeepSvcDocumentController::setActiveSlot (ContentKey key, int slot)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    if (modification->activeSlot == slot)
        return;

    modification->activeSlot = slot;
    ++modification->dataRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::setSlotBypass (ContentKey key, int slot, bool bypass)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return;

    auto& slotContent = modification->slotAt (slot);
    if (slotContent.bypass == bypass)
        return;

    slotContent.bypass = bypass;
    ++modification->dataRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyF0 (ContentKey key, int slot,
                                         std::vector<float> times, std::vector<float> values)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return;

    PitchData pitch;
    pitch.f0Times = std::move (times);
    pitch.f0Values = std::move (values);
    modification->slotAt (slot).pitchData = std::move (pitch);
    ++modification->dataRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyRenderedAudio (ContentKey key,
                                                    int slot,
                                                    std::shared_ptr<const std::vector<float>> samples,
                                                    double synthStartTime,
                                                    double synthEndTime,
                                                    const EngineSynthParams& synthParams,
                                                    const juce::String& synthTimbreFile,
                                                    double elapsedSeconds)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return;

    SynthAudio synth;
    synth.engineSamples = std::move (samples);
    synth.synthStartTime = synthStartTime;
    synth.synthEndTime = synthEndTime;
    fillSynthPlayback (synth);
    auto& slotContent = modification->slotAt (slot);
    slotContent.synthAudio = std::move (synth);
    slotContent.synthParams = synthParams;
    slotContent.synthTimbreFile = synthTimbreFile;
    if (elapsedSeconds >= 0.0)
        slotContent.lastSynthElapsedSeconds = elapsedSeconds;
    ++modification->dataRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::clearSynthAudio (ContentKey key, int slot)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return;

    auto& slotContent = modification->slotAt (slot);
    if (! slotContent.hasSynthAudio() && ! slotContent.lastSynthElapsedSeconds.has_value())
        return;

    slotContent.synthAudio.reset();
    slotContent.synthParams = {};
    slotContent.synthTimbreFile = {};
    slotContent.lastSynthElapsedSeconds.reset();
    slotContent.bypass = false;
    ++modification->dataRevision;
    notifyPersistedStateChanged (*modification);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::requestDetect (ContentKey key,
                                               int slot,
                                               EngineEstimator estimator)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return;

    const auto range = modification->workingRange();
    const auto audio = ensureSourceAudio (*modification);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    auto pcm = extractWorkingRangePcm (*audio, range);
    if (pcm.empty())
        return;

    slot = juce::jlimit (0, 1, slot);
    const JobKey jobKey { juce::String (modification->getPersistentID()), slot };
    pendingDetectRanges[jobKey] = range;
    debugLog ("requestDetect slot=" + juce::String (slot)
              + " rangeStart=" + juce::String (range.startSeconds, 3)
              + " rangeEnd=" + juce::String (range.endSeconds, 3)
              + " samples=" + juce::String (static_cast<int64_t> (pcm.size())));
    jobManager.submitDetect (jobKey, std::move (pcm),
                             static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                             estimator);
}

void DeepSvcDocumentController::requestSynth (ContentKey key,
                                              int slot,
                                              const juce::String& timbreAbsolutePath,
                                              const EngineSynthParams& params)
{
    auto* modification = findModification (key);
    if (modification == nullptr)
        return;

    const auto range = modification->workingRange();
    const auto audio = ensureSourceAudio (*modification);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    auto pcm = extractWorkingRangePcm (*audio, range);
    if (pcm.empty())
        return;

    slot = juce::jlimit (0, 1, slot);
    const JobKey jobKey { juce::String (modification->getPersistentID()), slot };
    pendingSynthParams[jobKey] = params;
    pendingSynthTimbres[jobKey] = juce::File (timbreAbsolutePath).getFileName();
    pendingSynthRanges[jobKey] = range;

    dumpDebugWav ("synth_input", pcm.data(), pcm.size());
    debugLog ("requestSynth slot=" + juce::String (slot)
              + " rangeStart=" + juce::String (range.startSeconds, 3)
              + " rangeEnd=" + juce::String (range.endSeconds, 3)
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

void DeepSvcDocumentController::cancelJobs (ContentKey key, int slot)
{
    const auto* modification = findModification (key);
    if (modification == nullptr)
        return;
    jobManager.cancelJobsFor ({ juce::String (modification->getPersistentID()), juce::jlimit (0, 1, slot) });
}

JobStatus DeepSvcDocumentController::jobStatusFor (ContentKey key, int slot) const
{
    const auto* modification = findModification (key);
    if (modification == nullptr)
        return {};
    return jobManager.statusFor ({ juce::String (modification->getPersistentID()), juce::jlimit (0, 1, slot) });
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::ensureSourceAudio (DeepSvcAudioModification& modification)
{
    if (modification.sourceAudio != nullptr)
        return modification.sourceAudio;

    auto* audioSource = modification.getAudioSource();
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

    const double ratio = sourceRate / TimeCoordinate::kRenderSampleRate;
    const int outputSamples = static_cast<int> (static_cast<double> (numSamples) / ratio);

    auto mono = std::make_shared<juce::AudioBuffer<float>> (1, juce::jmax (1, outputSamples));
    mono->clear();

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

    modification.sourceAudio = mono;
    ++modification.dataRevision;
    return mono;
}

void DeepSvcDocumentController::dumpAraGraph (const juce::String& reason)
{
    auto* document = getDocument();
    int modificationCount = 0;
    int regionCount = 0;
    if (document != nullptr)
        for (auto* source : document->getAudioSources())
            for (auto* audioModification : source->getAudioModifications())
            {
                ++modificationCount;
                regionCount += static_cast<int> (audioModification->getPlaybackRegions().size());
            }

    debugLog ("ara dump begin reason=" + reason
              + " mods=" + juce::String (modificationCount)
              + " regions=" + juce::String (regionCount));

    if (document == nullptr)
    {
        debugLog ("ara dump document=null");
        return;
    }

    int hostModificationCount = 0;
    int hostRegionCount = 0;
    for (auto* source : document->getAudioSources())
    {
        const auto& audioModifications = source->getAudioModifications();
        debugLog ("ara dump src p=" + ptrHex (source)
                  + " pid=" + pidOf (source)
                  + " deactivated=" + juce::String (source->isDeactivatedForUndoHistory() ? 1 : 0)
                  + " mods=" + juce::String (static_cast<int> (audioModifications.size())));
        for (auto* audioModification : audioModifications)
        {
            ++hostModificationCount;
            debugLog ("ara dump   " + describeAudioModification (audioModification)
                      + " " + modificationSummary (asModification (audioModification)));
            for (auto* region : audioModification->getPlaybackRegions())
            {
                ++hostRegionCount;
                debugLog ("ara dump     " + describeRegion (region));
            }
        }
    }

    debugLog ("ara dump end hostMods=" + juce::String (hostModificationCount)
              + " hostRegions=" + juce::String (hostRegionCount));
}

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
    forEachModification ([&] (DeepSvcAudioModification& modification)
    {
        if (modification.getAudioSource() == audioSource)
            modification.sourceAudio.reset();
    });
}

void DeepSvcDocumentController::doUpdateAudioSourceContent (juce::ARAAudioSource* audioSource,
                                                            const juce::ARAContentUpdateScopes scopeFlags)
{
    debugLog ("ara doUpdateAudioSourceContent p=" + ptrHex (audioSource)
              + " pid=" + pidOf (audioSource)
              + " affectSamples=" + juce::String (scopeFlags.affectSamples() ? 1 : 0));
    if (! scopeFlags.affectSamples())
        return;

    forEachModification ([&] (DeepSvcAudioModification& modification)
    {
        if (modification.getAudioSource() != audioSource)
            return;
        modification.sourceAudio.reset();
        for (auto& slot : modification.slots)
        {
            slot.synthAudio.reset();
            slot.pitchData.reset();
        }
        ++modification.dataRevision;
    });

    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioSource (juce::ARAAudioSource* audioSource)
{
    debugLog ("ara willDestroyAudioSource p=" + ptrHex (audioSource)
              + " pid=" + pidOf (audioSource));
}

void DeepSvcDocumentController::didAddAudioModificationToAudioSource (
    juce::ARAAudioSource* audioSource,
    juce::ARAAudioModification* audioModification)
{
    debugLog ("ara didAddAudioModificationToAudioSource src=" + ptrHex (audioSource)
              + " srcPid=" + pidOf (audioSource)
              + " " + describeAudioModification (audioModification)
              + " " + modificationSummary (asModification (audioModification)));
}

void DeepSvcDocumentController::willRemoveAudioModificationFromAudioSource (
    juce::ARAAudioSource* audioSource,
    juce::ARAAudioModification* audioModification)
{
    debugLog ("ara willRemoveAudioModificationFromAudioSource src=" + ptrHex (audioSource)
              + " srcPid=" + pidOf (audioSource)
              + " " + describeAudioModification (audioModification)
              + " " + modificationSummary (asModification (audioModification)));
}

void DeepSvcDocumentController::willUpdateAudioModificationProperties (
    juce::ARAAudioModification* audioModification,
    juce::ARAAudioModification::PropertiesPtr newProperties)
{
    juce::String newPid = "-";
    if (newProperties != nullptr && newProperties->persistentID != nullptr
        && newProperties->persistentID[0] != 0)
        newPid = juce::String (newProperties->persistentID);
    debugLog ("ara willUpdateAudioModificationProperties " + describeAudioModification (audioModification)
              + " newPid=" + newPid);
}

void DeepSvcDocumentController::willDeactivateAudioModificationForUndoHistory (
    juce::ARAAudioModification* audioModification,
    bool deactivate)
{
    debugLog ("ara willDeactivateAudioModificationForUndoHistory deactivate="
              + juce::String (deactivate ? 1 : 0) + " " + describeAudioModification (audioModification)
              + " " + modificationSummary (asModification (audioModification)));
}

void DeepSvcDocumentController::didDeactivateAudioModificationForUndoHistory (
    juce::ARAAudioModification* audioModification,
    bool deactivate)
{
    debugLog ("ara didDeactivateAudioModificationForUndoHistory deactivate="
              + juce::String (deactivate ? 1 : 0) + " " + describeAudioModification (audioModification)
              + " " + modificationSummary (asModification (audioModification)));
}

void DeepSvcDocumentController::didUpdateAudioModificationProperties (juce::ARAAudioModification* audioModification)
{
    debugLog ("ara didUpdateAudioModificationProperties " + describeAudioModification (audioModification)
              + " " + modificationSummary (asModification (audioModification)));
    if (auto* modification = asModification (audioModification))
        modification->bindContentKey();
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioModification (juce::ARAAudioModification* audioModification)
{
    debugLog ("ara willDestroyAudioModification " + describeAudioModification (audioModification)
              + " " + modificationSummary (asModification (audioModification)));
    const auto persistentId = pidOf (audioModification);
    if (persistentId != "-")
        for (int slot = 0; slot < 2; ++slot)
        {
            const JobKey key { persistentId, slot };
            jobManager.cancelJobsFor (key);
            pendingSynthParams.erase (key);
            pendingSynthTimbres.erase (key);
            pendingDetectRanges.erase (key);
            pendingSynthRanges.erase (key);
        }
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didAddPlaybackRegionToAudioModification (
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara didAddPlaybackRegionToAudioModification " + describeAudioModification (audioModification)
              + " " + describeRegion (playbackRegion));
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didEndEditing (juce::ARADocument* document)
{
    juce::ignoreUnused (document);
    debugLog ("ara didEndEditing");
    dumpAraGraph ("didEndEditing");

    forEachModification ([&] (DeepSvcAudioModification& modification)
    {
        if (modification.workingRange().isValid())
            ensureSourceAudio (modification);
    });

    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource,
                                                                   bool enable)
{
    if (! enable)
        return;

    forEachModification ([&] (DeepSvcAudioModification& modification)
    {
        if (modification.getAudioSource() != audioSource)
            return;
        if (modification.workingRange().isValid())
            ensureSourceAudio (modification);
    });
}

void DeepSvcDocumentController::didUpdateRegionSequenceProperties (juce::ARARegionSequence* regionSequence)
{
    debugLog ("ara didUpdateRegionSequenceProperties seq=" + ptrHex (regionSequence)
              + " seqName=" + regionSequenceNameOf (regionSequence));
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didAddPlaybackRegionToRegionSequence (
    juce::ARARegionSequence* regionSequence,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara didAddPlaybackRegionToRegionSequence seq=" + ptrHex (regionSequence)
              + " seqName=" + regionSequenceNameOf (regionSequence)
              + " " + describeRegion (playbackRegion));
}

void DeepSvcDocumentController::willRemovePlaybackRegionFromRegionSequence (
    juce::ARARegionSequence* regionSequence,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara willRemovePlaybackRegionFromRegionSequence seq=" + ptrHex (regionSequence)
              + " seqName=" + regionSequenceNameOf (regionSequence)
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
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willRemovePlaybackRegionFromAudioModification (
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara willRemovePlaybackRegionFromAudioModification " + describeAudioModification (audioModification)
              + " " + describeRegion (playbackRegion));
}

void DeepSvcDocumentController::willDestroyPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara willDestroyPlaybackRegion " + describeRegion (playbackRegion));
    reconcileEditorSelectionPlaybackRegions();
    refreshRegisteredRenderers (publishModelChange());
}

namespace
{

constexpr int kArchiveMagic = 0x44535643;
constexpr int kArchiveVersion = 4;
constexpr int kMaxArchiveRecords = 65536;

} // namespace

bool DeepSvcDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output,
                                                        const juce::ARAStoreObjectsFilter* filter)
{
    std::vector<DeepSvcAudioModification*> modifications;

    if (filter == nullptr)
    {
        forEachModification ([&] (DeepSvcAudioModification& modification)
        {
            if (pidOf (&modification) != "-")
                modifications.push_back (&modification);
        });
    }
    else
    {
        const auto& modsToStore = filter->getAudioModificationsToStore();
        forEachModification ([&] (DeepSvcAudioModification& modification)
        {
            const auto* basePtr = static_cast<const ARA::PlugIn::AudioModification*> (&modification);
            if (std::find (modsToStore.begin(), modsToStore.end(), basePtr) != modsToStore.end())
                modifications.push_back (&modification);
        });
    }

    if (modifications.size() > static_cast<size_t> (kMaxArchiveRecords))
        return false;

    juce::String stored;
    for (const auto* modification : modifications)
        stored += pidOf (modification) + ",";
    debugLog ("ara doStoreObjects count=" + juce::String (static_cast<int> (modifications.size()))
              + " pids=[" + stored + "]");

    bool ok = output.writeInt (kArchiveMagic);
    ok = output.writeInt (kArchiveVersion) && ok;
    ok = output.writeInt (static_cast<int> (modifications.size())) && ok;

    for (const auto* modification : modifications)
    {
        ok = output.writeString (juce::String (modification->getPersistentID())) && ok;
        ok = output.writeString (juce::JSON::toString (archive::modificationToJson (*modification))) && ok;
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

        DeepSvcAudioModification* modification = nullptr;
        if (filter != nullptr)
            modification = filter->getAudioModificationToRestoreStateWithID<DeepSvcAudioModification> (
                archivedPersistentId.toRawUTF8());
        else
            modification = findModificationByPersistentId (archivedPersistentId);

        if (modification == nullptr)
        {
            debugLog ("ara doRestore skip noHostMap archivedPid=" + archivedPersistentId);
            continue;
        }

        const auto json = juce::JSON::parse (jsonText);
        archive::modificationFromJson (*modification, json);
        modification->bindContentKey();
        for (auto& slot : modification->slots)
            if (slot.synthAudio.has_value() && slot.synthAudio->isValid())
                fillSynthPlayback (*slot.synthAudio);
        debugLog ("ara doRestore applied restoredPid=" + pidOf (modification)
                  + " rev=" + juce::String (static_cast<juce::int64> (modification->dataRevision)));
        notifyPersistedStateChanged (*modification);
    }

    refreshRegisteredRenderers (publishModelChange());
    return true;
}

void DeepSvcDocumentController::jobStatusChanged (JobKey key, const JobStatus& status)
{
    if (status.state == JobStatus::State::failed || status.state == JobStatus::State::cancelled)
    {
        pendingSynthParams.erase (key);
        pendingSynthTimbres.erase (key);
        pendingDetectRanges.erase (key);
        pendingSynthRanges.erase (key);
    }
}

void DeepSvcDocumentController::detectFinished (JobKey key, std::vector<float> f0)
{
    const auto rangeIt = pendingDetectRanges.find (key);
    if (rangeIt == pendingDetectRanges.end())
        return;
    const double start = rangeIt->second.startSeconds;
    pendingDetectRanges.erase (rangeIt);

    const auto* modification = findModificationByPersistentId (key.persistentId);
    if (modification == nullptr)
        return;

    applyF0 (modification->contentKey, key.slot,
             f0TimesForWorkingRange (start, f0.size()), std::move (f0));
}

void DeepSvcDocumentController::synthFinished (JobKey key,
                                               std::vector<float> audio,
                                               std::vector<float> firstVocoder,
                                               std::vector<float> f0,
                                               double elapsedSeconds)
{
    debugLog ("synthFinished slot=" + juce::String (key.slot)
              + " audio=" + juce::String (static_cast<int64_t> (audio.size()))
              + " firstVocoder=" + juce::String (static_cast<int64_t> (firstVocoder.size()))
              + " f0=" + juce::String (static_cast<int64_t> (f0.size())));
    dumpDebugWav ("synth_output_final", audio.data(), audio.size());
    if (! firstVocoder.empty())
        dumpDebugWav ("synth_output_first_vocoder", firstVocoder.data(), firstVocoder.size());

    auto output = firstVocoder.empty() ? std::move (audio) : std::move (firstVocoder);

    const auto rangeIt = pendingSynthRanges.find (key);
    if (rangeIt == pendingSynthRanges.end())
        return;
    const WorkingRange range = rangeIt->second;
    pendingSynthRanges.erase (rangeIt);

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

    const auto* modification = findModificationByPersistentId (key.persistentId);
    if (modification == nullptr)
        return;

    applyRenderedAudio (modification->contentKey, key.slot,
                        std::make_shared<const std::vector<float>> (std::move (output)),
                        range.startSeconds, range.endSeconds, synthParams, synthTimbre, elapsedSeconds);
    if (! f0.empty())
        applyF0 (modification->contentKey, key.slot,
                 f0TimesForWorkingRange (range.startSeconds, f0.size()), std::move (f0));
}

DeepSvcDocumentController::PlaybackRegionProjection
DeepSvcDocumentController::makeProjection (juce::ARAPlaybackRegion* region) const
{
    PlaybackRegionProjection projection;
    projection.playbackRegion = region;
    if (region == nullptr)
        return projection;

    auto* modification = region->getAudioModification();
    projection.audioModificationPersistentId = pidOf (modification);
    projection.startInPlaybackTime = region->getStartInPlaybackTime();
    projection.startInModificationTime = region->getStartInAudioModificationTime();
    projection.durationInPlaybackTime = region->getDurationInPlaybackTime();
    projection.durationInModificationTime = region->getDurationInAudioModificationTime();
    projection.displayColour = colourOf (region);

    if (const auto* modificationContent = asModification (modification))
    {
        projection.contentKey = modificationContent->contentKey;
        projection.contentRevision = modificationContent->dataRevision;

        const auto& slot = modificationContent->active();
        projection.hasRenderedAudio = slot.hasSynthAudio() && ! slot.bypass;
        if (slot.hasSynthAudio())
        {
            projection.hasSynthCoverage = true;
            projection.synthStartTime = slot.synthAudio->synthStartTime;
            projection.synthEndTime = slot.synthAudio->synthEndTime;
        }
    }
    return projection;
}

std::vector<DeepSvcDocumentController::PlaybackRegionProjection>
DeepSvcDocumentController::buildProjections() const
{
    std::vector<PlaybackRegionProjection> result;
    auto* document = const_cast<DeepSvcDocumentController*> (this)->getDocument();
    if (document == nullptr)
        return result;

    for (auto* source : document->getAudioSources())
        for (auto* modification : source->getAudioModifications())
            for (auto* region : modification->getPlaybackRegions())
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
    auto* document = getDocument();
    editorSelectionPlaybackRegions.erase (
        std::remove_if (editorSelectionPlaybackRegions.begin(), editorSelectionPlaybackRegions.end(),
                        [document] (juce::ARAPlaybackRegion* region)
                        {
                            if (region == nullptr || document == nullptr)
                                return true;
                            for (auto* source : document->getAudioSources())
                                for (auto* modification : source->getAudioModifications())
                                    for (auto* live : modification->getPlaybackRegions())
                                        if (live == region)
                                            return false;
                            return true;
                        }),
        editorSelectionPlaybackRegions.end());
}

void DeepSvcDocumentController::notifyPersistedStateChanged (DeepSvcAudioModification& modification)
{
    modification.notifyContentChanged (juce::ARAContentUpdateScopes(), true);
}

juce::ARAAudioModification* DeepSvcDocumentController::doCreateAudioModification (
    juce::ARAAudioSource* audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification* optionalModificationToClone)
{
    auto* created = new DeepSvcAudioModification (audioSource, hostRef, optionalModificationToClone);
    debugLog ("ara doCreateAudioModification created=" + ptrHex (created)
              + " clone=" + ptrHex (optionalModificationToClone)
              + " clonePid=" + pidOf (optionalModificationToClone)
              + " " + describeAudioModification (optionalModificationToClone)
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
              + " " + describeAudioModification (modification));
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
