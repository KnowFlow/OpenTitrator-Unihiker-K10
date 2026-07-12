#include <cstddef>
#include <cstdint>
#include <iostream>

#include "../ph_titrator/run_engine.h"

namespace {

int failures = 0;

void expectTrue(bool value, const char *name) {
  if (!value) {
    std::cerr << "FAIL: " << name << "\n";
    ++failures;
  }
}

void expectEqual(RunPhase actual, RunPhase expected, const char *name) {
  if (actual != expected) {
    std::cerr << "FAIL: " << name << "\n";
    ++failures;
  }
}

void expectEqual(RunStatusCode actual, RunStatusCode expected, const char *name) {
  if (actual != expected) {
    std::cerr << "FAIL: " << name << "\n";
    ++failures;
  }
}

void expectPumpStopped(const PumpIntent &intent, const char *name) {
  expectTrue(intent.mode == PumpMode::Stop, name);
  expectTrue(intent.durationMs == 0U, "stopped pump has no duration");
}

void expectPumpsStopped(const RunOutput &output) {
  expectPumpStopped(output.titrant, "titrant pump is stopped");
  expectPumpStopped(output.sample, "sample pump is stopped");
}

RunInput tickInput() {
  RunInput input{};
  input.command = RunCommand::Tick;
  return input;
}

RunInput startNormal(float targetSampleGrams, uint32_t nowMs = 0U) {
  RunInput input = tickInput();
  input.command = RunCommand::StartNormal;
  input.context.targetSampleGrams = targetSampleGrams;
  input.nowMs = nowMs;
  return input;
}

RunInput startExistingSample(uint32_t nowMs = 0U) {
  RunInput input = tickInput();
  input.command = RunCommand::StartExistingSample;
  input.nowMs = nowMs;
  return input;
}

RunInput warmupTick(uint32_t nowMs, bool stable) {
  RunInput input = tickInput();
  input.nowMs = nowMs;
  input.sensor.sensorValid = stable;
  input.sensor.sensorFresh = stable;
  input.sensor.controlSettled = stable;
  return input;
}

}  // namespace

