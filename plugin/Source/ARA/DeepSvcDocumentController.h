#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <map>
#include <optional>
#include <vector>

#include "../Content/ContentKey.h"
#include "../Content/ContentStore.h"
#include "../Engine/JobManager.h"
#include "../Utils/ContentTimelineProjection.h"
#include "../Utils/SourceWindow.h"
#include "AudioModificationState.h"
#include "AudioSourceRef.h"
#include "PlaybackRegion.h"

// 对应 OpenTune Source/ARA/OpenTuneDocumentController.h
namespace deepsvc
{

class DeepSvcPlaybackRenderer;

class DeepSvcDocumentController : public juce::ARADocumentControllerSpecialisation
                                , private JobManager::Listener
{
public:
    // 一个 PlaybackRegion 的完整投影：放置快照 + 内容引用
    struct PlaybackRegionProjection
    {
        juce::ARAPlaybackRegion* playbackRegion { nullptr };
        juce::String audioModificationPersistentId;
        uint64_t placementRevision { 0 };
        double startInPlaybackTime { 0.0 };
        double startInModificationTime { 0.0 };
        double durationInPlaybackTime { 0.0 };
        double durationInModificationTime { 0.0 };
        bool timestretchEnabled { false };
        std::optional<juce::Colour> displayColour;

        ContentKey contentKey;
        // 承载本片段的分段：由片段的内容窗口在分段列表中定位
        SegmentKey segmentKey;
        SegmentRange segmentRange;
        uint64_t contentRevision { 0 };
        double contentDurationSeconds { 0.0 };
        bool hasRenderedAudio { false };

        bool hasValidPlacement() const noexcept
        {
            return playbackRegion != nullptr
                && audioModificationPersistentId.isNotEmpty()
                && durationInPlaybackTime > 0.0
                && durationInModificationTime > 0.0;
        }
        bool isPlaybackRenderable() const noexcept
        {
            return hasValidPlacement() && contentKey.isValid() && hasRenderedAudio
                && contentDurationSeconds > 0.0;
        }
        double endInPlaybackTime() const noexcept
        {
            return startInPlaybackTime + durationInPlaybackTime;
        }

        // 片段覆盖的修改内部时间区间
        SegmentRange modificationRange() const noexcept
        {
            return SegmentRange { startInModificationTime, durationInModificationTime };
        }

        // 时间线时间 ↔ content 本地时间（content 0 = sourceWindow 起点）
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

    // ---- 投影查询（消息线程） ----
    std::vector<PlaybackRegionProjection> getPlaybackRegionProjections() const;
    std::vector<PlaybackRegionProjection> getPlaybackRegionProjectionsFor (
        const std::vector<juce::ARAPlaybackRegion*>& playbackRegions) const;
    std::vector<PlaybackRegionProjection> getEditorSelectionPlaybackRegionProjections() const;
    std::optional<PlaybackRegionProjection> getFocusedEditorPlaybackRegionProjection() const;

    void setEditorViewSelectionPlaybackRegions (std::vector<juce::ARAPlaybackRegion*> playbackRegions);

    // 选区更新计数：每次宿主选区通知递增。编辑器只信任自己创建之后到达的选区，
    // 避免新建 Event FX 的编辑器把上一个编辑器会话留下的选区当成当前焦点
    uint64_t readEditorSelectionRevision() const noexcept { return editorSelectionRevision; }

    // ---- 渲染器注册 ----
    void registerPlaybackRenderer (DeepSvcPlaybackRenderer& renderer);
    void unregisterPlaybackRenderer (DeepSvcPlaybackRenderer& renderer);

    // ---- 内容读取（消息线程；渲染器经渲染计划拿数据，不走这里） ----
    const ModificationContent* findContent (ContentKey key) const;
    // 分段状态：按分段键定位；分段不存在时返回 nullptr
    const ContentSegment* findSegment (SegmentKey key) const;
    uint64_t readContentRevision (ContentKey key) const;
    int readActiveSlot (SegmentKey key) const;
    // 修改的源音频（44.1kHz 单声道，覆盖整个源窗口），首次调用时提取并重采样，之后走缓存
    std::shared_ptr<const juce::AudioBuffer<float>> readSourceAudio (ContentKey key);

    // ---- 分段槽位状态（A/B，docs/ara.md 第 4.1 节） ----
    void setActiveSlot (SegmentKey key, int slot);
    void setSlotBypass (SegmentKey key, int slot, bool bypass);
    void applyTimbreFile (SegmentKey key, int slot, const juce::String& timbreFile);
    void applySlotParams (SegmentKey key, int slot, const EngineSynthParams& params);

    // ---- 内容写入（任务完成时调用，消息线程） ----
    void applyF0 (SegmentKey key, int slot, std::vector<float> times, std::vector<float> values);
    void applyRenderedAudio (SegmentKey key,
                             int slot,
                             std::shared_ptr<const std::vector<float>> samples,
                             const EngineSynthParams& synthParams,
                             const juce::String& synthTimbreFile);

    // ---- 任务入口（消息线程） ----
    void requestDetect (SegmentKey key, int slot, EngineEstimator estimator);
    void requestSynth (SegmentKey key,
                       int slot,
                       const juce::String& timbreAbsolutePath,
                       const EngineSynthParams& params);
    void cancelJobs (SegmentKey key, int slot);
    JobStatus jobStatusFor (SegmentKey key, int slot) const;

    // ---- ARA 通知 ----
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
    // JobManager::Listener
    void jobStatusChanged (JobKey key, const JobStatus& status) override;
    void detectFinished (JobKey key, std::vector<float> f0) override;
    void synthFinished (JobKey key,
                        std::vector<float> audio,
                        std::vector<float> firstVocoder,
                        std::vector<float> f0) override;

    AudioSourceRef* findAudioSource (juce::ARAAudioSource* audioSource);
    AudioSourceRef* findAudioSource (const juce::String& persistentId);
    const AudioSourceRef* findAudioSource (const juce::String& persistentId) const;
    AudioSourceRef& ensureAudioSource (juce::ARAAudioSource* audioSource);

    AudioModificationState* findAudioModification (const juce::String& persistentId);
    const AudioModificationState* findAudioModification (const juce::String& persistentId) const;
    AudioModificationState* findAudioModification (juce::ARAAudioModification* audioModification);
    AudioModificationState* findAudioModificationByContentKey (const ContentKey& key);
    const AudioModificationState* findAudioModificationByContentKey (const ContentKey& key) const;
    AudioModificationState& ensureAudioModification (juce::ARAAudioModification* audioModification);
    void attachSource (AudioModificationState& modification);
    ContentKey bindAudioModificationIdentity (AudioModificationState& modification);
    ContentKey makeAudioModificationContentKey (const juce::String& persistentId);

    PlaybackRegion* findPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion);
    const PlaybackRegion* findPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion) const;
    PlaybackRegion& ensurePlaybackRegion (juce::ARAPlaybackRegion* playbackRegion);
    bool removePlaybackRegion (juce::ARAPlaybackRegion* playbackRegion);

