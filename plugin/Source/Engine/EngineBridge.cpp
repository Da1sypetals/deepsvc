#include "EngineBridge.h"

#include <dlfcn.h>

// 对应 OpenTune Source/Inference/ProcessF0Runtime.cpp 的进程级共享 session 模式

// ---- native/src/ffi.rs 的 C ABI ----

extern "C"
{

struct FfiSynthParams
{
    uint32_t f0Estimator;
    uint32_t diffusionSteps;
    float pitchShift;
    float cfgRate;
    float inputGainDb;
    bool keepFirstVocoderOutput;
};

struct FfiEvent
{
    uint64_t jobId;
    uint32_t kind;
    uint32_t state;
    uint32_t queuePosition;
    double fraction;
    const char* stage;
    const char* error;
    const float* f0;
    uint64_t f0Len;
    const float* audio;
    uint64_t audioLen;
    const float* firstVocoder;
    uint64_t firstVocoderLen;
};

char* deepsvc_engine_set_model_dir (const char* path);
void deepsvc_engine_free_string (char* text);
uint64_t deepsvc_engine_listener_add (void (*callback) (const FfiEvent*, void*), void* userData);
void deepsvc_engine_listener_remove (uint64_t handle);
int deepsvc_engine_submit_detect (uint64_t jobId,
                                  const float* pcm,
                                  uint64_t pcmLen,
                                  uint32_t sampleRate,
                                  uint32_t estimator);
int deepsvc_engine_submit_synth (uint64_t jobId,
                                 const float* pcm,
                                 uint64_t pcmLen,
                                 uint32_t sampleRate,
                                 const char* referencePath,
                                 FfiSynthParams params);
void deepsvc_engine_cancel (uint64_t jobId);

} // extern "C"

