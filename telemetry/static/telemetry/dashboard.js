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

  // Master-node (ESP32-S3) spike-risk decision threshold. Overwritten on
  // every poll from the API's echoed `thresholds.ammonia_spike_risk_threshold`
  // (telemetry.classifier.AMMONIA_SPIKE_RISK_THRESHOLD) so this default is
  // only ever used before the first successful response lands.
  let spikeRiskThreshold = 0.21;

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
    // Tab controller wiring: two buttons -> two mutually-exclusive views.
    tabs:       Array.from(document.querySelectorAll("[role='tab']")),
    views: {
      overview: document.getElementById("view-overview"),
      logs:     document.getElementById("view-logs"),
    },
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
    "25.0 ppm safety max"
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
    const labels = points.map((p) => fmtClock(p.timestamp));

    // Chart.js treats null y-values as gaps; map missing forecasts to null
    // so partially-linked hardware (some points forecast, some not) also
    // renders correctly.
    const nullSafe = (v) => (typeof v === "number" ? v : null);

    tempChart.data.labels = labels;
    tempChart.data.datasets[0].data = points.map((p) => p.temperature);
    tempChart.data.datasets[1].data = points.map((p) => nullSafe(p.predicted_temperature));
    tempChart.data.datasets[2].data = points.map(() => thresholds.heat_stress_temp_c);
    tempChart.update();

    ammoniaChart.data.labels = labels;
    ammoniaChart.data.datasets[0].data = points.map((p) => p.ammonia_level);
    ammoniaChart.data.datasets[1].data = points.map((p) => nullSafe(p.predicted_ammonia));
    ammoniaChart.data.datasets[2].data = points.map(() => thresholds.ammonia_critical_ppm);
    ammoniaChart.update();
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
     Single-page view switch. Both panels stay mounted so the polling loop
     continues feeding the console tail even while the user reads the
     Overview view; switching to Logs shows the accumulated tail immediately
     with no re-fetch latency. */

  function activateTab(view) {
    for (const tab of el.tabs) {
      const isActive = tab.dataset.view === view;
      tab.setAttribute("aria-selected", String(isActive));
    }
    el.views.overview.hidden = view !== "overview";
    el.views.logs.hidden     = view !== "logs";

    // On entering the logs view, honor the pause state -- if the user paused
    // scroll on a previous visit, don't yank them to the bottom.
    if (view === "logs" && el.consolePause.dataset.paused !== "true") {
      el.consoleFeed.scrollTop = el.consoleFeed.scrollHeight;
    }
  }

  for (const tab of el.tabs) {
    tab.addEventListener("click", () => activateTab(tab.dataset.view));
  }

  /* ------------------------------ Link state ------------------------------ */

  function setLinkState(state, label) {
    el.linkState.dataset.state = state;
    el.linkLabel.textContent = label;
  }

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

      setLinkState("online", "Link online");
      renderCharts(body.data, body.thresholds);
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
  }

  el.windowSel.addEventListener("change", () => {
    resetConsole();
    refresh();
  });

  refresh();
  setInterval(refresh, POLL_INTERVAL_MS);
})();
