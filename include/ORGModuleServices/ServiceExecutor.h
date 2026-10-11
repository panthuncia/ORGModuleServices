#pragma once

#include <functional>

namespace org::services {

// The executor hook ShaderCompiler and PipelineService run their work through, shaped like
// org::async::SerializedTaskPump::Submit so one host scheduler can serve both.
//
// A submitter takes ownership of the task and runs it exactly once, on whichever thread it chooses. It returns false
// (instead of throwing) when it will never run the task; the request then completes with an error artifact, which
// still reaches the returned future and every completion callback, so no request is ever lost or left pending.
// An empty submitter (the default) keeps the original behaviour: one new thread per request.
using ServiceTask = std::function<void()>;
using ServiceSubmit = std::function<bool(ServiceTask)>;

}
