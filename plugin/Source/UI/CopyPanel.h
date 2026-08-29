#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <optional>

#include "../Engine/EngineBridge.h"

namespace deepsvc
{

class CopyPanel : public juce::Component
{
public:
    struct State
    {
        bool hasModification = false;
        bool hasPitch = false;
        bool hasSynth = false;
        bool bypass = false;
        bool stale = false;
        juce::String synthTimbreFile;
        EngineSynthParams synthParams;
        double synthStartTime = 0.0;
        double synthEndTime = 0.0;
        std::optional<double> lastSynthElapsedSeconds;
    };

    CopyPanel() = default;

    void setState (const State& newState);

    void paint (juce::Graphics& g) override;

    static constexpr int kWidth = 240;

private:
    State state;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CopyPanel)
};

} // namespace deepsvc
