#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <memory>
#include <vector>

#include "../Engine/EngineBridge.h"
#include "../Utils/SourceWindow.h"

// 对应 OpenTune Source/Content/AudioModificationContentState.h 的精简版：
// 一个 AudioModification 的全部插件侧内容
namespace deepsvc
{

// A/B 槽位：每个槽位是完全独立的一套状态，槽位之间零共享。
// OpenTune 无对应物，设计见 docs/ara.md 第 4.1 节
struct SlotContent
{
    // 源音频：44.1kHz 单声道，覆盖整个 sourceWindow，首次使用时提取并缓存
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;

    // 6 个合成参数的当前编辑值（激活槽位与 APVTS 同步）
    EngineSynthParams params;

    // 上次合成时的参数与音色快照：回放指示与失效标记用；renderedAudio 非空时有效
    EngineSynthParams synthParams;
    juce::String synthTimbreFile;

    // 音色库相对路径（音色库根目录下的文件名）
    juce::String timbreFile;

    // 音高检测结果（content 本地时间，秒；Hz，0 表示无声）
    std::vector<float> f0Times;
    std::vector<float> f0Values;

    // 合成结果：44.1kHz 单声道，覆盖整个 sourceWindow
    std::shared_ptr<const std::vector<float>> renderedAudio;

    // 该槽位是否直通原声
    bool bypass = false;

    bool hasRenderedAudio() const noexcept
    {
        return renderedAudio != nullptr && ! renderedAudio->empty();
    }
};

struct ModificationContent
{
    SourceWindow sourceWindow;
    uint64_t contentRevision { 0 };

    int activeSlot = 0;
    std::array<SlotContent, 2> slots;

    SlotContent& active() noexcept { return slots[static_cast<size_t> (activeSlot)]; }
    const SlotContent& active() const noexcept { return slots[static_cast<size_t> (activeSlot)]; }
};

} // namespace deepsvc
