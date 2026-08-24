#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// 对应 OpenTune Source/ARA/OpenTuneEditorView.h
namespace deepsvc
{

class DeepSvcDocumentController;

class DeepSvcEditorView : public juce::ARAEditorView
{
public:
    DeepSvcEditorView (ARA::PlugIn::DocumentController* araDocumentController,
                       DeepSvcDocumentController& deepSvcDocumentController);

    void doNotifySelection (const ARA::PlugIn::ViewSelection* selection) noexcept override;

private:
    DeepSvcDocumentController& deepSvcDocumentController;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeepSvcEditorView)
};

} // namespace deepsvc
