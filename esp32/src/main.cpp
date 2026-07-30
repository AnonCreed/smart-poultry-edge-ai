// ============================================================================
// main.cpp -- Poultry Telemetry ESP32 sensor-node firmware.
//
// Responsibilities, in order of execution priority:
//   1. Maintain WiFi association; reconnect with linear back-off on drop.
//      (Needed only for channel alignment with the ESP32-S3 master and for
//      NTP time -- see the network note in config.h. This node no longer
//      speaks HTTP to Django; the master owns that leg.)
//   2. Sample DHT22 (temperature, humidity) and MQ-137 (ammonia PPM) on a
//      fixed 5 s cadence.
//   3. Send a compact binary SensorPacket over ESP-NOW to the ESP32-S3
//      master node, which runs the TFLite Micro forecast/spike model and
//      forwards the combined record to Django.
//   4. Receive the resulting classification back from the master over
//      ESP-NOW and drive the PWM_FAN / PWM_HEATER outputs accordingly,
//      closing the environmental control loop end-to-end across two boards.
//
// The main loop is non-blocking; all timing is millis()-based.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <time.h>
#include <DHT.h>
#include <math.h>

#include "config.h"

// ----------------------------------------------------------------------------
// Peripheral objects
// ----------------------------------------------------------------------------
static DHT dht(PIN_DHT_DATA, DHT22);

// Millis() timestamp of the last completed sample cycle. Deliberately
// separate from FreeRTOS timers so the schedule survives temporary WiFi
// stalls without drift accumulating on the reconnect side.
static uint32_t nextSampleAt = 0;
static uint32_t txSeq = 0;

// ----------------------------------------------------------------------------
// Wire contract shared with esp32s3_master/esp32s3_inference_receiver.ino.
// Field order/types must stay byte-identical on both sides -- this struct
// is copied directly out of the ESP-NOW payload with memcpy, no serializer.
// ----------------------------------------------------------------------------
struct SensorPacket {
    uint32_t seq;
    float temperature;
    float humidity;
    float ammonia_ppm;
    uint8_t hour;   // 0-23, local time
    uint8_t month;  // 1-12
};

// Classification vocabulary the master relays back -- must remain
// byte-identical to the Django telemetry.EnvironmentalState enum.
struct ActuatorCommand {
    char state[24];
};

static constexpr const char* STATE_OPTIMAL  = "OPTIMAL_ENVIRONMENT";
static constexpr const char* STATE_HEAT     = "HEAT_STRESS_WARNING";
static constexpr const char* STATE_LOW_TEMP = "LOW_TEMP_ALERT";
static constexpr const char* STATE_CRITICAL = "CRITICAL_AMMONIA";

static uint8_t masterMac[6] = MASTER_MAC_ADDR;

// Latest actuator command from the master, handed off from the ESP-NOW recv
// callback (runs in the WiFi task context) to the main loop via a critical
// section -- same pattern the master firmware uses for its own inbound path.
static ActuatorCommand g_lastCommand;
static volatile bool g_newCommand = false;
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// Last-known wall-clock fields, used as a fallback if NTP sync fails so the
// model's cyclic features stay in a plausible (not pathological) range.
static uint8_t lastHour = 12;
static uint8_t lastMonth = 6;

// ============================================================================
// WiFi lifecycle
// ============================================================================
static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // Latency over power savings for control loop.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    Serial.printf("[WIFI] Associating with SSID '%s'\n", WIFI_SSID);
    for (int attempt = 0; attempt < WIFI_MAX_RETRIES; ++attempt) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WIFI] Link up. IP=%s RSSI=%d dBm Channel=%d\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
            return;
        }
        delay(WIFI_RETRY_MS);
        Serial.printf("[WIFI] retry %d/%d\n", attempt + 1, WIFI_MAX_RETRIES);
    }
    Serial.println("[WIFI] Association failed; will retry on next tick.");
}

static void ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.println("[WIFI] Link lost -- reconnecting.");
    WiFi.disconnect(true, true);
    connectWifi();
}

/**
 * Sync wall-clock time over NTP. Only meaningful while WiFi is associated;
 * called once at boot and again on every reconnect since a dropped link may
 * have let the RTC drift uncorrected for a while.
 */
static void syncNtpTime() {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, NTP_SYNC_TIMEOUT_MS)) {
        Serial.printf("[NTP] Synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println("[NTP] Sync failed -- hour/month features will use last-known/default values.");
    }
}

