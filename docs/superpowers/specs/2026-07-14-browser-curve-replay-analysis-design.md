# Browser Curve Replay Analysis Design

## Goal

Make titration-curve slope and equivalence-point (EQP) review reproducible from
the current browser curve or an exported browser run-record JSON file, without
changing firmware control, device storage, authentication, or any pump command.

## Scope

The browser accepts either the existing in-memory `curve` points or a local
`{record: {points: [...]}}` JSON export. It normalizes a copy of the points,
derives a replay result, and displays that result in the Run tab. Import uses
the browser File API only; it makes no HTTP request.

Out of scope: automatic parameter application, firmware EQP-control changes,
flash/IndexedDB persistence, CSV import, and multi-run comparison.

## Analysis model

1. Reject records with fewer than three usable dose-change points or mixed
   endpoints.
2. Sort a copy by `used_g`, then collapse adjacent points whose used mass differs
   by less than 0.01 g, retaining the newest point.
3. Create centered, local slopes from each three-point neighborhood:
   `abs((signal[i + 1] - signal[i - 1]) / (used_g[i + 1] - used_g[i - 1]))`.
   Points without a positive mass span are skipped.
4. The largest valid centered slope is the automatic replay EQP candidate. The
   candidate uses the middle point's mass, elapsed time, pH/mV, and endpoint.
5. Quality is `high` for at least five valid local slopes, `review` for two to
   four, and `insufficient` otherwise. A human-readable reason accompanies all
   non-high results.

This intentionally measures curve shape independently of the firmware's live
EDTA stop tracker. It is an offline review aid, not an authority for a running
experiment.

## Browser behavior

The Run tab gains an `Import record` file selector, `Replay analysis` button,
and a result text area. `Replay analysis` first uses the imported record when
present; otherwise it uses the live curve. It never overwrites the manual EQP
selection, changes device settings, or adds API calls. The imported data stays
only in browser memory and is cleared on page reload.

## Validation

Static tests assert a local File API path, no control/API write seam in replay
code, and the centered-slope/quality contracts. A deterministic browser-side
test fixture covers duplicate mass collapse, the selected EQP, and insufficient
data. Existing safety/authentication static tests and a normal firmware build
remain required.
