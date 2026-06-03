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

UNIHIKER_K10 k10;
Servo titrantPumpServo;
Servo samplePumpServo;
WebServer server(80);
Preferences preferences;

const uint8_t SCREEN_DIR = 2;
const uint8_t ADS1115_ADDR = 0x49;
const uint8_t SCALE_ADDR = 0x64;
const uint8_t PH_ADC_CHANNEL = 0;
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
const int PUMP_DOSE_US = 1000;
const uint32_t SAMPLE_INTERVAL_MS = 2000;
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

class PumpController {
public:
  void begin(Servo &servoRef, int pin) {
    servo = &servoRef;
    servo->setPeriodHertz(50);
    servo->attach(pin, 500, 2500);
    stop();
  }

  void stop() {
    runUntilMs = 0;
    writeStop();
  }

  void runForMs(uint16_t ms) {
    runUntilMs = millis() + ms;
    writeRun();
  }

  void runContinuous() {
    runUntilMs = millis() + 30000; // 30s max safety
    writeRun();
  }

  void update() {
    if (runUntilMs > 0 && millis() >= runUntilMs) {
      stop();
    }
  }

  bool isRunning() const {
    return runUntilMs > 0 && millis() < runUntilMs;
  }

private:
  Servo *servo = nullptr;
  uint32_t runUntilMs = 0;

  void writeStop() {
    if (servo != nullptr) {
      servo->writeMicroseconds(PUMP_STOP_US);
    }
  }

  void writeRun() {
    if (servo != nullptr) {
      servo->writeMicroseconds(PUMP_DOSE_US);
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
RunState pausedFromState = RunState::Running;
TitrationStopReason stopReason = TitrationStopReason::None;

PhReading lastPh;
ScaleReading lastScale;
PhFilter phFilter;
TitrationDynamics phDynamics;
float titrantPumpFlowRateGps = 0.0f;
float samplePumpFlowRateGps = 0.0f;
float initialBottleWeight = 0.0f;
float sampleStartWeight = 0.0f;
float sampleDeliveredGrams = 0.0f;
float consumedGrams = 0.0f;
float resultValue = 0.0f;
uint32_t stateStartedMs = 0;
uint32_t runStartedMs = 0;
uint32_t endpointHoldStartedMs = 0;
uint16_t activePulseMs = 0;
uint16_t activeSettleMs = 0;
uint8_t sensorFaultCount = 0;
bool phReady = false;
bool scaleReady = false;
bool phSampleFresh = false;
bool sensorFault = false;
bool displayDirty = true;
bool webReady = false;
bool otaReady = false;
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
  float manualResultFactor = 1.0f;
};

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

void updatePumpTimeouts() {
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

float activeControlValue() {
  return settings.endpoint == ControlEndpoint::Millivolts ? lastPh.millivolts : lastPh.ph;
}

float activeControlTarget() {
  return controlTarget(settings);
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
  aux.manualResultFactor = preset.manualResultFactor;
  return aux;
}

MethodAuxValues loadMethodAux(TitrationMethod method) {
  MethodAuxValues aux = defaultMethodAux(method);
  Preferences prefs;
  if (prefs.begin("method_aux", true)) {
    String molarityKey = methodAuxKey(method, "m");
    String blankKey = methodAuxKey(method, "blank");
    String factorKey = methodAuxKey(method, "factor");
    aux.titrantMolarity = prefs.getFloat(molarityKey.c_str(), aux.titrantMolarity);
    aux.blankGrams = prefs.getFloat(blankKey.c_str(), aux.blankGrams);
    aux.manualResultFactor = prefs.getFloat(factorKey.c_str(), aux.manualResultFactor);
    prefs.end();
  }
  aux.titrantMolarity = constrain(aux.titrantMolarity, 0.0001f, 10.0f);
  aux.blankGrams = constrain(aux.blankGrams, 0.0f, 1000.0f);
  aux.manualResultFactor = constrain(aux.manualResultFactor, -1000000.0f, 1000000.0f);
  return aux;
}

void applyMethodAux(TitrationSettings &targetSettings, const MethodAuxValues &aux) {
  targetSettings.titrantMolarity = aux.titrantMolarity;
  targetSettings.blankGrams = aux.blankGrams;
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
    String factorKey = methodAuxKey(method, "factor");
    prefs.putFloat(molarityKey.c_str(), settings.titrantMolarity);
    prefs.putFloat(blankKey.c_str(), settings.blankGrams);
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
  phDynamics.reset();
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
         absoluteFloat(preset.maxConsumedGrams - settings.maxConsumedGrams) <= 0.001f &&
         absoluteFloat(preset.sampleGrams - settings.sampleGrams) <= 0.001f &&
         preset.resultFormula == settings.resultFormula &&
         absoluteFloat(preset.controlBand - settings.controlBand) <= 0.001f &&
         absoluteFloat(preset.stableDelta - settings.stableDelta) <= 0.001f &&
         preset.holdSeconds == settings.holdSeconds &&
         preset.minSettleSeconds == settings.minSettleSeconds &&
         preset.maxSettleSeconds == settings.maxSettleSeconds &&
         preset.maxTimeSeconds == settings.maxTimeSeconds;
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
  pump.stop();
  samplePump.stop();
  phFilter.reset();
  phDynamics.reset();
  scaleFilter.reset(scaleReady ? lastScale.grams : 0.0f);
  sensorFaultCount = 0;
  sensorFault = false;
  sampleStartWeight = scaleReady ? lastScale.grams : 0.0f;
  sampleDeliveredGrams = 0.0f;
  initialBottleWeight = scaleReady ? lastScale.grams : 0.0f;
  consumedGrams = 0.0f;
  resultValue = 0.0f;
  phReady = false;
  phSampleFresh = false;
  stopReason = TitrationStopReason::None;
  displayDirty = true;
}

bool startTitration() {
  if (!lastPh.adcOk || !scaleReady || sensorFault) {
    pump.stop();
    samplePump.stop();
    stopReason = TitrationStopReason::InvalidReading;
    setState(RunState::Error, "Sensor missing");
    return false;
  }

  pump.stop();
  samplePump.stop();
  phFilter.reset();
  phDynamics.reset();
  scaleFilter.reset(lastScale.grams);
  sensorFaultCount = 0;
  sensorFault = false;
  sampleStartWeight = lastScale.grams;
  sampleDeliveredGrams = 0.0f;
  initialBottleWeight = lastScale.grams;
  consumedGrams = 0.0f;
  resultValue = 0.0f;
  runStartedMs = 0;
  endpointHoldStartedMs = 0;
  phReady = false;
  stopReason = TitrationStopReason::None;
  runStartedMs = millis();
  endpointHoldStartedMs = 0;
  if (settings.sampleGrams <= 0.01f) {
    setState(RunState::FilterWarmup, "Stabilizing pH");
  } else {
    samplePump.runContinuous();
    setState(RunState::SampleFilling, "Filling sample");
  }
  return true;
}

void stopTitration(const String &message) {
  pump.stop();
  samplePump.stop();
  stopReason = TitrationStopReason::None;
  setState(RunState::Done, message);
}

bool isActiveState() {
  return state == RunState::SampleFilling ||
         state == RunState::FilterWarmup ||
         state == RunState::Running ||
         state == RunState::Dosing ||
         state == RunState::Settling;
}

void pauseTitration() {
  if (!isActiveState()) {
    return;
  }
  pausedFromState = state;
  pump.stop();
  samplePump.stop();
  setState(RunState::Paused, "Paused");
}

void resumeTitration() {
  if (state != RunState::Paused) {
    return;
  }
  if (pausedFromState == RunState::SampleFilling && sampleDeliveredGrams < settings.sampleGrams) {
    samplePump.runContinuous();
    setState(RunState::SampleFilling, "Filling sample");
  } else if (pausedFromState == RunState::FilterWarmup || !phReady) {
    pump.stop();
    samplePump.stop();
    setState(RunState::FilterWarmup, "Stabilizing pH");
  } else {
    pump.stop();
    samplePump.stop();
    setState(RunState::Running, "Checking");
  }
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

  if (event == ButtonEvent::ABLong) {
    pump.stop();
    samplePump.stop();
    stopReason = TitrationStopReason::None;
    setState(RunState::Done, "Emergency stop");
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
        startTitration();
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
        resumeTitration();
      }
      break;

    case RunState::Done:
    case RunState::Error:
      if (event == ButtonEvent::ABShort) {
        resetRunData();
        setState(RunState::SetupMode, "Reset");
      }
      break;

    default:
      if (event == ButtonEvent::ABShort) {
        pauseTitration();
      }
      break;
  }
}

void sampleSensors() {
  static uint32_t lastSampleMs = 0;
  phSampleFresh = false;
  if (millis() - lastSampleMs < SAMPLE_INTERVAL_MS) {
    return;
  }
  lastSampleMs = millis();

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
      stopReason = TitrationStopReason::SensorFault;
      setState(RunState::Error, "SENSOR_FAULT");
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
      if (phReady) {
        phDynamics.add(activeControlValue(), millis());
      }
    } else {
      phReady = false;
    }
  } else {
    lastPh = rawPh;
    phReady = false;
  }

