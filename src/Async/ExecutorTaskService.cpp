// ExecutorTaskService: OpenRenderGraph's ITaskService over an Executor.
#include <ORGModuleServices/Async/ExecutorTaskService.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <string>
#include <utility>

namespace org::async {

namespace {

// One of the graph's scopes. It counts its tasks, queued or running. Cancel drops those not yet started, and Wait returns
// once every one has left. A task leaves when the executor drops it, run or not.
class CountingScope final : public runtime::ITaskScope {
public:
    void Cancel() noexcept override { cancelled.store(true, std::memory_order_release); }
    void Wait() override {
        for (auto value = pending.load(std::memory_order_acquire); value != 0; value = pending.load(std::memory_order_acquire))
            pending.wait(value);
    }
    void CancelAndWait() override {
        Cancel();
        Wait();
    }
    bool Idle() const noexcept override { return pending.load(std::memory_order_acquire) == 0; }
    bool Cancelled() const noexcept { return cancelled.load(std::memory_order_acquire); }
    void Enter() { pending.fetch_add(1, std::memory_order_acq_rel); }
    void Leave() {
        if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
            pending.notify_all();
    }

private:
    std::atomic_bool cancelled{false};
    std::atomic<std::size_t> pending{0};
};

// A task's hold on its scope, released when the last copy of the task goes.
struct ScopeTicket {
    std::shared_ptr<CountingScope> scope;
    ~ScopeTicket() {
        if (scope)
            scope->Leave();
    }
};

class ExecutorTaskService final : public runtime::ITaskService {
public:
    ExecutorTaskService(Executor& a_executor, ExecutorTaskServiceDesc a_desc) :
        executor(a_executor), parallelClass(a_executor.ParallelClass()), desc(std::move(a_desc)),
        executorScope(executor.CreateScope("ORG")) {}

    void ParallelFor(std::string_view a_name, std::size_t a_count, std::function<void(std::size_t)> a_body) override {
        ParallelForLimited(a_name, a_count, a_count, std::move(a_body));
    }

    void ParallelForLimited(std::string_view a_name, std::size_t a_count, std::size_t a_concurrency,
        std::function<void(std::size_t)> a_body) override {
        if (!a_count)
            return;
        // At most a_concurrency chunks, so at most that many run at once. The caller takes part.
        const std::size_t chunks = std::clamp<std::size_t>(a_concurrency, 1, a_count);
        executor.ParallelFor(a_name, a_count, (a_count + chunks - 1) / chunks, [&a_body](std::size_t a_begin, std::size_t a_end) {
            for (std::size_t i = a_begin; i < a_end; ++i)
                a_body(i);
        });
    }

    std::shared_ptr<runtime::ITaskScope> CreateScope(std::string_view) override { return std::make_shared<CountingScope>(); }

    bool Submit(const std::shared_ptr<runtime::ITaskScope>& a_scope, runtime::TaskPriority a_priority, std::string_view a_name,
        std::function<void()>&& a_task) override {
        return Enqueue(a_scope, a_priority, {}, a_name, std::move(a_task));
    }

    bool ScheduleAfter(const std::shared_ptr<runtime::ITaskScope>& a_scope, std::chrono::steady_clock::duration a_delay,
        runtime::TaskPriority a_priority, std::string_view a_name, std::function<void()>&& a_task) override {
        return Enqueue(a_scope, a_priority, a_delay, a_name, std::move(a_task));
    }

private:
    bool Enqueue(const std::shared_ptr<runtime::ITaskScope>& a_scope, runtime::TaskPriority a_priority,
        std::chrono::steady_clock::duration a_delay, std::string_view a_name, std::function<void()>&& a_task) {
        auto scope = std::dynamic_pointer_cast<CountingScope>(a_scope);
        if ((a_scope && !scope) || (scope && scope->Cancelled()) || !a_task)
            return false;
        if (scope)
            scope->Enter();
        auto ticket = std::make_shared<ScopeTicket>();
        ticket->scope = std::move(scope);
        // The service outlives its tasks, so the task may point at its error callback.
        const auto* onTaskError = &desc.onTaskError;
        Executor::Task task = [ticket, work = std::move(a_task), name = std::string(a_name), onTaskError](const TaskContext&) mutable {
            std::exception_ptr unreported;
            if (!ticket->scope || !ticket->scope->Cancelled()) {
                try {
                    work();
                } catch (const std::exception& e) {
                    if (*onTaskError)
                        (*onTaskError)(name + " threw: " + e.what());
                    else
                        unreported = std::current_exception();
                } catch (...) {
                    if (*onTaskError)
                        (*onTaskError)(name + " threw");
                    else
                        unreported = std::current_exception();
                }
            }
            // The work's captures go before its scope is left.
            work = nullptr;
            ticket.reset();
            if (unreported)
                std::rethrow_exception(unreported);
        };
        const bool critical = a_priority == runtime::TaskPriority::FrameCritical;
        // Every other priority goes to the parallel domain, never to a blocking domain: blocking jobs may wait on locks
        // the graph holds, and a graph task queued behind them could deadlock a thread that waits for that task.
        if (critical && a_delay <= std::chrono::steady_clock::duration::zero())
            return executor.DispatchFrameCritical(executorScope, a_name, std::move(task));
        return executor.DispatchAfter(executorScope, (std::max)(a_delay, std::chrono::steady_clock::duration::zero()),
            parallelClass, a_name, std::move(task));
    }

    Executor& executor;
    const TaskClass parallelClass;
    ExecutorTaskServiceDesc desc;
    Scope executorScope;  // every task's, never waited on
};

} // namespace

std::shared_ptr<runtime::ITaskService> CreateExecutorTaskService(Executor& a_executor, ExecutorTaskServiceDesc a_desc) {
    return std::make_shared<ExecutorTaskService>(a_executor, std::move(a_desc));
}

} // namespace org::async
