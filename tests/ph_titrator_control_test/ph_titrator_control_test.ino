#include "../../ph_titrator/control_logic.h"

int failures = 0;

void expectTrue(bool value, const char *name) {
  if (!value) {
    Serial.print("FAIL: ");
    Serial.println(name);
    failures++;
  }
}

void expectEqual(int actual, int expected, const char *name) {
  if (actual != expected) {
    Serial.print("FAIL: ");
    Serial.print(name);
    Serial.print(" expected ");
    Serial.print(expected);
    Serial.print(" got ");
    Serial.println(actual);
    failures++;
  }
}

void expectNear(float actual, float expected, float tolerance, const char *name) {
  if (fabs(actual - expected) > tolerance) {
    Serial.print("FAIL: ");
    Serial.print(name);
    Serial.print(" expected ");
    Serial.print(expected);
    Serial.print(" got ");
    Serial.println(actual);
    failures++;
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Running ph titrator control tests...");

  TitrationSettings settings;
  settings.targetPh = 7.00f;
  settings.tolerancePh = 0.05f;
  settings.maxConsumedGrams = 75.0f;

  // ---- computePumpControl: boundary conditions ----

  {
    PumpControlState ctrl;
    PumpControlDecision d = computePumpControl(settings, -1.0f, 12.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Error, "invalid pH returns Error");
    expectTrue(d.reason == TitrationStopReason::InvalidReading, "invalid pH reason is InvalidReading");
    expectEqual(d.pwm, 0, "invalid pH uses zero pwm");
  }

  {
    PumpControlState ctrl;
    PumpControlDecision d = computePumpControl(settings, 5.80f, 75.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Error, "mass limit returns Error");
    expectTrue(d.reason == TitrationStopReason::MassLimit, "mass limit reason is MassLimit");
    expectEqual(d.pwm, 0, "mass limit uses zero pwm");
  }

  {
    PumpControlState ctrl;
    PumpControlDecision d = computePumpControl(settings, 6.95f, 12.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Done, "deadband returns Done");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "deadband reason is TargetReached");
    expectEqual(d.pwm, 0, "deadband uses zero pwm");
  }

  {
    PumpControlState ctrl;
    PumpControlDecision d = computePumpControl(settings, 7.20f, 12.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Done, "base overshoot returns Done");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "base overshoot reason is TargetReached");
  }

  {
    PumpControlState ctrl;
    PumpControlDecision d = computePumpControl(settings, 5.80f, 12.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Dose, "fast error doses");
    expectEqual(d.pwm, 255, "fast error uses full pwm");
    expectTrue(!d.finePulse, "fast error disables fine pulse");
  }

  {
    PumpControlState ctrl;
    PumpControlDecision d = computePumpControl(settings, 6.55f, 12.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Dose, "medium error doses");
    expectTrue(d.pwm > 0 && d.pwm < 255, "medium error uses reduced pwm");
    expectTrue(!d.finePulse, "medium error disables fine pulse");
  }

  {
    PumpControlState ctrl;
    PumpControlDecision d = computePumpControl(settings, 6.82f, 12.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Dose, "fine error doses");
    expectTrue(d.finePulse, "fine error uses fine pulse");
    expectTrue(d.pwm > 0 && d.pwm < 60, "fine error uses low pwm");
  }

  {
    PumpControlState ctrl;
    uint8_t pwm1 = computePumpControl(settings, 6.55f, 12.0f, 2.0f, ctrl).pwm;
    uint8_t pwm2 = computePumpControl(settings, 6.55f, 12.0f, 2.0f, ctrl).pwm;
    expectTrue(pwm2 > pwm1, "integral accumulates and increases pwm");
  }

  {
    PumpControlState ctrl;
    settings.mode = TitrationMode::AddAcid;
    PumpControlDecision d = computePumpControl(settings, 8.40f, 12.0f, 2.0f, ctrl);
    expectTrue(d.action == TitrationAction::Dose, "acid mode doses above target");
    expectEqual(d.pwm, 255, "acid far error uses full pwm");
  }

  {
    PumpControlState ctrl;
    settings.mode = TitrationMode::AddAcid;
    PumpControlDecision d = computePumpControl(settings, 7.03f, 12.0f, 2.0f, ctrl);
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
  for (int i = 0; i < 7; i++) {
    filter.add(rawSamples[i]);
  }
  expectTrue(filter.ready(), "filter is ready after window fills");
  expectNear(filter.filteredRaw(), 100.0f, 0.001f, "filter trims outliers and averages middle samples");

  if (failures == 0) {
    Serial.println("All ph titrator control tests passed");
  } else {
    Serial.print("Tests failed: ");
    Serial.println(failures);
  }
}

void loop() {
}
