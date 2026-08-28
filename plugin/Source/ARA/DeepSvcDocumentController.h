#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "../Content/ContentKey.h"
#include "../Engine/JobManager.h"
#include "../Utils/ContentTimelineProjection.h"
#include "EventAudioModification.h"

namespace deepsvc
{

class DeepSvcPlaybackRenderer;

class DeepSvcDocumentController : public juce::ARADocumentControllerSpecialisation
                                , private JobManager::Listener
{
public:
    struct PlaybackRegionProjection
    {
        juce::ARAPlaybackRegion* playbackRegion { nullptr };
        juce::String audioModificationPersistentId;
        double startInPlaybackTime { 0.0 };
        double startInModificationTime { 0.0 };
        double durationInPlaybackTime { 0.0 };
        double durationInModificationTime { 0.0 };
        std::optional<juce::Colour> displayColour;

        ContentKey contentKey;
        uint64_t contentRevision { 0 };
        double contentDurationSeconds { 0.0 };
        bool hasRenderedAudio { false };
        bool hasSynthCoverage { false };
        double synthStartTime { 0.0 };
        double synthEndTime { 0.0 };

        bool hasValidPlacement() const noexcept
        {
            return playbackRegion != nullptr
                && audioModificationPersistentId.isNotEmpty()
                && durationInPlaybackTime > 0.0
                && durationInModificationTime > 0.0;
        }
        double endInPlaybackTime() const noexcept
        {
            return startInPlaybackTime + durationInPlaybackTime;
        }

        ContentTimelineProjection toTimelineProjection() const noexcept
        {
            ContentTimelineProjection projection;
            projection.timelineStartSeconds = startInPlaybackTime;
            projection.timelineDurationSeconds = durationInPlaybackTime;
            projection.contentStartSeconds = startInModificationTime;
            projection.contentDurationSeconds = durationInModificationTime;
            return projection;
        }
    };

    DeepSvcDocumentController (const ARA::PlugIn::PlugInEntry* entry,
                               const ARA::ARADocumentControllerHostInstance* instance);
    ~DeepSvcDocumentController() override;

    std::vector<PlaybackRegionProjection> getPlaybackRegionProjections() const;
    std::vector<PlaybackRegionProjection> getPlaybackRegionProjectionsFor (
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const;
    std::vector<PlaybackRegionProjection> getEditorSelectionPlaybackRegionProjections() const;
    std::optional<PlaybackRegionProjection> getFocusedEditorPlaybackRegionProjection() const;

    void setEditorViewSelectionPlaybackRegions (std::vector<juce::ARAPlaybackRegion*> playbackRegions);

    uint64_t readEditorSelectionRevision() const noexcept { return editorSelectionRevision; }

    void registerPlaybackRenderer (DeepSvcPlaybackRenderer& renderer);
    void unregisterPlaybackRenderer (DeepSvcPlaybackRenderer& renderer);

    EventAudioModification* findEvent (ContentKey key);
    const EventAudioModification* findEvent (ContentKey key) const;
    EventAudioModification* findEventByPersistentId (const juce::String& persistentId);
    const EventAudioModification* findEventByPersistentId (const juce::String& persistentId) const;

    uint64_t readContentRevision (ContentKey key) const;
    int readActiveSlot (ContentKey key) const;
    std::shared_ptr<const juce::AudioBuffer<float>> readSourceAudio (ContentKey key);

    void setActiveSlot (ContentKey key, int slot);
    void setSlotBypass (ContentKey key, int slot, bool bypass);
    void applyTimbreFile (ContentKey key, int slot, const juce::String& timbreFile);
    void applySlotParams (ContentKey key, int slot, const EngineSynthParams& params);

    void applyF0 (ContentKey key, int slot, std::vector<float> times, std::vector<float> values);
    void applyRenderedAudio (ContentKey key,
                             int slot,
                             std::shared_ptr<const std::vector<float>> samples,
                             double synthStartTime,
                             double synthEndTime,
                             const EngineSynthParams& synthParams,
                             const juce::String& synthTimbreFile);

    void requestDetect (ContentKey key, int slot, EngineEstimator estimator);
    void requestSynth (ContentKey key,
                       int slot,
                       const juce::String& timbreAbsolutePath,
                       const EngineSynthParams& params);
    void cancelJobs (ContentKey key, int slot);
    JobStatus jobStatusFor (ContentKey key, int slot) const;

    void willBeginEditing (juce::ARADocument* document) override;
    void didEndEditing (juce::ARADocument* document) override;
    void didUpdateAudioSourceProperties (juce::ARAAudioSource* audioSource) override;
    void doUpdateAudioSourceContent (juce::ARAAudioSource* audioSource,
                                     const juce::ARAContentUpdateScopes scopeFlags) override;
    void willDestroyAudioSource (juce::ARAAudioSource* audioSource) override;
    void didAddAudioModificationToAudioSource (juce::ARAAudioSource* audioSource,
                                               juce::ARAAudioModification* audioModification) override;
    void willRemoveAudioModificationFromAudioSource (juce::ARAAudioSource* audioSource,
                                                     juce::ARAAudioModification* audioModification) override;
    void willUpdateAudioModificationProperties (juce::ARAAudioModification* audioModification,
                                                juce::ARAAudioModification::PropertiesPtr newProperties) override;
    void didUpdateAudioModificationProperties (juce::ARAAudioModification* audioModification) override;
    void willDeactivateAudioModificationForUndoHistory (juce::ARAAudioModification* audioModification,
                                                        bool deactivate) override;
    void didDeactivateAudioModificationForUndoHistory (juce::ARAAudioModification* audioModification,
                                                       bool deactivate) override;
    void willDestroyAudioModification (juce::ARAAudioModification* audioModification) override;
    void didAddPlaybackRegionToAudioModification (juce::ARAAudioModification* audioModification,
                                                  juce::ARAPlaybackRegion* playbackRegion) override;
    void willRemovePlaybackRegionFromAudioModification (juce::ARAAudioModification* audioModification,
                                                        juce::ARAPlaybackRegion* playbackRegion) override;
    void didEnableAudioSourceSamplesAccess (juce::ARAAudioSource* audioSource, bool enable) override;
    void didUpdateRegionSequenceProperties (juce::ARARegionSequence* regionSequence) override;
    void didAddPlaybackRegionToRegionSequence (juce::ARARegionSequence* regionSequence,
                                               juce::ARAPlaybackRegion* playbackRegion) override;
    void willRemovePlaybackRegionFromRegionSequence (juce::ARARegionSequence* regionSequence,
                                                     juce::ARAPlaybackRegion* playbackRegion) override;
    void willUpdatePlaybackRegionProperties (juce::ARAPlaybackRegion* playbackRegion,
                                             juce::ARAPlaybackRegion::PropertiesPtr newProperties) override;
    void didUpdatePlaybackRegionProperties (juce::ARAPlaybackRegion* playbackRegion) override;
    void willDestroyPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion) override;

protected:
    bool doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                     const juce::ARARestoreObjectsFilter* filter) override;
    bool doStoreObjectsToStream (juce::ARAOutputStream& output,
                                 const juce::ARAStoreObjectsFilter* filter) override;

