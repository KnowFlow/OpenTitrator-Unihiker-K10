# K10 pH 滴定仪

[English](README.md) · [使用说明书](docs/MANUAL_CN.md) · [User Manual](docs/MANUAL.md) · [后续路线图](docs/ROADMAP_CN.md)

基于 **UNIHIKER K10**（ESP32-S3）的独立 pH 滴定控制器。采用自适应纯脉冲加药策略，配合双路蠕动泵、ADS1115 pH 探头和 I2C 电子秤，实现全自动酸碱滴定。

---

## 硬件

| 组件 | 接口 | 地址 / 引脚 | 说明 |
|-----------|-----------|---------------|-------|
| UNIHIKER K10 | — | — | Arduino core `UNIHIKER:esp32:k10` |
| ADS1115 ADC | I2C | `0x49` | pH 探头接 A0 |
| DFRobot KIT0176 电子秤 | I2C | `0x64` | HX711 方案，读取反应瓶重量 |
| 滴定泵 | 舵机 PWM | `P0` | 蠕动泵（如 DFR0523） |
| 样品泵 | 舵机 PWM | `P1` | 蠕动泵，用于自动加样品 |
| 泵电源 | 外部 5–6 V | — | 与 K10 共地 |

### 接线示意

```text
K10 (3.3 V I2C)          ADS1115 (0x49)           电子秤 (0x64)
├─ SDA ──────────────────┬────────────────────────┬
├─ SCL ──────────────────┼────────────────────────┤
├─ GND ──────────────────┴────────────────────────┴
│
├─ P0  ──► 滴定泵舵机信号线
├─ P1  ──► 样品泵舵机信号线
└─ 5V/GND ──► 共用地线（泵由外部电源供电）
```

---

## 软件亮点

### 网页截图

| Run | Calibration | Manual |
|-----|-------------|--------|
| ![Run tab](docs/screenshots/web-run.png) | ![Calibration tab](docs/screenshots/web-cal.png) | ![Manual tab](docs/screenshots/web-manual.png) |

| Admin | Guide |
|-------|-------|
| ![Admin tab](docs/screenshots/web-admin.png) | ![Guide tab](docs/screenshots/web-guide.png) |

### 自适应纯脉冲滴定策略
控制器不再使用连续 PWM，而是根据当前 pH 与目标值的距离，自动选择**脉冲时长**和**静置等待时间**：

![滴定曲线](titration_curve.png)

上图为典型的 S 型滴定曲线：接近等当点时，即使很小的加药量也会引起 pH 的大幅跃迁。控制器通过 `dpH/dt` 检测这一陡峭区，自动切换为微脉冲并延长静置时间，防止过冲。

| 区域 | pH 误差 | 脉冲 | 静置 | 用途 |
|------|----------|-------|--------|---------|
| 陡峭区 | `|dpH/dt| > 0.08` | 25 ms | 15 s | 接近等当点，防止过冲 |
| 远区 | `> 1.0` | 300 ms | 6 s | 保守逼近 |
| 中区 | `0.3 – 1.0` | 100 ms | 10 s | 可控逼近 |
| 近区 | `≤ 0.3` | 40 ms | 15 s | 精细调节 |
| 死区 | `≤ 0.05` | — | — | 停止，已达目标 |

`TitrationDynamics` 动态追踪器实时计算 `dpH/dt`，一旦检测到过冲趋势立即停泵。每次加药决策都会携带独立的静置时间，控制器会等待混合和电极响应后再读取下一次 pH。

### 蠕动泵自动校准
在 **SetupReady（就绪）** 状态按 **B 键**进入校准。控制器依次让滴定泵和样品泵各运行 2 秒，每个泵停泵后静置 5 秒再读取重量差，计算流量（g/s）并保存到 ESP32 Preferences，掉电不丢失。

### 校准页面
网页 **Calibration** tab 将校准分为泵流量、电子秤、pH/mV 传感器和滴定液标准四类。pH/mV 区域显示两点校准斜率百分比、pH 7 偏移和状态；**Reset pH/mV filter** 只重启采样滤波，不会改写已保存的两点校准。滴定液浓度、空白量和结果公式仍在 **Admin** tab 设置。

