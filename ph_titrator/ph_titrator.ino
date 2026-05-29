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
PumpController pump;
PumpController samplePump;

TitrationSettings settings;
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
float resultConcentrationM = 0.0f;
uint32_t stateStartedMs = 0;
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

String modeLabel() {
  return modeText();
}

String titrantLabel() {
  switch (settings.titrantPreset) {
    case TitrantPreset::Naoh001: return "0.01M NaOH";
    case TitrantPreset::Hcl001: return "0.01M HCl";
    case TitrantPreset::Manual: return String(settings.titrantMolarity, 4) + "M manual";
  }
  return "Unknown";
}

float activeTitrantMolarity() {
  return titrantMolarityForPreset(settings.titrantPreset, settings.titrantMolarity);
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
  statusLine = "Tare done";
  displayDirty = true;
}

void resetRunData() {
  pump.stop();
  samplePump.stop();
  phFilter.reset();
  phDynamics.reset();
  sensorFaultCount = 0;
  sensorFault = false;
  sampleStartWeight = scaleReady ? lastScale.grams : 0.0f;
  sampleDeliveredGrams = 0.0f;
  initialBottleWeight = scaleReady ? lastScale.grams : 0.0f;
  consumedGrams = 0.0f;
  resultConcentrationM = 0.0f;
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
  samplePump.runContinuous();
  phFilter.reset();
  phDynamics.reset();
  sensorFaultCount = 0;
  sensorFault = false;
  sampleStartWeight = lastScale.grams;
  sampleDeliveredGrams = 0.0f;
  initialBottleWeight = lastScale.grams;
  consumedGrams = 0.0f;
  resultConcentrationM = 0.0f;
  phReady = false;
  stopReason = TitrationStopReason::None;
  setState(RunState::SampleFilling, "Filling sample");
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
        settings.mode = settings.mode == TitrationMode::AddBase ? TitrationMode::AddAcid : TitrationMode::AddBase;
        displayDirty = true;
      } else if (event == ButtonEvent::ABShort) {
        setState(RunState::SetupTarget, "Target pH");
      }
      break;

    case RunState::SetupTarget:
      if (event == ButtonEvent::A) {
        settings.targetPh = max(0.0f, settings.targetPh - 0.05f);
        displayDirty = true;
      } else if (event == ButtonEvent::B) {
        settings.targetPh = min(14.0f, settings.targetPh + 0.05f);
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
        phDynamics.add(lastPh.ph, millis());
      }
    } else {
      phReady = false;
    }
  } else {
    lastPh = rawPh;
    phReady = false;
  }

  lastScale = scaleSensor.read();
  scaleReady = lastScale.ok;
  if (scaleReady) {
    if (state == RunState::SampleFilling) {
      sampleDeliveredGrams = computeSampleGainGrams(sampleStartWeight, lastScale.grams);
    }
    consumedGrams = computeSampleGainGrams(initialBottleWeight, lastScale.grams);
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
    if (elapsed < MAX_SETTLING_TIME_MS && (!phSampleFresh || !phDynamics.isSettled())) {
      statusLine = "Settling pH";
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

  TitrationDecision decision = decideAdaptiveDose(
      settings, lastPh.ph, consumedGrams, phDynamics);

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
      resultConcentrationM = computeSampleConcentrationMolar(activeTitrantMolarity(), consumedGrams, settings.sampleGrams);
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
  page.reserve(11000);
  int usedPercent = (int)constrain((consumedGrams / settings.maxConsumedGrams) * 100.0f, 0.0f, 100.0f);
  page += F("<!doctype html><html><head><meta charset='utf-8'>");
  page += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  page += F("<title>K10 pH Titrator</title><style>");
  page += F(":root{--bg:#071014;--panel:#0d1d24;--panel2:#102a34;--line:#244c59;--text:#eaf7ff;--muted:#8db0bd;--ok:#67f09a;--warn:#ffd15c;--bad:#ff6b6b;--blue:#7bd5ff}");
  page += F("*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 20% 0,#15333b 0,#071014 38%,#04080a 100%);color:var(--text);font-family:Verdana,Geneva,sans-serif}");
  page += F("main{max-width:880px;margin:auto;padding:16px}.top{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;margin-bottom:12px}.brand{letter-spacing:.08em;color:var(--muted);font-size:12px}.title{font-size:28px;font-weight:700;margin:2px 0 0}.pill{display:flex;flex-wrap:wrap;gap:6px;justify-content:flex-end;max-width:100%;color:var(--blue)}.pill span{border:1px solid var(--line);border-radius:999px;padding:7px 10px;background:#071820;white-space:nowrap}");
  page += F(".hero{border:1px solid var(--line);border-radius:10px;background:linear-gradient(135deg,#0f2630,#081219);padding:18px;margin-bottom:12px;display:grid;grid-template-columns:1.2fr .8fr;gap:14px}.ph{font-size:72px;line-height:.95;font-weight:800}.unit{font-size:18px;color:var(--muted);margin-left:6px}.sub{color:var(--muted);margin-top:10px}.status{display:grid;gap:8px;align-content:center}.status b{font-size:22px}");
  page += F(".grid{display:grid;grid-template-columns:repeat(6,1fr);gap:10px}.card{border:1px solid var(--line);border-radius:8px;padding:13px;background:rgba(13,29,36,.9)}.k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.07em}.v{font-size:28px;font-weight:700;margin-top:5px}.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}");
  page += F(".bar{height:10px;background:#071014;border:1px solid var(--line);border-radius:99px;overflow:hidden;margin-top:10px}.fill{height:100%;background:linear-gradient(90deg,var(--ok),var(--warn))}.split{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-top:10px}.row{display:flex;gap:8px;flex-wrap:wrap;align-items:end}label{display:grid;gap:5px;color:var(--muted);font-size:12px;min-width:130px;flex:1}");
  page += F("button,.btn,input,select{font:inherit;border-radius:7px;border:1px solid #3a6472;background:#0a1a21;color:var(--text);padding:10px 12px;text-decoration:none}button,.btn{display:inline-block;cursor:pointer;font-weight:700}.primary{background:#123b2b;border-color:#2d8a5a;color:#bfffd4}.danger{background:#351216;border-color:#8c3640;color:#ffd1d1}.ghost{color:var(--blue)}h2{margin:0 0 10px;font-size:16px}.tiny{font-size:12px;color:var(--muted)}.tabs{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}.tab{background:#071820;color:var(--blue)}.tab.active{background:#123b2b;border-color:#2d8a5a;color:#bfffd4}.panel{display:none}.panel.active{display:block}.full{margin-top:10px}.mini{max-width:170px}@media(max-width:720px){.hero,.split{grid-template-columns:1fr}.grid{grid-template-columns:repeat(2,1fr)}.ph{font-size:58px}.top{display:block}.pill{justify-content:flex-start;margin-top:8px}.pill span{border-radius:7px}}</style></head><body><main>");
  page += F("<style>.ph,.v,#status,#mv{font-variant-numeric:tabular-nums}.ph{min-height:72px}.sub{min-height:22px}</style>");

  page += F("<div class='top'><div><div class='brand'>K10 LAB CONTROLLER</div><div class='title'>pH Titrator</div></div><div id='network' class='pill'>");
  page += F("<span>");
  page += htmlEscape(networkLabel);
  page += F("</span><span>AP ");
  page += htmlEscape(apIpAddress);
  page += F("</span><span>STA ");
  page += htmlEscape(staIpAddress);
  page += otaReady ? F("</span><span>OTA ON</span>") : F("</span><span>OTA OFF</span>");
  page += F("</div></div>");

  page += F("<section class='hero'><div><div class='k'>Current pH</div><div id='ph' class='ph ");
  page += lastPh.adcOk ? (phReady ? F("ok'>") : F("warn'>")) : F("warn'>");
  page += F("<span id='phvalue'>");
  page += lastPh.adcOk ? String(lastPh.ph, 2) : F("--.--");
  page += F("</span><span class='unit'>pH</span></div><div id='mv' class='sub'><span id='mvvalue'>");
  if (lastPh.adcOk) {
    page += String(lastPh.millivolts, 0);
    page += F("</span> mV from ADS1115 A0");
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

  page += F("<section class='grid'><div class='card'><div class='k'>Target</div><div id='target' class='v'>");
  page += String(settings.targetPh, 2);
  page += F("</div></div><div class='card'><div class='k'>mV</div><div id='mvcard' class='v'>");
  page += lastPh.adcOk ? String(lastPh.millivolts, 0) : F("--");
  page += F("</div></div><div class='card'><div class='k'>Mode</div><div id='mode' class='v'>");
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

  page += F("<nav class='tabs'><button class='tab active' data-tab='run' type='button'>Run</button><button class='tab' data-tab='cal' type='button'>Calibration</button><button class='tab' data-tab='manual' type='button'>Manual</button><button class='tab' data-tab='admin' type='button'>Admin</button></nav>");
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
  page += F("</p><p class='tiny'><a class='ghost' href='/json'>JSON status</a></p></div></section>");

  page += F("<section id='tab-cal' class='panel'><div class='card full'><h2>Calibration Actions</h2><div class='row'>");
  page += F("<a class='btn' href='/action?cmd=ready'>Enter ready</a>");
  page += F("<a class='btn primary' href='/action?cmd=calibrate'>Calibrate pumps</a>");
  page += F("<a class='btn' href='/action?cmd=scale_calibrate'>Scale calibrate</a>");
  page += F("<a class='btn' href='/action?cmd=ph_signal_calibrate'>pH/mV calibrate</a>");
  page += F("</div><p class='tiny'>Enter ready stops both pumps and puts the controller in READY. Pump calibration can then be started here or with the K10 B key. Each pump runs 2 s, then waits 5 s before reading the scale.</p></div>");
  page += F("<form action='/set' method='get' class='card full'><h2>Calibration Values</h2><div class='row'>");
  page += F("<label>pH point A<input name='low_ph' type='number' min='0' max='14' step='0.01' value='");
  page += String(phCalibration.lowPh, 2);
  page += F("'></label><label>Point A probe mV<input name='low_probe_mv' type='number' min='-1000' max='1000' step='0.1' value='");
  page += String(phCalibration.lowProbeMillivolts, 1);
  page += F("'></label><label>Point A ADS mV<input name='low_ads_mv' type='number' min='-4096' max='4096' step='0.1' value='");
  page += String(phCalibration.lowAdsMillivolts, 1);
  page += F("'></label></div><div class='row' style='margin-top:10px'>");
  page += F("<label>pH point B<input name='high_ph' type='number' min='0' max='14' step='0.01' value='");
  page += String(phCalibration.highPh, 2);
  page += F("'></label><label>Point B probe mV<input name='high_probe_mv' type='number' min='-1000' max='1000' step='0.1' value='");
  page += String(phCalibration.highProbeMillivolts, 1);
  page += F("'></label><label>Point B ADS mV<input name='high_ads_mv' type='number' min='-4096' max='4096' step='0.1' value='");
  page += String(phCalibration.highAdsMillivolts, 1);
  page += F("'></label></div><div class='row' style='margin-top:10px'>");
  page += F("<label>Titrant pump g/s<input name='titrant_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(titrantPumpFlowRateGps, 3);
  page += F("'></label><label>Sample pump g/s<input name='sample_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(samplePumpFlowRateGps, 3);
  page += F("'></label><label>Scale factor<input name='scale_factor' type='number' min='1' max='100000' step='0.1' value='");
  page += String(scaleSensor.calibrationFactor(), 1);
  page += F("'></label></div><p class='tiny'>Use pump calibration to measure flow, Scale calibrate to tare the reactor scale, and pH/mV calibrate to restart filtered pH acquisition before saving refined points here.</p><button class='primary' type='submit'>Save calibration</button></form></section>");

  page += F("<section id='tab-manual' class='panel'><div class='card full'><h2>Manual Operation</h2><form id='manualForm' action='/action' method='get' class='row'><label class='mini'>Run seconds<input name='sec' type='number' min='0.1' max='30' step='0.1' value='1.0'></label><button class='btn' name='cmd' value='manual_titrant' type='submit'>Run titrant pump</button><button class='btn' name='cmd' value='manual_sample' type='submit'>Run sample pump</button><button class='btn danger' name='cmd' value='manual_stop' type='submit'>Stop pumps</button></form><p class='tiny'>Manual pump actions are blocked while titration or calibration is active. Use seconds here for priming tubing and experiment preparation.</p></div></section>");

  page += F("<section id='tab-admin' class='panel'><div class='split'><div><form action='/set' method='get' class='card'><h2>Settings</h2><div class='row'>");
  page += F("<label>Mode<select name='mode'><option value='base'");
  if (settings.mode == TitrationMode::AddBase) page += F(" selected");
  page += F(">Add base</option><option value='acid'");
  if (settings.mode == TitrationMode::AddAcid) page += F(" selected");
  page += F(">Add acid</option></select></label>");
  page += F("<label>Target pH<input name='target' type='number' min='0' max='14' step='0.05' value='");
  page += String(settings.targetPh, 2);
  page += F("'></label><label>Max used g<input name='max' type='number' min='1' max='1000' step='1' value='");
  page += String(settings.maxConsumedGrams, 1);
  page += F("'></label><label>Sample g<input name='sample' type='number' min='1' max='1000' step='0.1' value='");
  page += String(settings.sampleGrams, 1);
  page += F("'></label></div><div class='row' style='margin-top:10px'>");
  page += F("<label>Titrant<select name='titrant'><option value='naoh001'");
  if (settings.titrantPreset == TitrantPreset::Naoh001) page += F(" selected");
  page += F(">0.01 mol/L NaOH</option><option value='hcl001'");
  if (settings.titrantPreset == TitrantPreset::Hcl001) page += F(" selected");
  page += F(">0.01 mol/L HCl</option><option value='manual'");
  if (settings.titrantPreset == TitrantPreset::Manual) page += F(" selected");
  page += F(">Manual</option></select></label>");
  page += F("<label>Manual mol/L<input name='titrant_m' type='number' min='0.0001' max='10' step='0.0001' value='");
  page += String(settings.titrantMolarity, 4);
  page += F("'></label></div><p class='tiny'>Active titrant: <span id='titrant'>");
  page += htmlEscape(titrantLabel());
  page += F("</span> / result <span id='resultm'>");
  page += String(resultConcentrationM, 5);
  page += F("</span> mol/L</p><p class='tiny'>Titrant flow <span id='titrantgps'>");
  page += String(titrantPumpFlowRateGps, 3);
  page += F("</span> g/s / Sample flow <span id='samplegps'>");
  page += String(samplePumpFlowRateGps, 3);
  page += F("</span> g/s</p><button class='primary' type='submit'>Save settings</button></form>");

  page += F("<form action='/set' method='get' class='card' style='margin-top:10px'><h2>WiFi</h2><div class='row'>");
  page += F("<label>SSID<input name='ssid' maxlength='32' value='");
  page += htmlEscape(wifiSsid);
  page += F("' placeholder='Leave empty for AP only'></label>");
  page += F("<label>Password<input name='wifi_password' type='password' maxlength='64' placeholder='Leave blank to keep'></label>");
  page += F("</div><p class='tiny'>AP stays on. Blank SSID disables STA. Changing WiFi restarts the controller.</p>");
  page += F("<button class='primary' type='submit'>Save WiFi</button></form></div></div></section>");
  page += F("<script>");
  page += F("function text(id,v){var e=document.getElementById(id);if(e)e.textContent=v}");
  page += F("function html(id,v){var e=document.getElementById(id);if(e)e.innerHTML=v}");
  page += F("async function poll(){try{let r=await fetch('/json',{cache:'no-store'});let d=await r.json();");
  page += F("text('phvalue',d.adc_ok?Number(d.ph).toFixed(2):'--.--');");
  page += F("text('mvvalue',d.adc_ok?Number(d.mv).toFixed(0):'--');");
  page += F("text('state',d.state);text('status',d.status);");
  page += F("html('pump',d.pump?'<span class=\"warn\">ON</span>':'<span class=\"ok\">STOP</span>');");
  page += F("text('target',Number(d.target_ph).toFixed(2));text('mvcard',d.adc_ok?Number(d.mv).toFixed(0):'--');text('mode',d.mode);");
  page += F("let used=Number(d.used_g),max=Number(d.max_g);text('used',used.toFixed(1)+' g');text('limit','Limit '+max.toFixed(0)+' g');");
  page += F("text('sample',Number(d.sample_delivered_g).toFixed(1)+' g');text('sampletarget','Target '+Number(d.sample_g).toFixed(1)+' g');");
  page += F("text('titrant',d.titrant);text('resultm',Number(d.result_m).toFixed(5));");
  page += F("text('bottle',d.bottle_g>=0?Number(d.bottle_g).toFixed(1)+' g':'-- g');");
  page += F("html('network','<span>'+d.network+'</span><span>AP '+d.ap_ip+'</span><span>STA '+d.sta_ip+'</span><span>OTA '+(d.ota?'ON':'OFF')+'</span>');");
  page += F("text('netdetail','AP '+d.ap_ip+' / STA '+d.sta_ip+' / OTA host k10-ph-titrator');");
  page += F("let f=document.querySelector('.fill');if(f)f.style.width=Math.max(0,Math.min(100,used/max*100))+'%';");
  page += F("text('titrantgps',Number(d.titrant_gps).toFixed(3));text('samplegps',Number(d.sample_gps).toFixed(3));");
  page += F("}catch(e){}}setInterval(poll,2000);");
  page += F("function activateTab(name){var p=document.getElementById('tab-'+name);if(!p)return;document.querySelectorAll('.tab').forEach(function(x){x.classList.toggle('active',x.dataset.tab===name)});document.querySelectorAll('.panel').forEach(function(x){x.classList.remove('active')});p.classList.add('active')}");
  page += F("document.querySelectorAll('.tab').forEach(function(b){b.onclick=function(){activateTab(b.dataset.tab);location.hash=b.dataset.tab}});var initial=(location.hash||'#run').slice(1);activateTab(initial);");
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
  if (server.hasArg("mode")) {
    settings.mode = server.arg("mode") == "acid" ? TitrationMode::AddAcid : TitrationMode::AddBase;
  }
  if (server.hasArg("target")) {
    settings.targetPh = constrain(server.arg("target").toFloat(), 0.0f, 14.0f);
  }
  if (server.hasArg("max")) {
    settings.maxConsumedGrams = constrain(server.arg("max").toFloat(), 1.0f, 1000.0f);
  }
  if (server.hasArg("sample")) {
    settings.sampleGrams = constrain(server.arg("sample").toFloat(), 1.0f, 1000.0f);
  }
  if (server.hasArg("titrant")) {
    String titrant = server.arg("titrant");
    if (titrant == "hcl001") {
      settings.titrantPreset = TitrantPreset::Hcl001;
    } else if (titrant == "manual") {
      settings.titrantPreset = TitrantPreset::Manual;
    } else {
      settings.titrantPreset = TitrantPreset::Naoh001;
    }
  }
  if (server.hasArg("titrant_m")) {
    settings.titrantMolarity = constrain(server.arg("titrant_m").toFloat(), 0.0001f, 10.0f);
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

  statusLine = wifiChanged ? "WiFi saved" : (calibrationChanged ? "Calibration saved" : "Settings saved");
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
  json += ",\"used_g\":" + String(consumedGrams, 1);
  json += ",\"sample_g\":" + String(settings.sampleGrams, 1);
  json += ",\"sample_delivered_g\":" + String(sampleDeliveredGrams, 1);
  json += ",\"titrant\":\"" + jsonEscape(titrantLabel()) + "\"";
  json += ",\"titrant_m\":" + String(activeTitrantMolarity(), 5);
  json += ",\"result_m\":" + String(resultConcentrationM, 5);
  json += ",\"target_ph\":" + String(settings.targetPh, 2);
  json += ",\"max_g\":" + String(settings.maxConsumedGrams, 1);
  json += ",\"mode\":\"" + modeLabel() + "\"";
  json += ",\"state\":\"" + stateLabel() + "\"";
  json += ",\"status\":\"" + jsonEscape(statusLine) + "\"";
  json += ",\"pump\":" + String(pump.isRunning() ? "true" : "false");
  json += ",\"pump_pulse_ms\":" + String(pump.isRunning() ? 1 : 0);
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
  json += ",\"low_ph\":" + String(phCalibration.lowPh, 2);
  json += ",\"low_probe_mv\":" + String(phCalibration.lowProbeMillivolts, 1);
  json += ",\"low_ads_mv\":" + String(phCalibration.lowAdsMillivolts, 1);
  json += ",\"high_ph\":" + String(phCalibration.highPh, 2);
  json += ",\"high_probe_mv\":" + String(phCalibration.highProbeMillivolts, 1);
  json += ",\"high_ads_mv\":" + String(phCalibration.highAdsMillivolts, 1);
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

  k10.canvas->canvasText("K10 PH TITRATOR", 1, COLOR_WARN);

  if (lastPh.adcOk) {
    snprintf(line, sizeof(line), "PH %.2f  MV %.0f", lastPh.ph, lastPh.millivolts);
  } else {
    snprintf(line, sizeof(line), "PH --    MV --");
  }
  k10.canvas->canvasText(line, 3, lastPh.adcOk ? COLOR_OK : COLOR_WARN);

  snprintf(line, sizeof(line), "TARGET %.2f %s", settings.targetPh, modeText());
  k10.canvas->canvasText(line, 4, COLOR_TEXT);

  snprintf(line, sizeof(line), "USED %.1f/%.0fG", consumedGrams, settings.maxConsumedGrams);
  k10.canvas->canvasText(line, 5, consumedGrams >= settings.maxConsumedGrams ? COLOR_ERROR : COLOR_OK);

  if (scaleReady) {
    snprintf(line, sizeof(line), "REACTOR %.1fG", lastScale.grams);
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

  snprintf(line, sizeof(line), "RESULT %.5fM", resultConcentrationM);
  k10.canvas->canvasText(line, 12, resultConcentrationM > 0.0f ? COLOR_OK : COLOR_MUTED);

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

  if (!phReady || !scaleReady) {
    stopReason = TitrationStopReason::InvalidReading;
    setState(RunState::Error, "Check I2C");
  } else {
    lastPh = phSensor.read();
    lastScale = scaleSensor.read();
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
