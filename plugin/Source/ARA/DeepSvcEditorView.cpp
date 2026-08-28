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
    juce::String listed;
    for (auto* region : playbackRegions)
    {
        if (region == nullptr)
            continue;
        const auto* mod = region->getAudioModification();
        const auto* seq = region->getRegionSequence();
        listed += " region p=0x" + juce::String::toHexString (reinterpret_cast<juce::int64> (region))
                + " mod=0x" + juce::String::toHexString (reinterpret_cast<juce::int64> (mod))
                + " modPid=" + (mod != nullptr ? juce::String (mod->getPersistentID()) : juce::String ("-"))
                + " seq=0x" + juce::String::toHexString (reinterpret_cast<juce::int64> (seq))
                + " win=" + juce::String (region->getStartInAudioModificationTime(), 6)
                + "+" + juce::String (region->getDurationInAudioModificationTime(), 6)
                + " place=" + juce::String (region->getStartInPlaybackTime(), 6)
                + "+" + juce::String (region->getDurationInPlaybackTime(), 6)
                + " ||";
    }
    debugLog ("selection effective=" + juce::String (playbackRegions.size())
              + " timeRange=" + (selection->getTimeRange() != nullptr ? "yes" : "no")
              + listed);
    deepSvcDocumentController.setEditorViewSelectionPlaybackRegions (std::move (playbackRegions));
}

} // namespace deepsvc
