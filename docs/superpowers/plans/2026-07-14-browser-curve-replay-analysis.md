# Browser Curve Replay Analysis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add browser-local replay analysis for current or imported titration records without creating device control paths.

**Architecture:** Embed a pure JavaScript replay analyzer in the existing browser page. It receives copied point arrays, normalizes them, returns a data-only result, and updates a Run-tab status element. Local JSON import uses `FileReader` and remains in RAM.

**Tech Stack:** Arduino C++ HTML/JavaScript string page, PowerShell static tests, PlatformIO.

## Global Constraints

- No replay path may call `apiPost`, `/action`, `/set`, `/ota`, or any device endpoint.
- Imported records remain in browser memory only.
- Replay output never changes `eqpManual`, controller parameters, or pump state.
- A record needs one endpoint and three dose-change points before it can produce a candidate.

---

### Task 1: Specify the replay UI and safety boundary

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`
- Modify: `tests/web_auth_static_test.ps1`

**Interfaces:**
- Produces `replayRecordInput`, `replayAnalysisButton`, and `replayInfo` DOM elements.

- [ ] **Step 1: Add failing static checks**

```powershell
Need "id='replayRecordInput' type='file'" 'replay needs local JSON import'
Need "id='replayAnalysisButton' type='button'" 'replay needs explicit invocation'
Reject "function replayAnalysis[\s\S]*?apiPost\(" 'replay must never control the device'
```

- [ ] **Step 2: Run RED**

Run: `powershell -ExecutionPolicy Bypass -File tests/web_auth_static_test.ps1`

Expected: failure because the replay controls are absent.

- [ ] **Step 3: Add controls in the Run tab**

```html
<input id='replayRecordInput' type='file' accept='application/json'>
<button id='replayAnalysisButton' type='button'>Replay analysis</button>
<p id='replayInfo' class='tiny'>Replay uses the live curve until a record is imported.</p>
```

- [ ] **Step 4: Run GREEN**

Run: `powershell -ExecutionPolicy Bypass -File tests/web_auth_static_test.ps1`

Expected: `All web authentication static tests passed`.

### Task 2: Implement and test deterministic replay analysis

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`
- Modify: `tests/web_auth_static_test.ps1`

**Interfaces:**
- Produces `analyzeReplay(points)` returning `{quality, reason, candidate, slopes}`.

- [ ] **Step 1: Add failing contracts**

```powershell
Need "function analyzeReplay(points)" 'replay analyzer must be a pure point-array function'
Need "Math.abs((next.signal-prev.signal)/(next.used_g-prev.used_g))" 'replay must use centered local slopes'
Need "quality:'insufficient'" 'replay must represent insufficient data explicitly'
```

- [ ] **Step 2: Run RED**

Run: `powershell -ExecutionPolicy Bypass -File tests/web_auth_static_test.ps1`

Expected: failure because `analyzeReplay` is absent.

- [ ] **Step 3: Implement the analyzer**

Normalize a copied array by endpoint and mass, collapse points within 0.01 g,
then compute centered slopes. Return no candidate for an invalid/mixed/short
record. Do not mutate `curve` or `eqpManual`.

- [ ] **Step 4: Run GREEN**

Run: `powershell -ExecutionPolicy Bypass -File tests/web_auth_static_test.ps1`

Expected: `All web authentication static tests passed`.

### Task 3: Wire local import, verify, and document

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`
- Modify: `docs/MANUAL_CN.md`

- [ ] **Step 1: Add local FileReader import**

Accept only `record.points` or `points` arrays. Display a local parse error
without network access. Clear imported points on reload by leaving them in RAM.

- [ ] **Step 2: Document operator use**

Document that replay is a review aid, imports JSON locally, and never applies
or controls settings.

- [ ] **Step 3: Verify**

Run: `powershell -ExecutionPolicy Bypass -File tests/web_auth_static_test.ps1`

Run: `powershell -ExecutionPolicy Bypass -File tests/sketch_safety_static_test.ps1`

Run: `pio run -e unihiker`

Expected: static suites pass and PlatformIO produces `firmware.bin`.
