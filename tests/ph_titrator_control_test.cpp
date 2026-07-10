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
    expectEqual(d.pumpPulseMs, 450, "far error uses 450ms pulse");
    expectEqual(d.settleMs, 5000, "far error uses 5s settle");
  }

  // Medium error -> medium pulse
  {
    TitrationDecision d = decideAdaptiveDose(settings, 6.55f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "medium error doses");
    expectEqual(d.pumpPulseMs, 150, "medium error uses 150ms pulse");
    expectEqual(d.settleMs, 8000, "medium error uses 8s settle");
  }

  // Wider control band slows the same error earlier.
  {
    settings.controlBand = 0.50f;
    TitrationDecision d = decideAdaptiveDose(settings, 6.55f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "wide control band doses");
    expectEqual(d.pumpPulseMs, 60, "wide control band slows medium error");
    settings.controlBand = 0.30f;
  }

  // Settle limits clamp dose decisions.
  {
    settings.minSettleSeconds = 10;
    settings.maxSettleSeconds = 11;
    TitrationDecision d = decideAdaptiveDose(settings, 5.80f, 12.0f, dyn);
    expectEqual(d.settleMs, 10000, "min settle clamps far dose settle");
    settings.minSettleSeconds = 5;
    settings.maxSettleSeconds = 30;
  }

  // Near error -> small pulse
  {
    TitrationDecision d = decideAdaptiveDose(settings, 6.82f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "near error doses");
    expectEqual(d.pumpPulseMs, 60, "near error uses 60ms pulse");
    expectEqual(d.settleMs, 12000, "near error uses 12s settle");
  }

  // Very near error -> micro pulse
  {
    TitrationDecision d = decideAdaptiveDose(settings, 6.93f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "very near error doses");
    expectEqual(d.pumpPulseMs, 25, "very near error uses 25ms pulse");
    expectEqual(d.settleMs, 15000, "very near error uses 15s settle");
  }

  // Steep slope -> micro pulse
  {
    dyn.reset();
    dyn.add(5.00f, 0);
    dyn.add(5.20f, 1000); // dpH/dt = 0.2 > 0.08
    TitrationDecision d = decideAdaptiveDose(settings, 5.20f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "steep slope doses");
    expectEqual(d.pumpPulseMs, 25, "steep slope uses 25ms pulse");
    expectEqual(d.settleMs, 15000, "steep slope uses 15s settle");
  }
  dyn.reset();

  // Near target and still drifting upward -> preemptive stop
  {
    dyn.reset();
    dyn.add(9.43f, 0);
    dyn.add(9.44f, 2000);
    settings.targetPh = 9.50f;
    TitrationDecision d = decideAdaptiveDose(settings, 9.44f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "base near target and rising stops early");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "base predictive stop reason is TargetReached");
    settings.targetPh = 7.00f;
  }
  dyn.reset();

  // Acid mode: far above target -> large dose
  {
    settings.mode = TitrationMode::AddAcid;
    settings.controlTrend = ControlTrend::Decrease;
    TitrationDecision d = decideAdaptiveDose(settings, 8.40f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "acid mode doses above target");
    expectEqual(d.pumpPulseMs, 450, "acid far error uses 450ms pulse");
  }

  // Acid mode: near target -> stop
  {
    dyn.reset();
    TitrationDecision d = decideAdaptiveDose(settings, 7.03f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "acid near target stops");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "acid near target reason is TargetReached");
  }

  // Acid mode near target and drifting downward -> preemptive stop
  {
    dyn.reset();
    dyn.add(7.57f, 0);
    dyn.add(7.56f, 2000);
    settings.targetPh = 7.50f;
    TitrationDecision d = decideAdaptiveDose(settings, 7.56f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "acid near target and falling stops early");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "acid predictive stop reason is TargetReached");
    settings.targetPh = 7.00f;
  }

  // Acid mode gets a wider predictive stop to account for slower downward response.
  {
    dyn.reset();
    dyn.add(8.16f, 0);
    dyn.add(8.14f, 2000);
    settings.targetPh = 8.00f;
    TitrationDecision d = decideAdaptiveDose(settings, 8.14f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "acid 0.14 above target and falling stops early");
    expectTrue(d.reason == TitrationStopReason::TargetReached, "acid wider predictive stop reason is TargetReached");
    settings.targetPh = 7.00f;
  }

  settings.mode = TitrationMode::AddBase;
  settings.controlTrend = ControlTrend::Increase;

  // mV endpoint can be controlled without assuming an acid/base titrant.
  {
    settings.endpoint = ControlEndpoint::Millivolts;
    settings.targetMillivolts = 100.0f;
    settings.controlTrend = ControlTrend::Increase;
    dyn.reset();
    TitrationDecision d = decideAdaptiveDose(settings, 40.0f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "mV rising endpoint doses below target");
    d = decideAdaptiveDose(settings, 104.0f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "mV rising endpoint stops inside tolerance");

    settings.controlTrend = ControlTrend::Decrease;
    settings.targetMillivolts = -50.0f;
    d = decideAdaptiveDose(settings, 0.0f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Dose, "mV falling endpoint doses above target");
    d = decideAdaptiveDose(settings, -54.0f, 12.0f, dyn);
    expectTrue(d.action == TitrationAction::Done, "mV falling endpoint stops inside tolerance");

    settings.endpoint = ControlEndpoint::Ph;
    settings.controlTrend = ControlTrend::Increase;
    settings.targetPh = 7.00f;
  }

  // ---- Helper functions ----

  expectTrue(isEndpointReached(settings, 7.00f), "endpoint reached inside pH tolerance");
  expectTrue(!isEndpointReached(settings, 6.80f), "endpoint not reached before pH target");
  settings.endpoint = ControlEndpoint::Millivolts;
  settings.controlTrend = ControlTrend::Decrease;
  settings.targetMillivolts = -50.0f;
  expectTrue(isEndpointReached(settings, -51.0f), "falling mV endpoint reached past target");
  settings.endpoint = ControlEndpoint::Ph;
  settings.controlTrend = ControlTrend::Increase;
  settings.targetPh = 7.00f;

  expectNear(computeConsumedGrams(312.5f, 300.0f), 12.5f, 0.001f, "consumed grams from bottle weight loss");
  expectNear(computeConsumedGrams(312.5f, 313.2f), 0.0f, 0.001f, "negative consumption is clamped");
  expectNear(computeSampleGainGrams(100.0f, 120.0f), 20.0f, 0.001f, "sample delivery uses reactor weight gain");
  expectNear(computeSampleGainGrams(120.0f, 100.0f), 0.0f, 0.001f, "sample delivery ignores weight loss");
  expectNear(titrantMolarityForPreset(TitrantPreset::Naoh001, 0.2f), 0.01f, 0.001f, "NaOH preset uses 0.01 mol/L");
  expectNear(titrantMolarityForPreset(TitrantPreset::Edta001, 0.2f), 0.01f, 0.001f, "EDTA preset uses 0.01 mol/L");
  expectNear(titrantMolarityForPreset(TitrantPreset::Manual, 0.05f), 0.05f, 0.001f, "manual titrant molarity is used");
  expectNear(computeSampleConcentrationMolar(0.01f, 4.0f, 40.0f), 0.001f, 0.000001f, "sample concentration uses titrant molarity and mass ratio");
  expectNear(computeSampleConcentrationMolar(0.01f, 4.0f, 40.0f, 0.8f, 1.0f), 0.00125f, 0.000001f, "sample concentration converts titrant mass through density");
  expectNear(netTitrantGrams(4.0f, 0.5f), 3.5f, 0.001f, "blank correction subtracts titrant use");
  expectNear(netTitrantGrams(0.4f, 0.5f), 0.0f, 0.001f, "blank correction clamps negative titrant use");
  expectNear(computeEdtaHardnessCaCO3MgL(0.01f, 2.0f, 50.0f), 40.03476f, 0.001f, "EDTA hardness reports CaCO3 mg/L");
  expectNear(computeEdtaHardnessCaCO3MgL(0.01f, 2.0f, 50.0f, 0.8f, 1.0f), 50.04345f, 0.001f, "EDTA hardness converts titrant mass through density");
  expectNear(computeManualFactorResult(2.0f, 40.0f, 100.0f), 5.0f, 0.001f, "manual factor result scales net titrant by sample");

  settings.resultFormula = ResultFormula::EdtaHardnessCaCO3;
  settings.blankGrams = 0.5f;
  settings.titrantDensityGramsPerMl = 1.0f;
  settings.sampleDensityGramsPerMl = 1.0f;
  expectNear(computeTitrationResult(settings, 0.01f, 2.5f, 50.0f), 40.03476f, 0.001f, "result formula applies blank-corrected EDTA hardness");
  settings.titrantDensityGramsPerMl = 0.8f;
  expectNear(computeTitrationResult(settings, 0.01f, 2.5f, 50.0f), 50.04345f, 0.001f, "result formula applies density-corrected EDTA hardness");
  settings.titrantDensityGramsPerMl = 1.0f;
  settings.resultFormula = ResultFormula::ManualFactor;
  settings.manualResultFactor = 100.0f;
  expectNear(computeTitrationResult(settings, 0.01f, 2.5f, 40.0f), 5.0f, 0.001f, "result formula applies manual factor");
  settings.resultFormula = ResultFormula::AcidBaseMolar;
  settings.blankGrams = 0.0f;
  settings.manualResultFactor = 1.0f;

  {
    EqpTracker eqp;
    expectTrue(!eqp.add(0.0f, 300.0f), "EQP tracker stores initial point");
    expectTrue(!eqp.add(0.1f, 295.0f), "EQP tracker waits before peak confirmation");
    expectTrue(!eqp.add(0.2f, 280.0f), "EQP tracker records steepest slope");
    expectTrue(!eqp.add(0.3f, 276.0f), "EQP tracker needs two post-peak drops");
    expectTrue(eqp.add(0.4f, 274.0f), "EQP tracker confirms slope peak after two drops");
    expectNear(eqp.bestUsedGrams, 0.2f, 0.001f, "EQP tracker keeps peak used grams");
    expectNear(eqp.bestSignal, 280.0f, 0.001f, "EQP tracker keeps peak signal");

    settings.endpoint = ControlEndpoint::Millivolts;
    settings.maxConsumedGrams = 20.0f;
    TitrationDecision eqpDecision = decideEqpDose(settings, 274.0f, 0.4f, eqp);
    expectTrue(eqpDecision.action == TitrationAction::Done, "EQP dose decision stops at confirmed equivalence point");
    expectTrue(eqpDecision.reason == TitrationStopReason::EquivalencePoint, "EQP dose decision reports equivalence point");
  }

  {
    TitrationSettings methodSettings;
    applyTitrationMethodPreset(methodSettings, TitrationMethod::PhEndpoint);
    expectTrue(methodSettings.endpoint == ControlEndpoint::Ph, "pH method uses pH endpoint");
    expectTrue(methodSettings.controlTrend == ControlTrend::Increase, "pH method uses rising signal");
    expectTrue(methodSettings.titrantPreset == TitrantPreset::Naoh001, "pH method uses NaOH preset");
    expectTrue(methodSettings.resultFormula == ResultFormula::AcidBaseMolar, "pH method uses molar result");

    applyTitrationMethodPreset(methodSettings, TitrationMethod::MvEndpoint);
    expectTrue(methodSettings.endpoint == ControlEndpoint::Millivolts, "mV method uses mV endpoint");
    expectTrue(methodSettings.controlTrend == ControlTrend::Increase, "mV method uses rising signal");
    expectTrue(methodSettings.titrantPreset == TitrantPreset::Manual, "mV method uses manual titrant");
    expectTrue(methodSettings.resultFormula == ResultFormula::ManualFactor, "mV method uses manual factor result");

    applyTitrationMethodPreset(methodSettings, TitrationMethod::EdtaHardness);
    expectTrue(methodSettings.endpoint == ControlEndpoint::Millivolts, "EDTA method uses mV endpoint");
    expectTrue(methodSettings.controlTrend == ControlTrend::Decrease, "EDTA method uses falling signal");
    expectTrue(methodSettings.titrantPreset == TitrantPreset::Edta001, "EDTA method uses EDTA preset");
    expectTrue(methodSettings.resultFormula == ResultFormula::EdtaHardnessCaCO3, "EDTA method uses hardness result");

    methodSettings.targetMillivolts = 123.0f;
    applyTitrationMethodPreset(methodSettings, TitrationMethod::Manual);
    expectTrue(methodSettings.titrantPreset == TitrantPreset::Manual, "manual method uses manual titrant");
    expectNear(methodSettings.targetMillivolts, 123.0f, 0.001f, "manual method keeps current endpoint target");
  }

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
    expectTrue(!d.isSettled(), "empty dynamics is not settled");
    d.add(6.0f, 0);
    d.add(6.05f, 1000);
    expectTrue(!d.isSteep(), "gentle slope is not steep");
    expectTrue(!d.isSettled(), "two samples are not settled");
    d.add(6.054f, 2000);
    expectTrue(d.isSettled(), "stable response is settled");
    d.reset();
    d.add(6.0f, 0);
    d.add(6.1f, 1000);
    expectTrue(d.isSteep(), "steep slope detected");
    d.add(6.3f, 2000);
    expectTrue(!d.isSettled(), "moving response is not settled");
  }

  if (failures != 0) {
    return 1;
  }

  std::cout << "All ph titrator control tests passed\n";
  return 0;
}
