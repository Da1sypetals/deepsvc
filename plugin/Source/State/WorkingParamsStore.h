#pragma once

#include <juce_events/juce_events.h>

#include "../Engine/EngineBridge.h"

namespace deepsvc
{

// 进程内唯一一份工作参数：旋钮与当前音色。界面 APVTS 从这里镜像。
// 持久化通过 ARA archive 完成（doStoreObjectsToStream / doRestoreObjectsFromStream）。
class WorkingParamsStore : public juce::ChangeBroadcaster
{
public:
    static WorkingParamsStore& getInstance();

    EngineSynthParams params() const noexcept { return params_; }
    juce::String timbreFile() const { return timbreFile_; }

    void setParams (const EngineSynthParams& params);
    void setTimbreFile (const juce::String& fileName);

private:
    WorkingParamsStore() = default;

    EngineSynthParams params_;
    juce::String timbreFile_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WorkingParamsStore)
};

} // namespace deepsvc
