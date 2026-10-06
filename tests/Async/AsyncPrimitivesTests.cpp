#include <ORGModuleServices/Async/ArtifactBuild.h>
#include <ORGModuleServices/Async/ArtifactSnapshot.h>
#include <ORGModuleServices/Async/GraphSchedulingLayout.h>
#include <ORGModuleServices/Async/ArtifactResources.h>
#include <ORGModuleServices/Async/LeasedArraySlots.h>
#include <ORGModuleServices/Async/PublicationExchange.h>
#include <ORGModuleServices/Async/RevisionAssembly.h>
#include <ORGModuleServices/Async/SerializedTaskPump.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

enum class ContractLane : std::uint8_t { Capture, Prepare };
enum class ContractDomain : std::uint8_t { Build, Accept };
struct ContractScheduling {
    using Lane = ContractLane;
    using Domain = ContractDomain;
    static constexpr Lane initialLane = Lane::Prepare;
    static constexpr Lane continuationLane = Lane::Capture;
    static constexpr Domain acceptanceDomain = Domain::Accept;
    static constexpr Domain producerDomain = Domain::Build;
    static constexpr std::uint8_t admissionGroup = 5;
};


void TestArtifactBuildContracts() {
    using namespace org::async;
    enum class Kind : std::uint16_t { Group, Geometry };
    using Result = ArtifactBuildResult<Kind>;
    using Suspension = ArtifactSuspension<Kind>;
    const ArtifactAddress<Kind> key{Kind::Group, 7, 0};
    const ArtifactVersionID<Kind> version{key, 3, 9};
    const auto payload = ArtifactPayload::Make(std::make_shared<const unsigned>(42));
    ArtifactBuildContext<Kind> context;
    context.dependencies.push_back({key, 3, 9, ArtifactReadiness::CpuReady, payload});
    assert(*context.Dependency<unsigned>(key).payload == 42);
    assert(!context.Dependency<float>(key));
    assert(!context.Dependency<unsigned>({Kind::Geometry, 7, 0}));
    bool stopped = false;
    context.stopRequested = [&] { return stopped; };
    stopped = true;
    assert(context.stopRequested());

    auto gpu = std::make_shared<GpuSubmissionSet>();
    auto ready = Result::Ready(payload, gpu);
    assert(ready.outcome == Result::Outcome::Ready && ready.gpuSubmissions == gpu);
    assert(*ready.payload.Get<unsigned>() == 42);
    auto needs = Result::Needs({Exact(version)}, payload);
    assert(needs.outcome == Result::Outcome::NeedsDependencies);
    assert(needs.requirements.front().requiredGeneration == 9);
    assert(needs.checkpoint.Get<unsigned>() == payload.Get<unsigned>());
    auto retry = Result::Retry(std::chrono::milliseconds(3), payload);
    assert(retry.outcome == Result::Outcome::RetryAfter && retry.retryDelay == std::chrono::milliseconds(3));
    assert(retry.checkpoint.Get<unsigned>() == payload.Get<unsigned>());
    auto exact = Suspension::Exact(version, ArtifactReadiness::GpuReady);
    assert(exact.dependency == version && exact.kind == ArtifactSuspensionKind::ExactDependency);
    assert(exact.milestone == ArtifactReadiness::GpuReady);
    auto capacity = Suspension::Capacity(10);
    auto external = Suspension::External(11, "upload");
    const auto deadline = std::chrono::steady_clock::now();
    auto transient = Suspension::Transient(12, deadline, 4, "retry");
    assert(capacity.identity == 10 && capacity.kind == ArtifactSuspensionKind::Capacity);
    assert(external.identity == 11 && external.kind == ArtifactSuspensionKind::ExternalOperation && external.reason == "upload");
    assert(transient.kind == ArtifactSuspensionKind::TransientRetry && transient.identity == 12);
    assert(transient.deadline == deadline && transient.maximumAttempts == 4 && transient.reason == "retry");
    auto suspended = Result::Suspend(exact, payload);
    assert(suspended.outcome == Result::Outcome::Suspended && suspended.suspension->dependency == version);
    assert(suspended.checkpoint.Get<unsigned>() == payload.Get<unsigned>());
    auto failed = Result::Failure("failed");
    assert(failed.outcome == Result::Outcome::Failed && failed.error == "failed");
    assert(Result::Cancelled().outcome == Result::Outcome::Cancelled);
    assert(Result{}.outcome == Result::Outcome::Failed);

    ArtifactRequestResult<Kind> request;
    assert(!request);
    request.status = ArtifactRequestStatus::Accepted;
    request.version = version;
    assert(request && request.Handle().version == version);
    request.status = ArtifactRequestStatus::AlreadyDesired;
    assert(request);
    request.status = ArtifactRequestStatus::ConflictingRevision;
    assert(!request);
    ArtifactProducerRegistration<Kind> producer;
    assert(producer.lane == 0 && producer.domain == 0 && producer.scheduling.admissionGroup == 0);
    producer.producer = [](const auto& input) { return Result::Ready(input.input); };
    context.input = payload;
    assert(producer.producer(context).payload.Get<unsigned>() == payload.Get<unsigned>());
    AsyncStateGraphStats<2> stats;
    ArtifactProducerRegistration<Kind, ContractScheduling> custom;
    ArtifactBuildResult<Kind, ContractScheduling> customResult;
    assert(custom.lane == ContractLane::Prepare && custom.domain == ContractDomain::Build);
    assert(custom.scheduling.initialLane == ContractLane::Prepare);
    assert(custom.scheduling.continuationLane == ContractLane::Capture && custom.scheduling.admissionGroup == 5);
    assert(customResult.acceptance.lane == ContractLane::Prepare && customResult.acceptance.domain == ContractDomain::Accept);
    bool accepted = false;
    customResult.acceptance.action = [&](const auto& snapshot) { accepted = snapshot.Version() == version; };
    customResult.acceptance.action(context.dependencies.front());
    assert(accepted);
    assert(stats.intentsByKind.size() == 2 && stats.requests == 0);
}

