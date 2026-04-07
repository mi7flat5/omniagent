#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace omni::engine {

struct MCPServerConfig {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    std::chrono::seconds init_timeout{10};
    std::chrono::seconds request_timeout{30};
};

}  // namespace omni::engine