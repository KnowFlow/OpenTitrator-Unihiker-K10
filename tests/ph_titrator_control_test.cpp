#include <cmath>
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
  expectNear(computeSampleGainGrams(100.0f, 120.0f), 20.0f, 0.001f, "sample delivery uses reactor weight gain");
  expectNear(computeSampleGainGrams(120.0f, 100.0f), 0.0f, 0.001f, "sample delivery ignores weight loss");
  expectNear(titrantMolarityForPreset(TitrantPreset::Naoh001, 0.2f), 0.01f, 0.001f, "NaOH preset uses 0.01 mol/L");
  expectNear(titrantMolarityForPreset(TitrantPreset::Manual, 0.05f), 0.05f, 0.001f, "manual titrant molarity is used");
  expectNear(computeSampleConcentrationMolar(0.01f, 4.0f, 40.0f), 0.001f, 0.000001f, "sample concentration uses titrant molarity and mass ratio");
  expectNear(computeProbeMillivoltsFromAdsInput(1329.3334f), -59.0f, 0.1f, "alkaline calibration maps ADS input to probe millivolts");
  expectNear(computeProbeMillivoltsFromAdsInput(2387.3333f), 296.0f, 0.1f, "acid calibration maps ADS input to probe millivolts");
  expectNear(computePhFromProbeMillivolts(-58.0f), 8.11f, 0.01f, "probe millivolts maps to calibrated pH");
  expectNear(computePhFromProbeMillivolts(296.0f), 2.14f, 0.01f, "acid probe millivolts maps to calibrated pH");

  PhFilter filter;
  int16_t rawSamples[] = {100, 102, 98, 250, 101, 0, 99};
  for (int16_t raw : rawSamples) {
    filter.add(raw);
  }
  expectTrue(filter.ready(), "filter is ready after window fills");
  expectNear(filter.filteredRaw(), 100.0f, 0.001f, "filter trims outliers and averages middle samples");

  PumpControlState control;
  PumpControlDecision fastDose = computePumpControl(settings, 5.80f, 12.0f, 2.0f, control);
  expectTrue(fastDose.action == TitrationAction::Dose, "pump controller doses outside target");
  expectEqual(fastDose.pwm, 255, "large pH error uses full pump command");

  PumpControlDecision mediumDose = computePumpControl(settings, 6.55f, 12.0f, 2.0f, control);
  expectTrue(mediumDose.action == TitrationAction::Dose, "medium pH error still doses");
  expectTrue(mediumDose.pwm > 0 && mediumDose.pwm < 120, "medium pH error uses reduced pump command");

  PumpControlDecision precontrolDose = computePumpControl(settings, 6.82f, 12.0f, 2.0f, control);
  expectTrue(precontrolDose.action == TitrationAction::Dose, "precontrol pH error still doses");
  expectTrue(precontrolDose.finePulse, "precontrol pH error uses intermittent fine pulse");

  PumpControlDecision deadbandStop = computePumpControl(settings, 6.95f, 12.0f, 2.0f, control);
  expectTrue(deadbandStop.action == TitrationAction::Done, "pump controller stops in deadband");
  expectEqual(deadbandStop.pwm, 0, "deadband uses zero pump command");

  if (failures != 0) {
    return 1;
  }

  std::cout << "All ph titrator control tests passed\n";
  return 0;
}
