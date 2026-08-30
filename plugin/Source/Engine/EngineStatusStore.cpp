#include "EngineStatusStore.h"

namespace deepsvc
{

namespace
{

JobStatus::State toJobStatusState (EngineJobState state)
{
    switch (state)
    {
        case EngineJobState::queued:        return JobStatus::State::queued;
        case EngineJobState::loadingModels: return JobStatus::State::loadingModels;
        case EngineJobState::running:       return JobStatus::State::running;
        case EngineJobState::succeeded:     return JobStatus::State::succeeded;
        case EngineJobState::failed:        return JobStatus::State::failed;
        case EngineJobState::cancelled:     return JobStatus::State::cancelled;
    }
    return JobStatus::State::idle;
}

bool isTerminal (JobStatus::State state)
{
    return state == JobStatus::State::succeeded
        || state == JobStatus::State::failed
        || state == JobStatus::State::cancelled;
}

constexpr double kLingerMs = 3000.0;

} // namespace

EngineStatusStore& EngineStatusStore::getInstance()
{
    static EngineStatusStore instance;
    return instance;
}

void EngineStatusStore::applyJobState (EngineJobState engineState,
                                       uint32_t queuePosition,
                                       const juce::String& stage,
                                       double fraction,
                                       const juce::String& error)
{
    const juce::ScopedLock scoped (lock);
    status.state = toJobStatusState (engineState);
    status.stage = stage;
    status.fraction = fraction;
    status.queuePosition = queuePosition;
    status.error = error;
    if (isTerminal (status.state))
        lingerUntilMs = juce::Time::getMillisecondCounterHiRes() + kLingerMs;
    else
        lingerUntilMs = 0.0;
}

JobStatus EngineStatusStore::displayStatus() const
{
    const juce::ScopedLock scoped (lock);
    if (isTerminal (status.state)
        && juce::Time::getMillisecondCounterHiRes() >= lingerUntilMs)
        return {};
    return status;
}

} // namespace deepsvc
