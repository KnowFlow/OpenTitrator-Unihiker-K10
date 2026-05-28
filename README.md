# K10 pH Titrator

A standalone pH titration controller for the **UNIHIKER K10** (ESP32-S3). It automates acid–base titration with an adaptive pure-pulse dosing strategy, dual peristaltic pumps, a pH probe with ADS1115 ADC, and an I2C electronic scale.

---

## Hardware

| Component | Interface | Address / Pin | Notes |
|-----------|-----------|---------------|-------|
| UNIHIKER K10 | — | — | Arduino core `UNIHIKER:esp32:k10` |
| ADS1115 ADC | I2C | `0x49` | pH probe on A0 |
| DFRobot KIT0176 scale | I2C | `0x64` | HX711-based, reads reactor weight |
| Titrant pump | Servo PWM | `P0` | Peristaltic pump (e.g. DFR0523) |
| Sample pump | Servo PWM | `P1` | Peristaltic pump for sample delivery |
| Pump power | External 5–6 V | — | Common ground with K10 |

### Wiring diagram (conceptual)

```text
K10 (3.3 V I2C)          ADS1115 (0x49)           Scale (0x64)
├─ SDA ──────────────────┬────────────────────────┬
├─ SCL ──────────────────┼────────────────────────┤
├─ GND ──────────────────┴────────────────────────┴
│
├─ P0  ──► Titrant pump servo signal
├─ P1  ──► Sample pump servo signal
└─ 5V/GND ──► Shared power rail (pumps externally powered)
```

---

## Software Highlights

### Adaptive Pure-Pulse Titration
Instead of continuous PWM, the controller doses the titrant in **timed pulses** whose length and settle time adapt to how far the current pH is from the target:

| Zone | pH error | Pulse | Settle | Purpose |
|------|----------|-------|--------|---------|
| Steep | `|dpH/dt| > 0.08` | 30 ms | 8 s | Near equivalence point, prevent overshoot |
| Far | `> 1.0` | 600 ms | 2 s | Fast approach |
| Medium | `0.3 – 1.0` | 200 ms | 3.5 s | Controlled approach |
| Near | `≤ 0.3` | 80 ms | 5 s | Fine-tuning |
| Deadband | `≤ 0.05` | — | — | Stop, target reached |

A `TitrationDynamics` tracker watches `dpH/dt` and halts immediately if the curve shows overshoot.

### Automatic Pump Calibration
From **SetupReady**, press **B** to enter calibration. The controller runs each pump for exactly 2 seconds, measures the weight change on the scale, computes the flow rate (g/s), and saves it to ESP32 Preferences.

### Network & Remote Control
- **AP mode** is always on (`K10-pH-Titrator` / `12345678`).
- Optional **STA WiFi** configurable from the web UI and persisted in flash.
- Responsive web dashboard with live `/json` polling (2 s).
- **HTTP OTA** via `POST /ota` for browser-less firmware updates.
- Arduino OTA (UDP 3232) also available.

### Safety
- Pump stops on boot, error, completion, emergency stop, and OTA start.
- Sensor-fault detection (stuck ADC values 0 or 1023) triggers emergency stop.
- Dual-stage filtering: EMA inside the pH driver + median-trimmed-mean (`PhFilter`) in the control loop.

---

## Quick Start

### 1. Build

```bash
arduino-cli compile --fqbn UNIHIKER:esp32:k10 ./ph_titrator
```

### 2. Upload (USB)

```bash
arduino-cli upload -p COM4 --fqbn UNIHIKER:esp32:k10 ./ph_titrator
```

### 3. Upload (HTTP OTA)

```bash
python scripts/ota_upload.py ph_titrator/build/ph_titrator.ino.bin --ip 192.168.9.42
```

### 4. Connect

Join the `K10-pH-Titrator` WiFi, open the AP IP shown on the K10 screen (usually `http://192.168.4.1/`), or use the STA IP if configured.

---

## On-Device Controls

| State | Button A | Button B | AB Short | AB Long |
|-------|----------|----------|----------|---------|
| SetupMode | Toggle mode | Toggle mode | → SetupTarget | Panic |
| SetupTarget | Target –0.05 | Target +0.05 | → SetupReady | Panic |
| SetupReady | Tare | **Calibrate** | Start titration | Panic |
| Running / Dosing / … | — | — | Pause | Panic |
| Paused | — | — | Resume | Panic |
| Calibrating | Cancel | Cancel | Cancel | Panic |
| Done / Error | — | — | Reset | Panic |

---

## Web UI Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Main dashboard |
| `/json` | GET | Live status JSON |
| `/set` | GET | Save settings (`mode`, `target`, `max`, `sample`, `titrant`, `titrant_m`, `ssid`, `wifi_password`) |
| `/action?cmd=` | GET | `start`, `stop`, `panic`, `tare`, `reset` |
| `/ota` | POST | Firmware binary upload |

---

## Project Structure

```
ph_titrator/
├── ph_titrator.ino      # Main sketch (state machine, web UI, display)
├── control_logic.h      # Titration math, filters, adaptive dose logic
└── partitions.csv       # 16 MB OTA partition table

tests/
├── ph_titrator_control_test.cpp         # Native C++ unit tests
└── ph_titrator_control_test/
    └── ph_titrator_control_test.ino     # Arduino-hosted unit tests

scripts/
├── ota_upload.py        # HTTP OTA helper
└── ota_upload.ps1       # PowerShell OTA helper
```

---

## License

MIT — see repository for details.