template <class Registration>
void CheckRegistrationLifetime() {
    using namespace org::async;
    using Snapshot = typename Registration::ArtifactSnapshot;
    unsigned cancelled = 0;
    auto owner = std::make_shared<int>(1);
    std::weak_ptr<int> weak = owner;
    Snapshot snapshot;
    snapshot.lease = ArtifactLease(owner);
    owner.reset();
    {
        auto make = [&](std::uint64_t id) {
            if constexpr (std::is_constructible_v<Registration, std::uint64_t, std::uint64_t, Snapshot, std::function<void()>>)
                return Registration(id, 8, snapshot, [&] { ++cancelled; });
            else return Registration(id, snapshot, [&] { ++cancelled; });
        };
        auto first = make(1);
        auto second = make(2);
        snapshot = {};
        auto moved = std::move(first);
        assert(first.subscription == 0 && moved.subscription == 1 && cancelled == 0);
        second = std::move(moved); // Unregister only the replaced registration.
        assert(cancelled == 1 && moved.subscription == 0 && second.subscription == 1);
        second.Reset();
        second.Reset();
        assert(cancelled == 2 && second.subscription == 0 && !weak.expired());
        // Reset cancels registration, but preserves its exact-version snapshot lease.
    }
    assert(cancelled == 2 && weak.expired());
}

void TestArtifactRegistrations() {
    enum class Kind : std::uint16_t { Group };
    static_assert(!std::is_copy_constructible_v<org::async::ArtifactObservation<Kind>>);
    static_assert(!std::is_copy_constructible_v<org::async::ArtifactAwaiter<Kind>>);
    CheckRegistrationLifetime<org::async::ArtifactObservation<Kind>>();
    CheckRegistrationLifetime<org::async::ArtifactAwaiter<Kind>>();
}

