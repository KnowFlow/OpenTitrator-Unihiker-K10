#ifndef PH_TITRATOR_CONTROL_LOGIC_H
#define PH_TITRATOR_CONTROL_LOGIC_H

#include <stdint.h>

enum class TitrationMode : uint8_t {
  AddBase,
  AddAcid
};

enum class ControlEndpoint : uint8_t {
  Ph,
  Millivolts
};

enum class ControlTrend : uint8_t {
  Increase,
  Decrease
};

enum class TitrantPreset : uint8_t {
  Naoh001,
  Hcl001,
  Edta001,
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
  ControlEndpoint endpoint = ControlEndpoint::Ph;
  ControlTrend controlTrend = ControlTrend::Increase;
  TitrantPreset titrantPreset = TitrantPreset::Naoh001;
  float targetPh = 7.00f;
  float tolerancePh = 0.05f;
  float targetMillivolts = 0.0f;
  float toleranceMillivolts = 5.0f;
  float maxConsumedGrams = 20.0f;
  float sampleGrams = 20.0f;
  float titrantMolarity = 0.01f;
};

struct TitrationDecision {
  TitrationAction action = TitrationAction::Done;
  TitrationStopReason reason = TitrationStopReason::None;
  uint16_t pumpPulseMs = 0;
  uint16_t settleMs = 10000;
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

inline float absoluteFloat(float value);

struct TitrationDynamics {
  static constexpr uint8_t N = 5;
  float ph[N] = {};
  uint32_t t_ms[N] = {};
  uint8_t count = 0;
  uint8_t idx = 0;

  void reset() {
    count = 0;
    idx = 0;
  }

  void add(float phValue, uint32_t ms) {
    ph[idx] = phValue;
    t_ms[idx] = ms;
    idx = (idx + 1) % N;
    if (count < N) {
      count++;
    }
  }

  float dpH_dt() const {
    if (count < 2) {
      return 0.0f;
    }
    uint8_t i = (idx + N - 1) % N;
    uint8_t j = (idx + N - 2) % N;
    float dt = (t_ms[i] - t_ms[j]) / 1000.0f;
    return dt > 0.001f ? (ph[i] - ph[j]) / dt : 0.0f;
  }

  bool isSteep() const {
    return absoluteFloat(dpH_dt()) > 0.08f;
  }

  bool isSteep(ControlEndpoint endpoint) const {
    float limit = endpoint == ControlEndpoint::Millivolts ? 8.0f : 0.08f;
    return absoluteFloat(dpH_dt()) > limit;
  }

  bool isSettled(float maxAbsDpHPerSecond = 0.005f) const {
    return count >= 3 && absoluteFloat(dpH_dt()) <= maxAbsDpHPerSecond;
  }

  bool isSettled(ControlEndpoint endpoint) const {
    float limit = endpoint == ControlEndpoint::Millivolts ? 0.5f : 0.005f;
    return count >= 3 && absoluteFloat(dpH_dt()) <= limit;
  }

  bool isOvershooting(TitrationMode mode, float target) const {
    ControlTrend trend = mode == TitrationMode::AddBase ? ControlTrend::Increase : ControlTrend::Decrease;
    return isOvershooting(trend, ControlEndpoint::Ph, target);
  }

