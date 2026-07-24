# K10 pH 滴定仪 — 物料清单

[English](BOM.md) · [项目说明](../README_CN.md) · [使用说明书](MANUAL_CN.md)

修订日期：2026-07-24

本清单对应 1 台参考版本滴定仪，不含备件和实验室日常耗材。下表中的每一项 DFRobot 物料均给出了官方 SKU 和 DFRobot 官方链接。

## DFRobot 物料

| 数量 | 用途 | DFRobot 产品 | SKU | 官方链接 |
|---:|---|---|---|---|
| 1 | 主控制器、屏幕和 Wi-Fi | UNIHIKER K10 | `DFR0992-EN`（DFR0992 系列） | [DFRobot 官方产品页](https://www.dfrobot.com/product-2904.html) |
| 1 | 16 位 pH 信号采集 | Gravity: ADS1115 16-Bit ADC Module | `DFR0553` | [DFRobot 官方产品页](https://www.dfrobot.com/product-1730.html) |
| 1 | 反应器质量测量 | Gravity: I2C 1 kg Weight Sensor Kit (HX711) | `KIT0176` | [DFRobot 官方产品页](https://www.dfrobot.com/product-2289.html) |
| 2 | 滴定剂和样品输送 | Gravity: Digital Peristaltic Pump | `DFR0523` | [DFRobot 官方产品页](https://www.dfrobot.com/product-1698.html) |

K10 的销售 SKU 可能带地区后缀。当前 DFRobot 国际站产品页使用 `DFR0992-EN`，硬件系列和机械模型中也可能标记为 `DFR0992`。

## 其他必需物料

| 数量 | 物料 | 选型说明 |
|---:|---|---|
| 1 | pH 电极及 BNC 信号调理/转接板 | 调理后的信号必须处于 ADS1115 输入范围内；应使用两种缓冲液对实际电极和前端进行校准。 |
| 1 | 稳压外置泵电源 | 使用 `DFR0523` 时应为 **5–6 V DC**。电源和线材需按两台泵选型：每台泵最大连续电流 1.8 A、峰值电流 2.5 A。泵电源地必须与 K10 共地。 |
| 1 | K10 用 USB-C 电源和线缆 | 使用稳定且适合 K10 的电源；不要使用 K10 给蠕动泵供电。 |
| 1 | 反应容器及固定件 | 应适配 KIT0176 秤台，且总质量不超过 1 kg 量程。 |
| 按需 | 软管、接头、杜邦/Gravity 线和接线端子 | 每个 `DFR0523` 包装内含 1 根数字传感器线和 1 m 硅胶管；采购额外管路前先确认整机长度。 |
| 按需 | 缓冲液、滴定剂、样品容器和个人防护用品 | 根据实际分析方法选择。 |

## 采购与供电说明

- `KIT0176` 套件内含称重传感器、信号板、亚克力结构、紧固件和 1 根 Gravity I2C 线。
- 每个 `DFR0523` 包装内含泵和驱动板、1 根数字传感器线、1 m 硅胶管及 10 mL 量筒。
- 蠕动泵必须使用外置电源。`DFR0523` 官方规格为 5–6 V，并非 12 V。
- SKU、地区供货状态和包装内容可能变化；批量采购前请以所链接的 DFRobot 官方页面为准。
