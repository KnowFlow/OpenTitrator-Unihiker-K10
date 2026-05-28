#!/usr/bin/env python3
"""Apply all patches to ph_titrator.ino, control_logic.h and tests."""
import os


def patch_file(path, replacements):
    """Apply a list of (old, new) replacements to a file."""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()

    for i, (old, new) in enumerate(replacements):
        if old not in text:
            print(f"  WARNING [{path}] replacement #{i} not found, skipping")
            continue
        text = text.replace(old, new, 1)

    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"  Patched {path}")


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    # ------------------------------------------------------------------
    # 1. Fix field-name inconsistency in control_logic.h
    # ------------------------------------------------------------------
    h_path = os.path.join(root, "ph_titrator", "control_logic.h")
    patch_file(h_path, [
        # decideAdaptiveDose uses d.pulseMs but struct declares pumpPulseMs
        ("d.pulseMs = 30;", "d.pumpPulseMs = 30;"),
        ("d.pulseMs = 600;", "d.pumpPulseMs = 600;"),
        ("d.pulseMs = 200;", "d.pumpPulseMs = 200;"),
        ("d.pulseMs = 80;", "d.pumpPulseMs = 80;"),
    ])

    # ------------------------------------------------------------------
    # 2. ph_titrator.ino – multiple surgical replacements
    # ------------------------------------------------------------------
    ino_path = os.path.join(root, "ph_titrator", "ph_titrator.ino")

    patch_file(ino_path, [
        # 2a. stateLabel
        (
            '    case RunState::Paused:\n      return "Paused";\n    case RunState::Error:\n      return "Error";\n  }\n  return "Unknown";\n}',
            '    case RunState::Paused:\n      return "Paused";\n    case RunState::Calibrating:\n      return "Calibrating";\n    case RunState::Error:\n      return "Error";\n  }\n  return "Unknown";\n}'
        ),
        # 2b. stateColor
        (
            '    case RunState::Error:\n      return COLOR_ERROR;\n  }\n  return COLOR_TEXT;\n}',
            '    case RunState::Calibrating:\n      return COLOR_WARN;\n    case RunState::Error:\n      return COLOR_ERROR;\n  }\n  return COLOR_TEXT;\n}'
        ),
        # 2c. primaryHint
        (
            '    case RunState::Paused:\n      return "B=Resume";\n    case RunState::Error:\n      return "A/B=Ack";\n    default:\n      return "";\n  }\n}',
            '    case RunState::Paused:\n      return "B=Resume";\n    case RunState::Calibrating:\n      return "A/B=Cancel";\n    case RunState::Error:\n      return "A/B=Ack";\n    default:\n      return "";\n  }\n}'
        ),
        # 2d. secondaryHint
        (
            '    case RunState::Error:\n      return "";\n    default:\n      return "B=Pause";\n  }\n}',
            '    case RunState::Calibrating:\n      return "";\n    case RunState::Error:\n      return "";\n    default:\n      return "B=Pause";\n  }\n}'
        ),
        # 2e. handleButton – SetupReady B-key
        (
            '    case RunState::SetupReady: {\n      if (keys.aPressed) {\n        resetRunData();\n        startTitration();\n      } else if (keys.bPressed) {\n        tareScale();\n      }\n      break;\n    }',
            '    case RunState::SetupReady: {\n      if (keys.aPressed) {\n        resetRunData();\n        startTitration();\n      } else if (keys.bPressed) {\n        setState(RunState::Calibrating, "Calibrating pumps");\n      }\n      break;\n    }\n    case RunState::Calibrating: {\n      if (keys.aPressed || keys.bPressed) {\n        pump.stop();\n        samplePump.stop();\n        setState(RunState::SetupReady, "Calib cancelled");\n      }\n      break;\n    }'
        ),
        # 2f. sampleSensors – feed dynamics
        (
            '    if (phReady) {\n      phFilter.add(lastPh.ph, millis());\n    }',
            '    if (phReady) {\n      phFilter.add(lastPh.ph, millis());\n      phDynamics.add(lastPh.ph, millis());\n    }'
        ),
        # 2g. runController – FULL REWRITE
        (
            'void runController() {\n  // --- sample dosing ---\n  static uint32_t sampleDoseStartMs = 0;\n  if (state == RunState::SetupReady) {\n    if (sampleDeliveredGrams < settings.sampleGrams - SAMPLE_DEADBAND_G) {\n      if (!samplePump.isRunning() && millis() - sampleDoseStartMs >= SAMPLE_DOSE_SETTLE_MS) {\n        samplePump.runForMs(SAMPLE_DOSE_PULSE_MS);\n        sampleDoseStartMs = millis();\n      }\n    } else {\n      samplePump.stop();\n    }\n  } else if (state == RunState::Titrating) {\n    samplePump.stop();\n  }\n  samplePump.update();\n\n  // --- titration control ---\n  if (state == RunState::Titrating) {\n    if (!phReady) {\n      stopTitration("pH fail");\n      return;\n    }\n    if (!scaleReady) {\n      stopTitration("Scale fail");\n      return;\n    }\n\n    TitrationControlResult result = computePumpControl(\n      settings, phFilter.lastPh(), lastScale.grams,\n      initialBottleWeight, consumedGrams, pump.commandPwm(), millis());\n\n    consumedGrams = result.usedGrams;\n\n    if (result.action == TitrationAction::Done) {\n      if (result.stopReason == TitrationStopReason::None) {\n        stopTitration("Done");\n      } else {\n        stopTitration(stopReasonLabel(result.stopReason));\n      }\n      return;\n    }\n\n    if (result.pumpCommand != pump.commandPwm()) {\n      pump.commandPwm(result.pumpCommand);\n    }\n  } else if (state == RunState::SetupReady || state == RunState::Paused || state == RunState::Error) {\n    if (pump.isCommanded()) {\n      pump.stop();\n    }\n  }\n\n  pump.update();\n\n  // --- result calculation ---\n  if (state == RunState::Finished || state == RunState::Paused || state == RunState::Error) {\n    computeResult(settings, consumedGrams, resultConcentrationM);\n  }\n}',
            'void runController() {\n  // --- sample dosing ---\n  static uint32_t sampleDoseStartMs = 0;\n  if (state == RunState::SetupReady || state == RunState::Calibrating) {\n    if (sampleDeliveredGrams < settings.sampleGrams - SAMPLE_DEADBAND_G) {\n      if (!samplePump.isRunning() && millis() - sampleDoseStartMs >= SAMPLE_DOSE_SETTLE_MS) {\n        samplePump.runForMs(SAMPLE_DOSE_PULSE_MS);\n        sampleDoseStartMs = millis();\n      }\n    } else {\n      samplePump.stop();\n    }\n  } else if (state == RunState::Titrating) {\n    samplePump.stop();\n  }\n  samplePump.update();\n\n  // --- titration control ---\n  if (state == RunState::Titrating) {\n    if (!phReady) {\n      stopTitration("pH fail");\n      return;\n    }\n    if (!scaleReady) {\n      stopTitration("Scale fail");\n      return;\n    }\n\n    TitrationDecision decision = decideAdaptiveDose(\n        settings, phFilter.lastPh(), consumedGrams, phDynamics);\n\n    consumedGrams = (scaleReady ? lastScale.grams : consumedGrams) - initialBottleWeight;\n    consumedGrams = max(0.0f, consumedGrams);\n\n    if (decision.action == TitrationAction::Done) {\n      stopTitration(stopReasonLabel(decision.reason));\n      return;\n    }\n\n    if (decision.pumpPulseMs > 0 && !pump.isRunning()) {\n      pump.runForMs(decision.pumpPulseMs);\n      statusLine = String("Pulse ") + String(decision.pumpPulseMs) + "ms";\n      displayDirty = true;\n    }\n  } else if (state != RunState::Calibrating) {\n    if (pump.isRunning()) {\n      pump.stop();\n    }\n  }\n\n  pump.update();\n\n  // --- result calculation ---\n  if (state == RunState::Finished || state == RunState::Paused || state == RunState::Error) {\n    computeResult(settings, consumedGrams, resultConcentrationM);\n  }\n}'
        ),
        # 2h. drawDisplay – pump status line
        (
            '  snprintf(line, sizeof(line), "P0 %u P1 %.1f/%.1f", pump.commandPwm(), sampleDeliveredGrams, settings.sampleGrams);\n  k10.canvas->canvasText(line, 11, pump.isCommanded() || samplePump.isCommanded() ? COLOR_WARN : COLOR_MUTED);',
            '  snprintf(line, sizeof(line), "PULSE %s  S %.1f/%.1f", pump.isRunning() ? "ON" : "off", sampleDeliveredGrams, settings.sampleGrams);\n  k10.canvas->canvasText(line, 11, pump.isRunning() || samplePump.isRunning() ? COLOR_WARN : COLOR_MUTED);'
        ),
        # 2i. handleJson – pump fields
        (
            '  json += ",\\"pump\\":" + String(pump.isCommanded() ? "true" : "false");\n  json += ",\\"pump_pwm\\":" + String(pump.commandPwm());\n  json += ",\\"sample_pump\\":" + String(samplePump.isCommanded() ? "true" : "false");',
            '  json += ",\\"pump\\":" + String(pump.isRunning() ? "true" : "false");\n  json += ",\\"pump_pulse_ms\\":" + String(pump.isRunning() ? 1 : 0);\n  json += ",\\"sample_pump\\":" + String(samplePump.isRunning() ? "true" : "false");'
        ),
        # 2j. loop – branch to calibration
        (
            '  handleButton(readButtons());\n  sampleSensors();\n  runController();\n  drawDisplay();\n}',
            '  handleButton(readButtons());\n  sampleSensors();\n  if (state == RunState::Calibrating) {\n    runCalibration();\n  } else {\n    runController();\n  }\n  drawDisplay();\n}'
        ),
    ])

    # ------------------------------------------------------------------
    # 3. Add calibration functions + loadCalibration in setup()
    # ------------------------------------------------------------------
    with open(ino_path, "r", encoding="utf-8") as f:
        ino = f.read()

    # Insert runCalibration / saveCalibration / loadCalibration before drawDisplay()
    cal_code = (
        "void runCalibration() {\n"
        "  static uint32_t calStartMs = 0;\n"
        "  static int calPhase = 0;\n"
        "  static float calInitialWeight = 0.0f;\n"
        "\n"
        "  if (calPhase == 0) {\n"
        "    calPhase = 1;\n"
        "    calStartMs = millis();\n"
        "    calInitialWeight = lastScale.grams;\n"
        "    pump.stop();\n"
        "    samplePump.stop();\n"
        "    statusLine = \"Calib: place bottle + tare\";\n"
        "    displayDirty = true;\n"
        "    return;\n"
        "  }\n"
        "\n"
        "  uint32_t elapsed = millis() - calStartMs;\n"
        "\n"
        "  if (calPhase == 1 && elapsed >= 2000) {\n"
        "    pump.runForMs(2000);\n"
        "    calPhase = 2;\n"
        "    statusLine = \"Calib: titrant 2s\";\n"
        "    displayDirty = true;\n"
        "    return;\n"
        "  }\n"
        "\n"
        "  if (calPhase == 2 && !pump.isRunning() && elapsed >= 6000) {\n"
        "    float delta = lastScale.grams - calInitialWeight;\n"
        "    titrantPumpFlowRateGps = delta / 2.0f;\n"
        "    calInitialWeight = lastScale.grams;\n"
        "    samplePump.runForMs(2000);\n"
        "    calPhase = 3;\n"
        "    statusLine = \"Calib: sample 2s\";\n"
        "    displayDirty = true;\n"
        "    return;\n"
        "  }\n"
        "\n"
        "  if (calPhase == 3 && !samplePump.isRunning() && elapsed >= 10000) {\n"
        "    float delta = lastScale.grams - calInitialWeight;\n"
        "    samplePumpFlowRateGps = delta / 2.0f;\n"
        "    saveCalibration();\n"
        "    calPhase = 0;\n"
        "    setState(RunState::SetupReady, \"Calibration done\");\n"
        "    statusLine = \"Calib finished\";\n"
        "    displayDirty = true;\n"
        "    return;\n"
        "  }\n"
        "}\n"
        "\n"
        "void saveCalibration() {\n"
        "  Preferences prefs;\n"
        "  if (prefs.begin(\"cal\", false)) {\n"
        "    prefs.putFloat(\"titrant_gps\", titrantPumpFlowRateGps);\n"
        "    prefs.putFloat(\"sample_gps\", samplePumpFlowRateGps);\n"
        "    prefs.end();\n"
        "  }\n"
        "}\n"
        "\n"
        "void loadCalibration() {\n"
        "  Preferences prefs;\n"
        "  if (prefs.begin(\"cal\", true)) {\n"
        "    titrantPumpFlowRateGps = prefs.getFloat(\"titrant_gps\", 0.0f);\n"
        "    samplePumpFlowRateGps = prefs.getFloat(\"sample_gps\", 0.0f);\n"
        "    prefs.end();\n"
        "  }\n"
        "}\n"
        "\n"
    )

    marker = "void drawDisplay() {\n  if (!displayDirty) {\n    return;\n  }"
    if marker in ino:
        ino = ino.replace(marker, cal_code + marker, 1)
        print("  Inserted calibration functions into ph_titrator.ino")
    else:
        print("  WARNING: could not find insertion point for calibration functions")

    # Add loadCalibration() call in setup()
    setup_marker = "  scaleReady = scaleSensor.begin();\n\n  if (!phReady || !scaleReady) {"
    setup_replacement = "  scaleReady = scaleSensor.begin();\n\n  loadCalibration();\n\n  if (!phReady || !scaleReady) {"
    if setup_marker in ino:
        ino = ino.replace(setup_marker, setup_replacement, 1)
        print("  Inserted loadCalibration() into setup()")
    else:
        print("  WARNING: could not find setup() insertion point")

    with open(ino_path, "w", encoding="utf-8") as f:
        f.write(ino)

    # ------------------------------------------------------------------
    # 4. Rewrite tests
    # ------------------------------------------------------------------
    test_path = os.path.join(root, "tests", "ph_titrator_control_test.cpp")
    test_code = r'''#include <cmath>
#include <iostream>
#include <string>

#include "../ph_titrator/control_logic.h"

static int failures = 0;

void expectTrue(bool value, const std::string &name) {
  if (!value) {
    std::cerr << "FAIL: " << name << "\n";
    failures++;
  }
}

void expectEqual(int actual, int expected, const std::string &name) {
  if (actual != expected) {
    std::cerr << "FAIL: " << name << " expected " << expected << " got " << actual << "\n";
    failures++;
  }
}

void expectNear(float actual, float expected, float tolerance, const std::string &name) {
  if (std::fabs(actual - expected) > tolerance) {
    std::cerr << "FAIL: " << name << " expected " << expected << " got " << actual << "\n";
    failures++;
  }
}

int main() {
  TitrationSettings settings;
  settings.mode = TitrationMode::AddBase;
  settings.targetPh = 7.00f;
  settings.tolerancePh = 0.05f;
  settings.maxConsumedGrams = 75.0f;

  TitrationDynamics dyn;

  // ---- decideAdaptiveDose: boundary conditions ----

  // Invalid pH -> Error / InvalidReading
  {
    TitrationDecision d = decideAdaptiveDose(settings, -1.0f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Error, "invalid pH returns Error");
    expectTrue(d.reason == TitrationStopReason::InvalidReading, "invalid pH reason is InvalidReading");
    expectEqual(d.pumpPulseMs, 0, "invalid pH uses zero pulse");
  }

  // Mass limit -> Error / MassLimit
  {
    TitrationDecision d = decideAdaptiveDose(settings, 5.80f, 75.0f, dyn);
    expectTrue(d.action == TitrationAction::Error, "mass limit returns Error");
    expectTrue(d.reason == TitrationStopReason::MassLimit, "mass limit reason is MassLimit");
    expectEqual(d.pumpPulseMs, 0, "mass limit uses zero pulse");
  }

  // Deadband -> Done / TargetReached
  {
    TitrationDecision d = decideAdaptiveDose(settings, 6.98f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "deadband returns Done");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "deadband reason is TargetReached");
    expectEqual(d.pumpPulseMs, 0, "deadband uses zero pulse");
  }

  // Base mode overshoot -> Done / TargetReached
  {
    dyn.reset();
    dyn.add(7.10f, 0);
    dyn.add(7.20f, 1000);
    TitrationDecision d = decideAdaptiveDose(settings, 7.20f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "base overshoot returns Done");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "base overshoot reason is TargetReached");
  }
  dyn.reset();

  // Far error -> large pulse
  {
    TitrationDecision d = decideAdaptiveDose(settings, 5.80f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "far error doses");
    expectEqual(d.pumpPulseMs, 600, "far error uses 600ms pulse");
    expectEqual(d.settleMs, 2000, "far error uses 2s settle");
  }

  // Medium error -> medium pulse
  {
    TitrationDecision d = decideAdaptiveDose(settings, 6.55f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "medium error doses");
    expectEqual(d.pumpPulseMs, 200, "medium error uses 200ms pulse");
    expectEqual(d.settleMs, 3500, "medium error uses 3.5s settle");
  }

  // Near error -> small pulse
  {
    TitrationDecision d = decideAdaptiveDose(settings, 6.82f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "near error doses");
    expectEqual(d.pumpPulseMs, 80, "near error uses 80ms pulse");
    expectEqual(d.settleMs, 5000, "near error uses 5s settle");
  }

  // Steep slope -> micro pulse
  {
    dyn.reset();
    dyn.add(6.50f, 0);
    dyn.add(7.50f, 1000); // dpH/dt = 1.0 > 0.08
    TitrationDecision d = decideAdaptiveDose(settings, 6.82f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "steep slope doses");
    expectEqual(d.pumpPulseMs, 30, "steep slope uses 30ms pulse");
    expectEqual(d.settleMs, 8000, "steep slope uses 8s settle");
  }
  dyn.reset();

  // Acid mode: far above target -> large dose
  {
    settings.mode = TitrationMode::AddAcid;
    TitrationDecision d = decideAdaptiveDose(settings, 8.40f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "acid mode doses above target");
    expectEqual(d.pumpPulseMs, 600, "acid far error uses 600ms pulse");
  }

  // Acid mode: near target -> stop
  {
    TitrationDecision d = decideAdaptiveDose(settings, 7.03f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "acid near target stops");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "acid near target reason is TargetReached");
  }

  settings.mode = TitrationMode::AddBase;

  // ---- Helper functions ----

  expectNear(computeConsumedGrams(312.5f, 300.0f), 12.5f, 0.001f, "consumed grams from bottle weight loss");
  expectNear(computeConsumedGrams(312.5f, 313.2f), 0.0f, 0.001f, "negative consumption is clamped");
  expectNear(computeSampleGainGrams(100.0f, 120.0f), 20.0f, 0.001f, "sample delivery uses reactor weight gain");
  expectNear(computeSampleGainGrams(120.0f, 100.0f), 0.0f, 0.001f, "sample delivery ignores weight loss");
  expectNear(titrantMolarityForPreset(TitrantPreset::Naoh001, 0.2f), 0.01f, 0.001f, "NaOH preset uses 0.01 mol/L");
  expectNear(titrantMolarityForPreset(TitrantPreset::Manual, 0.05f), 0.05f, 0.001f, "manual titrant molarity is used");
  expectNear(computeSampleConcentrationMolar(0.01f, 4.0f, 40.0f), 0.001f, 0.000001f, "sample concentration uses titrant molarity and mass ratio");
  expectNear(computeProbeMillivoltsFromAdsInput(1329.3334f), -59.0f, 0.1f, "alkaline calibration maps ADS input to probe millivolts");
  expectNear(computeProbeMillivoltsFromAdsInput(2387.3333f), 296.0f, 0.1f, "acid calibration maps ADS input to probe millivolts");
  expectNear(computePhFromProbeMillivolts(-58.0f), 8.11f, 0.01f, "probe millivolts maps to calibrated pH");
  expectNear(computePhFromProbeMillivolts(296.0f), 2.14f, 0.01f, "acid probe millivolts maps to calibrated pH");

  // ---- PhFilter ----

  PhFilter filter;
  int16_t rawSamples[] = {100, 102, 98, 250, 101, 0, 99};
  for (int16_t raw : rawSamples) {
    filter.add(raw);
  }
  expectTrue(filter.ready(), "filter is ready after window fills");
  expectNear(filter.filteredRaw(), 100.0f, 0.001f, "filter trims outliers and averages middle samples");

  // ---- TitrationDynamics ----
  {
    TitrationDynamics d;
    expectTrue(!d.isSteep(), "empty dynamics is not steep");
    d.add(6.0f, 0);
    d.add(6.05f, 1000);
    expectTrue(!d.isSteep(), "gentle slope is not steep");
    d.reset();
    d.add(6.0f, 0);
    d.add(6.1f, 1000);
    expectTrue(d.isSteep(), "steep slope detected");
  }

  if (failures != 0) {
    return 1;
  }

  std::cout << "All ph titrator control tests passed\n";
  return 0;
}
'''
    with open(test_path, "w", encoding="utf-8") as f:
        f.write(test_code)
    print(f"  Rewrote {test_path}")

    print("\nAll patches applied.")


if __name__ == "__main__":
    main()
