#pragma once

#include <optional>
#include <vector>

#include <juce_core/juce_core.h>

#include "ContentStore.h"
#include "../State/Parameters.h"

// 分段状态的 JSON 编解码（docs/ara.md 第 4.2 节）。
// 与 OpenTune 的差异：OpenTune 记录 XML、渲染结果不入库、恢复后由渲染服务重建；
// 本插件的推理结果（音高、合成音频）直接入库
namespace deepsvc::archive
{

inline juce::var floatVectorToJson (const std::vector<float>& values)
{
    juce::Array<juce::var> array;
    array.ensureStorageAllocated (static_cast<int> (values.size()));
    for (const float value : values)
        array.add (static_cast<double> (value));
    return juce::var (array);
}

inline std::vector<float> floatVectorFromJson (const juce::var& json)
{
    std::vector<float> values;
    if (const auto* array = json.getArray())
    {
        values.reserve (static_cast<size_t> (array->size()));
        for (const auto& element : *array)
            values.push_back (static_cast<float> (static_cast<double> (element)));
    }
    return values;
}

inline juce::var slotToJson (const SlotContent& slot)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("params", parameters::synthParamsToJson (slot.params));
    object->setProperty ("timbreFile", slot.timbreFile);
    object->setProperty ("bypass", slot.bypass);
    object->setProperty ("f0Times", floatVectorToJson (slot.f0Times));
    object->setProperty ("f0Values", floatVectorToJson (slot.f0Values));
    if (slot.hasRenderedAudio())
    {
        object->setProperty ("renderedAudio", floatVectorToJson (*slot.renderedAudio));
        object->setProperty ("synthParams", parameters::synthParamsToJson (slot.synthParams));
        object->setProperty ("synthTimbreFile", slot.synthTimbreFile);
    }
    return juce::var (object);
}

inline void slotFromJson (SlotContent& slot, const juce::var& json)
{
    slot.params = parameters::synthParamsFromJson (json.getProperty ("params", juce::var()));
    slot.timbreFile = json.getProperty ("timbreFile", juce::String()).toString();
    slot.bypass = static_cast<bool> (json.getProperty ("bypass", false));
    slot.f0Times = floatVectorFromJson (json.getProperty ("f0Times", juce::var()));
    slot.f0Values = floatVectorFromJson (json.getProperty ("f0Values", juce::var()));

    auto rendered = floatVectorFromJson (json.getProperty ("renderedAudio", juce::var()));
    if (! rendered.empty())
    {
        slot.renderedAudio = std::make_shared<const std::vector<float>> (std::move (rendered));
        slot.synthParams = parameters::synthParamsFromJson (json.getProperty ("synthParams", juce::var()));
        slot.synthTimbreFile = json.getProperty ("synthTimbreFile", juce::String()).toString();
    }
}

inline juce::var segmentToJson (const ContentSegment& segment)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("start", segment.range.startSeconds);
    object->setProperty ("duration", segment.range.durationSeconds);
    object->setProperty ("activeSlot", segment.activeSlot);
    juce::Array<juce::var> slotsJson;
    for (const auto& slot : segment.slots)
        slotsJson.add (slotToJson (slot));
    object->setProperty ("slots", juce::var (slotsJson));
    return juce::var (object);
}

inline std::optional<ContentSegment> segmentFromJson (const juce::var& json)
{
    if (! json.isObject())
        return std::nullopt;

    ContentSegment segment;
    segment.range.startSeconds = static_cast<double> (json.getProperty ("start", -1.0));
    segment.range.durationSeconds = static_cast<double> (json.getProperty ("duration", 0.0));
    if (segment.range.startSeconds < 0.0 || ! segment.range.isValid())
        return std::nullopt;

    segment.activeSlot = juce::jlimit (0, 1, static_cast<int> (json.getProperty ("activeSlot", 0)));
    if (const auto* slotsJson = json.getProperty ("slots", juce::var()).getArray())
        for (int s = 0; s < juce::jmin (2, slotsJson->size()); ++s)
            slotFromJson (segment.slots[static_cast<size_t> (s)], (*slotsJson)[s]);
    return segment;
}

// 分段数组的编解码：恢复后按起点升序，与写入顺序一致
inline juce::var segmentsToJson (const std::vector<ContentSegment>& segments)
{
    juce::Array<juce::var> array;
    for (const auto& segment : segments)
        array.add (segmentToJson (segment));
    return juce::var (array);
}

inline std::vector<ContentSegment> segmentsFromJson (const juce::var& json)
{
    std::vector<ContentSegment> segments;
    if (const auto* array = json.getArray())
        for (const auto& element : *array)
            if (auto segment = segmentFromJson (element))
                segments.push_back (std::move (*segment));

    std::sort (segments.begin(), segments.end(),
               [] (const ContentSegment& lhs, const ContentSegment& rhs)
               { return lhs.range.startSeconds < rhs.range.startSeconds; });
    return segments;
}

} // namespace deepsvc::archive
