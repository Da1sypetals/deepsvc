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
    uint64_t readContentRevision (ContentKey key) const;
    juce::String readTimbreFile (ContentKey key) const;
    // 源音频（44.1kHz 单声道），首次调用时提取并重采样，之后走缓存
    std::shared_ptr<const juce::AudioBuffer<float>> readSourceAudio (ContentKey key);

    // ---- 内容写入（任务完成时调用，消息线程） ----
    void applyF0 (ContentKey key, std::vector<float> times, std::vector<float> values);
    void applyRenderedAudio (ContentKey key,
                             std::shared_ptr<const std::vector<float>> samples,
                             const juce::String& fingerprint);
    void applyTimbreFile (ContentKey key, const juce::String& timbreFile);

    // ---- 任务入口（消息线程） ----
    // 检测只覆盖选中区域的内容窗口 [contentStartSeconds, +contentDurationSeconds)
    void requestDetect (ContentKey key,
                        double contentStartSeconds,
                        double contentDurationSeconds,
                        EngineEstimator estimator);
    void requestSynth (ContentKey key,
                       const juce::String& timbreAbsolutePath,
                       const EngineSynthParams& params,
                       const juce::String& fingerprint);
    void cancelJobs (ContentKey key);
    JobStatus jobStatusFor (ContentKey key) const;

    // ---- A/B 对比：true = 听原声（渲染器直通） ----
    void setAbBypass (bool bypass);
    bool isAbBypass() const noexcept { return abBypass; }

    // ---- 合成指纹：参数 + 音色，一致则合成结果有效 ----
    static juce::String makeFingerprint (const EngineSynthParams& params,
                                         const juce::String& timbreFile);

    // ---- ARA 通知 ----
    void didUpdateAudioSourceProperties (juce::ARAAudioSource* audioSource) override;
    void doUpdateAudioSourceContent (juce::ARAAudioSource* audioSource,
                                     const juce::ARAContentUpdateScopes scopeFlags) override;
    void willDestroyAudioSource (juce::ARAAudioSource* audioSource) override;
    void didUpdateAudioModificationProperties (juce::ARAAudioModification* audioModification) override;
    void willDestroyAudioModification (juce::ARAAudioModification* audioModification) override;
    void didAddPlaybackRegionToAudioModification (juce::ARAAudioModification* audioModification,
                                                  juce::ARAPlaybackRegion* playbackRegion) override;
    void didUpdatePlaybackRegionProperties (juce::ARAPlaybackRegion* playbackRegion) override;
    void willRemovePlaybackRegionFromAudioModification (juce::ARAAudioModification* audioModification,
                                                        juce::ARAPlaybackRegion* playbackRegion) override;
    void willDestroyPlaybackRegion (juce::ARAPlaybackRegion* playbackRegion) override;

protected:
    bool doRestoreObjectsFromStream (juce::ARAInputStream& input,
                                     const juce::ARARestoreObjectsFilter* filter) override;
    bool doStoreObjectsToStream (juce::ARAOutputStream& output,
                                 const juce::ARAStoreObjectsFilter* filter) override;

    juce::ARAPlaybackRenderer* doCreatePlaybackRenderer() override;
    juce::ARAEditorView* doCreateEditorView() override;

private:
    // JobManager::Listener
    void jobStatusChanged (ContentKey key, const JobStatus& status) override;
    void detectFinished (ContentKey key, double windowStartSeconds, std::vector<float> f0) override;
    void synthFinished (ContentKey key,
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

    // 提取 modification 整个源窗口的音频，重采样到 44.1kHz 单声道并缓存
    std::shared_ptr<const juce::AudioBuffer<float>> ensureSourceAudio (AudioModificationState& modification);

    std::vector<AudioSourceRef> audioSources;
    std::vector<AudioModificationState> audioModifications;
    std::vector<PlaybackRegion> playbackRegions;
    std::vector<juce::ARAPlaybackRegion*> editorSelectionPlaybackRegions;
    uint64_t editorSelectionRevision = 0;
    std::vector<DeepSvcPlaybackRenderer*> playbackRenderers;
    std::map<uint64_t, juce::String> araPersistentIdsByObjectId;
    std::map<juce::String, uint64_t> araObjectIdsByPersistentId;

    JobManager jobManager;
    bool abBypass { false };
    std::map<ContentKey, juce::String> synthFingerprintsByKey;

    JUCE_DECLARE_NON_COPYABLE (DeepSvcDocumentController)
};

} // namespace deepsvc

const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory();
