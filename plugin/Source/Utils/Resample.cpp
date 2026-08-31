#include "Resample.h"

#include <cmath>
#include <cstdlib>

#include <soxr.h>

#include "../DebugLog.h"

namespace deepsvc
{

std::vector<float> resampleMono (const float* input,
                                 size_t inputCount,
                                 double inputRate,
                                 double outputRate)
{
    if (input == nullptr || inputCount == 0)
        std::abort();
    if (!(inputRate > 0.0) || !(outputRate > 0.0))
        std::abort();

    if (inputRate == outputRate)
        return std::vector<float> (input, input + inputCount);

    const auto outputCount = static_cast<size_t> (
        std::ceil (static_cast<double> (inputCount) * outputRate / inputRate));
    if (outputCount == 0)
        std::abort();

    std::vector<float> output (outputCount);
    size_t inputDone = 0;
    size_t outputDone = 0;
    const auto io = soxr_io_spec (SOXR_FLOAT32_I, SOXR_FLOAT32_I);
    const auto quality = soxr_quality_spec (SOXR_VHQ, 0);
    const auto runtime = soxr_runtime_spec (1);
    const soxr_error_t error = soxr_oneshot (inputRate,
                                             outputRate,
                                             1,
                                             input,
                                             inputCount,
                                             &inputDone,
                                             output.data(),
                                             output.size(),
                                             &outputDone,
                                             &io,
                                             &quality,
                                             &runtime);
    if (error != nullptr || outputDone == 0)
    {
        debugLog ("resampleMono failed error="
                  + juce::String (error != nullptr ? error : "empty"));
        std::abort();
    }

    output.resize (outputDone);
    return output;
}

} // namespace deepsvc
