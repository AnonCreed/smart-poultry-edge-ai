// ============================================================================
// config_master.h -- Compile-time configuration for the ESP32-S3 master node.
//
// The master is the only board that talks to Django: it receives raw
// samples from the sensor node over ESP-NOW, runs the on-device TFLite
// Micro model (model_data.h / scaler_params.h -- trained artifacts, do not
// hand-edit), and POSTs the combined record (raw + forecast + spike risk)
// to the same ingestion endpoint the Python simulator and the original
// single-board firmware used. The classification Django returns is relayed
// back to the sensor node over ESP-NOW so it can drive its own fan/heater.
// ============================================================================
#pragma once

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------
// Overwrite before flashing. Must be the SAME network the sensor node joins
// -- ESP-NOW requires both peers on the same WiFi channel, and the simplest
// way to guarantee that is for both boards to associate with the same AP.
#define WIFI_SSID       "your-network-ssid"
#define WIFI_PASSWORD   "your-network-password"

// LAN address of the Django server hosting the telemetry API.
#define API_BASE_URL    "http://192.168.1.100:8000"
#define API_SUBMIT_PATH "/api/telemetry/submit/"

// HTTP timeout: keep short so a transient outage doesn't stall the receive
// loop. A dropped POST simply means that one sample never reaches the
// dashboard -- the next ESP-NOW packet from the sensor node tries again.
#define HTTP_TIMEOUT_MS 4000

// MAC address of the ESP32 DevKit V1 sensor node's WiFi station interface.
// Flash esp32/ first (see esp32/README.md) and copy the MAC it prints on
// boot into this array. Required so the master can send the actuator
// classification back over ESP-NOW after each successful ingestion.
#define SENSOR_NODE_MAC_ADDR { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
