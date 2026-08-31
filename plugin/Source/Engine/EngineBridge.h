#pragma once

#include <juce_core/juce_core.h>

#include <vector>

struct FfiEvent; // native/src/ffi.rs 的 C ABI 事件结构，定义在 EngineBridge.cpp

// 引擎桥接：推理在插件进程内执行，Rust 静态库经 C ABI 直接调用；
// 模型进程级加载一次，全部插件实例复用。
namespace deepsvc
{

enum class EngineEstimator : uint32_t
{
    rmvpe = 0,
    fcpe = 1
};

// 与 native/src/ffi.rs 的 FfiSynthParams 对应；初始值为参数默认值
struct EngineSynthParams
{
    EngineEstimator f0Estimator = EngineEstimator::rmvpe;
    uint32_t diffusionSteps = 16;
    float pitchShift = 12.0f;
    float pitchFineTuneCents = 0.0f;
    float cfgRate = 0.9f;
    float inputGainDb = -2.0f;
    bool keepFirstVocoderOutput = false;

    bool operator== (const EngineSynthParams& rhs) const noexcept
    {
        return f0Estimator == rhs.f0Estimator
            && diffusionSteps == rhs.diffusionSteps
            && pitchShift == rhs.pitchShift
            && pitchFineTuneCents == rhs.pitchFineTuneCents
            && cfgRate == rhs.cfgRate
            && inputGainDb == rhs.inputGainDb
            && keepFirstVocoderOutput == rhs.keepFirstVocoderOutput;
    }
    bool operator!= (const EngineSynthParams& rhs) const noexcept { return ! (*this == rhs); }
};

// 与 native/src/engine.rs 的 JobStateName 对应
enum class EngineJobState : uint32_t
{
    queued = 0,
    loadingModels = 1,
    running = 2,
    succeeded = 3,
    failed = 4,
    cancelled = 5
};

class EngineBridge
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        // 全部在引擎工作线程触发，数据已拷贝；接收方负责切换到目标线程
        virtual void engineJobState (uint64_t jobId,
                                     EngineJobState state,
                                     uint32_t queuePosition,
                                     const juce::String& stage,
                                     double fraction,
                                     const juce::String& error) = 0;
        virtual void engineDetectResult (uint64_t jobId, std::vector<float> f0) = 0;
        virtual void engineSynthResult (uint64_t jobId,
                                        std::vector<float> audio,
                                        std::vector<float> firstVocoder,
                                        std::vector<float> f0) = 0;
    };

    static EngineBridge& getInstance();

    // 幂等；失败返回 false 并填充 error
    bool initialise (juce::String& error);

    void addListener (Listener* listener);
    void removeListener (Listener* listener);

    // 返回 false 表示提交失败（失败事件仍会经监听器广播）
    bool submitDetect (uint64_t jobId,
                       const std::vector<float>& pcm,
                       uint32_t sampleRate,
                       EngineEstimator estimator);
    bool submitSynth (uint64_t jobId,
                      const std::vector<float>& pcm,
                      uint32_t sampleRate,
                      const juce::String& referencePath,
                      const EngineSynthParams& params);
    void cancel (uint64_t jobId);

private:
    EngineBridge();

    void dispatchJobState (uint64_t jobId,
                           EngineJobState state,
                           uint32_t queuePosition,
                           const juce::String& stage,
                           double fraction,
                           const juce::String& error);
    void dispatchDetectResult (uint64_t jobId, const float* data, uint64_t length);
    void dispatchSynthResult (uint64_t jobId,
                              const float* audio,
                              uint64_t audioLength,
                              const float* firstVocoder,
                              uint64_t firstVocoderLength,
                              const float* f0,
                              uint64_t f0Length);

    static void ffiEventCallback (const ::FfiEvent* event, void* userData);

    juce::File modelDirectoryPath;
    uint64_t listenerHandle = 0;
    std::atomic<bool> initialised { false };

    juce::CriticalSection listenerLock;
    juce::Array<Listener*> listeners;

    JUCE_DECLARE_NON_COPYABLE (EngineBridge)
};

} // namespace deepsvc
