// ============================================================================
// main.cpp -- ESP32-S3 master node firmware.
//
// Responsibilities:
//   1. Connect to WiFi, print own MAC address (needed to configure sensor
//      node), and sync NTP -- the sole source of wall-clock time for the
//      model's hour-of-day feature now, since the sensor node has no WiFi
//      of its own (see esp32/include/config.h's "Radio" section).
//   2. Register as ESP-NOW receiver; accept SensorPacket from the sensor
//      ESP32. If none arrives for 3x its expected interval, self-heal by
//      re-initializing ESP-NOW (see the watchdog block in loop()).
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
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <esp_now.h>
#include <ArduinoJson.h>
#include <time.h>  // getLocalTime() -- see currentHour() below

#include "config.h"
#include "model_runner.h"

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

// millis() timestamp of the last SensorPacket actually received, and of the
// last watchdog-triggered ESP-NOW re-init -- see the silence check in
// loop(). Seeded at the end of setup() (not left at 0/epoch) so the
// silence window starts counting from this board's own boot, giving the
// sensor node time to boot and start transmitting before being flagged.
static uint32_t g_lastPacketRxMs = 0;

// Django host, found via UDP broadcast discovery instead of a hardcoded IP
// (see discoverDjangoHost() below). Empty/unknown until discovery succeeds.
static String         g_djangoHost  = "";
static bool           g_djangoKnown = false;

// Last classification string relayed to the sensor node -- used only as the
// `state` field on the ActuatorCommand sent by pollManualControl() below
// (the sensor no longer acts on `state`, only logs it, so staleness here is
// cosmetic). Updated whenever a real ingestion round-trip succeeds.
static char g_lastState[24] = "OPTIMAL_ENVIRONMENT";

// Last successful on-device model prediction, cached so the independent
// MANUAL-override poll below (which runs on its own timer, decoupled from
// sensor packets) can still relay it to the sensor node's LCD instead of
// blanking the "Predicted" screen back to a warm-up placeholder every time
// a MANUAL command goes out.
static ModelPrediction g_lastPrediction;
static bool            g_havePrediction = false;

// Flock age in weeks, synced from Django's FlockProfile via the independent
// CONTROL_PATH poll below (see fetchManualControl()'s age_weeks parsing) --
// the retrained on-device model takes flock age as a feature ("week" in
// feature_cols), so the master needs it locally even though inference runs
// before this sample is ever POSTed to Django. Defaults to 1 (youngest
// band) until the first successful poll after boot.
static uint8_t g_flockAgeWeeks = 1;

// Forward declarations -- ensureWifi() (below) needs to re-run ESP-NOW
// bring-up after a reconnect, but initEspNow()/registerSensorPeer() are
// defined further down alongside the rest of the ESP-NOW callbacks.
static bool initEspNow();
static void registerSensorPeer();

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
            // The sensor node hardcodes its ESP-NOW channel (it has no AP
            // to negotiate one from -- see esp32/include/config.h's
            // "Radio" section) instead of following this router
            // dynamically. If the router ever assigns a different channel
            // than the sensor is fixed to, ESP-NOW between the two boards
            // goes silently dead -- flag it loudly here since there's
            // nothing this board can do about it besides tell the operator.
            if (WiFi.channel() != ESPNOW_WIFI_CHANNEL) {
                Serial.printf("[WIFI] WARNING: router assigned channel %d but the "
                              "sensor node is hardcoded to channel %d "
                              "(ESPNOW_WIFI_CHANNEL, esp32/include/config.h) -- "
                              "ESP-NOW will NOT work until these match. Update the "
                              "sensor's config.h and reflash it, or fix the "
                              "router's channel.\n",
                              WiFi.channel(), ESPNOW_WIFI_CHANNEL);
            }
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
    // CRITICAL: esp_now_deinit() wipes BOTH the registered send/recv
    // callbacks (only ever set up once, in initEspNow() from setup()) AND
    // the peer table -- if either isn't redone here, the board goes
    // silently deaf (never fires onDataRecv for incoming SensorPackets) or
    // mute (esp_now_send() to the now-unregistered sensor peer fails) over
    // ESP-NOW, permanently, until the next full power-cycle. This is the
    // most likely failure mode on a board running off external/battery
    // power where WiFi drops (brownouts, AP roaming, signal dropouts) are
    // routine rather than rare -- a board on USB power during development
    // may simply never hit this path, masking the bug until field
    // deployment. initEspNow() re-registers the callbacks; if the sensor's
    // MAC was already learned from an earlier packet, registerSensorPeer()
    // re-adds it so ActuatorCommands (fan/heater duty, LCD predictions)
    // can keep flowing back to it too.
    esp_now_deinit();
    if (!initEspNow()) {
        Serial.println("[ESPNOW] Re-init after WiFi reconnect failed -- "
                        "sensor RX and actuator relay are down until next reset.");
    } else if (g_sensorKnown) {
        registerSensorPeer();
    }

    // A reconnect can mean a new DHCP lease -- possibly for the Django PC
    // too -- so don't trust the previously-discovered host anymore.
    g_djangoKnown = false;
}

