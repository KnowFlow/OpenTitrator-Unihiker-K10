# Safety and Correctness Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make HTTP OTA pump-safe, enforce endpoint hold confirmation consistently, and make pump deadlines correct across `millis()` rollover.

**Architecture:** Add two small pure timing modules to `control_logic.h`: rollover-safe elapsed/deadline helpers and an `EndpointHoldTracker`. Keep hardware execution in the Arduino sketch, integrate the hold tracker at the existing Run seam, and add an HTTP OTA safety lock that has precedence over all pump-producing paths.

**Tech Stack:** Arduino C++ on UNIHIKER K10 / ESP32-S3, PlatformIO, ESP32 `WebServer` and `Update`, existing native C++ test executable, GitHub Actions CI.

## Global Constraints

- Recovery must not depend on the K10 A/B hardware buttons.
- HTTP OTA success restarts the device; normal startup enters `SetupMode` and never resumes the interrupted run.
- HTTP OTA failure leaves both pumps stopped until Web safe reset returns to `SetupMode`.
- Automatic EQP completion remains unchanged and does not use EP hold confirmation.
- Existing pump PWM, burst timing, adaptive dose sizes, and method settings remain unchanged.
- Do not add authentication, CSRF handling, task-watchdog setup, hardware interlocks, curve persistence, or a full Run module extraction in this phase.

---

## File Structure

- Modify `ph_titrator/control_logic.h`: add pure rollover-safe timing helpers and `EndpointHoldTracker`.
- Modify `tests/ph_titrator_control_test.cpp`: test time rollover and endpoint hold behavior.
- Modify `ph_titrator/ph_titrator.ino`: use safe pump deadlines, integrate endpoint hold, and implement the HTTP OTA safety lock and Web reset.
- Modify `README.md`, `README_CN.md`, `docs/MANUAL.md`, and `docs/MANUAL_CN.md`: document OTA recovery and EP hold semantics.
- Keep `.github/workflows/ci.yml` unchanged: it already builds firmware and runs native tests.

### Task 1: Add Rollover-Safe Time Primitives

**Files:**
- Modify: `ph_titrator/control_logic.h`
- Test: `tests/ph_titrator_control_test.cpp`

**Interfaces:**
- Produces: `inline bool elapsedAtLeast(uint32_t now, uint32_t startedAt, uint32_t durationMs)`.
- Produces: `inline bool deadlineReached(uint32_t now, uint32_t deadline)`.
- Consumes: standard unsigned 32-bit modular arithmetic; deadlines must be less than `INT32_MAX` milliseconds ahead.

- [ ] **Step 1: Write failing rollover tests**

Add these assertions near the start of `main()` in `tests/ph_titrator_control_test.cpp`:

```cpp
  // ---- rollover-safe time helpers ----
  expectTrue(!elapsedAtLeast(1050U, 1000U, 100U), "elapsed duration not reached");
  expectTrue(elapsedAtLeast(1100U, 1000U, 100U), "elapsed duration reached");
  expectTrue(
      elapsedAtLeast(25U, UINT32_MAX - 49U, 75U),
      "elapsed duration survives millis rollover");

  expectTrue(!deadlineReached(1000U, 0U), "zero deadline is inactive");
  expectTrue(!deadlineReached(1099U, 1100U), "deadline not reached");
  expectTrue(deadlineReached(1100U, 1100U), "deadline reached exactly");
  expectTrue(deadlineReached(25U, 20U), "deadline survives millis rollover");
```

- [ ] **Step 2: Run the native test and confirm RED**

Run:

```powershell
New-Item -ItemType Directory -Force build | Out-Null
g++ -std=c++17 -Wall -Wextra -pedantic tests/ph_titrator_control_test.cpp -o build/ph_titrator_control_test.exe
```

Expected: compilation fails because `elapsedAtLeast` and `deadlineReached` are not declared. If `g++` is unavailable locally, confirm that exact limitation and use the CI `unit-tests` job after pushing; do not claim the native tests passed locally.

- [ ] **Step 3: Implement the minimal timing helpers**

Add after `absoluteFloat` in `ph_titrator/control_logic.h`:

```cpp
inline bool elapsedAtLeast(uint32_t now, uint32_t startedAt, uint32_t durationMs) {
  return (uint32_t)(now - startedAt) >= durationMs;
}

inline bool deadlineReached(uint32_t now, uint32_t deadline) {
  return deadline != 0U && (int32_t)(now - deadline) >= 0;
}
```

