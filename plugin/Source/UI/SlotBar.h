#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace deepsvc
{

class SlotBar : public juce::Component
{
public:
    struct State
    {
        bool hasModification = false;
        int activeSlot = 0;
        bool hasSynth = false;
        bool bypass = false;
        bool jobActive = false;
    };

    SlotBar();

    void setState (const State& newState);

    std::function<void (int slot)> onSlotChanged;
    std::function<void (bool bypass)> onBypassChanged;
    std::function<void()> onClearRequested;

    void paint (juce::Graphics& g) override;
    void resized() override;

    static constexpr int kWidth = 240;
    static constexpr int kHeight = 40;

private:
    void syncControlsFromState();

    State state;
    juce::TextButton slotAButton { juce::String (u8"A") };
    juce::TextButton slotBButton { juce::String (u8"B") };
    juce::TextButton bypassButton { juce::String (u8"旁通") };
    juce::TextButton clearButton { juce::String (u8"清空") };
    bool syncing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotBar)
};

} // namespace deepsvc
