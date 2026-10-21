#include <ORGModuleServices/PipelineRegistry.h>
#include "CompletionFlight.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace org::services {

namespace {
// A shader file's name as the registry compares it: lower case, forward slashes, relative to the shader directory where the path
// names it ("data/shaders/").
std::string NormalizeShaderPath(std::string_view path) {
    std::string out(path);
    for (auto& c : out) c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    constexpr std::string_view kShaders = "data/shaders/";
    if (const auto at = out.rfind(kShaders); at != std::string::npos) out.erase(0, at + kShaders.size());
    while (out.starts_with("./")) out.erase(0, 2);
    return out;
}
// A changed file (normalized) is a source's when it is that file, or a path ending in it (an absolute path, a mod manager's).
bool SameShaderFile(std::string_view changed, std::string_view source) {
    if (source.empty()) return false;
    if (changed == source) return true;
    return changed.size() > source.size() && changed.ends_with(source) && changed[changed.size() - source.size() - 1] == '/';
}
constexpr size_t kMaxErrors = 8;
}

struct PipelineRegistry::Entry {
    std::weak_ptr<LivePipelineSlot> slot;
    LivePipelineDesc desc;     // sources normalized; immutable once registered
    uint64_t fingerprint{};    // the slot's payload's (mutex_)
    bool stale{};              // built for an earlier SourcesGeneration: in the next reload (mutex_)
    bool inBatch{};            // the running reload's (mutex_)
};

// One reload: its entries and their rebuilds' results, each written once by its done, counted down to zero.
struct PipelineRegistry::Batch {
    struct Result {
        std::atomic<bool> done{ false };
        LiveBuild build;
    };
    std::vector<std::shared_ptr<Entry>> entries;
    std::vector<LiveRebuildRequest> requests;
    std::unique_ptr<Result[]> results;
    std::atomic<uint32_t> remaining{ 0 };
    std::chrono::steady_clock::time_point started;

    void Complete(size_t index, LiveBuild build) {
        auto& result = results[index];
        if (result.done.exchange(true, std::memory_order_acq_rel)) return;  // a second call is ignored
        result.build = std::move(build);
        remaining.fetch_sub(1, std::memory_order_acq_rel);
    }
};

PipelineRegistry::~PipelineRegistry() = default;

void PipelineRegistry::Configure(ServiceSubmit submit) { std::scoped_lock lock(mutex_); submit_ = std::move(submit); }

void PipelineRegistry::AddListener(std::function<void()> listener) {
    if (!listener) return;
    std::scoped_lock lock(mutex_);
    listeners_.push_back(std::move(listener));
}

void PipelineRegistry::PruneLocked() {
    std::erase_if(entries_, [](const std::shared_ptr<Entry>& entry) { return entry->slot.expired() && !entry->inBatch; });
    pruneAt_ = (std::max<size_t>)(64, entries_.size() * 2);
}

std::shared_ptr<LivePipelineSlot> PipelineRegistry::Register(LivePipelineDesc desc, PipelinePayload payload, uint64_t fingerprint, uint64_t builtFor) {
    auto slot = std::make_shared<LivePipelineSlot>(std::move(payload));
    auto entry = std::make_shared<Entry>();
    entry->slot = slot;
    for (auto& source : desc.sources) source = NormalizeShaderPath(source);
    entry->desc = std::move(desc);
    entry->fingerprint = fingerprint;
    std::scoped_lock lock(mutex_);
    if (entries_.size() >= pruneAt_) PruneLocked();
    // Its files were read before the last reload began: the next Service rebuilds it from them as they are now.
    if (builtFor < sourcesGeneration_.load(std::memory_order_acquire)) {
        entry->stale = true;
        asked_.store(true, std::memory_order_release);
    }
    entries_.push_back(std::move(entry));
    return slot;
}

void PipelineRegistry::RequestReload(std::string_view changedFile, bool force) {
    std::scoped_lock lock(mutex_);
    request_.asked = true;
    request_.force = request_.force || force;
    if (changedFile.empty()) request_.all = true;
    else if (!request_.all) {
        auto file = NormalizeShaderPath(changedFile);
        if (std::ranges::find(request_.files, file) == request_.files.end()) request_.files.push_back(std::move(file));
    }
    asked_.store(true, std::memory_order_release);
}

PipelineRegistry::ServiceResult PipelineRegistry::Service() {
    ServiceResult result;
    if (running_ && running_->remaining.load(std::memory_order_acquire) == 0) {
        FinishBatch();
        result.finished = true;
    }
    if (!running_ && asked_.load(std::memory_order_acquire)) {
        StartBatch();
        // Every rebuild may have completed at once (nothing changed, or no entry to rebuild).
        if (running_ && running_->remaining.load(std::memory_order_acquire) == 0) {
            FinishBatch();
            result.finished = true;
        }
    }
    std::scoped_lock lock(mutex_);
    result.staged = !staged_.empty();
    return result;
}

