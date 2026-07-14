#if defined(AUTH_USB_RECOVERY)

#include <Arduino.h>
#include "auth_store.h"

namespace {
AuthStore authStore;
}

void setup() {
  Serial.begin(115200);
  delay(250);
  AuthCredential credential;
  if (!authStore.loadRecoveryAdministrator(credential) ||
      !authStore.saveAdministrator(credential)) {
    Serial.println("AUTH_RECOVERY:FAIL");
    return;
  }
  Serial.println("AUTH_RECOVERY:OK");
}

void loop() { delay(1000); }

#endif