    juce::ARAAudioModification* doCreateAudioModification (
        juce::ARAAudioSource* audioSource,
        ARA::ARAAudioModificationHostRef hostRef,
        const juce::ARAAudioModification* optionalModificationToClone) override;
    juce::ARAPlaybackRegion* doCreatePlaybackRegion (
        juce::ARAAudioModification* modification,
        ARA::ARAPlaybackRegionHostRef hostRef) override;
    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() override;
    juce::ARAEditorView* doCreateEditorView() override;

private:
    void jobStatusChanged (JobKey key, const JobStatus& status) override;
    void detectFinished (JobKey key, std::vector<float> f0) override;
    void synthFinished (JobKey key,
                        std::vector<float> audio,
                        std::vector<float> firstVocoder,
                        std::vector<float> f0) override;

    EventAudioModification* asEvent (juce::ARAAudioModification* audioModification) const;
    const EventAudioModification* asEvent (const juce::ARAAudioModification* audioModification) const;

    PlaybackRegionProjection makeProjection (juce::ARAPlaybackRegion* region) const;
    std::vector<PlaybackRegionProjection> buildProjections() const;
    std::vector<DeepSvcPlaybackRenderer*> publishModelChange();
    void refreshRegisteredRenderers (const std::vector<DeepSvcPlaybackRenderer*>& renderers);
    void reconcileEditorSelectionPlaybackRegions();

    std::shared_ptr<const juce::AudioBuffer<float>> ensureSourceAudio (EventAudioModification& event);
    void forEachEvent (const std::function<void (EventAudioModification&)>& fn);
    void forEachEvent (const std::function<void (const EventAudioModification&)>& fn) const;

    void dumpAraGraph (const juce::String& reason);
    static void notifyPersistedStateChanged (EventAudioModification& event);

    std::vector<juce::ARAPlaybackRegion*> editorSelectionPlaybackRegions;
    uint64_t editorSelectionRevision = 0;
    std::vector<DeepSvcPlaybackRenderer*> playbackRenderers;

    JobManager jobManager;
    std::map<JobKey, EngineSynthParams> pendingSynthParams;
    std::map<JobKey, juce::String> pendingSynthTimbres;
    std::map<JobKey, FileRange> pendingDetectRanges;
    std::map<JobKey, FileRange> pendingSynthRanges;

    JUCE_DECLARE_NON_COPYABLE (DeepSvcDocumentController)
};

} // namespace deepsvc

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();
