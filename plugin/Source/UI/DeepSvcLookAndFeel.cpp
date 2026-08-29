#include "DeepSvcLookAndFeel.h"

#include "UIColors.h"

// 组件样式按 docs/ara.md 设计语言章节；OpenTune 的 Aurora 主题不适用
namespace deepsvc
{

DeepSvcLookAndFeel::DeepSvcLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, UIColors::pink050);
    setColour (juce::Label::textColourId, UIColors::ink900);
    setColour (juce::ListBox::backgroundColourId, UIColors::pink050);
    setColour (juce::ListBox::outlineColourId, UIColors::pink200);
    setColour (juce::ComboBox::backgroundColourId, UIColors::pink050);
    setColour (juce::ComboBox::textColourId, UIColors::ink900);
    setColour (juce::ComboBox::outlineColourId, UIColors::pink200);
    setColour (juce::ComboBox::arrowColourId, UIColors::ink600);
    setColour (juce::PopupMenu::backgroundColourId, UIColors::pink050);
    setColour (juce::PopupMenu::textColourId, UIColors::ink900);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, UIColors::pink100);
    setColour (juce::PopupMenu::highlightedTextColourId, UIColors::ink900);
    setColour (juce::Slider::backgroundColourId, UIColors::pink200);
    setColour (juce::Slider::trackColourId, UIColors::pink600);
    setColour (juce::Slider::thumbColourId, UIColors::keyWhite);
    setColour (juce::Slider::textBoxTextColourId, UIColors::ink900);
    setColour (juce::Slider::textBoxBackgroundColourId, UIColors::pink050);
    setColour (juce::Slider::textBoxOutlineColourId, UIColors::pink200);
    setColour (juce::TextEditor::backgroundColourId, UIColors::pink050);
    setColour (juce::TextEditor::textColourId, UIColors::ink900);
    setColour (juce::TextEditor::outlineColourId, UIColors::pink200);
    setColour (juce::TextEditor::focusedOutlineColourId, UIColors::pink600);
    setColour (juce::AlertWindow::backgroundColourId, UIColors::pink050);
    setColour (juce::AlertWindow::textColourId, UIColors::ink900);
    setColour (juce::ToggleButton::textColourId, UIColors::ink900);
    setColour (juce::ToggleButton::tickColourId, UIColors::pink600);
    setColour (juce::ToggleButton::tickDisabledColourId, UIColors::ink300);
    setColour (juce::ProgressBar::backgroundColourId, UIColors::pink200);
    setColour (juce::ProgressBar::foregroundColourId, UIColors::pink600);
    setColour (juce::ScrollBar::thumbColourId, UIColors::pink300);
    setColour (juce::TextButton::textColourOffId, UIColors::ink900);
    setColour (juce::TextButton::textColourOnId, UIColors::ink900);
}

void DeepSvcLookAndFeel::drawButtonBackground (juce::Graphics& g,
                                               juce::Button& button,
                                               const juce::Colour&,
                                               bool shouldDrawButtonAsHighlighted,
                                               bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const float radius = UIColors::controlCornerRadius;
    const bool primary = button.getProperties().getWithDefault ("primary", false);

    if (primary)
    {
        auto fill = UIColors::pink600;
        if (! button.isEnabled())
            fill = UIColors::pink200;
        else if (shouldDrawButtonAsDown)
            fill = UIColors::pink700;
        else if (shouldDrawButtonAsHighlighted)
            fill = UIColors::pink500;

        g.setColour (fill);
        g.fillRoundedRectangle (bounds, radius);
        return;
    }

    // 次按钮：描边样式；toggle 按下态用主色浅底表达
    auto* toggle = dynamic_cast<juce::TextButton*> (&button);
    const bool toggledOn = toggle != nullptr && toggle->getClickingTogglesState()
        && toggle->getToggleState();

    auto fill = juce::Colours::transparentBlack;
    if (! button.isEnabled())
        fill = juce::Colours::transparentBlack;
    else if (shouldDrawButtonAsDown || toggledOn)
        fill = UIColors::pink200;
    else if (shouldDrawButtonAsHighlighted)
        fill = UIColors::pink100;

    if (fill != juce::Colours::transparentBlack)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (bounds, radius);
    }

    g.setColour (button.isEnabled() ? UIColors::pink200 : UIColors::pink200.withAlpha (0.6f));
    g.drawRoundedRectangle (bounds, radius, 1.0f);
}

