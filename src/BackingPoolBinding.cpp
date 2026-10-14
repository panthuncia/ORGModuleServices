#include "BackingPoolBinding.h"

#include <utility>

#include "Resources/Buffers/Buffer.h"

namespace org::services::detail {

void InstallConsumerLeaseFactory(Buffer& buffer, std::function<std::shared_ptr<const void>()> factory) {
    buffer.SetSemanticConsumerLeaseFactory(std::move(factory));
}

} // namespace org::services::detail