void TestArtifactIdentity() {
    using namespace org::async;
    enum class Kind : std::uint16_t { Group, Geometry };
    enum class OtherKind : std::uint16_t { Group };
    using Address = ArtifactAddress<Kind>;
    using Version = ArtifactVersionID<Kind>;
    static_assert(!std::is_convertible_v<Address, ArtifactAddress<OtherKind>>);
    const Address address{Kind::Group, 91, 3};
    const Version first{address, 7, 20}, reincarnated{address, 7, 21};
    assert(first != reincarnated && first && !Version{});
    assert(Address::Hasher{}(address) == Address::Hasher{}(first.address));
    assert(Exact(first).requiredGeneration == 20);
    assert(Exact(first) != Exact(reincarnated));
    assert(Exact(first).invalidation == DependencyInvalidationPolicy::ExactSnapshot);
    assert(Latest(address).minimumRevision == 0);
    assert(LatestAtLeast(address, 4).minimumRevision == 4);
    assert(ReadyGate(first).requiredGeneration == 20);
    assert(ReadyGate(address).requiredGeneration == 0);
    assert(ReadyGate(first).invalidation == DependencyInvalidationPolicy::ReadyGate);
    assert(LifetimeHold(first).policy == DependencyPolicy::Optional);
    assert(LifetimeHold(first).invalidation == DependencyInvalidationPolicy::LifetimeHold);

    ArtifactRequirement<Kind> recipe;
    std::weak_ptr<int> weak;
    {
        auto backing = std::make_shared<int>(1);
        weak = backing;
        ArtifactVersionHandle<Kind> handle{first, ArtifactLease(backing)};
        assert(handle);
        recipe = Exact(handle, ArtifactReadiness::UploadSubmitted);
    }
    assert(weak.expired()); // Requirements describe identity; they do not own pins.
    assert(recipe.requiredGeneration == 20 && recipe.requiredReadiness == ArtifactReadiness::UploadSubmitted);

    ArtifactHandle<unsigned, Kind> retained;
    {
        auto backing = std::make_shared<int>(2);
        weak = backing;
        ArtifactSnapshot<Kind> snapshot{address, 7, 20, ArtifactReadiness::GpuReady,
            ArtifactPayload::Make(std::make_shared<const unsigned>(42)), {}, ArtifactLease(backing)};
        retained = MakeArtifactHandle<unsigned>(snapshot);
        assert(retained.Version() == first);
    }
    assert(!weak.expired() && retained && *retained.payload == 42);
    retained = {};
    assert(weak.expired());
    DependencyExpression<Kind> alternatives = FirstReady<Kind>{{{address, 7, ArtifactReadiness::CpuReady}}};
    assert(std::get<FirstReady<Kind>>(alternatives).alternatives.front().key == address);
}

void TestSchedulingLayout() {
    using namespace org::async;
    const GraphSchedulingLayout layout{2, 3, {1, 2}, {0, 1}};
    assert(layout.Valid() && layout.MailboxCount() == 6);
    for (std::size_t i = 0; i != layout.MailboxCount(); ++i)
        assert(layout.Index(layout.ClassAt(i)) == i);
    assert(!layout.Index({2, 0}) && !layout.Index({0, 3}));
    bool rejected = false;
    try { (void)layout.ClassAt(6); } catch (const std::out_of_range&) { rejected = true; }
    assert(rejected);
    for (const auto invalid : {GraphSchedulingLayout{0, 1}, GraphSchedulingLayout{1, 0},
        GraphSchedulingLayout{257, 1}, GraphSchedulingLayout{1, 257},
        GraphSchedulingLayout{1, 1, {1, 0}}, GraphSchedulingLayout{1, 1, {}, {0, 1}}}) {
        assert(!invalid.Valid() && !invalid.Index({}));
    }
}

void TestArtifactResources() {
    using namespace org::async;
    auto value = std::make_shared<const unsigned>(37);
    std::weak_ptr<const unsigned> weak = value;
    ArtifactLease lease(value);
    const auto payload = ArtifactPayload::Make(value);
    value.reset();
    assert(lease && !weak.expired());
    assert(payload.Valid() && *payload.Get<unsigned>() == 37);
    assert(!payload.Get<float>());
    auto anotherLease = lease;
    lease.reset();
    assert(anotherLease && !weak.expired());
    anotherLease.reset();
    assert(!weak.expired()); // The immutable payload is still a consumer.
    ArtifactLease empty;
    assert(!empty);
    assert(!ArtifactPayload{}.Valid());

    assert(ArtifactReachedMilestone(ArtifactReadiness::GpuReady, ArtifactReadiness::CpuReady));
    assert(ArtifactReachedMilestone(ArtifactReadiness::UploadSubmitted, ArtifactReadiness::CpuReady));
    assert(!ArtifactReachedMilestone(ArtifactReadiness::CpuReady, ArtifactReadiness::UploadSubmitted));
    assert(!ArtifactReachedMilestone(ArtifactReadiness::UploadSubmitted, ArtifactReadiness::GpuReady));
    assert(ArtifactReachedMilestone(ArtifactReadiness::Published, ArtifactReadiness::GpuReady));
    for (auto terminal : {ArtifactReadiness::Cancelled, ArtifactReadiness::Failed, ArtifactReadiness::Superseded})
        assert(!ArtifactReachedMilestone(terminal, ArtifactReadiness::CpuReady));

    auto timeline = std::make_shared<int>(1);
    std::weak_ptr<int> timelineLifetime = timeline;
    GpuSubmissionSet uploads;
    bool submitted = false, complete = false, failed = false, cancelled = false;
    uploads.isSubmitted = [&] { return submitted; };
    uploads.isComplete = [&] { return complete; };
    uploads.isFailed = [&] { return failed; };
    uploads.failure = [] { return std::string("upload rejected"); };
    uploads.cancel = [&] { cancelled = true; return true; };
    std::uint64_t signalValue = 5;
    uploads.submissions.push_back({timeline, 0, {}, [&] { return signalValue; }});
    timeline.reset();
    assert(!timelineLifetime.expired() && !uploads.Submitted() && !uploads.Complete());
    assert(uploads.MaximumTimelineValue() == 5 && uploads.submissions.front().TimelineOwner());
    submitted = true;
    assert(uploads.Submitted() && !uploads.Complete()); // Submitted is not CPU-complete.
    signalValue = 9;
    assert(uploads.MaximumTimelineValue() == 9);
    complete = true;
    assert(uploads.Complete());
    failed = true;
    assert(uploads.Failed() && uploads.Failure() == "upload rejected");
    assert(uploads.Cancel() && cancelled);
    uploads.submissions.clear();
    assert(timelineLifetime.expired());
}

