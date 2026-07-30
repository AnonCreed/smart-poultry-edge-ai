# ESP32 Edge Node -- Firmware

Production-grade Arduino/ESP32 firmware for the Poultry Telemetry edge node.
Reads DHT22 (temperature + humidity) and MQ-137 (ammonia PPM), POSTs a JSON
payload matching the Django ingestion contract every 5 seconds, and closes
the environmental control loop by driving the fan and heater PWM outputs
from the classification returned by the backend.

## Pin map (must match the KiCad schematic)

| Signal        | GPIO   | Socket pin | Notes |
|---------------|--------|------------|-------|
| `DHT_DATA`    | GPIO4  | 20         | 10 kOhm pull-up to 3V3; single-wire DHT22 protocol |
| `MQ_DATA`     | GPIO34 | 12         | INPUT-ONLY, ADC1_CH6 -- required so WiFi and ADC coexist |
| `PWM_FAN`     | GPIO32 | 10         | LEDC ch 0, 25 kHz -- gate driven through R5 = 200 Ohm |
| `PWM_HEATER`  | GPIO33 | 9          | LEDC ch 1, 25 kHz -- gate driven through R4 = 200 Ohm |
| `VIN` (+5V)   | -      | 1          | USB or external 5 V bench supply |
| `GND`         | -      | 2 / 17     | Common ground with sensor and actuator returns |
| `3V3`         | -      | 16         | Sensor Vcc supply |

Two constraints are non-negotiable:

1. **MQ-137 output must be scaled to 0-3.3 V before reaching GPIO34.** MQ
   modules commonly swing to 5 V; feeding that directly into any ESP32 pin
   damages the SoC. Use an on-module divider or add an external 1:2 divider
   (e.g. 10 kOhm / 20 kOhm to GND).
2. **GPIO34 is input-only.** It has no internal pull-ups and cannot be an
   output. Any attempt to move `MQ_DATA` to a different pin also invalidates
   the "ADC1 while WiFi is active" guarantee -- ADC2 channels are unusable
   during WiFi association.

## Build and flash

```bash
cd esp32
# Edit include/config.h: WIFI_SSID, WIFI_PASSWORD, API_BASE_URL (Django host IP).
pio run                 # Compile
pio run -t upload       # Flash over USB
pio device monitor      # Serial console at 115200 baud
```

Alternatively in the Arduino IDE: open `src/main.cpp` as `main.ino` (or copy
into a sketch with the same name as the folder), install the same three
libraries listed in `platformio.ini`, and select "ESP32 Dev Module".

## MQ-137 calibration

The datasheet curve fit ships in `config.h` (`MQ137_A = 102.2`, `MQ137_B =
-2.473`), but `MQ137_R0_KOHMS` **must be recalibrated per unit** after a 24-48
hour burn-in:

1. Power the sensor continuously in known-clean outdoor air for at least 24 h.
2. Read the averaged Rs value from the serial log (raw `[MQ] Rs=...` line
   after uncommenting the diagnostic printf near `mqCountsToPpm`).
3. Set `MQ137_R0_KOHMS` to that value.
4. Reflash.

Without this step the reported PPM has a per-unit multiplicative error that
can easily reach 3-5x. The classifier threshold (25 PPM CRITICAL) is set
against calibrated readings, so uncalibrated sensors will either false-alarm
or, worse, miss real breaches.

## Control loop

The backend classifies each incoming record and returns
`record.predicted_class` in the response body. The firmware maps that string
to actuator state:

| Classification          | Fan  | Heater |
|-------------------------|------|--------|
| `CRITICAL_AMMONIA`      | ON   | OFF    |
| `HEAT_STRESS_WARNING`   | ON   | OFF    |
| `LOW_TEMP_ALERT`        | OFF  | ON     |
| `OPTIMAL_ENVIRONMENT`   | OFF  | OFF    |

The interlock (fan and heater never both on) is inherent to the
classification -- a single state is returned per record.

## Edge-AI forecast

`ENABLE_EDGE_AI_STUB = 1` (default) transmits a one-tick-ahead linear
extrapolation in `predicted_temperature` and `predicted_ammonia`. This
exercises the dashboard's dashed forecast lines and the console
`(Pred: ...)` markers with realistic values while a real TFLite Micro model
is not yet trained/deployed. Setting the flag to 0 transmits JSON null for
those fields; the dashboard falls back to `AI Forecast: pending`.

When the real model is ready, replace the body of `edgeAiForecast()` in
`main.cpp` with a TFLite Micro inference call. The payload contract does not
change, so no backend or dashboard updates are required at model swap-in.
