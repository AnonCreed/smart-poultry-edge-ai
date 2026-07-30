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
// esp32s3_master/ first and copy the "Receiver MAC: XX:XX:XX:XX:XX:XX" line
// it prints on boot into this array (byte order matches, no reversal needed).
// Placeholder until the master's actual boot-log MAC is read back.
#define MASTER_MAC_ADDR { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

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
// DHT22 data line. GPIO4 is a general-purpose IO with hardware pull-up
// support -- the standard 10 kOhm pull-up between DATA and 3V3 is assumed
// on the sensor breakout.
#define PIN_DHT_DATA    4

// MQ-137 analog output. GPIO34 is INPUT-ONLY on the classic ESP32 and is
// wired to ADC1 channel 6 -- ADC1 is the only ADC block usable while WiFi
// is active, so this channel selection is mandatory, not incidental.
// Sensor output must be scaled to 0-3.3 V by an external divider before
// reaching this pin; MQ modules that swing to 5 V will damage the SoC.
#define PIN_MQ_DATA     34

// PWM outputs. Series 200 Ohm gate resistors (R5, R4) are on the board.
// LEDC channels 0/1 drive these; frequency and resolution below.
#define PIN_PWM_FAN     32
#define PIN_PWM_HEATER  33

// LEDC (ESP32 PWM peripheral) parameters. 25 kHz is above audible range so
// four-pin PC fans do not whine; 8-bit gives 0-255 duty granularity which
// is finer than the actuator needs.
#define PWM_FAN_CHANNEL     0
#define PWM_HEATER_CHANNEL  1
#define PWM_FREQ_HZ         25000
#define PWM_RESOLUTION_BITS 8
#define PWM_DUTY_OFF        0
#define PWM_DUTY_ON         220   // ~86% -- headroom to protect MOSFET

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
