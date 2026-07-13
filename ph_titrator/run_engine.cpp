#include "run_engine.h"

#include <cmath>

namespace {

const uint32_t kSampleProgressTimeoutMs = 12000U;
const float kSampleProgressGrams = 0.2f;
const float kMinimumKnownSampleFlowRateGps = 0.001f;
const float kSampleFillDurationMultiplierMs = 1200.0f;
const float kSampleFillAllowanceMs = 5000.0f;
const float kMaximumUint32AsFloat = 4294967295.0f;

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

uint32_t settleLimitMs(uint16_t seconds) {
  return static_cast<uint32_t>(seconds) * 1000UL;
}

RunStopReason stopReasonForDecision(TitrationStopReason reason) {
  switch (reason) {
    case TitrationStopReason::MassLimit:
      return RunStopReason::MassLimit;
    case TitrationStopReason::TimeLimit:
      return RunStopReason::TimeLimit;
    case TitrationStopReason::SensorFault:
    case TitrationStopReason::InvalidReading:
      return RunStopReason::SensorFault;
    default:
      return RunStopReason::None;
  }
}

}  // namespace

RunEngine::RunEngine()
    : phase_(RunPhase::Inactive),
      stopReason_(RunStopReason::None),
      pausedPhase_(RunPhase::Inactive),
      fillStartedAtMs_(0U),
      lastSampleProgressAtMs_(0U),
      lastSampleProgressGrams_(0.0f),
      reStabilizingAfterResume_(false),
      settings_{},
      dynamics_{},
      endpointHold_{},
      runStartedAtMs_(0U),
      pulseStartedAtMs_(0U),
      settleStartedAtMs_(0U),
      activePulseMs_(0U),
      activeSettleMs_(0U),
      activeMaxSettleMs_(0U) {}

