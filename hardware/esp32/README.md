# ESP32 Sensor Node -- Firmware

Production-grade Arduino/ESP32 firmware for the Poultry Telemetry sensor
node. Reads DHT11 (temperature + humidity) and MQ-137 (ammonia PPM) every
5 seconds and sends them over **ESP-NOW** to the ESP32-S3 master node
(`../esp32s3_master/`), which runs the on-device forecast/spike model and
owns the WiFi/HTTP leg to Django. The master relays Django's classification
back over ESP-NOW so this board can still drive its own fan (PWM speed
control) and PTC heater (active-LOW ON/OFF relay), closing the control loop
end-to-end across the two boards.

This board still joins WiFi (see `include/config.h`), but only to (a) land
on the same channel as the master -- ESP-NOW requires both peers on the
same channel, and joining the same AP is the simplest way to guarantee that
-- and (b) sync wall-clock time over NTP for the model's hour/month cyclic
features. It no longer POSTs to Django directly; see
`../esp32s3_master/README.md` for the full two-board pipeline and the MAC
pairing procedure required before either board can talk to the other.

## Pin map (must match the KiCad schematic)

| Signal        | GPIO   | Socket pin | Notes |
|---------------|--------|------------|-------|
| `DHT_DATA`    | GPIO4  | 20         | 10 kOhm pull-up to 3V3; single-wire DHT11 protocol |
| `MQ_DATA`     | GPIO34 | 12         | INPUT-ONLY, ADC1_CH6 -- required so WiFi and ADC coexist |
| `PWM_FAN`     | GPIO32 | 10         | LEDC PWM (channel 0, 25 kHz, 8-bit) speed target to the fan's own driver IC -- not a power switch, see `FAN_ENABLE` below |
| `FAN_ENABLE`  | GPIO27 | TBD -- verify against schematic | Digital output gating an external N-MOSFET on the fan's GND leg; HIGH = fan powered, LOW = de-energized. Real hard-off, since many cheap fans don't honor 0% PWM duty as a true stop |
| `HEATER_RELAY`| GPIO25 | TBD -- verify against schematic | Digital output to relay module IN pin; active-LOW (LOW = heater ON). ON/OFF only, no speed control |
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
# Edit include/config.h: WIFI_SSID, WIFI_PASSWORD, MASTER_MAC_ADDR
# (see the MAC pairing procedure in ../esp32s3_master/README.md), and
# GMT_OFFSET_SEC for your timezone.
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
can easily reach 3-5x. The classifier threshold (25 PPM CRITICAL) is set
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
GPIO21, `PIN_LCD_SCL` = GPIO22; GND to GND, VCC to VIN/5V). Rotates through
three screens every `LCD_SCREEN_MS` (3 s by default), independent of the 5 s
sample cadence:

1. **Live Reading** -- current temperature + humidity from the DHT11.
2. **Ammonia (NH3)** -- current MQ-137 PPM reading.
3. **Predicted** -- `predicted_temperature` / `predicted_ammonia` relayed
   back from the master's on-device model, in the extended `ActuatorCommand`
   (`has_prediction`, `predicted_temperature`, `predicted_ammonia` -- see the
   wire-contract comment above `struct ActuatorCommand` in `src/main.cpp`).
   Shows "Warming up..." until the master's model has filled its rolling
   window (~30-40 s after boot) and a first prediction actually exists.

The backpack answers at I2C address `0x27` by default (`LCD_I2C_ADDR` in
`config.h`); some PCF8574AT clones use `0x3F` instead -- if the display stays
blank after wiring is confirmed correct, run an I2C scanner sketch to find
the real address before assuming a wiring fault. Requires the
`marcoschwartz/LiquidCrystal_I2C` library, pulled automatically via
`platformio.ini`'s `lib_deps`.
