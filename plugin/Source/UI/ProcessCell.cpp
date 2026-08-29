#include "ProcessCell.h"

#include <cmath>

#include "UIColors.h"

namespace deepsvc
{

void ProcessCell::setDisplay (const juce::String& text,
                              juce::Colour textColour,
                              double fraction,
                              bool showFill)
{
    if (displayText == text && displayColour == textColour
        && std::abs (displayFraction - fraction) < 1.0e-6
        && fillVisible == showFill)
        return;

    displayText = text;
    displayColour = textColour;
    displayFraction = fraction;
    fillVisible = showFill;
    setTooltip (text);
    repaint();
}

void ProcessCell::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = UIColors::controlCornerRadius;

    g.setColour (UIColors::pink100);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (UIColors::pink200);
    g.drawRoundedRectangle (bounds, radius, 1.0f);

    if (fillVisible && displayFraction > 0.0)
    {
        auto filled = bounds.withWidth (bounds.getWidth()
            * static_cast<float> (juce::jlimit (0.0, 1.0, displayFraction)));
        g.setColour (UIColors::pink300);
        g.fillRoundedRectangle (filled, radius);
        g.setColour (UIColors::pink200);
        g.drawRoundedRectangle (bounds, radius, 1.0f);
    }

    g.setColour (displayColour);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText (displayText, getLocalBounds().reduced (8, 0),
                juce::Justification::centred, true);
}

} // namespace deepsvc
