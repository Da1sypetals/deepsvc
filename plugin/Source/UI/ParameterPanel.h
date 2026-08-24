#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"

// 合成参数面板：全部控件绑定 APVTS（docs/ara.md 参数表）
namespace deepsvc
{

class ParameterPanel : public juce::Component
{
public:
    explicit ParameterPanel (juce::AudioProcessorValueTreeState& state);

    void paint (juce::Graphics& g) override;
    void resized() override;

    static constexpr int kWidth = 240;
    static constexpr int kMinimumPanelHeight = 320;

private:
    juce::AudioProcessorValueTreeState& apvts;

    juce::Label estimatorLabel;
    juce::ComboBox estimatorCombo;
    juce::Label stepsLabel;
    juce::Slider stepsSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label pitchShiftLabel;
    juce::Slider pitchShiftSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton octaveDownButton { juce::String (u8"-12") };
    juce::TextButton octaveUpButton { juce::String (u8"+12") };
    juce::Label cfgLabel;
    juce::Slider cfgSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label gainLabel;
    juce::Slider gainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label vocoderLabel;
    juce::ComboBox vocoderCombo;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> estimatorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> stepsAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchShiftAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> cfgAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> vocoderAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterPanel)
};

} // namespace deepsvc
