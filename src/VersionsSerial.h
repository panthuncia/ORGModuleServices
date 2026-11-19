#pragma once

#include <cstdint>

namespace org::services::detail {

// The next serial of a set of versions: NamedVersions and ComposedVersions draw from one process-wide sequence.
std::uint64_t NextVersionsSerial() noexcept;

} // namespace org::services::detail
