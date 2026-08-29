#include "RightColumn.h"

namespace deepsvc
{

RightColumn::RightColumn (juce::AudioProcessorValueTreeState& state)
    : parameterPanel (state)
{
    addAndMakeVisible (slotBar);
    addAndMakeVisible (parameterPanel);
    addChildComponent (copyPanel);
}

void RightColumn::setShowingSlotContent (bool showing)
{
    if (showingSlotContent == showing)
        return;
    showingSlotContent = showing;
    parameterPanel.setVisible (! showing);
    copyPanel.setVisible (showing);
    resized();
}

void RightColumn::resized()
{
    auto area = getLocalBounds();
    slotBar.setBounds (area.removeFromTop (SlotBar::kHeight));
    area.removeFromTop (8);
    if (showingSlotContent)
        copyPanel.setBounds (area);
    else
        parameterPanel.setBounds (area);
}

} // namespace deepsvc
