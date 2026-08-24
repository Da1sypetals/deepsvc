#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <map>

#include "../Content/ContentKey.h"
#include "EngineBridge.h"

// 任务管理：提交检测/合成任务、按 ContentKey 追踪任务状态
namespace deepsvc
{

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
    // 终态（succeeded）时的耗时秒数；其他状态为 -1
    double elapsedSeconds = -1.0;
};

class JobManager : private EngineBridge::Listener
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        // 全部在消息线程触发
        virtual void jobStatusChanged (ContentKey key, const JobStatus& status) = 0;
        virtual void detectFinished (ContentKey key, double windowStartSeconds, std::vector<float> f0) = 0;
        // firstVocoder：提交时请求了第一级声码器输出则非空
        virtual void synthFinished (ContentKey key,
                                    std::vector<float> audio,
                                    std::vector<float> firstVocoder,
                                    std::vector<float> f0) = 0;
    };

    explicit JobManager (Listener& listener);
    ~JobManager() override;

    // 消息线程调用，返回任务 ID；引擎初始化失败时返回 0 并广播失败状态
    // windowStartSeconds：pcm 切片在内容内的起始秒数，完成时原样回传用于换算 F0 时间
    uint64_t submitDetect (ContentKey key,
                           std::vector<float> pcm,
                           uint32_t sampleRate,
                           double windowStartSeconds,
                           EngineEstimator estimator);
    uint64_t submitSynth (ContentKey key,
                          std::vector<float> pcm,
                          uint32_t sampleRate,
                          const juce::String& referencePath,
                          const EngineSynthParams& params);
    void cancelJobsFor (ContentKey key);

    JobStatus statusFor (ContentKey key) const;

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
    std::map<uint64_t, ContentKey> jobKeys;
    std::map<uint64_t, double> jobSubmitTimeMs;
    std::map<uint64_t, double> jobDetectWindowStart;
    std::map<ContentKey, std::vector<uint64_t>> activeJobsByKey;
    std::map<ContentKey, JobStatus> latestStatus;

    JUCE_DECLARE_NON_COPYABLE (JobManager)
};

} // namespace deepsvc