- [ ] **Step 4: Run the native test and confirm GREEN**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic tests/ph_titrator_control_test.cpp -o build/ph_titrator_control_test.exe
./build/ph_titrator_control_test.exe
```

Expected: `All ph titrator control tests passed`.

- [ ] **Step 5: Route `PumpController` through the deadline helper**

In `ph_titrator/ph_titrator.ino`, replace the direct deadline comparisons in `PumpController::update()` and `PumpController::isRunning()`:

```cpp
  void update() {
    uint32_t now = millis();
    if (deadlineReached(now, runUntilMs)) {
      stop();
      return;
    }
    if (burstMode && isRunning()) {
      updateBurstOutput();
    }
  }

  bool isRunning() const {
    uint32_t now = millis();
    return runUntilMs != 0U && !deadlineReached(now, runUntilMs);
  }
```

- [ ] **Step 6: Build the firmware**

Run:

```powershell
pio run --environment unihiker
```

Expected: firmware build succeeds. If PlatformIO is unavailable locally, record the exact error and require the CI `build-firmware` job before completion.

- [ ] **Step 7: Commit Task 1**

Run:

```powershell
git add ph_titrator/control_logic.h ph_titrator/ph_titrator.ino tests/ph_titrator_control_test.cpp
git commit -m "Make pump deadlines rollover safe"
```

Expected: the commit contains only timing helpers, tests, and the two pump deadline call sites.

### Task 2: Enforce Endpoint Hold Through One Testable Module

**Files:**
- Modify: `ph_titrator/control_logic.h`
- Modify: `ph_titrator/ph_titrator.ino`
- Test: `tests/ph_titrator_control_test.cpp`

**Interfaces:**
- Produces: `EndpointHoldTracker::reset()`.
- Produces: `EndpointHoldTracker::update(bool fresh, bool inRange, uint16_t holdSeconds, uint32_t now)` returning `true` only when the endpoint is confirmed.
- Consumes: `elapsedAtLeast` from Task 1 and the existing `isEndpointReached(settings, value)` function.

- [ ] **Step 1: Write failing endpoint-hold tests**

Add this block to `tests/ph_titrator_control_test.cpp`:

```cpp
  // ---- EndpointHoldTracker ----
  {
    EndpointHoldTracker hold;
    expectTrue(!hold.update(true, true, 5, 1000U), "hold starts without completing");
    expectTrue(!hold.update(false, true, 5, 7000U), "stale reading cannot complete hold");
    expectTrue(!hold.update(true, false, 5, 7000U), "out-of-range reading resets hold");
    expectTrue(!hold.active(), "hold inactive after leaving range");
    expectTrue(!hold.update(true, true, 5, 8000U), "re-entry starts a new hold");
    expectTrue(!hold.update(true, true, 5, 12999U), "new hold needs full duration");
    expectTrue(hold.update(true, true, 5, 13000U), "continuous hold completes");
  }

  {
    EndpointHoldTracker immediate;
    expectTrue(immediate.update(true, true, 0, 42U), "zero-second hold completes immediately");
  }

  {
    EndpointHoldTracker rollover;
    expectTrue(
        !rollover.update(true, true, 1, UINT32_MAX - 499U),
        "rollover hold starts");
    expectTrue(
        rollover.update(true, true, 1, 500U),
        "rollover hold completes");
  }
```

- [ ] **Step 2: Run the native test and confirm RED**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic tests/ph_titrator_control_test.cpp -o build/ph_titrator_control_test.exe
```

Expected: compilation fails because `EndpointHoldTracker` is not declared.

- [ ] **Step 3: Implement `EndpointHoldTracker`**

Add to `ph_titrator/control_logic.h` after the timing helpers:

```cpp
struct EndpointHoldTracker {
  void reset() {
    active_ = false;
    startedAtMs_ = 0U;
  }

  bool active() const {
    return active_;
  }

  bool update(
      bool fresh,
      bool inRange,
      uint16_t holdSeconds,
      uint32_t now) {
    if (!fresh) {
      return false;
    }
    if (!inRange) {
      reset();
      return false;
    }
    if (holdSeconds == 0U) {
      return true;
    }
    if (!active_) {
      active_ = true;
      startedAtMs_ = now;
      return false;
    }
    return elapsedAtLeast(now, startedAtMs_, (uint32_t)holdSeconds * 1000UL);
  }

private:
  bool active_ = false;
  uint32_t startedAtMs_ = 0U;
};
```

