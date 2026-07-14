#if !defined(AUTH_USB_RECOVERY)

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <Preferences.h>
#include <ESP32Servo.h>
#include "unihiker_k10.h"
#include "control_logic.h"
#include "auth_core.h"
#include "auth_crypto_esp32.h"
#include "auth_store.h"
#include "command_admission.h"
#include "run_engine.h"

struct SettingsCandidate;
class PumpController;

UNIHIKER_K10 k10;
Servo titrantPumpServo;
Servo samplePumpServo;
WebServer server(80);
Preferences preferences;
Esp32AuthCrypto authCrypto;
AuthStore authStore;
AuthManager authManager(authCrypto);
bool authStorageReady = false;
bool otaUploadStartSeen = false;
bool otaRequestAccepted = false;
uint8_t otaSessionSlot = 0;
int otaRejectedStatus = 0;

const uint8_t SCREEN_DIR = 2;
const uint8_t ADS1115_ADDR = 0x49;
const uint8_t SCALE_ADDR = 0x64;
const uint8_t PH_ADC_CHANNEL = 1;
const float DEFAULT_TEMPERATURE_C = 25.0f;
const uint32_t ADC_CONVERSION_MS = 12;
const uint32_t SCALE_SETTLE_MS = 60;
const uint32_t I2C_READ_MS = 22;
const uint32_t BUTTON_LONG_PRESS_MS = 1200;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 8000;
const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
const uint32_t RESTART_DELAY_MS = 1200;

const int TITRANT_PUMP_PIN = P0;
const int SAMPLE_PUMP_PIN = P1;
const int PUMP_STOP_US = 1500;
const int PUMP_MIN_RUN_US = 1000;
const int PUMP_MAX_RUN_US = 1500;
const int TITRANT_PUMP_DEFAULT_RUN_US = 1000;
const int SAMPLE_PUMP_DEFAULT_RUN_US = 1000;
const int TITRANT_DOSE_MIN_PERCENT = 5;
const int TITRANT_DOSE_MAX_PERCENT = 100;
const int TITRANT_DOSE_DEFAULT_PERCENT = 100;
const uint16_t TITRANT_MIN_PULSE_MS = 5;
const uint16_t TITRANT_BURST_ON_DEFAULT_MS = 5;
const uint16_t TITRANT_BURST_OFF_DEFAULT_MS = 100;
const uint16_t TITRANT_BURST_ON_MIN_MS = 1;
const uint16_t TITRANT_BURST_ON_MAX_MS = 1000;
const uint16_t TITRANT_BURST_OFF_MAX_MS = 5000;
const uint32_t SAMPLE_INTERVAL_MS = 2000;
const uint32_t SCALE_SAMPLE_FILL_INTERVAL_MS = 250;
const uint32_t SCALE_SAMPLE_INTERVAL_MS = 500;
const uint32_t SETTLING_TIME_MS = 5000;
const uint32_t MAX_SETTLING_TIME_MS = 60000;
const uint32_t PUMP_DUTY_CYCLE_MS = 5000;
const uint32_t FINE_PULSE_RUN_MS = 500;
const uint32_t CALIBRATION_PREP_MS = 2000;
const uint32_t CALIBRATION_PUMP_RUN_MS = 2000;
const uint32_t CALIBRATION_SETTLE_MS = 5000;
const uint8_t SENSOR_FAULT_LIMIT = 10;
const uint8_t SCALE_FILTER_WINDOW = 5;
const float SCALE_ACTIVE_JUMP_LIMIT_G = 4.0f;
const float SCALE_IDLE_JUMP_LIMIT_G = 2.0f;
const float SCALE_IDLE_ACCEPT_STABILITY_G = 0.8f;
const uint8_t SCALE_IDLE_ACCEPT_COUNT = 3;
const uint32_t SAMPLE_PROGRESS_TIMEOUT_MS = 12000;
const uint32_t SAMPLE_FILL_EXTRA_MS = 5000;
const float STOICH_PREDOSE_RATIO = 0.70f;
const float STOICH_PREDOSE_STEP_G = 2.0f;
const uint16_t STOICH_PREDOSE_FALLBACK_PULSE_MS = 2000;
const uint16_t STOICH_PREDOSE_MAX_PULSE_MS = 5000;
const uint16_t STOICH_PREDOSE_SETTLE_MS = 6000;

const char *AP_SSID = "K10-pH-Titrator";
const char *AP_PASSWORD = "12345678";
const char *OTA_HOSTNAME = "k10-ph-titrator";
const char *OTA_PASSWORD = "k10ph";

const uint32_t COLOR_BG = 0x000000;
const uint32_t COLOR_TEXT = 0xEAF7FF;
const uint32_t COLOR_MUTED = 0x7BB2CC;
const uint32_t COLOR_OK = 0x48E27B;
const uint32_t COLOR_WARN = 0xFFD24A;
const uint32_t COLOR_ERROR = 0xFF4D4D;
const uint32_t COLOR_PANEL = 0x000000;
const uint32_t COLOR_CARD = 0x000000;
const uint32_t COLOR_LINE = 0x235066;

enum class RunState : uint8_t {
  SetupMode,
  SetupTarget,
  SetupReady,
  Calibrating,
  SampleFilling,
  FilterWarmup,
  Running,
  Dosing,
  Settling,
  Paused,
  Done,
  Error
};

enum class ButtonEvent : uint8_t {
  None,
  A,
  B,
  ABShort,
  ABLong
};

struct PhReading {
  float millivolts = 0.0f;
  float ph = 7.0f;
  int16_t raw = 0;
  bool adcOk = false;
  bool ok = false;
};

struct ScaleReading {
  float grams = 0.0f;
  float rawGrams = 0.0f;
  bool filtered = false;
  bool rejected = false;
  bool ok = false;
};

struct PhCalibration {
  float lowAdsMillivolts = 1329.3334f;
  float lowProbeMillivolts = -59.0f;
  float lowPh = 8.11f;
  float highAdsMillivolts = 2387.3333f;
  float highProbeMillivolts = 296.0f;
  float highPh = 2.14f;
};

PhCalibration phCalibration;

bool devicePresent(uint8_t address);

extern bool otaReady;
extern bool webReady;

class Ads1115PhSensor {
public:
  bool begin() {
    return devicePresent(ADS1115_ADDR);
  }

  PhReading read() {
    PhReading reading;
    int16_t raw = readSingleEnded(PH_ADC_CHANNEL);
    if (raw == INT16_MIN) {
      return reading;
    }

    reading.raw = raw;
    reading.adcOk = true;
    reading.millivolts = computeProbeMillivoltsFromAdsInput(
        raw * 0.125f,
        phCalibration.lowAdsMillivolts,
        phCalibration.lowProbeMillivolts,
        phCalibration.highAdsMillivolts,
        phCalibration.highProbeMillivolts);
    reading.ph = smoothPh(computePhFromProbeMillivolts(
        reading.millivolts,
        phCalibration.lowProbeMillivolts,
        phCalibration.lowPh,
        phCalibration.highProbeMillivolts,
        phCalibration.highPh));
    reading.ok = isValidPh(reading.ph);
    return reading;
  }

private:
  float filteredPh = 7.0f;
  bool hasFilter = false;

  int16_t readSingleEnded(uint8_t channel) {
    if (channel > 3) {
      return INT16_MIN;
    }

    uint16_t mux = 0x4000 + (channel * 0x1000);
    uint16_t config = 0x8000 | mux | 0x0200 | 0x0100 | 0x0080 | 0x0003;

    Wire.beginTransmission(ADS1115_ADDR);
    Wire.write(0x01);
    Wire.write(config >> 8);
    Wire.write(config & 0xFF);
    if (Wire.endTransmission() != 0) {
      return INT16_MIN;
    }

    uint32_t adcWait = millis();
    while (millis() - adcWait < ADC_CONVERSION_MS) {
      if (otaReady) ArduinoOTA.handle();
      if (webReady) server.handleClient();
    }
    Wire.beginTransmission(ADS1115_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission() != 0) {
      return INT16_MIN;
    }

    if (Wire.requestFrom((int)ADS1115_ADDR, 2) != 2) {
      return INT16_MIN;
    }

    uint16_t value = ((uint16_t)Wire.read() << 8) | Wire.read();
    return (int16_t)value;
  }

  // First-stage EMA filter inside the sensor driver (0.75/0.25) to
  // suppress single-sample ADC spikes before they reach the control loop.
  float smoothPh(float ph) {
    if (!hasFilter) {
      filteredPh = ph;
      hasFilter = true;
    } else {
      filteredPh = (filteredPh * 0.75f) + (ph * 0.25f);
    }
    return filteredPh;
  }
};

class ScaleSensor {
public:
  bool begin() {
    if (!devicePresent(SCALE_ADDR)) {
      return false;
    }

    Wire.beginTransmission(SCALE_ADDR);
    Wire.write(0x70);
    Wire.write(0x65);
    if (Wire.endTransmission() != 0) {
      return false;
    }

    uint32_t scaleWait = millis();
    while (millis() - scaleWait < SCALE_SETTLE_MS) {
      if (otaReady) ArduinoOTA.handle();
      if (webReady) server.handleClient();
    }
    offset = averageRaw(8);
    if (offset == 0) {
      return false;
    }

    float storedCalibration = readCalibration();
    if (storedCalibration > 0.0f && storedCalibration < 100000.0f) {
      calibration = storedCalibration;
    }
    return true;
  }

  void peel() {
    offset = averageRaw(10);
    uint8_t data = 0;
    writeRegister(0x73, &data, 1);
  }

  float calibrationFactor() const {
    return calibration;
  }

  void setCalibrationFactor(float value) {
    if (value > 0.0f && value < 100000.0f) {
      calibration = value;
    }
  }

  ScaleReading read() {
    ScaleReading reading;
    long value = averageRaw(6);
    if (value == 0) {
      return reading;
    }

    uint8_t flag = peelFlag();
    if (flag == 1) {
      offset = averageRaw(6);
    } else if (flag == 2) {
      float storedCalibration = readCalibration();
      if (storedCalibration > 0.0f && storedCalibration < 100000.0f) {
        calibration = storedCalibration;
      }
    }

    reading.grams = ((float)value - (float)offset) / calibration;
    reading.rawGrams = reading.grams;
    reading.ok = true;
    return reading;
  }

private:
  long offset = 0;
  float calibration = 2236.0f;

  long averageRaw(uint8_t times) {
    long sum = 0;
    uint8_t good = 0;
    for (uint8_t i = 0; i < times; i++) {
      long value = rawValue();
      if (value != 0) {
        sum += value;
        good++;
      }
    }
    return good == 0 ? 0 : sum / good;
  }

  long rawValue() {
    uint8_t data[4] = {0, 0, 0, 0};
    if (!readRegister(0x66, data, 4) || data[0] != 0x12) {
      return 0;
    }

    long value = data[1];
    value = (value << 8) | data[2];
    value = (value << 8) | data[3];
    return value ^ 0x800000;
  }

  uint8_t peelFlag() {
    uint8_t data = 0;
    if (!readRegister(0x69, &data, 1)) {
      return 0;
    }
    return data;
  }

  float readCalibration() {
    uint8_t data[4] = {0, 0, 0, 0};
    uint32_t value = 0;
    if (!readRegister(0x67, data, 4)) {
      return calibration;
    }

    value = data[0];
    value = (value << 8) | data[1];
    value = (value << 8) | data[2];
    value = (value << 8) | data[3];

    float result;
    memcpy(&result, &value, sizeof(result));
    return result;
  }

  bool readRegister(uint8_t reg, uint8_t *data, size_t size) {
    Wire.beginTransmission(SCALE_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission() != 0) {
      return false;
    }

    uint32_t i2cWait = millis();
    while (millis() - i2cWait < I2C_READ_MS) {
      if (otaReady) ArduinoOTA.handle();
    }
    if (Wire.requestFrom((int)SCALE_ADDR, (int)size) != (int)size) {
      return false;
    }

    for (size_t i = 0; i < size; i++) {
      data[i] = Wire.read();
    }
    return true;
  }

  bool writeRegister(uint8_t reg, const uint8_t *data, size_t size) {
    Wire.beginTransmission(SCALE_ADDR);
    Wire.write(reg);
    for (size_t i = 0; i < size; i++) {
      Wire.write(data[i]);
    }
    return Wire.endTransmission() == 0;
  }
};

class ScaleFilter {
public:
  void reset() {
    count = 0;
    idx = 0;
    filteredGrams = 0.0f;
    pendingJumpGrams = 0.0f;
    pendingJumpCount = 0;
    hasFiltered = false;
    rejected = false;
  }

  void reset(float grams) {
    reset();
    buffer[0] = grams;
    count = 1;
    idx = 1;
    filteredGrams = grams;
    hasFiltered = true;
  }

  ScaleReading apply(const ScaleReading &raw, bool active) {
    ScaleReading out = raw;
    if (!raw.ok) {
      return out;
    }

    out.rawGrams = raw.grams;
    rejected = false;
    if (hasFiltered) {
      float jumpLimit = active ? SCALE_ACTIVE_JUMP_LIMIT_G : SCALE_IDLE_JUMP_LIMIT_G;
      if (absoluteFloat(raw.grams - filteredGrams) > jumpLimit) {
        if (!active && acceptIdleJump(raw.grams)) {
          reset(raw.grams);
          out.grams = filteredGrams;
          out.filtered = true;
          out.rejected = false;
          out.ok = true;
          return out;
        }
        rejected = true;
        out.grams = filteredGrams;
        out.filtered = true;
        out.rejected = true;
        out.ok = true;
        return out;
      }
    }

    pendingJumpGrams = 0.0f;
    pendingJumpCount = 0;
    buffer[idx] = raw.grams;
    idx = (idx + 1) % SCALE_FILTER_WINDOW;
    if (count < SCALE_FILTER_WINDOW) {
      count++;
    }

    filteredGrams = trimmedMean();
    hasFiltered = true;
    out.grams = filteredGrams;
    out.filtered = true;
    out.rejected = false;
    return out;
  }

  bool rejectedLast() const {
    return rejected;
  }

private:
  float buffer[SCALE_FILTER_WINDOW] = {};
  uint8_t count = 0;
  uint8_t idx = 0;
  float filteredGrams = 0.0f;
  float pendingJumpGrams = 0.0f;
  uint8_t pendingJumpCount = 0;
  bool hasFiltered = false;
  bool rejected = false;

  bool acceptIdleJump(float grams) {
    if (pendingJumpCount == 0 || absoluteFloat(grams - pendingJumpGrams) > SCALE_IDLE_ACCEPT_STABILITY_G) {
      pendingJumpGrams = grams;
      pendingJumpCount = 1;
    } else if (pendingJumpCount < SCALE_IDLE_ACCEPT_COUNT) {
      pendingJumpCount++;
      pendingJumpGrams = (pendingJumpGrams + grams) * 0.5f;
    }
    return pendingJumpCount >= SCALE_IDLE_ACCEPT_COUNT;
  }

  float trimmedMean() const {
    if (count == 0) {
      return 0.0f;
    }
    float temp[SCALE_FILTER_WINDOW] = {};
    for (uint8_t i = 0; i < count; i++) {
      temp[i] = buffer[i];
    }
    for (uint8_t i = 1; i < count; i++) {
      float key = temp[i];
      int8_t j = i - 1;
      while (j >= 0 && temp[j] > key) {
        temp[j + 1] = temp[j];
        j--;
      }
      temp[j + 1] = key;
    }
    uint8_t start = count >= 5 ? 1 : 0;
    uint8_t end = count >= 5 ? count - 1 : count;
    float sum = 0.0f;
    uint8_t n = 0;
    for (uint8_t i = start; i < end; i++) {
      sum += temp[i];
      n++;
    }
    return n == 0 ? temp[0] : sum / n;
  }
};

int constrainPumpRunUs(int value) {
  return constrain(value, PUMP_MIN_RUN_US, PUMP_MAX_RUN_US);
}

int constrainTitrantDosePercent(int value) {
  return constrain(value, TITRANT_DOSE_MIN_PERCENT, TITRANT_DOSE_MAX_PERCENT);
}

uint16_t constrainTitrantBurstOnMs(int value) {
  return (uint16_t)constrain(value, TITRANT_BURST_ON_MIN_MS, TITRANT_BURST_ON_MAX_MS);
}

uint16_t constrainTitrantBurstOffMs(int value) {
  return (uint16_t)constrain(value, 0, TITRANT_BURST_OFF_MAX_MS);
}

uint16_t scaleTitrantPulseMs(uint16_t pulseMs, int percent) {
  if (pulseMs == 0) {
    return 0;
  }
  uint32_t scaled = ((uint32_t)pulseMs * (uint32_t)constrainTitrantDosePercent(percent) + 50UL) / 100UL;
  if (scaled < TITRANT_MIN_PULSE_MS) {
    scaled = TITRANT_MIN_PULSE_MS;
  }
  if (scaled > UINT16_MAX) {
    scaled = UINT16_MAX;
  }
  return (uint16_t)scaled;
}

class PumpController {
public:
  void begin(Servo &servoRef, int pin, int runPulseUs) {
    servo = &servoRef;
    servo->setPeriodHertz(50);
    servo->attach(pin, 500, 2500);
    setRunPulseUs(runPulseUs);
    stop();
  }

  void setRunPulseUs(int pulseUs) {
    runUs = constrainPumpRunUs(pulseUs);
  }

  int runPulseUs() const {
    return runUs;
  }

  void stop() {
    runUntilMs = 0;
    burstMode = false;
    outputRunning = false;
    writeStop();
  }

  void runForMs(uint16_t ms) {
    burstMode = false;
    runUntilMs = millis() + ms;
    writeRun();
  }

  void runForMsAtUs(uint16_t ms, int pulseUs) {
    burstMode = false;
    runUntilMs = millis() + ms;
    writeRun(constrainPumpRunUs(pulseUs));
  }

  void runForMsAtUsBurst(uint16_t ms, int pulseUs, uint16_t onMs, uint16_t offMs) {
    uint32_t now = millis();
    runUntilMs = now + ms;
    burstCycleStartedMs = now;
    burstOnMs = onMs;
    burstOffMs = offMs;
    burstRunUs = constrainPumpRunUs(pulseUs);
    burstMode = offMs > 0;
    outputRunning = false;
    updateBurstOutput();
  }

  void runContinuous() {
    burstMode = false;
    runUntilMs = millis() + 30000; // 30s max safety
    writeRun();
  }

  void runContinuousAtUs(int pulseUs) {
    burstMode = false;
    runUntilMs = millis() + 30000; // 30s max safety
    writeRun(constrainPumpRunUs(pulseUs));
  }

  void update() {
    uint32_t now = millis();
    if (deadlineReached(now, runUntilMs)) {
      stop();
      return;
    }
    if (burstMode && isRunning()) {
      updateBurstOutput();
    }
  }

  bool isRunning() const {
    uint32_t now = millis();
    return runUntilMs != 0U && !deadlineReached(now, runUntilMs);
  }

private:
  Servo *servo = nullptr;
  uint32_t runUntilMs = 0;
  uint32_t burstCycleStartedMs = 0;
  int runUs = PUMP_STOP_US;
  int burstRunUs = PUMP_STOP_US;
  uint16_t burstOnMs = 0;
  uint16_t burstOffMs = 0;
  bool burstMode = false;
  bool outputRunning = false;

  void updateBurstOutput() {
    if (!burstMode || burstOffMs == 0) {
      writeRun(burstRunUs);
      outputRunning = true;
      return;
    }
    uint32_t cycleMs = (uint32_t)burstOnMs + (uint32_t)burstOffMs;
    if (cycleMs == 0) {
      writeStop();
      outputRunning = false;
      return;
    }
    uint32_t elapsed = (millis() - burstCycleStartedMs) % cycleMs;
    bool shouldRun = elapsed < burstOnMs;
    if (shouldRun && !outputRunning) {
      writeRun(burstRunUs);
      outputRunning = true;
    } else if (!shouldRun && outputRunning) {
      writeStop();
      outputRunning = false;
    }
  }

  void writeStop() {
    if (servo != nullptr) {
      servo->writeMicroseconds(PUMP_STOP_US);
    }
  }

  void writeRun() {
    writeRun(runUs);
  }

  void writeRun(int pulseUs) {
    if (servo != nullptr) {
      servo->writeMicroseconds(constrainPumpRunUs(pulseUs));
    }
  }
};

Ads1115PhSensor phSensor;
ScaleSensor scaleSensor;
ScaleFilter scaleFilter;
PumpController pump;
PumpController samplePump;

TitrationSettings settings;
TitrationMethod currentMethod = TitrationMethod::PhEndpoint;
RunState state = RunState::SetupMode;
TitrationStopReason stopReason = TitrationStopReason::None;

PhReading lastPh;
ScaleReading lastScale;
PhFilter phFilter;
RunEngine runEngine;
RunOutput lastRunOutput;
float titrantPumpFlowRateGps = 0.0f;
float samplePumpFlowRateGps = 0.0f;
int titrantPumpRunUs = TITRANT_PUMP_DEFAULT_RUN_US;
int samplePumpRunUs = SAMPLE_PUMP_DEFAULT_RUN_US;
int titrantDosePercent = TITRANT_DOSE_DEFAULT_PERCENT;
uint16_t titrantBurstOnMs = TITRANT_BURST_ON_DEFAULT_MS;
uint16_t titrantBurstOffMs = TITRANT_BURST_OFF_DEFAULT_MS;
bool manualSweepActive = false;
bool manualSweepTitrant = true;
int manualSweepStartUs = TITRANT_PUMP_DEFAULT_RUN_US;
int manualSweepEndUs = PUMP_MAX_RUN_US;
int manualSweepLastUs = 0;
uint32_t manualSweepStartedMs = 0;
uint32_t manualSweepDurationMs = 10000;
uint32_t manualSweepLastCommandMs = 0;
float sampleStartWeight = 0.0f;
float sampleDeliveredGrams = 0.0f;
float consumedGrams = 0.0f;
float endpointUsedGrams = 0.0f;
float resultValue = 0.0f;
uint32_t stateStartedMs = 0;
uint32_t displayedPulseMs = 0;
uint8_t sensorFaultCount = 0;
bool phReady = false;
bool scaleReady = false;
bool phSampleFresh = false;
bool sensorFault = false;
bool displayDirty = true;
bool webReady = false;
bool otaReady = false;
bool httpOtaSafetyLock = false;
bool httpOtaInProgress = false;
bool httpOtaSucceeded = false;
bool restartPending = false;
uint32_t restartAtMs = 0;
bool calibrationNeedsReset = false;
String statusLine = "Booting";
String networkLabel = "AP";
String ipAddress = "0.0.0.0";
String apIpAddress = "0.0.0.0";
String staIpAddress = "0.0.0.0";
String wifiSsid = "";
String wifiPassword = "";

