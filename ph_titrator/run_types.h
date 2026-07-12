#ifndef PH_TITRATOR_RUN_TYPES_H
#define PH_TITRATOR_RUN_TYPES_H

#include <stdint.h>

enum class RunPhase : uint8_t {
  Inactive = 0,
  SampleFilling = 1,
  FilterWarmup = 2,
  Running = 3,
  Dosing = 4,
  Settling = 5,
  Paused = 6,
  Done = 7,
  Error = 8
};

enum class RunCommand : uint8_t {
  Tick = 0,
  StartNormal = 1,
  StartExistingSample = 2,
  Pause = 3,
  Resume = 4,
  Reset = 5
};

enum class RunStatusCode : uint8_t {
  Inactive = 0,
  FillingSample = 1,
  WaitingForStableSignal = 2,
  CheckingEndpoint = 3,
  Dosing = 4,
  Settling = 5,
  HoldingEndpoint = 6,
  Paused = 7,
  TargetReached = 8,
  EquivalencePointReached = 9,
  SafetyLocked = 10,
  SensorFault = 11,
  ScaleFailure = 12,
  SampleProgressTimeout = 13,
  SampleFillTimeout = 14,
  MassLimit = 15,
  TimeLimit = 16,
  ReStabilizingAfterResume = 17,
  OtaSafetyLock = SafetyLocked
};

enum class RunStopReason : uint8_t {
  None = 0,
  TargetReached = 1,
  EquivalencePoint = 2,
  SafetyLock = 3,
  SensorFault = 4,
  ScaleFailure = 5,
  SampleProgressTimeout = 6,
  SampleFillTimeout = 7,
  MassLimit = 8,
  TimeLimit = 9,
  InvalidReading = 10,
  OtaSafetyLock = SafetyLock
};

enum class PumpMode : uint8_t {
  Stop = 0,
  RunContinuous = 1,
  RunForMs = 2
};

struct PumpIntent {
  PumpMode mode = PumpMode::Stop;
  uint32_t durationMs = 0U;
};

struct SensorSnapshot {
  float ph = 0.0f;
  float millivolts = 0.0f;
  float controlValue = 0.0f;
  float reactorMassGrams = 0.0f;
  float consumedTitrantGrams = 0.0f;
  float deliveredSampleGrams = 0.0f;
  float controlSlopePerSecond = 0.0f;
  bool sensorValid = false;
  bool sensorFresh = false;
  bool controlSettled = false;
  bool scaleValid = false;
};

struct RunContext {
  float targetSampleGrams = 0.0f;
  float maximumTitrantGrams = 0.0f;
  uint32_t maximumRunMs = 0U;
  bool otaLocked = false;
};

struct RunInput {
  uint32_t nowMs = 0U;
  RunCommand command = RunCommand::Tick;
  SensorSnapshot sensor{};
  RunContext context{};
};

struct RunOutput {
  RunPhase phase = RunPhase::Inactive;
  PumpIntent titrant{};
  PumpIntent sample{};
  RunStatusCode status = RunStatusCode::Inactive;
  RunStopReason stopReason = RunStopReason::None;
  bool finalizeResult = false;
  bool recordEqpPoint = false;
  bool hasRequestedSettleMs = false;
  uint32_t requestedSettleMs = 0U;
};

#endif
