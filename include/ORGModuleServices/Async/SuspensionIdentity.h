#pragma once

#include <cstdint>

namespace org::async {

// Shared by all graph instances and artifact kinds linked to this runtime.
// Hosts in different DLLs must route through one runtime owner if they exchange
// suspension notifications; independently linked static copies are not a shared
// cross-module namespace.
[[nodiscard]] std::uint64_t AllocateArtifactSuspensionIdentity() noexcept;

} // namespace org::async
