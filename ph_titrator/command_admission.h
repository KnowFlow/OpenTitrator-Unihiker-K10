#pragma once

#include <stdint.h>

enum class WebCommand : uint8_t {
  EmergencyStop,
  Start,
  StartExisting,
  Pause,
  Reset,
  Tare,
  EnterReady,
  CalibratePumps,
  ResetSignalFilter,
  ManualTitrant,
  ManualSample,
  ManualSweep,
  ManualStop,
  SaveMethodSettings,
  SaveCalibration,
  SaveWifi,
  OtaUpload,
  Login,
  Logout,
  Recover
};

enum class AdmissionRunState : uint8_t { Idle, Active, Calibrating };

struct AdmissionContext {
  bool authenticated;
  bool otaSafetyLock;
  bool otaInProgress;
  bool runActive;
  bool calibrating;
};

enum class AdmissionResult : uint8_t {
  Allowed,
  AuthenticationRequired,
  OtaLocked,
  InvalidState
};

inline AdmissionResult admitWebCommand(WebCommand command,
                                       const AdmissionContext &context) {
  switch (command) {
    case WebCommand::EmergencyStop:
      return AdmissionResult::Allowed;

    case WebCommand::Login:
    case WebCommand::Recover:
      return (context.otaSafetyLock || context.otaInProgress)
                 ? AdmissionResult::OtaLocked
                 : AdmissionResult::Allowed;

    case WebCommand::OtaUpload:
      if (!context.authenticated)
        return AdmissionResult::AuthenticationRequired;
      return (context.otaSafetyLock || context.otaInProgress)
                 ? AdmissionResult::OtaLocked
                 : AdmissionResult::Allowed;

    case WebCommand::SaveMethodSettings:
    case WebCommand::SaveCalibration:
    case WebCommand::SaveWifi:
      if (!context.authenticated)
        return AdmissionResult::AuthenticationRequired;
      return (context.otaSafetyLock || context.otaInProgress)
                 ? AdmissionResult::OtaLocked
                 : AdmissionResult::Allowed;

    case WebCommand::EnterReady:
    case WebCommand::CalibratePumps:
    case WebCommand::ResetSignalFilter:
    case WebCommand::ManualTitrant:
    case WebCommand::ManualSample:
    case WebCommand::ManualSweep:
    case WebCommand::ManualStop:
      if (!context.authenticated)
        return AdmissionResult::AuthenticationRequired;
      return (context.runActive || context.calibrating)
                 ? AdmissionResult::InvalidState
                 : AdmissionResult::Allowed;

    case WebCommand::Start:
    case WebCommand::StartExisting:
    case WebCommand::Pause:
    case WebCommand::Reset:
    case WebCommand::Tare:
    case WebCommand::Logout:
      return context.authenticated ? AdmissionResult::Allowed
                                   : AdmissionResult::AuthenticationRequired;
  }

  return AdmissionResult::InvalidState;
}
