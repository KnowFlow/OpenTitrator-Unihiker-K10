#ifndef PH_TITRATOR_CONTROL_LOGIC_H
#define PH_TITRATOR_CONTROL_LOGIC_H

#include <stdint.h>

enum class TitrationMode : uint8_t {
  AddBase,
  AddAcid
};

enum class TitrantPreset : uint8_t {
  Naoh001,
  Hcl001,
  Manual
};

enum class TitrationAction : uint8_t {
  Dose,
  Done,
  Error
};

enum class TitrationStopReason : uint8_t {
  None,
  TargetReached,
  MassLimit,
  InvalidReading,
  SensorFault
};

struct TitrationSettings {
  TitrationMode mode = TitrationMode::AddBase;
  TitrantPreset titrantPreset = TitrantPreset::Naoh001;
  float targetPh = 7.00f;
  float tolerancePh = 0.05f;
  float maxConsumedGrams = 75.0f;
  float sampleGrams = 20.0f;
  float titrantMolarity = 0.01f;
};

struct TitrationDecision {
  TitrationAction action = TitrationAction::Done;
  TitrationStopReason reason = TitrationStopReason::None;
  uint16_t pumpPulseMs = 0;
  uint16_t settleMs = 0;
};

struct PhFilter {
  static constexpr uint8_t Window = 7;
  int16_t values[Window] = {};
  uint8_t index = 0;
  uint8_t count = 0;

  void reset() {
    index = 0;
    count = 0;
    for (uint8_t i = 0; i < Window; i++) {
      values[i] = 0;
    }
  }

  void add(int16_t raw) {
    values[index] = raw;
    index = (index + 1) % Window;
    if (count < Window) {
      count++;
    }
  }

  bool ready() const {
    return count == Window;
  }

  float filteredRaw() const {
    int16_t sorted[Window];
    for (uint8_t i = 0; i < Window; i++) {
      sorted[i] = values[i];
    }
    for (uint8_t i = 1; i < Window; i++) {
      int16_t key = sorted[i];
      int8_t j = i - 1;
      while (j >= 0 && sorted[j] > key) {
        sorted[j + 1] = sorted[j];
        j--;
      }
      sorted[j + 1] = key;
    }

    int32_t sum = 0;
    for (uint8_t i = 1; i < Window - 1; i++) {
      sum += sorted[i];
    }
    return (float)sum / (float)(Window - 2);
  }
};

struct PumpControlState {
  float integral = 0.0f;
};

struct PumpControlDecision {
  TitrationAction action = TitrationAction::Done;
  TitrationStopReason reason = TitrationStopReason::None;
  uint8_t pwm = 0;
  bool finePulse = false;
  bool precontrol = false;
};

inline float absoluteFloat(float value) {
  return value < 0.0f ? -value : value;
}

inline bool isValidPh(float ph) {
  return ph >= 0.0f && ph <= 14.0f;
}

inline float computeConsumedGrams(float initialWeightGrams, float currentWeightGrams) {
  float consumed = initialWeightGrams - currentWeightGrams;
  return consumed > 0.0f ? consumed : 0.0f;
}

inline float computeSampleGainGrams(float initialWeightGrams, float currentWeightGrams) {
  float gained = currentWeightGrams - initialWeightGrams;
  return gained > 0.0f ? gained : 0.0f;
}

inline float titrantMolarityForPreset(TitrantPreset preset, float manualMolarity) {
  switch (preset) {
    case TitrantPreset::Naoh001:
    case TitrantPreset::Hcl001:
      return 0.01f;
    case TitrantPreset::Manual:
      return manualMolarity > 0.0f ? manualMolarity : 0.0f;
  }
  return 0.0f;
}

inline float computeSampleConcentrationMolar(
    float titrantMolarity,
    float titrantUsedGrams,
    float sampleGrams) {
  if (titrantMolarity <= 0.0f || titrantUsedGrams <= 0.0f || sampleGrams <= 0.0f) {
    return 0.0f;
  }
  return titrantMolarity * titrantUsedGrams / sampleGrams;
}

inline float mapLinear(float value, float inLow, float inHigh, float outLow, float outHigh) {
  return outLow + ((value - inLow) * (outHigh - outLow)) / (inHigh - inLow);
}

inline float computeProbeMillivoltsFromAdsInput(float adsInputMillivolts) {
  constexpr float alkalineAdsInputMillivolts = 1329.3334f;
  constexpr float alkalineProbeMillivolts = -59.0f;
  constexpr float acidAdsInputMillivolts = 2387.3333f;
  constexpr float acidProbeMillivolts = 296.0f;
  return mapLinear(
      adsInputMillivolts,
      alkalineAdsInputMillivolts,
      acidAdsInputMillivolts,
      alkalineProbeMillivolts,
      acidProbeMillivolts);
}

