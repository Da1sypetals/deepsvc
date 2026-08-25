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

    detectButton.onClick = [this] { startDetect(); };
    synthButton.onClick = [this] { startSynth(); };
    cancelButton.onClick = [this] { cancelJobs(); };

    // A/B 槽位切换（docs/ara.md 第 4.1 节）
    slotAButton.setClickingTogglesState (true);
    slotBButton.setClickingTogglesState (true);
    slotAButton.setRadioGroupId (0x5A01);
    slotBButton.setRadioGroupId (0x5A01);
    slotAButton.onClick = [this] { switchToSlot (0); };
    slotBButton.onClick = [this] { switchToSlot (1); };

    bypassButton.setClickingTogglesState (true);
    bypassButton.setTooltip (juce::String (u8"当前槽位直通原声"));
    bypassButton.onClick = [this]
    {
        if (auto* dc = documentController())
            if (presentedContentKey.isValid() && displayedSlot >= 0)
                dc->setSlotBypass (presentedContentKey, displayedSlot, bypassButton.getToggleState());
    };

    playbackIndicator.setFont (juce::Font (juce::FontOptions (12.0f)));
    playbackIndicator.setColour (juce::Label::textColourId, UIColors::ink600);
    playbackIndicator.setJustificationType (juce::Justification::centredLeft);

    addAndMakeVisible (detectButton);
    addAndMakeVisible (synthButton);
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (slotAButton);
    addAndMakeVisible (slotBButton);
    addAndMakeVisible (bypassButton);
    addAndMakeVisible (playbackIndicator);

    // APVTS 参数变化写回激活槽位（docs/ara.md 第 4.1 节）
    for (const auto* id : { &parameters::f0Estimator, &parameters::diffusionSteps,
                            &parameters::pitchShift, &parameters::cfgRate,
                            &parameters::inputGainDb, &parameters::outputVocoder })
        audioProcessor.apvts.addParameterListener (id->getParamID(), this);

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

    // 音色选择写入激活槽位并随内容持久化
    timbrePanel.onSelectionChanged = [this] (const juce::String& timbre)
    {
        if (auto* dc = documentController())
            if (presentedContentKey.isValid() && displayedSlot >= 0 && timbre.isNotEmpty())
                dc->applyTimbreFile (presentedContentKey, displayedSlot, timbre);
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
    for (const auto* id : { &parameters::f0Estimator, &parameters::diffusionSteps,
                            &parameters::pitchShift, &parameters::cfgRate,
                            &parameters::inputGainDb, &parameters::outputVocoder })
        audioProcessor.apvts.removeParameterListener (id->getParamID(), this);
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

    // 诊断：记录完整祖先链尺寸，定位容器错位发生在哪一层
    juce::String chain;
    for (const juce::Component* component = this; component != nullptr; component = component->getParentComponent())
        chain += " [" + juce::String (component->getWidth()) + "x" + juce::String (component->getHeight())
               + " @" + juce::String (component->getX()) + "," + juce::String (component->getY()) + "]";
    if (auto* peer = getPeer())
        chain += " peer=" + peer->getBounds().toString();

    debugLog ("editor parentChanged p=" + juce::String::toHexString (reinterpret_cast<int64_t> (&audioProcessor))
              + " chain:" + chain);

    reconcileParentSize();
}

