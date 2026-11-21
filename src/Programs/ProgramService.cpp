#include <ORGModuleServices/Programs/ProgramService.h>

#include <ORGModuleServices/Async/Lanes.h>
#include <ORGModuleServices/Programs/ProgramBuild.h>

#include <OpenRenderGraph/GraphHost.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <exception>
#include <format>
#include <stdexcept>

namespace org::services {

namespace {

namespace build = program_build;

std::string DefinesLabel(const ProgramDefines& defines) {
    std::string text;
    for (const auto& [define, value] : defines)
        text += std::format("{}{}{}{}", text.empty() ? "" : " ", build::Narrow(define), value.empty() ? "" : "=", build::Narrow(value));
    return text;
}

// On the pool. Writes the result, then done, and wakes the setup join.
void RunSlot(ProgramSlot& slot) {
    try {
        slot.program = slot.build();
        if (!slot.program)
            slot.error = "its build returned nothing (the error logged above has the compiler's messages)";
    } catch (const std::exception& e) {
        slot.error = e.what();
    }
    slot.build = {};
    slot.done.store(true, std::memory_order_release);
    slot.done.notify_all();
}

} // namespace

void ResolveProgramDefines(RasterProgram::Desc& desc) {
    if (!desc.commonDefines)
        return;
    if (desc.vertexStageDefines.empty())
        desc.vertexStageDefines = build::WithCommonDefines(desc.defines, L"vs_6_6");
    if (desc.pixelStageDefines.empty())
        desc.pixelStageDefines = build::WithCommonDefines(desc.defines, L"ps_6_6");
}

void ResolveProgramDefines(ComputeProgram::Desc& desc) {
    if (!desc.commonDefines)
        return;
    desc.defines = build::WithCommonDefines(desc.defines, L"cs_6_6");
    desc.commonDefines = false;
}

std::string ProgramLabel(const RasterProgram::Desc& desc) {
    return std::format("{} + {} ({}) [{}]", desc.vertexSource, desc.pixelSource, build::Narrow(desc.pixelEntry), DefinesLabel(desc.defines));
}

std::string ProgramLabel(const ComputeProgram::Desc& desc) {
    return std::format("{} ({}) [{}]", desc.source, build::Narrow(desc.entry), DefinesLabel(desc.defines));
}

void ReportProgramNotBuilt(const ProgramSlot& slot) {
    const auto message = std::format("{} was read before its build was done; it is queued at setup and read only after the setup join or a revision start",
        slot.label);
    if (slot.service)
        slot.service->ReportError(message);
    else
        build::Log(ProgramLogLevel::Error, message);
}

// A few lanes that take jobs from one queue, so several builds run at once.
//
// A build waits for its compiles. PlanWaitingPool sizes the pool from the compile domain, so the compiles always keep a thread.
class ProgramService::Pool {
public:
    explicit Pool(const ProgramService& service) {
        auto& executor = *service.desc.executor;
        const auto plan = org::async::PlanWaitingPool(executor, service.desc.taskClass, service.desc.compileDomain, service.desc.fallback);
        active.assign(plan.lanes, 0);
        for (unsigned i = 0; i < plan.lanes; ++i) {
            auto name = std::format("{} {}", service.desc.laneName, i);
            org::async::LaneDesc lane{ name, plan.taskClass, org::async::TaskDispatch::Cpu,
                [&service, name] { service.ReportError(std::format("the {} lane stopped: its task was refused or its work threw", name)); } };
            lanes.push_back(std::make_unique<org::async::Lane>(executor, std::move(lane), [this, i] { Drain(i); }));
        }
        build::Log(ProgramLogLevel::Info, std::format("programs: {} builds run at once, on the {} threads", plan.lanes, plan.shared ? "compile domain's" : "fallback"));
    }

    // Any thread. Queues job and wakes an idle lane, if any. False once stopped.
    bool Post(std::function<void()> job) {
        std::size_t lane = 0;
        {
            const std::lock_guard lock(mutex);
            if (stopping)
                return false;
            queue.push_back(std::move(job));
            // Every active lane drains the queue before it goes idle, so with none idle the job is taken anyway.
            while (lane < active.size() && active[lane])
                ++lane;
            if (lane == active.size())
                return true;
            active[lane] = 1;
        }
        return lanes[lane]->Wake();
    }