- [ ] **Step 4: Run the native test and confirm GREEN**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic tests/ph_titrator_control_test.cpp -o build/ph_titrator_control_test.exe
./build/ph_titrator_control_test.exe
```

Expected: `All ph titrator control tests passed`.

- [ ] **Step 5: Replace the sketch hold timestamp with the tracker**

In `ph_titrator/ph_titrator.ino`, replace:

```cpp
uint32_t endpointHoldStartedMs = 0;
```

with:

```cpp
EndpointHoldTracker endpointHold;
```

Replace every assignment `endpointHoldStartedMs = 0;` in run start/reset/pause/error paths with:

```cpp
endpointHold.reset();
```

Add `endpointHold.reset();` to `pauseTitration()` before entering `Paused` and to the sensor-error path before entering `Error`.

- [ ] **Step 6: Remove the Settling bypass**

Delete this immediate-completion block from the `RunState::Settling` branch:

```cpp
    if (!autoEqpEnabled() && phReady && isEndpointReached(settings, activeControlValue())) {
      stopReason = TitrationStopReason::TargetReached;
      resultValue = computeCurrentResult();
      setState(RunState::Done, reasonLabel(stopReason));
      return;
    }
```

Keep settling/stability checks and EQP handling unchanged. After settling, ordinary EP methods return to `Running`, where the next fresh sample enters the common hold path.

- [ ] **Step 7: Replace the Running hold block**

In `runController()`, replace the `endpointHoldStartedMs` block with:

```cpp
  if (!autoEqpEnabled()) {
    bool inRange = isEndpointReached(settings, activeControlValue());
    bool confirmed = endpointHold.update(
        phSampleFresh,
        inRange,
        settings.holdSeconds,
        millis());
    if (inRange) {
      pump.stop();
      if (confirmed) {
        stopReason = TitrationStopReason::TargetReached;
        resultValue = computeCurrentResult();
        setState(RunState::Done, reasonLabel(stopReason));
      } else {
        statusLine = String("Holding ") + endpointText();
        displayDirty = true;
      }
      return;
    }
  }
```

Because `runController()` already returns when `phSampleFresh` is false, only a fresh reading reaches this block. The explicit `fresh` argument keeps the module interface correct when the full Run module is extracted later.

- [ ] **Step 8: Verify no legacy hold timestamp remains**

Run:

```powershell
rg -n "endpointHoldStartedMs|EndpointHoldTracker|endpointHold\." ph_titrator tests
```

Expected: no `endpointHoldStartedMs` matches; tracker matches appear in `control_logic.h`, tests, and lifecycle/call sites in the sketch.

- [ ] **Step 9: Run tests and firmware build**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic tests/ph_titrator_control_test.cpp -o build/ph_titrator_control_test.exe
./build/ph_titrator_control_test.exe
pio run --environment unihiker
```

Expected: native tests pass and firmware builds.

- [ ] **Step 10: Commit Task 2**

Run:

```powershell
git add ph_titrator/control_logic.h ph_titrator/ph_titrator.ino tests/ph_titrator_control_test.cpp
git commit -m "Enforce endpoint hold confirmation"
```

### Task 3: Add the HTTP OTA Safety Lock and Web Recovery

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`

**Interfaces:**
- Produces: `enterHttpOtaSafety()`, `failHttpOta(const String&)`, and `resetFromHttpOtaFailure()`.
- Produces: flags `httpOtaSafetyLock`, `httpOtaInProgress`, and `httpOtaSucceeded`.
- Consumes: existing `pump`, `samplePump`, `stopManualSweep`, `resetRunData`, `setState`, `Update`, and `scheduleRestart` interfaces.

- [ ] **Step 1: Add OTA state and safety helpers**

Near the existing OTA globals in `ph_titrator/ph_titrator.ino`, add:

```cpp
bool httpOtaSafetyLock = false;
bool httpOtaInProgress = false;
bool httpOtaSucceeded = false;
```

Add these helpers before `handleOta()`:

```cpp
void enterHttpOtaSafety() {
  httpOtaSafetyLock = true;
  httpOtaInProgress = true;
  httpOtaSucceeded = false;
  stopManualSweep(false);
  pump.stop();
  samplePump.stop();
  activePulseMs = 0;
  endpointHold.reset();
  setState(RunState::Error, "OTA upload");
}

