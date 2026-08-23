// ============================================================================
// config.h -- Compile-time configuration for the ESP32-S3 master node.
// ============================================================================
#pragma once

#include "credentials.h"   // WIFI_SSID / WIFI_PASSWORD (gitignored)

// ---------------------------------------------------------------------------
// Django backend -- discovered at boot, no hardcoded IP.
// ---------------------------------------------------------------------------
// The master node no longer bakes in the PC's LAN IP (it changes with every
// network the board is flashed onto). Instead it broadcasts a small UDP
// discovery request and takes whichever IP replies -- see
// discoverDjangoHost() in main.cpp and telemetry/discovery.py on the server
// side. Only the port and path are still fixed, since those don't change
// between networks.
#define DJANGO_PORT              8000
#define SUBMIT_PATH              "/api/telemetry/submit/"
#define CONTROL_PATH             "/api/telemetry/control/"

// How often to poll CONTROL_PATH independently of incoming sensor packets --
// needed so a dashboard MANUAL fan/heater override still reaches the sensor
// node even when it's offline/not transmitting (see pollManualControl() in
// main.cpp). Deliberately faster than SAMPLE_INTERVAL_MS on the sensor side
// so a manual change feels responsive when testing the relay/actuators alone.
#define CONTROL_POLL_MS          3000

#define DISCOVERY_PORT           42424
#define DISCOVERY_REQUEST_MAGIC  "POULTRY_TELEMETRY_DISCOVER_V1"
#define DISCOVERY_RESPONSE_MAGIC "POULTRY_TELEMETRY_HERE_V1"
#define DISCOVERY_TIMEOUT_MS     1500   // per-attempt wait for a reply
#define DISCOVERY_MAX_ATTEMPTS   8      // ~12 s worst case at boot

// ---------------------------------------------------------------------------
// NTP (for logging timestamps only -- model features come from the sensor node)
// ---------------------------------------------------------------------------
#define NTP_SERVER          "pool.ntp.org"
#define GMT_OFFSET_SEC       20700   // Nepal Standard Time (UTC+5:45)
#define DAYLIGHT_OFFSET_SEC  0
#define NTP_SYNC_TIMEOUT_MS  5000

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
#define WIFI_RETRY_MS    2000
#define WIFI_MAX_RETRIES 10
#define HTTP_TIMEOUT_MS  4000

// ---------------------------------------------------------------------------
// Wire contract -- must remain byte-identical with esp32/src/main.cpp
// ---------------------------------------------------------------------------
// SensorPacket is sent by the sensor ESP32 to this master over ESP-NOW.
struct SensorPacket {
    uint32_t seq;
    float    temperature;
    float    humidity;
    float    ammonia_ppm;
    uint8_t  hour;    // 0-23 local time (NTP-synced on sensor side)
    uint8_t  month;   // 1-12
};

// ActuatorCommand is sent back from this master to the sensor ESP32.
struct ActuatorCommand {
    char state[24];      // e.g. "CRITICAL_AMMONIA"
    uint8_t fan_pwm;     // 0-255 duty from MLP-predicted NH3 error, or a
                          // dashboard MANUAL override (main.cpp) -- sensor's
                          // fan is real PWM, applied verbatim.
    uint8_t heater_pwm;  // NEW: 0-255 from classification (LOW_TEMP_ALERT ->
                          // 255, else 0), or a dashboard MANUAL override
                          // (main.cpp) -- sensor's heater is a plain relay,
                          // so it just thresholds this at >0.
    uint8_t has_prediction;       // NEW: 1 once the on-device model has
                                   // warmed up and predicted_temperature/
                                   // predicted_ammonia below are valid; 0
                                   // during the ~30-40s warm-up window right
                                   // after boot, in which case the sensor
                                   // node's LCD shows a placeholder instead.
    float predicted_temperature;  // NEW: model_runner's temperature_next,
                                   // relayed purely for display on the
                                   // sensor node's LCD -- not used for
                                   // actuation (fan_pwm above already
                                   // encodes the model's ammonia-error
                                   // decision).
    float predicted_ammonia;      // NEW: model_runner's ammonia_next, same
                                   // display-only purpose as above.
};