void PipelineRegistry::StartBatch() {
    auto batch = std::make_shared<Batch>();
    batch->started = std::chrono::steady_clock::now();
    bool moved = false;
    ServiceSubmit submit;
    {
        std::scoped_lock lock(mutex_);
        const Request request = std::exchange(request_, {});
        asked_.store(false, std::memory_order_release);
        PruneLocked();
        // A changed file no entry is built from is an include (or unknown): it may be any entry's, so every entry is rebuilt; the
        // unchanged ones come back so (their fingerprints) at a compile cache hit's cost.
        bool all = request.all;
        for (const auto& file : request.files) {
            const bool declared = std::ranges::any_of(entries_, [&](const std::shared_ptr<Entry>& entry) {
                return std::ranges::any_of(entry->desc.sources, [&](const std::string& source) { return SameShaderFile(file, source); });
            });
            all = all || !declared;
        }
        for (const auto& entry : entries_) {
            if (entry->inBatch || entry->slot.expired()) continue;
            const bool picked = request.asked && (all || std::ranges::any_of(request.files, [&](const std::string& file) {
                return std::ranges::any_of(entry->desc.sources, [&](const std::string& source) { return SameShaderFile(file, source); });
            }));
            if (!picked && !entry->stale) continue;
            batch->entries.push_back(entry);
            batch->requests.push_back({ entry->fingerprint, picked && request.force });
            entry->stale = false;
            entry->inBatch = true;
        }
        // The files are read again from here on (a rebuild below reads them after this).
        if (request.asked) {
            sourcesGeneration_.fetch_add(1, std::memory_order_acq_rel);
            moved = true;
        }
        submit = submit_;
    }
    batch->results = std::make_unique<Batch::Result[]>(batch->entries.size());
    batch->remaining.store(static_cast<uint32_t>(batch->entries.size()), std::memory_order_release);
    running_ = batch;
    if (moved) Notify();
    if (batch->entries.empty()) return;
    // One task starts every rebuild in turn; a rebuild only starts its work (compiles), which completes on other threads.
    ServiceTask task = [batch] {
        for (size_t i = 0; i < batch->entries.size(); ++i) {
            const auto& rebuild = batch->entries[i]->desc.rebuild;
            if (!rebuild) {
                batch->Complete(i, LiveBuild{ {}, 0, "no rebuild function" });
                continue;
            }
            try {
                rebuild(batch->requests[i], [batch, i](LiveBuild build) { batch->Complete(i, std::move(build)); });
            } catch (const std::exception& e) {
                batch->Complete(i, LiveBuild{ {}, 0, e.what() });
            } catch (...) {
                batch->Complete(i, LiveBuild{ {}, 0, "the rebuild threw" });
            }
        }
    };
    if (!detail::LaunchServiceTask(submit, std::move(task)))
        for (size_t i = 0; i < batch->entries.size(); ++i) batch->Complete(i, LiveBuild{ {}, 0, "the registry's submitter refused the reload" });
}

void PipelineRegistry::FinishBatch() {
    auto batch = std::exchange(running_, {});
    ReloadOutcome outcome;
    outcome.entries = static_cast<uint32_t>(batch->entries.size());
    outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - batch->started).count();
    std::scoped_lock lock(mutex_);
    outcome.serial = outcome_.serial + 1;
    for (size_t i = 0; i < batch->entries.size(); ++i) {
        auto& entry = *batch->entries[i];
        auto& build = batch->results[i].build;
        entry.inBatch = false;
        if (build.payload) {
            staged_.push_back({ entry.slot, batch->entries[i], std::move(build.payload), build.fingerprint });
            ++outcome.rebuilt;
        } else if (!build.error.empty()) {
            ++outcome.failed;
            if (outcome.errors.size() < kMaxErrors) outcome.errors.push_back(entry.desc.id + ": " + build.error);
        } else {
            ++outcome.unchanged;
        }
    }
    outcome_ = std::move(outcome);
}

uint32_t PipelineRegistry::Publish(uint64_t retirementPoint) {
    std::vector<Staged> staged;
    {
        std::scoped_lock lock(mutex_);
        staged.swap(staged_);
    }
    if (staged.empty()) return 0;
    uint32_t published = 0;
    std::vector<Retired> replaced;
    replaced.reserve(staged.size());
    for (auto& item : staged) {
        const auto slot = item.slot.lock();
        if (!slot) continue;  // released meanwhile: nothing binds it
        replaced.push_back({ retirementPoint, slot->Exchange(std::move(item.payload)) });
        if (const auto entry = item.entry.lock()) {
            std::scoped_lock lock(mutex_);
            entry->fingerprint = item.fingerprint;
        }
        ++published;
    }
    {
        std::scoped_lock lock(retiredMutex_);
        for (auto& retired : replaced) retired_.push_back(std::move(retired));
    }
    if (published) {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        Notify();
    }
    return published;
}

void PipelineRegistry::Retire(uint64_t completedPoint) noexcept {
    std::vector<PipelinePayload> released;  // destroyed outside the lock
    {
        std::scoped_lock lock(retiredMutex_);
        while (!retired_.empty() && retired_.front().point <= completedPoint) {
            released.push_back(std::move(retired_.front().payload));
            retired_.pop_front();
        }
    }
}

void PipelineRegistry::DrainRetired() noexcept {
    std::deque<Retired> retired;
    std::vector<Staged> staged;
    {
        std::scoped_lock lock(retiredMutex_);
        retired.swap(retired_);
    }
    {
        std::scoped_lock lock(mutex_);
        staged.swap(staged_);
    }
}

void PipelineRegistry::Notify() {
    std::vector<std::function<void()>> listeners;
    {
        std::scoped_lock lock(mutex_);
        listeners = listeners_;
    }
    for (const auto& listener : listeners) listener();
}

PipelineRegistry::ReloadOutcome PipelineRegistry::LastReload() const { std::scoped_lock lock(mutex_); return outcome_; }

size_t PipelineRegistry::EntryCount() const { std::scoped_lock lock(mutex_); return entries_.size(); }

}
