#include "ParameterPanel.h"

#include "UIColors.h"

namespace deepsvc
{

ParameterPanel::ParameterPanel (juce::AudioProcessorValueTreeState& state)
    : apvts (state)
{
    // 标签文字显式使用主文字色，不依赖 LookAndFeel 颜色解析
    for (auto* label : { &estimatorLabel, &stepsLabel, &pitchShiftLabel, &pitchFineTuneLabel,
                         &cfgLabel, &gainLabel, &vocoderLabel })
        label->setColour (juce::Label::textColourId, UIColors::ink900);

    estimatorLabel.setText (juce::String (u8"F0 检测算法"), juce::dontSendNotification);
    addAndMakeVisible (estimatorLabel);
    estimatorCombo.addItem ("RMVPE", 1);
    estimatorCombo.addItem ("FCPE", 2);
    addAndMakeVisible (estimatorCombo);
    estimatorAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        apvts, parameters::f0Estimator.getParamID(), estimatorCombo);

    stepsLabel.setText (juce::String (u8"扩散步数"), juce::dontSendNotification);
    addAndMakeVisible (stepsLabel);
    stepsValueLabel.setJustificationType (juce::Justification::centred);
    stepsValueLabel.setColour (juce::Label::backgroundColourId, UIColors::pink050);
    stepsValueLabel.setColour (juce::Label::textColourId, UIColors::ink900);
    stepsValueLabel.setColour (juce::Label::outlineColourId, UIColors::pink200);
    addAndMakeVisible (stepsValueLabel);
    stepsDownButton.onClick = [this] { nudgeDiffusionSteps (-1); };
    stepsUpButton.onClick = [this] { nudgeDiffusionSteps (1); };
    addAndMakeVisible (stepsDownButton);
    addAndMakeVisible (stepsUpButton);
    apvts.addParameterListener (parameters::diffusionSteps.getParamID(), this);
    syncDiffusionStepsValue();

    pitchShiftLabel.setText (juce::String (u8"音高偏移 (半音)"), juce::dontSendNotification);
    addAndMakeVisible (pitchShiftLabel);
    pitchShiftSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    addAndMakeVisible (pitchShiftSlider);
    pitchShiftAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, parameters::pitchShift.getParamID(), pitchShiftSlider);

    octaveDownButton.onClick = [this] { pitchShiftSlider.setValue (-12.0, juce::sendNotificationSync); };
    octaveUpButton.onClick = [this] { pitchShiftSlider.setValue (12.0, juce::sendNotificationSync); };
    addAndMakeVisible (octaveDownButton);
    addAndMakeVisible (octaveUpButton);

    pitchFineTuneLabel.setText (juce::String (u8"音高微调 (cents)"), juce::dontSendNotification);
    addAndMakeVisible (pitchFineTuneLabel);
    pitchFineTuneSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 20);
    addAndMakeVisible (pitchFineTuneSlider);
    pitchFineTuneAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, parameters::pitchFineTuneCents.getParamID(), pitchFineTuneSlider);

    fineTuneDownButton.onClick = [this]
    {
        pitchFineTuneSlider.setValue (pitchFineTuneSlider.getValue() - 5.0, juce::sendNotificationSync);
    };
    fineTuneUpButton.onClick = [this]
    {
        pitchFineTuneSlider.setValue (pitchFineTuneSlider.getValue() + 5.0, juce::sendNotificationSync);
    };
    addAndMakeVisible (fineTuneDownButton);
    addAndMakeVisible (fineTuneUpButton);

    cfgLabel.setText (juce::String (u8"CFG 强度"), juce::dontSendNotification);
    addAndMakeVisible (cfgLabel);
    cfgSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    addAndMakeVisible (cfgSlider);
    cfgAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, parameters::cfgRate.getParamID(), cfgSlider);

    gainLabel.setText (juce::String (u8"输入增益 (dB)"), juce::dontSendNotification);
    addAndMakeVisible (gainLabel);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 20);
    addAndMakeVisible (gainSlider);
    gainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, parameters::inputGainDb.getParamID(), gainSlider);

    vocoderLabel.setText (juce::String (u8"输出声码器"), juce::dontSendNotification);
    addAndMakeVisible (vocoderLabel);
    vocoderCombo.addItem ("pupu-vocoder (level 1)", 1);
    vocoderCombo.addItem ("pc-nsf-hifigan (level 2)", 2);
    addAndMakeVisible (vocoderCombo);
    vocoderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        apvts, parameters::outputVocoder.getParamID(), vocoderCombo);
}

