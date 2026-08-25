#include "PianoRollView.h"

#include <algorithm>
#include <cmath>

#include "../DebugLog.h"
#include "UIColors.h"

// 对应 OpenTune Source/Standalone/UI/PianoRollComponent.cpp 的显示与视口部分
namespace deepsvc
{

namespace
{

bool isBlackKey (int midi)
{
    switch (midi % 12)
    {
        case 1: case 3: case 6: case 8: case 10: return true;
        default: return false;
    }
}

juce::String noteNameFor (int midi)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[midi % 12]) + juce::String (midi / 12 - 1);
}

// 选择使刻度间距不小于 minPixels 的时间步长
double rulerStepSeconds (double pixelsPerSecond, double minPixels)
{
    static const double steps[] = { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 120.0, 300.0 };
    for (const double step : steps)
        if (step * pixelsPerSecond >= minPixels)
            return step;
    return 600.0;
}

} // namespace

PianoRollView::PianoRollView (const PlayHeadState& playHeadStateRef)
    : playHeadState (playHeadStateRef)
{
    verticalScrollBar.addListener (this);
    addAndMakeVisible (verticalScrollBar);
    startTimerHz (30);
}

PianoRollView::~PianoRollView()
{
    stopTimer();
    verticalScrollBar.removeListener (this);
}

//==============================================================================
// 内容推入

void PianoRollView::setEditedContent (ContentKey key, const ContentTimelineProjection& projection)
{
    if (editedContentKey == key
        && editedProjection.timelineStartSeconds == projection.timelineStartSeconds
        && editedProjection.timelineDurationSeconds == projection.timelineDurationSeconds
        && editedProjection.contentStartSeconds == projection.contentStartSeconds
        && editedProjection.contentDurationSeconds == projection.contentDurationSeconds)
        return;

    editedContentKey = key;
    editedProjection = projection;
    editedAudio.reset();
    editedF0Times.clear();
    editedF0Values.clear();
    editedContentRevision = 0;
    initialF0ViewPending = key.isValid();
    repaint();
}

void PianoRollView::updateEditedContentData (std::shared_ptr<const juce::AudioBuffer<float>> audio,
                                             std::vector<float> f0Times,
                                             std::vector<float> f0Values,
                                             uint64_t contentRevision)
{
    if (contentRevision == editedContentRevision && editedAudio == audio)
        return;

    editedAudio = std::move (audio);
    editedF0Times = std::move (f0Times);
    editedF0Values = std::move (f0Values);
    editedContentRevision = contentRevision;

    if (editedContentKey.isValid() && editedAudio != nullptr)
        waveformCache.setAudioSource (editedContentKey, editedAudio);

    repaint();
}

void PianoRollView::setTimelineContentPlacements (std::vector<TimelineContentPlacement> newPlacements)
{
    // 对应 OpenTune applyTimelineContentPlacements：内容没有变化就不触发重绘
    const bool changed = newPlacements.size() != placements.size()
        || ! std::equal (newPlacements.begin(), newPlacements.end(), placements.begin(),
                         [] (const auto& lhs, const auto& rhs)
                         {
                             return lhs.contentKey == rhs.contentKey
                                 && lhs.displayColour == rhs.displayColour
                                 && std::abs (lhs.projection.timelineStartSeconds - rhs.projection.timelineStartSeconds) <= 1.0e-9
                                 && std::abs (lhs.projection.timelineDurationSeconds - rhs.projection.timelineDurationSeconds) <= 1.0e-9
                                 && std::abs (lhs.projection.contentStartSeconds - rhs.projection.contentStartSeconds) <= 1.0e-9
                                 && std::abs (lhs.projection.contentDurationSeconds - rhs.projection.contentDurationSeconds) <= 1.0e-9;
                         });

    if (! changed)
        return;

    placements = std::move (newPlacements);
    repaint();
}

const TimelineContentPlacement* PianoRollView::findEditedPlacement() const noexcept
{
    for (const auto& placement : placements)
        if (placement.contentKey == editedContentKey)
            return &placement;
    return nullptr;
}

//==============================================================================
// 视口

PianoRollView::ViewportState PianoRollView::viewportState() const noexcept
{
    return { timelineCamera, pixelsPerSemitone, verticalScrollOffset };
}

