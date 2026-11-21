# ORGModuleServices

Optional module-facing services shared by OpenRenderGraph hosts. OpenRenderGraph does not depend on this package.

Provided services:

- Exact-fence-retired D3D12 upload allocations and shader-visible descriptor heaps.
- A versioned-buffer pool: GPU backings leased per version, filled by copying what they lack of the predecessor plus the delta, and reused once their frames retired on a host retirement clock.
- Content-addressed DXC compilation with memory/disk artifacts and deduplicated in-flight requests.
- DXIL targets by default and optional SPIR-V targets through `ORG_MODULE_SERVICES_ENABLE_VULKAN`.
- One-shot asynchronous pipeline builds (`BuildPipeline`) and live pipeline slots for hot reload, with pinned versions and retirement (`PipelineRegistry`). Graph builds that pin a generation, carried in a recording tag (`PipelinePins`, `VersionFor`).
- Stage reflection (`ShaderReflection.h`): inputs, used bindings, constant buffer members and outputs of DXC's SPIR-V (read word by word) and DXIL (through the loaded dxcompiler.dll, on Windows), with DXIL registers shifted onto the host's SPIR-V binding numbers. Tests: `tests/Reflection`.
- Runtime-compiled programs (`Programs/`): `ComputeProgram` (a `ComputeKernel`) and `RasterProgram`, built from HLSL by
  the shader compiler into live pipeline slots, and `ProgramService`, which builds them several at once on an executor
  and gates on them without waiting. The host gives the shader root, the backend, its defines and the pipeline version
  a recording binds through `ProgramEnvironment` (`Programs/ProgramBuild.h`).
- Staged graph builds off the owner thread (`StagedBuilds`): rebuild requests are built on an executor lane, attached or staged at frame boundaries, and staged sets attach as one. Extensions defer work to a build's staging, attach or failure (`BuildContext.h`). Immutable build inputs with a serial, and which one is built and which attached (`BuildInputs.h`).
- Header-only async primitives: serialized task pumps, leased immutable array slots, and a bounded single-producer/single-consumer publication exchange.
- Header-only containers in `Async/`: a latest-wins mailbox for any number of producers (`LatestSlot`), a lossless single-producer log with several consumer cursors (`PublicationLog`), generational slot tables freed after their last reference (`SlotTable`), and kept arrays: copy-on-write chunked arrays with change journals, so each mirroring buffer is sent only what it lacks (`KeptState.h`).
- A host frame clock with the host's slot count (`Async/FrameClock.h`), a starvation rule for work asked for with no new input (`Async/StarvationWatch.h`), and a violation policy that makes rule violations fatal or counted (`ViolationPolicy.h`).
- Ownership-based deferred reclamation (`RetirementChain.h`): batches return once every publication that may name them is gone, and `Retired<T>` hands the last drop of a publication to another thread. It queues through OpenRenderGraph's `EventQueue`.
- GPU passes in `Passes/` with their shaders in `shaders/`: prefix sum, sort scatter, a min or max HZB (FidelityFX SPD), UAV clears, mip generation and multisample resolve. `shaders/FullscreenVS.hlsl` is the fullscreen-triangle vertex shader (`RasterProgram::kFullscreenVertexShader`).
- Texel encoding for textures born with a value (`TexelEncoding.h`), and buffer chains that build a host's per-publication buffers as `VersionedBufferBuild` versions (`BufferChains.h`).
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

The thread-pool scheduler is `ORGModuleServices::AsyncExecutor` (`cmake/AsyncExecutor.cmake`, CPU only, needs TBB):

- `Async/Executor.h`: `org::async::Executor`, a `GraphScheduler` with per-domain worker pools, unbounded queues, timers,
  cancellable scopes, `ParallelFor` with priority-boosted helpers, and frame-critical jobs that go first and can have
  threads of their own. The domains, worker counts, thread start hook and priority callbacks come from `Executor::Desc`;
  the default layout has a coordinator thread, a parallel pool, a serial thread and a small blocking pool.
