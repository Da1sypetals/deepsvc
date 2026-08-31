#include "TimelineViewportPolicy.h"

#include <algorithm>
#include <cmath>

namespace deepsvc
{

double TimelineViewportPolicy::normalisePixelsPerSecond (double pps) noexcept
{
    return std::clamp (pps, kMinPixelsPerSecond, kMaxPixelsPerSecond);
}

double TimelineViewportPolicy::clampStartSeconds (double startSeconds)
{
    return std::max (0.0, startSeconds);
}

double TimelineViewportPolicy::visibleEndSeconds (const TimelineViewportCamera& camera, int viewportWidth)
{
    if (viewportWidth <= 0 || camera.pixelsPerSecond <= 0.0)
        return camera.visibleStartSeconds;
    return camera.visibleStartSeconds + viewportWidth / camera.pixelsPerSecond;
}

TimelineViewportCamera TimelineViewportPolicy::resolve (const TimelineViewportRequest& request)
{
    const double pps = normalisePixelsPerSecond (request.pixelsPerSecond);
    const int vw = request.viewportWidth;

    TimelineViewportCamera camera;
    camera.pixelsPerSecond = pps;

    switch (request.kind)
    {
        case TimelineViewportRequest::Kind::Manual:
            camera.visibleStartSeconds = clampStartSeconds (request.targetTime);
            break;

        case TimelineViewportRequest::Kind::Cont:
        {
            const double visibleDuration = vw / pps;
            camera.visibleStartSeconds = clampStartSeconds (request.targetTime - visibleDuration * 0.5);
            break;
        }

        case TimelineViewportRequest::Kind::Page:
        {
            const double visibleDuration = vw / pps;
            const double currentStart = request.currentVisibleStartSeconds;
            const double currentEnd = currentStart + visibleDuration;
            double pageStart = currentStart;
            // 越界后放在运动方向进入边缘，避免下一帧重触发
            if (request.targetTime < currentStart)
                pageStart = request.targetTime - visibleDuration;
            else if (request.targetTime > currentEnd)
                pageStart = request.targetTime;
            camera.visibleStartSeconds = clampStartSeconds (pageStart);
            break;
        }

        case TimelineViewportRequest::Kind::Click:
        case TimelineViewportRequest::Kind::Zoom:
        {
            const double anchorSeconds = (vw > 0) ? request.anchorViewportX / pps : 0.0;
            camera.visibleStartSeconds = clampStartSeconds (request.targetTime - anchorSeconds);
            break;
        }
    }

    return camera;
}

} // namespace deepsvc
