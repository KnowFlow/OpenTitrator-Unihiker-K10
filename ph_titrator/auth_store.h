#pragma once
#include "auth_core.h"

class AuthStore {
public:
  bool loadAdministrator(AuthCredential &credential);
  bool saveAdministrator(const AuthCredential &credential);
  bool loadFactory(AuthCredential &credential);
  bool loadRecoveryAdministrator(AuthCredential &credential);
};
