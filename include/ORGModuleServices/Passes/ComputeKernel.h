#pragma once

#include <cstdint>
#include <span>

namespace org {
class LatchBlock;
class RecordingContext;
}

namespace org::services {

// A compute pipeline the host built, with a block of root constants at b0. A pass records through it and never builds or
// waits for one: a pass whose kernel is not built yet is not made (its host lags it a revision).
class ComputeKernel {
public:
    virtual ~ComputeKernel() = default;

    // Binds the pipeline, sets the constants and dispatches. Records nothing for a zero group count.
    virtual void Dispatch(org::RecordingContext& recording, std::span<const uint32_t> constants, uint32_t x, uint32_t y,
        uint32_t z) const = 0;

    // As Dispatch, with the group counts at offset in the latch region of the recording's run (three uints).
    virtual void DispatchIndirect(org::RecordingContext& recording, std::span<const uint32_t> constants, const org::LatchBlock& latch,
        uint32_t offset) const = 0;
};

}
