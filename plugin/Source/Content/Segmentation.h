#pragma once

#include <algorithm>
#include <vector>

#include "ContentStore.h"

// 分段划分与继承：按片段窗口端点切分修改内部时间，新分段从旧布局深拷贝继承。
// 对应 OpenTune Source/PluginProcessor.cpp 的 copyContentRange（按区间派生独立内容：
// 音频按样本区间切片、音高曲线平移到本地区间、其余设置整体拷贝）
namespace deepsvc::segmentation
{

// 分段边界合并容差：宿主给出的窗口端点存在浮点误差
constexpr double kBoundaryTolerance = 1.0e-4;

// 把片段窗口集合切分为互不重叠的分段区间：取所有端点的并集作为边界，
// 相邻边界之间被至少一个窗口覆盖的区间成为一个分段
inline std::vector<SegmentRange> computeRanges (const std::vector<SegmentRange>& windows)
{
    std::vector<double> boundaries;
    boundaries.reserve (windows.size() * 2);
    for (const auto& window : windows)
    {
        if (! window.isValid())
            continue;
        boundaries.push_back (window.startSeconds);
        boundaries.push_back (window.endSeconds());
    }
    std::sort (boundaries.begin(), boundaries.end());

    std::vector<double> merged;
    for (const double boundary : boundaries)
        if (merged.empty() || boundary - merged.back() > kBoundaryTolerance)
            merged.push_back (boundary);

    std::vector<SegmentRange> ranges;
    for (size_t i = 0; i + 1 < merged.size(); ++i)
    {
        const SegmentRange candidate { merged[i], merged[i + 1] - merged[i] };
        if (! candidate.isValid())
            continue;
        const double midpoint = candidate.startSeconds + candidate.durationSeconds * 0.5;
        const bool covered = std::any_of (windows.begin(), windows.end(),
                                          [midpoint] (const SegmentRange& window)
                                          { return window.isValid() && window.contains (midpoint); });
        if (covered)
            ranges.push_back (candidate);
    }
    return ranges;
}

// 分段布局是否与目标区间一致
inline bool layoutMatches (const std::vector<ContentSegment>& segments,
                           const std::vector<SegmentRange>& ranges)
{
    if (segments.size() != ranges.size())
        return false;
    for (size_t i = 0; i < ranges.size(); ++i)
        if (! segments[i].range.matches (ranges[i], kBoundaryTolerance))
            return false;
    return true;
}

// 槽位的时间数据继承：把按分段本地时间存放的音高与合成音频平移 offset 秒并裁剪到新区间。
// offset 是新区间起点相对供体区间起点的偏移
inline void inheritSlotTimeData (SlotContent& slot,
                                 double offset,
                                 double durationSeconds,
                                 double renderSampleRate)
{
    std::vector<float> times;
    std::vector<float> values;
    const auto frameCount = std::min (slot.f0Times.size(), slot.f0Values.size());
    times.reserve (frameCount);
    values.reserve (frameCount);
    for (size_t i = 0; i < frameCount; ++i)
    {
        const double local = static_cast<double> (slot.f0Times[i]) - offset;
        if (local < 0.0 || local >= durationSeconds)
            continue;
        times.push_back (static_cast<float> (local));
        values.push_back (slot.f0Values[i]);
    }
    slot.f0Times = std::move (times);
    slot.f0Values = std::move (values);

    if (slot.renderedAudio == nullptr || slot.renderedAudio->empty())
        return;

    const auto& source = *slot.renderedAudio;
    const auto total = static_cast<int64_t> (source.size());
    const auto clamp = [total] (int64_t value)
    {
        return std::min (total, std::max<int64_t> (0, value));
    };
    const auto begin = clamp (static_cast<int64_t> (offset * renderSampleRate));
    const auto end = std::max (begin,
                               clamp (static_cast<int64_t> ((offset + durationSeconds) * renderSampleRate)));
    if (end > begin)
        slot.renderedAudio = std::make_shared<const std::vector<float>> (
            source.begin() + static_cast<ptrdiff_t> (begin),
            source.begin() + static_cast<ptrdiff_t> (end));
    else
        slot.renderedAudio.reset();
}

// 按目标区间重建分段列表：每个区间从旧布局中重叠最大的分段深拷贝继承状态。
// 切分时两个新区间都落在原分段之内，各得原状态的一份完整副本，之后互不相干
inline std::vector<ContentSegment> rebuild (const std::vector<ContentSegment>& previous,
                                            const std::vector<SegmentRange>& ranges,
                                            double renderSampleRate)
{
    std::vector<ContentSegment> next;
    next.reserve (ranges.size());

    for (const auto& range : ranges)
    {
        const ContentSegment* donor = nullptr;
        double bestOverlap = 0.0;
        for (const auto& candidate : previous)
        {
            const double overlap = candidate.range.overlapWith (range);
            if (overlap > bestOverlap)
            {
                bestOverlap = overlap;
                donor = &candidate;
            }
        }

        ContentSegment segment;
        if (donor != nullptr)
        {
            segment = *donor;
            segment.range = range;
            const double offset = range.startSeconds - donor->range.startSeconds;
            for (auto& slot : segment.slots)
                inheritSlotTimeData (slot, offset, range.durationSeconds, renderSampleRate);
        }
        else
        {
            segment.range = range;
        }
        next.push_back (std::move (segment));
    }
    return next;
}

} // namespace deepsvc::segmentation