    // Teardown only. Drops the queued jobs and waits for the running ones.
    void Stop() {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
            queue.clear();
        }
        for (auto& lane : lanes)
            lane->Stop();
    }

private:
    void Drain(std::size_t index) {
        for (;;) {
            std::function<void()> job;
            {
                const std::lock_guard lock(mutex);
                if (stopping || queue.empty()) {
                    active[index] = 0;
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }
            job();
        }
    }

    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    std::vector<char> active;  // a lane was woken and has not found the queue empty since
    bool stopping = false;
    std::vector<std::unique_ptr<org::async::Lane>> lanes;
};

ProgramService::ProgramService(Desc a_desc) : desc(std::move(a_desc)) {
    if (!desc.executor)
        throw std::invalid_argument("ProgramService needs an executor");
}

ProgramService::~ProgramService() { Stop(); }

ProgramService::Pool& ProgramService::Builds() {
    if (auto* made = pool.load(std::memory_order_acquire))
        return *made;
    const std::lock_guard lock(poolMutex);
    if (!ownedPool) {
        ownedPool = std::make_unique<Pool>(*this);
        pool.store(ownedPool.get(), std::memory_order_release);
    }
    return *ownedPool;
}

std::shared_ptr<const RasterProgram> ProgramService::Load(const RasterProgram::Desc& program) const {
    const auto* host = desc.host ? desc.host() : nullptr;
    return host ? RasterProgram::Load(host->Device(), program) : nullptr;
}

std::shared_ptr<const ComputeProgram> ProgramService::Load(const ComputeProgram::Desc& program) const {
    const auto* host = desc.host ? desc.host() : nullptr;
    return host ? ComputeProgram::Load(host->Device(), program) : nullptr;
}

bool ProgramService::Post(std::function<void()> job) { return Builds().Post(std::move(job)); }

bool ProgramService::Available() const {
    if (desc.available)
        return desc.available();
    return desc.host && desc.host();
}

void ProgramService::Stop() {
    // A pool never made has nothing to stop.
    if (auto* made = pool.load(std::memory_order_acquire))
        made->Stop();
}

void ProgramService::ReportError(std::string_view message) const {
    if (desc.onError)
        desc.onError(message);
    else
        build::Log(ProgramLogLevel::Error, message);
}

std::shared_ptr<ProgramSlot> ProgramService::Queue(std::string label, std::function<std::shared_ptr<const void>()> buildFunction, bool gates) {
    auto slot = std::make_shared<ProgramSlot>();
    slot->label = std::move(label);
    slot->gates = gates;
    slot->build = std::move(buildFunction);
    slot->service = this;
    inFlight.push_back(slot);
    if (!Post([slot] { RunSlot(*slot); })) {
        slot->error = "the programs pool refused it";
        slot->reported = true;
        slot->build = {};
        slot->done.store(true, std::memory_order_release);
        slot->done.notify_all();
        ReportError(std::format("the programs pool refused the build of {}", slot->label));
    }
    return slot;
}

namespace {

template <class Desc>
std::shared_ptr<ProgramSlot> QueueDesc(ProgramService& service, std::string_view owner, Desc desc) {
    if (!service.Available())
        return nullptr;
    ResolveProgramDefines(desc);
    auto label = std::format("{}: {}", owner, ProgramLabel(desc));
    return service.Queue(std::move(label), [&service, desc = std::move(desc)]() -> std::shared_ptr<const void> { return service.Load(desc); }, true);
}

} // namespace

FixedProgram<RasterProgram> ProgramService::Queue(std::string_view owner, RasterProgram::Desc program) {
    return FixedProgram<RasterProgram>(QueueDesc(*this, owner, std::move(program)));
}

FixedProgram<ComputeProgram> ProgramService::Queue(std::string_view owner, ComputeProgram::Desc program) {
    return FixedProgram<ComputeProgram>(QueueDesc(*this, owner, std::move(program)));
}

bool ProgramService::Collect() {
    std::erase_if(inFlight, [this](const std::shared_ptr<ProgramSlot>& slot) {
        if (!slot->done.load(std::memory_order_acquire))
            return false;
        if (!slot->error.empty()) {
            if (!slot->reported)
                ReportError(std::format("{} could not be built: {}", slot->label, slot->error));
            return true;
        }
        slot->collected = true;
        return true;
    });
    return std::ranges::none_of(inFlight, [](const std::shared_ptr<ProgramSlot>& slot) { return slot->gates; });
}

bool ProgramService::Pending() const { return !inFlight.empty(); }

void ProgramService::JoinSetup(std::string_view point) {
    const auto count = inFlight.size();
    if (!count)
        return;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& slot : inFlight)
        slot->done.wait(false, std::memory_order_acquire);
    (void)Collect();
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    build::Log(ProgramLogLevel::Info, std::format("programs: the setup batch of {} programs is built at {}; {:.0f} ms waited", count, point, ms));
}

} // namespace org::services