/**
 * Read current local hour/month for the model's cyclic features. Falls back
 * to the last successfully-read values (seeded to a neutral noon/June
 * default) rather than propagating a hard failure, since sensor sampling
 * must not stall on a clock read.
 */
static void currentHourMonth(uint8_t& hour, uint8_t& month) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
        lastHour = static_cast<uint8_t>(timeinfo.tm_hour);
        lastMonth = static_cast<uint8_t>(timeinfo.tm_mon + 1);
    }
    hour = lastHour;
    month = lastMonth;
}

// ============================================================================
// Sensor sampling
// ============================================================================

/**
 * Read DHT22. The library returns NAN on checksum failure or if polled
 * faster than the sensor's 500 ms internal refresh; both cases signal an
 * invalid tick to the caller.
 */
static bool sampleDht(float& temperatureC, float& humidityPct) {
    humidityPct  = dht.readHumidity();
    temperatureC = dht.readTemperature();  // Celsius by default.
    if (isnan(humidityPct) || isnan(temperatureC)) {
        Serial.println("[DHT] Read failed -- checksum or timing violation.");
        return false;
    }
    return true;
}

/**
 * Convert an averaged MQ-137 ADC reading to NH3 PPM using the datasheet
 * characteristic curve. The intermediate Rs value is derived from the
 * sensor's resistor divider; PPM comes from a power-law fit of the log-log
 * NH3 curve. Numerical guards clamp the output to a plausible envelope so
 * a disconnected sensor floats to a diagnosable rail rather than a random
 * huge number.
 */
static float mqCountsToPpm(uint32_t sumCounts, uint16_t samples) {
    const float avgCounts = static_cast<float>(sumCounts) / samples;
    const float vOut = (avgCounts / ADC_MAX_COUNTS) * ADC_MAX_VOLTS;

    // Open-circuit / rail conditions: return a diagnostic zero rather than
    // dividing by a near-zero denominator and producing an inf.
    if (vOut < 0.05f || vOut > (MQ137_VC_VOLTS - 0.05f)) {
        return 0.0f;
    }

    const float rs    = MQ137_RL_KOHMS * (MQ137_VC_VOLTS - vOut) / vOut;
    const float ratio = rs / MQ137_R0_KOHMS;
    float ppm = MQ137_A * powf(ratio, MQ137_B);

    if (ppm < 0.0f)   ppm = 0.0f;
    if (ppm > 500.0f) ppm = 500.0f;  // Backend ingestion envelope ceiling.
    return ppm;
}

static float sampleMq137() {
    uint32_t sum = 0;
    for (uint16_t i = 0; i < MQ_ADC_SAMPLES; ++i) {
        sum += analogRead(PIN_MQ_DATA);
        delayMicroseconds(200);  // ADC settling; shorter than any RTOS tick.
    }
    return mqCountsToPpm(sum, MQ_ADC_SAMPLES);
}

// ============================================================================
// ESP-NOW transport
// ============================================================================

/**
 * Fired by the ESP-NOW stack (WiFi task context) once the previous send
 * completes. Diagnostic only -- failures are logged, not retried, since the
 * next 5 s tick naturally supersedes a stale sample.
 */
static void onDataSent(const uint8_t* /*mac*/, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("[ESPNOW] Delivery to master failed.");
    }
}

/**
 * Fired by the ESP-NOW stack when the master relays back a classification.
 * Runs in the WiFi task context, so the payload is copied out under a
 * critical section for the main loop to consume -- mirrors the pattern the
 * master firmware uses for its own inbound sensor packets.
 */
static void onDataRecv(const esp_now_recv_info_t* /*info*/, const uint8_t* data, int len) {
    if (len != sizeof(ActuatorCommand)) return;

    portENTER_CRITICAL_ISR(&g_mux);
    memcpy(&g_lastCommand, data, sizeof(ActuatorCommand));
    g_newCommand = true;
    portEXIT_CRITICAL_ISR(&g_mux);
}

static bool initEspNow() {
    if (esp_now_init() != ESP_OK) {
        Serial.println("[ESPNOW] Init failed.");
        return false;
    }
    esp_now_register_send_cb(onDataSent);
    esp_now_register_recv_cb(onDataRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, masterMac, 6);
    peer.channel = 0;  // 0 == use the current WiFi STA channel.
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        Serial.println("[ESPNOW] Failed to register master as peer.");
        return false;
    }

    Serial.printf("[ESPNOW] Ready. Local MAC=%s  Master peer=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  WiFi.macAddress().c_str(),
                  masterMac[0], masterMac[1], masterMac[2],
                  masterMac[3], masterMac[4], masterMac[5]);
    return true;
}