void PianoRollView::restoreViewportState (const ViewportState& state)
{
    // 恢复镜头代表用户明确的视图意图：阻止初始自动定位
    userHasManuallyZoomed = true;
    initialF0ViewPending = false;
    initialVerticalCenterPending = false;

    timelineCamera = state.camera;
    pixelsPerSemitone = state.pixelsPerSemitone;
    const float maxScroll = juce::jmax (0.0f, getTotalHeight() - static_cast<float> (getTimelineContentViewportHeight()));
    verticalScrollOffset = juce::jlimit (0.0f, maxScroll, state.verticalScrollOffset);

    updateScrollBarRange();
    repaint();
}

void PianoRollView::fitToScreen()
{
    if (userHasManuallyZoomed)
        return;

    // 横向适配内容全长；纵向保持当前键高
    double fitStartSeconds = 0.0;
    double fitEndSeconds = 16.0;
    if (editedProjection.isValid())
    {
        fitStartSeconds = editedProjection.timelineStartSeconds;
        fitEndSeconds = editedProjection.timelineEndSeconds();
    }
    else if (! placements.empty())
    {
        fitStartSeconds = std::numeric_limits<double>::max();
        fitEndSeconds = 0.0;
        for (const auto& placement : placements)
        {
            if (! placement.isValid())
                continue;
            fitStartSeconds = std::min (fitStartSeconds, placement.projection.timelineStartSeconds);
            fitEndSeconds = std::max (fitEndSeconds, placement.projection.timelineEndSeconds());
        }
        if (! (fitEndSeconds > fitStartSeconds))
        {
            fitStartSeconds = 0.0;
            fitEndSeconds = 16.0;
        }
    }

    const int viewWidth = getTimelineContentViewportWidth();
    const double duration = fitEndSeconds - fitStartSeconds;
    if (viewWidth > 0 && duration > 0.0)
    {
        commitViewportRequest (makeViewportRequest (TimelineViewportRequest::Kind::Manual,
                                                    fitStartSeconds,
                                                    0.0,
                                                    static_cast<double> (viewWidth) / duration));
    }
}

int PianoRollView::getTimelineContentViewportWidth() const
{
    return juce::jmax (0, getWidth() - verticalScrollBar.getWidth() - pianoKeyWidth);
}

int PianoRollView::getTimelineContentViewportHeight() const
{
    return juce::jmax (0, getHeight() - rulerHeight);
}

void PianoRollView::navigateFromOverview (TimelineViewportRequest request)
{
    commitViewportRequest (std::move (request));
}

ViewMapper PianoRollView::makeViewMapper() const noexcept
{
    return ViewMapper {
        timelineCamera.visibleStartSeconds,
        timelineCamera.pixelsPerSecond,
        pianoKeyWidth,
        getTimelineContentViewportWidth(),
        getTimelineContentViewportHeight(),
        pixelsPerSemitone,
        verticalScrollOffset,
        maxMidi
    };
}

TimelineViewportRequest PianoRollView::makeViewportRequest (TimelineViewportRequest::Kind kind,
                                                            double targetTime,
                                                            double anchorViewportX,
                                                            double pps) const
{
    TimelineViewportRequest request;
    request.kind = kind;
    request.targetTime = targetTime;
    request.currentVisibleStartSeconds = timelineCamera.visibleStartSeconds;
    request.anchorViewportX = anchorViewportX;
    request.viewportWidth = getTimelineContentViewportWidth();
    request.pixelsPerSecond = pps;
    return request;
}

void PianoRollView::commitViewportRequest (TimelineViewportRequest request)
{
    activateTimelineCamera (TimelineViewportPolicy::resolve (request));
}

void PianoRollView::activateTimelineCamera (TimelineViewportCamera camera)
{
    timelineCamera = camera;
    repaint();
}

float PianoRollView::getTotalHeight() const
{
    return (maxMidi - minMidi + 1.0f) * pixelsPerSemitone;
}

void PianoRollView::zoomHorizontalAt (int mouseX, double factor)
{
    const auto mapper = makeViewMapper();
    const double anchorTime = mapper.xToTime (mouseX);
    const double newPps = timelineCamera.pixelsPerSecond * factor;

    commitViewportRequest (makeViewportRequest (TimelineViewportRequest::Kind::Zoom,
                                                anchorTime,
                                                mouseX - pianoKeyWidth,
                                                newPps));
    userHasManuallyZoomed = true;
    if (onUserViewportChanged)
        onUserViewportChanged();
}

