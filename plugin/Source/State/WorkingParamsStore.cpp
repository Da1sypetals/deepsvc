#include "WorkingParamsStore.h"

#include "Parameters.h"

namespace deepsvc
{

WorkingParamsStore& WorkingParamsStore::getInstance()
{
    static WorkingParamsStore instance;
    return instance;
}

WorkingParamsStore::WorkingParamsStore()
{
    EngineSynthParams loaded;
    juce::String timbre;
    if (parameters::loadWorkingState (loaded, timbre))
    {
        params_ = loaded;
        timbreFile_ = timbre;
    }
}

void WorkingParamsStore::setParams (const EngineSynthParams& params)
{
    if (params_ == params)
        return;
    params_ = params;
    persist();
    sendSynchronousChangeMessage();
}

void WorkingParamsStore::setTimbreFile (const juce::String& fileName)
{
    if (timbreFile_ == fileName)
        return;
    timbreFile_ = fileName;
    persist();
    sendSynchronousChangeMessage();
}

void WorkingParamsStore::persist() const
{
    parameters::saveWorkingState (params_, timbreFile_);
}

} // namespace deepsvc
