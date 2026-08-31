#pragma once

namespace deepsvc
{

// 时间轴视口相机状态
struct TimelineViewportCamera
{
    static constexpr double kDefaultPixelsPerSecond = 100.0;

    double visibleStartSeconds = 0.0;  // 可见窗口的绝对起始时间（秒）
    double pixelsPerSecond = kDefaultPixelsPerSecond;

    bool operator== (const TimelineViewportCamera& other) const noexcept
    {
        return visibleStartSeconds == other.visibleStartSeconds
            && pixelsPerSecond == other.pixelsPerSecond;
    }
    bool operator!= (const TimelineViewportCamera& other) const noexcept { return ! (*this == other); }
};

} // namespace deepsvc
