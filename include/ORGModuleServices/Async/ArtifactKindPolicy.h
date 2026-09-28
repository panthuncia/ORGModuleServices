#pragma once

namespace org::async {

// Immutable host metadata, not a runtime graph mutation. Coalescing may only
// replace complete, unstarted intents; lifecycle/transaction kinds opt out.
struct ArtifactKindPolicy {
    bool allowCoalescing = true;
    // Conservative default: a replaceable consumer still owns exact content.
    // ReadyGate and Latest requirements never acquire these exact recipe pins.
    bool pinExactContent = true;
    bool traceResourceLifetime = false;
};

} // namespace org::async
