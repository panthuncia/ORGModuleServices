# ORGModuleServices

Optional module-facing services shared by OpenRenderGraph hosts. OpenRenderGraph does not depend on this package.

Provided services:

- Exact-fence-retired D3D12 upload allocations and shader-visible descriptor heaps.
- A versioned-buffer pool: GPU backings leased per version, filled by journal catch-up, and reused once their frames retired on a host retirement clock.
- Content-addressed DXC compilation with memory/disk artifacts and deduplicated in-flight requests.
- DXIL targets by default and optional SPIR-V targets through `ORG_MODULE_SERVICES_ENABLE_VULKAN`.
- Backend-neutral pipeline recipes, asynchronous deduplicated construction, family generations, and fence retirement.
- Header-only async primitives: serialized task pumps, leased immutable array slots, and a bounded single-producer/single-consumer publication exchange.
- Shared graph contracts: classified scheduler dispatch/cancellation, immutable artifact payloads and leases, readiness milestones, backend-opaque GPU submission prerequisites, and host-defined artifact policy.
- Host-kind-parameterized artifact identities, exact dependency recipes, snapshots and typed handles; validated host-provided scheduler queue layouts.
- Shared build/request/suspension and move-only continuation-registration contracts, host-defined scheduling defaults, and kind-count-parameterized graph statistics.
- Host-parameterized trace sessions/configuration and common graph diagnostics; renderer event vocabularies and report contents remain host-owned.
- Compiled suspension identity allocation through `ORGModuleServices::AsyncRuntime`, shared by graph instances and artifact kinds in one linked runtime.

The async primitives use the separate `ORGModuleServices::AsyncPrimitives` target.
Source-tree CPU-only hosts can include `cmake/AsyncPrimitives.cmake` without
configuring the RHI/shader services.

CPU-only consumers needing the compiled identity service include
`cmake/AsyncRuntime.cmake` and link `ORGModuleServices::AsyncRuntime` instead.
This static runtime must have one owner when exchanging identities across DLLs;
independently linked copies do not provide cross-module uniqueness.

Standalone tests:

```sh
cmake -S tests/Async -B build/async-tests
cmake --build build/async-tests --config Release
ctest --test-dir build/async-tests -C Release --output-on-failure
```

The extracted dependency engine is available through
`ORGModuleServices::AsyncStateGraph`. Source-tree consumers include
`cmake/AsyncStateGraph.cmake`; installed consumers use
`find_package(ORGAsyncStateGraph CONFIG REQUIRED)`. This CPU-only package depends
on TBB, spdlog and BasicTelemetry, not BasicRenderer/RHI/ORG. The full GPU module
can opt into building it with `ORG_MODULE_SERVICES_ENABLE_ASYNC_STATE_GRAPH=ON`.

`org::async::AsyncStateGraph` is the compiled numeric-kind (0..63) binding.
`StateGraph<Binding>` supports host-specific enums, scheduling defaults and trace
reports. Custom bindings include `Detail/StateGraphImplementation.h` and explicitly
instantiate the class in one translation unit, with those CPU dependencies
available there. BasicRenderer now uses that same algorithm through its wrapper.
The engine retains synchronous APIs for legacy hosts; render-thread consumers
must use posted requests and completion mailboxes instead of calling those APIs.
Existing SARP telemetry names are preserved during extraction.

Renderer-free graph tests live in `tests/StateGraph`. They accept
`ORG_ASYNC_TEST_TELEMETRY_SOURCE` for a source checkout of BasicTelemetry, or
`ORG_ASYNC_TEST_USE_INSTALLED=ON` to test only the installed graph package.

A publication exchange does not validate GPU readiness or reserve execution slots; its consumer
must do that in its nonblocking acceptance predicate before claiming native work.

## Growable buffer sets

