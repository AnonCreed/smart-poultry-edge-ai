# Poultry Environmental Control - Telemetry Console

Production-grade, API-driven telemetry dashboard for a smart poultry environment.
Django backend, Edge-AI-ready JSON REST API, server-side environmental classifier,
standalone virtual-ESP32-S3 sensor simulator, and a refined developer-tool
dashboard (deep zinc dark mode, dual-line forecast charts, live console feed;
Chart.js vendored locally for air-gapped operation).

## Architecture

```
sensor_simulator.py  ──HTTP POST──>  /api/telemetry/submit/
   (Virtual ESP32,                      │ validate -> classify -> persist
    5 s cadence,                        ▼
    random walk +               PoultryTelemetry (SQLite, indexed timestamp)
    anomaly injection)                  │
                                        ▼
dashboard (browser)  ──HTTP GET──>  /api/telemetry/historical/?hours=N
   (5 s polling loop feeds metrics grid, charts, audit log)
```

| Component | Path |
|---|---|
| Data model + classification enum | `telemetry/models.py` |
| Pure classifier decision framework | `telemetry/classifier.py` |
| API views (submit / historical) + dashboard shell | `telemetry/views.py` |
| URL map | `telemetry/urls.py`, `config/urls.py` |
| Virtual ESP32 client | `sensor_simulator.py` |
| ESP32 edge-node firmware (DHT22 + MQ-137, actuator loop) | `esp32/src/main.cpp`, `esp32/include/config.h` |
| Dashboard template | `telemetry/templates/telemetry/dashboard.html` |
| Console stylesheet / client runtime | `telemetry/static/telemetry/dashboard.css`, `dashboard.js` |
| Unit tests (classifier + API + forecast contracts) | `telemetry/tests.py` |

## Classifier rules (first match wins)

1. `ammonia_level > 25.0` -> `CRITICAL_AMMONIA` (dark red background, white text)
2. `temperature > 35.0 AND humidity > 70.0` -> `HEAT_STRESS_WARNING` (orange border, orange text)
3. `temperature < 18.0` -> `LOW_TEMP_ALERT` (deep blue text, light blue accents)
4. otherwise -> `OPTIMAL_ENVIRONMENT` (charcoal grid, solid green indicator dot)

Classification is computed server-side at ingestion, before the INSERT, so every
stored row is complete and immutable. Thresholds are exposed in the historical
API response so the front end never hardcodes them.

## API

`POST /api/telemetry/submit/`
```json
{
  "temperature": 29.4,
  "humidity": 61.2,
  "ammonia_level": 8.7,
  "predicted_temperature": null,
  "predicted_ammonia": null
}
```
Returns `201` with the persisted record including `predicted_class`.

Forecast channels (`predicted_temperature`, `predicted_ammonia`) are optional and
nullable: absent key, explicit null, and numeric value are all accepted, since the
ESP32-S3 Edge-AI firmware is not yet linked. When numeric, forecasts are
bounds-checked against the same physical envelope as their live counterparts and
serialized back as JSON null when absent, so chart clients can rely on key
presence. Classification is always driven by live readings only; forecasts are
advisory overlays. Payloads with missing live fields, non-numeric values, or
readings outside physical sensor envelopes are rejected with `400`.

`GET /api/telemetry/historical/?hours=24`
Returns records in the trailing window, ascending chronologically, plus the active
threshold set — ready for direct ingestion by chart libraries.

## Running the system

Two terminals.

Terminal 1 — backend and dashboard:
```bash
pip install -r requirements.txt
python manage.py migrate
python manage.py runserver
# Dashboard: http://127.0.0.1:8000/
```

Terminal 2 — virtual sensor:
```bash
python sensor_simulator.py
# Edge-AI forecast emulation is ON by default so the KPI forecast lines,
# dashed chart series, and console (Pred: ...) markers all have live numbers.
# To transmit null forecasts (matching the deployment state before the
# ESP32-S3 firmware is linked):
python sensor_simulator.py --no-edge-ai
```

The simulator transmits every 5 seconds using an Ornstein-Uhlenbeck-style random
walk (Gaussian step + mean reversion) per channel, and superimposes two anomaly
generators: a triangular thermal spike exceeding 37 C every 2 minutes, and an
ammonia accumulation ramp that climbs past 30 PPM until a simulated ventilation
fan engages and extracts it back to baseline.

## Tests

```bash
python manage.py test telemetry
```

Covers classifier rule precedence (ammonia dominance, heat-stress conjunction,
low-temperature boundary), ingestion validation, nullable forecast-channel
contracts (absent key, explicit null, populated, invalid type), and historical
ordering/threshold contracts.

## Hardware firmware (ESP32 DevKit V1)

The `esp32/` directory ships production firmware for the physical edge node.
It reads DHT22 on GPIO4 and MQ-137 on GPIO34, transmits payloads matching
the same JSON contract as the Python simulator, and closes the control
loop by driving fan (GPIO32) and heater (GPIO33) PWM outputs from the
classification the backend returns. See `esp32/README.md` for the full pin
map, MQ-137 calibration procedure, and control-loop table.

Quickstart:
```bash
cd esp32
# Edit include/config.h: WIFI_SSID, WIFI_PASSWORD, and the Django host's LAN IP
pio run                 # Compile
pio run -t upload       # Flash over USB
pio device monitor      # Serial diagnostics at 115200 baud
```

The virtual simulator and the physical firmware are byte-compatible: both
POST the same payload shape, so the backend and dashboard cannot tell them
apart. The Python simulator remains useful for CI, demos, and development
without hardware in the loop.

## Views

Two tab-switched views share a single polling loop, so the console tail
accumulates in the background even while the user is on the Overview.

### Overview Dashboard
- KPI cards: instantaneous readings with desaturated state accents (colored
  value text plus a 2px left-border highlight; backgrounds stay untouched).
  Each forecastable channel shows a muted secondary line beneath the primary
  value: `AI Forecast: 27.8 degC`, `AI Forecast: 8.4 ppm`. When the ESP32-S3
  link is inactive the line reads `AI Forecast: pending`.
- Dual-line charts (full vertical breathing room): solid stroke for live
  sensor data, dashed light stroke for Edge-AI forecasts (`spanGaps: false`,
  so null forecast points render as gaps and a fully-null channel is simply
  invisible while remaining mounted), plus a dashed threshold baseline.

### System Logs Feed
Dedicated full-page console. Append-only tail keyed by record id; each new
ingestion appends one line in the format
`[YYYY-MM-DD HH:MM:SS] INGESTION SUCCESS -> T: 27.4C (Pred: 27.8C) | RH: 60.2% | NH3: 8.1ppm (Pred: 8.4ppm) | STATE: OPTIMAL`
with text tinted by classification. When a forecast channel is null the
inline marker reads `(Pred: n/a)`. Includes a rolling 200-line buffer and a
pause-scroll control. The server emits the same line via the
`telemetry.ingest` logger, so `journalctl` and the browser can be
cross-referenced without translation.
