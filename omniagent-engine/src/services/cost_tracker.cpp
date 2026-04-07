#include "cost_tracker.h"

namespace omni::engine {

void CostTracker::set_model_cost(const std::string& model, const ModelCost& cost) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_costs_[model] = cost;
}

void CostTracker::record(const std::string& model, const Usage& usage) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_usage_[model] += usage;
}

CostSnapshot CostTracker::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);

    CostSnapshot snap;
    snap.per_model_usage = model_usage_;

    for (const auto& [model, usage] : model_usage_) {
        snap.total_usage += usage;

        double cost = 0.0;
        auto it = model_costs_.find(model);
        if (it != model_costs_.end()) {
            const ModelCost& mc = it->second;
            cost += (usage.input_tokens      / 1000.0) * mc.input_cost_per_1k;
            cost += (usage.output_tokens     / 1000.0) * mc.output_cost_per_1k;
            cost += (usage.cache_read_tokens / 1000.0) * mc.cache_cost_per_1k;
        }

        snap.per_model_cost[model] = cost;
        snap.total_cost_usd += cost;
    }

    return snap;
}

void CostTracker::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    model_usage_.clear();
}

}  // namespace omni::engine
