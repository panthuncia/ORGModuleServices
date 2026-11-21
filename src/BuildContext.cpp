#include <ORGModuleServices/BuildContext.h>

namespace org::services {

namespace {
thread_local BuildContext* t_build = nullptr;
}

BuildContext* CurrentBuild() noexcept { return t_build; }

BuildScope::BuildScope(BuildContext& context) noexcept : previous_(std::exchange(t_build, &context)) {}

BuildScope::~BuildScope() { t_build = previous_; }

bool DeferToAttach(std::function<void()> attached, std::function<void(const std::string&)> failed) {
    if (!t_build)
        return false;
    if (attached)
        t_build->attached.push_back(std::move(attached));
    if (failed)
        t_build->failed.push_back(std::move(failed));
    return true;
}

bool DeferToStaging(std::function<void()> staged) {
    if (!t_build)
        return false;
    if (staged)
        t_build->staged.push_back(std::move(staged));
    return true;
}

} // namespace org::services
