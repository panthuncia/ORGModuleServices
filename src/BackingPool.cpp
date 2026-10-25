#include <ORGModuleServices/BackingPool.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <utility>
#include <vector>

#include "BackingPoolBinding.h"

namespace org::services {

namespace detail {

// The pool's lock-free side, shared with its backings so a lease released after the pool can still return (or drop) its backing.
// - returned: the Treiber stack of backings whose last lease went (Backing::m_nextReturned links it). Pushed from any thread,
//   drained only by the acquirer. A backing is pushed at most once per lease cycle: only the acquirer leases it again, and only
//   after the drain that took it in.
// - waiters: availability subscriptions (a Treiber stack), run by whichever thread raises an event.
// - events: counts what may have made a backing reusable (a return, an unpin, the clock reaching a free backing's safeAt).
struct BackingHome {
    struct Waiter {
        std::uint64_t identity = 0;
        std::function<void(std::uint64_t)> wake;
        Waiter* next = nullptr;
    };

    std::atomic<Backing*> returned{nullptr};
    std::atomic<bool> closed{false};  // the pool went: returns only drop their backing
    std::atomic<Waiter*> waiters{nullptr};
    std::atomic<std::uint64_t> events{0};
    std::atomic<std::uint64_t> scanEvents{0};  // events when the last Acquire began looking
    std::atomic<std::uint64_t> armedAt{0};     // the clock point an availability wake is subscribed for (0: none)

    std::atomic<std::uint64_t> live{0}, free{0}, bytes{0};
    std::atomic<std::uint64_t> allocated{0}, reused{0}, released{0}, exhausted{0};

    std::string name;
    std::function<void(BackingPoolEvent, std::string_view, std::uint64_t)> telemetry;

    BackingHome() = default;
    BackingHome(const BackingHome&) = delete;
    BackingHome& operator=(const BackingHome&) = delete;
    ~BackingHome() {
        for (auto* waiter = waiters.exchange(nullptr, std::memory_order_acquire); waiter;) {
            auto* next = waiter->next;
            delete waiter;
            waiter = next;
        }
    }

    void Emit(BackingPoolEvent event, std::uint64_t value) const noexcept {
        if (!telemetry) return;
        try {
            telemetry(event, name, value);
        } catch (...) {
        }
    }

    void Push(Waiter* waiter) noexcept {
        waiter->next = waiters.load(std::memory_order_relaxed);
        while (!waiters.compare_exchange_weak(waiter->next, waiter, std::memory_order_release, std::memory_order_relaxed)) {}
    }

