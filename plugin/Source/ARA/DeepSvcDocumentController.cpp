#include "DeepSvcDocumentController.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../Content/ContentArchive.h"
#include "../DebugLog.h"
#include "../State/Parameters.h"
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

std::vector<float> extractFileRangePcm (const juce::AudioBuffer<float>& sourceAudio,
                                        const FileRange& range)
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

juce::String eventSummary (const EventAudioModification* event)
{
    if (event == nullptr)
        return "event=miss";

    int rendered = 0;
    int pitched = 0;
    for (const auto& slot : event->slots)
    {
        if (slot.hasSynthAudio())
            ++rendered;
        if (slot.pitchData.has_value() && ! slot.pitchData->f0Values.empty())
            ++pitched;
    }

    return "event pid=" + pidOf (event)
         + " rev=" + juce::String (static_cast<juce::int64> (event->dataRevision))
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

EventAudioModification* DeepSvcDocumentController::asEvent (juce::ARAAudioModification* audioModification) const
{
    return dynamic_cast<EventAudioModification*> (audioModification);
}

const EventAudioModification* DeepSvcDocumentController::asEvent (
    const juce::ARAAudioModification* audioModification) const
{
    return dynamic_cast<const EventAudioModification*> (audioModification);
}

void DeepSvcDocumentController::forEachEvent (const std::function<void (EventAudioModification&)>& fn)
{
    auto* document = getDocument();
    if (document == nullptr)
        return;
    for (auto* source : document->getAudioSources())
        for (auto* modification : source->getAudioModifications<EventAudioModification>())
            if (modification != nullptr)
                fn (*modification);
}

void DeepSvcDocumentController::forEachEvent (const std::function<void (const EventAudioModification&)>& fn) const
{
    const_cast<DeepSvcDocumentController*> (this)->forEachEvent (
        [&] (EventAudioModification& event) { fn (event); });
}

EventAudioModification* DeepSvcDocumentController::findEvent (ContentKey key)
{
    EventAudioModification* found = nullptr;
    forEachEvent ([&] (EventAudioModification& event)
    {
        if (event.contentKey == key)
            found = &event;
    });
    return found;
}

const EventAudioModification* DeepSvcDocumentController::findEvent (ContentKey key) const
{
    const EventAudioModification* found = nullptr;
    forEachEvent ([&] (const EventAudioModification& event)
    {
        if (event.contentKey == key)
            found = &event;
    });
    return found;
}

EventAudioModification* DeepSvcDocumentController::findEventByPersistentId (const juce::String& persistentId)
{
    if (persistentId.isEmpty())
        return nullptr;
    EventAudioModification* found = nullptr;
    forEachEvent ([&] (EventAudioModification& event)
    {
        if (pidOf (&event) == persistentId)
            found = &event;
    });
    return found;
}

const EventAudioModification* DeepSvcDocumentController::findEventByPersistentId (const juce::String& persistentId) const
{
    if (persistentId.isEmpty())
        return nullptr;
    const EventAudioModification* found = nullptr;
    forEachEvent ([&] (const EventAudioModification& event)
    {
        if (pidOf (&event) == persistentId)
            found = &event;
    });
    return found;
}

uint64_t DeepSvcDocumentController::readContentRevision (ContentKey key) const
{
    const auto* event = findEvent (key);
    return event != nullptr ? event->dataRevision : 0;
}

int DeepSvcDocumentController::readActiveSlot (ContentKey key) const
{
    const auto* event = findEvent (key);
    return event != nullptr ? event->activeSlot : 0;
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::readSourceAudio (ContentKey key)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return nullptr;
    return ensureSourceAudio (*event);
}

void DeepSvcDocumentController::setActiveSlot (ContentKey key, int slot)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    slot = juce::jlimit (0, 1, slot);
    if (event->activeSlot == slot)
        return;

    event->activeSlot = slot;
    ++event->dataRevision;
    notifyPersistedStateChanged (*event);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::setSlotBypass (ContentKey key, int slot, bool bypass)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    auto& slotContent = event->slotAt (slot);
    if (slotContent.bypass == bypass)
        return;

    slotContent.bypass = bypass;
    ++event->dataRevision;
    notifyPersistedStateChanged (*event);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyTimbreFile (ContentKey key, int slot, const juce::String& timbreFile)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    event->slotAt (slot).timbreFile = timbreFile;
    notifyPersistedStateChanged (*event);
}

