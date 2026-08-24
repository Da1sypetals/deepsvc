#include "DeepSvcEditorView.h"

#include "DeepSvcDocumentController.h"
#include "../DebugLog.h"

#include <utility>
#include <vector>

// 对应 OpenTune Source/ARA/OpenTuneEditorView.cpp
namespace deepsvc
{

DeepSvcEditorView::DeepSvcEditorView (ARA::PlugIn::DocumentController* araDocumentController,
                                      DeepSvcDocumentController& deepSvcDocumentControllerRef)
    : ARAEditorView (araDocumentController)
    , deepSvcDocumentController (deepSvcDocumentControllerRef)
{
}

void DeepSvcEditorView::doNotifySelection (const ARA::PlugIn::ViewSelection* selection) noexcept
{
    juce::ARAEditorView::doNotifySelection (selection);

    auto playbackRegions = selection->getEffectivePlaybackRegions<juce::ARAPlaybackRegion>();
    debugLog ("selection effective=" + juce::String (playbackRegions.size())
              + " timeRange=" + (selection->getTimeRange() != nullptr ? "yes" : "no"));
    deepSvcDocumentController.setEditorViewSelectionPlaybackRegions (std::move (playbackRegions));
}

} // namespace deepsvc
