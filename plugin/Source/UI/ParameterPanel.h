#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../PluginProcessor.h"

// 合成参数面板：全部控件绑定 APVTS（docs/ara.md 参数表）。
// 对应 OpenTune Source/Standalone/UI/ParameterPanel.h
namespace deepsvc
{

class ParameterPanel : public juce::Component
                     , private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit ParameterPanel (juce::AudioProcessorValueTreeState& state);
    ~ParameterPanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    static constexpr int kWidth = 240;
    static int preferredHeight() noexcept;

private:
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void nudgeDiffusionSteps (int delta);
    void syncDiffusionStepsValue();

    juce::AudioProcessorValueTreeState& apvts;

    juce::Label estimatorLabel;
    juce::ComboBox estimatorCombo;
    juce::Label stepsLabel;
    juce::Label stepsValueLabel;
    juce::TextButton stepsDownButton { juce::String (u8"-") };
    juce::TextButton stepsUpButton { juce::String (u8"+") };
    juce::Label pitchShiftLabel;
    juce::Slider pitchShiftSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton octaveDownButton { juce::String (u8"-12") };
    juce::TextButton octaveUpButton { juce::String (u8"+12") };
    juce::Label pitchFineTuneLabel;
    juce::Slider pitchFineTuneSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::TextButton fineTuneDownButton { juce::String (u8"-5") };
    juce::TextButton fineTuneUpButton { juce::String (u8"+5") };
    juce::Label cfgLabel;
    juce::Slider cfgSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label gainLabel;
    juce::Slider gainSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label vocoderLabel;
    juce::ComboBox vocoderCombo;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> estimatorAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchShiftAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> pitchFineTuneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> cfgAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> gainAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> vocoderAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParameterPanel)
};

} // namespace deepsvc
