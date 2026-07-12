#include "run_engine.h"

#include <cmath>

namespace {

const uint32_t kSampleProgressTimeoutMs = 12000U;
const float kSampleProgressGrams = 0.2f;
const float kMinimumKnownSampleFlowRateGps = 0.001f;
const float kSampleFillDurationMultiplierMs = 1200.0f;
const float kSampleFillAllowanceMs = 5000.0f;
const float kMaximumUint32AsFloat = 4294967295.0f;

bool elapsedAtLeast(uint32_t nowMs, uint32_t startedAtMs, uint32_t durationMs) {
  return static_cast<uint32_t>(nowMs - startedAtMs) >= durationMs;
}

bool maximumFillDurationMs(const RunContext &context, uint32_t &durationMs) {
  if (!std::isfinite(context.samplePumpFlowRateGps) ||
      context.samplePumpFlowRateGps <= kMinimumKnownSampleFlowRateGps) {
    return false;
  }

  const float computedDurationMs =
      (context.targetSampleGrams / context.samplePumpFlowRateGps) *
          kSampleFillDurationMultiplierMs +
      kSampleFillAllowanceMs;
  if (!std::isfinite(computedDurationMs) || computedDurationMs < 0.0f ||
      computedDurationMs >= kMaximumUint32AsFloat) {
    return false;
  }
  durationMs = static_cast<uint32_t>(computedDurationMs);
  return true;
}

}  // namespace

RunEngine::RunEngine()
    : phase_(RunPhase::Inactive),
      stopReason_(RunStopReason::None),
      pausedPhase_(RunPhase::Inactive),
      fillStartedAtMs_(0U),
      lastSampleProgressAtMs_(0U),
      lastSampleProgressGrams_(0.0f),
      reStabilizingAfterResume_(false) {}

RunOutput RunEngine::step(const RunInput &input) {
  RunOutput output{};

  if (input.context.otaLocked) {
    phase_ = RunPhase::Error;
    stopReason_ = RunStopReason::SafetyLock;
  } else if (input.command == RunCommand::Reset) {
    reset();
  } else if (input.command == RunCommand::StartNormal) {
    stopReason_ = RunStopReason::None;
    pausedPhase_ = RunPhase::Inactive;
    reStabilizingAfterResume_ = false;
    if (input.context.targetSampleGrams > 0.01f) {
      phase_ = RunPhase::SampleFilling;
      fillStartedAtMs_ = input.nowMs;
      lastSampleProgressAtMs_ = input.nowMs;
      lastSampleProgressGrams_ = input.sensor.deliveredSampleGrams;
    } else {
      phase_ = RunPhase::FilterWarmup;
    }
  } else if (input.command == RunCommand::StartExistingSample) {
    phase_ = RunPhase::FilterWarmup;
    stopReason_ = RunStopReason::None;
    pausedPhase_ = RunPhase::Inactive;
    reStabilizingAfterResume_ = false;
  } else if (input.command == RunCommand::Pause &&
             (phase_ == RunPhase::SampleFilling || phase_ == RunPhase::FilterWarmup ||
              phase_ == RunPhase::Running || phase_ == RunPhase::Dosing ||
              phase_ == RunPhase::Settling)) {
    pausedPhase_ = phase_;
    phase_ = RunPhase::Paused;
  } else if (input.command == RunCommand::Resume && phase_ == RunPhase::Paused) {
    phase_ = RunPhase::FilterWarmup;
    reStabilizingAfterResume_ = true;
  }

  if (input.command == RunCommand::Tick && phase_ == RunPhase::SampleFilling) {
    if (input.sensor.deliveredSampleGrams >= input.context.targetSampleGrams) {
      phase_ = RunPhase::FilterWarmup;
    } else {
      if (input.sensor.deliveredSampleGrams >=
          lastSampleProgressGrams_ + kSampleProgressGrams) {
        lastSampleProgressGrams_ = input.sensor.deliveredSampleGrams;
        lastSampleProgressAtMs_ = input.nowMs;
      }

      uint32_t maxFillDurationMs = 0U;
      if (maximumFillDurationMs(input.context, maxFillDurationMs) &&
          elapsedAtLeast(input.nowMs, fillStartedAtMs_, maxFillDurationMs)) {
        phase_ = RunPhase::Error;
        stopReason_ = RunStopReason::SampleFillTimeout;
      } else if (elapsedAtLeast(
                     input.nowMs,
                     lastSampleProgressAtMs_,
                     kSampleProgressTimeoutMs)) {
        phase_ = RunPhase::Error;
        stopReason_ = RunStopReason::SampleProgressTimeout;
      }
    }
  }

  if (input.command == RunCommand::Tick && phase_ == RunPhase::FilterWarmup &&
      input.sensor.sensorValid && input.sensor.sensorFresh && input.sensor.controlSettled) {
    phase_ = RunPhase::Running;
    reStabilizingAfterResume_ = false;
  }

  output.phase = phase_;
  output.stopReason = stopReason_;

  if (stopReason_ == RunStopReason::SafetyLock) {
    output.status = RunStatusCode::SafetyLocked;
  } else if (stopReason_ == RunStopReason::SampleProgressTimeout) {
    output.status = RunStatusCode::SampleProgressTimeout;
  } else if (stopReason_ == RunStopReason::SampleFillTimeout) {
    output.status = RunStatusCode::SampleFillTimeout;
  } else if (phase_ == RunPhase::SampleFilling) {
    output.status = RunStatusCode::FillingSample;
    output.sample.mode = PumpMode::RunContinuous;
  } else if (phase_ == RunPhase::FilterWarmup) {
    output.status = reStabilizingAfterResume_
                        ? RunStatusCode::ReStabilizingAfterResume
                        : RunStatusCode::WaitingForStableSignal;
  } else if (phase_ == RunPhase::Running) {
    output.status = RunStatusCode::CheckingEndpoint;
  } else if (phase_ == RunPhase::Paused) {
    output.status = RunStatusCode::Paused;
  }

  return output;
}

void RunEngine::reset() {
  phase_ = RunPhase::Inactive;
  stopReason_ = RunStopReason::None;
  pausedPhase_ = RunPhase::Inactive;
  fillStartedAtMs_ = 0U;
  lastSampleProgressAtMs_ = 0U;
  lastSampleProgressGrams_ = 0.0f;
  reStabilizingAfterResume_ = false;
}

RunPhase RunEngine::phase() const {
  return phase_;
}
