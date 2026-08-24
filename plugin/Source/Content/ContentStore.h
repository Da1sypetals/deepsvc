#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>
#include <vector>

#include "../Utils/SourceWindow.h"

// 对应 OpenTune Source/Content/AudioModificationContentState.h 的精简版：
// 一个 AudioModification 的全部插件侧内容
namespace deepsvc
{

struct ModificationContent
{
    SourceWindow sourceWindow;
    uint64_t contentRevision { 0 };

    // 源音频：44.1kHz 单声道，覆盖整个 sourceWindow，首次使用时提取并缓存
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;

    // 音高检测结果（content 本地时间，秒；Hz，0 表示无声）
    std::vector<float> f0Times;
    std::vector<float> f0Values;

    // 合成结果：44.1kHz 单声道，覆盖整个 sourceWindow
    std::shared_ptr<const std::vector<float>> renderedAudio;
    juce::String renderedFingerprint;  // 合成参数指纹，用于判断结果是否与当前参数一致

    // 音色库相对路径（音色库根目录下的文件名）
    juce::String timbreFile;
};

} // namespace deepsvc
