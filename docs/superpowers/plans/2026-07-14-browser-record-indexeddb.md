# Browser Record IndexedDB Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist finalized browser-only run records locally and make them safely reviewable after a reload.

**Architecture:** An IndexedDB adapter owns database access and emits only local callbacks. Existing finalization calls one save function; selector summaries are loaded at startup and individual records only on explicit load.

**Tech Stack:** Arduino HTML/JavaScript string page, IndexedDB, Node fixture, PowerShell static tests, PlatformIO.

## Global Constraints

- Save only completed or aborted records after `runRecord.final` exists.
- Retain at most 50 locally saved records.
- Never store or expose credentials, session tokens, or Wi-Fi passwords.
- No IndexedDB function may issue HTTP or device-control calls.

---

### Task 1: Add failing local-storage UI and boundary contracts

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`
- Modify: `tests/web_auth_static_test.ps1`

- [ ] Add failing checks for `savedRecordSelect`, load/delete buttons,
  `indexedDB.open`, `saveFinalRecord`, and the absence of `apiPost` in the
  storage adapter region.
- [ ] Run `powershell -ExecutionPolicy Bypass -File tests/web_auth_static_test.ps1` and confirm RED.
- [ ] Add the history UI and a local IndexedDB adapter.
- [ ] Rerun the static test and confirm GREEN.

### Task 2: Persist finalized records and enforce retention

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`
- Create: `tests/record_indexeddb_test.js`

- [ ] Create a failing Node fixture for id assignment, final-only persistence,
  descending summary order, and 50-record retention.
- [ ] Implement `saveFinalRecord`, summary refresh, load, and explicit delete.
- [ ] Run `node tests/record_indexeddb_test.js` and confirm GREEN.

### Task 3: Document and verify

**Files:**
- Modify: `docs/MANUAL_CN.md`

- [ ] Document browser-profile-local retention, 50-record cap, and no active
  run persistence.
- [ ] Run replay Node tests, IndexedDB Node tests, web/safety static tests,
  Python authentication tests, and `pio run -e unihiker`.
- [ ] Commit the implementation after `git diff --check` passes.
