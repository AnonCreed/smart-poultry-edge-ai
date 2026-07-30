// ============================================================================
// main.cpp -- ESP32-S3 master node firmware.
//
// Responsibilities:
//   1. Connect to WiFi, print own MAC address (needed to configure sensor node).
//   2. Register as ESP-NOW receiver; accept SensorPacket from the sensor ESP32.
//   3. On each received packet, POST the data to the Django telemetry API.
//   4. Parse the HTTP 201 response to extract predicted_class.
//   5. Send ActuatorCommand back to the sensor ESP32 over ESP-NOW so it can
//      drive fan/heater actuators to close the control loop.
//
// Boot sequence:
//   a) Flash this firmware first.
//   b) Open Serial Monitor -- the board prints:
//        [BOOT] Master MAC: AA:BB:CC:DD:EE:FF
//   c) Copy that MAC into esp32/include/config.h as MASTER_MAC_ADDR.
//   d) Flash the sensor ESP32 firmware.
//   e) Both boards are now running; sensor packets arrive every 5 s and
//      the dashboard shows real sensor data.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <ArduinoJson.h>

#include "config.h"

// ----------------------------------------------------------------------------
// State
// ----------------------------------------------------------------------------
// Last SensorPacket received over ESP-NOW, handed off from the WiFi-task
// context (ESP-NOW callback) to loop() via a critical section.
static SensorPacket  g_packet;
static volatile bool g_newPacket  = false;
static uint8_t       g_sensorMac[6] = {0};
static bool          g_sensorKnown  = false;
static portMUX_TYPE  g_mux = portMUX_INITIALIZER_UNLOCKED;

// ----------------------------------------------------------------------------
// WiFi helpers
// ----------------------------------------------------------------------------
static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.printf("[WIFI] Connecting to '%s'...\n", WIFI_SSID);
    for (int i = 0; i < WIFI_MAX_RETRIES; ++i) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] Connected. IP=%s  Channel=%d  RSSI=%d dBm\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.channel(), WiFi.RSSI());
            return;
        }
        delay(WIFI_RETRY_MS);
        Serial.printf("[WIFI] retry %d/%d\n", i + 1, WIFI_MAX_RETRIES);
    }
    Serial.println("[WIFI] Failed to connect.");
}

static void ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("[WIFI] Link lost -- reconnecting.");
    WiFi.disconnect(true, true);
    connectWifi();
    // Re-init ESP-NOW after WiFi reconnect (channel may have changed).
    esp_now_deinit();
    esp_now_init();
}

// ----------------------------------------------------------------------------
// ESP-NOW callbacks
// ----------------------------------------------------------------------------
static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
    if (len != sizeof(SensorPacket)) return;

    portENTER_CRITICAL_ISR(&g_mux);
    memcpy(&g_packet, data, sizeof(SensorPacket));
    g_newPacket = true;

    // Auto-learn sensor MAC on first packet so we can send ActuatorCommand back.
    if (!g_sensorKnown) {
        memcpy(g_sensorMac, info->src_addr, 6);
        g_sensorKnown = true;
    }
    portEXIT_CRITICAL_ISR(&g_mux);
}

static void onDataSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESPNOW] ActuatorCommand delivery failed.");
    }
}

static bool initEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] Init failed.");
        return false;
    }
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);
    Serial.println("[ESPNOW] Listening for sensor packets.");
    return true;
}

// Register the sensor node as a peer once its MAC is known (first packet).
static void registerSensorPeer() {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, g_sensorMac, 6);
    peer.channel = 0;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) == ESP_OK) {
        Serial.printf("[ESPNOW] Sensor peer registered: %02X:%02X:%02X:%02X:%02X:%02X\n",
                      g_sensorMac[0], g_sensorMac[1], g_sensorMac[2],
                      g_sensorMac[3], g_sensorMac[4], g_sensorMac[5]);
    }
}