  ScaleReading rawScale = scaleSensor.read();
  lastScale = scaleFilter.apply(rawScale, isActiveState());
  scaleReady = lastScale.ok;
  if (scaleReady) {
    if (state == RunState::SampleFilling) {
      float delivered = computeSampleGainGrams(sampleStartWeight, lastScale.grams);
      if (delivered > sampleDeliveredGrams) {
        sampleDeliveredGrams = delivered;
      }
    }
    float consumed = computeSampleGainGrams(initialBottleWeight, lastScale.grams);
    if (consumed > consumedGrams) {
      consumedGrams = consumed;
    }
  }
  displayDirty = true;
}

void runController() {
  updatePumpTimeouts();

  if (state == RunState::SampleFilling) {
    if (!scaleReady) {
      pump.stop();
      samplePump.stop();
      stopReason = TitrationStopReason::InvalidReading;
      setState(RunState::Error, "Scale error");
      return;
    }

    if (sampleDeliveredGrams >= settings.sampleGrams) {
      samplePump.stop();
      phFilter.reset();
      phReady = false;
      initialBottleWeight = lastScale.grams;
      scaleFilter.reset(lastScale.grams);
      consumedGrams = 0.0f;
      setState(RunState::FilterWarmup, "Stabilizing pH");
    } else {
      samplePump.runContinuous();
      statusLine = String("Sample ") + String(sampleDeliveredGrams, 1) + "/" + String(settings.sampleGrams, 1) + "g";
      displayDirty = true;
    }
    return;
  }

  if (state == RunState::FilterWarmup) {
    pump.stop();
    samplePump.stop();
    if (sensorFault) {
      return;
    }
    if (phReady) {
      setState(RunState::Running, "Checking");
    }
    return;
  }

  if (state == RunState::Dosing && activePulseMs > 0 && millis() - stateStartedMs >= activePulseMs) {
    pump.stop();
    setState(RunState::Settling, "Settling");
    return;
  }

  if (state == RunState::Settling) {
    pump.stop();
    uint32_t elapsed = millis() - stateStartedMs;
    uint16_t settleMs = activeSettleMs > 0 ? activeSettleMs : SETTLING_TIME_MS;
    if (elapsed < settleMs) {
      return;
    }
    uint32_t maxSettleMs = (uint32_t)settings.maxSettleSeconds * 1000UL;
    if (maxSettleMs < settleMs) {
      maxSettleMs = settleMs;
    }
    if (elapsed < maxSettleMs && (!phSampleFresh || !phDynamics.isSettledWithin(settings.stableDelta))) {
      statusLine = String("Settling ") + endpointText();
      displayDirty = true;
      return;
    }
    setState(RunState::Running, "Checking");
    return;
  }

  if (state != RunState::Running && state != RunState::Dosing) {
    return;
  }

  if (!phSampleFresh) {
    return;
  }

  if (!phReady || !scaleReady || sensorFault) {
    pump.stop();
    samplePump.stop();
    stopReason = TitrationStopReason::InvalidReading;
    if (sensorFault) {
      stopReason = TitrationStopReason::SensorFault;
    }
    setState(RunState::Error, reasonLabel(stopReason));
    return;
  }

  if (runStartedMs > 0 && settings.maxTimeSeconds > 0 &&
      millis() - runStartedMs >= (uint32_t)settings.maxTimeSeconds * 1000UL) {
    pump.stop();
    samplePump.stop();
    stopReason = TitrationStopReason::TimeLimit;
    setState(RunState::Error, "Time limit");
    return;
  }

  if (isEndpointReached(settings, activeControlValue())) {
    if (endpointHoldStartedMs == 0) {
      endpointHoldStartedMs = millis();
    }
    uint32_t holdMs = (uint32_t)settings.holdSeconds * 1000UL;
    if (holdMs == 0 || millis() - endpointHoldStartedMs >= holdMs) {
      pump.stop();
      stopReason = TitrationStopReason::TargetReached;
      resultValue = computeTitrationResult(settings, activeTitrantMolarity(), consumedGrams, settings.sampleGrams);
      setState(RunState::Done, reasonLabel(stopReason));
    } else {
      pump.stop();
      statusLine = String("Holding ") + endpointText();
      displayDirty = true;
    }
    return;
  }
  endpointHoldStartedMs = 0;

  TitrationDecision decision = decideAdaptiveDose(
      settings, activeControlValue(), consumedGrams, phDynamics);

  if (decision.action == TitrationAction::Dose) {
    activePulseMs = decision.pumpPulseMs;
    activeSettleMs = decision.settleMs;
    pump.runForMs(decision.pumpPulseMs);
    setState(RunState::Dosing, String("Pulse ") + String(decision.pumpPulseMs) + "ms");
  } else if (decision.action == TitrationAction::Done) {
    bool wasRunning = pump.isRunning();
    pump.stop();
    stopReason = decision.reason;
    if (wasRunning) {
      setState(RunState::Settling, "Settling");
    } else {
      resultValue = computeTitrationResult(settings, activeTitrantMolarity(), consumedGrams, settings.sampleGrams);
      setState(RunState::Done, reasonLabel(stopReason));
    }
  } else {
    pump.stop();
    samplePump.stop();
    stopReason = decision.reason;
    setState(RunState::Error, reasonLabel(stopReason));
  }
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
  page += state == RunState::Paused ? F("<a class='btn primary' href='/action?cmd=start'>Resume</a>") : F("<a class='btn primary' href='/action?cmd=start'>Start</a>");
  page += F("<a class='btn' href='/action?cmd=stop'>Pause</a>");
  page += F("<a class='btn' href='/action?cmd=tare'>Tare scale</a>");
  page += F("<a class='btn ghost' href='/action?cmd=reset'>Reset</a>");
  page += F("<a class='btn danger' href='/action?cmd=panic'>Emergency stop</a>");
  page += F("</div><p class='tiny'>ADC ");
  page += lastPh.adcOk ? F("OK") : F("NO DATA");
  page += F(" / raw ");
  page += String(lastPh.raw);
  page += F(" / pH valid ");
  page += phReady ? F("yes") : F("no");
  page += F(" / scale ");
  page += scaleReady ? F("OK") : F("NO");
  page += F("</p><p id='netdetail' class='tiny'>AP ");
  page += htmlEscape(apIpAddress);
  page += F(" / STA ");
  page += htmlEscape(staIpAddress);
  page += F(" / OTA host ");
  page += htmlEscape(String(OTA_HOSTNAME));
  page += F("</p><p class='tiny'><a class='ghost' href='/json'>JSON status</a></p></div>");
  page += F("<div class='card full'><h2>Run Data</h2><div class='chartbar'>");
  page += F("<label>X axis<select id='chartX'><option value='time'>Time s</option><option value='used'>Used g</option></select></label>");
  page += F("<label>Y axis<select id='chartY'><option value='auto'>Endpoint</option><option value='ph'>pH</option><option value='mv'>mV</option></select></label>");
  page += F("<button id='curveClear' type='button'>Clear</button><button id='eqpAuto' type='button'>Auto EQP</button><button id='learnParams' type='button'>Suggest Params</button><button id='curveCsv' type='button'>CSV</button><button id='curveJson' type='button'>JSON</button>");
  page += F("</div><canvas id='curveCanvas' class='chart' width='820' height='260'></canvas><p id='curveInfo' class='tiny'>0 points</p><p id='eqpInfo' class='tiny'>EQP waits for dose changes. Click the curve to correct the candidate point.</p><p id='learnInfo' class='tiny'>Suggestions wait for at least 4 dose-change points.</p></div></section>");

  page += F("<section id='tab-cal' class='panel'><div class='card full'><h2>Calibration Start</h2><div class='row'>");
  page += F("<a class='btn' href='/action?cmd=ready'>Enter ready</a>");
  page += F("<a class='btn primary' href='/action?cmd=calibrate'>Calibrate pumps</a>");
  page += F("<a class='btn' href='/action?cmd=scale_calibrate'>Tare scale</a>");
  page += F("<a class='btn' href='/action?cmd=ph_signal_calibrate'>Reset pH/mV filter</a>");
  page += F("</div><p class='tiny'>Enter ready stops both pumps and puts the controller in READY. Pump calibration can then be started here or with the K10 B key. Each pump runs 2 s, then waits 5 s before reading the scale.</p></div>");
  page += F("<form action='/set' method='get' class='split'>");
  page += F("<div class='card'><h2>Pump Flow</h2><div class='row'>");
  page += F("<label>Titrant pump g/s<input name='titrant_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(titrantPumpFlowRateGps, 3);
  page += F("'></label><label>Sample pump g/s<input name='sample_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(samplePumpFlowRateGps, 3);
  page += F("'></label></div><p class='tiny'>Measures each pump independently by mass. Re-run after tubing, pump head, liquid, or viscosity changes.</p></div>");
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

  page += F("<section id='tab-manual' class='panel'><div class='card full'><h2>Manual Operation</h2><form id='manualForm' action='/action' method='get' class='row'><label class='mini'>Run seconds<input name='sec' type='number' min='0.1' max='30' step='0.1' value='1.0'></label><button class='btn' name='cmd' value='manual_titrant' type='submit'>Run titrant pump</button><button class='btn' name='cmd' value='manual_sample' type='submit'>Run sample pump</button><button class='btn danger' name='cmd' value='manual_stop' type='submit'>Stop pumps</button></form><p class='tiny'>Manual pump actions are blocked while titration or calibration is active. Use seconds here for priming tubing and experiment preparation.</p></div></section>");

  page += F("<section id='tab-admin' class='panel'><div class='split'><div><form action='/set' method='get' class='card'><h2>Settings</h2><div class='row'>");
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
  page += F("'></label></div><div class='row' style='margin-top:10px'><label>Manual factor<input id='manualFactorInput' name='manual_factor' type='number' min='-1000000' max='1000000' step='0.0001' value='");
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
  page += F("</span> g/s</p><p class='tiny'>Use pH or mV as the endpoint, then choose whether dosing makes that signal rise or fall.</p><button class='primary' type='submit'>Save settings</button></form>");

  page += F("<form action='/set' method='get' class='card' style='margin-top:10px'><h2>WiFi</h2><div class='row'>");
  page += F("<label>SSID<input name='ssid' maxlength='32' value='");
  page += htmlEscape(wifiSsid);
  page += F("' placeholder='Leave empty for AP only'></label>");
  page += F("<label>Password<input name='wifi_password' type='password' maxlength='64' placeholder='Leave blank to keep'></label>");
  page += F("</div><p class='tiny'>AP stays on. Blank SSID disables STA. Changing WiFi restarts the controller.</p>");
  page += F("<button class='primary' type='submit'>Save WiFi</button></form></div></div></section>");
  page += F("<section id='tab-guide' class='panel'><div class='guide'>");
  page += F("<div class='card'><h2>Method and Endpoint</h2><p><span class='term'>Method</span> loads a preset group of endpoint, titrant, result formula, and control defaults. Manual keeps custom values.</p><p><span class='term'>Endpoint</span> selects the control signal. Use pH for acid/base endpoint work, or mV for potentiometric endpoints.</p><p><span class='term'>Signal trend</span> tells the controller whether dosing should raise or lower the endpoint signal.</p><p><span class='term'>Target pH / mV</span> is the EP stop value. Only the active endpoint is used for control.</p></div>");
  page += F("<div class='card'><h2>Endpoint Control</h2><p><span class='term'>Control band</span> is the near-target zone. Larger values slow dosing earlier; smaller values dose faster but risk overshoot.</p><p><span class='term'>Stable delta/s</span> is the allowed signal drift while settling. Lower values wait for a flatter response.</p><p><span class='term'>Hold s</span> confirms the endpoint after it is reached. If the signal moves back out, dosing resumes.</p><p><span class='term'>Min / Max settle s</span> controls wait time after each pulse. Slow probes or slow reactions need longer settling.</p><p><span class='term'>Max time s</span> stops a run that takes too long.</p></div>");
  page += F("<div class='card'><h2>Calibration</h2><p><span class='term'>Enter ready</span> stops both pumps before any calibration action.</p><p><span class='term'>Pump flow</span> measures titrant and sample pump delivery in g/s. Recalibrate after tubing, pump head, or liquid changes.</p><p><span class='term'>Scale</span> uses tare for the reactor baseline; scale factor is the grams conversion value.</p><p><span class='term'>pH/mV sensor</span> stores two buffer points and reports slope %, pH7 offset, and status. Reset pH/mV filter only restarts acquisition.</p><p><span class='term'>Titrant standard</span> is configured in Admin through molarity, blank, and result formula.</p></div>");
  page += F("<div class='card'><h2>Dosing and Results</h2><p><span class='term'>Titrant</span> selects the known solution. Manual mol/L is used only when titrant is Manual.</p><p><span class='term'>Max used g</span> is the safety limit for titrant consumption.</p><p><span class='term'>Sample g</span> is the sample mass delivered by the P1 pump before titration.</p><p><span class='term'>Result formula</span> controls only calculation and display; it does not change pump control.</p><p><span class='term'>Blank g</span> subtracts blank titration consumption before calculating and is saved per Method.</p><p><span class='term'>Manual factor</span> uses result = net titrant g x factor / sample g for custom tests. Manual mol/L, blank, and factor are method auxiliary values.</p></div>");
  page += F("<div class='card'><h2>Run Data and EQP</h2><p><span class='term'>Time s</span> is the safer default X axis because data keeps moving even while used g is unchanged.</p><p><span class='term'>Used g</span> is useful for final analysis after enough dose changes have happened.</p><p><span class='term'>Auto EQP</span> marks the largest d(signal)/d(used g) candidate. It is an analysis point, not an automatic stop command.</p><p><span class='term'>Suggest Params</span> estimates control band, stable delta, and settle time from the current curve. It does not apply settings automatically.</p><p>Click the curve to manually correct the EQP point, then export CSV or JSON to save the run on the computer.</p></div>");
  page += F("</div></section>");
  page += F("<script>");
  page += F("function text(id,v){var e=document.getElementById(id);if(e)e.textContent=v}");
  page += F("function html(id,v){var e=document.getElementById(id);if(e)e.innerHTML=v}");
  page += F("var curve=[],curveStart=0,eqpManual=null,lastPlot=[];function num(v){return Number(v||0)}function curveTarget(d){return d.endpoint==='mV'?num(d.target_mv):num(d.target_ph)}");
  page += F("function recordCurve(d){if(!d.adc_ok)return;var now=Date.now();if(!curveStart)curveStart=now;curve.push({ts:new Date(now).toISOString(),elapsed_s:(now-curveStart)/1000,ph:num(d.ph),mv:num(d.mv),used_g:num(d.used_g),sample_g:num(d.sample_delivered_g),endpoint:d.endpoint,target:curveTarget(d),trend:d.mode,state:d.state,pump:!!d.pump,pulse_ms:num(d.pump_pulse_ms),status:d.status,method:d.method,result_value:num(d.result_value),result_unit:d.result_unit,result_formula:d.result_formula,blank_g:num(d.blank_g),manual_factor:num(d.manual_factor)});if(curve.length>2000)curve.shift();drawCurve()}");
  page += F("function dosePoints(){var pts=[];curve.forEach(function(p){if(!isFinite(p.used_g))return;if(pts.length&&Math.abs(p.used_g-pts[pts.length-1].used_g)<0.01){pts[pts.length-1]=p}else pts.push(p)});return pts}");
  page += F("function analyzeEqp(){var pts=dosePoints();if(pts.length<3)return null;var yk=pts[pts.length-1].endpoint==='mV'?'mv':'ph',best=null;for(var i=1;i<pts.length;i++){var dx=pts[i].used_g-pts[i-1].used_g;if(Math.abs(dx)<0.01)continue;var dy=pts[i][yk]-pts[i-1][yk],s=Math.abs(dy/dx);if(!best||s>best.slope){best={mode:'auto',index:i,used_g:pts[i].used_g,elapsed_s:pts[i].elapsed_s,signal:pts[i][yk],ph:pts[i].ph,mv:pts[i].mv,endpoint:pts[i].endpoint,slope:s}}}return best}");
  page += F("function currentEqp(){var a=analyzeEqp();return eqpManual||a}function eqpText(e){if(!e)return 'EQP waits for at least 3 dose-change points.';var v=e.endpoint==='mV'?e.signal.toFixed(0)+' mV':e.signal.toFixed(2)+' pH';return (e.mode==='manual'?'Manual':'Auto')+' EQP: '+e.used_g.toFixed(2)+' g, '+v+', slope '+e.slope.toFixed(e.endpoint==='mV'?1:3)+' '+e.endpoint+'/g'}");
  page += F("function median(a){if(!a.length)return 0;var b=a.slice().sort(function(x,y){return x-y}),m=Math.floor(b.length/2);return b.length%2?b[m]:(b[m-1]+b[m])/2}function clamp(v,a,b){return Math.max(a,Math.min(b,v))}");
  page += F("function suggestParams(){var pts=dosePoints();if(pts.length<4){text('learnInfo','Need at least 4 dose-change points before recommending parameters.');return}var ep=pts[pts.length-1].endpoint,yk=ep==='mV'?'mv':'ph',dose=[],slope=[],timeSlope=[];for(var i=1;i<pts.length;i++){var dx=pts[i].used_g-pts[i-1].used_g,dy=pts[i][yk]-pts[i-1][yk],dt=pts[i].elapsed_s-pts[i-1].elapsed_s;if(Math.abs(dx)>=0.01){dose.push(Math.abs(dx));slope.push(Math.abs(dy/dx))}if(dt>0)timeSlope.push(Math.abs(dy/dt))}if(!slope.length){text('learnInfo','Need changing used g values before recommending parameters.');return}var md=median(dose),ms=Math.max.apply(null,slope),drift=median(timeSlope);var mv=ep==='mV';var band=mv?clamp(ms*md*2.0,10,120):clamp(ms*md*2.0,0.10,1.50);var stable=mv?clamp(Math.max(drift*1.5,0.3),0.3,5.0):clamp(Math.max(drift*1.5,0.003),0.003,0.050);var steep=mv?ms>80:ms>2.0;var minSettle=steep?10:5,maxSettle=steep?60:30;var msg='Suggested: control band '+band.toFixed(mv?1:3)+', stable delta/s '+stable.toFixed(mv?1:3)+', min/max settle '+minSettle+'/'+maxSettle+' s';msg+=' from max slope '+ms.toFixed(mv?1:3)+' '+ep+'/g and median dose '+md.toFixed(2)+' g. Review before applying in Admin.';text('learnInfo',msg)}");
  page += F("function drawCurve(){var c=document.getElementById('curveCanvas');if(!c)return;var r=c.getBoundingClientRect();if(r.width>0&&c.width!==Math.floor(r.width)){c.width=Math.floor(r.width);c.height=260}var g=c.getContext('2d'),w=c.width,h=c.height,l=58,t=18,ri=18,b=36;lastPlot=[];g.clearRect(0,0,w,h);g.fillStyle='#071014';g.fillRect(0,0,w,h);g.strokeStyle='#244c59';g.lineWidth=1;g.strokeRect(l,t,w-l-ri,h-t-b);text('curveInfo',curve.length+' points');text('eqpInfo',eqpText(currentEqp()));if(curve.length<2)return;var xs=document.getElementById('chartX'),ys=document.getElementById('chartY');var xk=xs&&xs.value==='used'?'used_g':'elapsed_s';var ysel=ys?ys.value:'auto';var yk=ysel==='auto'?(curve[curve.length-1].endpoint==='mV'?'mv':'ph'):ysel;var pts=[];curve.forEach(function(p){if(xk==='used_g'&&pts.length&&Math.abs(p.used_g-pts[pts.length-1].used_g)<0.01){pts[pts.length-1]=p}else pts.push(p)});if(pts.length<2){text('curveInfo',curve.length+' points / waiting for '+(xk==='used_g'?'dose change':'more samples'));return}var minx=pts[0][xk],maxx=pts[0][xk],miny=pts[0][yk],maxy=miny;pts.forEach(function(p){if(p[xk]<minx)minx=p[xk];if(p[xk]>maxx)maxx=p[xk];if(p[yk]<miny)miny=p[yk];if(p[yk]>maxy)maxy=p[yk]});if(maxx===minx)maxx=minx+1;if(maxy===miny)maxy=miny+1;var ypad=(maxy-miny)*0.08;miny-=ypad;maxy+=ypad;var px=function(x){return l+(x-minx)/(maxx-minx)*(w-l-ri)};var py=function(y){return h-b-(y-miny)/(maxy-miny)*(h-t-b)};g.strokeStyle='#67f09a';g.lineWidth=2;g.beginPath();pts.forEach(function(p,i){var x=px(p[xk]),y=py(p[yk]);lastPlot.push({x:x,y:y,p:p,yk:yk});if(i)g.lineTo(x,y);else g.moveTo(x,y)});g.stroke();var e=currentEqp();if(e){var ex=px(xk==='used_g'?e.used_g:e.elapsed_s),ey=py(yk==='mv'?e.mv:e.ph);g.strokeStyle='#ffd15c';g.fillStyle='#ffd15c';g.lineWidth=1;g.beginPath();g.moveTo(ex,t);g.lineTo(ex,h-b);g.stroke();g.beginPath();g.arc(ex,ey,5,0,6.283);g.fill()}g.fillStyle='#8db0bd';g.font='12px Verdana';g.fillText(xk==='used_g'?'used g':'time s',w-86,h-10);g.fillText(yk,l,13);g.fillText(miny.toFixed(yk==='ph'?2:0),8,h-b);g.fillText(maxy.toFixed(yk==='ph'?2:0),8,t+5)}");
  page += F("function chooseEqpAt(ev){if(!lastPlot.length)return;var c=ev.currentTarget,r=c.getBoundingClientRect(),x=(ev.clientX-r.left)*c.width/r.width,y=(ev.clientY-r.top)*c.height/r.height,b=null;lastPlot.forEach(function(pt){var d=(pt.x-x)*(pt.x-x)+(pt.y-y)*(pt.y-y);if(!b||d<b.d)b={d:d,pt:pt}});if(!b)return;var auto=analyzeEqp(),p=b.pt.p,yk=b.pt.yk;eqpManual={mode:'manual',used_g:p.used_g,elapsed_s:p.elapsed_s,signal:p[yk],ph:p.ph,mv:p.mv,endpoint:yk==='mv'?'mV':'pH',slope:auto?auto.slope:0};drawCurve()}");
  page += F("function exportCurve(fmt){if(!curve.length)return;var eqp=currentEqp();var data,mime,name;if(fmt==='json'){data=JSON.stringify({eqp:eqp,points:curve},null,2);mime='application/json';name='titration-data.json'}else{var keys=Object.keys(curve[0]).concat(['eqp_used_g','eqp_signal','eqp_slope','eqp_mode']);data=keys.join(',')+'\\n'+curve.map(function(r){return keys.map(function(k){var v=k==='eqp_used_g'&&eqp?eqp.used_g:k==='eqp_signal'&&eqp?eqp.signal:k==='eqp_slope'&&eqp?eqp.slope:k==='eqp_mode'&&eqp?eqp.mode:r[k];return String(v===undefined?'':v).replace(/\"/g,'\"\"')}).join(',')}).join('\\n');mime='text/csv';name='titration-data.csv'}var a=document.createElement('a');a.href=URL.createObjectURL(new Blob([data],{type:mime}));a.download=name;a.click();setTimeout(function(){URL.revokeObjectURL(a.href)},1000)}");
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
  page += F("}catch(e){}}setInterval(poll,2000);");
  page += F("function activateTab(name){var p=document.getElementById('tab-'+name);if(!p)return;document.querySelectorAll('.tab').forEach(function(x){x.classList.toggle('active',x.dataset.tab===name)});document.querySelectorAll('.panel').forEach(function(x){x.classList.remove('active')});p.classList.add('active')}");
  page += F("document.querySelectorAll('.tab').forEach(function(b){b.onclick=function(){activateTab(b.dataset.tab);location.hash=b.dataset.tab}});var initial=(location.hash||'#run').slice(1);activateTab(initial);");
  page += F("['chartX','chartY'].forEach(function(id){var e=document.getElementById(id);if(e)e.onchange=drawCurve});var cv=document.getElementById('curveCanvas');if(cv)cv.onclick=chooseEqpAt;var ea=document.getElementById('eqpAuto');if(ea)ea.onclick=function(){eqpManual=null;drawCurve()};var lp=document.getElementById('learnParams');if(lp)lp.onclick=suggestParams;var cc=document.getElementById('curveClear');if(cc)cc.onclick=function(){curve=[];curveStart=0;eqpManual=null;text('learnInfo','Suggestions wait for at least 4 dose-change points.');drawCurve()};var ec=document.getElementById('curveCsv');if(ec)ec.onclick=function(){exportCurve('csv')};var ej=document.getElementById('curveJson');if(ej)ej.onclick=function(){exportCurve('json')};drawCurve();");
  page += F("var presets={");
  appendMethodPresetJs(page, TitrationMethod::PhEndpoint, true);
  appendMethodPresetJs(page, TitrationMethod::MvEndpoint, true);
  appendMethodPresetJs(page, TitrationMethod::EdtaHardness, true);
  appendMethodPresetJs(page, TitrationMethod::Manual, false);
  page += F("};");
  page += F("function setv(id,v){var e=document.getElementById(id);if(e)e.value=v}var ms=document.getElementById('methodSelect');if(ms)ms.addEventListener('change',function(){var p=presets[ms.value];if(!p)return;setv('endpointSelect',p.endpoint);setv('trendSelect',p.trend);setv('targetPhInput',p.target);setv('targetMvInput',p.target_mv);setv('maxInput',p.max);setv('sampleInput',p.sample);setv('titrantSelect',p.titrant);setv('titrantMInput',p.titrant_m);setv('resultFormulaSelect',p.result_formula);setv('blankInput',p.blank_g);setv('manualFactorInput',p.manual_factor);setv('controlBandInput',p.control_band);setv('stableDeltaInput',p.stable_delta);setv('holdInput',p.hold_s);setv('minSettleInput',p.min_settle_s);setv('maxSettleInput',p.max_settle_s);setv('maxTimeInput',p.max_time_s)});");
  page += F("var mf=document.getElementById('manualForm');if(mf)mf.addEventListener('submit',async function(e){e.preventDefault();var fd=new FormData(mf);var cmd=e.submitter&&e.submitter.name?e.submitter.value:fd.get('cmd');fd.set('cmd',cmd);fd.set('ajax','1');try{await fetch('/action?'+new URLSearchParams(fd).toString(),{cache:'no-store'});poll()}catch(err){}});");
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

void handleSet() {
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
    methodFieldChanged = methodFieldChanged || absoluteFloat(nextMax - settings.maxConsumedGrams) > 0.001f;
    settings.maxConsumedGrams = nextMax;
  }
  if (server.hasArg("sample")) {
    float nextSample = constrain(server.arg("sample").toFloat(), 0.0f, 1000.0f);
    methodFieldChanged = methodFieldChanged || absoluteFloat(nextSample - settings.sampleGrams) > 0.001f;
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
    methodFieldChanged = methodFieldChanged || nextHold != settings.holdSeconds;
    settings.holdSeconds = nextHold;
  }
  if (server.hasArg("min_settle_s")) {
    uint16_t nextMinSettle = (uint16_t)constrain(server.arg("min_settle_s").toInt(), 1, 120);
    methodFieldChanged = methodFieldChanged || nextMinSettle != settings.minSettleSeconds;
    settings.minSettleSeconds = nextMinSettle;
  }
  if (server.hasArg("max_settle_s")) {
    uint16_t nextMaxSettle = (uint16_t)constrain(server.arg("max_settle_s").toInt(), 1, 180);
    methodFieldChanged = methodFieldChanged || nextMaxSettle != settings.maxSettleSeconds;
    settings.maxSettleSeconds = nextMaxSettle;
  }
  if (server.hasArg("max_time_s")) {
    uint16_t nextMaxTime = (uint16_t)constrain(server.arg("max_time_s").toInt(), 10, 7200);
    methodFieldChanged = methodFieldChanged || nextMaxTime != settings.maxTimeSeconds;
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
  if (server.hasArg("scale_factor")) {
    scaleSensor.setCalibrationFactor(constrain(server.arg("scale_factor").toFloat(), 1.0f, 100000.0f));
    calibrationChanged = true;
  }
  if (calibrationChanged) {
    if (absoluteFloat(phCalibration.highProbeMillivolts - phCalibration.lowProbeMillivolts) > 0.01f &&
        absoluteFloat(phCalibration.highAdsMillivolts - phCalibration.lowAdsMillivolts) > 0.01f) {
      saveCalibration();
      phFilter.reset();
      phReady = false;
    } else {
      statusLine = "Bad calibration span";
      displayDirty = true;
      redirectHome();
      return;
    }
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
      saveWifiSettings(nextSsid, nextPassword);
      wifiChanged = true;
    }
  }

  if (methodRequested && requestedMethod != currentMethod) {
    methodChanged = true;
    selectMethod(requestedMethod, true);
    endpointChanged = false;
  } else if (methodRequested && requestedMethod != TitrationMethod::Manual &&
             !methodFieldChanged && !methodMatchesPreset(requestedMethod)) {
    methodChanged = true;
    selectMethod(requestedMethod, true);
    endpointChanged = false;
  } else if (methodFieldChanged && currentMethod != TitrationMethod::Manual) {
    currentMethod = TitrationMethod::Manual;
    saveSelectedMethod();
  } else if (methodRequested) {
    saveSelectedMethod();
  }
  if (methodAuxChanged) {
    saveMethodAux(currentMethod);
  }
  if (endpointChanged) {
    phDynamics.reset();
  }
  statusLine = wifiChanged ? "WiFi saved" : (calibrationChanged ? "Calibration saved" : (methodChanged ? "Method loaded" : "Settings saved"));
  displayDirty = true;
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
  float manualSeconds = server.hasArg("sec") ? server.arg("sec").toFloat() : (server.arg("ms").toFloat() / 1000.0f);
  manualSeconds = constrain(manualSeconds, 0.1f, 30.0f);
  uint16_t manualMs = (uint16_t)(manualSeconds * 1000.0f + 0.5f);
  const char *returnTab = "run";
  if (cmd == "start") {
    if (state == RunState::Paused) {
      resumeTitration();
    } else {
      startTitration();
    }
  } else if (cmd == "stop") {
    pauseTitration();
  } else if (cmd == "panic") {
    stopTitration("Emergency stop");
  } else if (cmd == "tare") {
    tareScale();
  } else if (cmd == "reset") {
    resetRunData();
    setState(RunState::SetupMode, "Reset");
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
      phDynamics.reset();
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
      samplePump.stop();
      pump.runForMs(manualMs);
      statusLine = String("Manual titrant ") + String(manualSeconds, 1) + "s";
      displayDirty = true;
    }
  } else if (cmd == "manual_sample") {
    returnTab = "manual";
    if (!isActiveState() && state != RunState::Calibrating) {
      pump.stop();
      samplePump.runForMs(manualMs);
      statusLine = String("Manual sample ") + String(manualSeconds, 1) + "s";
      displayDirty = true;
    }
  } else if (cmd == "manual_stop") {
    returnTab = "manual";
    pump.stop();
    samplePump.stop();
    statusLine = "Manual stop";
    displayDirty = true;
  }
  updatePumpTimeouts();
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
  String json = "{";
  json += "\"adc_ok\":" + String(lastPh.adcOk ? "true" : "false");
  json += ",\"ph_valid\":" + String(phReady ? "true" : "false");
  json += ",\"raw\":" + String(lastPh.raw);
  json += ",\"ph\":" + String(lastPh.adcOk ? lastPh.ph : -1.0f, 2);
  json += ",\"mv\":" + String(lastPh.adcOk ? lastPh.millivolts : -1.0f, 0);
  json += ",\"bottle_g\":" + String(scaleReady ? lastScale.grams : -1.0f, 1);
  json += ",\"raw_bottle_g\":" + String(scaleReady ? lastScale.rawGrams : -1.0f, 1);
  json += ",\"used_g\":" + String(consumedGrams, 1);
  json += ",\"sample_g\":" + String(settings.sampleGrams, 1);
  json += ",\"sample_delivered_g\":" + String(sampleDeliveredGrams, 1);
  json += ",\"titrant\":\"" + jsonEscape(titrantLabel()) + "\"";
  json += ",\"titrant_m\":" + String(activeTitrantMolarity(), 5);
  json += ",\"result_m\":" + String(settings.resultFormula == ResultFormula::AcidBaseMolar ? resultValue : 0.0f, 5);
  json += ",\"result_value\":" + String(resultValue, (unsigned int)resultDecimals());
  json += ",\"result_unit\":\"" + jsonEscape(resultUnit()) + "\"";
  json += ",\"result_formula\":\"" + String(resultFormulaValue(settings.resultFormula)) + "\"";
  json += ",\"blank_g\":" + String(settings.blankGrams, 2);
  json += ",\"manual_factor\":" + String(settings.manualResultFactor, 4);
  json += ",\"method\":\"" + String(methodValue(currentMethod)) + "\"";
  json += ",\"method_label\":\"" + jsonEscape(methodLabel(currentMethod)) + "\"";
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
  json += ",\"pump_pulse_ms\":" + String(pump.isRunning() ? activePulseMs : 0);
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
  json += ",\"titrant_gps\":" + String(titrantPumpFlowRateGps, 4);
  json += ",\"sample_gps\":" + String(samplePumpFlowRateGps, 4);
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

void handleOta() {
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", Update.hasError() ? "FAIL" : "OK");
  if (!Update.hasError()) {
    scheduleRestart("OTA update done");
  }
}

void handleOtaUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    statusLine = "OTA upload";
    displayDirty = true;
    Serial.printf("OTA: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("OTA Success: %u bytes\n", upload.totalSize);
      statusLine = "OTA done";
    } else {
      Update.printError(Serial);
      statusLine = "OTA fail";
    }
    displayDirty = true;
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
  server.on("/set", handleSet);
  server.on("/action", handleAction);
  server.on("/json", handleJson);
  server.on("/ota", HTTP_POST, handleOta, handleOtaUpload);
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
  loadWifiSettings();
  startNetwork();

  k10.begin();
  k10.initScreen(SCREEN_DIR);
  k10.creatCanvas();
  k10.setScreenBackground(COLOR_BG);
  Wire.begin();

  pump.begin(titrantPumpServo, TITRANT_PUMP_PIN);
  samplePump.begin(samplePumpServo, SAMPLE_PUMP_PIN);

  phReady = phSensor.begin();
  scaleReady = scaleSensor.begin();

  loadCalibration();
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
    initialBottleWeight = lastScale.grams;
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