RunOutput RunEngine::step(const RunInput &input) {
  RunOutput output{};
  settings_ = input.context.settings;

  if (input.context.otaLocked) {
    phase_ = RunPhase::Error;
    stopReason_ = RunStopReason::SafetyLock;
  } else if (input.command == RunCommand::Reset) {
    reset();
  } else if (input.command == RunCommand::StartNormal ||
             input.command == RunCommand::StartExistingSample) {
    stopReason_ = RunStopReason::None;
    pausedPhase_ = RunPhase::Inactive;
    reStabilizingAfterResume_ = false;
    dynamics_.reset();
    endpointHold_.reset();
    runStartedAtMs_ = input.nowMs;
    pulseStartedAtMs_ = 0U;
    settleStartedAtMs_ = 0U;
    activePulseMs_ = 0U;
    activeSettleMs_ = 0U;
    activeMaxSettleMs_ = 0U;
    if (input.command == RunCommand::StartNormal && input.context.targetSampleGrams > 0.01f) {
      phase_ = RunPhase::SampleFilling;
      fillStartedAtMs_ = input.nowMs;
      lastSampleProgressAtMs_ = input.nowMs;
      lastSampleProgressGrams_ = input.sensor.deliveredSampleGrams;
    } else {
      phase_ = RunPhase::FilterWarmup;
    }
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

  const RunPhase phaseAtTick = phase_;

  if (input.command == RunCommand::Tick && phaseAtTick == RunPhase::SampleFilling) {
    if (input.sensor.deliveredSampleGrams >= input.context.targetSampleGrams) {
      phase_ = RunPhase::FilterWarmup;
    } else {
      if (input.sensor.deliveredSampleGrams >= lastSampleProgressGrams_ + kSampleProgressGrams) {
        lastSampleProgressGrams_ = input.sensor.deliveredSampleGrams;
        lastSampleProgressAtMs_ = input.nowMs;
      }
      uint32_t maxFillDurationMs = 0U;
      if (maximumFillDurationMs(input.context, maxFillDurationMs) &&
          elapsedAtLeast(input.nowMs, fillStartedAtMs_, maxFillDurationMs)) {
        phase_ = RunPhase::Error;
        stopReason_ = RunStopReason::SampleFillTimeout;
      } else if (elapsedAtLeast(input.nowMs, lastSampleProgressAtMs_, kSampleProgressTimeoutMs)) {
        phase_ = RunPhase::Error;
        stopReason_ = RunStopReason::SampleProgressTimeout;
      }
    }
  }

  if (input.command == RunCommand::Tick && phaseAtTick == RunPhase::FilterWarmup &&
      input.sensor.sensorValid && input.sensor.sensorFresh && input.sensor.controlSettled) {
    phase_ = RunPhase::Running;
    reStabilizingAfterResume_ = false;
  }

  if (input.command == RunCommand::Tick && phaseAtTick == RunPhase::Running) {
    if (!input.sensor.sensorValid) {
      phase_ = RunPhase::Error;
      stopReason_ = RunStopReason::SensorFault;
    } else if (!input.sensor.scaleValid) {
      phase_ = RunPhase::Error;
      stopReason_ = RunStopReason::ScaleFailure;
    } else if (input.sensor.sensorFresh) {
      if (elapsedAtLeast(
              input.nowMs,
              runStartedAtMs_,
              static_cast<uint32_t>(settings_.maxTimeSeconds) * 1000UL)) {
        phase_ = RunPhase::Error;
        stopReason_ = RunStopReason::TimeLimit;
      } else {
        dynamics_.add(input.sensor.controlValue, input.nowMs);
        const bool inEndpoint = isEndpointReached(settings_, input.sensor.controlValue);
        if (endpointHold_.update(true, inEndpoint, settings_.holdSeconds, input.nowMs)) {
          phase_ = RunPhase::Done;
          stopReason_ = RunStopReason::TargetReached;
          output.finalizeResult = true;
        } else if (endpointHold_.active()) {
          output.status = RunStatusCode::HoldingEndpoint;
        } else {
          const TitrationDecision decision = decideAdaptiveDose(
              settings_, input.sensor.controlValue, input.sensor.consumedTitrantGrams, dynamics_);
          if (decision.action == TitrationAction::Error) {
            phase_ = RunPhase::Error;
            stopReason_ = stopReasonForDecision(decision.reason);
          } else if (decision.action == TitrationAction::Dose) {
            phase_ = RunPhase::Dosing;
            pulseStartedAtMs_ = input.nowMs;
            activePulseMs_ = decision.pumpPulseMs;
            activeSettleMs_ = decision.settleMs;
            activeMaxSettleMs_ = settleLimitMs(settings_.maxSettleSeconds);
            if (activeMaxSettleMs_ < activeSettleMs_) {
              activeMaxSettleMs_ = activeSettleMs_;
            }
            output.titrant.mode = PumpMode::RunForMs;
            output.titrant.durationMs = activePulseMs_;
          } else if (decision.action == TitrationAction::Wait) {
            phase_ = RunPhase::Settling;
            settleStartedAtMs_ = input.nowMs;
            activePulseMs_ = 0U;
            activeSettleMs_ = decision.settleMs;
            activeMaxSettleMs_ = settleLimitMs(settings_.maxSettleSeconds);
            if (activeMaxSettleMs_ < activeSettleMs_) {
              activeMaxSettleMs_ = activeSettleMs_;
            }
          } else if (decision.action == TitrationAction::Done) {
            phase_ = RunPhase::Done;
            stopReason_ = RunStopReason::TargetReached;
            output.finalizeResult = true;
          }
        }
      }
    }
  }

  if (input.command == RunCommand::Tick && phaseAtTick == RunPhase::Dosing &&
      elapsedAtLeast(input.nowMs, pulseStartedAtMs_, activePulseMs_)) {
    phase_ = RunPhase::Settling;
    settleStartedAtMs_ = input.nowMs;
    output.hasRequestedSettleMs = true;
    output.requestedSettleMs = activeSettleMs_;
  }

  if (input.command == RunCommand::Tick && phaseAtTick == RunPhase::Settling) {
    const bool minimumElapsed = elapsedAtLeast(input.nowMs, settleStartedAtMs_, activeSettleMs_);
    const bool maximumElapsed = elapsedAtLeast(input.nowMs, settleStartedAtMs_, activeMaxSettleMs_);
    if (maximumElapsed ||
        (minimumElapsed && input.sensor.sensorValid && input.sensor.sensorFresh &&
         input.sensor.controlSettled && input.sensor.scaleValid)) {
      phase_ = RunPhase::Running;
    }
  }

  output.phase = phase_;
  output.stopReason = stopReason_;
  if (stopReason_ == RunStopReason::SafetyLock) {
    output.status = RunStatusCode::SafetyLocked;
  } else if (stopReason_ == RunStopReason::SensorFault || stopReason_ == RunStopReason::InvalidReading) {
    output.status = RunStatusCode::SensorFault;
  } else if (stopReason_ == RunStopReason::ScaleFailure) {
    output.status = RunStatusCode::ScaleFailure;
  } else if (stopReason_ == RunStopReason::SampleProgressTimeout) {
    output.status = RunStatusCode::SampleProgressTimeout;
  } else if (stopReason_ == RunStopReason::SampleFillTimeout) {
    output.status = RunStatusCode::SampleFillTimeout;
  } else if (stopReason_ == RunStopReason::MassLimit) {
    output.status = RunStatusCode::MassLimit;
  } else if (stopReason_ == RunStopReason::TimeLimit) {
    output.status = RunStatusCode::TimeLimit;
  } else if (phase_ == RunPhase::SampleFilling) {
    output.status = RunStatusCode::FillingSample;
    output.sample.mode = PumpMode::RunContinuous;
  } else if (phase_ == RunPhase::FilterWarmup) {
    output.status = reStabilizingAfterResume_ ? RunStatusCode::ReStabilizingAfterResume
                                               : RunStatusCode::WaitingForStableSignal;
  } else if (phase_ == RunPhase::Running && output.status != RunStatusCode::HoldingEndpoint) {
    output.status = RunStatusCode::CheckingEndpoint;
  } else if (phase_ == RunPhase::Dosing) {
    output.status = RunStatusCode::Dosing;
  } else if (phase_ == RunPhase::Settling) {
    output.status = RunStatusCode::Settling;
  } else if (phase_ == RunPhase::Paused) {
    output.status = RunStatusCode::Paused;
  } else if (phase_ == RunPhase::Done) {
    output.status = stopReason_ == RunStopReason::EquivalencePoint
                        ? RunStatusCode::EquivalencePointReached
                        : RunStatusCode::TargetReached;
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
  settings_ = TitrationSettings{};
  dynamics_.reset();
  endpointHold_.reset();
  runStartedAtMs_ = 0U;
  pulseStartedAtMs_ = 0U;
  settleStartedAtMs_ = 0U;
  activePulseMs_ = 0U;
  activeSettleMs_ = 0U;
  activeMaxSettleMs_ = 0U;
}

RunPhase RunEngine::phase() const {
  return phase_;
}