// 纵向缩放：以光标下的音高为锚点调整琴键高度
void PianoRollView::zoomVerticalAt (int mouseY, double factor)
{
    const auto mapper = makeViewMapper();
    const float anchorMidi = mapper.yToMidi (static_cast<float> (mouseY - rulerHeight));

    pixelsPerSemitone = juce::jlimit (5.0f, 40.0f,
                                      pixelsPerSemitone * static_cast<float> (factor));

    verticalScrollOffset = (maxMidi - anchorMidi) * pixelsPerSemitone
        - static_cast<float> (mouseY - rulerHeight);
    const float maxScroll = juce::jmax (0.0f, getTotalHeight() - static_cast<float> (getTimelineContentViewportHeight()));
    verticalScrollOffset = juce::jlimit (0.0f, maxScroll, verticalScrollOffset);

    updateScrollBarRange();
    userHasManuallyZoomed = true;
    repaint();
    if (onUserViewportChanged)
        onUserViewportChanged();
}

void PianoRollView::tryConsumeInitialF0View()
{
    if (! initialF0ViewPending || ! editedContentKey.isValid())
        return;

    if (editedF0Values.empty() || ! editedProjection.isValid())
        return;

    if (! isShowing() || getTimelineContentViewportWidth() <= 0 || getTimelineContentViewportHeight() <= 0)
        return;

    // 在当前投影的 content 区间内寻找首个有效 F0
    const double contentEnd = editedProjection.contentStartSeconds + editedProjection.contentDurationSeconds;
    int firstFrame = -1;
    for (int frame = 0; frame < static_cast<int> (editedF0Values.size()); ++frame)
    {
        const float f0 = editedF0Values[static_cast<size_t> (frame)];
        if (! (std::isfinite (f0) && f0 >= 20.0f && f0 <= 2000.0f))
            continue;
        const double contentSeconds = frame < static_cast<int> (editedF0Times.size())
            ? static_cast<double> (editedF0Times[static_cast<size_t> (frame)])
            : frame * 0.01;
        if (contentSeconds >= editedProjection.contentStartSeconds && contentSeconds < contentEnd)
        {
            firstFrame = frame;
            break;
        }
    }

    if (firstFrame < 0)
    {
        initialF0ViewPending = false;
        return;
    }

    const double firstContentSeconds = firstFrame < static_cast<int> (editedF0Times.size())
        ? static_cast<double> (editedF0Times[static_cast<size_t> (firstFrame)])
        : firstFrame * 0.01;
    const double timelineSeconds = editedProjection.projectContentTimeToTimeline (firstContentSeconds);
    if (! std::isfinite (timelineSeconds))
        return;

    const auto request = makeViewportRequest (TimelineViewportRequest::Kind::Manual,
                                              timelineSeconds,
                                              0.0,
                                              timelineCamera.pixelsPerSecond);
    const auto resolvedCamera = TimelineViewportPolicy::resolve (request);

    // 纵向适配到窗口内 F0 的音高范围，键高限制在可读区间
    const auto mapper = makeViewMapper();
    float highestMidi = -std::numeric_limits<float>::infinity();
    float lowestMidi = std::numeric_limits<float>::infinity();
    for (size_t frame = 0; frame < editedF0Values.size(); ++frame)
    {
        const float f0 = editedF0Values[frame];
        if (! (std::isfinite (f0) && f0 >= 20.0f && f0 <= 2000.0f))
            continue;
        const double contentSeconds = frame < editedF0Times.size()
            ? static_cast<double> (editedF0Times[frame])
            : static_cast<double> (frame) * 0.01;
        if (contentSeconds < editedProjection.contentStartSeconds || contentSeconds >= contentEnd)
            continue;
        const float midi = mapper.freqToMidi (f0);
        highestMidi = std::max (highestMidi, midi);
        lowestMidi = std::min (lowestMidi, midi);
    }

    const int contentHeight = getTimelineContentViewportHeight();
    if (highestMidi > lowestMidi && contentHeight > 0)
    {
        const float midiSpan = highestMidi - lowestMidi + 2.0f;
        pixelsPerSemitone = juce::jlimit (5.0f, 40.0f,
                                          static_cast<float> (contentHeight) / midiSpan);
        verticalScrollOffset = (maxMidi - highestMidi - 1.0f) * pixelsPerSemitone;
        verticalScrollOffset = std::clamp (verticalScrollOffset, 0.0f,
                                           juce::jmax (0.0f, getTotalHeight() - static_cast<float> (contentHeight)));
    }

    activateTimelineCamera (resolvedCamera);
    updateScrollBarRange();
    initialF0ViewPending = false;
}

//==============================================================================
// 布局