int main() {
  static_assert(sizeof(RunEngine) <= 4096U, "RunEngine exceeds its memory budget");
  static_assert(
      sizeof(RunInput) + sizeof(RunOutput) <= 1024U,
      "RunInput and RunOutput exceed their memory budget");

  RunEngine engine;
  expectEqual(engine.phase(), RunPhase::Inactive, "construction starts inactive");

  RunOutput initial = engine.step(tickInput());
  expectEqual(initial.phase, RunPhase::Inactive, "tick keeps a new engine inactive");
  expectEqual(initial.status, RunStatusCode::Inactive, "inactive tick has inactive status");
  expectPumpsStopped(initial);

  RunInput unknown = tickInput();
  unknown.command = static_cast<RunCommand>(255U);
  RunOutput unknownOutput = engine.step(unknown);
  expectEqual(unknownOutput.phase, RunPhase::Inactive, "unknown command is a no-op");
  expectPumpsStopped(unknownOutput);

  RunInput lockedStart = tickInput();
  lockedStart.command = RunCommand::StartNormal;
  lockedStart.context.otaLocked = true;
  RunOutput locked = engine.step(lockedStart);
  expectEqual(locked.phase, RunPhase::Error, "safety lock enters error phase");
  expectEqual(locked.status, RunStatusCode::SafetyLocked, "safety lock has stable status");
  expectTrue(locked.stopReason == RunStopReason::SafetyLock, "safety lock retains stop reason");
  expectPumpsStopped(locked);

  RunInput lockedReset = tickInput();
  lockedReset.command = RunCommand::Reset;
  lockedReset.context.otaLocked = true;
  RunOutput lockedResetOutput = engine.step(lockedReset);
  expectEqual(lockedResetOutput.phase, RunPhase::Error, "locked reset keeps error phase");
  expectEqual(lockedResetOutput.status, RunStatusCode::SafetyLocked, "locked reset keeps safety status");
  expectTrue(
      lockedResetOutput.stopReason == RunStopReason::SafetyLock,
      "locked reset keeps safety stop reason");
  expectPumpsStopped(lockedResetOutput);

  RunInput unknownAfterLock = tickInput();
  unknownAfterLock.command = static_cast<RunCommand>(255U);
  RunOutput unknownAfterLockOutput = engine.step(unknownAfterLock);
  expectEqual(
      unknownAfterLockOutput.phase,
      RunPhase::Error,
      "unknown command keeps durable safety error phase");
  expectEqual(
      unknownAfterLockOutput.status,
      RunStatusCode::SafetyLocked,
      "unknown command keeps durable safety status");
  expectTrue(
      unknownAfterLockOutput.stopReason == RunStopReason::SafetyLock,
      "unknown command keeps durable safety stop reason");
  expectPumpsStopped(unknownAfterLockOutput);

  RunInput reset = tickInput();
  reset.command = RunCommand::Reset;
  RunOutput resetOutput = engine.step(reset);
  expectEqual(resetOutput.phase, RunPhase::Inactive, "reset returns inactive");
  expectEqual(resetOutput.status, RunStatusCode::Inactive, "reset clears status");
  expectTrue(resetOutput.stopReason == RunStopReason::None, "reset clears stop reason");
  expectPumpsStopped(resetOutput);

  // ---- Phase 2: safe lifecycle entry ----
  {
    RunEngine normalEngine;
    RunOutput filling = normalEngine.step(startNormal(10.0f, 100U));
    expectEqual(filling.phase, RunPhase::SampleFilling, "normal start fills a non-trivial target");
    expectEqual(filling.status, RunStatusCode::FillingSample, "normal start reports filling status");
    expectTrue(
        filling.sample.mode == PumpMode::RunContinuous,
        "normal start runs the sample pump continuously");
    expectPumpStopped(filling.titrant, "normal start keeps titrant stopped");

    RunInput beforeTarget = warmupTick(101U, false);
    beforeTarget.context.targetSampleGrams = 10.0f;
    RunOutput complete = normalEngine.step(beforeTarget);
    expectEqual(complete.phase, RunPhase::SampleFilling, "filling waits for target mass");
    expectTrue(
        complete.sample.mode == PumpMode::RunContinuous,
        "filling keeps sample pump running before target");

    RunInput atTarget = warmupTick(102U, false);
    atTarget.context.targetSampleGrams = 10.0f;
    atTarget.sensor.deliveredSampleGrams = 10.0f;
    RunOutput filled = normalEngine.step(atTarget);
    expectEqual(filled.phase, RunPhase::FilterWarmup, "target sample mass ends filling");
    expectEqual(filled.status, RunStatusCode::WaitingForStableSignal, "filled sample enters warmup status");
    expectPumpsStopped(filled);
  }

  {
    RunEngine smallTargetEngine;
    RunOutput small = smallTargetEngine.step(startNormal(0.01f));
    expectEqual(small.phase, RunPhase::FilterWarmup, "small normal target skips filling");
    expectPumpsStopped(small);

    RunEngine existingEngine;
    RunOutput existing = existingEngine.step(startExistingSample());
    expectEqual(existing.phase, RunPhase::FilterWarmup, "existing sample enters warmup");
    expectPumpsStopped(existing);

    RunOutput unstable = existingEngine.step(warmupTick(10U, false));
    expectEqual(unstable.phase, RunPhase::FilterWarmup, "unstable warmup remains in warmup");
    expectEqual(unstable.status, RunStatusCode::WaitingForStableSignal, "unstable warmup keeps status");
    expectPumpsStopped(unstable);

    RunOutput ready = existingEngine.step(warmupTick(11U, true));
    expectEqual(ready.phase, RunPhase::Running, "stable warmup enters running");
    expectPumpsStopped(ready);
  }

  {
    RunEngine resumeEngine;
    resumeEngine.step(startNormal(1.0f));
    RunInput pause = tickInput();
    pause.command = RunCommand::Pause;
    RunOutput paused = resumeEngine.step(pause);
    expectEqual(paused.phase, RunPhase::Paused, "pause interrupts sample filling");
    expectEqual(paused.status, RunStatusCode::Paused, "pause has paused status");
    expectPumpsStopped(paused);

    RunInput resume = tickInput();
    resume.command = RunCommand::Resume;
    RunOutput resumed = resumeEngine.step(resume);
    expectEqual(resumed.phase, RunPhase::FilterWarmup, "resume always returns to warmup");
    expectEqual(
        resumed.status,
        RunStatusCode::ReStabilizingAfterResume,
        "resume announces re-stabilization");
    expectPumpsStopped(resumed);

    RunOutput stillUnstable = resumeEngine.step(warmupTick(20U, false));
    expectEqual(
        stillUnstable.status,
        RunStatusCode::ReStabilizingAfterResume,
        "unstable tick retains re-stabilization status");
    expectPumpsStopped(stillUnstable);
  }

  {
    RunEngine progressTimeoutEngine;
    const uint32_t startAt = UINT32_MAX - 999U;
    RunInput start = startNormal(5.0f, startAt);
    progressTimeoutEngine.step(start);
    RunInput timedOut = warmupTick(11000U, false);
    timedOut.context.targetSampleGrams = 5.0f;
    RunOutput timeout = progressTimeoutEngine.step(timedOut);
    expectEqual(timeout.phase, RunPhase::Error, "no-progress timeout survives millis rollover");
    expectEqual(
        timeout.status,
        RunStatusCode::SampleProgressTimeout,
        "no-progress timeout has stable status");
    expectTrue(
        timeout.stopReason == RunStopReason::SampleProgressTimeout,
        "no-progress timeout has stable stop reason");
    expectPumpsStopped(timeout);
  }

  {
    RunEngine durationTimeoutEngine;
    RunInput start = startNormal(10.0f, 1000U);
    start.context.samplePumpFlowRateGps = 1.0f;
    durationTimeoutEngine.step(start);

    RunInput progress = warmupTick(17999U, false);
    progress.context.targetSampleGrams = 10.0f;
    progress.context.samplePumpFlowRateGps = 1.0f;
    progress.sensor.deliveredSampleGrams = 0.2f;
    RunOutput beforeDeadline = durationTimeoutEngine.step(progress);
    expectEqual(beforeDeadline.phase, RunPhase::SampleFilling, "fill duration waits through exact allowance");

    RunInput timedOut = warmupTick(18000U, false);
    timedOut.context.targetSampleGrams = 10.0f;
    timedOut.context.samplePumpFlowRateGps = 1.0f;
    timedOut.sensor.deliveredSampleGrams = 0.4f;
    RunOutput timeout = durationTimeoutEngine.step(timedOut);
    expectEqual(timeout.phase, RunPhase::Error, "maximum fill duration enters error");
    expectEqual(timeout.status, RunStatusCode::SampleFillTimeout, "maximum fill duration has stable status");
    expectTrue(
        timeout.stopReason == RunStopReason::SampleFillTimeout,
        "maximum fill duration has stable stop reason");
    expectPumpsStopped(timeout);
  }

  {
    RunEngine rolloverDurationEngine;
    const uint32_t startAt = UINT32_MAX - 999U;
    RunInput start = startNormal(10.0f, startAt);
    start.context.samplePumpFlowRateGps = 1.0f;
    rolloverDurationEngine.step(start);

    RunInput progress = warmupTick(15999U, false);
    progress.context.targetSampleGrams = 10.0f;
    progress.context.samplePumpFlowRateGps = 1.0f;
    progress.sensor.deliveredSampleGrams = 0.2f;
    expectEqual(
        rolloverDurationEngine.step(progress).phase,
        RunPhase::SampleFilling,
        "maximum fill duration waits through rollover allowance");

    RunInput timedOut = warmupTick(16000U, false);
    timedOut.context.targetSampleGrams = 10.0f;
    timedOut.context.samplePumpFlowRateGps = 1.0f;
    timedOut.sensor.deliveredSampleGrams = 0.4f;
    RunOutput timeout = rolloverDurationEngine.step(timedOut);
    expectEqual(timeout.phase, RunPhase::Error, "maximum fill duration survives millis rollover");
    expectEqual(
        timeout.status,
        RunStatusCode::SampleFillTimeout,
        "maximum fill rollover timeout has stable status");
    expectPumpsStopped(timeout);
  }

  {
    RunEngine resetEngine;
    resetEngine.step(startNormal(3.0f, 100U));
    RunInput resetRun = tickInput();
    resetRun.command = RunCommand::Reset;
    RunOutput inactive = resetEngine.step(resetRun);
    expectEqual(inactive.phase, RunPhase::Inactive, "reset clears active fill phase");

    RunOutput fresh = resetEngine.step(startNormal(3.0f, 20000U));
    expectEqual(fresh.phase, RunPhase::SampleFilling, "reset allows a fresh fill run");
    expectTrue(fresh.sample.mode == PumpMode::RunContinuous, "fresh fill run restarts sample pump");
  }

  if (failures != 0) {
    return 1;
  }

  std::cout << "All RunEngine shell tests passed\n";
  return 0;
}