- `Async/Lanes.h`: `Lane` (one serial pass at a time, wakes coalesce), `JobLane<Job>` (posted jobs in order) and
  `MakeLane` (a lane for the process's life), on an executor. `PlanWaitingPool` sizes a pool whose jobs wait for other
  work in the same domain, so that work always has a thread.
- `Async/ExecutorTaskService.h` (in `ORGModuleServices`, which links OpenRenderGraph): OpenRenderGraph's `ITaskService`
  over an executor. A task that throws is reported through the host's `onTaskError` callback.
The CPU sphere cull (`Cpu/SphereCull.h`) is the separate `ORGModuleServices::SphereCull`
target, from `cmake/SphereCull.cmake`. It tests sphere columns against a plane set with
SSE2, AVX2 or AVX-512 kernels, picked at run time from CPUID, and every kernel gives the
same verdicts. Its tests are in `tests/Cpu`.

The Dear ImGui renderer backend (`ImGui/ImGuiBackend.h`) is the optional `ORGModuleServices::ImGui`
target, from `cmake/ImGuiBackend.cmake` or `ORG_MODULE_SERVICES_ENABLE_IMGUI`. ImGui comes from the host: its target
(`ORG_MODULE_SERVICES_IMGUI_TARGET`, `imgui::imgui` by default) or the folder holding `imgui.h`
(`ORG_MODULE_SERVICES_IMGUI_INCLUDE_DIR`). The host builds the draw program from `ProgramDesc` and passes it in, and
ships `shaders/ImGui.hlsl` with the other pass shaders. Its device-free tests are in `tests/ImGui`.

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

## Shader store

`ORGModuleServices::ShaderStore` is a persistent shader cache that needs no renderer, RHI or compiler. A saved shader
is used only when every file it read last time still has the same contents, so file times do not matter and a
rebuild has a stated reason. `ShaderCompiler` uses it for DXC, and a host can give it other compilers through
`ShaderVariantBuild`. Tests: `tests/ShaderStore` (engine, with a fake compiler) and `tests/Services` (real DXC).

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
  (`NamedVersions::Bind(safeAt)`) with the clock point that retires once that submission completes on the GPU, while
  it still holds the set. Binding at the frame's start would let a backing return while
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

## Commit uploads

`CommitUploads.h` (namespace `org::services`) writes a commit's bytes on the upload service's owner thread:

- **`CommitUploads`** stages each write into a pooled `StagedUploadBatch` (`AcquireStagedBatch`) and submits the batch to the
  active upload service when it goes. A versioned buffer is written in the version the adopted `NamedVersions` set names.
- **`LatchedUploads`** writes per-run values into a `LatchedList`'s copies in the run's latch region; a value with no copy is staged
  by its `CommitUploads`. `Finish` zeroes the copies not written.
- **`OwnedRow<Row, Binding>`** (`OwnedRows.h`) is a GPU row plus the owners of the descriptors its `DescriptorIndex` fields name.
  `Binding` is the host's descriptor binding (an index, an owner, a validity test; `OwnedBinding` by default).
- **`ContentOwners`** (`ContentOwners.h`) tracks those owners by target key and byte range. `Snapshot(seal)` gives one owner for an
  execution to hold; the host's `seal` decides how they are released.
- The host passes `CommitUploadHooks`: `onError` for an error a write can't return (a row naming descriptors uploaded with no
  content owners), and an optional `IUploadBytesSink` for byte statistics.

The CPU tests (owned rows, content owners) are in `tests/Uploads`.

## Versioned-buffer pool