void PianoRollView::resized()
{
    verticalScrollBar.setBounds (getWidth() - UIColors::scrollBarThickness, 0,
                                 UIColors::scrollBarThickness, getHeight());

    // 默认视图：C4 纵向居中
    if (initialVerticalCenterPending && getTimelineContentViewportHeight() > 0)
    {
        initialVerticalCenterPending = false;
        const float contentHeight = static_cast<float> (getTimelineContentViewportHeight());
        verticalScrollOffset = (maxMidi - 60.5f) * pixelsPerSemitone - contentHeight * 0.5f;
        const float maxScroll = juce::jmax (0.0f, getTotalHeight() - contentHeight);
        verticalScrollOffset = juce::jlimit (0.0f, maxScroll, verticalScrollOffset);
    }

    updateScrollBarRange();
}

void PianoRollView::updateScrollBarRange()
{
    const float totalHeight = getTotalHeight();
    const int visibleHeight = juce::jmax (1, getTimelineContentViewportHeight());
    verticalScrollBar.setRangeLimits (0.0, static_cast<double> (totalHeight), juce::dontSendNotification);
    verticalScrollBar.setCurrentRange (verticalScrollOffset, visibleHeight, juce::dontSendNotification);
}

void PianoRollView::scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar != &verticalScrollBar)
        return;

    const float maxScroll = juce::jmax (0.0f, getTotalHeight() - static_cast<float> (getTimelineContentViewportHeight()));
    verticalScrollOffset = juce::jlimit (0.0f, maxScroll, static_cast<float> (newRangeStart));
    repaint();
}

//==============================================================================
// 交互：滚轮上下滚动、Shift+滚轮左右滚动、Cmd+滚轮纵向缩放、Cmd+Shift+滚轮横向缩放、
// 触控板双指两轴滚动、捏合横向缩放、双击适配全长、拖拽双轴平移（docs/ara.md 第 6.5 节）