    PlaybackRegionProjection makeProjection (const PlaybackRegion& region) const;
    std::vector<PlaybackRegionProjection> buildProjections() const;
    std::vector<DeepSvcPlaybackRenderer*> publishModelChange();
    void refreshRegisteredRenderers (const std::vector<DeepSvcPlaybackRenderer*>& renderers);
    void reconcileEditorSelectionPlaybackRegions();

    // 提取修改的源音频：整个源窗口，重采样到 44.1kHz 单声道并缓存
    std::shared_ptr<const juce::AudioBuffer<float>> ensureSourceAudio (AudioModificationState& modification);

    // 分段定位：按分段键找到可写的分段
    ContentSegment* findSegmentForWrite (SegmentKey key);

    // 分段划分：按当前所有 playback region 的窗口端点重新切分该修改的分段列表，
    // 新分段的状态从旧布局中重叠最大的分段深拷贝继承（docs/ara.md 第 4.1 节）
    void reconcileSegments (AudioModificationState& modification);
    void reconcileAllSegments();

    // 把宿主对象图与插件影子表打进 debug.log，用于核对切分/换轨时事件实例是否保留
    void dumpAraGraph (const juce::String& reason);

    // 对应 OpenTune 的 notifyContentChanged 调用点（OpenTuneDocumentController.cpp 第 1877 行等）：
    // 入库状态变化后通知宿主工程已修改，宿主才会保存
    static void notifyPersistedStateChanged (AudioModificationState& modification);

    std::vector<AudioSourceRef> audioSources;
    std::vector<AudioModificationState> audioModifications;
    std::vector<PlaybackRegion> playbackRegions;
    std::vector<juce::ARAPlaybackRegion*> editorSelectionPlaybackRegions;
    uint64_t editorSelectionRevision = 0;
    std::vector<DeepSvcPlaybackRenderer*> playbackRenderers;
    std::map<uint64_t, juce::String> araPersistentIdsByObjectId;
    std::map<juce::String, uint64_t> araObjectIdsByPersistentId;

    JobManager jobManager;
    // 合成任务提交时的参数与音色：完成时写入槽位快照
    std::map<JobKey, EngineSynthParams> pendingSynthParams;
    std::map<JobKey, juce::String> pendingSynthTimbres;

    JUCE_DECLARE_NON_COPYABLE (DeepSvcDocumentController)
};

} // namespace deepsvc

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();
