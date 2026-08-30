#pragma once

#include <juce_core/juce_core.h>

namespace deepsvc
{

struct JobStatus
{
    enum class State
    {
        idle,
        queued,
        loadingModels,
        running,
        succeeded,
        failed,
        cancelled
    };

    State state = State::idle;
    juce::String stage;
    double fraction = 0.0;
    uint32_t queuePosition = 0;
    juce::String error;
    double elapsedSeconds = -1.0;
};

} // namespace deepsvc