void PianoRollView::mouseDown (const juce::MouseEvent& e)
{
    if (e.x < pianoKeyWidth)
        return;

    isPanning = true;
    panStartPos = e.getPosition();
    panStartVisibleStart = timelineCamera.visibleStartSeconds;
    panStartVerticalOffset = verticalScrollOffset;
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void PianoRollView::mouseDrag (const juce::MouseEvent& e)
{
    if (! isPanning)
        return;

    const int deltaX = e.x - panStartPos.x;
    const int deltaY = e.y - panStartPos.y;

    // 自由平移：两个轴同时跟随拖拽
    const float maxScroll = juce::jmax (0.0f, getTotalHeight() - static_cast<float> (getTimelineContentViewportHeight()));
    verticalScrollOffset = juce::jlimit (0.0f, maxScroll, panStartVerticalOffset - static_cast<float> (deltaY));
    updateScrollBarRange();

    const double newVisibleStart = panStartVisibleStart - deltaX / timelineCamera.pixelsPerSecond;
    commitViewportRequest (makeViewportRequest (TimelineViewportRequest::Kind::Manual,
                                                newVisibleStart,
                                                0.0,
                                                timelineCamera.pixelsPerSecond));
    repaint();

    userHasManuallyZoomed = true;
    if (onUserViewportChanged)
        onUserViewportChanged();
}

void PianoRollView::mouseUp (const juce::MouseEvent&)
{
    isPanning = false;
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void PianoRollView::mouseMove (const juce::MouseEvent& e)
{
    mousePosition = e.getPosition();
    repaint();
}

void PianoRollView::mouseExit (const juce::MouseEvent&)
{
    mousePosition.reset();
    repaint();
}

void PianoRollView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // 诊断：cmd+shift+滚轮无响应排查，记录修饰键与 delta 原始值
    debugLog ("wheel cmd=" + juce::String (e.mods.isCommandDown() ? 1 : 0)
              + " shift=" + juce::String (e.mods.isShiftDown() ? 1 : 0)
              + " dX=" + juce::String (wheel.deltaX, 4)
              + " dY=" + juce::String (wheel.deltaY, 4)
              + " smooth=" + juce::String (wheel.isSmooth ? 1 : 0)
              + " x=" + juce::String (e.x));

    if (e.x < pianoKeyWidth)
        return;

    // 操作逻辑与 Studio One 一致（docs/ara.md 第 6.5 节）：
    // 滚轮上下滚动，Shift+滚轮左右滚动，Cmd+滚轮纵向缩放，Cmd+Shift+滚轮横向缩放。
    // macOS 在按住 Shift 时把垂直滚轮转换成横向 delta（deltaY 恒为 0，滚动量在
    // deltaX），鼠标路径取两者中非零的一个。
    // delta 归一化为滚轮格数：JUCE 在 macOS 对鼠标滚轮的换算是 10/256（一格约
    // 0.039），对触控板是 pixels × 0.5/256（120 像素折一格）
    const float mouseDelta = wheel.deltaY != 0.0f ? wheel.deltaY : wheel.deltaX;
    const double wheelNotches = wheel.isSmooth
        ? static_cast<double> (wheel.deltaY) * 512.0 / 120.0
        : static_cast<double> (mouseDelta) * 25.6;

    if (e.mods.isCommandDown() && e.mods.isShiftDown())
    {
        // Cmd+Shift+滚轮：以光标为锚点横向缩放（时间轴），每格 1.25 倍
        zoomHorizontalAt (e.x, std::pow (1.25, wheelNotches));
        return;
    }

    if (e.mods.isCommandDown())
    {
        // Cmd+滚轮 / 触控板 Cmd+上下滑动：以光标为锚点纵向缩放（琴键高度）
        zoomVerticalAt (e.y, std::pow (1.25, wheelNotches));
        return;
    }

    if (wheel.isSmooth)
    {
        // 触控板双指：上下左右同时滚动两个轴
        constexpr float kTrackpadPixelsPerDelta = 512.0f;
        if (wheel.deltaX != 0.0f)
        {
            const double newVisibleStart = timelineCamera.visibleStartSeconds
                - wheel.deltaX * kTrackpadPixelsPerDelta / timelineCamera.pixelsPerSecond;
            commitViewportRequest (makeViewportRequest (TimelineViewportRequest::Kind::Manual,
                                                        newVisibleStart,
                                                        0.0,
                                                        timelineCamera.pixelsPerSecond));
        }
        if (wheel.deltaY != 0.0f)
        {
            const float maxScroll = juce::jmax (0.0f, getTotalHeight() - static_cast<float> (getTimelineContentViewportHeight()));
            verticalScrollOffset = juce::jlimit (0.0f, maxScroll,
                                                 verticalScrollOffset - wheel.deltaY * kTrackpadPixelsPerDelta);
            updateScrollBarRange();
            repaint();
        }
        if (onUserViewportChanged)
            onUserViewportChanged();
        return;
    }

    if (e.mods.isShiftDown())
    {
        // 鼠标 Shift+滚轮：横向滚动，每格 120 像素
        const double newVisibleStart = timelineCamera.visibleStartSeconds
            - wheelNotches * 120.0 / timelineCamera.pixelsPerSecond;
        commitViewportRequest (makeViewportRequest (TimelineViewportRequest::Kind::Manual,
                                                    newVisibleStart,
                                                    0.0,
                                                    timelineCamera.pixelsPerSecond));
        if (onUserViewportChanged)
            onUserViewportChanged();
        return;
    }

    // 鼠标滚轮：纵向滚动，每格 120 像素
    const float maxScroll = juce::jmax (0.0f, getTotalHeight() - static_cast<float> (getTimelineContentViewportHeight()));
    verticalScrollOffset = juce::jlimit (0.0f, maxScroll,
                                         verticalScrollOffset - static_cast<float> (wheelNotches * 120.0));
    updateScrollBarRange();
    repaint();
}

void PianoRollView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (e.x < pianoKeyWidth || scaleFactor <= 0.0f)
        return;

    // 触控板捏合：以光标为锚点横向缩放
    zoomHorizontalAt (e.x, static_cast<double> (scaleFactor));
}

void PianoRollView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.x < pianoKeyWidth)
        return;

    userHasManuallyZoomed = false;
    fitToScreen();
    if (onUserViewportChanged)
        onUserViewportChanged();
}

//==============================================================================
// 心跳

void PianoRollView::timerCallback()
{
    tryConsumeInitialF0View();

    bool needsRepaint = false;

    if (waveformCache.buildIncremental (4.0))
        needsRepaint = true;

    const double playheadSeconds = playHeadState.getPresentedPositionSeconds();
    if (playheadSeconds != lastPlayheadSeconds)
    {
        lastPlayheadSeconds = playheadSeconds;
        needsRepaint = true;
    }

    if (needsRepaint)
        repaint();
}

//==============================================================================
// 绘制

void PianoRollView::paint (juce::Graphics& g)
{
    g.fillAll (UIColors::pink050);

    paintLanes (g);
    paintPlacements (g);

    if (const auto* placement = findEditedPlacement())
    {
        paintWaveform (g, *placement);
        paintF0Curve (g, *placement);
    }

    paintKeyBed (g);
    paintRuler (g);
    paintPlayhead (g);
    paintCoordinateReadout (g);
}

