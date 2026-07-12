#include "run_engine.h"

RunEngine::RunEngine()
    : phase_(RunPhase::Inactive), stopReason_(RunStopReason::None) {}

RunOutput RunEngine::step(const RunInput &input) {
  if (input.command == RunCommand::Reset) {
    reset();
  } else if (input.context.otaLocked) {
    phase_ = RunPhase::Error;
    stopReason_ = RunStopReason::SafetyLock;
  }

  RunOutput output{};
  output.phase = phase_;
  output.stopReason = stopReason_;

  if (stopReason_ == RunStopReason::SafetyLock) {
    output.status = RunStatusCode::SafetyLocked;
  }

  return output;
}

void RunEngine::reset() {
  phase_ = RunPhase::Inactive;
  stopReason_ = RunStopReason::None;
}

RunPhase RunEngine::phase() const {
  return phase_;
}