void failHttpOta(const String &detail) {
  httpOtaSafetyLock = true;
  httpOtaInProgress = false;
  httpOtaSucceeded = false;
  stopManualSweep(false);
  pump.stop();
  samplePump.stop();
  activePulseMs = 0;
  endpointHold.reset();
  setState(RunState::Error, detail.length() > 0 ? detail : "OTA failed");
}

void resetFromHttpOtaFailure() {
  pump.stop();
  samplePump.stop();
  stopManualSweep(false);
  resetRunData();
  httpOtaInProgress = false;
  httpOtaSafetyLock = false;
  httpOtaSucceeded = false;
  setState(RunState::SetupMode, "OTA reset");
}
```

- [ ] **Step 2: Stop pumps before flash writing**

At `UPLOAD_FILE_START`, call `enterHttpOtaSafety()` before `Update.begin`:

```cpp
  if (upload.status == UPLOAD_FILE_START) {
    enterHttpOtaSafety();
    Serial.printf("OTA: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      failHttpOta("OTA start failed");
    }
```

For write failure, retain the error report and lock the device:

```cpp
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      failHttpOta("OTA write failed");
    }
```

Handle completion and abort explicitly:

```cpp
  } else if (upload.status == UPLOAD_FILE_END) {
    httpOtaInProgress = false;
    if (Update.end(true)) {
      httpOtaSucceeded = true;
      statusLine = "OTA done";
    } else {
      Update.printError(Serial);
      failHttpOta("OTA failed");
    }
    displayDirty = true;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    failHttpOta("OTA aborted");
  }
```

- [ ] **Step 3: Make the final OTA response preserve failure safety**

Replace `handleOta()` with:

```cpp
void handleOta() {
  bool success = httpOtaSucceeded && !httpOtaInProgress;
  server.sendHeader("Connection", "close");
  server.send(success ? 200 : 500, "text/plain", success ? "OK" : "FAIL");
  if (success) {
    httpOtaSafetyLock = true;
    scheduleRestart("OTA update done");
  } else {
    failHttpOta("OTA failed");
  }
}
```

The scheduled restart goes through `setup()` and ends in `SetupMode` when required sensors initialize successfully.

- [ ] **Step 4: Give the OTA lock precedence in controller paths**

At the beginning of `runController()` and `runCalibration()`, after `updatePumpTimeouts()`, add:

```cpp
  if (httpOtaSafetyLock) {
    pump.stop();
    samplePump.stop();
    return;
  }
```

At the beginning of `startTitration()`, `startExistingSampleTitration()`, and `startManualSweep()`, reject while locked. Boolean start functions return `false`; the manual sweep function sets `statusLine = "OTA locked"`, marks the display dirty, and returns.

- [ ] **Step 5: Gate Web commands and enable safe reset**

In `handleAction()`, immediately after reading `cmd`, add:

```cpp
  if (httpOtaSafetyLock) {
    if (cmd == "reset" && !httpOtaInProgress) {
      resetFromHttpOtaFailure();
    } else if (cmd == "panic") {
      pump.stop();
      samplePump.stop();
      statusLine = "OTA locked";
      displayDirty = true;
    } else {
      statusLine = httpOtaInProgress ? "OTA in progress" : "OTA failed: reset required";
      displayDirty = true;
    }
    redirectHomeTab("run");
    return;
  }
```

This makes the existing Web Reset control the safe recovery path without relying on hardware buttons. The original normal `reset` branch remains unchanged for non-OTA states.

- [ ] **Step 6: Expose OTA safety state in `/json`**

Add to `handleJson()`:

```cpp
  json += ",\"ota_safety_lock\":" + String(httpOtaSafetyLock ? "true" : "false");
  json += ",\"ota_in_progress\":" + String(httpOtaInProgress ? "true" : "false");
```

Update the Run panel status copy so that `OTA failed: reset required` clearly tells the user to use the existing Web Reset button. Do not add an A/B-button recovery instruction.

- [ ] **Step 7: Verify all HTTP OTA exits preserve stopped pumps**

Run:

```powershell
rg -n "enterHttpOtaSafety|failHttpOta|resetFromHttpOtaFailure|httpOta(SafetyLock|InProgress|Succeeded)|UPLOAD_FILE_(START|WRITE|END|ABORTED)|Update\.(begin|write|end|abort)" ph_titrator/ph_titrator.ino
```

Expected: upload start calls `enterHttpOtaSafety` before `Update.begin`; every failure path calls `failHttpOta`; reset is only allowed after `httpOtaInProgress` becomes false.

- [ ] **Step 8: Run full automated verification**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic tests/ph_titrator_control_test.cpp -o build/ph_titrator_control_test.exe
./build/ph_titrator_control_test.exe
pio run --environment unihiker
```

