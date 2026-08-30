#pragma once

#include <juce_core/juce_core.h>

#include "EngineBridge.h"
#include "JobStatus.h"

namespace deepsvc
{

// 进程内唯一一份引擎状态：右下角与忙碌按钮都读这里。
class EngineStatusStore
{
public:
    static EngineStatusStore& getInstance();

    void applyJobState (EngineJobState engineState,
                        uint32_t queuePosition,
                        const juce::String& stage,
                        double fraction,
                        const juce::String& error);

    // 结束态超过停留窗口后按空闲返回
    JobStatus displayStatus() const;

private:
    EngineStatusStore() = default;

    mutable juce::CriticalSection lock;
    JobStatus status;
    double lingerUntilMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE (EngineStatusStore)
};

} // namespace deepsvc
