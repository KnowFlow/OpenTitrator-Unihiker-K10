#include "web_ui_escape.h"
#include <WebServer.h>

extern WebServer server;

void sendChineseUiScript() {
  static const char script[] PROGMEM = R"JS(
Object.assign(translations,{
'Calibration Start':'开始校准','Pump Flow':'泵流量','Scale':'天平','pH/mV Sensor':'pH/mV 传感器','Titrant Standard':'滴定剂标准','Manual Operation':'手动操作','Settings':'设置','WiFi':'网络设置','Firmware update':'固件更新','Method and Endpoint':'方法与终点','Endpoint Control':'终点控制','Dosing and Results':'加液与结果','Run Data and EQP':'运行数据与等当点','Controller login':'控制器登录','Factory recovery':'出厂恢复',
'Enter ready':'进入就绪','Calibrate pumps':'校准泵','Reset pH/mV filter':'重置 pH/mV 滤波器','Save calibration':'保存校准','Titrant pump g/s':'滴定泵 g/s','Sample pump g/s':'样品泵 g/s','Titrant pump PWM us':'滴定泵 PWM 微秒','Sample pump PWM us':'样品泵 PWM 微秒','Titrant dose %':'滴定剂量 %','Burst on ms':'脉冲开启毫秒','Burst off ms':'脉冲关闭毫秒','Scale factor':'天平系数','Buffer 1 pH':'缓冲液 1 pH','Buffer 2 pH':'缓冲液 2 pH','Run titrant pump':'运行滴定泵','Run sample pump':'运行样品泵','Sweep titrant':'扫描滴定泵','Sweep sample':'扫描样品泵','Capture current':'采集当前值','Stop pumps':'停止泵','Save settings':'保存设置','Save WiFi':'保存网络设置','Upload OTA':'上传 OTA','Factory password':'出厂密码','New password':'新密码','Confirm password':'确认密码','Recover':'恢复',
'Method':'方法','Signal trend':'信号趋势','Endpoint':'终点','Target pH':'目标 pH','Target mV':'目标 mV','Max used g':'最大用量 g','Sample g':'样品 g','Titrant':'滴定剂','Result formula':'结果公式','Control band':'控制带','Stable delta/s':'稳定变化/秒','Hold s':'保持秒数','Min settle s':'最小静置秒数','Max settle s':'最大静置秒数','Max time s':'最大时间秒数','SSID':'网络名称','Password':'密码',
'Enter ready stops both pumps and puts the controller in READY. Pump calibration can then be started here or with the K10 B key. Each pump runs 2 s, then waits 5 s before reading the scale.':'进入就绪会停止两台泵并进入就绪状态。随后可在此开始泵校准。每台泵运行 2 秒，等待 5 秒后读取天平。',
'Save two buffer points after entering the actual buffer pH and measured probe/ADS mV values. Reset pH/mV filter only restarts acquisition; it does not overwrite saved calibration.':'输入实际缓冲液 pH 和测得的探头/ADS mV 后保存两点。重置 pH/mV 滤波器只会重新采集，不会覆盖已保存的校准。',
'Manual actions are blocked while titration or calibration is active. Run uses burst on/off values, e.g. 5ms on and 100ms off.':'滴定或校准进行时禁止手动操作。运行使用脉冲开/关参数，例如开 5 毫秒、关 100 毫秒。',
'AP stays on. Blank SSID disables STA. Changing WiFi restarts the controller.':'AP 保持开启。SSID 留空会关闭 STA。修改网络会重启控制器。'
,' loads a preset group of endpoint, titrant, result formula, and control defaults. Manual keeps custom values.':'会载入终点、滴定剂、结果公式和控制默认值的一组预设。手动方法保留自定义值。'
,' selects the control signal. Use pH for acid/base endpoint work, or mV for potentiometric endpoints.':'选择控制信号。酸碱终点使用 pH；电位终点使用 mV。'
,' tells the controller whether dosing should raise or lower the endpoint signal.':'告诉控制器加液应使终点信号上升还是下降。'
,' is the EP stop value. Only the active endpoint is used for control.':'是终点停止值。控制只使用当前启用的终点。'
,' is the near-target zone. Larger values slow dosing earlier; smaller values dose faster but risk overshoot.':'是接近目标时的控制区。数值越大越早减慢加液；越小加液越快但更易过冲。'
,' is the allowed signal drift while settling. Lower values wait for a flatter response.':'是静置期间允许的信号漂移。数值越低，等待的响应越平稳。'
,' confirms the endpoint after it is reached. If the signal moves back out, dosing resumes.':'在达到终点后进行确认。若信号重新离开终点范围，则恢复加液。'
,' controls wait time after each pulse. Slow probes or slow reactions need longer settling.':'控制每次脉冲后的等待时间。响应慢的探头或反应需要更长静置。'
,' stops a run that takes too long.':'会停止耗时过长的运行。'
,' stops both pumps before any calibration action.':'会在任何校准操作前停止两台泵。'
,' measures titrant and sample pump delivery in g/s. Recalibrate after tubing, pump head, or liquid changes.':'以 g/s 测量滴定泵和样品泵的输送量。更换管路、泵头或液体后应重新校准。'
,' uses tare for the reactor baseline; scale factor is the grams conversion value.':'使用去皮设置反应器基线；天平系数是换算为克的数值。'
,' stores two buffer points and reports slope %, pH7 offset, and status. Reset pH/mV filter only restarts acquisition.':'保存两个缓冲液点，并报告斜率百分比、pH7 偏移和状态。重置 pH/mV 滤波器只会重新采集。'
,' is configured in Admin through molarity, blank, and result formula.':'在管理页通过摩尔浓度、空白值和结果公式配置。'
,' selects the known solution. Manual mol/L is used only when titrant is Manual.':'选择已知溶液。手动 mol/L 只在滴定剂设为手动时使用。'
,' is the safety limit for titrant consumption.':'是滴定剂消耗的安全上限。'
,' is the sample mass delivered by the P1 pump before titration.':'是滴定前由 P1 泵输送的样品质量。'
,' controls only calculation and display; it does not change pump control.':'只控制计算和显示，不改变泵控制。'
,' subtracts blank titration consumption before calculating and is saved per Method.':'会在计算前扣除空白滴定消耗，并按方法保存。'
,' converts scale mass to mL for molarity and EDTA hardness. Defaults 1.000 for water-like solutions.':'将天平质量换算为 mL，用于摩尔浓度和 EDTA 硬度计算。水状溶液默认值为 1.000。'
,' uses result = net titrant g x factor / sample g for custom tests. Manual mol/L, blank, densities, and factor are method auxiliary values.':'自定义试验使用“结果 = 净滴定剂 g × 系数 / 样品 g”。手动 mol/L、空白值、密度和系数均为方法辅助值。'
,'Dose % scales automatic titrant pulse time. Burst mode runs the titrant pump for on ms, then pauses for off ms inside each dose window.':'剂量百分比会缩放自动滴定脉冲时间。脉冲模式会在每个加液窗口内让滴定泵开启指定毫秒，再暂停指定毫秒。'
,'Tare scale resets the reactor baseline. Scale factor is the HX711 conversion value used for grams.':'去皮会重置反应器基线。天平系数是 HX711 用于克换算的数值。'
,'Saving stores pump flow, scale factor, and pH/mV two-point calibration in flash. WiFi and method settings are kept separate.':'保存会将泵流量、天平系数和 pH/mV 两点校准写入闪存；网络和方法设置独立保存。'
,'Use Admin for known titrant molarity, blank, and formula. A future standardization step can calculate titrant factor from a primary standard.':'在管理页设置已知滴定剂摩尔浓度、空白值和结果公式。后续标准化步骤可用基准物计算滴定剂系数。'
,' is the safer default X axis because data keeps moving even while used g is unchanged.':'是更安全的默认 X 轴，因为即使已用质量不变，数据仍会持续更新。'
,' is useful for final analysis after enough dose changes have happened.':'在发生足够多次加液变化后，可用于最终分析。'
,' on the chart marks the largest d(signal)/d(used g) candidate for review. EDTA hardness also uses a firmware-side EQP tracker to stop after the mV slope peak falls back.':'会在图表上标记最大的 d(信号)/d(已用 g) 候选点供复核。EDTA 硬度也使用固件端 EQP 跟踪器，在 mV 斜率峰值回落后停止。'
,' estimates control band, stable delta, and settle time from the current curve. It does not apply settings automatically.':'从当前曲线估计控制带、稳定变化和静置时间；不会自动应用设置。'
,'Click the curve to manually correct the EQP point, then export CSV or JSON to save the run on the computer.':'点击曲线可手动修正 EQP 点，然后导出 CSV 或 JSON，将运行记录保存到电脑。'
,'Active titrant: ':'当前滴定剂：',' / result ':' / 结果：','Titrant flow ':'滴定剂流量：',' g/s / Sample flow ':' g/s / 样品流量：','Use pH or mV as the endpoint, then choose whether dosing makes that signal rise or fall.':'选择 pH 或 mV 作为终点，再选择加液使该信号上升或下降。'
});
translatePage();
)JS";
  server.send_P(200, "application/javascript; charset=utf-8", script);
}
