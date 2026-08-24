#pragma once

#include <cmath>
#include <cstdint>

// 对应 OpenTune Source/Utils/TimeCoordinate.h
namespace deepsvc::TimeCoordinate
{

// 所有内部音频存储与渲染统一使用 44.1kHz
constexpr double kRenderSampleRate = 44100.0;

inline double samplesToSeconds (int64_t samples, double sampleRate)
{
    if (sampleRate <= 0.0)
        return 0.0;
    return static_cast<double> (samples) / sampleRate;
}

inline double secondsToSamplesExact (double seconds, double sampleRate)
{
    if (sampleRate <= 0.0)
        return 0.0;
    return seconds * sampleRate;
}

inline int64_t secondsToSamples (double seconds, double sampleRate)
{
    return static_cast<int64_t> (secondsToSamplesExact (seconds, sampleRate));
}

inline int64_t secondsToSamplesFloor (double seconds, double sampleRate)
{
    return static_cast<int64_t> (std::floor (secondsToSamplesExact (seconds, sampleRate)));
}

inline int64_t secondsToSamplesCeil (double seconds, double sampleRate)
{
    return static_cast<int64_t> (std::ceil (secondsToSamplesExact (seconds, sampleRate)));
}

} // namespace deepsvc::TimeCoordinate
