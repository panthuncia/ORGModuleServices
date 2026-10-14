// Standalone pool tests: their backings have no org::Buffer (the allocate hook returns none), so the pin factory is never
// installed. Stands in for src/BackingPoolBinding.cpp, which needs OpenRenderGraph's Buffer definition.
#include "../../src/BackingPoolBinding.h"

namespace org::services::detail {

void InstallConsumerLeaseFactory(Buffer&, std::function<std::shared_ptr<const void>()>) {}

} // namespace org::services::detail
