#pragma once

#include <functional>
#include <memory>

namespace org {
class Buffer;
}

namespace org::services::detail {

// Installs a pooled backing's binding-consumer pin factory on its buffer (Resource::SetSemanticConsumerLeaseFactory). The only
// use of org::Buffer's definition by the pool, kept in its own translation unit (BackingPoolBinding.cpp) so the pool itself builds
// against MS headers alone (standalone tests link tests/Buffers/NoBufferBinding.cpp instead).
void InstallConsumerLeaseFactory(Buffer& buffer, std::function<std::shared_ptr<const void>()> factory);

} // namespace org::services::detail
