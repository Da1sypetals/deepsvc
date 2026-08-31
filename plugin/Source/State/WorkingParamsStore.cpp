#include "WorkingParamsStore.h"

namespace deepsvc
{

WorkingParamsStore& WorkingParamsStore::getInstance()
{
    static WorkingParamsStore instance;
    return instance;
}

void WorkingParamsStore::setParams (const EngineSynthParams& params)
{
    if (params_ == params)
        return;
    params_ = params;
    sendSynchronousChangeMessage();
}

void WorkingParamsStore::setTimbreFile (const juce::String& fileName)
{
    if (timbreFile_ == fileName)
        return;
    timbreFile_ = fileName;
    sendSynchronousChangeMessage();
}

} // namespace deepsvc
