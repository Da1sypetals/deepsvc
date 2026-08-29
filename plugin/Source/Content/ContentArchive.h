#pragma once

#include <optional>

#include <juce_core/juce_core.h>

#include "../ARA/DeepSvcAudioModification.h"
#include "../State/Parameters.h"

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

inline juce::var slotToJson (const Slot& slot)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("params", parameters::synthParamsToJson (slot.params));
    object->setProperty ("timbreFile", slot.timbreFile);
    object->setProperty ("bypass", slot.bypass);
    if (slot.pitchData.has_value())
    {
        auto* pitch = new juce::DynamicObject();
        pitch->setProperty ("f0Times", floatVectorToJson (slot.pitchData->f0Times));
        pitch->setProperty ("f0Values", floatVectorToJson (slot.pitchData->f0Values));
        object->setProperty ("pitchData", juce::var (pitch));
    }
    if (slot.hasSynthAudio())
    {
        auto* synth = new juce::DynamicObject();
        synth->setProperty ("samples", floatVectorToJson (*slot.synthAudio->samples));
        synth->setProperty ("synthStartTime", slot.synthAudio->synthStartTime);
        synth->setProperty ("synthEndTime", slot.synthAudio->synthEndTime);
        object->setProperty ("synthAudio", juce::var (synth));
        object->setProperty ("synthParams", parameters::synthParamsToJson (slot.synthParams));
        object->setProperty ("synthTimbreFile", slot.synthTimbreFile);
        if (slot.lastSynthElapsedSeconds.has_value())
            object->setProperty ("lastSynthElapsedSeconds", *slot.lastSynthElapsedSeconds);
    }
    return juce::var (object);
}

inline void slotFromJson (Slot& slot, const juce::var& json)
{
    slot.params = parameters::synthParamsFromJson (json.getProperty ("params", juce::var()));
    slot.timbreFile = json.getProperty ("timbreFile", juce::String()).toString();
    slot.bypass = static_cast<bool> (json.getProperty ("bypass", false));

    const auto pitchJson = json.getProperty ("pitchData", juce::var());
    if (pitchJson.isObject())
    {
        PitchData pitch;
        pitch.f0Times = floatVectorFromJson (pitchJson.getProperty ("f0Times", juce::var()));
        pitch.f0Values = floatVectorFromJson (pitchJson.getProperty ("f0Values", juce::var()));
        if (! pitch.f0Times.empty() && pitch.f0Times.size() == pitch.f0Values.size())
            slot.pitchData = std::move (pitch);
    }

    const auto synthJson = json.getProperty ("synthAudio", juce::var());
    if (synthJson.isObject())
    {
        auto samples = floatVectorFromJson (synthJson.getProperty ("samples", juce::var()));
        if (! samples.empty())
        {
            SynthAudio synth;
            synth.samples = std::make_shared<const std::vector<float>> (std::move (samples));
            synth.synthStartTime = static_cast<double> (synthJson.getProperty ("synthStartTime", 0.0));
            synth.synthEndTime = static_cast<double> (synthJson.getProperty ("synthEndTime", 0.0));
            if (synth.isValid())
            {
                slot.synthAudio = std::move (synth);
                slot.synthParams = parameters::synthParamsFromJson (json.getProperty ("synthParams", juce::var()));
                slot.synthTimbreFile = json.getProperty ("synthTimbreFile", juce::String()).toString();
                const auto elapsedVar = json.getProperty ("lastSynthElapsedSeconds", juce::var());
                if (! elapsedVar.isVoid())
                    slot.lastSynthElapsedSeconds = static_cast<double> (elapsedVar);
            }
        }
    }
}

inline juce::var modificationToJson (const DeepSvcAudioModification& modification)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("dataRevision", static_cast<juce::int64> (modification.dataRevision));
    root->setProperty ("activeSlot", modification.activeSlot);
    juce::Array<juce::var> slotsJson;
    for (const auto& slot : modification.slots)
        slotsJson.add (slotToJson (slot));
    root->setProperty ("slots", juce::var (slotsJson));
    return juce::var (root);
}

inline void modificationFromJson (DeepSvcAudioModification& modification, const juce::var& json)
{
    if (! json.isObject())
        return;

    modification.activeSlot = juce::jlimit (0, 1, static_cast<int> (json.getProperty ("activeSlot", 0)));
    if (const auto* slotsJson = json.getProperty ("slots", juce::var()).getArray())
        for (int s = 0; s < juce::jmin (2, slotsJson->size()); ++s)
            slotFromJson (modification.slots[static_cast<size_t> (s)], (*slotsJson)[s]);
    modification.dataRevision = static_cast<uint64_t> (
        static_cast<juce::int64> (json.getProperty ("dataRevision", 0)));
    ++modification.dataRevision;
}

} // namespace deepsvc::archive
