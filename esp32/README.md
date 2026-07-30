# ESP32 Sensor Node -- Firmware

Production-grade Arduino/ESP32 firmware for the Poultry Telemetry sensor
node. Reads DHT22 (temperature + humidity) and MQ-137 (ammonia PPM) every
5 seconds and sends them over **ESP-NOW** to the ESP32-S3 master node
(`../esp32s3_master/`), which runs the on-device forecast/spike model and
owns the WiFi/HTTP leg to Django. The master relays Django's classification
back over ESP-NOW so this board can still drive its own fan and heater PWM
outputs, closing the control loop end-to-end across the two boards.

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
this board over ESP-NOW (`ActuatorCommand`), and the firmware maps it to
actuator state:

| Classification          | Fan  | Heater |
|-------------------------|------|--------|
| `CRITICAL_AMMONIA`      | ON   | OFF    |
| `HEAT_STRESS_WARNING`   | ON   | OFF    |
| `LOW_TEMP_ALERT`        | OFF  | ON     |
| `OPTIMAL_ENVIRONMENT`   | OFF  | OFF    |

The interlock (fan and heater never both on) is inherent to the
classification -- a single state is returned per record.

## Edge-AI forecast and spike-risk prediction

This board no longer computes a forecast itself -- the on-node linear-
extrapolation stub from earlier versions has been superseded by the ESP32-S3
master's real TFLite Micro model, which produces `predicted_temperature`,
`predicted_ammonia`, and `predicted_spike_probability` from a rolling window
of this board's raw readings. See `../esp32s3_master/README.md` for the
model details and the spike-risk threshold.
