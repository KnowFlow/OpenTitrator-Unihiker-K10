# Browser Run Record and Printable Report Design

## Goal

Add a browser-only, single-sample experiment record and printable report to the
existing K10 pH titrator Web UI. It must not alter RunEngine decisions, pump
control, device Preferences, authentication policy, or OTA behavior.

## Scope

The page already maintains the current run curve in browser memory and exports
CSV/JSON. This feature adds a record envelope around that existing data.

### Record lifecycle

- **Draft**: user enters optional sample metadata: sample ID, batch/reference,
  operator, and notes.
- **Running**: the first observed active run state creates a method/device
  snapshot and records curve points as today.
- **Completed**: a normal terminal result captures final result, selected
  titrant mass, endpoint/EQP facts, and final device status.
- **Aborted**: Error, emergency stop, or reset after a started run captures the
  terminal status and explicitly marks the record as not result-confirmed.

The browser never starts, stops, pauses, resets, or otherwise controls a run
as part of record keeping.

## Data model

```text
RunRecord
  schemaVersion: 1
  recordStatus: draft | running | completed | aborted
  metadata: sampleId, batchReference, operator, notes
  startedAt / finishedAt
  methodSnapshot: method, endpoint, target, trend, titrant, formula,
                  densities, blank, control/settle/hold limits
  deviceSnapshot: device ID when supplied by JSON, firmware-independent
                  network/status facts, calibration flow values
  final: state, status, stop reason, result value/unit, used mass,
         endpoint/EQP values, confirmed boolean
  points: existing browser curve point array
```

All record data stays in JavaScript memory until the operator exports it or
reloads/leaves the page. There is no device Flash persistence, server-side
storage, account synchronization, or data retention promise.

## UI

Add a compact Run Record card next to the existing curve controls:

- editable metadata fields and a `New record` action;
- status badge and point count;
- `Print report` action opening the browser print dialog;
- `Record JSON` action exporting the complete envelope;
- existing curve CSV/JSON buttons remain unchanged.

The printable view contains title, record state, metadata, method/device
snapshot, final result or prominent aborted warning, EQP summary, and a compact
point-count/elapsed-time summary. It does not need to render the canvas chart
in the first version; raw data remains available from exports.

## Integration

The existing `/json` polling handler feeds the record state machine after it
updates the curve. It derives state only from returned facts such as `state`,
`status`, result fields, EQP fields, and pump facts. No new device endpoint is
required.

`completed` is entered only for normal terminal results (for example target or
equivalence completion). Any Error, emergency-stop status, or reset following
an active record becomes `aborted` and cannot be presented as confirmed.

## Safety and error handling

- Record/export code is strictly display-side and never issues `/action`,
  `/set`, `/ota`, or any pump command.
- Missing or malformed telemetry creates an incomplete snapshot/point only;
  it cannot change pump or run state.
- Export/print clearly identifies an aborted or incomplete record.
- Existing authentication and OTA restrictions are unaffected.

## Testing

- Extend sketch static tests to require the record UI and prohibit record code
  from calling device-control endpoints.
- Add a browser-independent JavaScript contract check where practical (source
  assertions for status/finalization/aborted labeling and export fields).
- Run existing native RunEngine, control, authentication, PowerShell/static,
  and Python tests; firmware compilation remains an optional local gate that
  may require the ignored factory-auth header.

## Out of scope

- Multi-sample queues, automated rinse/changeover, report signing, cloud/LIMS
  integration, PDF rendering libraries, device-side record storage, and any
  onsite EQP tuning.
