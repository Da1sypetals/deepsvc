#include "PluginProcessor.h"

#include "ARA/DeepSvcDocumentController.h"
#include "ARA/DeepSvcPlaybackRenderer.h"
#include "DebugLog.h"
#include "Plugin/PluginEditor.h"

// 对应 OpenTune Source/PluginProcessor.cpp 的 ARA 路径
namespace deepsvc
{

DeepSvcAudioProcessor::DeepSvcAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
    , apvts (*this, nullptr, "PARAMETERS", parameters::createParameterLayout())
{
    debugLog ("processor created p=" + juce::String::toHexString (reinterpret_cast<int64_t> (this)));
}

DeepSvcAudioProcessor::~DeepSvcAudioProcessor()
{
    debugLog ("processor destroyed p=" + juce::String::toHexString (reinterpret_cast<int64_t> (this)));
}

void DeepSvcAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    playHeadState.reset();
    prepareToPlayForARA (sampleRate,
                         samplesPerBlock,
                         getMainBusNumOutputChannels(),
                         getProcessingPrecision());
}

void DeepSvcAudioProcessor::releaseResources()
{
    playHeadState.reset();
    releaseResourcesForARA();
}

bool DeepSvcAudioProcessor::isBusesLayoutSupported (const BusesLayout&) const
{
    return true;
}

void DeepSvcAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    // 每块只消费一次宿主 PositionInfo，更新处理器持有的传输状态
    juce::Optional<juce::AudioPlayHead::PositionInfo> hostPosition;
    if (wrapperType == juce::AudioProcessor::wrapperType_VST3
        || wrapperType == juce::AudioProcessor::wrapperType_AudioUnit)
        if (auto* playHead = getPlayHead())
            hostPosition = playHead->getPosition();

    playHeadState.update (hostPosition);

    if (buffer.getNumSamples() <= 0)
        return;

    if (isBoundToARA())
    {
        const juce::AudioPlayHead::PositionInfo emptyPositionInfo;
        const auto& araPositionInfo = hostPosition.hasValue() ? *hostPosition : emptyPositionInfo;
        if (processBlockForARA (buffer, isRealtime(), araPositionInfo))
            return;
    }

    // 未绑定 ARA：直通
}

void DeepSvcAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer,
                                                  juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    juce::ignoreUnused (midiMessages);

    // 宿主旁通：由回放渲染器渲染源音频（docs/ara.md 第 4.3 节）；
    // 无可用源音频时保持宿主输入不变
    if (isBoundToARA())
        if (auto* renderer = getPlaybackRenderer<DeepSvcPlaybackRenderer>())
            renderer->renderSourcePassthrough (buffer, getPlayHead());
}

void DeepSvcAudioProcessor::didBindToARA() noexcept
{
    juce::AudioProcessorARAExtension::didBindToARA();
}

juce::AudioProcessorEditor* DeepSvcAudioProcessor::createEditor()
{
    return new DeepSvcEditor (*this);
}

DeepSvcDocumentController* DeepSvcAudioProcessor::getDeepSvcDocumentController() const
{
    auto* controller = AudioProcessorARAExtension::getDocumentController();
    if (controller == nullptr)
        return nullptr;

    return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<DeepSvcDocumentController> (
        controller);
}

void DeepSvcAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void DeepSvcAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

void DeepSvcAudioProcessor::rememberPianoRollViewport (PianoRollPlacementIdentity placement,
                                                       PianoRollViewportPrimitive viewport)
{
    auto& remembered = pianoRollSession.remembered;
    const auto it = std::find_if (remembered.begin(), remembered.end(),
                                  [&placement] (const auto& entry) { return entry.first == placement; });
    if (it != remembered.end())
        it->second = viewport;
    else
        remembered.emplace_back (placement, viewport);
}

std::optional<PianoRollViewportPrimitive> DeepSvcAudioProcessor::readPianoRollViewport (
    const PianoRollPlacementIdentity& placement) const
{
    const auto& remembered = pianoRollSession.remembered;
    const auto it = std::find_if (remembered.begin(), remembered.end(),
                                  [&placement] (const auto& entry) { return entry.first == placement; });
    if (it == remembered.end())
        return std::nullopt;
    return it->second;
}

std::optional<PianoRollPlacementIdentity> DeepSvcAudioProcessor::lastActivePianoRollPlacement() const noexcept
{
    return pianoRollSession.lastActivePlacement;
}

void DeepSvcAudioProcessor::setLastActivePianoRollPlacement (const PianoRollPlacementIdentity& placement) noexcept
{
    pianoRollSession.lastActivePlacement = placement;
}

} // namespace deepsvc

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new deepsvc::DeepSvcAudioProcessor();
}