Expected: native tests pass and firmware builds.

- [ ] **Step 9: Commit Task 3**

Run:

```powershell
git add ph_titrator/ph_titrator.ino
git commit -m "Lock pumps during HTTP OTA"
```

### Task 4: Update Documentation and Perform Final Verification

**Files:**
- Modify: `README.md`
- Modify: `README_CN.md`
- Modify: `docs/MANUAL.md`
- Modify: `docs/MANUAL_CN.md`

**Interfaces:**
- Consumes: implemented endpoint hold behavior, OTA safety lock, Web Reset recovery, and `SetupMode` restart behavior.
- Produces: accurate operator guidance in English and Chinese.

- [ ] **Step 1: Document endpoint hold semantics**

Update the endpoint-control sections in both manuals to state:

```markdown
Endpoint hold uses fresh sensor readings. Entering the endpoint range stops dosing and starts the Hold timer. If any fresh reading leaves the range, the timer resets and dosing evaluation resumes. The run finishes only after the signal remains in range for the complete Hold period.
```

Chinese equivalent:

```markdown
终点保持只使用新的传感器读数。信号进入终点范围后停止加药并开始 Hold 计时；任一新读数退出范围都会清零计时并恢复滴定判断。只有信号在完整 Hold 时段内持续位于终点范围，实验才会完成。
```

- [ ] **Step 2: Document HTTP OTA recovery**

Update the OTA sections in README and manuals to state:

```markdown
HTTP OTA stops and locks both pumps before flash writing. A successful update restarts into SetupMode and never resumes the interrupted run. After a failed or aborted upload, use the Web Reset control; hardware A/B buttons are not required for recovery.
```

Chinese equivalent:

```markdown
HTTP OTA 在写入固件前会停止并锁定两路泵。更新成功后设备重启进入 SetupMode，不会恢复中断的实验；上传失败或中止后，请使用网页 Reset 复位，无需依赖 A/B 实体按键。
```

- [ ] **Step 3: Run final searches**

Run:

```powershell
rg -n "endpointHoldStartedMs|millis\(\) >= runUntilMs" ph_titrator
rg -n "EndpointHoldTracker|deadlineReached|httpOtaSafetyLock|OTA failed|SetupMode" ph_titrator tests README.md README_CN.md docs/MANUAL.md docs/MANUAL_CN.md
```

Expected: legacy unsafe expressions have no matches; new logic and documentation have the expected matches.

- [ ] **Step 4: Run final automated verification**

Run:

```powershell
g++ -std=c++17 -Wall -Wextra -pedantic tests/ph_titrator_control_test.cpp -o build/ph_titrator_control_test.exe
./build/ph_titrator_control_test.exe
pio run --environment unihiker
git diff --check
git status --short
```

Expected: tests pass, firmware builds, `git diff --check` is clean, and only the four documentation files are uncommitted at this task boundary.

- [ ] **Step 5: Commit documentation**

Run:

```powershell
git add README.md README_CN.md docs/MANUAL.md docs/MANUAL_CN.md
git commit -m "Document OTA and endpoint recovery"
```

- [ ] **Step 6: Inspect the complete phase**

Run:

```powershell
git log --oneline -5
git diff HEAD~4..HEAD --stat
git status --short
```

Expected: four focused implementation commits after the design/plan commits and a clean working tree.

## Self-Review

- Spec coverage: Task 1 covers rollover-safe pump deadlines; Task 2 covers the single EP hold path and all requested reset cases; Task 3 covers HTTP OTA start, success, failure, lock precedence, and Web-only recovery; Task 4 covers operator documentation and final verification.
- Placeholder scan: the plan contains no deferred implementation placeholders. Each code-changing step includes exact code or exact replacement instructions.
- Type consistency: `EndpointHoldTracker`, `elapsedAtLeast`, `deadlineReached`, `httpOtaSafetyLock`, `httpOtaInProgress`, and `httpOtaSucceeded` use the same names and types in producer and consumer tasks.
- Scope: authentication, full Run extraction, watchdogs, hardware interlocks, EQP changes, and curve persistence remain explicitly deferred.
