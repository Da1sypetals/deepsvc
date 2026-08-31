#pragma once

#include <cmath>

namespace deepsvc
{

// 内容（modification 本地时间）到时间线（宿主播放时间）的投影
struct ContentTimelineProjection
{
    double timelineStartSeconds { 0.0 };
    double timelineDurationSeconds { 0.0 };
    double contentStartSeconds { 0.0 };
    double contentDurationSeconds { 0.0 };

    bool isValid() const noexcept
    {
        return timelineDurationSeconds > 0.0 && contentDurationSeconds > 0.0;
    }

    bool equals (const ContentTimelineProjection& rhs) const noexcept
    {
        constexpr double kEps = 1.0e-9;
        return std::abs (timelineStartSeconds - rhs.timelineStartSeconds) <= kEps
            && std::abs (timelineDurationSeconds - rhs.timelineDurationSeconds) <= kEps
            && std::abs (contentStartSeconds - rhs.contentStartSeconds) <= kEps
            && std::abs (contentDurationSeconds - rhs.contentDurationSeconds) <= kEps;
    }

    double timelineEndSeconds() const noexcept
    {
        return timelineStartSeconds + timelineDurationSeconds;
    }

    double projectTimelineTimeToContent (double timelineSeconds) const noexcept
    {
        const double normalized = (timelineSeconds - timelineStartSeconds) / timelineDurationSeconds;
        return contentStartSeconds + normalized * contentDurationSeconds;
    }

    double projectContentTimeToTimeline (double contentSeconds) const noexcept
    {
        const double normalized = (contentSeconds - contentStartSeconds) / contentDurationSeconds;
        return timelineStartSeconds + normalized * timelineDurationSeconds;
    }
};

} // namespace deepsvc
