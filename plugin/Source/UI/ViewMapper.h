#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

#include "TimelineViewportCamera.h"

// 对应 OpenTune Source/Standalone/UI/ViewMapper.h
// （OpenTune 的调律与网格风格偏好在我们的插件中固定为 440 Hz 与琴键车道式）
namespace deepsvc
{

struct ViewMapper
{
    double visibleStartSeconds { 0.0 };
    double pixelsPerSecond { TimelineViewportCamera::kDefaultPixelsPerSecond };
    int contentStartX { 0 };   // 内容区在组件内的起始 X（琴键列宽）
    int contentWidth { 0 };
    int contentHeight { 0 };
    float pixelsPerSemitone { 1.0f };
    float verticalScrollOffset { 0.0f };
    float maxMidi { 127.0f };

    int timeToX (double absoluteSeconds) const
    {
        return contentStartX + static_cast<int> (
            std::llround ((absoluteSeconds - visibleStartSeconds) * pixelsPerSecond));
    }

    double xToTime (int x) const
    {
        return visibleStartSeconds + static_cast<double> (x - contentStartX) / pixelsPerSecond;
    }

    float midiToY (float midi) const
    {
        return (maxMidi - midi) * pixelsPerSemitone - verticalScrollOffset;
    }

    float yToMidi (float y) const
    {
        return maxMidi - (y + verticalScrollOffset) / pixelsPerSemitone;
    }

    // 琴键车道式：音高曲线落在车道中央，频率换算偏移半个半音
    float freqToMidi (float hz) const
    {
        return static_cast<float> (12.0 * std::log2 (hz / 440.0) + 69.0) - 0.5f;
    }

    float midiToFreq (float midi) const
    {
        return 440.0f * std::pow (2.0f, (midi + 0.5f - 69.0f) / 12.0f);
    }

    float freqToY (float hz) const
    {
        return midiToY (freqToMidi (hz));
    }

    float yToFreq (float y) const
    {
        return midiToFreq (yToMidi (y));
    }
};

} // namespace deepsvc
