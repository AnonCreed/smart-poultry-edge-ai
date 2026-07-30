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
// Overwrite before flashing production units. In a proper deployment these
// should come from NVS provisioning (WiFiManager or ESP-IDF wifi_prov_mgr),
// not compile-time constants -- kept here as literals for demo clarity only.
#define WIFI_SSID       "your-network-ssid"
#define WIFI_PASSWORD   "your-network-password"

// LAN address of the Django server hosting the telemetry API. When the ESP32
// and the server share a subnet, use the server's private IP; using
// "127.0.0.1" here would resolve to the ESP32 itself and fail.
#define API_BASE_URL    "http://192.168.1.100:8000"
#define API_SUBMIT_PATH "/api/telemetry/submit/"

// HTTP timeout: keep short so a transient outage doesn't stall the sample
// loop for tens of seconds. Retries happen on the next tick.
#define HTTP_TIMEOUT_MS 4000

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

// ---------------------------------------------------------------------------
// Edge-AI forecast stub
// ---------------------------------------------------------------------------
// Set to 1 to transmit a lightweight one-tick-ahead extrapolation in the
// predicted_temperature / predicted_ammonia fields. Set to 0 to transmit
// them as JSON null, matching the dashboard's "hardware not yet linked"
// default state. When the real TFLite Micro model is deployed, replace the
// stub in main.cpp; the payload contract stays identical.
#define ENABLE_EDGE_AI_STUB 1
