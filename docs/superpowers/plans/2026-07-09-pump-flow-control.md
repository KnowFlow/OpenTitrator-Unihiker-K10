# Pump Flow Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add saved PWM speed controls for both titrant and sample pumps, with manual per-pump test overrides and documentation.

**Architecture:** Keep dosing decisions in `control_logic.h` unchanged: it still decides pulse duration in milliseconds. Add pump PWM defaults to `ph_titrator.ino`, store them with calibration preferences, and route every pump run through the existing `PumpController` default or explicit override path.

**Tech Stack:** Arduino C++ for UNIHIKER K10 / ESP32-S3, `ESP32Servo`, `WebServer`, `Preferences`, existing native C++ control tests, Arduino CLI firmware build.

## Global Constraints

- Defaults remain `1000us` to preserve current behavior after upgrade.
- Editable PWM range is `1000..1500us`.
- `1500us` is the stop midpoint; values closer to `1500us` run slower.
- Do not expose reverse or opposite-direction PWM values.
- Store `titrant_run_us` and `sample_run_us` in the existing `cal` Preferences namespace.
- Manual overrides do not change saved defaults unless submitted through `/set`.
- Emergency stop, active-state blocking, OTA pump stop, and existing stop paths remain unchanged.

---

## File Structure

- Modify `ph_titrator/ph_titrator.ino`: add persisted pump PWM defaults, update `PumpController`, route automatic/sample/calibration/manual pump runs, extend `/set`, `/status`, and web forms.
- Modify `README.md` and `README_CN.md`: summarize adjustable pump PWM flow control and recalibration guidance.
- Modify `docs/MANUAL.md` and `docs/MANUAL_CN.md`: describe calibration-page PWM controls, manual testing, and recalibration after speed changes.
- Leave `ph_titrator/control_logic.h` unchanged: pulse-duration control is not part of this feature.
- Leave `tests/ph_titrator_control_test.cpp` unchanged unless compile failures reveal a control-logic regression: the feature is in firmware integration, not pure dosing math.

---

### Task 1: Persist Pump PWM Defaults and Route Firmware Pump Runs

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`

**Interfaces:**
- Produces: global `int titrantPumpRunUs` and `int samplePumpRunUs`, constrained by `constrainPumpRunUs(int value)`.
- Produces: `PumpController::setRunPulseUs(int pulseUs)` and `PumpController::runPulseUs() const`.
- Consumes: existing `PumpController::runForMs`, `runForMsAtUs`, and `runContinuous`.

- [ ] **Step 1: Inspect current pump run sites**

Run:

```powershell
rg -n "runForMs\\(|runForMsAtUs\\(|runContinuous\\(|TITRANT_PUMP_RUN_US|SAMPLE_PUMP_RUN_US|saveCalibration|loadCalibration" ph_titrator\ph_titrator.ino
```

Expected: output includes `PumpController`, automatic titrant dosing, sample filling/resume, manual actions, calibration, setup `pump.begin(...)`, and calibration preference save/load.

- [ ] **Step 2: Add PWM range constants and saved globals**

In `ph_titrator/ph_titrator.ino`, near the pump constants, replace the fixed run constants with default/range constants:

```cpp
const int PUMP_STOP_US = 1500;
const int PUMP_MIN_RUN_US = 1000;
const int PUMP_MAX_RUN_US = 1500;
const int TITRANT_PUMP_DEFAULT_RUN_US = 1000;
const int SAMPLE_PUMP_DEFAULT_RUN_US = 1000;
```

Near the existing global pump calibration values, add:

```cpp
int titrantPumpRunUs = TITRANT_PUMP_DEFAULT_RUN_US;
int samplePumpRunUs = SAMPLE_PUMP_DEFAULT_RUN_US;
```

If the existing flow-rate globals are hard to find, run:

```powershell
rg -n "titrantPumpFlowRateGps|samplePumpFlowRateGps" ph_titrator\ph_titrator.ino
```

- [ ] **Step 3: Add a single constrain helper**

Add this helper before the first code that parses or loads pump PWM values:

```cpp
int constrainPumpRunUs(int value) {
  return constrain(value, PUMP_MIN_RUN_US, PUMP_MAX_RUN_US);
}
```

- [ ] **Step 4: Extend `PumpController` defaults**

Update `PumpController` in `ph_titrator/ph_titrator.ino` so it has these public methods:

```cpp
  void setRunPulseUs(int pulseUs) {
    runUs = constrainPumpRunUs(pulseUs);
  }

  int runPulseUs() const {
    return runUs;
  }
