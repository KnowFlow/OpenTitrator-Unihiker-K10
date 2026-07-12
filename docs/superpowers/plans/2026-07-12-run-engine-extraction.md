# RunEngine Extraction Implementation Plan

**Goal:** Move the active titration lifecycle out of `ph_titrator.ino` into a deterministic, host-testable `RunEngine` without changing the approved dosing mathematics or hardware behavior.

**Architecture:** Build a pure C++17 state engine first, implement the lifecycle in test-backed vertical slices, then convert the sketch into an adapter. `RunEngine` owns all history required for decisions; `control_logic.h` remains stateless mathematics. Hardware, Web/authentication, display, persistence, calibration, setup mode, and OTA transport remain in the sketch.

**Constraints:** No Arduino headers, `String`, `millis()`, or heap allocation in the engine. Every `step()` defaults both pump outputs to `Stop`. `sizeof(RunEngine) <= 4096`; `sizeof(RunInput) + sizeof(RunOutput) <= 1024`.

## Phase 1 — Contracts and fail-safe shell

### Task 1: Define the pure interface

**Create:**
- `ph_titrator/run_types.h`
- `ph_titrator/run_engine.h`
- `ph_titrator/run_engine.cpp`
- `tests/run_engine_test.cpp`

Define:
- `RunPhase`: `Inactive`, `SampleFilling`, `FilterWarmup`, `Running`, `Dosing`, `Settling`, `Paused`, `Done`, `Error`.
- `RunCommand`: `Tick`, `StartNormal`, `StartExistingSample`, `Pause`, `Resume`, `Reset`.
- Stable `RunStatusCode` values for progress, completion, safety, sensor, scale, timeout, mass, and time outcomes, including `ReStabilizingAfterResume`.
- `PumpMode`: `Stop`, `RunContinuous`, `RunForMs`; `PumpIntent` carries its own duration.
- `SensorSnapshot`, `RunContext`, `RunInput`, and `RunOutput` as fixed-size value types.

`RunOutput` contains phase, titrant/sample intents, status, stop reason, `finalizeResult`, `recordEqpPoint`, and optional `requestedSettleMs`. The settle value is display/diagnostic metadata only; adapters must never use it to drive transitions.

Write failing tests first for construction, reset, unknown/no-op ticks, safety lock, default pump stops, and size budgets. Implement only enough shell behavior to pass them.

**Verify:**
```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests/run_engine_test.cpp ph_titrator/run_engine.cpp -o run_engine_test.exe
./run_engine_test.exe
```

## Phase 2 — Start, filling, warmup, pause, resume

### Task 2: Implement safe lifecycle entry

Add tests and implementation for:
- `StartNormal` -> `SampleFilling`; sample pump runs continuously.
- `StartExistingSample` -> `FilterWarmup`; neither pump runs.
- Sample-fill completion, 12 s no-progress timeout, and 5 s fill allowance.
- Warmup requiring valid, fresh, settled readings before `Running`.
- `Pause` from every active phase immediately stops both pumps and records the prior phase.
- `Resume` never resumes a pulse; it always enters `FilterWarmup` with `ReStabilizingAfterResume`.
- Reset returns to `Inactive` and clears all run-owned history.
- Unsigned time rollover behavior.

Keep all timers and progress history inside `RunEngine`.

## Phase 3 — Ordinary endpoint lifecycle

### Task 3: Implement Running, Dosing, Settling, and Hold

Use existing pure calculations from `control_logic.h`, including `decideAdaptiveDose`, while moving live `TitrationDynamics` and `EndpointHoldTracker` instances into `RunEngine`.

Test before implementation:
- Invalid/stale sensor input never doses and produces the correct terminal or waiting status.
- A dose decision emits exactly one `RunForMs` intent and enters `Dosing`.
- The pulse duration is carried only by `PumpIntent`; there is no duplicate pulse field.
- Pulse expiry stops the pump and enters `Settling`.
- Only engine time and sensor facts advance settling; adapter metadata cannot do so.
- Endpoint hold resets on instability and completes only after the configured continuous hold.
- Mass and elapsed-time limits stop safely.
- Every transition and error path stops both pumps unless that exact step explicitly commands one.

## Phase 4 — EQP and stoichiometric predose

### Task 4: Preserve equivalence-point behavior

Move live EQP tracking, initial/sample/consumed mass, predose progress, and result-finalization decisions into `RunEngine`. Keep approved constants unchanged:
- Predose ratio: `0.70`.
- Predose step: `2 g`.
- Fallback pulse: `2000 ms`.
- Maximum pulse: `5000 ms`.
- Settle interval: `6000 ms`.

Add complete scenario tests for normal EQP completion, predose fallback, EQP point recording, invalid readings, scale failure, mass/time limits, pause during pulse, and reset after completion/error.

## Phase 5 — Arduino adapter cutover

### Task 5: Integrate the engine into the sketch

**Modify:**
- `ph_titrator/ph_titrator.ino`
- `tests/sketch_safety_static_test.ps1`

Add thin adapter functions such as:
- `buildRunInput()` — snapshots time, sensors, configuration, and the OTA/auth safety fact.
- `applyPumpIntent()` — translates intents to the existing pump driver.
- `applyRunOutput()` — updates display/result persistence without making transitions.
- `runStatusText()` — includes the user-facing “正在重新稳定信号” resume status.

Use one command mapping at every entry point:

| Source | RunCommand |
|---|---|
| Web `POST /run/start` and panel Start | `StartNormal` |
| Web `POST /run/start_existing` | `StartExistingSample` |
| Web `POST /run/stop` and panel Pause | `Pause` |
| Start while paused and panel Resume | `Resume` |
| Web `POST /run/reset` | `Reset` |
| Main loop | `Tick` |

OTA is not a special engine command. The adapter passes a safety-lock fact, and successful OTA/reboot continues to enter `SetupMode` as already approved.

Delete the legacy lifecycle globals and transition branches only after the corresponding adapter tests pass. Extend static tests to reject reintroduction of sketch-owned active-state timers, pulse progression, EQP/Hold instances, or direct transition logic.

## Phase 6 — CI, documentation, and verification

### Task 6: Make the boundary durable

**Modify:**
- `.github/workflows/ci.yml`
- `README.md` and/or the existing architecture/manual document

Add the native RunEngine test to CI for both supported host compilers where available. Document module ownership, command mapping, resume semantics, memory budgets, and the rule that `requestedSettleMs` is metadata.

Run the full relevant suite:
```powershell
g++ -std=c++17 -Wall -Wextra -Werror tests/run_engine_test.cpp ph_titrator/run_engine.cpp -o run_engine_test.exe
./run_engine_test.exe
Get-ChildItem tests -Filter '*.ps1' | ForEach-Object { & $_.FullName }
git diff --check
```

If a PlatformIO build environment is available, compile the firmware as an additional gate. Do not claim onsite deployment: device installation and labelled-device smoke testing remain blocked by Task 7 of `2026-07-10-web-auth-command-security.md`; the onsite operator owns label attachment and device verification.

## Execution checkpoints

Commit after each phase or independently reviewable vertical slice. At every checkpoint:
1. Run the new native tests and existing safety tests.
2. Review that both pump intents default to `Stop` on every path.
3. Confirm no hardware/Web/display code entered the pure engine.
4. Confirm `control_logic.h` gained no live experiment state.
5. Preserve ignored factory credentials and the unrelated untracked `docs/CODE_REVIEW.md`.
