# Unihiker K10 pH Titrator

[中文](README_CN.md) · [User Manual](docs/MANUAL.md) · [使用说明书](docs/MANUAL_CN.md) · [Roadmap](docs/ROADMAP_CN.md)

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
| Pump power | External 12 V | — | Common ground with K10 |

### Wiring diagram (conceptual)

```text
K10 (3.3 V I2C)          ADS1115 (0x49)           Scale (0x64)
├─ SDA ──────────────────┬────────────────────────┬
├─ SCL ──────────────────┼────────────────────────┤
├─ GND ──────────────────┴────────────────────────┴
│
├─ P0  ──► Titrant pump servo signal
├─ P1  ──► Sample pump servo signal
└─ 12V/GND ─► Shared power rail (pumps externally powered)
```

---

## Software Highlights

### Web Screenshots

| Run | Calibration | Manual |
|-----|-------------|--------|
| ![Run tab](docs/screenshots/web-run.png) | ![Calibration tab](docs/screenshots/web-cal.png) | ![Manual tab](docs/screenshots/web-manual.png) |

| Admin | Guide |
|-------|-------|
| ![Admin tab](docs/screenshots/web-admin.png) | ![Guide tab](docs/screenshots/web-guide.png) |

### Adaptive Pure-Pulse Titration
Instead of continuous PWM, the controller doses the titrant in **timed pulses** whose length and settle time adapt to how far the current pH is from the target:

![Titration curve](titration_curve.png)

The S-shaped curve above illustrates why pulse dosing works: near the steep equivalence point, even a small dose causes a large pH jump. The controller detects this via `dpH/dt` and switches to micro-pulses with longer settle times.

| Zone | Error threshold | Pulse | Settle | Purpose |
|------|----------|-------|--------|---------|
| Steep | `|dpH/dt| > 0.08` | 25 ms | 15 s | Near equivalence point, prevent overshoot |
| Far | `> controlBand × 3` | 450 ms | 5 s | Faster approach while far from endpoint |
| Medium | `> controlBand` | 150 ms | 8 s | Controlled approach |
| Near | `> controlBand × 0.33` | 60 ms | 12 s | Fine-tuning |
| Micro | `≤ controlBand × 0.33` | 25 ms | 15 s | Final micro-dose if still below endpoint |
| Endpoint | `≤ tolerance` or predictive stop | — | — | Stop, target reached |

A `TitrationDynamics` tracker watches `dpH/dt` and halts immediately if the curve shows overshoot. Each dose decision also carries its own settling interval, clamped by the configured `Min / Max settle s`, so the controller waits long enough for mixing and electrode response before reading pH again. Default pH `controlBand` is `0.30`, so Far/Medium/Near thresholds are approximately `0.90`, `0.30`, and `0.10` pH.

### Automatic Pump Calibration
From **SetupReady**, press **B** to enter calibration. The controller runs each pump for exactly 2 seconds, waits 5 seconds after each pump stops, measures the weight change on the scale, computes the flow rate (g/s), and saves it to ESP32 Preferences.

Pump speed is configurable per pump as a servo PWM pulse width. The default `1000us` preserves the original speed; values closer to `1500us` run slower. Recalibrate pump g/s after changing PWM speed so dosing estimates match the actual flow.

### Calibration Page
The web **Calibration** tab separates pump flow, scale, pH/mV sensor, and titrant standard settings. The pH/mV section displays two-point slope percentage, pH 7 offset, and calibration status. **Reset pH/mV filter** only restarts acquisition filtering; it does not overwrite the saved two-point calibration. Titrant molarity, blank, and result formula remain in the **Admin** tab.

### Network & Remote Control
- **AP mode** is always on (`K10-pH-Titrator` / `12345678`).
- Optional **STA WiFi** configurable from the web UI and persisted in flash.
- Responsive web dashboard with live `/json` polling (2 s).
- Browser-side titration curves with pH/mV plotting and CSV/JSON export to the computer.
- **HTTP OTA** via `POST /ota` for browser-less firmware updates.
- Arduino OTA (UDP 3232) also available.

### Safety
- Pump stops on boot, error, completion, emergency stop, and OTA start.
- Sensor-fault detection (stuck ADC values 0 or 1023) triggers emergency stop.
- Dual-stage filtering: EMA inside the pH driver + median-trimmed-mean (`PhFilter`) in the control loop.

---

## ToDo / Roadmap

