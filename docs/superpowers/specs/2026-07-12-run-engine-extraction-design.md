# Run Engine Extraction Design

Date: 2026-07-12

## Goal

Extract the active titration lifecycle from `ph_titrator.ino` into a pure, host-testable RunEngine. Centralize state transitions, timing, endpoint confirmation, safety stops, and pump intentions without changing current dosing, calibration, Web, authentication, display, or result behavior.

## Scope

RunEngine owns these active experiment phases:

- `SampleFilling`
- `FilterWarmup`
- `Running`
- `Dosing`
- `Settling`
- `Paused`
- `Done`
- `Error`

The Arduino sketch continues to own:

- `SetupMode`
- `SetupTarget`
- `SetupReady`
- Pump calibration
- Sensor drivers and filters
- Pump drivers and PWM/burst implementation
- Web routes, authentication, and command admission
- Screen rendering
- Preferences and method storage
- OTA transport and firmware update implementation

This phase does not split the Web, display, sensor, calibration, or persistence code into additional modules.

## Chosen Approach

Create a pure RunEngine instead of moving `runController()` into another file or adopting full event sourcing.

A direct function move would retain global variables and hardware dependencies, producing no useful test seam. Full event sourcing would provide complete audit replay but adds storage and event-schema complexity not required by the current controller. RunEngine accepts complete input facts and returns complete output intentions, providing deterministic replay without committing to persistent event storage.

## Files and Modules

### `run_types.h`

Defines the plain C++ domain types shared by RunEngine, the Arduino adapter, and native tests:

- `RunPhase`
- `RunCommand`
- `SensorSnapshot`
- `RunInput`
- `PumpMode`
- `PumpIntent`
- `RunOutput`
- Stable run status codes

These types contain no Arduino `String`, WebServer, Servo, Preferences, or sensor-driver references.

### `run_engine.h/.cpp`

Owns active-run state and the transition implementation. Its small external interface supports:

- Resetting to an inactive state before Setup starts a new run.
- Starting a normal run with automatic sample filling.
- Starting from an existing sample.
- Applying a command and sensor/time input through `step()`.
- Reading the current phase and durable run result snapshot.

RunEngine owns only historical state required by the experiment lifecycle, including phase start times, run start time, pause recovery phase, sample progress timers, endpoint hold state, dynamics/EQP state, active dose/settle timing, consumed/sample amounts, stop reason, and final-result trigger data.

### `control_logic.h`

Remains the mathematical control module. RunEngine calls existing adaptive-dose, EQP, endpoint, result, filter/dynamics, and rollover-safe time logic rather than duplicating it.

### Arduino Adapter in `ph_titrator.ino`

The sketch converts hardware/global facts into `RunInput`, calls RunEngine once per loop/command, executes `RunOutput` pump intentions, and maps stable status codes to existing display/Web strings.

The adapter contains no active-run transition policy.

## Input Model

Each engine step receives a complete input snapshot:

```text
RunInput
├── nowMs
├── RunCommand
├── SensorSnapshot
├── TitrationSettings
├── method/result context
├── calibrated pump flow facts
└── safety facts, including OTA lock
```

`SensorSnapshot` contains only facts already computed by the hardware and filtering adapters:

- pH and millivolts
- Active control value
- Sensor validity and freshness
- Scale validity and current reactor mass
- Consumed titrant mass
- Delivered sample mass
- Signal derivative/settled facts required by the current algorithms

RunEngine does not own or retain sensor objects. It stores only the historical samples required by existing control mathematics.

The caller supplies `nowMs`; RunEngine never calls `millis()`.

## Output Model

Every step returns a complete `RunOutput`:

```text
RunOutput
├── current RunPhase
├── titrant PumpIntent
├── sample PumpIntent
├── pulse and settle timing
├── stable status code
├── stop reason
├── result-finalization intent
└── EQP-record intent
```

Each pump intent is one of:

- `Stop`
- `RunContinuous`
- `RunForMs`

The output defaults to `Stop` for both pumps. A pump runs only when the current transition explicitly emits a running intent. The sketch executes the complete intent every loop rather than relying on a previous command to remain correct.

RunEngine does not know servo PWM microseconds or call pump methods. The adapter applies existing saved PWM and burst behavior when executing the intent.

## Commands

Run commands include:

- Start normal run
- Start with existing sample
- Pause
- Resume
- Reset
- Tick/no user command

Web and hardware-button adapters translate their existing operations into the same command values. Authentication and command admission happen before a Web command reaches RunEngine.

Setup and calibration do not become RunEngine commands in this phase.

## State and Transition Rules

### Start

Normal start resets active-run history, records the run start time, and enters `SampleFilling` unless configured sample mass is effectively zero. Existing-sample start records the configured sample amount and enters `FilterWarmup`.

### Sample Filling

The sample pump runs continuously only while scale input is valid and progress/absolute time limits remain satisfied. Reaching the configured sample amount stops the sample pump, resets signal warmup history, and enters `FilterWarmup`. Missing scale, stalled progress, or maximum fill time enters `Error` with both pumps stopped.

### Filter Warmup

