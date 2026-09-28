#include <ORGModuleServices/Async/SuspensionIdentity.h>

std::uint64_t AllocateFromOtherTranslationUnit() {
    return org::async::AllocateArtifactSuspensionIdentity();
}