void DeepSvcDocumentController::applySlotParams (ContentKey key, int slot, const EngineSynthParams& params)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    event->slotAt (slot).params = params;
    notifyPersistedStateChanged (*event);
}

void DeepSvcDocumentController::applyF0 (ContentKey key, int slot,
                                         std::vector<float> times, std::vector<float> values)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    PitchData pitch;
    pitch.f0Times = std::move (times);
    pitch.f0Values = std::move (values);
    event->slotAt (slot).pitchData = std::move (pitch);
    ++event->dataRevision;
    notifyPersistedStateChanged (*event);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::applyRenderedAudio (ContentKey key,
                                                    int slot,
                                                    std::shared_ptr<const std::vector<float>> samples,
                                                    double synthStartTime,
                                                    double synthEndTime,
                                                    const EngineSynthParams& synthParams,
                                                    const juce::String& synthTimbreFile)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    SynthAudio synth;
    synth.samples = std::move (samples);
    synth.synthStartTime = synthStartTime;
    synth.synthEndTime = synthEndTime;
    auto& slotContent = event->slotAt (slot);
    slotContent.synthAudio = std::move (synth);
    slotContent.synthParams = synthParams;
    slotContent.synthTimbreFile = synthTimbreFile;
    ++event->dataRevision;
    notifyPersistedStateChanged (*event);
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::requestDetect (ContentKey key,
                                               int slot,
                                               EngineEstimator estimator)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    const auto range = event->windowUnion();
    const auto audio = ensureSourceAudio (*event);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    auto pcm = extractFileRangePcm (*audio, range);
    if (pcm.empty())
        return;

    slot = juce::jlimit (0, 1, slot);
    const JobKey jobKey { juce::String (event->getPersistentID()), slot };
    pendingDetectRanges[jobKey] = range;
    jobManager.submitDetect (jobKey, std::move (pcm),
                             static_cast<uint32_t> (TimeCoordinate::kRenderSampleRate),
                             estimator);
}

