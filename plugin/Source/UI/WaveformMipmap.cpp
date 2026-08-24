#include "WaveformMipmap.h"

#include <algorithm>
#include <cmath>

// 对应 OpenTune Source/Standalone/UI/WaveformMipmap.cpp
namespace deepsvc
{

void WaveformMipmap::setAudioSource (std::shared_ptr<const juce::AudioBuffer<float>> buffer)
{
    if (! buffer || buffer->getNumSamples() == 0)
    {
        clear();
        return;
    }

    audioBuffer = buffer;
    numSamples = buffer->getNumSamples();
    numChannels = buffer->getNumChannels();

    initializeLevels();
}

bool WaveformMipmap::isSourceChanged (std::shared_ptr<const juce::AudioBuffer<float>> buffer) const noexcept
{
    if (audioBuffer.get() != buffer.get())
        return true;

    if (buffer)
        if (numSamples != buffer->getNumSamples() || numChannels != buffer->getNumChannels())
            return true;

    return false;
}

void WaveformMipmap::initializeLevels()
{
    for (int i = 0; i < kNumLevels; ++i)
    {
        const int64_t numPeaks = (numSamples + kSamplesPerPeak[i] - 1) / kSamplesPerPeak[i];
        levels[i].peaks.assign (static_cast<std::size_t> (numPeaks), PeakSample());
        levels[i].numSamplesCovered = numSamples;
        levels[i].complete = false;
        levels[i].buildProgress = 0;
    }
}

bool WaveformMipmap::buildIncremental (double timeBudgetMs)
{
    if (! audioBuffer || numSamples <= 0)
        return false;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool levelCompleted = false;

    for (int level = 0; level < kNumLevels; ++level)
    {
        if (levels[level].complete)
            continue;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
            break;

        buildLevelSlice (level, remain);

        if (levels[level].complete)
        {
            levelCompleted = true;
            continue;
        }

        break;
    }

    return levelCompleted;
}

bool WaveformMipmap::buildLevelSlice (int level, double timeBudgetMs)
{
    auto& lvl = levels[level];

    if (lvl.complete || ! audioBuffer || numSamples <= 0)
        return false;

    const int samplesPerPeak = kSamplesPerPeak[level];
    const int64_t totalPeaks = static_cast<int64_t> (lvl.peaks.size());
    if (totalPeaks <= 0)
        return false;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool progressed = false;
    const int batchSize = 256;

    while (lvl.buildProgress < totalPeaks)
    {
        const int64_t startIdx = lvl.buildProgress;
        const int64_t endIdx = std::min (startIdx + batchSize, totalPeaks);

        for (int64_t peakIdx = startIdx; peakIdx < endIdx; ++peakIdx)
        {
            const int64_t sampleStart = peakIdx * samplesPerPeak;
            const int64_t sampleEnd = std::min (sampleStart + samplesPerPeak, numSamples);
            const int64_t numToProcess = sampleEnd - sampleStart;

            if (numToProcess <= 0)
            {
                lvl.peaks[static_cast<std::size_t> (peakIdx)] = PeakSample();
                continue;
            }

            float globalMin = 0.0f;
            float globalMax = 0.0f;
            bool hasData = false;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* channelData = audioBuffer->getReadPointer (ch);
                auto range = juce::FloatVectorOperations::findMinAndMax (
                    channelData + sampleStart, static_cast<int> (numToProcess));

                if (! hasData)
                {
                    globalMin = range.getStart();
                    globalMax = range.getEnd();
                    hasData = true;
                }
                else
                {
                    globalMin = std::min (globalMin, range.getStart());
                    globalMax = std::max (globalMax, range.getEnd());
                }
            }

            lvl.peaks[static_cast<std::size_t> (peakIdx)].setRange (globalMin, globalMax);
        }

        lvl.buildProgress = endIdx;
        progressed = true;

        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if ((nowMs - startMs) >= timeBudgetMs)
            break;
    }

    if (lvl.buildProgress >= totalPeaks)
        lvl.complete = true;

    return progressed;
}

bool WaveformMipmap::isComplete() const noexcept
{
    for (int i = 0; i < kNumLevels; ++i)
        if (! levels[i].complete)
            return false;
    return true;
}

float WaveformMipmap::getBuildProgress() const noexcept
{
    if (! audioBuffer || numSamples <= 0)
        return 0.0f;

    int64_t totalPeaks = 0;
    int64_t completedPeaks = 0;

    for (int i = 0; i < kNumLevels; ++i)
    {
        totalPeaks += static_cast<int64_t> (levels[i].peaks.size());
        completedPeaks += levels[i].buildProgress;
    }

    if (totalPeaks == 0)
        return 0.0f;

    return static_cast<float> (completedPeaks) / static_cast<float> (totalPeaks);
}

int WaveformMipmap::selectBestLevelIndex (double pixelsPerSecond) const
{
    const double secondsPerPixel = 1.0 / pixelsPerSecond;

    for (int i = kNumLevels - 1; i >= 0; --i)
    {
        const double secondsPerPeak = static_cast<double> (kSamplesPerPeak[i]) / kBaseSampleRate;
        if (secondsPerPeak <= secondsPerPixel * 2.0 && levels[i].complete && ! levels[i].peaks.empty())
            return i;
    }

    for (int i = 0; i < kNumLevels; ++i)
        if (levels[i].complete && ! levels[i].peaks.empty())
            return i;

    return -1;
}

void WaveformMipmap::clear()
{
    audioBuffer.reset();
    numSamples = 0;
    numChannels = 0;

    for (int i = 0; i < kNumLevels; ++i)
    {
        levels[i].peaks.clear();
        levels[i].numSamplesCovered = 0;
        levels[i].complete = false;
        levels[i].buildProgress = 0;
    }
}

WaveformMipmap& WaveformMipmapCache::getOrCreate (ContentKey contentKey)
{
    auto it = caches.find (contentKey);
    if (it != caches.end())
        return *it->second;

    auto inserted = caches.emplace (contentKey, std::make_unique<WaveformMipmap>());
    allMipmapsComplete = false;
    return *inserted.first->second;
}

void WaveformMipmapCache::remove (ContentKey contentKey)
{
    caches.erase (contentKey);
}

void WaveformMipmapCache::prune (const std::set<ContentKey>& alive)
{
    for (auto it = caches.begin(); it != caches.end();)
    {
        if (alive.find (it->first) == alive.end())
        {
            it = caches.erase (it);
            continue;
        }
        ++it;
    }
}

void WaveformMipmapCache::clear()
{
    caches.clear();
    allMipmapsComplete = true;
}

void WaveformMipmapCache::setAudioSource (ContentKey contentKey,
                                          std::shared_ptr<const juce::AudioBuffer<float>> buffer)
{
    auto& mipmap = getOrCreate (contentKey);
    if (! mipmap.isSourceChanged (buffer))
        return;
    mipmap.setAudioSource (buffer);
    allMipmapsComplete = false;
}

bool WaveformMipmapCache::buildIncremental (double timeBudgetMs)
{
    if (timeBudgetMs <= 0.0 || allMipmapsComplete)
        return false;

    const double startMs = juce::Time::getMillisecondCounterHiRes();
    bool levelCompleted = false;
    bool allComplete = true;

    for (auto& kv : caches)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        const double remain = timeBudgetMs - (nowMs - startMs);
        if (remain <= 0.0)
        {
            allComplete = false;
            break;
        }

        if (kv.second->buildIncremental (remain))
            levelCompleted = true;
        if (! kv.second->isComplete())
            allComplete = false;
    }

    allMipmapsComplete = allComplete;
    return levelCompleted;
}

} // namespace deepsvc
