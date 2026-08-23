// ============================================================================
// config.h -- Compile-time configuration for the ESP32 edge node.
//
// All hardware-specific magic numbers live here. Pin assignments mirror the
// KiCad schematic (2x15 pin socket, ESP32 DevKit V1); do not change without
// updating the board layout and this comment block simultaneously.
// ============================================================================
#pragma once

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
// WiFi credentials are loaded from credentials.h (gitignored).
// Copy esp32/include/credentials.h.example -> credentials.h and fill in
// your SSID and password before building.
//
// This node no longer talks to Django directly (see HARDWARE_DOCUMENTATION.md
// "ESP-NOW pipeline"): it joins the WiFi AP only so it (a) lands on the same
// 2.4 GHz channel as the ESP32-S3 master, which ESP-NOW requires, and (b) can
// pull real wall-clock time via NTP for the model's hour/month cyclic
// features. Telemetry itself goes out over ESP-NOW to MASTER_MAC_ADDR below;
// the master owns the WiFi/HTTP leg to Django.
#include "credentials.h"

// MAC address of the ESP32-S3 master node's WiFi station interface. Flash
// esp32s3_master/ first and copy the "[BOOT] Master MAC (WiFi STA): XX:XX:XX:XX:XX:XX"
// line it prints on boot into this array (byte order matches, no reversal
// needed). Read back live from the currently-flashed master board on
// 2026-08-07: DC:B4:D9:14:14:6C. Re-copy this if the master is ever
// reflashed onto different hardware (a new board has a different MAC).
#define MASTER_MAC_ADDR { 0xDC, 0xB4, 0xD9, 0x14, 0x14, 0x6C }

// NTP time source for the model's hour-of-day / month-of-year cyclic
// features (see esp32s3_master/esp32s3_inference_receiver.ino).
// GMT_OFFSET_SEC = 20700 = Nepal Standard Time (UTC+5:45).
#define NTP_SERVER            "pool.ntp.org"
#define GMT_OFFSET_SEC         20700
#define DAYLIGHT_OFFSET_SEC    0
#define NTP_SYNC_TIMEOUT_MS    5000

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
// Transmit cadence must match the dashboard's polling interval (5 s) so the
// chart tail advances one point per refresh -- no aliasing or empty ticks.
#define SAMPLE_INTERVAL_MS 5000

// WiFi reconnect back-off: linear, capped. Wall-clock reconnection latency
// stays below one sample interval even at the ceiling.
#define WIFI_RETRY_MS      2000
#define WIFI_MAX_RETRIES   10

// ---------------------------------------------------------------------------
// Pin map (mirrors schematic; do not divorce)
// ---------------------------------------------------------------------------
// DHT11 data line. GPIO4 is a general-purpose IO with hardware pull-up
// support -- the standard 10 kOhm pull-up between DATA and 3V3 is assumed
// on the sensor breakout.
#define PIN_DHT_DATA    4

// MQ-137 analog output. GPIO34 is INPUT-ONLY on the classic ESP32 and is
// wired to ADC1 channel 6 -- ADC1 is the only ADC block usable while WiFi
// is active, so this channel selection is mandatory, not incidental.
// Sensor output must be scaled to 0-3.3 V by an external divider before
// reaching this pin; MQ modules that swing to 5 V will damage the SoC.
#define PIN_MQ_DATA     34

// Fan PWM output (speed target). Series 200 Ohm gate resistor (R5) is on
// the board. LEDC channel 0 drives this; frequency and resolution below.
#define PIN_PWM_FAN     32

// Hard power cutoff for the fan. The 4-pin fan's +12V/GND leads are
// wired directly to the rail (see schematic) -- PIN_PWM_FAN alone only
// reaches the fan's own internal speed-controller IC via its PWM pin, and
// many cheap 4-pin fans don't honor 0% duty as a true stop (they idle at a
// minimum RPM floor instead). This pin gates an external N-MOSFET on the
// fan's GND leg for a real hard-off, independent of that quirk. GPIO27 is
// general-purpose/non-strapping and otherwise unused on this board.
#define PIN_FAN_ENABLE  27

