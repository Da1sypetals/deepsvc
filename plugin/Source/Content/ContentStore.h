#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "../Engine/EngineBridge.h"
#include "../Utils/SourceWindow.h"

// 对应 OpenTune Source/Content/AudioModificationContentState.h 的精简版：
// 一个 AudioModification 的全部插件侧内容，按修改内部时间划分为分段
namespace deepsvc
{

// A/B 槽位：每个槽位是完全独立的一套状态，槽位之间零共享。
// OpenTune 无对应物，设计见 docs/ara.md 第 4.1 节
struct SlotContent
{
    // 6 个合成参数的当前编辑值（激活槽位与 APVTS 同步）
    EngineSynthParams params;

    // 上次合成时的参数与音色快照：回放指示与失效标记用；renderedAudio 非空时有效
    EngineSynthParams synthParams;
    juce::String synthTimbreFile;

    // 音色库相对路径（音色库根目录下的文件名）
    juce::String timbreFile;

    // 音高检测结果（分段本地时间，秒；Hz，0 表示无声）
    std::vector<float> f0Times;
    std::vector<float> f0Values;

    // 合成结果：44.1kHz 单声道，覆盖所属分段的区间
    std::shared_ptr<const std::vector<float>> renderedAudio;

    // 该槽位是否直通原声
    bool bypass = false;

    bool hasRenderedAudio() const noexcept
    {
        return renderedAudio != nullptr && ! renderedAudio->empty();
    }
};

// 分段区间在修改内部时间上的端点，单位秒。宿主每次编辑事务结束时按当前所有
// playback region 的窗口端点重新划分（docs/ara.md 第 4.1 节）
struct SegmentRange
{
    double startSeconds { 0.0 };
    double durationSeconds { 0.0 };

    double endSeconds() const noexcept { return startSeconds + durationSeconds; }
    bool isValid() const noexcept { return durationSeconds > 0.0; }

    bool contains (double timeSeconds) const noexcept
    {
        return timeSeconds >= startSeconds && timeSeconds < endSeconds();
    }

    // 与另一区间的重叠长度，无重叠时为 0
    double overlapWith (const SegmentRange& other) const noexcept
    {
        const double start = std::max (startSeconds, other.startSeconds);
        const double end = std::min (endSeconds(), other.endSeconds());
        return end > start ? end - start : 0.0;
    }

    bool matches (const SegmentRange& other, double tolerance = 1.0e-6) const noexcept
    {
        return std::abs (startSeconds - other.startSeconds) <= tolerance
            && std::abs (durationSeconds - other.durationSeconds) <= tolerance;
    }
};

// 一个分段：区间 + 完整一套状态。切分时新分段从重叠最大的旧分段深拷贝继承
struct ContentSegment
{
    SegmentRange range;

    int activeSlot = 0;
    std::array<SlotContent, 2> slots;

    SlotContent& active() noexcept { return slots[static_cast<size_t> (activeSlot)]; }
    const SlotContent& active() const noexcept { return slots[static_cast<size_t> (activeSlot)]; }
};

struct ModificationContent
{
    SourceWindow sourceWindow;
    uint64_t contentRevision { 0 };

    // 源音频：44.1kHz 单声道，覆盖整个 sourceWindow。由音频文件唯一决定，
    // 所有分段共享同一份不可变缓存
    std::shared_ptr<const juce::AudioBuffer<float>> sourceAudio;

    // 分段列表，按 range.startSeconds 升序且互不重叠
    std::vector<ContentSegment> segments;

    // 覆盖修改内部时间 timeSeconds 的分段；无覆盖时返回 nullptr
    ContentSegment* segmentAt (double timeSeconds) noexcept
    {
        for (auto& segment : segments)
            if (segment.range.contains (timeSeconds))
                return &segment;
        return nullptr;
    }

    const ContentSegment* segmentAt (double timeSeconds) const noexcept
    {
        for (const auto& segment : segments)
            if (segment.range.contains (timeSeconds))
                return &segment;
        return nullptr;
    }

    // 区间与 range 一致的分段；恢复归档与片段状态定位用
    ContentSegment* segmentMatching (const SegmentRange& range) noexcept
    {
        for (auto& segment : segments)
            if (segment.range.matches (range))
                return &segment;
        return nullptr;
    }

    const ContentSegment* segmentMatching (const SegmentRange& range) const noexcept
    {
        for (const auto& segment : segments)
            if (segment.range.matches (range))
                return &segment;
        return nullptr;
    }

    // 与 range 重叠最大的分段：片段窗口被宿主修剪后仍能定位到承载它的分段
    ContentSegment* segmentOverlapping (const SegmentRange& range) noexcept
    {
        ContentSegment* best = nullptr;
        double bestOverlap = 0.0;
        for (auto& segment : segments)
        {
            const double overlap = segment.range.overlapWith (range);
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = &segment;
            }
        }
        return best;
    }

    const ContentSegment* segmentOverlapping (const SegmentRange& range) const noexcept
    {
        const ContentSegment* best = nullptr;
        double bestOverlap = 0.0;
        for (const auto& segment : segments)
        {
            const double overlap = segment.range.overlapWith (range);
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                best = &segment;
            }
        }
        return best;
    }
};

} // namespace deepsvc
