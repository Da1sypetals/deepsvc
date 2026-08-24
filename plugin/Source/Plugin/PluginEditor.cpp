#include "PluginEditor.h"

#include <algorithm>
#include <atomic>

#include "../ARA/DeepSvcPlaybackRenderer.h"
#include "../DebugLog.h"
#include "../Directories.h"
#include "../UI/UIColors.h"

namespace deepsvc
{

namespace
{

// 引擎阶段名（native/src/worker.rs、yingmusic-svc-mlx/src/core.rs）映射为中文
juce::String stageText (const juce::String& stage)
{
    if (stage == "detect pitch")     return juce::String (u8"音高检测");
    if (stage == "load audio")       return juce::String (u8"加载音频");
    if (stage == "extract features") return juce::String (u8"提取特征");
    if (stage == "encode content")   return juce::String (u8"编码内容");
    if (stage == "diffusion")        return juce::String (u8"扩散合成");
    if (stage == "pupu vocoder")     return juce::String (u8"一级声码器");
    if (stage == "pc-nsf-hifigan")   return juce::String (u8"二级声码器");
    if (stage == "stitch audio")     return juce::String (u8"拼接音频");
    if (stage == "write output"
        || stage == "write first vocoder output")
        return juce::String (u8"写出结果");
    return juce::String (u8"处理中");
}

juce::String jobStateText (const JobStatus& status)
{
    switch (status.state)
    {
        case JobStatus::State::idle:
            return {};
        case JobStatus::State::queued:
            return status.queuePosition > 0
                ? juce::String (u8"排队中 #") + juce::String (status.queuePosition)
                : juce::String (u8"排队中");
        case JobStatus::State::loadingModels:
            return juce::String (u8"模型加载中");
        case JobStatus::State::running:
            break;
        case JobStatus::State::succeeded:
            return status.elapsedSeconds >= 0.0
                ? juce::String (u8"完成 · 耗时 ") + juce::String (status.elapsedSeconds, 1) + juce::String (u8" 秒")
                : juce::String (u8"完成");
        case JobStatus::State::failed:
            return juce::String (u8"失败：") + status.error;
        case JobStatus::State::cancelled:
            return juce::String (u8"已取消");
    }

    auto text = stageText (status.stage);
    if (status.fraction >= 0.0)
        text << " " << juce::String (juce::roundToInt (status.fraction * 100.0)) << "%";
    return text;
}

bool isJobActive (const JobStatus& status)
{
    return status.state == JobStatus::State::queued
        || status.state == JobStatus::State::loadingModels
        || status.state == JobStatus::State::running;
}

PianoRollViewportPrimitive toPrimitive (const PianoRollView::ViewportState& state)
{
    PianoRollViewportPrimitive primitive;
    primitive.cameraStartSeconds = state.camera.visibleStartSeconds;
    primitive.cameraPixelsPerSecond = state.camera.pixelsPerSecond;
    primitive.pixelsPerSemitone = state.pixelsPerSemitone;
    primitive.verticalScrollOffset = state.verticalScrollOffset;
    return primitive;
}

PianoRollView::ViewportState fromPrimitive (const PianoRollViewportPrimitive& primitive)
{
    PianoRollView::ViewportState state;
    state.camera.visibleStartSeconds = primitive.cameraStartSeconds;
    state.camera.pixelsPerSecond = primitive.cameraPixelsPerSecond;
    state.pixelsPerSemitone = primitive.pixelsPerSemitone;
    state.verticalScrollOffset = primitive.verticalScrollOffset;
    return state;
}

// Studio One 按编辑器构造时的初始尺寸创建容器视图，之后不跟随面板尺寸变化；
// 切换音频片段时编辑器被销毁重建，用上一次的实际尺寸作为初始尺寸，容器才不会与面板错位
std::atomic<int> lastEditorWidth { 1100 };
std::atomic<int> lastEditorHeight { 700 };

} // namespace

DeepSvcEditor::DeepSvcEditor (DeepSvcAudioProcessor& p)
    : juce::AudioProcessorEditor (&p)
    , juce::AudioProcessorEditorARAExtension (&p)
    , audioProcessor (p)
    , timbreLibrary (directories::timbresDirectory())
    , parameterPanel (p.apvts)
    , timbrePanel (timbreLibrary)
    , pianoRoll (p.playHeadState)
    , overviewStrip (pianoRoll.waveforms())
{
    setLookAndFeel (&lookAndFeel);

    detectButton.getProperties().set ("primary", true);
    synthButton.getProperties().set ("primary", true);
    compareButton.setClickingTogglesState (true);
    compareButton.setTooltip (juce::String (u8"按下听原声，弹起听合成结果"));

    detectButton.onClick = [this] { startDetect(); };
    synthButton.onClick = [this] { startSynth(); };
    cancelButton.onClick = [this] { cancelJobs(); };
    compareButton.onClick = [this]
    {
        if (auto* dc = documentController())
            dc->setAbBypass (compareButton.getToggleState());
    };

    addAndMakeVisible (detectButton);
    addAndMakeVisible (synthButton);
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (compareButton);

    statusLabel.setFont (juce::Font (juce::FontOptions (13.0f)));
    statusLabel.setColour (juce::Label::textColourId, UIColors::ink600);
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    staleBadge.setText (juce::String (u8"参数已修改"), juce::dontSendNotification);
    staleBadge.setFont (juce::Font (juce::FontOptions (12.0f)));
    staleBadge.setJustificationType (juce::Justification::centred);
    staleBadge.setColour (juce::Label::textColourId, UIColors::warning);
    staleBadge.setColour (juce::Label::backgroundColourId, UIColors::warningBg);
    addChildComponent (staleBadge);

    addChildComponent (progressBar);

    addAndMakeVisible (parameterPanel);
    addAndMakeVisible (timbrePanel);
    addAndMakeVisible (pianoRoll);
    addChildComponent (overviewStrip);

    overviewStrip.addListener (this);

    // 音色选择随内容持久化
    timbrePanel.onSelectionChanged = [this] (const juce::String& timbre)
    {
        if (auto* dc = documentController())
            if (presentedContentKey.isValid() && timbre.isNotEmpty())
                dc->applyTimbreFile (presentedContentKey, timbre);
    };

    // 用户手动改变视口时记入会话记忆
    pianoRoll.onUserViewportChanged = [this] { rememberPresentedPianoRollViewport(); };

    setResizable (true, true);
    setResizeLimits (860, 480, 2000, 1400);
    setSize (lastEditorWidth.load(), lastEditorHeight.load());
    // 构造期间的 resized（setResizeLimits 钳到最小值、setSize）不是宿主的实际尺寸，之后才开始记录
    sizeRecordingEnabled = true;

    {
        auto* ownRenderer = audioProcessor.getPlaybackRenderer<DeepSvcPlaybackRenderer>();
        debugLog ("editor created p=" + juce::String::toHexString (reinterpret_cast<int64_t> (&audioProcessor))
                  + " renderer=" + (ownRenderer != nullptr
                                        ? juce::String::toHexString (reinterpret_cast<int64_t> (ownRenderer))
                                            + " assigned=" + juce::String (ownRenderer->assignedPlaybackRegions().size())
                                        : juce::String ("null"))
                  + " size=" + juce::String (getWidth()) + "x" + juce::String (getHeight()));
    }
    startTimerHz (kHeartbeatHz);
}

DeepSvcEditor::~DeepSvcEditor()
{
    debugLog ("editor destroyed");
    stopTimer();
    overviewStrip.removeListener (this);
    setLookAndFeel (nullptr);
}

DeepSvcDocumentController* DeepSvcEditor::documentController() const
{
    return audioProcessor.getDeepSvcDocumentController();
}

void DeepSvcEditor::paint (juce::Graphics& g)
{
    g.fillAll (UIColors::pink050);
}

void DeepSvcEditor::parentHierarchyChanged()
{
    juce::AudioProcessorEditor::parentHierarchyChanged();

    auto* parent = getParentComponent();
    debugLog ("editor parentChanged p=" + juce::String::toHexString (reinterpret_cast<int64_t> (&audioProcessor))
              + " parent=" + (parent != nullptr
                                  ? juce::String (parent->getWidth()) + "x" + juce::String (parent->getHeight())
                                  : juce::String ("null"))
              + " mySize=" + juce::String (getWidth()) + "x" + juce::String (getHeight()));
}

void DeepSvcEditor::resized()
{
    debugLog ("editor resized p=" + juce::String::toHexString (reinterpret_cast<int64_t> (&audioProcessor))
              + " size=" + juce::String (getWidth()) + "x" + juce::String (getHeight())
              + " peer=" + juce::String (getPeer() != nullptr ? 1 : 0)
              + " onDesktop=" + juce::String (isOnDesktop() ? 1 : 0));

    if (sizeRecordingEnabled && getWidth() > 0 && getHeight() > 0)
    {
        lastEditorWidth = getWidth();
        lastEditorHeight = getHeight();
    }

    auto bounds = getLocalBounds().reduced (8);

    // 底部操作与状态栏
    auto bar = bounds.removeFromBottom (kBottomBarHeight);
    bounds.removeFromBottom (8);

    // 左栏音色库
    timbrePanel.setBounds (bounds.removeFromLeft (kTimbrePanelWidth));
    bounds.removeFromLeft (8);

    // 右侧参数面板
    parameterPanel.setBounds (bounds.removeFromRight (ParameterPanel::kWidth));
    bounds.removeFromRight (8);

    // 中部钢琴卷
    pianoRoll.setBounds (bounds);

    // Overview strip：覆盖在钢琴卷时间线视口底部
    {
        const auto timelineViewport = pianoRoll.getTimelineViewportBounds();
        const int overviewX = bounds.getX() + 12;
        const int overviewRight = bounds.getX() + timelineViewport.getRight();
        const int overviewBottom = bounds.getY() + timelineViewport.getBottom();
        const int overviewHeight = kOverviewStripHeight + UIColors::scrollBarThickness;
        overviewStrip.setBounds (overviewX,
                                 overviewBottom - overviewHeight,
                                 overviewRight - overviewX,
                                 overviewHeight);
    }

    // 底栏：左侧操作按钮，右侧状态
    auto row = bar.reduced (0, (kBottomBarHeight - 28) / 2);
    detectButton.setBounds (row.removeFromLeft (88));
    row.removeFromLeft (8);
    synthButton.setBounds (row.removeFromLeft (88));
    row.removeFromLeft (8);
    cancelButton.setBounds (row.removeFromLeft (64));
    row.removeFromLeft (8);
    compareButton.setBounds (row.removeFromLeft (88));
    row.removeFromLeft (12);

    progressBar.setBounds (row.removeFromRight (140));
    row.removeFromRight (8);
    staleBadge.setBounds (row.removeFromRight (96));
    row.removeFromRight (8);
    statusLabel.setBounds (row);
}

// ---- 心跳同步 ----

void DeepSvcEditor::timerCallback()
{
    syncContentProjectionToPianoRoll();
    pushEditedContentData();
    updateJobStatusDisplay();

    if (auto* dc = documentController())
    {
        ContentTimelineProjection projection;
        const bool selectionIsFresh = selectionRevisionAtStart.has_value()
                                      && dc->readEditorSelectionRevision() > *selectionRevisionAtStart;
        if (selectionIsFresh)
            if (const auto focused = dc->getFocusedEditorPlaybackRegionProjection())
                projection = focused->toTimelineProjection();

        overviewStrip.onHeartbeatTick (presentedContentKey, projection,
                                       pianoRoll.camera(),
                                       pianoRoll.getTimelineContentViewportWidth());
    }
}

void DeepSvcEditor::syncContentProjectionToPianoRoll()
{
    auto* dc = documentController();
    if (dc == nullptr)
        return;

    if (! selectionRevisionAtStart.has_value())
        selectionRevisionAtStart = dc->readEditorSelectionRevision();

    // 全部有效放置（对应 OpenTune resolveCurrentContentSync 的 entries）
    struct RegionEntry
    {
        PianoRollPlacementIdentity identity;
        TimelineContentPlacement placement;
    };
    std::vector<RegionEntry> entries;
    for (const auto& projection : dc->getPlaybackRegionProjections())
    {
        if (! projection.contentKey.isValid())
            continue;
        const auto localProjection = projection.toTimelineProjection();
        if (! localProjection.isValid())
            continue;

        RegionEntry entry;
        entry.identity = PianoRollPlacementIdentity { projection.contentKey, localProjection };
        entry.placement.contentKey = projection.contentKey;
        entry.placement.projection = localProjection;
        if (projection.displayColour.has_value())
            entry.placement.displayColour = *projection.displayColour;
        entries.push_back (std::move (entry));
    }

    if (entries.empty())
    {
        if (presentedPlacementIdentity.has_value())
        {
            debugLog ("entries empty, clearing presented");
            rememberPresentedPianoRollViewport();
        }
        presentedPlacementIdentity.reset();
        presentedContentKey = ContentKey {};
        presentedContentRevision = 0;
        pianoRoll.setEditedContent ({}, {});
        pianoRoll.setTimelineContentPlacements ({});
        return;
    }

    // 活动放置解析：focused → lastActive → 本实例渲染区域 → earliest
    std::optional<PianoRollPlacementIdentity> activeIdentity;
    const char* resolveSource = "none";
    int rendererAssignedRaw = 0;
    int rendererAssignedMatched = 0;

    bool resolvedFromFocused = false;
    // 存的选区只在本编辑器存活期间有选区通知到达后才可信：
    // 新建 Event FX 时宿主不一定更新选区，共享文档控制器里存的可能是上一会话选中的别的音频块
    const bool selectionIsFresh = dc->readEditorSelectionRevision() > *selectionRevisionAtStart;
    if (selectionIsFresh)
        if (const auto focused = dc->getFocusedEditorPlaybackRegionProjection())
        {
            if (focused->contentKey.isValid())
            {
                PianoRollPlacementIdentity focusedIdentity { focused->contentKey,
                                                             focused->toTimelineProjection() };
                const bool matched = std::any_of (entries.begin(), entries.end(),
                                                  [&] (const RegionEntry& e)
                                                  { return e.identity == focusedIdentity; });
                if (matched)
                {
                    activeIdentity = focusedIdentity;
                    resolvedFromFocused = true;
                    resolveSource = "focused";
                }
            }
        }

    if (! activeIdentity.has_value())
    {
        activeIdentity = audioProcessor.lastActivePianoRollPlacement();
        if (activeIdentity.has_value())
        {
            const bool found = std::any_of (entries.begin(), entries.end(),
                                            [&] (const RegionEntry& e)
                                            { return e.identity == *activeIdentity; });
            if (! found)
                activeIdentity = std::nullopt;
            else
                resolveSource = "lastActive";
        }
    }

    // 本实例渲染的音频块（Event FX：新建实例落在自己所在的音频事件上，取时间线最早的一个）。
    // ARA 文档控制器由同文档的全部实例共享，必须只查本实例自己的回放渲染器角色
    if (! activeIdentity.has_value())
    {
        if (auto* renderer = audioProcessor.getPlaybackRenderer<DeepSvcPlaybackRenderer>())
        {
            const auto assignedProjections = dc->getPlaybackRegionProjectionsFor (renderer->assignedPlaybackRegions());
            rendererAssignedRaw = static_cast<int> (assignedProjections.size());
            for (const auto& projection : assignedProjections)
            {
                if (! projection.contentKey.isValid())
                    continue;
                PianoRollPlacementIdentity identity { projection.contentKey, projection.toTimelineProjection() };
                if (! identity.projection.isValid())
                    continue;
                const bool matched = std::any_of (entries.begin(), entries.end(),
                                                  [&] (const RegionEntry& e)
                                                  { return e.identity == identity; });
                if (! matched)
                    continue;
                ++rendererAssignedMatched;
                if (! activeIdentity.has_value()
                    || identity.projection.timelineStartSeconds < activeIdentity->projection.timelineStartSeconds)
                    activeIdentity = identity;
            }
        }
        if (activeIdentity.has_value())
            resolveSource = "renderer";
    }

    if (! activeIdentity.has_value())
    {
        const auto* earliest = &entries.front();
        for (const auto& e : entries)
            if (e.identity.projection.timelineStartSeconds
                < earliest->identity.projection.timelineStartSeconds)
                earliest = &e;
        activeIdentity = earliest->identity;
        resolveSource = "earliest";
    }


    // 活动放置放在最前，findEditedPlacement 才能解析到它
    const auto activeIt = std::find_if (entries.begin(), entries.end(),
                                        [&] (const RegionEntry& e)
                                        { return e.identity == *activeIdentity; });
    std::vector<TimelineContentPlacement> placements;
    placements.push_back (activeIt->placement);
    for (auto& e : entries)
        if (&e != &*activeIt)
            placements.push_back (std::move (e.placement));

    const bool identityChanged = activeIdentity != presentedPlacementIdentity;
    if (identityChanged && presentedPlacementIdentity.has_value())
        rememberPresentedPianoRollViewport();

    // contentRevision 是每个内容各自计数的，切换内容后必须强制重新推送数据
    if (identityChanged)
        presentedContentRevision = 0;

    pianoRoll.setTimelineContentPlacements (std::move (placements));
    pianoRoll.setEditedContent (activeIdentity->contentKey, activeIdentity->projection);

    // 切换放置：有记忆视口则恢复，否则重置缩放标记后适配（与 OpenTune 一致）
    if (identityChanged)
    {
        debugLog ("identity changed: src=" + juce::String (resolveSource)
                  + " old=" + (presentedPlacementIdentity.has_value()
                                   ? juce::String::toHexString (static_cast<int64_t> (presentedPlacementIdentity->contentKey.objectId))
                                       + "@" + juce::String (presentedPlacementIdentity->projection.timelineStartSeconds, 3)
                                   : juce::String ("none"))
                  + " new=" + juce::String::toHexString (static_cast<int64_t> (activeIdentity->contentKey.objectId))
                  + "@" + juce::String (activeIdentity->projection.timelineStartSeconds, 3)
                  + " entries=" + juce::String (entries.size())
                  + " lastActive=" + (audioProcessor.lastActivePianoRollPlacement().has_value() ? "set" : "empty")
                  + " rendererRaw=" + juce::String (rendererAssignedRaw)
                  + " rendererMatched=" + juce::String (rendererAssignedMatched));

        // lastActive 只记录显式选区解析出的身份。回退解析（渲染器分配、时间线最早）
        // 不写 lastActive，否则新建实例在自身信号到达前落到别的音频块上会被永久锁存
        if (resolvedFromFocused)
            audioProcessor.setLastActivePianoRollPlacement (*activeIdentity);

        timbrePanel.selectTimbre (dc->readTimbreFile (activeIdentity->contentKey));

        if (const auto restored = audioProcessor.readPianoRollViewport (*activeIdentity))
        {
            pianoRoll.restoreViewportState (fromPrimitive (*restored));
        }
        else
        {
            pianoRoll.resetUserZoomFlag();
            pianoRoll.fitToScreen();
        }
    }

    presentedPlacementIdentity = activeIdentity;
    presentedContentKey = activeIdentity->contentKey;
}

void DeepSvcEditor::rememberPresentedPianoRollViewport()
{
    if (! presentedPlacementIdentity.has_value())
        return;
    audioProcessor.rememberPianoRollViewport (*presentedPlacementIdentity,
                                              toPrimitive (pianoRoll.viewportState()));
}

void DeepSvcEditor::pushEditedContentData()
{
    if (! presentedPlacementIdentity.has_value())
        return;

    auto* dc = documentController();
    if (dc == nullptr)
        return;

    const auto revision = dc->readContentRevision (presentedContentKey);
    if (revision == presentedContentRevision)
        return;

    presentedContentRevision = revision;

    std::vector<float> f0Times;
    std::vector<float> f0Values;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;

    if (const auto* content = dc->findContent (presentedContentKey))
    {
        f0Times = content->f0Times;
        f0Values = content->f0Values;

        const bool hasRendered = content->renderedAudio != nullptr
            && ! content->renderedAudio->empty();
        if (dc->isAbBypass() && hasRendered)
        {
            // 对比原声态显示合成结果的波形
            auto rendered = std::make_shared<juce::AudioBuffer<float>> (
                1, static_cast<int> (content->renderedAudio->size()));
            rendered->copyFrom (0, 0, content->renderedAudio->data(),
                                static_cast<int> (content->renderedAudio->size()));
            audio = std::move (rendered);
        }
        else
        {
            audio = dc->readSourceAudio (presentedContentKey);
        }
    }

    pianoRoll.updateEditedContentData (std::move (audio), std::move (f0Times),
                                       std::move (f0Values), revision);
}

void DeepSvcEditor::updateJobStatusDisplay()
{
    auto* dc = documentController();

    JobStatus status;
    bool hasRendered = false;
    bool stale = false;
    if (dc != nullptr && presentedContentKey.isValid())
    {
        status = dc->jobStatusFor (presentedContentKey);
        if (const auto* content = dc->findContent (presentedContentKey))
        {
            hasRendered = content->renderedAudio != nullptr && ! content->renderedAudio->empty();
            if (hasRendered && content->renderedFingerprint.isNotEmpty())
            {
                // 参数或音色与上次合成不一致：结果失效
                const auto current = DeepSvcDocumentController::makeFingerprint (
                    parameters::makeSynthParams (audioProcessor.apvts),
                    timbrePanel.selectedTimbre());
                stale = content->renderedFingerprint != current;
            }
        }
    }

    const bool active = isJobActive (status);
    const bool hasContent = presentedContentKey.isValid();

    detectButton.setEnabled (hasContent && ! active);
    synthButton.setEnabled (hasContent && ! active);
    cancelButton.setEnabled (hasContent && active);
    compareButton.setEnabled (hasRendered && ! active);
    if (dc != nullptr)
        compareButton.setToggleState (dc->isAbBypass(), juce::dontSendNotification);

    auto text = jobStateText (status);
    auto colour = UIColors::ink600;
    if (status.state == JobStatus::State::failed)
        colour = UIColors::failure;
    else if (status.state == JobStatus::State::succeeded)
        colour = UIColors::success;

    // 空闲时显示短暂提示（如未选音色）
    if (text.isEmpty() && transientMessage.isNotEmpty())
    {
        if (juce::Time::getMillisecondCounterHiRes() < transientMessageExpiryMs)
        {
            text = transientMessage;
            colour = UIColors::warning;
        }
        else
        {
            transientMessage.clear();
        }
    }

    statusLabel.setText (text, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, colour);

    progressValue = status.fraction >= 0.0 ? status.fraction : 0.0;
    progressBar.setVisible (active);

    staleBadge.setVisible (stale && ! active);
}

void DeepSvcEditor::overviewNavigateRequested (double visibleStartSeconds, double pixelsPerSecond)
{
    TimelineViewportRequest request;
    request.kind = TimelineViewportRequest::Kind::Manual;
    request.targetTime = visibleStartSeconds;
    request.currentVisibleStartSeconds = pianoRoll.camera().visibleStartSeconds;
    request.viewportWidth = pianoRoll.getTimelineContentViewportWidth();
    request.pixelsPerSecond = pixelsPerSecond;
    pianoRoll.navigateFromOverview (request);
}

// ---- 动作 ----

void DeepSvcEditor::startDetect()
{
    if (! presentedPlacementIdentity.has_value())
        return;

    auto* dc = documentController();
    if (dc == nullptr)
        return;

    const auto estimator = parameters::makeSynthParams (audioProcessor.apvts).f0Estimator;
    dc->requestDetect (presentedContentKey,
                       presentedPlacementIdentity->projection.contentStartSeconds,
                       presentedPlacementIdentity->projection.contentDurationSeconds,
                       estimator);
}

void DeepSvcEditor::startSynth()
{
    if (! presentedPlacementIdentity.has_value())
        return;

    auto* dc = documentController();
    if (dc == nullptr)
        return;

    const auto timbre = timbrePanel.selectedTimbre();
    if (timbre.isEmpty())
    {
        transientMessage = juce::String (u8"请先在音色库中选择参考音频");
        transientMessageExpiryMs = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        return;
    }

    const auto params = parameters::makeSynthParams (audioProcessor.apvts);
    dc->applyTimbreFile (presentedContentKey, timbre);
    dc->requestSynth (presentedContentKey,
                      timbreLibrary.fileFor (timbre).getFullPathName(),
                      params,
                      DeepSvcDocumentController::makeFingerprint (params, timbre));
}

void DeepSvcEditor::cancelJobs()
{
    if (auto* dc = documentController())
        if (presentedContentKey.isValid())
            dc->cancelJobs (presentedContentKey);
}

} // namespace deepsvc