// Studio One 按编辑器加入时的初始尺寸创建容器视图，之后容器不跟随面板；
// 编辑器被宿主调整为面板实际尺寸后，把容器校正到同一尺寸，消除右侧与底部的黑色遮挡条
void DeepSvcEditor::reconcileParentSize()
{
    if (reconcilingParentSize)
        return;

    auto* parent = getParentComponent();
    if (parent == nullptr || getWidth() <= 0 || getHeight() <= 0)
        return;

    if (parent->getWidth() == getWidth() && parent->getHeight() == getHeight())
        return;

    reconcilingParentSize = true;
    parent->setSize (getWidth(), getHeight());
    reconcilingParentSize = false;

    debugLog ("editor reconcileParent p=" + juce::String::toHexString (reinterpret_cast<int64_t> (&audioProcessor))
              + " to=" + juce::String (getWidth()) + "x" + juce::String (getHeight())
              + " parentNow=" + juce::String (parent->getWidth()) + "x" + juce::String (parent->getHeight()));
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

    // 宿主调整编辑器尺寸后，容器可能仍停留在旧尺寸，校正之
    if (getPeer() != nullptr)
        reconcileParentSize();

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

    // 底栏：左侧操作按钮与 A/B 槽位，右侧状态（docs/ara.md 第 6.2 节）
    auto row = bar.reduced (0, (kBottomBarHeight - 28) / 2);
    detectButton.setBounds (row.removeFromLeft (88));
    row.removeFromLeft (8);
    synthButton.setBounds (row.removeFromLeft (88));
    row.removeFromLeft (8);
    cancelButton.setBounds (row.removeFromLeft (64));
    row.removeFromLeft (12);
    slotAButton.setBounds (row.removeFromLeft (28));
    slotBButton.setBounds (row.removeFromLeft (28));
    row.removeFromLeft (8);
    bypassButton.setBounds (row.removeFromLeft (56));
    row.removeFromLeft (8);
    playbackIndicator.setBounds (row.removeFromLeft (150));
    row.removeFromLeft (8);

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

        // 切换音频块后按新内容的激活槽位同步界面
        displayedSlot = -1;
        syncUiFromActiveSlot();

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
        // 钢琴卷永远显示激活槽位的原音频波形与原音频音高（docs/ara.md 第 6.5 节）
        const auto& slot = content->active();
        f0Times = slot.f0Times;
        f0Values = slot.f0Values;
        audio = slot.sourceAudio;
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
    bool slotBypass = false;
    juce::String playbackText;

    if (dc != nullptr && presentedContentKey.isValid())
    {
        if (const auto* content = dc->findContent (presentedContentKey))
        {
            // 激活槽位被外部改变（归档恢复等）时重新同步界面
            if (displayedSlot != content->activeSlot)
                syncUiFromActiveSlot();

            const auto& slot = content->active();
            status = dc->jobStatusFor (presentedContentKey, content->activeSlot);
            hasRendered = slot.hasRenderedAudio();
            slotBypass = slot.bypass;

            // 参数或音色与上次合成不一致：结果失效
            stale = hasRendered
                && (slot.synthParams != slot.params || slot.synthTimbreFile != slot.timbreFile);

            // 回放指示（docs/ara.md 第 6.7 节）
            if (slotBypass || ! hasRendered)
                playbackText = juce::String (u8"原声");
            else
                playbackText = juce::String (u8"合成 · 声码器 level ")
                    + juce::String (slot.synthParams.keepFirstVocoderOutput ? 1 : 2);
        }
    }

    const bool active = isJobActive (status);
    const bool hasContent = presentedContentKey.isValid();

    detectButton.setEnabled (hasContent && ! active);
    synthButton.setEnabled (hasContent && ! active);
    cancelButton.setEnabled (hasContent && active);
    slotAButton.setEnabled (hasContent);
    slotBButton.setEnabled (hasContent);
    bypassButton.setEnabled (hasContent && ! active);
    bypassButton.setToggleState (slotBypass, juce::dontSendNotification);
    playbackIndicator.setText (playbackText, juce::dontSendNotification);

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
    if (dc == nullptr || displayedSlot < 0)
        return;

    const auto estimator = parameters::makeSynthParams (audioProcessor.apvts).f0Estimator;
    dc->requestDetect (presentedContentKey, displayedSlot, estimator);
}

void DeepSvcEditor::startSynth()
{
    if (! presentedPlacementIdentity.has_value())
        return;

    auto* dc = documentController();
    if (dc == nullptr || displayedSlot < 0)
        return;

    const auto timbre = timbrePanel.selectedTimbre();
    if (timbre.isEmpty())
    {
        transientMessage = juce::String (u8"请先在音色库中选择参考音频");
        transientMessageExpiryMs = juce::Time::getMillisecondCounterHiRes() + 3000.0;
        return;
    }

    const auto params = parameters::makeSynthParams (audioProcessor.apvts);
    dc->applyTimbreFile (presentedContentKey, displayedSlot, timbre);
    dc->requestSynth (presentedContentKey, displayedSlot,
                      timbreLibrary.fileFor (timbre).getFullPathName(),
                      params);
}

void DeepSvcEditor::cancelJobs()
{
    if (auto* dc = documentController())
        if (presentedContentKey.isValid() && displayedSlot >= 0)
            dc->cancelJobs (presentedContentKey, displayedSlot);
}

void DeepSvcEditor::switchToSlot (int slot)
{
    auto* dc = documentController();
    if (dc == nullptr || ! presentedContentKey.isValid())
        return;

    dc->setActiveSlot (presentedContentKey, slot);
    syncUiFromActiveSlot();
}

void DeepSvcEditor::syncUiFromActiveSlot()
{
    auto* dc = documentController();
    if (dc == nullptr || ! presentedContentKey.isValid())
        return;

    const auto* content = dc->findContent (presentedContentKey);
    if (content == nullptr)
        return;

    displayedSlot = content->activeSlot;
    const auto& slot = content->active();

    slotAButton.setToggleState (displayedSlot == 0, juce::dontSendNotification);
    slotBButton.setToggleState (displayedSlot == 1, juce::dontSendNotification);
    bypassButton.setToggleState (slot.bypass, juce::dontSendNotification);
    timbrePanel.selectTimbre (slot.timbreFile);

    // 槽位参数推回 APVTS，参数面板显示激活槽位的值
    syncingParamsFromSlot = true;
    parameters::pushSynthParamsToApvts (audioProcessor.apvts, slot.params);
    syncingParamsFromSlot = false;
}

void DeepSvcEditor::parameterChanged (const juce::String&, float)
{
    if (syncingParamsFromSlot)
        return;

    // 宿主自动化可能在任意线程触发；槽位写回只在消息线程做
    if (! juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync ([this] { parameterChanged ({}, 0.0f); });
        return;
    }

    if (auto* dc = documentController())
        if (presentedContentKey.isValid() && displayedSlot >= 0)
            dc->applySlotParams (presentedContentKey, displayedSlot,
                                 parameters::makeSynthParams (audioProcessor.apvts));
}

} // namespace deepsvc
