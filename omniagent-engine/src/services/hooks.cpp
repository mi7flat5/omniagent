#include "hooks.h"

#include <algorithm>
#include <future>
#include <thread>

namespace omni::engine {

void HookEngine::add(HookRegistration hook) {
    std::lock_guard<std::mutex> lk(mutex_);
    hooks_.push_back(std::move(hook));
}

void HookEngine::remove(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    hooks_.erase(
        std::remove_if(hooks_.begin(), hooks_.end(),
                       [&](const HookRegistration& h) { return h.name == name; }),
        hooks_.end());
}

size_t HookEngine::count() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return hooks_.size();
}

HookResult HookEngine::fire(HookEvent event, const nlohmann::json& context) {
    // Copy matching hooks under lock so we don't hold the lock during execution.
    std::vector<HookRegistration> matching;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& h : hooks_) {
            if (h.event == event) {
                matching.push_back(h);
            }
        }
    }

    HookResult combined;

    for (const HookRegistration& reg : matching) {
        // Use packaged_task + detached thread so the future's destructor does NOT
        // block when we decide to abandon a timed-out hook.
        //
        // std::async with std::launch::async returns a future whose destructor joins
        // the thread — that blocks us for the full hook sleep duration even after
        // wait_for() says "timeout". A packaged_task + detached thread avoids this:
        // the thread keeps running independently and the future becomes detached
        // (its shared state lives until the task completes, but our future handle
        // can be dropped immediately).
        //
        // Lifetime: reg, event, context are stack variables in this loop body.
        // We copy them by value into the lambda to avoid dangling references.
        std::packaged_task<HookResult()> task(
            [handler = reg.handler, ev = event, ctx = context]() -> HookResult {
                try {
                    return handler(ev, ctx);
                } catch (...) {
                    return HookResult{false, "hook threw an exception"};
                }
            });

        std::future<HookResult> fut = task.get_future();

        // Detach the thread: it owns the packaged_task; once the task completes
        // the shared state is updated and the thread exits on its own.
        std::thread worker(std::move(task));
        worker.detach();

        const auto status = fut.wait_for(reg.timeout);
        if (status == std::future_status::timeout) {
            // Timed out — treat as non-blocking, drop the future (does NOT join).
            continue;
        }

        HookResult r = fut.get();
        if (r.should_block) {
            combined.should_block = true;
            combined.message      = r.message;
        }
    }

    return combined;
}

}  // namespace omni::engine