// Last-known local hour, used as a fallback if NTP hasn't synced (or has
// lapsed) so the model's hour_sin/hour_cos cyclic features stay in a
// plausible (not pathological) range rather than going stale forever.
// Seeded to a neutral noon default, same fallback pattern the sensor node
// used to use for its own now-removed currentHourMonth().
static uint8_t g_lastHour = 12;

/**
 * Read current local hour for the model's hour_sin/hour_cos cyclic
 * features. The sensor node has no WiFi/NTP of its own anymore (see
 * esp32/include/config.h's "Radio" section) -- this board is the only one
 * with a wall-clock, so it now supplies hour itself instead of trusting a
 * value in the SensorPacket (which no longer carries one at all). Falls
 * back to the last successfully-read hour rather than propagating a hard
 * failure, since inference must not stall on a clock read.
 */
static uint8_t currentHour() {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        g_lastHour = static_cast<uint8_t>(timeinfo.tm_hour);
    }
    return g_lastHour;
}

// ----------------------------------------------------------------------------
// ESP-NOW callbacks
// ----------------------------------------------------------------------------
static void onDataRecv(const uint8_t *mac_addr, const uint8_t* data, int len) {
    if (len != sizeof(SensorPacket)) return;

    portENTER_CRITICAL_ISR(&g_mux);
    memcpy(&g_packet, data, sizeof(SensorPacket));
    g_newPacket = true;

    // Auto-learn sensor MAC on first packet so we can send ActuatorCommand back.
    if (!g_sensorKnown) {
        memcpy(g_sensorMac, mac_addr, 6);
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
// Django host discovery (UDP broadcast -- no hardcoded IP)
// ----------------------------------------------------------------------------
/**
 * Broadcasts a discovery request on the local subnet and waits for the
 * Django dev server's UDP responder (telemetry/discovery.py) to answer.
 * The reply's *source IP* -- not its payload -- becomes g_djangoHost, so
 * this works unmodified on any network the board and the Django PC share,
 * with no IP baked into firmware.
 *
 * Broadcasts to the computed subnet broadcast address (e.g. 192.168.1.255)
 * rather than the global 255.255.255.255, since some routers/APs drop the
 * latter but not subnet-directed broadcasts.
 *
 * Blocking: up to DISCOVERY_MAX_ATTEMPTS * DISCOVERY_TIMEOUT_MS. Called
 * once at boot and again from loop() (non-blocking cadence there) whenever
 * the host is unknown -- e.g. Django hasn't started yet, or POSTs started
 * failing because its IP changed.
 */
static bool discoverDjangoHost() {
    WiFiUDP udp;
    if (!udp.begin(DISCOVERY_PORT + 1)) {  // ephemeral local port for the reply
        Serial.println("[DISCOVERY] Failed to open UDP socket.");
        return false;
    }

    const IPAddress local = WiFi.localIP();
    const IPAddress mask  = WiFi.subnetMask();
    const IPAddress broadcast(local[0] | ~mask[0], local[1] | ~mask[1],
                               local[2] | ~mask[2], local[3] | ~mask[3]);

    for (int attempt = 1; attempt <= DISCOVERY_MAX_ATTEMPTS; ++attempt) {
        Serial.printf("[DISCOVERY] Broadcasting to %s:%d (attempt %d/%d)...\n",
                      broadcast.toString().c_str(), DISCOVERY_PORT,
                      attempt, DISCOVERY_MAX_ATTEMPTS);
        udp.beginPacket(broadcast, DISCOVERY_PORT);
        udp.write(reinterpret_cast<const uint8_t*>(DISCOVERY_REQUEST_MAGIC),
                  strlen(DISCOVERY_REQUEST_MAGIC));
        udp.endPacket();

        const uint32_t deadline = millis() + DISCOVERY_TIMEOUT_MS;
        while (millis() < deadline) {
            const int packetSize = udp.parsePacket();
            if (packetSize <= 0) {
                delay(20);
                continue;
            }

            char buf[64] = {0};
            const int len = udp.read(buf, sizeof(buf) - 1);
            buf[len > 0 ? len : 0] = '\0';

            if (strncmp(buf, DISCOVERY_RESPONSE_MAGIC,
                        strlen(DISCOVERY_RESPONSE_MAGIC)) == 0) {
                g_djangoHost = udp.remoteIP().toString();
                Serial.printf("[DISCOVERY] Found Django server at %s\n",
                              g_djangoHost.c_str());
                udp.stop();
                return true;
            }
        }
    }

    udp.stop();
    Serial.println("[DISCOVERY] No response -- will keep retrying.");
    return false;
}

// ----------------------------------------------------------------------------
// HTTP POST to Django
// ----------------------------------------------------------------------------
/**
 * POST one sensor reading to /api/telemetry/submit/.
 * `pred` is null while the on-device model is still warming up (first
 * ~9 samples after boot) -- Django accepts null forecast fields and
 * classifies from live readings alone in that window.
 * Fills outState[24] with the classification string returned by Django.
 * Also fills outManualMode/outManualFanPwm/outManualHeaterPwm from the
 * response's `control` block -- lets a dashboard MANUAL override (fan speed
 * + heater power sliders) take priority over the on-device MLP-predicted
 * fan_pwm and the classification-derived heater_pwm the caller computes.
 * All three stay false/0/0 (i.e. "no override, use AUTO/predicted") if the
 * response can't be parsed.
 * Fills outLowTemperatureAlert from the response's top-level
 * `low_temperature_alert` -- a raw temperature-vs-threshold check computed
 * server-side, independent of outState. Django's classify_environment()
 * returns one mutually-exclusive label with ammonia checked first, so a
 * reading that's simultaneously CRITICAL_AMMONIA and genuinely cold comes
 * back as outState="CRITICAL_AMMONIA", never "LOW_TEMP_ALERT" -- checking
 * outState alone for the heater decision would then leave it off while
 * it's actually cold. outLowTemperatureAlert sidesteps that masking; see
 * telemetry/models.py's ActuatorControl.auto_duty_for_state() docstring
 * for the full reasoning (this mirrors that same fix here).
 * Returns true on HTTP 201.
 */
static bool postToDjango(const SensorPacket& pkt, const ModelPrediction* pred, char outState[24],
                          bool& outManualMode, uint8_t& outManualFanPwm, uint8_t& outManualHeaterPwm,
                          bool& outLowTemperatureAlert) {
    if (!g_djangoKnown) {
        Serial.println("[HTTP] Django host not discovered yet -- skipping POST.");
        return false;
    }

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", g_djangoHost.c_str(), DJANGO_PORT, SUBMIT_PATH);

    JsonDocument doc;
    doc["temperature"]   = roundf(pkt.temperature * 10.0f) / 10.0f;
    doc["humidity"]      = roundf(pkt.humidity * 10.0f) / 10.0f;
    doc["ammonia_level"] = roundf(pkt.ammonia_ppm * 10.0f) / 10.0f;
    if (pred != nullptr) {
        doc["predicted_temperature"]      = pred->temperature_next;
        doc["predicted_ammonia"]          = pred->ammonia_next;
        // Django's predicted_spike_probability field predates the retrained
        // model's second (temperature-spike) classifier head -- kept wired
        // to the ammonia one only, matching prior behavior exactly.
        // pred->temp_spike_probability/temp_spike_predicted are real,
        // computed values (see the [MODEL] Serial log in loop()) but not
        // yet threaded through to Django/the dashboard; doing so needs a
        // new persisted field and UI, deliberately out of scope here.
        doc["predicted_spike_probability"] = pred->ammonia_spike_probability;
    } else {
        doc["predicted_temperature"]      = nullptr;
        doc["predicted_ammonia"]          = nullptr;
        doc["predicted_spike_probability"] = nullptr;
    }

    char body[256];
    serializeJson(doc, body, sizeof(body));

    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(HTTP_TIMEOUT_MS);

    const int httpCode = http.POST(body);

    if (httpCode == 201) {
        // Parse classification from response.
        JsonDocument resp;
        DeserializationError err = deserializeJson(resp, http.getStream());
        outManualMode = false;      // default -- no override unless parsed below
        outManualFanPwm = 0;
        outManualHeaterPwm = 0;     // NEW
        outLowTemperatureAlert = false;
        if (!err) {
            const char* cls = resp["record"]["predicted_class"] | "UNKNOWN";
            strncpy(outState, cls, 23);
            outState[23] = '\0';
            outLowTemperatureAlert = resp["low_temperature_alert"] | false;

            // Dashboard actuator control -- MANUAL means the operator pinned
            // fan speed + heater power % from the UI, which should win over
            // the on-device MLP-predicted fan_pwm and the classification-
            // derived heater_pwm. AUTO leaves both of the caller's computed
            // values untouched.
            const char* mode = resp["control"]["mode"] | "AUTO";
            outManualMode = (strcmp(mode, "MANUAL") == 0);
            if (outManualMode) {
                const int fanPct = resp["control"]["effective_fan_pct"] | 0;  // 0-100
                outManualFanPwm = (uint8_t)constrain((fanPct * 255) / 100, 0, 255);
                const int heaterPct = resp["control"]["effective_heater_pct"] | 0;  // 0-100
                outManualHeaterPwm = (uint8_t)constrain((heaterPct * 255) / 100, 0, 255);
            }
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

/**
 * Relay one ActuatorCommand to the sensor node over ESP-NOW, if its MAC is
 * known. `pred` is null while the on-device model is still warming up --
 * has_prediction/predicted_temperature/predicted_ammonia are sent as
 * 0/0.0/0.0 in that case, and the sensor node's LCD shows a placeholder
 * instead of stale/zeroed numbers (see g_havePrediction there).
 */
static void sendActuatorCommand(const char* state, uint8_t fanPwm, uint8_t heaterPwm,
                                 const ModelPrediction* pred) {
    if (!g_sensorKnown) return;
    ActuatorCommand cmd{};
    strncpy(cmd.state, state, sizeof(cmd.state) - 1);
    cmd.fan_pwm = fanPwm;
    cmd.heater_pwm = heaterPwm;
    cmd.has_prediction = (pred != nullptr) ? 1 : 0;
    cmd.predicted_temperature = (pred != nullptr) ? pred->temperature_next : 0.0f;
    cmd.predicted_ammonia = (pred != nullptr) ? pred->ammonia_next : 0.0f;

    // Check the synchronous return, not just onDataSent's async delivery
    // status -- a send to an unregistered peer (e.g. the peer-table wipe in
    // ensureWifi() this comment used to warn about) fails right here with
    // no callback ever firing, so onDataSent's failure log alone would never
    // catch it. This is what makes that class of bug diagnosable from the
    // serial log instead of just going silently mute.
    const esp_err_t result = esp_now_send(
        g_sensorMac, reinterpret_cast<const uint8_t*>(&cmd), sizeof(cmd));
    if (result != ESP_OK) {
        Serial.printf("[ESPNOW] ActuatorCommand send call failed immediately (err=%d) "
                      "-- sensor peer may not be registered.\n", result);
    }
}

/**
 * GET CONTROL_PATH directly (no sensor packet involved) so a dashboard
 * MANUAL fan/heater override still reaches the sensor node even when it's
 * offline/not transmitting -- postToDjango()'s control parsing only runs
 * piggybacked on a fresh SensorPacket, which starves MANUAL delivery
 * whenever the sensor node isn't actively sending. This is the independent
 * path that fixes that: called on its own timer from loop(), regardless of
 * g_newPacket.
 *
 * Also opportunistically updates g_flockAgeWeeks from the same response's
 * top-level `age_weeks` field (added to Django's control endpoint alongside
 * this project's other flock-profile plumbing) -- unconditionally, whether
 * mode is MANUAL or AUTO, since the on-device model needs flock age as a
 * feature regardless of actuator mode. Left at its previous cached value
 * (default 1) if the field is missing or the poll fails, rather than
 * resetting -- a transient miss shouldn't suddenly change what "week" the
 * model thinks it's predicting for.
 *
 * Returns true and fills outFanPwm/outHeaterPwm only when mode is MANUAL and
 * the request succeeded; false otherwise (AUTO, unreachable, or unparsable --
 * callers should send nothing in that case, since the packet-driven AUTO
 * path already owns AUTO's actuation whenever real sensor data is flowing,
 * and there's nothing sensible to force when it isn't).
 */
static bool fetchManualControl(uint8_t& outFanPwm, uint8_t& outHeaterPwm) {
    if (!g_djangoKnown) return false;

    char url[96];
    snprintf(url, sizeof(url), "http://%s:%d%s", g_djangoHost.c_str(), DJANGO_PORT, CONTROL_PATH);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    const int httpCode = http.GET();
    if (httpCode != 200) {
        http.end();
        return false;
    }

    JsonDocument resp;
    const DeserializationError err = deserializeJson(resp, http.getStream());
    http.end();
    if (err) return false;

    if (resp["age_weeks"].is<int>()) {
        int ageWeeks = resp["age_weeks"];
        g_flockAgeWeeks = (uint8_t)constrain(ageWeeks, 1, 255);
    }

    const char* mode = resp["control"]["mode"] | "AUTO";
    if (strcmp(mode, "MANUAL") != 0) return false;

    const int fanPct = resp["control"]["effective_fan_pct"] | 0;
    const int heaterPct = resp["control"]["effective_heater_pct"] | 0;
    outFanPwm = (uint8_t)constrain((fanPct * 255) / 100, 0, 255);
    outHeaterPwm = (uint8_t)constrain((heaterPct * 255) / 100, 0, 255);
    return true;
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

    if (!model_runner::init()) {
        Serial.println("[BOOT] Model init failed -- halting.");
        while (true) delay(1000);
    }
    Serial.println("[BOOT] TFLite model loaded.");

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    Serial.println("[BOOT] NTP sync started.");

    Serial.println("[BOOT] Discovering Django host via UDP broadcast...");
    g_djangoKnown = discoverDjangoHost();
    if (!g_djangoKnown) {
        Serial.println("[BOOT] Django host not found yet -- will keep retrying from loop().");
    }

    Serial.println("[BOOT] Ready. Waiting for sensor packets over ESP-NOW...");

    // Start the sensor-silence watchdog's window from boot, not from
    // millis()==0 -- see g_lastPacketRxMs's doc comment.
    g_lastPacketRxMs = millis();
}

void loop() {
    ensureWifi();

    // Sensor silence watchdog: if no SensorPacket has arrived in 3x the
    // sensor's expected transmission interval, the ESP-NOW stack itself may
    // be stuck without this board's own WiFi ever having dropped (the case
    // ensureWifi()'s reconnect-triggered re-init above doesn't cover). Cheap
    // self-heal: re-run the identical esp_now_deinit()/initEspNow()/
    // registerSensorPeer() sequence. Harmless no-op if the sensor is
    // genuinely powered off -- it just keeps retrying every window; doesn't
    // touch the model's bucket history, so a brief real outage doesn't cost
    // the ~70 min warm-up.
    {
        static uint32_t lastWatchdogRecoveryMs = 0;
        const uint32_t sinceLastPacket = millis() - g_lastPacketRxMs;
        if (sinceLastPacket > SENSOR_SILENCE_TIMEOUT_MS &&
            millis() - lastWatchdogRecoveryMs > SENSOR_SILENCE_TIMEOUT_MS) {
            lastWatchdogRecoveryMs = millis();
            Serial.printf("[ESPNOW] No sensor packet in %lu ms (>= 3x expected "
                          "interval) -- re-initializing ESP-NOW.\n",
                          (unsigned long)sinceLastPacket);
            esp_now_deinit();
            if (!initEspNow()) {
                Serial.println("[ESPNOW] Watchdog re-init failed -- will retry next window.");
            } else if (g_sensorKnown) {
                registerSensorPeer();
            }
        }
    }

    // Keep trying to (re)discover the Django host in the background --
    // covers "Django hadn't started yet at boot" and "its IP just changed"
    // without hammering the network with broadcasts every iteration.
    if (!g_djangoKnown) {
        static uint32_t lastAttemptMs = 0;
        if (millis() - lastAttemptMs > 5000) {
            lastAttemptMs = millis();
            g_djangoKnown = discoverDjangoHost();
        }
    }

    // Independent MANUAL-override poll, decoupled from sensor-packet arrival
    // -- without this, a dashboard MANUAL fan/heater command would only ever
    // reach the sensor node piggybacked on a fresh SensorPacket below, which
    // means it silently never fires while the sensor node is offline (see
    // fetchManualControl() doc comment). Runs on its own CONTROL_POLL_MS
    // cadence, independent of g_newPacket.
    {
        static uint32_t lastControlPollMs = 0;
        if (g_sensorKnown && millis() - lastControlPollMs > CONTROL_POLL_MS) {
            lastControlPollMs = millis();
            uint8_t manualFanPwm = 0, manualHeaterPwm = 0;
            if (fetchManualControl(manualFanPwm, manualHeaterPwm)) {
                Serial.printf("[CONTROL] Independent poll -- MANUAL fan_pwm=%u heater_pwm=%u (state=%s)\n",
                              manualFanPwm, manualHeaterPwm, g_lastState);
                sendActuatorCommand(g_lastState, manualFanPwm, manualHeaterPwm,
                                    g_havePrediction ? &g_lastPrediction : nullptr);
            }
        }
    }

    if (!g_newPacket) return;

    // Safely copy the packet out of the ISR-shared buffer.
    SensorPacket pkt;
    portENTER_CRITICAL(&g_mux);
    pkt = g_packet;
    g_newPacket = false;
    portEXIT_CRITICAL(&g_mux);
    g_lastPacketRxMs = millis();  // Feeds the silence watchdog above.

    // Auto-register sensor as ESP-NOW peer on first packet.
    static bool peerAdded = false;
    if (g_sensorKnown && !peerAdded) {
        registerSensorPeer();
        peerAdded = true;
    }

    Serial.printf("[RX] seq=%u T=%.1fC RH=%.1f%% NH3=%.1fppm\n",
                  pkt.seq, pkt.temperature, pkt.humidity, pkt.ammonia_ppm);

    // Run on-device inference. Each raw sample feeds a rolling 10-minute
    // bucket internally (see model_runner.cpp) -- returns false almost every
    // call, and only ever produces a fresh prediction once each ~10-minute
    // bucket finalizes, needing 7 finalized buckets (~70 min after boot)
    // before the first one. hour comes from this board's own clock now
    // (currentHour() above), not the packet -- see RawSample's doc comment.
    RawSample raw{pkt.temperature, pkt.humidity, pkt.ammonia_ppm, currentHour()};
    ModelPrediction pred;
    const bool havePrediction = model_runner::predict(raw, g_flockAgeWeeks, pred);
    if (havePrediction) {
        Serial.printf("[MODEL] next T=%.2fC NH3=%.2fppm ammonia_spike_prob=%.4f(%d) temp_spike_prob=%.4f(%d)\n",
                      pred.temperature_next, pred.ammonia_next,
                      pred.ammonia_spike_probability, pred.ammonia_spike_predicted,
                      pred.temp_spike_probability, pred.temp_spike_predicted);
        g_lastPrediction = pred;
        g_havePrediction = true;
    } else {
        Serial.println("[MODEL] warming up -- not enough bucketed history yet (~70 min after boot).");
    }

    // Fan PWM duty (0-255) from predicted-NH3 error. 0 (fan off) while the
    // model is still warming up and havePrediction is false. Sent verbatim
    // to the sensor node, whose fan is real PWM.
    uint8_t fanPwm = 0;
    if (havePrediction) {
        float nh3_error = pred.ammonia_next - 5.0f;        // setpoint = 5 ppm
        if (nh3_error < 0) nh3_error = 0;
        int pwm = (int)((nh3_error / 25.0f) * 255);        // scale 0-25 ppm error -> 0-255
        fanPwm = (uint8_t)constrain(pwm, 0, 255);
    }

    // POST to Django and get classification.
    char state[24] = "OPTIMAL_ENVIRONMENT";
    static uint8_t consecutiveFailures = 0;
    bool manualMode = false;
    uint8_t manualFanPwm = 0;
    uint8_t manualHeaterPwm = 0;   // NEW
    bool lowTemperatureAlert = false;
    if (postToDjango(pkt, havePrediction ? &pred : nullptr, state, manualMode, manualFanPwm, manualHeaterPwm,
                      lowTemperatureAlert)) {
        consecutiveFailures = 0;
        strncpy(g_lastState, state, sizeof(g_lastState) - 1);  // for the independent MANUAL poll's log/state field
        Serial.printf("[TX] Classification: %s -- sending ActuatorCommand to sensor.\n", state);
    } else {
        Serial.println("[TX] HTTP failed; sending last-known state to sensor.");
        // A handful of failures against a "known" host most likely means the
        // Django PC's IP changed underneath us (new DHCP lease) -- rather
        // than fail forever, drop it and let loop() re-discover.
        if (g_djangoKnown && ++consecutiveFailures >= 3) {
            Serial.println("[DISCOVERY] Repeated POST failures -- re-discovering Django host.");
            g_djangoKnown = false;
            consecutiveFailures = 0;
        }
    }

    // Heater PWM byte -- driven by lowTemperatureAlert (a raw temperature-
    // vs-threshold check Django computes independently), NOT by checking
    // state=="LOW_TEMP_ALERT". state is a single mutually-exclusive label
    // with ammonia checked first (see classify_environment() in
    // telemetry/classifier.py), so a reading that's simultaneously
    // CRITICAL_AMMONIA and genuinely cold comes back with
    // state=="CRITICAL_AMMONIA" -- checking state alone here would then
    // leave the heater off exactly when a chick is both gassed and
    // freezing. lowTemperatureAlert sidesteps that masking entirely (see
    // the postToDjango() doc comment and ActuatorControl.auto_duty_for_state()
    // in telemetry/models.py for the full reasoning).
    uint8_t heaterPwm = lowTemperatureAlert ? 255 : 0;

    // Safety floor: a live reading already past threshold forces the fan to
    // 100% regardless of what the forecast says -- mirrors
    // ActuatorControl.auto_duty_for_state() in telemetry/models.py. Until
    // this, that floor existed ONLY as a number Django computed for the
    // dashboard's AUTO display; it never reached this fanPwm calculation
    // (which only ever looks at pred.ammonia_next above, never `state`), so
    // a live CRITICAL_AMMONIA/HEAT_STRESS_WARNING reading with a low
    // forecast left the dashboard reading "Fan 100%" while the physical fan
    // sat near idle. A reading already past a safety threshold must not
    // wait on a prediction to react.
    if (strcmp(state, "CRITICAL_AMMONIA") == 0 || strcmp(state, "HEAT_STRESS_WARNING") == 0) {
        fanPwm = 255;
    }

    // Dashboard MANUAL override (fan speed + heater power sliders) wins over
    // the on-device MLP-predicted fanPwm and the classification-derived
    // heaterPwm computed above. AUTO leaves both as-is.
    if (manualMode) {
        Serial.printf("[CONTROL] Dashboard MANUAL override -- fan_pwm=%u (was predicted %u)  heater_pwm=%u (was %u)\n",
                      manualFanPwm, fanPwm, manualHeaterPwm, heaterPwm);
        fanPwm = manualFanPwm;
        heaterPwm = manualHeaterPwm;
    }

    // Relay classification + fan/heater bytes back to sensor node over
    // ESP-NOW, plus this tick's prediction (if any) for the LCD. Logged
    // here (not at the earlier "[TX] Classification" line) since fanPwm
    // isn't final until the safety-floor/MANUAL-override adjustments above
    // have both had a chance to run.
    Serial.printf("[ACTUATE] fan_pwm=%u heater_pwm=%u (state=%s)\n", fanPwm, heaterPwm, state);
    sendActuatorCommand(state, fanPwm, heaterPwm, havePrediction ? &pred : nullptr);
}
