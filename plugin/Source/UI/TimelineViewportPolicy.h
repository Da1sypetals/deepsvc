#pragma once

#include "TimelineViewportCamera.h"

// 对应 OpenTune Source/Standalone/UI/TimelineViewportPolicy.h（只有钢琴卷一个视图）
namespace deepsvc
{

struct TimelineViewportRequest
{
    enum class Kind
    {
        Cont,    // 连续滚动：居中 targetTime
        Page,    // 翻页：显示包含 targetTime 的页
        Manual,  // 手动拖拽：targetTime 即期望的 visibleStartSeconds
        Click,   // 点击定位：把 targetTime 放到 anchorViewportX
        Zoom     // 以鼠标为锚点缩放
    };

    Kind kind = Kind::Manual;
    double targetTime = 0.0;
    double currentVisibleStartSeconds = 0.0;
    double anchorViewportX = 0.0;
    int viewportWidth = 0;
    double pixelsPerSecond = TimelineViewportCamera::kDefaultPixelsPerSecond;
};

class TimelineViewportPolicy
{
public:
    static constexpr double kMinPixelsPerSecond = 10.0;
    static constexpr double kMaxPixelsPerSecond = 500.0;

    static double normalisePixelsPerSecond (double pps) noexcept;

    // 唯一入口：根据 request 计算 camera
    static TimelineViewportCamera resolve (const TimelineViewportRequest& request);

    // 钳制 visibleStartSeconds >= 0
    static double clampStartSeconds (double startSeconds);

    static double visibleEndSeconds (const TimelineViewportCamera& camera, int viewportWidth);
};

} // namespace deepsvc
