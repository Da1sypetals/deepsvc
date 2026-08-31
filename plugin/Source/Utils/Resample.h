#pragma once

#include <cstddef>
#include <vector>

namespace deepsvc
{

std::vector<float> resampleMono (const float* input,
                                 size_t inputCount,
                                 double inputRate,
                                 double outputRate);

} // namespace deepsvc
