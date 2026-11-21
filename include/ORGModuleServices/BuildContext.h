#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace org::services {

// What one graph build's extensions deferred to its staging, its attach or its failure, and the input it is made from.
// StagedBuilds opens a BuildScope around each build, so an extension reaches it through CurrentBuild on the building thread.
struct BuildContext {
    std::vector<std::function<void()>> attached;
    std::vector<std::function<void(const std::string&)>> failed;
    std::vector<std::function<void()>> staged;  // run when the graph is staged
    std::shared_ptr<const void> input;           // what the build is made from, taken when it was handed out
};

// The calling thread's build. Null outside one.
BuildContext* CurrentBuild() noexcept;

// Makes a context the calling thread's build while it lives. Scopes nest: the outer one comes back at the end.
class BuildScope {
public:
    explicit BuildScope(BuildContext& context) noexcept;
    ~BuildScope();
    BuildScope(const BuildScope&) = delete;
    BuildScope& operator=(const BuildScope&) = delete;

private:
    BuildContext* previous_;
};

// Inside a build: attached runs when the graph is attached, failed when the build fails. Neither runs if the build is
// dropped. Returns false outside a build, where the caller applies its change at once.
bool DeferToAttach(std::function<void()> attached, std::function<void(const std::string&)> failed = {});

// Inside a build: staged runs when the graph is staged, before its attach. Returns false outside a build.
bool DeferToStaging(std::function<void()> staged);

} // namespace org::services
