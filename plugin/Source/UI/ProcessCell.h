#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace deepsvc
{

class ProcessCell : public juce::Component
                  , public juce::SettableTooltipClient
{
public:
    ProcessCell() = default;

    void setDisplay (const juce::String& text,
                     juce::Colour textColour,
                     double fraction,
                     bool showFill);

    void paint (juce::Graphics& g) override;

    static constexpr int kWidth = 280;
    static constexpr int kHeight = 28;

private:
    juce::String displayText;
    juce::Colour displayColour { 0xFF7A5C68 };
    double displayFraction = 0.0;
    bool fillVisible = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProcessCell)
};

} // namespace deepsvc
