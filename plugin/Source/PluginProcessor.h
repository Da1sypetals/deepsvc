#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <optional>
#include <vector>

#include "Content/ContentKey.h"
#include "State/Parameters.h"
#include "Utils/ContentTimelineProjection.h"

// 对应 OpenTune Source/PluginProcessor.h
namespace deepsvc
{

class DeepSvcDocumentController;

// 处理器持有的传输状态：每块由 processBlock 消费宿主 PositionInfo 更新一次，
// 界面直接读原子量。update 契约：nullopt 不动；无 timeInSeconds 时保留上次时间。
struct PlayHeadState
{
    std::atomic<bool> isPlaying { false };
    std::atomic<double> timeInSeconds { 0.0 };
    std::atomic<uint64_t> hostPositionRevision { 0 };

    double getPresentedPositionSeconds() const
    {
        return timeInSeconds.load (std::memory_order_relaxed);
    }

    void update (const juce::Optional<juce::AudioPlayHead::PositionInfo>& info)
    {
        if (! info.hasValue())
            return;

        if (const auto timeSeconds = info->getTimeInSeconds())
        {
            timeInSeconds.store (*timeSeconds, std::memory_order_relaxed);
            hostPositionRevision.fetch_add (1, std::memory_order_acq_rel);
        }

        isPlaying.store (info->getIsPlaying(), std::memory_order_release);
    }

    void reset()
    {
        isPlaying.store (false, std::memory_order_release);
    }
};

// 放置身份 = ContentKey + 投影的四个时间字段：
// 同一 AudioModification 的不同 PlaybackRegion 不共享时间线相机
struct PianoRollPlacementIdentity
{
    ContentKey contentKey;
    ContentTimelineProjection projection;

    bool operator== (const PianoRollPlacementIdentity& rhs) const noexcept
    {
        return contentKey == rhs.contentKey && projection.equals (rhs.projection);
    }
};

struct PianoRollViewportPrimitive
{
    double cameraStartSeconds = 0.0;
    double cameraPixelsPerSecond = 100.0;
    float pixelsPerSemitone = 25.0f;
    float verticalScrollOffset = 0.0f;
};

struct PluginPianoRollSessionState
{
    std::optional<PianoRollPlacementIdentity> lastActivePlacement;
    std::vector<std::pair<PianoRollPlacementIdentity, PianoRollViewportPrimitive>> remembered;
};

class DeepSvcAudioProcessor : public juce::AudioProcessor
                            , public juce::AudioProcessorARAExtension
                            , private juce::ChangeListener
{
public:
    DeepSvcAudioProcessor();
    ~DeepSvcAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    // 宿主旁通：渲染源音频（docs/ara.md 第 4.3 节）
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    void didBindToARA() noexcept override;

    DeepSvcDocumentController* getDeepSvcDocumentController() const;

    juce::AudioProcessorValueTreeState apvts;
    PlayHeadState playHeadState;

    void mirrorWorkingParamsFromStore();
    bool isMirroringWorkingParams() const noexcept { return mirroringWorkingParams; }

    // 钢琴卷视口会话记忆：只在消息线程访问，随处理器生命周期存在，不进归档
    void rememberPianoRollViewport (PianoRollPlacementIdentity placement,
                                    PianoRollViewportPrimitive viewport);
    std::optional<PianoRollViewportPrimitive> readPianoRollViewport (
        const PianoRollPlacementIdentity& placement) const;
    std::optional<PianoRollPlacementIdentity> lastActivePianoRollPlacement() const noexcept;
    void setLastActivePianoRollPlacement (const PianoRollPlacementIdentity& placement) noexcept;

private:
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    PluginPianoRollSessionState pianoRollSession;
    bool mirroringWorkingParams = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeepSvcAudioProcessor)
};

} // namespace deepsvc
