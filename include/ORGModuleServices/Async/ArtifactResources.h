#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace org::async {

// Explicit ownership of an immutable artifact version. Identity values and
// dependency descriptions are deliberately non-owning so copying them into
// diagnostics, signatures, or tombstones cannot retain GPU resources.
class ArtifactLease {
public:
    ArtifactLease() = default;
    explicit ArtifactLease(std::shared_ptr<const void> token) : m_token(std::move(token)) {}

    void reset() noexcept { m_token.reset(); }
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_token); }
    [[nodiscard]] long use_count() const noexcept { return m_token.use_count(); }
    [[nodiscard]] const std::shared_ptr<const void>& Token() const noexcept { return m_token; }

private:
    std::shared_ptr<const void> m_token;
};

enum class ArtifactReadiness : std::uint8_t {
    Missing,
    Blocked,
    Queued,
    Preparing,
    CpuReady,
    UploadSubmitted,
    GpuReady,
    Published,
    Superseded,
    Cancelled,
    Failed,
};

[[nodiscard]] inline bool ArtifactReachedMilestone(
    ArtifactReadiness actual, ArtifactReadiness required) noexcept {
    if (actual == ArtifactReadiness::Published) return true;
    if (actual == ArtifactReadiness::Failed || actual == ArtifactReadiness::Cancelled ||
        actual == ArtifactReadiness::Superseded) return false;
    return static_cast<unsigned>(actual) >= static_cast<unsigned>(required);
}

class ArtifactPayload {
public:
    ArtifactPayload() = default;

    template <class T>
    static ArtifactPayload Make(std::shared_ptr<const T> value) {
        ArtifactPayload result;
        result.m_value = std::move(value);
        result.m_type = std::type_index(typeid(T));
        return result;
    }

    template <class T>
    [[nodiscard]] std::shared_ptr<const T> Get() const {
        if (m_type != std::type_index(typeid(T))) return {};
        return std::static_pointer_cast<const T>(m_value);
    }

    [[nodiscard]] bool Valid() const noexcept { return static_cast<bool>(m_value); }
    [[nodiscard]] std::type_index Type() const noexcept { return m_type; }

private:
    std::shared_ptr<const void> m_value;
    std::type_index m_type{ typeid(void) };
};

struct GpuQueueSubmission {
    std::shared_ptr<const void> timelineOwner;
    std::uint64_t value = 0;
    std::function<std::shared_ptr<const void>()> currentTimelineOwner;
    std::function<std::uint64_t()> currentValue;

    [[nodiscard]] std::shared_ptr<const void> TimelineOwner() const {
        return currentTimelineOwner ? currentTimelineOwner() : timelineOwner;
    }
    [[nodiscard]] std::uint64_t TimelineValue() const {
        return currentValue ? currentValue() : value;
    }
};

struct GpuSubmissionSet {
    // Queue timeline/value pairs are intentionally backend-opaque. Published
    // manifests can forward these to ORG without waiting on the CPU.
    std::vector<GpuQueueSubmission> submissions;
    std::function<bool()> isSubmitted;
    std::function<bool()> isComplete;
	std::function<bool()> isFailed;
	std::function<std::string()> failure;
    std::function<std::string()> describe;
    std::function<void(std::function<void()>)> subscribe;
    std::function<bool()> cancel;
    // True only when the backend guarantees a notification for every state
    // transition. Callback-less or best-effort adapters remain on the single
    // graph recovery path during broker migration.
    bool completionNotificationsAreAuthoritative = false;

    [[nodiscard]] bool Submitted() const { return !isSubmitted || isSubmitted(); }
    [[nodiscard]] bool Complete() const { return !isComplete || isComplete(); }
	[[nodiscard]] bool Failed() const { return isFailed && isFailed(); }
	[[nodiscard]] std::string Failure() const {
		return failure ? failure() : std::string{ "GPU submission failed" };
	}
    [[nodiscard]] std::uint64_t MaximumTimelineValue() const {
        std::uint64_t result = 0;
        for (const auto& submission : submissions) {
            result = (std::max)(result, submission.TimelineValue());
        }
        return result;
    }
	[[nodiscard]] std::string Describe() const { return describe ? describe() : std::string{}; }
    [[nodiscard]] bool Cancel() const { return cancel && cancel(); }
};

} // namespace org::async