### 网络与远程控制
- **AP 热点**常开（`K10-pH-Titrator` / `12345678`）。
- 可选 **STA WiFi**，在网页设置后自动保存并重启。
- 响应式网页仪表盘，每 2 秒通过 `/json` 轮询实时数据。
- 网页端记录滴定曲线，支持 pH/mV 曲线显示和 CSV/JSON 导出到电脑。
- **HTTP OTA** 通过 `POST /ota` 上传固件，无需数据线。
- Arduino OTA（UDP 3232）同时可用。

### 安全机制
- 启动、报错、完成、紧急停止、OTA 开始时自动停泵。
- 传感器故障检测（ADC 卡死在 0 或 1023）触发紧急停止。
- 两级滤波：pH 驱动内 EMA 一阶滤波 + 控制环中值截尾平均滤波 (`PhFilter`)。

---

## ToDo / 后续路线

项目正在从 pH 滴定仪扩展为通用电位滴定仪。详细任务记录在 [docs/ROADMAP_CN.md](docs/ROADMAP_CN.md)，当前建议顺序：

- [x] 方法预设：保存 pH、mV、EDTA 硬度和手动方法。
- [x] EP 终点滴定参数化：控制区、滞后时间、稳定阈值、最长时间。
- [x] 网页曲线：浏览器记录 `/json` 数据，在电脑端保存并导出 CSV/JSON。
- [x] 轻量 EQP：基于曲线数据计算斜率并标记等当点。
- [x] 结果计算：支持酸碱浓度、EDTA 硬度和手动系数。
- [ ] 高级能力：学习滴定、方法流程、空白值、系列样品和报告模板。

---

## 快速开始

### 1. 编译

```bash
arduino-cli compile --fqbn UNIHIKER:esp32:k10 ./ph_titrator
```

### 2. 上传（USB）

```bash
arduino-cli upload -p COM4 --fqbn UNIHIKER:esp32:k10 ./ph_titrator
```

### 3. 上传（HTTP OTA）

```bash
python scripts/ota_upload.py ph_titrator/build/ph_titrator.ino.bin --ip 192.168.9.42
```

### 4. 连接

连接 `K10-pH-Titrator` WiFi，打开 K10 屏幕上显示的 AP IP（通常为 `http://192.168.4.1/`），或 STA IP（若已配置）。

---

## 设备按键操作

| 状态 | A 键 | B 键 | AB 短按 | AB 长按 |
|-------|----------|----------|----------|---------|
| SetupMode（模式） | 切换模式 | 切换模式 | → 设置目标 | 紧急停止 |
| SetupTarget（目标） | 目标 –0.05 | 目标 +0.05 | → 就绪 | 紧急停止 |
| SetupReady（就绪） | 去皮 | **校准** | 开始滴定 | 紧急停止 |
| 运行中 / 加药中 … | — | — | 暂停 | 紧急停止 |
| Paused（暂停） | — | — | 继续 | 紧急停止 |
| Calibrating（校准中） | 取消 | 取消 | 取消 | 紧急停止 |
| Done / Error | — | — | 复位 | 紧急停止 |

---

## Web 接口

| 地址 | 方法 | 说明 |
|----------|--------|-------------|
| `/` | GET | 主仪表盘 |
| `/json` | GET | 实时状态 JSON |
| `/set` | GET | 保存设置（`mode`, `target`, `max`, `sample`, `titrant`, `titrant_m`, `ssid`, `wifi_password`） |
| `/action?cmd=` | GET | `start` 开始、`stop` 暂停、`panic` 紧急停止、`tare` 去皮、`reset` 复位 |
| `/ota` | POST | 固件二进制上传 |

---

## 项目结构

```
ph_titrator/
├── ph_titrator.ino      # 主程序（状态机、网页、屏幕）
├── control_logic.h      # 滴定算法、滤波器、自适应剂量逻辑
└── partitions.csv       # 16 MB OTA 分区表

tests/
├── ph_titrator_control_test.cpp         # 本地 C++ 单元测试
└── ph_titrator_control_test/
    └── ph_titrator_control_test.ino     # Arduino 板载单元测试

scripts/
├── ota_upload.py        # HTTP OTA 上传脚本
└── ota_upload.ps1       # PowerShell OTA 上传脚本
```

---

## 许可证

MIT — 详见仓库。
