#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "../Content/ContentKey.h"

// 对应 OpenTune Source/Standalone/UI/WaveformMipmap.h
namespace deepsvc
{

struct PeakSample
{
    int8_t min;
    int8_t max;

    PeakSample() noexcept : min (0), max (0) {}

    void setRange (float minVal, float maxVal) noexcept
    {
        min = static_cast<int8_t> (juce::jlimit (-127, 127, juce::roundToInt (minVal * 127.0f)));
        max = static_cast<int8_t> (juce::jlimit (-127, 127, juce::roundToInt (maxVal * 127.0f)));
    }

    float getMin() const noexcept { return min / 127.0f; }
    float getMax() const noexcept { return max / 127.0f; }
    bool isZero() const noexcept { return min == 0 && max == 0; }
};

class WaveformMipmap
{
public:
    static constexpr int kNumLevels = 6;
    static constexpr int kBaseSampleRate = 44100;

    static constexpr int kSamplesPerPeak[kNumLevels] = {
        32, 128, 512, 2048, 8192, 32768
    };

    struct Level
    {
        std::vector<PeakSample> peaks;
        int64_t numSamplesCovered = 0;
        bool complete = false;
        int64_t buildProgress = 0;
    };

    WaveformMipmap() = default;

    void setAudioSource (std::shared_ptr<const juce::AudioBuffer<float>> buffer);
    bool hasSource() const noexcept { return audioBuffer != nullptr; }
    bool isSourceChanged (std::shared_ptr<const juce::AudioBuffer<float>> buffer) const noexcept;

    int64_t getNumSamples() const noexcept { return numSamples; }

    const Level& getLevel (int level) const { return levels[level]; }

    bool buildIncremental (double timeBudgetMs);
    bool isComplete() const noexcept;
    float getBuildProgress() const noexcept;

    // 返回适合当前缩放的 complete 非空 level 索引；无可用 level 时返回 -1
    int selectBestLevelIndex (double pixelsPerSecond) const;

    void clear();

private:
    std::shared_ptr<const juce::AudioBuffer<float>> audioBuffer;
    int64_t numSamples = 0;
    int numChannels = 0;
    Level levels[kNumLevels];

    void initializeLevels();
    bool buildLevelSlice (int level, double timeBudgetMs);
};

class WaveformMipmapCache
{
public:
    void setAudioSource (ContentKey contentKey, std::shared_ptr<const juce::AudioBuffer<float>> buffer);
    void remove (ContentKey contentKey);
    void prune (const std::set<ContentKey>& alive);
    void clear();

    bool buildIncremental (double timeBudgetMs);
    bool isComplete() const noexcept { return allMipmapsComplete; }

    const WaveformMipmap* get (ContentKey contentKey) const
    {
        auto it = caches.find (contentKey);
        return it != caches.end() ? it->second.get() : nullptr;
    }

private:
    WaveformMipmap& getOrCreate (ContentKey contentKey);
    std::map<ContentKey, std::unique_ptr<WaveformMipmap>> caches;
    bool allMipmapsComplete = true;
};

} // namespace deepsvc
