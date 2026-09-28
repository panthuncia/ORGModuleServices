#include <ORGModuleServices/Async/SuspensionIdentity.h>

#include <atomic>

namespace org::async {
namespace {
std::atomic_uint64_t nextIdentity{1};
}

std::uint64_t AllocateArtifactSuspensionIdentity() noexcept {
    auto identity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
    while (identity == 0) {
        identity = nextIdentity.fetch_add(1, std::memory_order_relaxed);
    }
    return identity;
}

} // namespace org::async