void TestPublicationExchange() {
    struct Publication {
        unsigned world;
        std::array<unsigned, 4> actorParts;
        std::atomic<unsigned>* destroyed;
        ~Publication() { ++*destroyed; }
    };
    using Exchange = org::async::PublicationExchange<Publication>;
    std::atomic<unsigned> destroyed{0};
    Exchange exchange;
    auto make = [&](unsigned world, unsigned revision) -> Exchange::Lease {
        return Exchange::Lease(new Publication{world, {revision, revision, revision, revision}, &destroyed});
    };
    auto first = make(1, 10);
    auto second = make(1, 11);
    assert(exchange.TryPublish(first) && !first);
    assert(!exchange.TryPublish(second) && second);
    const auto accept = [](const Publication& publication) noexcept {
        return publication.world == 1 && std::all_of(publication.actorParts.begin(), publication.actorParts.end(),
            [&](unsigned revision) { return revision == publication.actorParts.front(); });
    };
    assert(exchange.TrySelect(accept) == Exchange::Selection::Selected);
    auto frameLease = exchange.AcquireActive();
    assert(exchange.TrySelect(accept) == Exchange::Selection::Unchanged);
    assert(exchange.TryPublish(second));
    assert(exchange.TrySelect(accept) == Exchange::Selection::Selected);
    assert(destroyed == 0 && frameLease->actorParts.front() == 10);
    exchange.ReclaimRetired();
    assert(destroyed == 0); // GPU/frame lease still owns revision 10
    frameLease.reset();
    assert(destroyed == 1);
    auto staleWorld = make(0, 12);
    assert(exchange.TryPublish(staleWorld));
    assert(exchange.TrySelect(accept) == Exchange::Selection::Rejected);
    assert(exchange.Active()->actorParts.front() == 11 && destroyed == 1);
    exchange.ReclaimRetired();
    assert(destroyed == 2);
    exchange.ClearActive();
    assert(!exchange.Active() && destroyed == 2);
    exchange.ReclaimRetired();
    assert(destroyed == 3);
}

void TestConcurrentPublicationExchange() {
    org::async::PublicationExchange<std::array<unsigned, 4>> exchange;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        for (unsigned revision = 1; revision <= 10000; ++revision) {
            decltype(exchange)::Lease candidate = std::make_shared<const std::array<unsigned, 4>>(
                std::array<unsigned, 4>{revision, revision, revision, revision});
            while (!exchange.TryPublish(candidate)) std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
    });
    unsigned selected = 0;
    do {
        (void)exchange.TrySelect([&](const auto& value) noexcept {
            assert(value.front() > selected);
            assert(std::all_of(value.begin(), value.end(), [&](unsigned v) { return v == value.front(); }));
            selected = value.front();
            return true;
        });
    } while (!done.load(std::memory_order_acquire) || selected != 10000);
    producer.join();
    exchange.ClearActive();
    exchange.ReclaimRetired();
}

