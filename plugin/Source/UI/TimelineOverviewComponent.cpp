#include "TimelineOverviewComponent.h"

#include "UIColors.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace deepsvc
{

namespace
{

constexpr int kOverviewVisualHeight = 60;
constexpr float kOverviewContentInsetX = 4.0f;
constexpr float kOverviewContentInsetY = 5.0f;
constexpr float kWaveformAlpha = 0.42f;

void hashCombine (uint64_t& seed, uint64_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

void paintOverviewClipWaveform (juce::Graphics& g,
                                ContentKey contentKey,
                                const ContentTimelineProjection& projection,
                                juce::Rectangle<float> contentBounds,
                                double pixelsPerSecond,
                                const WaveformMipmapCache& waveformMipmapCache)
{
    if (! projection.isValid() || pixelsPerSecond <= 0.0)
        return;

    const auto* mipmap = waveformMipmapCache.get (contentKey);
    if (mipmap == nullptr || ! mipmap->hasSource())
        return;

    const int levelIndex = mipmap->selectBestLevelIndex (pixelsPerSecond);
    if (levelIndex < 0)
        return;
    const auto& level = mipmap->getLevel (levelIndex);

    const int64_t numPeaks = static_cast<int64_t> (level.peaks.size());
    const int samplesPerPeak = WaveformMipmap::kSamplesPerPeak[levelIndex];
    const double timePerPeak = static_cast<double> (samplesPerPeak)
        / WaveformMipmap::kBaseSampleRate;
    const double clipStartSeconds = projection.timelineStartSeconds;
    const double clipEndSeconds = projection.timelineEndSeconds();

    juce::Path wavePath;
    const int firstX = static_cast<int> (std::floor (contentBounds.getX()));
    const int lastX = static_cast<int> (std::ceil (contentBounds.getRight()));
    const float midY = contentBounds.getCentreY();
    const float halfH = contentBounds.getHeight() * 0.40f;

    for (int x = firstX; x < lastX; ++x)
    {
        const double timelineTime = clipStartSeconds
            + (static_cast<double> (x) + 0.5 - static_cast<double> (contentBounds.getX()))
                / pixelsPerSecond;
        if (timelineTime < clipStartSeconds || timelineTime >= clipEndSeconds)
            continue;

        const double contentTime = projection.projectTimelineTimeToContent (timelineTime);
        const int64_t peakIndex = static_cast<int64_t> (std::floor (contentTime / timePerPeak));

        const double nextTimelineTime = std::min (
            clipEndSeconds,
            clipStartSeconds
                + (static_cast<double> (x) + 1.5 - static_cast<double> (contentBounds.getX()))
                    / pixelsPerSecond);
        const double nextContentTime = projection.projectTimelineTimeToContent (nextTimelineTime);

        int64_t indexStart = peakIndex;
        int64_t indexEnd = static_cast<int64_t> (std::floor (nextContentTime / timePerPeak));
        if (indexEnd <= indexStart)
            indexEnd = indexStart + 1;

        indexStart = std::max<int64_t> (0, indexStart);
        indexEnd = std::min<int64_t> (numPeaks, indexEnd);
        if (indexStart >= indexEnd)
            continue;

        float aggregateMin = 0.0f;
        float aggregateMax = 0.0f;
        bool hasData = false;

        for (int64_t i = indexStart; i < indexEnd; ++i)
        {
            const auto& peak = level.peaks[static_cast<std::size_t> (i)];
            if (peak.isZero())
                continue;

            if (! hasData)
            {
                aggregateMin = peak.getMin();
                aggregateMax = peak.getMax();
                hasData = true;
            }
            else
            {
                aggregateMin = std::min (aggregateMin, peak.getMin());
                aggregateMax = std::max (aggregateMax, peak.getMax());
            }
        }

        if (! hasData)
            continue;

        const float displayTop = aggregateMax * halfH;
        const float displayBottom = aggregateMin * halfH;
        float y1 = midY - displayTop;
        float y2 = midY - displayBottom;
        if ((y2 - y1) < 1.0f)
        {
            const float expand = (1.0f - (y2 - y1)) * 0.5f;
            y1 -= expand;
            y2 += expand;
        }

        const float lineX = static_cast<float> (x) + 0.5f;
        wavePath.startNewSubPath (lineX, y1);
        wavePath.lineTo (lineX, y2);
    }

    if (wavePath.isEmpty())
        return;

    juce::Graphics::ScopedSaveState scoped (g);
    g.reduceClipRegion (contentBounds.getSmallestIntegerContainer());
    g.setColour (UIColors::pink300.withAlpha (kWaveformAlpha));
    g.strokePath (wavePath, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
}

} // namespace

TimelineOverviewComponent::TimelineOverviewComponent (WaveformMipmapCache& cache)
    : waveformMipmapCache (cache)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void TimelineOverviewComponent::addListener (Listener* listener)
{
    listeners.add (listener);
}

void TimelineOverviewComponent::removeListener (Listener* listener)
{
    listeners.remove (listener);
}

juce::Rectangle<float> TimelineOverviewComponent::getContentBounds() const noexcept
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop (juce::jmax (0, bounds.getHeight() - kOverviewVisualHeight));
    return bounds.toFloat().reduced (kOverviewContentInsetX, kOverviewContentInsetY);
}

uint64_t TimelineOverviewComponent::calculateContentSignature() const
{
    uint64_t signature = 0xcbf29ce484222325ull;
    hashCombine (signature, static_cast<uint64_t> (contentKey.domainKind));
    hashCombine (signature, contentKey.objectId);
    hashCombine (signature, static_cast<uint64_t> (std::hash<double> {} (projection.timelineStartSeconds)));
    hashCombine (signature, static_cast<uint64_t> (std::hash<double> {} (projection.timelineDurationSeconds)));
    hashCombine (signature, static_cast<uint64_t> (std::hash<double> {} (projection.contentStartSeconds)));
    hashCombine (signature, static_cast<uint64_t> (std::hash<double> {} (projection.contentDurationSeconds)));
    hashCombine (signature, static_cast<uint64_t> (viewportWidthPx));
    return signature;
}

TimelineOverviewComponent::Geometry TimelineOverviewComponent::calculateGeometry() const
{
    Geometry geometry;
    geometry.contentBounds = getContentBounds();
    if (geometry.contentBounds.isEmpty() || ! projection.isValid())
        return geometry;

    geometry.spanSeconds = projection.timelineDurationSeconds;
    if (geometry.spanSeconds <= 0.0)
        return geometry;

    geometry.previewPixelsPerSecond = geometry.contentBounds.getWidth() / geometry.spanSeconds;
    if (camera.pixelsPerSecond > 0.0 && viewportWidthPx > 0)
        geometry.visibleDurationSeconds = static_cast<double> (viewportWidthPx) / camera.pixelsPerSecond;

    const double clipStartSeconds = projection.timelineStartSeconds;
    const double clipEndSeconds = projection.timelineEndSeconds();
    geometry.maxVisibleStartSeconds = std::max (clipStartSeconds,
                                                clipEndSeconds - geometry.visibleDurationSeconds);
    geometry.visibleStartSeconds = juce::jlimit (clipStartSeconds,
                                                 geometry.maxVisibleStartSeconds,
                                                 camera.visibleStartSeconds);
    return geometry;
}

void TimelineOverviewComponent::onHeartbeatTick (ContentKey key,
                                                 const ContentTimelineProjection& newProjection,
                                                 TimelineViewportCamera newCamera,
                                                 int newViewportWidthPx)
{
    contentKey = key;
    projection = newProjection;
    camera = newCamera;
    viewportWidthPx = newViewportWidthPx;

    const uint64_t signature = calculateContentSignature();

    float buildProgress = 0.0f;
    if (const auto* mipmap = waveformMipmapCache.get (contentKey))
        buildProgress = mipmap->getBuildProgress();

    const bool dirty = camera != lastCamera
        || signature != lastSignature
        || buildProgress != lastBuildProgress;

    if (! dirty)
        return;

    lastCamera = camera;
    lastSignature = signature;
    lastBuildProgress = buildProgress;
    repaint();
}

void TimelineOverviewComponent::requestNavigation (double visibleStartSeconds)
{
    const auto geometry = calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0 || camera.pixelsPerSecond <= 0.0)
        return;

    const double targetStart = juce::jlimit (projection.timelineStartSeconds,
                                             geometry.maxVisibleStartSeconds,
                                             visibleStartSeconds);
    camera.visibleStartSeconds = targetStart;

    listeners.call ([targetStart, pixelsPerSecond = camera.pixelsPerSecond] (Listener& listener)
    {
        listener.overviewNavigateRequested (targetStart, pixelsPerSecond);
    });
    repaint();
}

void TimelineOverviewComponent::updateMouseCursor (juce::Point<float> position)
{
    const auto geometry = calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0 || ! geometry.contentBounds.contains (position))
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
        return;
    }

    if (isDragging)
    {
        setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        return;
    }

    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void TimelineOverviewComponent::mouseDown (const juce::MouseEvent& e)
{
    const auto geometry = calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0 || ! geometry.contentBounds.contains (e.position))
        return;

    const double pointerTime = projection.timelineStartSeconds
        + (static_cast<double> (e.position.x) - geometry.contentBounds.getX())
            / geometry.previewPixelsPerSecond;
    const float viewX = geometry.contentBounds.getX()
        + static_cast<float> ((geometry.visibleStartSeconds - projection.timelineStartSeconds)
                              * geometry.previewPixelsPerSecond);
    const float viewRight = viewX + static_cast<float> (geometry.visibleDurationSeconds
                                                        * geometry.previewPixelsPerSecond);
    const bool insideViewport = e.position.x >= viewX && e.position.x <= viewRight;

    dragPointerOffsetSeconds = insideViewport
        ? pointerTime - geometry.visibleStartSeconds
        : geometry.visibleDurationSeconds * 0.5;
    isDragging = true;
    requestNavigation (pointerTime - dragPointerOffsetSeconds);
    updateMouseCursor (e.position);
}

void TimelineOverviewComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging)
        return;

    const auto geometry = calculateGeometry();
    if (geometry.previewPixelsPerSecond <= 0.0)
        return;

    const double pointerTime = projection.timelineStartSeconds
        + (static_cast<double> (e.position.x) - geometry.contentBounds.getX())
            / geometry.previewPixelsPerSecond;
    requestNavigation (pointerTime - dragPointerOffsetSeconds);
    updateMouseCursor (e.position);
}

void TimelineOverviewComponent::mouseMove (const juce::MouseEvent& e)
{
    updateMouseCursor (e.position);
}

void TimelineOverviewComponent::mouseUp (const juce::MouseEvent& e)
{
    isDragging = false;
    dragPointerOffsetSeconds = 0.0;
    updateMouseCursor (e.position);
}

void TimelineOverviewComponent::paint (juce::Graphics& g)
{
    auto panelBounds = getLocalBounds();
    panelBounds.removeFromTop (juce::jmax (0, panelBounds.getHeight() - kOverviewVisualHeight));
    const auto panel = panelBounds.toFloat().reduced (0.5f);
    if (panel.isEmpty())
        return;

    g.setColour (UIColors::pink100.withAlpha (0.95f));
    g.fillRoundedRectangle (panel, UIColors::panelCornerRadius);

    const auto contentBounds = getContentBounds();
    const auto geometry = calculateGeometry();
    if (! contentBounds.isEmpty() && geometry.previewPixelsPerSecond > 0.0)
    {
        paintOverviewClipWaveform (g, contentKey, projection, contentBounds,
                                   geometry.previewPixelsPerSecond, waveformMipmapCache);

        const auto viewBounds = juce::Rectangle<float> (
            contentBounds.getX()
                + static_cast<float> ((geometry.visibleStartSeconds - projection.timelineStartSeconds)
                                      * geometry.previewPixelsPerSecond),
            contentBounds.getY(),
            static_cast<float> (geometry.visibleDurationSeconds * geometry.previewPixelsPerSecond),
            contentBounds.getHeight()).getIntersection (contentBounds);

        if (! viewBounds.isEmpty())
        {
            g.setColour (UIColors::pink050.withAlpha (0.55f));
            g.fillRoundedRectangle (viewBounds, 4.0f);
            g.setColour (UIColors::pink600.withAlpha (0.70f));
            g.drawRoundedRectangle (viewBounds, 4.0f, 1.0f);
        }
    }

    g.setColour (UIColors::pink200);
    g.drawRoundedRectangle (panel, UIColors::panelCornerRadius, 1.0f);
}

} // namespace deepsvc
