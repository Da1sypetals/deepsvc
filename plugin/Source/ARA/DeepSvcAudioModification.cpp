#include "DeepSvcAudioModification.h"

#include <algorithm>

namespace deepsvc
{

DeepSvcAudioModification::DeepSvcAudioModification (
    juce::ARAAudioSource* audioSource,
    ARA::ARAAudioModificationHostRef hostRef,
    const juce::ARAAudioModification* optionalModificationToClone)
    : juce::ARAAudioModification (audioSource, hostRef, optionalModificationToClone)
{
    if (const auto* clone = dynamic_cast<const DeepSvcAudioModification*> (optionalModificationToClone))
    {
        slots = clone->slots;
        activeSlot = clone->activeSlot;
        dataRevision = clone->dataRevision;
        sourceAudio = clone->sourceAudio;
    }
    bindContentKey();
}

void DeepSvcAudioModification::bindContentKey()
{
    contentKey.objectId = static_cast<uint64_t> (juce::String (getPersistentID()).hashCode64());
}

WorkingRange DeepSvcAudioModification::workingRange() const
{
    WorkingRange range;
    bool any = false;
    for (auto* region : getPlaybackRegions())
    {
        if (region == nullptr)
            continue;
        const double start = region->getStartInAudioModificationTime();
        const double duration = region->getDurationInAudioModificationTime();
        if (duration <= 0.0)
            continue;
        const double end = start + duration;
        if (! any)
        {
            range.startSeconds = start;
            range.endSeconds = end;
            any = true;
            continue;
        }
        range.startSeconds = std::min (range.startSeconds, start);
        range.endSeconds = std::max (range.endSeconds, end);
    }
    return range;
}

} // namespace deepsvc