void TestRevisionAssembly() {
    using namespace org::async;
    using Fragment = RevisionFragment;
    using Assembler = RevisionAssembler;
    std::atomic<unsigned> notified{0};
    Assembler assembler(3, [&] { ++notified; });
    auto pending = [] { return std::make_shared<Fragment>(); };
    auto value = [](int v) { return std::make_shared<const int>(v); };

    // Partial readiness: published only once every named fragment is ready.
    auto a1 = pending(), b1 = pending();
    auto c1 = Fragment::MakeReady(value(3));
    auto draft = assembler.Begin();
    draft.Set(0, a1);
    draft.Set(1, b1);
    draft.Set(2, c1);
    assert(assembler.Seal(std::move(draft)) == 1);
    assert(assembler.Collect().published == 0 && assembler.TrySelect() == Assembler::Selection::Unchanged);
    assert(a1->Resolve(value(1)) && !a1->Resolve(value(9)));
    assert(assembler.Collect().published == 0 && notified == 0);
    assert(b1->Resolve(value(2)) && notified == 1);
    assert(assembler.Collect().published == 1);
    assert(assembler.TrySelect() == Assembler::Selection::Selected);
    const auto* active = assembler.Active();
    assert(active->Sequence() == 1 && *active->Get<int>(0) == 1 && *active->Get<int>(1) == 2 && *active->Get<int>(2) == 3);
    assert(!active->Get<float>(0)); // the resolved type only

    // Unchanged slots inherit exact versions.
    auto c2 = pending();
    draft = assembler.Begin();
    draft.Set(2, c2);
    assert(assembler.Seal(std::move(draft)) == 2);
    assert(assembler.Collect().published == 0 && assembler.Active()->Sequence() == 1);
    (void)c2->Resolve(value(30));
    assert(assembler.Collect().published == 2 && assembler.TrySelect() == Assembler::Selection::Selected);
    active = assembler.Active();
    assert(active->Fragment(0) == a1 && active->Fragment(1) == b1 && *active->Get<int>(2) == 30);

    // Closure: a fragment requiring an exact version in another slot cannot be named beside anything else.
    auto a3 = pending();
    auto b3 = std::make_shared<Fragment>(std::vector<Fragment::Requirement>{{0, a3}});
    draft = assembler.Begin();
    draft.Set(1, b3);
    bool rejected = false;
    try { (void)assembler.Seal(draft); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected && assembler.LatestSequence() == 2);
    draft.Set(0, a3);
    assert(assembler.Seal(std::move(draft)) == 3);
    // A draft begun before another seal is stale.
    auto stale = assembler.Begin();
    draft = assembler.Begin();
    draft.Set(2, Fragment::MakeReady(value(40)));
    assert(assembler.Seal(std::move(draft)) == 4); // inherits revision 3's pending a3 and b3
    rejected = false;
    try { (void)assembler.Seal(std::move(stale)); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    // Revision 4 shares 3's work in flight; both complete together, the newest wins, the older is superseded.
    (void)b3->Resolve(value(20));
    assert(assembler.Collect().published == 0);
    (void)a3->Resolve(value(10));
    assert(assembler.Collect().published == 4 && assembler.GetStats().superseded == 1 && assembler.Pending() == 0);
    assert(assembler.TrySelect() == Assembler::Selection::Selected);
    active = assembler.Active();
    assert(*active->Get<int>(0) == 10 && *active->Get<int>(1) == 20 && *active->Get<int>(2) == 40);

    // A newer complete revision abandons an older pending one (never cancelled before that), whose fragment may
    // still settle later without effect.
    auto a5 = pending();
    draft = assembler.Begin();
    draft.Set(0, a5);
    draft.Set(1, Fragment::MakeReady(value(50)));
    assert(assembler.Seal(std::move(draft)) == 5);
    draft = assembler.Begin();
    draft.Set(0, Fragment::MakeReady(value(60)));
    draft.Set(1, Fragment::MakeReady(value(61)));
    assert(assembler.Seal(std::move(draft)) == 6);
    assert(assembler.Collect().published == 6 && assembler.GetStats().abandoned == 1 && assembler.Pending() == 0);
    const auto before = notified.load();
    (void)a5->Resolve(value(5));
    assert(notified == before && assembler.Collect().published == 0);

    // Latest wins over a publication the frame has not selected yet.
    for (int v = 70; v < 72; ++v) {
        draft = assembler.Begin();
        draft.Set(2, Fragment::MakeReady(value(v)));
        (void)assembler.Seal(std::move(draft));
        assert(assembler.Collect().published == assembler.LatestSequence());
    }
    auto frameLease = assembler.AcquireActive();
    assert(assembler.TrySelect() == Assembler::Selection::Selected && *assembler.Active()->Get<int>(2) == 71);
    assert(frameLease->Sequence() == 4); // the frame's lease still owns what it selected

    // Failure: the revision fails whole and the active one stays.
    auto a9 = pending(), b9 = pending();
    draft = assembler.Begin();
    draft.Set(0, a9);
    draft.Set(1, b9);
    const auto failing = assembler.Seal(std::move(draft));
    (void)a9->Resolve(value(90));
    (void)b9->Fail(std::make_exception_ptr(std::runtime_error("no pipeline")));
    auto collected = assembler.Collect();
    assert(collected.published == 0 && collected.failures.size() == 1 && collected.failures[0].sequence == failing);
    rejected = false;
    try { std::rethrow_exception(collected.failures[0].error); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected && assembler.TrySelect() == Assembler::Selection::Unchanged && *assembler.Active()->Get<int>(2) == 71);
    // A later draft inherits the failed fragment until the coordinator names a new one.
    draft = assembler.Begin();
    assert(draft.Get(1) == b9 && b9->GetState() == Fragment::State::Failed);
    draft.Set(1, Fragment::MakeReady(value(91)));
    (void)assembler.Seal(std::move(draft));
    assert(assembler.Collect().published == assembler.LatestSequence() && assembler.TrySelect() == Assembler::Selection::Selected);
    assert(*assembler.Active()->Get<int>(0) == 90 && *assembler.Active()->Get<int>(1) == 91);
    assert(assembler.GetStats().failed == 1);
}

// Producers settle fragments on many threads in any order; the frame only ever selects whole revisions, in order.
void TestConcurrentRevisionAssembly() {
    using namespace org::async;
    constexpr unsigned kSlots = 4, kRevisions = 3000, kResolvers = 4;
    RevisionAssembler assembler(kSlots);
    std::mutex queueMutex;
    std::deque<std::pair<std::shared_ptr<RevisionFragment>, unsigned>> queue;
    std::atomic<bool> sealed{false};
    std::vector<std::thread> resolvers;
    for (unsigned t = 0; t < kResolvers; ++t)
        resolvers.emplace_back([&, t] {
            for (unsigned spin = 0;;) {
                std::pair<std::shared_ptr<RevisionFragment>, unsigned> item;
                {
                    std::lock_guard lock(queueMutex);
                    if (!queue.empty()) {
                        // Take from either end, so fragments settle out of order.
                        if ((spin++ + t) & 1) { item = std::move(queue.front()); queue.pop_front(); }
                        else { item = std::move(queue.back()); queue.pop_back(); }
                    }
                }
                if (item.first) (void)item.first->Resolve(std::make_shared<const unsigned>(item.second));
                else if (sealed.load(std::memory_order_acquire)) return;
                else std::this_thread::yield();
            }
        });
    std::atomic<std::uint64_t> published{0};
    std::thread coordinator([&] {
        auto queueFragment = [&](std::shared_ptr<RevisionFragment> fragment, unsigned revision) {
            std::lock_guard lock(queueMutex);
            queue.emplace_back(std::move(fragment), revision);
        };
        for (unsigned revision = 1; revision <= kRevisions; ++revision) {
            auto draft = assembler.Begin();
            // Slot 0 and slot 1 change every revision (slot 1 requires slot 0's exact version: never split from it);
            // slots 2 and 3 every few, keeping the inherited (possibly pending) version in between.
            auto head = std::make_shared<RevisionFragment>();
            draft.Set(0, head);
            queueFragment(head, revision);
            auto paired = std::make_shared<RevisionFragment>(std::vector<RevisionFragment::Requirement>{{0, head}});
            draft.Set(1, paired);
            queueFragment(paired, revision);
            for (unsigned slot = 2; slot < kSlots; ++slot) {
                if (revision != 1 && revision % (slot + 1) != 1) continue;
                auto fragment = std::make_shared<RevisionFragment>();
                draft.Set(slot, fragment);
                queueFragment(std::move(fragment), revision);
            }
            assert(assembler.Seal(std::move(draft)) == revision);
            if (const auto sequence = assembler.Collect().published) published.store(sequence, std::memory_order_release);
        }
        sealed.store(true, std::memory_order_release);
        while (published.load(std::memory_order_acquire) != kRevisions) {
            if (const auto sequence = assembler.Collect().published) published.store(sequence, std::memory_order_release);
            std::this_thread::yield();
        }
    });
    std::uint64_t selected = 0, selections = 0;
    while (selected != kRevisions) {
        if (assembler.TrySelect([&](const AssembledRevision& revision) noexcept {
            assert(revision.Sequence() > selected);
            for (unsigned slot = 0; slot < kSlots; ++slot) {
                const auto v = revision.Get<unsigned>(slot);
                assert(v && *v <= revision.Sequence());
            }
            assert(*revision.Get<unsigned>(0) == revision.Sequence() && *revision.Get<unsigned>(1) == revision.Sequence());
            assert(revision.Fragment(1)->Requirements()[0].fragment == revision.Fragment(0));
            return true;
        }) == RevisionAssembler::Selection::Selected) {
            selected = assembler.Active()->Sequence();
            ++selections;
        } else std::this_thread::yield();
    }
    coordinator.join();
    for (auto& resolver : resolvers) resolver.join();
    const auto& stats = assembler.GetStats();
    assert(stats.sealed == kRevisions && stats.failed == 0);
    assert(stats.published + stats.superseded + stats.abandoned == kRevisions);
    std::printf("revision assembly: %llu published, %llu superseded, %llu abandoned, %llu selected\n",
        static_cast<unsigned long long>(stats.published), static_cast<unsigned long long>(stats.superseded),
        static_cast<unsigned long long>(stats.abandoned), static_cast<unsigned long long>(selections));
}

void TestLeases() {
    using Slots = org::async::LeasedArraySlots<unsigned, 3>;
    std::vector<Slots::Lease> held;
    {
        Slots slots;
        for (unsigned generation = 1; generation <= 1000; ++generation) {
            const auto slot = generation % 3;
            auto output = slots.PrepareWrite(slot, 64);
            std::fill(output.begin(), output.end(), generation);
            held.push_back(slots.Acquire(slot));
            for (unsigned i = 0; i < held.size(); ++i)
                assert(std::all_of(held[i]->begin(), held[i]->end(),
                    [i](unsigned value) { return value == i + 1; }));
        }
        // No lease: recycling a slot should keep its allocation.
        Slots reusable;
        const auto first = reusable.PrepareWrite(0, 32).data();
        assert(reusable.PrepareWrite(0, 32).data() == first);
        const auto lease = reusable.Acquire(0);
        assert(reusable.PrepareWrite(0, 32).data() != first);
    }
    // Destroying the head/slot owner cannot invalidate a retained payload.
    for (unsigned i = 0; i < held.size(); ++i) assert(held[i]->at(31) == i + 1);
}

void TestConcurrentTripleBuffer() {
    org::async::LeasedArraySlots<unsigned, 3> slots;
    std::atomic<unsigned> latest{1};
    std::atomic<bool> done{false};
    std::thread writer([&] {
        unsigned write = 0;
        for (unsigned generation = 1; generation <= 20000; ++generation) {
            auto output = slots.PrepareWrite(write, 128);
            std::fill(output.begin(), output.end(), generation);
            write = latest.exchange(write | 4, std::memory_order_acq_rel) & 3;
        }
        done.store(true, std::memory_order_release);
    });
    unsigned read = 2;
    std::vector<std::pair<unsigned, decltype(slots)::Lease>> held;
    do {
        if (latest.load(std::memory_order_relaxed) & 4) {
            read = latest.exchange(read, std::memory_order_acq_rel) & 3;
            auto lease = slots.Acquire(read);
            assert(lease && !lease->empty());
            held.emplace_back(lease->front(), std::move(lease));
        }
        if (held.size() > 32) held.erase(held.begin());
        for (const auto& [generation, lease] : held)
            assert(std::all_of(lease->begin(), lease->end(),
                [generation](unsigned value) { return value == generation; }));
    } while (!done.load(std::memory_order_acquire) || (latest.load(std::memory_order_acquire) & 4));
    writer.join();
    assert(!held.empty());
}

void TestPump() {
    using Pump = org::async::SerializedTaskPump;
    Pump pump;
    std::deque<Pump::Task> tasks;
    std::vector<Pump::Task> timers;
    unsigned drains = 0;
    bool notifyInDrain = true;
    pump.Configure([&](Pump::Task task) { tasks.push_back(std::move(task)); return true; },
        [&] { ++drains; if (notifyInDrain) { notifyInDrain = false; assert(pump.Notify()); } }, {},
        [&](auto, Pump::Task task) { timers.push_back(std::move(task)); return true; },
        Pump::HandoffMode::Resubmit);
    assert(pump.Notify());
    assert(pump.Notify());
    assert(tasks.size() == 1);
    while (!tasks.empty()) { auto task = std::move(tasks.front()); tasks.pop_front(); task(); }
    assert(drains == 2 && pump.IsIdle());
    assert(pump.NotifyAfter(std::chrono::seconds(2)));
    assert(pump.NotifyAfter(std::chrono::seconds(1)));
    assert(timers.size() == 2);
    timers[0](); // superseded timer must not schedule work
    assert(tasks.empty());
    timers[1]();
    assert(tasks.size() == 1);
    pump.Stop();
    tasks.front()();
    assert(drains == 2 && !pump.Notify());

    bool rejected = false;
    Pump failure;
    failure.Configure([](Pump::Task) { return false; }, [] {}, [&] { rejected = true; });
    assert(!failure.Notify() && rejected && !failure.Notify());
}

// Notify takes no lock: producers, two scheduler workers and an inline runner race it. Exactly one drain runs at a time, and
// every notification is followed by a drain that began after it (level-triggered): once everyone stops, the consumer has
// seen every producer's last value.
void TestConcurrentPump() {
    using Pump = org::async::SerializedTaskPump;
    for (const auto mode : { Pump::HandoffMode::Inline, Pump::HandoffMode::Resubmit }) {
        Pump pump;
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<Pump::Task> tasks;
        bool stopping = false;
        constexpr unsigned kProducers = 4, kPosts = 20000;
        std::atomic<unsigned> posted[kProducers]{};
        std::atomic<unsigned> seen[kProducers]{};
        std::atomic<int> draining{ 0 };
        std::atomic<unsigned> overlaps{ 0 };
        pump.Configure([&](Pump::Task task) {
                { std::lock_guard lock(mutex); tasks.push_back(std::move(task)); }
                changed.notify_one();
                return true;
            },
            [&] {
                if (draining.fetch_add(1) != 0) overlaps.fetch_add(1);
                for (unsigned p = 0; p < kProducers; ++p) seen[p].store(posted[p].load(std::memory_order_acquire), std::memory_order_relaxed);
                draining.fetch_sub(1);
            },
            {}, {}, mode);
        std::vector<std::thread> workers;
        for (int w = 0; w < 2; ++w)
            workers.emplace_back([&] {
                for (;;) {
                    Pump::Task task;
                    {
                        std::unique_lock lock(mutex);
                        changed.wait(lock, [&] { return stopping || !tasks.empty(); });
                        if (tasks.empty()) return;
                        task = std::move(tasks.front());
                        tasks.pop_front();
                    }
                    task();
                }
            });
        std::atomic<bool> producing{ true };
        std::thread inliner([&] { while (producing.load()) (void)pump.TryRunInline(); });
        std::vector<std::thread> producers;
        for (unsigned p = 0; p < kProducers; ++p)
            producers.emplace_back([&, p] {
                for (unsigned i = 1; i <= kPosts; ++i) {
                    posted[p].store(i, std::memory_order_release);
                    assert(pump.Notify());
                }
            });
        for (auto& producer : producers) producer.join();
        producing.store(false);
        inliner.join();
        while (!pump.IsIdle()) std::this_thread::yield();
        { std::lock_guard lock(mutex); stopping = true; }
        changed.notify_all();
        for (auto& worker : workers) worker.join();
        assert(overlaps.load() == 0);
        for (unsigned p = 0; p < kProducers; ++p) assert(seen[p].load() == kPosts);
        const auto stats = pump.GetStats();
        assert(stats.notifications == kProducers * kPosts && stats.drainedEpoch == stats.requestedEpoch);
    }
}
}

int main() {
    TestArtifactBuildContracts();
    TestArtifactRegistrations();
    TestArtifactIdentity();
    TestSchedulingLayout();
    TestArtifactResources();
    TestPublicationExchange();
    TestConcurrentPublicationExchange();
    TestRevisionAssembly();
    TestConcurrentRevisionAssembly();
    TestLeases();
    TestConcurrentTripleBuffer();
    TestPump();
    TestConcurrentPump();
}