namespace deepsvc
{

namespace
{

constexpr uint32_t kEventJobState = 0;
constexpr uint32_t kEventDetectResult = 1;
constexpr uint32_t kEventSynthResult = 2;

// 用 dladdr 定位当前插件二进制，向上找到 bundle 的 Contents/Resources
juce::File pluginResourcesDirectory()
{
    Dl_info info {};
    if (dladdr (reinterpret_cast<const void*> (&pluginResourcesDirectory), &info) == 0
        || info.dli_fname == nullptr)
        return {};

    auto file = juce::File (juce::String::fromUTF8 (info.dli_fname));
    // 二进制位于 <bundle>/Contents/MacOS/<name>
    return file.getParentDirectory().getParentDirectory().getChildFile ("Resources");
}

juce::File resolveModelDirectory()
{
    // 测试可经环境变量直接指定模型目录
    const auto overridden = juce::SystemStats::getEnvironmentVariable ("DEEPSVC_MODELS_DIR", {});
    if (overridden.isNotEmpty())
        return juce::File (overridden);
    return pluginResourcesDirectory().getChildFile ("Models");
}

std::vector<float> copySamples (const float* data, uint64_t length)
{
    if (data == nullptr || length == 0)
        return {};
    return std::vector<float> (data, data + length);
}

} // namespace

EngineBridge& EngineBridge::getInstance()
{
    static EngineBridge instance;
    return instance;
}

EngineBridge::EngineBridge()
{
    modelDirectoryPath = resolveModelDirectory();
    listenerHandle = deepsvc_engine_listener_add (&EngineBridge::ffiEventCallback, this);
}

bool EngineBridge::initialise (juce::String& error)
{
    if (initialised.load())
        return true;

    if (! modelDirectoryPath.isDirectory())
    {
        error = juce::String (u8"模型目录不存在: ") + modelDirectoryPath.getFullPathName();
        return false;
    }

    char* result = deepsvc_engine_set_model_dir (modelDirectoryPath.getFullPathName().toRawUTF8());
    if (result != nullptr)
    {
        error = juce::String::fromUTF8 (result);
        deepsvc_engine_free_string (result);
        return false;
    }

    initialised.store (true);
    return true;
}

void EngineBridge::addListener (Listener* listener)
{
    const juce::ScopedLock lock (listenerLock);
    listeners.addIfNotAlreadyThere (listener);
}

void EngineBridge::removeListener (Listener* listener)
{
    const juce::ScopedLock lock (listenerLock);
    listeners.removeAllInstancesOf (listener);
}

bool EngineBridge::submitDetect (uint64_t jobId,
                                 const std::vector<float>& pcm,
                                 uint32_t sampleRate,
                                 EngineEstimator estimator)
{
    return deepsvc_engine_submit_detect (jobId,
                                         pcm.data(),
                                         static_cast<uint64_t> (pcm.size()),
                                         sampleRate,
                                         static_cast<uint32_t> (estimator)) == 0;
}

bool EngineBridge::submitSynth (uint64_t jobId,
                                const std::vector<float>& pcm,
                                uint32_t sampleRate,
                                const juce::String& referencePath,
                                const EngineSynthParams& params)
{
    const FfiSynthParams ffiParams {
        static_cast<uint32_t> (params.f0Estimator),
        params.diffusionSteps,
        params.pitchShift,
        params.cfgRate,
        params.inputGainDb,
        params.keepFirstVocoderOutput
    };
    return deepsvc_engine_submit_synth (jobId,
                                        pcm.data(),
                                        static_cast<uint64_t> (pcm.size()),
                                        sampleRate,
                                        referencePath.toRawUTF8(),
                                        ffiParams) == 0;
}

void EngineBridge::cancel (uint64_t jobId)
{
    deepsvc_engine_cancel (jobId);
}

void EngineBridge::ffiEventCallback (const FfiEvent* eventPtr, void* userData)
{
    auto& bridge = *static_cast<EngineBridge*> (userData);
    const auto& event = *eventPtr;

    switch (event.kind)
    {
        case kEventJobState:
            bridge.dispatchJobState (event.jobId,
                                     static_cast<EngineJobState> (event.state),
                                     event.queuePosition,
                                     event.stage != nullptr ? juce::String::fromUTF8 (event.stage) : juce::String(),
                                     event.fraction,
                                     event.error != nullptr ? juce::String::fromUTF8 (event.error) : juce::String());
            break;
        case kEventDetectResult:
            bridge.dispatchDetectResult (event.jobId, event.f0, event.f0Len);
            break;
        case kEventSynthResult:
            bridge.dispatchSynthResult (event.jobId,
                                        event.audio,
                                        event.audioLen,
                                        event.firstVocoder,
                                        event.firstVocoderLen,
                                        event.f0,
                                        event.f0Len);
            break;
        default:
            break;
    }
}

void EngineBridge::dispatchJobState (uint64_t jobId,
                                     EngineJobState state,
                                     uint32_t queuePosition,
                                     const juce::String& stage,
                                     double fraction,
                                     const juce::String& error)
{
    const juce::ScopedLock lock (listenerLock);
    for (auto* listener : listeners)
        listener->engineJobState (jobId, state, queuePosition, stage, fraction, error);
}

void EngineBridge::dispatchDetectResult (uint64_t jobId, const float* data, uint64_t length)
{
    auto f0 = copySamples (data, length);
    const juce::ScopedLock lock (listenerLock);
    for (auto* listener : listeners)
        listener->engineDetectResult (jobId, f0);
}

void EngineBridge::dispatchSynthResult (uint64_t jobId,
                                        const float* audio,
                                        uint64_t audioLength,
                                        const float* firstVocoder,
                                        uint64_t firstVocoderLength,
                                        const float* f0,
                                        uint64_t f0Length)
{
    auto audioCopy = copySamples (audio, audioLength);
    auto firstVocoderCopy = copySamples (firstVocoder, firstVocoderLength);
    auto f0Copy = copySamples (f0, f0Length);
    const juce::ScopedLock lock (listenerLock);
    for (auto* listener : listeners)
        listener->engineSynthResult (jobId, audioCopy, firstVocoderCopy, f0Copy);
}

} // namespace deepsvc
