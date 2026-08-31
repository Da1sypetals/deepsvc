#pragma once

#include <juce_core/juce_core.h>

// 插件使用的一切文件系统路径
namespace deepsvc::directories
{

inline juce::File applicationSupport()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("deepsvc");
}

inline juce::File timbresDirectory()
{
    return applicationSupport().getChildFile ("Timbres");
}

} // namespace deepsvc::directories
