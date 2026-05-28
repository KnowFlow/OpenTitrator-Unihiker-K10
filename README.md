# K10 pH Titrator

UNIHIKER K10 based pH titrator controller. The sketch reads a pH probe through an ADS1115 ADC, measures titrant usage with an I2C HX711 scale module, and controls a DFR0523 peristaltic pump from P0.

## Hardware

- UNIHIKER K10, Arduino core `UNIHIKER:esp32:k10`
- ADS1115 I2C ADC at `0x49`, pH probe on A0
- DFRobot KIT0176 HX711 I2C scale module at `0x64`
- DFRobot DFR0523 peristaltic pump signal on `P0`
- Pump driver powered from an external 5-6 V supply, with common ground to K10

## Features

- Standalone K10 display with pH, mV, target pH, mode, titrant used, bottle weight, state, IP, pump, ADC, and scale status.
- Web control panel served by the K10.
- AP is always enabled for fallback access.
- Optional STA WiFi can be configured from the web UI and is saved in K10 Preferences.
- Web UI updates live from `/json` without full page refresh, so forms remain editable.
- Arduino OTA is enabled for future wireless firmware updates.
- Acid/base dosing modes with target pH tolerance and maximum titrant usage limit.
- Default maximum titrant usage is `75 g`.
- Pump is stopped on boot, error, completion, emergency stop, and OTA start/error.

## Network

Default AP:

```text
SSID: K10-pH-Titrator
Password: 12345678
```

Open the AP address shown on the K10 screen, usually:

```text
http://192.168.4.1/
```

From the web Settings panel, enter the STA WiFi SSID and password. After saving, the controller restarts and keeps AP enabled while also joining the configured WiFi. The web header shows both AP and STA IP addresses.

OTA:

```text
Hostname: k10-ph-titrator
Password: k10ph
```

## Controls

On the K10:

- `A` / `B`: adjust the current setup field
- `AB` short press: confirm or move to the next setup step
- `AB` long press: emergency stop

On the web UI:

- `Start`
- `Stop`
- `Tare scale`
- `Reset`
- `Emergency stop`
- mode, target pH, max usage, and WiFi settings

## Build And Upload

Compile:

```powershell
& 'C:\Users\rocke\.agents\skills\unihiker-k10-arduino\scripts\arduino-cli.exe' compile --fqbn UNIHIKER:esp32:k10 .\ph_titrator
```

Upload:

```powershell
& 'C:\Users\rocke\.agents\skills\unihiker-k10-arduino\scripts\arduino-cli.exe' upload -p COM4 --fqbn UNIHIKER:esp32:k10 .\ph_titrator
```

## Test Plan

- Compile the main sketch.
- Verify K10 screen text remains readable and does not fall back to blue blocks.
- Verify AP page opens and form inputs are not cleared by live updates.
- Save STA WiFi settings and confirm the web UI shows a STA IP after reconnecting.
- Confirm `/json` includes pH, mV, AP IP, STA IP, OTA state, pump state, bottle weight, and usage.
- Run first liquid tests with water or a safe substitute before connecting real titrant.
