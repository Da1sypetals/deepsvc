#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../ARA/DeepSvcDocumentController.h"
#include "../PluginProcessor.h"
#include "../Timbre/TimbreLibrary.h"
#include "../UI/DeepSvcLookAndFeel.h"
#include "../UI/PianoRollView.h"
#include "../UI/ProcessCell.h"
#include "../UI/RightColumn.h"
#include "../UI/TimbrePanel.h"
#include "../UI/TimelineOverviewComponent.h"

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
    void applyProcessorWorkingTimbre();

private:
    void timerCallback() override;

    void syncContentProjectionToPianoRoll();
    void rememberPresentedPianoRollViewport();
    void pushEditedContentData();
    void updateJobStatusDisplay();
    void confirmClearSynth();
    void requestHostPlaybackPosition (double seconds);

    void overviewNavigateRequested (double visibleStartSeconds, double pixelsPerSecond) override;

    void startDetect();
    void startSynth();
    void cancelJobs();
    void switchToSlot (int slot);
    void syncDisplayedSlot();

    void parameterChanged (const juce::String& parameterID, float newValue) override;

    DeepSvcDocumentController* documentController() const;

    DeepSvcAudioProcessor& audioProcessor;
    DeepSvcLookAndFeel lookAndFeel;
    TimbreLibrary timbreLibrary;

    RightColumn rightColumn;
    TimbrePanel timbrePanel;
    PianoRollView pianoRoll;
    TimelineOverviewComponent overviewStrip;
    ProcessCell processCell;

    juce::TextButton detectButton { juce::String (u8"音高检测") };
    juce::TextButton synthButton { juce::String (u8"开始合成") };
    juce::TextButton cancelButton { juce::String (u8"取消") };

    std::optional<PianoRollPlacementIdentity> presentedPlacementIdentity;
    ContentKey presentedContentKey;
    uint64_t presentedContentRevision = 0;
    int displayedSlot = -1;

    std::optional<uint64_t> selectionRevisionAtStart;

    bool sizeRecordingEnabled = false;

    void reconcileParentSize();
    bool reconcilingParentSize = false;

    juce::String transientMessage;
    double transientMessageExpiryMs = 0.0;

    bool lingerPosted = false;
    juce::String lingerText;
    juce::Colour lingerColour { 0xFF7A5C68 };
    double lingerUntilMs = 0.0;

    bool applyingWorkingTimbre = false;

    static constexpr int kTimbrePanelWidth = 200;
    static constexpr int kBottomBarHeight = 48;
    static constexpr int kOverviewStripHeight = 60;
    static constexpr int kHeartbeatHz = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeepSvcEditor)
};

} // namespace deepsvc
