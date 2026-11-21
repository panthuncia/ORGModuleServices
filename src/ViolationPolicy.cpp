#include <ORGModuleServices/ViolationPolicy.h>

#include <algorithm>
#include <utility>

#include <spdlog/spdlog.h>

namespace org::services {

ViolationPolicy::ViolationPolicy(ViolationPolicyDesc desc)
    : m_desc(std::move(desc)), m_counts(m_desc.kinds, 0), m_logged(m_desc.kinds, false) {}

ViolationPolicy::Outcome ViolationPolicy::Note(std::uint32_t kind, std::string_view message) {
    m_total.fetch_add(1, std::memory_order_relaxed);
    if (m_desc.fatal) {
        if (m_desc.onFatal) m_desc.onFatal(message);
        return Outcome::Fatal;
    }
    if (kind >= m_desc.kinds) {
        spdlog::error("{}: violation kind {} is out of range ({} kinds): {}", m_desc.errorLabel, kind, m_desc.kinds, message);
        return Outcome::Invalid;
    }
    bool first = false;
    {
        const std::lock_guard lock(m_mutex);
        ++m_counts[kind];
        first = !m_logged[kind];
        m_logged[kind] = true;
    }
    if (!first) return Outcome::Counted;
    spdlog::error("{}: {} {}", m_desc.errorLabel, message, m_desc.firstNote);
    return Outcome::First;
}

bool ViolationPolicy::RaiseWarning(std::string_view kind) {
    {
        const std::lock_guard lock(m_mutex);
        if (std::find(m_warnings.begin(), m_warnings.end(), kind) != m_warnings.end()) return false;
        m_warnings.emplace_back(kind);
    }
    spdlog::warn("{}: {}. {}", m_desc.warningLabel, kind, m_desc.warningNote);
    return true;
}

std::vector<std::string> ViolationPolicy::Warnings() const {
    const std::lock_guard lock(m_mutex);
    return m_warnings;
}

std::uint64_t ViolationPolicy::Count(std::uint32_t kind) const {
    const std::lock_guard lock(m_mutex);
    return kind < m_counts.size() ? m_counts[kind] : 0;
}

} // namespace org::services
