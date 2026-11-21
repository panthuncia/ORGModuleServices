#pragma once

#include <ORGModuleServices/Async/Executor.h>
#include <ORGModuleServices/Programs/ComputeProgram.h>
#include <ORGModuleServices/Programs/RasterProgram.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace org {
class GraphHost;
}

namespace org::services {

class ProgramService;

// Resolves a desc's common defines (ProgramEnvironment::commonDefines) where it is queued, never on the pool.
void ResolveProgramDefines(RasterProgram::Desc& desc);
void ResolveProgramDefines(ComputeProgram::Desc& desc);

// A desc's name for the logs.
std::string ProgramLabel(const RasterProgram::Desc& desc);
std::string ProgramLabel(const ComputeProgram::Desc& desc);

// One build on a program service's pool. The pool writes program or error, then done.
struct ProgramSlot {
    std::string label;
    bool gates = true;  // a gating build holds Collect's answer false until it is built
    std::function<std::shared_ptr<const void>()> build;  // runs on the pool
    std::shared_ptr<const void> program;
    std::string error;
    std::atomic<bool> done{ false };
    bool collected = false;  // the owner thread: a Collect or the setup join saw it built
    bool reported = false;   // its failure was reported when it was queued
    const ProgramService* service = nullptr;
};

// Reports that a fixed program was read before it was built, through the slot's service.
void ReportProgramNotBuilt(const ProgramSlot& slot);

// A fixed program queued at setup. Copy it freely. Read it once the setup join or a Collect saw it built: before that, Get
// reports an error and gives null.
template <class P>
class FixedProgram {
public:
    FixedProgram() = default;
    explicit FixedProgram(std::shared_ptr<ProgramSlot> slot) : slot(std::move(slot)) {}

    // The program. Null for an empty handle. Any thread.
    std::shared_ptr<const P> Get() const {
        if (!slot)
            return nullptr;
        if (!slot->done.load(std::memory_order_acquire) || !slot->program) {
            ReportProgramNotBuilt(*slot);
            return nullptr;
        }
        return std::static_pointer_cast<const P>(slot->program);
    }

    // Whether the build is done. Any thread.
    bool Ready() const { return slot && slot->done.load(std::memory_order_acquire) && slot->program; }

    const P* operator->() const { return Get().get(); }
    operator std::shared_ptr<const P>() const { return Get(); }
    // Whether a program was queued.
    explicit operator bool() const { return slot != nullptr; }

private:
    std::shared_ptr<ProgramSlot> slot;
};

// The program service: programs are built here, several at once on an executor. The owner thread never builds a program and
// never waits for one, except in the setup join.
//
// The pool's builds wait for their compiles. When the compiles run in the pool's own domain, the pool takes at most half of
// that domain's workers (org::async::PlanWaitingPool), so a compile always has a thread.
//
// Owner thread: Queue, Collect, Pending, JoinSetup. Post and the slots' reads are for any thread.
class ProgramService {
public:
    struct Desc {
        // Must outlive the service.
        org::async::Executor* executor = nullptr;
        // Where the builds run.
        org::async::TaskClass taskClass = org::async::Executor::Preparation;
        // The domain the builds' compiles run in, which the pool must leave a thread.
        std::uint32_t compileDomain = org::async::Executor::Preparation.domain;
        // Where a single build runs at a time when the compile domain has one worker.
        org::async::TaskClass fallback = org::async::Executor::Streaming;
        // The pool's lanes are named "<laneName> <index>".
        std::string laneName = "programs";
        // The host whose device builds the programs, or null while there is none.
        std::function<const org::GraphHost*()> host;
        // Whether programs can be built now. Empty: whenever there is a host.
        std::function<bool()> available;
        // A failure the call cannot return: a refused build, a failed build at Collect, a program read before it was built,
        // a lane that stopped. After it returns, the call acts as failed.
        std::function<void(std::string_view)> onError;
    };

    explicit ProgramService(Desc desc);
    // Stops the pool.
    ~ProgramService();
    ProgramService(const ProgramService&) = delete;
    ProgramService& operator=(const ProgramService&) = delete;

    // Builds desc on the calling thread, for build functions that run on the pool. Null when it failed or there is no host.
    std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& desc) const;
    std::shared_ptr<const ComputeProgram> Load(const ComputeProgram::Desc& desc) const;

    // Any thread. Queues job on the pool, which runs several jobs at once in no set order. False after Stop. The pool is
    // made on the first post.
    bool Post(std::function<void()> job);

    // Whether programs can be built now.
    bool Available() const;

    // Drops queued builds and waits for running ones. Teardown only, before the device goes.
    void Stop();

    // Owner thread. Posts build as a slot the next Collect checks. A refusal is reported. gates: Collect answers false while
    // it builds. JoinSetup waits for every slot either way.
    std::shared_ptr<ProgramSlot> Queue(std::string label, std::function<std::shared_ptr<const void>()> build, bool gates);

    // Owner thread, at setup. Queues desc's build on the pool, and resolves its common defines here. owner names it in the
    // logs. An empty handle when programs cannot be built.
    FixedProgram<RasterProgram> Queue(std::string_view owner, RasterProgram::Desc desc);
    FixedProgram<ComputeProgram> Queue(std::string_view owner, ComputeProgram::Desc desc);

    // Owner thread. Collects the finished builds and reports the failed ones. True when no gating build is in flight.
    bool Collect();

    // Owner thread. Whether a build is in flight, gating or not.
    bool Pending() const;

    // Owner thread, once at the end of setup, before any frame runs. Waits for every slot in flight and collects them.
    // point names the setup step in the log.
    void JoinSetup(std::string_view point);

    // Reports message through onError.
    void ReportError(std::string_view message) const;

private:
    class Pool;
    Pool& Builds();

    Desc desc;
    std::atomic<Pool*> pool{ nullptr };
    std::unique_ptr<Pool> ownedPool;
    std::mutex poolMutex;
    std::vector<std::shared_ptr<ProgramSlot>> inFlight;  // owner thread: the slots no Collect has seen done
};

} // namespace org::services
