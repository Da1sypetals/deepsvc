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

// 对应 OpenTune Source/Plugin/PluginEditor.h 的 ARA 路径（编辑器心跳、内容推送、焦点解析）。
// 布局：左栏音色库，中部钢琴卷，右侧参数面板，底部操作与状态栏（docs/ara.md）
namespace deepsvc
{

class DeepSvcEditor : public juce::AudioProcessorEditor
                    , public juce::AudioProcessorEditorARAExtension
                    , private juce::Timer
                    , private juce::AudioProcessorValueTreeState::Listener
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
    void switchToSlot (int slot);

    // 激活槽位变化后：参数推回 APVTS、音色选择、旁通按钮（docs/ara.md 第 4.1 节）
    void syncUiFromActiveSlot();

    // APVTS 参数变化写回激活槽位
    void parameterChanged (const juce::String& parameterID, float newValue) override;

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
    // A/B 槽位切换（docs/ara.md 第 4.1、6.2 节）
    juce::TextButton slotAButton { juce::String (u8"A") };
    juce::TextButton slotBButton { juce::String (u8"B") };
    // 激活槽位的旁通：直通原声
    juce::TextButton bypassButton { juce::String (u8"旁通") };
    // 回放指示：当前播放的是原声还是合成结果
    juce::Label playbackIndicator;
    juce::Label statusLabel;
    juce::Label staleBadge;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };

    std::optional<PianoRollPlacementIdentity> presentedPlacementIdentity;
    ContentKey presentedContentKey;
    uint64_t presentedContentRevision = 0;
    // 界面当前展示的槽位；-1 = 尚未同步
    int displayedSlot = -1;
    // 槽位切换推参数进 APVTS 期间屏蔽 parameterChanged 回写
    bool syncingParamsFromSlot = false;

    // 创建时宿主的选区更新计数：只有计数在此之后递增过，存的选区才视为本编辑器的焦点
    std::optional<uint64_t> selectionRevisionAtStart;

    // 构造完成后才开始把尺寸记入会话记忆（供宿主重建编辑器时恢复初始尺寸）
    bool sizeRecordingEnabled = false;

    // Studio One 容器尺寸校正（见 PluginEditor.cpp reconcileParentSize）
    void reconcileParentSize();
    bool reconcilingParentSize = false;

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
