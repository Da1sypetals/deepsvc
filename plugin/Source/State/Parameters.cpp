#include "Parameters.h"

#include <cmath>

namespace deepsvc::parameters
{

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        f0Estimator, "F0 Estimator", juce::StringArray { "RMVPE", "FCPE" }, 0));

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        diffusionSteps, "Diffusion Steps", 1, 64, 16));

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        pitchShift, "Pitch Shift", -24, 24, 12,
        juce::AudioParameterIntAttributes().withLabel ("semitones")));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        cfgRate, "CFG Rate", juce::NormalisableRange<float> (0.0f, 2.0f, 0.05f), 0.9f));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        inputGainDb, "Input Gain", juce::NormalisableRange<float> (-12.0f, 3.0f, 0.5f), -2.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        outputVocoder, "Output Vocoder",
        juce::StringArray { "pupu-vocoder (level 1)", "pc-nsf-hifigan (level 2)" }, 1));

    return { params.begin(), params.end() };
}

EngineSynthParams makeSynthParams (const juce::AudioProcessorValueTreeState& state)
{
    EngineSynthParams params;

    const auto* estimator = state.getParameter (f0Estimator.getParamID());
    params.f0Estimator = estimator != nullptr && estimator->getValue() > 0.5f
        ? EngineEstimator::fcpe
        : EngineEstimator::rmvpe;

    if (const auto* steps = state.getRawParameterValue (diffusionSteps.getParamID()))
        params.diffusionSteps = static_cast<uint32_t> (juce::jlimit (1, 64, static_cast<int> (*steps)));

    if (const auto* shift = state.getRawParameterValue (pitchShift.getParamID()))
        params.pitchShift = static_cast<float> (juce::jlimit (-24, 24, static_cast<int> (shift->load())));

    // 引擎要求 cfg_rate 按 0.05 步进
    if (const auto* cfg = state.getRawParameterValue (cfgRate.getParamID()))
        params.cfgRate = std::round (juce::jlimit (0.0f, 2.0f, cfg->load()) / 0.05f) * 0.05f;

    // 引擎要求 input_gain_db 按 0.5 dB 步进、范围 -12 到 +3
    if (const auto* gain = state.getRawParameterValue (inputGainDb.getParamID()))
        params.inputGainDb = std::round (juce::jlimit (-12.0f, 3.0f, gain->load()) / 0.5f) * 0.5f;

    // 输出声码器：level 1 = pupu-vocoder 输出，需要引擎回传第一级声码器结果
    if (const auto* vocoder = state.getRawParameterValue (outputVocoder.getParamID()))
        params.keepFirstVocoderOutput = static_cast<int> (vocoder->load()) == 0;

    return params;
}

} // namespace deepsvc::parameters
