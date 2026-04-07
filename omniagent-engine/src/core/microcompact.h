#pragma once

#include <omni/types.h>
#include <vector>

namespace omni::engine {

std::vector<Message> microcompact(
    const std::vector<Message>& messages,
    int preserve_tail    = 4,
    int max_result_chars = 500);

}  // namespace omni::engine
