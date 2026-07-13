#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

#define private public
#include "../ph_titrator/run_engine.h"
#undef private

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

RunInput fillingTick(uint32_t nowMs, float targetSampleGrams, float flowRateGps, float deliveredGrams) {
  RunInput input = tickInput();
  input.nowMs = nowMs;
  input.context.targetSampleGrams = targetSampleGrams;
  input.context.samplePumpFlowRateGps = flowRateGps;
  input.sensor.deliveredSampleGrams = deliveredGrams;
  return input;
}

RunInput runningTick(uint32_t nowMs, float controlValue) {
  RunInput input = tickInput();
  input.nowMs = nowMs;
  input.sensor.sensorValid = true;
  input.sensor.sensorFresh = true;
  input.sensor.controlSettled = true;
  input.sensor.scaleValid = true;
  input.sensor.controlValue = controlValue;
  input.context.settings.holdSeconds = 0U;
  input.context.settings.minSettleSeconds = 1U;
  input.context.settings.maxSettleSeconds = 3U;
  return input;
}

void enterRunning(RunEngine &engine, uint32_t nowMs = 0U) {
  engine.step(startExistingSample(nowMs));
  engine.step(warmupTick(nowMs + 1U, true));
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
    const char *gateNames[] = {"valid", "fresh", "settled"};
    for (size_t missingGate = 0U; missingGate < 3U; ++missingGate) {
      RunEngine gateEngine;
      gateEngine.step(startExistingSample());
      RunInput incomplete = warmupTick(10U, true);
      if (missingGate == 0U) {
        incomplete.sensor.sensorValid = false;
      } else if (missingGate == 1U) {
        incomplete.sensor.sensorFresh = false;
      } else {
        incomplete.sensor.controlSettled = false;
      }
      RunOutput blocked = gateEngine.step(incomplete);
      expectEqual(blocked.phase, RunPhase::FilterWarmup, gateNames[missingGate]);
      expectPumpsStopped(blocked);
    }
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

    RunOutput stable = resumeEngine.step(warmupTick(21U, true));
    expectEqual(stable.phase, RunPhase::Running, "stable resume tick reaches running");
    expectEqual(stable.status, RunStatusCode::CheckingEndpoint, "stable resume tick clears re-stabilization status");
    expectPumpsStopped(stable);
  }

  {
    const RunPhase activePhases[] = {
        RunPhase::SampleFilling,
        RunPhase::FilterWarmup,
        RunPhase::Running,
        RunPhase::Dosing,
        RunPhase::Settling,
    };
    for (RunPhase activePhase : activePhases) {
      RunEngine pauseEngine;
      pauseEngine.phase_ = activePhase;
      RunInput pause = tickInput();
      pause.command = RunCommand::Pause;
      RunOutput paused = pauseEngine.step(pause);
      expectEqual(paused.phase, RunPhase::Paused, "pause interrupts every active phase");
      expectEqual(paused.status, RunStatusCode::Paused, "pause reports paused for every active phase");
      expectEqual(pauseEngine.pausedPhase_, activePhase, "pause records the interrupted active phase");
      expectPumpsStopped(paused);
    }
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
    const float nonFiniteFlows[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };
    for (float nonFiniteFlow : nonFiniteFlows) {
      RunEngine nonFiniteFlowEngine;
      RunInput start = startNormal(10.0f, 100U);
      start.context.samplePumpFlowRateGps = nonFiniteFlow;
      nonFiniteFlowEngine.step(start);

      RunOutput beforeProgressTimeout = nonFiniteFlowEngine.step(
          fillingTick(12099U, 10.0f, nonFiniteFlow, 0.0f));
      expectEqual(
          beforeProgressTimeout.phase,
          RunPhase::SampleFilling,
          "non-finite flow leaves only the progress guard before its deadline");

      RunOutput progressTimeout = nonFiniteFlowEngine.step(
          fillingTick(12100U, 10.0f, nonFiniteFlow, 0.0f));
      expectEqual(
          progressTimeout.phase,
          RunPhase::Error,
          "non-finite flow retains the progress guard at its deadline");
      expectEqual(
          progressTimeout.status,
          RunStatusCode::SampleProgressTimeout,
          "non-finite flow does not create a fill-duration timeout");
      expectPumpsStopped(progressTimeout);
    }
  }

  {
    RunEngine negativeTargetEngine;
    RunInput start = startNormal(10.0f, 100U);
    start.context.samplePumpFlowRateGps = 1.0f;
    negativeTargetEngine.step(start);

    const float negativeTargetGrams = -4.167f;
    RunOutput beforeProgressTimeout = negativeTargetEngine.step(
        fillingTick(12099U, negativeTargetGrams, 1.0f, -5.0f));
    expectEqual(
        beforeProgressTimeout.phase,
        RunPhase::SampleFilling,
        "negative later target leaves only the progress guard before its deadline");

    RunOutput progressTimeout = negativeTargetEngine.step(
        fillingTick(12100U, negativeTargetGrams, 1.0f, -5.0f));
    expectEqual(
        progressTimeout.phase,
        RunPhase::Error,
        "negative later target retains the progress guard at its deadline");
    expectEqual(
        progressTimeout.status,
        RunStatusCode::SampleProgressTimeout,
        "negative later target does not create a fill-duration timeout");
    expectPumpsStopped(progressTimeout);
  }

  {
    RunEngine oversizedDurationEngine;
    RunInput start = startNormal(10.0f, 100U);
    start.context.samplePumpFlowRateGps = 1.0f;
    oversizedDurationEngine.step(start);

    const float targetWithOversizedDurationGrams = 4000000.0f;
    RunOutput beforeProgressTimeout = oversizedDurationEngine.step(
        fillingTick(12099U, targetWithOversizedDurationGrams, 1.0f, 0.0f));
    expectEqual(
        beforeProgressTimeout.phase,
        RunPhase::SampleFilling,
        "out-of-range fill duration leaves only the progress guard before its deadline");

    RunOutput progressTimeout = oversizedDurationEngine.step(
        fillingTick(12100U, targetWithOversizedDurationGrams, 1.0f, 0.0f));
    expectEqual(
        progressTimeout.phase,
        RunPhase::Error,
        "out-of-range fill duration retains the progress guard at its deadline");
    expectEqual(
        progressTimeout.status,
        RunStatusCode::SampleProgressTimeout,
        "out-of-range fill duration does not create a fill-duration timeout");
    expectPumpsStopped(progressTimeout);
  }

  {
    RunEngine nonFiniteDurationEngine;
    RunInput start = startNormal(10.0f, 100U);
    start.context.samplePumpFlowRateGps = 1.0f;
    nonFiniteDurationEngine.step(start);

    const float nonFiniteTargetGrams = std::numeric_limits<float>::infinity();
    RunOutput beforeProgressTimeout = nonFiniteDurationEngine.step(
        fillingTick(12099U, nonFiniteTargetGrams, 1.0f, 0.0f));
    expectEqual(
        beforeProgressTimeout.phase,
        RunPhase::SampleFilling,
        "non-finite fill duration leaves only the progress guard before its deadline");

    RunOutput progressTimeout = nonFiniteDurationEngine.step(
        fillingTick(12100U, nonFiniteTargetGrams, 1.0f, 0.0f));
    expectEqual(
        progressTimeout.phase,
        RunPhase::Error,
        "non-finite fill duration retains the progress guard at its deadline");
    expectEqual(
        progressTimeout.status,
        RunStatusCode::SampleProgressTimeout,
        "non-finite fill duration does not create a fill-duration timeout");
    expectPumpsStopped(progressTimeout);
  }

  {
    RunEngine exactProgressEngine;
    RunInput start = startNormal(10.0f, 100U);
    exactProgressEngine.step(start);

    RunOutput refreshed = exactProgressEngine.step(fillingTick(12100U, 10.0f, 0.0f, 0.2f));
    expectEqual(refreshed.phase, RunPhase::SampleFilling, "exactly 0.2g refreshes progress timer");

    RunOutput beforeRefreshedDeadline = exactProgressEngine.step(
        fillingTick(24099U, 10.0f, 0.0f, 0.2f));
    expectEqual(
        beforeRefreshedDeadline.phase,
        RunPhase::SampleFilling,
        "progress refresh defers timeout through its exact allowance");
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

    RunInput pause = tickInput();
    pause.command = RunCommand::Pause;
    resetEngine.step(pause);
    RunInput resetPaused = tickInput();
    resetPaused.command = RunCommand::Reset;
    resetEngine.step(resetPaused);
    expectEqual(resetEngine.pausedPhase_, RunPhase::Inactive, "reset clears paused-phase history");
    expectTrue(resetEngine.lastSampleProgressAtMs_ == 0U, "reset clears progress time history");
    expectTrue(resetEngine.lastSampleProgressGrams_ == 0.0f, "reset clears progress mass history");
    RunOutput ignoredResume = resetEngine.step(RunInput{0U, RunCommand::Resume});
    expectEqual(ignoredResume.phase, RunPhase::Inactive, "reset clears paused-phase history");

    RunOutput restarted = resetEngine.step(startNormal(3.0f, 40000U));
    expectEqual(restarted.phase, RunPhase::SampleFilling, "reset clears progress history for the next run");
  }

  // ---- Phase 3: ordinary endpoint lifecycle ----
  {
    RunEngine ordinaryEngine;
    enterRunning(ordinaryEngine);
    RunOutput dose = ordinaryEngine.step(runningTick(10U, 5.0f));
    expectEqual(dose.phase, RunPhase::Dosing, "fresh valid ordinary reading begins one dose");
    expectEqual(dose.status, RunStatusCode::Dosing, "dose reports dosing status");
    expectTrue(dose.titrant.mode == PumpMode::RunForMs, "ordinary dose runs titrant once");
    expectTrue(dose.titrant.durationMs == 450U, "ordinary dose uses adaptive pulse duration");
    expectPumpStopped(dose.sample, "ordinary dose keeps sample stopped");
    expectTrue(ordinaryEngine.dynamics_.count == 1U, "ordinary sample is stored in engine dynamics");

    RunOutput duringPulse = ordinaryEngine.step(runningTick(100U, 5.0f));
    expectEqual(duringPulse.phase, RunPhase::Dosing, "unexpired pulse remains dosing");
    expectPumpsStopped(duringPulse);

    RunOutput settling = ordinaryEngine.step(runningTick(460U, 5.0f));
    expectEqual(settling.phase, RunPhase::Settling, "expired pulse enters settling");
    expectTrue(settling.hasRequestedSettleMs, "settling exposes requested settle only as metadata");
    expectTrue(settling.requestedSettleMs == 3000U, "settling reports stored requested settle");
    expectPumpsStopped(settling);
  }

  {
    RunEngine faultEngine;
    enterRunning(faultEngine);
    RunInput invalid = runningTick(10U, 5.0f);
    invalid.sensor.sensorValid = false;
    RunOutput sensorFault = faultEngine.step(invalid);
    expectEqual(sensorFault.phase, RunPhase::Error, "invalid sensor enters error");
    expectEqual(sensorFault.status, RunStatusCode::SensorFault, "invalid sensor has sensor-fault status");
    expectTrue(sensorFault.stopReason == RunStopReason::SensorFault, "invalid sensor has sensor-fault reason");
    expectPumpsStopped(sensorFault);

    RunEngine scaleEngine;
    enterRunning(scaleEngine);
    RunInput invalidScale = runningTick(10U, 5.0f);
    invalidScale.sensor.scaleValid = false;
    RunOutput scaleFault = scaleEngine.step(invalidScale);
    expectEqual(scaleFault.phase, RunPhase::Error, "invalid scale enters error");
    expectEqual(scaleFault.status, RunStatusCode::ScaleFailure, "invalid scale has scale-failure status");
    expectTrue(scaleFault.stopReason == RunStopReason::ScaleFailure, "invalid scale has scale-failure reason");
    expectPumpsStopped(scaleFault);

    RunEngine staleEngine;
    enterRunning(staleEngine);
    RunInput stale = runningTick(10U, 5.0f);
    stale.sensor.sensorFresh = false;
    RunOutput waiting = staleEngine.step(stale);
    expectEqual(waiting.phase, RunPhase::Running, "stale reading waits in running");
    expectPumpsStopped(waiting);
  }

  {
    RunEngine limitEngine;
    enterRunning(limitEngine, 100U);
    RunInput massLimit = runningTick(110U, 5.0f);
    massLimit.sensor.consumedTitrantGrams = 2.0f;
    massLimit.context.settings.maxConsumedGrams = 2.0f;
    RunOutput massFault = limitEngine.step(massLimit);
    expectEqual(massFault.phase, RunPhase::Error, "mass limit blocks dose");
    expectEqual(massFault.status, RunStatusCode::MassLimit, "mass limit has stable status");
    expectPumpsStopped(massFault);

    RunEngine timeEngine;
    enterRunning(timeEngine, UINT32_MAX - 500U);
    RunInput timeLimit = runningTick(600U, 5.0f);
    timeLimit.context.settings.maxTimeSeconds = 1U;
    RunOutput timeFault = timeEngine.step(timeLimit);
    expectEqual(timeFault.phase, RunPhase::Error, "time limit survives rollover");
    expectEqual(timeFault.status, RunStatusCode::TimeLimit, "time limit has stable status");
    expectPumpsStopped(timeFault);
  }

  {
    RunEngine holdEngine;
    enterRunning(holdEngine);
    RunInput firstInRange = runningTick(10U, 7.0f);
    firstInRange.context.settings.holdSeconds = 2U;
    RunOutput holding = holdEngine.step(firstInRange);
    expectEqual(holding.phase, RunPhase::Running, "endpoint starts continuous hold");
    expectEqual(holding.status, RunStatusCode::HoldingEndpoint, "endpoint hold reports holding status");
    expectPumpsStopped(holding);

    RunInput outOfRange = runningTick(1010U, 6.0f);
    outOfRange.context.settings.holdSeconds = 2U;
    RunOutput resetHold = holdEngine.step(outOfRange);
    expectEqual(resetHold.phase, RunPhase::Dosing, "out-of-range sample resets hold before dosing");

    RunInput holdAgain = runningTick(2000U, 7.0f);
    holdAgain.context.settings.holdSeconds = 2U;
    holdEngine.phase_ = RunPhase::Running;
    RunOutput restartedHold = holdEngine.step(holdAgain);
    expectEqual(restartedHold.status, RunStatusCode::HoldingEndpoint, "fresh in-range sample restarts hold");
    RunInput completeHold = runningTick(4000U, 7.0f);
    completeHold.context.settings.holdSeconds = 2U;
    RunOutput done = holdEngine.step(completeHold);
    expectEqual(done.phase, RunPhase::Done, "continuous endpoint hold finalizes");
    expectEqual(done.status, RunStatusCode::TargetReached, "continuous hold has target status");
    expectTrue(done.finalizeResult, "continuous hold finalizes result");
    expectPumpsStopped(done);

    RunEngine immediateEngine;
    enterRunning(immediateEngine);
    RunOutput immediate = immediateEngine.step(runningTick(10U, 7.0f));
    expectEqual(immediate.phase, RunPhase::Done, "zero endpoint hold completes immediately");
  }

  {
    RunEngine settleEngine;
    enterRunning(settleEngine);
    settleEngine.step(runningTick(10U, 5.0f));
    settleEngine.step(runningTick(460U, 5.0f));
    RunInput stale = runningTick(1500U, 5.0f);
    stale.sensor.sensorFresh = false;
    RunOutput stillSettling = settleEngine.step(stale);
    expectEqual(stillSettling.phase, RunPhase::Settling, "stale sensor facts cannot advance settling");
    RunInput settled = runningTick(3500U, 5.0f);
    RunOutput backToRunning = settleEngine.step(settled);
    expectEqual(backToRunning.phase, RunPhase::Running, "elapsed settle needs fresh settled sensor to resume");
    expectPumpsStopped(backToRunning);

    RunEngine maxSettleEngine;
    enterRunning(maxSettleEngine);
    maxSettleEngine.step(runningTick(10U, 5.0f));
    maxSettleEngine.step(runningTick(460U, 5.0f));
    RunInput unready = runningTick(3500U, 5.0f);
    unready.sensor.sensorValid = false;
    RunOutput maxExpired = maxSettleEngine.step(unready);
    expectEqual(maxExpired.phase, RunPhase::Running, "maximum settle returns safely without sensor transition facts");
    expectPumpsStopped(maxExpired);
  }

  {
    RunEngine resetStateEngine;
    enterRunning(resetStateEngine);
    resetStateEngine.step(runningTick(10U, 5.0f));
    RunInput pause = tickInput();
    pause.command = RunCommand::Pause;
    RunOutput paused = resetStateEngine.step(pause);
    expectEqual(paused.phase, RunPhase::Paused, "pause immediately interrupts active dose");
    expectPumpsStopped(paused);
    RunInput resume = tickInput();
    resume.command = RunCommand::Resume;
    RunOutput resumed = resetStateEngine.step(resume);
    expectEqual(resumed.phase, RunPhase::FilterWarmup, "resume never restarts a pulse");
    expectPumpsStopped(resumed);

    RunInput resetRun = tickInput();
    resetRun.command = RunCommand::Reset;
    resetStateEngine.step(resetRun);
    expectTrue(resetStateEngine.dynamics_.count == 0U, "reset clears engine dynamics");
    expectTrue(!resetStateEngine.endpointHold_.active(), "reset clears endpoint hold state");
    expectTrue(resetStateEngine.runStartedAtMs_ == 0U, "reset clears run start time");
    expectTrue(resetStateEngine.pulseStartedAtMs_ == 0U, "reset clears pulse start time");
    expectTrue(resetStateEngine.settleStartedAtMs_ == 0U, "reset clears settle start time");
  }

  if (failures != 0) {
    return 1;
  }

  std::cout << "All RunEngine shell tests passed\n";
  return 0;
}
