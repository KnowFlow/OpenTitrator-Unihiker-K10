# K10 pH Titrator — Bill of Materials

[中文版](BOM_CN.md) · [Project README](../README.md) · [User Manual](MANUAL.md)

Revision: 2026-07-24

This list covers one reference-build titrator. Quantities do not include optional spares or laboratory consumables. Every DFRobot item below includes the manufacturer SKU and an official DFRobot link.

## DFRobot Parts

| Qty. | Function | DFRobot product | SKU | Official link |
|---:|---|---|---|---|
| 1 | Controller, display, Wi-Fi | UNIHIKER K10 | `DFR0992-EN` (DFR0992 family) | [DFRobot product page](https://www.dfrobot.com/product-2904.html) |
| 1 | 16-bit pH signal acquisition | Gravity: ADS1115 16-Bit ADC Module | `DFR0553` | [DFRobot product page](https://www.dfrobot.com/product-1730.html) |
| 1 | Reactor mass measurement | Gravity: I2C 1 kg Weight Sensor Kit (HX711) | `KIT0176` | [DFRobot product page](https://www.dfrobot.com/product-2289.html) |
| 2 | Titrant and sample delivery | Unreleased DFRobot peristaltic pump | Pending | Pending official product launch |

The K10 store SKU may include a regional suffix. The current international DFRobot product page uses `DFR0992-EN`; the hardware family and mechanical model may be labelled `DFR0992`.

## Other Required Parts

| Qty. | Item | Selection notes |
|---:|---|---|
| 1 | pH electrode and BNC signal-conditioning/adapter board | The conditioned signal must stay within the ADS1115 input range. Calibrate the actual electrode and front end with two buffer solutions. |
| 1 | Regulated external pump power supply | Select voltage and current from the prototype pump's engineering specification. Size the supply and wiring for both pumps, and connect pump-supply ground to K10 ground. |
| 1 | USB-C power supply and cable for K10 | Use a stable supply suitable for the K10. Do not power the pumps from the K10. |
| 1 | Reaction vessel and holder | Must fit the KIT0176 platform and stay within its 1 kg measuring range. |
| As needed | Tubing, fittings, jumper/Gravity cables, terminal blocks | Confirm the new pump's final package contents and the assembled plumbing length before ordering extras. |
| As needed | Buffer solutions, titrant, sample vessels and PPE | Select for the intended analytical method. |

## Ordering and Power Notes

- `KIT0176` includes the load cell, signal board, acrylic structure, hardware and one Gravity I2C cable.
- The peristaltic pump is an unreleased DFRobot product. Do not substitute another SKU in purchasing documents; add its official SKU, link and package contents after launch.
- The pump supply must be external and must follow the new pump's final electrical specification.
- SKU, regional availability and package contents can change; verify the linked DFRobot page before placing a production order.