```

Update `begin` to call `setRunPulseUs(runPulseUs)` after attach:

```cpp
  void begin(Servo &servoRef, int pin, int runPulseUs) {
    servo = &servoRef;
    servo->setPeriodHertz(50);
    servo->attach(pin, 500, 2500);
    setRunPulseUs(runPulseUs);
    stop();
  }
```

Update `runForMsAtUs` and `writeRun(int pulseUs)` to constrain explicit overrides:

```cpp
  void runForMsAtUs(uint16_t ms, int pulseUs) {
    runUntilMs = millis() + ms;
    writeRun(constrainPumpRunUs(pulseUs));
  }

  void writeRun(int pulseUs) {
    if (servo != nullptr) {
      servo->writeMicroseconds(constrainPumpRunUs(pulseUs));
    }
  }
```

- [ ] **Step 5: Route setup through saved globals**

In `setup()`, change pump initialization from fixed constants to saved globals:

```cpp
pump.begin(titrantPumpServo, TITRANT_PUMP_PIN, titrantPumpRunUs);
samplePump.begin(samplePumpServo, SAMPLE_PUMP_PIN, samplePumpRunUs);
```

Immediately after `loadCalibration();`, set the live controllers again because saved preferences load after `begin`:

```cpp
pump.setRunPulseUs(titrantPumpRunUs);
samplePump.setRunPulseUs(samplePumpRunUs);
```

- [ ] **Step 6: Save and load PWM defaults with calibration**

In `saveCalibration()`, add:

```cpp
prefs.putInt("titrant_us", titrantPumpRunUs);
prefs.putInt("sample_us", samplePumpRunUs);
```

In `loadCalibration()`, add:

```cpp
titrantPumpRunUs = constrainPumpRunUs(prefs.getInt("titrant_us", TITRANT_PUMP_DEFAULT_RUN_US));
samplePumpRunUs = constrainPumpRunUs(prefs.getInt("sample_us", SAMPLE_PUMP_DEFAULT_RUN_US));
```

Keep the preference keys short to match the existing namespace style; the API fields remain `titrant_run_us` and `sample_run_us`.

- [ ] **Step 7: Verify firmware routing by search**

Run:

```powershell
rg -n "TITRANT_PUMP_RUN_US|SAMPLE_PUMP_RUN_US|runForMsAtUs|runContinuous|pump.begin|samplePump.begin|titrantPumpRunUs|samplePumpRunUs" ph_titrator\ph_titrator.ino
```

Expected:
- No `TITRANT_PUMP_RUN_US` or `SAMPLE_PUMP_RUN_US` references remain.
- `pump.begin` uses `titrantPumpRunUs`.
- `samplePump.begin` uses `samplePumpRunUs`.
- Existing `runForMs(...)` and `runContinuous()` calls remain; they now use each controller default.

- [ ] **Step 8: Build-check the firmware**

Run:

```powershell
arduino-cli compile --fqbn UNIHIKER:esp32:k10 ./ph_titrator
```

Expected: compile succeeds. If `arduino-cli` or the board package is unavailable on the machine, record the exact error and continue only after the code has been syntax-checked by the closest available command.

- [ ] **Step 9: Commit Task 1**

Run:

```powershell
git add ph_titrator\ph_titrator.ino
git commit -m "Add saved pump PWM defaults"
```

Expected: commit includes only `ph_titrator/ph_titrator.ino`.

---

### Task 2: Add Web UI, API Fields, and Manual Per-Pump Overrides

**Files:**
- Modify: `ph_titrator/ph_titrator.ino`

**Interfaces:**
- Consumes: `titrantPumpRunUs`, `samplePumpRunUs`, `constrainPumpRunUs`, `PumpController::setRunPulseUs`.
- Produces: `/set` args `titrant_run_us`, `sample_run_us`.
- Produces: `/status` fields `titrant_run_us`, `sample_run_us`.
- Produces: `/action` manual args `titrant_us`, `sample_us`, with legacy `us` fallback.

- [ ] **Step 1: Update Calibration tab pump form**

In the Pump Flow card of `htmlPage()`, after the existing `Sample pump g/s` input, add two PWM fields:

```cpp
  page += F("'></label><label>Titrant pump PWM us<input name='titrant_run_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(titrantPumpRunUs);
  page += F("'></label><label>Sample pump PWM us<input name='sample_run_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(samplePumpRunUs);
```

Update the Pump Flow helper text to:

```cpp
  page += F("'></label></div><p class='tiny'>Measures each pump independently by mass. PWM: 1000 fast, 1500 stop/slow limit. Re-run calibration after changing tubing, pump head, liquid, viscosity, or PWM speed.</p></div>");
```

- [ ] **Step 2: Update Manual tab form**

Replace the current Manual Operation form string with a form that includes per-pump PWM inputs:

```cpp
  page += F("<section id='tab-manual' class='panel'><div class='card full'><h2>Manual Operation</h2><form id='manualForm' action='/action' method='get' class='row'><label class='mini'>Run seconds<input name='sec' type='number' min='0.1' max='30' step='0.1' value='1.0'></label><label class='mini'>Titrant PWM us<input name='titrant_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(titrantPumpRunUs);
  page += F("'></label><label class='mini'>Sample PWM us<input name='sample_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(samplePumpRunUs);
  page += F("'></label><button class='btn' name='cmd' value='manual_titrant' type='submit'>Run titrant pump</button><button class='btn' name='cmd' value='manual_sample' type='submit'>Run sample pump</button><button class='btn danger' name='cmd' value='manual_stop' type='submit'>Stop pumps</button></form><p class='tiny'>Manual pump actions are blocked while titration or calibration is active. PWM: 1000 fast, 1500 stop/slow limit. Use seconds and PWM here for priming tubing and speed tests.</p></div></section>");
```

- [ ] **Step 3: Parse and save `/set` PWM fields**

In `handleSet()`, in the calibration settings area near `titrant_gps` and `sample_gps`, add:

```cpp
  if (server.hasArg("titrant_run_us")) {
    titrantPumpRunUs = constrainPumpRunUs(server.arg("titrant_run_us").toInt());
    pump.setRunPulseUs(titrantPumpRunUs);
    calibrationChanged = true;
  }
  if (server.hasArg("sample_run_us")) {
    samplePumpRunUs = constrainPumpRunUs(server.arg("sample_run_us").toInt());
    samplePump.setRunPulseUs(samplePumpRunUs);
    calibrationChanged = true;
  }
```

Expected behavior: pressing Save calibration persists PWM defaults through the existing `calibrationChanged` and `saveCalibration()` flow.

- [ ] **Step 4: Add `/status` JSON fields**

In `statusJson()`, after `sample_gps`, add:

```cpp
  json += ",\"titrant_run_us\":" + String(titrantPumpRunUs);
  json += ",\"sample_run_us\":" + String(samplePumpRunUs);
```

- [ ] **Step 5: Add manual override parsing**

In `handleAction()`, replace the single `manualUs` variable with:

```cpp
  int manualTitrantUs = titrantPumpRunUs;
  int manualSampleUs = samplePumpRunUs;
  if (server.hasArg("titrant_us")) {
    manualTitrantUs = server.arg("titrant_us").toInt();
  } else if (server.hasArg("us")) {
    manualTitrantUs = server.arg("us").toInt();
  }
  if (server.hasArg("sample_us")) {
    manualSampleUs = server.arg("sample_us").toInt();
  } else if (server.hasArg("us")) {
    manualSampleUs = server.arg("us").toInt();
  }
  manualTitrantUs = constrainPumpRunUs(manualTitrantUs);
  manualSampleUs = constrainPumpRunUs(manualSampleUs);
```

Update manual titrant run:

```cpp
pump.runForMsAtUs(manualMs, manualTitrantUs);
statusLine = String("Manual titrant ") + String(manualSeconds, 1) + "s @ " + String(manualTitrantUs) + "us";
```

Update manual sample run:

```cpp
samplePump.runForMsAtUs(manualMs, manualSampleUs);
statusLine = String("Manual sample ") + String(manualSeconds, 1) + "s @ " + String(manualSampleUs) + "us";
```

- [ ] **Step 6: Search-check API wiring**

Run:

```powershell
rg -n "titrant_run_us|sample_run_us|titrant_us|sample_us|manualTitrantUs|manualSampleUs|titrantPumpRunUs|samplePumpRunUs" ph_titrator\ph_titrator.ino
```

Expected: matches appear in the Calibration tab, Manual tab, `/set`, `/status`, `handleAction`, preferences load/save, and setup.

- [ ] **Step 7: Build-check the firmware**

Run:

```powershell
arduino-cli compile --fqbn UNIHIKER:esp32:k10 ./ph_titrator
```

Expected: compile succeeds.

- [ ] **Step 8: Commit Task 2**

Run:

```powershell
git add ph_titrator\ph_titrator.ino
git commit -m "Expose pump PWM controls in web UI"
```

Expected: commit includes only `ph_titrator/ph_titrator.ino`.

---

### Task 3: Update User Documentation and Final Verification

**Files:**
- Modify: `README.md`
- Modify: `README_CN.md`
- Modify: `docs/MANUAL.md`
- Modify: `docs/MANUAL_CN.md`

**Interfaces:**
- Consumes: implemented fields `titrant_run_us`, `sample_run_us`, `titrant_us`, `sample_us`.
- Produces: user-facing guidance for slower pump operation and recalibration.

- [ ] **Step 1: Update README feature summary**

In `README.md`, update the pump calibration paragraph to include:

```markdown
Pump speed is configurable per pump as a servo PWM pulse width. The default `1000us` preserves the original speed; values closer to `1500us` run slower. Recalibrate pump g/s after changing PWM speed so dosing estimates match the actual flow.
```

In `README_CN.md`, add the Chinese equivalent near the pump calibration section:

```markdown
每个泵的速度可以用舵机 PWM 脉宽单独设置。默认 `1000us` 保持原有速度；越接近 `1500us` 越慢。修改 PWM 速度后应重新校准泵流量，确保 g/s 估算匹配实际出液。
```

- [ ] **Step 2: Update manual Calibration tab docs**

In `docs/MANUAL.md`, under the Calibration tab description, add:

```markdown
- **Pump PWM us**: saved speed for each pump. `1000us` is the original fast setting; values closer to `1500us` slow the pump toward the stop midpoint. After changing this value, run pump calibration again.
```

In `docs/MANUAL_CN.md`, add:

```markdown
- **泵 PWM us**：每个泵保存的运行速度。`1000us` 为原来的较快设置；越接近 `1500us` 越接近停泵中位，速度越慢。修改后请重新执行泵流量校准。
```

- [ ] **Step 3: Update manual operation docs**

In `docs/MANUAL.md`, under Manual/Admin controls, add:

```markdown
- Manual pump runs can use temporary P0/P1 PWM values for speed testing. These temporary values do not change the saved defaults; save PWM values in Calibration when the tested speed is suitable.
```

In `docs/MANUAL_CN.md`, add:

```markdown
- 手动运行泵时可以临时指定 P0/P1 PWM，用于单独测试速度。这些临时值不会改变已保存默认值；确认合适后在 Calibration 页保存。
```

- [ ] **Step 4: Final search verification**

Run:

```powershell
rg -n "PWM|titrant_run_us|sample_run_us|1000us|1500us|泵 PWM|重新校准|recalibrat" README.md README_CN.md docs\MANUAL.md docs\MANUAL_CN.md ph_titrator\ph_titrator.ino
```

Expected: code fields appear in `ph_titrator.ino`; user-facing docs mention `1000us`, `1500us`, slower speed, and recalibration after changing PWM.

- [ ] **Step 5: Run native control tests**

Run:

```powershell
g++ -std=c++17 -I ph_titrator tests\ph_titrator_control_test.cpp -o build\ph_titrator_control_test.exe
.\build\ph_titrator_control_test.exe
```

Expected: executable prints `All ph titrator control tests passed`.

- [ ] **Step 6: Run firmware build**

Run:

```powershell
arduino-cli compile --fqbn UNIHIKER:esp32:k10 ./ph_titrator
```

Expected: compile succeeds.

- [ ] **Step 7: Inspect final diff**

Run:

```powershell
git diff --stat
git diff -- ph_titrator\ph_titrator.ino README.md README_CN.md docs\MANUAL.md docs\MANUAL_CN.md
```

Expected: diff only contains pump PWM control implementation and related docs.

- [ ] **Step 8: Commit Task 3**

Run:

```powershell
git add README.md README_CN.md docs\MANUAL.md docs\MANUAL_CN.md
git commit -m "Document pump PWM flow control"
```

Expected: commit includes only documentation files.

---

## Self-Review

- Spec coverage: Task 1 covers persisted defaults, firmware routing, calibration behavior, and safe range. Task 2 covers web UI, `/set`, `/status`, and manual overrides with legacy `us`. Task 3 covers README/manual documentation and final verification.
- Placeholder scan: no deferred implementation language is used; each code change has concrete snippets and exact commands.
- Type consistency: the plan consistently uses `int` for PWM microsecond values, `titrantPumpRunUs`, `samplePumpRunUs`, API fields `titrant_run_us`/`sample_run_us`, and manual fields `titrant_us`/`sample_us`.
