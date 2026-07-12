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

  if (failures != 0) {
    return 1;
  }

  std::cout << "All RunEngine shell tests passed\n";
  return 0;
}
