// ============================================================================
// config.h -- Compile-time configuration for the ESP32-S3 master node.
// ============================================================================
#pragma once

#include "credentials.h"   // WIFI_SSID / WIFI_PASSWORD (gitignored)

// ---------------------------------------------------------------------------
// Django backend
// ---------------------------------------------------------------------------
// IP address of the PC running `python manage.py runserver`.
// Must be the LAN IP (not 127.0.0.1) so the ESP32-S3 can reach it over WiFi.
#define DJANGO_HOST    "192.168.0.11"
#define DJANGO_PORT    8000
#define SUBMIT_PATH    "/api/telemetry/submit/"

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
    char state[24];   // e.g. "CRITICAL_AMMONIA"
};