  bool isOvershooting(ControlTrend trend, ControlEndpoint endpoint, float target) const {
    if (count < 2) {
      return false;
    }
    float d = dpH_dt();
    uint8_t i = (idx + N - 1) % N;
    float margin = endpoint == ControlEndpoint::Millivolts ? 2.0f : 0.02f;
    bool doseRaisesValue = trend == ControlTrend::Increase;
    if (doseRaisesValue) {
      return ph[i] > target + margin && d > 0.0f;
    } else {
      return ph[i] < target - margin && d < 0.0f;
    }
  }
};

inline float absoluteFloat(float value) {
  return value < 0.0f ? -value : value;
}

inline bool isValidPh(float ph) {
  return ph >= 0.0f && ph <= 14.0f;
}

inline bool isValidMillivolts(float millivolts) {
  return millivolts >= -1000.0f && millivolts <= 1000.0f;
}

inline float controlTarget(const TitrationSettings &settings) {
  return settings.endpoint == ControlEndpoint::Millivolts ? settings.targetMillivolts : settings.targetPh;
}

inline float controlTolerance(const TitrationSettings &settings) {
  return settings.endpoint == ControlEndpoint::Millivolts ? settings.toleranceMillivolts : settings.tolerancePh;
}

inline bool isValidControlValue(const TitrationSettings &settings, float value) {
  return settings.endpoint == ControlEndpoint::Millivolts ? isValidMillivolts(value) : isValidPh(value);
}

inline bool doseIncreasesControlValue(const TitrationSettings &settings) {
  return settings.controlTrend == ControlTrend::Increase;
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
    case TitrantPreset::Edta001:
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

inline float computeProbeMillivoltsFromAdsInput(
    float adsInputMillivolts,
    float lowAdsInputMillivolts,
    float lowProbeMillivolts,
    float highAdsInputMillivolts,
    float highProbeMillivolts);

inline float computeProbeMillivoltsFromAdsInput(float adsInputMillivolts) {
  constexpr float alkalineAdsInputMillivolts = 1329.3334f;
  constexpr float alkalineProbeMillivolts = -59.0f;
  constexpr float acidAdsInputMillivolts = 2387.3333f;
  constexpr float acidProbeMillivolts = 296.0f;
  return computeProbeMillivoltsFromAdsInput(
      adsInputMillivolts,
      alkalineAdsInputMillivolts,
      alkalineProbeMillivolts,
      acidAdsInputMillivolts,
      acidProbeMillivolts);
}

inline float computeProbeMillivoltsFromAdsInput(
    float adsInputMillivolts,
    float lowAdsInputMillivolts,
    float lowProbeMillivolts,
    float highAdsInputMillivolts,
    float highProbeMillivolts) {
  return mapLinear(
      adsInputMillivolts,
      lowAdsInputMillivolts,
      highAdsInputMillivolts,
      lowProbeMillivolts,
      highProbeMillivolts);
}

inline float computePhFromProbeMillivolts(
    float probeMillivolts,
    float lowProbeMillivolts,
    float lowPh,
    float highProbeMillivolts,
    float highPh);

inline float computePhFromProbeMillivolts(float probeMillivolts) {
  constexpr float alkalineProbeMillivolts = -58.0f;
  constexpr float alkalinePh = 8.11f;
  constexpr float acidProbeMillivolts = 296.0f;
  constexpr float acidPh = 2.14f;
  return computePhFromProbeMillivolts(
      probeMillivolts,
      alkalineProbeMillivolts,
      alkalinePh,
      acidProbeMillivolts,
      acidPh);
}

inline float computePhFromProbeMillivolts(
    float probeMillivolts,
    float lowProbeMillivolts,
    float lowPh,
    float highProbeMillivolts,
    float highPh) {
  return mapLinear(
      probeMillivolts,
      lowProbeMillivolts,
      highProbeMillivolts,
      lowPh,
      highPh);
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

inline TitrationDecision decideAdaptiveDose(
    const TitrationSettings &settings,
    float currentValue,
    float consumedGrams,
    const TitrationDynamics &dyn) {
  TitrationDecision d;

  if (!isValidControlValue(settings, currentValue)) {
    d.action = TitrationAction::Error;
    d.reason = TitrationStopReason::InvalidReading;
    return d;
  }

  if (consumedGrams >= settings.maxConsumedGrams) {
    d.action = TitrationAction::Error;
    d.reason = TitrationStopReason::MassLimit;
    return d;
  }

  const float target = controlTarget(settings);
  const bool doseRaisesValue = doseIncreasesControlValue(settings);
  const bool shouldDose = doseRaisesValue ? currentValue < target : currentValue > target;
  if (!shouldDose) {
    d.action = TitrationAction::Done;
    d.reason = TitrationStopReason::TargetReached;
    return d;
  }

  if (dyn.isOvershooting(settings.controlTrend, settings.endpoint, target)) {
    d.action = TitrationAction::Done;
    d.reason = TitrationStopReason::TargetReached;
    return d;
  }

  const float error = absoluteFloat(currentValue - target);
  if (error <= controlTolerance(settings)) {
    d.action = TitrationAction::Done;
    d.reason = TitrationStopReason::TargetReached;
    return d;
  }

  const float slope = dyn.dpH_dt();
  const float driftThreshold = settings.endpoint == ControlEndpoint::Millivolts ? 0.1f : 0.001f;
  const bool driftingTowardTarget = doseRaisesValue ? slope > driftThreshold : slope < -driftThreshold;
  const float predictiveStopMargin =
      settings.endpoint == ControlEndpoint::Millivolts
          ? (settings.controlTrend == ControlTrend::Decrease ? 15.0f : 10.0f)
          : (settings.controlTrend == ControlTrend::Decrease ? 0.15f : 0.10f);
  if (error <= predictiveStopMargin && driftingTowardTarget) {
    d.action = TitrationAction::Done;
    d.reason = TitrationStopReason::TargetReached;
    return d;
  }

  const bool steep = dyn.isSteep(settings.endpoint);
  const float farError = settings.endpoint == ControlEndpoint::Millivolts ? 100.0f : 1.0f;
  const float mediumError = settings.endpoint == ControlEndpoint::Millivolts ? 30.0f : 0.3f;
  const float nearError = settings.endpoint == ControlEndpoint::Millivolts ? 10.0f : 0.1f;
  if (steep) {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 25;
    d.settleMs = 15000;
  } else if (error > farError) {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 450;
    d.settleMs = 5000;
  } else if (error > mediumError) {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 150;
    d.settleMs = 8000;
  } else if (error > nearError) {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 60;
    d.settleMs = 12000;
  } else {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 25;
    d.settleMs = 15000;
  }
  return d;
}

#endif
