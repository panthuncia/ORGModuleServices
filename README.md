# ORGModuleServices

Optional module-facing services shared by OpenRenderGraph hosts. OpenRenderGraph does not depend on this package.

Provided services:

- Exact-fence-retired D3D12 upload allocations and shader-visible descriptor heaps.
- Content-addressed DXC compilation with memory/disk artifacts and deduplicated in-flight requests.
- DXIL targets by default and optional SPIR-V targets through `ORG_MODULE_SERVICES_ENABLE_VULKAN`.
- Backend-neutral pipeline recipes, asynchronous deduplicated construction, family generations, and fence retirement.

The only mandatory library dependency is `BasicRHI::BasicRHI`. DXC is discovered from `dxcapi.h`, loaded dynamically, and can be disabled with `ORG_MODULE_SERVICES_ENABLE_DXC=OFF`. Vulkan headers and libraries are not required unless a host separately enables a Vulkan implementation.

Consumers use the exported `ORGModuleServices::ORGModuleServices` CMake target. Hosts install these services into their graph execution context; graph scheduling remains entirely owned by ORG.
