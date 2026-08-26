# ESP32 Sensor Node -- Firmware

Production-grade Arduino/ESP32 firmware for the Poultry Telemetry sensor
node. Reads DHT11 (temperature + humidity) and MQ-137 (ammonia PPM) every
5 seconds and sends them over **ESP-NOW** to the ESP32-S3 master node
(`../esp32s3_master/`), which runs the on-device forecast/spike model and
owns the WiFi/HTTP leg to Django. The master relays Django's classification
back over ESP-NOW so this board can still drive its own fan (PWM speed
control) and PTC heater (ON/OFF relay -- see `HEATER_RELAY_ON`/`_OFF` in
`include/config.h` for this board's relay polarity), closing the control
loop end-to-end across the two boards.

This board has **no WiFi at all** -- no credentials, no AP join, no IP, no
NTP. It only brings the radio up to speak ESP-NOW directly to the master,
on a fixed channel set at compile time (`ESPNOW_WIFI_CHANNEL`,
`include/config.h`) since there's no AP association to inherit a channel
from. See `include/config.h`'s "Radio" section for why the channel is
fixed rather than negotiated, and what to do if it ever needs to change.
This board never talks to Django directly either; see
`../esp32s3_master/README.md` for the full two-board pipeline and the MAC
pairing procedure required before either board can talk to the other.

## Pin map (must match the KiCad schematic)

| Signal        | GPIO   | Socket pin | Notes |
|---------------|--------|------------|-------|
| `DHT_DATA`    | GPIO4  | 20         | 10 kOhm pull-up to 3V3; single-wire DHT11 protocol |
| `MQ_DATA`     | GPIO34 | 12         | INPUT-ONLY, ADC1_CH6 -- required so WiFi and ADC coexist |
| `PWM_FAN`     | GPIO32 | 10         | LEDC PWM (channel 0, 25 kHz, 8-bit) speed target to the fan's own driver IC -- not a power switch, see `FAN_ENABLE` below |
| `FAN_ENABLE`  | GPIO27 | TBD -- verify against schematic | Digital output gating an external N-MOSFET on the fan's GND leg; HIGH = fan powered, LOW = de-energized. Real hard-off, since many cheap fans don't honor 0% PWM duty as a true stop |
| `HEATER_RELAY`| GPIO25 | TBD -- verify against schematic | Digital output to relay module IN pin; HIGH = heater ON, LOW = OFF (`HEATER_RELAY_ON`/`_OFF` in `config.h` -- confirmed by bench test 2026-08-23, opposite of the module's original active-LOW assumption). ON/OFF only, no speed control |
| `LCD_SDA`     | GPIO21 | TBD -- verify against schematic | I2C data to the LCD's PCF8574T backpack. ESP32 DevKit V1 hardware I2C default |
| `LCD_SCL`     | GPIO22 | TBD -- verify against schematic | I2C clock to the LCD's PCF8574T backpack. ESP32 DevKit V1 hardware I2C default |
| `VIN` (+5V)   | -      | 1          | USB or external 5 V bench supply; also feeds the LCD's VCC |
| `GND`         | -      | 2 / 17     | Common ground with sensor and actuator returns, and the LCD |
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
cd hardware/esp32
# Edit include/config.h: MASTER_MAC_ADDR (see the MAC pairing procedure in
# ../esp32s3_master/README.md) and ESPNOW_WIFI_CHANNEL if your router isn't
# on channel 1. No WiFi credentials to set -- this board has none.
pio run                 # Compile
pio run -t upload       # Flash over USB
pio device monitor      # Serial console at 115200 baud
```

Flash the ESP32-S3 master (`../esp32s3_master/`) first so you have its MAC
address to put in `MASTER_MAC_ADDR` before flashing this board.

Alternatively in the Arduino IDE: open `src/main.cpp` as `main.ino` (or copy
into a sketch with the same name as the folder), install the two libraries
listed in `platformio.ini` (ESP-NOW ships with the esp32 board package, no
separate library needed), and select "ESP32 Dev Module".

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
can easily reach 3-5x. The classifier threshold (15 PPM CRITICAL) is set
against calibrated readings, so uncalibrated sensors will either false-alarm
or, worse, miss real breaches.

## Control loop

The master node POSTs each record to Django, which classifies it and
returns `record.predicted_class`. The master relays that string back to
this board over ESP-NOW (`ActuatorCommand`), along with **two independently
computed bytes**: `fan_pwm` (0-255, from its on-device model's
predicted-NH3 error) and `heater_pwm` (0-255, from the classification --
`LOW_TEMP_ALERT` -> 255, else 0). A dashboard MANUAL override, if set,
replaces *both* bytes with the operator's fan speed % / heater power %
before they're sent. This board makes no actuation decisions of its own any
more -- it only applies whatever the master sends:

| Signal          | Fan (PWM)                                          | Heater relay |
|-----------------|-----------------------------------------------------|--------------|
| `fan_pwm`       | Written verbatim as PWM duty (0-255)                | -- |
| `fan_pwm > 0`   | `FAN_ENABLE` driven HIGH (powered)                  | -- |
| `fan_pwm == 0`  | `FAN_ENABLE` driven LOW (de-energized)              | -- |
| `heater_pwm > 0`| --                                                    | ON (relay energized) |
| `heater_pwm == 0`| --                                                   | OFF (relay de-energized) |

**Note:** earlier revisions of this board guaranteed fan and heater were
never both ON, since a single classification string drove both locally.
That guarantee no longer holds -- `fan_pwm` and `heater_pwm` are computed
independently on the master (and either can come from a MANUAL dashboard
override, decoupled from the other), so e.g. a nonzero MANUAL fan override
alongside a nonzero heater_pwm (MANUAL or `LOW_TEMP_ALERT`-driven) will run
the fan and energize the heater relay simultaneously. If that combination is
unsafe for the physical setup, it needs to be enforced explicitly (either
server-side in the dashboard/master's `loop()`, or by forcing `fan_pwm` to 0
in this board's `applyActuators()` whenever `heaterOn` is true) -- it is not
currently enforced anywhere in either firmware.

## Edge-AI forecast and spike-risk prediction

This board no longer computes a forecast itself -- the on-node linear-
extrapolation stub from earlier versions has been superseded by the ESP32-S3
master's real TFLite Micro model, which produces `predicted_temperature`,
`predicted_ammonia`, and `predicted_spike_probability` from a rolling window
of this board's raw readings. See `../esp32s3_master/README.md` for the
model details and the spike-risk threshold.

## LCD (I2C 16x2, RG1602A-IIC(P) / PCF8574T backpack)

Wired to the DevKit's hardware I2C bus (`include/config.h`: `PIN_LCD_SDA` =
GPIO21, `PIN_LCD_SCL` = GPIO22; GND to GND, VCC to VIN/5V). Shows a static
two-line readout of this board's own current reading -- no rotation, no
forecast:

- **Row 0** -- current temperature + humidity from the DHT11 (`T:25.3C H:60%`).
- **Row 1** -- current MQ-137 ammonia reading (`NH3: 12.3 ppm`).

Both rows redraw every time a fresh sample completes (`SAMPLE_INTERVAL_MS`,
5 s by default); before the first successful reading they show a boot
placeholder instead. The master still relays back `predicted_temperature`/
`predicted_ammonia`/`has_prediction` on the `ActuatorCommand` wire contract
(see the comment above `struct ActuatorCommand` in `src/main.cpp`) -- this
board receives them but intentionally doesn't display them; the LCD is
current-data-only on this branch.

The backpack answers at I2C address `0x27` by default (`LCD_I2C_ADDR` in
`config.h`); some PCF8574AT clones use `0x3F` instead -- if the display stays
blank after wiring is confirmed correct, run an I2C scanner sketch to find
the real address before assuming a wiring fault. Requires the
`marcoschwartz/LiquidCrystal_I2C` library, pulled automatically via
`platformio.ini`'s `lib_deps`.

## Benchmark mode -- synthetic data for testing the predictive system

`platformio.ini` defines a second environment, `esp32dev_benchmark`, that
replaces this board's real DHT11/MQ-137 reads with a scripted sequence of
scenarios instead -- for exercising the master's on-device predictive model,
Django's classifier, and the real actuator response on-demand and
repeatably, without needing a physical sensor attached or waiting on real
environmental drift to test an edge case. Everything downstream of the
sensor read is the genuine pipeline: real ESP-NOW, real TFLite inference on
the master, real classification, real fan/heater relay -- only the sensor
value itself is synthetic.

```bash
pio run -e esp32dev_benchmark -t upload    # flash the benchmark build
pio device monitor                         # watch [BENCHMARK]/[TX] lines

# ... test, watch the master's [MODEL]/[ACTUATE] lines and the dashboard ...

pio run -e esp32dev -t upload              # flash back to real sensors
```

This is **compile-time only** (`BENCHMARK_MODE`, set via the dedicated
environment's `build_flags`, never a runtime toggle) -- a board can't
accidentally start reporting fake data without an explicit reflash, and both
the boot banner and the LCD say "BENCHMARK MODE" loudly on startup as a
physical reminder while it's active.

The scenario list (`BENCHMARK_SCENARIOS` in `src/main.cpp`) cycles through,
holding or ramping each for `BENCHMARK_SAMPLES_PER_SCENARIO` samples
(`config.h`, default 24 * 5 s = 2 minutes per scenario). **Note:** this
2-minute-per-scenario default predates the current model's retrain -- the
old windowed model warmed up in ~9 samples (~45 s), so 2 minutes was
generous. The retrained model needs 7 finalized 10-minute buckets
(~70 minutes after boot, see `hardware/esp32s3_master/README.md`'s "Model
architecture" section) before it produces its first prediction at all, so
a full benchmark loop now cycles through several scenarios before any
prediction appears. Fine for exercising ESP-NOW/classification/actuation
(none of that depends on warm-up), but if you specifically want to watch
`predicted_ammonia` lead a live reading during `AMMONIA_RAMP_TO_SPIKE`,
either let one scenario hold well past 70 minutes (raise
`BENCHMARK_SAMPLES_PER_SCENARIO` for that run) or just wait out the real
warm-up once and watch subsequent scenarios normally after that.

| Scenario | Tests |
|---|---|
| `OPTIMAL_BASELINE` | Warm-up baseline; nothing should trip. |
| `AMMONIA_STEP_CRITICAL` / `_RECOVER` | Instant step across the 15 ppm `CRITICAL_AMMONIA` threshold, both directions -- classification latency, actuator response speed. |
| `AMMONIA_RAMP_TO_SPIKE` / `_RECOVER` | Gradual rise/fall through the threshold -- does `predicted_ammonia` (the forecast) lead the live reading, and does `predicted_spike_probability` rise before the classifier actually flips? |
| `AMMONIA_BOUNDARY_LOW` / `_HIGH` | Held just under/over 15 ppm -- exact threshold correctness, no off-by-one. |
| `HEAT_STRESS_STEP` / `_RECOVER` | T + RH combined past both thresholds at once (the classifier's conjunction rule) and back. |
| `LOW_TEMP_STEP` / `_RECOVER` | Below 18°C and back -- heater relay reactivity. |
| `COLD_AND_CRITICAL_AMMONIA` / `_RECOVER` | Cold and critical ammonia at once -- `predicted_class` comes back `CRITICAL_AMMONIA` only (classify_environment() checks ammonia first), so this is the regression case for the `low_temperature_alert` fix: both fan **and** heater must activate, not just the fan. |

Add, remove, or edit entries in `BENCHMARK_SCENARIOS` for other cases (a
different profile's thresholds, a slower/faster ramp, a longer soak at one
value) -- each row is just a name plus a start/end value per channel, so a
constant scenario is a step and a `Start != End` scenario is a ramp.
