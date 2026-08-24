#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../ARA/DeepSvcDocumentController.h"
#include "../PluginProcessor.h"
#include "../Timbre/TimbreLibrary.h"
#include "../UI/DeepSvcLookAndFeel.h"
#include "../UI/ParameterPanel.h"
#include "../UI/PianoRollView.h"
#include "../UI/TimbrePanel.h"
#include "../UI/TimelineOverviewComponent.h"

// 布局：左栏音色库，中部钢琴卷，右侧参数面板，底部操作与状态栏（docs/ara.md）
namespace deepsvc
{

class DeepSvcEditor : public juce::AudioProcessorEditor
                    , public juce::AudioProcessorEditorARAExtension
                    , private juce::Timer
                    , private TimelineOverviewComponent::Listener
{
public:
    explicit DeepSvcEditor (DeepSvcAudioProcessor& processor);
    ~DeepSvcEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    // ---- 心跳同步 ----
    void timerCallback() override;

    void syncContentProjectionToPianoRoll();
    void rememberPresentedPianoRollViewport();
    void pushEditedContentData();
    void updateJobStatusDisplay();

    // ---- TimelineOverviewComponent::Listener ----
    void overviewNavigateRequested (double visibleStartSeconds, double pixelsPerSecond) override;

    // ---- 动作 ----
    void startDetect();
    void startSynth();
    void cancelJobs();

    DeepSvcDocumentController* documentController() const;

    DeepSvcAudioProcessor& audioProcessor;
    DeepSvcLookAndFeel lookAndFeel;
    TimbreLibrary timbreLibrary;

    ParameterPanel parameterPanel;
    TimbrePanel timbrePanel;
    PianoRollView pianoRoll;
    TimelineOverviewComponent overviewStrip;

    // 底部操作与状态栏
    juce::TextButton detectButton { juce::String (u8"音高检测") };
    juce::TextButton synthButton { juce::String (u8"开始合成") };
    juce::TextButton cancelButton { juce::String (u8"取消") };
    juce::TextButton compareButton { juce::String (u8"对比原声") };
    juce::Label statusLabel;
    juce::Label staleBadge;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };

    std::optional<PianoRollPlacementIdentity> presentedPlacementIdentity;
    ContentKey presentedContentKey;
    uint64_t presentedContentRevision = 0;

    // 创建时宿主的选区更新计数：只有计数在此之后递增过，存的选区才视为本编辑器的焦点
    std::optional<uint64_t> selectionRevisionAtStart;

    // 构造完成后才开始把尺寸记入会话记忆（供宿主重建编辑器时恢复初始尺寸）
    bool sizeRecordingEnabled = false;

    // 空闲时的短暂状态提示（如未选音色）
    juce::String transientMessage;
    double transientMessageExpiryMs = 0.0;

    static constexpr int kTimbrePanelWidth = 200;
    static constexpr int kBottomBarHeight = 48;
    static constexpr int kOverviewStripHeight = 60;
    static constexpr int kHeartbeatHz = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeepSvcEditor)
};

} // namespace deepsvc
