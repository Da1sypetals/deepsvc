#include "CopyPanel.h"

#include "UIColors.h"

namespace deepsvc
{

namespace
{

juce::String dash()
{
    return juce::String (u8"无");
}

juce::String estimatorName (EngineEstimator estimator)
{
    switch (estimator)
    {
        case EngineEstimator::rmvpe: return "RMVPE";
        case EngineEstimator::fcpe:  return "FCPE";
    }
    return dash();
}

juce::String vocoderName (bool keepFirstVocoderOutput)
{
    return keepFirstVocoderOutput ? "pupu-vocoder" : "pc-nsf-hifigan";
}

juce::String signedNumber (float value, int decimals)
{
    const auto body = juce::String (value, decimals);
    if (value > 0.0f && ! body.startsWithChar ('+'))
        return juce::String ("+") + body;
    return body;
}

juce::String playbackText (const CopyPanel::State& state)
{
    if (! state.hasModification)
        return dash();
    if (! state.hasSynth)
        return juce::String (u8"空（宿主原声）");
    if (state.bypass)
        return juce::String (u8"原声（旁通）");
    return juce::String (u8"合成音频");
}

} // namespace

void CopyPanel::setState (const State& newState)
{
    state = newState;
    repaint();
}

void CopyPanel::paint (juce::Graphics& g)
{
    UIColors::fillPanelBackground (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);
    UIColors::drawPanelFrame (g, getLocalBounds().toFloat(), UIColors::panelCornerRadius);

    g.setColour (UIColors::ink900);
    g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    g.drawText (juce::String (u8"当前内容"), 12, 8, getWidth() - 24, 22,
                juce::Justification::centredLeft, false);

    int y = 36;
    if (state.hasModification && state.stale)
    {
        auto warn = juce::Rectangle<int> (10, y, getWidth() - 20, 24);
        g.setColour (UIColors::warningBg);
        g.fillRoundedRectangle (warn.toFloat(), 6.0f);
        g.setColour (UIColors::warning);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (juce::String (u8"编辑中的参数尚未写入这个副本"),
                    warn.reduced (8, 0), juce::Justification::centredLeft, true);
        y += 28;
    }

    const auto snap = [&] (const juce::String& value) -> juce::String
    {
        if (! state.hasModification || ! state.hasSynth)
            return dash();
        return value;
    };

    const juce::String fields[][2] = {
        { juce::String (u8"回放"), playbackText (state) },
        { juce::String (u8"音高"), ! state.hasModification ? dash()
            : (state.hasPitch ? juce::String (u8"已检测") : juce::String (u8"未检测")) },
        { juce::String (u8"音色"), snap (state.synthTimbreFile.isNotEmpty()
                                         ? state.synthTimbreFile : dash()) },
        { juce::String (u8"F0 检测算法"), snap (estimatorName (state.synthParams.f0Estimator)) },
        { juce::String (u8"扩散步数"), snap (juce::String (state.synthParams.diffusionSteps)) },
        { juce::String (u8"音高偏移"), snap (signedNumber (state.synthParams.pitchShift, 1)
                                           + juce::String (u8" 半音")) },
        { juce::String (u8"音高微调"), snap (signedNumber (state.synthParams.pitchFineTuneCents, 0)
                                           + juce::String (u8" 音分")) },
        { juce::String (u8"CFG 强度"), snap (juce::String (state.synthParams.cfgRate, 2)) },
        { juce::String (u8"输入增益"), snap (juce::String (state.synthParams.inputGainDb, 1)
                                           + " dB") },
        { juce::String (u8"输出声码器"), snap (vocoderName (state.synthParams.keepFirstVocoderOutput)) },
        { juce::String (u8"覆盖区间"), snap (juce::String (state.synthStartTime, 2)
                                           + juce::String (u8" – ")
                                           + juce::String (state.synthEndTime, 2)
                                           + juce::String (u8" 秒")) },
        { juce::String (u8"合成耗时"), snap (state.lastSynthElapsedSeconds.has_value()
                                           ? juce::String (*state.lastSynthElapsedSeconds, 1)
                                               + juce::String (u8" 秒")
                                           : dash()) },
    };

    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    for (const auto& field : fields)
    {
        auto row = juce::Rectangle<int> (12, y, getWidth() - 24, 18);
        g.setColour (UIColors::ink600);
        g.drawText (field[0], row.removeFromLeft (78), juce::Justification::centredLeft, true);
        g.setColour (UIColors::ink900);
        g.drawText (field[1], row, juce::Justification::centredRight, true);
        y += 18;
    }
}

} // namespace deepsvc
