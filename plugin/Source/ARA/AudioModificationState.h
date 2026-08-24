#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

#include "../Content/ContentKey.h"
#include "../Content/ContentStore.h"

// 对应 OpenTune Source/ARA/AudioModification.h 的精简版
namespace deepsvc
{

struct AudioModificationState
{
    juce::ARAAudioModification* audioModification { nullptr };
    juce::String persistentId;
    ContentKey contentIdentity;
    std::optional<ModificationContent> content;

    void updateIdentity (juce::ARAAudioModification* modification)
    {
        audioModification = modification;
        if (modification != nullptr)
            persistentId = juce::String (modification->getPersistentID());
    }

    bool hasContentState() const noexcept { return content.has_value(); }
    ContentKey contentKey() const noexcept { return contentIdentity; }
};

} // namespace deepsvc
