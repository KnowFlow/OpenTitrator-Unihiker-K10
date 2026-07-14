  String page;
  page.reserve(68000);
  int usedPercent = (int)constrain((consumedGrams / settings.maxConsumedGrams) * 100.0f, 0.0f, 100.0f);
  appendWebUiDocumentOpen(page);

  page += F("<div class='top'><div class='identity'><div class='brand'>K10 LAB CONTROLLER</div><div class='title'>Potentiometric Titrator</div></div><label class='mini language' for='languageSelect'>Language<select id='languageSelect'><option value='en'>English</option><option value='zh'>中文</option></select></label></div><div id='network' class='pill networkBar'>");
  page += F("<span>");
  page += htmlEscape(networkLabel);
  page += F("</span><span>AP ");
  page += htmlEscape(apIpAddress);
  page += F("</span><span>STA ");
  page += htmlEscape(staIpAddress);
  page += otaReady ? F("</span><span>OTA ON</span>") : F("</span><span>OTA OFF</span>");
  page += F("</div>");

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
  page += state == RunState::Paused ? F("<button class='btn primary action' data-cmd='start' type='button'>Resume</button>") : F("<button class='btn primary action' data-cmd='start' type='button'>Start</button>");
  page += F("<button class='btn action' data-cmd='start_existing' type='button'>Start current sample</button><button class='btn action' data-cmd='stop' type='button'>Pause</button><button class='btn action' data-cmd='tare' type='button'>Tare scale</button><button class='btn ghost action' data-cmd='reset' type='button'>Reset</button><button id='panicButton' class='btn danger' type='button'>Emergency stop</button>");
  page += F("</div><p class='tiny'>ADC ");
  page += lastPh.adcOk ? F("OK") : F("NO DATA");
  page += F(" / raw ");
  page += String(lastPh.raw);
  page += F(" / pH valid ");
  page += phReady ? F("yes") : F("no");
  page += F(" / scale ");
  page += scaleReady ? F("OK") : F("NO");
  page += F("</p><p class='tiny'>If the status says OTA failed: reset required, use Web Reset above.");
  page += F("</p><p id='netdetail' class='tiny'>AP ");
  page += htmlEscape(apIpAddress);
  page += F(" / STA ");
  page += htmlEscape(staIpAddress);
  page += F(" / OTA host ");
  page += htmlEscape(String(OTA_HOSTNAME));
  page += F("</p><p class='tiny'><a class='ghost' href='/json'>JSON status</a> / Source <a class='ghost' href='https://github.com/KnowFlow/OpenTitrator-Unihiker-K10'>KnowFlow/OpenTitrator-Unihiker-K10</a> / <a class='ghost' href='https://github.com/rockets-cn/k10-ph-titrator/tree/codex/ph-titrator'>rockets-cn mirror</a></p></div>");
  page += F("<div class='card full'><h2>Run Data</h2><div class='chartbar'>");
  page += F("<label>X axis<select id='chartX'><option value='time' selected>Time s</option><option value='used'>Used g</option></select></label>");
  page += F("<label>Y axis<select id='chartY'><option value='auto'>Endpoint</option><option value='ph'>pH</option><option value='mv'>mV</option></select></label>");
  page += F("<button id='curveClear' type='button'>Clear</button><button id='eqpAuto' type='button'>Auto EQP</button><button id='learnParams' type='button'>Suggest Params</button><button id='curveCsv' type='button'>CSV</button><button id='curveJson' type='button'>JSON</button><button id='recordJson' type='button'>Record JSON</button><button id='printRunReport' type='button'>Print report</button>");
  page += F("</div><div id='runRecordCard' class='row'><label>Sample ID<input id='recordSampleId' type='text'></label><label>Batch/reference<input id='recordBatchReference' type='text'></label><label>Operator<input id='recordOperator' type='text'></label><label>Notes<input id='recordNotes' type='text'></label><button id='newRunRecord' type='button'>New record</button><span id='recordStatus' class='tiny'>Draft</span><span id='recordPoints' class='tiny'>0 points</span></div><div class='row'><label>Saved records<select id='savedRecordSelect'><option value=''>No saved records</option></select></label><button id='loadSavedRecord' type='button'>Load saved</button><button id='deleteSavedRecord' type='button' class='danger'>Delete saved</button><span id='savedRecordInfo' class='tiny'>Completed records stay in this browser.</span></div><div class='row'><label>Import record JSON<input id='replayRecordInput' type='file' accept='application/json'></label><button id='replayAnalysisButton' type='button'>Replay analysis</button><span id='replayInfo' class='tiny'>Replay uses the live curve until a record is imported.</span></div><canvas id='curveCanvas' class='chart' width='820' height='260'></canvas><p id='curveInfo' class='tiny'>0 points</p><p id='eqpInfo' class='tiny'>EQP waits for dose changes. Click the curve to correct the candidate point.</p><p id='learnInfo' class='tiny'>Suggestions wait for at least 4 dose-change points.</p><canvas id='derivCanvas' class='chart' width='820' height='140'></canvas><p id='derivInfo' class='tiny'>Derivative d(signal) / d(used g)</p></div></section>");

  page += F("<section id='tab-cal' class='panel'><div class='card full'><h2>Calibration Start</h2><div class='row'>");
  page += F("<button class='btn action' data-cmd='ready' type='button'>Enter ready</button><button class='btn primary action' data-cmd='calibrate' type='button'>Calibrate pumps</button><button class='btn action' data-cmd='scale_calibrate' type='button'>Tare scale</button><button class='btn action' data-cmd='ph_signal_calibrate' type='button'>Reset pH/mV filter</button>");
  page += F("</div><p class='tiny'>Enter ready stops both pumps and puts the controller in READY. Pump calibration can then be started here or with the K10 B key. Each pump runs 2 s, then waits 5 s before reading the scale.</p></div>");
  page += F("<form action='/set' method='post' class='split mutationForm'>");
  page += F("<div class='card'><h2>Pump Flow</h2><div class='row'>");
  page += F("<label>Titrant pump g/s<input name='titrant_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(titrantPumpFlowRateGps, 3);
  page += F("'></label><label>Sample pump g/s<input name='sample_gps' type='number' min='0' max='100' step='0.001' value='");
  page += String(samplePumpFlowRateGps, 3);
  page += F("'></label><label>Titrant pump PWM us<input name='titrant_run_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(titrantPumpRunUs);
  page += F("'></label><label>Sample pump PWM us<input name='sample_run_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(samplePumpRunUs);
  page += F("'></label><label>Titrant dose %<input name='titrant_dose_pct' type='number' min='5' max='100' step='1' value='");
  page += String(titrantDosePercent);
  page += F("'></label><label>Burst on ms<input name='titrant_burst_on_ms' type='number' min='1' max='1000' step='1' value='");
  page += String(titrantBurstOnMs);
  page += F("'></label><label>Burst off ms<input name='titrant_burst_off_ms' type='number' min='0' max='5000' step='1' value='");
  page += String(titrantBurstOffMs);
  page += F("'></label></div><p class='tiny'>Dose % scales automatic titrant pulse time. Burst mode runs the titrant pump for on ms, then pauses for off ms inside each dose window.</p></div>");
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

  page += F("<section id='tab-manual' class='panel'><div class='card full'><h2>Manual Operation</h2><form id='manualForm' action='/action' method='post' class='row mutationForm'><label class='mini'>Run ms<input name='ms' type='number' min='5' max='30000' step='1' value='1000'></label><label class='mini'>Titrant PWM us<input name='titrant_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(titrantPumpRunUs);
  page += F("'></label><label class='mini'>Sample PWM us<input name='sample_us' type='number' min='1000' max='1500' step='1' value='");
  page += String(samplePumpRunUs);
  page += F("'></label><label class='mini'>Burst on ms<input name='burst_on_ms' type='number' min='1' max='1000' step='1' value='5'></label><label class='mini'>Burst off ms<input name='burst_off_ms' type='number' min='0' max='5000' step='1' value='100'></label><label class='mini'>Sweep start us<input name='sweep_start_us' type='number' min='1000' max='1500' step='1' value='1000'></label><label class='mini'>Sweep end us<input name='sweep_end_us' type='number' min='1000' max='1500' step='1' value='1500'></label><label class='mini'>Sweep seconds<input name='sweep_sec' type='number' min='1' max='120' step='1' value='20'></label><button class='btn' name='cmd' value='manual_titrant' type='submit'>Run titrant pump</button><button class='btn' name='cmd' value='manual_sample' type='submit'>Run sample pump</button><button class='btn' name='cmd' value='manual_sweep_titrant' type='submit'>Sweep titrant</button><button class='btn' name='cmd' value='manual_sweep_sample' type='submit'>Sweep sample</button><button class='btn primary' name='cmd' value='manual_capture_sweep' type='submit'>Capture current</button><button class='btn danger' name='cmd' value='manual_stop' type='submit'>Stop pumps</button></form><p class='tiny'>Manual actions are blocked while titration or calibration is active. Run uses burst on/off values, e.g. 5ms on and 100ms off.</p></div></section>");

  page += F("<section id='tab-admin' class='panel'><div class='split'><div><form action='/set' method='post' class='card mutationForm'><h2>Settings</h2><div class='row'>");
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
  page += F("'></label></div><div class='row' style='margin-top:10px'><label>Titrant density g/mL<input id='titrantDensityInput' name='titrant_density' type='number' min='0.1' max='5' step='0.001' value='");
  page += String(settings.titrantDensityGramsPerMl, 3);
  page += F("'></label><label>Sample density g/mL<input id='sampleDensityInput' name='sample_density' type='number' min='0.1' max='5' step='0.001' value='");
  page += String(settings.sampleDensityGramsPerMl, 3);
  page += F("'></label><label>Manual factor<input id='manualFactorInput' name='manual_factor' type='number' min='-1000000' max='1000000' step='0.0001' value='");
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
  page += F("</span> g/s</p><p class='tiny'>Use pH or mV as the endpoint, then choose whether dosing makes that signal rise or fall.");
  if (autoEqpEnabled()) {
    page += F(" EDTA hardness uses automatic EQP stop from the mV slope curve.");
  }
  page += F("</p><button class='primary' type='submit'>Save settings</button></form>");

  page += F("<form action='/set' method='post' class='card mutationForm' style='margin-top:10px'><h2>WiFi</h2><div class='row'>");
  page += F("<label>SSID<input name='ssid' maxlength='32' value='");
  page += htmlEscape(wifiSsid);
  page += F("' placeholder='Leave empty for AP only'></label>");
  page += F("<label>Password<input name='wifi_password' type='password' maxlength='64' placeholder='Leave blank to keep'></label>");
  page += F("</div><p class='tiny'>AP stays on. Blank SSID disables STA. Changing WiFi restarts the controller.</p>");
  page += F("<button class='primary' type='submit'>Save WiFi</button></form><form id='otaForm' class='card' style='margin-top:10px'><h2>Firmware update</h2><input name='update' type='file' required><button type='submit'>Upload OTA</button></form></div></div></section>");
  page += F("<section id='tab-guide' class='panel'><div class='guide'>");
  page += F("<div class='card'><h2>Method and Endpoint</h2><p><span class='term'>Method</span> loads a preset group of endpoint, titrant, result formula, and control defaults. Manual keeps custom values.</p><p><span class='term'>Endpoint</span> selects the control signal. Use pH for acid/base endpoint work, or mV for potentiometric endpoints.</p><p><span class='term'>Signal trend</span> tells the controller whether dosing should raise or lower the endpoint signal.</p><p><span class='term'>Target pH / mV</span> is the EP stop value. Only the active endpoint is used for control.</p></div>");
  page += F("<div class='card'><h2>Endpoint Control</h2><p><span class='term'>Control band</span> is the near-target zone. Larger values slow dosing earlier; smaller values dose faster but risk overshoot.</p><p><span class='term'>Stable delta/s</span> is the allowed signal drift while settling. Lower values wait for a flatter response.</p><p><span class='term'>Hold s</span> confirms the endpoint after it is reached. If the signal moves back out, dosing resumes.</p><p><span class='term'>Min / Max settle s</span> controls wait time after each pulse. Slow probes or slow reactions need longer settling.</p><p><span class='term'>Max time s</span> stops a run that takes too long.</p></div>");
  page += F("<div class='card'><h2>Calibration</h2><p><span class='term'>Enter ready</span> stops both pumps before any calibration action.</p><p><span class='term'>Pump flow</span> measures titrant and sample pump delivery in g/s. Recalibrate after tubing, pump head, or liquid changes.</p><p><span class='term'>Scale</span> uses tare for the reactor baseline; scale factor is the grams conversion value.</p><p><span class='term'>pH/mV sensor</span> stores two buffer points and reports slope %, pH7 offset, and status. Reset pH/mV filter only restarts acquisition.</p><p><span class='term'>Titrant standard</span> is configured in Admin through molarity, blank, and result formula.</p></div>");
  page += F("<div class='card'><h2>Dosing and Results</h2><p><span class='term'>Titrant</span> selects the known solution. Manual mol/L is used only when titrant is Manual.</p><p><span class='term'>Max used g</span> is the safety limit for titrant consumption.</p><p><span class='term'>Sample g</span> is the sample mass delivered by the P1 pump before titration.</p><p><span class='term'>Result formula</span> controls only calculation and display; it does not change pump control.</p><p><span class='term'>Blank g</span> subtracts blank titration consumption before calculating and is saved per Method.</p><p><span class='term'>Density g/mL</span> converts scale mass to mL for molarity and EDTA hardness. Defaults 1.000 for water-like solutions.</p><p><span class='term'>Manual factor</span> uses result = net titrant g x factor / sample g for custom tests. Manual mol/L, blank, densities, and factor are method auxiliary values.</p></div>");
  page += F("<div class='card'><h2>Run Data and EQP</h2><p><span class='term'>Time s</span> is the safer default X axis because data keeps moving even while used g is unchanged.</p><p><span class='term'>Used g</span> is useful for final analysis after enough dose changes have happened.</p><p><span class='term'>Auto EQP</span> on the chart marks the largest d(signal)/d(used g) candidate for review. EDTA hardness also uses a firmware-side EQP tracker to stop after the mV slope peak falls back.</p><p><span class='term'>Suggest Params</span> estimates control band, stable delta, and settle time from the current curve. It does not apply settings automatically.</p><p>Click the curve to manually correct the EQP point, then export CSV or JSON to save the run on the computer.</p></div>");
  page += F("</div></section>");
  page += F("<section id='authPanel' class='card full'><div id='loginPanel'><h2>Controller login</h2><div id='loginForm' class='row'><label>Password<input id='loginPassword' type='password' autocomplete='current-password'></label><button id='loginButton' type='button' class='primary'>Login</button></div></div><div id='sessionPanel' style='display:none'><span class='ok'>Authenticated</span> <button id='logoutButton' type='button'>Logout</button></div><details><summary>Factory recovery</summary><div id='recoveryForm' class='row'><label>Factory password<input id='factoryPassword' type='password' autocomplete='off'></label><label>New password<input id='newPassword' type='password' autocomplete='new-password'></label><label>Confirm password<input id='confirmPassword' type='password' autocomplete='new-password'></label><button id='recoveryButton' class='danger' type='button'>Recover</button></div></details><p id='authMessage' class='tiny'></p></section>");
  page += F("<script>");
#include "web_ui_script.inc"
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
