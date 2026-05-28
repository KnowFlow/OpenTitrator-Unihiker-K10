#include "C:/Users/rocke/Documents/New project/ph_titrator/control_logic.h"

int failures = 0;

void expectTrue(bool value) {
  if (!value) {
    failures++;
  }
}

void expectEqual(int actual, int expected) {
  if (actual != expected) {
    failures++;
  }
}

void setup() {
  TitrationSettings settings;
  settings.targetPh = 7.00f;
  settings.tolerancePh = 0.05f;
  settings.maxConsumedGrams = 75.0f;

  settings.mode = TitrationMode::AddBase;
  TitrationDecision baseFar = decideTitrationStep(settings, 5.80f, 12.0f);
  expectTrue(baseFar.action == TitrationAction::Dose);
  expectEqual(baseFar.pumpPulseMs, 900);
  expectEqual(baseFar.settleMs, 3500);

  TitrationDecision baseNear = decideTitrationStep(settings, 6.97f, 12.0f);
  expectTrue(baseNear.action == TitrationAction::Done);

  TitrationDecision baseOvershoot = decideTitrationStep(settings, 7.12f, 12.0f);
  expectTrue(baseOvershoot.action == TitrationAction::Done);

  settings.mode = TitrationMode::AddAcid;
  TitrationDecision acidFar = decideTitrationStep(settings, 8.40f, 12.0f);
  expectTrue(acidFar.action == TitrationAction::Dose);
  expectEqual(acidFar.pumpPulseMs, 900);

  TitrationDecision acidNear = decideTitrationStep(settings, 7.03f, 12.0f);
  expectTrue(acidNear.action == TitrationAction::Done);

  settings.mode = TitrationMode::AddBase;
  TitrationDecision mediumPulse = decideTitrationStep(settings, 6.65f, 12.0f);
  expectEqual(mediumPulse.pumpPulseMs, 450);

  TitrationDecision shortPulse = decideTitrationStep(settings, 6.88f, 12.0f);
  expectEqual(shortPulse.pumpPulseMs, 180);

  TitrationDecision massLimit = decideTitrationStep(settings, 5.80f, 75.0f);
  expectTrue(massLimit.action == TitrationAction::Error);
  expectTrue(massLimit.reason == TitrationStopReason::MassLimit);

  expectTrue(abs(computeConsumedGrams(312.5f, 300.0f) - 12.5f) < 0.001f);
  expectTrue(abs(computeConsumedGrams(312.5f, 313.2f) - 0.0f) < 0.001f);
}

void loop() {
}
