# K10 Potentiometric Titrator Roadmap

[中文](ROADMAP_CN.md) · [README](../README.md) · [User Manual](MANUAL.md)

Status as of **2026-07-14**. The project has completed the core transition from a fixed pH titrator to a configurable potentiometric-titration platform. The next phase is led by physical validation and production acceptance rather than more control-surface features.

## Status legend

- **Complete**: implemented and covered by automated checks.
- **Validation**: implemented, but representative chemistry or labelled-device acceptance is still required.
- **Planned**: not implemented.

## Completed foundation

- [x] Four method presets: pH endpoint, mV endpoint, EDTA hardness, and manual.
- [x] Per-method endpoint, target, signal trend, titrant, sample/max mass, EP-control settings, result formula, blank, densities, manual molarity, and manual factor.
- [x] Configurable control band, endpoint hold, signal stability, settle limits, and maximum run time.
- [x] Adaptive pulse dosing, predictive stop, rollover-safe timers, watchdog/safety-stop behavior, and calibration-validity checks.
- [x] A host-testable `RunEngine` owning the active experiment lifecycle, including pause/resume warmup, dosing, settling, endpoint hold, EQP tracking, completion, and emergency stop.
- [x] Acid/base concentration, EDTA hardness as CaCO3, and manual-factor calculations.

## Web, records, and security

- [x] Live pH/mV curves with mass/time axes, automatic and manually corrected EQP markers, CSV/JSON export, and non-applying parameter suggestions.
- [x] Finalized run records with sample ID, batch/reference, operator, notes, device snapshot, result, EQP facts, and printable report.
- [x] Browser-only IndexedDB persistence for the newest 50 completed or aborted records; active records remain memory-only.
- [x] Offline replay analysis for live or imported Run Record JSON, including duplicate-mass collapse, centered slope estimation, and quality guidance.
- [x] Complete English/Chinese Web UI with a remembered language selection and an on-demand Chinese resource.
- [x] Authenticated, POST-only mutations; centralized command/state admission; rate limits; 30-minute inactive-write session expiry; public read-only telemetry; and anonymous stop-only panic.
- [x] Per-device factory recovery, USB administrator recovery, and authenticated HTTP OTA with a pump safety lock.

## Current validation gates

- [ ] Run real hardness samples across the expected concentration range and tune the EDTA EQP minimum peak, decline ratio, and confirmation count.
- [ ] Compare replay EQP, firmware stop EQP, and a reference method; define acceptable bias and repeatability.
- [ ] Verify pump calibration and result accuracy with actual tubing, titrant viscosity, mixing, electrode response time, and sample matrix.
- [ ] For every production unit, generate and attach its unique factory credential/label, then perform onsite login, recovery, pump-stop, OTA-failure, sensor-fault, and representative-run acceptance checks.
- [ ] Record the validated firmware version, calibration data, and acceptance result for each instrument.

## Next functional milestones

### M7 — Calcium/magnesium hardness workflow

- [ ] Define the total-hardness and calcium-only sequence, including pH 10 and pH 12–13 conditioning guidance.
- [ ] Define how magnesium is calculated from the two results and how blanks, densities, units, and uncertainty are reported.
- [ ] Prototype the workflow without assuming a second pH probe; add hardware only if validation proves it necessary.

### M8 — Repeatable laboratory workflow

- [ ] Method workflow editor for sampling, conditioning/mixing, titration, cleaning, calculation, and reporting steps.
- [ ] Sample series and batch queues with operator and reference metadata.
- [ ] Report templates and multi-run comparison, including replicate statistics and outlier review.
- [ ] Backup/import for browser records with explicit schema versioning.

### M9 — Production hardening

- [ ] Versioned settings/record migration and a documented downgrade policy.
- [ ] Hardware interlock assessment for pump power and lid/reagent safeguards.
- [ ] Manufacturing and service checklist automation without storing secrets in Git or logs.
- [ ] Release artifacts, changelog, and a repeatable production smoke-test package.

## Design constraints

- Keep endpoint signal, titrant, trend, and result formula independent; do not bind the engine to NaOH/HCl.
- Keep high-frequency curve data in the browser, not K10 flash.
- Replay, saved-record, and reporting features must never issue device-control commands.
- Every milestone requires proportionate native/browser tests, a firmware build, and physical validation when chemistry or hardware behavior is involved.
