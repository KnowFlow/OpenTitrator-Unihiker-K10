# Pump Flow Control Design

Date: 2026-07-09

## Goal

Add adjustable pump speed control for both pumps so the titrator can run slower in real experiments and reduce endpoint overshoot. The feature should support saved defaults for automatic workflows and separate manual testing for each pump.

## Context

The current controller already prevents overshoot by using adaptive pulse durations: the titrant pump receives 25 ms, 60 ms, 150 ms, or 450 ms pulses depending on endpoint error and signal dynamics. However, the servo PWM run pulse is fixed at `1000us` for both pumps, so each pulse can still deliver too much liquid when tubing, liquid viscosity, or pump hardware make the flow rate high.

`PumpController` already supports `runForMsAtUs(ms, pulseUs)`, and manual titrant actions already accept a raw `us` value internally. The missing pieces are persisted default pump PWM settings, UI controls, consistent use in automatic and calibration flows, and per-pump manual overrides.

## Design

### Data Model

Add two persisted calibration settings:

- `titrant_run_us`: default PWM pulse width for the P0 titrant pump.
- `sample_run_us`: default PWM pulse width for the P1 sample pump.

Defaults remain `1000us` to preserve current behavior after upgrade. The editable range is `1000..1500us`. `1500us` is the stop midpoint for this servo-style pump signal, so values closer to `1500us` should run slower. Reverse or opposite-direction values are not exposed in this feature.

These settings belong with pump calibration data because flow rate measurements only make sense at a specific pump speed. Store them in the existing `cal` Preferences namespace with the pump g/s values.

### Firmware Behavior

`PumpController` should retain its small API and add a way to update the default run pulse width after construction. Existing calls to `runForMs(ms)` should continue to work, but they should use the controller's current default run pulse.

Automatic paths should use saved defaults:

- Titrant auto-dosing uses `titrant_run_us`.
- Stoichiometric predose uses `titrant_run_us`.
- Sample filling uses `sample_run_us`.
- Pump calibration uses each pump's current saved speed, so measured g/s matches the speed used during normal operation.

Manual paths should support temporary per-command overrides:

- `manual_titrant` accepts `titrant_us`, falling back to legacy `us`, then to `titrant_run_us`.
- `manual_sample` accepts `sample_us`, falling back to legacy `us`, then to `sample_run_us`.

Temporary manual overrides do not change saved defaults unless submitted through `/set`.

### Web UI

Add pump speed fields to the Calibration tab near pump flow:

- `Titrant pump PWM us`
- `Sample pump PWM us`

These fields should submit to `/set` and persist the defaults. The short helper text should explain: `1000 fast, 1500 stop/slow limit`.

Add separate speed inputs to the Manual tab:

- A titrant manual PWM field prefilled from `titrant_run_us`.
- A sample manual PWM field prefilled from `sample_run_us`.

The manual form continues to use one run duration field, but each pump button sends the matching PWM override. Manual actions remain blocked while titration or calibration is active.

### API Changes

`/set` accepts:

- `titrant_run_us`
- `sample_run_us`

Both values are constrained to `1000..1500` and saved with calibration settings.

`/status` returns:

- `titrant_run_us`
- `sample_run_us`

`/action` accepts:

- `titrant_us` for `manual_titrant`
- `sample_us` for `manual_sample`
- legacy `us` as a compatibility fallback

### Error Handling and Safety

Invalid or missing PWM values are constrained to the safe range. The default remains the current behavior, so existing devices continue to run as before until the user changes the settings.

Emergency stop, active-state blocking, OTA pump stop, and all existing pump stop paths remain unchanged.

### Documentation

Update the user documentation to explain:

- Pump flow calibration is tied to the configured PWM speed.
- Slowing the titrant pump can reduce endpoint overshoot when pulses still dose too much liquid.
- Manual operation can test each pump's speed before saving or recalibrating.
- After changing pump speed, users should recalibrate pump g/s.

### Verification

Implementation should be verified by:

- Building the firmware.
- Confirming `/set` persists both PWM defaults and `/status` reports them.
- Confirming automatic titrant dosing and sample filling use saved defaults.
- Confirming pump calibration uses saved defaults.
- Confirming manual P0 and P1 actions can use different temporary PWM values.
- Confirming legacy `us` still works for manual actions.
- Updating relevant docs without changing unrelated behavior.

## Out of Scope

This feature does not add closed-loop g/s control, multi-point PWM-to-flow calibration, reverse pump direction, or automatic selection of pump speed from endpoint slope. Those can be considered later if the simple saved PWM control is not enough in practice.
