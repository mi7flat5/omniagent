#pragma once

#include <omni/types.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace omni::engine {

struct ModelCost {
    double input_cost_per_1k   = 0.0;  // USD per 1K input tokens
    double output_cost_per_1k  = 0.0;  // USD per 1K output tokens
    double cache_cost_per_1k   = 0.0;  // USD per 1K cached tokens
};

struct CostSnapshot {
    Usage  total_usage;
    double total_cost_usd = 0.0;
    std::unordered_map<std::string, Usage>  per_model_usage;
    std::unordered_map<std::string, double> per_model_cost;
};

class CostTracker {
public:
    /// Register cost rates for a model. Local models should use {0,0,0}.
    void set_model_cost(const std::string& model, const ModelCost& cost);

    /// Record usage from a completion call.
    void record(const std::string& model, const Usage& usage);

    /// Get current cost snapshot (thread-safe).
    CostSnapshot snapshot() const;

    /// Reset all tracking.
    void reset();

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ModelCost> model_costs_;
    std::unordered_map<std::string, Usage>     model_usage_;
};

}  // namespace omni::engine
