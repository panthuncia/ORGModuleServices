# ORGModuleServices

Optional module-facing services shared by OpenRenderGraph hosts. OpenRenderGraph does not depend on this package.

Provided services:

- Exact-fence-retired D3D12 upload allocations and shader-visible descriptor heaps.
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
- **Frame transport.** `TransportBuffer.h`: `EnsureTransport` / `ReplaceTransport` for buffers a frame's producer fills and its work
  is handed by value (rings, staging), never versioned or named; a replaced buffer goes to the caller's retire, released off the
  render thread. `GrowthPolicy.h`: `Doubling(min)` and `Headroom(pct, min)`; `Next` never clamps and throws past 64 bits or the
  capacity's type.

The CPU tests (the policies, extents, requirements and the contents rule) are in `tests/Growth`.

The only mandatory library dependency is `BasicRHI::BasicRHI`. DXC is discovered from `dxcapi.h`, loaded dynamically, and can be disabled with `ORG_MODULE_SERVICES_ENABLE_DXC=OFF`. Vulkan headers and libraries are not required unless a host separately enables a Vulkan implementation.

Consumers use the exported `ORGModuleServices::ORGModuleServices` CMake target. Hosts install these services into their graph execution context; graph scheduling remains entirely owned by ORG.
