#pragma once

#include <ORGModuleServices/ServiceExecutor.h>

#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace org::services::detail {

// One in-flight request: its future and the completion callbacks of every requester that joined it.
//
// Completion makes the artifact available through the future first, then marks the flight done and takes the
// callbacks, both under the flight's own mutex, then invokes them outside it on the completing thread. A requester
// that attaches before the flight is done is queued and invoked by the completion; one that attaches after is invoked
// at once on its own thread. The done flag and the callback list share one lock, so no attach is ever lost.
template<class Artifact>
class CompletionFlight {
public:
    using Callback = std::function<void(const Artifact&)>;

    CompletionFlight() : future_(promise_.get_future().share()) {}
    CompletionFlight(const CompletionFlight&) = delete;
    CompletionFlight& operator=(const CompletionFlight&) = delete;

    const std::shared_future<Artifact>& Future() const noexcept { return future_; }

    void Attach(Callback callback) {
        if (!callback) return;
        {
            std::scoped_lock lock(mutex_);
            if (!done_) { callbacks_.push_back(std::move(callback)); return; }
        }
        callback(Result());
    }

    // Exactly one of Complete or Fail, once.
    void Complete(Artifact value) {
        promise_.set_value(std::move(value));
        Finish();
    }
    // The future rethrows error; callbacks receive delivered (an artifact describing the failure).
    void Fail(std::exception_ptr error, Artifact delivered) {
        failure_ = std::move(delivered);
        failed_ = true;
        promise_.set_exception(std::move(error));
        Finish();
    }

    static std::shared_future<Artifact> Ready(Artifact value) {
        std::promise<Artifact> promise;
        promise.set_value(std::move(value));
        return promise.get_future().share();
    }

private:
    // Immutable once done_ is observed (failure_/failed_ are written before done_, under no lock but before the
    // releasing unlock that publishes done_).
    const Artifact& Result() const { return failed_ ? failure_ : future_.get(); }

    void Finish() {
        std::vector<Callback> callbacks;
        {
            std::scoped_lock lock(mutex_);
            done_ = true;
            callbacks.swap(callbacks_);
        }
        for (auto& callback : callbacks) callback(Result());
    }

    std::promise<Artifact> promise_;
    std::shared_future<Artifact> future_;
    std::mutex mutex_;
    bool done_{};
    bool failed_{};
    Artifact failure_{};
    std::vector<Callback> callbacks_;
};

// Runs task through submit, or on a new detached thread when submit is empty. False: the task will never run.
// A throwing submitter is treated as a refusal, so it must not have accepted the task before throwing.
inline bool LaunchServiceTask(const ServiceSubmit& submit, ServiceTask task) {
    try {
        if (!submit) {
            std::thread(std::move(task)).detach();
            return true;
        }
        return submit(std::move(task));
    } catch (...) {
        return false;
    }
}

}