void PianoRollView::paintLanes (juce::Graphics& g)
{
    const auto mapper = makeViewMapper();
    const int contentLeft = pianoKeyWidth;
    const int contentRight = pianoKeyWidth + getTimelineContentViewportWidth();
    const int top = rulerHeight;
    const int bottom = rulerHeight + getTimelineContentViewportHeight();

    juce::Graphics::ScopedSaveState scoped (g);
    g.reduceClipRegion (contentLeft, top, contentRight - contentLeft, bottom - top);

    const int firstMidi = static_cast<int> (std::floor (mapper.yToMidi (static_cast<float> (bottom)))) - 1;
    const int lastMidi = static_cast<int> (std::ceil (mapper.yToMidi (static_cast<float> (top)))) + 1;

    for (int midi = juce::jmax (firstMidi, static_cast<int> (minMidi));
         midi <= juce::jmin (lastMidi, static_cast<int> (maxMidi));
         ++midi)
    {
        const float yTop = rulerHeight + mapper.midiToY (static_cast<float> (midi) + 1.0f);
        const float yBottom = rulerHeight + mapper.midiToY (static_cast<float> (midi));

        // 黑键行铺浅粉，半音线 pink100，C 行线 pink200
        if (isBlackKey (midi))
        {
            g.setColour (UIColors::pink100);
            g.fillRect (static_cast<float> (contentLeft), yTop,
                        static_cast<float> (contentRight - contentLeft), yBottom - yTop);
        }

        g.setColour (midi % 12 == 0 ? UIColors::pink200 : UIColors::pink100);
        g.drawHorizontalLine (static_cast<int> (std::round (yBottom)),
                              static_cast<float> (contentLeft), static_cast<float> (contentRight));
    }
}

void PianoRollView::paintKeyBed (juce::Graphics& g)
{
    const auto mapper = makeViewMapper();
    const int top = rulerHeight;
    const int bottom = rulerHeight + getTimelineContentViewportHeight();

    auto bedArea = juce::Rectangle<int> (0, top, pianoKeyWidth, bottom - top);
    g.setColour (UIColors::keyWhite);
    g.fillRect (bedArea);

    juce::Graphics::ScopedSaveState scoped (g);
    g.reduceClipRegion (bedArea);

    const int firstMidi = static_cast<int> (std::floor (mapper.yToMidi (static_cast<float> (bottom)))) - 1;
    const int lastMidi = static_cast<int> (std::ceil (mapper.yToMidi (static_cast<float> (top)))) + 1;

    // 白键：整行白色 + pink200 分隔线；黑键：左侧叠深色键
    for (int midi = juce::jmax (firstMidi, static_cast<int> (minMidi));
         midi <= juce::jmin (lastMidi, static_cast<int> (maxMidi));
         ++midi)
    {
        const float yTop = static_cast<float> (top) + mapper.midiToY (static_cast<float> (midi) + 1.0f);
        const float yBottom = static_cast<float> (top) + mapper.midiToY (static_cast<float> (midi));
        const float rowHeight = yBottom - yTop;

        if (isBlackKey (midi))
        {
            g.setColour (UIColors::keyBlack);
            g.fillRect (0.0f, yTop, pianoKeyWidth * 0.62f, juce::jmax (1.0f, rowHeight));
        }
        else
        {
            g.setColour (UIColors::pink200);
            g.drawHorizontalLine (static_cast<int> (std::round (yBottom)),
                                  0.0f, static_cast<float> (pianoKeyWidth));
        }

        if (midi % 12 == 0 && rowHeight >= 12.0f)
        {
            g.setColour (UIColors::ink600);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (noteNameFor (midi), 0, static_cast<int> (yTop),
                        pianoKeyWidth - 6, static_cast<int> (rowHeight),
                        juce::Justification::centredRight, false);
        }
    }

    g.setColour (UIColors::pink200);
    g.drawVerticalLine (pianoKeyWidth - 1, static_cast<float> (top), static_cast<float> (bottom));
}

