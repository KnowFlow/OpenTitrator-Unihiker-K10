#ifndef PH_TITRATOR_RUN_ENGINE_H
#define PH_TITRATOR_RUN_ENGINE_H

#include "run_types.h"

class RunEngine {
public:
  RunEngine();

  RunOutput step(const RunInput &input);
  void reset();
  RunPhase phase() const;

private:
  RunPhase phase_;
  RunStopReason stopReason_;
  RunPhase pausedPhase_;
  uint32_t fillStartedAtMs_;
  uint32_t lastSampleProgressAtMs_;
  float lastSampleProgressGrams_;
  bool reStabilizingAfterResume_;
};

#endif
