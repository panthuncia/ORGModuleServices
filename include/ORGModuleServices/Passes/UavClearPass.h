#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace org {
class IResourceResolver;
class RenderPass;
class Resource;
class RunGate;
}

namespace org::services {

// The texture a clear writes: a resolver, such as a versioned texture, or else a plain resource. A pass builder declares
// the resolver when there is one (SelfDeclaringResource).
struct UavClearTarget {
    std::shared_ptr<const IResourceResolver> resolver;
    std::shared_ptr<Resource> resource;

    const IResourceResolver* DeclaredResolver() const { return resolver.get(); }
    std::shared_ptr<Resource> DeclaredResource() const { return resource; }
};

// A gate entry the clear waits for. A gated clear must be recorded at execution (ExternalPassDesc::RecordAtExecution): it
// skips the runs whose commit left the entry closed (RunGate::IsOpen).
struct UavClearGate {
    std::shared_ptr<const RunGate> gate;
    uint32_t entry = 0;
};

// Clears a texture to a float value through an unordered-access clear, on the graphics queue. The clear is the texture's use
// in the pass, so a transient whose first use is the clear stays aliasable. An empty target fails when the pass is declared.
std::shared_ptr<RenderPass> MakeUavClearPass(UavClearTarget target, std::array<float, 4> value, UavClearGate gate = {});

// MakeUavClearPass to a uint value, for integer formats.
std::shared_ptr<RenderPass> MakeUavClearPassUint(UavClearTarget target, std::array<uint32_t, 4> value, UavClearGate gate = {});

}
