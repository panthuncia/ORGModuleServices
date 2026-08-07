#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <utility>

namespace org::services {

template<class Key, class Hash = std::hash<Key>>
class CompileFlightRegistry {
public:
    bool TryBecomeOwnerOrWait(const Key& key) {
        std::unique_lock lock(mutex_);
        if (active_.insert(key).second) return true;
        condition_.wait(lock, [&] { return !active_.contains(key); });
        return false;
    }
    void Complete(const Key& key) {
        { std::scoped_lock lock(mutex_); active_.erase(key); }
        condition_.notify_all();
    }
    size_t ActiveCount() const noexcept { std::scoped_lock lock(mutex_); return active_.size(); }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_set<Key, Hash> active_;
};

template<class Registry, class Key>
class CompileFlightScope {
public:
    CompileFlightScope(Registry& registry, Key key) : registry_(&registry), key_(std::move(key)) {}
    ~CompileFlightScope() { if (registry_) registry_->Complete(key_); }
    CompileFlightScope(const CompileFlightScope&) = delete;
    CompileFlightScope& operator=(const CompileFlightScope&) = delete;
private:
    Registry* registry_;
    Key key_;
};

}
