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

The only mandatory library dependency is `BasicRHI::BasicRHI`. DXC is discovered from `dxcapi.h`, loaded dynamically, and can be disabled with `ORG_MODULE_SERVICES_ENABLE_DXC=OFF`. Vulkan headers and libraries are not required unless a host separately enables a Vulkan implementation.

Consumers use the exported `ORGModuleServices::ORGModuleServices` CMake target. Hosts install these services into their graph execution context; graph scheduling remains entirely owned by ORG.