void PianoRollView::paintRuler (juce::Graphics& g)
{
    auto rulerArea = juce::Rectangle<int> (pianoKeyWidth, 0,
                                           getTimelineContentViewportWidth(), rulerHeight);
    g.setColour (UIColors::pink100);
    g.fillRect (rulerArea);

    const auto mapper = makeViewMapper();
    const double step = rulerStepSeconds (timelineCamera.pixelsPerSecond, 70.0);
    const double startTime = mapper.xToTime (pianoKeyWidth);
    const double endTime = mapper.xToTime (pianoKeyWidth + getTimelineContentViewportWidth());

    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    for (double t = std::floor (startTime / step) * step; t <= endTime; t += step)
    {
        if (t < 0.0)
            continue;
        const int x = mapper.timeToX (t);
        g.setColour (UIColors::pink300);
        g.drawVerticalLine (x, static_cast<float> (rulerHeight - 8), static_cast<float> (rulerHeight));
        g.setColour (UIColors::ink600);
        g.drawText (juce::String (t, step < 1.0 ? 1 : 0) + " s",
                    x + 3, 0, 60, rulerHeight - 8,
                    juce::Justification::centredLeft, false);
    }

    g.setColour (UIColors::pink200);
    g.drawHorizontalLine (rulerHeight - 1, static_cast<float> (pianoKeyWidth),
                          static_cast<float> (pianoKeyWidth + getTimelineContentViewportWidth()));
}

void PianoRollView::paintPlacements (juce::Graphics& g)
{
    const auto mapper = makeViewMapper();
    const int top = rulerHeight;
    const int height = getTimelineContentViewportHeight();

    juce::Graphics::ScopedSaveState scoped (g);
    g.reduceClipRegion (pianoKeyWidth, top, getTimelineContentViewportWidth(), height);

    for (const auto& placement : placements)
    {
        if (! placement.isValid())
            continue;

        const int x1 = mapper.timeToX (placement.projection.timelineStartSeconds);
        const int x2 = mapper.timeToX (placement.projection.timelineEndSeconds());
        if (x2 < pianoKeyWidth || x1 > pianoKeyWidth + getTimelineContentViewportWidth())
            continue;

        const bool isActive = placement.contentKey == editedContentKey;
        auto colour = placement.displayColour;

        g.setColour (colour.withAlpha (isActive ? 0.14f : 0.07f));
        g.fillRect (juce::Rectangle<int> (x1, top, x2 - x1, height));

        g.setColour (colour.withAlpha (isActive ? 0.9f : 0.4f));
        g.drawRect (juce::Rectangle<int> (x1, top, x2 - x1, height), isActive ? 2 : 1);
    }
}

void PianoRollView::paintWaveform (juce::Graphics& g, const TimelineContentPlacement& placement)
{
    const auto* mipmap = waveformCache.get (placement.contentKey);
    if (mipmap == nullptr || ! mipmap->hasSource())
        return;

    const int levelIndex = mipmap->selectBestLevelIndex (timelineCamera.pixelsPerSecond);
    if (levelIndex < 0)
        return;
    const auto& level = mipmap->getLevel (levelIndex);

    const auto mapper = makeViewMapper();
    const int top = rulerHeight;
    const int height = getTimelineContentViewportHeight();
    const int contentRight = pianoKeyWidth + getTimelineContentViewportWidth();

    const int clipX1 = juce::jmax (pianoKeyWidth, mapper.timeToX (placement.projection.timelineStartSeconds));
    const int clipX2 = juce::jmin (contentRight, mapper.timeToX (placement.projection.timelineEndSeconds()));
    if (clipX2 <= clipX1)
        return;

    const int64_t numPeaks = static_cast<int64_t> (level.peaks.size());
    const double timePerPeak = static_cast<double> (WaveformMipmap::kSamplesPerPeak[levelIndex])
        / WaveformMipmap::kBaseSampleRate;
    const float midY = top + height * 0.5f;
    const float halfH = height * 0.45f;

    juce::Graphics::ScopedSaveState scoped (g);
    g.reduceClipRegion (clipX1, top, clipX2 - clipX1, height);

    g.setColour (UIColors::pink300.withAlpha (0.7f));
    for (int x = clipX1; x < clipX2; ++x)
    {
        const double timelineTime = mapper.xToTime (x);
        if (timelineTime < placement.projection.timelineStartSeconds
            || timelineTime >= placement.projection.timelineEndSeconds())
            continue;

        const double contentTime = placement.projection.projectTimelineTimeToContent (timelineTime);
        const auto peakIndex = static_cast<int64_t> (std::floor (contentTime / timePerPeak));
        if (peakIndex < 0 || peakIndex >= numPeaks)
            continue;

        const auto& peak = level.peaks[static_cast<size_t> (peakIndex)];
        if (peak.isZero())
            continue;

        float y1 = midY - peak.getMax() * halfH;
        float y2 = midY - peak.getMin() * halfH;
        if (y2 - y1 < 1.0f)
        {
            const float expand = (1.0f - (y2 - y1)) * 0.5f;
            y1 -= expand;
            y2 += expand;
        }
        g.drawVerticalLine (x, y1, y2);
    }
}