// LEDC (ESP32 PWM peripheral) parameters for the fan channel. 25 kHz is
// above audible range so four-pin PC fans do not whine; 8-bit gives 0-255
// duty granularity which is finer than the actuator needs.
#define PWM_FAN_CHANNEL     0
#define PWM_FREQ_HZ         25000
#define PWM_RESOLUTION_BITS 8
#define PWM_DUTY_OFF        0

// Heater relay control. The PTC heater is switched by a plain relay
// module -- ON/OFF only, no speed target. Originally assumed active-LOW
// (LOW energizes the coil), but bench testing showed the physical heater
// runs OPPOSITE to the commanded state -- driving the pin LOW turns the
// heater OFF and HIGH turns it ON. That's consistent with either an
// active-HIGH relay module (this one energizes on HIGH, not LOW) or the
// heater's load being wired to the relay's NC (normally-closed) contact
// instead of NO (normally-open) -- either way, the fix is the same: flip
// which logic level means "on". GPIO25 is general-purpose/non-strapping
// and otherwise unused on this board.
#define PIN_HEATER_RELAY  25
#define HEATER_RELAY_ON    HIGH
#define HEATER_RELAY_OFF   LOW

// ---------------------------------------------------------------------------
// MQ-137 characteristic-curve calibration
// ---------------------------------------------------------------------------
// Rs computation uses the sensor's load resistor:
//   Rs = RL * (Vc - Vout) / Vout
// Then, from the datasheet's NH3 curve fit (log-log linearization):
//   PPM = MQ137_A * (Rs / R0) ^ MQ137_B
//
// R0 MUST be re-calibrated per unit in known-clean air after a ~24-48 hour
// burn-in. The value below is the datasheet nominal for illustration only.
// Recommended field procedure: expose sensor to ambient outdoor air, average
// Rs over 5 minutes, and store that value as R0 in NVS.
#define MQ137_VC_VOLTS   5.0f    // Sensor supply voltage
#define MQ137_RL_KOHMS   47.0f   // Load resistor on the module
#define MQ137_R0_KOHMS   3.6f    // Clean-air Rs baseline (CALIBRATE PER UNIT)
#define MQ137_A          102.2f
#define MQ137_B          -2.473f

// ADC oversampling. Averaging 16 raw samples suppresses the ESP32 ADC's
// well-known non-linearity noise floor by ~4x without introducing lag.
#define MQ_ADC_SAMPLES   16

// ADC full-scale: 12-bit resolution, 11 dB attenuation -> 0..3.3 V.
#define ADC_MAX_COUNTS   4095.0f
#define ADC_MAX_VOLTS    3.3f

// ---------------------------------------------------------------------------
// LCD -- I2C 16x2 (RG1602A-IIC(P), PCF8574T I2C backpack)
// ---------------------------------------------------------------------------
// Wired to the DevKit's hardware I2C bus, not a dedicated pair in the KiCad
// schematic -- GND to GND, VCC to VIN/5V (the PCF8574T backpack and the
// LCD's own logic run happily off 5V; the I2C lines are open-drain and the
// backpack's pull-ups reference its own VCC, so 5V here does not endanger
// the ESP32's 3.3V GPIOs).
#define PIN_LCD_SDA      21   // ESP32 DevKit V1 default I2C SDA
#define PIN_LCD_SCL      22   // ESP32 DevKit V1 default I2C SCL

// PCF8574T backpacks ship at 0x27 by default; PCF8574AT variants (or some
// clones) answer at 0x3F instead. If the display stays blank/garbled after
// wiring is confirmed correct, run an I2C scanner sketch to find the actual
// address rather than assuming a wiring fault.
#define LCD_I2C_ADDR     0x27
#define LCD_COLS         16
#define LCD_ROWS         2