inline float computePhFromProbeMillivolts(float probeMillivolts) {
  constexpr float alkalineProbeMillivolts = -58.0f;
  constexpr float alkalinePh = 8.11f;
  constexpr float acidProbeMillivolts = 296.0f;
  constexpr float acidPh = 2.14f;
  return mapLinear(
      probeMillivolts,
      alkalineProbeMillivolts,
      acidProbeMillivolts,
      alkalinePh,
      acidPh);
}

inline float clampFloat(float value, float low, float high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

inline uint8_t clampPwm(float value) {
  if (value <= 0.0f) {
    return 0;
  }
  if (value >= 255.0f) {
    return 255;
  }
  return (uint8_t)(value + 0.5f);
}

inline bool isSensorFaultRaw10Bit(int rawValue) {
  return rawValue == 0 || rawValue == 1023;
}

inline int adsRawToAnalog10Bit(int16_t adsRaw) {
  int scaled = (int)((adsRaw + 16) / 32);
  if (scaled < 0) {
    return 0;
  }
  if (scaled > 1023) {
    return 1023;
  }
  return scaled;
}

inline PumpControlDecision computePumpControl(
    const TitrationSettings &settings,
    float currentPh,
    float consumedGrams,
    float dtSeconds,
    PumpControlState &control) {
  constexpr float deadbandPh = 0.08f;
  constexpr float fineThresholdPh = 0.30f;
  constexpr float fastThresholdPh = 1.00f;
  constexpr float kp = 8.0f;
  constexpr float ki = 0.8f;
  constexpr float integralMax = 40.0f;

  PumpControlDecision decision;
  if (!isValidPh(currentPh)) {
    decision.action = TitrationAction::Error;
    decision.reason = TitrationStopReason::InvalidReading;
    return decision;
  }

  if (consumedGrams >= settings.maxConsumedGrams) {
    decision.action = TitrationAction::Error;
    decision.reason = TitrationStopReason::MassLimit;
    return decision;
  }

  const float error = currentPh - settings.targetPh;
  const float magnitude = absoluteFloat(error);
  if (magnitude <= deadbandPh) {
    decision.action = TitrationAction::Done;
    decision.reason = TitrationStopReason::TargetReached;
    return decision;
  }

  const bool shouldAddBase = settings.mode == TitrationMode::AddBase && currentPh < settings.targetPh;
  const bool shouldAddAcid = settings.mode == TitrationMode::AddAcid && currentPh > settings.targetPh;
  if (!shouldAddBase && !shouldAddAcid) {
    decision.action = TitrationAction::Done;
    decision.reason = TitrationStopReason::TargetReached;
    return decision;
  }

  float basePwm = 25.0f;
  float piOutput = 0.0f;
  decision.finePulse = true;
  decision.precontrol = magnitude <= fineThresholdPh;
  if (magnitude > fastThresholdPh) {
    basePwm = 255.0f;
    decision.finePulse = false;
    control.integral *= 0.5f;
  } else if (magnitude > fineThresholdPh) {
    basePwm = 55.0f;
    decision.finePulse = false;
    control.integral += magnitude * dtSeconds;
    control.integral = clampFloat(control.integral, -integralMax, integralMax);
    piOutput = (kp * magnitude) + (ki * control.integral);
  } else {
    control.integral *= 0.5f;
  }

  decision.action = TitrationAction::Dose;
  decision.pwm = clampPwm(basePwm + piOutput);
  return decision;
}

inline uint16_t pulseForError(float errorMagnitude) {
  if (errorMagnitude > 0.70f) {
    return 900;
  }
  if (errorMagnitude > 0.20f) {
    return 450;
  }
  return 180;
}

inline TitrationDecision decideTitrationStep(
    const TitrationSettings &settings,
    float currentPh,
    float consumedGrams) {
  TitrationDecision decision;

  if (!isValidPh(currentPh)) {
    decision.action = TitrationAction::Error;
    decision.reason = TitrationStopReason::InvalidReading;
    return decision;
  }

  if (consumedGrams >= settings.maxConsumedGrams) {
    decision.action = TitrationAction::Error;
    decision.reason = TitrationStopReason::MassLimit;
    return decision;
  }

  const float signedError = settings.targetPh - currentPh;
  const float magnitude = absoluteFloat(signedError);

  if (magnitude <= settings.tolerancePh) {
    decision.action = TitrationAction::Done;
    decision.reason = TitrationStopReason::TargetReached;
    return decision;
  }

  const bool shouldAddBase = settings.mode == TitrationMode::AddBase && currentPh < settings.targetPh;
  const bool shouldAddAcid = settings.mode == TitrationMode::AddAcid && currentPh > settings.targetPh;

  if (!shouldAddBase && !shouldAddAcid) {
    decision.action = TitrationAction::Done;
    decision.reason = TitrationStopReason::TargetReached;
    return decision;
  }

  decision.action = TitrationAction::Dose;
  decision.reason = TitrationStopReason::None;
  decision.pumpPulseMs = pulseForError(magnitude);
  decision.settleMs = 3500;
  return decision;
}

#endif
