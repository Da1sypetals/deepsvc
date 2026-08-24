#pragma once

#include <juce_core/juce_core.h>

// 对应 OpenTune Source/Utils/SourceWindow.h（我们只有 ARA 域，用 persistentID 标识源）
namespace deepsvc
{

// 源绝对窗口：一个内容从某 AudioSource 的哪一段提炼而来
struct SourceWindow
{
    juce::String sourcePersistentId;
    double sourceStartSeconds { 0.0 };
    double sourceEndSeconds { 0.0 };

    bool isValid() const noexcept
    {
        return sourcePersistentId.isNotEmpty() && sourceEndSeconds > sourceStartSeconds;
    }
    double durationSeconds() const noexcept { return sourceEndSeconds - sourceStartSeconds; }
};

} // namespace deepsvc
