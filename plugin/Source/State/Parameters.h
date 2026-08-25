#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../Engine/EngineBridge.h"

// 插件参数定义与合成参数换算。
// 对应 OpenTune Source/PluginProcessor.cpp 的 APVTS 参数定义部分；
// 参数表与默认值见 docs/ara.md 参数章节
namespace deepsvc::parameters
{

inline const juce::ParameterID f0Estimator { "f0_estimator", 1 };
inline const juce::ParameterID diffusionSteps { "diffusion_steps", 1 };
inline const juce::ParameterID pitchShift { "pitch_shift", 1 };
inline const juce::ParameterID cfgRate { "cfg_rate", 1 };
inline const juce::ParameterID inputGainDb { "input_gain_db", 1 };
inline const juce::ParameterID outputVocoder { "output_vocoder", 1 };

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// 从 APVTS 状态生成合成参数，量化到引擎要求的步进（cfg_rate 0.05、input_gain_db 0.5 dB）
EngineSynthParams makeSynthParams (const juce::AudioProcessorValueTreeState& state);

// 把槽位参数推进 APVTS（A/B 切换时，docs/ara.md 第 4.1 节），经 setValueNotifyingHost 通知宿主
void pushSynthParamsToApvts (juce::AudioProcessorValueTreeState& state, const EngineSynthParams& params);

// 参数的归档 JSON 表示（docs/ara.md 第 4.2 节）
juce::var synthParamsToJson (const EngineSynthParams& params);
EngineSynthParams synthParamsFromJson (const juce::var& json);

} // namespace deepsvc::parameters