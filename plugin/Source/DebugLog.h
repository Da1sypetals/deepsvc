#pragma once

#include <juce_core/juce_core.h>

#include "Directories.h"

// 诊断日志：追加写入 ~/Library/deepsvc/debug.log（插件上下文里 userApplicationDataDirectory 解析为 ~/Library）
namespace deepsvc
{

inline void debugLog (const juce::String& message)
{
    auto file = directories::applicationSupport().getChildFile ("debug.log");
    file.getParentDirectory().createDirectory();
    file.appendText (juce::Time::getCurrentTime().toString (true, true, true, true)
                     + " " + message + "\n");
}

} // namespace deepsvc