void DeepSvcLookAndFeel::drawButtonText (juce::Graphics& g,
                                         juce::TextButton& button,
                                         bool,
                                         bool)
{
    const bool primary = button.getProperties().getWithDefault ("primary", false);

    auto colour = primary ? juce::Colours::white : UIColors::ink900;
    if (! button.isEnabled())
        colour = UIColors::ink300;

    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.setColour (colour);

    const auto textArea = button.getLocalBounds().reduced (4, 0);
    g.drawFittedText (button.getButtonText(), textArea,
                      juce::Justification::centred, 1);
}

juce::Font DeepSvcLookAndFeel::getTextButtonFont (juce::TextButton&, int)
{
    return juce::Font (juce::FontOptions (13.0f));
}

juce::Font DeepSvcLookAndFeel::getLabelFont (juce::Label&)
{
    return juce::Font (juce::FontOptions (13.0f));
}

juce::Font DeepSvcLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return juce::Font (juce::FontOptions (13.0f));
}

void DeepSvcLookAndFeel::drawLinearSlider (juce::Graphics& g,
                                           int x, int y, int width, int height,
                                           float sliderPos,
                                           float minSliderPos,
                                           float maxSliderPos,
                                           juce::Slider::SliderStyle style,
                                           juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos,
                                          minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto trackY = static_cast<float> (y) + static_cast<float> (height) * 0.5f;
    const float trackHalf = 2.0f;
    const auto trackStart = static_cast<float> (x);
    const auto trackEnd = static_cast<float> (x + width);

    g.setColour (UIColors::pink200);
    g.fillRoundedRectangle (trackStart, trackY - trackHalf,
                            trackEnd - trackStart, trackHalf * 2.0f, trackHalf);

    if (slider.isEnabled())
    {
        g.setColour (UIColors::pink600);
        g.fillRoundedRectangle (trackStart, trackY - trackHalf,
                                sliderPos - trackStart, trackHalf * 2.0f, trackHalf);
    }

    // 滑块：白色圆形 + 主色描边
    const float thumbRadius = 6.0f;
    g.setColour (slider.isEnabled() ? UIColors::keyWhite : UIColors::pink100);
    g.fillEllipse (sliderPos - thumbRadius, trackY - thumbRadius,
                   thumbRadius * 2.0f, thumbRadius * 2.0f);
    g.setColour (slider.isEnabled() ? UIColors::pink600 : UIColors::ink300);
    g.drawEllipse (sliderPos - thumbRadius, trackY - thumbRadius,
                   thumbRadius * 2.0f, thumbRadius * 2.0f, 1.5f);
}

void DeepSvcLookAndFeel::drawScrollbar (juce::Graphics& g,
                                        juce::ScrollBar& scrollbar,
                                        int x, int y, int width, int height,
                                        bool isScrollbarVertical,
                                        int thumbStartPosition,
                                        int thumbSize,
                                        bool isMouseOver,
                                        bool isMouseDown)
{
    if (thumbSize <= 0)
        return;

    juce::Rectangle<float> thumb;
    if (isScrollbarVertical)
        thumb = { static_cast<float> (x) + 3.0f, static_cast<float> (thumbStartPosition),
                  static_cast<float> (width) - 6.0f, static_cast<float> (thumbSize) };
    else
        thumb = { static_cast<float> (thumbStartPosition), static_cast<float> (y) + 3.0f,
                  static_cast<float> (thumbSize), static_cast<float> (height) - 6.0f };

    auto colour = UIColors::pink200;
    if (isMouseDown)
        colour = UIColors::pink500;
    else if (isMouseOver)
        colour = UIColors::pink300;

    if (scrollbar.getProperties()["bypassDesaturate"])
        colour = colour.withMultipliedSaturation (UIColors::bypassSaturation);

    g.setColour (colour);
    g.fillRoundedRectangle (thumb, 3.0f);
}

void DeepSvcLookAndFeel::drawProgressBar (juce::Graphics& g,
                                          juce::ProgressBar&,
                                          int width, int height,
                                          double progress,
                                          const juce::String&)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f,
                                                static_cast<float> (width),
                                                static_cast<float> (height));
    const float radius = bounds.getHeight() * 0.5f;

    g.setColour (UIColors::pink200);
    g.fillRoundedRectangle (bounds, radius);

    const auto filled = bounds.withWidth (bounds.getWidth()
        * static_cast<float> (juce::jlimit (0.0, 1.0, progress)));
    if (filled.getWidth() > 0.5f)
    {
        g.setColour (UIColors::pink600);
        g.fillRoundedRectangle (filled, radius);
    }
}

juce::BorderSize<int> DeepSvcLookAndFeel::getLabelBorderSize (juce::Label&)
{
    return {};
}

void DeepSvcLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    constexpr int leftPadding = 8;
    label.setBounds (leftPadding, 1,
                     box.getWidth() - leftPadding - box.getHeight(),
                     box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

} // namespace deepsvc
