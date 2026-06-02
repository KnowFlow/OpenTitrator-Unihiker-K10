# K10 电位滴定仪后续路线图

本文档记录 K10 滴定仪从 pH 滴定扩展为通用电位滴定仪的后续任务。路线参考了 METTLER TOLEDO T50/T70/T90 的方法、终点、控制区、趋势、滞后时间和结果计算等概念，但实现上保持轻量，优先服务当前 K10 硬件能力。

## 设计原则

- 保持通用：不要把控制逻辑绑定到 NaOH/HCl。滴定液、测量信号、控制方向和结果公式应分开。
- 先做 EP，再做 EQP：先把“到指定 pH/mV 终点停止”做好，再做曲线自动找等当点。
- 曲线放在网页端：K10 固件只提供实时 JSON，浏览器负责采样、绘图和导出，数据保存在电脑上。
- 每一步都可验证：每个阶段都应有固件编译、控制逻辑测试和一次实际滴定/模拟验证。

## M1 方法预设 Method Presets

目标：把当前分散设置整理为可复用的方法。

- [x] 增加方法选择 UI。
- [x] 支持保存/加载至少 4 个方法：
  - pH 酸碱滴定
  - mV 电位滴定
  - EDTA 硬度滴定
  - 手动自定义方法
- [x] 每个方法保存：
  - endpoint: `pH` / `mV`
  - target value
  - signal trend: `RISE` / `FALL`
  - titrant preset / manual molarity
  - sample grams
  - max used grams
  - EP 控制参数
  - result formula preset
- [x] 切换方法时更新网页设置，并重置运行状态。

验收：

- [x] 切换方法后 `/json` 返回对应 endpoint、target、trend、titrant。
- [x] 重启后保留最后选中的方法。
- [x] 不影响现有 pH 模式滴定。

## M2 EP 终点滴定参数化

目标：把现在固定写死的加液节奏改成可配置的 EP 终点控制。

- [x] 增加 `controlBand`：进入控制区后减速。
- [x] 增加 `holdSeconds`：达到终点后保持一段时间再确认完成。
- [x] 增加 `minSettleSec` / `maxSettleSec`。
- [x] 增加 `stableDelta`：稳定判定阈值。
- [x] 增加 `maxTimeSec`：最长滴定时间保护。
- [x] 按 endpoint 使用不同默认值：
  - pH: stableDelta 默认可从 `0.005-0.02 pH/s` 起步
  - mV: stableDelta 默认可从 `0.5-2 mV/s` 起步
- [x] 网页 Admin 或 Method 设置中显示这些参数。

验收：

- [x] 远离终点时加液更快，进入控制区后减速。
- [x] 达到终点后不立刻结束，而是经过 hold 确认。
- [x] 若 hold 期间信号退回终点外，继续补加并重新计时。

## M3 网页曲线和电脑端数据保存

目标：网页端记录滴定过程数据，形成曲线和可导出的实验记录。

- [x] 浏览器每 1-2 秒轮询 `/json`。
- [x] 在网页内保存本次运行数据数组，不写入 K10 flash。
- [x] 记录字段：
  - timestamp
  - elapsed seconds
  - pH
  - mV
  - used grams
  - sample grams
  - endpoint
  - target
  - trend
  - state
  - pump state / pulse
- [x] 显示曲线：
  - x 轴：used grams 或 elapsed seconds
  - y 轴：pH 或 mV，根据 endpoint 默认选择
- [x] 支持导出：
  - CSV
  - JSON
- [x] 支持 reset 清空本地曲线数据。

验收：

- [x] 刷新页面前，曲线数据持续累积，不因轮询丢失。
- [x] Reset 后曲线和 sample 数据都清零。
- [x] 导出的 CSV 可在 Excel 中打开。

## M4 轻量 EQP 等当点分析

目标：先做“滴定后分析”，不急着实时自动停在 EQP。

- [ ] 基于 M3 数据计算 `d(signal)/d(used_g)`。
- [ ] 找最大斜率点作为候选 EQP。
- [ ] 在曲线上标记 EQP。
- [ ] 输出：
  - endpoint used grams
  - endpoint pH/mV
  - max slope
- [ ] 提供手动选择/修正 EQP 点。

验收：

- [ ] 对一条已有曲线能标出最大突跃点。
- [ ] 可导出带 EQP 结果的数据。
- [ ] 不改变 EP 滴定控制流程。

## M5 结果计算和硬度测试

目标：基于方法和终点用量，计算样品浓度或硬度。

- [ ] 增加结果公式预设：
  - 酸碱浓度
  - EDTA 总硬度，以 CaCO3 mg/L 表示
  - 手动系数
- [ ] 明确硬度计算所需输入：
  - EDTA molarity
  - endpoint used grams 或换算体积
  - sample mass/volume
  - density 或 g/mL 假设
  - blank correction
- [ ] 网页显示结果和单位。
- [ ] 导出数据时包含结果。

验收：

- [ ] EDTA 方法能输出 CaCO3 mg/L。
- [ ] 手动方法能通过用户系数输出自定义结果。
- [ ] 结果公式不会影响泵控制。

## M6 可选高级能力

这些能力有价值，但不应抢在前面实现。

- [ ] 学习滴定：根据一轮曲线推荐 controlBand、stableDelta、pulse 和 settle 参数。
- [ ] 方法流程编辑器：组合取样、混合、滴定、清洗、计算、报告等步骤。
- [ ] 空白值和辅助值管理。
- [ ] 系列样品记录。
- [ ] 报告模板。

## 当前推荐顺序

1. M1 方法预设
2. M2 EP 终点滴定参数化
3. M3 网页曲线和电脑端数据保存
4. M4 轻量 EQP 等当点分析
5. M5 结果计算和硬度测试
6. M6 可选高级能力
