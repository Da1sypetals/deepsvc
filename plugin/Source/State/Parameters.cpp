#include "Parameters.h"

#include <cmath>

#include "../Directories.h"

// 对应 OpenTune Source/PluginProcessor.cpp 的 APVTS 参数定义部分
// 对应 OpenTune Source/PluginProcessor.cpp 的 APVTS 参数定义部分
// 对应 OpenTune Source/PluginProcessor.cpp 的 APVTS 参数定义部分
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

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        pitchFineTuneCents, "Pitch Fine Tune", -100, 100, 0,
        juce::AudioParameterIntAttributes().withLabel ("cents")));

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

    if (const auto* cents = state.getRawParameterValue (pitchFineTuneCents.getParamID()))
        params.pitchFineTuneCents = static_cast<float> (
            juce::jlimit (-100, 100, static_cast<int> (cents->load())));

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

void pushSynthParamsToApvts (juce::AudioProcessorValueTreeState& state, const EngineSynthParams& params)
{
    const auto setValue = [&state] (const juce::ParameterID& id, float plainValue)
    {
        if (auto* parameter = state.getParameter (id.getParamID()))
            parameter->setValue (parameter->convertTo0to1 (plainValue));
    };

    setValue (f0Estimator, params.f0Estimator == EngineEstimator::fcpe ? 1.0f : 0.0f);
    setValue (diffusionSteps, static_cast<float> (params.diffusionSteps));
    setValue (pitchShift, params.pitchShift);
    setValue (pitchFineTuneCents, params.pitchFineTuneCents);
    setValue (cfgRate, params.cfgRate);
    setValue (inputGainDb, params.inputGainDb);
    setValue (outputVocoder, params.keepFirstVocoderOutput ? 0.0f : 1.0f);
}

juce::var synthParamsToJson (const EngineSynthParams& params)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("estimator", params.f0Estimator == EngineEstimator::fcpe ? 1 : 0);
    object->setProperty ("diffusionSteps", static_cast<int> (params.diffusionSteps));
    object->setProperty ("pitchShift", static_cast<double> (params.pitchShift));
    object->setProperty ("pitchFineTuneCents", static_cast<double> (params.pitchFineTuneCents));
    object->setProperty ("cfgRate", static_cast<double> (params.cfgRate));
    object->setProperty ("inputGainDb", static_cast<double> (params.inputGainDb));
    object->setProperty ("outputVocoder", params.keepFirstVocoderOutput ? 0 : 1);
    return juce::var (object);
}

EngineSynthParams synthParamsFromJson (const juce::var& json)
{
    EngineSynthParams params;
    params.f0Estimator = static_cast<int> (json.getProperty ("estimator", 0)) == 1
        ? EngineEstimator::fcpe
        : EngineEstimator::rmvpe;
    params.diffusionSteps = static_cast<uint32_t> (juce::jlimit (
        1, 64, static_cast<int> (json.getProperty ("diffusionSteps", 16))));
    params.pitchShift = static_cast<float> (juce::jlimit (
        -24.0, 24.0, static_cast<double> (json.getProperty ("pitchShift", 12.0))));
    params.pitchFineTuneCents = static_cast<float> (juce::jlimit (
        -100.0, 100.0, static_cast<double> (json.getProperty ("pitchFineTuneCents", 0.0))));
    params.cfgRate = static_cast<float> (juce::jlimit (
        0.0, 2.0, static_cast<double> (json.getProperty ("cfgRate", 0.9))));
    params.inputGainDb = static_cast<float> (juce::jlimit (
        -12.0, 3.0, static_cast<double> (json.getProperty ("inputGainDb", -2.0))));
    params.keepFirstVocoderOutput = static_cast<int> (json.getProperty ("outputVocoder", 1)) == 0;
    return params;
}

void saveWorkingState (const EngineSynthParams& params, const juce::String& timbreFile)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("params", synthParamsToJson (params));
    object->setProperty ("timbreFile", timbreFile);
    auto file = directories::workingStateFile();
    file.getParentDirectory().createDirectory();
    if (! file.replaceWithText (juce::JSON::toString (juce::var (object))))
        jassertfalse;
}

bool loadWorkingState (EngineSynthParams& params, juce::String& timbreFile)
{
    auto file = directories::workingStateFile();
    if (! file.existsAsFile())
        return false;

    const auto json = juce::JSON::parse (file);
    if (! json.isObject())
    {
        jassertfalse;
        return false;
    }

    params = synthParamsFromJson (json.getProperty ("params", juce::var()));
    timbreFile = json.getProperty ("timbreFile", juce::String()).toString();
    return true;
}

} // namespace deepsvc::parameters
