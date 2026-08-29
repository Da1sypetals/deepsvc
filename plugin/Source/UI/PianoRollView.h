#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

#include "../Content/ContentKey.h"
#include "../PluginProcessor.h"
#include "../Utils/ContentTimelineProjection.h"
#include "TimelineViewportCamera.h"
#include "TimelineViewportPolicy.h"
#include "ViewMapper.h"
#include "WaveformMipmap.h"

// 对应 OpenTune Source/Standalone/UI/PianoRollComponent.{h,cpp} 的显示层：
// 多区域时间线钢琴卷，活动内容由编辑器推入，播放头自拉
namespace deepsvc
{

// 对应 OpenTune Source/Standalone/UI/PianoRoll/PianoRollRenderer.h 的 TimelineContentPlacement
struct TimelineContentPlacement
{
    ContentKey contentKey;
    ContentTimelineProjection projection;
    juce::Colour displayColour { 0xFFB5446E };  // pink600
    bool hasSynthCoverage { false };
    double synthStartTime { 0.0 };
    double synthEndTime { 0.0 };

    bool isValid() const noexcept { return contentKey.isValid() && projection.isValid(); }
};

class PianoRollView : public juce::Component
                    , private juce::Timer
                    , private juce::ScrollBar::Listener
{
public:
    explicit PianoRollView (const PlayHeadState& playHeadState);
    ~PianoRollView() override;

    // ---- 活动内容（编辑器在心跳里推入） ----
    void setEditedContent (ContentKey key, const ContentTimelineProjection& projection);
    void updateEditedContentData (std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                  std::vector<float> f0Times,
                                  std::vector<float> f0Values,
                                  uint64_t contentRevision);
    void setTimelineContentPlacements (std::vector<TimelineContentPlacement> placements);

    // ---- 视口 ----
    struct ViewportState
    {
        TimelineViewportCamera camera;
        float pixelsPerSemitone = 20.0f;
        float verticalScrollOffset = 0.0f;
    };
    ViewportState viewportState() const noexcept;
    void restoreViewportState (const ViewportState& state);
    void fitToScreen();
    void resetUserZoomFlag() noexcept { userHasManuallyZoomed = false; }

    TimelineViewportCamera camera() const noexcept { return timelineCamera; }
    // 对应 OpenTune PianoRollComponent::getTimelineViewportBounds：去掉右侧垂直滚动条
    juce::Rectangle<int> getTimelineViewportBounds() const
    {
        const int viewportWidth = juce::jmax (0, getWidth() - verticalScrollBar.getWidth());
        return { 0, 0, viewportWidth, getHeight() };
    }
    int getTimelineContentViewportWidth() const;
    int getTimelineContentViewportHeight() const;
    void navigateFromOverview (TimelineViewportRequest request);
    void setCoordinateBottomInset (int pixels) noexcept;

    WaveformMipmapCache& waveforms() noexcept { return waveformCache; }

    // 用户手动改变视口时触发（编辑器用来记忆会话视口）
    std::function<void()> onUserViewportChanged;
    // 在时间轴标尺上按下或拖动左键时，把宿主播放头写到该时刻
    std::function<void (double seconds)> onSeekPlayhead;
    // 按住右下角圆圈 i 时为 true，松开为 false
    std::function<void (bool held)> onInfoHoldChanged;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;
    void mouseMagnify (const juce::MouseEvent& e, float scaleFactor) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

private:
    void timerCallback() override;
    void scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart) override;
    void updateScrollBarRange();

    ViewMapper makeViewMapper() const noexcept;
    TimelineViewportRequest makeViewportRequest (TimelineViewportRequest::Kind kind,
                                                 double targetTime,
                                                 double anchorViewportX,
                                                 double pps) const;
    void commitViewportRequest (TimelineViewportRequest request);
    void activateTimelineCamera (TimelineViewportCamera camera);
    void tryConsumeInitialF0View();
    void zoomHorizontalAt (int mouseX, double factor);
    void zoomVerticalAt (int mouseY, double factor);
    float getTotalHeight() const;
    const TimelineContentPlacement* findEditedPlacement() const noexcept;

    void paintKeyBed (juce::Graphics& g);
    void paintLanes (juce::Graphics& g);
    void paintRuler (juce::Graphics& g);
    void paintPlacements (juce::Graphics& g);
    void paintWaveform (juce::Graphics& g, const TimelineContentPlacement& placement);
    void paintF0Curve (juce::Graphics& g, const TimelineContentPlacement& placement);
    void paintUnsynthesized (juce::Graphics& g, const TimelineContentPlacement& placement);
    void paintPlayhead (juce::Graphics& g);
    void paintCoordinateReadout (juce::Graphics& g);
    void layoutOverlayControls();
    void seekPlayheadAt (int x);
    juce::Rectangle<int> coordinateChipBounds() const noexcept;
    juce::Rectangle<int> infoButtonBounds() const noexcept;

    class InfoHoldButton : public juce::Component
    {
    public:
        InfoHoldButton();
        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        std::function<void (bool held)> onHoldChanged;
    private:
        bool held = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InfoHoldButton)
    };

    const PlayHeadState& playHeadState;

    TimelineViewportCamera timelineCamera;
    float pixelsPerSemitone = 20.0f;
    float verticalScrollOffset = 0.0f;

    static constexpr float minMidi = 24.0f;   // C1
    static constexpr float maxMidi = 108.0f;  // C8
    static constexpr int pianoKeyWidth = 60;
    static constexpr int rulerHeight = 30;

    ContentKey editedContentKey;
    ContentTimelineProjection editedProjection;
    std::shared_ptr<const juce::AudioBuffer<float>> editedAudio;
    std::vector<float> editedF0Times;
    std::vector<float> editedF0Values;
    uint64_t editedContentRevision = 0;
    std::vector<TimelineContentPlacement> placements;

    WaveformMipmapCache waveformCache;
    juce::ScrollBar verticalScrollBar { true };
    InfoHoldButton infoButton;

    bool userHasManuallyZoomed = false;
    bool initialF0ViewPending = false;
    bool initialVerticalCenterPending = true;

    bool isSeeking = false;
    bool isPanning = false;
    double panStartVisibleSeconds = 0.0;
    float panStartVerticalScroll = 0.0f;
    std::optional<juce::Point<int>> mousePosition;
    int coordinateBottomInset = 0;

    static constexpr int kChipWidth = 248;
    static constexpr int kChipHeight = 22;
    static constexpr int kChipMargin = 6;
    static constexpr int kInfoSize = 22;
    static constexpr int kInfoGap = 4;

    double lastPlayheadSeconds = -1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};

} // namespace deepsvc