Pooled GPU backings reused once retired (BasicRenderer's `VersionedGpuBufferBackingPool`, generalised; namespace `org::services`):
a version is made into a backing leased from a pool and filled from what that backing already holds (a predecessor copy and the
delta); the backing is reused only after the frames that used it retired and its last lease went. Nothing waits on the GPU. Hosts use it for
per-frame payloads, and growable-set growth uses it too; BasicRenderer uses it through its adapter
(`BasicRenderer/Streaming/VersionedGpuBuffer.h`).

- **`RetirementClock`** (`BackingPool.h`; shared_ptr, lock-free, any thread): a host's monotonic completed point (a host frame, a
  renderer frame count). `Advance(completed)` publishes it atomically and runs the waiters it reached (a Treiber stack);
  `Completed()`, `Reached(point)`; `Subscribe(point, wake)` is a one-shot wake. One clock per host (advanced, for example, from the
  graph host's completed-frame callback; BasicRenderer: one per renderer). It replaces BasicRenderer's process-global
  `NotifyVersionedGpuBufferFrameRetirement`.
- **`Backing`**: fixed `buffer`, `generation`, `capacityClass`, `byteCapacity`, `address`, `srvIndex`, `stride`, flags and
  compatibility; atomics `contentEpoch` (the write position its contents hold; 0: nothing), `safeAt`, `leases`, `retired`,
  `readbackPins`, `bindingConsumers`.
- **`BackingRequest`**: `capacityClass` (elements of `stride`, or bytes when `stride` is 0; the caller's size class), `stride`,
  `BackingFlags` (`unorderedAccess`, `indirectArguments`, `deviceAddress`), a host `compatibility` key, `name`, and an optional
  `like` (a buffer whose kind the allocation copies). Backings are reused only for an equal class, stride, flags and compatibility.
- **`BackingPoolHooks`**: `allocate` (required; BasicRenderer: `CreateIndexedStructuredBuffer`, `GraphHostBackingHooks`: an `org::Buffer` and its SRV;
  throws when it cannot: no fallback), `tag` (memory attribution), `telemetry(BackingPoolEvent, name, value)` (any thread),
  `exclusive(body)` (runs the acquirer-side bodies inside the host's
  serialization: BasicRenderer's pool mutex; empty for a single acquirer) and `retire` (takes a released backing's buffer so the
  host picks the destroying thread, such as one that is not the render thread).
- **`BackingPool::Create(hooks, clock, Config)`**: `Config{name, maxPerClass (0: no cap), framesInFlight (the host's, for
  callers' safety windows), spareFree (free backings of the asked class kept warm)}`.
  - **Single acquirer.** `Acquire`, `Trim`, `OldestContentEpoch` and `Visit` run on one thread at a time (a documented contract,
    asserted in debug builds); there is no mutex inside. A host with several acquirers serializes them through `exclusive`.
  - **`Acquire(request, preferEpoch, info)`**: a backing is reusable when it has no lease, no binding consumer and no readback pin,
    and `clock.Completed() >= safeAt`. Among those it prefers the highest `contentEpoch <= preferEpoch` (the least catch-up), else
    it allocates; it releases free backings of other classes and spares past `spareFree`. The lease is empty only when
    `maxPerClass` is reached: the caller suspends through `SubscribeAvailability(identity, wake)`, which runs once a backing may be
    reusable (a lease returned, a pin released, the clock passing a free backing's `safeAt`; `ArtifactSuspension::Capacity`).
  - **`OldestContentEpoch()`**: the oldest write position a backing in circulation holds, ignoring backings that hold nothing:
    a host change journal's trim floor. Also `PinBindingConsumer` (a binding consumer's pin, refused for a backing already retired),
    `Count()` (census), `Trim()` and `Visit()`.
- **`BackingLease`**: copyable (each copy counts); the last one released, on any thread, returns the backing lock-free through a
  Treiber return stack the acquirer drains. `Bind(safeAt)` (any thread, max-merged): not reused before the clock reaches `safeAt`,
  so a frame binds the point that retires with it and the backing stays immutable for the GPU after the last lease goes.
- **`RetirementJoin`** (`RetirementJoin.h`; std and ORG's `EventQueue` only): one `RetirementClock` over two sources, a graph
  host's submissions and a CPU pipeline's frame timeline, read through a `Source` interface. `SubmittedPoint()` (submitting thread)
  notes a point that counts both; it retires once both passed it. The host calls `NoteGraphSubmissionsRetired` (its retired
  callback), `NoteCpuFrameOpened`/`NoteCpuFrameSignalled`/`SealCpuFrames` (the CPU frames) and `Poll` (any thread, one run at a
  time, nobody waits). Before the first CPU frame the points are the host's submission counts.
- **`GraphHostRetirement`** (`GraphHostRetirement.h`): the join with a graph host as its source (`GraphHostAccess`, a callback that
  returns the host or null), the host's chosen CPU frame timeline (`SetCpuFrameTimeline`), and the GPU points behind the points:
  `SubmittedGpuPoint`, `SealedGpuPoint` and `Sealed(point)`.
- **`GraphHostBackings.h`**: default pool hooks for a graph host (`GraphHostBackingHooks`: allocate without an ECS entity, retire on
  the host's cleanup lane) and the pool defaults `PerFrameSpares(framesInFlight)`, `PerFrameCopies(asked, framesInFlight)` and
  `BackingShrinkPolicy(BackingShrink)`.
- **`RecordingPools.h`** (std and ORG's `EventQueue` only): `BumpArena` (a frame slot's CPU memory in chunks that never move, reset
  at the slot's retirement), `ResetPools<Pool>` (pools reset off the owning thread through a host `Defer` callback; any thread
  hands in, one takes) and `RecordingSlot<Pool>` (an arena per worker plus one pool; `Retire` resets the arenas and recycles the
  pool). **`SecondaryListPool.h`**: the pool of secondary command lists from one allocator that `RecordingSlot` holds.
- **`VersionedBufferBuild.h`**: `VersionedBufferBuildInput` (the predecessor, the host's `changedSince` and `emitDelta`;
  `backingPool` required; the host's identity as an opaque `tag`) and `PublishedBufferVersion` (owns its backing's lease;
  `hostState` for host-seeded state that holds no consumer pin). `BuildVersionedBuffer(input, context)` acquires (the
  predecessor's class for a Patch, a grown class for a Replace, preferring the backing nearest the predecessor's sequence),
  GPU-copies what that backing lacks of the predecessor, uploads the delta as `PooledBackingLease` writes, sets `contentEpoch`,
  and returns Ready with a readiness token, or Suspended on an exhausted pool; it never throws. `TokenForTickets` is the one
  readiness token over upload tickets. The host keeps the change journal.
- **`Async/VersionedBufferProducer.h`**: `RegisterVersionedBufferProducer<Binding>(graph, kinds, lane, domain, taskName, hooks)`
  runs `BuildVersionedBuffer` as graph work (modelled on `RegisterVersionGrowthProducer`): ready with its uploads, suspended
  (`Capacity`) until the pool's availability wakes the graph. Hooks: `decorate` (seeds `hostState` before the version is frozen)
  and `wrap` (the artifact payload; BasicRenderer: a `RendererStateFragmentArtifact`).
- **`Async/VersionedBufferPosts.h`**: `PostVersionedBuffer` posts one version of that producer (lock-free, without a `wrap` hook) at
  an address of its own, and runs the poster's `done` on its lane at GPU readiness with the version or why there is none.
- **`Async/WindowGates.h`**: a resource's window made as graph work, a newer revision replacing an unstarted one, and gates that run a
  request once a window reaches a revision and is GPU ready (`ReadyGateAtLeast`), each released after it ran.
- **`Passes/IndirectRuns.h`**: CPU-recorded indirect draws coalesced into runs of one pipeline (`IndirectRunCounts`,
  `IndirectRunWriter` on the host's bucket binder, `RecordIndirectRuns`), and `OrderBySlabs` for depth-only draw order. The host
  keeps its draw record's layout. `ORGIndirectRunsTests` needs no device.
- **`shaders/OrgResources.hlsli`**: one resource list per shader, expanded to register-bound declarations (FXC) or push-block
  handles read from the descriptor heap (`ORG_BINDLESS`). Its C++ side is OpenRenderGraph's `Render/ShaderBindings.h`.

The buffer tests are in `tests/Buffers`. `ORGBufferPoolTests` needs only these headers (the clock and pool, and the public
headers' self-containment). `ORGRetirementTests` (the join, the recording pools and the pool defaults) also needs
OpenRenderGraph's header-only `EventQueue.h` (`ORG_RETIREMENT_TEST_ORG_INCLUDE`). `ORGBufferBuildTests` (`BuildVersionedBuffer` through a recording upload service) also needs
OpenRenderGraph's third-party headers (e.g. a host's vcpkg tree) and the Vulkan SDK; it is skipped without them.

```sh
cmake -S tests/Buffers -B build/buffers-tests
cmake --build build/buffers-tests --config Release
ctest --test-dir build/buffers-tests -C Release --output-on-failure
```

The only mandatory library dependency is `BasicRHI::BasicRHI`. DXC is discovered from `dxcapi.h`, loaded dynamically, and can be disabled with `ORG_MODULE_SERVICES_ENABLE_DXC=OFF`. Vulkan headers and libraries are not required unless a host separately enables a Vulkan implementation.

Consumers use the exported `ORGModuleServices::ORGModuleServices` CMake target. Hosts install these services into their graph execution context; graph scheduling remains entirely owned by ORG.