Both pumps remain stopped. A fresh valid control signal transitions to `Running`. Sensor faults enter `Error`.

### Running

Fresh valid inputs drive current endpoint/EQP and adaptive-dose rules. RunEngine preserves existing stoichiometric predose behavior, endpoint Hold behavior, maximum mass/time checks, EQP tracking, and result-finalization semantics.

### Dosing

The titrant pump emits its current `RunForMs` intent. When the pulse duration elapses, the output stops the pump and enters `Settling`.

### Settling

Both pumps remain stopped. The phase respects minimum/requested settle time, maximum settle time, freshness, and stability. Ordinary EP runs return through the common endpoint Hold path; EQP recording remains method-specific.

### Pause and Resume

Pause immediately emits stop intentions and remembers the logical phase required for safe recovery. Resume never continues an interrupted active pulse. It re-enters `FilterWarmup`, forcing fresh sensor stabilization before returning to dosing decisions. Sample filling may restart only through the explicit sample-fill recovery rule and must reset its progress timers.

### Done and Error

Both phases always emit stop intentions and are terminal until explicit Reset. Error retains a stable stop reason. Done retains final result/EQP facts. Reset clears run history and returns control to the sketch's Setup flow.

### OTA and Safety Lock

An asserted safety/OTA lock immediately emits stop intentions and prevents any new active transition. The existing OTA handler remains responsible for its visible Error/lock lifecycle; RunEngine cannot override it.

## Safety Invariants

- Both pump outputs default to `Stop` on every step.
- Only explicit active phases may emit a running intent.
- Invalid sensors, OTA lock, maximum run time, maximum mass, sample-fill timeout, and sample-progress timeout stop both pumps in the same step.
- `Paused`, `Done`, and `Error` always stop both pumps.
- Resume never restarts an interrupted dose pulse.
- State time comparisons use unsigned elapsed arithmetic and remain correct across `millis()` rollover.
- Error and Reset clear endpoint Hold state.
- EQP and ordinary EP completion remain separate.
- No Web or hardware adapter may change active phase directly after migration.

## Stable Status Codes

RunEngine returns stable enum status codes such as:

- Filling sample
- Waiting for stable signal
- Checking endpoint
- Dosing
- Settling
- Holding endpoint
- Paused
- Target reached
- Equivalence point reached
- Sensor fault
- Scale failure
- Sample progress timeout
- Mass limit
- Time limit
- OTA/safety lock

The sketch maps these codes to existing English display strings. This avoids Arduino `String` in the core and allows later localization without modifying RunEngine.

## Migration Strategy

### Step 1: Add the Pure Module

Implement RunEngine and host tests while the sketch still uses the existing `runController()`. Lock current behavior with scenario tests before changing firmware routing.

### Step 2: Compare Behavior

Feed representative input sequences through the new engine and equivalent existing-behavior fixtures. Compare phases, pump intentions, timing, and stop reasons for normal and failure cases.

### Step 3: Switch the Arduino Adapter

Replace active-run branches in the sketch with RunEngine input/output translation. Remove the old active-run transition implementation after parity tests pass. Setup, calibration, hardware, Web, authentication, and display adapters remain in the sketch.

## Testing

Native tests use virtual time and complete sensor snapshots. They test public RunEngine output, not private helpers.

Required transition coverage:

- Normal automatic sample fill
- Start with existing sample
- pH EP increase and decrease
- mV EP increase and decrease
- EDTA EQP completion
- Endpoint Hold completion, exit, and restart
- Adaptive dosing and stoichiometric predose
- Settling minimum, maximum, freshness, and stability
- Pause from filling, running, dosing, and settling
- Resume through fresh warmup without continuing a pulse
- Sensor invalidity and sensor fault
- Scale invalidity
- Sample progress and absolute fill timeout
- Maximum titrant mass and maximum run time
- OTA lock in every active phase
- Reset from Done and Error
- `uint32_t` time rollover in dosing, settling, Hold, fill, and run timeout

Scenario tests cover at least:

- One complete ordinary EP run
- One complete EDTA EQP run
- One run ending in a sensor fault

Every phase assertion includes both pump intentions.

Integration verification includes:

- Existing control-logic tests
- Authentication/session tests
- Web authentication static tests
- OTA/sketch safety static tests updated to require RunEngine routing
- PlatformIO firmware build
- GitHub Actions native and firmware jobs

## Completion Criteria

- `ph_titrator.ino` no longer owns active-run state transitions.
- `runController()` is removed or reduced to a thin RunEngine adapter.
- RunEngine compiles and runs on the host without Arduino dependencies.
- Web and hardware commands enter active runs through RunEngine commands.
- Existing pump PWM, burst parameters, dose durations, settle behavior, result formulas, methods, authentication routes, and public Web contracts remain unchanged.
- All native, static, and firmware builds pass.
- On-device deployment remains blocked until the deferred labelled-device Task 7 is completed onsite.

## Future Work

After this extraction:

1. Validate pump calibration and bind it to pump PWM speed.
2. Improve signal-slope estimation through recorded-curve replay.
3. Add resilient experiment summaries and browser IndexedDB records.
4. Consider persistent event replay only if operational diagnostics require it.

