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

  bool isSettled(float maxAbsDpHPerSecond = 0.005f) const {
    return count >= 3 && absoluteFloat(dpH_dt()) <= maxAbsDpHPerSecond;
  }

  bool isOvershooting(TitrationMode mode, float target) const {
    if (count < 2) {
      return false;
    }
    float d = dpH_dt();
    uint8_t i = (idx + N - 1) % N;
    if (mode == TitrationMode::AddBase) {
      return ph[i] > target + 0.02f && d > 0.0f;
    } else {
      return ph[i] < target - 0.02f && d < 0.0f;
    }
  }
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
    float currentPh,
    float consumedGrams,
    const TitrationDynamics &dyn) {
  TitrationDecision d;

  if (!isValidPh(currentPh)) {
    d.action = TitrationAction::Error;
    d.reason = TitrationStopReason::InvalidReading;
    return d;
  }

  if (consumedGrams >= settings.maxConsumedGrams) {
    d.action = TitrationAction::Error;
    d.reason = TitrationStopReason::MassLimit;
    return d;
  }

  const bool shouldAddBase = settings.mode == TitrationMode::AddBase && currentPh < settings.targetPh;
  const bool shouldAddAcid = settings.mode == TitrationMode::AddAcid && currentPh > settings.targetPh;
  if (!shouldAddBase && !shouldAddAcid) {
    d.action = TitrationAction::Done;
    d.reason = TitrationStopReason::TargetReached;
    return d;
  }

  if (dyn.isOvershooting(settings.mode, settings.targetPh)) {
    d.action = TitrationAction::Done;
    d.reason = TitrationStopReason::TargetReached;
    return d;
  }

  const float error = absoluteFloat(currentPh - settings.targetPh);
  if (error <= 0.05f) {
    d.action = TitrationAction::Done;
    d.reason = TitrationStopReason::TargetReached;
    return d;
  }

  const bool steep = dyn.isSteep();
  if (steep) {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 25;
    d.settleMs = 15000;
  } else if (error > 1.0f) {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 300;
    d.settleMs = 6000;
  } else if (error > 0.3f) {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 100;
    d.settleMs = 10000;
  } else {
    d.action = TitrationAction::Dose;
    d.pumpPulseMs = 40;
    d.settleMs = 15000;
  }
  return d;
}

#endif
