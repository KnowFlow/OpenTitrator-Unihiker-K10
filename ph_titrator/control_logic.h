#ifndef PH_TITRATOR_CONTROL_LOGIC_H
#define PH_TITRATOR_CONTROL_LOGIC_H

#include <stdint.h>

enum class TitrationMode : uint8_t {
  AddBase,
  AddAcid
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
  InvalidReading
};

struct TitrationSettings {
  TitrationMode mode = TitrationMode::AddBase;
  float targetPh = 7.00f;
  float tolerancePh = 0.05f;
  float maxConsumedGrams = 75.0f;
};

struct TitrationDecision {
  TitrationAction action = TitrationAction::Done;
  TitrationStopReason reason = TitrationStopReason::None;
  uint16_t pumpPulseMs = 0;
  uint16_t settleMs = 0;
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

inline float computeProbeMillivoltsFromAdsInput(float adsInputMillivolts) {
  constexpr float frontEndGain = 3.0f;
  constexpr float frontEndOffsetMillivolts = 1500.0f;
  constexpr float probeZeroMillivolts = 5547.0f;
  return (adsInputMillivolts * frontEndGain) + frontEndOffsetMillivolts - probeZeroMillivolts;
}

inline float computePhFromProbeMillivolts(float probeMillivolts) {
  constexpr float neutralPh = 7.0f;
  constexpr float millivoltsPerPh = 58.0f / 1.11f;
  return neutralPh - (probeMillivolts / millivoltsPerPh);
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
