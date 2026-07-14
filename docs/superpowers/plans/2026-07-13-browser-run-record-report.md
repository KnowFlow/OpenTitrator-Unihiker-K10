# Browser Run Record and Printable Report Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans task-by-task.

**Goal:** Add a browser-only single-sample record and printable report around the existing curve.

**Architecture:** Inline page JavaScript owns one `runRecord` envelope derived only from the existing `/json` poll. It never issues a device command and is discarded on page reload unless exported/printed.

**Tech Stack:** Arduino-rendered HTML/JavaScript, PowerShell static tests, MSVC native tests.

## Global Constraints

- Never call `/action`, `/set`, `/ota`, or a pump command from record code.
- No Flash, server, cloud, or account persistence.
- Done with Target/Equivalence is `completed`; Error, Emergency stop, and reset after start are `aborted` and unconfirmed.
- Preserve existing curve exports, authentication, OTA, RunEngine behavior, ignored credentials, and `docs/CODE_REVIEW.md`.

### Task 1: Record lifecycle and metadata card

**Files:** Modify `ph_titrator/ph_titrator.ino`; modify `tests/sketch_safety_static_test.ps1`.

**Interfaces:** Produce browser globals `runRecord`, `newRunRecord()`, `observeRunRecord(d)`, and `renderRunRecord()`.

- [ ] **Step 1: Write failing static contract checks**

```powershell
Assert-Match 'id=''recordSampleId''' 'record sample metadata field exists'
Assert-Match 'function observeRunRecord\(d\)' 'record observer exists'
if ([regex]::Match($sketch, 'function observeRunRecord\(d\)[\s\S]*?\n\}').Value -match '/action|/set|/ota') { throw 'FAIL: record observer must not control device' }
```

- [ ] **Step 2: Run RED**

Run `powershell -ExecutionPolicy Bypass -File tests/sketch_safety_static_test.ps1`; expect the metadata-field assertion to fail.

- [ ] **Step 3: Implement minimal browser-only record state**

Add a compact Run Record card beside curve controls with sample ID, batch/reference, operator, notes, status, point count, and New record button. Add:

```javascript
var runRecord=null;
function newRunRecord(){runRecord={schemaVersion:1,recordStatus:'draft',metadata:readRecordMetadata(),startedAt:null,finishedAt:null,methodSnapshot:null,deviceSnapshot:null,final:null,points:[]};renderRunRecord()}
function observeRunRecord(d){/* derive active/completed/aborted from JSON facts only */}
```

Call `observeRunRecord(d)` after the existing curve recorder. Freeze method/device/final snapshots with `JSON.parse(JSON.stringify(value))`; append copied curve points only after a record starts. No record function may call `apiPost`.

- [ ] **Step 4: Run GREEN and commit**

Run the sketch and Web-auth static tests; commit only sketch and static-test changes with `git commit -m "Add browser run record state"`.

### Task 2: Record JSON export and printable terminal report

**Files:** Modify `ph_titrator/ph_titrator.ino`; modify `tests/sketch_safety_static_test.ps1`.

**Interfaces:** Consume `runRecord`; produce `exportRunRecord()` and `printRunReport()`.

- [ ] **Step 1: Write failing report contracts**

```powershell
Assert-Match 'function exportRunRecord\(\)' 'record JSON exporter exists'
Assert-Match 'function printRunReport\(\)' 'print report function exists'
Assert-Match 'ABORTED / NOT CONFIRMED' 'aborted reports are visibly marked'
Assert-Match 'confirmed' 'report stores confirmation state'
```

- [ ] **Step 2: Run RED**

Run the sketch static test; expect its first missing report assertion to fail.

- [ ] **Step 3: Implement exports**

Add Record JSON and Print report buttons. `exportRunRecord()` downloads `{record:runRecord}` using Blob as `titration-run-record.json`. `printRunReport()` opens an escaped-text HTML report containing metadata, timestamps, method/device snapshots, final result/used mass/EQP, point count and elapsed time. For aborted records show exactly `ABORTED / NOT CONFIRMED`. Do not copy canvas, add libraries, or send network requests.

- [ ] **Step 4: Verify and commit**

Run sketch/Web-auth static tests, MSVC `run_engine_test`, `git diff --check`, and confirm only expected files change. Commit with `git commit -m "Add printable browser run report"`.

## Self-review

- Task 1 covers browser-only metadata, lifecycle, snapshots, and non-control boundary.
- Task 2 covers JSON/print exports and completed/aborted result labeling.
- No task adds device persistence, automatic control, multi-sample queues, LIMS, or onsite tuning.
