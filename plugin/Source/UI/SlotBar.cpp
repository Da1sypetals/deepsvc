#include "SlotBar.h"

#include "UIColors.h"

namespace deepsvc
{

SlotBar::SlotBar()
{
    slotAButton.setClickingTogglesState (true);
    slotBButton.setClickingTogglesState (true);
    slotAButton.setRadioGroupId (0x5A01);
    slotBButton.setRadioGroupId (0x5A01);
    slotAButton.onClick = [this]
    {
        if (! syncing && onSlotChanged)
            onSlotChanged (0);
    };
    slotBButton.onClick = [this]
    {
        if (! syncing && onSlotChanged)
            onSlotChanged (1);
    };
    addAndMakeVisible (slotAButton);
    addAndMakeVisible (slotBButton);

    bypassButton.setClickingTogglesState (true);
    bypassButton.onClick = [this]
    {
        if (! syncing && onBypassChanged)
            onBypassChanged (bypassButton.getToggleState());
    };
    addAndMakeVisible (bypassButton);

    clearButton.onClick = [this]
    {
        if (onClearRequested)
            onClearRequested();
    };
    addAndMakeVisible (clearButton);

    syncControlsFromState();
}

void SlotBar::setState (const State& newState)
{
    state = newState;
    syncControlsFromState();
}

void SlotBar::syncControlsFromState()
{
    syncing = true;
    const bool hasModification = state.hasModification;
    slotAButton.setEnabled (hasModification);
    slotBButton.setEnabled (hasModification);
    slotAButton.setToggleState (state.activeSlot == 0, juce::dontSendNotification);
    slotBButton.setToggleState (state.activeSlot == 1, juce::dontSendNotification);
    slotAButton.getProperties().set ("primary", hasModification && state.activeSlot == 0);
    slotBButton.getProperties().set ("primary", hasModification && state.activeSlot == 1);
    bypassButton.setEnabled (hasModification && state.hasSynth && ! state.jobActive);
    bypassButton.setToggleState (state.bypass, juce::dontSendNotification);
    bypassButton.getProperties().set ("primary", hasModification && state.hasSynth && state.bypass);
    clearButton.setEnabled (hasModification && state.hasSynth && ! state.jobActive);
    syncing = false;
    slotAButton.repaint();
    slotBButton.repaint();
    bypassButton.repaint();
}

void SlotBar::paint (juce::Graphics& g)
{
    UIColors::fillPanelBackground (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);
    UIColors::drawPanelFrame (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);
}

void SlotBar::resized()
{
    auto area = getLocalBounds().reduced (10, 8);
    slotAButton.setBounds (area.removeFromLeft (28));
    slotBButton.setBounds (area.removeFromLeft (28));
    clearButton.setBounds (area.removeFromRight (52));
    area.removeFromRight (6);
    bypassButton.setBounds (area.removeFromRight (52));
}

} // namespace deepsvc
