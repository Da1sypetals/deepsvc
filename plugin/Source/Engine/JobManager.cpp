#include "JobManager.h"

#include <juce_events/juce_events.h>

#include <algorithm>

// 对应 OpenTune Source/Inference/F0InferenceService.cpp 的任务提交与状态追踪部分
namespace deepsvc
{

namespace
{

// 任务 ID 进程级唯一：引擎是进程级共享的，多个插件实例的任务不能撞号
uint64_t nextJobId()
{
    static std::atomic<uint64_t> counter { 0 };
    return ++counter;
}

JobStatus::State toJobStatusState (EngineJobState state)
{
    switch (state)
    {
        case EngineJobState::queued:        return JobStatus::State::queued;
        case EngineJobState::loadingModels: return JobStatus::State::loadingModels;
        case EngineJobState::running:       return JobStatus::State::running;
        case EngineJobState::succeeded:     return JobStatus::State::succeeded;
        case EngineJobState::failed:        return JobStatus::State::failed;
        case EngineJobState::cancelled:     return JobStatus::State::cancelled;
    }
    return JobStatus::State::idle;
}

} // namespace

JobManager::JobManager (Listener& listenerRef)
    : listener (listenerRef)
{
    auto& bridge = EngineBridge::getInstance();
    bridge.addListener (this);
    bridge.initialise (initError);
}

JobManager::~JobManager()
{
    EngineBridge::getInstance().removeListener (this);
}

uint64_t JobManager::submitDetect (JobKey key,
                                   std::vector<float> pcm,
                                   uint32_t sampleRate,
                                   EngineEstimator estimator)
{
    const uint64_t jobId = nextJobId();
    {
        const juce::ScopedLock lock (stateLock);
        jobKeys[jobId] = key;
        jobSubmitTimeMs[jobId] = juce::Time::getMillisecondCounterHiRes();
        activeJobsByKey[key].push_back (jobId);
    }

    if (initError.isNotEmpty())
    {
        failJob (jobId, initError);
        return 0;
    }

    EngineBridge::getInstance().submitDetect (jobId, pcm, sampleRate, estimator);
    return jobId;
}

uint64_t JobManager::submitSynth (JobKey key,
                                  std::vector<float> pcm,
                                  uint32_t sampleRate,
                                  const juce::String& referencePath,
                                  const EngineSynthParams& params)
{
    const uint64_t jobId = nextJobId();
    {
        const juce::ScopedLock lock (stateLock);
        jobKeys[jobId] = key;
        jobSubmitTimeMs[jobId] = juce::Time::getMillisecondCounterHiRes();
        activeJobsByKey[key].push_back (jobId);
    }

    if (initError.isNotEmpty())
    {
        failJob (jobId, initError);
        return 0;
    }

    EngineBridge::getInstance().submitSynth (jobId, pcm, sampleRate, referencePath, params);
    return jobId;
}

void JobManager::cancelJobsFor (JobKey key)
{
    std::vector<uint64_t> jobs;
    {
        const juce::ScopedLock lock (stateLock);
        const auto it = activeJobsByKey.find (key);
        if (it == activeJobsByKey.end())
            return;
        jobs = it->second;
    }

    auto& bridge = EngineBridge::getInstance();
    for (const auto jobId : jobs)
        bridge.cancel (jobId);
}

JobStatus JobManager::statusFor (JobKey key) const
{
    const juce::ScopedLock lock (stateLock);
    const auto it = latestStatus.find (key);
    return it != latestStatus.end() ? it->second : JobStatus {};
}

// ---- EngineBridge::Listener（引擎工作线程） ----

void JobManager::engineJobState (uint64_t jobId,
                                 EngineJobState state,
                                 uint32_t queuePosition,
                                 const juce::String& stage,
                                 double fraction,
                                 const juce::String& error)
{
    juce::MessageManager::callAsync ([this, jobId, state, queuePosition, stage, fraction, error]
    {
        handleJobState (jobId, state, queuePosition, stage, fraction, error);
    });
}

void JobManager::engineDetectResult (uint64_t jobId, std::vector<float> f0)
{
    juce::MessageManager::callAsync ([this, jobId, f0 = std::move (f0)]() mutable
    {
        JobKey key;
        {
            const juce::ScopedLock lock (stateLock);
            const auto it = jobKeys.find (jobId);
            if (it == jobKeys.end())
                return;
            key = it->second;
        }
        listener.detectFinished (key, std::move (f0));
    });
}

void JobManager::engineSynthResult (uint64_t jobId,
                                    std::vector<float> audio,
                                    std::vector<float> firstVocoder,
                                    std::vector<float> f0)
{
    juce::MessageManager::callAsync ([this, jobId, audio = std::move (audio),
                                     firstVocoder = std::move (firstVocoder),
                                     f0 = std::move (f0)]() mutable
    {
        JobKey key;
        {
            const juce::ScopedLock lock (stateLock);
            const auto it = jobKeys.find (jobId);
            if (it == jobKeys.end())
                return;
            key = it->second;
        }
        listener.synthFinished (key, std::move (audio), std::move (firstVocoder), std::move (f0));
    });
}

// ---- 消息线程 ----

void JobManager::handleJobState (uint64_t jobId,
                                 EngineJobState engineState,
                                 uint32_t queuePosition,
                                 const juce::String& stage,
                                 double fraction,
                                 const juce::String& error)
{
    JobKey key;
    JobStatus status;
    {
        const juce::ScopedLock lock (stateLock);
        const auto it = jobKeys.find (jobId);
        if (it == jobKeys.end())
            return;
        key = it->second;

        status.state = toJobStatusState (engineState);
        status.stage = stage;
        status.fraction = fraction;
        status.queuePosition = queuePosition;
        status.error = error;

        if (engineState == EngineJobState::succeeded)
            if (const auto submitIt = jobSubmitTimeMs.find (jobId); submitIt != jobSubmitTimeMs.end())
                status.elapsedSeconds = (juce::Time::getMillisecondCounterHiRes() - submitIt->second) / 1000.0;

        latestStatus[key] = status;

        if (engineState == EngineJobState::succeeded
            || engineState == EngineJobState::failed
            || engineState == EngineJobState::cancelled)
        {
            jobKeys.erase (jobId);
            jobSubmitTimeMs.erase (jobId);
            auto& active = activeJobsByKey[key];
            active.erase (std::remove (active.begin(), active.end(), jobId), active.end());
            if (active.empty())
                activeJobsByKey.erase (key);
        }
    }

    listener.jobStatusChanged (key, status);
}

void JobManager::failJob (uint64_t jobId, const juce::String& error)
{
    handleJobState (jobId, EngineJobState::failed, 0, {}, -1.0, error);
}

} // namespace deepsvc