struct MethodAuxValues {
  float titrantMolarity = 0.01f;
  float blankGrams = 0.0f;
  float titrantDensityGramsPerMl = 1.0f;
  float sampleDensityGramsPerMl = 1.0f;
  float manualResultFactor = 1.0f;
};

MethodAuxValues defaultMethodAux(TitrationMethod method);
MethodAuxValues loadMethodAux(TitrationMethod method);
void applyMethodAux(TitrationSettings &targetSettings, const MethodAuxValues &aux);
void loadMethodAuxIntoSettings(TitrationMethod method);

bool devicePresent(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void setState(RunState next, const String &status) {
  state = next;
  statusLine = status;
  stateStartedMs = millis();
  displayDirty = true;
}

int currentManualSweepUs() {
  if (!manualSweepActive || manualSweepDurationMs == 0) {
    return manualSweepLastUs > 0 ? manualSweepLastUs : manualSweepStartUs;
  }
  uint32_t elapsed = millis() - manualSweepStartedMs;
  if (elapsed >= manualSweepDurationMs) {
    return manualSweepEndUs;
  }
  long delta = (long)manualSweepEndUs - (long)manualSweepStartUs;
  long current = (long)manualSweepStartUs + (delta * (long)elapsed) / (long)manualSweepDurationMs;
  return constrainPumpRunUs((int)current);
}

void stopManualSweep(bool stopPumps) {
  manualSweepActive = false;
  manualSweepLastCommandMs = 0;
  if (stopPumps) {
    pump.stop();
    samplePump.stop();
  }
}

void startManualSweep(bool titrant, int startUs, int endUs, uint16_t seconds) {
  if (httpOtaSafetyLock) {
    statusLine = "OTA locked";
    displayDirty = true;
    return;
  }
  manualSweepActive = true;
  manualSweepTitrant = titrant;
  manualSweepStartUs = constrainPumpRunUs(startUs);
  manualSweepEndUs = constrainPumpRunUs(endUs);
  manualSweepStartedMs = millis();
  manualSweepDurationMs = (uint32_t)constrain(seconds, 1, 120) * 1000UL;
  manualSweepLastUs = 0;
  manualSweepLastCommandMs = 0;
  if (manualSweepTitrant) {
    samplePump.stop();
  } else {
    pump.stop();
  }
  statusLine = String("Sweep ") + (manualSweepTitrant ? "titrant" : "sample");
  displayDirty = true;
}

void updateManualSweep() {
  if (!manualSweepActive) {
    return;
  }
  uint32_t now = millis();
  int currentUs = currentManualSweepUs();
  if (currentUs != manualSweepLastUs || now - manualSweepLastCommandMs >= 1000UL) {
    if (manualSweepTitrant) {
      pump.runContinuousAtUs(currentUs);
    } else {
      samplePump.runContinuousAtUs(currentUs);
    }
    manualSweepLastUs = currentUs;
    manualSweepLastCommandMs = now;
    statusLine = String("Sweep ") + (manualSweepTitrant ? "titrant " : "sample ") + String(currentUs) + "us";
    displayDirty = true;
  }
  if (now - manualSweepStartedMs >= manualSweepDurationMs) {
    stopManualSweep(true);
    statusLine = String("Sweep done @ ") + String(currentUs) + "us";
    displayDirty = true;
  }
}

void updatePumpTimeouts() {
  updateManualSweep();
  pump.update();
  samplePump.update();
}

const char *modeText() {
  return settings.mode == TitrationMode::AddBase ? "BASE" : "ACID";
}

const char *trendText() {
  return settings.controlTrend == ControlTrend::Increase ? "RISE" : "FALL";
}

String modeLabel() {
  return trendText();
}

const char *endpointText() {
  return settings.endpoint == ControlEndpoint::Millivolts ? "mV" : "pH";
}

float activeTitrantMolarity();

float activeControlValue() {
  return settings.endpoint == ControlEndpoint::Millivolts ? lastPh.millivolts : lastPh.ph;
}

float activeControlTarget() {
  return controlTarget(settings);
}

bool autoEqpEnabled() {
  return currentMethod == TitrationMethod::EdtaHardness &&
         settings.endpoint == ControlEndpoint::Millivolts &&
         settings.resultFormula == ResultFormula::EdtaHardnessCaCO3;
}

float resultConsumedGrams() {
  if (endpointUsedGrams > 0.0f) {
    return endpointUsedGrams;
  }
  return consumedGrams;
}

float computeCurrentResult() {
  return computeTitrationResult(settings, activeTitrantMolarity(), resultConsumedGrams(), settings.sampleGrams);
}

bool acidBaseStoichPredoseAllowed() {
  return settings.endpoint == ControlEndpoint::Ph &&
         settings.resultFormula == ResultFormula::AcidBaseMolar &&
         activeTitrantMolarity() > 0.0f &&
         settings.sampleGrams > 0.0f &&
         settings.titrantDensityGramsPerMl > 0.0f &&
         settings.sampleDensityGramsPerMl > 0.0f;
}

float estimateStoichTitrantGrams() {
  if (!acidBaseStoichPredoseAllowed()) {
    return 0.0f;
  }
  float sampleMilliliters = gramsToMilliliters(settings.sampleGrams, settings.sampleDensityGramsPerMl);
  if (sampleMilliliters <= 0.0f) {
    return 0.0f;
  }
  return sampleMilliliters * settings.titrantDensityGramsPerMl;
}

String titrantLabel() {
  switch (settings.titrantPreset) {
    case TitrantPreset::Naoh001: return "0.01M NaOH";
    case TitrantPreset::Hcl001: return "0.01M HCl";
    case TitrantPreset::Edta001: return "0.01M EDTA";
    case TitrantPreset::Manual: return String(settings.titrantMolarity, 4) + "M manual";
  }
  return "Unknown";
}

float activeTitrantMolarity() {
  return titrantMolarityForPreset(settings.titrantPreset, settings.titrantMolarity);
}

const char *resultFormulaValue(ResultFormula formula) {
  switch (formula) {
    case ResultFormula::AcidBaseMolar: return "acid_base_m";
    case ResultFormula::EdtaHardnessCaCO3: return "edta_hardness";
    case ResultFormula::ManualFactor: return "manual_factor";
  }
  return "acid_base_m";
}

String resultFormulaLabel() {
  switch (settings.resultFormula) {
    case ResultFormula::AcidBaseMolar: return "Acid/base mol/L";
    case ResultFormula::EdtaHardnessCaCO3: return "EDTA hardness";
    case ResultFormula::ManualFactor: return "Manual factor";
  }
  return "Acid/base mol/L";
}

String resultUnit() {
  switch (settings.resultFormula) {
    case ResultFormula::AcidBaseMolar: return "mol/L";
    case ResultFormula::EdtaHardnessCaCO3: return "mg/L as CaCO3";
    case ResultFormula::ManualFactor: return "manual";
  }
  return "mol/L";
}

float phCalibrationSlopeMvPerPh() {
  float phSpan = phCalibration.highPh - phCalibration.lowPh;
  if (absoluteFloat(phSpan) < 0.001f) {
    return 0.0f;
  }
  return (phCalibration.highProbeMillivolts - phCalibration.lowProbeMillivolts) / phSpan;
}

float phCalibrationSlopePercent() {
  const float theoreticalSlopeMvPerPh = 59.16f;
  return absoluteFloat(phCalibrationSlopeMvPerPh()) / theoreticalSlopeMvPerPh * 100.0f;
}

float phCalibrationOffsetAtPh7Mv() {
  return phCalibration.lowProbeMillivolts + (7.0f - phCalibration.lowPh) * phCalibrationSlopeMvPerPh();
}

String phCalibrationStatus() {
  float phSpan = absoluteFloat(phCalibration.highPh - phCalibration.lowPh);
  float probeSpan = absoluteFloat(phCalibration.highProbeMillivolts - phCalibration.lowProbeMillivolts);
  float adsSpan = absoluteFloat(phCalibration.highAdsMillivolts - phCalibration.lowAdsMillivolts);
  float slopePct = phCalibrationSlopePercent();
  if (phSpan < 1.0f || probeSpan < 20.0f || adsSpan < 20.0f || slopePct < 70.0f || slopePct > 120.0f) {
    return "Check sensor calibration";
  }
  if (slopePct < 85.0f || slopePct > 105.0f) {
    return "Usable, recalibrate soon";
  }
  return "OK";
}

uint8_t resultDecimals() {
  switch (settings.resultFormula) {
    case ResultFormula::AcidBaseMolar: return 5;
    case ResultFormula::EdtaHardnessCaCO3: return 1;
    case ResultFormula::ManualFactor: return 4;
  }
  return 5;
}

void resetRunData();

const char *methodValue(TitrationMethod method) {
  switch (method) {
    case TitrationMethod::PhEndpoint: return "ph_ep";
    case TitrationMethod::MvEndpoint: return "mv_ep";
    case TitrationMethod::EdtaHardness: return "edta_hardness";
    case TitrationMethod::Manual: return "manual";
  }
  return "ph_ep";
}

String methodLabel(TitrationMethod method) {
  switch (method) {
    case TitrationMethod::PhEndpoint: return "pH endpoint";
    case TitrationMethod::MvEndpoint: return "mV endpoint";
    case TitrationMethod::EdtaHardness: return "EDTA hardness";
    case TitrationMethod::Manual: return "Manual method";
  }
  return "pH endpoint";
}

TitrationMethod methodFromValue(const String &value) {
  if (value == "mv_ep") {
    return TitrationMethod::MvEndpoint;
  }
  if (value == "edta_hardness") {
    return TitrationMethod::EdtaHardness;
  }
  if (value == "manual") {
    return TitrationMethod::Manual;
  }
  return TitrationMethod::PhEndpoint;
}

const char *methodAuxPrefix(TitrationMethod method) {
  switch (method) {
    case TitrationMethod::PhEndpoint: return "ph";
    case TitrationMethod::MvEndpoint: return "mv";
    case TitrationMethod::EdtaHardness: return "edta";
    case TitrationMethod::Manual: return "manual";
  }
  return "ph";
}

String methodAuxKey(TitrationMethod method, const char *suffix) {
  String key = methodAuxPrefix(method);
  key += "_";
  key += suffix;
  return key;
}

MethodAuxValues defaultMethodAux(TitrationMethod method) {
  TitrationSettings preset;
  applyTitrationMethodPreset(preset, method);
  MethodAuxValues aux;
  aux.titrantMolarity = preset.titrantMolarity;
  aux.blankGrams = preset.blankGrams;
  aux.titrantDensityGramsPerMl = preset.titrantDensityGramsPerMl;
  aux.sampleDensityGramsPerMl = preset.sampleDensityGramsPerMl;
  aux.manualResultFactor = preset.manualResultFactor;
  return aux;
}

MethodAuxValues loadMethodAux(TitrationMethod method) {
  MethodAuxValues aux = defaultMethodAux(method);
  Preferences prefs;
  if (prefs.begin("method_aux", true)) {
    String molarityKey = methodAuxKey(method, "m");
    String blankKey = methodAuxKey(method, "blank");
    String titrantDensityKey = methodAuxKey(method, "tdens");
    String sampleDensityKey = methodAuxKey(method, "sdens");
    String factorKey = methodAuxKey(method, "factor");
    aux.titrantMolarity = prefs.getFloat(molarityKey.c_str(), aux.titrantMolarity);
    aux.blankGrams = prefs.getFloat(blankKey.c_str(), aux.blankGrams);
    aux.titrantDensityGramsPerMl = prefs.getFloat(titrantDensityKey.c_str(), aux.titrantDensityGramsPerMl);
    aux.sampleDensityGramsPerMl = prefs.getFloat(sampleDensityKey.c_str(), aux.sampleDensityGramsPerMl);
    aux.manualResultFactor = prefs.getFloat(factorKey.c_str(), aux.manualResultFactor);
    prefs.end();
  }
  aux.titrantMolarity = constrain(aux.titrantMolarity, 0.0001f, 10.0f);
  aux.blankGrams = constrain(aux.blankGrams, 0.0f, 1000.0f);
  aux.titrantDensityGramsPerMl = constrain(aux.titrantDensityGramsPerMl, 0.1f, 5.0f);
  aux.sampleDensityGramsPerMl = constrain(aux.sampleDensityGramsPerMl, 0.1f, 5.0f);
  aux.manualResultFactor = constrain(aux.manualResultFactor, -1000000.0f, 1000000.0f);
  return aux;
}

void applyMethodAux(TitrationSettings &targetSettings, const MethodAuxValues &aux) {
  targetSettings.titrantMolarity = aux.titrantMolarity;
  targetSettings.blankGrams = aux.blankGrams;
  targetSettings.titrantDensityGramsPerMl = aux.titrantDensityGramsPerMl;
  targetSettings.sampleDensityGramsPerMl = aux.sampleDensityGramsPerMl;
  targetSettings.manualResultFactor = aux.manualResultFactor;
}

void loadMethodAuxIntoSettings(TitrationMethod method) {
  applyMethodAux(settings, loadMethodAux(method));
}

void saveMethodAux(TitrationMethod method) {
  Preferences prefs;
  if (prefs.begin("method_aux", false)) {
    String molarityKey = methodAuxKey(method, "m");
    String blankKey = methodAuxKey(method, "blank");
    String titrantDensityKey = methodAuxKey(method, "tdens");
    String sampleDensityKey = methodAuxKey(method, "sdens");
    String factorKey = methodAuxKey(method, "factor");
    prefs.putFloat(molarityKey.c_str(), settings.titrantMolarity);
    prefs.putFloat(blankKey.c_str(), settings.blankGrams);
    prefs.putFloat(titrantDensityKey.c_str(), settings.titrantDensityGramsPerMl);
    prefs.putFloat(sampleDensityKey.c_str(), settings.sampleDensityGramsPerMl);
    prefs.putFloat(factorKey.c_str(), settings.manualResultFactor);
    prefs.end();
  }
}

void saveSelectedMethod() {
  if (preferences.begin("method", false)) {
    preferences.putUChar("selected", (uint8_t)currentMethod);
    preferences.end();
  }
}

void loadSelectedMethod() {
  if (preferences.begin("method", true)) {
    uint8_t stored = preferences.getUChar("selected", (uint8_t)TitrationMethod::PhEndpoint);
    preferences.end();
    if (stored <= (uint8_t)TitrationMethod::Manual) {
      currentMethod = (TitrationMethod)stored;
    }
  }
  applyTitrationMethodPreset(settings, currentMethod);
  loadMethodAuxIntoSettings(currentMethod);
}

void selectMethod(TitrationMethod method, bool persist) {
  currentMethod = method;
  applyTitrationMethodPreset(settings, currentMethod);
  loadMethodAuxIntoSettings(currentMethod);
  phFilter.reset();
  phReady = false;
  phSampleFresh = false;
  resetRunData();
  setState(RunState::SetupMode, String("Method ") + methodLabel(currentMethod));
  if (persist) {
    saveSelectedMethod();
  }
}

bool methodMatchesPreset(TitrationMethod method) {
  if (method == TitrationMethod::Manual) {
    return true;
  }
  TitrationSettings preset;
  applyTitrationMethodPreset(preset, method);
  return preset.endpoint == settings.endpoint &&
         preset.controlTrend == settings.controlTrend &&
         preset.titrantPreset == settings.titrantPreset &&
         absoluteFloat(preset.targetPh - settings.targetPh) <= 0.001f &&
         absoluteFloat(preset.targetMillivolts - settings.targetMillivolts) <= 0.001f &&
         preset.resultFormula == settings.resultFormula &&
         absoluteFloat(preset.controlBand - settings.controlBand) <= 0.001f &&
         absoluteFloat(preset.stableDelta - settings.stableDelta) <= 0.001f;
}

void appendMethodPresetJs(String &page, TitrationMethod method, bool addComma) {
  TitrationSettings preset;
  applyTitrationMethodPreset(preset, method);
  applyMethodAux(preset, loadMethodAux(method));
  page += methodValue(method);
  page += F(":{endpoint:'");
  page += preset.endpoint == ControlEndpoint::Millivolts ? F("mv") : F("ph");
  page += F("',trend:'");
  page += preset.controlTrend == ControlTrend::Decrease ? F("fall") : F("rise");
  page += F("',target:'");
  page += String(preset.targetPh, 2);
  page += F("',target_mv:'");
  page += String(preset.targetMillivolts, 0);
  page += F("',max:'");
  page += String(preset.maxConsumedGrams, 1);
  page += F("',sample:'");
  page += String(preset.sampleGrams, 1);
  page += F("',titrant:'");
  switch (preset.titrantPreset) {
    case TitrantPreset::Naoh001: page += F("naoh001"); break;
    case TitrantPreset::Hcl001: page += F("hcl001"); break;
    case TitrantPreset::Edta001: page += F("edta001"); break;
    case TitrantPreset::Manual: page += F("manual"); break;
  }
  page += F("',titrant_m:'");
  page += String(preset.titrantMolarity, 4);
  page += F("',result_formula:'");
  page += resultFormulaValue(preset.resultFormula);
  page += F("',blank_g:'");
  page += String(preset.blankGrams, 2);
  page += F("',titrant_density:'");
  page += String(preset.titrantDensityGramsPerMl, 3);
  page += F("',sample_density:'");
  page += String(preset.sampleDensityGramsPerMl, 3);
  page += F("',manual_factor:'");
  page += String(preset.manualResultFactor, 4);
  page += F("',control_band:'");
  page += String(preset.controlBand, preset.endpoint == ControlEndpoint::Millivolts ? 1 : 3);
  page += F("',stable_delta:'");
  page += String(preset.stableDelta, preset.endpoint == ControlEndpoint::Millivolts ? 1 : 3);
  page += F("',hold_s:'");
  page += String(preset.holdSeconds);
  page += F("',min_settle_s:'");
  page += String(preset.minSettleSeconds);
  page += F("',max_settle_s:'");
  page += String(preset.maxSettleSeconds);
  page += F("',max_time_s:'");
  page += String(preset.maxTimeSeconds);
  page += F("'}");
  if (addComma) {
    page += F(",");
  }
}

String stateLabel() {
  switch (state) {
    case RunState::SetupMode: return "MODE";
    case RunState::SetupTarget: return "TARGET";
    case RunState::SetupReady: return "READY";
    case RunState::SampleFilling: return "SAMPLE";
    case RunState::FilterWarmup: return "WARMUP";
    case RunState::Running: return "RUN";
    case RunState::Dosing: return "DOSE";
    case RunState::Settling: return "WAIT";
    case RunState::Paused: return "PAUSE";
    case RunState::Calibrating: return "CALIB";
    case RunState::Done: return "DONE";
    case RunState::Error: return "ERROR";
  }
  return "?";
}

String reasonLabel(TitrationStopReason reason) {
  switch (reason) {
    case TitrationStopReason::None: return "";
    case TitrationStopReason::TargetReached: return "Target reached";
    case TitrationStopReason::EquivalencePoint: return "EQP reached";
    case TitrationStopReason::MassLimit: return "Mass limit";
    case TitrationStopReason::TimeLimit: return "Time limit";
    case TitrationStopReason::InvalidReading: return "Bad pH";
    case TitrationStopReason::SensorFault: return "SENSOR_FAULT";
  }
  return "";
}

String stateText() {
  return stateLabel() + " " + statusLine;
}

void tareScale() {
  scaleSensor.peel();
  ScaleReading raw = scaleSensor.read();
  if (raw.ok) {
    scaleFilter.reset(raw.grams);
    lastScale = scaleFilter.apply(raw, false);
    scaleReady = lastScale.ok;
  } else {
    scaleFilter.reset();
  }
  statusLine = "Tare done";
  displayDirty = true;
}

void resetRunData() {
  runEngine.reset();
  lastRunOutput = RunOutput{};
  pump.stop();
  samplePump.stop();
  phFilter.reset();
  scaleFilter.reset(scaleReady ? lastScale.grams : 0.0f);
  sensorFaultCount = 0;
  sensorFault = false;
  sampleStartWeight = scaleReady ? lastScale.grams : 0.0f;
  sampleDeliveredGrams = 0.0f;
  consumedGrams = 0.0f;
  endpointUsedGrams = 0.0f;
  resultValue = 0.0f;
  displayedPulseMs = 0;
  phReady = false;
  phSampleFresh = false;
  stopReason = TitrationStopReason::None;
  displayDirty = true;
}

bool isActiveState() {
  return state == RunState::SampleFilling ||
         state == RunState::FilterWarmup ||
         state == RunState::Running ||
         state == RunState::Dosing ||
         state == RunState::Settling;
}

RunState runStateForPhase(RunPhase phase) {
  switch (phase) {
    case RunPhase::SampleFilling: return RunState::SampleFilling;
    case RunPhase::FilterWarmup: return RunState::FilterWarmup;
    case RunPhase::Running: return RunState::Running;
    case RunPhase::Dosing: return RunState::Dosing;
    case RunPhase::Settling: return RunState::Settling;
    case RunPhase::Paused: return RunState::Paused;
    case RunPhase::Done: return RunState::Done;
    case RunPhase::Error: return RunState::Error;
    case RunPhase::Inactive: return RunState::SetupMode;
  }
  return RunState::SetupMode;
}

String runStatusText(RunStatusCode status) {
  switch (status) {
    case RunStatusCode::FillingSample: return "Filling sample";
    case RunStatusCode::WaitingForStableSignal: return "Stabilizing pH";
    case RunStatusCode::ReStabilizingAfterResume: return "正在重新稳定信号";
    case RunStatusCode::CheckingEndpoint: return "Checking";
    case RunStatusCode::Dosing: return "Dosing";
    case RunStatusCode::Settling: return "Settling";
    case RunStatusCode::HoldingEndpoint: return "Holding endpoint";
    case RunStatusCode::Paused: return "Paused";
    case RunStatusCode::TargetReached: return "Target reached";
    case RunStatusCode::EquivalencePointReached: return "Equivalence point";
    case RunStatusCode::SafetyLocked: return "OTA locked";
    case RunStatusCode::SensorFault: return "Sensor missing";
    case RunStatusCode::ScaleFailure: return "Scale error";
    case RunStatusCode::SampleProgressTimeout: return "Sample scale stalled";
    case RunStatusCode::SampleFillTimeout: return "Sample timeout";
    case RunStatusCode::MassLimit: return "Mass limit";
    case RunStatusCode::TimeLimit: return "Time limit";
    case RunStatusCode::EmergencyStopped: return "Emergency stop";
    case RunStatusCode::Inactive: return "Ready";
  }
  return "Ready";
}

TitrationStopReason displayStopReason(RunStopReason reason) {
  switch (reason) {
    case RunStopReason::TargetReached: return TitrationStopReason::TargetReached;
    case RunStopReason::EquivalencePoint: return TitrationStopReason::EquivalencePoint;
    case RunStopReason::MassLimit: return TitrationStopReason::MassLimit;
    case RunStopReason::TimeLimit: return TitrationStopReason::TimeLimit;
    case RunStopReason::SensorFault: return TitrationStopReason::SensorFault;
    case RunStopReason::InvalidReading: return TitrationStopReason::InvalidReading;
    case RunStopReason::SafetyLock:
    case RunStopReason::ScaleFailure:
    case RunStopReason::SampleProgressTimeout:
    case RunStopReason::SampleFillTimeout:
    case RunStopReason::None: return TitrationStopReason::None;
  }
  return TitrationStopReason::None;
}

void applyPumpIntent(PumpController &target, const PumpIntent &intent, bool titrant) {
  if (intent.mode == PumpMode::Stop) {
    target.stop();
  } else if (intent.mode == PumpMode::RunContinuous) {
    target.runContinuous();
  } else if (intent.mode == PumpMode::RunForMs) {
    if (titrant) {
      target.runForMsAtUsBurst(intent.durationMs, titrantPumpRunUs, titrantBurstOnMs, titrantBurstOffMs);
    } else {
      target.runForMsAtUs(intent.durationMs, samplePumpRunUs);
    }
  }
}

RunInput buildRunInput(RunCommand command) {
  RunInput input{};
  input.nowMs = millis();
  input.command = command;
  input.sensor.ph = lastPh.ph;
  input.sensor.millivolts = lastPh.millivolts;
  input.sensor.controlValue = activeControlValue();
  input.sensor.reactorMassGrams = lastScale.grams;
  input.sensor.consumedTitrantGrams = consumedGrams;
  input.sensor.deliveredSampleGrams = command == RunCommand::StartExistingSample ? settings.sampleGrams : sampleDeliveredGrams;
  input.sensor.sensorValid = lastPh.adcOk && phReady && !sensorFault;
  input.sensor.sensorFresh = phSampleFresh;
  input.sensor.controlSettled = phFilter.ready();
  input.sensor.scaleValid = scaleReady;
  input.context.targetSampleGrams = settings.sampleGrams;
  input.context.samplePumpFlowRateGps = samplePumpFlowRateGps;
  input.context.titrantPumpFlowRateGps = titrantPumpFlowRateGps;
  input.context.maximumTitrantGrams = settings.maxConsumedGrams;
  input.context.maximumRunMs = (uint32_t)settings.maxTimeSeconds * 1000UL;
  input.context.settings = settings;
  input.context.automaticEqp = autoEqpEnabled();
  input.context.otaLocked = httpOtaSafetyLock;
  return input;
}

void applyRunOutput(const RunOutput &output) {
  const RunPhase previousPhase = lastRunOutput.phase;
  if (output.titrant.mode != PumpMode::Stop || output.phase != previousPhase) {
    applyPumpIntent(pump, output.titrant, true);
  }
  if (output.sample.mode != PumpMode::Stop || output.phase != previousPhase) {
    applyPumpIntent(samplePump, output.sample, false);
  }
  displayedPulseMs = output.titrant.mode == PumpMode::RunForMs ? output.titrant.durationMs : 0U;
  stopReason = displayStopReason(output.stopReason);
  if (output.finalizeResult) {
    endpointUsedGrams = output.selectedUsedTitrantGrams;
    consumedGrams = output.selectedUsedTitrantGrams;
    resultValue = computeTitrationResult(settings, activeTitrantMolarity(), endpointUsedGrams, settings.sampleGrams);
  }
  setState(runStateForPhase(output.phase), runStatusText(output.status));
  lastRunOutput = output;
}

void dispatchRunCommand(RunCommand command) {
  const bool startsRun = command == RunCommand::StartNormal || command == RunCommand::StartExistingSample;
  if (startsRun && (!lastPh.adcOk || !scaleReady || sensorFault)) {
    pump.stop();
    samplePump.stop();
    stopReason = TitrationStopReason::InvalidReading;
    setState(RunState::Error, "Sensor missing");
    return;
  }
  if (command == RunCommand::StartNormal || command == RunCommand::StartExistingSample) {
    pump.stop();
    samplePump.stop();
    phFilter.reset();
    scaleFilter.reset(lastScale.grams);
    sampleStartWeight = lastScale.grams;
    sampleDeliveredGrams = command == RunCommand::StartExistingSample ? settings.sampleGrams : 0.0f;
    consumedGrams = 0.0f;
    endpointUsedGrams = 0.0f;
    resultValue = 0.0f;
    phReady = false;
    phSampleFresh = false;
  }
  if (command == RunCommand::Reset) {
    resetRunData();
  }
  applyRunOutput(runEngine.step(buildRunInput(command)));
}

uint32_t stateColor() {
  switch (state) {
    case RunState::Running:
    case RunState::SampleFilling:
    case RunState::FilterWarmup:
    case RunState::Settling:
    case RunState::Paused:
      return COLOR_OK;
    case RunState::Dosing:
      return COLOR_WARN;
    case RunState::Done:
      return COLOR_OK;
    case RunState::Calibrating:
      return COLOR_WARN;
    case RunState::Error:
      return COLOR_ERROR;
    default:
      return COLOR_MUTED;
  }
}

String primaryHint() {
  switch (state) {
    case RunState::SetupMode: return "A/B SELECT";
    case RunState::SetupTarget: return "A-   B+";
    case RunState::SetupReady: return "A TARE";
    case RunState::Running:
    case RunState::SampleFilling:
    case RunState::FilterWarmup:
    case RunState::Dosing:
    case RunState::Settling: return "AB STOP";
    case RunState::Paused: return "AB RESUME";
    case RunState::Calibrating: return "AB CANCEL";
    case RunState::Done:
    case RunState::Error: return "AB RESET";
  }
  return "";
}

String secondaryHint() {
  switch (state) {
    case RunState::SetupMode:
    case RunState::SetupTarget:
    case RunState::SetupReady:
      return "AB NEXT";
    case RunState::Running:
    case RunState::SampleFilling:
    case RunState::FilterWarmup:
    case RunState::Dosing:
    case RunState::Settling:
    case RunState::Paused:
      return "HOLD AB PANIC";
    case RunState::Calibrating:
      return "Place bottle";
    case RunState::Done:
    case RunState::Error:
      return statusLine;
  }
  return "";
}

ButtonEvent readButtons() {
  static bool wasA = false;
  static bool wasB = false;
  static bool wasAB = false;
  static uint32_t abPressedAt = 0;
  static bool longSent = false;

  bool ab = k10.buttonAB->isPressed();
  bool a = k10.buttonA->isPressed();
  bool b = k10.buttonB->isPressed();

  // K10 hardware: AB can falsely trigger when only A or B is pressed.
  // Only treat as AB combo when both A and B are actually pressed.
  if (ab && !(a && b)) {
    ab = false;
  }

  if (ab && !wasAB) {
    abPressedAt = millis();
    longSent = false;
  }
  if (ab && !longSent && millis() - abPressedAt > BUTTON_LONG_PRESS_MS) {
    longSent = true;
    wasAB = ab;
    wasA = a;
    wasB = b;
    return ButtonEvent::ABLong;
  }
  if (!ab && wasAB && !longSent) {
    wasAB = ab;
    wasA = a;
    wasB = b;
    return ButtonEvent::ABShort;
  }

  ButtonEvent event = ButtonEvent::None;
  if (!a && wasA) {
    event = ButtonEvent::A;
  } else if (!b && wasB) {
    event = ButtonEvent::B;
  }

  wasAB = ab;
  wasA = a;
  wasB = b;
  return event;
}

void handleButton(ButtonEvent event) {
  if (event == ButtonEvent::None) {
    return;
  }

  if (httpOtaSafetyLock) {
    if (event == ButtonEvent::ABLong) {
      pump.stop();
      samplePump.stop();
      dispatchRunCommand(RunCommand::EmergencyStop);
    }
    return;
  }

  if (event == ButtonEvent::ABLong) {
    pump.stop();
    samplePump.stop();
    dispatchRunCommand(RunCommand::EmergencyStop);
    return;
  }

  switch (state) {
    case RunState::SetupMode:
      if (event == ButtonEvent::A || event == ButtonEvent::B) {
        settings.controlTrend = settings.controlTrend == ControlTrend::Increase ? ControlTrend::Decrease : ControlTrend::Increase;
        displayDirty = true;
      } else if (event == ButtonEvent::ABShort) {
        setState(RunState::SetupTarget, String("Target ") + endpointText());
      }
      break;

    case RunState::SetupTarget:
      if (event == ButtonEvent::A) {
        if (settings.endpoint == ControlEndpoint::Millivolts) {
          settings.targetMillivolts = max(-1000.0f, settings.targetMillivolts - 5.0f);
        } else {
          settings.targetPh = max(0.0f, settings.targetPh - 0.05f);
        }
        displayDirty = true;
      } else if (event == ButtonEvent::B) {
        if (settings.endpoint == ControlEndpoint::Millivolts) {
          settings.targetMillivolts = min(1000.0f, settings.targetMillivolts + 5.0f);
        } else {
          settings.targetPh = min(14.0f, settings.targetPh + 0.05f);
        }
        displayDirty = true;
      } else if (event == ButtonEvent::ABShort) {
        setState(RunState::SetupReady, "A tare, AB start");
      }
      break;

    case RunState::SetupReady:
      if (event == ButtonEvent::A) {
        tareScale();
      } else if (event == ButtonEvent::B) {
        setState(RunState::Calibrating, "Calibrating pumps");
      } else if (event == ButtonEvent::ABShort) {
        dispatchRunCommand(RunCommand::StartNormal);
      }
      break;

    case RunState::Calibrating:
      pump.stop();
      samplePump.stop();
      calibrationNeedsReset = true;
      setState(RunState::SetupReady, "Calib cancelled");
      break;

    case RunState::Paused:
      if (event == ButtonEvent::ABShort) {
        dispatchRunCommand(RunCommand::Resume);
      }
      break;

    case RunState::Done:
    case RunState::Error:
      if (event == ButtonEvent::ABShort) {
        dispatchRunCommand(RunCommand::Reset);
      }
      break;

    default:
      if (event == ButtonEvent::ABShort) {
        dispatchRunCommand(RunCommand::Pause);
      }
      break;
  }
}

void sampleSensors() {
  static uint32_t lastPhSampleMs = 0;
  static uint32_t lastScaleSampleMs = 0;
  phSampleFresh = false;

  if (millis() - lastPhSampleMs >= SAMPLE_INTERVAL_MS) {
    lastPhSampleMs = millis();
    PhReading rawPh = phSensor.read();
    if (rawPh.adcOk) {
      int analogRaw = adsRawToAnalog10Bit(rawPh.raw);
      if (isSensorFaultRaw10Bit(analogRaw)) {
        if (sensorFaultCount < SENSOR_FAULT_LIMIT) {
          sensorFaultCount++;
        }
      } else {
        sensorFaultCount = 0;
      }

      if (sensorFaultCount >= SENSOR_FAULT_LIMIT) {
        sensorFault = true;
        pump.stop();
        samplePump.stop();
        Serial.println("SENSOR_FAULT");
      }

      // Second-stage median-trimmed-mean filter (PhFilter) running in the
      // main loop. This operates on raw ADS counts and provides the pH value
      // that drives the titration controller.
      phFilter.add(rawPh.raw);
      lastPh = rawPh;
      if (phFilter.ready()) {
        float filteredRaw = phFilter.filteredRaw();
        lastPh.raw = (int16_t)(filteredRaw + (filteredRaw >= 0.0f ? 0.5f : -0.5f));
        lastPh.millivolts = computeProbeMillivoltsFromAdsInput(
            filteredRaw * 0.125f,
            phCalibration.lowAdsMillivolts,
            phCalibration.lowProbeMillivolts,
            phCalibration.highAdsMillivolts,
            phCalibration.highProbeMillivolts);
        lastPh.ph = computePhFromProbeMillivolts(
            lastPh.millivolts,
            phCalibration.lowProbeMillivolts,
            phCalibration.lowPh,
            phCalibration.highProbeMillivolts,
            phCalibration.highPh);
        lastPh.ok = isValidPh(lastPh.ph);
        phReady = lastPh.ok && !sensorFault;
        phSampleFresh = phReady;
      } else {
        phReady = false;
      }
    } else {
      lastPh = rawPh;
      phReady = false;
    }
  }

  uint32_t scaleIntervalMs = state == RunState::SampleFilling ? SCALE_SAMPLE_FILL_INTERVAL_MS : SCALE_SAMPLE_INTERVAL_MS;
  if (millis() - lastScaleSampleMs < scaleIntervalMs) {
    return;
  }
  lastScaleSampleMs = millis();

  ScaleReading rawScale = scaleSensor.read();
  lastScale = scaleFilter.apply(rawScale, isActiveState());
  scaleReady = lastScale.ok;
  if (scaleReady) {
      if (runEngine.phase() == RunPhase::SampleFilling) {
      float delivered = computeSampleGainGrams(sampleStartWeight, lastScale.grams);
      if (rawScale.ok) {
        float rawDelivered = computeSampleGainGrams(sampleStartWeight, rawScale.grams);
        if (rawDelivered > delivered) {
          delivered = rawDelivered;
        }
      }
      if (delivered > sampleDeliveredGrams) {
        sampleDeliveredGrams = delivered;
      }
    }
  }
  displayDirty = true;
}

void runController() {
  dispatchRunCommand(RunCommand::Tick);
}

String htmlEscape(const String &value) {
  String out = value;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  out.replace("'", "&#39;");
  return out;
}

String jsonEscape(const String &value) {
  String out = value;
  out.replace("\\", "\\\\");
  out.replace("\"", "\\\"");
  out.replace("\b", "\\b");
  out.replace("\f", "\\f");
  out.replace("\n", "\\n");
  out.replace("\r", "\\r");
  out.replace("\t", "\\t");
  return out;
}

void loadWifiSettings() {
  preferences.begin("network", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPassword = preferences.getString("password", "");
  preferences.end();
}

void saveWifiSettings(const String &ssid, const String &password) {
  preferences.begin("network", false);
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  wifiSsid = ssid;
  wifiPassword = password;
}

void scheduleRestart(const String &message) {
  statusLine = message;
  restartPending = true;
  restartAtMs = millis();
  displayDirty = true;
}

String htmlPage() {
  String page;
  page.reserve(20000);
  int usedPercent = (int)constrain((consumedGrams / settings.maxConsumedGrams) * 100.0f, 0.0f, 100.0f);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>K10 pH Titrator</title><style>");
  page += F(":root{--bg:#071014;--panel:#0d1d24;--panel2:#102a34;--line:#244c59;--text:#eaf7ff;--muted:#8db0bd;--ok:#67f09a;--warn:#ffd15c;--bad:#ff6b6b;--blue:#7bd5ff}");
  page += F("*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0,#15333b 0,#071014 38%,#04080a 100%);color:var(--text);font-family:Verdana,Geneva,sans-serif}");
  page += F("main{max-width:880px;margin:auto;padding:16px}.top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;margin-bottom:12px}.brand{letter-spacing:.08em;color:var(--muted);font-size:12px}.title{font-size:28px;font-weight:700;margin:2px 0 0}.pill{display:flex;flex-wrap:wrap;gap:6px;justify-content:flex-end;max-width:100%;color:var(--blue)}.pill span{border:1px solid var(--line);border-radius:999px;padding:7px 10px;background:#071820;white-space:nowrap}");
  page += F(".hero{border:1px solid var(--line);border-radius:10px;background:linear-gradient(135deg,#0f2630,#081219);padding:18px;margin-bottom:12px;display:grid;grid-template-columns:1.2fr .8fr;gap:14px}.ph{font-size:72px;line-height:.95;font-weight:800}.unit{font-size:18px;color:var(--muted);margin-left:6px}.sub{color:var(--muted);margin-top:10px}.status{display:grid;gap:8px;align-content:center}.status b{font-size:22px}");
  page += F(".grid{display:grid;grid-template-columns:repeat(6,1fr);gap:10px}.card{border:1px solid var(--line);border-radius:8px;padding:13px;background:rgba(13,29,36,.9)}.k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.07em}.v{font-size:28px;font-weight:700;margin-top:5px}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}");
  page += F(".bar{height:10px;background:#071014;border:1px solid var(--line);border-radius:99px;overflow:hidden;margin-top:10px}.fill{height:100%;background:linear-gradient(90deg,var(--ok),var(--warn))}.split{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}.split>.full{grid-column:1/-1}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:end}label{display:grid;gap:5px;color:var(--muted);font-size:12px;min-width:130px;flex:1}");
  page += F("button,.btn,input,select{font:inherit;border-radius:7px;border:1px solid #3a6472;background:#0a1a21;color:var(--text);padding:10px 12px;text-decoration:none}button,.btn{display:inline-block;cursor:pointer;font-weight:700}.primary{background:#123b2b;border-color:#2d8a5a;color:#bfffd4}.danger{background:#351216;border-color:#8c3640;color:#ffd1d1}.ghost{color:var(--blue)}h2{margin:0 0 10px;font-size:16px}.tiny{font-size:12px;color:var(--muted)}.tabs{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}.tab{background:#071820;color:var(--blue)}.tab.active{background:#123b2b;border-color:#2d8a5a;color:#bfffd4}.panel{display:none}.panel.active{display:block}.full{margin-top:10px}.mini{max-width:170px}.chart{width:100%;height:260px;border:1px solid var(--line);border-radius:8px;background:#071014;cursor:crosshair}.chartbar{display:flex;gap:8px;flex-wrap:wrap;align-items:end;margin-bottom:10px}.chartbar label{min-width:110px;max-width:180px}.eqp{color:var(--warn);font-weight:700}.guide{display:grid;grid-template-columns:1fr 1fr;gap:10px}.guide p{margin:7px 0}.term{color:var(--blue);font-weight:700}@media(max-width:720px){.hero,.split,.guide{grid-template-columns:1fr}.grid{grid-template-columns:repeat(2,1fr)}.ph{font-size:58px}.top{display:block}.pill{justify-content:flex-start;margin-top:8px}.pill span{border-radius:7px}}</style></head><body><main>");
  page += F("<style>.ph,.v,#status,#mv{font-variant-numeric:tabular-nums}.ph{min-height:72px}.sub{min-height:22px}</style>");

  page += F("<div class='top'><div><div class='brand'>K10 LAB CONTROLLER</div><div class='title'>Potentiometric Titrator</div></div><div id='network' class='pill'>");
  page += F("<span>");
  page += htmlEscape(networkLabel);
  page += F("</span><span>AP ");
  page += htmlEscape(apIpAddress);
  page += F("</span><span>STA ");
  page += htmlEscape(staIpAddress);
  page += otaReady ? F("</span><span>OTA ON</span>") : F("</span><span>OTA OFF</span>");
  page += F("</div></div>");

  page += F("<section class='hero'><div><div id='primarylabel' class='k'>Current ");
  page += endpointText();
  page += F("</div><div id='ph' class='ph ");
  page += lastPh.adcOk ? (phReady ? F("ok'>") : F("warn'>")) : F("warn'>");
  page += F("<span id='primaryvalue'>");
  if (lastPh.adcOk && settings.endpoint == ControlEndpoint::Millivolts) {
    page += String(lastPh.millivolts, 0);
  } else if (lastPh.adcOk) {
    page += String(lastPh.ph, 2);
  } else if (settings.endpoint == ControlEndpoint::Millivolts) {
    page += F("--");
  } else {
    page += F("--.--");
  }
  page += F("</span><span id='primaryunit' class='unit'>");
  page += endpointText();
  page += F("</span></div><div id='mv' class='sub'><span id='secondaryvalue'>");
  if (lastPh.adcOk && settings.endpoint == ControlEndpoint::Millivolts) {
    page += String(lastPh.ph, 2);
    page += F("</span> pH from ADS1115 A0");
  } else if (lastPh.adcOk) {
    page += String(lastPh.millivolts, 0);
    page += F("</span> mV from ADS1115 A0");
  } else if (settings.endpoint == ControlEndpoint::Millivolts) {
    page += F("--.--</span> pH from ADS1115 A0");
  } else {
    page += F("--</span> mV from ADS1115 A0");
  }
  page += F("</div></div><div class='status'><div class='k'>State</div><b id='state'>");
  page += stateLabel();
  page += F("</b><div id='status' class='sub'>");
  page += htmlEscape(statusLine);
  page += F("</div><div>Pump: <span id='pump'>");
  page += pump.isRunning() ? F("<span class='warn'>ON</span>") : F("<span class='ok'>STOP</span>");
  page += F("</span></div></div></section>");

  page += F("<section class='grid'><div class='card'><div class='k'>Target <span id='targetunit'>");
  page += endpointText();
  page += F("</span></div><div id='target' class='v'>");
  page += settings.endpoint == ControlEndpoint::Millivolts ? String(settings.targetMillivolts, 0) : String(settings.targetPh, 2);
  page += F("</div></div><div class='card'><div class='k'>mV</div><div id='mvcard' class='v'>");
  page += lastPh.adcOk ? String(lastPh.millivolts, 0) : F("--");
  page += F("</div></div><div class='card'><div class='k'>Trend</div><div id='mode' class='v'>");
  page += modeLabel();
  page += F("</div></div><div class='card'><div class='k'>Used</div><div id='used' class='v ");
  page += consumedGrams >= settings.maxConsumedGrams ? F("bad'>") : F("ok'>");
  page += String(consumedGrams, 1);
  page += F(" g</div><div class='bar'><div class='fill' style='width:");
  page += String(usedPercent);
  page += F("%'></div></div><div id='limit' class='tiny'>Limit ");
  page += String(settings.maxConsumedGrams, 0);
  page += F(" g</div></div><div class='card'><div class='k'>Reactor</div><div id='bottle' class='v'>");
  page += scaleReady ? String(lastScale.grams, 1) : F("--");
  page += F(" g</div></div><div class='card'><div class='k'>Sample</div><div id='sample' class='v'>");
  page += String(sampleDeliveredGrams, 1);
  page += F(" g</div><div id='sampletarget' class='tiny'>Target ");
  page += String(settings.sampleGrams, 1);
  page += F(" g</div></div></section>");

  page += F("<nav class='tabs'><button class='tab active' data-tab='run' type='button'>Run</button><button class='tab' data-tab='cal' type='button'>Calibration</button><button class='tab' data-tab='manual' type='button'>Manual</button><button class='tab' data-tab='admin' type='button'>Admin</button><button class='tab' data-tab='guide' type='button'>Guide</button></nav>");
  page += F("<section id='tab-run' class='panel active'><div class='card full'><h2>Actions</h2><div class='row'>");
  page += state == RunState::Paused ? F("<button class='btn primary action' data-cmd='start' type='button'>Resume</button>") : F("<button class='btn primary action' data-cmd='start' type='button'>Start</button>");
  page += F("<button class='btn action' data-cmd='start_existing' type='button'>Start current sample</button><button class='btn action' data-cmd='stop' type='button'>Pause</button><button class='btn action' data-cmd='tare' type='button'>Tare scale</button><button class='btn ghost action' data-cmd='reset' type='button'>Reset</button><button id='panicButton' class='btn danger' type='button'>Emergency stop</button>");
  page += F("</div><p class='tiny'>ADC ");
  page += lastPh.adcOk ? F("OK") : F("NO DATA");
  page += F(" / raw ");
  page += String(lastPh.raw);
  page += F(" / pH valid ");
  page += phReady ? F("yes") : F("no");
  page += F(" / scale ");
  page += scaleReady ? F("OK") : F("NO");
  page += F("</p><p class='tiny'>If the status says OTA failed: reset required, use Web Reset above.");
  page += F("</p><p id='netdetail' class='tiny'>AP ");
  page += htmlEscape(apIpAddress);
  page += F(" / STA ");
  page += htmlEscape(staIpAddress);
  page += F(" / OTA host ");
  page += htmlEscape(String(OTA_HOSTNAME));
  page += F("</p><p class='tiny'><a class='ghost' href='/json'>JSON status</a> / Source <a class='ghost' href='https://github.com/KnowFlow/OpenTitrator/tree/codex/ph-titrator'>KnowFlow/OpenTitrator</a> / <a class='ghost' href='https://github.com/rockets-cn/k10-ph-titrator/tree/codex/ph-titrator'>rockets-cn mirror</a></p></div>");
  page += F("<div class='card full'><h2>Run Data</h2><div class='chartbar'>");
  page += F("<label>X axis<select id='chartX'><option value='time' selected>Time s</option><option value='used'>Used g</option></select></label>");
  page += F("<label>Y axis<select id='chartY'><option value='auto'>Endpoint</option><option value='ph'>pH</option><option value='mv'>mV</option></select></label>");
  page += F("<button id='curveClear' type='button'>Clear</button><button id='eqpAuto' type='button'>Auto EQP</button><button id='learnParams' type='button'>Suggest Params</button><button id='curveCsv' type='button'>CSV</button><button id='curveJson' type='button'>JSON</button><button id='recordJson' type='button'>Record JSON</button><button id='printRunReport' type='button'>Print report</button>");
  page += F("</div><div id='runRecordCard' class='row'><label>Sample ID<input id='recordSampleId' type='text'></label><label>Batch/reference<input id='recordBatchReference' type='text'></label><label>Operator<input id='recordOperator' type='text'></label><label>Notes<input id='recordNotes' type='text'></label><button id='newRunRecord' type='button'>New record</button><span id='recordStatus' class='tiny'>Draft</span><span id='recordPoints' class='tiny'>0 points</span></div><div class='row'><label>Import record JSON<input id='replayRecordInput' type='file' accept='application/json'></label><button id='replayAnalysisButton' type='button'>Replay analysis</button><span id='replayInfo' class='tiny'>Replay uses the live curve until a record is imported.</span></div><canvas id='curveCanvas' class='chart' width='820' height='260'></canvas><p id='curveInfo' class='tiny'>0 points</p><p id='eqpInfo' class='tiny'>EQP waits for dose changes. Click the curve to correct the candidate point.</p><p id='learnInfo' class='tiny'>Suggestions wait for at least 4 dose-change points.</p><canvas id='derivCanvas' class='chart' width='820' height='140'></canvas><p id='derivInfo' class='tiny'>Derivative d(signal) / d(used g)</p></div></section>");

  page += F("<section id='tab-cal' class='panel'><div class='card full'><h2>Calibration Start</h2><div class='row'>");
  page += F("<button class='btn action' data-cmd='ready' type='button'>Enter ready</button><button class='btn primary action' data-cmd='calibrate' type='button'>Calibrate pumps</button><button class='btn action' data-cmd='scale_calibrate' type='button'>Tare scale</button><button class='btn action' data-cmd='ph_signal_calibrate' type='button'>Reset pH/mV filter</button>");
  page += F("</div><p class='tiny'>Enter ready stops both pumps and puts the controller in READY. Pump calibration can then be started here or with the K10 B key. Each pump runs 2 s, then waits 5 s before reading the scale.</p></div>");
  page += F("<form action='/set' method='post' class='split mutationForm'>");
  page += F("<div class='card'><h2>Pump Flow</h2><div class='row'>");
  page += F("<label>Titrant pump g/s<input name='titrant_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(titrantPumpFlowRateGps, 3);
  page += F("'></label><label>Sample pump g/s<input name='sample_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(samplePumpFlowRateGps, 3);
  page += F("'></label><label>Titrant pump PWM us<input name='titrant_run_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(titrantPumpRunUs);
  page += F("'></label><label>Sample pump PWM us<input name='sample_run_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(samplePumpRunUs);
  page += F("'></label><label>Titrant dose %<input name='titrant_dose_pct' type='number' min='5' max='100' step='1' value='");
  page += String(titrantDosePercent);
  page += F("'></label><label>Burst on ms<input name='titrant_burst_on_ms' type='number' min='1' max='1000' step='1' value='");
  page += String(titrantBurstOnMs);
  page += F("'></label><label>Burst off ms<input name='titrant_burst_off_ms' type='number' min='0' max='5000' step='1' value='");
  page += String(titrantBurstOffMs);
  page += F("'></label></div><p class='tiny'>Dose % scales automatic titrant pulse time. Burst mode runs the titrant pump for on ms, then pauses for off ms inside each dose window.</p></div>");
  page += F("<div class='card'><h2>Scale</h2><div class='row'><label>Scale factor<input name='scale_factor' type='number' min='1' max='100000' step='0.1' value='");
  page += String(scaleSensor.calibrationFactor(), 1);
  page += F("'></label></div><p class='tiny'>Tare scale resets the reactor baseline. Scale factor is the HX711 conversion value used for grams.</p></div>");
  page += F("<div class='card full'><h2>pH/mV Sensor</h2><p class='tiny'>Live ");
  page += String(lastPh.millivolts, 0);
  page += F(" mV / ");
  page += String(lastPh.ph, 2);
  page += F(" pH. Status: ");
  page += htmlEscape(phCalibrationStatus());
  page += F(" / slope ");
  page += String(phCalibrationSlopePercent(), 1);
  page += F("% / pH7 offset ");
  page += String(phCalibrationOffsetAtPh7Mv(), 1);
  page += F(" mV.</p><div class='row'>");
  page += F("<label>Buffer 1 pH<input name='low_ph' type='number' min='0' max='14' step='0.01' value='");
  page += String(phCalibration.lowPh, 2);
  page += F("'></label><label>Buffer 1 probe mV<input name='low_probe_mv' type='number' min='-1000' max='1000' step='0.1' value='");
  page += String(phCalibration.lowProbeMillivolts, 1);
  page += F("'></label><label>Buffer 1 ADS mV<input name='low_ads_mv' type='number' min='-4096' max='4096' step='0.1' value='");
  page += String(phCalibration.lowAdsMillivolts, 1);
  page += F("'></label></div><div class='row' style='margin-top:10px'>");
  page += F("<label>Buffer 2 pH<input name='high_ph' type='number' min='0' max='14' step='0.01' value='");
  page += String(phCalibration.highPh, 2);
  page += F("'></label><label>Buffer 2 probe mV<input name='high_probe_mv' type='number' min='-1000' max='1000' step='0.1' value='");
  page += String(phCalibration.highProbeMillivolts, 1);
  page += F("'></label><label>Buffer 2 ADS mV<input name='high_ads_mv' type='number' min='-4096' max='4096' step='0.1' value='");
  page += String(phCalibration.highAdsMillivolts, 1);
  page += F("'></label></div><p class='tiny'>Save two buffer points after entering the actual buffer pH and measured probe/ADS mV values. Reset pH/mV filter only restarts acquisition; it does not overwrite saved calibration.</p></div>");
  page += F("<div class='card'><h2>Titrant Standard</h2><p class='tiny'>Current titrant: ");
  page += htmlEscape(titrantLabel());
  page += F("</p><p class='tiny'>Result formula: ");
  page += htmlEscape(resultFormulaLabel());
  page += F(" / blank ");
  page += String(settings.blankGrams, 2);
  page += F(" g.</p><p class='tiny'>Use Admin for known titrant molarity, blank, and formula. A future standardization step can calculate titrant factor from a primary standard.</p></div>");
  page += F("<div class='card'><h2>Save</h2><p class='tiny'>Saving stores pump flow, scale factor, and pH/mV two-point calibration in flash. WiFi and method settings are kept separate.</p><button class='primary' type='submit'>Save calibration</button></div></form></section>");

  page += F("<section id='tab-manual' class='panel'><div class='card full'><h2>Manual Operation</h2><form id='manualForm' action='/action' method='post' class='row mutationForm'><label class='mini'>Run ms<input name='ms' type='number' min='5' max='30000' step='1' value='1000'></label><label class='mini'>Titrant PWM us<input name='titrant_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(titrantPumpRunUs);
  page += F("'></label><label class='mini'>Sample PWM us<input name='sample_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(samplePumpRunUs);
  page += F("'></label><label class='mini'>Burst on ms<input name='burst_on_ms' type='number' min='1' max='1000' step='1' value='5'></label><label class='mini'>Burst off ms<input name='burst_off_ms' type='number' min='0' max='5000' step='1' value='100'></label><label class='mini'>Sweep start us<input name='sweep_start_us' type='number' min='1000' max='1500' step='1' value='1000'></label><label class='mini'>Sweep end us<input name='sweep_end_us' type='number' min='1000' max='1500' step='1' value='1500'></label><label class='mini'>Sweep seconds<input name='sweep_sec' type='number' min='1' max='120' step='1' value='20'></label><button class='btn' name='cmd' value='manual_titrant' type='submit'>Run titrant pump</button><button class='btn' name='cmd' value='manual_sample' type='submit'>Run sample pump</button><button class='btn' name='cmd' value='manual_sweep_titrant' type='submit'>Sweep titrant</button><button class='btn' name='cmd' value='manual_sweep_sample' type='submit'>Sweep sample</button><button class='btn primary' name='cmd' value='manual_capture_sweep' type='submit'>Capture current</button><button class='btn danger' name='cmd' value='manual_stop' type='submit'>Stop pumps</button></form><p class='tiny'>Manual actions are blocked while titration or calibration is active. Run uses burst on/off values, e.g. 5ms on and 100ms off.</p></div></section>");

  page += F("<section id='tab-admin' class='panel'><div class='split'><div><form action='/set' method='post' class='card mutationForm'><h2>Settings</h2><div class='row'>");
  page += F("<label>Method<select id='methodSelect' name='method'><option value='ph_ep'");
  if (currentMethod == TitrationMethod::PhEndpoint) page += F(" selected");
  page += F(">pH endpoint</option><option value='mv_ep'");
  if (currentMethod == TitrationMethod::MvEndpoint) page += F(" selected");
  page += F(">mV endpoint</option><option value='edta_hardness'");
  if (currentMethod == TitrationMethod::EdtaHardness) page += F(" selected");
  page += F(">EDTA hardness</option><option value='manual'");
  if (currentMethod == TitrationMethod::Manual) page += F(" selected");
  page += F(">Manual method</option></select></label>");
  page += F("<label>Signal trend<select id='trendSelect' name='trend'><option value='rise'");
  if (settings.controlTrend == ControlTrend::Increase) page += F(" selected");
  page += F(">Dose raises signal</option><option value='fall'");
  if (settings.controlTrend == ControlTrend::Decrease) page += F(" selected");
  page += F(">Dose lowers signal</option></select></label>");
  page += F("<label>Endpoint<select id='endpointSelect' name='endpoint'><option value='ph'");
  if (settings.endpoint == ControlEndpoint::Ph) page += F(" selected");
  page += F(">pH</option><option value='mv'");
  if (settings.endpoint == ControlEndpoint::Millivolts) page += F(" selected");
  page += F(">mV</option></select></label>");
  page += F("<label>Target pH<input id='targetPhInput' name='target' type='number' min='0' max='14' step='0.05' value='");
  page += String(settings.targetPh, 2);
  page += F("'></label><label>Target mV<input id='targetMvInput' name='target_mv' type='number' min='-1000' max='1000' step='1' value='");
  page += String(settings.targetMillivolts, 0);
  page += F("'></label><label>Max used g<input id='maxInput' name='max' type='number' min='1' max='1000' step='1' value='");
  page += String(settings.maxConsumedGrams, 1);
  page += F("'></label><label>Sample g<input id='sampleInput' name='sample' type='number' min='0' max='1000' step='0.1' value='");
  page += String(settings.sampleGrams, 1);
  page += F("'></label></div><div class='row' style='margin-top:10px'>");
  page += F("<label>Titrant<select id='titrantSelect' name='titrant'><option value='naoh001'");
  if (settings.titrantPreset == TitrantPreset::Naoh001) page += F(" selected");
  page += F(">0.01 mol/L NaOH</option><option value='hcl001'");
  if (settings.titrantPreset == TitrantPreset::Hcl001) page += F(" selected");
  page += F(">0.01 mol/L HCl</option><option value='edta001'");
  if (settings.titrantPreset == TitrantPreset::Edta001) page += F(" selected");
  page += F(">0.01 mol/L EDTA</option><option value='manual'");
  if (settings.titrantPreset == TitrantPreset::Manual) page += F(" selected");
  page += F(">Manual</option></select></label>");
  page += F("<label>Manual mol/L<input id='titrantMInput' name='titrant_m' type='number' min='0.0001' max='10' step='0.0001' value='");
  page += String(settings.titrantMolarity, 4);
  page += F("'></label><label>Result formula<select id='resultFormulaSelect' name='result_formula'><option value='acid_base_m'");
  if (settings.resultFormula == ResultFormula::AcidBaseMolar) page += F(" selected");
  page += F(">Acid/base mol/L</option><option value='edta_hardness'");
  if (settings.resultFormula == ResultFormula::EdtaHardnessCaCO3) page += F(" selected");
  page += F(">EDTA hardness</option><option value='manual_factor'");
  if (settings.resultFormula == ResultFormula::ManualFactor) page += F(" selected");
  page += F(">Manual factor</option></select></label><label>Blank g<input id='blankInput' name='blank_g' type='number' min='0' max='1000' step='0.01' value='");
  page += String(settings.blankGrams, 2);
  page += F("'></label></div><div class='row' style='margin-top:10px'><label>Titrant density g/mL<input id='titrantDensityInput' name='titrant_density' type='number' min='0.1' max='5' step='0.001' value='");
  page += String(settings.titrantDensityGramsPerMl, 3);
  page += F("'></label><label>Sample density g/mL<input id='sampleDensityInput' name='sample_density' type='number' min='0.1' max='5' step='0.001' value='");
  page += String(settings.sampleDensityGramsPerMl, 3);
  page += F("'></label><label>Manual factor<input id='manualFactorInput' name='manual_factor' type='number' min='-1000000' max='1000000' step='0.0001' value='");
  page += String(settings.manualResultFactor, 4);
  page += F("'></label><label>Control band<input id='controlBandInput' name='control_band' type='number' min='0.001' max='1000' step='0.001' value='");
  page += String(settings.controlBand, settings.endpoint == ControlEndpoint::Millivolts ? 1 : 3);
  page += F("'></label><label>Stable delta/s<input id='stableDeltaInput' name='stable_delta' type='number' min='0.001' max='1000' step='0.001' value='");
  page += String(settings.stableDelta, settings.endpoint == ControlEndpoint::Millivolts ? 1 : 3);
  page += F("'></label></div><div class='row' style='margin-top:10px'>");
  page += F("<label>Hold s<input id='holdInput' name='hold_s' type='number' min='0' max='120' step='1' value='");
  page += String(settings.holdSeconds);
  page += F("'></label><label>Min settle s<input id='minSettleInput' name='min_settle_s' type='number' min='1' max='120' step='1' value='");
  page += String(settings.minSettleSeconds);
  page += F("'></label><label>Max settle s<input id='maxSettleInput' name='max_settle_s' type='number' min='1' max='180' step='1' value='");
  page += String(settings.maxSettleSeconds);
  page += F("'></label><label>Max time s<input id='maxTimeInput' name='max_time_s' type='number' min='10' max='7200' step='10' value='");
  page += String(settings.maxTimeSeconds);
  page += F("'></label></div><p class='tiny'>Active titrant: <span id='titrant'>");
  page += htmlEscape(titrantLabel());
  page += F("</span> / result <span id='resultm'>");
  page += String(resultValue, (unsigned int)resultDecimals());
  page += F("</span> <span id='resultunit'>");
  page += htmlEscape(resultUnit());
  page += F("</span></p><p class='tiny'>Titrant flow <span id='titrantgps'>");
  page += String(titrantPumpFlowRateGps, 3);
  page += F("</span> g/s / Sample flow <span id='samplegps'>");
  page += String(samplePumpFlowRateGps, 3);
  page += F("</span> g/s</p><p class='tiny'>Use pH or mV as the endpoint, then choose whether dosing makes that signal rise or fall.");
  if (autoEqpEnabled()) {
    page += F(" EDTA hardness uses automatic EQP stop from the mV slope curve.");
  }
  page += F("</p><button class='primary' type='submit'>Save settings</button></form>");

  page += F("<form action='/set' method='post' class='card mutationForm' style='margin-top:10px'><h2>WiFi</h2><div class='row'>");
  page += F("<label>SSID<input name='ssid' maxlength='32' value='");
  page += htmlEscape(wifiSsid);
  page += F("' placeholder='Leave empty for AP only'></label>");
  page += F("<label>Password<input name='wifi_password' type='password' maxlength='64' placeholder='Leave blank to keep'></label>");
  page += F("</div><p class='tiny'>AP stays on. Blank SSID disables STA. Changing WiFi restarts the controller.</p>");
  page += F("<button class='primary' type='submit'>Save WiFi</button></form><form id='otaForm' class='card' style='margin-top:10px'><h2>Firmware update</h2><input name='update' type='file' required><button type='submit'>Upload OTA</button></form></div></div></section>");
  page += F("<section id='tab-guide' class='panel'><div class='guide'>");
  page += F("<div class='card'><h2>Method and Endpoint</h2><p><span class='term'>Method</span> loads a preset group of endpoint, titrant, result formula, and control defaults. Manual keeps custom values.</p><p><span class='term'>Endpoint</span> selects the control signal. Use pH for acid/base endpoint work, or mV for potentiometric endpoints.</p><p><span class='term'>Signal trend</span> tells the controller whether dosing should raise or lower the endpoint signal.</p><p><span class='term'>Target pH / mV</span> is the EP stop value. Only the active endpoint is used for control.</p></div>");
  page += F("<div class='card'><h2>Endpoint Control</h2><p><span class='term'>Control band</span> is the near-target zone. Larger values slow dosing earlier; smaller values dose faster but risk overshoot.</p><p><span class='term'>Stable delta/s</span> is the allowed signal drift while settling. Lower values wait for a flatter response.</p><p><span class='term'>Hold s</span> confirms the endpoint after it is reached. If the signal moves back out, dosing resumes.</p><p><span class='term'>Min / Max settle s</span> controls wait time after each pulse. Slow probes or slow reactions need longer settling.</p><p><span class='term'>Max time s</span> stops a run that takes too long.</p></div>");
  page += F("<div class='card'><h2>Calibration</h2><p><span class='term'>Enter ready</span> stops both pumps before any calibration action.</p><p><span class='term'>Pump flow</span> measures titrant and sample pump delivery in g/s. Recalibrate after tubing, pump head, or liquid changes.</p><p><span class='term'>Scale</span> uses tare for the reactor baseline; scale factor is the grams conversion value.</p><p><span class='term'>pH/mV sensor</span> stores two buffer points and reports slope %, pH7 offset, and status. Reset pH/mV filter only restarts acquisition.</p><p><span class='term'>Titrant standard</span> is configured in Admin through molarity, blank, and result formula.</p></div>");
  page += F("<div class='card'><h2>Dosing and Results</h2><p><span class='term'>Titrant</span> selects the known solution. Manual mol/L is used only when titrant is Manual.</p><p><span class='term'>Max used g</span> is the safety limit for titrant consumption.</p><p><span class='term'>Sample g</span> is the sample mass delivered by the P1 pump before titration.</p><p><span class='term'>Result formula</span> controls only calculation and display; it does not change pump control.</p><p><span class='term'>Blank g</span> subtracts blank titration consumption before calculating and is saved per Method.</p><p><span class='term'>Density g/mL</span> converts scale mass to mL for molarity and EDTA hardness. Defaults 1.000 for water-like solutions.</p><p><span class='term'>Manual factor</span> uses result = net titrant g x factor / sample g for custom tests. Manual mol/L, blank, densities, and factor are method auxiliary values.</p></div>");
  page += F("<div class='card'><h2>Run Data and EQP</h2><p><span class='term'>Time s</span> is the safer default X axis because data keeps moving even while used g is unchanged.</p><p><span class='term'>Used g</span> is useful for final analysis after enough dose changes have happened.</p><p><span class='term'>Auto EQP</span> on the chart marks the largest d(signal)/d(used g) candidate for review. EDTA hardness also uses a firmware-side EQP tracker to stop after the mV slope peak falls back.</p><p><span class='term'>Suggest Params</span> estimates control band, stable delta, and settle time from the current curve. It does not apply settings automatically.</p><p>Click the curve to manually correct the EQP point, then export CSV or JSON to save the run on the computer.</p></div>");
  page += F("</div></section>");
  page += F("<section id='authPanel' class='card full'><div id='loginPanel'><h2>Controller login</h2><div id='loginForm' class='row'><label>Password<input id='loginPassword' type='password' autocomplete='current-password'></label><button id='loginButton' type='button' class='primary'>Login</button></div></div><div id='sessionPanel' style='display:none'><span class='ok'>Authenticated</span> <button id='logoutButton' type='button'>Logout</button></div><details><summary>Factory recovery</summary><div id='recoveryForm' class='row'><label>Factory password<input id='factoryPassword' type='password' autocomplete='off'></label><label>New password<input id='newPassword' type='password' autocomplete='new-password'></label><label>Confirm password<input id='confirmPassword' type='password' autocomplete='new-password'></label><button id='recoveryButton' class='danger' type='button'>Recover</button></div></details><p id='authMessage' class='tiny'></p></section>");
  page += F("<script>");
  page += F("function text(id,v){var e=document.getElementById(id);if(e)e.textContent=v}");
  page += F("function html(id,v){var e=document.getElementById(id);if(e)e.innerHTML=v}");
  page += F("function sessionToken(){return sessionStorage.getItem('k10_session')||''}function showLogin(){document.getElementById('loginPanel').style.display='block';document.getElementById('sessionPanel').style.display='none'}function showSession(){document.getElementById('loginPanel').style.display='none';document.getElementById('sessionPanel').style.display='block'}function authMessage(v){text('authMessage',v)}");
  page += F("async function apiPost(path,form,allowAnonymous){const headers={'Content-Type':'application/x-www-form-urlencoded;charset=UTF-8'};const token=sessionToken();if(token)headers['X-Session-Token']=token;if(!token&&!allowAnonymous&&path!=='/login'&&path!=='/recover'){showLogin();throw new Error('Login required')}const response=await fetch(path,{method:'POST',headers,body:new URLSearchParams(form)});const data=await response.json();if(response.status===401){sessionStorage.removeItem('k10_session');showLogin()}if(response.status===403)authMessage('Request is not permitted in the current state.');if(response.status===429)throw new Error('Too many attempts. Try again later.');if(!response.ok)throw new Error(data.message||data.code||'Request failed');return data}");
  page += F("var curve=[],curveStart=0,eqpManual=null,lastPlot=[];function num(v){return Number(v||0)}function curveTarget(d){return d.endpoint==='mV'?num(d.target_mv):num(d.target_ph)}");
  page += F("function recordCurve(d){if(!d.adc_ok)return;var now=Date.now();if(!curveStart)curveStart=now;curve.push({ts:new Date(now).toISOString(),elapsed_s:(now-curveStart)/1000,ph:num(d.ph),mv:num(d.mv),used_g:num(d.used_g),endpoint_used_g:num(d.endpoint_used_g),sample_g:num(d.sample_delivered_g),endpoint:d.endpoint,target:curveTarget(d),trend:d.mode,state:d.state,pump:!!d.pump,pulse_ms:num(d.pump_pulse_ms),status:d.status,method:d.method,result_value:num(d.result_value),result_unit:d.result_unit,result_formula:d.result_formula,blank_g:num(d.blank_g),titrant_density:num(d.titrant_density),sample_density:num(d.sample_density),auto_eqp:!!d.auto_eqp,eqp_reached:!!d.eqp_reached,eqp_used_g:num(d.eqp_used_g),eqp_signal:num(d.eqp_signal),eqp_slope:num(d.eqp_slope),manual_factor:num(d.manual_factor)});var cutoff=now-86400000;while(curve.length&&new Date(curve[0].ts).getTime()<cutoff)curve.shift();drawCurve()}");
  page += F("var runRecord=null;function recordCopy(v){return JSON.parse(JSON.stringify(v))}function readRecordMetadata(){return{sampleId:document.getElementById('recordSampleId').value,batchReference:document.getElementById('recordBatchReference').value,operator:document.getElementById('recordOperator').value,notes:document.getElementById('recordNotes').value}}function newRunRecord(){runRecord={schemaVersion:1,recordStatus:'draft',metadata:readRecordMetadata(),startedAt:null,finishedAt:null,methodSnapshot:null,deviceSnapshot:null,confirmed:null,final:null,points:[]};renderRunRecord()}function renderRunRecord(){text('recordStatus',runRecord?runRecord.recordStatus.charAt(0).toUpperCase()+runRecord.recordStatus.slice(1):'Draft');text('recordPoints',runRecord?runRecord.points.length+' points':'0 points')}function recordMethodSnapshot(d){return{method:d.method,methodLabel:d.method_label,endpoint:d.endpoint,targetPh:d.target_ph,targetMv:d.target_mv,maxGrams:d.max_g,controlBand:d.control_band,stableDelta:d.stable_delta,holdSeconds:d.hold_s,minSettleSeconds:d.min_settle_s,maxSettleSeconds:d.max_settle_s,titrant:d.titrant,titrantM:d.titrant_m,resultFormula:d.result_formula,blankGrams:d.blank_g,titrantDensity:d.titrant_density,sampleDensity:d.sample_density,manualFactor:d.manual_factor}}function recordDeviceSnapshot(d){return{network:d.network,ip:d.ip,ap_ip:d.ap_ip,sta_ip:d.sta_ip,sta_connected:d.sta_connected,ota:d.ota,ota_safety_lock:d.ota_safety_lock,status:d.status,titrantGps:d.titrant_gps,sampleGps:d.sample_gps,titrantRunUs:d.titrant_run_us,sampleRunUs:d.sample_run_us,titrantDosePercent:d.titrant_dose_pct,scaleFactor:d.scale_factor,phCalibrationStatus:d.ph_cal_status,phSlopePercent:d.ph_slope_percent,phOffset7Mv:d.ph_offset7_mv}}function recordPoint(){var p=recordCopy(curve[curve.length-1]);p.elapsed_s=runRecord.points.length?(new Date(p.ts)-new Date(runRecord.startedAt))/1000:0;return p}function recordFinal(d){return{state:d.state,status:d.status,resultValue:d.result_value,resultUnit:d.result_unit,usedGrams:d.used_g,endpointUsedGrams:d.endpoint_used_g,sampleDeliveredGrams:d.sample_delivered_g,eqpReached:d.eqp_reached,eqpUsedGrams:d.eqp_used_g,eqpSignal:d.eqp_signal,eqpSlope:d.eqp_slope}}function observeRunRecord(d){var active=['SAMPLE','WARMUP','RUN','DOSE','WAIT','PAUSE'].indexOf(d.state)>=0;if(active&&!runRecord)newRunRecord();if(active&&runRecord&&!runRecord.startedAt){runRecord.recordStatus='running';runRecord.startedAt=new Date().toISOString();runRecord.methodSnapshot=recordCopy(recordMethodSnapshot(d));runRecord.deviceSnapshot=recordCopy(recordDeviceSnapshot(d));runRecord.points=d.adc_ok&&curve.length?[recordPoint()]:[]}else if(runRecord&&runRecord.startedAt&&!runRecord.final&&d.adc_ok&&curve.length){runRecord.points.push(recordPoint())}if(runRecord&&runRecord.startedAt&&!runRecord.final){var completed=d.state==='DONE'&&(d.status==='Target reached'||d.status==='Equivalence point');var aborted=d.state==='ERROR'||d.status==='Emergency stop'||(!active&&d.state!=='DONE');if(completed||aborted){runRecord.recordStatus=completed?'completed':'aborted';runRecord.confirmed=completed;runRecord.finishedAt=new Date().toISOString();runRecord.final=recordCopy(recordFinal(d))}}renderRunRecord()}");
  page += F("var replayPoints=null;function analyzeReplay(points){var empty=function(reason){return{quality:'insufficient',reason:reason,candidate:null,slopes:[]}};if(!Array.isArray(points))return empty('No curve points available.');var normalized=[];points.forEach(function(p){var endpoint=p.endpoint==='mV'?'mV':'pH',used=num(p.used_g),signal=num(endpoint==='mV'?p.mv:p.ph);if(isFinite(used)&&isFinite(signal))normalized.push({endpoint:endpoint,used_g:used,signal:signal,elapsed_s:num(p.elapsed_s),ph:num(p.ph),mv:num(p.mv)})});if(normalized.length<3)return empty('Need at least 3 usable points.');var endpoint=normalized[0].endpoint;if(normalized.some(function(p){return p.endpoint!==endpoint}))return empty('Mixed pH and mV records cannot be replayed together.');normalized.sort(function(a,b){return a.used_g-b.used_g||a.elapsed_s-b.elapsed_s});var dose=[];normalized.forEach(function(p){if(dose.length&&Math.abs(p.used_g-dose[dose.length-1].used_g)<0.01)dose[dose.length-1]=p;else dose.push(p)});if(dose.length<3)return empty('Need at least 3 separated dose-change points.');var slopes=[];for(var i=1;i<dose.length-1;i++){var prev=dose[i-1],next=dose[i+1],span=next.used_g-prev.used_g;if(span>0)slopes.push({index:i,used_g:dose[i].used_g,elapsed_s:dose[i].elapsed_s,signal:dose[i].signal,ph:dose[i].ph,mv:dose[i].mv,endpoint:endpoint,slope:Math.abs((next.signal-prev.signal)/(next.used_g-prev.used_g))})}if(!slopes.length)return empty('No positive dose span is available.');var best=slopes[0];slopes.forEach(function(s){if(s.slope>best.slope)best=s});var quality=slopes.length>=5?'high':slopes.length>=2?'review':'insufficient',reason=quality==='high'?'Enough local slopes for review.':quality==='review'?'Limited slope count; review the curve manually.':'Only one local slope; add more dose changes.';return{quality:quality,reason:reason,candidate:best,slopes:slopes}}");
  page += F("function replayText(result){if(!result.candidate)return 'Replay: '+result.reason;var c=result.candidate,v=c.endpoint==='mV'?c.signal.toFixed(0)+' mV':c.signal.toFixed(2)+' pH';return 'Replay '+result.quality+': EQP '+c.used_g.toFixed(2)+' g, '+v+', centered slope '+c.slope.toFixed(c.endpoint==='mV'?1:3)+' '+c.endpoint+'/g. '+result.reason}function runReplayAnalysis(){text('replayInfo',replayText(analyzeReplay(replayPoints||curve)))}function importReplayRecord(file){if(!file)return;var reader=new FileReader();reader.onload=function(){try{var data=JSON.parse(reader.result),points=data&&data.record&&Array.isArray(data.record.points)?data.record.points:data&&Array.isArray(data.points)?data.points:null;if(!points)throw new Error('JSON does not contain record points.');replayPoints=recordCopy(points);text('replayInfo','Imported '+replayPoints.length+' points locally. Click Replay analysis.')}catch(e){replayPoints=null;text('replayInfo','Import failed: '+e.message)}};reader.readAsText(file)}");
  page += F("function dosePoints(){var pts=[];curve.forEach(function(p){if(!isFinite(p.used_g))return;if(pts.length&&Math.abs(p.used_g-pts[pts.length-1].used_g)<0.01){pts[pts.length-1]=p}else pts.push(p)});return pts}");
  page += F("function analyzeEqp(){var pts=dosePoints();if(pts.length<3)return null;var yk=pts[pts.length-1].endpoint==='mV'?'mv':'ph',best=null;for(var i=1;i<pts.length;i++){var dx=pts[i].used_g-pts[i-1].used_g;if(Math.abs(dx)<0.01)continue;var dy=pts[i][yk]-pts[i-1][yk],s=Math.abs(dy/dx);if(!best||s>best.slope){best={mode:'auto',index:i,used_g:pts[i].used_g,elapsed_s:pts[i].elapsed_s,signal:pts[i][yk],ph:pts[i].ph,mv:pts[i].mv,endpoint:pts[i].endpoint,slope:s}}}return best}");
  page += F("function currentEqp(){var a=analyzeEqp();return eqpManual||a}function eqpText(e){if(!e)return 'EQP waits for at least 3 dose-change points.';var v=e.endpoint==='mV'?e.signal.toFixed(0)+' mV':e.signal.toFixed(2)+' pH';return (e.mode==='manual'?'Manual':'Auto')+' EQP: '+e.used_g.toFixed(2)+' g, '+v+', slope '+e.slope.toFixed(e.endpoint==='mV'?1:3)+' '+e.endpoint+'/g'}");
  page += F("function median(a){if(!a.length)return 0;var b=a.slice().sort(function(x,y){return x-y}),m=Math.floor(b.length/2);return b.length%2?b[m]:(b[m-1]+b[m])/2}function clamp(v,a,b){return Math.max(a,Math.min(b,v))}");
  page += F("function suggestParams(){var pts=dosePoints();if(pts.length<4){text('learnInfo','Need at least 4 dose-change points before recommending parameters.');return}var ep=pts[pts.length-1].endpoint,yk=ep==='mV'?'mv':'ph',dose=[],slope=[],timeSlope=[];for(var i=1;i<pts.length;i++){var dx=pts[i].used_g-pts[i-1].used_g,dy=pts[i][yk]-pts[i-1][yk],dt=pts[i].elapsed_s-pts[i-1].elapsed_s;if(Math.abs(dx)>=0.01){dose.push(Math.abs(dx));slope.push(Math.abs(dy/dx))}if(dt>0)timeSlope.push(Math.abs(dy/dt))}if(!slope.length){text('learnInfo','Need changing used g values before recommending parameters.');return}var md=median(dose),ms=Math.max.apply(null,slope),drift=median(timeSlope);var mv=ep==='mV';var band=mv?clamp(ms*md*2.0,10,120):clamp(ms*md*2.0,0.10,1.50);var stable=mv?clamp(Math.max(drift*1.5,0.3),0.3,5.0):clamp(Math.max(drift*1.5,0.003),0.003,0.050);var steep=mv?ms>80:ms>2.0;var minSettle=steep?10:5,maxSettle=steep?60:30;var msg='Suggested: control band '+band.toFixed(mv?1:3)+', stable delta/s '+stable.toFixed(mv?1:3)+', min/max settle '+minSettle+'/'+maxSettle+' s';msg+=' from max slope '+ms.toFixed(mv?1:3)+' '+ep+'/g and median dose '+md.toFixed(2)+' g. Review before applying in Admin.';text('learnInfo',msg)}");
  page += F("function drawCurve(){var c=document.getElementById('curveCanvas');if(!c)return;var r=c.getBoundingClientRect();if(r.width>0&&c.width!==Math.floor(r.width)){c.width=Math.floor(r.width);c.height=260}var g=c.getContext('2d'),w=c.width,h=c.height,l=58,t=18,ri=18,b=36;lastPlot=[];g.clearRect(0,0,w,h);g.fillStyle='#071014';g.fillRect(0,0,w,h);g.strokeStyle='#244c59';g.lineWidth=1;g.strokeRect(l,t,w-l-ri,h-t-b);text('curveInfo',curve.length+' points');text('eqpInfo',eqpText(currentEqp()));if(curve.length<2){drawDeriv();return}var xs=document.getElementById('chartX'),ys=document.getElementById('chartY');var xk=xs&&xs.value==='used'?'used_g':'elapsed_s';var ysel=ys?ys.value:'auto';var yk=ysel==='auto'?(curve[curve.length-1].endpoint==='mV'?'mv':'ph'):ysel;var yl=yk==='ph'?'pH':'mV';var view=curve;if(xk==='elapsed_s'){var latest=curve[curve.length-1].elapsed_s,from=Math.max(curve[0].elapsed_s,latest-600);view=curve.filter(function(p){return p.elapsed_s>=from})}var pts=[];view.forEach(function(p){if(xk==='used_g'&&pts.length&&Math.abs(p.used_g-pts[pts.length-1].used_g)<0.01){pts[pts.length-1]=p}else pts.push(p)});var info=curve.length+' saved / '+pts.length+' shown'+(xk==='elapsed_s'?' / last 10 min':'');text('curveInfo',info);if(pts.length<2){text('curveInfo',info+' / waiting for '+(xk==='used_g'?'dose change':'more samples'));drawDeriv();return}var smooth=pts.map(function(p,i){var s=0,n=0;for(var j=Math.max(0,i-2);j<=Math.min(pts.length-1,i+2);j++){s+=pts[j][yk];n++}var q=Object.assign({},p);q[yk]=s/n;return q});var last=pts[pts.length-1],lastSmooth=smooth[smooth.length-1],tgt=last.target,tol=yk==='mv'?5:0.05;var minx=pts[0][xk],maxx=pts[0][xk],miny=pts[0][yk],maxy=miny;pts.concat(smooth).forEach(function(p){if(p[xk]<minx)minx=p[xk];if(p[xk]>maxx)maxx=p[xk];if(p[yk]<miny)miny=p[yk];if(p[yk]>maxy)maxy=p[yk]});if(yk==='ph'){miny=1;maxy=14}else{if(tgt>0){miny=Math.min(miny,tgt-tol);maxy=Math.max(maxy,tgt+tol)}if(maxy===miny)maxy=miny+1;var ypad=(maxy-miny)*0.10;miny-=ypad;maxy+=ypad}if(maxx===minx)maxx=minx+1;var px=function(x){return l+(x-minx)/(maxx-minx)*(w-l-ri)};var py=function(y){return h-b-(y-miny)/(maxy-miny)*(h-t-b)};if(yk==='ph'){[{a:4,b:6,c:'rgba(85,190,255,0.10)'},{a:6,b:8,c:'rgba(103,240,154,0.11)'},{a:8,b:10,c:'rgba(255,209,92,0.10)'}].forEach(function(z){g.fillStyle=z.c;g.fillRect(l,py(z.b),w-l-ri,py(z.a)-py(z.b))});[4,7,10].forEach(function(v){g.strokeStyle='rgba(141,176,189,0.25)';g.lineWidth=1;g.beginPath();g.moveTo(l,py(v));g.lineTo(w-ri,py(v));g.stroke();g.fillStyle='#8db0bd';g.font='10px Verdana';g.fillText(String(v),l-18,py(v)+3)})}if(tgt>0){var ty1=py(tgt+tol),ty2=py(tgt-tol),ty=py(tgt);g.fillStyle='rgba(255,77,77,0.12)';g.fillRect(l,Math.min(ty1,ty2),w-l-ri,Math.abs(ty2-ty1));g.strokeStyle='#ff4d4d';g.lineWidth=1;g.setLineDash([6,4]);g.beginPath();g.moveTo(l,ty);g.lineTo(w-ri,ty);g.stroke();g.setLineDash([]);g.fillStyle='#ff6b6b';g.font='11px Verdana';g.fillText('target '+tgt.toFixed(yk==='ph'?2:0)+' ±'+tol.toFixed(yk==='ph'?2:0),w-ri-120,ty-5)}g.strokeStyle='rgba(103,240,154,0.28)';g.lineWidth=1;g.beginPath();pts.forEach(function(p,i){var x=px(p[xk]),y=py(p[yk]);lastPlot.push({x:x,y:y,p:p,yk:yk});if(i)g.lineTo(x,y);else g.moveTo(x,y)});g.stroke();g.strokeStyle='#67f09a';g.lineWidth=3;g.beginPath();smooth.forEach(function(p,i){var x=px(p[xk]),y=py(p[yk]);if(i)g.lineTo(x,y);else g.moveTo(x,y)});g.stroke();var lx=px(last[xk]),ly=py(lastSmooth[yk]);g.fillStyle='#eaf7ff';g.strokeStyle='#071014';g.lineWidth=2;g.beginPath();g.arc(lx,ly,5,0,6.283);g.fill();g.stroke();var err=last[yk]-tgt,tag=(yk==='ph'?last[yk].toFixed(2):last[yk].toFixed(0))+' '+yl+(tgt>0?' / Δ '+(err>=0?'+':'')+err.toFixed(yk==='ph'?2:0):'');g.fillStyle='rgba(7,16,20,0.86)';g.fillRect(Math.max(l,lx-154),Math.max(t,ly-28),150,22);g.fillStyle='#eaf7ff';g.font='12px Verdana';g.fillText(tag,Math.max(l+4,lx-150),Math.max(t+14,ly-13));var e=currentEqp();if(e){var ex=px(xk==='used_g'?e.used_g:e.elapsed_s),ey=py(yk==='mv'?e.mv:e.ph);if(ex>=l&&ex<=w-ri){g.strokeStyle='#ffd15c';g.fillStyle='#ffd15c';g.lineWidth=1;g.beginPath();g.moveTo(ex,t);g.lineTo(ex,h-b);g.stroke();g.beginPath();g.arc(ex,ey,5,0,6.283);g.fill()}}var lastPump=false;var doseCount=0;view.forEach(function(p){if(p.pump&&!lastPump&&p[xk]>=minx&&p[xk]<=maxx){var dx=px(p[xk]);g.strokeStyle='rgba(255,210,74,0.4)';g.lineWidth=1;g.setLineDash([2,6]);g.beginPath();g.moveTo(dx,t);g.lineTo(dx,h-b);g.stroke();g.setLineDash([]);if(++doseCount<=8){g.fillStyle='rgba(255,210,74,0.7)';g.font='9px Verdana';g.fillText((p.pulse_ms||0)+'ms',dx+2,t+10)}}lastPump=p.pump});g.fillStyle='#8db0bd';g.font='12px Verdana';g.fillText(xk==='used_g'?'used g':'time s',w-86,h-10);g.fillText(yl,l,13);g.fillText(miny.toFixed(yk==='ph'?0:0),8,h-b);g.fillText(maxy.toFixed(yk==='ph'?0:0),8,t+5);drawDeriv()}");
  page += F("function drawDeriv(){var c=document.getElementById('derivCanvas');if(!c)return;var r=c.getBoundingClientRect();if(r.width>0&&c.width!==Math.floor(r.width)){c.width=Math.floor(r.width);c.height=140}var g=c.getContext('2d'),w=c.width,h=c.height,l=58,t=14,ri=18,b=30;g.clearRect(0,0,w,h);g.fillStyle='#071014';g.fillRect(0,0,w,h);g.strokeStyle='#244c59';g.lineWidth=1;g.strokeRect(l,t,w-l-ri,h-t-b);var pts=dosePoints();if(pts.length<3){text('derivInfo','Derivative d(signal) / d(used g) - need >=3 dose-change points');return}var yk=pts[pts.length-1].endpoint==='mV'?'mv':'ph';var derivs=[];for(var i=1;i<pts.length;i++){var dx=pts[i].used_g-pts[i-1].used_g;if(Math.abs(dx)<0.005)continue;var dy=pts[i][yk]-pts[i-1][yk];derivs.push({x:(pts[i].used_g+pts[i-1].used_g)/2,y:dy/dx})}if(derivs.length<2){text('derivInfo','Derivative d(signal) / d(used g) - need more dose separation');return}var minx=derivs[0].x,maxx=derivs[0].x,miny=derivs[0].y,maxy=miny;derivs.forEach(function(d){if(d.x<minx)minx=d.x;if(d.x>maxx)maxx=d.x;if(d.y<miny)miny=d.y;if(d.y>maxy)maxy=d.y});if(maxx===minx)maxx=minx+0.1;if(maxy===miny)maxy=Math.max(miny+1,Math.abs(miny)*0.2);var ypad=(maxy-miny)*0.1;miny-=ypad;maxy+=ypad;var px=function(x){return l+(x-minx)/(maxx-minx)*(w-l-ri)};var py=function(y){return h-b-(y-miny)/(maxy-miny)*(h-t-b)};g.strokeStyle='#48e27b';g.lineWidth=2;g.beginPath();derivs.forEach(function(d,i){var x=px(d.x),y=py(d.y);if(i)g.lineTo(x,y);else g.moveTo(x,y)});g.stroke();var e=currentEqp();if(e){var ex=px(e.used_g),ey=py(e.slope);g.strokeStyle='#ffd15c';g.fillStyle='#ffd15c';g.lineWidth=1;g.beginPath();g.moveTo(ex,t);g.lineTo(ex,h-b);g.stroke();g.beginPath();g.arc(ex,ey,4,0,6.283);g.fill()}g.fillStyle='#8db0bd';g.font='11px Verdana';g.fillText('used g',w-70,h-8);g.fillText('d'+yk+'/dg',l,10);g.fillText(miny.toFixed(yk==='ph'?3:1),4,h-b);g.fillText(maxy.toFixed(yk==='ph'?3:1),4,t+5);text('derivInfo','Derivative d('+yk+')/dg - '+derivs.length+' slope points')}");
  page += F("function chooseEqpAt(ev){if(!lastPlot.length)return;var c=ev.currentTarget,r=c.getBoundingClientRect(),x=(ev.clientX-r.left)*c.width/r.width,y=(ev.clientY-r.top)*c.height/r.height,b=null;lastPlot.forEach(function(pt){var d=(pt.x-x)*(pt.x-x)+(pt.y-y)*(pt.y-y);if(!b||d<b.d)b={d:d,pt:pt}});if(!b)return;var auto=analyzeEqp(),p=b.pt.p,yk=b.pt.yk;eqpManual={mode:'manual',used_g:p.used_g,elapsed_s:p.elapsed_s,signal:p[yk],ph:p.ph,mv:p.mv,endpoint:yk==='mv'?'mV':'pH',slope:auto?auto.slope:0};drawCurve()}");
  page += F("function exportCurve(fmt){if(!curve.length)return;var eqp=currentEqp();var data,mime,name;if(fmt==='json'){data=JSON.stringify({eqp:eqp,points:curve},null,2);mime='application/json';name='titration-data.json'}else{var keys=Object.keys(curve[0]).concat(['eqp_used_g','eqp_signal','eqp_slope','eqp_mode']);data=keys.join(',')+'\\n'+curve.map(function(r){return keys.map(function(k){var v=k==='eqp_used_g'&&eqp?eqp.used_g:k==='eqp_signal'&&eqp?eqp.signal:k==='eqp_slope'&&eqp?eqp.slope:k==='eqp_mode'&&eqp?eqp.mode:r[k];return String(v===undefined?'':v).replace(/\"/g,'\"\"')}).join(',')}).join('\\n');mime='text/csv';name='titration-data.csv'}var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([data],{type:mime}));a.download=name;a.click();setTimeout(function(){URL.revokeObjectURL(a.href)},1000)}");
  page += F("function exportRunRecord(){if(!runRecord)return;var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([JSON.stringify({record:runRecord},null,2)],{type:'application/json'}));a.download='titration-run-record.json';a.click();setTimeout(function(){URL.revokeObjectURL(a.href)},1000)}function reportEscape(v){return String(v===undefined||v===null?'':v).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;').replace(/'/g,'&#39;')}function reportPair(label,value){return '<dt>'+reportEscape(label)+'</dt><dd>'+reportEscape(value)+'</dd>'}function printRunReport(){if(!runRecord)return;var final=runRecord.final||{},elapsed=runRecord.startedAt&&runRecord.finishedAt?(new Date(runRecord.finishedAt)-new Date(runRecord.startedAt))/1000:0,confirmation=runRecord.recordStatus==='aborted'?'ABORTED / NOT CONFIRMED':(runRecord.confirmed?'CONFIRMED':'NOT CONFIRMED'),rows='';rows+=reportPair('Record status',runRecord.recordStatus);rows+=reportPair('Confirmation',confirmation);rows+=reportPair('Sample ID',runRecord.metadata&&runRecord.metadata.sampleId);rows+=reportPair('Batch/reference',runRecord.metadata&&runRecord.metadata.batchReference);rows+=reportPair('Operator',runRecord.metadata&&runRecord.metadata.operator);rows+=reportPair('Notes',runRecord.metadata&&runRecord.metadata.notes);rows+=reportPair('Started',runRecord.startedAt);rows+=reportPair('Finished',runRecord.finishedAt);rows+=reportPair('Final state',final.state);rows+=reportPair('Final status',final.status);rows+=reportPair('Result',String(final.resultValue===undefined?'':final.resultValue)+' '+String(final.resultUnit||''));rows+=reportPair('Used mass (g)',final.usedGrams);rows+=reportPair('EQP used mass (g)',final.eqpUsedGrams);rows+=reportPair('EQP signal',final.eqpSignal);rows+=reportPair('EQP slope',final.eqpSlope);rows+=reportPair('Point count',runRecord.points.length);rows+=reportPair('Elapsed time (s)',elapsed.toFixed(1));var report='<!doctype html><title>Titration run record</title><style>body{font:14px sans-serif;margin:24px}dl{display:grid;grid-template-columns:180px 1fr;gap:6px}dt{font-weight:bold}dd{margin:0}pre{white-space:pre-wrap;word-break:break-word}</style><h1>Titration run record</h1><dl>'+rows+'</dl><h2>Method snapshot</h2><pre>'+reportEscape(JSON.stringify(runRecord.methodSnapshot||{},null,2))+'</pre><h2>Device snapshot</h2><pre>'+reportEscape(JSON.stringify(runRecord.deviceSnapshot||{},null,2))+'</pre>';var w=window.open('','_blank');if(!w)return;w.document.write(report);w.document.close();w.focus();w.print()}");
  page += F("async function poll(){try{let r=await fetch('/json',{cache:'no-store'});let d=await r.json();");
  page += F("let mvMode=d.endpoint==='mV';text('primarylabel','Current '+d.endpoint);text('primaryunit',d.endpoint);");
  page += F("text('primaryvalue',d.adc_ok?(mvMode?Number(d.mv).toFixed(0):Number(d.ph).toFixed(2)):(mvMode?'--':'--.--'));");
  page += F("text('secondaryvalue',d.adc_ok?(mvMode?Number(d.ph).toFixed(2):Number(d.mv).toFixed(0)):(mvMode?'--.--':'--'));var se=document.getElementById('secondaryvalue');if(se&&se.parentNode)se.parentNode.lastChild.textContent=mvMode?' pH from ADS1115 A0':' mV from ADS1115 A0';");
  page += F("text('state',d.state);text('status',d.status);");
  page += F("html('pump',d.pump?'<span class=\"warn\">ON</span>':'<span class=\"ok\">STOP</span>');");
  page += F("text('targetunit',d.endpoint);text('target',d.endpoint==='mV'?Number(d.target_mv).toFixed(0):Number(d.target_ph).toFixed(2));text('mvcard',d.adc_ok?Number(d.mv).toFixed(0):'--');text('mode',d.mode);");
  page += F("let used=Number(d.used_g),max=Number(d.max_g);text('used',used.toFixed(1)+' g');text('limit','Limit '+max.toFixed(0)+' g');");
  page += F("text('sample',Number(d.sample_delivered_g).toFixed(1)+' g');text('sampletarget','Target '+Number(d.sample_g).toFixed(1)+' g');");
  page += F("text('titrant',d.titrant);var rd=d.result_formula==='edta_hardness'?1:(d.result_formula==='manual_factor'?4:5);text('resultm',Number(d.result_value).toFixed(rd));text('resultunit',d.result_unit);");
  page += F("text('bottle',d.bottle_g>=0?Number(d.bottle_g).toFixed(1)+' g':'-- g');");
  page += F("html('network','<span>'+d.network+'</span><span>AP '+d.ap_ip+'</span><span>STA '+d.sta_ip+'</span><span>OTA '+(d.ota?'ON':'OFF')+'</span>');");
  page += F("text('netdetail','AP '+d.ap_ip+' / STA '+d.sta_ip+' / OTA host k10-ph-titrator');");
  page += F("let f=document.querySelector('.fill');if(f)f.style.width=Math.max(0,Math.min(100,used/max*100))+'%';");
  page += F("text('titrantgps',Number(d.titrant_gps).toFixed(3));text('samplegps',Number(d.sample_gps).toFixed(3));");
  page += F("recordCurve(d);");
  page += F("observeRunRecord(d);");
  page += F("}catch(e){}}setInterval(poll,2000);");
  page += F("function activateTab(name){var p=document.getElementById('tab-'+name);if(!p)return;document.querySelectorAll('.tab').forEach(function(x){x.classList.toggle('active',x.dataset.tab===name)});document.querySelectorAll('.panel').forEach(function(x){x.classList.remove('active')});p.classList.add('active')}");
  page += F("document.querySelectorAll('.tab').forEach(function(b){b.onclick=function(){activateTab(b.dataset.tab);location.hash=b.dataset.tab}});var initial=(location.hash||'#run').slice(1);activateTab(initial);");
  page += F("['chartX','chartY'].forEach(function(id){var e=document.getElementById(id);if(e)e.onchange=drawCurve});var cv=document.getElementById('curveCanvas');if(cv)cv.onclick=chooseEqpAt;var dv=document.getElementById('derivCanvas');if(dv)dv.onclick=chooseEqpAt;var ea=document.getElementById('eqpAuto');if(ea)ea.onclick=function(){eqpManual=null;drawCurve()};var lp=document.getElementById('learnParams');if(lp)lp.onclick=suggestParams;var cc=document.getElementById('curveClear');if(cc)cc.onclick=function(){curve=[];curveStart=0;eqpManual=null;text('learnInfo','Suggestions wait for at least 4 dose-change points.');drawCurve()};var ec=document.getElementById('curveCsv');if(ec)ec.onclick=function(){exportCurve('csv')};var ej=document.getElementById('curveJson');if(ej)ej.onclick=function(){exportCurve('json')};var er=document.getElementById('recordJson');if(er)er.onclick=exportRunRecord;var pr=document.getElementById('printRunReport');if(pr)pr.onclick=printRunReport;var nr=document.getElementById('newRunRecord');if(nr)nr.onclick=newRunRecord;var rr=document.getElementById('replayRecordInput');if(rr)rr.onchange=function(){importReplayRecord(rr.files&&rr.files[0])};var ra=document.getElementById('replayAnalysisButton');if(ra)ra.onclick=runReplayAnalysis;renderRunRecord();drawCurve();");
  page += F("var presets={");
  appendMethodPresetJs(page, TitrationMethod::PhEndpoint, true);
  appendMethodPresetJs(page, TitrationMethod::MvEndpoint, true);
  appendMethodPresetJs(page, TitrationMethod::EdtaHardness, true);
  appendMethodPresetJs(page, TitrationMethod::Manual, false);
  page += F("};");
  page += F("function setv(id,v){var e=document.getElementById(id);if(e)e.value=v}var ms=document.getElementById('methodSelect');if(ms)ms.addEventListener('change',function(){var p=presets[ms.value];if(!p)return;setv('endpointSelect',p.endpoint);setv('trendSelect',p.trend);setv('targetPhInput',p.target);setv('targetMvInput',p.target_mv);setv('maxInput',p.max);setv('sampleInput',p.sample);setv('titrantSelect',p.titrant);setv('titrantMInput',p.titrant_m);setv('resultFormulaSelect',p.result_formula);setv('blankInput',p.blank_g);setv('titrantDensityInput',p.titrant_density);setv('sampleDensityInput',p.sample_density);setv('manualFactorInput',p.manual_factor);setv('controlBandInput',p.control_band);setv('stableDeltaInput',p.stable_delta);setv('holdInput',p.hold_s);setv('minSettleInput',p.min_settle_s);setv('maxSettleInput',p.max_settle_s);setv('maxTimeInput',p.max_time_s)});");
  page += F("if(sessionToken())showSession();else showLogin();document.querySelectorAll('.action').forEach(function(b){b.onclick=async function(){try{await apiPost('/action',{cmd:b.dataset.cmd},false);authMessage('Done');poll()}catch(e){authMessage(e.message)}}});document.getElementById('panicButton').onclick=async function(){try{await apiPost('/panic',{},true);authMessage('Pumps stopped');poll()}catch(e){authMessage(e.message)}};document.querySelectorAll('.mutationForm').forEach(function(f){f.addEventListener('submit',async function(e){e.preventDefault();var data=Object.fromEntries(new FormData(f).entries());if(f.id==='manualForm'){data.cmd=e.submitter&&e.submitter.name?e.submitter.value:data.cmd;data.ajax='1'}try{await apiPost(f.action.endsWith('/action')?'/action':'/set',data,false);authMessage('Saved');poll()}catch(err){authMessage(err.message)}})});");
  page += F("async function loginNow(){var input=document.getElementById('loginPassword');try{var d=await apiPost('/login',{password:input.value},true);sessionStorage.setItem('k10_session',d.token);input.value='';showSession();authMessage('Logged in')}catch(err){authMessage(err.message)}}document.getElementById('loginButton').onclick=loginNow;document.getElementById('loginForm').addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();loginNow()}});document.getElementById('logoutButton').onclick=async function(){try{await apiPost('/logout',{},false)}catch(e){}sessionStorage.removeItem('k10_session');showLogin();authMessage('Logged out')};");
  page += F("async function recoverNow(){var fp=document.getElementById('factoryPassword'),npInput=document.getElementById('newPassword'),cpInput=document.getElementById('confirmPassword'),np=npInput.value,cp=cpInput.value,bytes=new TextEncoder().encode(np).length;if(np!==cp){authMessage('New passwords do not match.');return}if(bytes<10||bytes>64){authMessage('New password must be 10 to 64 UTF-8 bytes.');return}try{await apiPost('/recover',{factory_password:fp.value,new_password:np},true);sessionStorage.removeItem('k10_session');fp.value='';npInput.value='';cpInput.value='';showLogin();authMessage('Recovery complete. Log in with the new password.')}catch(err){authMessage(err.message)}}document.getElementById('recoveryButton').onclick=recoverNow;document.getElementById('recoveryForm').addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();recoverNow()}});");
  page += F("document.getElementById('otaForm').addEventListener('submit',async function(e){e.preventDefault();var fd=new FormData(e.currentTarget),headers={},token=sessionToken();if(token)headers['X-Session-Token']=token;try{var response=await fetch('/ota',{method:'POST',headers:headers,body:fd});var data=await response.json();if(response.status===401){sessionStorage.removeItem('k10_session');showLogin()}if(response.status===403)authMessage('Request is not permitted in the current state.');if(response.status===429)throw new Error('Too many attempts. Try again later.');if(!response.ok)throw new Error(data.message||data.code||'Request failed');authMessage('Firmware uploaded')}catch(err){authMessage(err.message)}});");
  page += F("</script></main></body></html>");
  return page;
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void redirectHomeTab(const char *tab) {
  String location = "/";
  if (tab != nullptr && tab[0] != '\0') {
    location += "#";
    location += tab;
  }
  server.sendHeader("Location", location, true);
  server.send(302, "text/plain", "");
}

void handleRoot() {
  server.send(200, "text/html", htmlPage());
}

void sendApiError(int status, const char *code, const String &message) {
  server.send(status, "application/json", String("{\"ok\":false,\"error\":\"") +
      jsonEscape(code) + "\",\"message\":\"" + jsonEscape(message) + "\"}");
}

bool readSessionToken(char out[33]) {
  String token = server.header("X-Session-Token");
  if (token.length() != 32) return false;
  memcpy(out, token.c_str(), 32); out[32] = '\0'; return true;
}

bool requireSession(uint8_t &slot, bool refreshAfterSuccess = false) {
  (void)refreshAfterSuccess;
  char token[33];
  if (!authStorageReady || !readSessionToken(token)) {
    sendApiError(401, "authentication_required", "Authentication required"); return false;
  }
  AuthResult result = authManager.validateSession(token, millis(), slot);
  memset(token, 0, sizeof token);
  if (result != AuthResult::Ok) {
    sendApiError(401, result == AuthResult::Expired ? "session_expired" : "authentication_required",
                 result == AuthResult::Expired ? "Session expired" : "Authentication required");
    return false;
  }
  return true;
}

AdmissionContext admissionRunState(bool authenticated) {
  AdmissionContext context = {authenticated, httpOtaSafetyLock, httpOtaInProgress,
                              isActiveState(), state == RunState::Calibrating};
  return context;
}

bool requireCommand(WebCommand command, uint8_t &sessionSlot) {
  bool anonymousEmergency = command == WebCommand::EmergencyStop;
  if (!anonymousEmergency && !requireSession(sessionSlot)) return false;
  AdmissionResult result = admitWebCommand(command, admissionRunState(!anonymousEmergency));
  if (result == AdmissionResult::Allowed) return true;
  sendApiError(result == AdmissionResult::AuthenticationRequired ? 401 : 403,
               result == AdmissionResult::OtaLocked ? "ota_locked" : "invalid_state", "Command rejected");
  return false;
}

bool saveRecoveredCredential(void *context, const AuthCredential &credential) {
  return static_cast<AuthStore *>(context)->saveAdministrator(credential);
}

void handleMethodNotAllowed() { sendApiError(405, "method_not_allowed", "POST required"); }
void handlePanic() {
  uint8_t sessionSlot;
  if (!requireCommand(WebCommand::EmergencyStop, sessionSlot)) return;
  pump.stop();
  samplePump.stop();
  dispatchRunCommand(RunCommand::EmergencyStop);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLogin() {
  AdmissionResult admission = admitWebCommand(WebCommand::Login, admissionRunState(false));
  if (admission != AdmissionResult::Allowed) { sendApiError(403, "ota_locked", "Login unavailable"); return; }
  if (!authStorageReady || !server.hasArg("password")) { sendApiError(401, "invalid_credentials", "Invalid credentials"); return; }
  String password = server.arg("password"); char token[33] = {};
  AuthResult result = authManager.login(password.c_str(), password.length(), millis(), token);
  password = "";
  if (result == AuthResult::RateLimited) { sendApiError(429, "rate_limited", "Try again later"); return; }
  if (result != AuthResult::Ok) { sendApiError(401, "invalid_credentials", "Invalid credentials"); return; }
  server.send(200, "application/json", String("{\"ok\":true,\"token\":\"") + token + "\"}");
  memset(token, 0, sizeof token);
}

void handleLogout() {
  uint8_t slot; if (!requireCommand(WebCommand::Logout, slot)) return;
  char token[33]; if (!readSessionToken(token)) return;
  authManager.logout(token); memset(token, 0, sizeof token);
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleRecover() {
  pump.stop();
  samplePump.stop();
  AdmissionResult admission = admitWebCommand(WebCommand::Recover, admissionRunState(false));
  if (admission != AdmissionResult::Allowed) { sendApiError(403, "ota_locked", "Recovery unavailable"); return; }
  if (!authStorageReady || !server.hasArg("factory_password") || !server.hasArg("new_password")) {
    sendApiError(401, "recovery_failed", "Recovery failed"); return;
  }
  String factoryPassword = server.arg("factory_password"), newPassword = server.arg("new_password");
  AuthResult result = authManager.recover(factoryPassword.c_str(), factoryPassword.length(),
      newPassword.c_str(), newPassword.length(), millis(), saveRecoveredCredential, &authStore);
  factoryPassword = ""; newPassword = "";
  if (result == AuthResult::RateLimited) { sendApiError(429, "rate_limited", "Try again later"); return; }
  if (result != AuthResult::Ok) { sendApiError(result == AuthResult::StorageError ? 500 : 401, "recovery_failed", "Recovery failed"); return; }
  authManager.clearSessions();
  resetRunData();
  setState(RunState::SetupMode, "Recovered");
  server.send(200, "application/json", "{\"ok\":true}");
}

struct SettingsCandidate {
  TitrationSettings settings;
  PhCalibration calibration;
  TitrationMethod method;
  float titrantGps;
  float sampleGps;
  int titrantRunUs;
  int sampleRunUs;
  int titrantDosePercent;
  uint16_t burstOnMs;
  uint16_t burstOffMs;
  float scaleFactor;
  String wifiSsid;
  String wifiPassword;
};

bool validateSettingsCandidate(const SettingsCandidate &candidate, String &error) {
  if (absoluteFloat(candidate.calibration.highProbeMillivolts - candidate.calibration.lowProbeMillivolts) <= 0.01f ||
      absoluteFloat(candidate.calibration.highAdsMillivolts - candidate.calibration.lowAdsMillivolts) <= 0.01f) {
    error = "Bad calibration span"; return false;
  }
  return true;
}

void commitSettingsCandidate(const SettingsCandidate &candidate) {
  settings = candidate.settings;
  phCalibration = candidate.calibration;
  currentMethod = candidate.method;
  titrantPumpFlowRateGps = candidate.titrantGps;
  samplePumpFlowRateGps = candidate.sampleGps;
  titrantPumpRunUs = candidate.titrantRunUs;
  samplePumpRunUs = candidate.sampleRunUs;
  titrantDosePercent = candidate.titrantDosePercent;
  titrantBurstOnMs = candidate.burstOnMs;
  titrantBurstOffMs = candidate.burstOffMs;
  scaleSensor.setCalibrationFactor(candidate.scaleFactor);
  wifiSsid = candidate.wifiSsid;
  wifiPassword = candidate.wifiPassword;
  pump.setRunPulseUs(titrantPumpRunUs);
  samplePump.setRunPulseUs(samplePumpRunUs);
}

void handleSet() {
  uint8_t sessionSlot;
  if (!requireCommand(WebCommand::SaveMethodSettings, sessionSlot)) return;

  SettingsCandidate candidate = {settings, phCalibration, currentMethod,
      titrantPumpFlowRateGps, samplePumpFlowRateGps, titrantPumpRunUs, samplePumpRunUs,
      titrantDosePercent, titrantBurstOnMs, titrantBurstOffMs,
      scaleSensor.calibrationFactor(), wifiSsid, wifiPassword};
  TitrationSettings &settings = candidate.settings;
  PhCalibration &phCalibration = candidate.calibration;
  TitrationMethod &currentMethod = candidate.method;
  float &titrantPumpFlowRateGps = candidate.titrantGps;
  float &samplePumpFlowRateGps = candidate.sampleGps;
  int &titrantPumpRunUs = candidate.titrantRunUs;
  int &samplePumpRunUs = candidate.sampleRunUs;
  int &titrantDosePercent = candidate.titrantDosePercent;
  uint16_t &titrantBurstOnMs = candidate.burstOnMs;
  uint16_t &titrantBurstOffMs = candidate.burstOffMs;
  String &wifiSsid = candidate.wifiSsid;
  String &wifiPassword = candidate.wifiPassword;

  bool wifiChanged = false;
  bool endpointChanged = false;
  bool methodRequested = false;
  bool methodChanged = false;
  bool methodFieldChanged = false;
  bool methodAuxChanged = false;
  TitrationMethod requestedMethod = currentMethod;
  if (server.hasArg("method")) {
    methodRequested = true;
    requestedMethod = methodFromValue(server.arg("method"));
  }
  if (server.hasArg("mode")) {
    TitrationMode nextMode = server.arg("mode") == "acid" ? TitrationMode::AddAcid : TitrationMode::AddBase;
    ControlTrend nextTrend = nextMode == TitrationMode::AddAcid ? ControlTrend::Decrease : ControlTrend::Increase;
    methodFieldChanged = methodFieldChanged || nextMode != settings.mode || nextTrend != settings.controlTrend;
    settings.mode = nextMode;
    settings.controlTrend = nextTrend;
  }
  if (server.hasArg("trend")) {
    ControlTrend nextTrend = server.arg("trend") == "fall" ? ControlTrend::Decrease : ControlTrend::Increase;
    TitrationMode nextMode = nextTrend == ControlTrend::Decrease ? TitrationMode::AddAcid : TitrationMode::AddBase;
    methodFieldChanged = methodFieldChanged || nextTrend != settings.controlTrend || nextMode != settings.mode;
    settings.controlTrend = nextTrend;
    settings.mode = nextMode;
  }
  if (server.hasArg("endpoint")) {
    ControlEndpoint nextEndpoint =
        server.arg("endpoint") == "mv" ? ControlEndpoint::Millivolts : ControlEndpoint::Ph;
    endpointChanged = nextEndpoint != settings.endpoint;
    settings.endpoint = nextEndpoint;
    methodFieldChanged = methodFieldChanged || endpointChanged;
  }
  if (server.hasArg("target")) {
    float nextTargetPh = constrain(server.arg("target").toFloat(), 0.0f, 14.0f);
    methodFieldChanged = methodFieldChanged || absoluteFloat(nextTargetPh - settings.targetPh) > 0.001f;
    settings.targetPh = nextTargetPh;
  }
  if (server.hasArg("target_mv")) {
    float nextTargetMv = constrain(server.arg("target_mv").toFloat(), -1000.0f, 1000.0f);
    methodFieldChanged = methodFieldChanged || absoluteFloat(nextTargetMv - settings.targetMillivolts) > 0.001f;
    settings.targetMillivolts = nextTargetMv;
  }
  if (server.hasArg("max")) {
    float nextMax = constrain(server.arg("max").toFloat(), 1.0f, 1000.0f);
    settings.maxConsumedGrams = nextMax;
  }
  if (server.hasArg("sample")) {
    float nextSample = constrain(server.arg("sample").toFloat(), 0.0f, 1000.0f);
    settings.sampleGrams = nextSample;
  }
  if (server.hasArg("titrant")) {
    String titrant = server.arg("titrant");
    TitrantPreset nextTitrant = TitrantPreset::Naoh001;
    if (titrant == "hcl001") {
      nextTitrant = TitrantPreset::Hcl001;
    } else if (titrant == "edta001") {
      nextTitrant = TitrantPreset::Edta001;
    } else if (titrant == "manual") {
      nextTitrant = TitrantPreset::Manual;
    }
    methodFieldChanged = methodFieldChanged || nextTitrant != settings.titrantPreset;
    settings.titrantPreset = nextTitrant;
  }
  if (server.hasArg("titrant_m")) {
    float nextMolarity = constrain(server.arg("titrant_m").toFloat(), 0.0001f, 10.0f);
    methodAuxChanged = methodAuxChanged || absoluteFloat(nextMolarity - settings.titrantMolarity) > 0.00001f;
    settings.titrantMolarity = nextMolarity;
  }
  if (server.hasArg("result_formula")) {
    String formula = server.arg("result_formula");
    ResultFormula nextFormula = ResultFormula::AcidBaseMolar;
    if (formula == "edta_hardness") {
      nextFormula = ResultFormula::EdtaHardnessCaCO3;
    } else if (formula == "manual_factor") {
      nextFormula = ResultFormula::ManualFactor;
    }
    methodFieldChanged = methodFieldChanged || nextFormula != settings.resultFormula;
    settings.resultFormula = nextFormula;
  }
  if (server.hasArg("blank_g")) {
    float nextBlank = constrain(server.arg("blank_g").toFloat(), 0.0f, 1000.0f);
    methodAuxChanged = methodAuxChanged || absoluteFloat(nextBlank - settings.blankGrams) > 0.001f;
    settings.blankGrams = nextBlank;
  }
  if (server.hasArg("titrant_density")) {
    float nextDensity = constrain(server.arg("titrant_density").toFloat(), 0.1f, 5.0f);
    methodAuxChanged = methodAuxChanged || absoluteFloat(nextDensity - settings.titrantDensityGramsPerMl) > 0.001f;
    settings.titrantDensityGramsPerMl = nextDensity;
  }
  if (server.hasArg("sample_density")) {
    float nextDensity = constrain(server.arg("sample_density").toFloat(), 0.1f, 5.0f);
    methodAuxChanged = methodAuxChanged || absoluteFloat(nextDensity - settings.sampleDensityGramsPerMl) > 0.001f;
    settings.sampleDensityGramsPerMl = nextDensity;
  }
  if (server.hasArg("manual_factor")) {
    float nextFactor = constrain(server.arg("manual_factor").toFloat(), -1000000.0f, 1000000.0f);
    methodAuxChanged = methodAuxChanged || absoluteFloat(nextFactor - settings.manualResultFactor) > 0.001f;
    settings.manualResultFactor = nextFactor;
  }
  if (server.hasArg("control_band")) {
    float nextBand = constrain(server.arg("control_band").toFloat(), 0.001f, 1000.0f);
    methodFieldChanged = methodFieldChanged || absoluteFloat(nextBand - settings.controlBand) > 0.001f;
    settings.controlBand = nextBand;
  }
  if (server.hasArg("stable_delta")) {
    float nextStableDelta = constrain(server.arg("stable_delta").toFloat(), 0.001f, 1000.0f);
    methodFieldChanged = methodFieldChanged || absoluteFloat(nextStableDelta - settings.stableDelta) > 0.001f;
    settings.stableDelta = nextStableDelta;
  }
  if (server.hasArg("hold_s")) {
    uint16_t nextHold = (uint16_t)constrain(server.arg("hold_s").toInt(), 0, 120);
    settings.holdSeconds = nextHold;
  }
  if (server.hasArg("min_settle_s")) {
    uint16_t nextMinSettle = (uint16_t)constrain(server.arg("min_settle_s").toInt(), 1, 120);
    settings.minSettleSeconds = nextMinSettle;
  }
  if (server.hasArg("max_settle_s")) {
    uint16_t nextMaxSettle = (uint16_t)constrain(server.arg("max_settle_s").toInt(), 1, 180);
    settings.maxSettleSeconds = nextMaxSettle;
  }
  if (server.hasArg("max_time_s")) {
    uint16_t nextMaxTime = (uint16_t)constrain(server.arg("max_time_s").toInt(), 10, 7200);
    settings.maxTimeSeconds = nextMaxTime;
  }
  if (settings.maxSettleSeconds < settings.minSettleSeconds) {
    settings.maxSettleSeconds = settings.minSettleSeconds;
  }

  bool calibrationChanged = false;
  if (server.hasArg("low_ph")) {
    phCalibration.lowPh = constrain(server.arg("low_ph").toFloat(), 0.0f, 14.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("low_probe_mv")) {
    phCalibration.lowProbeMillivolts = constrain(server.arg("low_probe_mv").toFloat(), -1000.0f, 1000.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("low_ads_mv")) {
    phCalibration.lowAdsMillivolts = constrain(server.arg("low_ads_mv").toFloat(), -4096.0f, 4096.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("high_ph")) {
    phCalibration.highPh = constrain(server.arg("high_ph").toFloat(), 0.0f, 14.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("high_probe_mv")) {
    phCalibration.highProbeMillivolts = constrain(server.arg("high_probe_mv").toFloat(), -1000.0f, 1000.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("high_ads_mv")) {
    phCalibration.highAdsMillivolts = constrain(server.arg("high_ads_mv").toFloat(), -4096.0f, 4096.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("titrant_gps")) {
    titrantPumpFlowRateGps = constrain(server.arg("titrant_gps").toFloat(), 0.0f, 100.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("sample_gps")) {
    samplePumpFlowRateGps = constrain(server.arg("sample_gps").toFloat(), 0.0f, 100.0f);
    calibrationChanged = true;
  }
  if (server.hasArg("titrant_run_us")) {
    titrantPumpRunUs = constrainPumpRunUs(server.arg("titrant_run_us").toInt());
    calibrationChanged = true;
  }
  if (server.hasArg("sample_run_us")) {
    samplePumpRunUs = constrainPumpRunUs(server.arg("sample_run_us").toInt());
    calibrationChanged = true;
  }
  if (server.hasArg("titrant_dose_pct")) {
    titrantDosePercent = constrainTitrantDosePercent(server.arg("titrant_dose_pct").toInt());
    calibrationChanged = true;
  }
  if (server.hasArg("titrant_burst_on_ms")) {
    titrantBurstOnMs = constrainTitrantBurstOnMs(server.arg("titrant_burst_on_ms").toInt());
    calibrationChanged = true;
  }
  if (server.hasArg("titrant_burst_off_ms")) {
    titrantBurstOffMs = constrainTitrantBurstOffMs(server.arg("titrant_burst_off_ms").toInt());
    calibrationChanged = true;
  }
  if (server.hasArg("scale_factor")) {
    candidate.scaleFactor = constrain(server.arg("scale_factor").toFloat(), 1.0f, 100000.0f);
    calibrationChanged = true;
  }

  if (server.hasArg("ssid")) {
    String nextSsid = server.arg("ssid");
    nextSsid.trim();
    String nextPassword = wifiPassword;
    if (nextSsid.length() == 0) {
      nextPassword = "";
    } else if (server.hasArg("wifi_password") && server.arg("wifi_password").length() > 0) {
      nextPassword = server.arg("wifi_password");
    }

    if (nextSsid != wifiSsid || nextPassword != wifiPassword) {
      wifiSsid = nextSsid;
      wifiPassword = nextPassword;
      wifiChanged = true;
    }
  }

  if (methodRequested && requestedMethod != currentMethod) {
    methodChanged = true;
    currentMethod = requestedMethod;
    applyTitrationMethodPreset(settings, currentMethod);
    applyMethodAux(settings, loadMethodAux(currentMethod));
    endpointChanged = false;
  } else if (methodRequested && requestedMethod != TitrationMethod::Manual &&
             !methodFieldChanged && !methodMatchesPreset(requestedMethod)) {
    methodChanged = true;
    currentMethod = requestedMethod;
    applyTitrationMethodPreset(settings, currentMethod);
    applyMethodAux(settings, loadMethodAux(currentMethod));
    endpointChanged = false;
  } else if (methodFieldChanged && currentMethod != TitrationMethod::Manual) {
    currentMethod = TitrationMethod::Manual;
  }
  String validationError;
  if (calibrationChanged && !validateSettingsCandidate(candidate, validationError)) {
    sendApiError(422, "invalid_settings", validationError); return;
  }
  commitSettingsCandidate(candidate);
  if (methodChanged) {
    phFilter.reset();
    phReady = false; phSampleFresh = false;
    resetRunData();
    setState(RunState::SetupMode, String("Method ") + methodLabel(currentMethod));
  }
  if (methodRequested || methodFieldChanged) saveSelectedMethod();
  if (methodAuxChanged) saveMethodAux(currentMethod);
  if (calibrationChanged) {
    saveCalibration();
    phFilter.reset(); phReady = false;
  }
  if (wifiChanged) saveWifiSettings(wifiSsid, wifiPassword);
  statusLine = wifiChanged ? "WiFi saved" : (calibrationChanged ? "Calibration saved" : (methodChanged ? "Method loaded" : "Settings saved"));
  displayDirty = true;
  authManager.recordSuccessfulWrite(sessionSlot, millis());
  redirectHomeTab(calibrationChanged ? "cal" : "admin");
  if (wifiChanged) {
    pump.stop();
    samplePump.stop();
    scheduleRestart("WiFi restart");
  }
}

void handleAction() {
  updatePumpTimeouts();
  String cmd = server.arg("cmd");
  WebCommand command;
  bool known = true;
  if (cmd == "start") command = WebCommand::Start;
  else if (cmd == "start_existing") command = WebCommand::StartExisting;
  else if (cmd == "stop") command = WebCommand::Pause;
  else if (cmd == "tare" || cmd == "scale_calibrate") command = WebCommand::Tare;
  else if (cmd == "reset") command = WebCommand::Reset;
  else if (cmd == "ready") command = WebCommand::EnterReady;
  else if (cmd == "calibrate") command = WebCommand::CalibratePumps;
  else if (cmd == "ph_signal_calibrate") command = WebCommand::ResetSignalFilter;
  else if (cmd == "manual_titrant") command = WebCommand::ManualTitrant;
  else if (cmd == "manual_sample") command = WebCommand::ManualSample;
  else if (cmd.startsWith("manual_sweep") || cmd == "manual_capture_sweep") command = WebCommand::ManualSweep;
  else if (cmd == "manual_stop") command = WebCommand::ManualStop;
  else known = false;
  if (!known) { sendApiError(400, "unknown_command", "Unknown command"); return; }
  uint8_t sessionSlot = 0;
  if (!requireCommand(command, sessionSlot)) return;
  if (httpOtaSafetyLock) {
    if (cmd == "reset" && !httpOtaInProgress && !httpOtaSucceeded) {
      resetFromHttpOtaFailure();
      authManager.recordSuccessfulWrite(sessionSlot, millis());
    } else {
      statusLine = httpOtaInProgress ? "OTA in progress" : "OTA failed: reset required";
      displayDirty = true;
    }
    redirectHomeTab("run");
    return;
  }
  uint16_t manualMs = 25;
  if (server.hasArg("ms")) {
    manualMs = (uint16_t)constrain(server.arg("ms").toInt(), 5, 30000);
  } else if (server.hasArg("sec")) {
    float manualSeconds = constrain(server.arg("sec").toFloat(), 0.005f, 30.0f);
    manualMs = (uint16_t)(manualSeconds * 1000.0f + 0.5f);
  }
  uint16_t manualBurstOnMs = constrainTitrantBurstOnMs(server.hasArg("burst_on_ms") ? server.arg("burst_on_ms").toInt() : titrantBurstOnMs);
  uint16_t manualBurstOffMs = constrainTitrantBurstOffMs(server.hasArg("burst_off_ms") ? server.arg("burst_off_ms").toInt() : titrantBurstOffMs);
  int manualTitrantUs = titrantPumpRunUs;
  int manualSampleUs = samplePumpRunUs;
  if (server.hasArg("titrant_us")) {
    manualTitrantUs = server.arg("titrant_us").toInt();
  } else if (server.hasArg("us")) {
    manualTitrantUs = server.arg("us").toInt();
  }
  if (server.hasArg("sample_us")) {
    manualSampleUs = server.arg("sample_us").toInt();
  } else if (server.hasArg("us")) {
    manualSampleUs = server.arg("us").toInt();
  }
  manualTitrantUs = constrainPumpRunUs(manualTitrantUs);
  manualSampleUs = constrainPumpRunUs(manualSampleUs);
  const char *returnTab = "run";
  if (cmd == "start") {
    if (state == RunState::Paused) {
      dispatchRunCommand(RunCommand::Resume);
    } else {
      dispatchRunCommand(RunCommand::StartNormal);
    }
  } else if (cmd == "start_existing") {
    if (state == RunState::Paused) {
      dispatchRunCommand(RunCommand::Resume);
    } else {
      dispatchRunCommand(RunCommand::StartExistingSample);
    }
  } else if (cmd == "stop") {
    dispatchRunCommand(RunCommand::Pause);
  } else if (cmd == "tare") {
    tareScale();
  } else if (cmd == "reset") {
    dispatchRunCommand(RunCommand::Reset);
  } else if (cmd == "ready") {
    resetRunData();
    setState(RunState::SetupReady, "Ready for calibration");
    returnTab = "cal";
  } else if (cmd == "calibrate") {
    returnTab = "cal";
    if (state == RunState::SetupReady) {
      setState(RunState::Calibrating, "Calibrating pumps");
    } else {
      statusLine = "Enter ready first";
      displayDirty = true;
    }
  } else if (cmd == "scale_calibrate") {
    returnTab = "cal";
    if (!isActiveState() && state != RunState::Calibrating) {
      tareScale();
      statusLine = "Scale calibrated";
    } else {
      statusLine = "Enter ready first";
    }
    displayDirty = true;
  } else if (cmd == "ph_signal_calibrate") {
    returnTab = "cal";
    if (!isActiveState() && state != RunState::Calibrating) {
      phFilter.reset();
      phReady = false;
      phSampleFresh = false;
      statusLine = "pH/mV filter reset";
    } else {
      statusLine = "Enter ready first";
    }
    displayDirty = true;
  } else if (cmd == "manual_titrant") {
    returnTab = "manual";
    if (!isActiveState() && state != RunState::Calibrating) {
      stopManualSweep(false);
      samplePump.stop();
      pump.runForMsAtUsBurst(manualMs, manualTitrantUs, manualBurstOnMs, manualBurstOffMs);
      statusLine = String("Manual titrant ") + String(manualMs) + "ms @ " + String(manualTitrantUs) + "us " + String(manualBurstOnMs) + "/" + String(manualBurstOffMs);
      displayDirty = true;
    }
  } else if (cmd == "manual_sample") {
    returnTab = "manual";
    if (!isActiveState() && state != RunState::Calibrating) {
      stopManualSweep(false);
      pump.stop();
      samplePump.runForMsAtUsBurst(manualMs, manualSampleUs, manualBurstOnMs, manualBurstOffMs);
      statusLine = String("Manual sample ") + String(manualMs) + "ms @ " + String(manualSampleUs) + "us " + String(manualBurstOnMs) + "/" + String(manualBurstOffMs);
      displayDirty = true;
    }
  } else if (cmd == "manual_sweep_titrant") {
    returnTab = "manual";
    if (!isActiveState() && state != RunState::Calibrating) {
      int startUs = server.hasArg("sweep_start_us") ? server.arg("sweep_start_us").toInt() : titrantPumpRunUs;
      int endUs = server.hasArg("sweep_end_us") ? server.arg("sweep_end_us").toInt() : PUMP_MAX_RUN_US;
      uint16_t sweepSeconds = (uint16_t)(server.hasArg("sweep_sec") ? server.arg("sweep_sec").toInt() : 20);
      startManualSweep(true, startUs, endUs, sweepSeconds);
    }
  } else if (cmd == "manual_sweep_sample") {
    returnTab = "manual";
    if (!isActiveState() && state != RunState::Calibrating) {
      int startUs = server.hasArg("sweep_start_us") ? server.arg("sweep_start_us").toInt() : samplePumpRunUs;
      int endUs = server.hasArg("sweep_end_us") ? server.arg("sweep_end_us").toInt() : PUMP_MAX_RUN_US;
      uint16_t sweepSeconds = (uint16_t)(server.hasArg("sweep_sec") ? server.arg("sweep_sec").toInt() : 20);
      startManualSweep(false, startUs, endUs, sweepSeconds);
    }
  } else if (cmd == "manual_capture_sweep") {
    returnTab = "manual";
    if (manualSweepActive) {
      int capturedUs = currentManualSweepUs();
      if (manualSweepTitrant) {
        titrantPumpRunUs = capturedUs;
        pump.setRunPulseUs(titrantPumpRunUs);
      } else {
        samplePumpRunUs = capturedUs;
        samplePump.setRunPulseUs(samplePumpRunUs);
      }
      saveCalibration();
      stopManualSweep(true);
      statusLine = String("Captured ") + (manualSweepTitrant ? "titrant " : "sample ") + String(capturedUs) + "us";
      displayDirty = true;
    } else {
      statusLine = "No sweep active";
      displayDirty = true;
    }
  } else if (cmd == "manual_stop") {
    returnTab = "manual";
    stopManualSweep(false);
    pump.stop();
    samplePump.stop();
    statusLine = "Manual stop";
    displayDirty = true;
  }
  updatePumpTimeouts();
  if (command != WebCommand::EmergencyStop) authManager.recordSuccessfulWrite(sessionSlot, millis());
  if (server.hasArg("ajax")) {
    String json = "{\"ok\":true,\"tab\":\"";
    json += returnTab;
    json += "\",\"status\":\"";
    json += jsonEscape(statusLine);
    json += "\"}";
    server.send(200, "application/json", json);
    return;
  }
  redirectHomeTab(returnTab);
}

void handleJson() {
  RunTelemetry telemetry = runEngine.telemetry();
  String json = "{";
  json += "\"adc_ok\":" + String(lastPh.adcOk ? "true" : "false");
  json += ",\"ph_valid\":" + String(phReady ? "true" : "false");
  json += ",\"raw\":" + String(lastPh.raw);
  json += ",\"ph\":" + String(lastPh.adcOk ? lastPh.ph : -1.0f, 2);
  json += ",\"mv\":" + String(lastPh.adcOk ? lastPh.millivolts : -1.0f, 0);
  json += ",\"bottle_g\":" + String(scaleReady ? lastScale.grams : -1.0f, 1);
  json += ",\"raw_bottle_g\":" + String(scaleReady ? lastScale.rawGrams : -1.0f, 1);
  json += ",\"used_g\":" + String(consumedGrams, 1);
  json += ",\"endpoint_used_g\":" + String(resultConsumedGrams(), 2);
  json += ",\"predose_target_g\":" + String(telemetry.predoseTargetGrams, 2);
  json += ",\"predose_ratio\":" + String(STOICH_PREDOSE_RATIO, 2);
  json += ",\"sample_g\":" + String(settings.sampleGrams, 1);
  json += ",\"sample_delivered_g\":" + String(sampleDeliveredGrams, 1);
  json += ",\"titrant\":\"" + jsonEscape(titrantLabel()) + "\"";
  json += ",\"titrant_m\":" + String(activeTitrantMolarity(), 5);
  json += ",\"result_m\":" + String(settings.resultFormula == ResultFormula::AcidBaseMolar ? resultValue : 0.0f, 5);
  json += ",\"result_value\":" + String(resultValue, (unsigned int)resultDecimals());
  json += ",\"result_unit\":\"" + jsonEscape(resultUnit()) + "\"";
  json += ",\"result_formula\":\"" + String(resultFormulaValue(settings.resultFormula)) + "\"";
  json += ",\"blank_g\":" + String(settings.blankGrams, 2);
  json += ",\"titrant_density\":" + String(settings.titrantDensityGramsPerMl, 3);
  json += ",\"sample_density\":" + String(settings.sampleDensityGramsPerMl, 3);
  json += ",\"manual_factor\":" + String(settings.manualResultFactor, 4);
  json += ",\"method\":\"" + String(methodValue(currentMethod)) + "\"";
  json += ",\"method_label\":\"" + jsonEscape(methodLabel(currentMethod)) + "\"";
  json += ",\"auto_eqp\":" + String(autoEqpEnabled() ? "true" : "false");
  json += ",\"eqp_reached\":" + String(telemetry.eqpReached ? "true" : "false");
  json += ",\"eqp_points\":" + String(telemetry.eqpPointCount);
  json += ",\"eqp_used_g\":" + String(telemetry.eqpUsedGrams, 2);
  json += ",\"eqp_signal\":" + String(telemetry.eqpSignal, settings.endpoint == ControlEndpoint::Millivolts ? 0 : 2);
  json += ",\"eqp_slope\":" + String(telemetry.eqpSlope, settings.endpoint == ControlEndpoint::Millivolts ? 1 : 3);
  json += ",\"endpoint\":\"" + String(endpointText()) + "\"";
  json += ",\"target_mv\":" + String(settings.targetMillivolts, 0);
  json += ",\"target_ph\":" + String(settings.targetPh, 2);
  json += ",\"max_g\":" + String(settings.maxConsumedGrams, 1);
  json += ",\"control_band\":" + String(settings.controlBand, settings.endpoint == ControlEndpoint::Millivolts ? 1 : 3);
  json += ",\"stable_delta\":" + String(settings.stableDelta, settings.endpoint == ControlEndpoint::Millivolts ? 1 : 3);
  json += ",\"hold_s\":" + String(settings.holdSeconds);
  json += ",\"min_settle_s\":" + String(settings.minSettleSeconds);
  json += ",\"max_settle_s\":" + String(settings.maxSettleSeconds);
  json += ",\"max_time_s\":" + String(settings.maxTimeSeconds);
  json += ",\"mode\":\"" + modeLabel() + "\"";
  json += ",\"state\":\"" + stateLabel() + "\"";
  json += ",\"status\":\"" + jsonEscape(statusLine) + "\"";
  json += ",\"pump\":" + String(pump.isRunning() ? "true" : "false");
  json += ",\"pump_pulse_ms\":" + String(pump.isRunning() ? displayedPulseMs : 0);
  json += ",\"sample_pump\":" + String(samplePump.isRunning() ? "true" : "false");
  json += ",\"filter_ready\":" + String(phFilter.ready() ? "true" : "false");
  json += ",\"sensor_fault\":" + String(sensorFault ? "true" : "false");
  json += ",\"network\":\"" + jsonEscape(networkLabel) + "\"";
  json += ",\"ip\":\"" + ipAddress + "\"";
  json += ",\"ap_ip\":\"" + apIpAddress + "\"";
  json += ",\"sta_ip\":\"" + staIpAddress + "\"";
  json += ",\"sta_connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"wifi_ssid\":\"" + jsonEscape(wifiSsid) + "\"";
  json += ",\"ota\":" + String(otaReady ? "true" : "false");
  json += ",\"ota_safety_lock\":" + String(httpOtaSafetyLock ? "true" : "false");
  json += ",\"ota_in_progress\":" + String(httpOtaInProgress ? "true" : "false");
  json += ",\"titrant_gps\":" + String(titrantPumpFlowRateGps, 4);
  json += ",\"sample_gps\":" + String(samplePumpFlowRateGps, 4);
  json += ",\"titrant_run_us\":" + String(titrantPumpRunUs);
  json += ",\"sample_run_us\":" + String(samplePumpRunUs);
  json += ",\"titrant_dose_pct\":" + String(titrantDosePercent);
  json += ",\"titrant_burst_on_ms\":" + String(titrantBurstOnMs);
  json += ",\"titrant_burst_off_ms\":" + String(titrantBurstOffMs);
  json += ",\"manual_sweep\":" + String(manualSweepActive ? "true" : "false");
  json += ",\"manual_sweep_pump\":\"" + String(manualSweepTitrant ? "titrant" : "sample") + "\"";
  json += ",\"manual_sweep_us\":" + String(currentManualSweepUs());
  json += ",\"scale_factor\":" + String(scaleSensor.calibrationFactor(), 2);
  json += ",\"scale_filtered\":" + String(lastScale.filtered ? "true" : "false");
  json += ",\"scale_rejected\":" + String(lastScale.rejected ? "true" : "false");
  json += ",\"low_ph\":" + String(phCalibration.lowPh, 2);
  json += ",\"low_probe_mv\":" + String(phCalibration.lowProbeMillivolts, 1);
  json += ",\"low_ads_mv\":" + String(phCalibration.lowAdsMillivolts, 1);
  json += ",\"high_ph\":" + String(phCalibration.highPh, 2);
  json += ",\"high_probe_mv\":" + String(phCalibration.highProbeMillivolts, 1);
  json += ",\"high_ads_mv\":" + String(phCalibration.highAdsMillivolts, 1);
  json += ",\"ph_slope_percent\":" + String(phCalibrationSlopePercent(), 1);
  json += ",\"ph_offset7_mv\":" + String(phCalibrationOffsetAtPh7Mv(), 1);
  json += ",\"ph_cal_status\":\"" + jsonEscape(phCalibrationStatus()) + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void enterHttpOtaSafety() {
  httpOtaSafetyLock = true;
  httpOtaInProgress = true;
  httpOtaSucceeded = false;
  stopManualSweep(false);
  pump.stop();
  samplePump.stop();
  setState(RunState::Error, "OTA upload");
}

void failHttpOta(const String &detail) {
  httpOtaSafetyLock = true;
  httpOtaInProgress = false;
  httpOtaSucceeded = false;
  stopManualSweep(false);
  pump.stop();
  samplePump.stop();
  setState(RunState::Error, detail.length() > 0 ? detail : "OTA failed");
}

void resetFromHttpOtaFailure() {
  pump.stop();
  samplePump.stop();
  stopManualSweep(false);
  resetRunData();
  httpOtaInProgress = false;
  httpOtaSafetyLock = false;
  httpOtaSucceeded = false;
  setState(RunState::SetupMode, "OTA reset");
}

void clearOtaRequestState() {
  otaUploadStartSeen = false;
  otaRequestAccepted = false;
  otaSessionSlot = 0;
  otaRejectedStatus = 0;
}

void handleOta() {
  if (!otaUploadStartSeen || !otaRequestAccepted) {
    int status = otaRejectedStatus ? otaRejectedStatus : 401;
    sendApiError(status,
                 status == 403 ? "ota_locked" : "authentication_required", "OTA rejected");
    clearOtaRequestState();
    return;
  }
  bool success = httpOtaSucceeded && !httpOtaInProgress;
  server.sendHeader("Connection", "close");
  server.send(success ? 200 : 500, "text/plain", success ? "OK" : "FAIL");
  if (success) {
    authManager.recordSuccessfulWrite(otaSessionSlot, millis());
    httpOtaSafetyLock = true;
    scheduleRestart("OTA update done");
  } else {
    failHttpOta("OTA failed");
  }
  clearOtaRequestState();
}

void handleOtaUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    clearOtaRequestState();
    otaUploadStartSeen = true;
    otaRejectedStatus = 401;
    char token[33]; uint8_t slot = 0;
    if (authStorageReady && readSessionToken(token) &&
        authManager.validateSession(token, millis(), slot) == AuthResult::Ok) {
      AdmissionResult admission = admitWebCommand(WebCommand::OtaUpload, admissionRunState(true));
      if (admission == AdmissionResult::Allowed) {
        otaRequestAccepted = true; otaSessionSlot = slot;
      } else otaRejectedStatus = 403;
    }
    memset(token, 0, sizeof token);
    if (!otaRequestAccepted) return;
    enterHttpOtaSafety();
    Serial.printf("OTA: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      failHttpOta("OTA start failed");
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaRequestAccepted) return;
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      failHttpOta("OTA write failed");
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaRequestAccepted) return;
    httpOtaInProgress = false;
    if (Update.end(true)) {
      httpOtaSucceeded = true;
      statusLine = "OTA done";
    } else {
      Update.printError(Serial);
      failHttpOta("OTA failed");
    }
    displayDirty = true;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (otaRequestAccepted) {
      Update.abort();
      failHttpOta("OTA aborted");
    }
    clearOtaRequestState();
  }
}

void startNetwork() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  apIpAddress = WiFi.softAPIP().toString();
  ipAddress = apIpAddress;
  networkLabel = String("AP ") + AP_SSID;

  if (wifiSsid.length() > 0) {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      uint32_t wifiSpin = millis();
      while (millis() - wifiSpin < 250) {
        if (otaReady) ArduinoOTA.handle();
        if (webReady) server.handleClient();
      }
    }
    if (WiFi.status() == WL_CONNECTED) {
      staIpAddress = WiFi.localIP().toString();
      ipAddress = staIpAddress;
      networkLabel = String("AP+STA ") + AP_SSID;
    }
  }

  server.on("/", handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/set", HTTP_GET, handleMethodNotAllowed);
  server.on("/action", HTTP_POST, handleAction);
  server.on("/action", HTTP_GET, handleMethodNotAllowed);
  server.on("/panic", HTTP_POST, handlePanic);
  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", HTTP_POST, handleLogout);
  server.on("/recover", HTTP_POST, handleRecover);
  server.on("/json", handleJson);
  server.on("/ota", HTTP_POST, handleOta, handleOtaUpload);
  const char *headerKeys[] = {"X-Session-Token"};
  server.collectHeaders(headerKeys, 1);
  server.begin();
  webReady = true;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    pump.stop();
    samplePump.stop();
    statusLine = "OTA start";
    displayDirty = true;
  });
  ArduinoOTA.onEnd([]() {
    statusLine = "OTA done";
    displayDirty = true;
  });
  ArduinoOTA.onError([](ota_error_t error) {
    pump.stop();
    samplePump.stop();
    statusLine = "OTA error";
    displayDirty = true;
  });
  ArduinoOTA.begin();
  otaReady = true;
}

void updateNetworkStatus() {
  static uint32_t lastCheckMs = 0;
  static uint32_t lastReconnectMs = 0;
  if (millis() - lastCheckMs < 2000) {
    return;
  }
  lastCheckMs = millis();

  String oldIp = ipAddress;
  String oldStaIp = staIpAddress;
  String oldLabel = networkLabel;

  if (wifiSsid.length() > 0 && WiFi.status() == WL_CONNECTED) {
    staIpAddress = WiFi.localIP().toString();
    ipAddress = staIpAddress;
    networkLabel = String("AP+STA ") + AP_SSID;
  } else {
    staIpAddress = "0.0.0.0";
    ipAddress = apIpAddress;
    networkLabel = String("AP ") + AP_SSID;
    if (wifiSsid.length() > 0 && millis() - lastReconnectMs > WIFI_RETRY_INTERVAL_MS) {
      lastReconnectMs = millis();
      WiFi.reconnect();
    }
  }

  if (oldIp != ipAddress || oldStaIp != staIpAddress || oldLabel != networkLabel) {
    displayDirty = true;
  }
}

void runCalibration() {
  updatePumpTimeouts();

  if (httpOtaSafetyLock) {
    pump.stop();
    samplePump.stop();
    return;
  }

  static uint32_t calStartMs = 0;
  static int calPhase = 0;
  static float calInitialWeight = 0.0f;

  if (calibrationNeedsReset) {
    calPhase = 0;
    calibrationNeedsReset = false;
  }

  if (calPhase == 0) {
    calPhase = 1;
    calStartMs = millis();
    scaleFilter.reset(lastScale.grams);
    calInitialWeight = lastScale.grams;
    pump.stop();
    samplePump.stop();
    statusLine = "Calib: place bottle + tare";
    displayDirty = true;
    return;
  }

  uint32_t elapsed = millis() - calStartMs;

  if (calPhase == 1 && elapsed >= CALIBRATION_PREP_MS) {
    pump.runForMs(CALIBRATION_PUMP_RUN_MS);
    calPhase = 2;
    statusLine = "Calib: titrant 2s";
    displayDirty = true;
    return;
  }

  if (calPhase == 2 && !pump.isRunning() &&
      elapsed >= CALIBRATION_PREP_MS + CALIBRATION_PUMP_RUN_MS + CALIBRATION_SETTLE_MS) {
    float delta = lastScale.grams - calInitialWeight;
    titrantPumpFlowRateGps = delta / (CALIBRATION_PUMP_RUN_MS / 1000.0f);
    calInitialWeight = lastScale.grams;
    samplePump.runForMs(CALIBRATION_PUMP_RUN_MS);
    calPhase = 3;
    statusLine = "Calib: sample 2s";
    displayDirty = true;
    return;
  }

  if (calPhase == 3 && !samplePump.isRunning() &&
      elapsed >= CALIBRATION_PREP_MS + (CALIBRATION_PUMP_RUN_MS * 2) + (CALIBRATION_SETTLE_MS * 2)) {
    float delta = lastScale.grams - calInitialWeight;
    samplePumpFlowRateGps = delta / (CALIBRATION_PUMP_RUN_MS / 1000.0f);
    saveCalibration();
    calPhase = 0;
    setState(RunState::SetupReady, "Calibration done");
    statusLine = "Calib finished";
    displayDirty = true;
    return;
  }
}

void saveCalibration() {
  Preferences prefs;
  if (prefs.begin("cal", false)) {
    prefs.putFloat("titrant_gps", titrantPumpFlowRateGps);
    prefs.putFloat("sample_gps", samplePumpFlowRateGps);
    prefs.putInt("titrant_us", titrantPumpRunUs);
    prefs.putInt("sample_us", samplePumpRunUs);
    prefs.putInt("dose_pct", titrantDosePercent);
    prefs.putUShort("burst_on", titrantBurstOnMs);
    prefs.putUShort("burst_off", titrantBurstOffMs);
    prefs.putFloat("scale_factor", scaleSensor.calibrationFactor());
    prefs.putFloat("low_ads_mv", phCalibration.lowAdsMillivolts);
    prefs.putFloat("low_probe_mv", phCalibration.lowProbeMillivolts);
    prefs.putFloat("low_ph", phCalibration.lowPh);
    prefs.putFloat("high_ads_mv", phCalibration.highAdsMillivolts);
    prefs.putFloat("high_probe_mv", phCalibration.highProbeMillivolts);
    prefs.putFloat("high_ph", phCalibration.highPh);
    prefs.end();
  }
}

void loadCalibration() {
  Preferences prefs;
  if (prefs.begin("cal", true)) {
    titrantPumpFlowRateGps = prefs.getFloat("titrant_gps", 0.0f);
    samplePumpFlowRateGps = prefs.getFloat("sample_gps", 0.0f);
    titrantPumpRunUs = constrainPumpRunUs(prefs.getInt("titrant_us", TITRANT_PUMP_DEFAULT_RUN_US));
    samplePumpRunUs = constrainPumpRunUs(prefs.getInt("sample_us", SAMPLE_PUMP_DEFAULT_RUN_US));
    titrantDosePercent = constrainTitrantDosePercent(prefs.getInt("dose_pct", TITRANT_DOSE_DEFAULT_PERCENT));
    titrantBurstOnMs = constrainTitrantBurstOnMs(prefs.getUShort("burst_on", TITRANT_BURST_ON_DEFAULT_MS));
    titrantBurstOffMs = constrainTitrantBurstOffMs(prefs.getUShort("burst_off", TITRANT_BURST_OFF_DEFAULT_MS));
    scaleSensor.setCalibrationFactor(prefs.getFloat("scale_factor", scaleSensor.calibrationFactor()));
    phCalibration.lowAdsMillivolts = prefs.getFloat("low_ads_mv", phCalibration.lowAdsMillivolts);
    phCalibration.lowProbeMillivolts = prefs.getFloat("low_probe_mv", phCalibration.lowProbeMillivolts);
    phCalibration.lowPh = prefs.getFloat("low_ph", phCalibration.lowPh);
    phCalibration.highAdsMillivolts = prefs.getFloat("high_ads_mv", phCalibration.highAdsMillivolts);
    phCalibration.highProbeMillivolts = prefs.getFloat("high_probe_mv", phCalibration.highProbeMillivolts);
    phCalibration.highPh = prefs.getFloat("high_ph", phCalibration.highPh);
    prefs.end();
  }
}

void drawDisplay() {
  if (!displayDirty) {
    return;
  }
  displayDirty = false;

  char line[48];

  k10.canvas->canvasText("K10 POT TITRATOR", 1, COLOR_WARN);

  if (lastPh.adcOk) {
    snprintf(line, sizeof(line), "pH %.2f  mV %.0f", lastPh.ph, lastPh.millivolts);
  } else {
    snprintf(line, sizeof(line), "pH --    mV --");
  }
  k10.canvas->canvasText(line, 3, lastPh.adcOk ? COLOR_OK : COLOR_WARN);

  if (settings.endpoint == ControlEndpoint::Millivolts) {
    snprintf(line, sizeof(line), "TARGET %.0fmV %s", settings.targetMillivolts, trendText());
  } else {
    snprintf(line, sizeof(line), "TARGET %.2fpH %s", settings.targetPh, trendText());
  }
  k10.canvas->canvasText(line, 4, COLOR_TEXT);

  snprintf(line, sizeof(line), "USED %.1f/%.0fg", consumedGrams, settings.maxConsumedGrams);
  k10.canvas->canvasText(line, 5, consumedGrams >= settings.maxConsumedGrams ? COLOR_ERROR : COLOR_OK);

  if (scaleReady) {
    snprintf(line, sizeof(line), "REACTOR %.1fg", lastScale.grams);
  } else {
    snprintf(line, sizeof(line), "REACTOR --");
  }
  k10.canvas->canvasText(line, 6, scaleReady ? COLOR_TEXT : COLOR_WARN);

  snprintf(line, sizeof(line), "STATE %s", stateLabel().c_str());
  k10.canvas->canvasText(line, 8, state == RunState::Error ? COLOR_ERROR : COLOR_WARN);

  snprintf(line, sizeof(line), "%s %s", staIpAddress != "0.0.0.0" ? "STA" : "AP", ipAddress.c_str());
  k10.canvas->canvasText(line, 9, COLOR_WARN);

  snprintf(line, sizeof(line), "PULSE %s  S %.1f/%.1f", pump.isRunning() ? "ON" : "off", sampleDeliveredGrams, settings.sampleGrams);
  k10.canvas->canvasText(line, 11, pump.isRunning() || samplePump.isRunning() ? COLOR_WARN : COLOR_MUTED);

  snprintf(line, sizeof(line), "RESULT %.*f", resultDecimals(), resultValue);
  k10.canvas->canvasText(line, 12, resultValue > 0.0f ? COLOR_OK : COLOR_MUTED);

  snprintf(line, sizeof(line), "ADC %s  SCALE %s", lastPh.adcOk ? "OK" : "NO", scaleReady ? "OK" : "NO");
  k10.canvas->canvasText(line, 13, (lastPh.adcOk && scaleReady) ? COLOR_MUTED : COLOR_WARN);

  k10.canvas->updateCanvas();
}

void setup() {
  Serial.begin(115200);
  AuthCredential factoryCredential, administratorCredential;
  bool factoryLoaded = authStore.loadFactory(factoryCredential);
  bool administratorLoaded = authStore.loadAdministrator(administratorCredential);
  authStorageReady = factoryLoaded;
  if (factoryLoaded) authManager.setFactoryCredential(factoryCredential);
  if (administratorLoaded) authManager.setAdministratorCredential(administratorCredential);
  loadWifiSettings();
  startNetwork();

  k10.begin();
  k10.initScreen(SCREEN_DIR);
  k10.creatCanvas();
  k10.setScreenBackground(COLOR_BG);
  Wire.begin();

  pump.begin(titrantPumpServo, TITRANT_PUMP_PIN, titrantPumpRunUs);
  samplePump.begin(samplePumpServo, SAMPLE_PUMP_PIN, samplePumpRunUs);

  phReady = phSensor.begin();
  scaleReady = scaleSensor.begin();

  loadCalibration();
  pump.setRunPulseUs(titrantPumpRunUs);
  samplePump.setRunPulseUs(samplePumpRunUs);
  loadSelectedMethod();

  if (!phReady || !scaleReady) {
    stopReason = TitrationStopReason::InvalidReading;
    setState(RunState::Error, "Check I2C");
  } else {
    lastPh = phSensor.read();
    ScaleReading rawScale = scaleSensor.read();
    scaleFilter.reset(rawScale.ok ? rawScale.grams : 0.0f);
    lastScale = scaleFilter.apply(rawScale, false);
    scaleReady = lastScale.ok;
    setState(RunState::SetupMode, "Ready");
  }
}

void loop() {
  updatePumpTimeouts();
  if (otaReady) {
    ArduinoOTA.handle();
  }
  updatePumpTimeouts();
  if (webReady) {
    server.handleClient();
  }
  updatePumpTimeouts();
  updateNetworkStatus();
  if (restartPending && millis() - restartAtMs >= RESTART_DELAY_MS) {
    pump.stop();
    samplePump.stop();
    ESP.restart();
  }
  handleButton(readButtons());
  sampleSensors();
  if (state == RunState::Calibrating) {
    runCalibration();
  } else {
    runController();
  }
  drawDisplay();
}

#endif
