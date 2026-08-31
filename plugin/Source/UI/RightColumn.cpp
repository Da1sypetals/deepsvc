#include "RightColumn.h"

#include "UIColors.h"

namespace deepsvc
{

RightColumn::RightColumn (juce::AudioProcessorValueTreeState& state)
    : parameterPanel (state)
{
    addAndMakeVisible (slotBar);
    parameterViewport.setViewedComponent (&parameterPanel, false);
    parameterViewport.setScrollBarsShown (true, false);
    parameterViewport.setScrollBarThickness (UIColors::scrollBarThickness);
    addAndMakeVisible (parameterViewport);
    addChildComponent (copyPanel);
}

void RightColumn::setShowingSlotContent (bool showing)
{
    if (showingSlotContent == showing)
        return;
    showingSlotContent = showing;
    parameterViewport.setVisible (! showing);
    copyPanel.setVisible (showing);
    resized();
}

void RightColumn::resized()
{
    auto area = getLocalBounds();
    slotBar.setBounds (area.removeFromTop (SlotBar::kHeight));
    area.removeFromTop (8);
    if (showingSlotContent)
    {
        copyPanel.setBounds (area);
        return;
    }

    parameterViewport.setBounds (area);

    // 高度不足时面板保持完整高度，由视口滚动；垂直滚动条出现时相应缩窄面板
    const bool needsScroll = ParameterPanel::preferredHeight() > area.getHeight();
    const int contentWidth = area.getWidth() - (needsScroll ? UIColors::scrollBarThickness : 0);
    const int contentHeight = juce::jmax (area.getHeight(), ParameterPanel::preferredHeight());
    parameterPanel.setSize (contentWidth, contentHeight);
}

} // namespace deepsvc