The project is evolving from a pH titrator into a general potentiometric titrator. Detailed tasks are tracked in [docs/ROADMAP_CN.md](docs/ROADMAP_CN.md). Current order:

- [x] Method presets for pH, mV, EDTA hardness, and manual methods.
- [x] Parameterized EP endpoint control: control band, hold time, stability threshold, and max time.
- [x] Web-side curves: browser records `/json` data, saves on the computer, and exports CSV/JSON.
- [x] Lightweight EQP analysis: compute slope from curve data and mark the equivalence point.
- [x] Result calculation for acid/base concentration, EDTA hardness, and manual factors.
- [x] Method-level blank and auxiliary values: store blank, manual molarity, density, and manual factor per Method.
- [x] EDTA hardness method refinement: use mV curve slope to stop at an automatic equivalence point instead of a fixed mV target.
- [ ] Calcium/magnesium two-step hardness workflow with pH-condition guidance.
- [ ] Advanced features: method workflow, sample series, and reports.

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

HTTP OTA stops and locks both pumps before flash writing. A successful update restarts into SetupMode and never resumes the interrupted run. After a failed or aborted upload, use the Web Reset control; hardware A/B buttons are not required for recovery.

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
| `/set` | POST | Save settings (`mode`, `target`, `max`, `sample`, `titrant`, `titrant_m`, `blank_g`, `titrant_density`, `sample_density`, `ssid`, `wifi_password`) |
| `/action` | POST | Authenticated commands in the request body: `start`, `start_existing`, `stop`, `tare`, `reset` |
| `/panic` | POST | Anonymous emergency stop; immediately stops both pumps |
| `/ota` | POST | Firmware binary upload |

---

## RunEngine lifecycle boundary

`RunEngine` owns the active experiment lifecycle and its history: phase; filling/progress; run, pulse, and settle timers; dosing dynamics; endpoint hold; equivalence-point (EQP) tracking; predose progress; active mass/result selection; and emergency stop. It is a pure C++17 boundary with fixed memory budgets: `sizeof(RunEngine) <= 4096` and `sizeof(RunInput) + sizeof(RunOutput) <= 1024`.

`control_logic.h` provides pure calculations and reusable value types; it does not own live experiment state. The sketch owns hardware and sensors, Web/authentication, OTA, display, setup/calibration, persistence, and only translates inputs into `RunInput` and `RunOutput` intentions back to hardware/UI.

Every active-run entry point uses the same command mapping:

| Source | `RunCommand` |
|--------|--------------|
| Web/panel Start | `StartNormal` |
| Start existing sample | `StartExistingSample` |
| Stop/pause | `Pause` |
| Paused Start/resume | `Resume` |
| Reset | `Reset` |
| Main loop | `Tick` |
| AB-long and Web panic | `EmergencyStop` |

Resume always re-enters `FilterWarmup`; the display tells the operator `正在重新稳定信号` while the signal is re-stabilized. `requestedSettleMs` is display/diagnostic metadata only. It cannot advance lifecycle transitions; only engine time and input sensor facts can do that.

On-device deployment is deferred. The labelled-device Task 7 in [the Web/auth command-security plan](docs/superpowers/plans/2026-07-10-web-auth-command-security.md) must be completed onsite by the onsite operator before device installation or smoke verification. This phase does not claim deployment.

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

## Web authentication and provisioning

On first setup, sign in with the unique factory password printed on the device label and choose the administrator password. Keep the label private for Web-only password recovery; recovery stops both pumps, clears active sessions, and returns the instrument to `SetupMode`. Log out on shared computers. Sessions expire after 30 minutes without a successful authenticated write.

All control and settings integrations now use authenticated `POST` requests; legacy `GET` integrations are incompatible. OTA also requires a current session token: `python scripts/ota_upload.py firmware.bin --ip DEVICE_IP --token SESSION_TOKEN` or `.\scripts\ota_upload.ps1 -Bin firmware.bin -Ip DEVICE_IP -Token SESSION_TOKEN`. The helpers send the token in `X-Session-Token` and do not print it.

Manufacturing must run `generate_factory_auth.py` once per device, compile its generated header into that device only, attach the matching label, and delete both generated artifacts after the build. Never reuse or commit credentials or labels.

HTTP authentication remains plaintext on the local network and does not protect against a packet sniffer. Use the device AP or a trusted LAN.

## License

MIT — see repository for details.