void DeepSvcDocumentController::requestSynth (ContentKey key,
                                              int slot,
                                              const juce::String& timbreAbsolutePath,
                                              const EngineSynthParams& params)
{
    auto* event = findEvent (key);
    if (event == nullptr)
        return;

    const auto range = event->windowUnion();
    const auto audio = ensureSourceAudio (*event);
    if (audio == nullptr || audio->getNumSamples() == 0)
        return;

    auto pcm = extractFileRangePcm (*audio, range);
    if (pcm.empty())
        return;

    slot = juce::jlimit (0, 1, slot);
    const JobKey jobKey { juce::String (event->getPersistentID()), slot };
    pendingSynthParams[jobKey] = params;
    pendingSynthTimbres[jobKey] = event->slotAt (slot).timbreFile;
    pendingSynthRanges[jobKey] = range;

    dumpDebugWav ("synth_input", pcm.data(), pcm.size());
    debugLog ("requestSynth slot=" + juce::String (slot)
              + " winStart=" + juce::String (range.startSeconds, 3)
              + " winEnd=" + juce::String (range.endSeconds, 3)
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
    const auto* event = findEvent (key);
    if (event == nullptr)
        return;
    jobManager.cancelJobsFor ({ juce::String (event->getPersistentID()), juce::jlimit (0, 1, slot) });
}

JobStatus DeepSvcDocumentController::jobStatusFor (ContentKey key, int slot) const
{
    const auto* event = findEvent (key);
    if (event == nullptr)
        return {};
    return jobManager.statusFor ({ juce::String (event->getPersistentID()), juce::jlimit (0, 1, slot) });
}

std::shared_ptr<const juce::AudioBuffer<float>>
DeepSvcDocumentController::ensureSourceAudio (EventAudioModification& event)
{
    if (event.sourceAudio != nullptr)
        return event.sourceAudio;

    auto* audioSource = event.getAudioSource();
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

    event.sourceAudio = mono;
    ++event.dataRevision;
    return mono;
}

void DeepSvcDocumentController::dumpAraGraph (const juce::String& reason)
{
    auto* document = getDocument();
    int eventCount = 0;
    int regionCount = 0;
    if (document != nullptr)
        for (auto* source : document->getAudioSources())
            for (auto* mod : source->getAudioModifications())
            {
                ++eventCount;
                regionCount += static_cast<int> (mod->getPlaybackRegions().size());
            }

    debugLog ("ara dump begin reason=" + reason
              + " events=" + juce::String (eventCount)
              + " regions=" + juce::String (regionCount));

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
            debugLog ("ara dump   " + describeMod (mod) + " " + eventSummary (asEvent (mod)));
            for (auto* region : mod->getPlaybackRegions())
            {
                ++hostRegions;
                debugLog ("ara dump     " + describeRegion (region));
            }
        }
    }

    debugLog ("ara dump end hostMods=" + juce::String (hostMods)
              + " hostRegions=" + juce::String (hostRegions));
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
    forEachEvent ([&] (EventAudioModification& event)
    {
        if (event.getAudioSource() == audioSource)
            event.sourceAudio.reset();
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

    forEachEvent ([&] (EventAudioModification& event)
    {
        if (event.getAudioSource() != audioSource)
            return;
        event.sourceAudio.reset();
        for (auto& slot : event.slots)
        {
            slot.synthAudio.reset();
            slot.pitchData.reset();
        }
        ++event.dataRevision;
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
              + " " + describeMod (audioModification)
              + " " + eventSummary (asEvent (audioModification)));
}

void DeepSvcDocumentController::willRemoveAudioModificationFromAudioSource (
    juce::ARAAudioSource* audioSource,
    juce::ARAAudioModification* audioModification)
{
    debugLog ("ara willRemoveAudioModificationFromAudioSource src=" + ptrHex (audioSource)
              + " srcPid=" + pidOf (audioSource)
              + " " + describeMod (audioModification)
              + " " + eventSummary (asEvent (audioModification)));
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
              + " " + eventSummary (asEvent (audioModification)));
}

void DeepSvcDocumentController::didDeactivateAudioModificationForUndoHistory (
    juce::ARAAudioModification* audioModification,
    bool deactivate)
{
    debugLog ("ara didDeactivateAudioModificationForUndoHistory deactivate="
              + juce::String (deactivate ? 1 : 0) + " " + describeMod (audioModification)
              + " " + eventSummary (asEvent (audioModification)));
}

void DeepSvcDocumentController::didUpdateAudioModificationProperties (juce::ARAAudioModification* audioModification)
{
    debugLog ("ara didUpdateAudioModificationProperties " + describeMod (audioModification)
              + " " + eventSummary (asEvent (audioModification)));
    if (auto* event = asEvent (audioModification))
        event->bindContentKey();
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willDestroyAudioModification (juce::ARAAudioModification* audioModification)
{
    debugLog ("ara willDestroyAudioModification " + describeMod (audioModification)
              + " " + eventSummary (asEvent (audioModification)));
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
    debugLog ("ara didAddPlaybackRegionToAudioModification " + describeMod (audioModification)
              + " " + describeRegion (playbackRegion));
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didEndEditing (juce::ARADocument* document)
{
    juce::ignoreUnused (document);
    debugLog ("ara didEndEditing");
    dumpAraGraph ("didEndEditing");

    forEachEvent ([&] (EventAudioModification& event)
    {
        if (event.windowUnion().isValid())
            ensureSourceAudio (event);
    });

    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource,
                                                                   bool enable)
{
    if (! enable)
        return;

    forEachEvent ([&] (EventAudioModification& event)
    {
        if (event.getAudioSource() != audioSource)
            return;
        if (event.windowUnion().isValid())
            ensureSourceAudio (event);
    });
}

void DeepSvcDocumentController::didUpdateRegionSequenceProperties (juce::ARARegionSequence* regionSequence)
{
    debugLog ("ara didUpdateRegionSequenceProperties seq=" + ptrHex (regionSequence)
              + " seqName=" + seqNameOf (regionSequence));
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
    refreshRegisteredRenderers (publishModelChange());
}

void DeepSvcDocumentController::willRemovePlaybackRegionFromAudioModification (
    juce::ARAAudioModification* audioModification,
    juce::ARAPlaybackRegion* playbackRegion)
{
    debugLog ("ara willRemovePlaybackRegionFromAudioModification " + describeMod (audioModification)
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
constexpr int kArchiveVersion = 2;
constexpr int kMaxArchiveRecords = 65536;

} // namespace

bool DeepSvcDocumentController::doStoreObjectsToStream (juce::ARAOutputStream& output,
                                                        const juce::ARAStoreObjectsFilter* filter)
{
    std::vector<EventAudioModification*> events;

    if (filter == nullptr)
    {
        forEachEvent ([&] (EventAudioModification& event)
        {
            if (pidOf (&event) != "-")
                events.push_back (&event);
        });
    }
    else
    {
        const auto& modsToStore = filter->getAudioModificationsToStore();
        forEachEvent ([&] (EventAudioModification& event)
        {
            const auto* basePtr = static_cast<const ARA::PlugIn::AudioModification*> (&event);
            if (std::find (modsToStore.begin(), modsToStore.end(), basePtr) != modsToStore.end())
                events.push_back (&event);
        });
    }

    if (events.size() > static_cast<size_t> (kMaxArchiveRecords))
        return false;

    juce::String stored;
    for (const auto* event : events)
        stored += pidOf (event) + ",";
    debugLog ("ara doStoreObjects count=" + juce::String (static_cast<int> (events.size()))
              + " pids=[" + stored + "]");

    bool ok = output.writeInt (kArchiveMagic);
    ok = output.writeInt (kArchiveVersion) && ok;
    ok = output.writeInt (static_cast<int> (events.size())) && ok;

    for (const auto* event : events)
    {
        ok = output.writeString (juce::String (event->getPersistentID())) && ok;
        ok = output.writeString (juce::JSON::toString (archive::eventToJson (*event))) && ok;
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

        EventAudioModification* event = nullptr;
        if (filter != nullptr)
            event = filter->getAudioModificationToRestoreStateWithID<EventAudioModification> (
                archivedPersistentId.toRawUTF8());
        else
            event = findEventByPersistentId (archivedPersistentId);

        if (event == nullptr)
        {
            debugLog ("ara doRestore skip noHostMap archivedPid=" + archivedPersistentId);
            continue;
        }

        const auto json = juce::JSON::parse (jsonText);
        archive::eventFromJson (*event, json);
        event->bindContentKey();
        debugLog ("ara doRestore applied restoredPid=" + pidOf (event)
                  + " rev=" + juce::String (static_cast<juce::int64> (event->dataRevision)));
        notifyPersistedStateChanged (*event);
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
    const double start = rangeIt != pendingDetectRanges.end() ? rangeIt->second.startSeconds : 0.0;
    if (rangeIt != pendingDetectRanges.end())
        pendingDetectRanges.erase (rangeIt);

    const auto* event = findEventByPersistentId (key.persistentId);
    if (event == nullptr)
        return;

    std::vector<float> times (f0.size());
    for (size_t i = 0; i < f0.size(); ++i)
        times[i] = static_cast<float> (start + static_cast<double> (i) * 0.01);

    applyF0 (event->contentKey, key.slot, std::move (times), std::move (f0));
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

    auto output = firstVocoder.empty() ? std::move (audio) : std::move (firstVocoder);

    FileRange range;
    if (const auto it = pendingSynthRanges.find (key); it != pendingSynthRanges.end())
    {
        range = it->second;
        pendingSynthRanges.erase (it);
    }

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

    const auto* event = findEventByPersistentId (key.persistentId);
    if (event == nullptr)
        return;

    const double synthStart = range.startSeconds;
    const double synthEnd = synthStart
        + static_cast<double> (output.size()) / TimeCoordinate::kRenderSampleRate;
    applyRenderedAudio (event->contentKey, key.slot,
                        std::make_shared<const std::vector<float>> (std::move (output)),
                        synthStart, synthEnd, synthParams, synthTimbre);
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

    if (const auto* event = asEvent (modification))
    {
        projection.contentKey = event->contentKey;
        projection.contentRevision = event->dataRevision;
        if (const auto* source = event->getAudioSource())
        {
            const double rate = source->getSampleRate();
            if (rate > 0.0)
                projection.contentDurationSeconds = static_cast<double> (source->getSampleCount()) / rate;
        }

        const auto& slot = event->active();
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

void DeepSvcDocumentController::notifyPersistedStateChanged (EventAudioModification& event)
{
    event.notifyContentChanged (juce::ARAContentUpdateScopes(), true);
}

juce::ARAAudioModification* DeepSvcDocumentController::doCreateAudioModification (
    juce::ARAAudioSource* audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification* optionalModificationToClone)
{
    auto* created = new EventAudioModification (audioSource, hostRef, optionalModificationToClone);
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
