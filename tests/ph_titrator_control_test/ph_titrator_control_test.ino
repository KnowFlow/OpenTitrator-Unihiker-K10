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
  if (abs(actual - expected) > tolerance) {
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

  settings.mode = TitrationMode::AddBase;
  TitrationDecision baseFar = decideTitrationStep(settings, 5.80f, 12.0f);
  expectTrue(baseFar.action == TitrationAction::Dose, "base mode doses below target");
  expectEqual(baseFar.pumpPulseMs, 900, "base mode uses long pulse far from target");
  expectEqual(baseFar.settleMs, 3500, "base mode waits after dose");

  TitrationDecision baseNear = decideTitrationStep(settings, 6.97f, 12.0f);
  expectTrue(baseNear.action == TitrationAction::Done, "base mode stops inside tolerance");

  TitrationDecision baseOvershoot = decideTitrationStep(settings, 7.12f, 12.0f);
  expectTrue(baseOvershoot.action == TitrationAction::Done, "base mode stops after overshoot");

  settings.mode = TitrationMode::AddAcid;
  TitrationDecision acidFar = decideTitrationStep(settings, 8.40f, 12.0f);
  expectTrue(acidFar.action == TitrationAction::Dose, "acid mode doses above target");
  expectEqual(acidFar.pumpPulseMs, 900, "acid mode uses long pulse far from target");

  TitrationDecision acidNear = decideTitrationStep(settings, 7.03f, 12.0f);
  expectTrue(acidNear.action == TitrationAction::Done, "acid mode stops inside tolerance");

  settings.mode = TitrationMode::AddBase;
  TitrationDecision mediumPulse = decideTitrationStep(settings, 6.65f, 12.0f);
  expectEqual(mediumPulse.pumpPulseMs, 450, "medium error uses medium pulse");

  TitrationDecision shortPulse = decideTitrationStep(settings, 6.88f, 12.0f);
  expectEqual(shortPulse.pumpPulseMs, 180, "small error uses short pulse");

  TitrationDecision massLimit = decideTitrationStep(settings, 5.80f, 75.0f);
  expectTrue(massLimit.action == TitrationAction::Error, "mass limit raises error");
  expectTrue(massLimit.reason == TitrationStopReason::MassLimit, "mass limit reason is recorded");

  expectNear(computeConsumedGrams(312.5f, 300.0f), 12.5f, 0.001f, "consumed grams from bottle weight loss");
  expectNear(computeConsumedGrams(312.5f, 313.2f), 0.0f, 0.001f, "negative consumption is clamped");
  expectNear(computeProbeMillivoltsFromAdsInput(1329.3334f), -59.0f, 0.1f, "ADS input maps to probe millivolts");
  expectNear(computePhFromProbeMillivolts(-58.0f), 8.11f, 0.01f, "probe millivolts maps to calibrated pH");

  if (failures == 0) {
    Serial.println("All ph titrator control tests passed");
  } else {
    Serial.print("Tests failed: ");
    Serial.println(failures);
  }
}

void loop() {
}