ParameterPanel::~ParameterPanel()
{
    apvts.removeParameterListener (parameters::diffusionSteps.getParamID(), this);
}

void ParameterPanel::nudgeDiffusionSteps (int delta)
{
    auto* parameter = apvts.getParameter (parameters::diffusionSteps.getParamID());
    if (parameter == nullptr)
        return;
    const int current = juce::roundToInt (parameter->convertFrom0to1 (parameter->getValue()));
    const int next = juce::jlimit (1, 64, current + delta);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (static_cast<float> (next)));
}

void ParameterPanel::syncDiffusionStepsValue()
{
    auto* parameter = apvts.getParameter (parameters::diffusionSteps.getParamID());
    if (parameter == nullptr)
        return;
    const int value = juce::roundToInt (parameter->convertFrom0to1 (parameter->getValue()));
    stepsValueLabel.setText (juce::String (value), juce::dontSendNotification);
}

void ParameterPanel::parameterChanged (const juce::String& parameterID, float)
{
    if (parameterID != parameters::diffusionSteps.getParamID())
        return;
    if (! juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ParameterPanel> (this)]
        {
            if (safe != nullptr)
                safe->syncDiffusionStepsValue();
        });
        return;
    }
    syncDiffusionStepsValue();
}

void ParameterPanel::paint (juce::Graphics& g)
{
    UIColors::fillPanelBackground (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);
    UIColors::drawPanelFrame (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);

    g.setColour (UIColors::ink900);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawText (juce::String (u8"合成参数"), 12, 8, getWidth() - 24, 22,
                juce::Justification::centredLeft, false);
}

int ParameterPanel::preferredHeight() noexcept
{
    constexpr int padding = 12;
    constexpr int titleHeight = 30;
    constexpr int labelHeight = 18;
    constexpr int controlHeight = 24;
    constexpr int rowGap = 4;
    constexpr int labelGap = 2;
    constexpr int rowCount = 7;
    const int row = labelHeight + labelGap + controlHeight + rowGap;
    return padding + titleHeight + row * rowCount + padding;
}

void ParameterPanel::resized()
{
    auto area = getLocalBounds().reduced (12);
    area.removeFromTop (30);

    constexpr int labelHeight = 18;
    constexpr int controlHeight = 24;
    constexpr int rowGap = 4;

    auto placeRow = [&] (juce::Label& label, juce::Component& control)
    {
        label.setBounds (area.removeFromTop (labelHeight));
        area.removeFromTop (2);
        control.setBounds (area.removeFromTop (controlHeight));
        area.removeFromTop (rowGap);
    };

    placeRow (estimatorLabel, estimatorCombo);

    stepsLabel.setBounds (area.removeFromTop (labelHeight));
    area.removeFromTop (2);
    {
        auto row = area.removeFromTop (controlHeight);
        stepsDownButton.setBounds (row.removeFromLeft (36));
        row.removeFromLeft (4);
        stepsValueLabel.setBounds (row.removeFromLeft (56));
        row.removeFromLeft (4);
        stepsUpButton.setBounds (row.removeFromLeft (36));
    }
    area.removeFromTop (rowGap);

    auto placeSliderWithStepButtons = [&] (juce::Label& label,
                                           juce::Button& downButton,
                                           juce::Slider& slider,
                                           juce::Button& upButton)
    {
        label.setBounds (area.removeFromTop (labelHeight));
        area.removeFromTop (2);
        auto row = area.removeFromTop (controlHeight);
        downButton.setBounds (row.removeFromLeft (36));
        row.removeFromLeft (4);
        upButton.setBounds (row.removeFromRight (36));
        row.removeFromRight (4);
        slider.setBounds (row);
        area.removeFromTop (rowGap);
    };

    placeSliderWithStepButtons (pitchShiftLabel, octaveDownButton, pitchShiftSlider, octaveUpButton);
    placeSliderWithStepButtons (pitchFineTuneLabel, fineTuneDownButton, pitchFineTuneSlider, fineTuneUpButton);
    placeRow (cfgLabel, cfgSlider);
    placeRow (gainLabel, gainSlider);
    placeRow (vocoderLabel, vocoderCombo);
}

} // namespace deepsvc