/** Send one sample to the master. Returns false only on a local send error. */
static bool sendSample(float temperatureC, float humidityPct, float ammoniaPpm) {
    SensorPacket packet{};
    packet.seq = txSeq++;
    packet.temperature = temperatureC;
    packet.humidity = humidityPct;
    packet.ammonia_ppm = ammoniaPpm;
    currentHourMonth(packet.hour, packet.month);

    const esp_err_t result = esp_now_send(
        masterMac, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet)
    );
    return result == ESP_OK;
}

// ============================================================================
// Actuator control -- closes the environmental loop
// ============================================================================

/**
 * Map the master-relayed classification to fan/heater duty cycles.
 *
 * Rules (independent of the classifier's precedence -- these describe the
 * mechanical response, not the diagnostic):
 *   CRITICAL_AMMONIA    -> fan ON, heater OFF (ventilate)
 *   HEAT_STRESS_WARNING -> fan ON, heater OFF (cool by exhaust)
 *   LOW_TEMP_ALERT      -> heater ON, fan OFF
 *   OPTIMAL_ENVIRONMENT -> both OFF
 *
 * Fan and heater are never both ON simultaneously; the interlock is
 * inherent to the classification since a single state is returned.
 */
static void applyActuators(const char* state) {
    uint32_t fanDuty    = PWM_DUTY_OFF;
    uint32_t heaterDuty = PWM_DUTY_OFF;

    if (strcmp(state, STATE_CRITICAL) == 0 || strcmp(state, STATE_HEAT) == 0) {
        fanDuty = PWM_DUTY_ON;
    } else if (strcmp(state, STATE_LOW_TEMP) == 0) {
        heaterDuty = PWM_DUTY_ON;
    }

    ledcWrite(PWM_FAN_CHANNEL,    fanDuty);
    ledcWrite(PWM_HEATER_CHANNEL, heaterDuty);
}

// ============================================================================
// Arduino entrypoints
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println("\n[BOOT] Poultry Telemetry sensor node starting.");

    // Sensors
    dht.begin();
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);  // 0..3.3 V range on GPIO34.

    // PWM peripherals for fan and heater. Attach BEFORE writing duty so the
    // first control cycle doesn't glitch the MOSFET gates high.
    ledcSetup(PWM_FAN_CHANNEL,    PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcSetup(PWM_HEATER_CHANNEL, PWM_FREQ_HZ, PWM_RESOLUTION_BITS);
    ledcAttachPin(PIN_PWM_FAN,    PWM_FAN_CHANNEL);
    ledcAttachPin(PIN_PWM_HEATER, PWM_HEATER_CHANNEL);
    ledcWrite(PWM_FAN_CHANNEL,    PWM_DUTY_OFF);
    ledcWrite(PWM_HEATER_CHANNEL, PWM_DUTY_OFF);

    connectWifi();
    syncNtpTime();

    if (!initEspNow()) {
        Serial.println("[BOOT] ESP-NOW bring-up failed; halting.");
        while (true) delay(1000);
    }

    nextSampleAt = millis();  // First tick fires immediately.
}

void loop() {
    ensureWifi();

    // Apply any actuator command relayed back from the master since the
    // last loop iteration, independent of the sampling cadence below.
    if (g_newCommand) {
        ActuatorCommand cmd;
        portENTER_CRITICAL(&g_mux);
        cmd = g_lastCommand;
        g_newCommand = false;
        portEXIT_CRITICAL(&g_mux);

        applyActuators(cmd.state);
        Serial.printf("[RX] Master classification: %s\n", cmd.state);
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextSampleAt) < 0) {
        return;  // Not time yet; cooperatively yield.
    }
    nextSampleAt = now + SAMPLE_INTERVAL_MS;

    // -- Sample --------------------------------------------------------------
    float temperatureC = 0.0f, humidityPct = 0.0f;
    if (!sampleDht(temperatureC, humidityPct)) {
        return;  // Skip tick on invalid DHT read; next tick retries.
    }
    const float ammoniaPpm = sampleMq137();

    // -- Transmit over ESP-NOW -------------------------------------------------
    if (sendSample(temperatureC, humidityPct, ammoniaPpm)) {
        Serial.printf("[TX] T=%.2fC RH=%.2f%% NH3=%.2fppm -> master\n",
                      temperatureC, humidityPct, ammoniaPpm);
    } else {
        Serial.println("[TX] ESP-NOW send failed; actuators held at last state.");
    }
}
