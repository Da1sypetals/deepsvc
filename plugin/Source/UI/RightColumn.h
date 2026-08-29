#pragma once

#include "CopyPanel.h"
#include "ParameterPanel.h"
#include "SlotBar.h"

namespace deepsvc
{

class RightColumn : public juce::Component
{
public:
    explicit RightColumn (juce::AudioProcessorValueTreeState& state);

    SlotBar slotBar;
    ParameterPanel parameterPanel;
    CopyPanel copyPanel;

    void setShowingSlotContent (bool showing);
    void resized() override;

    static constexpr int kWidth = ParameterPanel::kWidth;

private:
    bool showingSlotContent = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RightColumn)
};

} // namespace deepsvc