    // Something may have become reusable: every waiter subscribed so far runs once, on this thread.
    void Event() noexcept {
        events.fetch_add(1, std::memory_order_acq_rel);
        auto* list = waiters.exchange(nullptr, std::memory_order_acq_rel);
        std::uint64_t woken = 0;
        while (list) {
            auto* next = list->next;
            try {
                list->wake(list->identity);
            } catch (...) {
            }
            delete list;
            ++woken;
            list = next;
        }
        if (woken) Emit(BackingPoolEvent::Woken, woken);
    }
};

namespace {

// A binding consumer's pin: the backing is not reused while it lives (lock-free against TryClaim, BasicRenderer's protocol).
std::shared_ptr<const void> Pin(const std::shared_ptr<Backing>& backing, std::shared_ptr<BackingHome> home) {
    struct Hold {
        std::weak_ptr<Backing> backing;
        std::shared_ptr<BackingHome> home;
        Hold(const std::shared_ptr<Backing>& value, std::shared_ptr<BackingHome> owner) : backing(value), home(std::move(owner)) {
            value->bindingConsumers.fetch_add(1, std::memory_order_seq_cst);
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        ~Hold() {
            if (const auto value = backing.lock())
                if (value->bindingConsumers.fetch_sub(1, std::memory_order_acq_rel) == 1 && home) home->Event();
        }
    };
    auto hold = std::make_shared<const Hold>(backing, std::move(home));
    // The pool claims a backing (retired = true, then reads bindingConsumers); this pins in the opposite order, so one side
    // always sees the other.
    if (backing->retired.load(std::memory_order_seq_cst)) {
        hold.reset();
        throw std::invalid_argument("BackingPool: cannot pin a backing the pool claimed");
    }
    return hold;
}

// Marks a backing as leaving circulation; fails when a binding consumer pinned it meanwhile (the caller keeps it).
bool TryClaim(Backing& backing) noexcept {
    backing.retired.store(true, std::memory_order_seq_cst);
    if (backing.bindingConsumers.load(std::memory_order_seq_cst) != 0u) {
        backing.retired.store(false, std::memory_order_seq_cst);
        return false;
    }
    return true;
}

bool Reusable(const Backing& backing, std::uint64_t completed) noexcept {
    return backing.leases.load(std::memory_order_acquire) == 0 && completed >= backing.safeAt.load(std::memory_order_acquire) &&
        backing.readbackPins.load(std::memory_order_acquire) == 0 && backing.bindingConsumers.load(std::memory_order_acquire) == 0;
}

} // namespace

} // namespace detail

// RetirementClock -------------------------------------------------------------------------------------------------------------

struct RetirementClock::Waiter {
    std::uint64_t point = 0;
    Wake wake;
    Waiter* next = nullptr;
};

RetirementClock::~RetirementClock() {
    // Waiters not reached are dropped unrun (their pools went with the host).
    for (auto* waiter = m_waiters.exchange(nullptr, std::memory_order_acquire); waiter;) {
        auto* next = waiter->next;
        delete waiter;
        waiter = next;
    }
}

void RetirementClock::Advance(std::uint64_t completed) noexcept {
    auto current = m_completed.load(std::memory_order_relaxed);
    while (current < completed && !m_completed.compare_exchange_weak(current, completed, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    RunReached();
}

void RetirementClock::Subscribe(std::uint64_t point, Wake wake) {
    if (!wake) return;
    if (Reached(point)) {
        wake(Completed());
        return;
    }
    auto* waiter = new Waiter{point, std::move(wake), nullptr};
    m_waiting.fetch_add(1, std::memory_order_relaxed);
    waiter->next = m_waiters.load(std::memory_order_relaxed);
    while (!m_waiters.compare_exchange_weak(waiter->next, waiter, std::memory_order_release, std::memory_order_relaxed)) {}
    // An Advance between the check and the push may have missed it.
    if (Reached(point)) RunReached();
}

void RetirementClock::RunReached() noexcept {
    for (;;) {
        // Each waiter is owned by the one thread that took it off the stack: it runs once.
        Waiter* list = m_waiters.exchange(nullptr, std::memory_order_acq_rel);
        if (!list) return;
        const auto completed = Completed();
        Waiter* kept = nullptr;
        Waiter* keptTail = nullptr;
        std::uint64_t lowestKept = UINT64_MAX;
        while (list) {
            auto* next = list->next;
            if (list->point <= completed) {
                m_waiting.fetch_sub(1, std::memory_order_relaxed);
                try {
                    list->wake(completed);
                } catch (...) {
                }
                delete list;
            } else {
                list->next = kept;
                kept = list;
                if (!keptTail) keptTail = list;
                if (list->point < lowestKept) lowestKept = list->point;
            }
            list = next;
        }
        if (!kept) return;
        keptTail->next = m_waiters.load(std::memory_order_relaxed);
        while (!m_waiters.compare_exchange_weak(keptTail->next, kept, std::memory_order_release, std::memory_order_relaxed)) {}
        // A concurrent Advance may have raised the point after `completed` was read and found the stack empty.
        if (lowestKept > Completed()) return;
    }
}

// BackingLease ----------------------------------------------------------------------------------------------------------------

BackingLease::BackingLease(std::shared_ptr<Backing> backing) noexcept : m_backing(std::move(backing)) {
    if (m_backing) m_backing->leases.fetch_add(1, std::memory_order_acq_rel);
}

BackingLease::BackingLease(const BackingLease& other) noexcept : m_backing(other.m_backing) {
    if (m_backing) m_backing->leases.fetch_add(1, std::memory_order_acq_rel);
}

BackingLease::BackingLease(BackingLease&& other) noexcept : m_backing(std::move(other.m_backing)) {}

BackingLease& BackingLease::operator=(const BackingLease& other) noexcept {
    if (this != &other) {
        BackingLease copy(other);
        *this = std::move(copy);
    }
    return *this;
}

BackingLease& BackingLease::operator=(BackingLease&& other) noexcept {
    if (this != &other) {
        Reset();
        m_backing = std::move(other.m_backing);
    }
    return *this;
}

BackingLease::~BackingLease() { Reset(); }

void BackingLease::Bind(std::uint64_t safeAt) const noexcept {
    if (!m_backing) return;
    auto current = m_backing->safeAt.load(std::memory_order_relaxed);
    while (current < safeAt && !m_backing->safeAt.compare_exchange_weak(current, safeAt, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
}

void BackingLease::Reset() noexcept {
    if (!m_backing) return;
    auto backing = std::move(m_backing);  // keeps the backing alive through its return
    if (backing->leases.fetch_sub(1, std::memory_order_acq_rel) == 1) BackingPool::Returned(*backing);
}

// BackingPool ------------------------------------------------------------------------------------------------------------------

struct BackingPool::State {
    struct Key {
        std::uint32_t stride = 0;
        BackingFlags flags;
        std::uint64_t compatibility = 0;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct Active {
        Key key;
        std::uint64_t capacityClass = 0;  // the class last asked for this key: its free backings of other classes are released
    };
    std::vector<std::shared_ptr<Backing>> all;   // every backing the pool holds
    std::vector<std::shared_ptr<Backing>> free;  // those whose last lease went (taken in from the return stack)
    std::vector<Active> active;
    std::uint64_t nextGeneration = 1;

    static Key KeyOf(const Backing& backing) noexcept { return {backing.stride, backing.flags, backing.compatibility}; }
    static Key KeyOf(const BackingRequest& request) noexcept { return {request.stride, request.flags, request.compatibility}; }
};

namespace {

// The single-acquirer contract, checked in debug builds (a host-serialized pool runs this inside its own exclusion).
class AcquirerScope {
public:
    explicit AcquirerScope(std::atomic<bool>& flag) noexcept : m_flag(flag) {
        [[maybe_unused]] const bool busy = m_flag.exchange(true, std::memory_order_acquire);
        assert(!busy && "BackingPool: a second acquirer (the pool has one acquirer at a time)");
    }
    ~AcquirerScope() { m_flag.store(false, std::memory_order_release); }
    AcquirerScope(const AcquirerScope&) = delete;
    AcquirerScope& operator=(const AcquirerScope&) = delete;

private:
    std::atomic<bool>& m_flag;
};

template <class Body>
void RunExclusive(const BackingPoolHooks& hooks, std::atomic<bool>& acquiring, Body&& body) {
    if (hooks.exclusive) {
        hooks.exclusive([&] {
            AcquirerScope scope(acquiring);
            body();
        });
        return;
    }
    AcquirerScope scope(acquiring);
    body();
}

} // namespace

BackingPool::BackingPool(BackingPoolHooks hooks, std::shared_ptr<RetirementClock> clock, Config config)
    : m_hooks(std::move(hooks)), m_clock(std::move(clock)), m_config(std::move(config)), m_home(std::make_shared<detail::BackingHome>()),
      m_state(std::make_unique<State>()) {
    m_home->name = m_config.name;
    m_home->telemetry = m_hooks.telemetry;
}

BackingPool::~BackingPool() {
    // Leases that outlive the pool drop their backings when they go (closed); returns already pushed are discarded unread.
    m_home->closed.store(true, std::memory_order_seq_cst);
    m_home->returned.exchange(nullptr, std::memory_order_acq_rel);
    for (auto& backing : m_state->all) {
        backing->retired.store(true, std::memory_order_seq_cst);
        if (m_hooks.retire && backing->buffer && backing->leases.load(std::memory_order_acquire) == 0 &&
            backing->bindingConsumers.load(std::memory_order_acquire) == 0) {
            try {
                m_hooks.retire(std::move(backing->buffer));
            } catch (...) {
            }
        }
    }
    m_home->live.store(0, std::memory_order_relaxed);
    m_home->free.store(0, std::memory_order_relaxed);
    m_home->bytes.store(0, std::memory_order_relaxed);
}

std::shared_ptr<BackingPool> BackingPool::Create(BackingPoolHooks hooks, std::shared_ptr<RetirementClock> clock, Config config) {
    if (!hooks.allocate) throw std::invalid_argument("BackingPool: hooks.allocate is required");
    if (!clock) throw std::invalid_argument("BackingPool: a retirement clock is required");
    return std::shared_ptr<BackingPool>(new BackingPool(std::move(hooks), std::move(clock), std::move(config)));
}

void BackingPool::Returned(Backing& backing) noexcept {
    const auto& home = backing.m_home;
    if (!home) return;
    home->free.fetch_add(1, std::memory_order_relaxed);
    if (!home->closed.load(std::memory_order_seq_cst)) {
        Backing* expected = home->returned.load(std::memory_order_relaxed);
        do {
            backing.m_nextReturned = expected;
        } while (!home->returned.compare_exchange_weak(expected, &backing, std::memory_order_release, std::memory_order_relaxed));
    }
    home->Event();
}

void BackingPool::TakeReturned() {
    auto& state = *m_state;
    auto* list = m_home->returned.exchange(nullptr, std::memory_order_acquire);
    while (list) {
        auto* next = list->m_nextReturned;
        list->m_nextReturned = nullptr;
        const auto found = std::ranges::find_if(state.all, [&](const auto& backing) { return backing.get() == list; });
        if (found != state.all.end()) state.free.push_back(*found);
        list = next;
    }
}

BackingLease BackingPool::Acquire(const BackingRequest& request, std::uint64_t preferEpoch, AcquireInfo* info) {
    BackingLease lease;
    RunExclusive(m_hooks, m_acquiring, [&] {
        auto& state = *m_state;
        auto& home = *m_home;
        AcquireInfo local;
        home.scanEvents.store(home.events.load(std::memory_order_acquire), std::memory_order_release);
        TakeReturned();

        const auto key = State::KeyOf(request);
        if (auto found = std::ranges::find_if(state.active, [&](const auto& active) { return active.key == key; }); found != state.active.end())
            found->capacityClass = request.capacityClass;
        else
            state.active.push_back({key, request.capacityClass});

        const auto completed = m_clock->Completed();
        // Nearest the journal first: the highest contentEpoch at or below preferEpoch, then those past it (lowest first).
        const auto nearer = [&](const std::shared_ptr<Backing>& a, const std::shared_ptr<Backing>& b) {
            const auto ea = a->contentEpoch.load(std::memory_order_relaxed), eb = b->contentEpoch.load(std::memory_order_relaxed);
            const bool ua = ea <= preferEpoch, ub = eb <= preferEpoch;
            if (ua != ub) return ua;
            return ua ? ea > eb : ea < eb;
        };

        std::vector<std::shared_ptr<Backing>> sameClass;
        std::vector<std::shared_ptr<Backing>> release;
        for (const auto& backing : state.free) {
            if (State::KeyOf(*backing) != key || !detail::Reusable(*backing, completed)) continue;
            // Superseded classes of this key go once reusable; other keys (another registration sharing the pool) stay.
            if (backing->capacityClass == request.capacityClass)
                sameClass.push_back(backing);
            else
                release.push_back(backing);
        }
        std::ranges::sort(sameClass, nearer);

        std::shared_ptr<Backing> chosen;
        std::uint32_t spares = 0;
        for (const auto& backing : sameClass) {
            if (!chosen) {
                if (detail::TryClaim(*backing)) {
                    backing->retired.store(false, std::memory_order_seq_cst);  // reused: back in circulation
                    backing->sealedBytes.store(0, std::memory_order_relaxed);  // a fresh fill: no version names it
                    chosen = backing;
                }
                continue;
            }
            if (spares < m_config.spareFree) {
                ++spares;
                continue;
            }
            release.push_back(backing);
        }
        for (const auto& backing : release) {
            if (!detail::TryClaim(*backing)) continue;
            ReleaseBacking(backing);
            ++local.released;
        }

        if (chosen) {
            std::erase(state.free, chosen);
            home.free.fetch_sub(1, std::memory_order_relaxed);
            home.reused.fetch_add(1, std::memory_order_relaxed);
            home.Emit(BackingPoolEvent::Reused, 1);
            lease = BackingLease(std::move(chosen));
        } else {
            const auto classCount = static_cast<std::uint64_t>(std::ranges::count_if(state.all, [&](const auto& backing) {
                return State::KeyOf(*backing) == key && backing->capacityClass == request.capacityClass;
            }));
            // reuseBound: the class's free backing bound longest ago, still bound to a frame in flight; the caller's writes wait for it.
            std::shared_ptr<Backing> bound;
            if (m_config.maxPerClass != 0 && classCount >= m_config.maxPerClass && m_config.reuseBound) {
                for (const auto& backing : state.free) {
                    if (State::KeyOf(*backing) != key || backing->capacityClass != request.capacityClass ||
                        backing->leases.load(std::memory_order_acquire) != 0 || backing->readbackPins.load(std::memory_order_acquire) != 0 ||
                        backing->bindingConsumers.load(std::memory_order_acquire) != 0)
                        continue;
                    if (!bound || backing->safeAt.load(std::memory_order_acquire) < bound->safeAt.load(std::memory_order_acquire))
                        bound = backing;
                }
                if (bound && !detail::TryClaim(*bound))
                    bound.reset();
            }
            if (bound) {
                local.writeAfter = bound->safeAt.load(std::memory_order_acquire);
                bound->retired.store(false, std::memory_order_seq_cst);
                bound->sealedBytes.store(0, std::memory_order_relaxed);
                std::erase(state.free, bound);
                home.free.fetch_sub(1, std::memory_order_relaxed);
                home.reused.fetch_add(1, std::memory_order_relaxed);
                home.Emit(BackingPoolEvent::Reused, 1);
                lease = BackingLease(std::move(bound));
            } else if (m_config.maxPerClass != 0 && classCount >= m_config.maxPerClass) {
                home.exhausted.fetch_add(1, std::memory_order_relaxed);
                home.Emit(BackingPoolEvent::Exhausted, 1);
                ArmAvailability(request);
            } else {
                lease = Allocate(request);
                local.allocated = true;
            }
        }
        EmitGauges();
        if (info) *info = local;
    });
    return lease;
}

void BackingPool::ReleaseBacking(const std::shared_ptr<Backing>& backing) {
    auto& state = *m_state;
    auto& home = *m_home;
    auto held = backing;  // the caller's reference may live in one of the lists
    std::erase(state.free, held);
    std::erase(state.all, held);
    home.free.fetch_sub(1, std::memory_order_relaxed);
    home.live.fetch_sub(1, std::memory_order_relaxed);
    home.bytes.fetch_sub(held->byteCapacity, std::memory_order_relaxed);
    home.released.fetch_add(1, std::memory_order_relaxed);
    home.Emit(BackingPoolEvent::Released, held->byteCapacity);
    if (m_hooks.retire && held->buffer) m_hooks.retire(std::move(held->buffer));
}

void BackingPool::ArmAvailability(const BackingRequest& request) {
    // Exhausted: a lease's return or a pin's release wakes availability by itself; a free backing held back only by the clock
    // (safeAt) or a readback pin needs the clock to wake it.
    auto& home = *m_home;
    const auto completed = m_clock->Completed();
    const auto key = State::KeyOf(request);
    std::uint64_t point = 0;
    for (const auto& backing : m_state->free) {
        if (State::KeyOf(*backing) != key || backing->capacityClass != request.capacityClass) continue;
        std::uint64_t at = 0;
        if (const auto safeAt = backing->safeAt.load(std::memory_order_acquire); safeAt > completed)
            at = safeAt;
        else if (backing->readbackPins.load(std::memory_order_acquire) != 0)
            at = completed + 1;
        if (at && (!point || at < point)) point = at;
    }
    if (!point) return;
    auto armed = home.armedAt.load(std::memory_order_acquire);
    do {
        if (armed && armed <= point) return;
    } while (!home.armedAt.compare_exchange_weak(armed, point, std::memory_order_acq_rel, std::memory_order_acquire));
    m_clock->Subscribe(point, [weak = std::weak_ptr<detail::BackingHome>(m_home), point](std::uint64_t) {
        const auto owner = weak.lock();
        if (!owner) return;
        auto expected = point;
        owner->armedAt.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        owner->Event();
    });
}

BackingLease BackingPool::Allocate(const BackingRequest& request) {
    auto& state = *m_state;
    auto& home = *m_home;
    auto allocation = m_hooks.allocate(request);
    auto backing = std::make_shared<Backing>();
    backing->buffer = std::move(allocation.buffer);
    backing->generation = state.nextGeneration++;
    backing->capacityClass = request.capacityClass;
    backing->byteCapacity = request.ByteCapacity();
    backing->address = allocation.address;
    backing->srvIndex = allocation.srvIndex;
    backing->stride = request.stride;
    backing->flags = request.flags;
    backing->compatibility = request.compatibility;
    backing->m_home = m_home;
    if (backing->buffer) {
        if (m_hooks.tag) m_hooks.tag(*backing->buffer, request.name);
        // Every backing carries the binding-consumer pin factory: a binding that captures the buffer keeps it from reuse.
        detail::InstallConsumerLeaseFactory(*backing->buffer,
            [weak = std::weak_ptr<Backing>(backing), home = std::weak_ptr<detail::BackingHome>(m_home)]() -> std::shared_ptr<const void> {
                const auto value = weak.lock();
                auto owner = home.lock();
                return value && owner ? detail::Pin(value, std::move(owner)) : std::shared_ptr<const void>{};
            });
    }
    state.all.push_back(backing);
    home.live.fetch_add(1, std::memory_order_relaxed);
    home.bytes.fetch_add(backing->byteCapacity, std::memory_order_relaxed);
    home.allocated.fetch_add(1, std::memory_order_relaxed);
    home.Emit(BackingPoolEvent::Allocated, backing->byteCapacity);
    return BackingLease(std::move(backing));
}

void BackingPool::EmitGauges() const noexcept {
    if (!m_hooks.telemetry) return;
    const auto& home = *m_home;
    home.Emit(BackingPoolEvent::LiveBackings, home.live.load(std::memory_order_relaxed));
    home.Emit(BackingPoolEvent::LiveBytes, home.bytes.load(std::memory_order_relaxed));
    home.Emit(BackingPoolEvent::FreeBackings, home.free.load(std::memory_order_relaxed));
}

void BackingPool::SubscribeAvailability(std::uint64_t identity, std::function<void(std::uint64_t identity)> wake) {
    if (!wake) return;
    auto& home = *m_home;
    home.Push(new detail::BackingHome::Waiter{identity, std::move(wake), nullptr});
    home.Emit(BackingPoolEvent::Suspended, 1);
    // An event between the last Acquire's look and this subscription may have made a backing reusable: wake now (the caller
    // retries) rather than miss it.
    if (home.events.load(std::memory_order_acquire) != home.scanEvents.load(std::memory_order_acquire)) home.Event();
}

std::shared_ptr<const void> BackingPool::PinBindingConsumer(const std::shared_ptr<Backing>& backing) {
    if (!backing) throw std::invalid_argument("BackingPool: no backing to pin");
    return detail::Pin(backing, m_home);
}

std::optional<std::uint64_t> BackingPool::OldestContentEpoch() const {
    std::optional<std::uint64_t> oldest;
    RunExclusive(m_hooks, m_acquiring, [&] {
        for (const auto& backing : m_state->all) {
            const auto epoch = backing->contentEpoch.load(std::memory_order_acquire);
            if (epoch != 0 && (!oldest || epoch < *oldest)) oldest = epoch;
        }
    });
    return oldest;
}

void BackingPool::Trim() {
    RunExclusive(m_hooks, m_acquiring, [&] {
        auto& state = *m_state;
        TakeReturned();
        const auto completed = m_clock->Completed();
        std::vector<std::shared_ptr<Backing>> release;
        for (const auto& active : state.active) {
            // Keep the active class's spares nearest the newest contents (highest contentEpoch); release the rest once reusable.
            std::vector<std::shared_ptr<Backing>> candidates;
            for (const auto& backing : state.free)
                if (State::KeyOf(*backing) == active.key && detail::Reusable(*backing, completed)) candidates.push_back(backing);
            std::ranges::sort(candidates, [](const auto& a, const auto& b) {
                return a->contentEpoch.load(std::memory_order_relaxed) > b->contentEpoch.load(std::memory_order_relaxed);
            });
            std::uint32_t spares = 0;
            for (const auto& backing : candidates) {
                if (backing->capacityClass == active.capacityClass && spares < m_config.spareFree) {
                    ++spares;
                    continue;
                }
                release.push_back(backing);
            }
        }
        for (const auto& backing : release)
            if (detail::TryClaim(*backing)) ReleaseBacking(backing);
        EmitGauges();
    });
}

void BackingPool::Visit(const std::function<void(const Backing&)>& visit) const {
    if (!visit) return;
    RunExclusive(m_hooks, m_acquiring, [&] {
        for (const auto& backing : m_state->all) visit(*backing);
    });
}

BackingPool::Census BackingPool::Count() const noexcept {
    const auto& home = *m_home;
    Census census;
    census.live = home.live.load(std::memory_order_relaxed);
    census.free = (std::min)(home.free.load(std::memory_order_relaxed), census.live);
    census.leased = census.live - census.free;
    census.bytes = home.bytes.load(std::memory_order_relaxed);
    census.allocated = home.allocated.load(std::memory_order_relaxed);
    census.reused = home.reused.load(std::memory_order_relaxed);
    census.released = home.released.load(std::memory_order_relaxed);
    census.exhausted = home.exhausted.load(std::memory_order_relaxed);
    return census;
}

} // namespace org::services
