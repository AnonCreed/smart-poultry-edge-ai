/* ==========================================================================
   Telemetry console client runtime.

   Architecture:
   - Single polling loop (5 s cadence, matching sensor transmit interval)
     against GET /api/telemetry/historical/?hours=N.
   - One fetch feeds all three zones: metrics row, dual-line charts, and the
     live console feed. Deriving everything from one response snapshot keeps
     the badge, chart tails, and console traffic guaranteed-consistent.
   - Forecast channels (predicted_temperature / predicted_ammonia) may be
     null on every point until the ESP32-S3 edge hardware is linked. Chart.js
     skips null points natively when spanGaps is false, so the prediction
     dataset stays mounted but invisible -- structurally ready, zero layout
     impact.
   - The console feed is append-only keyed by record id: each poll appends
     only records newer than the last rendered id, emulating a live tail.
   ========================================================================== */

(function () {
  "use strict";

  const POLL_INTERVAL_MS = 5000;
  const CONSOLE_MAX_LINES = 200;   // Rolling buffer cap to bound DOM size.
  const API_HISTORICAL = "/api/telemetry/historical/";
  const API_PROFILE = "/api/telemetry/profile/";
  const API_CONTROL = "/api/telemetry/control/";
  const API_REPORT = "/api/telemetry/report/";
  const API_EXPORT = "/api/telemetry/export/";
  const API_TEST_CASE_REEL = "/api/telemetry/test-case-reel/";
  const TEST_CASE_FRAME_MS = 6000;  // Each case HOLDS this long -- comfortably longer than
                                     // CONTROL_POLL_MS (3s, esp32s3_master/config.h) so the
                                     // master's poll is guaranteed to pick up and relay every
                                     // frame to the real fan/heater before it's replaced.

  // Chart scroll state: all historical points kept in memory, only a window
  // slice rendered to Chart.js so the canvas stays readable regardless of
  // how many data points have accumulated.
  let allChartPoints = [];
  let allChartThresholds = {};
  let chartWindowSize = 200;   // Number of points to display at once (0 = all).
  let chartOffset = 0;         // Points from the latest end that are hidden (pan left = increase).

  // Master-node (ESP32-S3) spike-risk decision threshold. Overwritten on
  // every poll from the API's echoed `thresholds.ammonia_spike_risk_threshold`
  // (telemetry.classifier.AMMONIA_SPIKE_RISK_THRESHOLD) so this default is
  // only ever used before the first successful response lands.
  let spikeRiskThreshold = 0.21;

  // Reference tables echoed by the historical endpoint (Table 4-1 / Table
  // 4-2) -- kept client-side so the age-band and ammonia-risk lookups used
  // for live preview don't need a round trip per keystroke.
  let ammoniaRiskLevels = [];
  let ageTemperatureBands = [];
  let profileFormInitialized = false;   // Don't let the 5s poll clobber an in-progress edit.
  let controlFormInitialized = false;   // Same guard, for the actuator control panel.
  let selectedControlMode = "AUTO";     // Locally-selected mode, committed on Apply.

  // CSS custom properties are the single source of truth for series colors.
  const css = getComputedStyle(document.documentElement);
  const COLOR = {
    tempLive: css.getPropertyValue("--temp-live").trim(),
    tempPred: css.getPropertyValue("--temp-pred").trim(),
    nh3Live:  css.getPropertyValue("--nh3-live").trim(),
    nh3Pred:  css.getPropertyValue("--nh3-pred").trim(),
    crit:     css.getPropertyValue("--crit").trim(),
    grid:     css.getPropertyValue("--border-soft").trim(),
    tick:     css.getPropertyValue("--text-dim").trim(),
  };

  const el = {
    linkState:  document.getElementById("link-state"),
    linkLabel:  document.getElementById("link-state-label"),
    lastIngest: document.getElementById("last-ingest"),
    windowSel:  document.getElementById("window-select"),
    valTemp:    document.getElementById("value-temperature"),
    valHum:     document.getElementById("value-humidity"),
    valNh3:     document.getElementById("value-ammonia"),
    valSpike:   document.getElementById("value-spike-risk"),
    // Forecast lines: muted secondary text under each primary value.
    fcTemp:     document.getElementById("forecast-temperature"),
    fcNh3:      document.getElementById("forecast-ammonia"),
    spikeNote:  document.getElementById("spike-risk-note"),
    cardTemp:   document.getElementById("card-temperature"),
    cardHum:    document.getElementById("card-humidity"),
    cardNh3:    document.getElementById("card-ammonia"),
    cardSpike:  document.getElementById("card-spike-risk"),
    stateBadge: document.getElementById("state-badge"),
    stateLabel: document.getElementById("state-label"),
    stateTime:  document.getElementById("state-time"),
    consoleFeed:  document.getElementById("console-feed"),
    consoleCount: document.getElementById("console-count"),
    consolePause: document.getElementById("console-pause"),
    ammoniaRisk:      document.getElementById("ammonia-risk"),
    ammoniaRiskLabel: document.getElementById("ammonia-risk-label"),
    // Flock profile controls.
    profileAge:        document.getElementById("profile-age"),
    profileAgeBand:    document.getElementById("profile-age-band"),
    profileUseCustom:  document.getElementById("profile-use-custom"),
    profileTempMin:    document.getElementById("profile-temp-min"),
    profileTempMax:    document.getElementById("profile-temp-max"),
    profileAmmoniaCrit: document.getElementById("profile-ammonia-crit"),
    profileApply:      document.getElementById("profile-apply"),
    profileStatus:     document.getElementById("profile-status"),
    profileCustomFields: Array.from(document.querySelectorAll("[data-custom-field]")),
    // Actuator control controls.
    controlModeAuto:    document.getElementById("control-mode-auto"),
    controlModeManual:  document.getElementById("control-mode-manual"),
    controlFan:         document.getElementById("control-fan"),
    controlFanValue:    document.getElementById("control-fan-value"),
    controlHeater:      document.getElementById("control-heater"),
    controlHeaterValue: document.getElementById("control-heater-value"),
    controlApply:       document.getElementById("control-apply"),
    controlStatus:      document.getElementById("control-status"),
    controlEffective:   document.getElementById("control-effective"),
    // Reports tab controls.
    reportStart:     document.getElementById("report-start"),
    reportEnd:       document.getElementById("report-end"),
    reportHours:     document.getElementById("report-hours"),
    reportGenerate:  document.getElementById("report-generate"),
    reportDownload:  document.getElementById("report-download"),
    reportStatus:    document.getElementById("report-status"),
    reportSummary:   document.getElementById("report-summary"),
    reportCount:     document.getElementById("report-count"),
    reportTemp:      document.getElementById("report-temp"),
    reportHumidity:  document.getElementById("report-humidity"),
    reportAmmonia:   document.getElementById("report-ammonia"),
    reportStateBody: document.getElementById("report-state-body"),
    // Test Cases tab controls.
    testCaseStart:          document.getElementById("test-case-start"),
    testCaseStop:           document.getElementById("test-case-stop"),
    testCaseProgress:       document.getElementById("test-case-progress"),
    testCaseReading:        document.getElementById("test-case-reading"),
    testCaseClassification: document.getElementById("test-case-classification"),
    testCaseForecast:       document.getElementById("test-case-forecast"),
    testCaseActuator:       document.getElementById("test-case-actuator"),
  };

  const STATE_META = {
    CRITICAL_AMMONIA:    { label: "CRITICAL",  lineClass: "line-crit" },
    HEAT_STRESS_WARNING: { label: "WARNING",   lineClass: "line-warn" },
    LOW_TEMP_ALERT:      { label: "LOW_TEMP",  lineClass: "line-cold" },
    OPTIMAL_ENVIRONMENT: { label: "OPTIMAL",   lineClass: "line-ok" },
  };

  const BADGE_LABELS = {
    CRITICAL_AMMONIA:    "Critical ammonia",
    HEAT_STRESS_WARNING: "Heat stress warning",
    LOW_TEMP_ALERT:      "Low temperature alert",
    OPTIMAL_ENVIRONMENT: "Optimal environment",
  };

  /* ------------------------------ Formatting ----------------------------- */

  function fmtClock(isoString) {
    return new Date(isoString).toLocaleTimeString([], { hour12: false });
  }

  function fmtConsoleStamp(isoString) {
    const d = new Date(isoString);
    const pad = (n) => String(n).padStart(2, "0");
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
           `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  }

  /* ------------------------------ Chart setup ---------------------------- */

  Chart.defaults.font.family = '"IBM Plex Mono", monospace';
  Chart.defaults.font.size = 10;
  Chart.defaults.color = COLOR.tick;
  Chart.defaults.animation = false; // Live console: animation adds latency, not information.

  function makeScales() {
    return {
      x: { grid: { color: COLOR.grid }, ticks: { maxTicksLimit: 10, maxRotation: 0 } },
      y: { grid: { color: COLOR.grid } },
    };
  }

  const basePlugins = {
    legend: { display: false }, // Legends are rendered in HTML panel headers.
    tooltip: { intersect: false, mode: "index" },
  };

  /**
   * Dual-line chart factory: solid live series + dashed forecast series +
   * dashed threshold baseline. The forecast dataset uses spanGaps:false so
   * null points are simply skipped -- when the entire channel is null (edge
   * hardware not yet linked) the line is invisible but remains mounted and
   * structurally ready, with no layout or tooltip breakage.
   */
  function makeDualLineChart(canvasId, liveLabel, liveColor, predLabel, predColor, thresholdLabel) {
    return new Chart(document.getElementById(canvasId), {
      type: "line",
      data: {
        labels: [],
        datasets: [
          {
            label: liveLabel,
            data: [],
            borderColor: liveColor,
            backgroundColor: liveColor,
            borderWidth: 1.6,
            pointRadius: 0,
            tension: 0.3,
          },
          {
            label: predLabel,
            data: [],
            borderColor: predColor,
            backgroundColor: predColor,
            borderWidth: 1.2,
            borderDash: [4, 4],     // Dotted light stroke for forecasts.
            pointRadius: 0,
            tension: 0.3,
            spanGaps: false,        // Null forecast points render as gaps.
          },
          {
            label: thresholdLabel,
            data: [],
            borderColor: COLOR.crit,
            borderDash: [6, 6],
            borderWidth: 1,
            pointRadius: 0,
            fill: false,
          },
        ],
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: makeScales(),
        plugins: basePlugins,
      },
    });
  }

  const tempChart = makeDualLineChart(
    "chart-temperature",
    "Live temperature degC", COLOR.tempLive,
    "Forecast temperature degC", COLOR.tempPred,
    "35.0 degC threshold"
  );

  const ammoniaChart = makeDualLineChart(
    "chart-ammonia",
    "Live ammonia ppm", COLOR.nh3Live,
    "Forecast ammonia ppm", COLOR.nh3Pred,
    "15.0 ppm safety max"
  );

  /* ------------------------------ Renderers ------------------------------ */

  function alertForMetric(state, metric) {
    if (metric === "ammonia")  return state === "CRITICAL_AMMONIA" ? "crit" : "ok";
    if (metric === "humidity") return state === "HEAT_STRESS_WARNING" ? "warn" : "ok";
    // temperature
    if (state === "HEAT_STRESS_WARNING") return "warn";
    if (state === "LOW_TEMP_ALERT")      return "cold";
    return "ok";
  }

  function fmtForecast(value, unit) {
    // Muted secondary text: numeric when the Edge-AI channel is populated,
    // "pending" placeholder when the ESP32-S3 link has not yet delivered
    // a forecast for that point.
    return (typeof value === "number")
      ? `AI Forecast: ${value.toFixed(1)} ${unit}`
      : "AI Forecast: pending";
  }

  function renderMetrics(latest) {
    el.valTemp.textContent = latest.temperature.toFixed(1);
    el.valHum.textContent = latest.humidity.toFixed(1);
    el.valNh3.textContent = latest.ammonia_level.toFixed(1);
    renderAmmoniaRisk(latest.ammonia_level);

    el.fcTemp.textContent = fmtForecast(latest.predicted_temperature, "degC");
    el.fcNh3.textContent = fmtForecast(latest.predicted_ammonia, "ppm");

    el.stateBadge.dataset.state = latest.predicted_class;
    el.stateLabel.textContent = BADGE_LABELS[latest.predicted_class] || latest.predicted_class;
    el.stateTime.textContent = fmtClock(latest.timestamp);
    el.lastIngest.textContent = fmtClock(latest.timestamp);

    el.cardTemp.dataset.alert = alertForMetric(latest.predicted_class, "temperature");
    el.cardHum.dataset.alert  = alertForMetric(latest.predicted_class, "humidity");
    el.cardNh3.dataset.alert  = alertForMetric(latest.predicted_class, "ammonia");

    renderSpikeRisk(latest.predicted_spike_probability);
  }

  /* ---------------------- Ammonia risk / age band lookups ------------------
     Reference-table lookups (Table 4-1 / Table 4-2), driven by whatever the
     historical endpoint most recently echoed in thresholds.*. Pure lookups
     over data already in memory, so the age-band preview updates as the
     operator types without a server round trip. */

  function lookupAmmoniaRisk(ppm) {
    for (const tier of ammoniaRiskLevels) {
      if (tier.max_ppm === null || ppm <= tier.max_ppm) return tier;
    }
    return ammoniaRiskLevels.length ? ammoniaRiskLevels[ammoniaRiskLevels.length - 1] : null;
  }

  function lookupAgeBand(ageWeeks) {
    const weeks = Math.max(1, ageWeeks || 1);
    for (const band of ageTemperatureBands) {
      if (band.max_week === null || weeks <= band.max_week) return band;
    }
    return ageTemperatureBands.length ? ageTemperatureBands[ageTemperatureBands.length - 1] : null;
  }

  function renderAmmoniaRisk(ammoniaPpm) {
    const tier = lookupAmmoniaRisk(ammoniaPpm);
    el.ammoniaRiskLabel.textContent = tier ? `Risk: ${tier.label}` : "Risk: --";
    el.ammoniaRisk.dataset.risk = tier ? tier.key : "";
  }

  /**
   * The ESP32-S3 master node's TFLite Micro spike-classifier probability.
   * Null until that board is linked (mirrors the predicted_temperature /
   * predicted_ammonia "pending" contract). Escalates ok -> warn -> crit as
   * the probability climbs past the calibrated threshold and twice that
   * threshold, so the tile's alert color tracks the same cutoff the
   * firmware and API already agree on rather than a second hardcoded value.
   */
  function renderSpikeRisk(probability) {
    if (typeof probability !== "number") {
      el.valSpike.textContent = "--.-";
      el.spikeNote.textContent = "Master node: pending";
      el.cardSpike.dataset.alert = "ok";
      return;
    }

    el.valSpike.textContent = (probability * 100).toFixed(1);
    el.spikeNote.textContent = `Threshold: ${(spikeRiskThreshold * 100).toFixed(0)}%`;

    if (probability >= spikeRiskThreshold * 2) {
      el.cardSpike.dataset.alert = "crit";
    } else if (probability >= spikeRiskThreshold) {
      el.cardSpike.dataset.alert = "warn";
    } else {
      el.cardSpike.dataset.alert = "ok";
    }
  }

  function renderCharts(points, thresholds) {
    const nullSafe = (v) => (typeof v === "number" ? v : null);

    // Determine visible slice based on window size and pan offset.
    let slice;
    if (chartWindowSize === 0 || points.length <= chartWindowSize) {
      slice = points;
    } else {
      const end = points.length - chartOffset;
      const start = Math.max(0, end - chartWindowSize);
      slice = points.slice(start, end);
    }

    const labels = slice.map((p) => fmtClock(p.timestamp));

    tempChart.data.labels = labels;
    tempChart.data.datasets[0].data = slice.map((p) => p.temperature);
    tempChart.data.datasets[1].data = slice.map((p) => nullSafe(p.predicted_temperature));
    tempChart.data.datasets[2].data = slice.map(() => thresholds.heat_stress_temp_c);
    tempChart.update();

    ammoniaChart.data.labels = labels;
    ammoniaChart.data.datasets[0].data = slice.map((p) => p.ammonia_level);
    ammoniaChart.data.datasets[1].data = slice.map((p) => nullSafe(p.predicted_ammonia));
    ammoniaChart.data.datasets[2].data = slice.map(() => thresholds.ammonia_critical_ppm);
    ammoniaChart.update();

    // Update nav button states.
    const btnPrev = document.getElementById("chart-prev");
    const btnNext = document.getElementById("chart-next");
    const btnLatest = document.getElementById("chart-latest");
    const totalPts = points.length;
    const win = chartWindowSize === 0 ? totalPts : chartWindowSize;
    if (btnPrev)   btnPrev.disabled = (chartOffset + win >= totalPts);
    if (btnNext)   btnNext.disabled = (chartOffset <= 0);
    if (btnLatest) btnLatest.disabled = (chartOffset <= 0);

    // Range info label.
    const rangeInfo = document.querySelector(".chart-scroll-range-info");
    if (rangeInfo) {
      if (chartWindowSize === 0 || points.length <= chartWindowSize) {
        rangeInfo.textContent = `All ${totalPts} pts`;
      } else {
        const end = totalPts - chartOffset;
        const start = Math.max(1, end - win + 1);
        rangeInfo.textContent = `${start}–${end} / ${totalPts}`;
      }
    }
  }

  /* --------------------------- Live console feed -------------------------- */

  let lastConsoleId = 0;     // High-water mark: only records above this id append.
  let consoleLineCount = 0;

  function consoleLineText(p) {
    const meta = STATE_META[p.predicted_class] || { label: p.predicted_class };
    // Forecast values render inline as "(Pred: X.XC)" / "(Pred: X.Xppm)".
    // When the Edge-AI channel is null the "(Pred: n/a)" marker preserves
    // the field layout so grep and column-based tooling stay stable.
    const predTemp = (typeof p.predicted_temperature === "number")
      ? `${p.predicted_temperature.toFixed(1)}C` : "n/a";
    const predNh3  = (typeof p.predicted_ammonia === "number")
      ? `${p.predicted_ammonia.toFixed(1)}ppm` : "n/a";
    // Master-node (ESP32-S3) spike probability. Omitted entirely (not "n/a")
    // when null, matching the server logger's format exactly so the browser
    // console and journalctl output are byte-identical.
    const spikeSuffix = (typeof p.predicted_spike_probability === "number")
      ? ` | SPIKE_RISK: ${Math.round(p.predicted_spike_probability * 100)}%`
      : "";
    return (
      `[${fmtConsoleStamp(p.timestamp)}] INGESTION SUCCESS -> ` +
      `T: ${p.temperature.toFixed(1)}C (Pred: ${predTemp}) | ` +
      `RH: ${p.humidity.toFixed(1)}% | ` +
      `NH3: ${p.ammonia_level.toFixed(1)}ppm (Pred: ${predNh3}) | ` +
      `STATE: ${meta.label}${spikeSuffix}`
    );
  }

  function appendConsoleLines(points) {
    const fresh = points.filter((p) => p.id > lastConsoleId);
    if (fresh.length === 0) return;

    const paused = el.consolePause.dataset.paused === "true";
    const fragment = document.createDocumentFragment();

    for (const p of fresh) {
      const meta = STATE_META[p.predicted_class] || { lineClass: "line-muted" };
      const line = document.createElement("div");
      line.className = `console-line ${meta.lineClass}`;
      line.textContent = consoleLineText(p);
      fragment.appendChild(line);
      lastConsoleId = Math.max(lastConsoleId, p.id);
      consoleLineCount += 1;
    }

    el.consoleFeed.appendChild(fragment);

    // Rolling buffer: trim oldest lines beyond the cap to bound DOM growth.
    while (el.consoleFeed.children.length > CONSOLE_MAX_LINES) {
      el.consoleFeed.removeChild(el.consoleFeed.firstChild);
    }

    el.consoleCount.textContent = `${consoleLineCount} entries`;

    if (!paused) {
      el.consoleFeed.scrollTop = el.consoleFeed.scrollHeight;
    }
  }

  function resetConsole() {
    // Window change re-baselines the tail; the feed restarts from the new snapshot.
    lastConsoleId = 0;
    consoleLineCount = 0;
    el.consoleFeed.innerHTML = "";
  }

  el.consolePause.addEventListener("click", () => {
    const paused = el.consolePause.dataset.paused === "true";
    el.consolePause.dataset.paused = String(!paused);
    el.consolePause.textContent = paused ? "Pause scroll" : "Resume scroll";
  });

  /* --------------------------- Tab controller ----------------------------
     Pane switching itself is Bootstrap's tab component (data-bs-toggle="tab"
     in the template); all panes stay mounted throughout so the polling loop
     keeps feeding the console tail even while another tab is active. The
     only behavior layered on top here: entering Logs should honor the
     pause state rather than always yanking the view to the bottom. */

  const logsTabButton = document.getElementById("tab-logs");
  if (logsTabButton) {
    logsTabButton.addEventListener("shown.bs.tab", () => {
      if (el.consolePause.dataset.paused !== "true") {
        el.consoleFeed.scrollTop = el.consoleFeed.scrollHeight;
      }
    });
  }

  /* ------------------------------ Link state ------------------------------ */

  function setLinkState(state, label) {
    el.linkState.dataset.state = state;
    el.linkLabel.textContent = label;
  }

  /* --------------------------- Flock profile panel -------------------------
     Age input drives the age-derived temperature band preview immediately
     (client-side lookup against the echoed Table 4-2); "Apply" is what
     actually persists the profile server-side and switches ingestion
     classification onto it (see FlockProfile.is_configured). */

  function updateAgeBandPreview() {
    const band = lookupAgeBand(parseInt(el.profileAge.value, 10));
    if (band) {
      el.profileAgeBand.textContent = `${band.temp_min_c.toFixed(1)} - ${band.temp_max_c.toFixed(1)} degC`;
    }
  }

  function updateCustomFieldVisibility() {
    const showCustom = el.profileUseCustom.checked;
    for (const field of el.profileCustomFields) field.hidden = !showCustom;
  }

  function applyProfileToForm(profile) {
    el.profileAge.value = profile.age_weeks;
    el.profileUseCustom.checked = profile.use_custom_thresholds;
    el.profileTempMin.value = profile.custom_temp_min_c ?? "";
    el.profileTempMax.value = profile.custom_temp_max_c ?? "";
    el.profileAmmoniaCrit.value = profile.custom_ammonia_critical_ppm ?? "";
    updateCustomFieldVisibility();
    updateAgeBandPreview();
  }

  el.profileAge.addEventListener("input", updateAgeBandPreview);
  el.profileUseCustom.addEventListener("change", updateCustomFieldVisibility);

  el.profileApply.addEventListener("click", async () => {
    const payload = {
      age_weeks: parseInt(el.profileAge.value, 10) || 1,
      use_custom_thresholds: el.profileUseCustom.checked,
    };
    if (el.profileUseCustom.checked) {
      const parseOrNull = (raw) => (raw === "" ? null : parseFloat(raw));
      payload.custom_temp_min_c = parseOrNull(el.profileTempMin.value);
      payload.custom_temp_max_c = parseOrNull(el.profileTempMax.value);
      payload.custom_ammonia_critical_ppm = parseOrNull(el.profileAmmoniaCrit.value);
    }

    el.profileStatus.textContent = "Saving...";
    el.profileStatus.dataset.state = "";
    try {
      const response = await fetch(API_PROFILE, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
      const body = await response.json();
      if (!response.ok) throw new Error(body.detail || `HTTP ${response.status}`);

      applyProfileToForm(body.profile);
      el.profileStatus.textContent = "Saved";
      el.profileStatus.dataset.state = "ok";
      refresh();   // Pull the freshly-active thresholds in immediately.
    } catch (err) {
      el.profileStatus.textContent = `Error: ${err.message}`;
      el.profileStatus.dataset.state = "error";
      console.error("Flock profile save failed:", err);
    }
  });

  /* -------------------------- Actuator control panel ------------------------
     Mode buttons and sliders are locally-staged; nothing is sent to the
     server until "Apply" is clicked (same discipline as the flock profile
     panel). The "effective right now" readout is refreshed every poll tick
     regardless, so AUTO's classifier-derived duty stays visible live even
     though the panel itself isn't submitted. */

  function setControlModeUI(mode) {
    selectedControlMode = mode;
    const isAuto = mode === "AUTO";
    el.controlModeAuto.classList.toggle("active", isAuto);
    el.controlModeAuto.setAttribute("aria-pressed", String(isAuto));
    el.controlModeManual.classList.toggle("active", !isAuto);
    el.controlModeManual.setAttribute("aria-pressed", String(!isAuto));
  }

  function renderControlEffective(control) {
    const heaterState = control.effective_heater_pct > 0 ? "ON" : "OFF";
    el.controlEffective.textContent =
      `Fan ${control.effective_fan_pct}% · Heater ${heaterState}`;
  }

  function applyControlToForm(control) {
    setControlModeUI(control.mode);
    el.controlFan.value = control.fan_speed_pct;
    el.controlFanValue.textContent = `${control.fan_speed_pct}%`;
    // Heater is relay-switched (on/off only, no variable power) -- checkbox,
    // not a percentage.
    el.controlHeater.checked = control.heater_power_pct > 0;
    el.controlHeaterValue.textContent = el.controlHeater.checked ? "ON" : "OFF";
    renderControlEffective(control);
  }

  el.controlModeAuto.addEventListener("click", () => setControlModeUI("AUTO"));
  el.controlModeManual.addEventListener("click", () => setControlModeUI("MANUAL"));

  el.controlFan.addEventListener("input", () => {
    el.controlFanValue.textContent = `${el.controlFan.value}%`;
  });
  el.controlHeater.addEventListener("change", () => {
    el.controlHeaterValue.textContent = el.controlHeater.checked ? "ON" : "OFF";
  });

  el.controlApply.addEventListener("click", async () => {
    const payload = {
      mode: selectedControlMode,
      fan_speed_pct: parseInt(el.controlFan.value, 10) || 0,
      heater_power_pct: el.controlHeater.checked ? 100 : 0,
    };

    el.controlStatus.textContent = "Saving...";
    el.controlStatus.dataset.state = "";
    try {
      const response = await fetch(API_CONTROL, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
      const body = await response.json();
      if (!response.ok) throw new Error(body.detail || `HTTP ${response.status}`);

      applyControlToForm(body.control);
      el.controlStatus.textContent = "Saved";
      el.controlStatus.dataset.state = "ok";
    } catch (err) {
      el.controlStatus.textContent = `Error: ${err.message}`;
      el.controlStatus.dataset.state = "error";
      console.error("Actuator control save failed:", err);
    }
  });

  async function refreshControl() {
    try {
      const response = await fetch(API_CONTROL, { headers: { Accept: "application/json" } });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const body = await response.json();

      if (!controlFormInitialized) {
        applyControlToForm(body.control);
        controlFormInitialized = true;
      } else {
        // Keep the live "effective" readout current without clobbering an
        // in-progress mode/slider edit the operator hasn't applied yet.
        renderControlEffective(body.control);
      }
    } catch (err) {
      console.error("Actuator control refresh failed:", err);
    }
  }

  /* -------------------------------- Reports -------------------------------
     On-demand only -- no polling. A quick-hours <select> and explicit
     start/end <input type=date> feed the same window semantics the
     /report/ and /export/ endpoints share; picking one clears the other so
     it's always unambiguous which the next click will use. */

  el.reportHours.addEventListener("change", () => {
    if (el.reportHours.value) {
      el.reportStart.value = "";
      el.reportEnd.value = "";
    }
  });
  for (const dateInput of [el.reportStart, el.reportEnd]) {
    dateInput.addEventListener("change", () => {
      if (dateInput.value) el.reportHours.value = "";
    });
  }

  function buildReportQuery() {
    if (el.reportHours.value) {
      return `hours=${encodeURIComponent(el.reportHours.value)}`;
    }
    const params = [];
    if (el.reportStart.value) params.push(`start=${encodeURIComponent(el.reportStart.value)}`);
    if (el.reportEnd.value) params.push(`end=${encodeURIComponent(el.reportEnd.value)}`);
    return params.join("&");
  }

  function renderReport(body) {
    el.reportSummary.hidden = false;
    el.reportCount.textContent = body.count;

    const fmt = (v) => (typeof v === "number" ? v.toFixed(1) : "--");
    const avg = body.averages;
    el.reportTemp.textContent =
      `${fmt(avg.min_temperature)} / ${fmt(avg.avg_temperature)} / ${fmt(avg.max_temperature)} degC`;
    el.reportHumidity.textContent =
      `${fmt(avg.min_humidity)} / ${fmt(avg.avg_humidity)} / ${fmt(avg.max_humidity)} %`;
    el.reportAmmonia.textContent =
      `${fmt(avg.min_ammonia_level)} / ${fmt(avg.avg_ammonia_level)} / ${fmt(avg.max_ammonia_level)} ppm`;

    el.reportStateBody.innerHTML = "";
    for (const [state, count] of Object.entries(body.state_counts)) {
      const row = document.createElement("tr");
      const label = BADGE_LABELS[state] || state;
      row.innerHTML = `<td>${label}</td><td class="mono">${count}</td>`;
      el.reportStateBody.appendChild(row);
    }
  }

  el.reportGenerate.addEventListener("click", async () => {
    const query = buildReportQuery();
    el.reportStatus.textContent = "Loading...";
    el.reportStatus.dataset.state = "";
    try {
      const response = await fetch(`${API_REPORT}${query ? `?${query}` : ""}`, {
        headers: { Accept: "application/json" },
      });
      const body = await response.json();
      if (!response.ok) throw new Error(body.detail || `HTTP ${response.status}`);

      renderReport(body);
      el.reportStatus.textContent = `Generated — ${body.count} record(s)`;
      el.reportStatus.dataset.state = "ok";
    } catch (err) {
      el.reportStatus.textContent = `Error: ${err.message}`;
      el.reportStatus.dataset.state = "error";
      console.error("Report generation failed:", err);
    }
  });

  el.reportDownload.addEventListener("click", () => {
    const query = buildReportQuery();
    window.location.href = `${API_EXPORT}${query ? `?${query}` : ""}`;
  });

  /* ------------------------------ Test Cases ------------------------------
     Scripted demo reel: fetch the whole precomputed sequence once on Start,
     then play it back client-side on its own timer -- separate Chart.js
     instances from Overview's (own canvases, own data arrays), so nothing
     here ever touches allChartPoints/renderCharts()/refresh(). The reel
     itself is still pure computation server-side (see telemetry/ml/
     reel.py's module docstring) -- but each frame IS now pushed to the
     REAL ActuatorControl row via the same API_CONTROL endpoint the
     Overview tab's MANUAL controls use, so the real fan/heater actually
     react during playback. Stop (and the reel ending naturally) reverts
     to AUTO so the real system doesn't stay pinned at the last case's
     duty. Running this at the same time as the Overview tab's own
     actuator controls will conflict -- same shared row. */

  let testCaseChartTemp = null;
  let testCaseChartAmmonia = null;
  let testCaseFrames = [];
  let testCaseThresholds = { heat_stress_temp_c: null, ammonia_critical_ppm: null };
  let testCaseFrameIndex = 0;
  let testCaseTimerId = null;

  function ensureTestCaseCharts() {
    if (testCaseChartTemp && testCaseChartAmmonia) return;
    testCaseChartTemp = makeDualLineChart(
      "test-case-chart-temperature",
      "Fed temperature degC", COLOR.tempLive,
      "Forecast temperature degC", COLOR.tempPred,
      "Heat stress threshold"
    );
    testCaseChartAmmonia = makeDualLineChart(
      "test-case-chart-ammonia",
      "Fed ammonia ppm", COLOR.nh3Live,
      "Forecast ammonia ppm", COLOR.nh3Pred,
      "Ammonia safety max"
    );
  }

  function resetTestCaseCharts() {
    for (const chart of [testCaseChartTemp, testCaseChartAmmonia]) {
      chart.data.labels = [];
      for (const dataset of chart.data.datasets) dataset.data = [];
      chart.update();
    }
  }

  /**
   * Push one duty to the REAL ActuatorControl row (MANUAL mode) -- the
   * master board's existing CONTROL_POLL_MS poll picks this up and
   * relays it to the real sensor's real GPIO/PWM, same mechanism the
   * Overview tab's manual fan/heater sliders already use. Fire-and-forget
   * from the playback timer (not awaited by the caller) -- a dropped
   * frame's POST just means that one case's duty arrives a beat late,
   * not a broken demo; errors are logged, not surfaced mid-playback.
   */
  async function pushActuatorDuty(fanPct, heaterPct) {
    try {
      await fetch(API_CONTROL, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode: "MANUAL", fan_speed_pct: fanPct, heater_power_pct: heaterPct }),
      });
    } catch (err) {
      console.error("Test case actuator push failed:", err);
    }
  }

  /** Hand the real system back to AUTO -- called on Stop and on natural
   * reel end, so the real fan/heater don't stay pinned at the last
   * case's duty once the demo is over. */
  async function revertToAuto() {
    try {
      await fetch(API_CONTROL, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ mode: "AUTO" }),
      });
    } catch (err) {
      console.error("Test case AUTO revert failed:", err);
    }
  }

  function renderTestCaseFrame(frame) {
    const label = `${frame.scenario_label} #${frame.frame_index + 1}`;
    pushActuatorDuty(frame.actuator.fan_pct, frame.actuator.heater_pct);

    testCaseChartTemp.data.labels.push(label);
    testCaseChartTemp.data.datasets[0].data.push(frame.reading.temperature);
    testCaseChartTemp.data.datasets[1].data.push(frame.forecast.predicted_temperature);
    testCaseChartTemp.data.datasets[2].data.push(testCaseThresholds.heat_stress_temp_c);
    testCaseChartTemp.update();

    testCaseChartAmmonia.data.labels.push(label);
    testCaseChartAmmonia.data.datasets[0].data.push(frame.reading.ammonia_level);
    testCaseChartAmmonia.data.datasets[1].data.push(frame.forecast.predicted_ammonia);
    testCaseChartAmmonia.data.datasets[2].data.push(testCaseThresholds.ammonia_critical_ppm);
    testCaseChartAmmonia.update();

    el.testCaseProgress.textContent =
      `Scenario ${frame.scenario_index + 1} of ${frame.scenario_count}: ${frame.scenario_label} ` +
      `— frame ${frame.frame_index + 1}/${frame.frames_in_scenario} — ${frame.scenario_description}`;

    el.testCaseReading.textContent =
      `T=${frame.reading.temperature.toFixed(1)}C  RH=${frame.reading.humidity.toFixed(1)}%  ` +
      `NH3=${frame.reading.ammonia_level.toFixed(1)}ppm`;

    const stateMeta = STATE_META[frame.classification];
    el.testCaseClassification.textContent = stateMeta ? stateMeta.label : frame.classification;

    el.testCaseForecast.textContent =
      `T→${frame.forecast.predicted_temperature.toFixed(1)}C  NH3→${frame.forecast.predicted_ammonia.toFixed(1)}ppm  ` +
      `spike(NH3)=${(frame.forecast.ammonia_spike_probability * 100).toFixed(0)}%  ` +
      `spike(T)=${(frame.forecast.temp_spike_probability * 100).toFixed(0)}%`;

    el.testCaseActuator.textContent =
      `Fan duty (PWM %): ${frame.actuator.fan_pct}%  ·  Heater: ${frame.actuator.heater_pct > 0 ? "ON" : "OFF"}`;
  }

  function stopTestCases(finalMessage) {
    if (testCaseTimerId !== null) {
      clearInterval(testCaseTimerId);
      testCaseTimerId = null;
    }
    el.testCaseStart.disabled = false;
    el.testCaseStop.disabled = true;
    if (finalMessage) el.testCaseProgress.textContent = finalMessage;
    revertToAuto();  // Real hardware goes back to normal AUTO operation.
  }

  function playNextTestCaseFrame() {
    if (testCaseFrameIndex >= testCaseFrames.length) {
      stopTestCases("Demo reel finished. Click Start to play it again.");
      return;
    }
    renderTestCaseFrame(testCaseFrames[testCaseFrameIndex]);
    testCaseFrameIndex += 1;
  }

  el.testCaseStart.addEventListener("click", async () => {
    el.testCaseStart.disabled = true;
    el.testCaseProgress.textContent = "Loading demo reel...";
    try {
      const response = await fetch(API_TEST_CASE_REEL, { headers: { Accept: "application/json" } });
      const body = await response.json();
      if (!response.ok) throw new Error(body.detail || `HTTP ${response.status}`);

      ensureTestCaseCharts();
      resetTestCaseCharts();
      testCaseFrames = body.frames;
      testCaseThresholds = body.thresholds;
      testCaseFrameIndex = 0;

      el.testCaseStop.disabled = false;
      playNextTestCaseFrame();  // First frame immediately, rest on the interval.
      testCaseTimerId = setInterval(playNextTestCaseFrame, TEST_CASE_FRAME_MS);
    } catch (err) {
      el.testCaseStart.disabled = false;
      el.testCaseProgress.textContent = `Error: ${err.message}`;
      console.error("Test case reel fetch failed:", err);
    }
  });

  el.testCaseStop.addEventListener("click", () => {
    stopTestCases("Stopped.");
  });

  /* ------------------------------ Poll loop ------------------------------ */

  let inFlight = false;

  async function refresh() {
    if (inFlight) return; // Guard against overlapping requests on slow links.
    inFlight = true;
    try {
      const hours = el.windowSel.value;
      const response = await fetch(`${API_HISTORICAL}?hours=${encodeURIComponent(hours)}`, {
        headers: { Accept: "application/json" },
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const body = await response.json();

      if (typeof body.thresholds.ammonia_spike_risk_threshold === "number") {
        spikeRiskThreshold = body.thresholds.ammonia_spike_risk_threshold;
      }
      if (Array.isArray(body.thresholds.ammonia_risk_levels)) {
        ammoniaRiskLevels = body.thresholds.ammonia_risk_levels;
      }
      if (Array.isArray(body.thresholds.age_temperature_bands)) {
        ageTemperatureBands = body.thresholds.age_temperature_bands;
      }
      // Only hydrate the profile form from the server on the first
      // successful poll -- afterwards the form is user-owned, and the 5s
      // poll must not overwrite an in-progress edit out from under them.
      if (!profileFormInitialized && body.profile) {
        applyProfileToForm(body.profile);
        profileFormInitialized = true;
      }

      setLinkState("online", "Link online");
      // Store all points for the windowed chart renderer.
      allChartPoints = body.data;
      allChartThresholds = body.thresholds;
      renderCharts(allChartPoints, allChartThresholds);
      appendConsoleLines(body.data);
      if (body.data.length > 0) {
        renderMetrics(body.data[body.data.length - 1]);
      }
    } catch (err) {
      setLinkState("offline", "Link down");
      console.error("Telemetry refresh failed:", err);
    } finally {
      inFlight = false;
    }

    refreshControl();   // Independent endpoint; a failure here shouldn't affect link state above.
  }

  el.windowSel.addEventListener("change", () => {
    resetConsole();
    // Reset the chart's own pan window so the newly-fetched time range is
    // actually visible instead of staying clipped to the last 200 points
    // of it -- otherwise picking 1h vs 72h renders an identical chart.
    chartOffset = 0;
    chartWindowSize = 0;
    const chartWinSel = document.getElementById("chart-window-select");
    if (chartWinSel) chartWinSel.value = "0";
    refresh();
  });

  /* -------------------- Chart scroll control wiring -------------------- */

  const SCROLL_STEP = 50;   // Points to pan per button press.

  function setupChartScrollControls() {
    const btnPrev    = document.getElementById("chart-prev");
    const btnNext    = document.getElementById("chart-next");
    const btnLatest  = document.getElementById("chart-latest");
    const winSelect  = document.getElementById("chart-window-select");

    if (!btnPrev || !btnNext || !btnLatest || !winSelect) return;

    // Add range info label next to Latest button.
    const rangeSpan = document.createElement("span");
    rangeSpan.className = "chart-scroll-range-info";
    btnLatest.parentNode.appendChild(rangeSpan);

    winSelect.addEventListener("change", () => {
      chartWindowSize = parseInt(winSelect.value, 10);
      chartOffset = 0;   // Reset to latest when changing window size.
      renderCharts(allChartPoints, allChartThresholds);
    });

    btnPrev.addEventListener("click", () => {
      const win = chartWindowSize === 0 ? allChartPoints.length : chartWindowSize;
      const maxOffset = Math.max(0, allChartPoints.length - win);
      chartOffset = Math.min(chartOffset + SCROLL_STEP, maxOffset);
      renderCharts(allChartPoints, allChartThresholds);
    });

    btnNext.addEventListener("click", () => {
      chartOffset = Math.max(0, chartOffset - SCROLL_STEP);
      renderCharts(allChartPoints, allChartThresholds);
    });

    btnLatest.addEventListener("click", () => {
      chartOffset = 0;
      renderCharts(allChartPoints, allChartThresholds);
    });
  }

  setupChartScrollControls();

  refresh();
  setInterval(refresh, POLL_INTERVAL_MS);
})();
