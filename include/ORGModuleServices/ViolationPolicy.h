#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace org::services {

struct ViolationPolicyDesc {
    // Violation kinds are 0 to kinds - 1. The host keeps their names.
    std::uint32_t kinds = 0;
    // Fatal: every violation goes to onFatal. Otherwise violations are counted
    // and the first of each kind logs.
    bool fatal = false;
    // Gets a violation's message when fatal is set. After it returns, the
    // violation counts as fatal and nothing else happens.
    std::function<void(std::string_view message)> onFatal;
    // The first violation of a kind logs as an error: "<errorLabel>: <message> <firstNote>".
    std::string errorLabel = "violation";
    std::string firstNote = "Later violations of this kind are counted, not logged.";
    // A raised warning logs once, as "<warningLabel>: <kind>. <warningNote>".
    std::string warningLabel = "violation warning raised";
    std::string warningNote = "It stays raised for the session";
};

// One policy for rule violations a host may keep running past. Each violation
// is either fatal or counted. When counted, the first of each kind logs the
// full message and later ones only count. Warnings are named kinds raised for
// the rest of the session, for the host to show. Any thread.
class ViolationPolicy {
public:
    enum class Outcome : std::uint8_t {
        Fatal,    // went to onFatal
        First,    // the first of its kind: logged
        Counted,  // a later one: counted only
        Invalid,  // kind out of range: logged, not counted
    };

    explicit ViolationPolicy(ViolationPolicyDesc desc);
    ViolationPolicy(const ViolationPolicy&) = delete;
    ViolationPolicy& operator=(const ViolationPolicy&) = delete;

    [[nodiscard]] bool Fatal() const noexcept { return m_desc.fatal; }

    // One violation of kind, with its full message.
    Outcome Note(std::uint32_t kind, std::string_view message);

    // Raises the warning kind for the session. The first raise logs; later
    // ones change nothing. Returns true for the first.
    bool RaiseWarning(std::string_view kind);

    // The warnings raised, in the order they first happened.
    [[nodiscard]] std::vector<std::string> Warnings() const;

    // Violations of kind, and of every kind, since the policy was made.
    // Fatal ones are in the total only.
    [[nodiscard]] std::uint64_t Count(std::uint32_t kind) const;
    [[nodiscard]] std::uint64_t Total() const noexcept { return m_total.load(std::memory_order_relaxed); }

private:
    ViolationPolicyDesc m_desc;
    mutable std::mutex m_mutex;
    std::vector<std::uint64_t> m_counts;
    std::vector<bool> m_logged;
    std::vector<std::string> m_warnings;
    std::atomic<std::uint64_t> m_total{0};
};

} // namespace org::services