`GrowableBufferSet.h` holds growable GPU buffers that one sealed set names (a host's snapshot): requirement -> one growth batch as
state-graph work -> the next sealed set. A recording made against a sealed set binds only versions that set names.

- **Register** (any thread, lock-free: a Treiber push). `GrowableRegistration` is a `VersionedBuffer` (held weakly: an expired buffer
  leaves the set), its `BufferExtent` (fixed bytes plus bytes per unit, and a structured stride), a `GrowthPolicy` and a contents
  mode. `LatchRegistration` is an `org::LatchBlock` with its frame slots and a policy. The handle is valid at once.
- **Build and Seal** (the producer's alone, one thread). `Begin()` takes in registrations and a settled batch; the `Build` collects
  requirements: `Require(handle, units)` (max-merged), `RequireNewer(handle, serial)` (a relayout at the same size) and
  `Require(latch, bytes, layoutKey)`, with `Units`, `Asked` and `Serial` to read what the set holds. `Seal(build)` names the newest
  ready versions when they meet every requirement; while nothing changed it returns the very same `NamedVersions` (shared pointer,
  `Serial()` and `RevisionFragment`), so recordings stay reused. Otherwise it posts one batch for every unmet buffer through the
  hooks' `post` and is pending until the batch settles (the hooks' `wake`). A requirement it cannot meet is never sealed: no caps,
  no fallback.
- **Latch blocks** are made inline at a seal, never pending: another layout key is a new block (so a frame in flight never reads one
  laid out anew), a block outgrown at the same key grows by its policy. A replaced block moves the sealed set's `Serial`.
- **`NamedVersions`** (immutable, an `IResourceVersions`): `Find` for the `VersionedBuffer` resolvers, `Of(handle | buffer)` ->
  `NamedEntry{version, serial, units, bytes, deviceAddress, srvIndex, contentsStamp}` (capacity read from the version itself),
  `Latch(handle)`, `AdoptAll()` (the frame's owner thread makes every named version current) and `Misses()` (diagnostics: resolves of
  buffers the set does not name). Nothing flows from the frame back to the producer.
- **Contents** (`GrowableContents`, what a new version holds when it turns ready): `Fresh` (nothing), `Zero` (zero-filled through the
  uploader; a Zero registration's first version is never named), `Capture` (the producer's `GrowableCapture` lays it out from
  immutable state, with that state's contents stamp).
- **The contents rule** (`FilledVersion`, `FilledContents<Stamp>`): a writer keeps the version it filled; before it writes, a named
  version it did not fill is written whole (or from the Capture stamp), then recorded. No adoption callback resets anything.
- **With the graph.** `Async/VersionGrowthArtifact.h`: `RegisterVersionGrowthProducer` registers the host's version-growth artifact
  kind; `PostVersionGrowth` posts a batch (lock-free) as an exact request whose producer makes the versions and queues their fills on
  the dedicated uploader (`GrowVersions`), ready once those copies completed. A sealed set's `RevisionFragment` goes in a
  `RevisionAssembler` slot, so a revision's recordings require exactly the set they were made against.
- **Pooled versions.** Growth allocates from a `BackingPool` (below): `GrowableRegistration::pool`, else the set's
  `GrowableBufferSetHooks::pool` (Register throws when both are null: no fresh buffers). `VersionedBufferGrowth` leases a backing of
  the policy's class (`BufferExtent::CapacityClassFor`, made like the registration's buffer through `BackingRequest::like`), wraps it
  as the version (`VersionedBuffer::Make`), and the version owns the lease: releasing its last reference returns the backing. A pool
  serves one set (its batches run in turn: the single acquirer). Fills are `VersionFill`s (`VersionFill.h`: segments gathered at an
  offset; `ZeroFill(bytes)` zeros the whole backing from one static page, so a reused backing keeps none of its older bytes).
- **Binding.** The frame's owner adopts the selected set (`AdoptAll`) and, after the frame's last submission, binds it
  (`NamedVersions::Bind(safeAt)`) with the clock point that retires once that submission completes on the GPU (CS:
  `DCLF::SubmittedRetirementPoint()`), while it still holds the set. Binding at the frame's start would let a backing return while
  that frame's GPU work still reads it. The growth's own fills need no bind: a version is named only once its copies completed.
- **Policies.** `GrowthPolicy.h`: `Doubling(min)` and `Headroom(pct, min)`; `Next` never clamps and throws past 64 bits or the
  capacity's type.
- **Shrinking.** `ElasticCapacity.h`: `ShrinkPolicy` (`Never` by default; `WhenBelow(pct, settle, observations)`) and its
  `ShrinkWindow`. A capacity asked at most `pct` of for at least `settle` and `observations` asks shrinks to the growth policy's fresh
  size for the window's peak. `GrowableRegistration::shrink` (a smaller version in a batch that does not hold the seal) and
  `VersionedBufferBuildInput::shrink` (a Replace into a smaller class; a predecessor copy reaches only the version's used bytes, and a
  backing smaller than them fails the build) apply it; `FitCapacity`/`ElasticCapacity` apply it to a host's own capacities.

The CPU tests (the policies, extents, requirements, the contents rule and growth's use of the pool) are in `tests/Growth`. They build
`BackingPool` from source and need only OpenRenderGraph's header-only `StreamingUploadTypes.h` (`ORG_GROWTH_TESTS_ORG_INCLUDE`):

```sh
cmake -S tests/Growth -B build/growth-tests
cmake --build build/growth-tests --config Release
ctest --test-dir build/growth-tests -C Release --output-on-failure
```

## Versioned-buffer pool

Pooled GPU backings reused once retired (BasicRenderer's `VersionedGpuBufferBackingPool`, generalised; namespace `org::services`):
a version is made into a backing leased from a pool and filled by journal catch-up from what that backing already holds; the
backing is reused only after the frames that used it retired and its last lease went. Nothing waits on the GPU. CS uses it for
DCLF's payload and FrameValues and for growable-set growth; BasicRenderer uses it through its adapter
(`BasicRenderer/Streaming/VersionedGpuBuffer.h`).

- **`RetirementClock`** (`BackingPool.h`; shared_ptr, lock-free, any thread): a host's monotonic completed point (a host frame, a
  renderer frame count). `Advance(completed)` publishes it atomically and runs the waiters it reached (a Treiber stack);
  `Completed()`, `Reached(point)`; `Subscribe(point, wake)` is a one-shot wake. One clock per host (CS: DCLF's, advanced from the
  graph host's completed-frame callback; BasicRenderer: one per renderer). It replaces BasicRenderer's process-global
  `NotifyVersionedGpuBufferFrameRetirement`.
- **`Backing`**: fixed `buffer`, `generation`, `capacityClass`, `byteCapacity`, `address`, `srvIndex`, `stride`, flags and
  compatibility; atomics `contentEpoch` (the journal position its contents hold; 0: nothing), `safeAt`, `leases`, `retired`,
  `readbackPins`, `bindingConsumers`; `image` (the lease holder's `BufferImage`; BasicRenderer).
- **`BackingRequest`**: `capacityClass` (elements of `stride`, or bytes when `stride` is 0; the caller's size class), `stride`,
  `BackingFlags` (`unorderedAccess`, `indirectArguments`, `deviceAddress`), a host `compatibility` key, `name`, and an optional
  `like` (a buffer whose kind the allocation copies). Backings are reused only for an equal class, stride, flags and compatibility.
- **`BackingPoolHooks`**: `allocate` (required; BasicRenderer: `CreateIndexedStructuredBuffer`, CS: an `org::Buffer` and its SRV;
  throws when it cannot: no fallback), `tag` (memory attribution), `telemetry(BackingPoolEvent, name, value)` (any thread),
  `env` (a portable environment read replacing `_dupenv_s`), `exclusive(body)` (runs the acquirer-side bodies inside the host's
  serialization: BasicRenderer's moved pool mutex; empty in CS) and `retire` (takes a released backing's buffer so the host picks
  the destroying thread; CS: not the render thread).
- **`BackingPool::Create(hooks, clock, Config)`**: `Config{name, maxPerClass (0: no cap; CS), framesInFlight (the host's, for
  callers' safety windows), spareFree (free backings of the asked class kept warm)}`.
  - **Single acquirer.** `Acquire`, `Trim`, `OldestContentEpoch` and `Visit` run on one thread at a time (a documented contract,
    asserted in debug builds); there is no mutex inside. A host with several acquirers serializes them through `exclusive`.
  - **`Acquire(request, preferEpoch, info)`**: a backing is reusable when it has no lease, no binding consumer and no readback pin,
    and `clock.Completed() >= safeAt`. Among those it prefers the highest `contentEpoch <= preferEpoch` (the least catch-up), else
    it allocates; it releases free backings of other classes and spares past `spareFree`. The lease is empty only when
    `maxPerClass` is reached: the caller suspends through `SubscribeAvailability(identity, wake)`, which runs once a backing may be
    reusable (a lease returned, a pin released, the clock passing a free backing's `safeAt`; `ArtifactSuspension::Capacity`).
  - **`OldestContentEpoch()`**: the oldest journal position a backing in circulation holds, ignoring backings that hold nothing:
    a journal's trim floor. Also `PinBindingConsumer` (a binding consumer's pin, refused for a backing already retired),
    `Count()` (census), `Trim()` and `Visit()`.
- **`BackingLease`**: copyable (each copy counts); the last one released, on any thread, returns the backing lock-free through a
  Treiber return stack the acquirer drains. `Bind(safeAt)` (any thread, max-merged): not reused before the clock reaches `safeAt`,
  so a frame binds the point that retires with it and the backing stays immutable for the GPU after the last lease goes.
- **`BufferImage`** (`BufferImage.h`): an immutable paged CPU image of a buffer's contents, in 64 KB pages shared between images
  (BasicRenderer's `VersionedGpuBufferImage`).
- **`BufferJournal`** (`BufferJournal.h`; BasicRenderer's `VersionedGpuBufferJournal`, its mutex moved): a manager's desired state
  (a paged image plus the writes since the acknowledged version); `CaptureDesired()` yields an immutable `Capture` for the
  producer. With `SetBackingPool(pool)`, `Acknowledge` keeps the writes back to min(acknowledged sequence,
  `pool.OldestContentEpoch()`), so a reused backing at an older `contentEpoch` still catches up instead of being written whole.
  BasicRenderer only; DCLF keeps its `KeptView`/`ChangeJournal`.
- **`VersionedBufferBuild.h`**: `VersionedBufferBuildInput` (a complete image, or the predecessor plus the journal's writes since
  `journalBaseSequence`; `backingPool` required; the host's identity as an opaque `tag`) and `PublishedBufferVersion` (owns its
  backing's lease; `hostState` for host-seeded state that holds no consumer pin). `BuildVersionedBuffer(input, context)` acquires
  (the predecessor's class for a Patch, a grown class for a Replace, preferring the backing nearest the request's sequence),
  uploads what changed since that backing's `contentEpoch` (or everything) as `PooledBackingLease` writes, sets `contentEpoch` and
  `image`, and returns Ready with a readiness token, or Suspended on an exhausted pool; it never throws.
  `ReplayVersionedBuffer` is the pure byte replay shared with tests; `TokenForTickets` is the one readiness token over upload
  tickets.
- **`Async/VersionedBufferProducer.h`**: `RegisterVersionedBufferProducer<Binding>(graph, kinds, lane, domain, taskName, hooks)`
  runs `BuildVersionedBuffer` as graph work (modelled on `RegisterVersionGrowthProducer`): ready with its uploads, suspended
  (`Capacity`) until the pool's availability wakes the graph. Hooks: `decorate` (seeds `hostState` before the version is frozen)
  and `wrap` (the artifact payload; BasicRenderer: a `RendererStateFragmentArtifact`).
- **`Async/VersionedBufferFamily.h`**: `VersionedBufferFamily<Binding>`, the request authority for one persistent buffer address
  (snapshot, GPU-written, content-derived and journal-capture requests, plus lock-free `Post*` variants), generic over the host's
  `Requests` service (`SubmitLatest`, `PostLatest`). `Acknowledge(version)` makes the version the successor's predecessor and binds
  the superseded backing to `Completed() + 2·framesInFlight + 1` (`Config::framesInFlight`, no longer a hardcoded 3).
- **`JournaledBufferSet.h`**: buffers published together by journal catch-up (the DCLF payload model). There is one pool per part
  (`JournaledPartConfig{name, stride, flags, policy}`; a part's class only grows). `Publish(requests, uploads)` (single
  publisher) leases a backing per present part (`preferEpoch`: the journal's position) and calls the part's
  `emit(target, sink)`. The emit sends every run since `target.contentEpoch` (everything at 0) through the `JournaledSink`
  (`PooledBackingLease` uploads; the data is copied when queued), patches rows that hold their own address with
  `target.address`, writes whole tails whole, and returns the new `contentEpoch` (0: written whole next time). The
  `JournaledPublication` holds the leases and a `ready` token over the uploads; `Bind(safeAt)` binds every part, `Discard` marks a
  failed publication's backings as holding nothing, and `OldestContentEpoch(part)` is the part's trim floor.

The buffer tests are in `tests/Buffers`. `ORGBufferPoolTests` needs only these headers (the image, journal, clock and pool, and
the public headers' self-containment). `ORGBufferBuildTests` (`BuildVersionedBuffer`, `ReplayVersionedBuffer`,
`JournaledBufferSet` and `VersionedBufferFamily` through a recording upload service) also needs OpenRenderGraph's third-party
headers (e.g. CommunityShaders' vcpkg tree) and the Vulkan SDK; it is skipped without them.

```sh
cmake -S tests/Buffers -B build/buffers-tests
cmake --build build/buffers-tests --config Release
ctest --test-dir build/buffers-tests -C Release --output-on-failure
```

The only mandatory library dependency is `BasicRHI::BasicRHI`. DXC is discovered from `dxcapi.h`, loaded dynamically, and can be disabled with `ORG_MODULE_SERVICES_ENABLE_DXC=OFF`. Vulkan headers and libraries are not required unless a host separately enables a Vulkan implementation.

Consumers use the exported `ORGModuleServices::ORGModuleServices` CMake target. Hosts install these services into their graph execution context; graph scheduling remains entirely owned by ORG.