void PianoRollView::paintF0Curve (juce::Graphics& g, const TimelineContentPlacement& placement)
{
    if (editedF0Values.empty())
        return;

    const auto mapper = makeViewMapper();
    const int top = rulerHeight;
    const int height = getTimelineContentViewportHeight();
    const int contentRight = pianoKeyWidth + getTimelineContentViewportWidth();

    juce::Graphics::ScopedSaveState scoped (g);
    g.reduceClipRegion (pianoKeyWidth, top, contentRight - pianoKeyWidth, height);

    juce::Path curve;
    bool penDown = false;
    const size_t frameCount = juce::jmin (editedF0Times.size(), editedF0Values.size());
    const double contentWindowEnd = placement.projection.contentStartSeconds
        + placement.projection.contentDurationSeconds;

    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        const float f0 = editedF0Values[frame];
        const bool voiced = std::isfinite (f0) && f0 >= 20.0f && f0 <= 2000.0f;
        if (! voiced)
        {
            penDown = false;
            continue;
        }

        // F0 覆盖整个源内容，只画落在本区域内容窗口内的帧
        const double contentSeconds = static_cast<double> (editedF0Times[frame]);
        if (contentSeconds < placement.projection.contentStartSeconds
            || contentSeconds >= contentWindowEnd)
        {
            penDown = false;
            continue;
        }

        const double timelineSeconds = placement.projection.projectContentTimeToTimeline (contentSeconds);
        const float x = static_cast<float> (mapper.timeToX (timelineSeconds));
        const float y = top + mapper.freqToY (f0);

        if (x < pianoKeyWidth - 8 || x > contentRight + 8)
        {
            penDown = false;
            continue;
        }

        if (penDown)
            curve.lineTo (x, y);
        else
            curve.startNewSubPath (x, y);
        penDown = true;
    }

    g.setColour (UIColors::pink600);
    g.strokePath (curve, juce::PathStrokeType (2.0f));
}

void PianoRollView::paintPlayhead (juce::Graphics& g)
{
    const auto mapper = makeViewMapper();
    const int x = mapper.timeToX (playHeadState.getPresentedPositionSeconds());
    const int contentRight = pianoKeyWidth + getTimelineContentViewportWidth();
    if (x < pianoKeyWidth || x > contentRight)
        return;

    g.setColour (UIColors::pink900);
    g.drawVerticalLine (x, 0.0f, static_cast<float> (rulerHeight + getTimelineContentViewportHeight()));
}

void PianoRollView::paintCoordinateReadout (juce::Graphics& g)
{
    if (! mousePosition.has_value() || mousePosition->x < pianoKeyWidth
        || mousePosition->y < rulerHeight)
        return;

    const auto mapper = makeViewMapper();
    const double seconds = mapper.xToTime (mousePosition->x);
    const float midiFloat = mapper.yToMidi (static_cast<float> (mousePosition->y - rulerHeight));
    const int midiRounded = juce::jlimit (0, 127, juce::roundToInt (midiFloat));
    const float freq = mapper.yToFreq (static_cast<float> (mousePosition->y - rulerHeight));
    const int cents = juce::roundToInt ((midiFloat - static_cast<float> (midiRounded)) * 100.0f);

    juce::String text = juce::String (seconds, 2) + juce::String (u8" 秒  ")
        + noteNameFor (midiRounded)
        + (cents >= 0 ? juce::String (u8" +") : juce::String (u8" "))
        + juce::String (cents) + juce::String (u8" 音分  ")
        + juce::String (freq, 1) + juce::String (u8" Hz");

    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    const int textWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 12;
    const int contentRight = pianoKeyWidth + getTimelineContentViewportWidth();
    const int contentBottom = rulerHeight + getTimelineContentViewportHeight();

    auto area = juce::Rectangle<int> (contentRight - textWidth - 6, contentBottom - 24,
                                      textWidth, 18);
    g.setColour (UIColors::pink100);
    g.fillRoundedRectangle (area.toFloat(), UIColors::controlCornerRadius);
    g.setColour (UIColors::pink200);
    g.drawRoundedRectangle (area.toFloat(), UIColors::controlCornerRadius, 1.0f);
    g.setColour (UIColors::ink900);
    g.drawText (text, area, juce::Justification::centred, false);
}

} // namespace deepsvc
