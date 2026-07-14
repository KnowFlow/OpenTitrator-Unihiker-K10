# Browser Record IndexedDB Design

## Goal

Preserve completed or aborted browser run records across page reloads without
writing experiment telemetry to the K10, device flash, a server, or browser
storage during an active run.

## Storage model

The browser opens an IndexedDB database named `k10-titration-records`, version
1, with a `runs` object store keyed by the record's generated `id`. Each stored
value is a deep copy of the existing `runRecord` structure plus its id and
`savedAt` timestamp. The record contains no administrator password, factory
password, session token, Wi-Fi password, or any new device data.

Only record finalization triggers a write. The browser retains the 50 newest
saved records by `savedAt`; after a successful write it deletes older entries.
Database failure is presented as local UI text and never changes the experiment
state or prevents the current record from being exported or printed.

## UI and data flow

The Run tab gains a saved-record selector plus Load and Delete buttons. Startup
loads only a small list of summaries (id, sample id, completion timestamp,
status), not every point array. Selecting Load reads one record into
`runRecord`, redraws its curve from the stored points, and leaves device state
unchanged. Delete removes only the selected local record after an explicit
button action.

## Constraints

- IndexedDB interactions never call `/json`, `apiPost`, `/action`, `/set`, or
  `/ota`.
- Loading a record never calls `observeRunRecord` and cannot resume or modify a
  run.
- Active/draft record telemetry remains memory-only.
- Historical data is local to the browser profile and may be cleared by the
  browser user.

## Validation

Static tests require a local IndexedDB-only storage seam, finalization-only
save trigger, and no control/API call in its function region. A Node fixture
tests final record storage ordering and 50-record retention through a fake
in-memory store adapter. Existing safety/authentication tests and PlatformIO
build remain required.
