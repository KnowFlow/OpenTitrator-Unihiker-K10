# Safety and Correctness Baseline Design

Date: 2026-07-10

## Goal

Establish the first safety and correctness baseline for the K10 potentiometric titrator without performing the full `ph_titrator.ino` decomposition. This phase fixes HTTP OTA pump safety, makes endpoint hold confirmation consistent, and makes pump deadlines safe across `millis()` rollover.

## Scope

This phase includes:

- Stop and lock both pumps when an HTTP OTA upload starts.
- Reboot after a successful HTTP OTA update and enter `SetupMode` through normal startup.
- Keep both pumps stopped after an HTTP OTA failure and provide a Web safe-reset action that returns to `SetupMode`.
- Route all ordinary EP endpoint completion through one hold-confirmation rule.
- Replace direct pump deadline comparison with rollover-safe time arithmetic.
- Add native tests for endpoint hold and rollover-safe deadlines.

This phase does not include Web authentication, POST/CSRF conversion, full state-machine extraction, task watchdog configuration, hardware interlocks, curve persistence, EQP algorithm changes, or bilingual UI work.

## Constraints

- Recovery must not depend on the K10 A/B hardware buttons because they are not currently operable.
- OTA recovery must be available through the Web interface.
- OTA recovery must never resume the interrupted titration automatically.
- Existing pump PWM, burst timing, adaptive dose sizes, and EQP behavior remain unchanged.
- Changes should create a small testable seam rather than mechanically splitting the 3000-line sketch into shallow files.

## Approach

Use a targeted testable seam instead of either an inline-only patch or a full Run module extraction.

`control_logic.h` will own pure endpoint-hold and time-deadline logic. The Arduino sketch will pass fresh sensor readings and `millis()` values into that logic and will remain responsible for executing pump commands and changing the device state. HTTP OTA handling remains in the sketch but gains an explicit OTA safety lock that prevents controller and manual-operation paths from restarting either pump.

This preserves a small first-phase diff while creating logic that native C++ tests can exercise. A later phase can move the complete Run state and transitions behind a deeper interface without discarding this work.

## Endpoint Hold Confirmation

Ordinary EP methods use one hold-confirmation path from both `Running` and `Settling`.

1. A fresh, valid sensor reading enters the configured endpoint range.
2. Both pumps remain stopped and the hold timer starts.
3. Only subsequent fresh, valid readings may advance or invalidate the hold.
4. If every fresh reading remains inside the endpoint range until `holdSeconds` elapses, the run enters `Done` with `TargetReached`.
5. If any fresh reading exits the endpoint range, the hold timer resets and the controller returns to normal endpoint evaluation.
6. Re-entering the endpoint range starts a new full hold period.
7. When `holdSeconds` is zero, a fresh in-range reading completes immediately.

The hold tracker resets when a run starts, pauses, resets, encounters a sensor error, or changes away from the active EP workflow. Stale readings, Web polling, and elapsed wall time without a fresh reading cannot complete the hold.

Automatic EQP methods retain their existing equivalence-point completion behavior and do not use the EP hold tracker in this phase.

## HTTP OTA Safety Flow

When `UPLOAD_FILE_START` is received:

1. Stop the titrant pump.
2. Stop the sample pump.
3. Stop any manual PWM sweep.
4. Clear the active pump pulse.
5. Set an OTA safety lock before flash writing begins.
6. Move the visible device status to an OTA-safe state.

While the OTA safety lock is active:

- The titration controller cannot start or renew a pump command.
- Calibration cannot start a pump.
- Manual Web pump and sweep commands are rejected.
- Reset commands cannot bypass an OTA upload that is still active.

On successful upload validation, the device schedules a restart. Normal startup clears previous run data and enters `SetupMode`; it does not restore or resume the interrupted experiment.

On upload start, write, validation, or completion failure, the device remains in `Error` with status `OTA failed`, both pumps remain stopped, and the OTA safety lock prevents pump commands. The Web interface exposes a safe-reset action. Safe reset stops both pumps again, clears run and hold data, clears the failed OTA lock, and enters `SetupMode`.

The safe-reset action must be usable without A/B hardware buttons.

## Rollover-Safe Pump Deadlines

Pump deadlines continue to use a 32-bit `millis()` value and a zero sentinel for “no active deadline.” Expiry uses signed-difference arithmetic rather than direct absolute comparison:

```cpp
static inline bool deadlineReached(uint32_t now, uint32_t deadline) {
  return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}
```

This is safe across `UINT32_MAX` rollover when deadlines are less than `INT32_MAX` milliseconds into the future. All current pump commands are far below that limit.

The existing 30-second continuous-run lease remains unchanged: normal controller calls may renew it. This phase corrects rollover behavior but does not claim that a software lease protects against a frozen CPU or failed output device. Task watchdog and hardware pump-enable interlocks require a separate design and hardware validation.

## Error Handling

- Invalid or stale endpoint readings cannot complete a hold.
- Sensor errors reset endpoint hold and follow the existing error stop path.
- OTA failure never returns automatically to a runnable state.
- Web safe reset always stops both pumps before clearing the OTA failure.
- OTA success always restarts before returning to `SetupMode`.
- An OTA safety lock has precedence over titration, calibration, sample filling, and manual commands.

## Testing

Native control tests will cover:

- First entry into endpoint range starts hold without completing.
- Continuous in-range fresh readings complete after `holdSeconds`.
- Leaving the range resets hold.
- Re-entry requires a complete new hold period.
- No fresh reading means no completion.
- `holdSeconds == 0` completes immediately on a fresh in-range reading.
- Deadline expiry before rollover.
- Deadline expiry across `UINT32_MAX` rollover.
- A zero deadline is inactive.
- Existing EQP decisions remain unchanged.

Firmware verification will include:

- Native control test compilation and execution in CI.
- PlatformIO firmware compilation in CI.
- Inspection of the HTTP OTA start path to confirm both pumps stop before `Update.begin`.
- Inspection of command admission to confirm OTA lock blocks automatic, calibration, and manual pump starts.
- Web safe reset from `OTA failed` to `SetupMode`.
- Successful OTA restart to `SetupMode` without run restoration.

## Future Phases

After this baseline is complete and verified:

1. Convert Web mutations to authenticated POST commands with unified command admission.
2. Extract a host-testable Run module that owns state transitions, timers, safety invariants, and pump intents.
3. Validate pump calibration values and bind calibration validity to pump speed.
4. Improve slope estimation using recorded-curve replay tests.
5. Add resilient experiment-record persistence without high-frequency flash writes.