// ----------------------------------------------------------------------------
// HTTP POST to Django
// ----------------------------------------------------------------------------
/**
 * POST one sensor reading to /api/telemetry/submit/.
 * Fills outState[24] with the classification string returned by Django.
 * Returns true on HTTP 201.
 */
static bool postToDjango(const SensorPacket& pkt, char outState[24]) {
    char url[80];
    snprintf(url, sizeof(url), "http://%s:%d%s", DJANGO_HOST, DJANGO_PORT, SUBMIT_PATH);

    // Build JSON payload. predicted_* fields are null -- no TFLite model yet;
    // the Django API accepts null and classifies from live readings alone.
    StaticJsonDocument<256> doc;
    doc["temperature"]          = roundf(pkt.temperature * 10.0f) / 10.0f;
    doc["humidity"]             = roundf(pkt.humidity * 10.0f) / 10.0f;
    doc["ammonia_level"]        = roundf(pkt.ammonia_ppm * 10.0f) / 10.0f;
    doc["predicted_temperature"] = nullptr;
    doc["predicted_ammonia"]     = nullptr;

    char body[256];
    serializeJson(doc, body, sizeof(body));

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);

    const int httpCode = http.POST(body);

    if (httpCode == 201) {
        // Parse classification from response.
        StaticJsonDocument<384> resp;
        DeserializationError err = deserializeJson(resp, http.getStream());
        if (!err) {
            const char* cls = resp["record"]["predicted_class"] | "UNKNOWN";
            strncpy(outState, cls, 23);
            outState[23] = '\0';
        } else {
            strncpy(outState, "OPTIMAL_ENVIRONMENT", 23);
        }
        http.end();
        return true;
    }

    Serial.printf("[HTTP] POST failed, code=%d\n", httpCode);
    http.end();
    return false;
}

// ----------------------------------------------------------------------------
// Arduino entrypoints
// ----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[BOOT] Poultry Telemetry ESP32-S3 master node starting.");

    connectWifi();

    // Print MAC so user can configure MASTER_MAC_ADDR in the sensor firmware.
    Serial.printf("[BOOT] Master MAC (WiFi STA): %s\n", WiFi.macAddress().c_str());
    Serial.println("[BOOT] Copy the MAC above into esp32/include/config.h MASTER_MAC_ADDR, then flash sensor node.");

    if (!initEspNow()) {
        Serial.println("[BOOT] ESP-NOW init failed -- halting.");
        while (true) delay(1000);
    }

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("[BOOT] NTP sync started.");
    Serial.println("[BOOT] Ready. Waiting for sensor packets over ESP-NOW...");
}

void loop() {
    ensureWifi();

    if (!g_newPacket) return;

    // Safely copy the packet out of the ISR-shared buffer.
    SensorPacket pkt;
    portENTER_CRITICAL(&g_mux);
    pkt = g_packet;
    g_newPacket = false;
    portEXIT_CRITICAL(&g_mux);

    // Auto-register sensor as ESP-NOW peer on first packet.
    static bool peerAdded = false;
    if (g_sensorKnown && !peerAdded) {
        registerSensorPeer();
        peerAdded = true;
    }

    Serial.printf("[RX] seq=%u T=%.1fC RH=%.1f%% NH3=%.1fppm hour=%u month=%u\n",
                  pkt.seq, pkt.temperature, pkt.humidity,
                  pkt.ammonia_ppm, pkt.hour, pkt.month);

    // POST to Django and get classification.
    char state[24] = "OPTIMAL_ENVIRONMENT";
    if (postToDjango(pkt, state)) {
        Serial.printf("[TX] Classification: %s -- sending ActuatorCommand to sensor.\n", state);
    } else {
        Serial.println("[TX] HTTP failed; sending last-known state to sensor.");
    }

    // Relay classification back to sensor node over ESP-NOW.
    if (g_sensorKnown) {
        ActuatorCommand cmd{};
        strncpy(cmd.state, state, sizeof(cmd.state) - 1);
        esp_now_send(g_sensorMac,
                     reinterpret_cast<const uint8_t*>(&cmd),
                     sizeof(cmd));
    }
}
