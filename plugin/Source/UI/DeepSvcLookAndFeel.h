#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// 组件样式：按钮、滑杆、下拉、开关、滚动条等（docs/ara.md 设计语言章节）
// 主按钮：组件属性 primary=true；其余按钮按次按钮绘制
namespace deepsvc
{

class DeepSvcLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DeepSvcLookAndFeel();

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics& g,
                         juce::TextButton& button,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;

    void drawLinearSlider (juce::Graphics& g,
                           int x, int y, int width, int height,
                           float sliderPos,
                           float minSliderPos,
                           float maxSliderPos,
                           juce::Slider::SliderStyle style,
                           juce::Slider& slider) override;

    void drawScrollbar (juce::Graphics& g,
                        juce::ScrollBar& scrollbar,
                        int x, int y, int width, int height,
                        bool isScrollbarVertical,
                        int thumbStartPosition,
                        int thumbSize,
                        bool isMouseOver,
                        bool isMouseDown) override;

    void drawProgressBar (juce::Graphics& g,
                          juce::ProgressBar& progressBar,
                          int width, int height,
                          double progress,
                          const juce::String& textToShow) override;

    juce::BorderSize<int> getLabelBorderSize (juce::Label&) override;
};

} // namespace deepsvc
