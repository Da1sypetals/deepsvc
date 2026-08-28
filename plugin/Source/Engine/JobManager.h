#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <map>

#include "EngineBridge.h"

namespace deepsvc
{

struct JobKey
{
    juce::String persistentId;
    int slot = 0;

    bool operator== (const JobKey& rhs) const noexcept
    {
        return persistentId == rhs.persistentId && slot == rhs.slot;
    }
    bool operator< (const JobKey& rhs) const noexcept
    {
        if (persistentId != rhs.persistentId)
            return persistentId < rhs.persistentId;
        return slot < rhs.slot;
    }
};

struct JobStatus
{
    enum class State
    {
        idle,
        queued,
        loadingModels,
        running,
        succeeded,
        failed,
        cancelled
    };

    State state = State::idle;
    juce::String stage;
    double fraction = 0.0;
    uint32_t queuePosition = 0;
    juce::String error;
    double elapsedSeconds = -1.0;
};

class JobManager : private EngineBridge::Listener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void jobStatusChanged (JobKey key, const JobStatus& status) = 0;
        virtual void detectFinished (JobKey key, std::vector<float> f0) = 0;
        virtual void synthFinished (JobKey key,
                                    std::vector<float> audio,
                                    std::vector<float> firstVocoder,
                                    std::vector<float> f0) = 0;
    };

    explicit JobManager (Listener& listener);
    ~JobManager() override;

    uint64_t submitDetect (JobKey key,
                           std::vector<float> pcm,
                           uint32_t sampleRate,
                           EngineEstimator estimator);
    uint64_t submitSynth (JobKey key,
                          std::vector<float> pcm,
                          uint32_t sampleRate,
                          const juce::String& referencePath,
                          const EngineSynthParams& params);
    void cancelJobsFor (JobKey key);

    JobStatus statusFor (JobKey key) const;

private:
    void engineJobState (uint64_t jobId,
                         EngineJobState state,
                         uint32_t queuePosition,
                         const juce::String& stage,
                         double fraction,
                         const juce::String& error) override;
    void engineDetectResult (uint64_t jobId, std::vector<float> f0) override;
    void engineSynthResult (uint64_t jobId,
                            std::vector<float> audio,
                            std::vector<float> firstVocoder,
                            std::vector<float> f0) override;

    void handleJobState (uint64_t jobId,
                         EngineJobState state,
                         uint32_t queuePosition,
                         const juce::String& stage,
                         double fraction,
                         const juce::String& error);
    void failJob (uint64_t jobId, const juce::String& error);

    Listener& listener;
    juce::String initError;

    juce::CriticalSection stateLock;
    std::map<uint64_t, JobKey> jobKeys;
    std::map<uint64_t, double> jobSubmitTimeMs;
    std::map<JobKey, std::vector<uint64_t>> activeJobsByKey;
    std::map<JobKey, JobStatus> latestStatus;

    JUCE_DECLARE_NON_COPYABLE (JobManager)
};

} // namespace deepsvc
